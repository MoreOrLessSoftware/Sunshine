/**
 * @file tests/unit/platform/windows/test_misc.cpp
 * @brief Test src/platform/windows/misc.cpp.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <filesystem>
  #include <fstream>

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

/**
 * @brief Creates a scratch registry key under HKCU for device lookups and removes it afterwards.
 */
class WindowsDeviceRegistryTest: public ::testing::Test {
protected:
  const std::wstring path = L"Software\\SunshineTests\\DeviceWithDriver";  ///< Scratch key holding device subkeys.

  void SetUp() override {
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }

  void TearDown() override {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\SunshineTests");
  }

  /**
   * @brief Create a device subkey, optionally with a `Driver` value.
   *
   * @param device Name of the device subkey.
   * @param with_driver Whether to give it a `Driver` value.
   */
  void add_device(const std::wstring &device, bool with_driver) {
    HKEY key = nullptr;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, (path + L"\\" + device).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr), ERROR_SUCCESS);
    if (with_driver) {
      const std::wstring driver = L"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}\\0001";
      RegSetValueExW(key, L"Driver", 0, REG_SZ, reinterpret_cast<const BYTE *>(driver.c_str()), static_cast<DWORD>((driver.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
  }
};

TEST_F(WindowsDeviceRegistryTest, MissingKeyHasNoDevice) {
  EXPECT_FALSE(platf::has_device_with_driver(HKEY_CURRENT_USER, path));
}

TEST_F(WindowsDeviceRegistryTest, DeviceWithoutDriverDoesNotCount) {
  add_device(L"0000", false);
  EXPECT_FALSE(platf::has_device_with_driver(HKEY_CURRENT_USER, path));
}

TEST_F(WindowsDeviceRegistryTest, AnyDeviceWithDriverCounts) {
  add_device(L"0000", false);
  add_device(L"0001", true);
  EXPECT_TRUE(platf::has_device_with_driver(HKEY_CURRENT_USER, path));
}

TEST_F(WindowsDeviceRegistryTest, KeyExistsOnlyOnceCreated) {
  EXPECT_FALSE(platf::registry_key_exists(HKEY_CURRENT_USER, path));
  add_device(L"0000", false);
  EXPECT_TRUE(platf::registry_key_exists(HKEY_CURRENT_USER, path));
}

TEST_F(WindowsDeviceRegistryTest, DriverNeedsBothItsServiceAndItsFile) {
  const auto file = std::filesystem::temp_directory_path() / "sunshine_test_driver.sys";  // NOSONAR(cpp:S5443): safe for tests
  std::filesystem::remove(file);

  // A service left registered after the driver was uninstalled, until the next restart
  add_device(L"0000", false);
  EXPECT_FALSE(platf::driver_installed(HKEY_CURRENT_USER, path, file));

  std::ofstream {file} << "driver";
  EXPECT_TRUE(platf::driver_installed(HKEY_CURRENT_USER, path, file));

  // A file without its service
  RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  EXPECT_FALSE(platf::driver_installed(HKEY_CURRENT_USER, path, file));
  std::filesystem::remove(file);
}

TEST(WindowsMiscTest, ViGEmBusCheckNeedsTheDriverFile) {
  WCHAR system_dir[MAX_PATH];
  ASSERT_NE(GetSystemDirectoryW(system_dir, MAX_PATH), 0u);
  const auto file = std::filesystem::path {system_dir} / L"drivers" / L"ViGEmBus.sys";
  EXPECT_EQ(platf::vigembus_installed(), platf::driver_installed(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\ViGEmBus", file));
}

TEST(WindowsMiscTest, VirtualHidDriverCheckMatchesTheRegistry) {
  EXPECT_EQ(platf::virtualhid_driver_installed(), platf::has_device_with_driver(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\ROOT\\LIBVIRTUALHID"));
}
#endif
