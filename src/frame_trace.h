/**
 * @file src/frame_trace.h
 * @brief Declarations for tracing each video frame through capture, encode and send.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Tracing where each video frame spends its time, to find what causes latency spikes.
 *
 * @details A frame is stamped as it passes each point of the pipeline: when it was presented,
 *          captured, queued for the encoder, converted, encoded, picked up by the network thread,
 *          and sent. The network thread also adds up what sending it cost. Once a frame is sent,
 *          the reporter gives a summary of every stage now and then, and a line for each frame
 *          that took much longer than usual, so a spike can be traced to the stage it came from.
 */
namespace frame_trace {
  using trace_clock = std::chrono::steady_clock;  ///< Clock that trace points are taken on.

  /**
   * @brief Points a frame passes on its way through the pipeline, in order.
   */
  enum class point_e : std::size_t {
    present,  ///< The game presented the frame.
    acquired,  ///< The capture API handed the frame over.
    queued,  ///< The frame was queued for the encoder.
    popped,  ///< The encoder took the frame.
    converted,  ///< The frame was converted into the encoder's input.
    encode_flushed,  ///< PyroWave: the conversion was flushed to the GPU.
    encode_submitted,  ///< PyroWave: the encode was submitted.
    encode_finished,  ///< PyroWave: the GPU finished encoding.
    encoded,  ///< The encoded frame was handed to the network thread.
    broadcast_popped,  ///< The network thread took the frame.
    header,  ///< The frame header was written. Host processing latency is measured to here.
    sent,  ///< The last packet of the frame was sent.
    count  ///< Number of points.
  };

  /**
   * @brief One frame's trace.
   */
  struct trace_t {
    std::array<trace_clock::time_point, static_cast<std::size_t>(point_e::count)> at {};  ///< When the frame passed each point. Unset (epoch) if it didn't.
    trace_clock::duration fec {};  ///< Time spent computing FEC.
    trace_clock::duration encrypt {};  ///< Time spent encrypting packets.
    trace_clock::duration pacing {};  ///< Time spent waiting to keep to the send rate.
    trace_clock::duration send {};  ///< Time spent in socket sends.
    std::int64_t frame_index = 0;  ///< Frame index.
    std::size_t bytes = 0;  ///< Encoded size of the frame.
    std::size_t frames_skipped = 0;  ///< Frames captured while the encoder was busy and replaced by this one.
    bool idr = false;  ///< Whether the frame is a key frame.

    /**
     * @brief Record that the frame passed a point.
     *
     * @param point The point passed.
     * @param when When it was passed.
     */
    void mark(point_e point, trace_clock::time_point when = trace_clock::now()) {
      at[static_cast<std::size_t>(point)] = when;
    }

    /**
     * @brief Check whether the frame passed a point.
     *
     * @param point The point to check.
     * @return `true` if it was marked.
     */
    bool has(point_e point) const {
      return at[static_cast<std::size_t>(point)].time_since_epoch().count() != 0;
    }

    /**
     * @brief Get the time from one point to another.
     *
     * @param from The earlier point.
     * @param to The later point.
     * @return Milliseconds between them, or nothing if the frame didn't pass both.
     */
    std::optional<double> ms(point_e from, point_e to) const {
      if (!has(from) || !has(to)) {
        return std::nullopt;
      }
      return std::chrono::duration<double, std::milli>(at[static_cast<std::size_t>(to)] - at[static_cast<std::size_t>(from)]).count();
    }
  };

  /**
   * @brief A stage of the pipeline, reported as one value per frame.
   */
  struct stage_t {
    const char *name;  ///< Name in the log.
    point_e from;  ///< Point the stage starts at, unless it is a sum.
    point_e to;  ///< Point the stage ends at, unless it is a sum.
    trace_clock::duration trace_t::*sum;  ///< Summed time the stage reports instead, or null.
  };

  /**
   * @brief The stages reported, in pipeline order. "latency" is what the client shows as host processing latency.
   */
  inline const std::array<stage_t, 16> stages {{
    {"present>acquire", point_e::present, point_e::acquired, nullptr},
    {"capture", point_e::acquired, point_e::queued, nullptr},
    {"queue", point_e::queued, point_e::popped, nullptr},
    {"convert", point_e::popped, point_e::converted, nullptr},
    {"flush", point_e::converted, point_e::encode_flushed, nullptr},
    {"submit", point_e::encode_flushed, point_e::encode_submitted, nullptr},
    {"gpu", point_e::encode_submitted, point_e::encode_finished, nullptr},
    {"packetize", point_e::encode_finished, point_e::encoded, nullptr},
    {"encode", point_e::converted, point_e::encoded, nullptr},
    {"hand-off", point_e::encoded, point_e::broadcast_popped, nullptr},
    {"latency", point_e::present, point_e::header, nullptr},
    {"fec", point_e::count, point_e::count, &trace_t::fec},
    {"encrypt", point_e::count, point_e::count, &trace_t::encrypt},
    {"pacing", point_e::count, point_e::count, &trace_t::pacing},
    {"send", point_e::count, point_e::count, &trace_t::send},
    {"sending", point_e::header, point_e::sent, nullptr},
  }};

