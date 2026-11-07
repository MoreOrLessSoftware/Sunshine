/**
 * @file tests/unit/platform/windows/test_amf_direct.cpp
 * @brief Test src/amf/amf_direct.cpp. The encoder tests are skipped without an AMD GPU.
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
  #include <src/amf/amf_direct.h>
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

TEST(AmfTileGroupsTest, ReadsTheTileRange) {
  // Of 4 tiles: the flag, then tg_start 2 (10) and tg_end 3 (11), as 1 10 11 000
  const std::vector<std::uint8_t> data {0x22, 0x02, 0xD8, 0xAA};
  const auto groups = amf_direct::tile_groups(data.data(), data.size(), 2);
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_EQ(groups[0].start, 2);
  EXPECT_EQ(groups[0].end, 3);
}

TEST(AmfTileGroupsTest, TakesAllTilesWithoutTheFlag) {
  const std::vector<std::uint8_t> data {0x22, 0x01, 0x00};
  const auto groups = amf_direct::tile_groups(data.data(), data.size(), 2);
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_EQ(groups[0].start, 0);
  EXPECT_EQ(groups[0].end, 3);
}

TEST(AmfTileGroupsTest, SkipsOtherObusAndStopsAtLeftoverBytes) {
  // Temporal delimiter, frame header of 1 byte, tile 0 of 4, tile 1 of 4, then zero bytes
  const std::vector<std::uint8_t> data {0x12, 0x00, 0x1A, 0x01, 0xCC, 0x22, 0x02, 0x80, 0xAA, 0x22, 0x02, 0xA8, 0xBB, 0, 0};
  const auto groups = amf_direct::tile_groups(data.data(), data.size(), 2);
  ASSERT_EQ(groups.size(), 2u);
  EXPECT_EQ(groups[0].start, 0);
  EXPECT_EQ(groups[0].end, 0);
  EXPECT_EQ(groups[1].start, 1);
  EXPECT_EQ(groups[1].end, 1);
}

TEST(AmfTileGroupsTest, MarksAFrameObuAsUnknown) {
  const std::vector<std::uint8_t> data {0x32, 0x01, 0x00};
  const auto groups = amf_direct::tile_groups(data.data(), data.size(), 2);
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_EQ(groups[0].start, -1);
}

TEST(AmfPartTimingTest, DescribesNoFrames) {
  const amf_direct::part_timing_t timing;
  EXPECT_EQ(timing.frames(), 0u);
  EXPECT_EQ(timing.summary(), "no frames");
}

TEST(AmfPartTimingTest, IgnoresAFrameWithNoParts) {
  amf_direct::part_timing_t timing;
  timing.add({});
  EXPECT_EQ(timing.frames(), 0u);
}

TEST(AmfPartTimingTest, AveragesWhenPartsComeOut) {
  amf_direct::part_timing_t timing;
  // Parts coming out as they finish: three quarters of the frame is out before the last part
  timing.add({{1ms, 100}, {2ms, 100}, {3ms, 100}, {4ms, 100}});
  timing.add({{2ms, 100}, {3ms, 100}, {4ms, 100}, {5ms, 100}});

  EXPECT_EQ(timing.frames(), 2u);
  EXPECT_EQ(timing.summary(), "2 frames, 4.0 parts: first 1.50 ms, last 4.50 ms after submit, 75% of the bytes before the last");
}

TEST(AmfPartTimingTest, DescribesWholeFrames) {
  amf_direct::part_timing_t timing;
  timing.add({{3ms, 1000}});

  EXPECT_EQ(timing.summary(), "1 frames, 1.0 parts: first 3.00 ms, last 3.00 ms after submit, 0% of the bytes before the last");
}

TEST(AmfPartTimingTest, ResetForgetsTheFrames) {
  amf_direct::part_timing_t timing;
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

TEST_F(UsesAmfDirectTest, EncodesHevcThroughAmfWhenEnabled) {
  config::video.amd.amd_direct = true;
  EXPECT_TRUE(video::uses_amf_direct(video::amdvce, stream(1)));
}

TEST_F(UsesAmfDirectTest, LeavesH264ToFfmpeg) {
  config::video.amd.amd_direct = true;
  EXPECT_FALSE(video::uses_amf_direct(video::amdvce, stream(0)));
}

TEST_F(UsesAmfDirectTest, LeavesYuv444ToFfmpeg) {
  config::video.amd.amd_direct = true;
  EXPECT_FALSE(video::uses_amf_direct(video::amdvce, stream(2, 1)));
  EXPECT_FALSE(video::uses_amf_direct(video::amdvce, stream(1, 1)));
}

TEST_F(UsesAmfDirectTest, LeavesOtherEncodersAlone) {
  config::video.amd.amd_direct = true;
  EXPECT_FALSE(video::uses_amf_direct(video::quicksync, stream(2)));
}

using AmfDirectConfigParam = std::tuple<std::string_view, bool, int, int>;

/**
 * @brief Parameterized coverage for parsing the direct AMF settings.
 */
