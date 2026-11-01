/**
 * @file src/platform/windows/xgpad_report.h
 * @brief Conversions between Moonlight gamepad state and xgpad (virtual Xbox Series controller) HID reports.
 *
 * Kept free of Windows APIs so the mapping can be unit tested on its own.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string_view>

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
  };

  /**
   * @brief Decide whether a controller should be created through xgpad (when the xgpad context exists).
   *
   * With an explicit "xgpad" policy every controller is an Xbox Series controller. Under "all", xgpad covers
   * the Xbox profiles while ViGEmBus keeps what only it can do: DualShock 4 (explicitly, or automatically
   * for PlayStation clients and the motion/touchpad-as-DS4 options) and an explicit Xbox 360 choice.
   *
   * @param choice Policy, profile, client metadata and ViGEmBus availability.
   * @return True to allocate through xgpad, false to use the remaining drivers.
   */
  constexpr bool should_allocate_xgpad(const allocation_choice_t &choice) {
    if (choice.driver_policy == "xgpad") {
      return true;
    }
    if (!choice.driver_policy.empty() && choice.driver_policy != "all") {
      return false;  // "vigembus" / "virtualhid" never use xgpad
    }

    const auto &gamepad = choice.gamepad;
    if (gamepad == "xseries" || gamepad == "xone") {
      return true;
    }
    if (gamepad == "x360") {
      return !choice.vigem_available;
    }
    if (gamepad != "auto") {
      return false;  // ds4, ds5, switch, generic: not an Xbox profile
    }

    // Automatic selection: leave controllers ViGEmBus would make a DualShock 4 to ViGEmBus
    if (choice.vigem_available) {
      constexpr std::uint8_t ctype_ps = 0x02;  // LI_CTYPE_PS
      constexpr std::uint16_t ccap_touchpad = 0x08;  // LI_CCAP_TOUCHPAD
      constexpr std::uint16_t ccap_motion = 0x10 | 0x20;  // LI_CCAP_ACCEL | LI_CCAP_GYRO
      if (choice.client_type == ctype_ps ||
          (choice.motion_as_ds4 && (choice.client_capabilities & ccap_motion)) ||
          (choice.touchpad_as_ds4 && (choice.client_capabilities & ccap_touchpad))) {
        return false;
      }
    }
    return true;
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

}  // namespace platf::xgpad
