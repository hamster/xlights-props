#ifndef ENCODER_HANDLER_H
#define ENCODER_HANDLER_H

#include <Arduino.h>

// Rotary encoder mounted directly on the motor shaft - gives a real,
// continuous, independent position reading. This is ground truth:
// getCurrentPosition() on the stepper side is pure step-count bookkeeping
// and has no way to notice a commanded step that didn't physically happen
// ($CHECKSTEPS / stepper_handler.h's StepCheckState are indirect
// workarounds that exist because of that gap) - the encoder can.
//
// Wired: channel A = D0, channel B = D9 - standard two-wire quadrature.
// channel B is no longer used by the current implementation (see below) -
// still physically wired, just not read.
//
// Firmware went through five implementations chasing an accurate count:
//   1-2. ESP32 PCNT peripheral (interrupt-driven, then polling-only for the
//        overflow) - both caused a severe regression: FastAccelStepper's own
//        MCPWM+PCNT backend (the DRIVER_DONT_CARE default on this chip)
//        already owns PCNT hardware/ISR state for step-pulse counting; a
//        second, independent PCNT config for the encoder corrupted that
//        shared state - not a coincidental pin conflict, the stepper
//        library's own permanent occupancy of PCNT on this chip.
//   3. Plain digitalRead() polling in loop() - stable, but a real gap: an
//      intermediate quadrature state is silently dropped if loop() stalls
//      longer than one quadrature step's worth of motion.
//   4. GPIO change-interrupts (attachInterrupt(..., CHANGE)) on both A and
//      B, X4 quadrature decode - closed the polling gap, and (after adding
//      0.1uF filter capacitors the A/B lines were missing) got clean,
//      consistent counts. But a real $ENCDIAG bench sweep with a "missed
//      transitions" diagnostic added showed firmware-side loss was still
//      real (~10-13 per full-range leg, every leg) - and neither pinning
//      the ISR to Core 0 (away from FastAccelStepper's Core-1-affine PCNT
//      interrupt) nor removing the web status poll from Core 0 meaningfully
//      reduced it. That ruled out "competing with a specific busy
//      neighbor" as the cause - pointing instead at something intrinsic to
//      edge-triggered GPIO interrupts here (either a genuinely tight-spaced
//      noise/bounce event, or a baseline ESP32 interrupt-dispatch latency
//      floor), not fixable by juggling task/core placement.
//   5. Current: ESP32 RMT peripheral, RX mode, channel A only (see below).
//
// Why RMT, and why only one channel: RMT's receive mode timestamps every
// level change in dedicated hardware, with a real hardware glitch filter
// (filter_ticks_thresh - discards pulses shorter than this before they're
// ever recorded) - it doesn't depend on an interrupt being serviced
// promptly the way GPIO edge-interrupts or a periodic poll both do, so it
// can't repeat implementation #4's failure mode. The toolchain actually
// available here only has the older, ring-buffer-based driver/rmt.h API
// (checked directly - the modern per-edge-callback channel-handle API
// FastLED's own IDF5 code assumes doesn't exist in this specific
// arduino-esp32 package, and upgrading the platform doesn't change that -
// see TODO.md), and each RMT channel only watches one GPIO. Running RMT on
// *both* A and B and merging their independently-flushed ring-buffer
// streams back into a combined timeline (to reconstruct real X4 direction)
// is possible but genuinely fragile - real synchronization complexity for
// code that isn't meant to be permanent. Instead: RMT captures channel A
// only (hardware-filtered edge count, X2 resolution - both rising and
// falling edges), and direction is taken from the stepper's own currently
// commanded direction (stepper->getCurrentSpeedInMilliHz()'s sign) rather
// than sampled from channel B. This is a deliberate, accepted scope
// reduction: it can no longer independently detect the shaft spinning the
// "wrong way" relative to what's commanded (never actually observed or
// suspected in this whole investigation), but it fully preserves what this
// tool has actually been used for - catching a real *count/magnitude*
// mismatch - while removing the cross-channel timing-correlation problem
// entirely. Resolution drops from ~10 steps/count (X4) to roughly
// ~20 steps/count (X2) - still far finer than the hundreds-of-steps lag
// this is meant to catch.
#define encoderPinA D0
#define encoderPinB D9  // wired, not read by the current (RMT) implementation

void initEncoder();    // Call from setup() (or a Core 0 task, per core0_task.h) - configures and starts RMT RX on channel A

// Call periodically (e.g. every ~10-20ms) to drain RMT's ring buffer and
// fold newly-captured edges into cumulativeCount. Unlike the GPIO-ISR
// version, this one does need a per-loop update call - RMT's ring buffer
// is drained by the application, not delivered via a per-edge callback.
// See core0_task.cpp, where this is called from the Core 0 task's own loop.
//
// Also safe (and expected) to call directly, synchronously, from wherever
// the stepper's commanded direction is about to change - found necessary
// on the bench (2026-09-06): if any real captured data is still sitting in
// the ring buffer at the moment a move finishes, and nothing drains it
// before the *next* (opposite-direction) move is issued, that whole
// leftover batch gets attributed the new, wrong direction once the Core 0
// task's periodic drain eventually gets to it - not a rare 1-2-edge
// rounding error as originally estimated, but entire legs' worth of data,
// ~19% of legs in one real sweep. Calling this right after detecting the
// stepper has stopped (encoder_diag.cpp's updateEncoderDiag() does this)
// and before issuing the reversed move flushes any straggler under the
// *old*, still-correct direction - stepper->getCurrentSpeedInMilliHz()
// reads 0 at that exact instant, so the "keep last known direction"
// fallback below does the right thing automatically. Thread-safe (a
// portMUX_TYPE spinlock guards the shared counters) specifically to make
// this multi-context calling pattern safe.
void updateEncoder();

// Cumulative edge count (X2 - both rising and falling edges on channel A
// only, see the file comment for why) since boot or the last
// resetEncoderCount(), signed - direction applied per drained batch from
// the stepper's own currently commanded direction. Sign convention matches
// stepper_handler's (increasing count = increasing stepper position,
// moving away from the switch) by construction, since direction is taken
// directly from the stepper rather than independently measured.
int32_t getEncoderCount();

// Heuristic diagnostic, not a precise count: increments if a single drain
// call pulls an unusually large batch of edges (see ENCODER_BACKLOG_THRESHOLD
// in encoder_handler.cpp) - suggestive of updateEncoder() not being called
// often enough and a backlog building up in the software ring buffer. RMT's
// own hardware-to-ring-buffer path is serviced by ESP-IDF's installed
// driver ISR independent of this code's own scheduling, so actual data loss
// here would require the *software* ring buffer itself (sized generously in
// initEncoder()) to fill - structurally a much smaller risk than
// implementation #4's per-edge ISR-latency loss, but not zero, hence this
// stays as a real (if approximate) health signal rather than being removed.
uint32_t getMissedTransitionCount();

// Zero the count - mirrors stepper->setCurrentPosition(0), for calibrating
// against a known reference. Called automatically at the end of a
// successful homing cycle (stepper_handler.cpp's HOMING_RETURN_TO_ZERO),
// once the stepper is confirmed back at its own trusted position 0 - so the
// encoder's zero reference resyncs to the stepper's every homing, rather
// than drifting from whatever it happened to read at boot.
void resetEncoderCount();
bool isEncoderInitialized();  // True once initEncoder() has configured successfully

#endif
