/**
 * @file drivers/xgpad/tools/xgpctl.cpp
 * @brief Test controller for the xgpad driver (must run elevated: SwDeviceCreate + Global\ objects).
 *
 *   xgpctl <pads 1..16> <seconds> [a|b|ds5] [pattern|idle]
 *
 * Creates the shared section/events and one software device per pad, then either plays a button/axis
 * pattern (pad N is offset by N steps so pads are distinguishable) or holds neutral input. Prints the
 * output reports received from the driver and the producer-write -> HID-read-completion latency.
 *
 * a / b: Xbox Series pads with the BLE-style (default) or USB-style hardware ID.
 * ds5:   DualSense pads; the pattern also sweeps the gyro and drags a finger across the touchpad, with a
 *        report every 4 ms like a real pad over USB.
 * Devices disappear when the process exits (SwDeviceCreate lifetime = handle).
 */
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <swdevice.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cmath>

#include "../xgpad_shared.h"

#pragma comment(lib, "onecore.lib")  // SwDeviceCreate/SwDeviceClose
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")

struct Pad {
  unsigned index;
  wchar_t token[XGPAD_TOKEN_MAX] = L"";
  HANDLE section = nullptr;
  XGPAD_SHARED *shared = nullptr;
  HANDLE inputEvent = nullptr;
  HANDLE outputEvent = nullptr;
  HSWDEVICE swd = nullptr;
  HANDLE created = nullptr;
  HRESULT createResult = E_PENDING;
  wchar_t instanceId[256] = L"";
  long lastOutputSeq = 0;
  std::vector<double> latencyUs;
  long pendingSeq = -1;
  int64_t pendingQpc = 0;
};

static double g_qpcToUs;
static std::atomic<bool> g_stop {false};

enum class Mode { xboxBle, xboxUsb, dualsense };
static Mode g_mode = Mode::xboxBle;

static void WriteInput(Pad &pad, uint8_t id, const void *payload, size_t length) {
  XGPAD_SHARED *s = pad.shared;
  const long seq = s->inputCount + 1;  // single writer
  XGPAD_REPORT &slot = s->inputRing[(seq - 1) % XGPAD_INPUT_RING_SIZE];
  slot.id = id;
  slot.length = (uint8_t) length;
  memcpy(slot.data, payload, length);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  s->inputQpc = t.QuadPart;
  MemoryBarrier();
  InterlockedExchange(&s->inputCount, seq);  // publish
  pad.pendingSeq = seq;
  pad.pendingQpc = t.QuadPart;
  SetEvent(pad.inputEvent);
}

static void CollectLatency(Pad &pad) {
  if (pad.pendingSeq >= 0 && pad.shared->deliveredSeq == pad.pendingSeq) {
    pad.latencyUs.push_back((pad.shared->deliveredQpc - pad.pendingQpc) * g_qpcToUs);
    pad.pendingSeq = -1;
  }
}

static void CALLBACK OnCreated(HSWDEVICE, HRESULT hr, PVOID context, PCWSTR instanceId) {
  Pad *pad = (Pad *) context;
  pad->createResult = hr;
  if (instanceId) {
    wcsncpy_s(pad->instanceId, instanceId, _TRUNCATE);
  }
  SetEvent(pad->created);
}

