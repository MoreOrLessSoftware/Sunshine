/**
 * @file drivers/xgpad/driver/xgpad.c
 * @brief UMDF2 HID minidriver: virtual Xbox Series X|S controller (Bluetooth LE layout, 045E:0B13).
 *
 * Stack (mirrors a real BLE pad): xinputhid (upper filter) -> mshidumdf (function) -> WUDFRd -> SWD.
 * The driver is a pipe: Sunshine formats the 16-byte input report in shared memory and signals an event;
 * a worker thread completes the pending IOCTL_HID_READ_REPORT. Output report 0x03 (rumble incl. impulse
 * triggers) is copied back to shared memory and signalled.
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

static const UCHAR g_ReportDescriptor[] = {
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

C_ASSERT(sizeof(g_ReportDescriptor) == 283);

static const HID_DESCRIPTOR g_HidDescriptor = {
  sizeof(HID_DESCRIPTOR),  // bLength
  HID_HID_DESCRIPTOR_TYPE,  // bDescriptorType
  0x0111,  // bcdHID
  0x00,  // bCountry
  0x01,  // bNumDescriptors
  {HID_REPORT_DESCRIPTOR_TYPE, sizeof(g_ReportDescriptor)},
};

#define INPUT_REPORT_SIZE (1 + sizeof(XGPAD_INPUT_REPORT))

typedef struct DEVICE_CONTEXT {
  WDFDEVICE device;
  WDFQUEUE readQueue;  // manual queue of pended IOCTL_HID_READ_REPORT
  ULONG padIndex;
  wchar_t token[XGPAD_TOKEN_MAX];  // object name token (device property, or the pad index)

  SRWLOCK lock;
  UCHAR report[INPUT_REPORT_SIZE];  // latest input report incl. report ID
  BOOLEAN reportPending;  // report not yet delivered to HID class
  long reportSeq;

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

static void SetNeutralReport(UCHAR *report) {
  XGPAD_INPUT_REPORT r = {0};
  r.lx = r.ly = r.rx = r.ry = 0x8000;
  report[0] = XGPAD_INPUT_REPORT_ID;
  memcpy(report + 1, &r, sizeof(r));
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
  if (!shared || shared->magic != XGPAD_SHARED_MAGIC || shared->version != XGPAD_SHARED_VERSION) {
    if (shared) {
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

/** Complete one pended read with the latest report, if both exist. */
static void TryCompleteRead(DEVICE_CONTEXT *ctx) {
  WDFREQUEST request = NULL;
  UCHAR report[INPUT_REPORT_SIZE];
  long seq = 0;

  AcquireSRWLockExclusive(&ctx->lock);
  if (ctx->reportPending && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->readQueue, &request))) {
    memcpy(report, ctx->report, sizeof(report));
    seq = ctx->reportSeq;
    ctx->reportPending = FALSE;
  }
  ReleaseSRWLockExclusive(&ctx->lock);

  if (!request) {
    return;
  }

  void *buffer;
  size_t length;
  NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, sizeof(report), &buffer, &length);
  if (NT_SUCCESS(status)) {
    memcpy(buffer, report, sizeof(report));
    if (ctx->shared) {
      ctx->shared->deliveredQpc = Qpc();
      ctx->shared->deliveredSeq = seq;
      InterlockedIncrement(&ctx->shared->readsCompleted);
    }
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(report));
  } else {
    WdfRequestComplete(request, status);
  }
}

/** Copy the producer's report out of shared memory (seqlock read). */
static BOOLEAN ReadSharedInput(DEVICE_CONTEXT *ctx) {
  XGPAD_SHARED *shared = ctx->shared;
  XGPAD_INPUT_REPORT input;
  long seq;

  for (int attempt = 0; attempt < 1000; attempt++) {
    seq = shared->inputSeq;
    MemoryBarrier();
    if (seq & 1) {
      YieldProcessor();
      continue;
    }
    memcpy(&input, (const void *) &shared->input, sizeof(input));
    MemoryBarrier();
    if (seq == shared->inputSeq) {
      AcquireSRWLockExclusive(&ctx->lock);
      BOOLEAN changed = seq != ctx->reportSeq;
      if (changed) {
        memcpy(ctx->report + 1, &input, sizeof(input));
        ctx->reportSeq = seq;
        ctx->reportPending = TRUE;
      }
      ReleaseSRWLockExclusive(&ctx->lock);
      return changed;
    }
  }
  return FALSE;
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

    ReadSharedInput(ctx);
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
// Output (rumble)

static void HandleOutputReport(DEVICE_CONTEXT *ctx, const UCHAR *data, size_t length, ULONG reportId) {
  XGPAD_OUTPUT_REPORT out;

  // HID class passes the report ID as the first byte of the buffer when the descriptor uses report IDs.
  if (length == 1 + sizeof(out) && data[0] == XGPAD_OUTPUT_REPORT_ID) {
    data++;
    length--;
  } else if (reportId != XGPAD_OUTPUT_REPORT_ID && reportId != 0) {
    Trace(L"pad %lu: ignoring output report id %lu len %zu\n", ctx->padIndex, reportId, length);
    return;
  }
  if (length < sizeof(out)) {
    Trace(L"pad %lu: short output report len %zu\n", ctx->padIndex, length);
    return;
  }
  memcpy(&out, data, sizeof(out));

  XGPAD_SHARED *shared = ctx->shared;
  if (!shared) {
    return;
  }
  InterlockedIncrement(&shared->outputSeq);  // odd: writing
  MemoryBarrier();
  memcpy((void *) &shared->output, &out, sizeof(out));
  shared->outputQpc = Qpc();
  MemoryBarrier();
  InterlockedIncrement(&shared->outputSeq);  // even: done
  InterlockedIncrement(&shared->outputsReceived);
  SetEvent(ctx->outputEvent);
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

static NTSTATUS GetString(WDFREQUEST request, ULONG_PTR *information) {
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
      string = L"Microsoft";
      break;
    case HID_STRING_ID_IPRODUCT:
      string = L"Xbox Wireless Controller";
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
      status = CopyToOutput(request, &g_HidDescriptor, sizeof(g_HidDescriptor), &information);
      break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
      status = CopyToOutput(request, g_ReportDescriptor, sizeof(g_ReportDescriptor), &information);
      break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
      {
        HID_DEVICE_ATTRIBUTES attributes = {0};
        attributes.Size = sizeof(attributes);
        attributes.VendorID = 0x045E;
        attributes.ProductID = 0x0B13;
        attributes.VersionNumber = XGPAD_VERSION_NUMBER;
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
          ReadSharedInput(ctx);
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
        // UMDF: report buffer = output buffer, report ID = input buffer length.
        UCHAR report[INPUT_REPORT_SIZE];
        AcquireSRWLockShared(&ctx->lock);
        memcpy(report, ctx->report, sizeof(report));
        ReleaseSRWLockShared(&ctx->lock);
        status = inputLength == XGPAD_INPUT_REPORT_ID || inputLength == 0 ? CopyToOutput(request, report, sizeof(report), &information) :
                                                                              STATUS_INVALID_PARAMETER;
        break;
      }

    case IOCTL_HID_GET_STRING:
      status = GetString(request, &information);
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
  SetNeutralReport(ctx->report);
  ctx->reportPending = TRUE;
  ctx->reportSeq = -1;
  ctx->padIndex = ParsePadIndex(device);
  QueryToken(device, ctx);

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
