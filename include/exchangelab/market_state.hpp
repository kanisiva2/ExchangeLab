#pragma once

#include "exchangelab/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace exchangelab {

class MarketState {
public:
  MarketState(std::uint16_t exchange_count, std::uint32_t instrument_count);

  [[nodiscard]] ApplyResult apply(const MarketUpdate &update);
  [[nodiscard]] std::optional<QueryResult>
  query(InstrumentId instrument_id) const;
  [[nodiscard]] std::uint64_t logical_checksum() const;

  [[nodiscard]] std::uint16_t exchange_count() const noexcept;
  [[nodiscard]] std::uint32_t instrument_count() const noexcept;
  [[nodiscard]] const MarketStats &stats() const noexcept;

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
};

} // namespace exchangelab
