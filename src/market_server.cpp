#include "exchangelab/market_server.hpp"

#include "exchangelab/query_protocol.hpp"
#include "exchangelab/query_worker_pool.hpp"
#include "exchangelab/update_codec.hpp"
#include "exchangelab/update_stream_decoder.hpp"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace exchangelab {

using asio::ip::tcp;

std::string_view market_server_mode_name(const MarketServerMode mode) noexcept {
  switch (mode) {
  case MarketServerMode::single_threaded:
    return "single-threaded";
  case MarketServerMode::global_read:
    return "global-read";
  case MarketServerMode::striped_read:
    return "striped-read";
  case MarketServerMode::striped_write:
    return "striped-write";
  }
  return "unknown";
}

namespace {

struct AtomicServerStats {
  std::atomic<std::uint64_t> feed_connections_accepted{};
  std::atomic<std::uint64_t> query_connections_accepted{};
  std::atomic<std::uint64_t> update_frames_applied{};
  std::atomic<std::uint64_t> update_frames_rejected{};
  std::atomic<std::uint64_t> truncated_feed_connections{};
  std::atomic<std::uint64_t> query_requests{};
  std::atomic<std::uint64_t> query_requests_rejected{};
  std::atomic<std::uint64_t> query_queue_full{};

  [[nodiscard]] MarketServerStats snapshot() const {
    return MarketServerStats{
        .feed_connections_accepted =
            feed_connections_accepted.load(std::memory_order_relaxed),
        .query_connections_accepted =
            query_connections_accepted.load(std::memory_order_relaxed),
        .update_frames_applied =
            update_frames_applied.load(std::memory_order_relaxed),
        .update_frames_rejected =
            update_frames_rejected.load(std::memory_order_relaxed),
        .truncated_feed_connections =
            truncated_feed_connections.load(std::memory_order_relaxed),
        .query_requests = query_requests.load(std::memory_order_relaxed),
        .query_requests_rejected =
            query_requests_rejected.load(std::memory_order_relaxed),
        .query_queue_full = query_queue_full.load(std::memory_order_relaxed),
    };
  }
};

} // namespace

struct MarketServer::Impl : std::enable_shared_from_this<Impl> {
  class SingleThreadedFeedSession;
  class FeedBootstrapSession;
  class FeedWorker;
  class QuerySession;

  Impl(asio::io_context &event_loop, MarketServerConfig server_config);

  void start();
  void stop();
  void accept_feed();
  void accept_query();

  bool bind_single_threaded_feed(
      const std::shared_ptr<SingleThreadedFeedSession> &session,
      ExchangeId exchange_id);
  void remove_single_threaded_feed(
      const std::shared_ptr<SingleThreadedFeedSession> &session,
      std::optional<ExchangeId> exchange_id, bool truncated);
  bool bind_threaded_feed(tcp::socket &socket, const MarketUpdate &first_update);
  void threaded_feed_finished(ExchangeId exchange_id, bool truncated);
  void remove_bootstrap(
      const std::shared_ptr<FeedBootstrapSession> &session, bool truncated);
  void remove_query(const std::shared_ptr<QuerySession> &session);

  void record_feed_connection();
  void record_query_connection();
  void record_update_rejected();
  void record_truncated_feed();
  void record_query_rejected();
  void apply_single_threaded_update(const MarketUpdate &update);
  void apply_threaded_update(const MarketUpdate &update);
  void handle_query(std::string_view line,
                    const std::shared_ptr<QuerySession> &session);

  [[nodiscard]] bool threaded() const noexcept {
    return config.mode != MarketServerMode::single_threaded;
  }

