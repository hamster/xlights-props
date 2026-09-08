#ifndef CORE0_TASK_H
#define CORE0_TASK_H

#include <Arduino.h>

// Dedicated FreeRTOS task pinned to Core 0, added 2026-09-05 as the first
// real piece of the "split stepper/protocol handling from LED output across
// both cores" work (see TODO.md's Priority 1 section for the full design
// history and why the original justification for it turned out to be
// different from what CLAUDE.md originally assumed).
//
// Right now this task exists solely to host initEncoder()'s
// attachInterrupt() call, so the encoder's GPIO interrupt ends up
// core-0-affine instead of core-1-affine. A real $ENCDIAG bench sweep
// showed the encoder's ISR was missing ~10-13 real quadrature transitions
// per full-range move, consistently, regardless of stepper speed - pointing
// at contention with something else active on Core 1 throughout every move,
// most likely FastAccelStepper's own MCPWM+PCNT backend, which installs its
// own raw PCNT interrupt handler and is core-affine to Core 1 for the same
// reason the encoder's ISR used to be (both get set up from setup(), which
// runs on Core 1). Moving the encoder off Core 1 entirely tests that
// hypothesis directly, rather than trying to out-prioritize the stepper's
// interrupt on the same core.
//
// FastLED's work moved here too, 2026-09-08 - see led_handler.h's
// declaration comments for initLedCore0()/serviceLedCore0()/
// signalLedShow()/requestLedReinit() for the full design. Every actual
// FastLED hardware call (addLeds()/setBrightness()/setCorrection()/show())
// now runs exclusively from this task, in the same loop as the encoder
// poll - not a second task, per the original plan below. Core 1's
// pixel-write call sites (ddp_handler.cpp, led_handler.cpp's own
// updatePixelLeds()/updatePixelLedsFragmented()/blankPixelLeds()/
// updateLedTestMode()) still write directly into the shared leds[] array,
// then signal this task via a binary semaphore instead of calling
// FastLED.show() themselves.
void startCore0Task();  // Call once from setup(), after initializeStepper() and before anything reads the encoder

// Diagnostics added 2026-09-06 to actually measure (not just infer) whether
// this task is being starved for extended stretches - raised as the
// leading suspect after a real $ENCDIAG sweep showed whole legs' worth of
// encoder data misattributed to the wrong direction even with encoder_diag.cpp's
// synchronous flush-before-reversal fix in place, which shouldn't be
// possible unless this task's own loop wasn't running for a large chunk of
// a ~4s leg, not just a few ms. Both are running high-water-marks since
// boot (worst case seen, not current value) - check these after a real
// test run rather than guessing further.
uint32_t getCore0TaskMaxGapMs();       // Longest gap ever observed between consecutive loop iterations
uint32_t getCore0TaskMinStackBytes();  // Least free stack ever observed (uxTaskGetStackHighWaterMark() * sizeof(StackType_t)) - low is a real problem, not just a curiosity

#endif
