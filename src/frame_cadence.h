/**
 * @file src/frame_cadence.h
 * @brief Declarations for evening out a repeating uneven cadence in captured frames' timestamps.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace platf {
  /**
   * @brief Stamps captured frames evenly when they are presented in a repeating uneven pattern.
   *
   * @details Frame generation presents a rendered frame and the frames generated for it as a
   *          batch. Paced by the game, the frames of a batch come unevenly, such as 7 ms then 9 ms
   *          over and over at twice the rendered frame rate, and a client that shows frames by
   *          their timestamps shows that as judder. This looks for such a pattern, of 2 to 4
   *          frames, in the last 8 batches, and while there is one, stamps each frame where it
   *          would be if the pattern's frames were evenly spaced.
   *
   *          A frame is never stamped later than it was presented, and never earlier by more than
   *          three tenths of the average frame interval. Frames are stamped as presented whenever
   *          no pattern is found: when the cadence is even already, when it is irregular, when the
   *          pattern is too lopsided to be pacing error, and after a frame was missed or a pause.
   */
  class frame_cadence_smoother_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Clock that frames are stamped on.

    static constexpr std::size_t max_pattern = 4;  ///< Longest pattern looked for, in frames.
    static constexpr std::size_t batches = 8;  ///< Repeats of a pattern needed to follow it.
    static constexpr double min_imbalance = 0.05;  ///< Smallest unevenness worth evening out, as a fraction of the average frame interval.
    static constexpr double max_imbalance = 0.3;  ///< Largest unevenness taken to be pacing error, as a fraction of the average frame interval.
    static constexpr double max_scatter = 0.5;  ///< Most the intervals may vary around the pattern, relative to how much the pattern itself varies.
    static constexpr std::chrono::milliseconds max_gap {250};  ///< A longer wait between frames starts the search over.

    /**
     * @brief Get the timestamp for a newly presented frame.
     *
     * @param present When the frame was presented.
     * @param presents Presents since the frame before this one, this frame's included. More than
     *                 1 means a frame was missed, which starts the search for a pattern over.
     * @return The frame's timestamp: `present`, or up to three tenths of a frame interval before it.
     */
    clock::time_point stamp(clock::time_point present, std::uint32_t presents = 1) {
      if (!last_present || presents > 1 || present <= *last_present || present - *last_present > max_gap) {
        count = 0;
      } else {
        if (count == intervals.size()) {
          std::copy(intervals.begin() + 1, intervals.end(), intervals.begin());
          --count;
        }
        intervals[count++] = std::chrono::duration<double, std::micro>(present - *last_present).count();
      }
      last_present = present;

      const auto shifted = present + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::micro>(correction()));
      last_stamp = std::max(shifted, last_stamp);
      return last_stamp;
    }

    /**
     * @brief Get the length of the pattern the last frame was stamped by.
     *
     * @return The pattern's length in frames, or 0 if the frame was stamped as presented.
     */
    std::size_t pattern() const {
      return pattern_frames;
    }

  private:
    /**
     * @brief Work out how far to move the newest frame's timestamp.
     *
     * @return Microseconds to add to the frame's present time, 0 or less. Also sets `pattern_frames`.
     */
    double correction() {
      for (std::size_t frames = 2; frames <= max_pattern; ++frames) {
        if (const auto shift = correction_for(frames)) {
          pattern_frames = frames;
          return *shift;
        }
      }
      pattern_frames = 0;
      return 0.0;
    }

    /**
     * @brief Work out how far to move the newest frame's timestamp for a pattern of a given length.
     *
     * @param frames Length of the pattern to look for.
     * @return Microseconds to add to the frame's present time, 0 or less, or nothing if the
     *         intervals don't follow a pattern of this length that should be evened out.
     */
    std::optional<double> correction_for(std::size_t frames) const {
      const auto window = frames * batches;
      if (count < window) {
        return std::nullopt;
      }

      // Average each interval of the pattern, counting back from the newest
      std::array<double, max_pattern> pattern_interval {};
      for (std::size_t back = 0; back < window; ++back) {
        pattern_interval[back % frames] += intervals[count - 1 - back] / batches;
      }
      double average = 0;
      for (std::size_t i = 0; i < frames; ++i) {
        average += pattern_interval[i] / frames;
      }

      // The intervals must keep to the pattern much more closely than the pattern varies
      double scatter = 0;
      double variation = 0;
      for (std::size_t back = 0; back < window; ++back) {
        const auto off_pattern = intervals[count - 1 - back] - pattern_interval[back % frames];
        const auto off_average = pattern_interval[back % frames] - average;
        scatter += off_pattern * off_pattern;
        variation += off_average * off_average;
      }
      if (scatter > max_scatter * variation) {
        return std::nullopt;
      }

      // How far each frame of the pattern is ahead of where even spacing would put it, relative
      // to the newest frame
      double earliest = 0;
      double latest = 0;
      double behind_newest = 0;
      for (std::size_t back = 1; back < frames; ++back) {
        behind_newest += pattern_interval[back - 1];
        const auto ahead = static_cast<double>(back) * average - behind_newest;
        earliest = std::min(earliest, ahead);
        latest = std::max(latest, ahead);
      }
      const auto imbalance = latest - earliest;
      if (imbalance < min_imbalance * average || imbalance > max_imbalance * average) {
        return std::nullopt;
      }

      // Line the pattern up on its latest frame, so no frame is stamped after its present
      return earliest;
    }

    std::array<double, max_pattern * batches> intervals {};  ///< Intervals between the last frames in microseconds, oldest first.
    std::size_t count = 0;  ///< Intervals held.
    std::size_t pattern_frames = 0;  ///< Length of the pattern the last frame was stamped by, or 0.
    std::optional<clock::time_point> last_present;  ///< When the last frame was presented.
    clock::time_point last_stamp {};  ///< Timestamp given to the last frame.
  };
}  // namespace platf
