/**
 * @file tests/unit/platform/windows/test_xgpad_ds5.cpp
 * @brief Test the xgpad DualSense report conversions.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <cstdint>
  #include <cstring>

  // local includes
  #include <src/platform/windows/xgpad_ds5_report.h>

using namespace platf;
using namespace platf::xgpad;

/**
 * @brief Test fixture for the xgpad DualSense conversions.
 */
class XgpadDs5Test: public BaseTest {};

namespace {
  gamepad_state_t with_buttons(const std::uint32_t flags) {
    gamepad_state_t state {};
    state.buttonFlags = flags;
    return state;
  }

  gamepad_touch_t touch_event(const std::uint8_t type, const std::uint32_t pointer, const float x = 0.0f, const float y = 0.0f) {
    gamepad_touch_t touch {};
    touch.eventType = type;
    touch.pointerId = pointer;
    touch.x = x;
    touch.y = y;
    return touch;
  }

  XGPAD_REPORT output_entry(const XGPAD_DS_OUTPUT_REPORT &report) {
    XGPAD_REPORT entry {};
    entry.id = XGPAD_DS_OUTPUT_REPORT_ID;
    entry.length = sizeof(report);
    std::memcpy(entry.data, &report, sizeof(report));
    return entry;
  }
}  // namespace

TEST_F(XgpadDs5Test, HatSwitch) {
  EXPECT_EQ(ds5_hat_from_buttons(0), XGPAD_DS_HAT_RELEASED);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_UP), 0);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_UP | DPAD_RIGHT), 1);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_RIGHT), 2);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_DOWN | DPAD_RIGHT), 3);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_DOWN), 4);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_DOWN | DPAD_LEFT), 5);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_LEFT), 6);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_UP | DPAD_LEFT), 7);
  // Opposing directions resolve to up and left
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_UP | DPAD_DOWN), 0);
  EXPECT_EQ(ds5_hat_from_buttons(DPAD_LEFT | DPAD_RIGHT), 6);
}

TEST_F(XgpadDs5Test, ButtonMapping) {
  const auto check = [](const std::uint32_t flag, const std::size_t index, const std::uint8_t bit) {
    auto buttons = ds5_buttons(with_buttons(flag));
    buttons[0] &= 0xF0;  // ignore the hat
    std::array<std::uint8_t, 3> expected {0, 0, 0};
    expected[index] = bit;
    EXPECT_EQ(buttons, expected) << "flag " << flag;
  };
  check(X, 0, XGPAD_DS_BTN0_SQUARE);
  check(A, 0, XGPAD_DS_BTN0_CROSS);
  check(B, 0, XGPAD_DS_BTN0_CIRCLE);
  check(Y, 0, XGPAD_DS_BTN0_TRIANGLE);
  check(LEFT_BUTTON, 1, XGPAD_DS_BTN1_L1);
  check(RIGHT_BUTTON, 1, XGPAD_DS_BTN1_R1);
  check(BACK, 1, XGPAD_DS_BTN1_CREATE);
  check(START, 1, XGPAD_DS_BTN1_OPTIONS);
  check(LEFT_STICK, 1, XGPAD_DS_BTN1_L3);
  check(RIGHT_STICK, 1, XGPAD_DS_BTN1_R3);
  check(HOME, 2, XGPAD_DS_BTN2_PS);
  check(TOUCHPAD_BUTTON, 2, XGPAD_DS_BTN2_TOUCHPAD);
  check(MISC_BUTTON, 2, XGPAD_DS_BTN2_MIC);

  // Paddles have no DualSense button
  EXPECT_EQ(ds5_buttons(with_buttons(PADDLE1 | PADDLE2 | PADDLE3 | PADDLE4)), (std::array<std::uint8_t, 3> {XGPAD_DS_HAT_RELEASED, 0, 0}));

  // The hat shares the first byte with the face buttons
  EXPECT_EQ(ds5_buttons(with_buttons(A | DPAD_DOWN))[0], XGPAD_DS_BTN0_CROSS | 4);
}

