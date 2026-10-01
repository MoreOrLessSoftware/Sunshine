/**
 * @file tests/unit/test_video_bitrate.cpp
 * @brief Test src/video_bitrate.h.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <optional>

// local includes
#include <src/video_bitrate.h>

using namespace std::chrono_literals;
using video::bitrate::frame_clock;

namespace {
  /**
   * @brief Work out the interval between frames at a rate.
   *
   * @param fps Frames per second.
   * @return Time between frames.
   */
  frame_clock::duration interval_at(double fps) {
    return std::chrono::duration_cast<frame_clock::duration>(std::chrono::duration<double> {1.0 / fps});
  }

  /**
   * @brief Record frames at an even rate.
   *
   * @param estimator Estimator to record into.
   * @param start When the first frame is encoded.
   * @param fps Frames per second.
   * @param count How many frames to record.
   * @return When the next frame would be encoded.
   */
  frame_clock::time_point add_frames(video::bitrate::frame_rate_estimator &estimator, frame_clock::time_point start, double fps, int count) {
    const auto interval = interval_at(fps);
    for (int i = 0; i < count; ++i) {
      estimator.add_frame(start + interval * i);
    }
    return start + interval * count;
  }

  /**
   * @brief Feed frames at an even rate to a booster, counting bitrate changes.
   *
   * @param booster Booster to feed.
   * @param start When the first frame is encoded.
   * @param fps Frames per second.
   * @param count How many frames to feed.
   * @param changes Incremented each time the bitrate changes.
   * @return When the next frame would be encoded.
   */
  frame_clock::time_point feed(video::bitrate::booster &booster, frame_clock::time_point start, double fps, int count, int &changes) {
    const auto interval = interval_at(fps);
    for (int i = 0; i < count; ++i) {
      if (booster.on_frame(start + interval * i)) {
        ++changes;
      }
    }
    return start + interval * count;
  }

  const frame_clock::time_point start = frame_clock::time_point {} + 1s;  ///< When the first test frame is encoded.
}  // namespace

TEST(FrameRateEstimatorTest, NeedsTwoFrames) {
  video::bitrate::frame_rate_estimator estimator;
  EXPECT_FALSE(estimator.fps().has_value());

  estimator.add_frame(start);
  EXPECT_FALSE(estimator.fps().has_value());
}

TEST(FrameRateEstimatorTest, IgnoresFramesAtTheSameTime) {
  video::bitrate::frame_rate_estimator estimator;
  estimator.add_frame(start);
  estimator.add_frame(start);

  EXPECT_FALSE(estimator.fps().has_value());
}

TEST(FrameRateEstimatorTest, MeasuresAnEvenRate) {
  video::bitrate::frame_rate_estimator estimator;
  add_frames(estimator, start, 60.0, 120);

  ASSERT_TRUE(estimator.fps().has_value());
  EXPECT_NEAR(*estimator.fps(), 60.0, 0.1);
}

TEST(FrameRateEstimatorTest, RisesAsSoonAsFramesComeFaster) {
  video::bitrate::frame_rate_estimator estimator;
  const auto next = add_frames(estimator, start, 60.0, 120);
  add_frames(estimator, next, 120.0, video::bitrate::recent_intervals + 1);

  ASSERT_TRUE(estimator.fps().has_value());
  EXPECT_NEAR(*estimator.fps(), 120.0, 1.0);
}

TEST(FrameRateEstimatorTest, FallsOnlyOverTheLongerWindow) {
  video::bitrate::frame_rate_estimator estimator;
  const auto next = add_frames(estimator, start, 120.0, 240);
  add_frames(estimator, next, 60.0, 10);

  ASSERT_TRUE(estimator.fps().has_value());
  EXPECT_GT(*estimator.fps(), 90.0);
}

TEST(FrameRateEstimatorTest, MeasuresAcrossAPause) {
  video::bitrate::frame_rate_estimator estimator;
  estimator.add_frame(start);
  estimator.add_frame(start + 5s);

  ASSERT_TRUE(estimator.fps().has_value());
  EXPECT_NEAR(*estimator.fps(), 0.2, 0.001);
}

TEST(FrameRateEstimatorTest, CountsFramesTheEncoderSkipped) {
  video::bitrate::frame_rate_estimator estimator;
  const auto interval = interval_at(60.0);

  // The game renders at 120 fps, but the encoder only gets to every second frame
  for (int i = 0; i < 120; ++i) {
    estimator.add_frame(start + interval * i, 2);
  }

  ASSERT_TRUE(estimator.fps().has_value());
  EXPECT_NEAR(*estimator.fps(), 120.0, 0.1);
}

