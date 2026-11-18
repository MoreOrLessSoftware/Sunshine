/**
 * @file drivers/xgpad/driver/xgpad.c
 * @brief UMDF2 HID minidriver: virtual Xbox Series X|S controller or DualSense controller.
 *
 * The XGPAD_PROFILE_PROPKEY device property picks the profile:
 *   Xbox Series (Bluetooth LE layout, 045E:0B13): xinputhid (upper filter) -> mshidumdf (function) -> WUDFRd -> SWD.
 *   DualSense (USB layout, 054C:0CE6): mshidumdf (function) -> WUDFRd -> SWD.
 * The driver is a pipe: Sunshine formats input reports in shared memory and signals an event; a worker thread
 * completes the pending IOCTL_HID_READ_REPORT. Output reports (Xbox 0x03 rumble, DualSense 0x02 effects) are copied
 * back to shared memory and signalled. The DualSense profile also answers the feature reports hosts read at start
 * (calibration, pairing info, firmware info) from fixed data.
 */
#define WIN32_NO_STATUS  // ntstatus.h (via wdf.h) provides the STATUS_* codes
#include <windows.h>
#undef WIN32_NO_STATUS
#include <wdf.h>
#include <hidport.h>
#include <initguid.h>
#include <devpkey.h>
#include <stdio.h>
#include <wchar.h>

#include "../xgpad_shared.h"

#define XGPAD_VERSION_NUMBER 0x0509  // firmware 5.9, matches the reference controller
#define XGPAD_DS_VERSION_NUMBER 0x0100

static const UCHAR g_XboxReportDescriptor[] = {
  0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xff,
  0xff, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xc0, 0x09, 0x01, 0xa1, 0x00, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00,
  0x27, 0xff, 0xff, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xc0, 0x05, 0x02, 0x09, 0xc5, 0x15, 0x00, 0x26, 0xff,
  0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03, 0x05, 0x02, 0x09,
  0xc4, 0x15, 0x00, 0x26, 0xff, 0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01,
  0x81, 0x03, 0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x66, 0x14, 0x00, 0x75, 0x04,
  0x95, 0x01, 0x81, 0x42, 0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x00, 0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x0f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0f, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00,
  0x75, 0x01, 0x95, 0x01, 0x81, 0x03, 0x05, 0x0c, 0x0a, 0xb2, 0x00, 0x15, 0x00, 0x25, 0x01, 0x95, 0x01, 0x75, 0x01, 0x81,
  0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03, 0x05, 0x0f, 0x09, 0x21, 0x85, 0x03, 0xa1, 0x02, 0x09,
  0x97, 0x15, 0x00, 0x25, 0x01, 0x75, 0x04, 0x95, 0x01, 0x91, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x04, 0x95, 0x01, 0x91,
  0x03, 0x09, 0x70, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x04, 0x91, 0x02, 0x09, 0x50, 0x66, 0x01, 0x10, 0x55, 0x0e,
  0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02, 0x09, 0xa7, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
  0x95, 0x01, 0x91, 0x02, 0x65, 0x00, 0x55, 0x00, 0x09, 0x7c, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91,
  0x02, 0xc0, 0xc0,
};

C_ASSERT(sizeof(g_XboxReportDescriptor) == 283);

// DualSense over USB: input 0x01 (63 bytes), output 0x02 (47 bytes) and the vendor feature reports, all sized like a
// real pad's, so hosts that read or write a feature report by its known length get exactly that length.
static const UCHAR g_DsReportDescriptor[] = {
  0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34,
  0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0x06, 0x00, 0xff, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02,
  0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81,
  0x42, 0x65, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0f, 0x81, 0x02, 0x06,
  0x00, 0xff, 0x09, 0x21, 0x95, 0x0d, 0x81, 0x02, 0x06, 0x00, 0xff, 0x09, 0x22, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
  0x95, 0x34, 0x81, 0x02, 0x85, 0x02, 0x09, 0x23, 0x95, 0x2f, 0x91, 0x02, 0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xb1, 0x02,
  0x85, 0x08, 0x09, 0x34, 0x95, 0x2f, 0xb1, 0x02, 0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xb1, 0x02, 0x85, 0x0a, 0x09, 0x25,
  0x95, 0x1a, 0xb1, 0x02, 0x85, 0x20, 0x09, 0x26, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x21, 0x09, 0x27, 0x95, 0x04, 0xb1, 0x02,
  0x85, 0x22, 0x09, 0x40, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x80, 0x09, 0x28, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x81, 0x09, 0x29,
  0x95, 0x3f, 0xb1, 0x02, 0x85, 0x82, 0x09, 0x2a, 0x95, 0x09, 0xb1, 0x02, 0x85, 0x83, 0x09, 0x2b, 0x95, 0x3f, 0xb1, 0x02,
  0x85, 0x84, 0x09, 0x2c, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0x85, 0x09, 0x2d, 0x95, 0x02, 0xb1, 0x02, 0x85, 0xa0, 0x09, 0x2e,
  0x95, 0x01, 0xb1, 0x02, 0x85, 0xe0, 0x09, 0x2f, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf0, 0x09, 0x30, 0x95, 0x3f, 0xb1, 0x02,
  0x85, 0xf1, 0x09, 0x31, 0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf2, 0x09, 0x32, 0x95, 0x0f, 0xb1, 0x02, 0x85, 0xf4, 0x09, 0x35,
  0x95, 0x3f, 0xb1, 0x02, 0x85, 0xf5, 0x09, 0x36, 0x95, 0x03, 0xb1, 0x02, 0xc0,
};

