/**
 * @file tests/unit/test_pyrowave_records.cpp
 * @brief Test src/pyrowave_records.h.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

// local includes
#include <src/pyrowave_records.h>

namespace {
  namespace records = pyrowave::records;

  constexpr std::size_t payload = 1392 - 16;  ///< Frame bytes per packet for the usual packet size.
  constexpr std::uint32_t width = 1920;  ///< Width of the test frames.
  constexpr std::uint32_t height = 1080;  ///< Height of the test frames.

  /**
   * @brief Append a little-endian word.
   *
   * @param out Bytes to append to.
   * @param value Word to append.
   */
  void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int i = 0; i < 4; i++) {
      out.push_back(std::uint8_t(value >> (8 * i)));
    }
  }

  /**
   * @brief Read a little-endian word.
   *
   * @param p Where it is.
   * @return The word.
   */
  std::uint32_t read_u32(const std::uint8_t *p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
  }

  /**
   * @brief Make a frame as PyroWave encodes it: the sequence header, then blocks by index.
   *
   * @param sizes Size of each block in bytes, a multiple of four and at least 8.
   * @return The frame.
   */
  std::vector<std::uint8_t> make_frame(const std::vector<std::size_t> &sizes) {
    std::vector<std::uint8_t> frame;
    append_u32(frame, 0x80000000u | (width - 1) | ((height - 1) << 14));
    append_u32(frame, std::uint32_t(sizes.size()));
    for (std::uint32_t i = 0; i < sizes.size(); i++) {
      append_u32(frame, std::uint32_t(sizes[i] / 4) << 16);
      append_u32(frame, i << 8);
      for (std::size_t w = 2; w < sizes[i] / 4; w++) {
        append_u32(frame, i * 7919 + std::uint32_t(w));
      }
    }
    return frame;
  }

  /**
   * @brief Block sizes like a real frame's: mostly small, some spanning packets.
   *
   * @param count Blocks.
   * @param seed Random seed.
   * @return Sizes in bytes.
   */
  std::vector<std::size_t> block_sizes(std::size_t count, unsigned seed) {
    std::mt19937 rng {seed};
    std::vector<std::size_t> sizes;
    for (std::size_t i = 0; i < count; i++) {
      const auto words = i % 37 == 0 ? 360 + rng() % 600 : 2 + rng() % 150;
      sizes.push_back(words * 4);
    }
    return sizes;
  }

  /**
   * @brief A record of a laid out frame.
   */
  struct record_t {
    std::size_t offset;  ///< Where it starts in the frame.
    std::size_t size;  ///< Its size in bytes.
    bool padding;  ///< Whether it's a padding record.
    std::uint32_t block_index;  ///< Its block index, for a block.
  };

  /**
   * @brief Walk a laid out frame's records, after its sequence header.
   *
   * @param frame The frame.
   * @return Its records, in order.
   */
  std::vector<record_t> walk(const std::vector<std::uint8_t> &frame) {
    std::vector<record_t> out;
    for (std::size_t position = records::header_bytes; position < frame.size();) {
      const auto word0 = read_u32(frame.data() + position);
      if (word0 == records::padding_magic) {
        const auto size = records::header_bytes + std::size_t(read_u32(frame.data() + position + 4)) * 4;
        out.push_back({position, size, true, 0});
      } else {
        out.push_back({position, std::size_t((word0 >> 16) & 0xfff) * 4, false, read_u32(frame.data() + position + 4) >> 8});
      }
      position += out.back().size;
    }
    return out;
  }
}  // namespace

TEST(PyroWaveRecordsTest, LaysRecordsOutOnlyOnPacketsTheyLineUpWith) {
  EXPECT_EQ(records::packet_payload_bytes(1392), 1376u);
  EXPECT_EQ(records::packet_payload_bytes(1390), 0u);
  EXPECT_EQ(records::packet_payload_bytes(16), 0u);
  EXPECT_EQ(records::packet_payload_bytes(32), 0u);
}

TEST(PyroWaveRecordsTest, CountsTheCoarsestBlocks) {
  EXPECT_EQ(records::coarse_block_count(1920, 1080), 48u);
  EXPECT_EQ(records::coarse_block_count(64, 64), 12u);
  EXPECT_EQ(records::coarse_block_count(3840, 2160), 144u);
}

TEST(PyroWaveRecordsTest, RejectsWhatIsNotAFrame) {
  std::vector<std::uint8_t> out;
  records::layout_t layout;
  const std::vector<std::uint8_t> no_sequence_header(16, 0);
  EXPECT_FALSE(records::write_record_frame(no_sequence_header, payload, out, layout));

  auto truncated = make_frame({64, 64});
  truncated.resize(truncated.size() - 4);
  EXPECT_FALSE(records::write_record_frame(truncated, payload, out, layout));
}

