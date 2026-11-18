/**
 * @file drivers/xgpad/xgpad_shared.h
 * @brief Shared-memory IPC contract between Sunshine and the xgpad UMDF2 virtual controller driver.
 *
 * One virtual pad = one software device + one named section + two auto-reset events. The producer (Sunshine)
 * creates the section and events *before* creating the device, so the driver can open them when it starts.
 *
 * Each pad has a profile, passed to the driver in the XGPAD_PROFILE_PROPKEY device property and repeated in
 * XGPAD_SHARED::profile:
 *   XGPAD_PROFILE_XBOX_SERIES  Xbox Series X|S controller (Bluetooth LE layout, 045E:0B13) under xinputhid.
 *   XGPAD_PROFILE_DUALSENSE    DualSense controller (USB layout, 054C:0CE6).
 *
 * Input path:  the producer appends every input report to the inputRing (single writer), publishes it by
 *              incrementing inputCount, then sets the input event. The driver drains the ring in order and
 *              completes pended IOCTL_HID_READ_REPORT requests. Nothing is lost unless the driver falls more than
 *              XGPAD_INPUT_RING_SIZE reports behind.
 * Output path: the driver appends every output report it receives (Xbox 0x03 rumble, DualSense 0x02 effects) to
 *              the outputRing, publishes it by incrementing outputCount, then sets the output event.
 *
 * Rings: report n (1-based) lives in ring[(n - 1) % ring size]. A reader that copied report n must re-check the count
 * afterwards: if it advanced by the ring size or more, the copy may be torn.
 */
#pragma once

#include <stdint.h>

#define XGPAD_MAX_PADS 16
#define XGPAD_SHARED_MAGIC 0x44415047u  // 'GPAD'
#define XGPAD_SHARED_VERSION 4u
#define XGPAD_INPUT_RING_SIZE 16u
#define XGPAD_OUTPUT_RING_SIZE 16u
#define XGPAD_MAX_REPORT 64u  ///< Largest report payload in bytes, report ID not included.

// Object names; %ls = per-device token. The producer generates a fresh token for every device it creates and
// passes it to the driver through the XGPAD_TOKEN_PROPKEY device property, so a stale or second driver
// instance can never share (and steal wake-ups from) another device's events. Without the property the
// driver falls back to the decimal pad index as token.
#define XGPAD_SECTION_NAME_FMT L"Global\\SunshineXgpad_%ls"
#define XGPAD_INPUT_EVENT_NAME_FMT L"Global\\SunshineXgpad_%ls_in"
#define XGPAD_OUTPUT_EVENT_NAME_FMT L"Global\\SunshineXgpad_%ls_out"
#define XGPAD_TOKEN_MAX 40  ///< Token length limit in characters, including the terminator.

// Custom device properties: {6c1f8d2e-3b7a-4e59-9a41-58d2c7e0b913}
//   pid 2: token (DEVPROP_TYPE_STRING)
//   pid 3: profile (DEVPROP_TYPE_UINT32, XGPAD_PROFILE_*); without it the driver uses XGPAD_PROFILE_XBOX_SERIES
#define XGPAD_TOKEN_PROPKEY_GUID {0x6c1f8d2e, 0x3b7a, 0x4e59, {0x9a, 0x41, 0x58, 0xd2, 0xc7, 0xe0, 0xb9, 0x13}}
#define XGPAD_TOKEN_PROPKEY_PID 2
#define XGPAD_PROFILE_PROPKEY_PID 3

#define XGPAD_PROFILE_XBOX_SERIES 0u
#define XGPAD_PROFILE_DUALSENSE 1u

// SwDeviceCreate parameters.
// HID class derives the HID child's device ID from the parent's *enumerator* segment
// (SWD\<enumerator>\<instance> -> HID\<enumerator with _ replaced by &>&IG_00\...). XInput, GameInput and SDL
// parse VID/PID/REV from that path, so the enumerator name must carry them, BLE-style like a real pad:
//   HID\{00001812-0000-1000-8000-00805f9b34fb}&Dev&VID_045e&PID_0b13&REV_0509&IG_00
#define XGPAD_SWD_ENUMERATOR L"{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02045e_PID&0b13_REV&0509"
#define XGPAD_SWD_ENUMERATOR_USB_STYLE L"VID_045E&PID_0B13&REV_0509"
#define XGPAD_SWD_INSTANCE_FMT L"Pad%u"