TEST_F(XgpadDs5Test, BackAsTouchpadClick) {
  EXPECT_EQ(ds5_buttons(with_buttons(BACK))[2], 0);
  EXPECT_EQ(ds5_buttons(with_buttons(BACK), true)[2], XGPAD_DS_BTN2_TOUCHPAD);
  EXPECT_EQ(ds5_buttons(with_buttons(BACK), true)[1], XGPAD_DS_BTN1_CREATE);  // Create stays pressed too
  EXPECT_EQ(ds5_buttons(with_buttons(START), true)[2], 0);

  ds5_state_t state {};
  state.back_as_touchpad = true;
  state.gamepad.buttonFlags = BACK;
  EXPECT_EQ(make_ds5_input_report(state, 0, 0).buttons[2], XGPAD_DS_BTN2_TOUCHPAD);
}

TEST_F(XgpadDs5Test, TriggerButtons) {
  gamepad_state_t state {};
  EXPECT_EQ(ds5_buttons(state)[1], 0);
  state.lt = 1;
  EXPECT_EQ(ds5_buttons(state)[1], XGPAD_DS_BTN1_L2);
  state.lt = 0;
  state.rt = 255;
  EXPECT_EQ(ds5_buttons(state)[1], XGPAD_DS_BTN1_R2);
}

TEST_F(XgpadDs5Test, Axes) {
  EXPECT_EQ(ds5_axis_x(0), 128);
  EXPECT_EQ(ds5_axis_x(-32768), 0);
  EXPECT_EQ(ds5_axis_x(32767), 255);
  EXPECT_EQ(ds5_axis_y(0), 127);
  EXPECT_EQ(ds5_axis_y(32767), 0);  // up
  EXPECT_EQ(ds5_axis_y(-32768), 255);  // down
}

TEST_F(XgpadDs5Test, InputReportAtRest) {
  const auto report = make_ds5_input_report(ds5_state_t {}, 7, 123456);
  EXPECT_EQ(report.lx, 128);
  EXPECT_EQ(report.ly, 127);
  EXPECT_EQ(report.rx, 128);
  EXPECT_EQ(report.ry, 127);
  EXPECT_EQ(report.l2, 0);
  EXPECT_EQ(report.r2, 0);
  EXPECT_EQ(report.seq, 7);
  EXPECT_EQ(report.buttons[0], XGPAD_DS_HAT_RELEASED);
  EXPECT_EQ(report.buttons[1], 0);
  EXPECT_EQ(report.buttons[2], 0);
  EXPECT_EQ(report.buttons[3], 0);
  EXPECT_EQ(report.gyro[0], 0);
  EXPECT_EQ(report.accel[1], playstation::accel_counts_per_g);  // lying flat
  EXPECT_EQ(report.sensorTimestamp, 123456u);
  EXPECT_EQ(report.points[0].contact, XGPAD_DS_TOUCH_NOT_TOUCHING);
  EXPECT_EQ(report.points[1].contact, XGPAD_DS_TOUCH_NOT_TOUCHING);
  EXPECT_EQ(report.status, (XGPAD_DS_POWER_FULL << 4) | 10);
}

TEST_F(XgpadDs5Test, InputReportCarriesState) {
  ds5_state_t state {};
  state.gamepad.buttonFlags = Y | HOME;
  state.gamepad.lt = 200;
  state.gamepad.rt = 10;
  state.gamepad.lsX = 32767;
  state.gamepad.rsY = 32767;
  state.gyro = {1, -2, 3};
  state.accel = {-4, 5, -6};
  state.touch[1] = {true, 5, 1919, 1079};
  state.status = 0x13;

  const auto report = make_ds5_input_report(state, 255, 0xFFFFFFFF);
  EXPECT_EQ(report.lx, 255);
  EXPECT_EQ(report.ry, 0);
  EXPECT_EQ(report.l2, 200);
  EXPECT_EQ(report.r2, 10);
  EXPECT_EQ(report.seq, 255);
  EXPECT_EQ(report.buttons[0], XGPAD_DS_BTN0_TRIANGLE | XGPAD_DS_HAT_RELEASED);
  EXPECT_EQ(report.buttons[1], XGPAD_DS_BTN1_L2 | XGPAD_DS_BTN1_R2);
  EXPECT_EQ(report.buttons[2], XGPAD_DS_BTN2_PS);
  EXPECT_EQ(report.gyro[0], 1);
  EXPECT_EQ(report.gyro[1], -2);
  EXPECT_EQ(report.gyro[2], 3);
  EXPECT_EQ(report.accel[0], -4);
  EXPECT_EQ(report.accel[2], -6);
  EXPECT_EQ(report.sensorTimestamp, 0xFFFFFFFFu);
  EXPECT_EQ(report.points[0].contact, XGPAD_DS_TOUCH_NOT_TOUCHING);
  EXPECT_EQ(report.points[1].contact, 5);
  EXPECT_EQ(report.points[1].xLow | ((report.points[1].xHighYLow & 0x0F) << 8), 1919);
  EXPECT_EQ((report.points[1].xHighYLow >> 4) | (report.points[1].yHigh << 4), 1079);
  EXPECT_EQ(report.status, 0x13);
}

