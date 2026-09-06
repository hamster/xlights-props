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
// Wired: channel A = D0, channel B = D9 - standard two-wire quadrature, both
// channels read by the current implementation (see below).
//
// Firmware went through six implementations chasing an accurate count:
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
//      reduced it.
//   5. ESP32 RMT peripheral, RX mode, channel A only, direction inferred
//      from the stepper's own commanded direction - chosen because RMT
//      timestamps edges in hardware rather than depending on interrupt
//      latency, and because the toolchain here only has the older, ring-
//      buffer-based driver/rmt.h API (not the modern per-edge-callback
//      channel-handle API), which made merging two independently-flushed
//      channels back into one timeline (for real X4 direction) look too
//      fragile to bother with. This turned out to be a mistake: bench
//      testing 2026-09-06 found the RMT RX overflow condition ("RMT RX
//      BUFFER FULL", from a single long continuous move exceeding the
//      hardware's on-chip memory before any idle gap triggers a flush) has
//      a real, non-deterministic chance of hard-crashing *both* cores
//      (Core 0 interrupt wdt timeout). Raising mem_block_num to survive
//      longer continuous runs made it worse, not better, on two different
//      channels (one silently stopped capturing anything at all; the other
//      still crashed). Lowering the idle-flush threshold to shrink a
//      separate direction-at-reversal race also crashed the device.
//      Isolating every one of those changes back out (WiFi on/off, RMT
//      channel/mem_block_num/idle_threshold, a settle-wait before trusting
//      a drain) failed to find a single reliable culprit - the crash
//      reproduced even with every setting back at its original, first-
//      light-of-day values. Conclusion: the crash isn't tied to any one
//      knob, it's inherent to this driver's overflow-recovery path under
//      sustained continuous motion, and the session's one clean run was
//      luck, not a stable baseline. Abandoned.
//   6. Current: hardware timer poll, both channels, in software. See below.
//
// Why a timer poll instead of any interrupt-driven scheme: every ISR-driven
// approach tried above (GPIO edge interrupts, RMT) turned out to have a real
// failure mode tied to servicing edges promptly or to on-chip buffering.
// A periodic hardware timer ISR sidesteps both - no ring buffer, no
// dependency on FreeRTOS task scheduling, and the poll rate is chosen with
// enormous headroom over the real signal: this project's own bench data
// (a full-range move's total encoder counts divided by its measured
// duration) puts the real edge rate under 200Hz even at the fastest tested
// stepper speed, so a 4kHz poll (see ENCODER_POLL_HZ, encoder_handler.cpp)
// gives roughly 20x oversampling - a missed transition at that margin would
// require the ISR itself to be starved for over 5ms, which would show up
// directly in core0_task.h's own gap instrumentation. Both channels are
// read every tick and decoded with the standard X4 state-transition table,
// so direction is genuinely measured again (not inferred from the stepper's
// commanded direction, as implementation #5 was reduced to) - this also
// directly answers whether this approach generalizes to real DDP/tracking-
// mode tuning with mid-flight reversals: yes, because direction no longer
// depends on there being an idle checkpoint at all.
#define encoderPinA D0
#define encoderPinB D9

void initEncoder();    // Call from setup() (or a Core 0 task, per core0_task.h) - configures and starts the hardware timer poll

// No-op in the current (timer-poll) implementation - counting happens
// directly in the timer ISR, not via a periodic drain. Kept as a real
// function (rather than removed) so core0_task.cpp's periodic call and
// encoder_diag.cpp's synchronous call don't need to change; see this
// file's top-of-file history comment for why a periodic drain call
// mattered for the RMT implementation this replaced, and doesn't anymore.
void updateEncoder();

// Cumulative X4 quadrature count (both channels, every real transition)
// since boot or the last resetEncoderCount(), signed - direction is
// measured directly from the A/B phase relationship, not inferred. Sign
// convention matches stepper_handler's (increasing count = increasing
// stepper position, moving away from the switch) - confirmed on the bench,
// not just assumed; see the encoder sign-flip history in TODO.md if this
// ever needs re-deriving after a wiring change.
int32_t getEncoderCount();

// Real (not heuristic) diagnostic: counts genuine skipped transitions - the
// timer ISR sampled a state where *both* quadrature bits changed since the
// last sample, which a valid signal can't do in one real step (it takes two
// real edges to get there). At ~20x oversampling this should be ~0 for the
// life of the device; a nonzero value means the poll rate genuinely isn't
// keeping up (or the timer ISR itself is being starved - cross-check
// core0_task.h's own gap instrumentation).
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
