#include "core0_task.h"
#include "encoder_handler.h"
#include <esp_task_wdt.h>

static TaskHandle_t core0TaskHandle = NULL;

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
  // is what actually moves the encoder's GPIO interrupt onto Core 0 -
  // attachInterrupt()'s underlying ISR is core-affine to whichever core
  // calls it, not something configured separately.
  initEncoder();

  // Nothing else scheduled here yet. Once FastLED's work is ready to move
  // here too, its show()-triggering logic (woken by a semaphore Core 1
  // gives after writing pixel data) replaces this idle loop.
  for (;;) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void startCore0Task() {
  xTaskCreatePinnedToCore(core0Task, "Core0Task", 4096, NULL, 1, &core0TaskHandle, 0);
}
