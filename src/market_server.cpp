#include "exchangelab/market_server.hpp"

#include "exchangelab/query_protocol.hpp"
#include "exchangelab/update_stream_decoder.hpp"

#include <asio.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace exchangelab {

using asio::ip::tcp;

struct MarketServer::Impl : std::enable_shared_from_this<Impl> {
  class FeedSession;
  class QuerySession;

  Impl(asio::io_context &event_loop, MarketServerConfig server_config);

  void start();
  void stop();
  void accept_feed();
  void accept_query();
  bool bind_feed(const std::shared_ptr<FeedSession> &session,
                 ExchangeId exchange_id);
  void remove_feed(const std::shared_ptr<FeedSession> &session,
                   std::optional<ExchangeId> exchange_id, bool truncated);
  void remove_query(const std::shared_ptr<QuerySession> &session);
  void reject_update();
  void apply_update(const MarketUpdate &update);
  std::string handle_query(std::string_view line);

  asio::io_context &event_loop;
  MarketServerConfig config;
  MarketState market_state;
  tcp::acceptor feed_acceptor;
  tcp::acceptor query_acceptor;
  std::set<std::shared_ptr<FeedSession>,
           std::owner_less<std::shared_ptr<FeedSession>>>
      feed_sessions;
  std::set<std::shared_ptr<QuerySession>,
           std::owner_less<std::shared_ptr<QuerySession>>>
      query_sessions;
  std::vector<std::weak_ptr<FeedSession>> feed_slots;
  MarketServerStats server_stats;
  bool started{};
  bool stopped{};
};

class MarketServer::Impl::FeedSession
    : public std::enable_shared_from_this<FeedSession> {
public:
  FeedSession(tcp::socket socket, const std::shared_ptr<Impl> &server)
      : socket_(std::move(socket)), server_(server),
        decoder_(
            {server->config.exchange_count, server->config.instrument_count}) {}

  void start() { read(); }

  void close() {
    asio::error_code ignored;
    socket_.cancel(ignored);
    socket_.close(ignored);
    finish(false);
  }

private:
  void read() {
    auto self = shared_from_this();
    socket_.async_read_some(
        asio::buffer(read_buffer_),
        [self](const asio::error_code &error, const std::size_t byte_count) {
          self->read_complete(error, byte_count);
        });
  }

  void read_complete(const asio::error_code &error,
                     const std::size_t byte_count) {
    if (error) {
      const bool peer_ended_connection =
          error != asio::error::operation_aborted;
      finish(peer_ended_connection && decoder_.finish());
      return;
    }

    auto batch = decoder_.append(
        std::span<const std::uint8_t>(read_buffer_.data(), byte_count));
    auto server = server_.lock();
    if (!server) {
      close();
      return;
    }

    for ([[maybe_unused]] const auto decode_error : batch.errors) {
      server->reject_update();
    }

    for (const auto &update : batch.updates) {
      if (!exchange_id_.has_value()) {
        if (!server->bind_feed(shared_from_this(), update.exchange_id)) {
          server->reject_update();
          close();
          return;
        }
        exchange_id_ = update.exchange_id;
      } else if (*exchange_id_ != update.exchange_id) {
        server->reject_update();
        close();
        return;
      }
      server->apply_update(update);
    }
    read();
  }

  void finish(const bool truncated) {
    if (finished_) {
      return;
    }
    finished_ = true;
    if (auto server = server_.lock()) {
      server->remove_feed(shared_from_this(), exchange_id_, truncated);
    }
  }

  tcp::socket socket_;
  std::weak_ptr<Impl> server_;
  UpdateStreamDecoder decoder_;
  std::array<std::uint8_t, 4096> read_buffer_{};
  std::optional<ExchangeId> exchange_id_;
  bool finished_{};
};

class MarketServer::Impl::QuerySession
    : public std::enable_shared_from_this<QuerySession> {
public:
  QuerySession(tcp::socket socket, const std::shared_ptr<Impl> &server)
      : socket_(std::move(socket)), server_(server) {}

  void start() { read_line(); }

  void close() {
    asio::error_code ignored;
    socket_.cancel(ignored);
    socket_.close(ignored);
    finish();
  }

private:
  void read_line() {
    auto self = shared_from_this();
    asio::async_read_until(
        socket_, asio::dynamic_buffer(input_, kMaximumQueryLineSize + 1), '\n',
        [self](const asio::error_code &error, const std::size_t byte_count) {
          self->read_complete(error, byte_count);
        });
  }

  void read_complete(const asio::error_code &error,
                     const std::size_t byte_count) {
    if (error) {
      if (input_.size() > kMaximumQueryLineSize) {
        if (auto server = server_.lock()) {
          ++server->server_stats.query_requests_rejected;
        }
        write_response(format_query_error("request-too-long"), true);
      } else {
        finish();
      }
      return;
    }

    std::string line = input_.substr(0, byte_count - 1);
    input_.erase(0, byte_count);
    if (auto server = server_.lock()) {
      write_response(server->handle_query(line), false);
    } else {
      close();
    }
  }

  void write_response(std::string response, const bool close_after_write) {
    output_ = std::move(response);
    auto self = shared_from_this();
    asio::async_write(
        socket_, asio::buffer(output_),
        [self, close_after_write](const asio::error_code &error, std::size_t) {
          if (error || close_after_write) {
            self->close();
          } else {
            self->read_line();
          }
        });
  }

  void finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    if (auto server = server_.lock()) {
      server->remove_query(shared_from_this());
    }
  }

  tcp::socket socket_;
  std::weak_ptr<Impl> server_;
  std::string input_;
  std::string output_;
  bool finished_{};
};

