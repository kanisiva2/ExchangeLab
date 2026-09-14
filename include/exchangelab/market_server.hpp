#pragma once

#include "exchangelab/concurrent_market_state.hpp"
#include "exchangelab/market_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace asio {
class io_context;
}

namespace exchangelab {

enum class MarketServerMode {
  single_threaded,
  global_read,
  striped_read,
  striped_write,
};

[[nodiscard]] std::string_view
market_server_mode_name(MarketServerMode mode) noexcept;

struct MarketServerConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t feed_port{9000};
  std::uint16_t query_port{9001};
  std::uint16_t exchange_count{10};
  std::uint32_t instrument_count{50'000};
  MarketServerMode mode{MarketServerMode::single_threaded};
  std::size_t query_worker_count{4};
  std::size_t query_queue_capacity{256};
  std::size_t stripe_count{64};
};

struct MarketServerStats {
  std::uint64_t feed_connections_accepted{};
  std::uint64_t query_connections_accepted{};
  std::uint64_t update_frames_applied{};
  std::uint64_t update_frames_rejected{};
  std::uint64_t truncated_feed_connections{};
  std::uint64_t query_requests{};
  std::uint64_t query_requests_rejected{};
  std::uint64_t query_queue_full{};
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
  [[nodiscard]] MarketStats market_stats() const;
  [[nodiscard]] std::uint64_t logical_checksum() const;

  // Retained for Phase 2 callers. This reference is available only in
  // single-threaded mode and must be observed on the event-loop thread or
  // after the server has stopped.
  [[nodiscard]] const MarketState &market_state() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace exchangelab