  asio::io_context &event_loop;
  MarketServerConfig config;
  std::unique_ptr<MarketState> single_threaded_state;
  std::unique_ptr<ConcurrentMarketState> concurrent_state;
  std::unique_ptr<QueryWorkerPool> query_pool;
  tcp::acceptor feed_acceptor;
  tcp::acceptor query_acceptor;
  std::set<std::shared_ptr<SingleThreadedFeedSession>,
           std::owner_less<std::shared_ptr<SingleThreadedFeedSession>>>
      single_threaded_feed_sessions;
  std::set<std::shared_ptr<FeedBootstrapSession>,
           std::owner_less<std::shared_ptr<FeedBootstrapSession>>>
      bootstrap_sessions;
  std::set<std::shared_ptr<QuerySession>,
           std::owner_less<std::shared_ptr<QuerySession>>>
      query_sessions;
  std::vector<std::weak_ptr<SingleThreadedFeedSession>>
      single_threaded_feed_slots;
  std::vector<std::unique_ptr<FeedWorker>> feed_workers;
  std::vector<bool> threaded_feed_occupied;
  MarketServerStats single_threaded_stats;
  AtomicServerStats threaded_stats;
  bool started{};
  bool stopped{};
};

class MarketServer::Impl::SingleThreadedFeedSession
    : public std::enable_shared_from_this<SingleThreadedFeedSession> {
public:
  SingleThreadedFeedSession(tcp::socket socket,
                            const std::shared_ptr<Impl> &server)
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
      server->record_update_rejected();
    }

    for (const auto &update : batch.updates) {
      if (!exchange_id_.has_value()) {
        if (!server->bind_single_threaded_feed(shared_from_this(),
                                                update.exchange_id)) {
          server->record_update_rejected();
          close();
          return;
        }
        exchange_id_ = update.exchange_id;
      } else if (*exchange_id_ != update.exchange_id) {
        server->record_update_rejected();
        close();
        return;
      }
      server->apply_single_threaded_update(update);
    }
    read();
  }

  void finish(const bool truncated) {
    if (finished_) {
      return;
    }
    finished_ = true;
    if (auto server = server_.lock()) {
      server->remove_single_threaded_feed(shared_from_this(), exchange_id_,
                                           truncated);
    }
  }

  tcp::socket socket_;
  std::weak_ptr<Impl> server_;
  UpdateStreamDecoder decoder_;
  std::array<std::uint8_t, 4096> read_buffer_{};
  std::optional<ExchangeId> exchange_id_;
  bool finished_{};
};

class MarketServer::Impl::FeedWorker {
public:
  FeedWorker(const ExchangeId exchange_id, const UpdateLimits limits,
             const std::shared_ptr<Impl> &server)
      : exchange_id_(exchange_id), limits_(limits), server_(server),
        work_guard_(asio::make_work_guard(event_loop_)), socket_(event_loop_) {}

  ~FeedWorker() { stop(); }

  FeedWorker(const FeedWorker &) = delete;
  FeedWorker &operator=(const FeedWorker &) = delete;

  void start() { thread_ = std::thread([this] { event_loop_.run(); }); }

  void assign(const tcp protocol, const tcp::socket::native_handle_type handle,
              const MarketUpdate first_update) {
    asio::post(event_loop_, [this, protocol, handle, first_update] {
      if (stopped_) {
        tcp::socket abandoned(event_loop_);
        asio::error_code ignored;
        abandoned.assign(protocol, handle, ignored);
        abandoned.close(ignored);
        return;
      }

      asio::error_code error;
      socket_.assign(protocol, handle, error);
      if (error) {
        notify_finished(false);
        return;
      }

      connected_ = true;
      decoder_ = std::make_unique<UpdateStreamDecoder>(limits_);
      if (auto server = server_.lock()) {
        server->apply_threaded_update(first_update);
      }
      read();
    });
  }