TEST_F(XgpadDs5Test, MotionIsScaledAndClamped) {
  ds5_state_t state {};
  EXPECT_TRUE(ds5_apply_motion(state, LI_MOTION_TYPE_GYRO, 1.0f, -90.0f, 5000.0f));
  EXPECT_EQ(state.gyro, (std::array<std::int16_t, 3> {16, -1440, INT16_MAX}));

  EXPECT_TRUE(ds5_apply_motion(state, LI_MOTION_TYPE_ACCEL, 0.0f, -playstation::earth_g, playstation::earth_g * 8));
  EXPECT_EQ(state.accel, (std::array<std::int16_t, 3> {0, -8192, INT16_MAX}));

  const auto before = state;
  EXPECT_FALSE(ds5_apply_motion(state, 0x7F, 1.0f, 1.0f, 1.0f));
  EXPECT_EQ(state.gyro, before.gyro);
  EXPECT_EQ(state.accel, before.accel);
}

TEST_F(XgpadDs5Test, TouchDownMoveUp) {
  ds5_state_t state {};
  playstation::touch_slots_t slots;

  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_DOWN, 42, 0.5f, 0.5f)));
  EXPECT_TRUE(state.touch[0].down);
  EXPECT_EQ(state.touch[0].tracking, 1);
  EXPECT_EQ(state.touch[0].x, 960);
  EXPECT_EQ(state.touch[0].y, 540);

  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_MOVE, 42, 1.0f, 0.0f)));
  EXPECT_EQ(state.touch[0].x, 1919);
  EXPECT_EQ(state.touch[0].y, 0);
  EXPECT_EQ(state.touch[0].tracking, 1);

  // A repeated down for the same pointer moves it without a new tracking number
  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_DOWN, 42, 0.25f, 0.25f)));
  EXPECT_EQ(state.touch[0].tracking, 1);
  EXPECT_EQ(state.touch[0].x, 480);

  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_UP, 42)));
  EXPECT_FALSE(state.touch[0].down);
  EXPECT_EQ(state.touch[0].x, 480);  // the last position stays, like on a real pad

  // The next touch in the slot gets a new tracking number
  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_DOWN, 43)));
  EXPECT_EQ(state.touch[0].tracking, 2);
}

TEST_F(XgpadDs5Test, TouchTrackingNumberWraps) {
  ds5_state_t state {};
  playstation::touch_slots_t slots;
  state.touch[0].tracking = 127;
  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_DOWN, 1)));
  EXPECT_EQ(state.touch[0].tracking, 0);
}

TEST_F(XgpadDs5Test, TouchTwoFingersAndCancel) {
  ds5_state_t state {};
  playstation::touch_slots_t slots;

  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_DOWN, 1)));
  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_DOWN, 2)));
  EXPECT_TRUE(state.touch[1].down);
  EXPECT_FALSE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_DOWN, 3)));  // no third slot

  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_CANCEL, 2)));
  EXPECT_FALSE(state.touch[1].down);
  EXPECT_TRUE(state.touch[0].down);

  ASSERT_TRUE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_CANCEL_ALL, 0)));
  EXPECT_FALSE(state.touch[0].down);
  EXPECT_FALSE(slots.find(1).has_value());
}

