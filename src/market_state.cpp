#include "exchangelab/market_state.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

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

void mix_checksum(std::uint64_t &hash, const std::uint64_t value) {
  constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
  for (unsigned int byte = 0; byte < 8; ++byte) {
    hash ^= (value >> (byte * 8U)) & 0xffU;
    hash *= kFnvPrime;
  }
}

} // namespace

MarketState::MarketState(const std::uint16_t exchange_count,
                         const std::uint32_t instrument_count)
    : exchange_count_(exchange_count), instrument_count_(instrument_count),
      entries_(checked_entry_count(exchange_count, instrument_count)),
      last_sequences_(exchange_count) {}

ApplyResult MarketState::apply(const MarketUpdate &update) {
  ++stats_.received;

  if (update.exchange_id >= exchange_count_) {
    ++stats_.invalid;
    return {ApplyStatus::invalid_exchange, 0};
  }
  if (update.instrument_id >= instrument_count_) {
    ++stats_.invalid;
    return {ApplyStatus::invalid_instrument, 0};
  }
  if (update.price <= 0) {
    ++stats_.invalid;
    return {ApplyStatus::invalid_price, 0};
  }
  if (update.sequence == 0) {
    ++stats_.invalid;
    return {ApplyStatus::invalid_sequence, 0};
  }

  auto &last_sequence = last_sequences_[update.exchange_id];
  if (update.sequence == last_sequence) {
    ++stats_.duplicates;
    return {ApplyStatus::duplicate, 0};
  }
  if (update.sequence < last_sequence) {
    ++stats_.stale;
    return {ApplyStatus::stale, 0};
  }

  const auto sequence_difference = update.sequence - last_sequence;
  const auto missing_sequence_count =
      sequence_difference > 1 ? sequence_difference - 1 : 0;

  last_sequence = update.sequence;
  entries_[entry_index(update.instrument_id, update.exchange_id)] = PriceEntry{
      .price = update.price,
      .last_sequence = update.sequence,
      .source_timestamp_ns = update.source_timestamp_ns,
      .valid = true,
  };

  ++stats_.accepted;
  if (missing_sequence_count > 0) {
    ++stats_.gap_events;
    stats_.missing_sequences += missing_sequence_count;
    return {ApplyStatus::accepted_with_gap, missing_sequence_count};
  }
  return {ApplyStatus::accepted, 0};
}

std::optional<QueryResult>
MarketState::query(const InstrumentId instrument_id) const {
  if (instrument_id >= instrument_count_) {
    return std::nullopt;
  }

  QueryResult result;
  result.instrument_id = instrument_id;
  result.prices.reserve(exchange_count_);
  result.missing_exchanges.reserve(exchange_count_);

  for (ExchangeId exchange_id = 0; exchange_id < exchange_count_;
       ++exchange_id) {
    const auto &entry = entries_[entry_index(instrument_id, exchange_id)];
    if (entry.valid) {
      result.prices.push_back(PriceView{
          .exchange_id = exchange_id,
          .price = entry.price,
          .last_sequence = entry.last_sequence,
          .source_timestamp_ns = entry.source_timestamp_ns,
      });
    } else {
      result.missing_exchanges.push_back(exchange_id);
    }
  }

  std::sort(result.prices.begin(), result.prices.end(),
            [](const PriceView &left, const PriceView &right) {
              if (left.price != right.price) {
                return left.price < right.price;
              }
              return left.exchange_id < right.exchange_id;
            });

  return result;
}

std::uint64_t MarketState::logical_checksum() const {
  constexpr std::uint64_t kFnvOffsetBasis = 14'695'981'039'346'656'037ULL;
  auto hash = kFnvOffsetBasis;

  mix_checksum(hash, exchange_count_);
  mix_checksum(hash, instrument_count_);
  for (const auto sequence : last_sequences_) {
    mix_checksum(hash, sequence);
  }

  for (const auto &entry : entries_) {
    mix_checksum(hash, entry.valid ? 1U : 0U);
    if (entry.valid) {
      mix_checksum(hash, static_cast<std::uint64_t>(entry.price));
      mix_checksum(hash, entry.last_sequence);
      mix_checksum(hash, entry.source_timestamp_ns);
    }
  }

  return hash;
}

std::uint16_t MarketState::exchange_count() const noexcept {
  return exchange_count_;
}

std::uint32_t MarketState::instrument_count() const noexcept {
  return instrument_count_;
}

const MarketStats &MarketState::stats() const noexcept { return stats_; }

std::size_t
MarketState::entry_index(const InstrumentId instrument_id,
                         const ExchangeId exchange_id) const noexcept {
  return static_cast<std::size_t>(instrument_id) * exchange_count_ +
         exchange_id;
}

} // namespace exchangelab
