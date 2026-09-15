#include "exchangelab/benchmark.hpp"

#include "exchangelab/concurrent_market_state.hpp"
#include "exchangelab/market_state.hpp"
#include "exchangelab/query_client.hpp"
#include "exchangelab/query_protocol.hpp"
#include "exchangelab/update_codec.hpp"

#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/utsname.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace exchangelab {
namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
using asio::ip::tcp;
using namespace std::chrono_literals;

struct PreparedWorkload {
  std::vector<MarketUpdate> warmup_updates;
  std::vector<MarketUpdate> measurement_updates;
  std::vector<std::vector<MarketUpdate>> warmup_updates_by_exchange;
  std::vector<std::vector<MarketUpdate>> measurement_updates_by_exchange;
  std::vector<InstrumentId> warmup_queries;
  std::vector<InstrumentId> measurement_queries;
};

struct PhaseMetrics {
  std::uint64_t successful_updates{};
  std::uint64_t successful_queries{};
  std::uint64_t errors{};
  std::uint64_t rejected_requests{};
  std::uint64_t queue_full_responses{};
  std::uint64_t update_frames_rejected{};
  double seconds{};
  std::vector<std::uint64_t> query_latencies_ns;
};

std::string distribution_name(const TrafficDistribution distribution) {
  return distribution == TrafficDistribution::uniform ? "uniform" : "hot";
}

std::string level_name(const BenchmarkLevel level) {
  return level == BenchmarkLevel::direct ? "direct" : "tcp";
}

InstrumentId next_query_instrument(std::mt19937_64 &random,
                                   const std::uint32_t instrument_count,
                                   const TrafficDistribution distribution) {
  if (distribution == TrafficDistribution::hot && random() % 100 < 80) {
    const auto hot_count = std::max<std::uint32_t>(1, instrument_count / 100);
    return static_cast<InstrumentId>(random() % hot_count);
  }
  return static_cast<InstrumentId>(random() % instrument_count);
}

PreparedWorkload prepare_workload(const BenchmarkCase &benchmark_case,
                                  const BenchmarkSettings &settings) {
  const auto [warmup_update_count, warmup_query_count] =
      split_operations(settings.warmup_operations,
                       benchmark_case.update_percent);
  const auto [measurement_update_count, measurement_query_count] =
      split_operations(settings.measurement_operations,
                       benchmark_case.update_percent);

  PreparedWorkload workload;
  workload.warmup_updates.reserve(warmup_update_count);
  workload.measurement_updates.reserve(measurement_update_count);
  workload.warmup_updates_by_exchange.resize(settings.exchange_count);
  workload.measurement_updates_by_exchange.resize(settings.exchange_count);
  workload.warmup_queries.reserve(warmup_query_count);
  workload.measurement_queries.reserve(measurement_query_count);

  Simulator simulator(SimulatorConfig{
      .exchange_count = settings.exchange_count,
      .instrument_count = settings.instrument_count,
      .event_count = warmup_update_count + measurement_update_count,
      .seed = settings.seed,
      .distribution = benchmark_case.distribution,
  });
  MarketUpdate update;
  for (std::uint64_t index = 0;
       index < warmup_update_count + measurement_update_count; ++index) {
    if (!simulator.next(update)) {
      throw std::logic_error("simulator ended before the prepared workload");
    }
    if (index < warmup_update_count) {
      workload.warmup_updates.push_back(update);
      workload.warmup_updates_by_exchange[update.exchange_id].push_back(update);
    } else {
      workload.measurement_updates.push_back(update);
      workload.measurement_updates_by_exchange[update.exchange_id].push_back(
          update);
    }
  }

  std::mt19937_64 query_random(settings.seed ^ 0x9e3779b97f4a7c15ULL);
  for (std::uint64_t index = 0;
       index < warmup_query_count + measurement_query_count; ++index) {
    const auto instrument = next_query_instrument(
        query_random, settings.instrument_count, benchmark_case.distribution);
    if (index < warmup_query_count) {
      workload.warmup_queries.push_back(instrument);
    } else {
      workload.measurement_queries.push_back(instrument);
    }
  }
  return workload;
}

bool accepted(const ApplyResult &result) {
  return result.status == ApplyStatus::accepted ||
         result.status == ApplyStatus::accepted_with_gap;
}