C_ASSERT(sizeof(g_DsReportDescriptor) == 273);
C_ASSERT(sizeof(XGPAD_INPUT_REPORT) == 16);
C_ASSERT(sizeof(XGPAD_OUTPUT_REPORT) == 8);
C_ASSERT(sizeof(XGPAD_DS_INPUT_REPORT) == 63);
C_ASSERT(sizeof(XGPAD_DS_OUTPUT_REPORT) == 47);

// A feature report the descriptor declares: ID and payload length (report ID not included).
typedef struct FEATURE_DESC {
  UCHAR id;
  UCHAR length;
} FEATURE_DESC;

static const FEATURE_DESC g_DsFeatures[] = {
  {0x05, 40}, {0x08, 47}, {0x09, 19}, {0x0A, 26}, {0x20, 63}, {0x21, 4}, {0x22, 63}, {0x80, 63}, {0x81, 63}, {0x82, 9},
  {0x83, 63}, {0x84, 63}, {0x85, 2}, {0xA0, 1}, {0xE0, 63}, {0xF0, 63}, {0xF1, 63}, {0xF2, 15}, {0xF4, 63}, {0xF5, 3},
};

// What a profile presents to HID class.
typedef struct PROFILE_DESC {
  const UCHAR *reportDescriptor;
  USHORT reportDescriptorLength;
  USHORT vendorId;
  USHORT productId;
  USHORT versionNumber;
  UCHAR inputReportId;
  UCHAR inputLength;  // payload bytes, report ID not included
  UCHAR outputReportId;
  UCHAR outputLength;  // payload bytes, report ID not included
  BOOLEAN mergeAnalog;  // fold analog-only changes into the newest undelivered report
  const wchar_t *manufacturer;
  const wchar_t *product;
  const FEATURE_DESC *features;
  ULONG featureCount;
} PROFILE_DESC;

static const PROFILE_DESC g_Profiles[] = {
  // XGPAD_PROFILE_XBOX_SERIES
  {
    g_XboxReportDescriptor,
    sizeof(g_XboxReportDescriptor),
    0x045E,
    0x0B13,
    XGPAD_VERSION_NUMBER,
    XGPAD_INPUT_REPORT_ID,
    sizeof(XGPAD_INPUT_REPORT),
    XGPAD_OUTPUT_REPORT_ID,
    sizeof(XGPAD_OUTPUT_REPORT),
    TRUE,
    L"Microsoft",
    L"Xbox Wireless Controller",
    NULL,
    0,
  },
  // XGPAD_PROFILE_DUALSENSE
  {
    g_DsReportDescriptor,
    sizeof(g_DsReportDescriptor),
    0x054C,
    0x0CE6,
    XGPAD_DS_VERSION_NUMBER,
    XGPAD_DS_INPUT_REPORT_ID,
    sizeof(XGPAD_DS_INPUT_REPORT),
    XGPAD_DS_OUTPUT_REPORT_ID,
    sizeof(XGPAD_DS_OUTPUT_REPORT),
    FALSE,  // every motion sample and touch change is its own report, like a real pad's
    L"Sony Interactive Entertainment",
    L"DualSense Wireless Controller",
    g_DsFeatures,
    ARRAYSIZE(g_DsFeatures),
  },
};

C_ASSERT(ARRAYSIZE(g_Profiles) == XGPAD_PROFILE_DUALSENSE + 1);

#define REPORT_BUFFER_SIZE (1 + XGPAD_MAX_REPORT)
#define PENDING_QUEUE_SIZE 16

// A report waiting for an IOCTL_HID_READ_REPORT, with the producer's report number (diagnostics).
typedef struct PENDING_REPORT {
  UCHAR data[XGPAD_MAX_REPORT];  // payload, report ID not included; the profile gives its length
  long seq;
} PENDING_REPORT;

