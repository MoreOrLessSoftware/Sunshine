/**
 * @file src/frame_rate_limiter.h
 * @brief Declarations for the limit on how often captured frames are taken.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <optional>

namespace platf {
  /**
   * @brief Limits captured frames to the client's frame rate without delaying frames that
   *        arrive at or below it.
   *
   * @details Frames are let through on arrival as long as they come no faster than the frame
   *          rate on average. A frame that comes early is held until it is due, and a newer one
   *          arriving meanwhile replaces it. Arrival jitter of up to half a frame interval never
   *          holds a frame.
   *
   *          A source can be allowed to run slightly faster than the frame rate. Held to exactly
   *          the frame rate, a display whose clock runs a little fast builds up a delay of up to
   *          a whole frame interval, dropping a frame each time it wraps around. Allowed to run
   *          faster, a source at twice the frame rate lets an extra frame through each time the
   *          allowance adds up to half an interval, so that is only for displays that cannot
   *          run much faster than the frame rate.
   */
  class frame_rate_limiter_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Clock the limiter works in.

    /**
     * @brief Create a limiter for a frame rate.
     *
     * @param frame_interval Interval between frames at the client's frame rate.
     * @param allow_fast_clock Whether to let a source up to 1% faster than the frame rate through
     *                         unheld.
     */
    frame_rate_limiter_t(clock::duration frame_interval, bool allow_fast_clock):
        min_interval {allow_fast_clock ? frame_interval * 100 / 101 : frame_interval},
        allowance {frame_interval / 2} {
    }

    /**
     * @brief Get the earliest time the next frame may be taken.
     *
     * @return Time the next frame is due, in the past if it may be taken now.
     */
    clock::time_point next_due() const {
      return theoretical_arrival - allowance;
    }

    /**
     * @brief Record that a frame was taken.
     *
     * @param now When the frame was taken.
     */
    void frame_taken(clock::time_point now) {
      theoretical_arrival = std::max(theoretical_arrival, now) + min_interval;
    }

  private:
    clock::duration min_interval;  ///< Average interval frames are held to.
    clock::duration allowance;  ///< How far ahead of that average a frame may still be taken.
    clock::time_point theoretical_arrival {};  ///< When the next frame would be due at exactly the frame rate.
  };

  /**
   * @brief Work out until when a cursor update that came without a new frame waits for one.
   *
   * @details On its own, a cursor update becomes a frame, so the cursor moves over a still
   *          desktop. While frames keep coming it is left for the next one to draw instead: as
   *          frames of their own, cursor moves slipped extra frames in between a game's, and a VRR
   *          client showed a stutter each time. It waits until two frame intervals after the last
   *          frame, so a game running below the stream's frame rate still has its cursor moves
   *          held back, and goes out on its own if no frame has come by then, so the cursor never
   *          stays behind on a screen that stopped changing.
   *
   * @param last_frame_presented When the last new frame was presented, if there has been one.
   * @param frame_rate The stream's frame rate.
   * @return When the cursor update stops waiting, or nothing if it should not wait at all.
   */
  inline std::optional<std::chrono::steady_clock::time_point> cursor_hold_deadline(std::optional<std::chrono::steady_clock::time_point> last_frame_presented, int frame_rate) {
    if (!last_frame_presented || frame_rate <= 0) {
      return std::nullopt;
    }

    return *last_frame_presented + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(std::chrono::seconds(2)) / frame_rate);
  }

  /**
   * @brief Decide whether a cursor update that came without a new frame waits for the next frame.
   *
   * @param last_frame_presented When the last new frame was presented, if there has been one.
   * @param now The current time.
   * @param frame_rate The stream's frame rate.
   * @return Whether to hold the cursor update back. See cursor_hold_deadline().
   */
  inline bool hold_back_cursor_update(std::optional<std::chrono::steady_clock::time_point> last_frame_presented, std::chrono::steady_clock::time_point now, int frame_rate) {
    const auto deadline = cursor_hold_deadline(last_frame_presented, frame_rate);
    return deadline && now < *deadline;
  }
}  // namespace platf