TEST_F(XgpadDs5Test, TouchEventsThatCannotApply) {
  ds5_state_t state {};
  playstation::touch_slots_t slots;

  EXPECT_FALSE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_MOVE, 9)));
  EXPECT_FALSE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_UP, 9)));
  EXPECT_FALSE(ds5_apply_touch(state, slots, touch_event(LI_TOUCH_EVENT_HOVER, 9)));
  EXPECT_FALSE(state.touch[0].down);
}

TEST_F(XgpadDs5Test, BatteryStatus) {
  constexpr std::uint8_t full = (XGPAD_DS_POWER_FULL << 4) | 10;

  EXPECT_EQ(ds5_battery_status(full, LI_BATTERY_STATE_DISCHARGING, 64), (XGPAD_DS_POWER_DISCHARGING << 4) | 6);
  EXPECT_EQ(ds5_battery_status(full, LI_BATTERY_STATE_CHARGING, 95), (XGPAD_DS_POWER_CHARGING << 4) | 10);
  EXPECT_EQ(ds5_battery_status(full, LI_BATTERY_STATE_CHARGING, 0), XGPAD_DS_POWER_CHARGING << 4);
  // An unknown percentage keeps the level, an unknown state keeps the power state
  EXPECT_EQ(ds5_battery_status(0x03, LI_BATTERY_STATE_DISCHARGING, LI_BATTERY_PERCENTAGE_UNKNOWN), 0x03);
  EXPECT_EQ(ds5_battery_status(0x13, LI_BATTERY_STATE_UNKNOWN, 50), 0x15);
  // Full is always level 10
  EXPECT_EQ(ds5_battery_status(0x03, LI_BATTERY_STATE_FULL, 20), full);
  // No battery or not charging: charging error
  EXPECT_EQ(ds5_battery_status(full, LI_BATTERY_STATE_NOT_PRESENT, LI_BATTERY_PERCENTAGE_UNKNOWN), 0xFA);
  EXPECT_EQ(ds5_battery_status(full, LI_BATTERY_STATE_NOT_CHARGING, 40), 0xF4);
  // Out of range percentages are limited to level 10
  EXPECT_EQ(ds5_battery_status(full, LI_BATTERY_STATE_DISCHARGING, 250), 10);
}

TEST_F(XgpadDs5Test, PlayerMask) {
  EXPECT_EQ(ds5_player_mask(0x04), 0x1);
  EXPECT_EQ(ds5_player_mask(0x0A), 0x2);
  EXPECT_EQ(ds5_player_mask(0x15), 0x4);
  EXPECT_EQ(ds5_player_mask(0x1B), 0x8);
  EXPECT_EQ(ds5_player_mask(0x00), 0x0);
  EXPECT_EQ(ds5_player_mask(0x1F), 0xF);  // all five: the four outer ones
  EXPECT_EQ(ds5_player_mask(0x01), 0x1);
  EXPECT_EQ(ds5_player_mask(0x10), 0x8);
  EXPECT_EQ(ds5_player_mask(0xE4), 0x1);  // bits above the five LEDs are ignored
}

TEST_F(XgpadDs5Test, ParseOutputRejectsOtherReports) {
  XGPAD_REPORT entry = output_entry(XGPAD_DS_OUTPUT_REPORT {});
  entry.id = XGPAD_OUTPUT_REPORT_ID;
  EXPECT_FALSE(parse_ds5_output(entry).has_value());

  entry.id = XGPAD_DS_OUTPUT_REPORT_ID;
  entry.length = sizeof(XGPAD_DS_OUTPUT_REPORT) - 1;
  EXPECT_FALSE(parse_ds5_output(entry).has_value());
}

