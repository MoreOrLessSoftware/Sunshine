/**
 * @file drivers/xgpad/xgpad_shared.h
 * @brief Shared-memory IPC contract between Sunshine and the xgpad UMDF2 virtual Xbox Series controller driver.
 *
 * One virtual pad = one software device (SWD\SunshineXgpad\Pad<N>) + one named section + two auto-reset events.
 * The producer (Sunshine) creates the section and events *before* creating the device, so the driver can open
 * them when it starts.
 *
 * Input path:  the producer appends every XGPAD_INPUT_REPORT to the inputRing (single writer), publishes it by
 *              incrementing inputCount, then sets the input event. The driver drains the ring in order and
 *              completes pended IOCTL_HID_READ_REPORT requests (report ID 0x01). Nothing is lost unless the driver
 *              falls more than XGPAD_INPUT_RING_SIZE reports behind.
 * Output path: the driver receives HID output report 0x03 (rumble incl. impulse triggers), writes it into Output
 *              under the OutputSeq seqlock, then sets the output event.
 *
 * Input ring: report n (1-based) lives in inputRing[(n - 1) % XGPAD_INPUT_RING_SIZE]. A reader that copied report n
 * must re-check inputCount afterwards: if it advanced by XGPAD_INPUT_RING_SIZE or more, the copy may be torn.
 *
 * Output seqlock (single writer): seq becomes odd, data is written, seq becomes even. Readers retry while seq is odd
 * or changed across the copy.
 */
#pragma once

#include <stdint.h>

#define XGPAD_MAX_PADS 16
#define XGPAD_SHARED_MAGIC 0x44415047u  // 'GPAD'
#define XGPAD_SHARED_VERSION 3u
#define XGPAD_INPUT_RING_SIZE 16u

// Object names; %ls = per-device token. The producer generates a fresh token for every device it creates and
// passes it to the driver through the XGPAD_TOKEN_PROPKEY device property, so a stale or second driver
// instance can never share (and steal wake-ups from) another device's events. Without the property the
// driver falls back to the decimal pad index as token.
#define XGPAD_SECTION_NAME_FMT L"Global\\SunshineXgpad_%ls"
#define XGPAD_INPUT_EVENT_NAME_FMT L"Global\\SunshineXgpad_%ls_in"
#define XGPAD_OUTPUT_EVENT_NAME_FMT L"Global\\SunshineXgpad_%ls_out"
#define XGPAD_TOKEN_MAX 40  ///< Token length limit in characters, including the terminator.

// Custom device property (DEVPROP_TYPE_STRING) carrying the token: {6c1f8d2e-3b7a-4e59-9a41-58d2c7e0b913}, 2
#define XGPAD_TOKEN_PROPKEY_GUID {0x6c1f8d2e, 0x3b7a, 0x4e59, {0x9a, 0x41, 0x58, 0xd2, 0xc7, 0xe0, 0xb9, 0x13}}
#define XGPAD_TOKEN_PROPKEY_PID 2

// SwDeviceCreate parameters.
// HID class derives the HID child's device ID from the parent's *enumerator* segment
// (SWD\<enumerator>\<instance> -> HID\<enumerator with _ replaced by &>&IG_00\...). XInput, GameInput and SDL
// parse VID/PID/REV from that path, so the enumerator name must carry them, BLE-style like a real pad:
//   HID\{00001812-0000-1000-8000-00805f9b34fb}&Dev&VID_045e&PID_0b13&REV_0509&IG_00
#define XGPAD_SWD_ENUMERATOR L"{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02045e_PID&0b13_REV&0509"
#define XGPAD_SWD_ENUMERATOR_USB_STYLE L"VID_045E&PID_0B13&REV_0509"
#define XGPAD_SWD_INSTANCE_FMT L"Pad%u"

// WUDFHost runs drivers as LOCAL SERVICE: grant SYSTEM, Administrators and LOCAL SERVICE full access.
#define XGPAD_OBJECT_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)"

#define XGPAD_INPUT_REPORT_ID 0x01
#define XGPAD_OUTPUT_REPORT_ID 0x03

#pragma pack(push, 1)

/**
 * @brief Input report 0x01 payload (16 bytes, report ID not included).
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
 * @brief Output report 0x03 payload (8 bytes, report ID not included): rumble incl. impulse triggers.
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

#pragma pack(pop)

// Button bits (HID button usages 1..15).
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

#define XGPAD_STATUS_DRIVER_STARTED 0x1

typedef struct XGPAD_SHARED {
  uint32_t magic;  ///< XGPAD_SHARED_MAGIC, written by the producer
  uint32_t version;  ///< XGPAD_SHARED_VERSION, written by the producer

  volatile long inputCount;  ///< number of input reports published so far
  XGPAD_INPUT_REPORT inputRing[XGPAD_INPUT_RING_SIZE];  ///< report n at index (n - 1) % XGPAD_INPUT_RING_SIZE
  int64_t inputQpc;  ///< producer QueryPerformanceCounter when the latest report was published (diagnostics)

  volatile long outputSeq;
  XGPAD_OUTPUT_REPORT output;
  int64_t outputQpc;  ///< driver QPC when the output report arrived

  volatile long driverStatus;  ///< XGPAD_STATUS_*
  volatile long deliveredSeq;  ///< inputCount value of the last report handed to HID class
  int64_t deliveredQpc;  ///< driver QPC when that report completed a read request
  volatile long readsCompleted;
  volatile long outputsReceived;
  volatile long workerWakeups;  ///< driver worker woke on the input event (diagnostics)
  volatile long readRequests;  ///< IOCTL_HID_READ_REPORT requests received from HID class (diagnostics)
  volatile long inputOverruns;  ///< reports lost because the driver fell a full ring behind (diagnostics)
} XGPAD_SHARED;

#ifdef __cplusplus
static_assert(sizeof(XGPAD_INPUT_REPORT) == 16, "input report payload must be 16 bytes");
static_assert(sizeof(XGPAD_OUTPUT_REPORT) == 8, "output report payload must be 8 bytes");
#endif
