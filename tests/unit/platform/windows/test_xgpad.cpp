/**
 * @file tests/unit/platform/windows/test_xgpad.cpp
 * @brief Test the xgpad (virtual Xbox Series controller) report conversions and context helpers.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <cstddef>
  #include <cstring>
  #include <memory>
  #include <vector>

  // platform includes
  #include <Windows.h>

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

TEST_F(XgpadReportTest, RumbleDuration) {
  XGPAD_OUTPUT_REPORT report {};

  // XInput / Windows.Gaming.Input / GameInput: continuous until changed
  report.duration = 0xFF;
  report.loopCount = 0xEB;
  EXPECT_EQ(rumble_duration_ms(report), 0u);
  report.duration = 0;
  EXPECT_EQ(rumble_duration_ms(report), 0u);

  // Finite effects: (start delay + duration) * (loops + 1), in 10 ms units
  report.duration = 25;
  report.startDelay = 0;
  report.loopCount = 0;
  EXPECT_EQ(rumble_duration_ms(report), 250u);
  report.startDelay = 5;
  report.loopCount = 2;
  EXPECT_EQ(rumble_duration_ms(report), 900u);
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

namespace {
  /**
   * @brief Publish an output report the way the driver does.
   * @param shared Shared section.
   * @param id Report ID.
   * @param first First payload byte, to tell reports apart.
   * @param length Payload length.
   */
  void driver_publish_output(XGPAD_SHARED &shared, const std::uint8_t id, const std::uint8_t first, const std::uint8_t length = 8) {
    const long n = shared.outputCount + 1;
    auto &slot = shared.outputRing[(n - 1) % XGPAD_OUTPUT_RING_SIZE];
    slot.id = id;
    slot.length = length;
    std::memset(slot.data, 0, sizeof(slot.data));
    slot.data[0] = first;
    shared.outputCount = n;
  }
}  // namespace

TEST_F(XgpadReportTest, PublishInputReportFillsRingInOrder) {
  auto shared = std::make_unique<XGPAD_SHARED>();

  XGPAD_INPUT_REPORT report = make_input_report(gamepad_state_t {});
  report.buttons = XGPAD_BTN_A;
  EXPECT_EQ(publish_input_report(*shared, report), 1);
  EXPECT_EQ(shared->inputCount, 1);
  EXPECT_EQ(shared->inputRing[0].id, XGPAD_INPUT_REPORT_ID);
  EXPECT_EQ(shared->inputRing[0].length, sizeof(XGPAD_INPUT_REPORT));
  EXPECT_EQ(std::memcmp(shared->inputRing[0].data, &report, sizeof(report)), 0);

  // The ring wraps: report n lives at (n - 1) % size
  for (long n = 2; n <= static_cast<long>(XGPAD_INPUT_RING_SIZE) + 1; ++n) {
    const std::uint8_t payload[] = {static_cast<std::uint8_t>(n)};
    EXPECT_EQ(publish_input_report(*shared, 0x07, payload), n);
  }
  EXPECT_EQ(shared->inputRing[0].id, 0x07);
  EXPECT_EQ(shared->inputRing[0].length, 1);
  EXPECT_EQ(shared->inputRing[0].data[0], XGPAD_INPUT_RING_SIZE + 1);
}

TEST_F(XgpadReportTest, PublishInputReportTruncatesOversizedPayloads) {
  auto shared = std::make_unique<XGPAD_SHARED>();
  std::vector<std::uint8_t> payload(XGPAD_MAX_REPORT + 10, 0xAB);

  publish_input_report(*shared, XGPAD_DS_INPUT_REPORT_ID, payload);
  EXPECT_EQ(shared->inputRing[0].length, XGPAD_MAX_REPORT);
  EXPECT_EQ(shared->inputRing[0].data[XGPAD_MAX_REPORT - 1], 0xAB);
}

TEST_F(XgpadReportTest, ReadOutputReportsReturnsNewReportsOnce) {
  auto shared = std::make_unique<XGPAD_SHARED>();
  long consumed = 0;
  long lost = 0;

  EXPECT_TRUE(read_output_reports(*shared, consumed, &lost).empty());

  driver_publish_output(*shared, XGPAD_OUTPUT_REPORT_ID, 1);
  driver_publish_output(*shared, XGPAD_DS_OUTPUT_REPORT_ID, 2, 47);
  auto reports = read_output_reports(*shared, consumed, &lost);
  ASSERT_EQ(reports.size(), 2u);
  EXPECT_EQ(reports[0].id, XGPAD_OUTPUT_REPORT_ID);
  EXPECT_EQ(reports[0].data[0], 1);
  EXPECT_EQ(reports[1].id, XGPAD_DS_OUTPUT_REPORT_ID);
  EXPECT_EQ(reports[1].length, 47);
  EXPECT_EQ(consumed, 2);
  EXPECT_EQ(lost, 0);

  EXPECT_TRUE(read_output_reports(*shared, consumed).empty());
}

