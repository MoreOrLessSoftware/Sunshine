/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  /**
   * @brief Write status details to the log.
   *
   * @param prefix Text prefix used when formatting the message.
   * @param status Native status code returned by the platform API.
   */
  void print_status(const std::string_view &prefix, HRESULT status);
  /**
   * @brief Synchronize thread desktop.
   *
   * @return true when the thread desktop was synchronized successfully.
   */
  HDESK syncThreadDesktop();

  /**
   * @brief Read the current Windows high-resolution performance counter.
   *
   * @return Raw QPC tick value from `QueryPerformanceCounter`.
   */
  int64_t qpc_counter();

  /**
   * @brief Convert the difference between two QPC readings to nanoseconds.
   *
   * @param performance_counter1 Newer performance-counter reading.
   * @param performance_counter2 Older performance-counter reading.
   * @return Duration represented by the difference between two QPC values.
   */
  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);

  /**
   * @brief Check whether commands that run as the signed-in user can start.
   *
   * When Sunshine runs as a service it starts such commands with the token of the user signed in at the console,
   * so they can't start until someone signs in. Otherwise they run as Sunshine's own user and can always start.
   *
   * @return `true` when Sunshine isn't running as SYSTEM, or a user is signed in at the console.
   */
  bool user_session_available();

  /**
   * @brief Check whether the current process runs with a full administrator token.
   *
   * @return `true` when the process token is elevated, as it is under the service or after "Run as administrator".
   */
  bool is_process_elevated();

  /**
   * @brief Return the arguments of a Windows command line, without the program name.
   *
   * The program name ends at its closing quote when it starts with one, otherwise at the first space or tab,
   * as the C runtime parses it. The whitespace following it is skipped too.
   *
   * @param command_line Full command line, as returned by `GetCommandLineW()`.
   * @return Arguments following the program name, unchanged.
   */
  std::wstring_view command_line_arguments(std::wstring_view command_line);

  /**
   * @brief Start this executable again with administrator rights and the same arguments.
   *
   * Shows the UAC prompt and returns without waiting for the new process.
   *
   * @return `true` when the elevated process started, `false` when the prompt was declined or the launch failed.
   */
  bool relaunch_elevated();

  /**
   * @brief Check whether a registry key lists a device that has a driver bound to it.
   *
   * @param root Registry hive holding the key.
   * @param path Key whose subkeys are device instances, such as a key under `SYSTEM\CurrentControlSet\Enum`.
   * @return `true` when a subkey of the key has a `Driver` value.
   */
  bool has_device_with_driver(HKEY root, const std::wstring &path);

  /**
   * @brief Check whether the Virtual HID Driver is installed.
   *
   * @return `true` when the driver's root device exists and has its driver bound.
   */
  bool virtualhid_driver_installed();

  /**
   * @brief Check whether a registry key exists.
   *
   * @param root Registry hive holding the key.
   * @param path Key to look for.
   * @return `true` when the key can be opened for reading.
   */
  bool registry_key_exists(HKEY root, const std::wstring &path);

  /**
   * @brief Check whether the ViGEmBus driver is installed.
   *
   * @return `true` when its service is registered.
   */
  bool vigembus_installed();
}  // namespace platf
