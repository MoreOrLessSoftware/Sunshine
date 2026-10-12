/**
 * @file tests/unit/platform/windows/test_amf_av1.cpp
 * @brief Test the helpers in src/amf/amf_av1.cpp that don't need an AMD GPU.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <string>
  #include <vector>

  // local includes
  #include <src/amf/amf_av1.h>
  #include <src/config.h>
  #include <src/video.h>

using namespace std::literals;

TEST(AmfColorTest, SignalsRec601) {
  const auto color = amf_direct::color_from_colorspace({video::colorspace_e::rec601, false, 8});
  EXPECT_EQ(color.profile, 0);
  EXPECT_EQ(color.transfer, 6);
  EXPECT_EQ(color.primaries, 6);
}

TEST(AmfColorTest, SignalsFullRangeRec601) {
  EXPECT_EQ(amf_direct::color_from_colorspace({video::colorspace_e::rec601, true, 8}).profile, 3);
}

TEST(AmfColorTest, SignalsRec709) {
  const auto limited = amf_direct::color_from_colorspace({video::colorspace_e::rec709, false, 8});
  EXPECT_EQ(limited.profile, 1);
  EXPECT_EQ(limited.transfer, 1);
  EXPECT_EQ(limited.primaries, 1);
  EXPECT_EQ(amf_direct::color_from_colorspace({video::colorspace_e::rec709, true, 8}).profile, 7);
}

TEST(AmfColorTest, SignalsRec2020Sdr) {
  const auto limited = amf_direct::color_from_colorspace({video::colorspace_e::bt2020sdr, false, 10});
  EXPECT_EQ(limited.profile, 2);
  EXPECT_EQ(limited.transfer, 14);
  EXPECT_EQ(limited.primaries, 9);
  EXPECT_EQ(amf_direct::color_from_colorspace({video::colorspace_e::bt2020sdr, true, 10}).profile, 8);
}

TEST(AmfColorTest, SignalsHdr) {
  const auto color = amf_direct::color_from_colorspace({video::colorspace_e::bt2020, false, 10});
  EXPECT_EQ(color.profile, 2);
  EXPECT_EQ(color.transfer, 16);
  EXPECT_EQ(color.primaries, 9);
}

TEST(AmfVbvTest, HoldsOneFrame) {
  EXPECT_EQ(amf_direct::vbv_buffer_bits(60'000'000, 60), 1'000'000);
}

TEST(AmfVbvTest, ToleratesAZeroFrameRate) {
  EXPECT_EQ(amf_direct::vbv_buffer_bits(60'000'000, 0), 60'000'000);
}

TEST(AmfFrameDurationTest, IsInHundredsOfNanoseconds) {
  EXPECT_EQ(amf_direct::frame_duration(60, 1), 166'666);
  EXPECT_EQ(amf_direct::frame_duration(60000, 1001), 166'833);
}

TEST(AmfFrameDurationTest, ToleratesAZeroFrameRate) {
  EXPECT_EQ(amf_direct::frame_duration(0, 0), 10'000'000);
}

TEST(AmfDescribeObusTest, DescribesAKeyFrame) {
  // Temporal delimiter, sequence header of 2 bytes, frame header of 1 byte, tile group of 3 bytes
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x0A, 0x02, 0xAA, 0xBB, 0x1A, 0x01, 0xCC, 0x22, 0x03, 1, 2, 3};
  EXPECT_EQ(amf_direct::describe_obus(data.data(), data.size()), "TD:0 SEQ:2 FH:1 TG:3");
}

TEST(AmfDescribeObusTest, ReadsMultiByteSizes) {
  // A padding OBU of 200 bytes, its size in two leb128 bytes
  std::vector<std::uint8_t> data {0x7A, 0xC8, 0x01};
  data.resize(data.size() + 200);
  EXPECT_EQ(amf_direct::describe_obus(data.data(), data.size()), "PAD:200");
}

TEST(AmfDescribeObusTest, SkipsTheExtensionHeader) {
  const std::vector<std::uint8_t> data {0x36, 0x00, 0x01, 0xFF};
  EXPECT_EQ(amf_direct::describe_obus(data.data(), data.size()), "FRAME:1");
}

TEST(AmfDescribeObusTest, TakesTheRestWithoutASizeField) {
  const std::vector<std::uint8_t> data {0x20, 1, 2, 3};
  EXPECT_EQ(amf_direct::describe_obus(data.data(), data.size()), "TG(no size):3");
}

TEST(AmfDescribeObusTest, StopsAtTheForbiddenBit) {
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x80, 0x00};
  EXPECT_EQ(amf_direct::describe_obus(data.data(), data.size()), "TD:0 invalid at 2");
}

TEST(AmfDescribeObusTest, StopsAtAnOverlongSize) {
  const std::vector<std::uint8_t> data {0x22, 0x05, 1, 2};
  EXPECT_EQ(amf_direct::describe_obus(data.data(), data.size()), "invalid at 0");
}

TEST(AmfDescribeObusTest, StopsAtATruncatedSize) {
  const std::vector<std::uint8_t> data {0x22, 0x80};
  EXPECT_EQ(amf_direct::describe_obus(data.data(), data.size()), "invalid at 0");
}

TEST(AmfObuBytesTest, KeepsTheWholeFrame) {
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x0A, 0x02, 0xAA, 0xBB, 0x22, 0x01, 0xCC};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size()), data.size());
}

TEST(AmfObuBytesTest, DropsTheZerosAfterATile) {
  // A tile group of 2 bytes, as AMF hands over a tile, then zero bytes up to the buffer's size
  const std::vector<std::uint8_t> data {0x22, 0x02, 1, 2, 0, 0, 0, 0};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size()), 4u);
}

TEST(AmfObuBytesTest, ReadsTheExtensionAndMultiByteSizes) {
  std::vector<std::uint8_t> data {0x26, 0x00, 0xC8, 0x01};
  data.resize(data.size() + 200, 1);
  data.push_back(0);
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size()), 204u);
}

TEST(AmfObuBytesTest, StopsAtAnObuWithoutASizeField) {
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x20, 1, 2};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size()), 2u);
}

TEST(AmfObuBytesTest, StopsAtTheForbiddenBit) {
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x92, 0x00};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size()), 2u);
}

TEST(AmfObuBytesTest, StopsAtAnObuRunningPastTheEnd) {
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x22, 0x05, 1, 2};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size()), 2u);
}

TEST(AmfObuBytesTest, StopsAtATruncatedSize) {
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x22, 0x80};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size()), 2u);
}

TEST(AmfObuBytesTest, StopsAfterTheTileGroup) {
  // A tile, then leftover bytes that happen to look like another tile group
  const std::vector<std::uint8_t> data {0x22, 0x02, 1, 2, 0x22, 0x01, 9};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size()), data.size());
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size(), true), 4u);
}

TEST(AmfObuBytesTest, KeepsTheHeadersBeforeTheFirstTile) {
  // Temporal delimiter, sequence header and frame header come with the first tile
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x0A, 0x01, 0xAA, 0x1A, 0x01, 0xBB, 0x22, 0x01, 1, 0x22, 0x01, 2};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size(), true), 11u);
}

TEST(AmfObuBytesTest, StopsAfterAFrameObu) {
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x32, 0x01, 1, 0x22, 0x01, 2};
  EXPECT_EQ(amf_direct::obu_bytes(data.data(), data.size(), true), 5u);
}

TEST(AmfObuBytesTest, IsZeroForAnEmptyBuffer) {
  EXPECT_EQ(amf_direct::obu_bytes(nullptr, 0), 0u);
}

TEST(AmfTileTimingTest, DescribesNoFrames) {
  const amf_direct::tile_timing_t timing;
  EXPECT_EQ(timing.frames(), 0u);
  EXPECT_EQ(timing.summary(), "no frames");
}

TEST(AmfTileTimingTest, IgnoresAFrameWithNoTiles) {
  amf_direct::tile_timing_t timing;
  timing.add({});
  EXPECT_EQ(timing.frames(), 0u);
}

TEST(AmfTileTimingTest, AveragesWhenTilesComeOut) {
  amf_direct::tile_timing_t timing;
  // Tiles coming out as they finish: three quarters of the frame is out before the last tile
  timing.add({{1ms, 100}, {2ms, 100}, {3ms, 100}, {4ms, 100}});
  timing.add({{2ms, 100}, {3ms, 100}, {4ms, 100}, {5ms, 100}});

  EXPECT_EQ(timing.frames(), 2u);
  EXPECT_EQ(timing.summary(), "2 frames, 4.0 tiles: first 1.50 ms, last 4.50 ms after submit, 75% of the bytes before the last");
}

TEST(AmfTileTimingTest, DescribesWholeFrames) {
  amf_direct::tile_timing_t timing;
  timing.add({{3ms, 1000}});

  EXPECT_EQ(timing.summary(), "1 frames, 1.0 tiles: first 3.00 ms, last 3.00 ms after submit, 0% of the bytes before the last");
}

TEST(AmfTileTimingTest, ResetForgetsTheFrames) {
  amf_direct::tile_timing_t timing;
  timing.add({{3ms, 1000}});
  timing.reset();

  EXPECT_EQ(timing.frames(), 0u);
  EXPECT_EQ(timing.summary(), "no frames");
}

/**
 * @brief Restores the AMD encoder settings changed by a test.
 */