TEST_F(XgpadReportTest, ReadOutputReportsSkipsOverwrittenReports) {
  auto shared = std::make_unique<XGPAD_SHARED>();
  long consumed = 0;
  long lost = 0;

  const long total = static_cast<long>(XGPAD_OUTPUT_RING_SIZE) + 3;
  for (long n = 1; n <= total; ++n) {
    driver_publish_output(*shared, XGPAD_OUTPUT_REPORT_ID, static_cast<std::uint8_t>(n));
  }
  const auto reports = read_output_reports(*shared, consumed, &lost);
  ASSERT_EQ(reports.size(), XGPAD_OUTPUT_RING_SIZE);
  EXPECT_EQ(reports.front().data[0], 4);  // reports 1..3 were overwritten
  EXPECT_EQ(reports.back().data[0], total);
  EXPECT_EQ(lost, 3);
  EXPECT_EQ(consumed, total);

  // Without a counter for lost reports the result is the same
  long consumed_again = 0;
  EXPECT_EQ(read_output_reports(*shared, consumed_again).size(), XGPAD_OUTPUT_RING_SIZE);
}

TEST_F(XgpadReportTest, ReadOutputReportsHandlesARestartedCounter) {
  auto shared = std::make_unique<XGPAD_SHARED>();
  long consumed = 5;  // read from an earlier section

  driver_publish_output(*shared, XGPAD_OUTPUT_REPORT_ID, 9);
  EXPECT_TRUE(read_output_reports(*shared, consumed).empty());
  EXPECT_EQ(consumed, 1);

  driver_publish_output(*shared, XGPAD_OUTPUT_REPORT_ID, 10);
  const auto reports = read_output_reports(*shared, consumed);
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_EQ(reports[0].data[0], 10);
}

TEST_F(XgpadReportTest, XboxOutputReportFromRingEntry) {
  XGPAD_REPORT entry {};
  entry.id = XGPAD_OUTPUT_REPORT_ID;
  entry.length = sizeof(XGPAD_OUTPUT_REPORT);
  entry.data[0] = XGPAD_MOTOR_LEFT;
  entry.data[3] = 100;

  XGPAD_OUTPUT_REPORT out {};
  ASSERT_TRUE(xbox_output_report(entry, out));
  EXPECT_EQ(out.enable, XGPAD_MOTOR_LEFT);
  EXPECT_EQ(out.leftMotor, 100);

  entry.length = sizeof(XGPAD_OUTPUT_REPORT) - 1;
  EXPECT_FALSE(xbox_output_report(entry, out));

  entry.length = sizeof(XGPAD_OUTPUT_REPORT);
  entry.id = XGPAD_DS_OUTPUT_REPORT_ID;
  EXPECT_FALSE(xbox_output_report(entry, out));
}

TEST_F(XgpadReportTest, SharedLayout) {
  EXPECT_EQ(sizeof(XGPAD_DS_INPUT_REPORT), 63u);
  EXPECT_EQ(sizeof(XGPAD_DS_OUTPUT_REPORT), 47u);
  EXPECT_EQ(offsetof(XGPAD_DS_INPUT_REPORT, gyro), 15u);
  EXPECT_EQ(offsetof(XGPAD_DS_INPUT_REPORT, sensorTimestamp), 27u);
  EXPECT_EQ(offsetof(XGPAD_DS_INPUT_REPORT, points), 32u);
  EXPECT_EQ(offsetof(XGPAD_DS_INPUT_REPORT, status), 52u);
  EXPECT_EQ(offsetof(XGPAD_DS_OUTPUT_REPORT, rightTriggerEffect), 10u);
  EXPECT_EQ(offsetof(XGPAD_DS_OUTPUT_REPORT, leftTriggerEffect), 21u);
  EXPECT_EQ(offsetof(XGPAD_DS_OUTPUT_REPORT, validFlag2), 38u);
  EXPECT_EQ(offsetof(XGPAD_DS_OUTPUT_REPORT, playerLeds), 43u);
  EXPECT_EQ(offsetof(XGPAD_DS_OUTPUT_REPORT, lightbarRed), 44u);
}

TEST_F(XgpadReportTest, PolicyUsesXgpad) {
  EXPECT_TRUE(policy_uses_xgpad("xgpad", true));
  EXPECT_TRUE(policy_uses_xgpad("xgpad", false));
  EXPECT_TRUE(policy_uses_xgpad("all", false));
  EXPECT_TRUE(policy_uses_xgpad("", false));  // unset behaves like "all"
  EXPECT_FALSE(policy_uses_xgpad("all", true));  // licensed Virtual HID Driver keeps priority
  EXPECT_FALSE(policy_uses_xgpad("", true));
  EXPECT_FALSE(policy_uses_xgpad("vigembus", false));
  EXPECT_FALSE(policy_uses_xgpad("virtualhid", false));
}

namespace {
  allocation_choice_t make_choice(std::string_view policy, std::string_view gamepad, std::uint8_t type, bool vigem) {
    return {
      .driver_policy = policy,
      .gamepad = gamepad,
      .client_type = type,
      .client_capabilities = 0,
      .vigem_available = vigem,
      .motion_as_ds4 = true,
      .touchpad_as_ds4 = true,
    };
  }
}  // namespace

