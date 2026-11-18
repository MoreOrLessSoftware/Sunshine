/**
 * @file src/platform/windows/xgpad.h
 * @brief Virtual Xbox Series X|S and DualSense controllers backed by the Sunshine xgpad UMDF2 driver.
 *
 * Each virtual pad is a software device (SwDeviceCreate) driven by the xgpad HID minidriver; Xbox Series pads
 * have the inbox xinputhid filter on top. Sunshine exchanges HID reports with the driver through a named shared
 * memory section and two events per pad (see drivers/xgpad/xgpad_shared.h).
 */
#pragma once

// standard includes
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "src/platform/common.h"
#include "xgpad_report.h"

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
   * @brief Check whether the installed xgpad driver can present DualSense controllers.
   *
   * Uses the result cached by `driver_installed()`.
   *
   * @return True when the driver package matches the DualSense hardware ID.
   */
  bool driver_supports_dualsense();

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
   * A DualSense pad asks the client for motion events.
   *
   * @param context xgpad context.
   * @param id Global and client-relative identifiers of the controller.
   * @param profile Controller the pad presents.
   * @param feedback_queue Queue used to send rumble, lightbar, player LEDs and trigger effects back to the client.
   * @param client_uuid Client the controller belongs to, so the client can switch its pads; empty when unknown.
   * @return 0 on success, -1 on failure (the caller may fall back to another driver).
   */
  int alloc(context_t &context, const gamepad_id_t &id, profile_e profile, feedback_queue_t feedback_queue, const std::string &client_uuid = {});

  /**
   * @brief Get the profile a client switched its pads to from its in-stream menu.
   *
   * The `xgpad.set_profile` client command action, registered while an xgpad context exists, recreates the
   * client's pads with the chosen profile. The choice also applies to pads the client connects later, until the
   * client's last pad is removed.
   *
   * @param context xgpad context.
   * @param client_uuid Client UUID.
   * @return The profile, or empty when the client hasn't switched (or the UUID is empty).
   */
  std::optional<profile_e> session_profile(context_t &context, std::string_view client_uuid);

  /**
   * @brief Check whether a global gamepad index is backed by an xgpad virtual pad.
   * @param context xgpad context, may be null.
   * @param nr Global gamepad index.
   * @return True when the slot holds an xgpad pad.
   */
  bool has(const context_t *context, int nr);

  /**
   * @brief Route an existing virtual pad's feedback to a resumed client.
   *
   * A DualSense pad asks the resumed client for motion events again.
   *
   * @param context xgpad context.
   * @param id Global and client-relative identifiers of the controller.
   * @param feedback_queue Queue used to send feedback back to the client.
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
   * @brief Submit a touchpad event to a virtual pad; Xbox Series pads ignore it.
   * @param context xgpad context.
   * @param touch Touch event from the client.
   */
  void touch(context_t &context, const gamepad_touch_t &touch);

  /**
   * @brief Submit a motion sample to a virtual pad; Xbox Series pads ignore it.
   * @param context xgpad context.
   * @param motion Accelerometer or gyro sample from the client.
   */
  void motion(context_t &context, const gamepad_motion_t &motion);

  /**
   * @brief Submit the client controller's battery state to a virtual pad; Xbox Series pads ignore it.
   * @param context xgpad context.
   * @param battery Battery state from the client.
   */
  void battery(context_t &context, const gamepad_battery_t &battery);

  /**
   * @brief Remove a virtual pad.
   * @param context xgpad context.
   * @param nr Global gamepad index.
   */
  void free(context_t &context, int nr);

}  // namespace platf::xgpad
