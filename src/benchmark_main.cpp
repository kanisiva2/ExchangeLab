#include "exchangelab/benchmark.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::string suite;
  exchangelab::BenchmarkSettings settings;
  std::filesystem::path output_directory{"results/phase4"};
  std::optional<std::size_t> client_override;
  bool allow_non_release{};
  bool show_help{};
};

std::uint64_t parse_unsigned(const std::string_view text,
                             const std::string_view option) {
  std::uint64_t value{};
  const auto [position, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || position != text.data() + text.size()) {
    throw std::invalid_argument(std::string(option) +
                                " requires a non-negative integer");
  }
  return value;
}

template <typename Target>
Target parse_bounded(const std::string_view text,
                     const std::string_view option) {
  const auto value = parse_unsigned(text, option);
  if (value > std::numeric_limits<Target>::max()) {
    throw std::invalid_argument(std::string(option) + " is too large");
  }
  return static_cast<Target>(value);
}

void print_usage(const char *program) {
  std::cout
      << "Usage: " << program << " --suite NAME [options]\n\n"
      << "Suites:\n"
      << "  direct-locking   Single-threaded/global/striped lock comparison\n"
      << "  direct-sorting   Striped sort-on-read/write comparison\n"
      << "  direct-workers   Direct query workers 1, 4, and 8\n"
      << "  tcp-modes        End-to-end TCP comparison of all runtime modes\n"
      << "  tcp-workers      TCP query workers 1, 4, and 8\n"
      << "  all              Run every focused suite\n"
      << "  smoke            Small functional run for Debug verification\n\n"
      << "Options:\n"
      << "  --operations N          Measured operations per case (default: 1000000)\n"
      << "  --warmup-operations N   Warm-up operations per case (default: 100000)\n"
      << "  --repetitions N         Repetitions per case (default: 5)\n"
      << "  --seed N                Deterministic workload seed (default: 42)\n"
      << "  --exchanges N           Exchange count (default: 10)\n"
      << "  --instruments N         Instrument count (default: 50000)\n"
      << "  --stripes N             Striped lock count (default: 64)\n"
      << "  --clients N             Override TCP client count (default: 16)\n"
      << "  --output-dir PATH       CSV directory (default: results/phase4)\n"
      << "  --allow-non-release     Permit a non-Release non-smoke run\n"
      << "  --help                  Show this help message\n";
}

Options parse_options(const int argc, char *argv[]) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--help") {
      options.show_help = true;
      continue;
    }
    if (option == "--allow-non-release") {
      options.allow_non_release = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value = argv[++index];
    if (option == "--suite") {
      options.suite = value;
    } else if (option == "--operations") {
      options.settings.measurement_operations = parse_unsigned(value, option);
    } else if (option == "--warmup-operations") {
      options.settings.warmup_operations = parse_unsigned(value, option);
    } else if (option == "--repetitions") {
      options.settings.repetitions = parse_bounded<std::size_t>(value, option);
    } else if (option == "--seed") {
      options.settings.seed = parse_unsigned(value, option);
    } else if (option == "--exchanges") {
      options.settings.exchange_count =
          parse_bounded<std::uint16_t>(value, option);
    } else if (option == "--instruments") {
      options.settings.instrument_count =
          parse_bounded<std::uint32_t>(value, option);
    } else if (option == "--stripes") {
      options.settings.stripe_count =
          parse_bounded<std::size_t>(value, option);
    } else if (option == "--clients") {
      options.client_override = parse_bounded<std::size_t>(value, option);
    } else if (option == "--output-dir") {
      options.output_directory = value;
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }
  if (!options.show_help && options.suite.empty()) {
    throw std::invalid_argument("--suite is required");
  }
  return options;
}

void print_result(const exchangelab::BenchmarkResult &result) {
  std::cout << result.level << ' ' << result.suite << ' ' << result.mode
            << " ratio=" << result.update_percent << '/'
            << result.query_percent << " distribution=" << result.distribution
            << " workers=" << result.query_worker_count
            << " repetition=" << result.repetition << '\n'
            << "  updates/sec=" << std::fixed << std::setprecision(0)
            << result.updates_per_second
            << " queries/sec=" << result.queries_per_second
            << " query_us(p50/p95/p99)=" << std::setprecision(3)
            << result.query_p50_us << '/' << result.query_p95_us << '/'
            << result.query_p99_us << " seconds=" << result.measurement_seconds
            << " errors=" << result.errors
            << " rejected=" << result.rejected_requests
            << " queue-full=" << result.queue_full_responses
            << " correct=" << (result.correct ? "yes" : "NO") << '\n';
}

} // namespace

int main(const int argc, char *argv[]) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.show_help) {
      print_usage(argv[0]);
      return 0;
    }
    if (options.suite != "smoke" &&
        !exchangelab::benchmark_build_is_release() &&
        !options.allow_non_release) {
      throw std::runtime_error(
          "publishable benchmarks require a Release build; use the smoke "
          "suite for Debug verification or --allow-non-release explicitly");
    }

    auto cases = exchangelab::benchmark_cases(options.suite);
    if (options.client_override.has_value()) {
      if (*options.client_override == 0) {
        throw std::invalid_argument("--clients must be greater than zero");
      }
      for (auto &benchmark_case : cases) {
        if (benchmark_case.level == exchangelab::BenchmarkLevel::tcp) {
          benchmark_case.concurrent_client_count = *options.client_override;
        }
      }
    }

    std::map<std::string, std::vector<exchangelab::BenchmarkResult>> results;
    bool every_result_correct = true;
    for (std::size_t repetition = 1;
         repetition <= options.settings.repetitions; ++repetition) {
      auto ordered_cases = cases;
      if (!ordered_cases.empty()) {
        const auto offset = (repetition - 1) % ordered_cases.size();
        std::rotate(ordered_cases.begin(), ordered_cases.begin() + offset,
                    ordered_cases.end());
      }
      for (const auto &benchmark_case : ordered_cases) {
        auto result = exchangelab::run_benchmark(
            benchmark_case, options.settings, repetition, repetition == 1);
        print_result(result);
        every_result_correct = every_result_correct && result.correct;
        results[result.suite].push_back(std::move(result));
      }
    }

    for (const auto &[suite, suite_results] : results) {
      const auto path = options.output_directory / (suite + ".csv");
      exchangelab::write_benchmark_csv(path, suite_results);
      std::cout << "Wrote " << suite_results.size() << " raw rows to "
                << path << '\n';
    }
    return every_result_correct ? 0 : 2;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
