/**
 * @file tests/unit/test_video_subframe.cpp
 * @brief Test src/video_subframe.h.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <numeric>
#include <vector>

// local includes
#include <src/video_subframe.h>

namespace {
  constexpr std::size_t packet_payload = 1000;  ///< Bytes of data per packet in these tests.
  constexpr std::size_t max_packets = 200;  ///< Most packets a block may have with FEC in these tests.

  /**
   * @brief Send a frame whose slices finish one group at a time, the way the video broadcast thread does.
   *
   * @param group_sizes Bytes each quarter of the frame's slices adds, the first including the frame header.
   * @return Bytes of each FEC block sent, in order.
   */
  std::vector<std::size_t> send_frame(const std::vector<std::size_t> &group_sizes) {
    std::vector<std::size_t> blocks;
    std::size_t pending = 0;

    for (std::size_t group = 0; group < group_sizes.size(); ++group) {
      pending += group_sizes[group];

      if (group + 1 < group_sizes.size()) {
        if (blocks.size() >= video::subframe::blocks - 1) {
          continue;
        }

        auto bytes = video::subframe::early_block_bytes(pending, packet_payload, max_packets);
        if (bytes > 0) {
          blocks.push_back(bytes);
          pending -= bytes;
        }
      } else {
        for (auto bytes : video::subframe::final_block_bytes(pending, packet_payload, video::subframe::blocks - blocks.size())) {
          blocks.push_back(bytes);
        }
      }
    }

    return blocks;
  }
}  // namespace

TEST(VideoSubframeTest, ReportsPartsAsQuartersOfTheSlicesFinish) {
  EXPECT_EQ(video::subframe::parts_ready(0, 4), 0u);
  EXPECT_EQ(video::subframe::parts_ready(1, 4), 1u);
  EXPECT_EQ(video::subframe::parts_ready(2, 4), 2u);
  EXPECT_EQ(video::subframe::parts_ready(3, 4), 3u);
  EXPECT_EQ(video::subframe::parts_ready(3, 8), 1u);
  EXPECT_EQ(video::subframe::parts_ready(6, 8), 3u);
}

TEST(VideoSubframeTest, KeepsTheLastPartForWhenTheFrameFinishes) {
  EXPECT_EQ(video::subframe::parts_ready(4, 4), video::subframe::blocks - 1);
  EXPECT_EQ(video::subframe::parts_ready(8, 8), video::subframe::blocks - 1);
}

TEST(VideoSubframeTest, ReportsNoPartsWithoutSlices) {
  EXPECT_EQ(video::subframe::parts_ready(3, 0), 0u);
}

TEST(VideoSubframeTest, SendsOnlyWholePacketsEarly) {
  EXPECT_EQ(video::subframe::early_block_bytes(999, packet_payload, max_packets), 0u);
  EXPECT_EQ(video::subframe::early_block_bytes(1000, packet_payload, max_packets), 1000u);
  EXPECT_EQ(video::subframe::early_block_bytes(2500, packet_payload, max_packets), 2000u);
}

TEST(VideoSubframeTest, KeepsEarlyBlocksSmallEnoughForFec) {
  EXPECT_EQ(video::subframe::early_block_bytes(500'000, packet_payload, max_packets), max_packets * packet_payload);
}

TEST(VideoSubframeTest, SplitsTheRestOfAFrameEvenlyByPacket) {
  // 11 packets over 3 blocks
  const auto sizes = video::subframe::final_block_bytes(10'001, packet_payload, 3);

  ASSERT_EQ(sizes.size(), 3u);
  EXPECT_EQ(sizes[0], 4000u);
  EXPECT_EQ(sizes[1], 4000u);
  EXPECT_EQ(sizes[2], 2001u);
}

TEST(VideoSubframeTest, LeavesBlocksWithNothingToCarryEmpty) {
  EXPECT_EQ(video::subframe::final_block_bytes(1, packet_payload, 3), (std::vector<std::size_t> {1, 0, 0}));
  EXPECT_EQ(video::subframe::final_block_bytes(0, packet_payload, 2), (std::vector<std::size_t> {0, 0}));
}

TEST(VideoSubframeTest, ReturnsNoBlocksWhenNoneAreLeft) {
  EXPECT_TRUE(video::subframe::final_block_bytes(5000, packet_payload, 0).empty());
}

TEST(VideoSubframeTest, AlwaysSendsFourBlocksOnceSendingStartsEarly) {
  EXPECT_EQ(send_frame({30'000, 30'000, 30'000, 30'000}).size(), video::subframe::blocks);
  EXPECT_EQ(send_frame({1'500, 200, 200, 200}).size(), video::subframe::blocks);
}

TEST(VideoSubframeTest, SendsEveryByteOfTheFrameInOrder) {
  const std::vector<std::size_t> groups {12'345, 678, 40'001, 9'999};
  const auto blocks = send_frame(groups);

  EXPECT_EQ(std::accumulate(blocks.begin(), blocks.end(), std::size_t {0}), std::accumulate(groups.begin(), groups.end(), std::size_t {0}));
}

TEST(VideoSubframeTest, PadsOnlyAfterTheEndOfTheFrame) {
  // Zero padding goes at the end of each block's last packet, so every block before the last one
  // carrying data must fill its packets exactly
  for (const auto &groups : std::vector<std::vector<std::size_t>> {
         {12'345, 678, 40'001, 9'999},
         {1'500, 200, 200, 200},
         {999, 999, 999, 999},
         {250'000, 250'000, 250'000, 250'000},
       }) {
    const auto blocks = send_frame(groups);

    std::size_t last_with_data = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i] > 0) {
        last_with_data = i;
      }
    }

    for (std::size_t i = 0; i < last_with_data; ++i) {
      EXPECT_EQ(blocks[i] % packet_payload, 0u) << "block " << i;
      EXPECT_GT(blocks[i], 0u) << "block " << i;
    }
  }
}