TEST(FrameRateEstimatorTest, CountsAZeroFrameCountAsOne) {
  video::bitrate::frame_rate_estimator estimator;
  add_frames(estimator, start, 60.0, 1);
  estimator.add_frame(start + interval_at(60.0), 0);

  ASSERT_TRUE(estimator.fps().has_value());
  EXPECT_NEAR(*estimator.fps(), 60.0, 0.1);
}

TEST(FrameRateEstimatorTest, IgnoresASinglePause) {
  video::bitrate::frame_rate_estimator estimator;
  const auto next = add_frames(estimator, start, 120.0, 240);

  // The game hitches for 100 ms, then carries on at full rate
  estimator.add_frame(next + 100ms);

  ASSERT_TRUE(estimator.fps().has_value());
  EXPECT_NEAR(*estimator.fps(), 120.0, 1.0);
}

TEST(FrameRateEstimatorTest, FollowsASustainedSlowdownByTheMedian) {
  video::bitrate::frame_rate_estimator estimator;
  const auto next = add_frames(estimator, start, 120.0, 240);
  add_frames(estimator, next, 60.0, 120);

  // Once the last second is all at the slower rate, every measurement agrees
  ASSERT_TRUE(estimator.fps().has_value());
  EXPECT_NEAR(*estimator.fps(), 60.0, 1.0);
}

TEST(MaxFrameBytesTest, FitsOneFrameIntervalAtTheSendRate) {
  // 800 Mbps for 1/120 of a second
  EXPECT_NEAR(video::bitrate::max_frame_bytes(120.0, 800), 833333.3, 0.1);
}

TEST(MaxFrameBytesTest, StaysWithinTheStreamsPackets) {
  EXPECT_DOUBLE_EQ(video::bitrate::max_frame_bytes(120.0, 0), 2'000'000.0);
  EXPECT_DOUBLE_EQ(video::bitrate::max_frame_bytes(60.0, 10000), 2'000'000.0);
  EXPECT_DOUBLE_EQ(video::bitrate::max_frame_bytes(0.0, 800), 2'000'000.0);
}

TEST(BoostFactorTest, KeepsFramesSendableWithinOneInterval) {
  // 740 Mbps at 120 fps is 770833 bytes a frame; at 800 Mbps, 833333 bytes can be sent in time
  EXPECT_NEAR(video::bitrate::boost_factor(120.0, 60.0, 4.0, 740000, 800), 833333.3 / 770833.3, 0.0001);
}

TEST(BoostFactorTest, NeverBoostsFramesThatAlreadyTakeAnInterval) {
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(120.0, 60.0, 4.0, 900000, 800), 1.0);
}

TEST(BoostFactorTest, MakesUpForMissingFrames) {
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(120.0, 60.0, 4.0, 100000), 2.0);
}

TEST(BoostFactorTest, NeverLowersTheBitrate) {
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(60.0, 120.0, 4.0, 100000), 1.0);
}

TEST(BoostFactorTest, StopsAtTheConfiguredMaximum) {
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(120.0, 10.0, 4.0, 100000), 4.0);
}

TEST(BoostFactorTest, KeepsFramesToASizeTheStreamCanSend) {
  // 500 Mbps at 120 fps is 520833 bytes a frame, so a frame can grow 3.84 times
  EXPECT_NEAR(video::bitrate::boost_factor(120.0, 20.0, 8.0, 500000), 3.84, 0.001);
}

TEST(BoostFactorTest, NeverBoostsFramesAlreadyTooLarge) {
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(10.0, 5.0, 4.0, 500000), 1.0);
}

TEST(BoostFactorTest, OffWithoutUsableInputs) {
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(120.0, 60.0, 1.0, 100000), 1.0);
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(0.0, 60.0, 4.0, 100000), 1.0);
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(120.0, 0.0, 4.0, 100000), 1.0);
  EXPECT_DOUBLE_EQ(video::bitrate::boost_factor(120.0, 60.0, 4.0, 0), 1.0);
}

TEST(MaxBoostFactorTest, IsTheConfiguredMaximum) {
  EXPECT_DOUBLE_EQ(video::bitrate::max_boost_factor(100.0, 4.0, 100000), 4.0);
}

TEST(MaxBoostFactorTest, StopsAtTheLargestFrame) {
  // 400 Mbps at 100 fps is 500000 bytes a frame, so a frame can grow 4 times
  EXPECT_NEAR(video::bitrate::max_boost_factor(100.0, 8.0, 400000), 4.0, 0.001);
}

TEST(MaxBoostFactorTest, IsOneWhenBoostingIsOff) {
  EXPECT_DOUBLE_EQ(video::bitrate::max_boost_factor(100.0, 1.0, 100000), 1.0);
  EXPECT_DOUBLE_EQ(video::bitrate::max_boost_factor(100.0, 0.5, 100000), 1.0);
}

