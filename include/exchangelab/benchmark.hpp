#pragma once

#include "exchangelab/market_server.hpp"
#include "exchangelab/simulator.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exchangelab {

enum class BenchmarkLevel {
  direct,
  tcp,
};

struct BenchmarkCase {
  std::string suite;
  BenchmarkLevel level{BenchmarkLevel::direct};
  MarketServerMode mode{MarketServerMode::single_threaded};
  std::uint32_t update_percent{50};
  TrafficDistribution distribution{TrafficDistribution::uniform};
  std::size_t query_worker_count{4};
  std::size_t concurrent_client_count{16};
};

struct BenchmarkSettings {
  std::uint64_t measurement_operations{1'000'000};
  std::uint64_t warmup_operations{100'000};
  std::size_t repetitions{5};
  std::uint64_t seed{42};
  std::uint16_t exchange_count{10};
  std::uint32_t instrument_count{50'000};
  std::size_t stripe_count{64};
};

struct BenchmarkResult {
  std::uint32_t schema_version{1};
  std::string level;
  std::string suite;
  std::string mode;
  std::size_t repetition{};
  std::uint64_t seed{};
  std::uint16_t exchange_count{};
  std::uint32_t instrument_count{};
  std::uint64_t measurement_operations{};
  std::uint64_t requested_updates{};
  std::uint64_t requested_queries{};
  std::uint64_t successful_updates{};
  std::uint64_t successful_queries{};
  std::uint32_t update_percent{};
  std::uint32_t query_percent{};
  std::string distribution;
  std::size_t writer_count{};
  std::size_t concurrent_client_count{};
  std::size_t query_worker_count{};
  std::size_t stripe_count{};
  std::uint64_t warmup_operations{};
  double warmup_seconds{};
  double measurement_seconds{};
  double updates_per_second{};
  double queries_per_second{};
  double query_p50_us{};
  double query_p95_us{};
  double query_p99_us{};
  std::uint64_t errors{};
  std::uint64_t rejected_requests{};
  std::uint64_t queue_full_responses{};
  std::uint64_t update_frames_rejected{};
  std::uint64_t logical_checksum{};
  std::uint64_t oracle_checksum{};
  bool correct{};
  std::string build_type;
  std::string compiler;
  std::string compiler_version;
  std::string system_name;
  std::string system_version;
  std::string architecture;
  std::string cpu_model;
  unsigned int logical_cpu_count{};
  std::string git_revision;
  bool git_dirty{};
};

struct LatencyPercentiles {
  std::uint64_t p50_ns{};
  std::uint64_t p95_ns{};
  std::uint64_t p99_ns{};

  bool operator==(const LatencyPercentiles &) const = default;
};

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t>
split_operations(std::uint64_t total, std::uint32_t update_percent);

[[nodiscard]] LatencyPercentiles
calculate_latency_percentiles(std::vector<std::uint64_t> samples);

[[nodiscard]] std::vector<BenchmarkCase>
benchmark_cases(std::string_view suite);

[[nodiscard]] BenchmarkResult
run_benchmark(const BenchmarkCase &benchmark_case,
              const BenchmarkSettings &settings, std::size_t repetition,
              bool full_verification);

void write_benchmark_csv(const std::filesystem::path &path,
                         const std::vector<BenchmarkResult> &results);

[[nodiscard]] bool benchmark_build_is_release() noexcept;

} // namespace exchangelab