MarketServer::Impl::Impl(asio::io_context &event_loop,
                         MarketServerConfig server_config)
    : event_loop(event_loop), config(std::move(server_config)),
      market_state(config.exchange_count, config.instrument_count),
      feed_acceptor(event_loop), query_acceptor(event_loop),
      feed_slots(config.exchange_count) {
  const auto address = asio::ip::make_address(config.bind_address);

  const auto prepare_acceptor = [&address](tcp::acceptor &acceptor,
                                           const std::uint16_t port) {
    const tcp::endpoint endpoint(address, port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
  };

  prepare_acceptor(feed_acceptor, config.feed_port);
  prepare_acceptor(query_acceptor, config.query_port);
}

void MarketServer::Impl::start() {
  if (started) {
    return;
  }
  started = true;
  accept_feed();
  accept_query();
}

void MarketServer::Impl::stop() {
  if (stopped) {
    return;
  }
  stopped = true;

  asio::error_code ignored;
  feed_acceptor.cancel(ignored);
  feed_acceptor.close(ignored);
  query_acceptor.cancel(ignored);
  query_acceptor.close(ignored);

  const auto feeds = feed_sessions;
  for (const auto &session : feeds) {
    session->close();
  }
  const auto queries = query_sessions;
  for (const auto &session : queries) {
    session->close();
  }
}

void MarketServer::Impl::accept_feed() {
  auto self = shared_from_this();
  feed_acceptor.async_accept(
      [self](const asio::error_code &error, tcp::socket socket) {
        if (!error && !self->stopped) {
          auto session = std::make_shared<FeedSession>(std::move(socket), self);
          self->feed_sessions.insert(session);
          ++self->server_stats.feed_connections_accepted;
          session->start();
        } else if (socket.is_open()) {
          asio::error_code ignored;
          socket.close(ignored);
        }
        if (!self->stopped) {
          self->accept_feed();
        }
      });
}

void MarketServer::Impl::accept_query() {
  auto self = shared_from_this();
  query_acceptor.async_accept([self](const asio::error_code &error,
                                     tcp::socket socket) {
    if (!error && !self->stopped) {
      auto session = std::make_shared<QuerySession>(std::move(socket), self);
      self->query_sessions.insert(session);
      ++self->server_stats.query_connections_accepted;
      session->start();
    } else if (socket.is_open()) {
      asio::error_code ignored;
      socket.close(ignored);
    }
    if (!self->stopped) {
      self->accept_query();
    }
  });
}

bool MarketServer::Impl::bind_feed(const std::shared_ptr<FeedSession> &session,
                                   const ExchangeId exchange_id) {
  if (exchange_id >= feed_slots.size()) {
    return false;
  }
  if (const auto current = feed_slots[exchange_id].lock();
      current && current != session) {
    return false;
  }
  feed_slots[exchange_id] = session;
  return true;
}

void MarketServer::Impl::remove_feed(
    const std::shared_ptr<FeedSession> &session,
    const std::optional<ExchangeId> exchange_id, const bool truncated) {
  if (truncated) {
    ++server_stats.truncated_feed_connections;
  }
  if (exchange_id.has_value()) {
    const auto current = feed_slots[*exchange_id].lock();
    if (current == session) {
      feed_slots[*exchange_id].reset();
    }
  }
  feed_sessions.erase(session);
}

void MarketServer::Impl::remove_query(
    const std::shared_ptr<QuerySession> &session) {
  query_sessions.erase(session);
}

void MarketServer::Impl::reject_update() {
  ++server_stats.update_frames_rejected;
}

void MarketServer::Impl::apply_update(const MarketUpdate &update) {
  [[maybe_unused]] const auto result = market_state.apply(update);
  ++server_stats.update_frames_applied;
}

std::string MarketServer::Impl::handle_query(const std::string_view line) {
  const auto parsed = parse_query_request(line, config.instrument_count);
  if (!parsed.instrument_id.has_value()) {
    ++server_stats.query_requests_rejected;
    return format_query_error(parsed.error ==
                                      QueryParseError::invalid_instrument
                                  ? "instrument-out-of-range"
                                  : "malformed-command");
  }

  ++server_stats.query_requests;
  const auto result = market_state.query(*parsed.instrument_id);
  if (!result.has_value()) {
    ++server_stats.query_requests_rejected;
    return format_query_error("instrument-out-of-range");
  }
  return format_query_response(*result);
}

MarketServer::MarketServer(asio::io_context &event_loop,
                           MarketServerConfig config)
    : impl_(std::make_shared<Impl>(event_loop, std::move(config))) {}

MarketServer::~MarketServer() { impl_->stop(); }

void MarketServer::start() { impl_->start(); }

void MarketServer::stop() { impl_->stop(); }

std::uint16_t MarketServer::feed_port() const {
  return impl_->feed_acceptor.local_endpoint().port();
}

std::uint16_t MarketServer::query_port() const {
  return impl_->query_acceptor.local_endpoint().port();
}

MarketServerStats MarketServer::stats() const { return impl_->server_stats; }

const MarketState &MarketServer::market_state() const {
  return impl_->market_state;
}

} // namespace exchangelab