struct AmfDirectConfigTest: BaseTest, testing::WithParamInterface<AmfDirectConfigParam> {
  void SetUp() override {
    BaseTest::SetUp();
    config::stream.file_apps = SUNSHINE_SOURCE_DIR "/tests/unit/platform/windows/test_amf_direct.cpp";
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
  const auto &[setting, direct, tiles, slices] = GetParam();
  config::video.amd.amd_direct = false;
  config::video.amd.amd_av1_tiles = 1;
  config::video.amd.amd_hevc_slices = 1;
  config::apply_config_for_test(setting);

  EXPECT_EQ(config::video.amd.amd_direct, direct);
  EXPECT_EQ(config::video.amd.amd_av1_tiles, tiles);
  EXPECT_EQ(config::video.amd.amd_hevc_slices, slices);
}

INSTANTIATE_TEST_SUITE_P(
  AmfDirectValues,
  AmfDirectConfigTest,
  testing::Values(
    AmfDirectConfigParam {""sv, false, 1, 1},
    AmfDirectConfigParam {"amd_direct = enabled\n"sv, true, 1, 1},
    AmfDirectConfigParam {"amd_direct = enabled\namd_av1_tiles = 4\n"sv, true, 4, 1},
    AmfDirectConfigParam {"amd_av1_tiles = 64\n"sv, false, 64, 1},
    AmfDirectConfigParam {"amd_av1_tiles = 0\n"sv, false, 1, 1},
    AmfDirectConfigParam {"amd_av1_tiles = 65\n"sv, false, 1, 1},
    AmfDirectConfigParam {"amd_direct = enabled\namd_hevc_slices = 4\n"sv, true, 1, 4},
    AmfDirectConfigParam {"amd_hevc_slices = 64\n"sv, false, 1, 64},
    AmfDirectConfigParam {"amd_hevc_slices = 0\n"sv, false, 1, 1},
    AmfDirectConfigParam {"amd_hevc_slices = 65\n"sv, false, 1, 1}
  )
);

TEST(AmfLtrIntervalTest, KeepsAReferenceEveryEighthOfASecond) {
  EXPECT_EQ(amf_direct::ltr_interval(60, 1), 7);
  EXPECT_EQ(amf_direct::ltr_interval(120, 1), 15);
  EXPECT_EQ(amf_direct::ltr_interval(60000, 1001), 7);
}

TEST(AmfHevcLevelTest, AsksForLevel51UpTo4K60) {
  EXPECT_EQ(amf_direct::hevc_level(1920, 1080, 60), 153);
  EXPECT_EQ(amf_direct::hevc_level(4096, 2176, 60), 153);
}

TEST(AmfHevcLevelTest, AsksForLevel52UpTo4K120) {
  EXPECT_EQ(amf_direct::hevc_level(3840, 2160, 120), 156);
}

TEST(AmfHevcLevelTest, LeavesTheLevelToAmfBeyondLevel52) {
  EXPECT_EQ(amf_direct::hevc_level(3840, 2160, 144), 0);
  EXPECT_EQ(amf_direct::hevc_level(7680, 4320, 30), 0);
}

TEST(AmfDescribeNalsTest, NamesAKeyFrame) {
  const std::vector<std::uint8_t> data {0, 0, 0, 1, 0x46, 0x01, 0x50, 0, 0, 1, 0x40, 0x01, 0x0C, 0, 0, 1, 0x42, 0x01, 0, 0, 1, 0x44, 0x01, 0, 0, 0, 1, 0x28, 0x01, 0xAF, 0x80};
  EXPECT_EQ(amf_direct::describe_nals(data.data(), data.size()), "AUD:3 VPS:3 SPS:2 PPS:2 IDR_N_LP:4");
}

TEST(AmfDescribeNalsTest, NamesOtherTypes) {
  const std::vector<std::uint8_t> data {0, 0, 1, 0x00, 0x01, 0, 0, 1, 0x02, 0x01, 0, 0, 1, 0x26, 0x01, 0, 0, 1, 0x2A, 0x01, 0, 0, 1, 0x4E, 0x01, 0, 0, 1, 0x50, 0x01, 0, 0, 1, 0x30, 0x01};
  EXPECT_EQ(amf_direct::describe_nals(data.data(), data.size()), "TRAIL_N:2 TRAIL_R:2 IDR_W_RADL:2 CRA:2 SEI:2 SEI:2 24:2");
}

TEST(AmfDescribeNalsTest, SaysWhenTheDataDoesNotStartWithAStartCode) {
  const std::vector<std::uint8_t> data {0x12, 0, 0, 1, 0x02, 0x01};
  EXPECT_EQ(amf_direct::describe_nals(data.data(), data.size()), "invalid at 0 TRAIL_R:2");

  const std::vector<std::uint8_t> late {0x12, 0x34, 0, 0, 1, 0x02, 0x01};
  EXPECT_EQ(amf_direct::describe_nals(late.data(), late.size()), "invalid at 0 TRAIL_R:2");

  const std::vector<std::uint8_t> none {0x12, 0x34};
  EXPECT_EQ(amf_direct::describe_nals(none.data(), none.size()), "invalid at 0");
}

TEST(AmfDescribeNalsTest, MarksAStartCodeAtTheEnd) {
  const std::vector<std::uint8_t> data {0, 0, 1};
  EXPECT_EQ(amf_direct::describe_nals(data.data(), data.size()), "?:0");
}

namespace {
  /**
   * @brief Drives the encoder on this machine's GPU, for each codec. Skipped where AMF can't
   *        encode the codec.
   */
  struct AmfEncoderTest: testing::TestWithParam<amf_direct::codec_e> {
    void SetUp() override {
      if (!amf_direct::runtime_available()) {
        GTEST_SKIP() << "The AMF runtime isn't available";
      }
      if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
        GTEST_SKIP() << "No Direct3D 11 device";
      }

