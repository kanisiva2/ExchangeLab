#include "exchangelab/concurrent_market_state.hpp"
#include "exchangelab/market_state.hpp"
#include "exchangelab/simulator.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

namespace exchangelab {
namespace {

std::vector<std::unique_ptr<ConcurrentMarketState>> concurrent_engines(
    const std::uint16_t exchange_count,
    const std::uint32_t instrument_count) {
  std::vector<std::unique_ptr<ConcurrentMarketState>> engines;
  engines.push_back(
      std::make_unique<GlobalMarketState>(exchange_count, instrument_count));
  engines.push_back(std::make_unique<StripedMarketState>(
      exchange_count, instrument_count, 8, SortingMode::on_read));
  engines.push_back(std::make_unique<StripedMarketState>(
      exchange_count, instrument_count, 8, SortingMode::on_write));
  return engines;
}

TEST(ConcurrentMarketStateTest, RejectsInvalidDimensionsAndStripeCount) {
  EXPECT_THROW(GlobalMarketState(0, 10), std::invalid_argument);
  EXPECT_THROW(GlobalMarketState(2, 0), std::invalid_argument);
  EXPECT_THROW(StripedMarketState(2, 10, 0, SortingMode::on_read),
               std::invalid_argument);
}

TEST(ConcurrentMarketStateTest, EveryEngineMatchesTheReferenceSequentially) {
  const SimulatorConfig config{
      .exchange_count = 3,
      .instrument_count = 100,
      .event_count = 10'000,
      .seed = 8675309,
      .distribution = TrafficDistribution::hot,
  };
  MarketState reference(config.exchange_count, config.instrument_count);
  auto engines =
      concurrent_engines(config.exchange_count, config.instrument_count);
  Simulator simulator(config);

  MarketUpdate update;
  while (simulator.next(update)) {
    const auto expected = reference.apply(update);
    for (auto &engine : engines) {
      const auto actual = engine->apply(update);
      EXPECT_EQ(actual.status, expected.status);
      EXPECT_EQ(actual.missing_sequence_count,
                expected.missing_sequence_count);
    }
  }

  for (const auto &engine : engines) {
    EXPECT_EQ(engine->stats(), reference.stats());
    EXPECT_EQ(engine->logical_checksum(), reference.logical_checksum());
    for (InstrumentId instrument = 0;
         instrument < config.instrument_count; ++instrument) {
      EXPECT_EQ(engine->query(instrument), reference.query(instrument));
    }
  }
}

TEST(ConcurrentMarketStateTest, PreservesSequenceRulesInEveryEngine) {
  for (auto &engine : concurrent_engines(2, 3)) {
    EXPECT_EQ(engine->apply(MarketUpdate{0, 0, 100, 1, 10}).status,
              ApplyStatus::accepted);
    EXPECT_EQ(engine->apply(MarketUpdate{0, 1, 101, 1, 11}).status,
              ApplyStatus::duplicate);
    EXPECT_EQ(engine->apply(MarketUpdate{0, 1, 101, 3, 12}).status,
              ApplyStatus::accepted_with_gap);
    EXPECT_EQ(engine->apply(MarketUpdate{0, 2, 102, 2, 13}).status,
              ApplyStatus::stale);
    EXPECT_EQ(engine->apply(MarketUpdate{2, 0, 100, 1, 14}).status,
              ApplyStatus::invalid_exchange);
    EXPECT_EQ(engine->apply(MarketUpdate{1, 3, 100, 1, 15}).status,
              ApplyStatus::invalid_instrument);
    EXPECT_EQ(engine->apply(MarketUpdate{1, 0, 0, 1, 16}).status,
              ApplyStatus::invalid_price);
    EXPECT_EQ(engine->apply(MarketUpdate{1, 0, 100, 0, 17}).status,
              ApplyStatus::invalid_sequence);
  }
}

TEST(ConcurrentMarketStateTest, ConcurrentExchangeStreamsMatchTheReference) {
  const SimulatorConfig config{
      .exchange_count = 10,
      .instrument_count = 500,
      .event_count = 50'000,
      .seed = 42,
      .distribution = TrafficDistribution::hot,
  };
  MarketState reference(config.exchange_count, config.instrument_count);
  std::vector<std::vector<MarketUpdate>> exchange_streams(
      config.exchange_count);
  Simulator simulator(config);
  MarketUpdate update;
  while (simulator.next(update)) {
    [[maybe_unused]] const auto result = reference.apply(update);
    exchange_streams[update.exchange_id].push_back(update);
  }

  for (auto &engine : concurrent_engines(config.exchange_count,
                                         config.instrument_count)) {
    std::vector<std::thread> writers;
    for (ExchangeId exchange = 0; exchange < config.exchange_count;
         ++exchange) {
      writers.emplace_back([&engine, &exchange_streams, exchange] {
        for (const auto &item : exchange_streams[exchange]) {
          [[maybe_unused]] const auto result = engine->apply(item);
        }
      });
    }
    for (auto &writer : writers) {
      writer.join();
    }

    EXPECT_EQ(engine->stats(), reference.stats());
    EXPECT_EQ(engine->logical_checksum(), reference.logical_checksum());
  }
}

TEST(ConcurrentMarketStateTest, QueriesObserveCompletePriceEntries) {
  constexpr SequenceNumber kUpdates = 20'000;

  for (auto &engine : concurrent_engines(1, 1)) {
    std::thread writer([&engine] {
      for (SequenceNumber sequence = 1; sequence <= kUpdates; ++sequence) {
        [[maybe_unused]] const auto result = engine->apply(MarketUpdate{
            0, 0, static_cast<Price>(sequence), sequence, sequence * 10});
      }
    });

    for (std::size_t attempt = 0; attempt < 20'000; ++attempt) {
      const auto result = engine->query(0);
      if (!result.has_value()) {
        ADD_FAILURE() << "in-range query unexpectedly failed";
        continue;
      }
      if (!result->prices.empty()) {
        const auto &price = result->prices.front();
        EXPECT_EQ(price.price, static_cast<Price>(price.last_sequence));
        EXPECT_EQ(price.source_timestamp_ns, price.last_sequence * 10);
      }
    }
    writer.join();
  }
}

} // namespace
} // namespace exchangelab