// The DualSense profile presents a USB pad: HID\VID_054C&PID_0CE6&REV_0100. A real DualSense never has this ID: over
// USB its HID child is HID\VID_054C&PID_0CE6&MI_03, over Bluetooth HID\{00001124-...}_VID&0002054c_PID&0ce6.
#define XGPAD_SWD_ENUMERATOR_DUALSENSE L"VID_054C&PID_0CE6&REV_0100"

// Hardware IDs matched by xgpad.inf, as REG_MULTI_SZ lists.
#define XGPAD_HARDWARE_IDS L"SunshineXgpad\\{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02045e_PID&0b13\0"
#define XGPAD_HARDWARE_IDS_DUALSENSE L"SunshineXgpad\\VID_054C&PID_0CE6\0"

// WUDFHost runs drivers as LOCAL SERVICE: grant SYSTEM, Administrators and LOCAL SERVICE full access.
#define XGPAD_OBJECT_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)"

#define XGPAD_INPUT_REPORT_ID 0x01
#define XGPAD_OUTPUT_REPORT_ID 0x03

#define XGPAD_DS_INPUT_REPORT_ID 0x01
#define XGPAD_DS_OUTPUT_REPORT_ID 0x02
#define XGPAD_DS_FEATURE_CALIBRATION 0x05
#define XGPAD_DS_FEATURE_PAIRING_INFO 0x09
#define XGPAD_DS_FEATURE_FIRMWARE_INFO 0x20

#pragma pack(push, 1)

/**
 * @brief Xbox Series input report 0x01 payload (16 bytes, report ID not included).
 * Layout of the Xbox Series X|S controller over Bluetooth LE, firmware 5.x (045E:0B13).
 */
typedef struct XGPAD_INPUT_REPORT {
  uint16_t lx;  ///< 0..65535, 32768 = center, increasing to the right
  uint16_t ly;  ///< 0..65535, 32768 = center, increasing downwards
  uint16_t rx;
  uint16_t ry;
  uint16_t lt;  ///< 0..1023 (upper 6 bits padding)
  uint16_t rt;  ///< 0..1023
  uint8_t hat;  ///< 0 = released, 1 = N, 2 = NE, 3 = E ... 8 = NW (upper 4 bits padding)
  uint16_t buttons;  ///< XGPAD_BTN_* (bit 15 padding)
  uint8_t share;  ///< bit 0 = Share/Record (consumer usage 0xB2)
} XGPAD_INPUT_REPORT;

/**
 * @brief Xbox Series output report 0x03 payload (8 bytes, report ID not included): rumble incl. impulse triggers.
 */
typedef struct XGPAD_OUTPUT_REPORT {
  uint8_t enable;  ///< XGPAD_MOTOR_* mask (low 4 bits)
  uint8_t leftTrigger;  ///< 0..100
  uint8_t rightTrigger;  ///< 0..100
  uint8_t leftMotor;  ///< 0..100 (strong / low frequency)
  uint8_t rightMotor;  ///< 0..100 (weak / high frequency)
  uint8_t duration;  ///< 10 ms units, 0xFF = continuous
  uint8_t startDelay;  ///< 10 ms units
  uint8_t loopCount;
} XGPAD_OUTPUT_REPORT;

/**
 * @brief One touch point of a DualSense input report.
 */
typedef struct XGPAD_DS_TOUCH_POINT {
  uint8_t contact;  ///< bit 7 set = not touching, bits 0..6 = tracking number (increments per new touch)
  uint8_t xLow;  ///< X bits 0..7 (0..1919)
  uint8_t xHighYLow;  ///< bits 0..3 = X bits 8..11, bits 4..7 = Y bits 0..3
  uint8_t yHigh;  ///< Y bits 4..11 (0..1079)
} XGPAD_DS_TOUCH_POINT;

/**
 * @brief DualSense USB input report 0x01 payload (63 bytes, report ID not included).
 */
