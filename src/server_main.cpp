#include "exchangelab/market_server.hpp"

#include <asio.hpp>

#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct ServerOptions {
  exchangelab::MarketServerConfig config;
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
      << "Usage: " << program << " [options]\n\n"
      << "Options:\n"
      << "  --bind ADDRESS       Address to listen on (default: 127.0.0.1)\n"
      << "  --feed-port N        Binary feed port (default: 9000)\n"
      << "  --query-port N       Text query port (default: 9001)\n"
      << "  --exchanges N        Exchange count (default: 10)\n"
      << "  --instruments N      Instrument count (default: 50000)\n"
      << "  --mode MODE          single-threaded, global-read, striped-read,\n"
      << "                       or striped-write (default: single-threaded)\n"
      << "  --query-workers N    Threaded query workers (default: 4)\n"
      << "  --query-queue N      Waiting query capacity (default: 256)\n"
      << "  --stripes N          Locks used by striped modes (default: 64)\n"
      << "  --help               Show this help message\n";
}

ServerOptions parse_options(const int argc, char *argv[]) {
  ServerOptions options;
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
    if (option == "--bind") {
      options.config.bind_address = value;
    } else if (option == "--feed-port") {
      options.config.feed_port = parse_bounded<std::uint16_t>(value, option);
    } else if (option == "--query-port") {
      options.config.query_port = parse_bounded<std::uint16_t>(value, option);
    } else if (option == "--exchanges") {
      options.config.exchange_count =
          parse_bounded<std::uint16_t>(value, option);
    } else if (option == "--instruments") {
      options.config.instrument_count =
          parse_bounded<std::uint32_t>(value, option);
    } else if (option == "--mode") {
      if (value == "single-threaded") {
        options.config.mode = exchangelab::MarketServerMode::single_threaded;
      } else if (value == "global-read") {
        options.config.mode = exchangelab::MarketServerMode::global_read;
      } else if (value == "striped-read") {
        options.config.mode = exchangelab::MarketServerMode::striped_read;
      } else if (value == "striped-write") {
        options.config.mode = exchangelab::MarketServerMode::striped_write;
      } else {
        throw std::invalid_argument(
            "--mode must be single-threaded, global-read, striped-read, or "
            "striped-write");
      }
    } else if (option == "--query-workers") {
      options.config.query_worker_count =
          parse_bounded<std::size_t>(value, option);
    } else if (option == "--query-queue") {
      options.config.query_queue_capacity =
          parse_bounded<std::size_t>(value, option);
    } else if (option == "--stripes") {
      options.config.stripe_count = parse_bounded<std::size_t>(value, option);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }
  if (options.config.feed_port == options.config.query_port &&
      options.config.feed_port != 0) {
    throw std::invalid_argument("feed and query ports must be different");
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
    exchangelab::MarketServer server(event_loop, options.config);
    asio::signal_set signals(event_loop, SIGINT, SIGTERM);
    signals.async_wait([&server](const asio::error_code &error, int) {
      if (!error) {
        std::cout << "\nStopping server...\n";
        server.stop();
      }
    });

    server.start();
    std::cout << "ExchangeLab server\n"
              << "  feed address: " << options.config.bind_address << ':'
              << server.feed_port() << '\n'
              << "  query address: " << options.config.bind_address << ':'
              << server.query_port() << '\n'
              << "  exchanges: " << options.config.exchange_count << '\n'
              << "  instruments: " << options.config.instrument_count << '\n'
              << "  mode: "
              << exchangelab::market_server_mode_name(options.config.mode)
              << '\n'
              << "  feed threads: "
              << (options.config.mode ==
                          exchangelab::MarketServerMode::single_threaded
                      ? 0
                      : options.config.exchange_count)
              << '\n'
              << "  query workers: "
              << (options.config.mode ==
                          exchangelab::MarketServerMode::single_threaded
                      ? 0
                      : options.config.query_worker_count)
              << '\n'
              << "  query queue capacity: "
              << (options.config.mode ==
                          exchangelab::MarketServerMode::single_threaded
                      ? 0
                      : options.config.query_queue_capacity)
              << '\n'
              << "Press Ctrl+C to stop.\n";
    event_loop.run();

    const auto network = server.stats();
    const auto market = server.market_stats();
    std::cout << "Server stopped\n"
              << "  mode: "
              << exchangelab::market_server_mode_name(options.config.mode)
              << '\n'
              << "  feed connections: " << network.feed_connections_accepted
              << '\n'
              << "  query connections: " << network.query_connections_accepted
              << '\n'
              << "  update frames applied: " << network.update_frames_applied
              << '\n'
              << "  update frames rejected: " << network.update_frames_rejected
              << '\n'
              << "  query requests: " << network.query_requests << '\n'
              << "  query queue full: " << network.query_queue_full << '\n'
              << "  market updates accepted: " << market.accepted << '\n'
              << "  logical checksum: 0x" << std::hex
              << server.logical_checksum() << std::dec << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