      const bool hevc = GetParam() == amf_direct::codec_e::hevc;
      config.width = 1280;
      config.height = 720;
      config.framerate = 60;
      config.bitrate = 20000;
      config.videoFormat = hevc ? 1 : 2;

      amf_direct::settings_t settings;
      settings.codec = GetParam();
      if (hevc) {
        settings.usage = 1;  // Ultra low latency
        settings.quality_preset = 10;  // Speed
        settings.rate_control = 3;  // CBR
        settings.vbaq = false;
      }
      settings.parts = 4;
      settings.ltr_frames = 4;
      if (!encoder.create(device, config, {video::colorspace_e::rec709, false, 8}, settings)) {
        GTEST_SKIP() << "This GPU can't encode the codec through AMF";
      }

      D3D11_TEXTURE2D_DESC desc {};
      encoder.input_texture()->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    }

    void TearDown() override {
      if (staging) {
        staging->Release();
      }
      if (context) {
        context->Release();
      }
      if (device) {
        device->Release();
      }
    }

    /**
     * @brief Draw a moving pattern into the encoder's input texture.
     *
     * @param frame Frame number, which moves the pattern.
     */
    void draw(int frame) {
      D3D11_MAPPED_SUBRESOURCE mapped;
      ASSERT_TRUE(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped)));
      for (int y = 0; y < config.height * 3 / 2; ++y) {
        auto *row = static_cast<std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (int x = 0; x < config.width; ++x) {
          row[x] = y < config.height ? static_cast<std::uint8_t>((x + frame * 7) ^ (y + frame * 3)) : static_cast<std::uint8_t>(128 + ((x / 16 + y / 16 + frame) % 2 ? 40 : -40));
        }
      }
      context->Unmap(staging, 0);
      context->CopyResource(encoder.input_texture(), staging);
      context->Flush();
    }

    /**
     * @brief Draw and encode a frame.
     *
     * @param index Frame index.
     * @param force_idr Whether to ask for a key frame.
     * @param frame Receives the frame.
     * @param on_part Optional. Called as each part but the last comes out.
     */
    void encode(int index, bool force_idr, amf_direct::encoded_frame_t &frame, const amf_direct::part_callback_t &on_part = {}) {
      draw(index);
      ASSERT_TRUE(encoder.encode_frame(index, force_idr, frame, nullptr, on_part));
      ASSERT_FALSE(frame.data.empty());
    }

    /**
     * @brief Describe a bitstream of the codec encoded.
     *
     * @param data The bitstream.
     * @return The OBUs or NAL units in it.
     */
    std::string describe(const std::vector<std::uint8_t> &data) const {
      return GetParam() == amf_direct::codec_e::hevc ? amf_direct::describe_nals(data.data(), data.size()) : amf_direct::describe_obus(data.data(), data.size());
    }

    ID3D11Device *device = nullptr;  ///< Device the encoder reads frames from.
    ID3D11DeviceContext *context = nullptr;  ///< The device's immediate context.
    ID3D11Texture2D *staging = nullptr;  ///< Texture frames are drawn into before they are copied to the encoder.
    video::config_t config {};  ///< Stream configuration.
    amf_direct::encoder_t encoder;  ///< The encoder.
  };
}  // namespace

