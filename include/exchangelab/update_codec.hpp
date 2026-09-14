#pragma once

#include "exchangelab/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace exchangelab {

// Version 1 update frame, with no compiler padding:
//   byte 0       protocol version
//   bytes 1-2    exchange ID
//   bytes 3-6    instrument ID
//   bytes 7-14   positive fixed-point price in micros
//   bytes 15-22  sequence number
//   bytes 23-30  source timestamp in nanoseconds
// Every multi-byte field is stored most-significant byte first (network order).
inline constexpr std::uint8_t kUpdateProtocolVersion = 1;
inline constexpr std::size_t kUpdateFrameSize = 31;
using UpdateFrame = std::array<std::uint8_t, kUpdateFrameSize>;

struct UpdateLimits {
  std::uint16_t exchange_count{};
  std::uint32_t instrument_count{};
};

enum class UpdateDecodeError {
  none,
  wrong_size,
  unsupported_version,
  invalid_exchange,
  invalid_instrument,
  invalid_price,
  invalid_sequence,
};

struct UpdateDecodeResult {
  std::optional<MarketUpdate> update;
  UpdateDecodeError error{UpdateDecodeError::none};
};

[[nodiscard]] UpdateFrame encode_update(const MarketUpdate &update);
[[nodiscard]] UpdateDecodeResult
decode_update(std::span<const std::uint8_t> frame, UpdateLimits limits);
[[nodiscard]] std::string_view
update_decode_error_name(UpdateDecodeError error) noexcept;

} // namespace exchangelab