TEST(BoosterTest, DoublesTheBitrateForHalfTheFrames) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  int changes = 0;
  feed(booster, start, 60.0, 120, changes);

  EXPECT_NEAR(booster.bitrate_kbps(), 200000, 1);
  EXPECT_EQ(changes, 1);
}

TEST(BoosterTest, KeepsTheClientBitrateAtTheStreamRate) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  int changes = 0;
  feed(booster, start, 120.0, 240, changes);

  EXPECT_EQ(booster.bitrate_kbps(), 100000);
  EXPECT_EQ(changes, 0);
}

TEST(BoosterTest, IgnoresSmallRises) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  int changes = 0;
  const auto next = feed(booster, start, 63.0, 126, changes);
  const auto boosted = booster.bitrate_kbps();
  changes = 0;

  // 60 fps would allow 5% more, which isn't worth changing the encoder for
  feed(booster, next, 60.0, 120, changes);

  EXPECT_EQ(booster.bitrate_kbps(), boosted);
  EXPECT_EQ(changes, 0);
}

TEST(BoosterTest, LowersRightAwayWhenFramesComeSlightlyFaster) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  int changes = 0;
  const auto next = feed(booster, start, 60.0, 120, changes);
  feed(booster, next, 63.0, 126, changes);

  // At 63 fps the bitrate sent is no more than the client's
  EXPECT_LT(booster.bitrate_kbps(), 200000);
  EXPECT_LE(booster.bitrate_kbps() * 63.0 / 120.0, 100000.0);
}

TEST(BoosterTest, NeverSendsMoreThanTheClientBitrateOnceSettled) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  int changes = 0;
  auto next = start;
  for (const double fps : {60.0, 64.0, 70.0, 66.0, 90.0, 85.0, 119.0, 45.0, 50.0}) {
    // A second at each rate, long enough for the slower measurement to settle
    next = feed(booster, next, fps, static_cast<int>(fps), changes);
    EXPECT_LE(booster.bitrate_kbps() * fps / 120.0, 100000.0) << "at " << fps << " fps";
  }
}

TEST(BoosterTest, ReturnsToTheClientBitrateWhenFramesComeFasterAgain) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  int changes = 0;
  const auto next = feed(booster, start, 60.0, 120, changes);

  // Back to the client's bitrate once the last few intervals are all at the stream's rate
  feed(booster, next, 120.0, video::bitrate::recent_intervals + 1, changes);

  EXPECT_EQ(booster.bitrate_kbps(), 100000);
}

TEST(BoosterTest, DoesNotBoostWhenTheEncoderFallsBehind) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  const auto interval = interval_at(60.0);

  // The game keeps up with the stream; the encoder only gets to every second frame
  int changes = 0;
  for (int i = 0; i < 240; ++i) {
    if (booster.on_frame(start + interval * i, 2)) {
      ++changes;
    }
  }

  EXPECT_EQ(booster.bitrate_kbps(), 100000);
  EXPECT_EQ(changes, 0);
  EXPECT_NEAR(booster.fps(), 120.0, 0.1);
}

TEST(BoosterTest, ReportsTheClientBitrate) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  int changes = 0;
  feed(booster, start, 60.0, 120, changes);

  EXPECT_EQ(booster.client_bitrate_kbps(), 100000);
  EXPECT_NEAR(booster.fps(), 60.0, 0.1);
}

TEST(BoosterTest, DoesNotBoostTheFrameAfterAHitch) {
  video::bitrate::booster booster {100000, 120.0, 4.0};
  int changes = 0;
  const auto next = feed(booster, start, 120.0, 240, changes);

  EXPECT_FALSE(booster.on_frame(next + 100ms).has_value());
  EXPECT_EQ(booster.bitrate_kbps(), 100000);
  EXPECT_EQ(changes, 0);
}

TEST(BoosterTest, KeepsBoostedFramesSendableWithinOneInterval) {
  video::bitrate::booster booster {740000, 120.0, 4.0, 800};
  int changes = 0;
  feed(booster, start, 60.0, 120, changes);

  // 800 Mbps sends 833333 bytes in 1/120 of a second, which is 800 Mbps of frames at 120 fps
  EXPECT_LE(booster.bitrate_kbps(), 800000);
  EXPECT_GT(booster.bitrate_kbps(), 740000);
}

TEST(BoosterTest, WaitsForAFrameRate) {
  video::bitrate::booster booster {100000, 120.0, 4.0};

  EXPECT_FALSE(booster.on_frame(start).has_value());
  EXPECT_EQ(booster.bitrate_kbps(), 100000);
}