TEST_F(XgpadReportTest, ExplicitPolicyAlwaysAllocatesXgpad) {
  EXPECT_TRUE(should_allocate_xgpad(make_choice("xgpad", "ds4", 0x02, true)));
  EXPECT_TRUE(should_allocate_xgpad(make_choice("xgpad", "auto", 0x02, true)));
}

TEST_F(XgpadReportTest, AllPolicyRoutesXboxProfilesToXgpad) {
  for (const bool vigem : {false, true}) {
    EXPECT_TRUE(should_allocate_xgpad(make_choice("all", "xseries", 0x00, vigem)));
    EXPECT_TRUE(should_allocate_xgpad(make_choice("all", "xone", 0x00, vigem)));
    EXPECT_TRUE(should_allocate_xgpad(make_choice("all", "auto", 0x01, vigem)));  // Xbox client
    EXPECT_TRUE(should_allocate_xgpad(make_choice("all", "auto", 0x00, vigem)));  // unknown client
    EXPECT_FALSE(should_allocate_xgpad(make_choice("all", "ds4", 0x00, vigem)));
    EXPECT_FALSE(should_allocate_xgpad(make_choice("all", "ds5", 0x00, vigem)));
  }
  // An explicit Xbox 360 choice goes to ViGEmBus when it can, xgpad otherwise
  EXPECT_FALSE(should_allocate_xgpad(make_choice("all", "x360", 0x00, true)));
  EXPECT_TRUE(should_allocate_xgpad(make_choice("all", "x360", 0x00, false)));
}

TEST_F(XgpadReportTest, OtherPoliciesNeverAllocateXgpad) {
  for (const auto policy : {"vigembus", "virtualhid"}) {
    EXPECT_FALSE(should_allocate_xgpad(make_choice(policy, "xseries", 0x01, false)));
    EXPECT_FALSE(should_allocate_xgpad(make_choice(policy, "auto", 0x01, false)));
  }
  EXPECT_TRUE(should_allocate_xgpad(make_choice("", "auto", 0x01, false)));  // unset behaves like "all"
}

TEST_F(XgpadReportTest, AllPolicyKeepsDualShockClientsOnViGEmBus) {
  // PlayStation client: DS4 through ViGEmBus when available, otherwise still a working Xbox controller
  EXPECT_FALSE(should_allocate_xgpad(make_choice("all", "auto", 0x02, true)));
  EXPECT_TRUE(should_allocate_xgpad(make_choice("all", "auto", 0x02, false)));

  // Motion / touchpad capable clients with the *_as_ds4 options
  auto choice = make_choice("all", "auto", 0x00, true);
  choice.client_capabilities = 0x10;  // LI_CCAP_ACCEL
  EXPECT_FALSE(should_allocate_xgpad(choice));
  choice.motion_as_ds4 = false;
  EXPECT_TRUE(should_allocate_xgpad(choice));
  choice.client_capabilities = 0x08;  // LI_CCAP_TOUCHPAD
  EXPECT_FALSE(should_allocate_xgpad(choice));
  choice.touchpad_as_ds4 = false;
  EXPECT_TRUE(should_allocate_xgpad(choice));
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

TEST_F(XgpadReportTest, DriverAttachState) {
  using namespace std::chrono_literals;
  // Attached as soon as the driver sets its flag, regardless of time
  EXPECT_EQ(driver_attach_state(XGPAD_STATUS_DRIVER_STARTED, 0ms), attach_state_e::attached);
  EXPECT_EQ(driver_attach_state(XGPAD_STATUS_DRIVER_STARTED, 1h), attach_state_e::attached);
  EXPECT_EQ(driver_attach_state(XGPAD_STATUS_DRIVER_STARTED | 0x10, 0ms), attach_state_e::attached);
  // Without the flag: pending inside the window, missing from the timeout on
  EXPECT_EQ(driver_attach_state(0, 0ms), attach_state_e::pending);
  EXPECT_EQ(driver_attach_state(0, driver_attach_timeout - 1ms), attach_state_e::pending);
  EXPECT_EQ(driver_attach_state(0, driver_attach_timeout), attach_state_e::missing);
  EXPECT_EQ(driver_attach_state(0x10, 1h), attach_state_e::missing);
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

TEST_F(XgpadReportTest, RefreshMatchesTheDriverStoreAndReportsAVersion) {
  const bool installed = driver_installed(true);
  EXPECT_EQ(driver_installed(), installed);
  EXPECT_EQ(!driver_version().empty(), installed);
}

TEST_F(XgpadReportTest, DriverVersionTextSplitsAllFourParts) {
  EXPECT_EQ(driver_version_text(0x0001000200030004ull), "1.2.3.4");
  EXPECT_EQ(driver_version_text(0xFFFF000000000000ull), "65535.0.0.0");
  EXPECT_EQ(driver_version_text(0), "0.0.0.0");
}

#endif