typedef struct DEVICE_CONTEXT {
  WDFDEVICE device;
  WDFQUEUE readQueue;  // manual queue of pended IOCTL_HID_READ_REPORT
  ULONG padIndex;
  wchar_t token[XGPAD_TOKEN_MAX];  // object name token (device property, or the pad index)
  ULONG profileId;  // XGPAD_PROFILE_*
  const PROFILE_DESC *profile;
  UCHAR mac[6];  // DualSense pairing address, derived from the token (least significant byte first)
  wchar_t serial[18];  // the address as text, most significant byte first

  SRWLOCK outputLock;  // serializes writers of the shared output ring (output IOCTLs arrive in parallel)

  SRWLOCK lock;  // guards everything below up to 'section'
  UCHAR current[XGPAD_MAX_REPORT];  // latest state (IOCTL_UMDF_HID_GET_INPUT_REPORT)
  PENDING_REPORT pending[PENDING_QUEUE_SIZE];  // reports not yet delivered to HID class, oldest first
  ULONG pendingHead;
  ULONG pendingCount;
  long consumedCount;  // shared inputCount already taken from the ring

  HANDLE section;
  XGPAD_SHARED *shared;
  HANDLE inputEvent;
  HANDLE outputEvent;
  HANDLE stopEvent;
  HANDLE thread;
} DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD XgpEvtDeviceAdd;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT XgpEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP XgpEvtSelfManagedIoCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL XgpEvtIoDeviceControl;

static void Trace(const wchar_t *fmt, ...) {
  wchar_t buf[256];
  va_list args;
  va_start(args, fmt);
  int n = _snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"xgpad: ");
  _vsnwprintf_s(buf + n, ARRAYSIZE(buf) - n, _TRUNCATE, fmt, args);
  va_end(args);
  OutputDebugStringW(buf);
}

