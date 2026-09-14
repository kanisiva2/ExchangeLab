#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace exchangelab {

class QueryWorkerPool {
public:
  using Task = std::function<void()>;

  QueryWorkerPool(std::size_t worker_count, std::size_t queue_capacity);
  ~QueryWorkerPool();

  QueryWorkerPool(const QueryWorkerPool &) = delete;
  QueryWorkerPool &operator=(const QueryWorkerPool &) = delete;

  void start();
  [[nodiscard]] bool try_submit(Task task);
  void stop();

  [[nodiscard]] std::size_t worker_count() const noexcept;
  [[nodiscard]] std::size_t queue_capacity() const noexcept;

private:
  void run_worker();

  std::size_t worker_count_;
  std::size_t queue_capacity_;
  mutable std::mutex mutex_;
  std::condition_variable task_available_;
  std::deque<Task> tasks_;
  std::vector<std::thread> workers_;
  bool started_{};
  bool stopping_{};
};

} // namespace exchangelab
