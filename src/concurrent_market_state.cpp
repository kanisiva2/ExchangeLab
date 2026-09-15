#include "exchangelab/concurrent_market_state.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace exchangelab {
namespace {

std::size_t checked_entry_count(const std::uint16_t exchange_count,
                                const std::uint32_t instrument_count) {
  if (exchange_count == 0) {
    throw std::invalid_argument("exchange count must be greater than zero");
  }
  if (instrument_count == 0) {
    throw std::invalid_argument("instrument count must be greater than zero");
  }

  const auto exchanges = static_cast<std::size_t>(exchange_count);
  const auto instruments = static_cast<std::size_t>(instrument_count);
  if (instruments > std::numeric_limits<std::size_t>::max() / exchanges) {
    throw std::length_error("market-state dimensions are too large");
  }
  return instruments * exchanges;
}

void sort_prices(std::vector<PriceView> &prices) {
  std::sort(prices.begin(), prices.end(),
            [](const PriceView &left, const PriceView &right) {
              if (left.price != right.price) {
                return left.price < right.price;
              }
              return left.exchange_id < right.exchange_id;
            });
}

void mix_checksum(std::uint64_t &hash, const std::uint64_t value) {
  constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
  for (unsigned int byte = 0; byte < 8; ++byte) {
    hash ^= (value >> (byte * 8U)) & 0xffU;
    hash *= kFnvPrime;
  }
}

template <typename Entries>
std::uint64_t calculate_checksum(
    const std::uint16_t exchange_count,
    const std::uint32_t instrument_count,
    const std::vector<SequenceNumber> &last_sequences,
    const Entries &entries) {
  constexpr std::uint64_t kFnvOffsetBasis = 14'695'981'039'346'656'037ULL;
  auto hash = kFnvOffsetBasis;

  mix_checksum(hash, exchange_count);
  mix_checksum(hash, instrument_count);
  for (const auto sequence : last_sequences) {
    mix_checksum(hash, sequence);
  }
  for (const auto &entry : entries) {
    mix_checksum(hash, entry.valid ? 1U : 0U);
    if (entry.valid) {
      mix_checksum(hash, static_cast<std::uint64_t>(entry.price));
      mix_checksum(hash, entry.last_sequence);
      mix_checksum(hash, entry.source_timestamp_ns);
    }
  }
  return hash;
}

} // namespace

GlobalMarketState::GlobalMarketState(const std::uint16_t exchange_count,
                                     const std::uint32_t instrument_count)
    : state_(exchange_count, instrument_count) {}

ApplyResult GlobalMarketState::apply(const MarketUpdate &update) {
  const std::unique_lock lock(state_mutex_);
  return state_.apply(update);
}

std::optional<QueryResult>
GlobalMarketState::query(const InstrumentId instrument_id) const {
  const std::shared_lock lock(state_mutex_);
  return state_.query(instrument_id);
}

MarketStats GlobalMarketState::stats() const {
  const std::shared_lock lock(state_mutex_);
  return state_.stats();
}

std::uint64_t GlobalMarketState::logical_checksum() const {
  const std::shared_lock lock(state_mutex_);
  return state_.logical_checksum();
}

StripedMarketState::StripedMarketState(const std::uint16_t exchange_count,
                                       const std::uint32_t instrument_count,
                                       const std::size_t stripe_count,
                                       const SortingMode sorting_mode)
    : exchange_count_(exchange_count), instrument_count_(instrument_count),
      stripe_count_(stripe_count), sorting_mode_(sorting_mode),
      entries_(checked_entry_count(exchange_count, instrument_count)),
      last_sequences_(exchange_count),
      sorted_entries_(sorting_mode == SortingMode::on_write ? entries_.size()
                                                            : 0),
      sorted_valid_counts_(sorting_mode == SortingMode::on_write
                               ? instrument_count
                               : 0),
      stripe_mutexes_(stripe_count == 0
                          ? nullptr
                          : std::make_unique<std::shared_mutex[]>(stripe_count)),
      sequence_mutexes_(std::make_unique<std::mutex[]>(exchange_count)) {
  if (stripe_count == 0) {
    throw std::invalid_argument("stripe count must be greater than zero");
  }
}

ApplyResult StripedMarketState::apply(const MarketUpdate &update) {
  stats_.received.fetch_add(1, std::memory_order_relaxed);

  if (update.exchange_id >= exchange_count_) {
    stats_.invalid.fetch_add(1, std::memory_order_relaxed);
    return {ApplyStatus::invalid_exchange, 0};
  }
  if (update.instrument_id >= instrument_count_) {
    stats_.invalid.fetch_add(1, std::memory_order_relaxed);
    return {ApplyStatus::invalid_instrument, 0};
  }
  if (update.price <= 0) {
    stats_.invalid.fetch_add(1, std::memory_order_relaxed);
    return {ApplyStatus::invalid_price, 0};
  }
  if (update.sequence == 0) {
    stats_.invalid.fetch_add(1, std::memory_order_relaxed);
    return {ApplyStatus::invalid_sequence, 0};
  }

  const std::unique_lock sequence_lock(sequence_mutexes_[update.exchange_id]);
  auto &last_sequence = last_sequences_[update.exchange_id];
  if (update.sequence == last_sequence) {
    stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
    return {ApplyStatus::duplicate, 0};
  }
  if (update.sequence < last_sequence) {
    stats_.stale.fetch_add(1, std::memory_order_relaxed);
    return {ApplyStatus::stale, 0};
  }

  const auto difference = update.sequence - last_sequence;
  const auto missing = difference > 1 ? difference - 1 : 0;
  const std::unique_lock stripe_lock(
      stripe_mutexes_[stripe_for(update.instrument_id)]);

  entries_[entry_index(update.instrument_id, update.exchange_id)] = PriceEntry{
      .price = update.price,
      .last_sequence = update.sequence,
      .source_timestamp_ns = update.source_timestamp_ns,
      .valid = true,
  };
  last_sequence = update.sequence;
  if (sorting_mode_ == SortingMode::on_write) {
    rebuild_sorted_view(update.instrument_id);
  }

  stats_.accepted.fetch_add(1, std::memory_order_relaxed);
  if (missing > 0) {
    stats_.gap_events.fetch_add(1, std::memory_order_relaxed);
    stats_.missing_sequences.fetch_add(missing, std::memory_order_relaxed);
    return {ApplyStatus::accepted_with_gap, missing};
  }
  return {ApplyStatus::accepted, 0};
}