static bool CreateObjects(Pad &pad, PSECURITY_DESCRIPTOR sd) {
  SECURITY_ATTRIBUTES sa {sizeof(sa), sd, FALSE};
  wchar_t name[64];
  swprintf_s(name, XGPAD_SECTION_NAME_FMT, pad.token);
  pad.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(XGPAD_SHARED), name);
  if (!pad.section) {
    printf("CreateFileMapping %ls failed %lu\n", name, GetLastError());
    return false;
  }
  pad.shared = (XGPAD_SHARED *) MapViewOfFile(pad.section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(XGPAD_SHARED));
  memset(pad.shared, 0, sizeof(*pad.shared));
  pad.shared->magic = XGPAD_SHARED_MAGIC;
  pad.shared->version = XGPAD_SHARED_VERSION;  // the driver starts from a neutral state
  pad.shared->profile = g_mode == Mode::dualsense ? XGPAD_PROFILE_DUALSENSE : XGPAD_PROFILE_XBOX_SERIES;
  swprintf_s(name, XGPAD_INPUT_EVENT_NAME_FMT, pad.token);
  pad.inputEvent = CreateEventW(&sa, FALSE, FALSE, name);
  swprintf_s(name, XGPAD_OUTPUT_EVENT_NAME_FMT, pad.token);
  pad.outputEvent = CreateEventW(&sa, FALSE, FALSE, name);
  return pad.inputEvent && pad.outputEvent;
}

static bool CreateDevice(Pad &pad) {
  wchar_t instance[32];
  swprintf_s(instance, XGPAD_SWD_INSTANCE_FMT, pad.index);
  // REG_MULTI_SZ style list
  const wchar_t *hwB = L"SunshineXgpad\\VID_045E&PID_0B13\0";

  SW_DEVICE_CREATE_INFO info {};
  info.cbSize = sizeof(info);
  info.pszInstanceId = instance;
  info.pszzCompatibleIds = L"\0";
  info.CapabilityFlags = SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;

  // Xbox BLE: BLE-style enumerator (child path identical to a real BLE pad). Xbox USB: USB-style.
  const wchar_t *enumerator;
  ULONG profile;
  switch (g_mode) {
    case Mode::dualsense:
      info.pszzHardwareIds = XGPAD_HARDWARE_IDS_DUALSENSE;
      info.pszDeviceDescription = L"Sunshine Virtual DualSense Controller";
      enumerator = XGPAD_SWD_ENUMERATOR_DUALSENSE;
      profile = XGPAD_PROFILE_DUALSENSE;
      break;
    case Mode::xboxUsb:
      info.pszzHardwareIds = hwB;
      info.pszDeviceDescription = L"Sunshine Virtual Xbox Series Controller";
      enumerator = XGPAD_SWD_ENUMERATOR_USB_STYLE;
      profile = XGPAD_PROFILE_XBOX_SERIES;
      break;
    default:
      info.pszzHardwareIds = XGPAD_HARDWARE_IDS;
      info.pszDeviceDescription = L"Sunshine Virtual Xbox Series Controller";
      enumerator = XGPAD_SWD_ENUMERATOR;
      profile = XGPAD_PROFILE_XBOX_SERIES;
      break;
  }

  pad.created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  static const DEVPROPKEY tokenKey = {XGPAD_TOKEN_PROPKEY_GUID, XGPAD_TOKEN_PROPKEY_PID};
  static const DEVPROPKEY profileKey = {XGPAD_TOKEN_PROPKEY_GUID, XGPAD_PROFILE_PROPKEY_PID};
  DEVPROPERTY props[2] {};
  props[0].CompKey.Key = tokenKey;
  props[0].CompKey.Store = DEVPROP_STORE_SYSTEM;
  props[0].Type = DEVPROP_TYPE_STRING;
  props[0].BufferSize = (ULONG) ((wcslen(pad.token) + 1) * sizeof(wchar_t));
  props[0].Buffer = pad.token;
  props[1].CompKey.Key = profileKey;
  props[1].CompKey.Store = DEVPROP_STORE_SYSTEM;
  props[1].Type = DEVPROP_TYPE_UINT32;
  props[1].BufferSize = sizeof(profile);
  props[1].Buffer = &profile;
  HRESULT hr = SwDeviceCreate(enumerator, L"HTREE\\ROOT\\0", &info, 2, props, OnCreated, &pad, &pad.swd);
  if (FAILED(hr)) {
    printf("pad %u: SwDeviceCreate failed 0x%08lx\n", pad.index, hr);
    return false;
  }
  WaitForSingleObject(pad.created, 15000);
  printf("pad %u: created hr=0x%08lx instance=%ls\n", pad.index, pad.createResult, pad.instanceId);
  return SUCCEEDED(pad.createResult);
}

