/**
 * @file src/nvenc/nvenc_encoded_frame.h
 * @brief Declarations for NVENC encoded frame.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <vector>

namespace nvenc {

  /**
   * @brief Encoded NVENC output frame and metadata needed by the packetizer.
   */
  struct nvenc_encoded_frame {
    std::vector<uint8_t> data;  ///< Encoded bitstream bytes returned by NVENC.
    uint64_t frame_index = 0;  ///< Capture-frame index associated with the encoded data.
    bool idr = false;  ///< Whether the encoded frame is an IDR frame.
    bool after_ref_frame_invalidation = false;  ///< Whether the frame follows reference-frame invalidation.
  };

  /**
   * @brief Called while a frame is encoded in slices, each time more of its slices are done.
   *
   * @details `data` holds the frame's bitstream so far, which only ever grows, `slices_done` of its
   *          `slices_total` slices are encoded, and `after_ref_frame_invalidation` tells whether the
   *          frame follows reference-frame invalidation. It is never called for IDR frames.
   */
  using nvenc_subframe_callback = std::function<void(const std::vector<uint8_t> &data, uint32_t slices_done, uint32_t slices_total, bool after_ref_frame_invalidation)>;

}  // namespace nvenc