struct UsesAmfDirectTest: testing::Test {
  void TearDown() override {
    config::video.amd = original_amd;
  }

  /**
   * @brief Build a stream configuration for a codec and chroma sampling.
   *
   * @param video_format 0 for H.264, 1 for HEVC, 2 for AV1.
   * @param chroma_sampling 0 for 4:2:0, 1 for 4:4:4.
   * @return The stream configuration.
   */
  static video::config_t stream(int video_format, int chroma_sampling = 0) {
    video::config_t config {};
    config.videoFormat = video_format;
    config.chromaSamplingType = chroma_sampling;
    return config;
  }

  decltype(config::video.amd) original_amd {config::video.amd};  ///< AMD encoder settings restored after each test.
};

TEST_F(UsesAmfDirectTest, IsOffByDefault) {
  EXPECT_FALSE(video::uses_amf_direct(video::amdvce, stream(2)));
}

TEST_F(UsesAmfDirectTest, EncodesAv1ThroughAmfWhenEnabled) {
  config::video.amd.amd_direct = true;
  EXPECT_TRUE(video::uses_amf_direct(video::amdvce, stream(2)));
}

TEST_F(UsesAmfDirectTest, LeavesOtherCodecsToFfmpeg) {
  config::video.amd.amd_direct = true;
  EXPECT_FALSE(video::uses_amf_direct(video::amdvce, stream(0)));
  EXPECT_FALSE(video::uses_amf_direct(video::amdvce, stream(1)));
}

