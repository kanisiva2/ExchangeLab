#pragma once

#include "exchangelab/update_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace exchangelab {

struct UpdateDecodeBatch {
  std::vector<MarketUpdate> updates;
  std::vector<UpdateDecodeError> errors;
};

class UpdateStreamDecoder {
public:
  explicit UpdateStreamDecoder(UpdateLimits limits);

  [[nodiscard]] UpdateDecodeBatch append(std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::size_t buffered_byte_count() const noexcept;
  [[nodiscard]] bool finish();

private:
  UpdateLimits limits_;
  std::vector<std::uint8_t> buffered_bytes_;
};

} // namespace exchangelab
