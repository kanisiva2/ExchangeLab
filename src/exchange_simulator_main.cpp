#include "exchangelab/exchange_publisher.hpp"

#include <asio.hpp>

#include <charconv>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct SimulatorOptions {
  exchangelab::ExchangePublisherConfig publisher;
  bool show_help{};
  bool continuous_selected{};
  bool events_selected{};
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
  std::cout << "Usage: " << program
            << " [--continuous | --events N] [options]\n\n"
            << "Modes:\n"
            << "  --continuous          Publish until interrupted\n"
            << "  --events N            Publish N updates, then exit (default: "
               "1000000)\n\n"
            << "Options:\n"
            << "  --host ADDRESS        Server address (default: 127.0.0.1)\n"
            << "  --port N              Server feed port (default: 9000)\n"
            << "  --exchanges N         Exchange count (default: 10)\n"
            << "  --instruments N       Instrument count (default: 50000)\n"
            << "  --rate N              Target aggregate updates/second "
               "(default: 100000)\n"
            << "  --seed N              Random seed (default: 42)\n"
            << "  --distribution MODE   uniform or hot (default: uniform)\n"
            << "  --help                Show this help message\n";
}

SimulatorOptions parse_options(const int argc, char *argv[]) {
  SimulatorOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--help") {
      options.show_help = true;
      continue;
    }
    if (option == "--continuous") {
      options.publisher.continuous = true;
      options.continuous_selected = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value = argv[++index];
    if (option == "--events") {
      options.publisher.simulation.event_count = parse_unsigned(value, option);
      options.events_selected = true;
    } else if (option == "--host") {
      options.publisher.server_address = value;
    } else if (option == "--port") {
      options.publisher.server_port =
          parse_bounded<std::uint16_t>(value, option);
    } else if (option == "--exchanges") {
      options.publisher.simulation.exchange_count =
          parse_bounded<std::uint16_t>(value, option);
    } else if (option == "--instruments") {
      options.publisher.simulation.instrument_count =
          parse_bounded<std::uint32_t>(value, option);
    } else if (option == "--rate") {
      options.publisher.updates_per_second = parse_unsigned(value, option);
    } else if (option == "--seed") {
      options.publisher.simulation.seed = parse_unsigned(value, option);
    } else if (option == "--distribution") {
      if (value == "uniform") {
        options.publisher.simulation.distribution =
            exchangelab::TrafficDistribution::uniform;
      } else if (value == "hot") {
        options.publisher.simulation.distribution =
            exchangelab::TrafficDistribution::hot;
      } else {
        throw std::invalid_argument("--distribution must be uniform or hot");
      }
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }
  if (options.continuous_selected && options.events_selected) {
    throw std::invalid_argument(
        "choose either --continuous or --events, not both");
  }
  return options;
}

} // namespace

int main(const int argc, char *argv[]) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.show_help) {
      print_usage(argv[0]);
      return 0;
    }

    asio::io_context event_loop;
    asio::signal_set signals(event_loop, SIGINT, SIGTERM);
    exchangelab::ExchangePublisher publisher(event_loop, options.publisher,
                                             [&signals] {
                                               asio::error_code ignored;
                                               signals.cancel(ignored);
                                             });
    signals.async_wait([&publisher](const asio::error_code &error, int) {
      if (!error) {
        std::cout << "\nStopping exchange simulator...\n";
        publisher.stop();
      }
    });

    std::cout << "ExchangeLab exchange simulator\n"
              << "  server: " << options.publisher.server_address << ':'
              << options.publisher.server_port << '\n'
              << "  feeds: " << options.publisher.simulation.exchange_count
              << '\n'
              << "  instruments: "
              << options.publisher.simulation.instrument_count << '\n'
              << "  mode: "
              << (options.publisher.continuous
                      ? "continuous"
                      : std::to_string(
                            options.publisher.simulation.event_count) +
                            " events")
              << '\n'
              << "  target rate: " << options.publisher.updates_per_second
              << " updates/second\n"
              << "  seed: " << options.publisher.simulation.seed << '\n';

    publisher.start();
    event_loop.run();

    const auto stats = publisher.stats();
    std::cout << "Exchange simulator stopped\n"
              << "  connection attempts: " << stats.connection_attempts << '\n'
              << "  connections established: " << stats.connections_established
              << '\n'
              << "  updates generated: " << stats.updates_generated << '\n'
              << "  updates sent: " << stats.updates_sent << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
