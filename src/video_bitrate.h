/**
 * @file src/video_bitrate.h
 * @brief Declarations for raising the bitrate when frames come slower than the stream's frame rate.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>

/**
 * @brief Keeping the stream at its bitrate when the game renders fewer frames than the stream's frame rate.
 *
 * @details Encoders are given the bitrate the client asked for, and work out how much each frame
 *          may use from the stream's frame rate. A game that renders 60 frames a second in a 120
 *          frame stream gets half the bitrate. Measuring how often the game produces frames, and
 *          raising the bitrate by the same factor, lets each frame use what the missing ones
 *          would have: the stream keeps to the client's bitrate on average.
 *
 *          The rate is measured over a second, so the boost grows slowly, but also over the last
 *          few frames, so it shrinks as soon as frames come faster again. Frames twice the size
 *          at twice the rate would otherwise go out until the slower measurement caught up.
 */
namespace video::bitrate {
  using frame_clock = std::chrono::steady_clock;  ///< Clock that frame times are measured on.

  /**
   * @brief Largest frame a boost makes room for, in bytes.
   *
   * A frame is sent in at most 4 FEC blocks of 1023 packets. With small packets that is
   * about 4 MB; this leaves room for the encoder going over its budget and for key frames.
   */
  constexpr std::size_t max_boosted_frame_bytes = 2'000'000;

  constexpr auto rate_window = std::chrono::seconds {1};  ///< How far back the slower measurement of the frame rate looks.
  constexpr std::size_t recent_intervals = 3;  ///< How many of the latest frame intervals the faster measurement uses.
  constexpr std::size_t median_intervals = 8;  ///< How many of the latest frame intervals the median measurement uses.
  constexpr double send_time_share = 0.85;  ///< Share of the game's frame interval a boosted frame may take to send, leaving room for the frame to come out late.
  constexpr double change_threshold = 0.1;  ///< How far, as a fraction, the bitrate must rise before the encoder is changed. Lowering it always goes through.

  /**
   * @brief Measures how often the game produces frames, as seen by the encoder.
   *
   * @details The encoder records each frame it encodes, with how many frames were captured
   *          since the last one. When the encoder falls behind, captured frames are replaced
   *          before it gets to them; counting those keeps the measured rate at the game's rate.
   *          Counting only encoded frames, a slow encode would look like a slow game, the
   *          bitrate would go up, and the bigger frames would keep the encoder slow.
   */
  class frame_rate_estimator {
  public:
    /**
     * @brief Record that a frame was encoded.
     *
     * @param when When the frame was encoded.
     * @param frames How many frames this one stands for: the frames captured since the last
     *               encoded frame, or 1 for a repeated frame.
     */
    void add_frame(frame_clock::time_point when, std::size_t frames = 1) {
      total += std::max<std::size_t>(frames, 1);
      samples.push_back({when, total});

      // The latest intervals are kept however old they are, so after a pause the rate is
      // measured across it
      while (samples.size() > std::max(recent_intervals, median_intervals) + 1 && when - samples.front().when > rate_window) {
        samples.pop_front();
      }
    }

    /**
     * @brief Estimate the frame rate.
     *
     * @details Three measurements are taken, and the highest is used, so the rate is never
     *          underestimated: the rate over the last second, the rate over the last few frames,
     *          which rises as soon as frames come faster, and the rate from the median of the
     *          last several frame intervals, which a single pause doesn't move. Without the
     *          median, a game that hitches for a moment would look slow, and the first frame
     *          after the hitch would be boosted while frames were already back at full rate.
     *
     * @return The frame rate in frames per second, or nothing until two frames have been recorded.
     */
    std::optional<double> fps() const {
      if (samples.size() < 2) {
        return std::nullopt;
      }

      const auto rate = [](const sample_t &first, const sample_t &last) {
        const std::chrono::duration<double> span = last.when - first.when;
        return span.count() > 0.0 ? static_cast<double>(last.total - first.total) / span.count() : 0.0;
      };

      auto estimate = rate(samples.front(), samples.back());
      if (samples.size() > recent_intervals) {
        estimate = std::max(estimate, rate(samples[samples.size() - 1 - recent_intervals], samples.back()));
      }
      if (samples.size() > median_intervals) {
        estimate = std::max(estimate, median_rate());
      }

      if (estimate <= 0.0) {
        return std::nullopt;
      }
      return estimate;
    }