TEST_F(UsesAmfDirectTest, LeavesYuv444ToFfmpeg) {
  config::video.amd.amd_direct = true;
  EXPECT_FALSE(video::uses_amf_direct(video::amdvce, stream(2, 1)));
}

TEST_F(UsesAmfDirectTest, LeavesOtherEncodersAlone) {
  config::video.amd.amd_direct = true;
  EXPECT_FALSE(video::uses_amf_direct(video::quicksync, stream(2)));
}

using AmfDirectConfigParam = std::tuple<std::string_view, bool, int>;

/**
 * @brief Parameterized coverage for parsing the direct AMF settings.
 */
struct AmfDirectConfigTest: BaseTest, testing::WithParamInterface<AmfDirectConfigParam> {
  void SetUp() override {
    BaseTest::SetUp();
    config::stream.file_apps = SUNSHINE_SOURCE_DIR "/tests/unit/platform/windows/test_amf_av1.cpp";
  }

  void TearDown() override {
    config::video = original_video;
    config::audio = original_audio;
    config::stream = original_stream;
    config::nvhttp = original_nvhttp;
    config::input = original_input;
    config::sunshine = original_sunshine;
    config::modified_config_settings = original_modified_config_settings;
    BaseTest::TearDown();
  }

  config::video_t original_video {config::video};  ///< Video configuration restored after each parameterized test.
  config::audio_t original_audio {config::audio};  ///< Audio configuration restored after each parameterized test.
  config::stream_t original_stream {config::stream};  ///< Stream configuration restored after each parameterized test.
  config::nvhttp_t original_nvhttp {config::nvhttp};  ///< HTTP configuration restored after each parameterized test.
  config::input_t original_input {config::input};  ///< Input configuration restored after each parameterized test.
  config::sunshine_t original_sunshine {config::sunshine};  ///< Core configuration restored after each parameterized test.
  decltype(config::modified_config_settings) original_modified_config_settings {config::modified_config_settings};  ///< Modified settings restored after each parameterized test.
};

