#include "exchangelab/benchmark.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace exchangelab {
namespace {

TEST(BenchmarkTest, SplitsOperationsWithoutOverflow) {
  EXPECT_EQ(split_operations(100, 90),
            (std::pair<std::uint64_t, std::uint64_t>{90, 10}));
  EXPECT_EQ(split_operations(101, 50),
            (std::pair<std::uint64_t, std::uint64_t>{50, 51}));
  EXPECT_THROW(static_cast<void>(split_operations(0, 50)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(split_operations(100, 100)),
               std::invalid_argument);
}

TEST(BenchmarkTest, CalculatesNearestRankPercentiles) {
  std::vector<std::uint64_t> samples;
  for (std::uint64_t value = 1; value <= 100; ++value) {
    samples.push_back(value);
  }
  EXPECT_EQ(calculate_latency_percentiles(samples),
            (LatencyPercentiles{.p50_ns = 50, .p95_ns = 95, .p99_ns = 99}));
  EXPECT_THROW(static_cast<void>(calculate_latency_percentiles({})),
               std::invalid_argument);
}

TEST(BenchmarkTest, FocusedSuitesContainTheRequiredCases) {
  EXPECT_EQ(benchmark_cases("direct-locking").size(), 9U);
  EXPECT_EQ(benchmark_cases("direct-sorting").size(), 8U);
  EXPECT_EQ(benchmark_cases("direct-workers").size(), 3U);
  EXPECT_EQ(benchmark_cases("tcp-modes").size(), 4U);
  EXPECT_EQ(benchmark_cases("tcp-workers").size(), 3U);
  EXPECT_THROW(static_cast<void>(benchmark_cases("unknown")),
               std::invalid_argument);
}

TEST(BenchmarkTest, DirectModesPassTheOracleGate) {
  const BenchmarkSettings settings{
      .measurement_operations = 2'000,
      .warmup_operations = 200,
      .repetitions = 1,
      .seed = 42,
      .exchange_count = 3,
      .instrument_count = 100,
      .stripe_count = 8,
  };
  for (auto benchmark_case : benchmark_cases("smoke")) {
    if (benchmark_case.level != BenchmarkLevel::direct) {
      continue;
    }
    SCOPED_TRACE(market_server_mode_name(benchmark_case.mode));
    const auto result = run_benchmark(benchmark_case, settings, 1, true);
    EXPECT_TRUE(result.correct);
    EXPECT_EQ(result.successful_updates, result.requested_updates);
    EXPECT_EQ(result.successful_queries, result.requested_queries);
    EXPECT_GT(result.query_p99_us, 0.0);
  }
}

TEST(BenchmarkTest, TcpModesPassTheOracleGate) {
  const BenchmarkSettings settings{
      .measurement_operations = 400,
      .warmup_operations = 100,
      .repetitions = 1,
      .seed = 7,
      .exchange_count = 2,
      .instrument_count = 20,
      .stripe_count = 4,
  };
  for (auto benchmark_case : benchmark_cases("smoke")) {
    if (benchmark_case.level != BenchmarkLevel::tcp) {
      continue;
    }
    SCOPED_TRACE(market_server_mode_name(benchmark_case.mode));
    const auto result = run_benchmark(benchmark_case, settings, 1, true);
    EXPECT_TRUE(result.correct);
    EXPECT_EQ(result.successful_updates, result.requested_updates);
    EXPECT_EQ(result.successful_queries + result.queue_full_responses,
              result.requested_queries);
  }
}

TEST(BenchmarkTest, WritesCompleteCsvMetadata) {
  BenchmarkResult result;
  result.level = "direct";
  result.suite = "test";
  result.mode = "striped-read";
  result.correct = true;
  result.compiler = "AppleClang";
  const auto path = std::filesystem::temp_directory_path() /
                    "exchangelab-benchmark-test.csv";
  write_benchmark_csv(path, {result});

  std::ifstream input(path);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  const std::vector<std::string> required_fields{
      "level",
      "mode",
      "repetition",
      "seed",
      "exchange_count",
      "instrument_count",
      "updates_per_second",
      "queries_per_second",
      "update_percent",
      "query_percent",
      "distribution",
      "concurrent_client_count",
      "query_worker_count",
      "stripe_count",
      "warmup_operations",
      "warmup_seconds",
      "measurement_seconds",
      "query_p50_us",
      "query_p95_us",
      "query_p99_us",
      "errors",
      "rejected_requests",
      "queue_full_responses",
      "update_frames_rejected",
      "correct",
      "build_type",
      "compiler",
      "system_name",
      "cpu_model",
  };
  for (const auto &field : required_fields) {
    EXPECT_NE(contents.find(field), std::string::npos) << field;
  }
  EXPECT_NE(contents.find("striped-read"), std::string::npos);
  std::filesystem::remove(path);
}

} // namespace
} // namespace exchangelab
