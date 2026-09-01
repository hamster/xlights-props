#ifndef ENCODER_HANDLER_H
#define ENCODER_HANDLER_H

#include <Arduino.h>

// Rotary encoder mounted directly on the pulley's center shaft (the same
// wheel the homing switch rides) - gives a real, continuous, independent
// position reading via the ESP32-S3's hardware PCNT (pulse counter)
// peripheral, decoded in silicon (no loop()/CPU cost for the quadrature
// decode itself). This is ground truth: getCurrentPosition() on the
// stepper side is pure step-count bookkeeping and has no way to notice a
// commanded step that didn't physically happen (see $CHECKSTEPS /
// stepper_handler.h's StepCheckState for the indirect workarounds that
// exist because of that gap) - the encoder can.
//
// Wired: channel A = D0, channel B = D9 - standard two-wire quadrature,
// which is all that's needed for both position and direction. The
// originally-planned third pin (D5, an optional Z/index channel for
// absolute recalibration) wasn't wired - genuinely optional, and A/B alone
// is already sufficient.
//
// Deliberately polling-based, not interrupt-driven (2026-09-02) - an
// earlier version installed a dedicated PCNT interrupt (via
// pcnt_isr_service_install()) to catch the 16-bit hardware counter's
// overflow via watch points. That coincided with a severe, unexplained
// regression on the bench (TMC stall detection firing continuously, the
// motor unstoppable and driving repeatedly into the homing stop even with
// stall detection disabled) - never conclusively root-caused, but not
// worth risking again for a feature this optional. updateEncoder() instead
// polls the raw counter and relies on 16-bit signed-subtraction arithmetic
// to handle the wraparound correctly on its own - no watch points, no ISR,
// no new interrupt source added to this firmware at all.
#define encoderPinA D0
#define encoderPinB D9

void initEncoder();    // Call from setup(), after initializeStepper()
void updateEncoder();  // Call every loop() iteration - see the file comment above for why this needs to run regularly

// Cumulative quadrature ticks (X4 decoding - all four A/B edges counted)
// since boot or the last resetEncoderCount(), signed. Only as current as
// the last updateEncoder() call. Whether increasing ticks correspond to
// increasing or decreasing stepper position depends on which physical
// direction the encoder shaft was mounted - not yet confirmed on the
// bench, so don't assume the sign matches stepper_handler's convention
// until checked against a real move.
int32_t getEncoderCount();

void resetEncoderCount();     // Zero the count - mirrors stepper->setCurrentPosition(0), for calibrating against a known reference (e.g. the homing switch)
bool isEncoderInitialized();  // True once initEncoder() has configured the PCNT unit successfully

#endif
