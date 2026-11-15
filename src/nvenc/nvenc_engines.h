/**
 * @file src/nvenc/nvenc_engines.h
 * @brief Declarations for remembering how many NVENC units the GPU has.
 */
#pragma once

// standard includes
#include <atomic>

namespace nvenc {

  /**
   * @brief NVENC units the GPU reported when an HEVC or AV1 encoder was last created, or 0 when none has been.
   *
   * @details Every NVENC SDK build of the encoder shares this one count, which the web UI reads to decide
   *          whether to recommend sending frames while they encode: that turns off split-frame encoding,
   *          which only GPUs with more than one unit can use.
   */
  inline std::atomic<int> last_encoder_engine_count {0};

}  // namespace nvenc
