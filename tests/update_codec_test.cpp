#include "exchangelab/update_codec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace exchangelab {
namespace {

TEST(UpdateCodecTest, EncodesKnownValuesInNetworkByteOrder) {
  const auto frame = encode_update(MarketUpdate{
      .exchange_id = 0x1234,
      .instrument_id = 0x01020304,
      .price = 0x0102030405060708,
      .sequence = 0x1112131415161718,
      .source_timestamp_ns = 0x2122232425262728,
  });

  const UpdateFrame expected{
      0x01, 0x12, 0x34, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04,
      0x05, 0x06, 0x07, 0x08, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
  };
  EXPECT_EQ(frame, expected);
}

TEST(UpdateCodecTest, DecodesAnEncodedUpdate) {
  const MarketUpdate expected{3, 1234, 101'250'000, 99, 7654321};
  const auto result = decode_update(encode_update(expected), {10, 50'000});

  ASSERT_TRUE(result.update.has_value());
  EXPECT_EQ(*result.update, expected);
  EXPECT_EQ(result.error, UpdateDecodeError::none);
}

TEST(UpdateCodecTest, RejectsWrongSizeAndInvalidFields) {
  const MarketUpdate valid{1, 2, 100'000'000, 1, 10};
  auto frame = encode_update(valid);

  EXPECT_EQ(
      decode_update(std::span(frame).first(frame.size() - 1), {10, 100}).error,
      UpdateDecodeError::wrong_size);

  frame[0] = 2;
  EXPECT_EQ(decode_update(frame, {10, 100}).error,
            UpdateDecodeError::unsupported_version);

  EXPECT_EQ(
      decode_update(encode_update(MarketUpdate{10, 2, 1, 1, 0}), {10, 100})
          .error,
      UpdateDecodeError::invalid_exchange);
  EXPECT_EQ(
      decode_update(encode_update(MarketUpdate{1, 100, 1, 1, 0}), {10, 100})
          .error,
      UpdateDecodeError::invalid_instrument);
  EXPECT_EQ(decode_update(encode_update(MarketUpdate{1, 2, 0, 1, 0}), {10, 100})
                .error,
            UpdateDecodeError::invalid_price);
  EXPECT_EQ(decode_update(encode_update(MarketUpdate{1, 2, 1, 0, 0}), {10, 100})
                .error,
            UpdateDecodeError::invalid_sequence);
}

} // namespace
} // namespace exchangelab