static int64_t Qpc(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

/** Write the profile's at-rest input report payload into data (XGPAD_MAX_REPORT bytes). */
static void NeutralReport(ULONG profileId, UCHAR *data) {
  memset(data, 0, XGPAD_MAX_REPORT);
  if (profileId == XGPAD_PROFILE_DUALSENSE) {
    XGPAD_DS_INPUT_REPORT *r = (XGPAD_DS_INPUT_REPORT *) data;
    r->lx = r->ly = r->rx = r->ry = 0x80;
    r->buttons[0] = XGPAD_DS_HAT_RELEASED;
    r->points[0].contact = r->points[1].contact = XGPAD_DS_TOUCH_NOT_TOUCHING;
    r->status = (XGPAD_DS_POWER_FULL << 4) | 10;
  } else {
    XGPAD_INPUT_REPORT *r = (XGPAD_INPUT_REPORT *) data;
    r->lx = r->rx = 0x8000;
    r->ly = r->ry = 0x7FFF;
  }
}

/** True when two Xbox reports differ in a digital control (buttons, D-pad or Share). */
static BOOLEAN DigitalStateDiffers(const UCHAR *a, const UCHAR *b) {
  const XGPAD_INPUT_REPORT *ra = (const XGPAD_INPUT_REPORT *) a;
  const XGPAD_INPUT_REPORT *rb = (const XGPAD_INPUT_REPORT *) b;
  return ra->buttons != rb->buttons || ra->hat != rb->hat || ra->share != rb->share;
}

/**
 * Queue a report payload for HID class (lock held). For Xbox pads every digital change gets its own entry so presses
 * and releases are never lost, while analog-only changes update the newest undelivered entry so the queue never
 * builds up stale stick positions (no added latency). DualSense reports are always queued: hosts integrate the
 * motion samples and track touches report by report.
 */
static void EnqueueReport(DEVICE_CONTEXT *ctx, const UCHAR *report, long seq) {
  const size_t length = ctx->profile->inputLength;
  memcpy(ctx->current, report, length);

  if (ctx->profile->mergeAnalog && ctx->pendingCount > 0) {
    PENDING_REPORT *tail = &ctx->pending[(ctx->pendingHead + ctx->pendingCount - 1) % PENDING_QUEUE_SIZE];
    if (!DigitalStateDiffers(tail->data, report)) {
      memcpy(tail->data, report, length);
      tail->seq = seq;
      return;
    }
  }

  if (ctx->pendingCount == PENDING_QUEUE_SIZE) {
    // HID class is not reading at all; keep the newest states
    ctx->pendingHead = (ctx->pendingHead + 1) % PENDING_QUEUE_SIZE;
    ctx->pendingCount--;
  }
  PENDING_REPORT *slot = &ctx->pending[(ctx->pendingHead + ctx->pendingCount) % PENDING_QUEUE_SIZE];
  memcpy(slot->data, report, length);
  slot->seq = seq;
  ctx->pendingCount++;
}

// ---------------------------------------------------------------------------------------------------------------
// Shared memory / events

static ULONG ParsePadIndex(WDFDEVICE device) {
  WDF_DEVICE_PROPERTY_DATA prop;
  WDFMEMORY memory = NULL;
  DEVPROPTYPE type;
  ULONG index = 0;

  WDF_DEVICE_PROPERTY_DATA_INIT(&prop, &DEVPKEY_Device_InstanceId);
  if (NT_SUCCESS(WdfDeviceAllocAndQueryPropertyEx(device, &prop, NonPagedPoolNx, WDF_NO_OBJECT_ATTRIBUTES, &memory, &type))) {
    const wchar_t *id = (const wchar_t *) WdfMemoryGetBuffer(memory, NULL);
    const wchar_t *last = wcsrchr(id, L'\\');
    const wchar_t *p = last ? last + 1 : id;
    while (*p && (*p < L'0' || *p > L'9')) {
      p++;
    }
    index = wcstoul(p, NULL, 10);
    Trace(L"instance %s -> pad %lu\n", id, index);
    WdfObjectDelete(memory);
  }
  return index < XGPAD_MAX_PADS ? index : 0;
}

/** Read the per-device object name token set by the producer; falls back to the pad index. */
static void QueryToken(WDFDEVICE device, DEVICE_CONTEXT *ctx) {
  static const DEVPROPKEY tokenKey = {XGPAD_TOKEN_PROPKEY_GUID, XGPAD_TOKEN_PROPKEY_PID};
  WDF_DEVICE_PROPERTY_DATA prop;
  WDFMEMORY memory = NULL;
  DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;

  swprintf_s(ctx->token, ARRAYSIZE(ctx->token), L"%lu", ctx->padIndex);
  WDF_DEVICE_PROPERTY_DATA_INIT(&prop, &tokenKey);
  if (NT_SUCCESS(WdfDeviceAllocAndQueryPropertyEx(device, &prop, NonPagedPoolNx, WDF_NO_OBJECT_ATTRIBUTES, &memory, &type))) {
    size_t length = 0;
    const wchar_t *value = (const wchar_t *) WdfMemoryGetBuffer(memory, &length);
    if (type == DEVPROP_TYPE_STRING && length >= sizeof(wchar_t)) {
      wcsncpy_s(ctx->token, ARRAYSIZE(ctx->token), value, _TRUNCATE);
    }
    WdfObjectDelete(memory);
  }
  Trace(L"pad %lu: token %s\n", ctx->padIndex, ctx->token);
}

/** Read the profile set by the producer; without the property (older producers) the pad is an Xbox pad. */
static void QueryProfile(WDFDEVICE device, DEVICE_CONTEXT *ctx) {
  static const DEVPROPKEY profileKey = {XGPAD_TOKEN_PROPKEY_GUID, XGPAD_PROFILE_PROPKEY_PID};
  WDF_DEVICE_PROPERTY_DATA prop;
  WDFMEMORY memory = NULL;
  DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;

  ctx->profileId = XGPAD_PROFILE_XBOX_SERIES;
  WDF_DEVICE_PROPERTY_DATA_INIT(&prop, &profileKey);
  if (NT_SUCCESS(WdfDeviceAllocAndQueryPropertyEx(device, &prop, NonPagedPoolNx, WDF_NO_OBJECT_ATTRIBUTES, &memory, &type))) {
    size_t length = 0;
    const ULONG *value = (const ULONG *) WdfMemoryGetBuffer(memory, &length);
    if (type == DEVPROP_TYPE_UINT32 && length >= sizeof(ULONG) && *value < ARRAYSIZE(g_Profiles)) {
      ctx->profileId = *value;
    }
    WdfObjectDelete(memory);
  }
  ctx->profile = &g_Profiles[ctx->profileId];
  Trace(L"pad %lu: profile %lu\n", ctx->padIndex, ctx->profileId);
}

/**
 * Derive a stable, locally administered unicast address from the token, so hosts that tell pads apart by their
 * pairing address or serial number see a different one per pad.
 */
static void DeriveAddress(DEVICE_CONTEXT *ctx) {
  ULONGLONG hash = 0xcbf29ce484222325ull;  // FNV-1a
  for (const wchar_t *p = ctx->token; *p; p++) {
    hash ^= (USHORT) *p;
    hash *= 0x100000001b3ull;
  }
  for (int i = 0; i < 6; i++) {
    ctx->mac[i] = (UCHAR) (hash >> (8 * i));
  }
  ctx->mac[5] = (UCHAR) ((ctx->mac[5] & 0xFC) | 0x02);  // most significant byte: locally administered, unicast
  swprintf_s(ctx->serial, ARRAYSIZE(ctx->serial), L"%02x:%02x:%02x:%02x:%02x:%02x", ctx->mac[5], ctx->mac[4], ctx->mac[3], ctx->mac[2], ctx->mac[1], ctx->mac[0]);
}

static BOOLEAN OpenSharedObjects(DEVICE_CONTEXT *ctx) {
  wchar_t name[96];

  if (ctx->shared) {
    return TRUE;
  }

  swprintf_s(name, ARRAYSIZE(name), XGPAD_SECTION_NAME_FMT, ctx->token);
  HANDLE section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
  if (!section) {
    return FALSE;
  }
  XGPAD_SHARED *shared = (XGPAD_SHARED *) MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(XGPAD_SHARED));
  if (!shared || shared->magic != XGPAD_SHARED_MAGIC || shared->version != XGPAD_SHARED_VERSION || shared->profile != ctx->profileId) {
    if (shared) {
      Trace(L"pad %lu: section rejected (version %lu, profile %lu)\n", ctx->padIndex, shared->version, shared->profile);
      UnmapViewOfFile(shared);
    }
    CloseHandle(section);
    return FALSE;
  }

  swprintf_s(name, ARRAYSIZE(name), XGPAD_INPUT_EVENT_NAME_FMT, ctx->token);
  HANDLE inputEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
  swprintf_s(name, ARRAYSIZE(name), XGPAD_OUTPUT_EVENT_NAME_FMT, ctx->token);
  HANDLE outputEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
  if (!inputEvent || !outputEvent) {
    if (inputEvent) {
      CloseHandle(inputEvent);
    }
    if (outputEvent) {
      CloseHandle(outputEvent);
    }
    UnmapViewOfFile(shared);
    CloseHandle(section);
    return FALSE;
  }

  ctx->section = section;
  ctx->inputEvent = inputEvent;
  ctx->outputEvent = outputEvent;
  ctx->shared = shared;
  InterlockedOr(&shared->driverStatus, XGPAD_STATUS_DRIVER_STARTED);
  Trace(L"pad %lu: shared objects opened\n", ctx->padIndex);
  return TRUE;
}

