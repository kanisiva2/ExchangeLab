#include "exchangelab/update_codec.hpp"

#include <limits>

namespace exchangelab {
namespace {

template <typename Unsigned>
void write_unsigned(UpdateFrame &frame, std::size_t offset, Unsigned value) {
  for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
    const auto shift = (sizeof(Unsigned) - index - 1U) * 8U;
    frame[offset + index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
}

template <typename Unsigned>
Unsigned read_unsigned(std::span<const std::uint8_t> frame,
                       std::size_t offset) {
  Unsigned value{};
  for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
    value = static_cast<Unsigned>((value << 8U) | frame[offset + index]);
  }
  return value;
}

} // namespace

UpdateFrame encode_update(const MarketUpdate &update) {
  UpdateFrame frame{};
  frame[0] = kUpdateProtocolVersion;
  write_unsigned(frame, 1, update.exchange_id);
  write_unsigned(frame, 3, update.instrument_id);
  write_unsigned(frame, 7, static_cast<std::uint64_t>(update.price));
  write_unsigned(frame, 15, update.sequence);
  write_unsigned(frame, 23, update.source_timestamp_ns);
  return frame;
}

UpdateDecodeResult decode_update(const std::span<const std::uint8_t> frame,
                                 const UpdateLimits limits) {
  if (frame.size() != kUpdateFrameSize) {
    return {.error = UpdateDecodeError::wrong_size};
  }
  if (frame[0] != kUpdateProtocolVersion) {
    return {.error = UpdateDecodeError::unsupported_version};
  }

  const auto exchange_id = read_unsigned<ExchangeId>(frame, 1);
  const auto instrument_id = read_unsigned<InstrumentId>(frame, 3);
  const auto encoded_price = read_unsigned<std::uint64_t>(frame, 7);
  const auto sequence = read_unsigned<SequenceNumber>(frame, 15);
  const auto source_timestamp_ns = read_unsigned<TimestampNs>(frame, 23);

  if (exchange_id >= limits.exchange_count) {
    return {.error = UpdateDecodeError::invalid_exchange};
  }
  if (instrument_id >= limits.instrument_count) {
    return {.error = UpdateDecodeError::invalid_instrument};
  }
  if (encoded_price == 0 ||
      encoded_price >
          static_cast<std::uint64_t>(std::numeric_limits<Price>::max())) {
    return {.error = UpdateDecodeError::invalid_price};
  }
  if (sequence == 0) {
    return {.error = UpdateDecodeError::invalid_sequence};
  }

  return {
      .update =
          MarketUpdate{
              .exchange_id = exchange_id,
              .instrument_id = instrument_id,
              .price = static_cast<Price>(encoded_price),
              .sequence = sequence,
              .source_timestamp_ns = source_timestamp_ns,
          },
      .error = UpdateDecodeError::none,
  };
}

} // namespace exchangelab