typedef struct XGPAD_DS_INPUT_REPORT {
  uint8_t lx;  ///< 0..255, 128 = center, increasing to the right
  uint8_t ly;  ///< 0..255, 128 = center, increasing downwards
  uint8_t rx;
  uint8_t ry;
  uint8_t l2;  ///< 0..255
  uint8_t r2;  ///< 0..255
  uint8_t seq;  ///< Increments with every report
  uint8_t buttons[4];  ///< [0]: hat (low nibble, 0 = N ... 7 = NW, 8 = released) | XGPAD_DS_BTN0_*; [1]: XGPAD_DS_BTN1_*; [2]: XGPAD_DS_BTN2_*
  uint8_t reserved[4];
  int16_t gyro[3];  ///< Pitch, yaw, roll; XGPAD_DS_GYRO_RES_PER_DEG_S per deg/s with the calibration the driver reports
  int16_t accel[3];  ///< X, Y, Z; XGPAD_DS_ACC_RES_PER_G per g with the calibration the driver reports
  uint32_t sensorTimestamp;  ///< 0.33 us units
  uint8_t reserved2;
  XGPAD_DS_TOUCH_POINT points[2];
  uint8_t reserved3[12];
  uint8_t status;  ///< bits 0..3 = battery level 0..10, bits 4..7 = XGPAD_DS_POWER_*
  uint8_t reserved4[10];
} XGPAD_DS_INPUT_REPORT;

/**
 * @brief DualSense USB output report 0x02 payload (47 bytes, report ID not included).
 */
typedef struct XGPAD_DS_OUTPUT_REPORT {
  uint8_t validFlag0;  ///< XGPAD_DS_FLAG0_*
  uint8_t validFlag1;  ///< XGPAD_DS_FLAG1_*
  uint8_t motorRight;  ///< 0..255 (weak / high frequency)
  uint8_t motorLeft;  ///< 0..255 (strong / low frequency)
  uint8_t headphoneVolume;
  uint8_t speakerVolume;
  uint8_t micVolume;
  uint8_t audioControl;
  uint8_t muteButtonLed;
  uint8_t powerSaveControl;
  uint8_t rightTriggerEffect[11];  ///< Mode byte followed by 10 parameter bytes
  uint8_t leftTriggerEffect[11];  ///< Mode byte followed by 10 parameter bytes
  uint8_t reserved[5];
  uint8_t audioControl2;
  uint8_t validFlag2;  ///< XGPAD_DS_FLAG2_*
  uint8_t reserved2[2];
  uint8_t lightbarSetup;
  uint8_t ledBrightness;
  uint8_t playerLeds;  ///< bits 0..4 = the five player LEDs from left to right
  uint8_t lightbarRed;
  uint8_t lightbarGreen;
  uint8_t lightbarBlue;
} XGPAD_DS_OUTPUT_REPORT;

/**
 * @brief One report in a shared ring.
 */
typedef struct XGPAD_REPORT {
  uint8_t id;  ///< Report ID
  uint8_t length;  ///< Payload bytes used in data, at most XGPAD_MAX_REPORT
  uint8_t data[XGPAD_MAX_REPORT];  ///< Payload, report ID not included
} XGPAD_REPORT;

#pragma pack(pop)

// Xbox Series button bits (HID button usages 1..15).
#define XGPAD_BTN_A 0x0001
#define XGPAD_BTN_B 0x0002
#define XGPAD_BTN_X 0x0008
#define XGPAD_BTN_Y 0x0010
#define XGPAD_BTN_LB 0x0040
#define XGPAD_BTN_RB 0x0080
#define XGPAD_BTN_VIEW 0x0400
#define XGPAD_BTN_MENU 0x0800
#define XGPAD_BTN_GUIDE 0x1000
#define XGPAD_BTN_LS 0x2000
#define XGPAD_BTN_RS 0x4000

#define XGPAD_MOTOR_RIGHT 0x01
#define XGPAD_MOTOR_LEFT 0x02
#define XGPAD_MOTOR_RIGHT_TRIGGER 0x04
#define XGPAD_MOTOR_LEFT_TRIGGER 0x08

// DualSense buttons[0] (upper nibble; the low nibble is the hat)
#define XGPAD_DS_HAT_RELEASED 0x08
#define XGPAD_DS_BTN0_SQUARE 0x10
#define XGPAD_DS_BTN0_CROSS 0x20
#define XGPAD_DS_BTN0_CIRCLE 0x40
#define XGPAD_DS_BTN0_TRIANGLE 0x80
// DualSense buttons[1]
#define XGPAD_DS_BTN1_L1 0x01
#define XGPAD_DS_BTN1_R1 0x02
#define XGPAD_DS_BTN1_L2 0x04
#define XGPAD_DS_BTN1_R2 0x08
#define XGPAD_DS_BTN1_CREATE 0x10
#define XGPAD_DS_BTN1_OPTIONS 0x20
#define XGPAD_DS_BTN1_L3 0x40
#define XGPAD_DS_BTN1_R3 0x80
// DualSense buttons[2]
#define XGPAD_DS_BTN2_PS 0x01
#define XGPAD_DS_BTN2_TOUCHPAD 0x02
#define XGPAD_DS_BTN2_MIC 0x04