std::optional<QueryResult>
StripedMarketState::query(const InstrumentId instrument_id) const {
  if (instrument_id >= instrument_count_) {
    return std::nullopt;
  }

  QueryResult result;
  result.instrument_id = instrument_id;
  result.prices.reserve(exchange_count_);
  result.missing_exchanges.reserve(exchange_count_);

  {
    const std::shared_lock stripe_lock(
        stripe_mutexes_[stripe_for(instrument_id)]);
    if (sorting_mode_ == SortingMode::on_write) {
      const auto begin = entry_index(instrument_id, 0);
      const auto count = sorted_valid_counts_[instrument_id];
      result.prices.insert(result.prices.end(), sorted_entries_.begin() + begin,
                           sorted_entries_.begin() + begin + count);
    }

    for (ExchangeId exchange_id = 0; exchange_id < exchange_count_;
         ++exchange_id) {
      const auto &entry = entries_[entry_index(instrument_id, exchange_id)];
      if (!entry.valid) {
        result.missing_exchanges.push_back(exchange_id);
      } else if (sorting_mode_ == SortingMode::on_read) {
        result.prices.push_back(PriceView{
            .exchange_id = exchange_id,
            .price = entry.price,
            .last_sequence = entry.last_sequence,
            .source_timestamp_ns = entry.source_timestamp_ns,
        });
      }
    }
  }

  if (sorting_mode_ == SortingMode::on_read) {
    sort_prices(result.prices);
  }
  return result;
}

MarketStats StripedMarketState::stats() const {
  return MarketStats{
      .received = stats_.received.load(std::memory_order_relaxed),
      .accepted = stats_.accepted.load(std::memory_order_relaxed),
      .duplicates = stats_.duplicates.load(std::memory_order_relaxed),
      .stale = stats_.stale.load(std::memory_order_relaxed),
      .gap_events = stats_.gap_events.load(std::memory_order_relaxed),
      .missing_sequences =
          stats_.missing_sequences.load(std::memory_order_relaxed),
      .invalid = stats_.invalid.load(std::memory_order_relaxed),
  };
}

std::uint64_t StripedMarketState::logical_checksum() const {
  // Updates always acquire sequence first and stripe second. Taking every lock
  // in that same order produces one coherent full-state snapshot.
  std::vector<std::unique_lock<std::mutex>> sequence_locks;
  sequence_locks.reserve(exchange_count_);
  for (ExchangeId exchange_id = 0; exchange_id < exchange_count_;
       ++exchange_id) {
    sequence_locks.emplace_back(sequence_mutexes_[exchange_id]);
  }

  std::vector<std::shared_lock<std::shared_mutex>> stripe_locks;
  stripe_locks.reserve(stripe_count_);
  for (std::size_t stripe = 0; stripe < stripe_count_; ++stripe) {
    stripe_locks.emplace_back(stripe_mutexes_[stripe]);
  }

  return calculate_checksum(exchange_count_, instrument_count_,
                            last_sequences_, entries_);
}

std::size_t
StripedMarketState::entry_index(const InstrumentId instrument_id,
                                const ExchangeId exchange_id) const noexcept {
  return static_cast<std::size_t>(instrument_id) * exchange_count_ +
         exchange_id;
}

std::size_t
StripedMarketState::stripe_for(const InstrumentId instrument_id) const
    noexcept {
  return instrument_id % stripe_count_;
}

void StripedMarketState::rebuild_sorted_view(
    const InstrumentId instrument_id) {
  const auto begin = entry_index(instrument_id, 0);
  auto count = std::uint16_t{};
  for (ExchangeId exchange_id = 0; exchange_id < exchange_count_;
       ++exchange_id) {
    const auto &entry = entries_[begin + exchange_id];
    if (entry.valid) {
      sorted_entries_[begin + count] = PriceView{
          .exchange_id = exchange_id,
          .price = entry.price,
          .last_sequence = entry.last_sequence,
          .source_timestamp_ns = entry.source_timestamp_ns,
      };
      ++count;
    }
  }

  auto first = sorted_entries_.begin() + begin;
  std::sort(first, first + count,
            [](const PriceView &left, const PriceView &right) {
              if (left.price != right.price) {
                return left.price < right.price;
              }
              return left.exchange_id < right.exchange_id;
            });
  sorted_valid_counts_[instrument_id] = count;
}

} // namespace exchangelab
