/**
 * @file tests/unit/test_frame_trace.cpp
 * @brief Test src/frame_trace.h.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <string>
#include <string_view>

// local includes
#include <src/frame_trace.h>
#include <src/logging.h>

using namespace std::chrono_literals;
using frame_trace::point_e;
using frame_trace::trace_clock;

namespace {
  /**
   * @brief Build a trace for a frame that passed every point.
   *
   * @param sent When the frame finished sending.
   * @param latency Time from present to the frame header.
   * @return The trace.
   */
  frame_trace::trace_t make_trace(trace_clock::time_point sent, trace_clock::duration latency) {
    frame_trace::trace_t trace;
    const auto present = sent - latency - 2ms;
    trace.mark(point_e::present, present);
    trace.mark(point_e::acquired, present + 100us);
    trace.mark(point_e::queued, present + 200us);
    trace.mark(point_e::popped, present + 300us);
    trace.mark(point_e::converted, present + 400us);
    trace.mark(point_e::encode_flushed, present + 500us);
    trace.mark(point_e::encode_submitted, present + 600us);
    trace.mark(point_e::encode_finished, present + latency - 1ms);
    trace.mark(point_e::encoded, present + latency - 500us);
    trace.mark(point_e::broadcast_popped, present + latency - 100us);
    trace.mark(point_e::header, present + latency);
    trace.mark(point_e::sent, sent);
    trace.fec = 300us;
    trace.encrypt = 400us;
    trace.pacing = 1ms;
    trace.send = 200us;
    trace.bytes = 500 * 1024;
    return trace;
  }

  /**
   * @brief Check whether a string contains another.
   *
   * @param text String to search.
   * @param part String to find.
   * @return `true` if found.
   */
  bool contains(const std::string &text, std::string_view part) {
    return text.find(part) != std::string::npos;
  }

  const trace_clock::time_point start = trace_clock::time_point {} + 100s;  ///< When the first test frame is sent.
}  // namespace

TEST(FrameTraceTest, MeasuresBetweenPoints) {
  frame_trace::trace_t trace;
  EXPECT_FALSE(trace.has(point_e::present));
  EXPECT_FALSE(trace.ms(point_e::present, point_e::header).has_value());

  trace.mark(point_e::present, start);
  trace.mark(point_e::header, start + 2500us);

  EXPECT_TRUE(trace.has(point_e::present));
  ASSERT_TRUE(trace.ms(point_e::present, point_e::header).has_value());
  EXPECT_DOUBLE_EQ(*trace.ms(point_e::present, point_e::header), 2.5);
}

TEST(FrameTraceTest, LatencyStageIsPresentToHeader) {
  const auto &stage = frame_trace::stages[frame_trace::latency_stage];
  EXPECT_STREQ(stage.name, "latency");
  EXPECT_EQ(stage.from, point_e::present);
  EXPECT_EQ(stage.to, point_e::header);
}

TEST(FrameTraceTest, ReportsSumsOnlyOnceSent) {
  frame_trace::trace_t trace;
  trace.fec = 1500us;
  const auto &fec = frame_trace::stages[11];
  ASSERT_STREQ(fec.name, "fec");

  EXPECT_FALSE(frame_trace::stage_ms(trace, fec).has_value());

  trace.mark(point_e::sent, start);
  ASSERT_TRUE(frame_trace::stage_ms(trace, fec).has_value());
  EXPECT_DOUBLE_EQ(*frame_trace::stage_ms(trace, fec), 1.5);
}

TEST(FrameTraceTest, SlowMeansHalfAsLongAgainAndThreeMillisecondsMore) {
  EXPECT_DOUBLE_EQ(frame_trace::slow_threshold_ms(4.0), 7.0);
  EXPECT_DOUBLE_EQ(frame_trace::slow_threshold_ms(10.0), 15.0);
}

TEST(FrameTraceTest, DescribesEveryStageOfAFrame) {
  auto trace = make_trace(start, 8ms);
  trace.frame_index = 42;
  trace.frames_skipped = 2;
  trace.idr = true;

  const auto line = frame_trace::reporter::describe(trace);
  EXPECT_TRUE(contains(line, "Slow frame 42:")) << line;
  EXPECT_TRUE(contains(line, "latency 8.00")) << line;
  EXPECT_TRUE(contains(line, "gpu ")) << line;
  EXPECT_TRUE(contains(line, "pacing 1.00")) << line;
  EXPECT_TRUE(contains(line, "500 KB")) << line;
  EXPECT_TRUE(contains(line, "replaced 2 captured frames")) << line;
  EXPECT_TRUE(contains(line, "key frame")) << line;
}