static void CloseSharedObjects(DEVICE_CONTEXT *ctx) {
  if (ctx->shared) {
    InterlockedAnd(&ctx->shared->driverStatus, ~XGPAD_STATUS_DRIVER_STARTED);
    UnmapViewOfFile(ctx->shared);
    ctx->shared = NULL;
  }
  if (ctx->inputEvent) {
    CloseHandle(ctx->inputEvent);
    ctx->inputEvent = NULL;
  }
  if (ctx->outputEvent) {
    CloseHandle(ctx->outputEvent);
    ctx->outputEvent = NULL;
  }
  if (ctx->section) {
    CloseHandle(ctx->section);
    ctx->section = NULL;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Input delivery

/** Complete pended reads with queued reports, oldest first, as long as both exist. */
static void TryCompleteRead(DEVICE_CONTEXT *ctx) {
  for (;;) {
    WDFREQUEST request = NULL;
    PENDING_REPORT entry;

    AcquireSRWLockExclusive(&ctx->lock);
    if (ctx->pendingCount == 0 || !NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->readQueue, &request))) {
      ReleaseSRWLockExclusive(&ctx->lock);
      return;
    }
    entry = ctx->pending[ctx->pendingHead];
    ctx->pendingHead = (ctx->pendingHead + 1) % PENDING_QUEUE_SIZE;
    ctx->pendingCount--;
    ReleaseSRWLockExclusive(&ctx->lock);

    const size_t size = 1 + (size_t) ctx->profile->inputLength;
    UCHAR report[REPORT_BUFFER_SIZE];
    report[0] = ctx->profile->inputReportId;
    memcpy(report + 1, entry.data, size - 1);

    void *buffer;
    size_t length;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, size, &buffer, &length);
    if (NT_SUCCESS(status)) {
      memcpy(buffer, report, size);
      XGPAD_SHARED *shared = ctx->shared;
      if (shared) {
        shared->deliveredQpc = Qpc();
        shared->deliveredSeq = entry.seq;
        InterlockedIncrement(&shared->readsCompleted);
      }
      WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, size);
    } else {
      WdfRequestComplete(request, status);
    }
  }
}

/** Move newly published reports from the shared ring into the pending queue, in order. */
static void DrainSharedInput(DEVICE_CONTEXT *ctx) {
  XGPAD_SHARED *shared = ctx->shared;
  if (!shared) {
    return;
  }

  AcquireSRWLockExclusive(&ctx->lock);
  const long available = shared->inputCount;
  MemoryBarrier();

  long next = ctx->consumedCount;
  if (available < next) {
    next = available;  // producer restarted its count
  }
  if ((unsigned long) (available - next) > XGPAD_INPUT_RING_SIZE) {
    InterlockedAdd(&shared->inputOverruns, (long) (available - next - XGPAD_INPUT_RING_SIZE));
    next = available - XGPAD_INPUT_RING_SIZE;
  }

  while (next < available) {
    const long n = next + 1;
    XGPAD_REPORT report;
    memcpy(&report, (const void *) &shared->inputRing[(n - 1) % XGPAD_INPUT_RING_SIZE], sizeof(report));
    MemoryBarrier();
    if ((unsigned long) (shared->inputCount - n) >= XGPAD_INPUT_RING_SIZE) {
      // The producer lapped this slot while we copied it: the copy may be torn, skip it
      InterlockedIncrement(&shared->inputOverruns);
    } else if (report.id != ctx->profile->inputReportId || report.length != ctx->profile->inputLength) {
      InterlockedIncrement(&shared->inputRejected);
    } else {
      EnqueueReport(ctx, report.data, n);
    }
    next = n;
  }
  ctx->consumedCount = next;
  ReleaseSRWLockExclusive(&ctx->lock);
}