TEST_P(AmfDirectConfigTest, ParsesTheSettings) {
  const auto &[setting, direct, tiles] = GetParam();
  config::video.amd.amd_direct = false;
  config::video.amd.amd_av1_tiles = 1;
  config::apply_config_for_test(setting);

  EXPECT_EQ(config::video.amd.amd_direct, direct);
  EXPECT_EQ(config::video.amd.amd_av1_tiles, tiles);
}

INSTANTIATE_TEST_SUITE_P(
  AmfDirectValues,
  AmfDirectConfigTest,
  testing::Values(
    AmfDirectConfigParam {""sv, false, 1},
    AmfDirectConfigParam {"amd_direct = enabled\n"sv, true, 1},
    AmfDirectConfigParam {"amd_direct = enabled\namd_av1_tiles = 4\n"sv, true, 4},
    AmfDirectConfigParam {"amd_av1_tiles = 64\n"sv, false, 64},
    AmfDirectConfigParam {"amd_av1_tiles = 0\n"sv, false, 1},
    AmfDirectConfigParam {"amd_av1_tiles = 65\n"sv, false, 1}
  )
);

TEST(AmfLtrIntervalTest, KeepsAReferenceEveryEighthOfASecond) {
  EXPECT_EQ(amf_direct::ltr_interval(60, 1), 7);
  EXPECT_EQ(amf_direct::ltr_interval(120, 1), 15);
  EXPECT_EQ(amf_direct::ltr_interval(60000, 1001), 7);
}

TEST(AmfLtrIntervalTest, KeepsAReferenceEveryFrameAtLowFrameRates) {
  EXPECT_EQ(amf_direct::ltr_interval(4, 1), 1);
  EXPECT_EQ(amf_direct::ltr_interval(0, 0), 1);
}

namespace {
  /**
   * @brief Plan frames from first to last, acknowledging every one marked.
   *
   * @param ltr The tracker.
   * @param first First frame index.
   * @param last Last frame index.
   */
  void plan_acknowledged(amf_direct::ltr_tracker_t &ltr, std::int64_t first, std::int64_t last) {
    for (auto frame = first; frame <= last; ++frame) {
      if (ltr.plan(frame, false).mark >= 0) {
        ltr.acknowledge(frame);
      }
    }
  }
}  // namespace

TEST(AmfLtrTrackerTest, MarksTheFirstKeyFrame) {
  amf_direct::ltr_tracker_t ltr {4, 8};
  const auto plan = ltr.plan(1, true);
  EXPECT_TRUE(plan.key);
  EXPECT_EQ(plan.mark, 0);
  EXPECT_EQ(plan.reference, -1);
  EXPECT_EQ(ltr.frame_in(0), 1);
}

TEST(AmfLtrTrackerTest, MarksAFrameEachInterval) {
  amf_direct::ltr_tracker_t ltr {4, 8};
  ltr.plan(1, true);
  for (std::int64_t frame = 2; frame < 9; ++frame) {
    EXPECT_EQ(ltr.plan(frame, false).mark, -1) << frame;
  }
  const auto plan = ltr.plan(9, false);
  EXPECT_FALSE(plan.key);
  EXPECT_EQ(plan.mark, 1);
  EXPECT_EQ(ltr.frame_in(1), 9);
}

TEST(AmfLtrTrackerTest, ReplacesTheOldestReferenceOnceAllAreUsed) {
  amf_direct::ltr_tracker_t ltr {2, 1};
  EXPECT_EQ(ltr.plan(1, false).mark, 0);
  EXPECT_EQ(ltr.plan(2, false).mark, 1);
  EXPECT_EQ(ltr.plan(3, false).mark, 0);
  EXPECT_EQ(ltr.plan(4, false).mark, 1);
  EXPECT_EQ(ltr.frame_in(0), 3);
  EXPECT_EQ(ltr.frame_in(1), 4);
}

