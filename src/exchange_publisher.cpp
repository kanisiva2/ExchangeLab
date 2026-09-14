#include "exchangelab/exchange_publisher.hpp"

#include "exchangelab/update_codec.hpp"

#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace exchangelab {

using asio::ip::tcp;
using namespace std::chrono_literals;

namespace {

SimulatorConfig simulator_config_for(const ExchangePublisherConfig &config) {
  auto simulation = config.simulation;
  if (config.continuous) {
    // At 100,000 updates/second this practical limit lasts for thousands of
    // years while keeping the simulator's logical timestamp arithmetic safe.
    simulation.event_count = std::numeric_limits<TimestampNs>::max() / 1'000;
  }
  return simulation;
}

} // namespace

struct ExchangePublisher::Impl : std::enable_shared_from_this<Impl> {
  class FeedConnection;

  Impl(asio::io_context &event_loop, ExchangePublisherConfig publisher_config,
       std::function<void()> finished_handler);

  void start();
  void stop();
  void schedule_tick(std::chrono::milliseconds delay);
  void tick(const asio::error_code &error);
  void connection_progress();
  void frame_sent();
  bool all_connections_ready() const;
  void finish_if_ready();
  void mark_complete();

  asio::io_context &event_loop;
  ExchangePublisherConfig config;
  Simulator simulator;
  tcp::endpoint server_endpoint;
  asio::steady_timer scheduler;
  std::vector<std::shared_ptr<FeedConnection>> feeds;
  ExchangePublisherStats publisher_stats;
  std::function<void()> completion_handler;
  std::uint64_t rate_credit{};
  bool started{};
  bool stopped{};
  bool generation_complete{};
  bool complete{};
};

class ExchangePublisher::Impl::FeedConnection
    : public std::enable_shared_from_this<FeedConnection> {
public:
  FeedConnection(asio::io_context &event_loop, tcp::endpoint endpoint,
                 const std::shared_ptr<Impl> &owner)
      : socket_(event_loop), retry_timer_(event_loop),
        endpoint_(std::move(endpoint)), owner_(owner) {}

  void start() { connect(); }

  [[nodiscard]] bool can_queue() const {
    const auto owner = owner_.lock();
    return connected_ && owner &&
           queue_.size() < owner->config.maximum_queued_frames_per_exchange;
  }

  void enqueue(UpdateFrame frame) {
    queue_.push_back(std::move(frame));
    if (!write_in_progress_) {
      write_next();
    }
  }

  [[nodiscard]] bool ready() const { return connected_; }

  [[nodiscard]] bool drained() const {
    return queue_.empty() && !write_in_progress_;
  }

  void close_gracefully() {
    stopped_ = true;
    asio::error_code ignored;
    retry_timer_.cancel();
    socket_.shutdown(tcp::socket::shutdown_send, ignored);
    socket_.close(ignored);
    connected_ = false;
  }

  void stop() {
    stopped_ = true;
    queue_.clear();
    asio::error_code ignored;
    retry_timer_.cancel();
    socket_.cancel(ignored);
    socket_.close(ignored);
    connected_ = false;
    write_in_progress_ = false;
  }

private:
  void connect() {
    if (stopped_ || connecting_ || connected_) {
      return;
    }
    connecting_ = true;
    if (auto owner = owner_.lock()) {
      ++owner->publisher_stats.connection_attempts;
    }

    auto self = shared_from_this();
    socket_.async_connect(endpoint_, [self](const asio::error_code &error) {
      self->connecting_ = false;
      if (self->stopped_) {
        return;
      }
      if (error) {
        self->schedule_reconnect();
        return;
      }
      self->connected_ = true;
      if (auto owner = self->owner_.lock()) {
        ++owner->publisher_stats.connections_established;
        owner->connection_progress();
      }
      if (!self->queue_.empty()) {
        self->write_next();
      }
    });
  }

  void schedule_reconnect() {
    connected_ = false;
    write_in_progress_ = false;
    asio::error_code ignored;
    socket_.close(ignored);

    auto owner = owner_.lock();
    if (!owner || owner->stopped || stopped_) {
      return;
    }
    retry_timer_.expires_after(owner->config.reconnect_delay);
    auto self = shared_from_this();
    retry_timer_.async_wait([self](const asio::error_code &error) {
      if (!error) {
        self->connect();
      }
    });
  }

  void write_next() {
    if (stopped_ || !connected_ || write_in_progress_ || queue_.empty()) {
      return;
    }
    write_in_progress_ = true;
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(queue_.front()),
                      [self](const asio::error_code &error, std::size_t) {
                        self->write_in_progress_ = false;
                        if (self->stopped_) {
                          return;
                        }
                        if (error) {
                          self->schedule_reconnect();
                          return;
                        }

                        self->queue_.pop_front();
                        if (auto owner = self->owner_.lock()) {
                          owner->frame_sent();
                        }
                        if (!self->queue_.empty()) {
                          self->write_next();
                        }
                      });
  }

  tcp::socket socket_;
  asio::steady_timer retry_timer_;
  tcp::endpoint endpoint_;
  std::weak_ptr<Impl> owner_;
  std::deque<UpdateFrame> queue_;
  bool connecting_{};
  bool connected_{};
  bool write_in_progress_{};
  bool stopped_{};
};

