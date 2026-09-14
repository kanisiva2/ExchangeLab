#include "exchangelab/update_stream_decoder.hpp"

namespace exchangelab {

UpdateStreamDecoder::UpdateStreamDecoder(const UpdateLimits limits)
    : limits_(limits) {
  buffered_bytes_.reserve(kUpdateFrameSize * 2);
}

UpdateDecodeBatch
UpdateStreamDecoder::append(const std::span<const std::uint8_t> bytes) {
  buffered_bytes_.insert(buffered_bytes_.end(), bytes.begin(), bytes.end());

  UpdateDecodeBatch batch;
  std::size_t consumed{};
  while (buffered_bytes_.size() - consumed >= kUpdateFrameSize) {
    const auto frame = std::span<const std::uint8_t>(buffered_bytes_)
                           .subspan(consumed, kUpdateFrameSize);
    auto result = decode_update(frame, limits_);
    if (result.update.has_value()) {
      batch.updates.push_back(*result.update);
    } else {
      batch.errors.push_back(result.error);
    }
    consumed += kUpdateFrameSize;
  }

  if (consumed > 0) {
    buffered_bytes_.erase(buffered_bytes_.begin(),
                          buffered_bytes_.begin() +
                              static_cast<std::ptrdiff_t>(consumed));
  }
  return batch;
}

std::size_t UpdateStreamDecoder::buffered_byte_count() const noexcept {
  return buffered_bytes_.size();
}

bool UpdateStreamDecoder::finish() {
  const bool was_truncated = !buffered_bytes_.empty();
  buffered_bytes_.clear();
  return was_truncated;
}

} // namespace exchangelab
