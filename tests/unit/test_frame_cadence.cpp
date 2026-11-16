/**
 * @file tests/unit/test_frame_cadence.cpp
 * @brief Test src/frame_cadence.h.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <vector>

// local includes
#include <src/frame_cadence.h>

namespace {
  using namespace std::chrono_literals;
  using clock = platf::frame_cadence_smoother_t::clock;
  using us = std::chrono::microseconds;

  /**
   * @brief What the smoother did with a run of frames.
   */
  struct result_t {
    std::vector<clock::duration> shifts;  ///< How far before its present each frame was stamped.
    std::vector<clock::duration> intervals;  ///< Intervals between the frames' timestamps.
    std::vector<std::size_t> patterns;  ///< Pattern each frame was stamped by.
  };

  /**
   * @brief Stamp frames presented at the given intervals.
   *
   * @param smoother Smoother to stamp them with.
   * @param present When the frame before the first was presented. Left at the last frame's present.
   * @param pattern Intervals between presents, repeated in order.
   * @param frames How many frames to stamp.
   * @return What the smoother did with them.
   */
  result_t stamp_frames(platf::frame_cadence_smoother_t &smoother, clock::time_point &present, const std::vector<clock::duration> &pattern, std::size_t frames) {
    result_t result;
    std::optional<clock::time_point> previous;

    for (std::size_t i = 0; i < frames; ++i) {
      present += pattern[i % pattern.size()];
      const auto stamp = smoother.stamp(present);
      result.shifts.push_back(present - stamp);
      if (previous) {
        result.intervals.push_back(stamp - *previous);
      }
      result.patterns.push_back(smoother.pattern());
      previous = stamp;
    }

    return result;
  }

  /**
   * @brief Check that every frame was stamped as presented.
   *
   * @param result What the smoother did.
   */
  void expect_untouched(const result_t &result) {
    for (std::size_t i = 0; i < result.shifts.size(); ++i) {
      EXPECT_EQ(result.shifts[i], 0ns) << "frame " << i;
      EXPECT_EQ(result.patterns[i], 0u) << "frame " << i;
    }
  }

  const clock::time_point start = clock::time_point {} + 10s;  ///< When the frame before the first was presented.
}  // namespace

TEST(FrameCadenceSmootherTest, LeavesAnEvenCadenceAlone) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;
  expect_untouched(stamp_frames(smoother, present, {8ms}, 100));
}

TEST(FrameCadenceSmootherTest, EvensOutFramesPresentedInUnevenPairs) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;

  // The first frame has no interval, so 16 intervals are known with the 17th frame
  expect_untouched(stamp_frames(smoother, present, {7ms, 9ms}, 16));

  // The 17th frame switches the pattern on, so the intervals are even from the one after it
  const auto result = stamp_frames(smoother, present, {7ms, 9ms}, 81);
  for (std::size_t i = 0; i < result.shifts.size(); ++i) {
    EXPECT_EQ(result.patterns[i], 2u) << "frame " << i;
    // The frame after the short interval is on time, the one after the long interval 1 ms late
    EXPECT_EQ(result.shifts[i], i % 2 == 0 ? 0ms : 1ms) << "frame " << i;
  }
  for (std::size_t i = 1; i < result.intervals.size(); ++i) {
    EXPECT_EQ(result.intervals[i], 8ms) << "interval " << i;
  }
}

TEST(FrameCadenceSmootherTest, EvensOutPatternsOfThreeAndFourFrames) {
  {
    platf::frame_cadence_smoother_t smoother;
    auto present = start;
    stamp_frames(smoother, present, {4500us, 5ms, 5500us}, 25);
    const auto result = stamp_frames(smoother, present, {5ms, 5500us, 4500us}, 30);
    for (std::size_t i = 0; i < result.intervals.size(); ++i) {
      EXPECT_EQ(result.patterns[i + 1], 3u) << "frame " << i + 1;
      EXPECT_EQ(result.intervals[i], 5ms) << "interval " << i;
    }
  }
  {
    platf::frame_cadence_smoother_t smoother;
    auto present = start;
    stamp_frames(smoother, present, {4ms, 4ms, 4ms, 5ms}, 33);
    const auto result = stamp_frames(smoother, present, {4ms, 4ms, 5ms, 4ms}, 40);
    for (std::size_t i = 0; i < result.intervals.size(); ++i) {
      EXPECT_EQ(result.patterns[i + 1], 4u) << "frame " << i + 1;
      EXPECT_EQ(result.intervals[i], 4250us) << "interval " << i;
    }
  }
}