static DWORD WINAPI WorkerThread(LPVOID param) {
  DEVICE_CONTEXT *ctx = (DEVICE_CONTEXT *) param;

  // Time-critical: this thread sits on the input path between Sunshine and HID class.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

  for (;;) {
    if (!OpenSharedObjects(ctx)) {
      // Producer objects not there (yet): poll slowly until they appear or we are stopped.
      if (WaitForSingleObject(ctx->stopEvent, 250) == WAIT_OBJECT_0) {
        break;
      }
      continue;
    }

    DrainSharedInput(ctx);
    TryCompleteRead(ctx);

    // The timeout is only a safety net: reports are normally picked up on the event (or on the next
    // read request), so a lost wake-up can delay a report by at most this long.
    HANDLE handles[2] = {ctx->stopEvent, ctx->inputEvent};
    DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 250);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait == WAIT_OBJECT_0 + 1) {
      InterlockedIncrement(&ctx->shared->workerWakeups);
    } else if (wait != WAIT_TIMEOUT) {
      Trace(L"pad %lu: wait failed %lu\n", ctx->padIndex, GetLastError());
      break;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Output (rumble, lightbar, adaptive triggers)

static void HandleOutputReport(DEVICE_CONTEXT *ctx, const UCHAR *data, size_t length, ULONG reportId) {
  const PROFILE_DESC *profile = ctx->profile;

  // HID class passes the report ID as the first byte of the buffer when the descriptor uses report IDs.
  if (length == 1 + (size_t) profile->outputLength && data[0] == profile->outputReportId) {
    data++;
    length--;
  } else if (reportId != profile->outputReportId && reportId != 0) {
    Trace(L"pad %lu: ignoring output report id %lu len %zu\n", ctx->padIndex, reportId, length);
    return;
  }
  if (length < profile->outputLength) {
    Trace(L"pad %lu: short output report len %zu\n", ctx->padIndex, length);
    return;
  }

  XGPAD_SHARED *shared = ctx->shared;
  if (!shared) {
    return;
  }
  AcquireSRWLockExclusive(&ctx->outputLock);
  const long n = shared->outputCount + 1;
  XGPAD_REPORT *slot = (XGPAD_REPORT *) &shared->outputRing[(n - 1) % XGPAD_OUTPUT_RING_SIZE];
  slot->id = profile->outputReportId;
  slot->length = profile->outputLength;
  memcpy(slot->data, data, profile->outputLength);
  shared->outputQpc = Qpc();
  MemoryBarrier();
  InterlockedExchange(&shared->outputCount, n);  // publish
  ReleaseSRWLockExclusive(&ctx->outputLock);
  InterlockedIncrement(&shared->outputsReceived);
  SetEvent(ctx->outputEvent);
}

// ---------------------------------------------------------------------------------------------------------------
// Feature reports (DualSense)

static void PutLe16(UCHAR *p, SHORT value) {
  p[0] = (UCHAR) (value & 0xFF);
  p[1] = (UCHAR) ((USHORT) value >> 8);
}

static void PutLe32(UCHAR *p, ULONG value) {
  for (int i = 0; i < 4; i++) {
    p[i] = (UCHAR) (value >> (8 * i));
  }
}

/** Fill a DualSense feature report payload (report ID not included); unknown contents stay zero. */
static void BuildDsFeature(const DEVICE_CONTEXT *ctx, UCHAR id, UCHAR *payload) {
  switch (id) {
    case XGPAD_DS_FEATURE_CALIBRATION:
      {
        // Gyro: biases 0, +/-8192 counts at +/-512 deg/s -> 16 counts per deg/s (XGPAD_DS_GYRO_RES_PER_DEG_S).
        // Accelerometer: +/-8192 counts at +/-1 g -> 8192 counts per g (XGPAD_DS_ACC_RES_PER_G).
        for (int axis = 0; axis < 3; axis++) {
          PutLe16(payload + 6 + axis * 4, 8192);  // pitch/yaw/roll plus
          PutLe16(payload + 8 + axis * 4, -8192);  // pitch/yaw/roll minus
          PutLe16(payload + 22 + axis * 4, 8192);  // x/y/z plus
          PutLe16(payload + 24 + axis * 4, -8192);  // x/y/z minus
        }
        PutLe16(payload + 18, 512);  // gyro speed plus
        PutLe16(payload + 20, 512);  // gyro speed minus
        break;
      }
    case XGPAD_DS_FEATURE_PAIRING_INFO:
      memcpy(payload, ctx->mac, sizeof(ctx->mac));
      payload[6] = 0x08;
      payload[7] = 0x25;
      break;
    case XGPAD_DS_FEATURE_FIRMWARE_INFO:
      memcpy(payload, "Jun 19 2024", 11);  // build date
      memcpy(payload + 11, "10:00:00", 8);  // build time
      PutLe16(payload + 19, 0x0003);  // firmware type
      PutLe16(payload + 21, 0x0004);  // software series
      PutLe32(payload + 23, 0x00000615);  // hardware version
      PutLe32(payload + 27, 0x0110002A);  // firmware version
      PutLe16(payload + 43, 0x0630);  // update version: new enough for hosts to use the improved rumble emulation
      break;
    default:
      break;
  }
}

static const FEATURE_DESC *FindFeature(const PROFILE_DESC *profile, UCHAR id) {
  for (ULONG i = 0; i < profile->featureCount; i++) {
    if (profile->features[i].id == id) {
      return &profile->features[i];
    }
  }
  return NULL;
}

/**
 * Read the report ID of a UMDF "get" request (IOCTL_UMDF_HID_GET_FEATURE / _GET_INPUT_REPORT): the report ID is the
 * first byte of the input buffer, the report goes to the output buffer.
 */
static BOOLEAN GetRequestedReportId(WDFREQUEST request, UCHAR *id) {
  UCHAR *input;
  size_t inputLength;
  if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(request, 1, (PVOID *) &input, &inputLength))) {
    return FALSE;
  }
  *id = input[0];
  return TRUE;
}

