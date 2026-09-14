#pragma once

#include "exchangelab/market_state.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace asio {
class io_context;
}

namespace exchangelab {

struct MarketServerConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t feed_port{9000};
  std::uint16_t query_port{9001};
  std::uint16_t exchange_count{10};
  std::uint32_t instrument_count{50'000};
};

struct MarketServerStats {
  std::uint64_t feed_connections_accepted{};
  std::uint64_t query_connections_accepted{};
  std::uint64_t update_frames_applied{};
  std::uint64_t update_frames_rejected{};
  std::uint64_t truncated_feed_connections{};
  std::uint64_t query_requests{};
  std::uint64_t query_requests_rejected{};
};

class MarketServer {
public:
  MarketServer(asio::io_context &event_loop, MarketServerConfig config);
  ~MarketServer();

  MarketServer(const MarketServer &) = delete;
  MarketServer &operator=(const MarketServer &) = delete;

  void start();
  void stop();

  [[nodiscard]] std::uint16_t feed_port() const;
  [[nodiscard]] std::uint16_t query_port() const;
  [[nodiscard]] MarketServerStats stats() const;
  [[nodiscard]] const MarketState &market_state() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace exchangelab
