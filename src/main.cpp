#include "exchangelab/market_state.hpp"
#include "exchangelab/simulator.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct ProgramOptions {
  exchangelab::SimulatorConfig simulation;
  exchangelab::InstrumentId query_instrument{1'234};
  bool show_help{};
};

void print_usage(const char *program_name) {
  std::cout
      << "Usage: " << program_name << " [options]\n\n"
      << "Options:\n"
      << "  --seed N                 Random seed (default: 42)\n"
      << "  --updates N              Number of generated updates (default: "
         "1000000)\n"
      << "  --exchanges N            Exchange count (default: 10)\n"
      << "  --instruments N          Instrument count (default: 50000)\n"
      << "  --distribution MODE      uniform or hot (default: uniform)\n"
      << "  --query N                Instrument to query (default: 1234)\n"
      << "  --help                   Show this help message\n";
}

std::uint64_t parse_unsigned(const std::string_view text,
                             const std::string_view option) {
  std::uint64_t value{};
  const auto *begin = text.data();
  const auto *end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || position != end) {
    throw std::invalid_argument(std::string(option) +
                                " requires a non-negative integer");
  }
  return value;
}

template <typename Target>
Target parse_bounded_unsigned(const std::string_view text,
                              const std::string_view option) {
  const auto value = parse_unsigned(text, option);
  if (value > std::numeric_limits<Target>::max()) {
    throw std::invalid_argument(std::string(option) + " is too large");
  }
  return static_cast<Target>(value);
}

ProgramOptions parse_options(const int argc, char *argv[]) {
  ProgramOptions options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--help") {
      options.show_help = true;
      continue;
    }

    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value = argv[++index];

    if (option == "--seed") {
      options.simulation.seed = parse_unsigned(value, option);
    } else if (option == "--updates") {
      options.simulation.event_count = parse_unsigned(value, option);
    } else if (option == "--exchanges") {
      options.simulation.exchange_count =
          parse_bounded_unsigned<std::uint16_t>(value, option);
    } else if (option == "--instruments") {
      options.simulation.instrument_count =
          parse_bounded_unsigned<std::uint32_t>(value, option);
    } else if (option == "--query") {
      options.query_instrument =
          parse_bounded_unsigned<std::uint32_t>(value, option);
    } else if (option == "--distribution") {
      if (value == "uniform") {
        options.simulation.distribution =
            exchangelab::TrafficDistribution::uniform;
      } else if (value == "hot") {
        options.simulation.distribution = exchangelab::TrafficDistribution::hot;
      } else {
        throw std::invalid_argument("--distribution must be uniform or hot");
      }
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }

  if (options.query_instrument >= options.simulation.instrument_count) {
    throw std::invalid_argument("--query must be less than --instruments");
  }
  return options;
}

std::string_view
distribution_name(const exchangelab::TrafficDistribution distribution) {
  return distribution == exchangelab::TrafficDistribution::uniform ? "uniform"
                                                                   : "hot";
}

void print_price(const exchangelab::Price price) {
  std::cout << '$' << price / exchangelab::kPriceScale << '.'
            << std::setfill('0') << std::setw(6)
            << price % exchangelab::kPriceScale << std::setfill(' ');
}

void print_result(const ProgramOptions &options,
                  const exchangelab::MarketState &state,
                  const exchangelab::QueryResult &query) {
  std::cout << "ExchangeLab phase 1\n"
            << "  exchanges: " << options.simulation.exchange_count << '\n'
            << "  instruments: " << options.simulation.instrument_count << '\n'
            << "  generated updates: " << options.simulation.event_count << '\n'
            << "  seed: " << options.simulation.seed << '\n'
            << "  distribution: "
            << distribution_name(options.simulation.distribution) << "\n\n"
            << "Instrument " << query.instrument_id << " prices:\n";

  for (const auto &price : query.prices) {
    std::cout << "  exchange " << price.exchange_id << ": ";
    print_price(price.price);
    std::cout << "  sequence=" << price.last_sequence
              << "  source_ns=" << price.source_timestamp_ns << '\n';
  }

  std::cout << "  missing exchanges:";
  if (query.missing_exchanges.empty()) {
    std::cout << " none";
  } else {
    for (const auto exchange_id : query.missing_exchanges) {
      std::cout << ' ' << exchange_id;
    }
  }

  const auto &stats = state.stats();
  std::cout << "\n\nUpdate counters:\n"
            << "  received: " << stats.received << '\n'
            << "  accepted: " << stats.accepted << '\n'
            << "  duplicates: " << stats.duplicates << '\n'
            << "  stale: " << stats.stale << '\n'
            << "  gap events: " << stats.gap_events << '\n'
            << "  missing sequences: " << stats.missing_sequences << '\n'
            << "  invalid: " << stats.invalid << '\n'
            << "Final-state checksum: 0x" << std::hex << std::setw(16)
            << std::setfill('0') << state.logical_checksum() << std::dec
            << '\n';
}

} // namespace

int main(const int argc, char *argv[]) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.show_help) {
      print_usage(argv[0]);
      return 0;
    }

    exchangelab::Simulator simulator(options.simulation);
    exchangelab::MarketState state(options.simulation.exchange_count,
                                   options.simulation.instrument_count);

    exchangelab::MarketUpdate update;
    while (simulator.next(update)) {
      [[maybe_unused]] const auto result = state.apply(update);
    }

    const auto query = state.query(options.query_instrument);
    if (!query.has_value()) {
      throw std::runtime_error(
          "query instrument is outside market-state range");
    }

    print_result(options, state, *query);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
