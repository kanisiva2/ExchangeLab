#include "exchangelab/market_state.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

namespace exchangelab {
namespace {

MarketUpdate update(const ExchangeId exchange_id,
                    const InstrumentId instrument_id, const Price price,
                    const SequenceNumber sequence,
                    const TimestampNs timestamp = 100) {
  return MarketUpdate{
      .exchange_id = exchange_id,
      .instrument_id = instrument_id,
      .price = price,
      .sequence = sequence,
      .source_timestamp_ns = timestamp,
  };
}

TEST(MarketStateTest, RejectsZeroDimensions) {
  EXPECT_THROW(MarketState(0, 10), std::invalid_argument);
  EXPECT_THROW(MarketState(2, 0), std::invalid_argument);
}

TEST(MarketStateTest, AcceptsUpdateAndReturnsItFromQuery) {
  MarketState state(2, 3);

  const auto result = state.apply(update(1, 2, 101 * kPriceScale, 1, 500));
  ASSERT_EQ(result.status, ApplyStatus::accepted);

  const auto query_result = state.query(2);
  ASSERT_TRUE(query_result.has_value());
  ASSERT_EQ(query_result->prices.size(), 1U);
  EXPECT_EQ(query_result->prices.front(), (PriceView{
                                              .exchange_id = 1,
                                              .price = 101 * kPriceScale,
                                              .last_sequence = 1,
                                              .source_timestamp_ns = 500,
                                          }));
  EXPECT_EQ(query_result->missing_exchanges, (std::vector<ExchangeId>{0}));
}

TEST(MarketStateTest, TracksSequenceAcrossAllInstrumentsForAnExchange) {
  MarketState state(2, 3);

  EXPECT_EQ(state.apply(update(0, 0, 100 * kPriceScale, 1)).status,
            ApplyStatus::accepted);
  EXPECT_EQ(state.apply(update(0, 1, 101 * kPriceScale, 2)).status,
            ApplyStatus::accepted);
  EXPECT_EQ(state.apply(update(0, 2, 102 * kPriceScale, 2)).status,
            ApplyStatus::duplicate);
  EXPECT_EQ(state.apply(update(0, 2, 102 * kPriceScale, 1)).status,
            ApplyStatus::stale);

  const auto instrument_two = state.query(2);
  ASSERT_TRUE(instrument_two.has_value());
  EXPECT_TRUE(instrument_two->prices.empty());
}

TEST(MarketStateTest, CountsAndAcceptsSequenceGaps) {
  MarketState state(1, 2);

  const auto result = state.apply(update(0, 1, 100 * kPriceScale, 4));

  EXPECT_EQ(result.status, ApplyStatus::accepted_with_gap);
  EXPECT_EQ(result.missing_sequence_count, 3U);
  EXPECT_EQ(state.stats().accepted, 1U);
  EXPECT_EQ(state.stats().gap_events, 1U);
  EXPECT_EQ(state.stats().missing_sequences, 3U);
}

TEST(MarketStateTest, InvalidUpdatesDoNotAdvanceSequenceOrChangePrices) {
  MarketState state(2, 2);
  const auto empty_checksum = state.logical_checksum();

  EXPECT_EQ(state.apply(update(2, 0, 100 * kPriceScale, 7)).status,
            ApplyStatus::invalid_exchange);
  EXPECT_EQ(state.apply(update(0, 2, 100 * kPriceScale, 7)).status,
            ApplyStatus::invalid_instrument);
  EXPECT_EQ(state.apply(update(0, 0, 0, 7)).status, ApplyStatus::invalid_price);
  EXPECT_EQ(state.apply(update(0, 0, 100 * kPriceScale, 0)).status,
            ApplyStatus::invalid_sequence);
  EXPECT_EQ(state.logical_checksum(), empty_checksum);

  const auto valid_result = state.apply(update(0, 0, 100 * kPriceScale, 1));
  EXPECT_EQ(valid_result.status, ApplyStatus::accepted);
  EXPECT_EQ(valid_result.missing_sequence_count, 0U);
  EXPECT_EQ(state.stats().invalid, 4U);
}

TEST(MarketStateTest, SortsByPriceThenExchangeIdAndReportsMissingExchanges) {
  MarketState state(4, 1);

  EXPECT_EQ(state.apply(update(0, 0, 102 * kPriceScale, 1)).status,
            ApplyStatus::accepted);
  EXPECT_EQ(state.apply(update(1, 0, 100 * kPriceScale, 1)).status,
            ApplyStatus::accepted);
  EXPECT_EQ(state.apply(update(2, 0, 100 * kPriceScale, 1)).status,
            ApplyStatus::accepted);

  const auto result = state.query(0);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->prices.size(), 3U);
  EXPECT_EQ(result->prices[0].exchange_id, 1);
  EXPECT_EQ(result->prices[1].exchange_id, 2);
  EXPECT_EQ(result->prices[2].exchange_id, 0);
  EXPECT_EQ(result->missing_exchanges, (std::vector<ExchangeId>{3}));
}

TEST(MarketStateTest, RejectsOutOfRangeQuery) {
  const MarketState state(2, 3);
  EXPECT_FALSE(state.query(3).has_value());
}

TEST(MarketStateTest, ChecksumTracksLogicalStateDeterministically) {
  MarketState first(2, 2);
  MarketState second(2, 2);

  EXPECT_EQ(first.logical_checksum(), second.logical_checksum());
  EXPECT_EQ(first.apply(update(0, 1, 100 * kPriceScale, 1)).status,
            ApplyStatus::accepted);
  EXPECT_NE(first.logical_checksum(), second.logical_checksum());
  EXPECT_EQ(second.apply(update(0, 1, 100 * kPriceScale, 1)).status,
            ApplyStatus::accepted);
  EXPECT_EQ(first.logical_checksum(), second.logical_checksum());
}

} // namespace
} // namespace exchangelab
