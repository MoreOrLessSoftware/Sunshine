/**
 * @file src/platform/windows/xgpad_ds5_report.h
 * @brief Conversions between Moonlight controller input and xgpad DualSense HID reports.
 *
 * Kept free of Windows APIs so the mapping can be unit tested on its own.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

// local includes
#include "drivers/xgpad/xgpad_shared.h"
#include "playstation_report.h"
#include "src/platform/common.h"

namespace platf::xgpad {

  /**
   * @brief One finger on the virtual DualSense touchpad.
   */
  struct ds5_touch_point_t {
    bool down = false;  ///< Whether the finger touches the pad.
    std::uint8_t tracking = 0;  ///< Tracking number (0..127), new for every touch.
    std::uint16_t x = 0;  ///< X in touchpad units (0..1919).
    std::uint16_t y = 0;  ///< Y in touchpad units (0..1079).
  };

  /**
   * @brief Everything a virtual DualSense reports, kept between client events.
   */
  struct ds5_state_t {
    gamepad_state_t gamepad {};  ///< Buttons, sticks and triggers.
    std::array<std::int16_t, 3> gyro {};  ///< Gyro counts (pitch, yaw, roll).
    std::array<std::int16_t, 3> accel {0, playstation::accel_counts_per_g, 0};  ///< Accelerometer counts; at rest the pad lies flat.
    std::array<ds5_touch_point_t, playstation::max_touch_points> touch {};  ///< Touch points by slot.
    std::uint8_t status = (XGPAD_DS_POWER_FULL << 4) | 10;  ///< Battery level (low nibble, 0..10) and power state (high nibble).
    bool back_as_touchpad = false;  ///< Back also clicks the touchpad (config::input.ds4_back_as_touchpad_click).
  };

  /**
   * @brief Convert Moonlight D-pad button flags to the DualSense hat value.
   *
   * Opposing directions resolve to up and left respectively.
   *
   * @param button_flags Moonlight button mask.
   * @return 0 (N) to 7 (NW) clockwise, or XGPAD_DS_HAT_RELEASED.
   */
  constexpr std::uint8_t ds5_hat_from_buttons(const std::uint32_t button_flags) {
    const bool up = button_flags & DPAD_UP;
    const bool down = !up && (button_flags & DPAD_DOWN);
    const bool left = button_flags & DPAD_LEFT;
    const bool right = !left && (button_flags & DPAD_RIGHT);

    if (up) {
      return right ? 1 : left ? 7 :
                                0;
    }
    if (down) {
      return right ? 3 : left ? 5 :
                                4;
    }
    if (right) {
      return 2;
    }
    if (left) {
      return 6;
    }
    return XGPAD_DS_HAT_RELEASED;
  }

  /**
   * @brief Convert Moonlight buttons and triggers to the DualSense button bytes.
   *
   * Face buttons keep their position (A = Cross, B = Circle, X = Square, Y = Triangle). Back is Create, Start is
   * Options, Guide is PS, and the Moonlight misc button (Xbox Share, Switch Capture, DualSense mic) is the mic
   * button. L2 and R2 report pressed while their trigger is off its rest position. Paddles are not mapped.
   *
   * @param state Gamepad state from the client.
   * @param back_as_touchpad Back also clicks the touchpad, for clients whose controllers have no touchpad.
   * @return buttons[0] (hat and face buttons), buttons[1] and buttons[2] of the input report.
   */
  constexpr std::array<std::uint8_t, 3> ds5_buttons(const gamepad_state_t &state, const bool back_as_touchpad = false) {
    const auto flags = state.buttonFlags;
    std::array<std::uint8_t, 3> buttons {ds5_hat_from_buttons(flags), 0, 0};
    const auto map = [&](const std::uint32_t flag, const std::size_t index, const std::uint8_t bit) {
      if (flags & flag) {
        buttons[index] |= bit;
      }
    };
    map(X, 0, XGPAD_DS_BTN0_SQUARE);
    map(A, 0, XGPAD_DS_BTN0_CROSS);
    map(B, 0, XGPAD_DS_BTN0_CIRCLE);
    map(Y, 0, XGPAD_DS_BTN0_TRIANGLE);
    map(LEFT_BUTTON, 1, XGPAD_DS_BTN1_L1);
    map(RIGHT_BUTTON, 1, XGPAD_DS_BTN1_R1);
    map(BACK, 1, XGPAD_DS_BTN1_CREATE);
    map(START, 1, XGPAD_DS_BTN1_OPTIONS);
    map(LEFT_STICK, 1, XGPAD_DS_BTN1_L3);
    map(RIGHT_STICK, 1, XGPAD_DS_BTN1_R3);
    map(HOME, 2, XGPAD_DS_BTN2_PS);
    map(TOUCHPAD_BUTTON, 2, XGPAD_DS_BTN2_TOUCHPAD);
    map(MISC_BUTTON, 2, XGPAD_DS_BTN2_MIC);
    if (state.lt > 0) {
      buttons[1] |= XGPAD_DS_BTN1_L2;
    }
    if (state.rt > 0) {
      buttons[1] |= XGPAD_DS_BTN1_R2;
    }
    if (back_as_touchpad && (flags & BACK)) {
      buttons[2] |= XGPAD_DS_BTN2_TOUCHPAD;
    }
    return buttons;
  }

  /**
   * @brief Convert a signed Moonlight X axis (positive = right) to the DualSense's 8-bit axis.
   * @param value Axis value -32768..32767.
   * @return 0..255, 128 at rest.
   */
  constexpr std::uint8_t ds5_axis_x(const std::int16_t value) {
    return static_cast<std::uint8_t>((static_cast<std::int32_t>(value) + 32768) >> 8);
  }

  /**
   * @brief Convert a signed Moonlight Y axis (positive = up) to the DualSense's 8-bit axis (increasing downwards).
   * @param value Axis value -32768..32767.
   * @return 0..255, 127 at rest.
   */
  constexpr std::uint8_t ds5_axis_y(const std::int16_t value) {
    return static_cast<std::uint8_t>((32767 - static_cast<std::int32_t>(value)) >> 8);
  }

  /**
   * @brief Build the 63-byte input report (ID 0x01 payload) for a DualSense state.
   * @param state Current DualSense state.
   * @param seq Report counter, incremented by the caller for every report.
   * @param timestamp Sensor timestamp in 0.33 us units.
   * @return Input report payload.
   */
  inline XGPAD_DS_INPUT_REPORT make_ds5_input_report(const ds5_state_t &state, const std::uint8_t seq, const std::uint32_t timestamp) {
    XGPAD_DS_INPUT_REPORT report {};
    report.lx = ds5_axis_x(state.gamepad.lsX);
    report.ly = ds5_axis_y(state.gamepad.lsY);
    report.rx = ds5_axis_x(state.gamepad.rsX);
    report.ry = ds5_axis_y(state.gamepad.rsY);
    report.l2 = state.gamepad.lt;
    report.r2 = state.gamepad.rt;
    report.seq = seq;
    const auto buttons = ds5_buttons(state.gamepad, state.back_as_touchpad);
    std::memcpy(report.buttons, buttons.data(), buttons.size());
    std::memcpy(report.gyro, state.gyro.data(), sizeof(report.gyro));
    std::memcpy(report.accel, state.accel.data(), sizeof(report.accel));
    report.sensorTimestamp = timestamp;
    for (std::size_t slot = 0; slot < state.touch.size(); ++slot) {
      const auto &point = state.touch[slot];
      const auto packed = playstation::pack_touch(point.x, point.y);
      report.points[slot].contact = static_cast<std::uint8_t>((point.tracking & 0x7F) | (point.down ? 0 : XGPAD_DS_TOUCH_NOT_TOUCHING));
      report.points[slot].xLow = packed[0];
      report.points[slot].xHighYLow = packed[1];
      report.points[slot].yHigh = packed[2];
    }
    report.status = state.status;
    return report;
  }

  /**
   * @brief Store a motion sample from the client.
   * @param state DualSense state to update.
   * @param motion_type LI_MOTION_TYPE_ACCEL (m/s^2) or LI_MOTION_TYPE_GYRO (deg/s).
   * @param x X component.
   * @param y Y component.
   * @param z Z component.
   * @return True when the sample was stored, false for an unknown motion type.
   */
  inline bool ds5_apply_motion(ds5_state_t &state, const std::uint8_t motion_type, const float x, const float y, const float z) {
    playstation::motion_counts_t counts;
    std::array<std::int16_t, 3> *target;
    switch (motion_type) {
      case LI_MOTION_TYPE_ACCEL:
        counts = playstation::accel_counts(x, y, z);
        target = &state.accel;
        break;
      case LI_MOTION_TYPE_GYRO:
        counts = playstation::gyro_counts(x, y, z);
        target = &state.gyro;
        break;
      default:
        return false;
    }
    *target = {playstation::clamp_i16(counts.x), playstation::clamp_i16(counts.y), playstation::clamp_i16(counts.z)};
    return true;
  }

  /**
   * @brief Apply a touch event from the client.
   * @param state DualSense state to update.
   * @param slots Touch slot assignment of the pad.
   * @param touch Touch event; x and y are normalized to 0.0..1.0.
   * @return True when the state changed, false for an event that could not be applied (no free slot, unknown
   *         pointer or unsupported event type).
   */
  inline bool ds5_apply_touch(ds5_state_t &state, playstation::touch_slots_t &slots, const gamepad_touch_t &touch) {
    const auto x = playstation::touch_coordinate(touch.x, XGPAD_DS_TOUCHPAD_WIDTH);
    const auto y = playstation::touch_coordinate(touch.y, XGPAD_DS_TOUCHPAD_HEIGHT);

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_DOWN:
        {
          const auto slot = slots.down(touch.pointerId);
          if (!slot) {
            return false;
          }
          auto &point = state.touch[*slot];
          if (!point.down) {
            point.tracking = static_cast<std::uint8_t>((point.tracking + 1) & 0x7F);
          }
          point = {true, point.tracking, x, y};
          return true;
        }
      case LI_TOUCH_EVENT_MOVE:
        {
          const auto slot = slots.find(touch.pointerId);
          if (!slot) {
            return false;
          }
          state.touch[*slot].x = x;
          state.touch[*slot].y = y;
          return true;
        }
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_CANCEL:
        {
          const auto slot = slots.up(touch.pointerId);
          if (!slot) {
            return false;
          }
          state.touch[*slot].down = false;
          return true;
        }
      case LI_TOUCH_EVENT_CANCEL_ALL:
        slots.clear();
        for (auto &point : state.touch) {
          point.down = false;
        }
        return true;
      default:
        return false;
    }
  }

  /**
   * @brief Combine a battery event from the client with the current status byte.
   *
   * See the DualSense status byte in hid-playstation: the low nibble is the level (0..10), the high nibble the power
   * state. An unknown state or percentage keeps that part of the status. The level is the percentage divided by ten,
   * rounded down, which inverts SDL's DualSense mapping (`level * 10 + 5`) exactly.
   *
   * @param status Current status byte.
   * @param state LI_BATTERY_STATE_* from the client.
   * @param percentage Battery percentage, or LI_BATTERY_PERCENTAGE_UNKNOWN.
   * @return New status byte.
   */
  constexpr std::uint8_t ds5_battery_status(const std::uint8_t status, const std::uint8_t state, const std::uint8_t percentage) {
    constexpr std::uint8_t charging_error = 0xF;
    std::uint8_t power = status >> 4;
    std::uint8_t level = status & 0x0F;

    switch (state) {
      case LI_BATTERY_STATE_DISCHARGING:
        power = XGPAD_DS_POWER_DISCHARGING;
        break;
      case LI_BATTERY_STATE_CHARGING:
        power = XGPAD_DS_POWER_CHARGING;
        break;
      case LI_BATTERY_STATE_FULL:
        power = XGPAD_DS_POWER_FULL;
        level = 10;
        break;
      case LI_BATTERY_STATE_NOT_PRESENT:
      case LI_BATTERY_STATE_NOT_CHARGING:
        power = charging_error;
        break;
      default:
        break;
    }
    if (percentage != LI_BATTERY_PERCENTAGE_UNKNOWN && state != LI_BATTERY_STATE_FULL) {
      const unsigned tenths = static_cast<unsigned>(percentage) / 10u;
      level = static_cast<std::uint8_t>(tenths > 10u ? 10u : tenths);
    }
    return static_cast<std::uint8_t>((power << 4) | (level & 0x0F));
  }

  /**
   * @brief Convert the DualSense's five player LEDs to Moonlight's four-bit player mask.
   *
   * The patterns hosts use for players 1 to 4 (centre; inner pair; centre and outer pair; both pairs) become that
   * player's bit. Any other pattern keeps the four outer LEDs as bits 0..3 and drops the centre one.
   *
   * @param leds DualSense player LED bits 0..4, left to right.
   * @return Moonlight player LED mask.
   */
  constexpr std::uint8_t ds5_player_mask(const std::uint8_t leds) {
    switch (leds & 0x1F) {
      case 0x04:
        return 0x1;
      case 0x0A:
        return 0x2;
      case 0x15:
        return 0x4;
      case 0x1B:
        return 0x8;
      default:
        return static_cast<std::uint8_t>((leds & 0x03) | ((leds >> 1) & 0x0C));
    }
  }

  /**
   * @brief Adaptive trigger effects from a DualSense output report.
   */
  struct ds5_triggers_t {
    std::uint8_t flags;  ///< XGPAD_DS_FLAG0_RIGHT_TRIGGER_EFFECT and/or XGPAD_DS_FLAG0_LEFT_TRIGGER_EFFECT.
    std::uint8_t type_left;  ///< Left effect mode.
    std::uint8_t type_right;  ///< Right effect mode.
    std::array<std::uint8_t, 10> left;  ///< Left effect parameters.
    std::array<std::uint8_t, 10> right;  ///< Right effect parameters.
  };

  /**
   * @brief What a DualSense output report asks the controller to do; fields the report leaves alone are empty.
   */
  struct ds5_output_t {
    std::optional<std::array<std::uint16_t, 2>> rumble;  ///< Low and high frequency motors in Moonlight's 0..65535 range.
    std::optional<ds5_triggers_t> triggers;  ///< Adaptive trigger effects.
    std::optional<std::array<std::uint8_t, 3>> lightbar;  ///< Lightbar red, green, blue.
    std::optional<std::uint8_t> player_leds;  ///< DualSense player LED bits 0..4, left to right.
    std::optional<std::uint8_t> mic_led;  ///< Mute LED: 0 = off, 1 = on, 2 = pulsing.
  };

  /**
   * @brief Parse a DualSense output report (ID 0x02) from the driver's output ring.
   *
   * Rumble counts when either compatible vibration flag is set; a report with no valid flags at all is a stop, as
   * some hosts send it that way.
   *
   * @param entry Output ring entry.
   * @return Requested changes, or nothing when the entry is not a complete DualSense output report.
   */
  inline std::optional<ds5_output_t> parse_ds5_output(const XGPAD_REPORT &entry) {
    if (entry.id != XGPAD_DS_OUTPUT_REPORT_ID || entry.length < sizeof(XGPAD_DS_OUTPUT_REPORT)) {
      return std::nullopt;
    }
    XGPAD_DS_OUTPUT_REPORT report;
    std::memcpy(&report, entry.data, sizeof(report));

    ds5_output_t output;
    const auto scale = [](const std::uint8_t value) {
      return static_cast<std::uint16_t>(value * 257u);
    };
    if ((report.validFlag0 & XGPAD_DS_FLAG0_COMPATIBLE_VIBRATION) || (report.validFlag2 & XGPAD_DS_FLAG2_COMPATIBLE_VIBRATION2)) {
      output.rumble = std::array {scale(report.motorLeft), scale(report.motorRight)};
    } else if (report.validFlag0 == 0 && report.validFlag1 == 0 && report.validFlag2 == 0) {
      output.rumble = std::array<std::uint16_t, 2> {0, 0};
    }

    if (const auto flags = report.validFlag0 & (XGPAD_DS_FLAG0_LEFT_TRIGGER_EFFECT | XGPAD_DS_FLAG0_RIGHT_TRIGGER_EFFECT)) {
      ds5_triggers_t triggers {static_cast<std::uint8_t>(flags), report.leftTriggerEffect[0], report.rightTriggerEffect[0], {}, {}};
      std::memcpy(triggers.left.data(), report.leftTriggerEffect + 1, triggers.left.size());
      std::memcpy(triggers.right.data(), report.rightTriggerEffect + 1, triggers.right.size());
      output.triggers = triggers;
    }

    if (report.validFlag1 & XGPAD_DS_FLAG1_LIGHTBAR) {
      output.lightbar = std::array {report.lightbarRed, report.lightbarGreen, report.lightbarBlue};
    }
    if (report.validFlag1 & XGPAD_DS_FLAG1_PLAYER_LEDS) {
      output.player_leds = static_cast<std::uint8_t>(report.playerLeds & 0x1F);
    }
    if (report.validFlag1 & XGPAD_DS_FLAG1_MIC_MUTE_LED) {
      output.mic_led = static_cast<std::uint8_t>(report.muteButtonLed > 2 ? 2 : report.muteButtonLed);
    }
    return output;
  }

}  // namespace platf::xgpad