template <typename Apply, typename Query>
PhaseMetrics run_serial_phase(const std::vector<MarketUpdate> &updates,
                              const std::vector<InstrumentId> &queries,
                              const bool collect_latencies, Apply apply,
                              Query query) {
  PhaseMetrics metrics;
  metrics.query_latencies_ns.reserve(collect_latencies ? queries.size() : 0);
  std::size_t update_index{};
  std::size_t query_index{};
  std::uint64_t update_credit{};
  const auto operation_count = updates.size() + queries.size();
  const auto started = Clock::now();

  for (std::size_t operation = 0; operation < operation_count; ++operation) {
    update_credit += updates.size();
    if (update_index < updates.size() &&
        (query_index >= queries.size() || update_credit >= operation_count)) {
      update_credit -= operation_count;
      if (accepted(apply(updates[update_index++]))) {
        ++metrics.successful_updates;
      } else {
        ++metrics.errors;
      }
      continue;
    }

    const auto query_started = Clock::now();
    const auto result = query(queries[query_index++]);
    const auto query_finished = Clock::now();
    if (result.has_value()) {
      ++metrics.successful_queries;
      if (collect_latencies) {
        metrics.query_latencies_ns.push_back(
            std::chrono::duration_cast<Nanoseconds>(query_finished -
                                                    query_started)
                .count());
      }
    } else {
      ++metrics.errors;
    }
  }

  metrics.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  return metrics;
}

