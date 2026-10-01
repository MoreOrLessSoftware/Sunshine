/**
 * @file tests/unit/test_video_level.cpp
 * @brief Test src/video_level.h.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <vector>

// local includes
#include <src/video_level.h>

namespace {
  /**
   * @brief Insert emulation prevention bytes the way an encoder does.
   *
   * @param raw NAL unit's raw bytes.
   * @return The bytes as they appear in the stream.
   */
  std::vector<std::uint8_t> escape(const std::vector<std::uint8_t> &raw) {
    std::vector<std::uint8_t> escaped;
    int zeros = 0;
    for (const auto byte : raw) {
      if (zeros >= 2 && byte <= 3) {
        escaped.push_back(3);
        zeros = 0;
      }
      zeros = byte == 0 ? zeros + 1 : 0;
      escaped.push_back(byte);
    }
    return escaped;
  }

  /**
   * @brief Append a NAL unit with a start code.
   *
   * @param stream Stream to append to.
   * @param raw NAL unit's raw bytes, escaped as they are appended.
   * @param long_start_code Whether to use a 4 byte start code rather than 3.
   */
  void append_nal(std::vector<std::uint8_t> &stream, const std::vector<std::uint8_t> &raw, bool long_start_code = true) {
    if (long_start_code) {
      stream.push_back(0);
    }
    stream.insert(stream.end(), {0, 0, 1});
    const auto escaped = escape(raw);
    stream.insert(stream.end(), escaped.begin(), escaped.end());
  }

  /**
   * @brief Build the raw bytes of an HEVC SPS, up to and a little past general_level_idc.
   *
   * @param tier general_tier_flag.
   * @param level general_level_idc.
   * @return Raw SPS bytes.
   */
  std::vector<std::uint8_t> hevc_sps(std::uint8_t tier, std::uint8_t level) {
    return {
      0x42,
      0x01,  // nal_unit_type 33
      0x01,  // sps_video_parameter_set_id, sps_max_sub_layers_minus1, sps_temporal_id_nesting_flag
      static_cast<std::uint8_t>((tier << 5) | 2),  // general_profile_space, general_tier_flag, Main 10
      0x20,
      0x00,
      0x00,
      0x00,  // general_profile_compatibility_flag for Main 10
      0x90,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,  // progressive and frame only constraint flags, the rest zero
      level,
      0xA0,
      0x01,  // more SPS fields
    };
  }

  const std::vector<std::uint8_t> hevc_vps {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF};  ///< Start of an HEVC VPS, nal_unit_type 32.
  const std::vector<std::uint8_t> hevc_pps {0x44, 0x01, 0xC1, 0x72};  ///< HEVC PPS, nal_unit_type 34.
}  // namespace

TEST(VideoLevelTest, SplitsNalUnitsAtBothStartCodeLengths) {
  std::vector<std::uint8_t> stream;
  append_nal(stream, {0x40, 0x01, 0x0C});
  append_nal(stream, {0x42, 0x01, 0x01}, false);
  append_nal(stream, {0x44, 0x01});

  const auto units = video::level::nal_units(stream);
  ASSERT_EQ(units.size(), 3u);
  EXPECT_EQ(std::vector<std::uint8_t>(units[0].begin(), units[0].end()), (std::vector<std::uint8_t> {0x40, 0x01, 0x0C}));
  EXPECT_EQ(std::vector<std::uint8_t>(units[1].begin(), units[1].end()), (std::vector<std::uint8_t> {0x42, 0x01, 0x01}));
  EXPECT_EQ(std::vector<std::uint8_t>(units[2].begin(), units[2].end()), (std::vector<std::uint8_t> {0x44, 0x01}));
}

TEST(VideoLevelTest, FindsNoNalUnitsWithoutAStartCode) {
  const std::vector<std::uint8_t> stream {0x42, 0x01, 0x01};
  EXPECT_TRUE(video::level::nal_units(stream).empty());
}

TEST(VideoLevelTest, RemovesEmulationPreventionBytes) {
  const std::vector<std::uint8_t> escaped {0x20, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x01, 0x03};
  EXPECT_EQ(video::level::unescape(escaped), (std::vector<std::uint8_t> {0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03}));
}

TEST(VideoLevelTest, ReadsTheHevcLevelAndTier) {
  std::vector<std::uint8_t> stream;
  append_nal(stream, hevc_vps);
  append_nal(stream, hevc_sps(1, 183));
  append_nal(stream, hevc_pps);

  // The compatibility and constraint flags are escaped in the stream
  ASSERT_GT(stream.size(), hevc_vps.size() + hevc_sps(1, 183).size() + hevc_pps.size() + 12);

  const auto level = video::level::hevc_level(stream);
  ASSERT_TRUE(level.has_value());
  EXPECT_EQ(level->level, 183u);  // 6.1
  EXPECT_EQ(level->tier, 1u);
}

TEST(VideoLevelTest, ReadsTheHevcMainTier) {
  std::vector<std::uint8_t> stream;
  append_nal(stream, hevc_sps(0, 153));

  const auto level = video::level::hevc_level(stream);
  ASSERT_TRUE(level.has_value());
  EXPECT_EQ(level->level, 153u);  // 5.1
  EXPECT_EQ(level->tier, 0u);
}

TEST(VideoLevelTest, NeedsAWholeHevcSps) {
  std::vector<std::uint8_t> stream;
  append_nal(stream, hevc_vps);
  append_nal(stream, {0x42, 0x01, 0x01, 0x22});
  EXPECT_FALSE(video::level::hevc_level(stream).has_value());

  std::vector<std::uint8_t> no_sps;
  append_nal(no_sps, hevc_vps);
  append_nal(no_sps, hevc_pps);
  EXPECT_FALSE(video::level::hevc_level(no_sps).has_value());
}

TEST(VideoLevelTest, ReadsTheH264Level) {
  std::vector<std::uint8_t> stream;
  append_nal(stream, {0x67, 0x64, 0x00, 0x34, 0xAC, 0x2B});  // SPS: High profile, level 5.2
  append_nal(stream, {0x68, 0xEE, 0x3C, 0x80});  // PPS

  const auto level = video::level::h264_level(stream);
  ASSERT_TRUE(level.has_value());
  EXPECT_EQ(level->level, 52u);
  EXPECT_EQ(level->tier, 0u);
}

TEST(VideoLevelTest, NeedsAWholeH264Sps) {
  std::vector<std::uint8_t> stream;
  append_nal(stream, {0x67, 0x64, 0x00});
  EXPECT_FALSE(video::level::h264_level(stream).has_value());

  std::vector<std::uint8_t> no_sps;
  append_nal(no_sps, {0x68, 0xEE, 0x3C, 0x80});
  EXPECT_FALSE(video::level::h264_level(no_sps).has_value());
}