TEST(PyroWaveRecordsTest, KeepsEveryBlockAndLinesThemUpWithPackets) {
  const auto sizes = block_sizes(1000, 1);
  const auto frame = make_frame(sizes);
  std::vector<std::uint8_t> out;
  records::layout_t layout;
  ASSERT_TRUE(records::write_record_frame(frame, payload, out, layout));

  // The sequence header first, then every block once, as encoded
  ASSERT_TRUE(std::equal(frame.begin(), frame.begin() + records::header_bytes, out.begin()));
  std::vector<bool> seen(sizes.size(), false);
  std::size_t padding = 0;
  bool fine_seen = false;
  const auto coarse = records::coarse_block_count(width, height);
  for (const auto &record : walk(out)) {
    const auto first_packet = (record.offset + records::frame_header_bytes) / payload;
    const auto last_packet = (record.offset + record.size - 1 + records::frame_header_bytes) / payload;
    if (record.padding) {
      padding += record.size;
      EXPECT_EQ(first_packet, last_packet) << "padding crosses a packet boundary";
      continue;
    }
    ASSERT_LT(record.block_index, sizes.size());
    EXPECT_FALSE(seen[record.block_index]);
    seen[record.block_index] = true;
    EXPECT_EQ(record.size, sizes[record.block_index]);
    if (record.size + records::header_bytes <= payload) {
      EXPECT_EQ(first_packet, last_packet) << "block " << record.block_index << " crosses a packet boundary";
    }

    // The coarsest level first, through the critical bytes
    if (record.block_index < coarse) {
      EXPECT_FALSE(fine_seen);
      EXPECT_LE(record.offset + record.size, layout.critical_bytes);
    } else {
      fine_seen = true;
      EXPECT_GE(record.offset, layout.critical_bytes);
    }
  }
  EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [](bool b) {
    return b;
  }));
  EXPECT_EQ(padding, layout.padding_bytes);
  EXPECT_EQ(out.size(), frame.size() + layout.padding_bytes);

  // Packing first-fit leaves little to pad (about 1% with these sizes, against about 20% when
  // records keep their order)
  EXPECT_LT(layout.padding_bytes * 50, frame.size());
}

TEST(PyroWaveRecordsTest, FlagsThePacketsThatStartWithARecord) {
  const auto frame = make_frame(block_sizes(1000, 2));
  std::vector<std::uint8_t> out;
  records::layout_t layout;
  ASSERT_TRUE(records::write_record_frame(frame, payload, out, layout));

  const auto starts = records::record_start_packets(out, payload);
  ASSERT_EQ(starts.size(), records::packets_for(out.size(), payload));
  EXPECT_TRUE(starts[0]);

  // Each packet that starts with a record is flagged, which is most of them: all but those
  // inside a record too large for one packet
  std::vector<bool> expected(starts.size(), false);
  expected[0] = true;
  for (const auto &record : walk(out)) {
    if ((record.offset + records::frame_header_bytes) % payload == 0) {
      expected[(record.offset + records::frame_header_bytes) / payload] = true;
    }
  }
  EXPECT_EQ(starts, expected);
  EXPECT_GT(std::count(starts.begin(), starts.end(), true) * 4, (std::ptrdiff_t) starts.size() * 3);
}

TEST(PyroWaveRecordsTest, FindsNoRecordStartsInWhatIsNotAFrame) {
  const std::vector<std::uint8_t> zeros(64, 0);
  EXPECT_TRUE(records::record_start_packets(zeros, payload).empty());
  EXPECT_TRUE(records::record_start_packets({}, payload).empty());
}

namespace {
  /**
   * @brief Describe FEC blocks for comparing them.
   *
   * @param blocks The blocks.
   * @return Each block's packets and percentage.
   */
  std::vector<std::pair<std::size_t, std::size_t>> describe(const std::vector<records::fec_block_t> &blocks) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (const auto &block : blocks) {
      out.emplace_back(block.packets, block.fec_percentage);
    }
    return out;
  }
}  // namespace

TEST(PyroWaveRecordsTest, LeavesFecAsItIsWhenItIsOffOrNothingIsCritical) {
  EXPECT_TRUE(records::critical_fec_blocks(250, 15, 0, 2).empty());
  EXPECT_TRUE(records::critical_fec_blocks(250, 0, 20, 2).empty());
  EXPECT_TRUE(records::critical_fec_blocks(10, 15, 20, 2).empty());
}

TEST(PyroWaveRecordsTest, GivesTheCriticalPacketsAllTheParity) {
  using blocks_t = std::vector<std::pair<std::size_t, std::size_t>>;
  EXPECT_EQ(describe(records::critical_fec_blocks(250, 15, 20, 2)), (blocks_t {{15, 20}, {235, 0}}));
  EXPECT_EQ(describe(records::critical_fec_blocks(15, 15, 20, 2)), (blocks_t {{15, 20}}));

  // The rest is split into blocks of a Reed-Solomon block's size where it can be
  EXPECT_EQ(describe(records::critical_fec_blocks(15 + 700, 15, 20, 2)), (blocks_t {{15, 20}, {234, 0}, {234, 0}, {232, 0}}));
  EXPECT_EQ(describe(records::critical_fec_blocks(15 + 2000, 15, 20, 2)), (blocks_t {{15, 20}, {667, 0}, {667, 0}, {666, 0}}));
}

TEST(PyroWaveRecordsTest, LeavesFecAsItIsWhenTheCriticalBlockDoesNotFit) {
  // 240 packets need 48 parity packets at 20%, over a Reed-Solomon block's 255
  EXPECT_TRUE(records::critical_fec_blocks(400, 240, 20, 2).empty());
  // The client's minimum of parity packets counts too
  EXPECT_EQ(records::critical_fec_blocks(400, 250, 1, 2).size(), 2u);
  EXPECT_TRUE(records::critical_fec_blocks(400, 250, 1, 6).empty());
  // The rest must fit three blocks
  EXPECT_TRUE(records::critical_fec_blocks(15 + 3 * 1023 + 1, 15, 20, 2).empty());
}
