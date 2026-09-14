#include "exchangelab/update_stream_decoder.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace exchangelab {
namespace {

TEST(UpdateStreamDecoderTest, ReassemblesAFrameAtEverySplitBoundary) {
  const MarketUpdate expected{3, 1234, 101'250'000, 99, 123456};
  const auto frame = encode_update(expected);

  for (std::size_t split = 1; split < frame.size(); ++split) {
    UpdateStreamDecoder decoder({10, 50'000});
    const auto first = decoder.append(std::span(frame).first(split));
    EXPECT_TRUE(first.updates.empty()) << "split=" << split;
    EXPECT_TRUE(first.errors.empty()) << "split=" << split;

    const auto second = decoder.append(std::span(frame).subspan(split));
    ASSERT_EQ(second.updates.size(), 1U) << "split=" << split;
    EXPECT_EQ(second.updates.front(), expected) << "split=" << split;
    EXPECT_TRUE(second.errors.empty()) << "split=" << split;
    EXPECT_EQ(decoder.buffered_byte_count(), 0U) << "split=" << split;
  }
}

TEST(UpdateStreamDecoderTest, DecodesSeveralFramesFromOneChunk) {
  const std::vector<MarketUpdate> expected{
      MarketUpdate{0, 10, 100'000'000, 1, 1},
      MarketUpdate{1, 11, 101'000'000, 1, 2},
      MarketUpdate{0, 12, 102'000'000, 2, 3},
  };
  std::vector<std::uint8_t> bytes;
  for (const auto &update : expected) {
    const auto frame = encode_update(update);
    bytes.insert(bytes.end(), frame.begin(), frame.end());
  }

  UpdateStreamDecoder decoder({2, 100});
  const auto batch = decoder.append(bytes);
  EXPECT_EQ(batch.updates, expected);
  EXPECT_TRUE(batch.errors.empty());
  EXPECT_EQ(decoder.buffered_byte_count(), 0U);
}

TEST(UpdateStreamDecoderTest, PreservesAndReportsAPartialTail) {
  const auto first = encode_update(MarketUpdate{0, 10, 100'000'000, 1, 1});
  const auto second = encode_update(MarketUpdate{1, 11, 101'000'000, 1, 2});
  std::vector<std::uint8_t> bytes(first.begin(), first.end());
  bytes.insert(bytes.end(), second.begin(), second.begin() + 7);

  UpdateStreamDecoder decoder({2, 100});
  const auto batch = decoder.append(bytes);
  ASSERT_EQ(batch.updates.size(), 1U);
  EXPECT_EQ(batch.updates.front().instrument_id, 10U);
  EXPECT_EQ(decoder.buffered_byte_count(), 7U);
  EXPECT_TRUE(decoder.finish());
  EXPECT_EQ(decoder.buffered_byte_count(), 0U);
  EXPECT_FALSE(decoder.finish());
}

TEST(UpdateStreamDecoderTest, RejectsAnInvalidFrameAndKeepsItsAlignment) {
  auto invalid = encode_update(MarketUpdate{0, 10, 100'000'000, 1, 1});
  invalid[0] = 99;
  const auto valid = encode_update(MarketUpdate{1, 11, 101'000'000, 1, 2});
  std::vector<std::uint8_t> bytes(invalid.begin(), invalid.end());
  bytes.insert(bytes.end(), valid.begin(), valid.end());

  UpdateStreamDecoder decoder({2, 100});
  const auto batch = decoder.append(bytes);
  EXPECT_EQ(batch.errors, (std::vector<UpdateDecodeError>{
                              UpdateDecodeError::unsupported_version}));
  ASSERT_EQ(batch.updates.size(), 1U);
  EXPECT_EQ(batch.updates.front().instrument_id, 11U);
}

} // namespace
} // namespace exchangelab
