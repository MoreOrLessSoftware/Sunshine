/**
 * @file tests/unit/platform/windows/test_xgpad.cpp
 * @brief Test the xgpad (virtual Xbox Series controller) report conversions and context helpers.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // local includes
  #include <src/platform/windows/xgpad.h>
  #include <src/platform/windows/xgpad_report.h>

using namespace platf;
using namespace platf::xgpad;

/**
 * @brief Test fixture for xgpad report conversions.
 */
class XgpadReportTest: public BaseTest {};

TEST_F(XgpadReportTest, NeutralStateIsCenteredAndReleased) {
  const auto report = make_input_report(gamepad_state_t {});

  EXPECT_EQ(report.lx, 0x8000);
  EXPECT_EQ(report.ly, 0x7FFF);  // 32767 - 0: XInput reads exactly 0
  EXPECT_EQ(report.rx, 0x8000);
  EXPECT_EQ(report.ry, 0x7FFF);
  EXPECT_EQ(report.lt, 0);
  EXPECT_EQ(report.rt, 0);
  EXPECT_EQ(report.hat, 0);
  EXPECT_EQ(report.buttons, 0);
  EXPECT_EQ(report.share, 0);
}

TEST_F(XgpadReportTest, AxisExtremes) {
  EXPECT_EQ(axis_x(INT16_MIN), 0);
  EXPECT_EQ(axis_x(INT16_MAX), 0xFFFF);
  // Moonlight Y is positive-up, HID Y is increasing-down
  EXPECT_EQ(axis_y(INT16_MAX), 0);
  EXPECT_EQ(axis_y(INT16_MIN), 0xFFFF);
}

TEST_F(XgpadReportTest, TriggerScaling) {
  EXPECT_EQ(trigger_from_u8(0), 0);
  EXPECT_EQ(trigger_from_u8(255), 1023);
  EXPECT_EQ(trigger_from_u8(128), 514);

  gamepad_state_t state {};
  state.lt = 255;
  state.rt = 1;
  const auto report = make_input_report(state);
  EXPECT_EQ(report.lt, 1023);
  EXPECT_EQ(report.rt, 4);
}

TEST_F(XgpadReportTest, ButtonMapping) {
  struct mapping_t {
    std::uint32_t flag;
    std::uint16_t bit;
  };

  constexpr mapping_t mappings[] = {
    {A, XGPAD_BTN_A},
    {B, XGPAD_BTN_B},
    {X, XGPAD_BTN_X},
    {Y, XGPAD_BTN_Y},
    {LEFT_BUTTON, XGPAD_BTN_LB},
    {RIGHT_BUTTON, XGPAD_BTN_RB},
    {BACK, XGPAD_BTN_VIEW},
    {START, XGPAD_BTN_MENU},
    {HOME, XGPAD_BTN_GUIDE},
    {LEFT_STICK, XGPAD_BTN_LS},
    {RIGHT_STICK, XGPAD_BTN_RS},
  };

  std::uint32_t all_flags = 0;
  std::uint16_t all_bits = 0;
  for (const auto &[flag, bit] : mappings) {
    EXPECT_EQ(buttons_from_flags(flag), bit) << "flag 0x" << std::hex << flag;
    all_flags |= flag;
    all_bits |= bit;
  }
  EXPECT_EQ(buttons_from_flags(all_flags), all_bits);

  // Paddles and D-pad flags do not leak into the button field
  EXPECT_EQ(buttons_from_flags(PADDLE1 | PADDLE2 | PADDLE3 | PADDLE4 | DPAD_UP | DPAD_LEFT), 0);
}

TEST_F(XgpadReportTest, ShareFromMiscOrTouchpad) {
  gamepad_state_t state {};
  state.buttonFlags = MISC_BUTTON;
  EXPECT_EQ(make_input_report(state).share, 1);

  state.buttonFlags = TOUCHPAD_BUTTON;
  EXPECT_EQ(make_input_report(state).share, 1);

  state.buttonFlags = HOME;
  EXPECT_EQ(make_input_report(state).share, 0);
  EXPECT_EQ(make_input_report(state).buttons, XGPAD_BTN_GUIDE);
}

TEST_F(XgpadReportTest, HatSwitch) {
  EXPECT_EQ(hat_from_buttons(0), 0);
  EXPECT_EQ(hat_from_buttons(DPAD_UP), 1);
  EXPECT_EQ(hat_from_buttons(DPAD_UP | DPAD_RIGHT), 2);
  EXPECT_EQ(hat_from_buttons(DPAD_RIGHT), 3);
  EXPECT_EQ(hat_from_buttons(DPAD_DOWN | DPAD_RIGHT), 4);
  EXPECT_EQ(hat_from_buttons(DPAD_DOWN), 5);
  EXPECT_EQ(hat_from_buttons(DPAD_DOWN | DPAD_LEFT), 6);
  EXPECT_EQ(hat_from_buttons(DPAD_LEFT), 7);
  EXPECT_EQ(hat_from_buttons(DPAD_UP | DPAD_LEFT), 8);

  // Opposing directions resolve to up / left
  EXPECT_EQ(hat_from_buttons(DPAD_UP | DPAD_DOWN), 1);
  EXPECT_EQ(hat_from_buttons(DPAD_LEFT | DPAD_RIGHT), 7);
  EXPECT_EQ(hat_from_buttons(DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT), 8);
}

