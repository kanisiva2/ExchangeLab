#pragma once

#include "exchangelab/simulator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace asio {
class io_context;
}

namespace exchangelab {

struct ExchangePublisherConfig {
  std::string server_address{"127.0.0.1"};
  std::uint16_t server_port{9000};
  SimulatorConfig simulation;
  bool continuous{};
  std::uint64_t updates_per_second{100'000};
  std::chrono::milliseconds reconnect_delay{1'000};
  std::size_t maximum_queued_frames_per_exchange{1'024};
};

struct ExchangePublisherStats {
  std::uint64_t connection_attempts{};
  std::uint64_t connections_established{};
  std::uint64_t updates_generated{};
  std::uint64_t updates_sent{};
};

class ExchangePublisher {
public:
  ExchangePublisher(asio::io_context &event_loop,
                    ExchangePublisherConfig config,
                    std::function<void()> completion_handler = {});
  ~ExchangePublisher();

  ExchangePublisher(const ExchangePublisher &) = delete;
  ExchangePublisher &operator=(const ExchangePublisher &) = delete;

  void start();
  void stop();

  [[nodiscard]] bool finished() const;
  [[nodiscard]] ExchangePublisherStats stats() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace exchangelab
