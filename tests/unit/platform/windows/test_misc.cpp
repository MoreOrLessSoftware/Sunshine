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

TEST(WindowsMiscTest, ProcessElevationMatchesTheToken) {
  HANDLE token = nullptr;
  ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
  TOKEN_ELEVATION elevation {};
  DWORD size = 0;
  ASSERT_TRUE(GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size));
  CloseHandle(token);
  EXPECT_EQ(platf::is_process_elevated(), elevation.TokenIsElevated != 0);
}

TEST(WindowsMiscTest, CommandLineArgumentsSkipAQuotedProgramName) {
  EXPECT_EQ(platf::command_line_arguments(LR"("C:\Program Files\Sunshine\sunshine.exe" a.conf port=1)"), L"a.conf port=1");
}

TEST(WindowsMiscTest, CommandLineArgumentsSkipAnUnquotedProgramName) {
  EXPECT_EQ(platf::command_line_arguments(L"sunshine.exe \t a.conf"), L"a.conf");
}

TEST(WindowsMiscTest, CommandLineArgumentsKeepQuotesInArguments) {
  EXPECT_EQ(platf::command_line_arguments(LR"(sunshine "D:\my config\sunshine.conf")"), LR"("D:\my config\sunshine.conf")");
}

TEST(WindowsMiscTest, CommandLineArgumentsAreEmptyWithoutArguments) {
  EXPECT_EQ(platf::command_line_arguments(L"sunshine.exe"), L"");
  EXPECT_EQ(platf::command_line_arguments(LR"("C:\sunshine.exe")"), L"");
  EXPECT_EQ(platf::command_line_arguments(LR"("C:\unterminated)"), L"");
  EXPECT_EQ(platf::command_line_arguments(L""), L"");
}
#endif
