#pragma once

#include "exchangelab/types.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace exchangelab {

enum class TrafficDistribution {
  uniform,
  hot,
};

struct SimulatorConfig {
  std::uint16_t exchange_count{10};
  std::uint32_t instrument_count{50'000};
  std::uint64_t event_count{1'000'000};
  std::uint64_t seed{42};
  TrafficDistribution distribution{TrafficDistribution::uniform};
};

class Simulator {
public:
  explicit Simulator(SimulatorConfig config);

  [[nodiscard]] bool next(MarketUpdate &output);

private:
  [[nodiscard]] std::uint64_t random_bounded(std::uint64_t upper_bound);
  [[nodiscard]] InstrumentId next_instrument();

  SimulatorConfig config_;
  std::mt19937_64 random_;
  std::vector<SequenceNumber> sequences_;
  std::vector<Price> prices_;
  std::uint64_t generated_events_{};
};

} // namespace exchangelab
