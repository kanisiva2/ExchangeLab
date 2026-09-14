#include "exchangelab/exchange_publisher.hpp"
#include "exchangelab/market_server.hpp"
#include "exchangelab/query_client.hpp"
#include "exchangelab/simulator.hpp"
#include "exchangelab/update_codec.hpp"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>
#include <istream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace exchangelab {
namespace {

using asio::ip::tcp;
using namespace std::chrono_literals;

class RunningServer {
public:
  RunningServer(const std::uint16_t exchanges, const std::uint32_t instruments,
                const std::uint16_t feed_port = 0,
                const std::uint16_t query_port = 0)
      : server_(event_loop_, MarketServerConfig{
                                 .feed_port = feed_port,
                                 .query_port = query_port,
                                 .exchange_count = exchanges,
                                 .instrument_count = instruments,
                             }) {
    server_.start();
    thread_ = std::thread([this] { event_loop_.run(); });
  }

  ~RunningServer() { stop(); }

  RunningServer(const RunningServer &) = delete;
  RunningServer &operator=(const RunningServer &) = delete;

  [[nodiscard]] std::uint16_t feed_port() const { return server_.feed_port(); }

  [[nodiscard]] std::uint16_t query_port() const {
    return server_.query_port();
  }

  [[nodiscard]] MarketServerStats snapshot_stats() {
    auto promise = std::make_shared<std::promise<MarketServerStats>>();
    auto future = promise->get_future();
    asio::post(event_loop_,
               [this, promise] { promise->set_value(server_.stats()); });
    return future.get();
  }

  bool wait_for_applied_updates(const std::uint64_t count) {
    return wait_for_stats([count](const MarketServerStats &stats) {
      return stats.update_frames_applied >= count;
    });
  }

  bool wait_for_stats(
      const std::function<bool(const MarketServerStats &)> &predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate(snapshot_stats())) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  void stop() {
    if (!thread_.joinable()) {
      return;
    }
    asio::post(event_loop_, [this] { server_.stop(); });
    thread_.join();
  }

