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
// FastLED's work is deliberately NOT moved here yet - LEDs are disabled
// (pixel count 0) during this stepper-tuning phase, freeing RMT entirely,
// and it's still an open question whether the eventual fix is pinning the
// existing GPIO-ISR approach here, switching to a periodic timer-based poll
// instead, or moving to RMT's RX/capture mode now that FastLED doesn't own
// RMT during this phase - see TODO.md. When FastLED's Core 0 work is ready,
// its show()-triggering logic belongs in this same task's loop, fed by a
// semaphore from Core 1's pixel-write call sites, not a second task.
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