ExchangePublisher::Impl::Impl(asio::io_context &event_loop,
                              ExchangePublisherConfig publisher_config,
                              std::function<void()> finished_handler)
    : event_loop(event_loop), config(std::move(publisher_config)),
      simulator(simulator_config_for(config)),
      server_endpoint(asio::ip::make_address(config.server_address),
                      config.server_port),
      scheduler(event_loop), completion_handler(std::move(finished_handler)) {
  if (config.updates_per_second == 0 ||
      config.updates_per_second > 10'000'000) {
    throw std::invalid_argument(
        "update rate must be between 1 and 10000000 per second");
  }
  if (config.maximum_queued_frames_per_exchange == 0) {
    throw std::invalid_argument(
        "publisher queue size must be greater than zero");
  }
}

void ExchangePublisher::Impl::start() {
  if (started) {
    return;
  }
  started = true;

  auto self = shared_from_this();
  feeds.reserve(config.simulation.exchange_count);
  for (ExchangeId exchange_id = 0;
       exchange_id < config.simulation.exchange_count; ++exchange_id) {
    auto feed =
        std::make_shared<FeedConnection>(event_loop, server_endpoint, self);
    feeds.push_back(feed);
    feed->start();
  }
  schedule_tick(0ms);
}

void ExchangePublisher::Impl::stop() {
  if (stopped) {
    return;
  }
  stopped = true;
  scheduler.cancel();
  for (const auto &feed : feeds) {
    feed->stop();
  }
  mark_complete();
}

void ExchangePublisher::Impl::schedule_tick(
    const std::chrono::milliseconds delay) {
  if (stopped || complete) {
    return;
  }
  scheduler.expires_after(delay);
  auto self = shared_from_this();
  scheduler.async_wait(
      [self](const asio::error_code &error) { self->tick(error); });
}

void ExchangePublisher::Impl::tick(const asio::error_code &error) {
  if (error || stopped || complete) {
    return;
  }

  if (all_connections_ready() && !generation_complete) {
    rate_credit += config.updates_per_second;
    auto updates_this_tick = rate_credit / 1'000;
    rate_credit %= 1'000;

    while (updates_this_tick > 0) {
      const auto exchange_id = static_cast<ExchangeId>(
          publisher_stats.updates_generated % config.simulation.exchange_count);
      if (!feeds[exchange_id]->can_queue()) {
        break;
      }

      MarketUpdate update;
      if (!simulator.next(update)) {
        generation_complete = true;
        break;
      }
      feeds[update.exchange_id]->enqueue(encode_update(update));
      ++publisher_stats.updates_generated;
      --updates_this_tick;
    }

    if (!config.continuous &&
        publisher_stats.updates_generated >= config.simulation.event_count) {
      generation_complete = true;
    }
  }

  finish_if_ready();
  if (!complete) {
    schedule_tick(1ms);
  }
}

void ExchangePublisher::Impl::connection_progress() { finish_if_ready(); }

void ExchangePublisher::Impl::frame_sent() {
  ++publisher_stats.updates_sent;
  finish_if_ready();
}

bool ExchangePublisher::Impl::all_connections_ready() const {
  return feeds.size() == config.simulation.exchange_count &&
         std::all_of(feeds.begin(), feeds.end(),
                     [](const auto &feed) { return feed->ready(); });
}

void ExchangePublisher::Impl::finish_if_ready() {
  if (config.continuous || !generation_complete || complete) {
    return;
  }
  const bool queues_are_empty =
      std::all_of(feeds.begin(), feeds.end(),
                  [](const auto &feed) { return feed->drained(); });
  if (!queues_are_empty) {
    return;
  }

  scheduler.cancel();
  for (const auto &feed : feeds) {
    feed->close_gracefully();
  }
  mark_complete();
}

void ExchangePublisher::Impl::mark_complete() {
  if (complete) {
    return;
  }
  complete = true;
  if (completion_handler) {
    completion_handler();
  }
}

ExchangePublisher::ExchangePublisher(asio::io_context &event_loop,
                                     ExchangePublisherConfig config,
                                     std::function<void()> completion_handler)
    : impl_(std::make_shared<Impl>(event_loop, std::move(config),
                                   std::move(completion_handler))) {}

ExchangePublisher::~ExchangePublisher() { impl_->stop(); }

void ExchangePublisher::start() { impl_->start(); }

void ExchangePublisher::stop() { impl_->stop(); }

bool ExchangePublisher::finished() const { return impl_->complete; }

ExchangePublisherStats ExchangePublisher::stats() const {
  return impl_->publisher_stats;
}

} // namespace exchangelab
