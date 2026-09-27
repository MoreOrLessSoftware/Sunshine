/**
 * @file tests/unit/test_frame_rate_limiter.cpp
 * @brief Test src/frame_rate_limiter.h.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <vector>

// local includes
#include <src/frame_rate_limiter.h>

namespace {
  using namespace std::chrono_literals;
  using clock = platf::frame_rate_limiter_t::clock;

  constexpr clock::duration frame_interval = 8ms;  ///< 125 FPS, a whole number of nanoseconds per half interval.

  /**
   * @brief Take frames arriving at the given times, each once it is due.
   *
   * @param limiter Limiter to take them through.
   * @param arrivals When each frame arrives, relative to the start.
   * @return How long each frame was held after it arrived.
   */
  std::vector<clock::duration> take_frames(platf::frame_rate_limiter_t &limiter, const std::vector<clock::duration> &arrivals) {
    const clock::time_point start = clock::now();
    std::vector<clock::duration> holds;

    for (const auto arrival : arrivals) {
      const auto taken = std::max(start + arrival, limiter.next_due());
      holds.push_back(taken - (start + arrival));
      limiter.frame_taken(taken);
    }

    return holds;
  }
}  // namespace

TEST(FrameRateLimiterTest, TakesTheFirstFrameImmediately) {
  platf::frame_rate_limiter_t limiter {frame_interval, true};

  EXPECT_LE(limiter.next_due(), clock::now());
}

TEST(FrameRateLimiterTest, NeverHoldsFramesAtTheFrameRate) {
  platf::frame_rate_limiter_t limiter {frame_interval, true};

  std::vector<clock::duration> arrivals;
  for (int i = 0; i < 10000; i++) {
    arrivals.push_back(frame_interval * i);
  }

  for (const auto hold : take_frames(limiter, arrivals)) {
    EXPECT_EQ(hold, 0ns);
  }
}

TEST(FrameRateLimiterTest, NeverHoldsFramesFromASlightlyFastClock) {
  platf::frame_rate_limiter_t limiter {frame_interval, true};

  // A display clock 0.5% faster than the frame rate, for over a minute
  std::vector<clock::duration> arrivals;
  for (int i = 0; i < 10000; i++) {
    arrivals.push_back(frame_interval * i * 200 / 201);
  }

  for (const auto hold : take_frames(limiter, arrivals)) {
    EXPECT_EQ(hold, 0ns);
  }
}

TEST(FrameRateLimiterTest, NeverHoldsJitteredFramesAtTheFrameRate) {
  platf::frame_rate_limiter_t limiter {frame_interval, true};

  // Alternately a third of an interval early and on time
  std::vector<clock::duration> arrivals;
  for (int i = 0; i < 1000; i++) {
    arrivals.push_back(frame_interval * i - (i % 2 ? frame_interval / 3 : 0ns));
  }

  for (const auto hold : take_frames(limiter, arrivals)) {
    EXPECT_EQ(hold, 0ns);
  }
}

TEST(FrameRateLimiterTest, NeverHoldsFramesBelowTheFrameRate) {
  platf::frame_rate_limiter_t limiter {frame_interval, true};

  std::vector<clock::duration> arrivals;
  for (int i = 0; i < 1000; i++) {
    arrivals.push_back(frame_interval * i * 3 / 2);
  }

  for (const auto hold : take_frames(limiter, arrivals)) {
    EXPECT_EQ(hold, 0ns);
  }
}

TEST(FrameRateLimiterTest, ThinsOutATwiceAsFastSourceEvenly) {
  platf::frame_rate_limiter_t limiter {frame_interval, false};
  const clock::time_point start = clock::now();

  // A source at twice the frame rate for 10 seconds, where a frame arriving while another
  // is held replaces it
  std::vector<clock::time_point> taken;
  for (int i = 0; i < 2500; i++) {
    const auto arrival = start + frame_interval * i / 2;
    if (arrival < limiter.next_due()) {
      continue;
    }

    limiter.frame_taken(arrival);
    taken.push_back(arrival);
  }

  // Only the first two come closer than a frame interval apart
  ASSERT_GT(taken.size(), 2u);
  for (size_t i = 2; i < taken.size(); i++) {
    EXPECT_EQ(taken[i] - taken[i - 1], frame_interval) << "frame " << i;
  }
  // The first frame, then one every interval from half an interval in: at 4, 12, ... 9996 ms
  EXPECT_EQ(taken.size(), 1251u);
}

TEST(FrameRateLimiterTest, LetsATwiceAsFastSourceThroughMoreOftenWithTheFastClockAllowance) {
  platf::frame_rate_limiter_t limiter {frame_interval, true};
  const clock::time_point start = clock::now();

  int taken = 0;
  for (int i = 0; i < 2500; i++) {
    const auto arrival = start + frame_interval * i / 2;
    if (arrival < limiter.next_due()) {
      continue;
    }

    limiter.frame_taken(arrival);
    taken++;
  }

  // About 1% over the frame rate
  EXPECT_GT(taken, 1250);
  EXPECT_LE(taken, 1250 * 101 / 100 + 2);
}

TEST(FrameRateLimiterTest, HoldsTheSecondFrameAfterIdling) {
  platf::frame_rate_limiter_t limiter {frame_interval, false};
  const auto start = clock::now();

  limiter.frame_taken(start);

  // After a long pause, the next frame may come half an interval after the one that
  // ended it at the earliest
  const auto resumed = start + 1s;
  limiter.frame_taken(resumed);
  EXPECT_EQ(limiter.next_due(), resumed + frame_interval / 2);
}

TEST(CursorUpdateTest, HoldsBackCursorMovesWhileFramesAreComing) {
  const auto now = clock::now();

  // 100 FPS stream: frames within 20 ms count as coming
  EXPECT_TRUE(platf::hold_back_cursor_update(now - 5ms, now, 100));
  EXPECT_TRUE(platf::hold_back_cursor_update(now - 14ms, now, 100));
}

TEST(CursorUpdateTest, SendsCursorMovesOverAStillScreen) {
  const auto now = clock::now();

  EXPECT_FALSE(platf::hold_back_cursor_update(now - 25ms, now, 100));
  EXPECT_FALSE(platf::hold_back_cursor_update(std::nullopt, now, 100));
}

TEST(CursorUpdateTest, SendsCursorMovesWithoutAFrameRate) {
  const auto now = clock::now();

  EXPECT_FALSE(platf::hold_back_cursor_update(now - 1ms, now, 0));
}

TEST(CursorUpdateTest, WaitsUntilTwoFrameIntervalsAfterTheLastFrame) {
  const auto presented = clock::now();

  EXPECT_EQ(platf::cursor_hold_deadline(presented, 100), presented + 20ms);
  EXPECT_EQ(platf::cursor_hold_deadline(presented, 60), presented + std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(2s) / 60));
}

TEST(CursorUpdateTest, DoesNotWaitWithoutAFrameOrFrameRate) {
  EXPECT_FALSE(platf::cursor_hold_deadline(std::nullopt, 100).has_value());
  EXPECT_FALSE(platf::cursor_hold_deadline(clock::now(), 0).has_value());
}
