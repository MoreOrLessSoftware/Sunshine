/**
 * @file tests/unit/platform/windows/test_playstation_report.cpp
 * @brief Test the conversions shared by the virtual PlayStation controllers.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <cstdint>
  #include <limits>

  // local includes
  #include <src/platform/windows/playstation_report.h>

using namespace platf::playstation;

/**
 * @brief Test fixture for the shared PlayStation conversions.
 */
class PlaystationReportTest: public BaseTest {};

TEST_F(PlaystationReportTest, AccelCountsAreScaledPerG) {
  EXPECT_EQ(accel_counts(0.0f, earth_g, -earth_g), (motion_counts_t {0, 8192, -8192}));
  EXPECT_EQ(accel_counts(earth_g / 2, 0.0f, 0.0f).x, 4096);
  // Truncated towards zero, not clamped
  EXPECT_EQ(accel_counts(earth_g * 4, 0.0f, 0.0f).x, 32768);
}

TEST_F(PlaystationReportTest, GyroCountsAreScaledPerDegreePerSecond) {
  EXPECT_EQ(gyro_counts(1.0f, -1.0f, 0.0f), (motion_counts_t {16, -16, 0}));
  EXPECT_EQ(gyro_counts(90.5f, 0.0f, 0.0f).x, 1448);
  EXPECT_EQ(gyro_counts(-0.05f, 0.0f, 0.0f).x, 0);
  EXPECT_EQ(gyro_counts(3000.0f, 0.0f, 0.0f).x, 48000);
}

TEST_F(PlaystationReportTest, ClampI16) {
  EXPECT_EQ(clamp_i16(0), 0);
  EXPECT_EQ(clamp_i16(-1234), -1234);
  EXPECT_EQ(clamp_i16(48000), INT16_MAX);
  EXPECT_EQ(clamp_i16(-48000), INT16_MIN);
}

TEST_F(PlaystationReportTest, TouchCoordinateStaysOnThePad) {
  EXPECT_EQ(touch_coordinate(0.0f, 1920), 0);
  EXPECT_EQ(touch_coordinate(0.5f, 1920), 960);
  EXPECT_EQ(touch_coordinate(1.0f, 1920), 1919);
  EXPECT_EQ(touch_coordinate(1.5f, 943), 942);
  EXPECT_EQ(touch_coordinate(-0.25f, 943), 0);
  EXPECT_EQ(touch_coordinate(std::numeric_limits<float>::quiet_NaN(), 943), 0);
}

TEST_F(PlaystationReportTest, PackTouch) {
  const auto packed = pack_touch(0x0ABC, 0x0123);
  EXPECT_EQ(packed[0], 0xBC);
  EXPECT_EQ(packed[1], 0x3A);  // X bits 8..11 = 0xA, Y bits 0..3 = 0x3
  EXPECT_EQ(packed[2], 0x12);

  const auto max = pack_touch(1919, 1079);
  EXPECT_EQ(max[0] | ((max[1] & 0x0F) << 8), 1919);
  EXPECT_EQ((max[1] >> 4) | (max[2] << 4), 1079);
}

TEST_F(PlaystationReportTest, TouchSlotsAssignLowestFreeSlot) {
  touch_slots_t slots;

  EXPECT_EQ(slots.down(10), 0);
  EXPECT_EQ(slots.down(20), 1);
  EXPECT_FALSE(slots.down(30).has_value());  // both taken

  // A pointer that is already down keeps its slot
  EXPECT_EQ(slots.down(20), 1);

  EXPECT_EQ(slots.find(10), 0);
  EXPECT_EQ(slots.find(20), 1);
  EXPECT_FALSE(slots.find(30).has_value());

  EXPECT_EQ(slots.up(10), 0);
  EXPECT_FALSE(slots.find(10).has_value());
  EXPECT_FALSE(slots.up(10).has_value());  // already up
  EXPECT_EQ(slots.down(30), 0);  // the freed slot is reused
}

TEST_F(PlaystationReportTest, TouchSlotsClear) {
  touch_slots_t slots;
  slots.down(1);
  slots.down(2);

  slots.clear();
  EXPECT_FALSE(slots.find(1).has_value());
  EXPECT_FALSE(slots.find(2).has_value());
  EXPECT_EQ(slots.down(3), 0);
  EXPECT_EQ(slots.down(4), 1);
}

#endif