  private:
    /**
     * @brief Work out the frame rate from the median of the latest frame intervals.
     *
     * @return Frames per second, or 0 if the median interval is zero. Needs more than
     *         `median_intervals` samples.
     */
    double median_rate() const {
      std::array<double, median_intervals> intervals {};
      for (std::size_t i = 0; i < median_intervals; ++i) {
        const auto &earlier = samples[samples.size() - 2 - i];
        const auto &later = samples[samples.size() - 1 - i];
        const std::chrono::duration<double> span = later.when - earlier.when;
        intervals[i] = span.count() / static_cast<double>(later.total - earlier.total);
      }

      std::sort(intervals.begin(), intervals.end());
      const auto median = (intervals[median_intervals / 2 - 1] + intervals[median_intervals / 2]) / 2.0;
      return median > 0.0 ? 1.0 / median : 0.0;
    }

    /**
     * @brief One encoded frame.
     */
    struct sample_t {
      frame_clock::time_point when;  ///< When the frame was encoded.
      std::size_t total;  ///< Frames counted up to and including this one.
    };

    std::deque<sample_t> samples;  ///< Recent encoded frames, oldest first.
    std::size_t total = 0;  ///< Frames counted so far.
  };

  /**
   * @brief Work out the largest frame a boost allows.
   *
   * @details A frame must fit in the stream's packets, and must be sent before the next frame
   *          is ready, or that frame and the ones after it wait for it. The next frame comes
   *          about one of the game's frame intervals later, so a frame may take most of that
   *          interval to send: `send_time_share` of it, leaving room for the frame to come out
   *          late. It may always take one interval of the stream's frame rate, which is what a
   *          frame takes at the client's bitrate.
   *
   *          When the game speeds up again, the boost comes down within a few frames (see
   *          frame_rate_estimator), so only those few take longer to send than frames then come.
   *
   * @param stream_fps The stream's frame rate.
   * @param send_rate_mbps The rate frames are sent at, in megabits per second, or 0 if unknown.
   * @param source_fps How often the game produces frames, or 0 if unknown.
   * @return The largest frame in bytes.
   */
  inline double max_frame_bytes(double stream_fps, int send_rate_mbps, double source_fps = 0.0) {
    double bytes = max_boosted_frame_bytes;
    if (send_rate_mbps > 0 && stream_fps > 0.0) {
      const double bytes_per_second = send_rate_mbps * 1'000'000.0 / 8.0;
      double sendable = bytes_per_second / stream_fps;
      if (source_fps > 0.0 && source_fps < stream_fps) {
        sendable = std::max(sendable, bytes_per_second * send_time_share / source_fps);
      }
      bytes = std::min(bytes, sendable);
    }
    return bytes;
  }

  /**
   * @brief Work out how much to raise the bitrate for frames coming at a given rate.
   *
   * @param stream_fps The stream's frame rate.
   * @param source_fps How often frames are actually encoded.
   * @param max_boost The most the bitrate may be multiplied by. 1 or less turns boosting off.
   * @param bitrate_kbps The client's bitrate in kilobits per second.
   * @param send_rate_mbps The rate frames are sent at, in megabits per second, or 0 if unknown.
   *                       See max_frame_bytes().
   * @return What to multiply the bitrate by, at least 1.
   */
  inline double boost_factor(double stream_fps, double source_fps, double max_boost, int bitrate_kbps, int send_rate_mbps = 0) {
    if (max_boost <= 1.0 || stream_fps <= 0.0 || source_fps <= 0.0 || bitrate_kbps <= 0) {
      return 1.0;
    }

    auto factor = std::clamp(stream_fps / source_fps, 1.0, max_boost);

    // Frames are kept to a size the stream can send in time
    const auto frame_bytes = bitrate_kbps * 1000.0 / 8.0 / stream_fps;
    return std::max(1.0, std::min(factor, max_frame_bytes(stream_fps, send_rate_mbps, source_fps) / frame_bytes));
  }

