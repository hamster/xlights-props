#include "core0_task.h"
#include "encoder_handler.h"
#include <esp_task_wdt.h>

static TaskHandle_t core0TaskHandle = NULL;

// See core0_task.h's declaration comments - high-water-marks since boot,
// not current values. Plain (not volatile/atomic) ints: written only from
// this task, read occasionally from elsewhere for a status report: a torn
// read of a 32-bit value here would show a slightly stale number for one
// call, not a real bug, so not worth a mutex for a diagnostic-only value.
static uint32_t maxGapMs = 0;
static uint32_t minStackBytes = UINT32_MAX;

// Runs on Core 0. See core0_task.h's file comment for why this exists and
// what it does (and doesn't, yet) do.
static void core0Task(void* param) {
  // Register with the watchdog so a wedged Core 0 task also gets caught,
  // not just loop()'s own task - esp_task_wdt_add(NULL) in main.cpp's
  // setup() only ever registered the calling (Core 1) task. This closes the
  // exact gap TODO.md's Priority 1 section already flagged before any of
  // this was implemented.
  esp_task_wdt_add(NULL);

  // initEncoder() called from here (not from setup(), which runs on Core 1)
  // configures and starts the encoder's RMT RX capture from this task's own
  // context (though RMT's capture-to-ring-buffer path is serviced by
  // ESP-IDF's own driver ISR regardless of which core installed it, so this
  // matters less than it did for the earlier GPIO-ISR implementation - see
  // encoder_handler.h's file comment for the full history).
  initEncoder();

  // Drains the encoder's RMT ring buffer every ~10ms - see updateEncoder()'s
  // declaration comment (encoder_handler.h) for why this needs a periodic
  // call at all now, unlike the GPIO-ISR version it replaced. Once FastLED's
  // work is ready to move here too, its show()-triggering logic (woken by a
  // semaphore Core 1 gives after writing pixel data) joins this same loop.
  unsigned long lastLoopMs = millis();
  for (;;) {
    unsigned long now = millis();
    unsigned long gap = now - lastLoopMs;
    if (gap > maxGapMs) maxGapMs = gap;
    lastLoopMs = now;

    UBaseType_t freeWords = uxTaskGetStackHighWaterMark(NULL);
    uint32_t freeBytes = (uint32_t)freeWords * sizeof(StackType_t);
    if (freeBytes < minStackBytes) minStackBytes = freeBytes;

    esp_task_wdt_reset();
    updateEncoder();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void startCore0Task() {
  xTaskCreatePinnedToCore(core0Task, "Core0Task", 4096, NULL, 1, &core0TaskHandle, 0);
}

uint32_t getCore0TaskMaxGapMs() {
  return maxGapMs;
}

uint32_t getCore0TaskMinStackBytes() {
  return (minStackBytes == UINT32_MAX) ? 0 : minStackBytes;
}