TEST(AmfLtrTrackerTest, KeepsTheNewestReferenceTheClientHas) {
  amf_direct::ltr_tracker_t ltr {2, 1};
  ltr.plan(1, false);
  ltr.acknowledge(1);
  ltr.plan(2, false);

  // Frame 1 is the only one the client confirmed, so slot 1 is replaced although it is newer
  EXPECT_EQ(ltr.plan(3, false).mark, 1);
  EXPECT_EQ(ltr.plan(4, false).mark, 1);
  EXPECT_EQ(ltr.frame_in(0), 1);
}

TEST(AmfLtrTrackerTest, ReplacesASingleReferenceEvenOnceConfirmed) {
  amf_direct::ltr_tracker_t ltr {1, 1};
  ltr.plan(1, false);
  ltr.acknowledge(1);
  EXPECT_EQ(ltr.plan(2, false).mark, 0);
  EXPECT_EQ(ltr.frame_in(0), 2);
}

TEST(AmfLtrTrackerTest, RecoversFromTheNewestConfirmedReferenceBeforeTheLoss) {
  amf_direct::ltr_tracker_t ltr {4, 4};
  ltr.plan(1, true);
  ltr.acknowledge(1);
  plan_acknowledged(ltr, 2, 12);  // Frames 5 and 9 marked and confirmed

  ltr.invalidate(10, 12);
  const auto plan = ltr.plan(13, false);
  EXPECT_FALSE(plan.key);
  EXPECT_EQ(plan.mark, -1);
  ASSERT_GE(plan.reference, 0);
  EXPECT_EQ(ltr.frame_in(plan.reference), 9);
}

TEST(AmfLtrTrackerTest, DropsReferencesFromTheLossOn) {
  amf_direct::ltr_tracker_t ltr {4, 4};
  ltr.plan(1, true);
  ltr.acknowledge(1);
  plan_acknowledged(ltr, 2, 12);  // Frames 5 and 9 marked and confirmed

  // Frame 9 came after the first lost frame, so it may depend on it
  ltr.invalidate(7, 12);
  const auto plan = ltr.plan(13, false);
  ASSERT_GE(plan.reference, 0);
  EXPECT_EQ(ltr.frame_in(plan.reference), 5);
  for (int slot = 0; slot < ltr.slots(); ++slot) {
    EXPECT_LT(ltr.frame_in(slot), 7) << slot;
  }
}

TEST(AmfLtrTrackerTest, SkipsReferencesNotConfirmed) {
  amf_direct::ltr_tracker_t ltr {4, 4};
  ltr.plan(1, true);
  ltr.acknowledge(1);
  for (std::int64_t frame = 2; frame <= 12; ++frame) {
    ltr.plan(frame, false);  // Frames 5 and 9 marked, never confirmed
  }

  ltr.invalidate(11, 12);
  const auto plan = ltr.plan(13, false);
  ASSERT_GE(plan.reference, 0);
  EXPECT_EQ(ltr.frame_in(plan.reference), 1);
}

TEST(AmfLtrTrackerTest, FallsBackToAKeyFrameWithNoConfirmedReference) {
  amf_direct::ltr_tracker_t ltr {4, 4};
  ltr.plan(1, true);
  ltr.plan(2, false);

  ltr.invalidate(2, 2);
  const auto plan = ltr.plan(3, false);
  EXPECT_TRUE(plan.key);
  EXPECT_EQ(plan.reference, -1);
  EXPECT_EQ(plan.mark, 0);
  EXPECT_EQ(ltr.frame_in(0), 3);
  for (int slot = 1; slot < ltr.slots(); ++slot) {
    EXPECT_EQ(ltr.frame_in(slot), -1) << slot;
  }
}

