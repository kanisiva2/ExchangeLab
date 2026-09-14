#include "exchangelab/query_client.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct ClientOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9001};
  bool show_help{};
};

std::optional<std::uint64_t> parse_unsigned(const std::string_view text) {
  std::uint64_t value{};
  const auto [position, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || position != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

void print_usage(const char *program) {
  std::cout << "Usage: " << program << " [options]\n\n"
            << "Options:\n"
            << "  --host ADDRESS   Server address (default: 127.0.0.1)\n"
            << "  --port N         Server query port (default: 9001)\n"
            << "  --help           Show this help message\n\n"
            << "Interactive commands:\n"
            << "  query N          Query instrument N\n"
            << "  reconnect        Reconnect to the server\n"
            << "  quit             Exit the client\n";
}

ClientOptions parse_options(const int argc, char *argv[]) {
  ClientOptions options;
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
    if (option == "--host") {
      options.host = value;
    } else if (option == "--port") {
      const auto port = parse_unsigned(value);
      if (!port.has_value() ||
          *port > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("--port requires a valid port number");
      }
      options.port = static_cast<std::uint16_t>(*port);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }
  return options;
}

bool connect(exchangelab::QueryClient &client, const ClientOptions &options) {
  std::string error;
  if (!client.connect(error)) {
    std::cout << "Could not connect to " << options.host << ':' << options.port
              << ": " << error << '\n';
    return false;
  }
  std::cout << "Connected to " << options.host << ':' << options.port << '\n';
  return true;
}

} // namespace

int main(const int argc, char *argv[]) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.show_help) {
      print_usage(argv[0]);
      return 0;
    }

    exchangelab::QueryClient client(options.host, options.port);
    connect(client, options);
    std::cout << "Commands: query INSTRUMENT, reconnect, quit\n";

    std::string command;
    while (std::cout << "> " && std::getline(std::cin, command)) {
      if (command == "quit" || command == "exit") {
        break;
      }
      if (command == "reconnect") {
        connect(client, options);
        continue;
      }

      constexpr std::string_view prefix = "query ";
      if (!std::string_view(command).starts_with(prefix)) {
        std::cout << "Invalid command. Use: query N, reconnect, or quit\n";
        continue;
      }
      const auto number =
          parse_unsigned(std::string_view(command).substr(prefix.size()));
      if (!number.has_value() ||
          *number > std::numeric_limits<exchangelab::InstrumentId>::max()) {
        std::cout << "Instrument must be a non-negative integer\n";
        continue;
      }

      if (!client.connected() && !connect(client, options)) {
        continue;
      }
      std::string error;
      const auto response =
          client.query(static_cast<exchangelab::InstrumentId>(*number), error);
      if (!response.has_value()) {
        std::cout << "Connection lost: " << error
                  << ". Use reconnect or try the query again.\n";
        continue;
      }
      std::cout << *response;
    }

    client.close();
    std::cout << "Client stopped\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