  void stop() {
    if (stop_requested_.exchange(true, std::memory_order_relaxed)) {
      return;
    }

    asio::post(event_loop_, [this] {
      stopped_ = true;
      asio::error_code ignored;
      socket_.cancel(ignored);
      socket_.close(ignored);
      connected_ = false;
      decoder_.reset();
      work_guard_.reset();
    });
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  void read() {
    socket_.async_read_some(
        asio::buffer(read_buffer_),
        [this](const asio::error_code &error, const std::size_t byte_count) {
          read_complete(error, byte_count);
        });
  }

  void read_complete(const asio::error_code &error,
                     const std::size_t byte_count) {
    if (error) {
      const bool peer_ended_connection =
          error != asio::error::operation_aborted;
      const bool truncated =
          peer_ended_connection && decoder_ && decoder_->finish();
      finish(truncated);
      return;
    }

    auto batch = decoder_->append(
        std::span<const std::uint8_t>(read_buffer_.data(), byte_count));
    auto server = server_.lock();
    if (!server) {
      finish(false);
      return;
    }

    for ([[maybe_unused]] const auto decode_error : batch.errors) {
      server->record_update_rejected();
    }
    for (const auto &update : batch.updates) {
      if (update.exchange_id != exchange_id_) {
        server->record_update_rejected();
        finish(false);
        return;
      }
      server->apply_threaded_update(update);
    }
    read();
  }

  void finish(const bool truncated) {
    if (!connected_) {
      return;
    }
    connected_ = false;
    asio::error_code ignored;
    socket_.close(ignored);
    decoder_.reset();
    notify_finished(truncated);
  }

  void notify_finished(const bool truncated) {
    const auto weak_server = server_;
    if (const auto server = weak_server.lock()) {
      asio::post(server->event_loop,
                 [weak_server, exchange_id = exchange_id_, truncated] {
                   if (const auto current_server = weak_server.lock()) {
                     current_server->threaded_feed_finished(exchange_id,
                                                              truncated);
                   }
                 });
    }
  }

  ExchangeId exchange_id_;
  UpdateLimits limits_;
  std::weak_ptr<Impl> server_;
  asio::io_context event_loop_;
  asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
  tcp::socket socket_;
  std::unique_ptr<UpdateStreamDecoder> decoder_;
  std::array<std::uint8_t, 4096> read_buffer_{};
  std::thread thread_;
  std::atomic<bool> stop_requested_{};
  bool connected_{};
  bool stopped_{};
};

class MarketServer::Impl::FeedBootstrapSession
    : public std::enable_shared_from_this<FeedBootstrapSession> {
public:
  FeedBootstrapSession(tcp::socket socket, const std::shared_ptr<Impl> &server)
      : socket_(std::move(socket)), server_(server) {}

  void start() {
    auto self = shared_from_this();
    asio::async_read(
        socket_, asio::buffer(first_frame_),
        [self](const asio::error_code &error, const std::size_t byte_count) {
          self->read_complete(error, byte_count);
        });
  }

  void close() {
    asio::error_code ignored;
    socket_.cancel(ignored);
    socket_.close(ignored);
    finish(false);
  }

private:
  void read_complete(const asio::error_code &error,
                     const std::size_t byte_count) {
    auto server = server_.lock();
    if (!server) {
      close();
      return;
    }
    if (error) {
      finish(error != asio::error::operation_aborted && byte_count > 0);
      return;
    }

    const auto decoded = decode_update(
        first_frame_,
        {server->config.exchange_count, server->config.instrument_count});
    if (!decoded.update.has_value()) {
      server->record_update_rejected();
      finish(false);
      return;
    }
    if (!server->bind_threaded_feed(socket_, *decoded.update)) {
      server->record_update_rejected();
      finish(false);
      return;
    }
    finish(false);
  }

  void finish(const bool truncated) {
    if (finished_) {
      return;
    }
    finished_ = true;
    asio::error_code ignored;
    socket_.close(ignored);
    if (auto server = server_.lock()) {
      server->remove_bootstrap(shared_from_this(), truncated);
    }
  }

  tcp::socket socket_;
  std::weak_ptr<Impl> server_;
  UpdateFrame first_frame_{};
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

  void write_response(std::string response, const bool close_after_write) {
    if (finished_) {
      return;
    }
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
          server->record_query_rejected();
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
      server->handle_query(line, shared_from_this());
    } else {
      close();
    }
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
      feed_acceptor(event_loop), query_acceptor(event_loop),
      single_threaded_feed_slots(config.exchange_count),
      threaded_feed_occupied(config.exchange_count) {
  switch (config.mode) {
  case MarketServerMode::single_threaded:
    single_threaded_state = std::make_unique<MarketState>(
        config.exchange_count, config.instrument_count);
    break;
  case MarketServerMode::global_read:
    concurrent_state = std::make_unique<GlobalMarketState>(
        config.exchange_count, config.instrument_count);
    break;
  case MarketServerMode::striped_read:
    concurrent_state = std::make_unique<StripedMarketState>(
        config.exchange_count, config.instrument_count, config.stripe_count,
        SortingMode::on_read);
    break;
  case MarketServerMode::striped_write:
    concurrent_state = std::make_unique<StripedMarketState>(
        config.exchange_count, config.instrument_count, config.stripe_count,
        SortingMode::on_write);
    break;
  }

  if (threaded()) {
    query_pool = std::make_unique<QueryWorkerPool>(
        config.query_worker_count, config.query_queue_capacity);
  }

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

  if (threaded()) {
    auto self = shared_from_this();
    feed_workers.reserve(config.exchange_count);
    for (ExchangeId exchange_id = 0; exchange_id < config.exchange_count;
         ++exchange_id) {
      auto worker = std::make_unique<FeedWorker>(
          exchange_id,
          UpdateLimits{config.exchange_count, config.instrument_count}, self);
      worker->start();
      feed_workers.push_back(std::move(worker));
    }
    query_pool->start();
  }

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

  const auto single_feeds = single_threaded_feed_sessions;
  for (const auto &session : single_feeds) {
    session->close();
  }
  const auto bootstraps = bootstrap_sessions;
  for (const auto &session : bootstraps) {
    session->close();
  }
  const auto queries = query_sessions;
  for (const auto &session : queries) {
    session->close();
  }

  for (const auto &worker : feed_workers) {
    worker->stop();
  }
  if (query_pool) {
    query_pool->stop();
  }
}

void MarketServer::Impl::accept_feed() {
  auto self = shared_from_this();
  feed_acceptor.async_accept(
      [self](const asio::error_code &error, tcp::socket socket) {
        if (!error && !self->stopped) {
          self->record_feed_connection();
          if (self->threaded()) {
            auto session =
                std::make_shared<FeedBootstrapSession>(std::move(socket), self);
            self->bootstrap_sessions.insert(session);
            session->start();
          } else {
            auto session = std::make_shared<SingleThreadedFeedSession>(
                std::move(socket), self);
            self->single_threaded_feed_sessions.insert(session);
            session->start();
          }
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
      self->record_query_connection();
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

bool MarketServer::Impl::bind_single_threaded_feed(
    const std::shared_ptr<SingleThreadedFeedSession> &session,
    const ExchangeId exchange_id) {
  if (exchange_id >= single_threaded_feed_slots.size()) {
    return false;
  }
  if (const auto current = single_threaded_feed_slots[exchange_id].lock();
      current && current != session) {
    return false;
  }
  single_threaded_feed_slots[exchange_id] = session;
  return true;
}

void MarketServer::Impl::remove_single_threaded_feed(
    const std::shared_ptr<SingleThreadedFeedSession> &session,
    const std::optional<ExchangeId> exchange_id, const bool truncated) {
  if (truncated) {
    record_truncated_feed();
  }
  if (exchange_id.has_value()) {
    const auto current = single_threaded_feed_slots[*exchange_id].lock();
    if (current == session) {
      single_threaded_feed_slots[*exchange_id].reset();
    }
  }
  single_threaded_feed_sessions.erase(session);
}

bool MarketServer::Impl::bind_threaded_feed(
    tcp::socket &socket, const MarketUpdate &first_update) {
  const auto exchange_id = first_update.exchange_id;
  if (exchange_id >= threaded_feed_occupied.size() ||
      threaded_feed_occupied[exchange_id]) {
    return false;
  }

  asio::error_code error;
  const auto protocol = socket.local_endpoint(error).protocol();
  if (error) {
    return false;
  }
  const auto handle = socket.release(error);
  if (error) {
    return false;
  }

  threaded_feed_occupied[exchange_id] = true;
  feed_workers[exchange_id]->assign(protocol, handle, first_update);
  return true;
}

void MarketServer::Impl::threaded_feed_finished(
    const ExchangeId exchange_id, const bool truncated) {
  if (exchange_id < threaded_feed_occupied.size()) {
    threaded_feed_occupied[exchange_id] = false;
  }
  if (truncated) {
    record_truncated_feed();
  }
}

void MarketServer::Impl::remove_bootstrap(
    const std::shared_ptr<FeedBootstrapSession> &session,
    const bool truncated) {
  if (truncated) {
    record_truncated_feed();
  }
  bootstrap_sessions.erase(session);
}

void MarketServer::Impl::remove_query(
    const std::shared_ptr<QuerySession> &session) {
  query_sessions.erase(session);
}

void MarketServer::Impl::record_feed_connection() {
  if (threaded()) {
    threaded_stats.feed_connections_accepted.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    ++single_threaded_stats.feed_connections_accepted;
  }
}

void MarketServer::Impl::record_query_connection() {
  if (threaded()) {
    threaded_stats.query_connections_accepted.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    ++single_threaded_stats.query_connections_accepted;
  }
}

void MarketServer::Impl::record_update_rejected() {
  if (threaded()) {
    threaded_stats.update_frames_rejected.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    ++single_threaded_stats.update_frames_rejected;
  }
}

void MarketServer::Impl::record_truncated_feed() {
  if (threaded()) {
    threaded_stats.truncated_feed_connections.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    ++single_threaded_stats.truncated_feed_connections;
  }
}

void MarketServer::Impl::record_query_rejected() {
  if (threaded()) {
    threaded_stats.query_requests_rejected.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    ++single_threaded_stats.query_requests_rejected;
  }
}

void MarketServer::Impl::apply_single_threaded_update(
    const MarketUpdate &update) {
  [[maybe_unused]] const auto result = single_threaded_state->apply(update);
  ++single_threaded_stats.update_frames_applied;
}

void MarketServer::Impl::apply_threaded_update(const MarketUpdate &update) {
  [[maybe_unused]] const auto result = concurrent_state->apply(update);
  threaded_stats.update_frames_applied.fetch_add(1,
                                                  std::memory_order_relaxed);
}

void MarketServer::Impl::handle_query(
    const std::string_view line, const std::shared_ptr<QuerySession> &session) {
  const auto parsed = parse_query_request(line, config.instrument_count);
  if (!parsed.instrument_id.has_value()) {
    record_query_rejected();
    session->write_response(
        format_query_error(parsed.error == QueryParseError::invalid_instrument
                               ? "instrument-out-of-range"
                               : "malformed-command"),
        false);
    return;
  }

  if (!threaded()) {
    ++single_threaded_stats.query_requests;
    const auto result = single_threaded_state->query(*parsed.instrument_id);
    session->write_response(result.has_value()
                                ? format_query_response(*result)
                                : format_query_error("instrument-out-of-range"),
                            false);
    return;
  }

  const auto instrument_id = *parsed.instrument_id;
  const std::weak_ptr<Impl> weak_server = shared_from_this();
  const std::weak_ptr<QuerySession> weak_session = session;
  const bool accepted = query_pool->try_submit(
      [weak_server, weak_session, instrument_id] {
        const auto server = weak_server.lock();
        if (!server || weak_session.expired()) {
          return;
        }
        const auto result = server->concurrent_state->query(instrument_id);
        auto response = result.has_value()
                            ? format_query_response(*result)
                            : format_query_error("instrument-out-of-range");
        asio::post(server->event_loop,
                   [weak_server, weak_session,
                    response = std::move(response)]() mutable {
                     const auto current_server = weak_server.lock();
                     const auto current_session = weak_session.lock();
                     if (current_server && current_session &&
                         !current_server->stopped) {
                       current_session->write_response(std::move(response),
                                                       false);
                     }
                   });
      });

  if (!accepted) {
    threaded_stats.query_requests_rejected.fetch_add(
        1, std::memory_order_relaxed);
    threaded_stats.query_queue_full.fetch_add(1, std::memory_order_relaxed);
    session->write_response(format_query_error("busy"), false);
  } else {
    threaded_stats.query_requests.fetch_add(1, std::memory_order_relaxed);
  }
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

MarketServerStats MarketServer::stats() const {
  return impl_->threaded() ? impl_->threaded_stats.snapshot()
                           : impl_->single_threaded_stats;
}

MarketStats MarketServer::market_stats() const {
  return impl_->threaded() ? impl_->concurrent_state->stats()
                           : impl_->single_threaded_state->stats();
}

std::uint64_t MarketServer::logical_checksum() const {
  return impl_->threaded() ? impl_->concurrent_state->logical_checksum()
                           : impl_->single_threaded_state->logical_checksum();
}

} // namespace exchangelab