TEST_F(XgpadReportTest, MotorScaling) {
  EXPECT_EQ(motor_from_percent(0), 0);
  EXPECT_EQ(motor_from_percent(50), 32767);
  EXPECT_EQ(motor_from_percent(100), 65535);
  EXPECT_EQ(motor_from_percent(255), 65535);  // clamped
}

TEST_F(XgpadReportTest, OutputReportAllMotors) {
  XGPAD_OUTPUT_REPORT report {};
  report.enable = XGPAD_MOTOR_LEFT | XGPAD_MOTOR_RIGHT | XGPAD_MOTOR_LEFT_TRIGGER | XGPAD_MOTOR_RIGHT_TRIGGER;
  report.leftMotor = 100;
  report.rightMotor = 50;
  report.leftTrigger = 75;
  report.rightTrigger = 25;
  report.duration = 0xFF;

  const auto state = apply_output_report(report, rumble_state_t {});
  EXPECT_EQ(state.low_freq, 65535);
  EXPECT_EQ(state.high_freq, 32767);
  EXPECT_EQ(state.left_trigger, 49151);
  EXPECT_EQ(state.right_trigger, 16383);
}

TEST_F(XgpadReportTest, OutputReportKeepsDisabledMotors) {
  const rumble_state_t previous {1, 2, 3, 4};
  XGPAD_OUTPUT_REPORT report {};
  report.enable = XGPAD_MOTOR_LEFT_TRIGGER;
  report.leftMotor = 100;
  report.leftTrigger = 100;

  const auto state = apply_output_report(report, previous);
  EXPECT_EQ(state.low_freq, 1);
  EXPECT_EQ(state.high_freq, 2);
  EXPECT_EQ(state.left_trigger, 65535);
  EXPECT_EQ(state.right_trigger, 4);

  report.enable = 0;
  EXPECT_EQ(apply_output_report(report, previous), previous);
}

TEST_F(XgpadReportTest, MatchesOwnInstanceIds) {
  EXPECT_TRUE(is_xgpad_instance_id(L"SWD\\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B13_REV&0509\\PAD0"));
  EXPECT_TRUE(is_xgpad_instance_id(L"swd\\{00001812-0000-1000-8000-00805f9b34fb}_dev_vid&02045e_pid&0b13_rev&0509\\pad15"));
  EXPECT_TRUE(is_xgpad_instance_id(L"HID\\{00001812-0000-1000-8000-00805F9B34FB}&DEV&VID_045E&PID_0B13&REV_0509&IG_00\\1&21CB6230&0&0000"));
  // Earlier test builds
  EXPECT_TRUE(is_xgpad_instance_id(L"SWD\\SunshineXgpad\\Pad1"));
  EXPECT_TRUE(is_xgpad_instance_id(L"HID\\SUNSHINEXGPAD&IG_00\\1&2D650065&0&0000"));
}

TEST_F(XgpadReportTest, NeverMatchesRealControllers) {
  // Real Bluetooth LE Xbox Series controller: parent and HID child (address before &IG_00)
  EXPECT_FALSE(is_xgpad_instance_id(L"BTHLEDEVICE\\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B13_REV&0509_AC8EBD0913A3\\A&165B0C48&1&0016"));
  EXPECT_FALSE(is_xgpad_instance_id(L"HID\\{00001812-0000-1000-8000-00805F9B34FB}&DEV&VID_045E&PID_0B13&REV_0509&AC8EBD0913A3&IG_00\\B&30AE7916&0&0000"));
  // Microsoft's own synthetic gamepad
  EXPECT_FALSE(is_xgpad_instance_id(L"SWD\\SYNTHETICGAMEPADENUM\\SYNTHETICGAMEPAD0"));
  // Prefix alone (no instance part) and unrelated IDs
  EXPECT_FALSE(is_xgpad_instance_id(L"SWD\\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B13_REV&0509\\"));
  EXPECT_FALSE(is_xgpad_instance_id(L""));
}

TEST_F(XgpadReportTest, HasHandlesMissingContext) {
  EXPECT_FALSE(has(nullptr, 0));
  EXPECT_FALSE(has(nullptr, -1));
}

TEST_F(XgpadReportTest, DriverInstalledIsStable) {
  // Result depends on the machine; it must not crash and must be cached.
  const bool first = driver_installed();
  EXPECT_EQ(driver_installed(), first);
}
#endif
