#pragma once

#include <cstdint>
#include <vector>

namespace exchangelab {

using ExchangeId = std::uint16_t;
using InstrumentId = std::uint32_t;
using Price = std::int64_t;
using SequenceNumber = std::uint64_t;
using TimestampNs = std::uint64_t;

inline constexpr Price kPriceScale = 1'000'000;

struct MarketUpdate {
  ExchangeId exchange_id{};
  InstrumentId instrument_id{};
  Price price{};
  SequenceNumber sequence{};
  TimestampNs source_timestamp_ns{};

  bool operator==(const MarketUpdate &) const = default;
};

enum class ApplyStatus {
  accepted,
  accepted_with_gap,
  duplicate,
  stale,
  invalid_exchange,
  invalid_instrument,
  invalid_price,
  invalid_sequence,
};

struct ApplyResult {
  ApplyStatus status{};
  std::uint64_t missing_sequence_count{};
};

struct PriceView {
  ExchangeId exchange_id{};
  Price price{};
  SequenceNumber last_sequence{};
  TimestampNs source_timestamp_ns{};

  bool operator==(const PriceView &) const = default;
};

struct QueryResult {
  InstrumentId instrument_id{};
  std::vector<PriceView> prices;
  std::vector<ExchangeId> missing_exchanges;

  bool operator==(const QueryResult &) const = default;
};

struct MarketStats {
  std::uint64_t received{};
  std::uint64_t accepted{};
  std::uint64_t duplicates{};
  std::uint64_t stale{};
  std::uint64_t gap_events{};
  std::uint64_t missing_sequences{};
  std::uint64_t invalid{};

  bool operator==(const MarketStats &) const = default;
};

} // namespace exchangelab
