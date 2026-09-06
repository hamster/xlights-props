#include "encoder_handler.h"

// Genuine GPIO change-interrupts, not the ESP32 PCNT peripheral, not
// polling - see encoder_handler.h's file comment for the two PCNT attempts
// that both caused a severe regression on the bench, why that's now
// understood to be PCNT-hardware/ISR contention with FastAccelStepper's own
// MCPWM+PCNT backend (not a coincidental pin issue), and why polling was
// itself only an interim step (it can silently drop a quadrature edge if
// the pulley advances more than one step between loop() iterations).
// handleEncoderInterrupt() is a plain GPIO ISR, the same category as
// stepper_handler.cpp's handleHomingInterrupt() already proven safe on this
// exact hardware - it only ever does cheap register reads and integer
// arithmetic, never anything that could touch non-IRAM flash code.
//
// Software quadrature decode via a standard 2-bit-state transition lookup
// table: index = (previous state << 2) | current state, where each state
// is (A<<1 | B). A legitimate quadrature signal only ever changes one bit
// at a time; the four transitions belonging to each direction of travel
// map to +1 or -1, and everything else (no real change, or an illegal
// two-bit jump) maps to 0 and is conservatively ignored rather than
// guessed at. An illegal jump should no longer actually happen now that
// every edge triggers the ISR directly, but the table stays defensive
// regardless.
//
// Signed so that increasing count matches stepper_handler's convention
// (increasing position = moving away from the switch, i.e. down - see
// stepper_handler.h's HomingState comments and CLAUDE.md's Homing Process
// section). Confirmed and negated for the original pulley-hub mount
// (2026-09-02). After the motor-shaft remount (2026-09-05), a bench report
// of the count still going the wrong way prompted negating this a second
// time (2026-09-05) - that turned out to be wrong (flashed test showed the
// *original* single-negated table was already correct on the motor shaft;
// the second negation broke it) - reverted back to this, the single
// negation, matching what was bench-confirmed working. If this ever needs
// re-checking again, confirm against a real move before changing it, not
// against a report alone - see resetEncoderCount()'s note.
static const int8_t QUAD_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

static volatile uint8_t lastQuadState = 0;
static volatile int32_t cumulativeCount = 0;
static volatile uint32_t missedTransitionCount = 0;
static bool encoderInitialized = false;

// Debounce guard - tried 2026-09-05, removed the same day. Added after a
// bench report of the encoder's zero reference drifting by ~150 counts per
// round trip even though the stepper's own return-to-switch position was
// perfectly repeatable, plus the discovery that the encoder's A/B lines have
// 10k pull-ups but no filter capacitor. A 1ms "ignore anything this soon
// after the last accepted edge" window was tried, reasoning that real edges
// even at max stepper speed should be several ms apart. Bench data promptly
// showed this was far too aggressive: a full 0-100% traverse that measured
// ~1267 counts with no filter dropped to 342-560 counts (and inconsistently
// so, cycle to cycle) with the 1ms filter active - meaning the real
// inter-edge spacing during actual motion is nowhere near as generous as
// that estimate assumed, and the filter was eating the vast majority of
// legitimate transitions, not just noise. Removed entirely rather than
// re-tuned to a smaller value, to get a clean, unfiltered baseline again
// before deciding whether debouncing (software or the hardware RC-filter
// alternative - see TODO.md) is worth pursuing further, or whether the
// original drift is actually mechanical after all.
//
// Shared by both channel interrupts - either pin changing means the
// combined 2-bit state may have changed, so just re-read both and let the
// table sort out what actually happened (including "nothing," if this
// fired on the other pin's own settle/debounce noise).
//
// missedTransitionCount (2026-09-05) - added to actually answer "are we
// missing counts in the firmware" with data instead of guessing. If both A
// and B appear to have changed by the time this ISR reads them (an
// "illegal" 2-bit jump in the table - can only happen if a real
// intermediate quadrature state occurred and was never sampled, i.e. this
// ISR wasn't serviced promptly enough for that one edge), the direction
// and count are genuinely unrecoverable without risking a wrong guess, so
// it's still dropped, same as before - but now counted, so a real firmware-
// side loss shows up as a nonzero, growing number instead of being
// invisible. See getMissedTransitionCount()'s declaration comment for how
// to read this against a real test run.
void IRAM_ATTR handleEncoderInterrupt() {
  uint8_t newState = (digitalRead(encoderPinA) << 1) | digitalRead(encoderPinB);
  if (newState != lastQuadState) {
    uint8_t index = (lastQuadState << 2) | newState;
    int8_t delta = QUAD_TABLE[index];
    if (delta == 0) {
      missedTransitionCount++;
    } else {
      cumulativeCount += delta;
    }
    lastQuadState = newState;
  }
}

void initEncoder() {
  // External 10k pull-ups to 3.3V are already present on both channels
  // (open-collector-style encoder output, common wired to ground) - plain
  // INPUT here, no need for the internal pull-up too.
  pinMode(encoderPinA, INPUT);
  pinMode(encoderPinB, INPUT);

  lastQuadState = (digitalRead(encoderPinA) << 1) | digitalRead(encoderPinB);
  cumulativeCount = 0;

  attachInterrupt(digitalPinToInterrupt(encoderPinA), handleEncoderInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(encoderPinB), handleEncoderInterrupt, CHANGE);

  encoderInitialized = true;
  Serial.print("Encoder: GPIO change-interrupt quadrature decode initialized on A=D0(GPIO");
  Serial.print(encoderPinA);
  Serial.print(") B=D9(GPIO");
  Serial.print(encoderPinB);
  Serial.println(")");
}

int32_t getEncoderCount() {
  return cumulativeCount;
}

uint32_t getMissedTransitionCount() {
  return missedTransitionCount;
}

void resetEncoderCount() {
  cumulativeCount = 0;
}

bool isEncoderInitialized() {
  return encoderInitialized;
}
