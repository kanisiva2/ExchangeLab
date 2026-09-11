#include "exchangelab/simulator.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace exchangelab {
namespace {

std::size_t checked_entry_count(const SimulatorConfig &config) {
  if (config.exchange_count == 0) {
    throw std::invalid_argument("exchange count must be greater than zero");
  }
  if (config.instrument_count == 0) {
    throw std::invalid_argument("instrument count must be greater than zero");
  }
  if (config.event_count > std::numeric_limits<TimestampNs>::max() / 1'000) {
    throw std::invalid_argument(
        "event count is too large for logical timestamps");
  }

  const auto exchanges = static_cast<std::size_t>(config.exchange_count);
  const auto instruments = static_cast<std::size_t>(config.instrument_count);
  if (instruments > std::numeric_limits<std::size_t>::max() / exchanges) {
    throw std::length_error("simulator dimensions are too large");
  }
  return instruments * exchanges;
}

} // namespace

Simulator::Simulator(SimulatorConfig config)
    : config_(config), random_(config.seed), sequences_(config.exchange_count),
      prices_(checked_entry_count(config)) {
  for (InstrumentId instrument_id = 0; instrument_id < config_.instrument_count;
       ++instrument_id) {
    for (ExchangeId exchange_id = 0; exchange_id < config_.exchange_count;
         ++exchange_id) {
      const auto index =
          static_cast<std::size_t>(instrument_id) * config_.exchange_count +
          exchange_id;
      prices_[index] = 100 * kPriceScale +
                       static_cast<Price>(instrument_id % 1'000) * 1'000 +
                       static_cast<Price>(exchange_id) * 10'000;
    }
  }
}

bool Simulator::next(MarketUpdate &output) {
  if (generated_events_ >= config_.event_count) {
    return false;
  }

  const auto exchange_id =
      static_cast<ExchangeId>(generated_events_ % config_.exchange_count);
  const auto instrument_id = next_instrument();
  const auto index =
      static_cast<std::size_t>(instrument_id) * config_.exchange_count +
      exchange_id;

  constexpr Price kStepMicros = 2'500;
  const auto step = static_cast<Price>(random_bounded(5)) - 2;
  prices_[index] = std::clamp(prices_[index] + step * kStepMicros,
                              1 * kPriceScale, 1'000'000 * kPriceScale);

  output = MarketUpdate{
      .exchange_id = exchange_id,
      .instrument_id = instrument_id,
      .price = prices_[index],
      .sequence = ++sequences_[exchange_id],
      .source_timestamp_ns = generated_events_ * 1'000,
  };

  ++generated_events_;
  return true;
}

std::uint64_t Simulator::random_bounded(const std::uint64_t upper_bound) {
  return random_() % upper_bound;
}

InstrumentId Simulator::next_instrument() {
  if (config_.distribution == TrafficDistribution::hot &&
      random_bounded(100) < 80) {
    const auto hot_instrument_count =
        std::max<std::uint32_t>(1, config_.instrument_count / 100);
    return static_cast<InstrumentId>(random_bounded(hot_instrument_count));
  }
  return static_cast<InstrumentId>(random_bounded(config_.instrument_count));
}

} // namespace exchangelab