TEST(AmfLtrTrackerTest, MergesLossesReportedBeforeTheNextFrame) {
  amf_direct::ltr_tracker_t ltr {4, 4};
  ltr.plan(1, true);
  ltr.acknowledge(1);
  plan_acknowledged(ltr, 2, 12);  // Frames 5 and 9 marked and confirmed

  ltr.invalidate(10, 11);
  ltr.invalidate(6, 12);
  const auto plan = ltr.plan(13, false);
  ASSERT_GE(plan.reference, 0);
  EXPECT_EQ(ltr.frame_in(plan.reference), 5);
}

TEST(AmfLtrTrackerTest, IgnoresLossesTheLastRecoveryCovered) {
  amf_direct::ltr_tracker_t ltr {4, 4};
  ltr.plan(1, true);
  ltr.acknowledge(1);
  plan_acknowledged(ltr, 2, 12);

  ltr.invalidate(10, 12);
  ASSERT_GE(ltr.plan(13, false).reference, 0);

  // The client hadn't seen frame 13 yet when it reported frames up to 12 again
  ltr.invalidate(10, 12);
  const auto plan = ltr.plan(14, false);
  EXPECT_FALSE(plan.key);
  EXPECT_EQ(plan.reference, -1);

  // A loss including the recovering frame needs another recovery
  ltr.invalidate(13, 14);
  EXPECT_GE(ltr.plan(15, false).reference, 0);
}

TEST(AmfLtrTrackerTest, IgnoresLossesBeforeTheLastKeyFrame) {
  amf_direct::ltr_tracker_t ltr {4, 4};
  ltr.plan(1, true);
  ltr.acknowledge(1);
  ltr.plan(2, true);

  ltr.invalidate(1, 1);
  const auto plan = ltr.plan(3, false);
  EXPECT_FALSE(plan.key);
  EXPECT_EQ(plan.reference, -1);
}

TEST(AmfLtrTrackerTest, LetsAKeyFrameRequestWinOverALoss) {
  amf_direct::ltr_tracker_t ltr {4, 4};
  ltr.plan(1, true);
  ltr.acknowledge(1);
  ltr.plan(2, false);

  ltr.invalidate(2, 2);
  const auto plan = ltr.plan(3, true);
  EXPECT_TRUE(plan.key);
  EXPECT_EQ(plan.reference, -1);

  // The loss went with the key frame
  EXPECT_EQ(ltr.plan(4, false).reference, -1);
}

TEST(AmfLtrTrackerTest, RecoversWithKeyFramesWithoutReferences) {
  amf_direct::ltr_tracker_t ltr;
  EXPECT_EQ(ltr.slots(), 0);
  const auto first = ltr.plan(1, true);
  EXPECT_TRUE(first.key);
  EXPECT_EQ(first.mark, -1);
  EXPECT_EQ(ltr.plan(2, false).mark, -1);

  ltr.invalidate(2, 2);
  const auto plan = ltr.plan(3, false);
  EXPECT_TRUE(plan.key);
  EXPECT_EQ(plan.reference, -1);
}

TEST(AmfLtrTrackerTest, ForgetsASlot) {
  amf_direct::ltr_tracker_t ltr {2, 1};
  ltr.plan(1, false);
  ltr.acknowledge(1);
  ltr.forget(0);
  EXPECT_EQ(ltr.frame_in(0), -1);

  // Nothing left to recover from
  ltr.invalidate(2, 2);
  EXPECT_TRUE(ltr.plan(3, false).key);
}

TEST(AmfLtrTrackerTest, IgnoresSlotsOutOfRange) {
  amf_direct::ltr_tracker_t ltr {2, 1};
  ltr.plan(1, false);
  ltr.forget(-1);
  ltr.forget(2);
  EXPECT_EQ(ltr.frame_in(0), 1);
  EXPECT_EQ(ltr.frame_in(-1), -1);
  EXPECT_EQ(ltr.frame_in(2), -1);
}

TEST(AmfLtrTrackerTest, IgnoresAcknowledgementsForOtherFrames) {
  amf_direct::ltr_tracker_t ltr {2, 4};
  ltr.plan(1, false);
  ltr.plan(2, false);
  ltr.acknowledge(2);

  ltr.invalidate(3, 3);
  EXPECT_TRUE(ltr.plan(4, false).key);
}
#endif
