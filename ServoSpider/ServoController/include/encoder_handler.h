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
// Genuine GPIO change-interrupts (2026-09-05), not the ESP32 PCNT
// peripheral, and not polling either. History:
//   1. PCNT, interrupt-driven (pcnt_isr_service_install() + H_LIM/L_LIM
//      watch points) - severe regression on the bench: TMC stall detection
//      firing continuously, motor unstoppable and grinding into the homing
//      stop even with stall detection disabled.
//   2. PCNT again, polling instead of interrupt-driven for the overflow -
//      reproduced the exact same regression, ruling out the interrupt
//      specifically and implicating the PCNT peripheral/pins more broadly.
//   3. Dropped PCNT entirely for plain digitalRead() polling in loop() -
//      stable, but with a known real gap: if the pulley advances more than
//      one quadrature step between loop() iterations (e.g. loop() stalled
//      briefly on a slow web request, a Preferences flash write, or a
//      FastLED.show() call), the intermediate state is silently dropped,
//      not miscounted - lost data, not wrong data, but still a gap.
//   4. Current: real edge-triggered GPIO interrupts (attachInterrupt(...,
//      CHANGE)) on both A and B, the same mechanism this firmware already
//      uses reliably for the homing switch - not a peripheral at all, so
//      it can't repeat the PCNT regression above. Root cause of that
//      regression is now much better understood too, not just "these pins
//      might be electrically busy": FastAccelStepper's own ESP32-S3 backend
//      (MCPWM+PCNT, the default `DRIVER_DONT_CARE` picks on this chip -
//      confirmed by reading its source) uses PCNT hardware to count actual
//      generated step pulses as ground truth, installing its own raw,
//      low-level PCNT interrupt handler via esp_intr_alloc() and poking
//      peripheral-wide control registers directly. A second, independent
//      PCNT configuration for the encoder was very likely corrupting that
//      shared hardware/ISR state - not a coincidental pin conflict, the
//      stepper library's own permanent occupancy of PCNT on this chip. This
//      is a plain GPIO ISR (same category as handleHomingInterrupt() in
//      stepper_handler.cpp), not PCNT and not RMT (FastLED's peripheral,
//      also untouched by this), so it doesn't compete with either.
//
// Resolution note (confirmed on the bench, 2026-09-02, before the
// motor-shaft remount below): the encoder was originally coupled to the
// pulley hub with no step-up gearing, and the pulley itself only turns
// ~1.9 revolutions across the full 0-100% travel range (see README.md's
// "Drivetrain" note - this cross-check is also what pinned the worm gear
// down to an actual 1:10 ratio, not the 1:20 originally assumed) - so the
// full-range count was inherently coarse (~154 counts end to end with this
// encoder's PPR). Since remounted on the motor shaft itself (upstream of
// the worm reduction, a mechanical/mounting change, not a firmware one) -
// ~1517 counts end to end (2026-09-05, after adding 0.1uF filter capacitors
// on both channels - see TODO.md for the drift/noise saga this resolved),
// landing resolution at ~10.2 steps/count. Also resolves the worm ratio
// back to the documented 1:10 (1517/154 =~ 9.85) - an earlier uncapacitor'd
// reading of ~1267 counts had looked like it implied a different ratio, but
// that turned out to be the encoder undercounting real transitions, not a
// wrong gear-ratio assumption.
#define encoderPinA D0
#define encoderPinB D9

void initEncoder();    // Call from setup(), after initializeStepper() - attaches the GPIO interrupts

// Cumulative quadrature ticks (X4 decoding - all four A/B edges counted)
// since boot or the last resetEncoderCount(), signed. Updated directly by
// the ISR, so this is always current - no per-loop update call needed
// (unlike the earlier polling version). Sign confirmed on the bench
// (2026-09-02) to match stepper_handler's convention - increasing count
// means increasing stepper position (moving away from the switch, i.e.
// down) - via the lookup table in encoder_handler.cpp. Re-confirmed still
// correct on the motor-shaft mount (2026-09-05) after a false start (a
// bench report prompted negating the table a second time; a flashed test
// showed that broke it, so it was reverted back to the original single
// negation). If the encoder's physical mounting ever changes again,
// re-check this against a real flashed move, not against a description of
// one - see the table's own comment for how this went wrong once already.
int32_t getEncoderCount();

// How many times the ISR saw an "illegal" 2-bit jump (both A and B appeared
// to have changed between samples) since boot - i.e. a real quadrature edge
// that was never sampled and is permanently lost, not just delayed. Cumulative
// since boot; deliberately NOT cleared by resetEncoderCount() (it's a
// diagnostic of firmware/ISR-timing health, not part of the position
// reference). If this stays at or near 0 across a real test run, the
// encoder's position isn't losing counts in firmware - any remaining
// noise/drift is happening upstream of this code (electrical or
// mechanical). If it climbs noticeably, that's direct evidence of real
// ISR-timing loss, worth pursuing (e.g. raising this interrupt's priority)
// rather than assuming the RC filter alone was the whole fix.
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