#define XGPAD_DS_TOUCH_NOT_TOUCHING 0x80
#define XGPAD_DS_TOUCHPAD_WIDTH 1920
#define XGPAD_DS_TOUCHPAD_HEIGHT 1080

#define XGPAD_DS_POWER_DISCHARGING 0x0
#define XGPAD_DS_POWER_CHARGING 0x1
#define XGPAD_DS_POWER_FULL 0x2

// Sensor resolution implied by the calibration feature report (0x05) the driver returns.
#define XGPAD_DS_GYRO_RES_PER_DEG_S 16
#define XGPAD_DS_ACC_RES_PER_G 8192

// DualSense output report valid flags
#define XGPAD_DS_FLAG0_COMPATIBLE_VIBRATION 0x01
#define XGPAD_DS_FLAG0_HAPTICS_SELECT 0x02
#define XGPAD_DS_FLAG0_RIGHT_TRIGGER_EFFECT 0x04
#define XGPAD_DS_FLAG0_LEFT_TRIGGER_EFFECT 0x08
#define XGPAD_DS_FLAG1_MIC_MUTE_LED 0x01
#define XGPAD_DS_FLAG1_POWER_SAVE 0x02
#define XGPAD_DS_FLAG1_LIGHTBAR 0x04
#define XGPAD_DS_FLAG1_RELEASE_LEDS 0x08
#define XGPAD_DS_FLAG1_PLAYER_LEDS 0x10
#define XGPAD_DS_FLAG2_LED_BRIGHTNESS 0x01
#define XGPAD_DS_FLAG2_LIGHTBAR_SETUP 0x02
#define XGPAD_DS_FLAG2_COMPATIBLE_VIBRATION2 0x04

#define XGPAD_STATUS_DRIVER_STARTED 0x1

typedef struct XGPAD_SHARED {
  uint32_t magic;  ///< XGPAD_SHARED_MAGIC, written by the producer
  uint32_t version;  ///< XGPAD_SHARED_VERSION, written by the producer
  uint32_t profile;  ///< XGPAD_PROFILE_*, written by the producer; must match the device property

  volatile long inputCount;  ///< number of input reports published so far
  XGPAD_REPORT inputRing[XGPAD_INPUT_RING_SIZE];  ///< report n at index (n - 1) % XGPAD_INPUT_RING_SIZE
  int64_t inputQpc;  ///< producer QueryPerformanceCounter when the latest report was published (diagnostics)

  volatile long outputCount;  ///< number of output reports published so far
  XGPAD_REPORT outputRing[XGPAD_OUTPUT_RING_SIZE];  ///< report n at index (n - 1) % XGPAD_OUTPUT_RING_SIZE
  int64_t outputQpc;  ///< driver QPC when the latest output report arrived

  volatile long driverStatus;  ///< XGPAD_STATUS_*
  volatile long deliveredSeq;  ///< inputCount value of the last report handed to HID class
  int64_t deliveredQpc;  ///< driver QPC when that report completed a read request
  volatile long readsCompleted;
  volatile long outputsReceived;
  volatile long workerWakeups;  ///< driver worker woke on the input event (diagnostics)
  volatile long readRequests;  ///< IOCTL_HID_READ_REPORT requests received from HID class (diagnostics)
  volatile long inputOverruns;  ///< reports lost because the driver fell a full ring behind (diagnostics)
  volatile long inputRejected;  ///< reports dropped because their ID or length does not fit the profile (diagnostics)
  volatile long featureRequests;  ///< feature reports requested by HID clients (diagnostics)
} XGPAD_SHARED;

#ifdef __cplusplus
static_assert(sizeof(XGPAD_INPUT_REPORT) == 16, "Xbox input report payload must be 16 bytes");
static_assert(sizeof(XGPAD_OUTPUT_REPORT) == 8, "Xbox output report payload must be 8 bytes");
static_assert(sizeof(XGPAD_DS_INPUT_REPORT) == 63, "DualSense input report payload must be 63 bytes");
static_assert(sizeof(XGPAD_DS_OUTPUT_REPORT) == 47, "DualSense output report payload must be 47 bytes");
static_assert(sizeof(XGPAD_REPORT) == 2 + XGPAD_MAX_REPORT, "ring entries must be packed");
#endif
