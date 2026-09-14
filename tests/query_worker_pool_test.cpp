#include "exchangelab/query_worker_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <stdexcept>

namespace exchangelab {
namespace {

TEST(QueryWorkerPoolTest, RejectsZeroWorkersAndCapacity) {
  EXPECT_THROW(QueryWorkerPool(0, 1), std::invalid_argument);
  EXPECT_THROW(QueryWorkerPool(1, 0), std::invalid_argument);
}

TEST(QueryWorkerPoolTest, ExecutesSubmittedTasks) {
  QueryWorkerPool pool(2, 4);
  pool.start();
  std::promise<void> completed;

  ASSERT_TRUE(pool.try_submit([&completed] { completed.set_value(); }));
  EXPECT_EQ(completed.get_future().wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  pool.stop();
}

TEST(QueryWorkerPoolTest, RejectsWhenTheWaitingQueueIsFull) {
  QueryWorkerPool pool(1, 1);
  pool.start();

  std::promise<void> first_started;
  std::promise<void> release_first;
  auto release = release_first.get_future().share();
  ASSERT_TRUE(pool.try_submit([&first_started, release] {
    first_started.set_value();
    release.wait();
  }));
  ASSERT_EQ(first_started.get_future().wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  ASSERT_TRUE(pool.try_submit([] {}));
  EXPECT_FALSE(pool.try_submit([] {}));

  release_first.set_value();
  pool.stop();
  EXPECT_FALSE(pool.try_submit([] {}));
}

TEST(QueryWorkerPoolTest, StopWakesIdleWorkers) {
  QueryWorkerPool pool(4, 8);
  pool.start();
  pool.stop();
  SUCCEED();
}

} // namespace
} // namespace exchangelab
