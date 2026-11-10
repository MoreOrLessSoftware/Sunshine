/**
 * @file src/platform/windows/xgpad.h
 * @brief Virtual Xbox Series X|S controllers backed by the Sunshine xgpad UMDF2 driver.
 *
 * Each virtual pad is a software device (SwDeviceCreate) driven by the xgpad HID minidriver, with the
 * inbox xinputhid filter on top. Sunshine exchanges HID reports with the driver through a named shared
 * memory section and two events per pad (see drivers/xgpad/xgpad_shared.h).
 */
#pragma once

// standard includes
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// local includes
#include "src/platform/common.h"

namespace platf::xgpad {

  class context_t;

  /**
   * @brief Release a context; joins the feedback thread and removes all virtual pads.
   * @param context Context to destroy.
   */
  void destroy(context_t *context);

  /**
   * @brief Owning pointer to an xgpad context.
   */
  using context_ptr = std::unique_ptr<context_t, decltype(&destroy)>;

  /**
   * @brief Operations supported by the bundled driver management script.
   */
  enum class driver_action_e {
    install,  ///< Trust the release certificate and add the driver package.
    uninstall,  ///< Remove the device nodes, driver package and certificates.
  };

  /**
   * @brief Check whether the xgpad driver package is present in the driver store.
   *
   * The result is cached, because enumerating the class drivers is slow.
   *
   * @param refresh Query the driver store again and update the cached result.
   * @return True when a HIDClass driver matching the xgpad hardware ID is installed.
   */
  bool driver_installed(bool refresh = false);

  /**
   * @brief Get the version of the installed xgpad driver package.
   *
   * Uses the result cached by `driver_installed()`.
   *
   * @return Dotted four-part version, or an empty string when the driver is not installed.
   */
  std::string driver_version();

  /**
   * @brief Get the folder Sunshine is installed in.
   * @return Directory containing the running executable.
   */
  std::filesystem::path install_root();

  /**
   * @brief Check whether the driver package and its management script are on disk.
   * @param root Sunshine install folder.
   * @return True when the driver can be installed from `root`.
   */
  bool driver_package_available(const std::filesystem::path &root);

  /**
   * @brief Build the command line that runs the driver management script.
   * @param powershell Full path of powershell.exe.
   * @param script Full path of xgpad-driver.ps1.
   * @param action Operation to run.
   * @return Command line for CreateProcessW.
   */
  std::wstring driver_script_command(const std::filesystem::path &powershell, const std::filesystem::path &script, driver_action_e action);

  /**
   * @brief Run the driver management script and wait for it.
   *
   * The script runs with Sunshine's own privileges, which is SYSTEM for the service, so it works without
   * a signed-in administrator. The script writes its output to config\\xgpad-driver.log.
   *
   * @param action Operation to run.
   * @param root Sunshine install folder.
   * @return Exit code of the script, or -1 when it could not run or timed out.
   */
  int run_driver_script(driver_action_e action, const std::filesystem::path &root);

  /**
   * @brief Check whether a device instance ID belongs to an xgpad virtual pad (software device or its HID child).
   *
   * Also matches the naming used by earlier xgpad test builds. A real Bluetooth Xbox Series controller never
   * matches: its HID child carries the Bluetooth address before "&IG_00".
   *
   * @param instance_id Device instance ID (case-insensitive).
   * @return True for xgpad device nodes.
   */
  bool is_xgpad_instance_id(std::wstring_view instance_id);

  /**
   * @brief Delete non-present ("phantom") xgpad device nodes so they do not pile up in Device Manager.
   *
   * Present devices are never touched. Requires administrator rights.
   *
   * @return Number of device nodes removed.
   */
  int remove_stale_devices();

  /**
   * @brief Create the xgpad context (no devices are created until a pad is allocated).
   * @return Context, or an empty pointer when the platform APIs are unavailable.
   */
  context_ptr create();

  /**
   * @brief Create a virtual pad for a client controller.
   *
   * Blocks until PnP reports the device as created (normally well under a second).
   *
   * @param context xgpad context.
   * @param id Global and client-relative identifiers of the controller.
   * @param feedback_queue Queue used to send rumble back to the client.
   * @return 0 on success, -1 on failure (the caller may fall back to another driver).
   */
  int alloc(context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue);

  /**
   * @brief Check whether a global gamepad index is backed by an xgpad virtual pad.
   * @param context xgpad context, may be null.
   * @param nr Global gamepad index.
   * @return True when the slot holds an xgpad pad.
   */
  bool has(const context_t *context, int nr);

  /**
   * @brief Route an existing virtual pad's feedback to a resumed client.
   * @param context xgpad context.
   * @param id Global and client-relative identifiers of the controller.
   * @param feedback_queue Queue used to send rumble back to the client.
   * @return 0 on success, -1 when the slot is empty.
   */
  int rebind(context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue);

  /**
   * @brief Submit new controller state to a virtual pad.
   * @param context xgpad context.
   * @param nr Global gamepad index.
   * @param state Button and axis state from the client.
   */
  void update(context_t &context, int nr, const gamepad_state_t &state);

  /**
   * @brief Remove a virtual pad.
   * @param context xgpad context.
   * @param nr Global gamepad index.
   */
  void free(context_t &context, int nr);

}  // namespace platf::xgpad
