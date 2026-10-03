/**
 * @file tests/unit/test_frame_trace.cpp
 * @brief Test src/frame_trace.h.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

TEST(FrameTraceTest, FrameSentInPartsTakesItsEncodeFromTheLastPart) {
  // Carried by the first part, with what sending the parts cost added on the way
  frame_trace::trace_t frame;
  frame.mark(point_e::present, start);
  frame.mark(point_e::converted, start + 400us);
  frame.mark(point_e::header, start + 1ms);
  frame.fec = 300us;
  frame.packets = 120;
  frame.frames_skipped = 1;
  frame.rtp_timestamp = 900;

  frame_trace::trace_t last_part = frame;
  last_part.mark(point_e::encoded, start + 4ms);
  last_part.bytes = 300 * 1024;
  last_part.idr = true;
  last_part.fec = 0us;
  last_part.packets = 0;

  frame_trace::finish_parts(frame, last_part);

  EXPECT_EQ(frame.at[static_cast<std::size_t>(point_e::encoded)], start + 4ms);
  EXPECT_EQ(frame.bytes, 300 * 1024);
  EXPECT_TRUE(frame.idr);

  // What the network thread collected stays
  EXPECT_EQ(frame.at[static_cast<std::size_t>(point_e::header)], start + 1ms);
  EXPECT_EQ(frame.fec, 300us);
  EXPECT_EQ(frame.packets, 120);
  EXPECT_EQ(frame.frames_skipped, 1);
  EXPECT_EQ(frame.rtp_timestamp, 900);

  // The header goes out before the frame is encoded, so latency is shorter than the encode
  ASSERT_TRUE(frame.ms(point_e::present, point_e::header).has_value());
  ASSERT_TRUE(frame.ms(point_e::converted, point_e::encoded).has_value());
  EXPECT_LT(*frame.ms(point_e::present, point_e::header), *frame.ms(point_e::converted, point_e::encoded));
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

namespace {
  /**
   * @brief Split a CSV line into its fields.
   *
   * @param line The line, with or without its newline.
   * @return Each field.
   */
  std::vector<std::string> fields(std::string line) {
    if (!line.empty() && line.back() == '\n') {
      line.pop_back();
    }
    std::vector<std::string> out;
    std::string field;
    for (const char c : line) {
      if (c == ',') {
        out.push_back(field);
        field.clear();
      } else {
        field += c;
      }
    }
    out.push_back(field);
    return out;
  }
}  // namespace

TEST(FrameTraceCsvTest, RowsHaveAFieldForEveryColumn) {
  const auto header = fields(frame_trace::csv_header());
  const auto row = fields(frame_trace::csv_row(make_trace(start, 6ms), {}));
  EXPECT_EQ(header.size(), row.size());
  EXPECT_EQ(header.front(), "frame");
  EXPECT_EQ(header.back(), "idr");
}

TEST(FrameTraceCsvTest, MatchesTheClientsPresentationTime) {
  auto trace = make_trace(start, 6ms);
  trace.frame_index = 7;
  trace.rtp_timestamp = 90'000 * 5 + 45;  // 5 s and half a millisecond at 90 kHz
  trace.packets = 412;
  trace.frames_skipped = 1;

  const auto row = fields(frame_trace::csv_row(trace, trace.at[static_cast<std::size_t>(point_e::present)] - 8333us));
  EXPECT_EQ(row[0], "7");
  EXPECT_EQ(row[1], "5000500");  // timestamp * 1000 / 90, as Moonlight computes it
  EXPECT_EQ(row[3], "8333");
  EXPECT_EQ(row[4], "100");  // acquired, after present
  EXPECT_EQ(row[13], "6000");  // header, after present: the frame's latency
  EXPECT_EQ(row[15], std::to_string(500 * 1024));
  EXPECT_EQ(row[16], "412");
  EXPECT_EQ(row[19], "1000");  // pacing
  EXPECT_EQ(row[21], "1");
  EXPECT_EQ(row[22], "0");
}

TEST(FrameTraceCsvTest, MarksPointsAFrameDidNotPass) {
  frame_trace::trace_t trace;
  trace.mark(point_e::present, start);
  trace.mark(point_e::header, start + 2ms);

  const auto row = fields(frame_trace::csv_row(trace, {}));
  EXPECT_EQ(row[3], "-1");  // no previous frame
  EXPECT_EQ(row[4], "-1");  // never acquired
  EXPECT_EQ(row[8], "-1");  // not a PyroWave frame
  EXPECT_EQ(row[13], "2000");
}

TEST(FrameTraceCsvTest, LeavesTimesOutWithoutAPresentTime) {
  frame_trace::trace_t trace;
  trace.mark(point_e::header, start);

  const auto row = fields(frame_trace::csv_row(trace, start));
  EXPECT_EQ(row[2], "-1");
  EXPECT_EQ(row[3], "-1");
  EXPECT_EQ(row[13], "-1");
}

TEST(FrameTraceCsvTest, FileIsNamedForTheSessionNextToTheLog) {
  const auto path = logging::frame_trace_csv_path(std::filesystem::path {"logs"} / "sunshine.log", std::chrono::system_clock::now());
  EXPECT_EQ(path.parent_path(), std::filesystem::path {"logs"});
  const auto name = path.filename().string();
  EXPECT_TRUE(name.starts_with("sunshine-frames-")) << name;
  EXPECT_TRUE(name.ends_with(".csv")) << name;
  EXPECT_EQ(name.size(), std::string_view {"sunshine-frames-20261001-153012.csv"}.size()) << name;
}