static DWORD WINAPI OutputThread(LPVOID param) {
  auto &pads = *(std::vector<Pad> *) param;
  std::vector<HANDLE> events;
  for (auto &p : pads) {
    events.push_back(p.outputEvent);
  }
  while (!g_stop) {
    DWORD w = WaitForMultipleObjects((DWORD) events.size(), events.data(), FALSE, 200);
    if (w >= WAIT_OBJECT_0 + events.size()) {
      continue;
    }
    Pad &p = pads[w - WAIT_OBJECT_0];
    const long available = p.shared->outputCount;
    MemoryBarrier();
    if (available - p.lastOutputSeq > (long) XGPAD_OUTPUT_RING_SIZE) {
      printf("OUTPUT pad %u: missed %ld reports\n", p.index, available - p.lastOutputSeq - (long) XGPAD_OUTPUT_RING_SIZE);
      p.lastOutputSeq = available - (long) XGPAD_OUTPUT_RING_SIZE;
    }
    for (; p.lastOutputSeq < available; p.lastOutputSeq++) {
      const XGPAD_REPORT &entry = p.shared->outputRing[p.lastOutputSeq % XGPAD_OUTPUT_RING_SIZE];
      if (entry.id == XGPAD_OUTPUT_REPORT_ID) {
        XGPAD_OUTPUT_REPORT out;
        memcpy(&out, entry.data, sizeof(out));
        printf("RUMBLE pad %u: enable=%x LT=%3u RT=%3u L=%3u R=%3u dur=%u delay=%u loop=%u\n", p.index, out.enable, out.leftTrigger,
               out.rightTrigger, out.leftMotor, out.rightMotor, out.duration, out.startDelay, out.loopCount);
      } else if (entry.id == XGPAD_DS_OUTPUT_REPORT_ID) {
        XGPAD_DS_OUTPUT_REPORT out;
        memcpy(&out, entry.data, sizeof(out));
        printf("DS5 pad %u: flags=%02x/%02x/%02x motor L=%3u R=%3u", p.index, out.validFlag0, out.validFlag1, out.validFlag2, out.motorLeft,
               out.motorRight);
        if (out.validFlag0 & (XGPAD_DS_FLAG0_LEFT_TRIGGER_EFFECT | XGPAD_DS_FLAG0_RIGHT_TRIGGER_EFFECT)) {
          printf(" L2 mode=%02x R2 mode=%02x", out.leftTriggerEffect[0], out.rightTriggerEffect[0]);
        }
        if (out.validFlag1 & XGPAD_DS_FLAG1_LIGHTBAR) {
          printf(" lightbar=%02x%02x%02x", out.lightbarRed, out.lightbarGreen, out.lightbarBlue);
        }
        if (out.validFlag1 & XGPAD_DS_FLAG1_PLAYER_LEDS) {
          printf(" players=%02x", out.playerLeds);
        }
        printf("\n");
      } else {
        printf("OUTPUT pad %u: id=%02x len=%u\n", p.index, entry.id, entry.length);
      }
    }
  }
  return 0;
}

struct Step {
  const char *name;
  uint16_t buttons;
  uint8_t hat;
  uint8_t share;
  int axis;  // 0 none, 1 LT, 2 RT, 3 LX+, 4 LY+(down), 5 RX+, 6 RY+(down)
};

