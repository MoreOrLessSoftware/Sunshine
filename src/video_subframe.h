/**
 * @file src/video_subframe.h
 * @brief Declarations for sending a video frame in parts while it is still being encoded.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief Sending a frame in parts as the encoder finishes its slices.
 *
 * @details A frame is normally sent once it is fully encoded. With sub-frame readback, the
 *          encoder hands over each slice as it completes, and groups of slices go out as the
 *          frame's FEC blocks while the rest is still being encoded, so sending overlaps
 *          encoding. The client needs no changes: it already receives frames in up to four FEC
 *          blocks and joins their packets in order. See parts_ready() for where the frame is split.
 *
 *          Every packet names the frame's last FEC block, so a frame sent in parts always has
 *          exactly `blocks` of them. Early blocks carry whole packets only and leave any
 *          remainder for the next block, so zero padding never lands inside the bitstream; the
 *          final block pads the frame's last packet as usual, and any block left with no data
 *          is a single packet of zeros, which H.264 and HEVC decoders ignore after the frame.
 */
namespace video::subframe {
  constexpr std::uint32_t blocks = 4;  ///< FEC blocks a frame sent in parts is split into, the protocol's maximum.
  constexpr std::uint32_t min_slices = 8;  ///< Slices a frame sent in parts is encoded in at least.
  constexpr std::size_t max_block_packets = 1023;  ///< Most packets a FEC block can have, set by the 10-bit packet index.

  /**
   * @brief Work out after how many slices each early part of a frame is sent.
   *
   * @details The last part can only go out once the frame finishes, so whatever it carries is
   *          sent after all of the encoding. The third part therefore goes once all but the last
   *          slice are done, leaving one slice for the end; the first goes as soon as an eighth
   *          of the frame is done, so sending starts early, and the second halfway. With 8
   *          slices that is after slices 1, 4 and 7, with 4 slices after 1, 2 and 3.
   *
   * @param slices_total Slices in the frame.
   * @return Slices done before each early part is sent.
   */
  inline std::array<std::uint32_t, blocks - 1> part_slices(std::uint32_t slices_total) {
    const auto first = std::max<std::uint32_t>(1, slices_total / 8);
    const auto second = std::max(first + 1, slices_total / 2);
    const auto third = std::max(second + 1, slices_total - 1);
    return {first, second, third};
  }

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

    std::uint32_t parts = 0;
    for (auto slices : part_slices(slices_total)) {
      if (slices_done >= slices) {
        ++parts;
      }
    }
    return parts;
  }

  /**
   * @brief Work out how much of the data waiting to be sent goes out in an early FEC block.
   *
   * @details A block keeps its FEC while it fits, carrying the rest to the next block. A part
   *          more than twice what fits would pile up in the frame's last block, which cannot grow
   *          past `max_block_packets`, so it goes out whole without FEC instead, as a whole frame
   *          too large for FEC does. That happens for large IDR frames.
   *
   * @param pending Bytes waiting to be sent.
   * @param packet_payload Bytes of data each packet carries.
   * @param max_fec_packets Most packets a block can have with FEC.
   * @return Bytes to send: whole packets only, or 0 if there is not a whole packet yet.
   */
  inline std::size_t early_block_bytes(std::size_t pending, std::size_t packet_payload, std::size_t max_fec_packets) {
    if (packet_payload == 0) {
      return 0;
    }

    const auto packets = pending / packet_payload;
    const auto limit = packets <= 2 * max_fec_packets ? max_fec_packets : max_block_packets;
    return std::min(packets, limit) * packet_payload;
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

  /**
   * @brief Estimate a frame's processing latency when its first part is sent.
   *
   * @details The latency the client shows comes from the frame header, which goes out with
   *          the first part, before the frame has finished encoding. Encoding the rest takes
   *          about as long from one frame to the next, so the previous frame's time from its
   *          first part to its last stands in for it.
   *
   * @param since_capture Time from capture to sending the first part.
   * @param last_remaining The previous frame's time from its first part to its last, or zero.
   * @return Estimated time from capture until the whole frame is encoded.
   */
  inline std::chrono::steady_clock::duration estimated_latency(std::chrono::steady_clock::duration since_capture, std::chrono::steady_clock::duration last_remaining) {
    return since_capture + std::max(last_remaining, std::chrono::steady_clock::duration::zero());
  }
}  // namespace video::subframe