PhaseMetrics run_concurrent_phase(
    ConcurrentMarketState &engine,
    const std::vector<std::vector<MarketUpdate>> &updates_by_exchange,
    const std::vector<InstrumentId> &queries, const std::size_t query_workers,
    const bool collect_latencies) {
  if (query_workers == 0) {
    throw std::invalid_argument("direct concurrent runs need query workers");
  }

  PhaseMetrics metrics;
  std::atomic<std::uint64_t> successful_updates{};
  std::atomic<std::uint64_t> successful_queries{};
  std::atomic<std::uint64_t> errors{};
  std::vector<std::vector<std::uint64_t>> local_latencies(query_workers);
  std::vector<std::thread> threads;
  threads.reserve(updates_by_exchange.size() + query_workers);
  Clock::time_point started;
  std::barrier start_barrier(
      static_cast<std::ptrdiff_t>(updates_by_exchange.size() + query_workers +
                                  1),
      [&started]() noexcept { started = Clock::now(); });

  for (std::size_t exchange = 0; exchange < updates_by_exchange.size();
       ++exchange) {
    threads.emplace_back([&, exchange] {
      start_barrier.arrive_and_wait();
      for (const auto &update : updates_by_exchange[exchange]) {
        if (accepted(engine.apply(update))) {
          successful_updates.fetch_add(1, std::memory_order_relaxed);
        } else {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (std::size_t worker = 0; worker < query_workers; ++worker) {
    threads.emplace_back([&, worker] {
      auto &latencies = local_latencies[worker];
      if (collect_latencies) {
        latencies.reserve((queries.size() + query_workers - 1) /
                          query_workers);
      }
      start_barrier.arrive_and_wait();
      for (std::size_t index = worker; index < queries.size();
           index += query_workers) {
        const auto query_started = Clock::now();
        const auto result = engine.query(queries[index]);
        const auto query_finished = Clock::now();
        if (result.has_value()) {
          successful_queries.fetch_add(1, std::memory_order_relaxed);
          if (collect_latencies) {
            latencies.push_back(
                std::chrono::duration_cast<Nanoseconds>(query_finished -
                                                        query_started)
                    .count());
          }
        } else {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  start_barrier.arrive_and_wait();
  for (auto &thread : threads) {
    thread.join();
  }
  metrics.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  metrics.successful_updates = successful_updates.load();
  metrics.successful_queries = successful_queries.load();
  metrics.errors = errors.load();
  for (auto &latencies : local_latencies) {
    metrics.query_latencies_ns.insert(metrics.query_latencies_ns.end(),
                                      latencies.begin(), latencies.end());
  }
  return metrics;
}

class RunningBenchmarkServer {
public:
  RunningBenchmarkServer(const BenchmarkCase &benchmark_case,
                         const BenchmarkSettings &settings)
      : server_(event_loop_, MarketServerConfig{
                                 .feed_port = 0,
                                 .query_port = 0,
                                 .exchange_count = settings.exchange_count,
                                 .instrument_count = settings.instrument_count,
                                 .mode = benchmark_case.mode,
                                 .query_worker_count =
                                     benchmark_case.query_worker_count,
                                 .query_queue_capacity = 256,
                                 .stripe_count = settings.stripe_count,
                             }) {
    server_.start();
    thread_ = std::thread([this] { event_loop_.run(); });
  }

  ~RunningBenchmarkServer() { stop(); }

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

  bool wait_for_processed_updates(const std::uint64_t count,
                                  const std::chrono::seconds timeout = 60s) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      const auto stats = snapshot_stats();
      if (stats.update_frames_applied + stats.update_frames_rejected >= count) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
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

  [[nodiscard]] MarketStats market_stats_after_stop() const {
    return server_.market_stats();
  }

  [[nodiscard]] std::uint64_t checksum_after_stop() const {
    return server_.logical_checksum();
  }

private:
  asio::io_context event_loop_;
  MarketServer server_;
  std::thread thread_;
};

PhaseMetrics run_tcp_phase(
    RunningBenchmarkServer &server,
    std::vector<std::unique_ptr<tcp::socket>> &feed_sockets,
    std::vector<std::unique_ptr<QueryClient>> &query_clients,
    const std::vector<std::vector<MarketUpdate>> &updates_by_exchange,
    const std::vector<InstrumentId> &queries, const bool collect_latencies) {
  const auto before = server.snapshot_stats();
  std::atomic<std::uint64_t> client_successes{};
  std::atomic<std::uint64_t> client_errors{};
  std::atomic<std::uint64_t> busy_responses{};
  std::vector<std::vector<std::uint64_t>> local_latencies(query_clients.size());
  std::vector<std::thread> threads;
  threads.reserve(feed_sockets.size() + query_clients.size());
  Clock::time_point started;
  std::barrier start_barrier(
      static_cast<std::ptrdiff_t>(feed_sockets.size() + query_clients.size() +
                                  1),
      [&started]() noexcept { started = Clock::now(); });

  for (std::size_t exchange = 0; exchange < feed_sockets.size(); ++exchange) {
    threads.emplace_back([&, exchange] {
      start_barrier.arrive_and_wait();
      for (const auto &update : updates_by_exchange[exchange]) {
        const auto frame = encode_update(update);
        asio::error_code error;
        asio::write(*feed_sockets[exchange], asio::buffer(frame), error);
        if (error) {
          client_errors.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    });
  }

  for (std::size_t client_index = 0; client_index < query_clients.size();
       ++client_index) {
    threads.emplace_back([&, client_index] {
      auto &latencies = local_latencies[client_index];
      if (collect_latencies) {
        latencies.reserve((queries.size() + query_clients.size() - 1) /
                          query_clients.size());
      }
      start_barrier.arrive_and_wait();
      for (std::size_t index = client_index; index < queries.size();
           index += query_clients.size()) {
        std::string error;
        const auto query_started = Clock::now();
        const auto response = query_clients[client_index]->query(queries[index],
                                                                 error);
        const auto query_finished = Clock::now();
        if (!response.has_value()) {
          client_errors.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (response->starts_with("ERROR busy\n")) {
          busy_responses.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (response->starts_with("ERROR ")) {
          client_errors.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        client_successes.fetch_add(1, std::memory_order_relaxed);
        if (collect_latencies) {
          latencies.push_back(
              std::chrono::duration_cast<Nanoseconds>(query_finished -
                                                      query_started)
                  .count());
        }
      }
    });
  }

  start_barrier.arrive_and_wait();
  for (auto &thread : threads) {
    thread.join();
  }
  const auto expected_processed = before.update_frames_applied +
                                  before.update_frames_rejected +
                                  std::accumulate(
                                      updates_by_exchange.begin(),
                                      updates_by_exchange.end(), std::uint64_t{},
                                      [](const std::uint64_t total,
                                         const auto &stream) {
                                        return total + stream.size();
                                      });
  if (!server.wait_for_processed_updates(expected_processed)) {
    client_errors.fetch_add(1, std::memory_order_relaxed);
  }
  const auto finished = Clock::now();
  const auto after = server.snapshot_stats();

  PhaseMetrics metrics;
  metrics.seconds = std::chrono::duration<double>(finished - started).count();
  metrics.successful_updates =
      after.update_frames_applied - before.update_frames_applied;
  metrics.successful_queries = client_successes.load();
  metrics.errors = client_errors.load();
  metrics.rejected_requests =
      after.query_requests_rejected - before.query_requests_rejected;
  metrics.queue_full_responses =
      after.query_queue_full - before.query_queue_full;
  metrics.update_frames_rejected =
      after.update_frames_rejected - before.update_frames_rejected;
  if (metrics.queue_full_responses != busy_responses.load()) {
    ++metrics.errors;
  }
  for (auto &latencies : local_latencies) {
    metrics.query_latencies_ns.insert(metrics.query_latencies_ns.end(),
                                      latencies.begin(), latencies.end());
  }
  return metrics;
}

std::unique_ptr<ConcurrentMarketState>
make_concurrent_engine(const BenchmarkCase &benchmark_case,
                       const BenchmarkSettings &settings) {
  switch (benchmark_case.mode) {
  case MarketServerMode::global_read:
    return std::make_unique<GlobalMarketState>(settings.exchange_count,
                                                settings.instrument_count);
  case MarketServerMode::striped_read:
    return std::make_unique<StripedMarketState>(
        settings.exchange_count, settings.instrument_count,
        settings.stripe_count, SortingMode::on_read);
  case MarketServerMode::striped_write:
    return std::make_unique<StripedMarketState>(
        settings.exchange_count, settings.instrument_count,
        settings.stripe_count, SortingMode::on_write);
  case MarketServerMode::single_threaded:
    break;
  }
  throw std::invalid_argument("single-threaded has no concurrent engine");
}

MarketState build_oracle(const BenchmarkSettings &settings,
                         const PreparedWorkload &workload) {
  MarketState oracle(settings.exchange_count, settings.instrument_count);
  for (const auto &update : workload.warmup_updates) {
    if (!accepted(oracle.apply(update))) {
      throw std::logic_error("oracle rejected a warm-up update");
    }
  }
  for (const auto &update : workload.measurement_updates) {
    if (!accepted(oracle.apply(update))) {
      throw std::logic_error("oracle rejected a measured update");
    }
  }
  return oracle;
}

std::string cpu_model() {
#if defined(__APPLE__)
  std::size_t size{};
  if (sysctlbyname("machdep.cpu.brand_string", nullptr, &size, nullptr, 0) ==
          0 &&
      size > 1) {
    std::string model(size, '\0');
    if (sysctlbyname("machdep.cpu.brand_string", model.data(), &size, nullptr,
                     0) == 0) {
      model.resize(std::char_traits<char>::length(model.c_str()));
      return model;
    }
  }
  size = 0;
  if (sysctlbyname("hw.model", nullptr, &size, nullptr, 0) == 0 && size > 1) {
    std::string model(size, '\0');
    if (sysctlbyname("hw.model", model.data(), &size, nullptr, 0) == 0) {
      model.resize(std::char_traits<char>::length(model.c_str()));
      return model;
    }
  }
#endif
  return "unavailable";
}

void add_system_metadata(BenchmarkResult &result) {
#if defined(__unix__) || defined(__APPLE__)
  utsname information{};
  if (uname(&information) == 0) {
    result.system_name = information.sysname;
    result.system_version = information.release;
    result.architecture = information.machine;
  } else {
    result.system_name = "unknown";
    result.system_version = "unknown";
    result.architecture = "unknown";
  }
#else
  result.system_name = "unknown";
  result.system_version = "unknown";
  result.architecture = "unknown";
#endif
  result.cpu_model = cpu_model();
  result.logical_cpu_count = std::thread::hardware_concurrency();
  result.build_type = EXCHANGELAB_BUILD_TYPE;
  result.compiler = EXCHANGELAB_COMPILER_ID;
  result.compiler_version = EXCHANGELAB_COMPILER_VERSION;
  result.git_revision = EXCHANGELAB_GIT_REVISION;
  result.git_dirty = EXCHANGELAB_GIT_DIRTY != 0;
}

void add_latency_metrics(BenchmarkResult &result,
                         std::vector<std::uint64_t> latencies) {
  if (latencies.empty()) {
    return;
  }
  const auto percentiles = calculate_latency_percentiles(std::move(latencies));
  result.query_p50_us = static_cast<double>(percentiles.p50_ns) / 1'000.0;
  result.query_p95_us = static_cast<double>(percentiles.p95_ns) / 1'000.0;
  result.query_p99_us = static_cast<double>(percentiles.p99_ns) / 1'000.0;
}

BenchmarkResult base_result(const BenchmarkCase &benchmark_case,
                            const BenchmarkSettings &settings,
                            const std::size_t repetition) {
  const auto [updates, queries] = split_operations(
      settings.measurement_operations, benchmark_case.update_percent);
  BenchmarkResult result;
  result.level = level_name(benchmark_case.level);
  result.suite = benchmark_case.suite;
  result.mode = std::string(market_server_mode_name(benchmark_case.mode));
  result.repetition = repetition;
  result.seed = settings.seed;
  result.exchange_count = settings.exchange_count;
  result.instrument_count = settings.instrument_count;
  result.measurement_operations = settings.measurement_operations;
  result.requested_updates = updates;
  result.requested_queries = queries;
  result.update_percent = benchmark_case.update_percent;
  result.query_percent = 100 - benchmark_case.update_percent;
  result.distribution = distribution_name(benchmark_case.distribution);
  result.writer_count = benchmark_case.mode == MarketServerMode::single_threaded
                            ? 1
                            : settings.exchange_count;
  result.concurrent_client_count =
      benchmark_case.level == BenchmarkLevel::tcp
          ? benchmark_case.concurrent_client_count
          : 0;
  result.query_worker_count =
      benchmark_case.mode == MarketServerMode::single_threaded
          ? 0
          : benchmark_case.query_worker_count;
  result.stripe_count =
      benchmark_case.mode == MarketServerMode::striped_read ||
              benchmark_case.mode == MarketServerMode::striped_write
          ? settings.stripe_count
          : 0;
  result.warmup_operations = settings.warmup_operations;
  add_system_metadata(result);
  return result;
}

BenchmarkResult run_direct(const BenchmarkCase &benchmark_case,
                           const BenchmarkSettings &settings,
                           const std::size_t repetition,
                           const bool full_verification) {
  const auto workload = prepare_workload(benchmark_case, settings);
  const auto oracle = build_oracle(settings, workload);
  auto result = base_result(benchmark_case, settings, repetition);
  PhaseMetrics warmup;
  PhaseMetrics measured;
  MarketStats final_stats;
  std::uint64_t final_checksum{};
  bool queries_match = true;

  if (benchmark_case.mode == MarketServerMode::single_threaded) {
    MarketState state(settings.exchange_count, settings.instrument_count);
    warmup = run_serial_phase(
        workload.warmup_updates, workload.warmup_queries,
        false,
        [&state](const MarketUpdate &update) { return state.apply(update); },
        [&state](const InstrumentId instrument) {
          return state.query(instrument);
        });
    measured = run_serial_phase(
        workload.measurement_updates, workload.measurement_queries,
        true,
        [&state](const MarketUpdate &update) { return state.apply(update); },
        [&state](const InstrumentId instrument) {
          return state.query(instrument);
        });
    final_stats = state.stats();
    final_checksum = state.logical_checksum();
    if (full_verification) {
      for (InstrumentId instrument = 0;
           instrument < settings.instrument_count; ++instrument) {
        if (state.query(instrument) != oracle.query(instrument)) {
          queries_match = false;
          break;
        }
      }
    }
  } else {
    auto engine = make_concurrent_engine(benchmark_case, settings);
    warmup = run_concurrent_phase(
        *engine, workload.warmup_updates_by_exchange,
        workload.warmup_queries, benchmark_case.query_worker_count,
        false);
    measured = run_concurrent_phase(
        *engine, workload.measurement_updates_by_exchange,
        workload.measurement_queries, benchmark_case.query_worker_count,
        true);
    final_stats = engine->stats();
    final_checksum = engine->logical_checksum();
    if (full_verification) {
      for (InstrumentId instrument = 0;
           instrument < settings.instrument_count; ++instrument) {
        if (engine->query(instrument) != oracle.query(instrument)) {
          queries_match = false;
          break;
        }
      }
    }
  }

  result.warmup_seconds = warmup.seconds;
  result.measurement_seconds = measured.seconds;
  result.successful_updates = measured.successful_updates;
  result.successful_queries = measured.successful_queries;
  result.updates_per_second = measured.successful_updates / measured.seconds;
  result.queries_per_second = measured.successful_queries / measured.seconds;
  result.errors = warmup.errors + measured.errors;
  result.logical_checksum = final_checksum;
  result.oracle_checksum = oracle.logical_checksum();
  result.correct = result.errors == 0 && queries_match &&
                   final_stats == oracle.stats() &&
                   final_checksum == oracle.logical_checksum() &&
                   measured.successful_updates == result.requested_updates &&
                   measured.successful_queries == result.requested_queries;
  add_latency_metrics(result, std::move(measured.query_latencies_ns));
  return result;
}

BenchmarkResult run_tcp(const BenchmarkCase &benchmark_case,
                        const BenchmarkSettings &settings,
                        const std::size_t repetition,
                        const bool full_verification) {
  const auto workload = prepare_workload(benchmark_case, settings);
  const auto oracle = build_oracle(settings, workload);
  auto result = base_result(benchmark_case, settings, repetition);
  RunningBenchmarkServer server(benchmark_case, settings);

  asio::io_context feed_context;
  std::vector<std::unique_ptr<tcp::socket>> feed_sockets;
  feed_sockets.reserve(settings.exchange_count);
  const tcp::endpoint feed_endpoint(asio::ip::make_address("127.0.0.1"),
                                    server.feed_port());
  for (ExchangeId exchange = 0; exchange < settings.exchange_count;
       ++exchange) {
    auto socket = std::make_unique<tcp::socket>(feed_context);
    socket->connect(feed_endpoint);
    feed_sockets.push_back(std::move(socket));
  }

  std::vector<std::unique_ptr<QueryClient>> query_clients;
  query_clients.reserve(benchmark_case.concurrent_client_count);
  for (std::size_t index = 0;
       index < benchmark_case.concurrent_client_count; ++index) {
    auto client =
        std::make_unique<QueryClient>("127.0.0.1", server.query_port());
    std::string error;
    if (!client->connect(error)) {
      throw std::runtime_error("benchmark query client could not connect: " +
                               error);
    }
    query_clients.push_back(std::move(client));
  }

  const auto warmup = run_tcp_phase(
      server, feed_sockets, query_clients, workload.warmup_updates_by_exchange,
      workload.warmup_queries, false);
  const auto measured = run_tcp_phase(
      server, feed_sockets, query_clients,
      workload.measurement_updates_by_exchange, workload.measurement_queries,
      true);

  bool queries_match = true;
  if (full_verification && !query_clients.empty()) {
    for (InstrumentId instrument = 0; instrument < settings.instrument_count;
         ++instrument) {
      std::string error;
      const auto response = query_clients.front()->query(instrument, error);
      const auto expected = oracle.query(instrument);
      if (!response.has_value() || !expected.has_value() ||
          *response != format_query_response(*expected)) {
        queries_match = false;
        break;
      }
    }
  }

  for (auto &client : query_clients) {
    client->close();
  }
  for (auto &socket : feed_sockets) {
    asio::error_code ignored;
    socket->shutdown(tcp::socket::shutdown_both, ignored);
    socket->close(ignored);
  }
  server.stop();
  const auto final_stats = server.market_stats_after_stop();
  const auto final_checksum = server.checksum_after_stop();

  result.warmup_seconds = warmup.seconds;
  result.measurement_seconds = measured.seconds;
  result.successful_updates = measured.successful_updates;
  result.successful_queries = measured.successful_queries;
  result.updates_per_second = measured.successful_updates / measured.seconds;
  result.queries_per_second = measured.successful_queries / measured.seconds;
  result.errors = warmup.errors + measured.errors;
  result.rejected_requests = measured.rejected_requests;
  result.queue_full_responses = measured.queue_full_responses;
  result.update_frames_rejected = measured.update_frames_rejected;
  result.logical_checksum = final_checksum;
  result.oracle_checksum = oracle.logical_checksum();
  result.correct = result.errors == 0 && queries_match &&
                   final_stats == oracle.stats() &&
                   final_checksum == oracle.logical_checksum() &&
                   measured.successful_updates == result.requested_updates &&
                   measured.successful_queries +
                           measured.queue_full_responses ==
                       result.requested_queries &&
                   measured.rejected_requests ==
                       measured.queue_full_responses &&
                   measured.update_frames_rejected == 0;
  add_latency_metrics(result, measured.query_latencies_ns);
  return result;
}

std::string csv_string(const std::string &value) {
  if (value.find_first_of(",\"\n") == std::string::npos) {
    return value;
  }
  std::string escaped{"\""};
  for (const char character : value) {
    if (character == '\"') {
      escaped += "\"\"";
    } else {
      escaped += character;
    }
  }
  return escaped + '\"';
}

void append_case(std::vector<BenchmarkCase> &cases, const std::string &suite,
                 const BenchmarkLevel level, const MarketServerMode mode,
                 const std::uint32_t update_percent,
                 const TrafficDistribution distribution,
                 const std::size_t workers = 4,
                 const std::size_t clients = 16) {
  cases.push_back(BenchmarkCase{
      .suite = suite,
      .level = level,
      .mode = mode,
      .update_percent = update_percent,
      .distribution = distribution,
      .query_worker_count = workers,
      .concurrent_client_count = clients,
  });
}

} // namespace

std::pair<std::uint64_t, std::uint64_t>
split_operations(const std::uint64_t total,
                 const std::uint32_t update_percent) {
  if (total == 0) {
    throw std::invalid_argument("operation count must be greater than zero");
  }
  if (update_percent == 0 || update_percent >= 100) {
    throw std::invalid_argument("update percent must be between 1 and 99");
  }
  const auto quotient = total / 100;
  const auto remainder = total % 100;
  const auto updates = quotient * update_percent +
                       remainder * update_percent / 100;
  return {updates, total - updates};
}

LatencyPercentiles
calculate_latency_percentiles(std::vector<std::uint64_t> samples) {
  if (samples.empty()) {
    throw std::invalid_argument("percentile needs at least one sample");
  }
  std::sort(samples.begin(), samples.end());
  const auto at = [&samples](const std::uint32_t percentile) {
    const auto rank =
        (static_cast<std::uint64_t>(percentile) * samples.size() + 99) / 100;
    return samples[rank - 1];
  };
  return {
      .p50_ns = at(50),
      .p95_ns = at(95),
      .p99_ns = at(99),
  };
}

std::vector<BenchmarkCase> benchmark_cases(const std::string_view suite) {
  std::vector<BenchmarkCase> cases;
  const auto add_direct_locking = [&] {
    constexpr MarketServerMode modes[]{MarketServerMode::single_threaded,
                                       MarketServerMode::global_read,
                                       MarketServerMode::striped_read};
    for (const auto mode : modes) {
      append_case(cases, "direct-locking", BenchmarkLevel::direct, mode, 90,
                  TrafficDistribution::uniform);
      append_case(cases, "direct-locking", BenchmarkLevel::direct, mode, 50,
                  TrafficDistribution::uniform);
      append_case(cases, "direct-locking", BenchmarkLevel::direct, mode, 50,
                  TrafficDistribution::hot);
    }
  };
  const auto add_direct_sorting = [&] {
    constexpr MarketServerMode modes[]{MarketServerMode::striped_read,
                                       MarketServerMode::striped_write};
    for (const auto mode : modes) {
      for (const auto update_percent : {90U, 50U, 10U}) {
        append_case(cases, "direct-sorting", BenchmarkLevel::direct, mode,
                    update_percent, TrafficDistribution::uniform);
      }
      append_case(cases, "direct-sorting", BenchmarkLevel::direct, mode, 50,
                  TrafficDistribution::hot);
    }
  };
  const auto add_direct_workers = [&] {
    for (const auto workers : {1U, 4U, 8U}) {
      append_case(cases, "direct-workers", BenchmarkLevel::direct,
                  MarketServerMode::striped_read, 50,
                  TrafficDistribution::uniform, workers);
    }
  };
  const auto add_tcp_modes = [&] {
    constexpr MarketServerMode modes[]{
        MarketServerMode::single_threaded, MarketServerMode::global_read,
        MarketServerMode::striped_read, MarketServerMode::striped_write};
    for (const auto mode : modes) {
      append_case(cases, "tcp-modes", BenchmarkLevel::tcp, mode, 90,
                  TrafficDistribution::uniform);
    }
  };
  const auto add_tcp_workers = [&] {
    for (const auto workers : {1U, 4U, 8U}) {
      append_case(cases, "tcp-workers", BenchmarkLevel::tcp,
                  MarketServerMode::striped_read, 50,
                  TrafficDistribution::uniform, workers);
    }
  };

  if (suite == "direct-locking") {
    add_direct_locking();
  } else if (suite == "direct-sorting") {
    add_direct_sorting();
  } else if (suite == "direct-workers") {
    add_direct_workers();
  } else if (suite == "tcp-modes") {
    add_tcp_modes();
  } else if (suite == "tcp-workers") {
    add_tcp_workers();
  } else if (suite == "all") {
    add_direct_locking();
    add_direct_sorting();
    add_direct_workers();
    add_tcp_modes();
    add_tcp_workers();
  } else if (suite == "smoke") {
    constexpr MarketServerMode modes[]{
        MarketServerMode::single_threaded, MarketServerMode::global_read,
        MarketServerMode::striped_read, MarketServerMode::striped_write};
    for (const auto mode : modes) {
      append_case(cases, "smoke-direct", BenchmarkLevel::direct, mode, 50,
                  TrafficDistribution::hot, 2, 2);
      append_case(cases, "smoke-tcp", BenchmarkLevel::tcp, mode, 50,
                  TrafficDistribution::hot, 2, 2);
    }
  } else {
    throw std::invalid_argument("unknown benchmark suite: " +
                                std::string(suite));
  }
  return cases;
}

BenchmarkResult run_benchmark(const BenchmarkCase &benchmark_case,
                              const BenchmarkSettings &settings,
                              const std::size_t repetition,
                              const bool full_verification) {
  if (settings.exchange_count == 0 || settings.instrument_count == 0 ||
      settings.stripe_count == 0 || settings.repetitions == 0) {
    throw std::invalid_argument("benchmark dimensions must be nonzero");
  }
  if (benchmark_case.level == BenchmarkLevel::tcp &&
      benchmark_case.concurrent_client_count == 0) {
    throw std::invalid_argument("TCP benchmark needs at least one client");
  }
  if (benchmark_case.mode != MarketServerMode::single_threaded &&
      benchmark_case.query_worker_count == 0) {
    throw std::invalid_argument("threaded benchmark needs query workers");
  }
  return benchmark_case.level == BenchmarkLevel::direct
             ? run_direct(benchmark_case, settings, repetition,
                          full_verification)
             : run_tcp(benchmark_case, settings, repetition,
                       full_verification);
}

void write_benchmark_csv(const std::filesystem::path &path,
                         const std::vector<BenchmarkResult> &results) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("could not open benchmark CSV: " + path.string());
  }
  output << "schema_version,level,suite,mode,repetition,seed,exchange_count,"
            "instrument_count,measurement_operations,requested_updates,"
            "requested_queries,successful_updates,successful_queries,"
            "update_percent,query_percent,distribution,writer_count,"
            "concurrent_client_count,query_worker_count,stripe_count,"
            "warmup_operations,warmup_seconds,measurement_seconds,"
            "updates_per_second,queries_per_second,query_p50_us,query_p95_us,"
            "query_p99_us,errors,rejected_requests,queue_full_responses,"
            "update_frames_rejected,logical_checksum,oracle_checksum,correct,"
            "build_type,compiler,compiler_version,system_name,system_version,"
            "architecture,cpu_model,logical_cpu_count,git_revision,git_dirty\n";
  output << std::fixed << std::setprecision(3);
  for (const auto &result : results) {
    output << result.schema_version << ',' << csv_string(result.level) << ','
           << csv_string(result.suite) << ',' << csv_string(result.mode) << ','
           << result.repetition << ',' << result.seed << ','
           << result.exchange_count << ',' << result.instrument_count << ','
           << result.measurement_operations << ',' << result.requested_updates
           << ',' << result.requested_queries << ','
           << result.successful_updates << ',' << result.successful_queries
           << ',' << result.update_percent << ',' << result.query_percent
           << ',' << csv_string(result.distribution) << ','
           << result.writer_count << ',' << result.concurrent_client_count
           << ',' << result.query_worker_count << ',' << result.stripe_count
           << ',' << result.warmup_operations << ',' << result.warmup_seconds
           << ',' << result.measurement_seconds << ','
           << result.updates_per_second << ',' << result.queries_per_second
           << ',' << result.query_p50_us << ',' << result.query_p95_us << ','
           << result.query_p99_us << ',' << result.errors << ','
           << result.rejected_requests << ',' << result.queue_full_responses
           << ',' << result.update_frames_rejected << ",0x" << std::hex
           << result.logical_checksum << ",0x" << result.oracle_checksum
           << std::dec << ',' << (result.correct ? "true" : "false") << ','
           << csv_string(result.build_type) << ',' << csv_string(result.compiler)
           << ',' << csv_string(result.compiler_version) << ','
           << csv_string(result.system_name) << ','
           << csv_string(result.system_version) << ','
           << csv_string(result.architecture) << ','
           << csv_string(result.cpu_model) << ',' << result.logical_cpu_count
           << ',' << csv_string(result.git_revision) << ','
           << (result.git_dirty ? "true" : "false") << '\n';
  }
}

bool benchmark_build_is_release() noexcept {
  return std::string_view(EXCHANGELAB_BUILD_TYPE) == "Release";
}

} // namespace exchangelab
