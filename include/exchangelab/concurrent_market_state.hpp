#pragma once

#include "exchangelab/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace exchangelab {

enum class SortingMode {
  on_read,
  on_write,
};

class ConcurrentMarketState {
public:
  virtual ~ConcurrentMarketState() = default;

  [[nodiscard]] virtual ApplyResult apply(const MarketUpdate &update) = 0;
  [[nodiscard]] virtual std::optional<QueryResult>
  query(InstrumentId instrument_id) const = 0;
  [[nodiscard]] virtual MarketStats stats() const = 0;
  [[nodiscard]] virtual std::uint64_t logical_checksum() const = 0;
};

class GlobalMarketState final : public ConcurrentMarketState {
public:
  GlobalMarketState(std::uint16_t exchange_count,
                    std::uint32_t instrument_count);

  [[nodiscard]] ApplyResult apply(const MarketUpdate &update) override;
  [[nodiscard]] std::optional<QueryResult>
  query(InstrumentId instrument_id) const override;
  [[nodiscard]] MarketStats stats() const override;
  [[nodiscard]] std::uint64_t logical_checksum() const override;

private:
  struct PriceEntry {
    Price price{};
    SequenceNumber last_sequence{};
    TimestampNs source_timestamp_ns{};
    bool valid{};
  };

  [[nodiscard]] std::size_t entry_index(InstrumentId instrument_id,
                                        ExchangeId exchange_id) const noexcept;

  std::uint16_t exchange_count_;
  std::uint32_t instrument_count_;
  std::vector<PriceEntry> entries_;
  std::vector<SequenceNumber> last_sequences_;
  MarketStats stats_;
  mutable std::shared_mutex state_mutex_;
};

class StripedMarketState final : public ConcurrentMarketState {
public:
  StripedMarketState(std::uint16_t exchange_count,
                     std::uint32_t instrument_count, std::size_t stripe_count,
                     SortingMode sorting_mode);

  [[nodiscard]] ApplyResult apply(const MarketUpdate &update) override;
  [[nodiscard]] std::optional<QueryResult>
  query(InstrumentId instrument_id) const override;
  [[nodiscard]] MarketStats stats() const override;
  [[nodiscard]] std::uint64_t logical_checksum() const override;

  [[nodiscard]] std::size_t stripe_count() const noexcept;
  [[nodiscard]] SortingMode sorting_mode() const noexcept;

private:
  struct PriceEntry {
    Price price{};
    SequenceNumber last_sequence{};
    TimestampNs source_timestamp_ns{};
    bool valid{};
  };

  struct AtomicStats {
    std::atomic<std::uint64_t> received{};
    std::atomic<std::uint64_t> accepted{};
    std::atomic<std::uint64_t> duplicates{};
    std::atomic<std::uint64_t> stale{};
    std::atomic<std::uint64_t> gap_events{};
    std::atomic<std::uint64_t> missing_sequences{};
    std::atomic<std::uint64_t> invalid{};
  };

  [[nodiscard]] std::size_t entry_index(InstrumentId instrument_id,
                                        ExchangeId exchange_id) const noexcept;
  [[nodiscard]] std::size_t stripe_for(InstrumentId instrument_id) const
      noexcept;
  void rebuild_sorted_view(InstrumentId instrument_id);

  std::uint16_t exchange_count_;
  std::uint32_t instrument_count_;
  std::size_t stripe_count_;
  SortingMode sorting_mode_;
  std::vector<PriceEntry> entries_;
  std::vector<SequenceNumber> last_sequences_;
  std::vector<PriceView> sorted_entries_;
  std::vector<std::uint16_t> sorted_valid_counts_;
  std::unique_ptr<std::shared_mutex[]> stripe_mutexes_;
  std::unique_ptr<std::mutex[]> sequence_mutexes_;
  AtomicStats stats_;
};

} // namespace exchangelab