  [[nodiscard]] const MarketState &market_state_after_stop() const {
    return server_.market_state();
  }

private:
  asio::io_context event_loop_;
  MarketServer server_;
  std::thread thread_;
};

void send_update(const std::uint16_t port, const MarketUpdate &update) {
  asio::io_context event_loop;
  tcp::socket socket(event_loop);
  socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  const auto frame = encode_update(update);
  asio::write(socket, asio::buffer(frame));
  asio::error_code ignored;
  socket.shutdown(tcp::socket::shutdown_send, ignored);
  socket.close(ignored);
}

std::string send_raw_query(const std::uint16_t port,
                           const std::string &request) {
  asio::io_context event_loop;
  tcp::socket socket(event_loop);
  socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  asio::write(socket, asio::buffer(request));

  asio::streambuf buffer;
  std::string response;
  for (;;) {
    asio::read_until(socket, buffer, '\n');
    std::istream input(&buffer);
    std::string line;
    std::getline(input, line);
    response += line + '\n';
    if (line == "END") {
      return response;
    }
  }
}

TEST(TcpSystemTest, FinitePublisherMatchesTheDirectReferencePath) {
  const SimulatorConfig simulation{
      .exchange_count = 3,
      .instrument_count = 100,
      .event_count = 300,
      .seed = 42,
      .distribution = TrafficDistribution::hot,
  };

  MarketState expected(simulation.exchange_count, simulation.instrument_count);
  Simulator direct_simulator(simulation);
  MarketUpdate update;
  while (direct_simulator.next(update)) {
    [[maybe_unused]] const auto result = expected.apply(update);
  }

  RunningServer running(simulation.exchange_count, simulation.instrument_count);
  asio::io_context publisher_loop;
  ExchangePublisher publisher(publisher_loop,
                              ExchangePublisherConfig{
                                  .server_port = running.feed_port(),
                                  .simulation = simulation,
                                  .continuous = false,
                                  .updates_per_second = 1'000'000,
                              });
  publisher.start();
  publisher_loop.run();

  EXPECT_TRUE(publisher.finished());
  EXPECT_EQ(publisher.stats().updates_generated, simulation.event_count);
  EXPECT_EQ(publisher.stats().updates_sent, simulation.event_count);
  ASSERT_TRUE(running.wait_for_applied_updates(simulation.event_count));

  QueryClient first_client("127.0.0.1", running.query_port());
  QueryClient second_client("127.0.0.1", running.query_port());
  std::string error;
  ASSERT_TRUE(first_client.connect(error)) << error;
  ASSERT_TRUE(second_client.connect(error)) << error;
  const auto first_response = first_client.query(0, error);
  ASSERT_TRUE(first_response.has_value()) << error;
  EXPECT_NE(first_response->find("RESULT 0\n"), std::string::npos);
  const auto second_response = second_client.query(1, error);
  ASSERT_TRUE(second_response.has_value()) << error;
  EXPECT_NE(second_response->find("RESULT 1\n"), std::string::npos);

  running.stop();
  EXPECT_EQ(running.market_state_after_stop().stats(), expected.stats());
  EXPECT_EQ(running.market_state_after_stop().logical_checksum(),
            expected.logical_checksum());
}

TEST(TcpSystemTest, HandlesTenFeedsFragmentationCombinationAndInvalidInput) {
  RunningServer running(10, 100);
  asio::io_context event_loop;
  std::vector<std::unique_ptr<tcp::socket>> sockets;
  for (ExchangeId exchange_id = 0; exchange_id < 10; ++exchange_id) {
    auto socket = std::make_unique<tcp::socket>(event_loop);
    socket->connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                  running.feed_port()));
    sockets.push_back(std::move(socket));
  }

  for (ExchangeId exchange_id = 0; exchange_id < 10; ++exchange_id) {
    const auto frame = encode_update(MarketUpdate{
        exchange_id,
        5,
        100'000'000 + static_cast<Price>(exchange_id) * 1'000'000,
        1,
        exchange_id,
    });
    if (exchange_id == 0) {
      asio::write(*sockets[exchange_id], asio::buffer(frame.data(), 7));
      asio::write(*sockets[exchange_id],
                  asio::buffer(frame.data() + 7, frame.size() - 7));
    } else if (exchange_id == 1) {
      const auto second = encode_update(MarketUpdate{1, 6, 102'000'000, 2, 20});
      std::vector<std::uint8_t> combined(frame.begin(), frame.end());
      combined.insert(combined.end(), second.begin(), second.end());
      asio::write(*sockets[exchange_id], asio::buffer(combined));
    } else {
      asio::write(*sockets[exchange_id], asio::buffer(frame));
    }
  }

  for (const auto &socket : sockets) {
    asio::error_code ignored;
    socket->shutdown(tcp::socket::shutdown_send, ignored);
    socket->close(ignored);
  }
  ASSERT_TRUE(running.wait_for_applied_updates(11));

  tcp::socket invalid_socket(event_loop);
  invalid_socket.connect(
      tcp::endpoint(asio::ip::make_address("127.0.0.1"), running.feed_port()));
  auto invalid = encode_update(MarketUpdate{0, 5, 999'000'000, 3, 30});
  invalid[0] = 99;
  asio::write(invalid_socket, asio::buffer(invalid));
  asio::error_code ignored;
  invalid_socket.close(ignored);

  tcp::socket truncated_socket(event_loop);
  truncated_socket.connect(
      tcp::endpoint(asio::ip::make_address("127.0.0.1"), running.feed_port()));
  const auto truncated = encode_update(MarketUpdate{0, 5, 999'000'000, 3, 30});
  asio::write(truncated_socket, asio::buffer(truncated.data(), 5));
  truncated_socket.close(ignored);

  ASSERT_TRUE(running.wait_for_stats([](const MarketServerStats &stats) {
    return stats.update_frames_rejected >= 1 &&
           stats.truncated_feed_connections >= 1;
  }));

  QueryClient client("127.0.0.1", running.query_port());
  std::string error;
  ASSERT_TRUE(client.connect(error)) << error;
  const auto response = client.query(5, error);
  ASSERT_TRUE(response.has_value()) << error;
  EXPECT_NE(response->find("MISSING none\n"), std::string::npos);

  running.stop();
  EXPECT_EQ(running.market_state_after_stop().stats().received, 11U);
  EXPECT_EQ(running.market_state_after_stop().stats().accepted, 11U);
}