  /**
   * @brief Work out the most a stream's bitrate can be raised.
   *
   * @param stream_fps The stream's frame rate.
   * @param max_boost The most the bitrate may be multiplied by. 1 or less turns boosting off.
   * @param bitrate_kbps The client's bitrate in kilobits per second.
   * @param send_rate_mbps The rate frames are sent at, in megabits per second, or 0 if unknown.
   * @return The largest factor boost_factor() can return for these, at least 1.
   */
  inline double max_boost_factor(double stream_fps, double max_boost, int bitrate_kbps, int send_rate_mbps = 0) {
    // However slow the frames, the boost stops at the maximum or the largest frame size
    return boost_factor(stream_fps, stream_fps / std::max(max_boost, 1.0), max_boost, bitrate_kbps, send_rate_mbps);
  }

  /**
   * @brief Decides when to change the encoder's bitrate as the frame rate changes.
   */
  class booster {
  public:
    /**
     * @brief Start at the client's bitrate.
     *
     * @param bitrate_kbps The client's bitrate in kilobits per second.
     * @param stream_fps The stream's frame rate.
     * @param max_boost The most the bitrate may be multiplied by.
     * @param send_rate_mbps The rate frames are sent at, in megabits per second, or 0 if unknown.
     *                       See max_frame_bytes().
     */
    booster(int bitrate_kbps, double stream_fps, double max_boost, int send_rate_mbps = 0):
        base_kbps {bitrate_kbps},
        current_kbps {bitrate_kbps},
        stream_fps {stream_fps},
        max_boost {max_boost},
        send_rate_mbps {send_rate_mbps} {
    }

    /**
     * @brief Record a frame about to be encoded and say whether to change the bitrate for it.
     *
     * @details The bitrate is lowered as soon as frames come fast enough that it would send more
     *          than the client asked for. It is only raised by more than `change_threshold`, or
     *          to the most the boost allows, so the encoder isn't changed for every frame of a
     *          game whose frame rate wanders: it errs on the side of sending less.
     *
     * @param when When the frame is encoded.
     * @param frames How many frames this one stands for. See frame_rate_estimator::add_frame().
     * @return The bitrate to change to in kilobits per second, or nothing to keep the current one.
     */
    std::optional<int> on_frame(frame_clock::time_point when, std::size_t frames = 1) {
      estimator.add_frame(when, frames);
      const auto source_fps = estimator.fps();
      if (!source_fps) {
        return std::nullopt;
      }
      measured_fps = *source_fps;

      // Rounded down, so the bitrate sent is never more than the client's
      const auto target = static_cast<int>(std::floor(base_kbps * boost_factor(stream_fps, *source_fps, max_boost, base_kbps, send_rate_mbps)));
      if (target == current_kbps) {
        return std::nullopt;
      }
      // A rise to the most the boost allows goes through however small: the bitrate can't
      // wander above it
      const auto max_kbps = static_cast<int>(std::floor(base_kbps * max_boost_factor(stream_fps, max_boost, base_kbps, send_rate_mbps)));
      if (target > current_kbps && target != max_kbps && target - current_kbps <= current_kbps * change_threshold) {
        return std::nullopt;
      }

      current_kbps = target;
      return target;
    }

    /**
     * @brief Get the bitrate the encoder was last set to.
     *
     * @return The bitrate in kilobits per second.
     */
    int bitrate_kbps() const {
      return current_kbps;
    }

    /**
     * @brief Get the client's bitrate, which boosts are applied to.
     *
     * @return The bitrate in kilobits per second.
     */
    int client_bitrate_kbps() const {
      return base_kbps;
    }

    /**
     * @brief Get the frame rate measured when the last frame was recorded.
     *
     * @return Frames per second, or 0 before there is a measurement.
     */
    double fps() const {
      return measured_fps;
    }

  private:
    int base_kbps;  ///< The client's bitrate.
    int current_kbps;  ///< The bitrate the encoder was last set to.
    double stream_fps;  ///< The stream's frame rate.
    double max_boost;  ///< The most the bitrate may be multiplied by.
    int send_rate_mbps;  ///< The rate frames are sent at, or 0 if unknown.
    double measured_fps = 0.0;  ///< Frame rate measured at the last frame.
    frame_rate_estimator estimator;  ///< Measures how often frames are encoded.
  };
}  // namespace video::bitrate