TEST_F(XgpadDs5Test, ParseOutputRumble) {
  XGPAD_DS_OUTPUT_REPORT report {};
  report.validFlag0 = XGPAD_DS_FLAG0_COMPATIBLE_VIBRATION | XGPAD_DS_FLAG0_HAPTICS_SELECT;
  report.motorLeft = 255;
  report.motorRight = 1;
  auto output = parse_ds5_output(output_entry(report));
  ASSERT_TRUE(output && output->rumble);
  EXPECT_EQ(*output->rumble, (std::array<std::uint16_t, 2> {65535, 257}));
  EXPECT_FALSE(output->triggers || output->lightbar || output->player_leds);

  // Newer hosts flag rumble in valid_flag2
  report.validFlag0 = XGPAD_DS_FLAG0_HAPTICS_SELECT;
  report.validFlag2 = XGPAD_DS_FLAG2_COMPATIBLE_VIBRATION2;
  output = parse_ds5_output(output_entry(report));
  ASSERT_TRUE(output && output->rumble);
  EXPECT_EQ((*output->rumble)[0], 65535);

  // Only haptics selected: no rumble change
  report.validFlag2 = 0;
  output = parse_ds5_output(output_entry(report));
  ASSERT_TRUE(output);
  EXPECT_FALSE(output->rumble);

  // No flags at all: stop
  report = {};
  report.motorLeft = 200;
  output = parse_ds5_output(output_entry(report));
  ASSERT_TRUE(output && output->rumble);
  EXPECT_EQ(*output->rumble, (std::array<std::uint16_t, 2> {0, 0}));
}

TEST_F(XgpadDs5Test, ParseOutputTriggerEffects) {
  XGPAD_DS_OUTPUT_REPORT report {};
  report.validFlag0 = XGPAD_DS_FLAG0_LEFT_TRIGGER_EFFECT;
  report.leftTriggerEffect[0] = 0x21;
  for (std::uint8_t i = 1; i <= 10; ++i) {
    report.leftTriggerEffect[i] = i;
    report.rightTriggerEffect[i] = static_cast<std::uint8_t>(100 + i);
  }
  report.rightTriggerEffect[0] = 0x26;

  auto output = parse_ds5_output(output_entry(report));
  ASSERT_TRUE(output && output->triggers);
  EXPECT_EQ(output->triggers->flags, XGPAD_DS_FLAG0_LEFT_TRIGGER_EFFECT);
  EXPECT_EQ(output->triggers->type_left, 0x21);
  EXPECT_EQ(output->triggers->type_right, 0x26);
  EXPECT_EQ(output->triggers->left[0], 1);
  EXPECT_EQ(output->triggers->left[9], 10);
  EXPECT_EQ(output->triggers->right[0], 101);
  EXPECT_EQ(output->triggers->right[9], 110);
  EXPECT_FALSE(output->rumble);

  report.validFlag0 = XGPAD_DS_FLAG0_LEFT_TRIGGER_EFFECT | XGPAD_DS_FLAG0_RIGHT_TRIGGER_EFFECT;
  output = parse_ds5_output(output_entry(report));
  ASSERT_TRUE(output && output->triggers);
  EXPECT_EQ(output->triggers->flags, XGPAD_DS_FLAG0_LEFT_TRIGGER_EFFECT | XGPAD_DS_FLAG0_RIGHT_TRIGGER_EFFECT);
}

TEST_F(XgpadDs5Test, ParseOutputLightbarAndPlayerLeds) {
  XGPAD_DS_OUTPUT_REPORT report {};
  report.validFlag1 = XGPAD_DS_FLAG1_LIGHTBAR;
  report.lightbarRed = 1;
  report.lightbarGreen = 2;
  report.lightbarBlue = 3;
  report.playerLeds = 0x0A;

  auto output = parse_ds5_output(output_entry(report));
  ASSERT_TRUE(output && output->lightbar);
  EXPECT_EQ(*output->lightbar, (std::array<std::uint8_t, 3> {1, 2, 3}));
  EXPECT_FALSE(output->player_leds);
  EXPECT_FALSE(output->rumble);  // a valid flag is set, so this is not a stop

  report.validFlag1 = XGPAD_DS_FLAG1_PLAYER_LEDS;
  output = parse_ds5_output(output_entry(report));
  ASSERT_TRUE(output && output->player_leds);
  EXPECT_EQ(*output->player_leds, 0x2);
  EXPECT_FALSE(output->lightbar);
}

#endif
