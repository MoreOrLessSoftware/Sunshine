/**
 * @file drivers/xgpad/tools/xgpctl.cpp
 * @brief Test controller for the xgpad driver (must run elevated: SwDeviceCreate + Global\ objects).
 *
 *   xgpctl <pads 1..16> <seconds> [a|b] [pattern|idle]
 *
 * Creates the shared section/events and one software device per pad, then either plays a button/axis
 * pattern (pad N is offset by N steps so pads are distinguishable) or holds neutral input. Prints rumble
 * output reports received from the driver and the producer-write -> HID-read-completion latency.
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

static void WriteInput(Pad &pad, const XGPAD_INPUT_REPORT &r) {
  XGPAD_SHARED *s = pad.shared;
  const long seq = s->inputCount + 1;  // single writer
  memcpy((void *) &s->inputRing[(seq - 1) % XGPAD_INPUT_RING_SIZE], &r, sizeof(r));
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
  swprintf_s(name, XGPAD_INPUT_EVENT_NAME_FMT, pad.token);
  pad.inputEvent = CreateEventW(&sa, FALSE, FALSE, name);
  swprintf_s(name, XGPAD_OUTPUT_EVENT_NAME_FMT, pad.token);
  pad.outputEvent = CreateEventW(&sa, FALSE, FALSE, name);
  return pad.inputEvent && pad.outputEvent;
}

static bool CreateDevice(Pad &pad, bool variantA) {
  wchar_t instance[32];
  swprintf_s(instance, XGPAD_SWD_INSTANCE_FMT, pad.index);
  // REG_MULTI_SZ style lists
  const wchar_t *hwA = L"SunshineXgpad\\{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02045e_PID&0b13\0";
  const wchar_t *hwB = L"SunshineXgpad\\VID_045E&PID_0B13\0";

  SW_DEVICE_CREATE_INFO info {};
  info.cbSize = sizeof(info);
  info.pszInstanceId = instance;
  info.pszzHardwareIds = variantA ? hwA : hwB;
  info.pszzCompatibleIds = L"\0";
  info.CapabilityFlags = SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;
  info.pszDeviceDescription = L"Sunshine Virtual Xbox Series Controller";

  pad.created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // Variant A: BLE-style enumerator (child path identical to a real BLE pad). Variant B: USB-style.
  const wchar_t *enumerator = variantA ? XGPAD_SWD_ENUMERATOR : XGPAD_SWD_ENUMERATOR_USB_STYLE;
  static const DEVPROPKEY tokenKey = {XGPAD_TOKEN_PROPKEY_GUID, XGPAD_TOKEN_PROPKEY_PID};
  DEVPROPERTY prop {};
  prop.CompKey.Key = tokenKey;
  prop.CompKey.Store = DEVPROP_STORE_SYSTEM;
  prop.Type = DEVPROP_TYPE_STRING;
  prop.BufferSize = (ULONG) ((wcslen(pad.token) + 1) * sizeof(wchar_t));
  prop.Buffer = pad.token;
  HRESULT hr = SwDeviceCreate(enumerator, L"HTREE\\ROOT\\0", &info, 1, &prop, OnCreated, &pad, &pad.swd);
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
    XGPAD_OUTPUT_REPORT out;
    long seq;
    do {
      seq = p.shared->outputSeq;
      MemoryBarrier();
      memcpy(&out, (const void *) &p.shared->output, sizeof(out));
      MemoryBarrier();
    } while ((seq & 1) || seq != p.shared->outputSeq);
    printf("RUMBLE pad %u: enable=%x LT=%3u RT=%3u L=%3u R=%3u dur=%u delay=%u loop=%u\n", p.index, out.enable, out.leftTrigger,
           out.rightTrigger, out.leftMotor, out.rightMotor, out.duration, out.startDelay, out.loopCount);
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

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  unsigned count = argc > 1 ? (unsigned) atoi(argv[1]) : 1;
  int seconds = argc > 2 ? atoi(argv[2]) : 60;
  bool variantA = !(argc > 3 && !strcmp(argv[3], "b"));
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
  printf("hardware id variant %s, %u pad(s), %s for %d s\n", variantA ? "A (BLE-style)" : "B (USB-style)", count,
         pattern ? "pattern" : "idle", seconds);
  for (auto &p : pads) {
    if (!CreateDevice(p, variantA)) {
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
  while (GetTickCount64() - start < (ULONGLONG) seconds * 1000) {
    ULONGLONG elapsed = GetTickCount64() - start;
    int step = (int) (elapsed / 700);
    bool released = (elapsed % 700) >= 400;  // 400 ms pressed, 300 ms released
    int phase = step * 2 + (released ? 1 : 0);
    if (pattern && phase != lastStep) {
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
        WriteInput(p, r);
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