TEST(TcpSystemTest, ContinuousPublisherReconnectsAfterServerRestart) {
  auto first_server = std::make_unique<RunningServer>(2, 100);
  const auto feed_port = first_server->feed_port();

  asio::io_context publisher_loop;
  ExchangePublisher publisher(publisher_loop,
                              ExchangePublisherConfig{
                                  .server_port = feed_port,
                                  .simulation =
                                      SimulatorConfig{
                                          .exchange_count = 2,
                                          .instrument_count = 100,
                                          .event_count = 1,
                                          .seed = 7,
                                      },
                                  .continuous = true,
                                  .updates_per_second = 10'000,
                                  .reconnect_delay = 20ms,
                              });
  publisher.start();
  std::thread publisher_thread([&publisher_loop] { publisher_loop.run(); });

  ASSERT_TRUE(first_server->wait_for_applied_updates(20));
  first_server->stop();
  first_server.reset();

  auto second_server = std::make_unique<RunningServer>(2, 100, feed_port, 0);
  ASSERT_TRUE(second_server->wait_for_applied_updates(20));

  asio::post(publisher_loop, [&publisher] { publisher.stop(); });
  publisher_thread.join();
  EXPECT_GE(publisher.stats().connections_established, 4U);
}

TEST(TcpSystemTest, AFeedAndQueryClientCanReconnect) {
  RunningServer running(1, 10);
  send_update(running.feed_port(), MarketUpdate{0, 5, 100'000'000, 1, 10});
  ASSERT_TRUE(running.wait_for_applied_updates(1));

  QueryClient client("127.0.0.1", running.query_port());
  std::string error;
  ASSERT_TRUE(client.connect(error)) << error;
  auto response = client.query(5, error);
  ASSERT_TRUE(response.has_value()) << error;
  EXPECT_NE(response->find("sequence=1"), std::string::npos);
  client.close();

  send_update(running.feed_port(), MarketUpdate{0, 5, 101'000'000, 2, 20});
  ASSERT_TRUE(running.wait_for_applied_updates(2));
  ASSERT_TRUE(client.connect(error)) << error;
  response = client.query(5, error);
  ASSERT_TRUE(response.has_value()) << error;
  EXPECT_NE(response->find("$101.000000 sequence=2"), std::string::npos);
}

TEST(TcpSystemTest, RejectsMalformedAndOversizedQueryCommands) {
  RunningServer running(1, 10);
  EXPECT_EQ(send_raw_query(running.query_port(), "WHAT 5\n"),
            "ERROR malformed-command\nEND\n");
  EXPECT_EQ(send_raw_query(running.query_port(), std::string(200, 'X') + "\n"),
            "ERROR request-too-long\nEND\n");
}

TEST(TcpSystemTest, ShutsDownWithActiveSockets) {
  RunningServer running(1, 10);
  asio::io_context client_loop;
  tcp::socket feed_socket(client_loop);
  tcp::socket query_socket(client_loop);
  feed_socket.connect(
      tcp::endpoint(asio::ip::make_address("127.0.0.1"), running.feed_port()));
  query_socket.connect(
      tcp::endpoint(asio::ip::make_address("127.0.0.1"), running.query_port()));
  running.stop();
  SUCCEED();
}

} // namespace
} // namespace exchangelab
