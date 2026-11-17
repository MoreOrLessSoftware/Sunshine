/**
 * @file tests/unit/test_nvhttp_session.cpp
 * @brief Test ending the current streaming session.
 */

#include "../tests_common.h"

// local includes
#include <src/nvhttp.h>
#include <src/process.h>
#include <src/rtsp.h>

TEST(NvhttpQuitSession, LeavesNothingRunningWhenIdle) {
  ASSERT_EQ(proc::proc.running(), 0);
  ASSERT_EQ(rtsp_stream::session_count(), 0);

  nvhttp::quit_session();

  EXPECT_EQ(proc::proc.running(), 0);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
}