static const Step kSteps[] = {
  {"A", XGPAD_BTN_A, 0, 0, 0},
  {"B", XGPAD_BTN_B, 0, 0, 0},
  {"X", XGPAD_BTN_X, 0, 0, 0},
  {"Y", XGPAD_BTN_Y, 0, 0, 0},
  {"LB", XGPAD_BTN_LB, 0, 0, 0},
  {"RB", XGPAD_BTN_RB, 0, 0, 0},
  {"View", XGPAD_BTN_VIEW, 0, 0, 0},
  {"Menu", XGPAD_BTN_MENU, 0, 0, 0},
  {"LS", XGPAD_BTN_LS, 0, 0, 0},
  {"RS", XGPAD_BTN_RS, 0, 0, 0},
  {"DpadUp", 0, 1, 0, 0},
  {"DpadRight", 0, 3, 0, 0},
  {"DpadDown", 0, 5, 0, 0},
  {"DpadLeft", 0, 7, 0, 0},
  {"LT full", 0, 0, 0, 1},
  {"RT full", 0, 0, 0, 2},
  {"LX right", 0, 0, 0, 3},
  {"LY down", 0, 0, 0, 4},
  {"RX right", 0, 0, 0, 5},
  {"RY down", 0, 0, 0, 6},
  {"Share", 0, 0, 1, 0},
  {"Guide", XGPAD_BTN_GUIDE, 0, 0, 0},
};
static const int kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

// DualSense pattern: the same steps on the DualSense layout, plus touch and mic.
struct DsStep {
  const char *name;
  uint8_t buttons0;  // face buttons (upper nibble)
  uint8_t buttons1;
  uint8_t buttons2;
  uint8_t hat;  // 8 = released
  int axis;  // 0 none, 1 L2, 2 R2, 3 LX+, 4 LY+(down), 5 RX+, 6 RY+(down)
  bool touch;  // drag a finger across the touchpad
};

static const DsStep kDsSteps[] = {
  {"Cross", XGPAD_DS_BTN0_CROSS, 0, 0, 8, 0, false},
  {"Circle", XGPAD_DS_BTN0_CIRCLE, 0, 0, 8, 0, false},
  {"Square", XGPAD_DS_BTN0_SQUARE, 0, 0, 8, 0, false},
  {"Triangle", XGPAD_DS_BTN0_TRIANGLE, 0, 0, 8, 0, false},
  {"L1", 0, XGPAD_DS_BTN1_L1, 0, 8, 0, false},
  {"R1", 0, XGPAD_DS_BTN1_R1, 0, 8, 0, false},
  {"Create", 0, XGPAD_DS_BTN1_CREATE, 0, 8, 0, false},
  {"Options", 0, XGPAD_DS_BTN1_OPTIONS, 0, 8, 0, false},
  {"L3", 0, XGPAD_DS_BTN1_L3, 0, 8, 0, false},
  {"R3", 0, XGPAD_DS_BTN1_R3, 0, 8, 0, false},
  {"DpadUp", 0, 0, 0, 0, 0, false},
  {"DpadRight", 0, 0, 0, 2, 0, false},
  {"DpadDown", 0, 0, 0, 4, 0, false},
  {"DpadLeft", 0, 0, 0, 6, 0, false},
  {"L2 full", 0, XGPAD_DS_BTN1_L2, 0, 8, 1, false},
  {"R2 full", 0, XGPAD_DS_BTN1_R2, 0, 8, 2, false},
  {"LX right", 0, 0, 0, 8, 3, false},
  {"LY down", 0, 0, 0, 8, 4, false},
  {"RX right", 0, 0, 0, 8, 5, false},
  {"RY down", 0, 0, 0, 8, 6, false},
  {"Touch drag", 0, 0, 0, 8, 0, true},
  {"Touchpad click", 0, 0, XGPAD_DS_BTN2_TOUCHPAD, 8, 0, false},
  {"Mic", 0, 0, XGPAD_DS_BTN2_MIC, 8, 0, false},
  {"PS", 0, 0, XGPAD_DS_BTN2_PS, 8, 0, false},
};
static const int kDsStepCount = sizeof(kDsSteps) / sizeof(kDsSteps[0]);

// Per-pad DualSense state carried between reports.
struct DsState {
  uint8_t seq = 0;
  uint8_t touchId = 0;
  bool touching = false;
};

