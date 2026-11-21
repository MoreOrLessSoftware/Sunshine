/**
 * @file src/platform/windows/xgpad_report.h
 * @brief Conversions between Moonlight gamepad state and xgpad (virtual Xbox Series controller) HID reports.
 *
 * Kept free of Windows APIs so the mapping can be unit tested on its own.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "drivers/xgpad/xgpad_shared.h"
#include "src/platform/common.h"

namespace platf::xgpad {

  /**
   * @brief Rumble motor state in Moonlight feedback units (0..65535 per motor).
   */
  struct rumble_state_t {
    std::uint16_t low_freq;  ///< Left (strong, low frequency) grip motor.
    std::uint16_t high_freq;  ///< Right (weak, high frequency) grip motor.
    std::uint16_t left_trigger;  ///< Left impulse trigger motor.
    std::uint16_t right_trigger;  ///< Right impulse trigger motor.

    /**
     * @brief Compare two rumble states.
     * @param other State to compare with.
     * @return True when all four motors match.
     */
    bool operator==(const rumble_state_t &other) const = default;
  };

  /**
   * @brief Convert Moonlight D-pad button flags to the HID hat switch value.
   *
   * Opposing directions resolve to up and left respectively.
   *
   * @param button_flags Moonlight button mask.
   * @return 0 when released, otherwise 1 (N) to 8 (NW) clockwise.
   */
  constexpr std::uint8_t hat_from_buttons(const std::uint32_t button_flags) {
    const bool up = button_flags & DPAD_UP;
    const bool down = !up && (button_flags & DPAD_DOWN);
    const bool left = button_flags & DPAD_LEFT;
    const bool right = !left && (button_flags & DPAD_RIGHT);

    if (up) {
      return right ? 2 : left ? 8 :
                                1;
    }
    if (down) {
      return right ? 4 : left ? 6 :
                                5;
    }
    if (right) {
      return 3;
    }
    if (left) {
      return 7;
    }
    return 0;
  }

  /**
   * @brief Convert Moonlight face/shoulder/system button flags to the HID button field.
   *
   * Paddles are not mapped: the Xbox Series controller has none.
   *
   * @param button_flags Moonlight button mask.
   * @return XGPAD_BTN_* bit mask.
   */
  constexpr std::uint16_t buttons_from_flags(const std::uint32_t button_flags) {
    std::uint16_t buttons = 0;
    const auto map = [&](const std::uint32_t flag, const std::uint16_t bit) {
      if (button_flags & flag) {
        buttons |= bit;
      }
    };
    map(A, XGPAD_BTN_A);
    map(B, XGPAD_BTN_B);
    map(X, XGPAD_BTN_X);
    map(Y, XGPAD_BTN_Y);
    map(LEFT_BUTTON, XGPAD_BTN_LB);
    map(RIGHT_BUTTON, XGPAD_BTN_RB);
    map(BACK, XGPAD_BTN_VIEW);
    map(START, XGPAD_BTN_MENU);
    map(HOME, XGPAD_BTN_GUIDE);
    map(LEFT_STICK, XGPAD_BTN_LS);
    map(RIGHT_STICK, XGPAD_BTN_RS);
    return buttons;
  }

  /**
   * @brief Convert an 8-bit Moonlight trigger value to the controller's 10-bit range.
   * @param value Trigger value 0..255.
   * @return Trigger value 0..1023.
   */
  constexpr std::uint16_t trigger_from_u8(const std::uint8_t value) {
    return static_cast<std::uint16_t>((value * 1023u + 127u) / 255u);
  }

  /**
   * @brief Convert a signed Moonlight X axis (positive = right) to the unsigned HID axis.
   * @param value Axis value -32768..32767.
   * @return HID axis value 0..65535, 32768 at rest.
   */
  constexpr std::uint16_t axis_x(const std::int16_t value) {
    return static_cast<std::uint16_t>(static_cast<std::int32_t>(value) + 32768);
  }

  /**
   * @brief Convert a signed Moonlight Y axis (positive = up) to the unsigned HID axis (increasing downwards).
   *
   * Maps 0 to 32767 so XInput reports exactly 0 at rest.
   *
   * @param value Axis value -32768..32767.
   * @return HID axis value 0..65535.
   */
  constexpr std::uint16_t axis_y(const std::int16_t value) {
    return static_cast<std::uint16_t>(32767 - static_cast<std::int32_t>(value));
  }

  /**
   * @brief Build the 16-byte input report (ID 0x01 payload) for a Moonlight gamepad state.
   *
   * The Share button is driven by the Moonlight misc button (Xbox Series share) and by a
   * PlayStation touchpad click.
   *
   * @param state Gamepad state received from the client.
   * @return Input report payload.
   */
  constexpr XGPAD_INPUT_REPORT make_input_report(const gamepad_state_t &state) {
    XGPAD_INPUT_REPORT report {};
    report.lx = axis_x(state.lsX);
    report.ly = axis_y(state.lsY);
    report.rx = axis_x(state.rsX);
    report.ry = axis_y(state.rsY);
    report.lt = trigger_from_u8(state.lt);
    report.rt = trigger_from_u8(state.rt);
    report.hat = hat_from_buttons(state.buttonFlags);
    report.buttons = buttons_from_flags(state.buttonFlags);
    report.share = (state.buttonFlags & (MISC_BUTTON | TOUCHPAD_BUTTON)) ? 1 : 0;
    return report;
  }

  /**
   * @brief Convert a 0..100 HID motor magnitude to Moonlight's 0..65535 feedback range.
   * @param magnitude Motor magnitude; values above 100 are clamped.
   * @return Feedback strength.
   */
  constexpr std::uint16_t motor_from_percent(const std::uint8_t magnitude) {
    const std::uint32_t clamped = magnitude > 100 ? 100u : magnitude;
    return static_cast<std::uint16_t>(clamped * 65535u / 100u);
  }

  /**
   * @brief Check whether a driver policy uses xgpad when it is installed.
   *
   * "xgpad" always does. "all" (and an unset policy, which behaves like "all") does when Virtual HID Driver
   * is not usable, so installing the xgpad component works without changing the configuration.
   *
   * @param driver_policy Configured gamepad_driver value.
   * @param virtualhid_usable Whether Virtual HID Driver is available and licensed.
   * @return True when the xgpad context should be created (if the driver is installed).
   */
  constexpr bool policy_uses_xgpad(const std::string_view driver_policy, const bool virtualhid_usable) {
    if (driver_policy == "xgpad") {
      return true;
    }
    return (driver_policy == "all" || driver_policy.empty()) && !virtualhid_usable;
  }

  /**
   * @brief Controller an xgpad virtual pad presents.
   */
  enum class profile_e {
    xbox_series,  ///< Xbox Series X|S controller (XInput through xinputhid).
    dualsense,  ///< DualSense controller (motion, touchpad, lightbar, adaptive triggers).
  };

  /**
   * @brief Name of a profile for logs.
   * @param profile Pad profile.
   * @return "Xbox Series" or "DualSense".
   */
  constexpr std::string_view profile_name(const profile_e profile) {
    return profile == profile_e::dualsense ? "DualSense" : "Xbox Series";
  }

  /**
   * @brief Lightbar color the client controller shows after switching to a profile.
   * @param profile Profile switched to.
   * @return Red, green and blue: blue for DualSense, green for Xbox Series.
   */
  constexpr std::array<std::uint8_t, 3> profile_led_color(const profile_e profile) {
    return profile == profile_e::dualsense ? std::array<std::uint8_t, 3> {0, 0, 255} : std::array<std::uint8_t, 3> {0, 255, 0};
  }

  /**
   * @brief Inputs for choosing between xgpad and ViGEmBus for one controller.
   */
  struct allocation_choice_t {
    std::string_view driver_policy;  ///< Configured gamepad_driver value.
    std::string_view gamepad;  ///< Configured gamepad profile ("auto", "x360", "ds4", ...).
    std::uint8_t client_type;  ///< LI_CTYPE_* reported by the client.
    std::uint16_t client_capabilities;  ///< LI_CCAP_* reported by the client.
    bool vigem_available;  ///< Whether ViGEmBus can create controllers.
    bool motion_as_ds4;  ///< config::input.motion_as_ds4
    bool touchpad_as_ds4;  ///< config::input.touchpad_as_ds4
    bool dualsense_supported;  ///< Whether the installed xgpad driver has the DualSense profile.
    std::optional<profile_e> preferred = std::nullopt;  ///< Profile the client switched to or the xgpad plugin's option asks for, if any.
  };

  /**
   * @brief Read the xgpad plugin's "controller" option.
   * @param value Option value: "automatic", "xbox" or "dualsense".
   * @return The profile it asks for, or empty for "automatic" and unknown values.
   */
  constexpr std::optional<profile_e> profile_from_option(const std::string_view value) {
    if (value == "xbox") {
      return profile_e::xbox_series;
    }
    if (value == "dualsense") {
      return profile_e::dualsense;
    }
    return std::nullopt;
  }

  /**
   * @brief Check whether a controller should be a PlayStation controller when its type is chosen automatically.
   * @param choice Client metadata and the motion/touchpad options.
   * @return True for PlayStation clients, and for clients with motion sensors or a touchpad when the matching
   *         option is on.
   */
  constexpr bool wants_playstation(const allocation_choice_t &choice) {
    constexpr std::uint8_t ctype_ps = 0x02;  // LI_CTYPE_PS
    constexpr std::uint16_t ccap_touchpad = 0x08;  // LI_CCAP_TOUCHPAD
    constexpr std::uint16_t ccap_motion = 0x10 | 0x20;  // LI_CCAP_ACCEL | LI_CCAP_GYRO
    return choice.client_type == ctype_ps ||
           (choice.motion_as_ds4 && (choice.client_capabilities & ccap_motion)) ||
           (choice.touchpad_as_ds4 && (choice.client_capabilities & ccap_touchpad));
  }

  /**
   * @brief Decide whether a controller is created through xgpad (when the xgpad context exists), and as what.
   *
   * With an explicit "xgpad" policy every controller is an xgpad pad: a DualSense for the PlayStation profiles and,
   * automatically, for controllers wants_playstation() picks; an Xbox Series controller otherwise, or when the
   * installed driver predates the DualSense profile. Under "all", xgpad covers the Xbox profiles and the DualSense,
   * while ViGEmBus keeps an explicit DualShock 4 or Xbox 360 choice, and what xgpad cannot present.
   *
   * A preferred profile (the client's switch, or the xgpad plugin's option) wins over all of that whenever the policy
   * lets xgpad create controllers; a preferred DualSense is an Xbox Series controller on a driver without it.
   *
   * @param choice Policy, profile, client metadata, preference and driver availability.
   * @return Profile to allocate through xgpad, or empty to use the remaining drivers.
   */
  constexpr std::optional<profile_e> choose_xgpad_profile(const allocation_choice_t &choice) {
    const auto &gamepad = choice.gamepad;
    const bool playstation_profile = gamepad == "ds5" || gamepad == "ds4";
    const auto dualsense_or_xbox = choice.dualsense_supported ? profile_e::dualsense : profile_e::xbox_series;

    const bool xgpad_allowed = choice.driver_policy == "xgpad" || choice.driver_policy == "all" || choice.driver_policy.empty();
    if (xgpad_allowed && choice.preferred) {
      return *choice.preferred == profile_e::dualsense ? dualsense_or_xbox : profile_e::xbox_series;
    }

    if (choice.driver_policy == "xgpad") {
      if (playstation_profile) {
        return dualsense_or_xbox;
      }
      if (gamepad == "auto" && wants_playstation(choice)) {
        return dualsense_or_xbox;
      }
      return profile_e::xbox_series;
    }
    if (!choice.driver_policy.empty() && choice.driver_policy != "all") {
      return std::nullopt;  // "vigembus" / "virtualhid" never use xgpad
    }

    if (gamepad == "xseries" || gamepad == "xone") {
      return profile_e::xbox_series;
    }
    if (gamepad == "x360") {
      return choice.vigem_available ? std::nullopt : std::optional {profile_e::xbox_series};
    }
    if (gamepad == "ds5") {
      return choice.dualsense_supported ? std::optional {profile_e::dualsense} : std::nullopt;
    }
    if (gamepad == "ds4") {
      return !choice.vigem_available && choice.dualsense_supported ? std::optional {profile_e::dualsense} : std::nullopt;
    }
    if (gamepad != "auto") {
      return std::nullopt;  // switch, generic: not something xgpad presents
    }

    if (wants_playstation(choice)) {
      if (choice.dualsense_supported) {
        return profile_e::dualsense;
      }
      if (choice.vigem_available) {
        return std::nullopt;  // ViGEmBus makes it a DualShock 4
      }
    }
    return profile_e::xbox_series;
  }

  /**
   * @brief One occupied xgpad slot, as a switch between profiles sees it.
   */
  struct pad_slot_t {
    int nr;  ///< Global gamepad index.
    std::string_view client_uuid;  ///< Client the pad belongs to.
    profile_e profile;  ///< Controller the pad presents.
  };

  /**
   * @brief Pick the pads a client's switch to a profile recreates.
   * @param slots Occupied xgpad slots.
   * @param client_uuid Client that asked for the switch.
   * @param target Profile to switch to.
   * @return Global indices of that client's pads that present another profile, in slot order.
   */
  inline std::vector<int> plan_profile_switch(const std::vector<pad_slot_t> &slots, const std::string_view client_uuid, const profile_e target) {
    std::vector<int> recreate;
    for (const auto &slot : slots) {
      if (slot.client_uuid == client_uuid && slot.profile != target) {
        recreate.push_back(slot.nr);
      }
    }
    return recreate;
  }

  /**
   * @brief How a "switch to profile" command shows in a client's menu.
   */
  enum class switch_menu_e {
    hidden,  ///< Not offered: the client has no xgpad pads, or the driver can't present the profile.
    available,  ///< Offered: at least one of the client's pads presents another profile.
    active,  ///< Offered and marked: all of the client's pads already present the profile.
  };

  /**
   * @brief Decide how a "switch to profile" command shows for a client.
   * @param slots Occupied xgpad slots.
   * @param client_uuid Client opening its menu.
   * @param target Profile the command switches to.
   * @param dualsense_supported Whether the installed driver has the DualSense profile.
   * @return Menu state of the command.
   */
  inline switch_menu_e switch_menu_state(const std::vector<pad_slot_t> &slots, const std::string_view client_uuid, const profile_e target, const bool dualsense_supported) {
    if (client_uuid.empty() || (target == profile_e::dualsense && !dualsense_supported)) {
      return switch_menu_e::hidden;
    }
    bool any = false;
    bool all_match = true;
    for (const auto &slot : slots) {
      if (slot.client_uuid == client_uuid) {
        any = true;
        all_match &= slot.profile == target;
      }
    }
    if (!any) {
      return switch_menu_e::hidden;
    }
    return all_match ? switch_menu_e::active : switch_menu_e::available;
  }

  /**
   * @brief How long a rumble output report lasts before the controller stops on its own.
   *
   * Duration and start delay are in 10 ms units; the effect repeats loopCount additional times. A duration of
   * 0xFF (what XInput, Windows.Gaming.Input and GameInput send) or 0 means "until changed".
   *
   * @param report Output report written by the game through the HID stack.
   * @return Milliseconds until the rumble should stop, or 0 for no automatic stop.
   */
  constexpr std::uint32_t rumble_duration_ms(const XGPAD_OUTPUT_REPORT &report) {
    if (report.duration == 0xFF || report.duration == 0) {
      return 0;
    }
    return (static_cast<std::uint32_t>(report.startDelay) + report.duration) * (static_cast<std::uint32_t>(report.loopCount) + 1u) * 10u;
  }

  /**
   * @brief Apply an output report (ID 0x03 payload) to the current rumble state.
   *
   * Motors whose enable bit is clear keep their previous strength.
   *
   * @param report Output report written by the game through the HID stack.
   * @param previous Rumble state before this report.
   * @return Updated rumble state.
   */
  constexpr rumble_state_t apply_output_report(const XGPAD_OUTPUT_REPORT &report, const rumble_state_t &previous) {
    rumble_state_t state = previous;
    if (report.enable & XGPAD_MOTOR_LEFT) {
      state.low_freq = motor_from_percent(report.leftMotor);
    }
    if (report.enable & XGPAD_MOTOR_RIGHT) {
      state.high_freq = motor_from_percent(report.rightMotor);
    }
    if (report.enable & XGPAD_MOTOR_LEFT_TRIGGER) {
      state.left_trigger = motor_from_percent(report.leftTrigger);
    }
    if (report.enable & XGPAD_MOTOR_RIGHT_TRIGGER) {
      state.right_trigger = motor_from_percent(report.rightTrigger);
    }
    return state;
  }

  /**
   * @brief Append an input report to a pad's shared input ring and publish it.
   *
   * The ring has a single writer (Sunshine's input thread). The driver reads entries in order and drops any whose
   * ID or length does not match the pad's profile.
   *
   * @param shared Shared section of the pad.
   * @param id Report ID.
   * @param payload Report payload, report ID not included; at most XGPAD_MAX_REPORT bytes are used.
   * @return Number the report was published as (the new inputCount).
   */
  inline long publish_input_report(XGPAD_SHARED &shared, const std::uint8_t id, const std::span<const std::uint8_t> payload) {
    const long count = shared.inputCount + 1;
    auto &slot = shared.inputRing[(count - 1) % XGPAD_INPUT_RING_SIZE];
    const auto length = payload.size() < XGPAD_MAX_REPORT ? payload.size() : std::size_t {XGPAD_MAX_REPORT};
    slot.id = id;
    slot.length = static_cast<std::uint8_t>(length);
    std::memcpy(slot.data, payload.data(), length);
    std::atomic_thread_fence(std::memory_order_release);
    shared.inputCount = count;  // publish
    return count;
  }

  /**
   * @brief Append an Xbox Series input report to a pad's shared input ring and publish it.
   * @param shared Shared section of the pad.
   * @param report Input report payload.
   * @return Number the report was published as (the new inputCount).
   */
  inline long publish_input_report(XGPAD_SHARED &shared, const XGPAD_INPUT_REPORT &report) {
    return publish_input_report(shared, XGPAD_INPUT_REPORT_ID, std::span {reinterpret_cast<const std::uint8_t *>(&report), sizeof(report)});
  }

  /**
   * @brief Copy the output reports the driver published since the last call, oldest first.
   *
   * Reports the driver overwrote before they could be read (more than XGPAD_OUTPUT_RING_SIZE behind), or that were
   * overwritten while being copied, are skipped and counted as lost.
   *
   * @param shared Shared section of the pad.
   * @param consumed Number of output reports already read; updated to the number now read.
   * @param lost Incremented by the number of reports skipped, if not null.
   * @return The new reports, oldest first.
   */
  inline std::vector<XGPAD_REPORT> read_output_reports(const XGPAD_SHARED &shared, long &consumed, long *lost = nullptr) {
    std::vector<XGPAD_REPORT> reports;
    const long available = shared.outputCount;
    std::atomic_thread_fence(std::memory_order_acquire);

    long next = consumed;
    if (available < next) {
      next = available;  // the counter restarted
    }
    if (static_cast<unsigned long>(available - next) > XGPAD_OUTPUT_RING_SIZE) {
      if (lost) {
        *lost += available - next - static_cast<long>(XGPAD_OUTPUT_RING_SIZE);
      }
      next = available - static_cast<long>(XGPAD_OUTPUT_RING_SIZE);
    }

    while (next < available) {
      const long n = next + 1;
      XGPAD_REPORT report;
      std::memcpy(&report, const_cast<const XGPAD_REPORT *>(&shared.outputRing[(n - 1) % XGPAD_OUTPUT_RING_SIZE]), sizeof(report));
      std::atomic_thread_fence(std::memory_order_acquire);
      if (static_cast<unsigned long>(shared.outputCount - n) >= XGPAD_OUTPUT_RING_SIZE) {
        if (lost) {
          ++*lost;  // the driver lapped this slot while it was copied
        }
      } else {
        reports.push_back(report);
      }
      next = n;
    }
    consumed = next;
    return reports;
  }

  /**
   * @brief Extract an Xbox Series rumble report from a ring entry.
   * @param report Output ring entry.
   * @param out Receives the rumble payload.
   * @return True when the entry is an Xbox rumble report (ID 0x03, at least 8 bytes).
   */
  inline bool xbox_output_report(const XGPAD_REPORT &report, XGPAD_OUTPUT_REPORT &out) {
    if (report.id != XGPAD_OUTPUT_REPORT_ID || report.length < sizeof(XGPAD_OUTPUT_REPORT)) {
      return false;
    }
    std::memcpy(&out, report.data, sizeof(out));
    return true;
  }

  /**
   * @brief How long a new pad may take for the driver to open its shared objects.
   *
   * The driver polls for them every 250 ms, so a working driver attaches well within this window.
   */
  constexpr std::chrono::milliseconds driver_attach_timeout {3000};

  /**
   * @brief Whether the xgpad driver has picked up a pad's shared memory.
   */
  enum class attach_state_e {
    pending,  ///< Not attached yet, still within driver_attach_timeout.
    attached,  ///< The driver set XGPAD_STATUS_DRIVER_STARTED.
    missing,  ///< Not attached after driver_attach_timeout; the installed driver likely does not match.
  };

  /**
   * @brief Classify a pad's driver attachment.
   *
   * A driver built for another XGPAD_SHARED_VERSION rejects the section without an error, so the device
   * still enumerates while every input report is ignored. This turns that silent state into something
   * Sunshine can log.
   *
   * @param driver_status XGPAD_SHARED::driverStatus.
   * @param since_created Time since the pad's device was created.
   * @return Attachment state.
   */
  constexpr attach_state_e driver_attach_state(const long driver_status, const std::chrono::steady_clock::duration since_created) {
    if (driver_status & XGPAD_STATUS_DRIVER_STARTED) {
      return attach_state_e::attached;
    }
    return since_created >= driver_attach_timeout ? attach_state_e::missing : attach_state_e::pending;
  }

  /**
   * @brief Format a packed driver version (SP_DRVINFO_DATA::DriverVersion).
   *
   * @param version Four 16-bit parts, most significant first.
   * @return Dotted version text.
   */
  inline std::string driver_version_text(const std::uint64_t version) {
    return std::format("{}.{}.{}.{}", (version >> 48) & 0xFFFF, (version >> 32) & 0xFFFF, (version >> 16) & 0xFFFF, version & 0xFFFF);
  }

}  // namespace platf::xgpad