  constexpr std::size_t latency_stage = 10;  ///< Index of the stage slow frames are judged by.
  constexpr std::size_t max_slow_frames_logged = 100;  ///< Most slow frames logged in one interval.

  /**
   * @brief Get a stage's value for a frame.
   *
   * @param trace The frame's trace.
   * @param stage The stage.
   * @return Milliseconds, or nothing if the frame didn't pass through the stage.
   */
  inline std::optional<double> stage_ms(const trace_t &trace, const stage_t &stage) {
    if (stage.sum) {
      if (!trace.has(point_e::sent)) {
        return std::nullopt;
      }
      return std::chrono::duration<double, std::milli>(trace.*stage.sum).count();
    }
    return trace.ms(stage.from, stage.to);
  }

  /**
   * @brief Decide how long a frame must take to count as slow.
   *
   * @param typical_ms The usual latency, the median of the last interval.
   * @return Half as long again as usual, and at least 3 ms more.
   */
  inline double slow_threshold_ms(double typical_ms) {
    return std::max(typical_ms * 1.5, typical_ms + 3.0);
  }

  /**
   * @brief Collects sent frames' traces and decides what to log.
   */
  class reporter {
  public:
    /**
     * @brief Start reporting.
     *
     * @param interval How often to summarize the stages.
     */
    explicit reporter(std::chrono::seconds interval = std::chrono::seconds {10}):
        interval {interval} {
    }

    /**
     * @brief Add a sent frame.
     *
     * @param trace The frame's trace, marked as sent.
     * @return Lines to log: the frame's breakdown if it was slow, and a summary when one is due.
     */
    std::vector<std::string> add(const trace_t &trace) {
      std::vector<std::string> lines;
      const auto now = trace.has(point_e::sent) ? trace.at[static_cast<std::size_t>(point_e::sent)] : trace_clock::now();
      if (interval_start.time_since_epoch().count() == 0) {
        interval_start = now;
      }

      for (std::size_t i = 0; i < stages.size(); ++i) {
        if (const auto value = stage_ms(trace, stages[i])) {
          samples[i].push_back(*value);
        }
      }
      ++frames;

      const auto latency = stage_ms(trace, stages[latency_stage]);
      if (typical_ms && latency && *latency >= slow_threshold_ms(*typical_ms)) {
        ++slow_frames;
        if (slow_frames <= max_slow_frames_logged) {
          lines.push_back(describe(trace));
        }
      }

      if (now - interval_start >= interval) {
        lines.push_back(summarize());
        interval_start = now;
      }

      return lines;
    }

    /**
     * @brief Describe one frame's time in each stage.
     *
     * @param trace The frame's trace.
     * @return A line naming the frame and every stage it passed through.
     */
    static std::string describe(const trace_t &trace) {
      std::string line = std::format("Slow frame {}:", trace.frame_index);
      for (const auto &stage : stages) {
        if (const auto value = stage_ms(trace, stage)) {
          line += std::format(" {} {:.2f}", stage.name, *value);
        }
      }
      line += std::format(" ms | {} KB", (trace.bytes + 512) / 1024);
      if (trace.frames_skipped) {
        line += std::format(", replaced {} captured frame{}", trace.frames_skipped, trace.frames_skipped == 1 ? "" : "s");
      }
      if (trace.idr) {
        line += ", key frame";
      }
      return line;
    }

    /**
     * @brief Summarize the stages over the frames since the last summary, and start over.
     *
     * @return A line with the median, 99th percentile and maximum of each stage.
     */
    std::string summarize() {
      std::string line = std::format("{} frames", frames);
      if (typical_ms) {
        line += std::format(", {} slow (latency over {:.2f} ms)", slow_frames, slow_threshold_ms(*typical_ms));
      }
      line += " | p50/p99/max ms:";

      std::optional<double> latency_median;
      for (std::size_t i = 0; i < stages.size(); ++i) {
        auto &values = samples[i];
        if (values.empty()) {
          continue;
        }
        std::sort(values.begin(), values.end());
        const auto median = percentile(values, 0.50);
        line += std::format(" {} {:.2f}/{:.2f}/{:.2f}", stages[i].name, median, percentile(values, 0.99), values.back());
        if (i == latency_stage) {
          latency_median = median;
        }
        values.clear();
      }

      // The next interval's slow frames are judged against this one
      if (latency_median) {
        typical_ms = latency_median;
      }
      frames = 0;
      slow_frames = 0;
      return line;
    }

  private:
    /**
     * @brief Find the value a fraction of the way through sorted values, using the nearest rank.
     *
     * @param sorted Values in ascending order. Must not be empty.
     * @param fraction How far through the values, from 0 to 1.
     * @return The value at that rank.
     */
    static double percentile(const std::vector<double> &sorted, double fraction) {
      const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
      return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
    }

    std::chrono::seconds interval;  ///< How often to summarize.
    trace_clock::time_point interval_start {};  ///< When the current interval started.
    std::array<std::vector<double>, stages.size()> samples;  ///< Each stage's values this interval.
    std::size_t frames = 0;  ///< Frames added this interval.
    std::size_t slow_frames = 0;  ///< Slow frames this interval, logged or not.
    std::optional<double> typical_ms;  ///< Median latency of the last interval.
  };
}  // namespace frame_trace