TEST_P(AmfEncoderTest, HandsOverEachPartAsItFinishes) {
  for (int index = 0; index < 4; ++index) {
    std::vector<std::size_t> sizes;
    int parts_total = 0;
    amf_direct::encoded_frame_t frame;
    encode(index, false, frame, [&](const amf_direct::encoded_frame_t &so_far, int total) {
      sizes.push_back(so_far.data.size());
      parts_total = total;
      EXPECT_EQ(so_far.idr, index == 0);
    });

    EXPECT_EQ(frame.idr, index == 0);
    EXPECT_GT(frame.parts.size(), 1u);
    EXPECT_EQ(sizes.size() + 1, frame.parts.size());
    EXPECT_EQ(static_cast<std::size_t>(parts_total), frame.parts.size());

    std::size_t bytes = 0;
    for (std::size_t i = 0; i < frame.parts.size(); ++i) {
      bytes += frame.parts[i].bytes;
      if (i < sizes.size()) {
        EXPECT_EQ(sizes[i], bytes);
      }
    }
    EXPECT_EQ(bytes, frame.data.size());

    const auto description = describe(frame.data);
    EXPECT_EQ(description.find("invalid"), std::string::npos) << description;
    if (GetParam() == amf_direct::codec_e::hevc) {
      // Each slice starts with its own start code, so the parts can be cut anywhere between them
      std::size_t offset = 0;
      for (const auto &part : frame.parts) {
        const auto start = amf_direct::describe_nals(frame.data.data() + offset, part.bytes);
        EXPECT_EQ(start.find("invalid"), std::string::npos) << start;
        offset += part.bytes;
      }
      EXPECT_NE(description.find(index == 0 ? "VPS:" : "TRAIL_R:"), std::string::npos) << description;
    }
  }
}

TEST_P(AmfEncoderTest, MakesAKeyFrameWhenAsked) {
  amf_direct::encoded_frame_t frame;
  encode(0, false, frame);
  encode(1, false, frame);
  EXPECT_FALSE(frame.idr);

  encode(2, true, frame);
  EXPECT_TRUE(frame.idr);
  if (GetParam() == amf_direct::codec_e::hevc) {
    EXPECT_NE(describe(frame.data).find("SPS:"), std::string::npos);
  }
}

TEST_P(AmfEncoderTest, RecoversFromLostFramesWithALongTermReference) {
  amf_direct::encoded_frame_t frame;
  int kept = 0;
  for (int index = 0; index < 16; ++index) {
    encode(index, false, frame);
    if (frame.ltr) {
      ++kept;
      encoder.acknowledge_ltr(index);
    }
  }
  EXPECT_GT(kept, 1);

  encoder.invalidate_ref_frames(14, 15);
  encode(16, false, frame);
  EXPECT_TRUE(frame.after_ref_frame_invalidation);
  EXPECT_FALSE(frame.idr);
}

INSTANTIATE_TEST_SUITE_P(
  AmfCodecs,
  AmfEncoderTest,
  testing::Values(amf_direct::codec_e::av1, amf_direct::codec_e::hevc),
  [](const testing::TestParamInfo<amf_direct::codec_e> &info) {
    return info.param == amf_direct::codec_e::hevc ? "Hevc" : "Av1";
  }
);

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