static NTSTATUS CopyToOutput(WDFREQUEST request, const void *data, size_t length, ULONG_PTR *information);

static NTSTATUS GetFeature(DEVICE_CONTEXT *ctx, WDFREQUEST request, ULONG_PTR *information) {
  UCHAR id;
  if (!GetRequestedReportId(request, &id)) {
    return STATUS_INVALID_PARAMETER;
  }
  const FEATURE_DESC *feature = FindFeature(ctx->profile, id);
  if (!feature) {
    Trace(L"pad %lu: unknown feature report 0x%02x\n", ctx->padIndex, id);
    return STATUS_INVALID_PARAMETER;
  }
  if (ctx->shared) {
    InterlockedIncrement(&ctx->shared->featureRequests);
  }

  UCHAR report[REPORT_BUFFER_SIZE] = {0};
  report[0] = id;
  BuildDsFeature(ctx, id, report + 1);
  return CopyToOutput(request, report, 1 + (size_t) feature->length, information);
}

// ---------------------------------------------------------------------------------------------------------------
// IOCTL handling

static NTSTATUS CopyToOutput(WDFREQUEST request, const void *data, size_t length, ULONG_PTR *information) {
  void *buffer;
  size_t bufferLength;
  NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, length, &buffer, &bufferLength);
  if (NT_SUCCESS(status)) {
    memcpy(buffer, data, length);
    *information = length;
  }
  return status;
}

static NTSTATUS GetString(const DEVICE_CONTEXT *ctx, WDFREQUEST request, ULONG_PTR *information) {
  ULONG *input;
  size_t inputLength;
  // UMDF: string ID (low word) and language (high word) are passed in the input buffer.
  NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(ULONG), (PVOID *) &input, &inputLength);
  if (!NT_SUCCESS(status)) {
    return status;
  }
  const wchar_t *string;
  switch (*input & 0xFFFF) {
    case HID_STRING_ID_IMANUFACTURER:
      string = ctx->profile->manufacturer;
      break;
    case HID_STRING_ID_IPRODUCT:
      string = ctx->profile->product;
      break;
    case HID_STRING_ID_ISERIALNUMBER:
      if (ctx->profileId != XGPAD_PROFILE_DUALSENSE) {
        return STATUS_INVALID_PARAMETER;
      }
      string = ctx->serial;
      break;
    default:
      return STATUS_INVALID_PARAMETER;
  }
  return CopyToOutput(request, string, (wcslen(string) + 1) * sizeof(wchar_t), information);
}

