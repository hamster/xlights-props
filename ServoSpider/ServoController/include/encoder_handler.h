#ifndef ENCODER_HANDLER_H
#define ENCODER_HANDLER_H

#include <Arduino.h>

// Rotary encoder mounted directly on the pulley's hub (the same wheel the
// homing switch rides) - gives a real, continuous, independent position
// reading. This is ground truth: getCurrentPosition() on the stepper side
// is pure step-count bookkeeping and has no way to notice a commanded step
// that didn't physically happen (see $CHECKSTEPS / stepper_handler.h's
// StepCheckState for the indirect workarounds that exist because of that
// gap) - the encoder can, though see the resolution note below.
//
// Wired: channel A = D0, channel B = D9 - standard two-wire quadrature,
// which is all that's needed for both position and direction. The
// originally-planned third pin (D5, an optional Z/index channel for
// absolute recalibration) wasn't wired - genuinely optional, and A/B alone
// is already sufficient.
//
// Deliberately plain digitalRead() polling, no ESP32 PCNT peripheral at all
// (2026-09-02) - see encoder_handler.cpp's file comment for the two
// separate PCNT-based attempts that both caused a severe regression on the
// bench (TMC stall detection firing continuously, motor unstoppable and
// grinding into the homing stop) before this approach replaced them.
// updateEncoder() polls both pins and decodes quadrature transitions in
// software via a lookup table - the same underlying mechanism (digitalRead())
// already used reliably for the homing switch, zero new peripheral or
// interrupt footprint.
//
// Resolution note (confirmed on the bench, 2026-09-02): the encoder is
// coupled directly to the pulley hub with no step-up gearing, and the
// pulley itself only turns ~1.9 revolutions across the full 0-100% travel
// range - so the full-range count is inherently coarse (~154 counts end to
// end with this encoder's PPR), nowhere near step-count resolution. Fine
// for a coarse sanity cross-check; not fine-grained enough to catch a
// single lost step the way $CHECKSTEPS's switch-based approach can.
#define encoderPinA D0
#define encoderPinB D9

void initEncoder();    // Call from setup(), after initializeStepper()
void updateEncoder();  // Call every loop() iteration - see the file comment above for why this needs to run regularly

// Cumulative quadrature ticks (X4 decoding - all four A/B edges counted)
// since boot or the last resetEncoderCount(), signed. Only as current as
// the last updateEncoder() call. Sign confirmed on the bench (2026-09-02)
// to match stepper_handler's convention - increasing count means
// increasing stepper position (moving away from the switch) - via the
// lookup table in encoder_handler.cpp, which is intentionally the physical
// wiring's natural table negated. If the encoder's physical mounting
// relative to the pulley ever changes, re-check this against a real move
// rather than assuming it still holds.
int32_t getEncoderCount();

void resetEncoderCount();     // Zero the count - mirrors stepper->setCurrentPosition(0), for calibrating against a known reference (e.g. the homing switch)
bool isEncoderInitialized();  // True once initEncoder() has configured successfully

#endif
