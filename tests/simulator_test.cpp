#include "exchangelab/market_state.hpp"
#include "exchangelab/simulator.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace exchangelab {
namespace {

TEST(SimulatorTest, RejectsInvalidConfiguration) {
  EXPECT_THROW(Simulator(SimulatorConfig{.exchange_count = 0}),
               std::invalid_argument);
  EXPECT_THROW(Simulator(SimulatorConfig{.instrument_count = 0}),
               std::invalid_argument);
}

TEST(SimulatorTest, KnownSeedProducesExpectedUpdates) {
  Simulator simulator(SimulatorConfig{
      .exchange_count = 2,
      .instrument_count = 10,
      .event_count = 3,
      .seed = 42,
      .distribution = TrafficDistribution::uniform,
  });

  const std::vector<MarketUpdate> expected{
      MarketUpdate{0, 6, 100'011'000, 1, 0},
      MarketUpdate{1, 0, 100'010'000, 1, 1'000},
      MarketUpdate{0, 1, 100'003'500, 2, 2'000},
  };

  for (const auto &expected_update : expected) {
    MarketUpdate actual;
    ASSERT_TRUE(simulator.next(actual));
    EXPECT_EQ(actual, expected_update);
  }

  MarketUpdate exhausted;
  EXPECT_FALSE(simulator.next(exhausted));
}

TEST(SimulatorTest, SameSeedProducesSameStream) {
  const SimulatorConfig config{
      .exchange_count = 3,
      .instrument_count = 100,
      .event_count = 1'000,
      .seed = 8675309,
      .distribution = TrafficDistribution::hot,
  };
  Simulator first(config);
  Simulator second(config);

  MarketUpdate first_update;
  MarketUpdate second_update;
  for (std::uint64_t index = 0; index < config.event_count; ++index) {
    ASSERT_TRUE(first.next(first_update));
    ASSERT_TRUE(second.next(second_update));
    EXPECT_EQ(first_update, second_update);
  }
}

TEST(SimulatorTest, HotDistributionConcentratesMostUpdatesInOnePercent) {
  const SimulatorConfig config{
      .exchange_count = 2,
      .instrument_count = 1'000,
      .event_count = 10'000,
      .seed = 7,
      .distribution = TrafficDistribution::hot,
  };
  Simulator simulator(config);
  std::uint64_t hot_updates{};

  MarketUpdate update;
  while (simulator.next(update)) {
    if (update.instrument_id < 10) {
      ++hot_updates;
    }
  }

  EXPECT_GT(hot_updates, 7'500U);
}

TEST(SimulatorTest, DefaultScaleRunIsRepeatable) {
  const SimulatorConfig config;
  Simulator first_simulator(config);
  Simulator second_simulator(config);
  MarketState first_state(config.exchange_count, config.instrument_count);
  MarketState second_state(config.exchange_count, config.instrument_count);

  MarketUpdate update;
  while (first_simulator.next(update)) {
    [[maybe_unused]] const auto result = first_state.apply(update);
  }
  while (second_simulator.next(update)) {
    [[maybe_unused]] const auto result = second_state.apply(update);
  }

  EXPECT_EQ(first_state.stats(), second_state.stats());
  EXPECT_EQ(first_state.logical_checksum(), second_state.logical_checksum());
  EXPECT_EQ(first_state.stats().accepted, config.event_count);
}

} // namespace
} // namespace exchangelab