TEST(FrameCadenceSmootherTest, NeverStampsAFrameAfterItsPresentOrMoreThanThreeTenthsOfAnIntervalBefore) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;
  const auto result = stamp_frames(smoother, present, {4500us, 5ms, 5500us}, 200);
  for (std::size_t i = 0; i < result.shifts.size(); ++i) {
    EXPECT_GE(result.shifts[i], 0ns) << "frame " << i;
    EXPECT_LE(result.shifts[i], 1500us) << "frame " << i;
  }
}

TEST(FrameCadenceSmootherTest, LeavesACadenceTooLopsidedToBePacingErrorAlone) {
  // 90 FPS on a 120 Hz display: every third frame stays up for 2 refreshes
  platf::frame_cadence_smoother_t smoother;
  auto present = start;
  expect_untouched(stamp_frames(smoother, present, {8333us, 8333us, 16667us}, 100));

  // 40 FPS on a 60 Hz display: frames stay up for 1 then 2 refreshes
  platf::frame_cadence_smoother_t another;
  expect_untouched(stamp_frames(another, present, {16667us, 33333us}, 100));
}

TEST(FrameCadenceSmootherTest, LeavesANearlyEvenCadenceAlone) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;
  expect_untouched(stamp_frames(smoother, present, {7900us, 8100us}, 100));
}

TEST(FrameCadenceSmootherTest, LeavesAnIrregularCadenceAlone) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;

  // Intervals of 6 to 10 ms in no repeating order
  std::vector<clock::duration> intervals;
  std::uint32_t state = 12345;
  for (int i = 0; i < 400; ++i) {
    state = state * 1664525u + 1013904223u;
    intervals.push_back(us {6000 + (state >> 8) % 4001});
  }
  expect_untouched(stamp_frames(smoother, present, intervals, intervals.size()));
}

TEST(FrameCadenceSmootherTest, GoesBackToPresentTimesWhenThePatternStops) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;
  EXPECT_EQ(stamp_frames(smoother, present, {7ms, 9ms}, 40).patterns.back(), 2u);

  const auto result = stamp_frames(smoother, present, {8ms}, 40);
  EXPECT_EQ(result.patterns.back(), 0u);
  EXPECT_EQ(result.shifts.back(), 0ns);
  // The timestamps never go back on the way
  for (std::size_t i = 0; i < result.intervals.size(); ++i) {
    EXPECT_GT(result.intervals[i], 0ns) << "interval " << i;
  }
}

TEST(FrameCadenceSmootherTest, StartsOverAfterAMissedFrame) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;
  EXPECT_EQ(stamp_frames(smoother, present, {7ms, 9ms}, 40).patterns.back(), 2u);

  // Two presents since the last frame: one was never captured
  present += 16ms;
  EXPECT_EQ(smoother.stamp(present, 2), present);
  EXPECT_EQ(smoother.pattern(), 0u);

  // The pattern has to be seen 8 times over again
  const auto result = stamp_frames(smoother, present, {7ms, 9ms}, 16);
  for (std::size_t i = 0; i < 15; ++i) {
    EXPECT_EQ(result.patterns[i], 0u) << "frame " << i;
    EXPECT_EQ(result.shifts[i], 0ns) << "frame " << i;
  }
  EXPECT_EQ(result.patterns[15], 2u);
}

TEST(FrameCadenceSmootherTest, StartsOverAfterAPause) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;
  EXPECT_EQ(stamp_frames(smoother, present, {7ms, 9ms}, 40).patterns.back(), 2u);

  present += 251ms;
  EXPECT_EQ(smoother.stamp(present), present);
  EXPECT_EQ(smoother.pattern(), 0u);
  expect_untouched(stamp_frames(smoother, present, {7ms, 9ms}, 15));
}

TEST(FrameCadenceSmootherTest, NeverStampsAFrameBeforeTheLastOne) {
  platf::frame_cadence_smoother_t smoother;
  auto present = start;
  stamp_frames(smoother, present, {7ms, 9ms}, 40);

  // A present time that goes back, as a cursor move's can, keeps the last timestamp
  const auto last = smoother.stamp(present + 8ms);
  EXPECT_EQ(smoother.stamp(present + 2ms), last);
  EXPECT_EQ(smoother.pattern(), 0u);
}