TEST(FrameTraceTest, LeavesOutStagesAFrameSkipped) {
  frame_trace::trace_t trace;
  trace.mark(point_e::present, start);
  trace.mark(point_e::header, start + 5ms);
  trace.frames_skipped = 1;

  const auto line = frame_trace::reporter::describe(trace);
  EXPECT_FALSE(contains(line, "gpu")) << line;
  EXPECT_FALSE(contains(line, "fec")) << line;
  EXPECT_TRUE(contains(line, "replaced 1 captured frame")) << line;
  EXPECT_FALSE(contains(line, "frames")) << line;
}

TEST(FrameTraceReporterTest, SummarizesEachInterval) {
  frame_trace::reporter reporter {10s};
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(reporter.add(make_trace(start + 10ms * i, 6ms)).empty());
  }

  const auto lines = reporter.add(make_trace(start + 10s, 6ms));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(contains(lines[0], "101 frames")) << lines[0];
  EXPECT_TRUE(contains(lines[0], "latency 6.00/6.00/6.00")) << lines[0];
  EXPECT_FALSE(contains(lines[0], "slow")) << lines[0];
}

TEST(FrameTraceReporterTest, LogsSlowFramesAgainstTheLastInterval) {
  frame_trace::reporter reporter {10s};
  reporter.add(make_trace(start, 6ms));
  reporter.add(make_trace(start + 10s, 6ms));

  // Usual is 6 ms, so slow is 9 ms or more
  EXPECT_TRUE(reporter.add(make_trace(start + 10s + 10ms, 8ms)).empty());
  const auto lines = reporter.add(make_trace(start + 10s + 20ms, 14ms));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(contains(lines[0], "Slow frame")) << lines[0];
  EXPECT_TRUE(contains(lines[0], "latency 14.00")) << lines[0];

  const auto summary = reporter.add(make_trace(start + 20s + 20ms, 6ms));
  ASSERT_EQ(summary.size(), 1u);
  EXPECT_TRUE(contains(summary[0], "1 slow (latency over 9.00 ms)")) << summary[0];
}

TEST(FrameTraceReporterTest, LogsNoSlowFramesBeforeItKnowsWhatIsUsual) {
  frame_trace::reporter reporter {10s};
  reporter.add(make_trace(start, 6ms));

  EXPECT_TRUE(reporter.add(make_trace(start + 10ms, 30ms)).empty());
}

TEST(FrameTraceReporterTest, LimitsSlowFramesLoggedButCountsThemAll) {
  frame_trace::reporter reporter {10s};
  reporter.add(make_trace(start, 6ms));
  reporter.add(make_trace(start + 10s, 6ms));

  std::size_t logged = 0;
  const auto slow = frame_trace::max_slow_frames_logged + 5;
  for (std::size_t i = 0; i < slow; ++i) {
    logged += reporter.add(make_trace(start + 10s + 10ms * (i + 1), 20ms)).size();
  }
  EXPECT_EQ(logged, frame_trace::max_slow_frames_logged);

  const auto summary = reporter.summarize();
  EXPECT_TRUE(contains(summary, std::to_string(slow) + " slow")) << summary;
}

TEST(FrameTraceReporterTest, UsesTheCurrentTimeForFramesNotSent) {
  frame_trace::reporter reporter {10s};
  frame_trace::trace_t trace;
  trace.mark(point_e::present, start);
  trace.mark(point_e::header, start + 5ms);

  EXPECT_TRUE(reporter.add(trace).empty());
  EXPECT_TRUE(contains(reporter.summarize(), "1 frames"));
}

TEST(LatencyLogPathTest, IsNextToTheMainLog) {
  EXPECT_EQ(logging::latency_log_path(std::filesystem::path {"logs"} / "sunshine.log"), std::filesystem::path {"logs"} / "sunshine-latency.log");
  EXPECT_EQ(logging::latency_log_path("sunshine"), std::filesystem::path {"sunshine-latency"});
}
