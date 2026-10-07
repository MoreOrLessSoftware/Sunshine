/**
 * @file tests/unit/platform/windows/test_pyrowave_d3d11.cpp
 * @brief Test the helpers in src/pyrowave/pyrowave_d3d11.cpp that don't need a GPU.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <array>
  #include <cstdint>
  #include <vector>

  // lib includes
  #include <vulkan/vulkan_core.h>
// clang-format off
  #include <pyrowave/pyrowave.h>
  // clang-format on

  // local includes
  #include <src/pyrowave/pyrowave_d3d11.h>

TEST(PyrowaveJoinPacketsTest, KeepsASinglePacketAtTheStartInPlace) {
  std::vector<std::uint8_t> frame {1, 2, 3, 0, 0, 0};
  const std::array packets {pyrowave_packet {0, 3}};

  pyrowave::join_packets(frame, packets.data(), packets.size());

  EXPECT_EQ(frame, (std::vector<std::uint8_t> {1, 2, 3}));
}

TEST(PyrowaveJoinPacketsTest, MovesPacketsTogether) {
  // Each packet starts at its own boundary, with unused space after it
  std::vector<std::uint8_t> frame {1, 2, 0, 0, 3, 4, 5, 0, 6, 0, 0, 0};
  const std::array packets {pyrowave_packet {0, 2}, pyrowave_packet {4, 3}, pyrowave_packet {8, 1}};

  pyrowave::join_packets(frame, packets.data(), packets.size());

  EXPECT_EQ(frame, (std::vector<std::uint8_t> {1, 2, 3, 4, 5, 6}));
}

TEST(PyrowaveJoinPacketsTest, MovesAPacketThatOverlapsWhereItGoes) {
  // The second packet is longer than the gap before it
  std::vector<std::uint8_t> frame {1, 0, 2, 3, 4, 5};
  const std::array packets {pyrowave_packet {0, 1}, pyrowave_packet {2, 4}};

  pyrowave::join_packets(frame, packets.data(), packets.size());

  EXPECT_EQ(frame, (std::vector<std::uint8_t> {1, 2, 3, 4, 5}));
}

TEST(PyrowaveJoinPacketsTest, LeavesAnEmptyFrameWithoutPackets) {
  std::vector<std::uint8_t> frame {1, 2, 3};

  pyrowave::join_packets(frame, nullptr, 0);

  EXPECT_TRUE(frame.empty());
}

TEST(PyrowavePriorityNameTest, NamesEachPriority) {
  EXPECT_EQ(pyrowave::priority_name(VK_QUEUE_GLOBAL_PRIORITY_LOW), "low");
  EXPECT_EQ(pyrowave::priority_name(VK_QUEUE_GLOBAL_PRIORITY_MEDIUM), "medium");
  EXPECT_EQ(pyrowave::priority_name(VK_QUEUE_GLOBAL_PRIORITY_HIGH), "high");
  EXPECT_EQ(pyrowave::priority_name(VK_QUEUE_GLOBAL_PRIORITY_REALTIME), "realtime");
}

TEST(PyrowavePriorityNameTest, CallsAnythingElseUnknown) {
  EXPECT_EQ(pyrowave::priority_name(static_cast<VkQueueGlobalPriority>(0)), "unknown");
}

TEST(PyrowavePerformanceStatsTest, OnlyAtDebugLevelOrBelow) {
  EXPECT_TRUE(pyrowave::performance_stats_enabled(0));  // verbose
  EXPECT_TRUE(pyrowave::performance_stats_enabled(1));  // debug
  EXPECT_FALSE(pyrowave::performance_stats_enabled(2));  // info
  EXPECT_FALSE(pyrowave::performance_stats_enabled(5));  // fatal
}

TEST(PyrowaveIntervalAverageTest, StartsEmpty) {
  pyrowave::interval_average_t average;
  EXPECT_EQ(average.count(), 0u);
  EXPECT_EQ(average.average_ms(), 0.0);
}

TEST(PyrowaveIntervalAverageTest, AveragesIntervalsInMilliseconds) {
  pyrowave::interval_average_t average;
  // 1 ms and 3 ms at 1 MHz
  average.add(1000, 2000, 1000000);
  average.add(5000, 8000, 1000000);

  EXPECT_EQ(average.count(), 2u);
  EXPECT_DOUBLE_EQ(average.average_ms(), 2.0);
}

TEST(PyrowaveIntervalAverageTest, IgnoresBackwardIntervalsAndZeroFrequency) {
  pyrowave::interval_average_t average;
  average.add(2000, 1000, 1000000);
  average.add(1000, 2000, 0);
  EXPECT_EQ(average.count(), 0u);
}

TEST(PyrowaveIntervalAverageTest, StartsOverAfterAReset) {
  pyrowave::interval_average_t average;
  average.add(0, 1000, 1000000);
  average.reset();

  EXPECT_EQ(average.count(), 0u);
  EXPECT_EQ(average.average_ms(), 0.0);
}
#endif
