/**
 * @file tests/unit/platform/windows/test_misc.cpp
 * @brief Test src/platform/windows/misc.cpp.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // local includes
  #include <src/platform/windows/misc.h>

TEST(WindowsMiscTest, CommandsCanRunAsTheCurrentUserOutsideTheService) {
  // The tests don't run as SYSTEM, so commands run as the current user and need no signed-in session.
  EXPECT_TRUE(platf::user_session_available());
}
#endif