static void WriteDsReport(Pad &pad, DsState &st, const DsStep *step, double elapsedS, double stepFraction) {
  XGPAD_DS_INPUT_REPORT r {};
  r.lx = r.ly = r.rx = r.ry = 0x80;
  r.buttons[0] = XGPAD_DS_HAT_RELEASED;
  r.seq = st.seq++;
  if (step) {
    r.buttons[0] = (uint8_t) (step->buttons0 | step->hat);
    r.buttons[1] = step->buttons1;
    r.buttons[2] = step->buttons2;
    switch (step->axis) {
      case 1: r.l2 = 255; break;
      case 2: r.r2 = 255; break;
      case 3: r.lx = 255; break;
      case 4: r.ly = 255; break;
      case 5: r.rx = 255; break;
      case 6: r.ry = 255; break;
    }
  }
  // Gyro: yaw sweeps +/-90 deg/s with a 2 s period; accelerometer: 1 g on Y (pad lying flat).
  r.gyro[1] = (int16_t) (90.0 * XGPAD_DS_GYRO_RES_PER_DEG_S * sin(elapsedS * 3.14159265358979));
  r.accel[1] = XGPAD_DS_ACC_RES_PER_G;
  r.sensorTimestamp = (uint32_t) (elapsedS * 3000000.0);  // 0.33 us units

  const bool touching = step && step->touch;
  if (touching && !st.touching) {
    st.touchId = (uint8_t) ((st.touchId + 1) & 0x7F);
  }
  st.touching = touching;
  r.points[0].contact = (uint8_t) (st.touchId | (touching ? 0 : XGPAD_DS_TOUCH_NOT_TOUCHING));
  r.points[1].contact = XGPAD_DS_TOUCH_NOT_TOUCHING;
  if (touching) {
    const unsigned x = (unsigned) (stepFraction * (XGPAD_DS_TOUCHPAD_WIDTH - 1));
    const unsigned y = XGPAD_DS_TOUCHPAD_HEIGHT / 2;
    r.points[0].xLow = (uint8_t) (x & 0xFF);
    r.points[0].xHighYLow = (uint8_t) (((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    r.points[0].yHigh = (uint8_t) (y >> 4);
  }
  r.status = (XGPAD_DS_POWER_DISCHARGING << 4) | 8;
  WriteInput(pad, XGPAD_DS_INPUT_REPORT_ID, &r, sizeof(r));
}

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned count = argc > 1 ? (unsigned) atoi(argv[1]) : 1;
  int seconds = argc > 2 ? atoi(argv[2]) : 60;
  if (argc > 3 && !strcmp(argv[3], "b")) {
    g_mode = Mode::xboxUsb;
  } else if (argc > 3 && !strcmp(argv[3], "ds5")) {
    g_mode = Mode::dualsense;
  }
  bool pattern = !(argc > 4 && !strcmp(argv[4], "idle"));
  count = std::clamp(count, 1u, (unsigned) XGPAD_MAX_PADS);

  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcToUs = 1e6 / (double) f.QuadPart;
  timeBeginPeriod(1);

  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(XGPAD_OBJECT_SDDL, SDDL_REVISION_1, &sd, nullptr)) {
    printf("SDDL failed %lu\n", GetLastError());
    return 1;
  }

  std::vector<Pad> pads(count);
  for (unsigned i = 0; i < count; i++) {
    pads[i].index = i;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    swprintf_s(pads[i].token, L"%u_%016llx", i, (unsigned long long) t.QuadPart ^ ((unsigned long long) GetCurrentProcessId() << 40));
    printf("pad %u token %ls\n", i, pads[i].token);
    if (!CreateObjects(pads[i], sd)) {
      return 2;
    }
  }
  static const char *const modeNames[] = {"Xbox Series, BLE-style ID", "Xbox Series, USB-style ID", "DualSense"};
  printf("%s, %u pad(s), %s for %d s\n", modeNames[(int) g_mode], count, pattern ? "pattern" : "idle", seconds);
  for (auto &p : pads) {
    if (!CreateDevice(p)) {
      return 3;
    }
  }

  // Give PnP time to start the stacks before reporting driver status.
  for (int i = 0; i < 50; i++) {
    bool all = true;
    for (auto &p : pads) {
      all &= (p.shared->driverStatus & XGPAD_STATUS_DRIVER_STARTED) != 0;
    }
    if (all) {
      break;
    }
    Sleep(100);
  }
  for (auto &p : pads) {
    printf("pad %u: driverStatus=%ld readsCompleted=%ld\n", p.index, p.shared->driverStatus, p.shared->readsCompleted);
  }

  HANDLE outThread = CreateThread(nullptr, 0, OutputThread, &pads, 0, nullptr);

  ULONGLONG start = GetTickCount64();
  int lastStep = -1;
  std::vector<DsState> dsStates(count);
  ULONGLONG lastDsReport = 0;
  while (GetTickCount64() - start < (ULONGLONG) seconds * 1000) {
    ULONGLONG elapsed = GetTickCount64() - start;
    int step = (int) (elapsed / 700);
    bool released = (elapsed % 700) >= 400;  // 400 ms pressed, 300 ms released
    int phase = step * 2 + (released ? 1 : 0);
    if (g_mode == Mode::dualsense) {
      // A real DualSense reports every 4 ms over USB, changed or not.
      if (elapsed - lastDsReport >= 4) {
        lastDsReport = elapsed;
        for (auto &p : pads) {
          const DsStep *s = pattern && !released ? &kDsSteps[(step + p.index) % kDsStepCount] : nullptr;
          if (phase != lastStep && s && p.index == 0) {
            printf("t=%6.1fs pad0 -> %s\n", elapsed / 1000.0, s->name);
          }
          WriteDsReport(p, dsStates[p.index], s, elapsed / 1000.0, (elapsed % 700) / 400.0);
        }
        lastStep = phase;
      }
    } else if (pattern && phase != lastStep) {
      lastStep = phase;
      for (auto &p : pads) {
        const Step &s = kSteps[(step + p.index) % kStepCount];
        XGPAD_INPUT_REPORT r {};
        r.lx = r.ly = r.rx = r.ry = 0x8000;
        if (!released) {
          r.buttons = s.buttons;
          r.hat = s.hat;
          r.share = s.share;
          switch (s.axis) {
            case 1: r.lt = 1023; break;
            case 2: r.rt = 1023; break;
            case 3: r.lx = 0xFFFF; break;
            case 4: r.ly = 0xFFFF; break;
            case 5: r.rx = 0xFFFF; break;
            case 6: r.ry = 0xFFFF; break;
          }
          if (p.index == 0) {
            printf("t=%6.1fs pad0 -> %s\n", elapsed / 1000.0, s.name);
          }
        }
        WriteInput(p, XGPAD_INPUT_REPORT_ID, &r, sizeof(r));
      }
    }
    for (auto &p : pads) {
      CollectLatency(p);
    }
    Sleep(1);
  }

  g_stop = true;
  WaitForSingleObject(outThread, 1000);
  for (auto &p : pads) {
    std::sort(p.latencyUs.begin(), p.latencyUs.end());
    size_t n = p.latencyUs.size();
    printf("pad %u: reads=%ld outputs=%ld write->read-complete latency n=%zu", p.index, p.shared->readsCompleted, p.shared->outputsReceived, n);
    if (n) {
      printf(" p50=%.0fus p99=%.0fus max=%.0fus", p.latencyUs[n / 2], p.latencyUs[std::min(n - 1, n * 99 / 100)], p.latencyUs[n - 1]);
    }
    printf("\n");
    SwDeviceClose(p.swd);
  }
  LocalFree(sd);
  return 0;
}
