/**
 * @file tests/unit/test_thread_safe.cpp
 * @brief Tests for thread-safe utility types.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <timeapi.h>
#endif

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/thread_safe.h"

static_assert(!std::is_convertible_v<std::uint32_t, safe::queue_t<int>>);

TEST(ThreadSafeQueue, RejectsNewestItemAtCapacity) {
  safe::queue_t<int> queue {1, safe::queue_t<int>::overflow_policy_e::reject};

  ASSERT_TRUE(queue.raise(1));
  EXPECT_FALSE(queue.raise(2));

  const auto value = queue.pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 1);
}

TEST(ThreadSafeQueue, DropsQueuedItemsByDefaultAtCapacity) {
  safe::queue_t<int> queue {2};

  ASSERT_TRUE(queue.raise(1));
  ASSERT_TRUE(queue.raise(2));
  ASSERT_TRUE(queue.raise(3));

  const auto value = queue.pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 3);
}

TEST(ThreadSafeQueue, RejectsItemsAfterStop) {
  safe::queue_t<int> queue;

  queue.stop();

  EXPECT_FALSE(queue.raise(1));
}

using namespace std::literals;

namespace {
  /**
   * @brief Raise the system timer resolution to 1 ms while it exists, as Sunshine does while streaming.
   *
   * Timed waits end on the next timer tick, so with the default 15.6 ms resolution they can end
   * that late whatever the condition variable.
   */
  struct timer_resolution_t {
#ifdef _WIN32
    timer_resolution_t() {
      timeBeginPeriod(1);
    }

    ~timer_resolution_t() {
      timeEndPeriod(1);
    }
#endif
  };
}  // namespace

TEST(ThreadSafeQueue, TimedPopEndsCloseToItsTimeout) {
  timer_resolution_t resolution;
  safe::queue_t<int> queue;

  // MinGW's std::condition_variable ended these waits about 15 ms late. The median of a few
  // waits keeps a single scheduling hiccup from failing the test.
  std::vector<double> late_ms;
  for (int i = 0; i < 5; ++i) {
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(queue.pop(30ms));
    late_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start - 30ms).count());
  }
  std::sort(late_ms.begin(), late_ms.end());

  EXPECT_GE(late_ms.front(), 0.0);
  EXPECT_LT(late_ms[late_ms.size() / 2], 5.0);
}

TEST(ThreadSafeQueue, TimedPopWakesForANewItem) {
  safe::queue_t<int> queue;

  std::thread producer([&queue]() {
    std::this_thread::sleep_for(20ms);
    queue.raise(7);
  });
  const auto start = std::chrono::steady_clock::now();
  const auto value = queue.pop(2s);
  const auto waited = std::chrono::steady_clock::now() - start;
  producer.join();

  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 7);
  EXPECT_LT(waited, 1s);
}

TEST(ThreadSafeEvent, TimedPopEndsCloseToItsTimeout) {
  timer_resolution_t resolution;
  safe::event_t<int> event;

  // The median of a few waits, as for the queue
  std::vector<double> late_ms;
  for (int i = 0; i < 5; ++i) {
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(event.pop(30ms));
    late_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start - 30ms).count());
  }
  std::sort(late_ms.begin(), late_ms.end());

  EXPECT_GE(late_ms.front(), 0.0);
  EXPECT_LT(late_ms[late_ms.size() / 2], 5.0);
}

TEST(ThreadSafeEvent, TimedPopReturnsARaisedValue) {
  safe::event_t<int> event;

  std::thread producer([&event]() {
    std::this_thread::sleep_for(20ms);
    event.raise(3);
  });
  const auto value = event.pop(2s);
  producer.join();

  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 3);
}
