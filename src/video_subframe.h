/**
 * @file src/video_subframe.h
 * @brief Declarations for sending a video frame in parts while it is still being encoded.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief Sending a frame in parts as the encoder finishes its slices.
 *
 * @details A frame is normally sent once it is fully encoded. With sub-frame readback, the
 *          encoder hands over each slice as it completes, and each quarter of the slices goes
 *          out as one of the frame's FEC blocks while the rest is still being encoded, so
 *          sending overlaps encoding. The client needs no changes: it already receives frames
 *          in up to four FEC blocks and joins their packets in order.
 *
 *          Every packet names the frame's last FEC block, so a frame sent in parts always has
 *          exactly `blocks` of them. Early blocks carry whole packets only and leave any
 *          remainder for the next block, so zero padding never lands inside the bitstream; the
 *          final block pads the frame's last packet as usual, and any block left with no data
 *          is a single packet of zeros, which H.264 and HEVC decoders ignore after the frame.
 */
namespace video::subframe {
  constexpr std::uint32_t blocks = 4;  ///< FEC blocks a frame sent in parts is split into, the protocol's maximum.
  constexpr std::uint32_t min_slices = blocks;  ///< Fewest slices a frame needs to be sent in parts.

  /**
   * @brief Work out how many parts of a frame can be sent before it finishes encoding.
   *
   * @param slices_done Slices encoded so far.
   * @param slices_total Slices in the frame.
   * @return Parts ready to send, at most one fewer than `blocks`: the last part is sent when
   *         the frame finishes.
   */
  inline std::uint32_t parts_ready(std::uint32_t slices_done, std::uint32_t slices_total) {
    if (slices_total == 0) {
      return 0;
    }

    return std::min<std::uint64_t>((std::uint64_t) slices_done * blocks / slices_total, blocks - 1);
  }

  /**
   * @brief Work out how much of the data waiting to be sent goes out in an early FEC block.
   *
   * @param pending Bytes waiting to be sent.
   * @param packet_payload Bytes of data each packet carries.
   * @param max_packets Most packets a block can have with FEC.
   * @return Bytes to send: whole packets only, at most `max_packets`, or 0 if there is not
   *         a whole packet yet.
   */
  inline std::size_t early_block_bytes(std::size_t pending, std::size_t packet_payload, std::size_t max_packets) {
    if (packet_payload == 0) {
      return 0;
    }

    return std::min(pending / packet_payload, max_packets) * packet_payload;
  }

  /**
   * @brief Split the rest of a frame into the FEC blocks it has left.
   *
   * @param pending Bytes left to send.
   * @param packet_payload Bytes of data each packet carries.
   * @param blocks_left FEC blocks left to send.
   * @return Bytes for each block, as even as whole packets allow and in order. A block given 0
   *         bytes is sent as a single packet of zeros.
   */
  inline std::vector<std::size_t> final_block_bytes(std::size_t pending, std::size_t packet_payload, std::size_t blocks_left) {
    std::vector<std::size_t> sizes(blocks_left, 0);
    if (blocks_left == 0 || packet_payload == 0) {
      return sizes;
    }

    const auto packets = (pending + packet_payload - 1) / packet_payload;
    const auto packets_per_block = (packets + blocks_left - 1) / blocks_left;

    for (auto &size : sizes) {
      size = std::min(pending, packets_per_block * packet_payload);
      pending -= size;
    }

    return sizes;
  }
}  // namespace video::subframe
