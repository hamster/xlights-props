#ifndef LED_HANDLER_H
#define LED_HANDLER_H

#include <Arduino.h>
#include <FastLED.h>

// Maximum number of LEDs supported
#define MAX_LEDS 1000

// LED data pin
#define LED_DATA_PIN D4

// LED Configuration
extern int ledPixelCount;
extern String ledColorOrder;
extern float ledGamma;
extern int ledBrightness;
extern int ledStartNullPixels;
extern int ledEndNullPixels;

// LED pixel array
extern CRGB leds[MAX_LEDS];
extern bool ledsInitialized;
extern bool ledsBlanked;

// LED statistics
extern int ledPixelsReceived;     // Current pixels received in last update
extern int ledMaxPixelsReceived;  // Maximum pixel index seen since last blank

// Status LED pin
#define STATUS_LED_PIN D8

// Status LED state
extern bool statusLedState;
extern unsigned long statusLedBlinkInterval;
extern bool locateMode;
extern hw_timer_t *ledTimer;

// Status LED functions
void initStatusLed();
void IRAM_ATTR onLedTimer();

// Pixel LED functions
void initPixelLeds();
void updatePixelLeds(uint8_t* data, uint16_t size, int stepperChannels);
void updatePixelLedsFragmented(uint8_t* data, uint16_t size, uint32_t pixelOffset);
void blankPixelLeds();

// Dual-core split (2026-09-08): every actual FastLED hardware call -
// addLeds()/setBrightness()/setCorrection()/show() - now runs exclusively
// from the Core 0 task (core0_task.cpp), never from Core 1, so FastLED's
// WS2812 output work never competes with FastAccelStepper's Core-1-affine
// MCPWM+PCNT work for CPU cycles - see TODO.md's "Priority 1" design
// section for the full reasoning (including why addLeds() itself has to
// move, not just show() - it's what binds the RMT completion interrupt to
// whichever core calls it).
//
// Core 1 (ddp_handler.cpp, main.cpp's loop(), html_handler.cpp) is
// unchanged otherwise: it still writes new pixel data directly into the
// shared leds[] array (no double-buffer - an accepted, self-correcting
// tradeoff per that same design doc) and calls signalLedShow() instead of
// FastLED.show(), or requestLedReinit() instead of touching
// FastLED.addLeds()/setBrightness()/setCorrection() directly. Both of
// initPixelLeds()'s existing call sites (main.cpp's setup(),
// html_handler.cpp's handleSaveLed()) are unchanged - initPixelLeds()
// itself now only loads config from preferences and calls
// requestLedReinit() at the end, instead of touching FastLED directly.
void initLedCore0();      // Call once from core0Task() (core0_task.cpp) - creates the show semaphore. NOT called from setup() (Core 1).
void serviceLedCore0();   // Call every core0Task() loop iteration - applies a pending reinit, then blocks up to ~10ms for a pending show() (this is also core0Task()'s loop timing source now, replacing its old flat vTaskDelay)
void signalLedShow();     // Core 1 calls this instead of FastLED.show() directly - binary semaphore, so a give() while one is already pending is a no-op: bursts of DDP fragments between Core 0 wake-ups naturally coalesce to one show() of the latest leds[] state
void requestLedReinit();  // Core 1 calls this instead of touching FastLED.addLeds()/setBrightness()/setCorrection() directly - re-applies the current ledPixelCount/ledColorOrder/ledBrightness/etc config on Core 0

// Deferred flash write for LED settings (2026-09-08) - same reasoning as
// stepperSettingsPendingSave (stepper_handler.h) and
// protocolDebugPendingSave (protocol_common.h). handleSaveLed() used to
// call six preferences.putX() calls synchronously - a real crash,
// reproduced on the bench: a real flash write briefly disables the flash
// cache, and doing so while the stepper is actively producing steps (not
// just during homing - PID continuous-run motion hit this too) can crash
// if FastAccelStepper's own step-generation ISR (not fully IRAM-resident)
// fires during that window. handleSaveLed() now applies everything in RAM
// immediately (including the actual LED reinit, via requestLedReinit() -
// this only defers the flash write itself) and sets this flag instead;
// persistLedSettingsIfPending() actually writes it, gated on the stepper
// being idle, same pattern as persistStepperSettingsIfPending().
extern bool ledSettingsPendingSave;
void persistLedSettingsIfPending();  // Call every loop() iteration - no-op unless a save is pending

// LED preview for web status (returns compact hex string "RRGGBBRRGGBB...", up to maxPixels)
// Uses static buffer to avoid heap allocation, returns const char* (not String)
const char* getLedPreviewJson(int maxPixels);

// LED test pattern - marching RGB pattern for local bench testing without a
// DDP source. Runtime-only (not persisted), mirrors locateMode's pattern.
extern bool ledTestModeActive;
void setLedTestMode(bool enable);  // Enable/disable; handles reset + blank-on-disable
void updateLedTestMode();          // Call every loop() iteration - advances the pattern once/second

#endif