VOID XgpEvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength, size_t inputLength, ULONG ioControlCode) {
  DEVICE_CONTEXT *ctx = GetDeviceContext(WdfIoQueueGetDevice(queue));
  NTSTATUS status = STATUS_NOT_SUPPORTED;
  ULONG_PTR information = 0;

  switch (ioControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
      {
        HID_DESCRIPTOR descriptor = {0};
        descriptor.bLength = sizeof(descriptor);
        descriptor.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
        descriptor.bcdHID = 0x0111;
        descriptor.bNumDescriptors = 1;
        descriptor.DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
        descriptor.DescriptorList[0].wReportLength = ctx->profile->reportDescriptorLength;
        status = CopyToOutput(request, &descriptor, sizeof(descriptor), &information);
        break;
      }

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
      status = CopyToOutput(request, ctx->profile->reportDescriptor, ctx->profile->reportDescriptorLength, &information);
      break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
      {
        HID_DEVICE_ATTRIBUTES attributes = {0};
        attributes.Size = sizeof(attributes);
        attributes.VendorID = ctx->profile->vendorId;
        attributes.ProductID = ctx->profile->productId;
        attributes.VersionNumber = ctx->profile->versionNumber;
        status = CopyToOutput(request, &attributes, sizeof(attributes), &information);
        break;
      }

    case IOCTL_HID_READ_REPORT:
      status = WdfRequestForwardToIoQueue(request, ctx->readQueue);
      if (NT_SUCCESS(status)) {
        // Pick up the producer's latest report here too, so delivery never depends on the worker having
        // seen the input event.
        XGPAD_SHARED *shared = ctx->shared;
        if (shared) {
          InterlockedIncrement(&shared->readRequests);
          DrainSharedInput(ctx);
        }
        TryCompleteRead(ctx);
        return;
      }
      break;

    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
      {
        // UMDF: report buffer = input buffer, report ID = output buffer length.
        void *buffer;
        size_t length;
        status = WdfRequestRetrieveInputBuffer(request, 1, &buffer, &length);
        if (NT_SUCCESS(status)) {
          HandleOutputReport(ctx, (const UCHAR *) buffer, length, (ULONG) outputLength);
          information = length;
        }
        break;
      }

    case IOCTL_UMDF_HID_GET_INPUT_REPORT:
      {
        // UMDF: report buffer = output buffer, report ID = first byte of the input buffer.
        UCHAR id = ctx->profile->inputReportId;
        if (inputLength > 0 && !GetRequestedReportId(request, &id)) {
          status = STATUS_INVALID_PARAMETER;
          break;
        }
        if (id != ctx->profile->inputReportId) {
          status = STATUS_INVALID_PARAMETER;
          break;
        }
        const size_t size = 1 + (size_t) ctx->profile->inputLength;
        UCHAR report[REPORT_BUFFER_SIZE];
        report[0] = id;
        AcquireSRWLockShared(&ctx->lock);
        memcpy(report + 1, ctx->current, size - 1);
        ReleaseSRWLockShared(&ctx->lock);
        status = CopyToOutput(request, report, size, &information);
        break;
      }

    case IOCTL_UMDF_HID_GET_FEATURE:
      status = ctx->profile->featureCount > 0 ? GetFeature(ctx, request, &information) : STATUS_NOT_SUPPORTED;
      break;

    case IOCTL_UMDF_HID_SET_FEATURE:
      {
        // Accepted and ignored: a virtual pad has no settings to keep. UMDF: report buffer = input buffer.
        void *buffer;
        size_t length = 0;
        status = ctx->profile->featureCount > 0 ? WdfRequestRetrieveInputBuffer(request, 1, &buffer, &length) : STATUS_NOT_SUPPORTED;
        if (NT_SUCCESS(status)) {
          information = length;
        }
        break;
      }

    case IOCTL_HID_GET_STRING:
      status = GetString(ctx, request, &information);
      break;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
      status = STATUS_SUCCESS;
      break;

    default:
      Trace(L"pad %lu: unsupported ioctl 0x%08lx in=%zu out=%zu\n", ctx->padIndex, ioControlCode, inputLength, outputLength);
      status = STATUS_NOT_SUPPORTED;
      break;
  }

  WdfRequestCompleteWithInformation(request, status, information);
}

// ---------------------------------------------------------------------------------------------------------------
// PnP

NTSTATUS XgpEvtSelfManagedIoInit(WDFDEVICE device) {
  DEVICE_CONTEXT *ctx = GetDeviceContext(device);

  ctx->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!ctx->stopEvent) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  ctx->thread = CreateThread(NULL, 0, WorkerThread, ctx, 0, NULL);
  if (!ctx->thread) {
    CloseHandle(ctx->stopEvent);
    ctx->stopEvent = NULL;
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  return STATUS_SUCCESS;
}

VOID XgpEvtSelfManagedIoCleanup(WDFDEVICE device) {
  DEVICE_CONTEXT *ctx = GetDeviceContext(device);

  if (ctx->thread) {
    SetEvent(ctx->stopEvent);
    WaitForSingleObject(ctx->thread, INFINITE);
    CloseHandle(ctx->thread);
    ctx->thread = NULL;
  }
  if (ctx->stopEvent) {
    CloseHandle(ctx->stopEvent);
    ctx->stopEvent = NULL;
  }
  CloseSharedObjects(ctx);
}

NTSTATUS XgpEvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit) {
  UNREFERENCED_PARAMETER(driver);

  // mshidumdf is the function driver; we are a filter below it and do not own power policy.
  WdfFdoInitSetFilter(deviceInit);

  WDF_PNPPOWER_EVENT_CALLBACKS pnp;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
  pnp.EvtDeviceSelfManagedIoInit = XgpEvtSelfManagedIoInit;
  pnp.EvtDeviceSelfManagedIoCleanup = XgpEvtSelfManagedIoCleanup;
  WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnp);

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
  WDFDEVICE device;
  NTSTATUS status = WdfDeviceCreate(&deviceInit, &attributes, &device);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  DEVICE_CONTEXT *ctx = GetDeviceContext(device);
  ctx->device = device;
  InitializeSRWLock(&ctx->lock);
  InitializeSRWLock(&ctx->outputLock);
  ctx->padIndex = ParsePadIndex(device);
  QueryToken(device, ctx);
  QueryProfile(device, ctx);
  DeriveAddress(ctx);

  // Hand HID class a neutral state first; reports from the producer follow in order
  UCHAR neutral[XGPAD_MAX_REPORT];
  NeutralReport(ctx->profileId, neutral);
  EnqueueReport(ctx, neutral, 0);
  ctx->consumedCount = 0;

  WDF_IO_QUEUE_CONFIG queueConfig;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
  queueConfig.EvtIoDeviceControl = XgpEvtIoDeviceControl;
  WDFQUEUE queue;
  status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
  queueConfig.PowerManaged = WdfFalse;
  status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->readQueue);
  return status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, XgpEvtDeviceAdd);
  return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
