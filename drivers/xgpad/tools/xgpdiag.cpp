/**
 * @file drivers/xgpad/tools/xgpdiag.cpp
 * @brief Read-only monitor for an xgpad pad's shared memory (run elevated).
 *
 *   xgpdiag <token> <seconds>
 *
 * <token> is logged by Sunshine ("Gamepad N will be Xbox Series controller (xgpad, token ...)").
 *
 * Prints producer/driver counters twice a second so a stall can be attributed to the producer (inputCount not
 * moving), the driver (deliveredSeq lagging inputCount) or HID class (readsCompleted not moving).
 */
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "../xgpad_shared.h"

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    printf("usage: xgpdiag <token> [seconds]\n");
    return 1;
  }
  wchar_t token[XGPAD_TOKEN_MAX];
  swprintf_s(token, L"%hs", argv[1]);
  int seconds = argc > 2 ? atoi(argv[2]) : 30;

  wchar_t name[128];
  swprintf_s(name, XGPAD_SECTION_NAME_FMT, token);
  HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
  if (!section) {
    printf("OpenFileMapping %ls failed %lu (run elevated?)\n", name, GetLastError());
    return 1;
  }
  auto *s = (const XGPAD_SHARED *) MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(XGPAD_SHARED));
  if (!s) {
    printf("MapViewOfFile failed %lu\n", GetLastError());
    return 1;
  }

  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  printf("magic=%08x version=%u\n", s->magic, s->version);
  for (int i = 0; i < seconds * 2; i++) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double sinceInput = (now.QuadPart - s->inputQpc) * 1000.0 / f.QuadPart;
    double sinceDelivered = (now.QuadPart - s->deliveredQpc) * 1000.0 / f.QuadPart;
    const long count = s->inputCount;
    const unsigned btn = count > 0 ? s->inputRing[(count - 1) % XGPAD_INPUT_RING_SIZE].buttons : 0;
    printf("inputCount=%ld deliveredSeq=%ld readsCompleted=%ld readRequests=%ld wakeups=%ld overruns=%ld outputs=%ld btn=%04x | last write %.0f ms ago, last delivery %.0f ms ago\n",
           count, s->deliveredSeq, s->readsCompleted, s->readRequests, s->workerWakeups, s->inputOverruns, s->outputsReceived, btn, sinceInput,
           sinceDelivered);
    Sleep(500);
  }
  return 0;
}
