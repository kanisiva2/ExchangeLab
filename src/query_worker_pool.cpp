#include "exchangelab/query_worker_pool.hpp"

#include <stdexcept>
#include <utility>

namespace exchangelab {

QueryWorkerPool::QueryWorkerPool(const std::size_t worker_count,
                                 const std::size_t queue_capacity)
    : worker_count_(worker_count), queue_capacity_(queue_capacity) {
  if (worker_count == 0) {
    throw std::invalid_argument("query worker count must be greater than zero");
  }
  if (queue_capacity == 0) {
    throw std::invalid_argument("query queue capacity must be greater than zero");
  }
}

QueryWorkerPool::~QueryWorkerPool() { stop(); }

void QueryWorkerPool::start() {
  const std::lock_guard lock(mutex_);
  if (started_) {
    return;
  }
  started_ = true;
  workers_.reserve(worker_count_);
  for (std::size_t index = 0; index < worker_count_; ++index) {
    workers_.emplace_back([this] { run_worker(); });
  }
}

bool QueryWorkerPool::try_submit(Task task) {
  {
    const std::lock_guard lock(mutex_);
    if (!started_ || stopping_ || tasks_.size() >= queue_capacity_) {
      return false;
    }
    tasks_.push_back(std::move(task));
  }
  task_available_.notify_one();
  return true;
}

void QueryWorkerPool::stop() {
  {
    const std::lock_guard lock(mutex_);
    if (!started_ || stopping_) {
      return;
    }
    stopping_ = true;
    tasks_.clear();
  }
  task_available_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

std::size_t QueryWorkerPool::worker_count() const noexcept {
  return worker_count_;
}

std::size_t QueryWorkerPool::queue_capacity() const noexcept {
  return queue_capacity_;
}

void QueryWorkerPool::run_worker() {
  for (;;) {
    Task task;
    {
      std::unique_lock lock(mutex_);
      task_available_.wait(lock,
                           [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

} // namespace exchangelab
