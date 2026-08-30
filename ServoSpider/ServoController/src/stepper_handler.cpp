#include "stepper_handler.h"
#include "tmc_handler.h"
#include <Preferences.h>

extern Preferences preferences;

// Stepper global variables
FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *stepper = NULL;
volatile bool interruptTriggered = false;
volatile bool homingStallDetected = false;
// Set alongside interruptTriggered, but consumed separately (see
// handleHomingInterrupt/updateHoming) so the deferred forceStop() fires
// exactly once per switch edge instead of on every loop() iteration for as
// long as interruptTriggered happens to stay set.
volatile bool pendingForceStop = false;
int bottomPosition = 0;
bool homed = false;
HomingState homingState = HOMING_IDLE;
unsigned long homingStateTime = 0;
int homingCounter = 0;

StepCheckState stepCheckState = STEPCHECK_IDLE;
bool stepCheckTripped = false;
long stepCheckTripPosition = 0;
long stepCheckTargetPosition = 0;
unsigned long stepCheckElapsedMs = 0;
static unsigned long stepCheckStartMs = 0;

// Stepper configuration variables
int stepperSpeedConfig = stepperSpeed;
int stepperAccelConfig = stepperAccel;
// Was 0 (no jump-start boost - every move ramps from a dead stop). Bench
// testing (2026-08-30) found the motor could intermittently stall right at
// the very first step of a move (buzzes in place briefly, then either
// self-recovers or needs a nudge) - a classic stepper starting-torque
// symptom, not something a lower cruise acceleration alone fully fixes.
// FastAccelStepper's jump-start feature exists specifically for this: one
// deliberately larger first step (speed = sqrt(2*accel*jump_step), so 20
// steps at stepperAccelHoming=20000 gives roughly an 894Hz starting kick)
// instead of ramping from true zero. Tested clean across multiple homing
// cycles alongside the stepperAccelHoming reduction above.
int jumpStartConfig = 20;
bool autoHomeOnBootConfig = true;
int stepperSpeedHomingConfig = stepperSpeedHoming;
int stepperAccelHomingConfig = stepperAccelHoming;

// Small-move tracking profile defaults. Validated on real hardware (not
// just a theoretical guess): same speed as the normal profile, but
// acceleration cut to roughly 1/4, with a 2000-step threshold. What
// matters most for avoiding stalls/skipped steps is the lower
// acceleration specifically - target speed barely matters for small
// moves since they rarely get anywhere near cruise speed anyway. Still
// worth re-tuning per prop: too aggressive an accel stalls/skips steps
// moving from near-rest on every small update; too gentle and the
// trolley visibly lags behind a fast-panning DDP curve.
bool stepperTrackEnabledConfig = true;
int stepperTrackThresholdConfig = 2000;
int stepperTrackSpeedConfig = stepperSpeed;
int stepperTrackAccelConfig = stepperAccel / 4;
int stepperTrackMaxLagConfig = 3000;

// Motion strategy - see StepperTrackMode's declaration comment. Default
// stays DIRECT (the original/current behavior) so flashing this firmware
// doesn't change anything until a mode is explicitly picked in Settings.
int stepperTrackModeConfig = TRACK_MODE_DIRECT;
int stepperCoalesceMsConfig = 250;
int stepperCoalesceStepsConfig = 400;
int stepperStreamRateWindowMsConfig = 250;
int stepperStreamSettleMsConfig = 150;

// TRACK_MODE_LOOKAHEAD defaults. 5000 steps comfortably exceeds the worst-case
// stopping distance (speed^2/(2*accel)) at the default tracking profile
// (6500 Hz / 5000 steps/s^2 -> 4225 steps) - re-check this margin if either
// is tuned to a lower accel or higher speed than the defaults.
int stepperLookaheadStepsConfig = 5000;
int stepperLookaheadSettleMsConfig = 150;

bool stepperSettingsPendingSave = false;

// Deferred flash write for Stepper Configuration settings - see the
// declaration comment in stepper_handler.h for why this can't just happen
// synchronously inside the save handler.
void persistStepperSettingsIfPending() {
  if (!stepperSettingsPendingSave) {
    return;
  }
  if (stepper != NULL && stepper->isRunning()) {
    return;  // wait for a quiet moment - values are already live in RAM
  }

  preferences.putInt("stepperSpeed", stepperSpeedConfig);
  preferences.putInt("stepperAccel", stepperAccelConfig);
  preferences.putInt("jumpStart", jumpStartConfig);
  preferences.putBool("autoHomeOnBoot", autoHomeOnBootConfig);
  preferences.putInt("stepSpeedHome", stepperSpeedHomingConfig);
  preferences.putInt("stepAccelHome", stepperAccelHomingConfig);
  preferences.putBool("stepTrackEn", stepperTrackEnabledConfig);
  preferences.putInt("stepTrackThresh", stepperTrackThresholdConfig);
  preferences.putInt("stepTrackSpeed", stepperTrackSpeedConfig);
  preferences.putInt("stepTrackAccel", stepperTrackAccelConfig);
  preferences.putInt("stepTrackMaxLag", stepperTrackMaxLagConfig);
  preferences.putInt("stepTrackMode", stepperTrackModeConfig);
  preferences.putInt("stepCoalesceMs", stepperCoalesceMsConfig);
  preferences.putInt("stepCoalesceSt", stepperCoalesceStepsConfig);
  preferences.putInt("stepStreamRateW", stepperStreamRateWindowMsConfig);
  preferences.putInt("stepStreamSettl", stepperStreamSettleMsConfig);
  preferences.putInt("stepLookaheadSt", stepperLookaheadStepsConfig);
  preferences.putInt("stepLookaheadMs", stepperLookaheadSettleMsConfig);

  stepperSettingsPendingSave = false;
  Serial.println("Stepper settings persisted to flash (motor now idle)");
}

// Homing switch interrupt
//
// Deliberately does nothing but set flags. FastAccelStepper::forceStop()
// is a regular (non-IRAM) function; calling it directly from here used to
// work in practice, but crashes ("Cache disabled but cached memory region
// accessed") if this ISR fires while flash cache happens to be disabled -
// which any Preferences.putX() write briefly does. updateHoming() (called
// every loop() iteration, always in normal task context, never from an
// ISR) calls forceStop() from there instead, which is always cache-safe.
//
// pendingForceStop is separate from interruptTriggered, and deliberately
// consumed (cleared) the instant updateHoming() acts on it, regardless of
// homing state - this replicates the original "stop exactly once, right
// when the switch trips" behavior. interruptTriggered stays a level flag
// that only the specific HOMING_* state waiting for this edge clears, once
// it's actually ready to react to it. Without this split, a stray edge
// during a state that doesn't touch interruptTriggered (e.g. the initial
// small-step move off the switch) would leave it stuck true, and a naive
// "if (interruptTriggered) forceStop()" at the top of updateHoming() would
// then force-stop every subsequent move - including the very next
// clear-the-switch move - before it can actually get clear.
void IRAM_ATTR handleHomingInterrupt() {
  interruptTriggered = true;
  pendingForceStop = true;
}

// Find the home.  We run in one direction until we hit the homing switch.
// Then we zero the steps, and run in the other direction until we hit the homing switch.
// Halfway is the bottom point.
void initializeStepper() {
  // Setup stepper controller
  engine.init();
  stepper = engine.stepperConnectToPin(stepperStepPin);
  if (stepper) {
    stepper->setDirectionPin(stepperDirectionPin, LOW);
    stepper->setEnablePin(stepperEnablePin);
    stepper->setAutoEnable(true);
    stepper->setSpeedInHz(stepperSpeedConfig);
    stepper->setAcceleration(stepperAccelConfig);
    stepper->setJumpStart(jumpStartConfig);

    Serial.println("Stepper initialized with configuration:");
    Serial.print("Speed: ");
    Serial.print(stepperSpeedConfig);
    Serial.print(" Hz, Acceleration: ");
    Serial.print(stepperAccelConfig);
    Serial.print(" Hz/s, Jump Start: ");
    Serial.print(jumpStartConfig);
    Serial.println(" steps");
  }

  // Setup homing switch
  pinMode(homingSwitchPin, INPUT_PULLUP);
  // Trigger on RISING edge (when switch activates based on actual hardware behavior)
  attachInterrupt(digitalPinToInterrupt(homingSwitchPin), handleHomingInterrupt, RISING);
  // Delay a moment for the debounce to charge
  delay(50);
}

// Non-blocking homing functions

void startHoming() {
  // Don't start homing if already in progress
  if (isHoming()) {
    Serial.println("Homing already in progress, ignoring request");
    return;
  }

  Serial.println("Starting non-blocking homing...");
  homed = false;
  homingState = HOMING_CHECK_SWITCH;
  homingStateTime = millis();
  homingCounter = 0;
  interruptTriggered = false;
  pendingForceStop = false;  // Defensive: never let a stale flag from before this attempt fire mid-sequence

  stepper->setAcceleration(stepperAccelHomingConfig);
  stepper->setSpeedInHz(stepperSpeedHomingConfig);
}

bool isHoming() {
  return (homingState != HOMING_IDLE && homingState != HOMING_COMPLETE && homingState != HOMING_ERROR);
}

bool isHomingSwitchTripped() {
  // Based on actual hardware: HIGH = switch triggered, LOW = switch not triggered
  return (digitalRead(homingSwitchPin) == HIGH);
}

bool isStepChecking() {
  return stepCheckState != STEPCHECK_IDLE;
}

// Begins a deliberate verification move toward targetPosition, using the
// normal (not tracking) profile, entirely independent of DDP/tracking-mode -
// this is a direct diagnostic move, not something a control strategy under
// test should be able to influence. Caller (tuning_handler.cpp) is expected
// to have already checked homed/isHoming()/isRunning(); this only adds a
// defensive re-check so it can't be started twice concurrently.
void startStepCheck(long targetPosition) {
  if (isStepChecking() || isHoming() || stepper->isRunning()) {
    return;
  }
  stepCheckTargetPosition = targetPosition;
  stepCheckTripped = false;
  stepCheckTripPosition = stepper->getCurrentPosition();
  stepCheckStartMs = millis();
  stepper->setAcceleration(stepperAccelConfig);
  stepper->setSpeedInHz(stepperSpeedConfig);
  stepper->moveTo(targetPosition);
  tmcResetStallRampTimer();
  stepCheckState = STEPCHECK_MOVING;
}

// Call every loop() iteration, *before* updateHoming() - both react to the
// same pendingForceStop flag, and this needs first refusal on it while a
// check is in progress so updateHoming() doesn't instead treat the trip as
// a generic "out of homing" event (which would stop the motor correctly but
// never record where it happened or print the CHECKSTEPS_RESULT line the
// harness/serial user is waiting for).
void updateStepCheck() {
  if (stepCheckState != STEPCHECK_MOVING) {
    return;
  }

  if (pendingForceStop) {
    pendingForceStop = false;
    stepCheckTripPosition = stepper->getCurrentPosition();
    stepper->forceStop();
    stepCheckTripped = true;
    stepCheckElapsedMs = millis() - stepCheckStartMs;
    stepCheckState = STEPCHECK_IDLE;
    // The switch firing here is only physically possible if it's genuinely
    // at that location right now - which means our belief about where "0"
    // (or wherever the target was) sits is stale. Same call as an
    // unexpected trip during normal operation: require a fresh re-home
    // before trusting position again.
    homed = false;
    Serial.print("CHECKSTEPS_RESULT tripped=1 tripPos=");
    Serial.print(stepCheckTripPosition);
    Serial.print(" target=");
    Serial.print(stepCheckTargetPosition);
    Serial.print(" elapsedMs=");
    Serial.println(stepCheckElapsedMs);
    return;
  }

  if (!stepper->isRunning()) {
    stepCheckTripped = false;
    stepCheckTripPosition = stepper->getCurrentPosition();
    stepCheckElapsedMs = millis() - stepCheckStartMs;
    stepCheckState = STEPCHECK_IDLE;
    Serial.print("CHECKSTEPS_RESULT tripped=0 tripPos=");
    Serial.print(stepCheckTripPosition);
    Serial.print(" target=");
    Serial.print(stepCheckTargetPosition);
    Serial.print(" elapsedMs=");
    Serial.println(stepCheckElapsedMs);
  }
}

static unsigned long lastWaitClearPrintMs = 0;

// Shared periodic position/speed print for the three "wait for a move-off-
// switch move to finish" states, to directly see a reported "pauses
// partway through, then continues" symptom rather than guessing at it
// (2026-08-30). Uses its own timer, separate from homingStateTime (which
// these states also use for their own timeouts).
static void printWaitClearDiag(unsigned long currentTime) {
  if (currentTime - lastWaitClearPrintMs < 100) return;
  lastWaitClearPrintMs = currentTime;
  Serial.print("  [wait clear switch] pos=");
  Serial.print(stepper->getCurrentPosition());
  Serial.print(" speed=");
  Serial.print(stepper->getCurrentSpeedInMilliHz() / 1000);
  Serial.print(" running=");
  Serial.println(stepper->isRunning() ? 1 : 0);
}

void updateHoming() {
  // Deferred from the ISR (see handleHomingInterrupt) - forceStop() isn't
  // IRAM-safe, so it's issued here instead, in normal task context, as soon
  // as we notice the flag. Checked and cleared *before* the "not homing"
  // early return below, and unconditionally - not just while homing is
  // active. updateStepCheck() (called before this, from loop()) claims the
  // flag first whenever a skipped-step check is in progress, so this path
  // only ever sees it when nothing else was already handling it. This used
  // to be gated behind that early return, which meant a
  // switch trip during normal (non-homing) operation left the flag stuck
  // true indefinitely (never consumed, since nothing here ran while idle).
  // The next time homing started, that stale flag would be treated as "the
  // switch just tripped right now" and force-stop the motor in the very
  // same loop() iteration as HOMING_CHECK_SWITCH's first runBackward() call
  // for HOMING_FIND_INITIAL - racing against it and sometimes silently
  // swallowing that first move entirely (reported as "homing prints that
  // it's searching but the motor never actually moves"; recoverable by any
  // unrelated manual move, which happened to reset the stepper library's
  // internal state enough to unstick it).
  //
  // A trip while not homing is now also treated as a real event rather
  // than a silently-discarded one - *if* it happens somewhere the trolley
  // had no reason to be near the switch at all, that's real drift and the
  // right response is to stop and require a fresh re-home rather than
  // pretend nothing happened.
  //
  // But position 0 - where this switch physically lives - is not itself
  // off-limits outside of homing: normal DDP operation maps its whole
  // 0-255 range onto [0, bottomPosition], so legitimately driving the
  // trolley all the way down to real position 0 is an ordinary, intended
  // endpoint of travel, not a diagnostic event. Confirmed on the bench
  // (2026-08-30): a real triangle-wave run reaching position 0 tripped the
  // switch exactly as designed, and treating every such trip as drift
  // broke every subsequent command with "not homed" for the rest of the
  // session - the fix below only escalates when the trip happens far
  // enough from 0 that it can't be explained by legitimately arriving
  // there, using the same tolerance $CHECKSTEPS's bench data suggested
  // (measured early-trip offset there was ~25 steps; this is deliberately
  // generous versus that).
  if (pendingForceStop) {
    pendingForceStop = false;
    long tripPosition = stepper->getCurrentPosition();
    stepper->forceStop();
    if (!isHoming() && homed) {
      const long ZERO_TRIP_TOLERANCE = 200;  // steps
      if (labs(tripPosition) > ZERO_TRIP_TOLERANCE) {
        Serial.print("WARNING: Homing switch tripped outside of homing, far from position 0 (at ");
        Serial.print(tripPosition);
        Serial.println(") - stopping and marking system as not homed");
        homed = false;
      } else {
        Serial.print("Homing switch tripped at position ");
        Serial.print(tripPosition);
        Serial.println(" during normal operation - expected endpoint of travel, not treated as drift");
      }
    }
  }

  // Real stall during an active homing search (tmc_handler.cpp's stall
  // check, extended to also run while homing - see homingStallDetected's
  // declaration comment). Consumed unconditionally, same pattern as
  // pendingForceStop above, so it can't go stale. Checked before the
  // "not homing" early return, but only actually aborts anything if a
  // search genuinely is in progress - a stray/late flag while idle is just
  // discarded.
  if (homingStallDetected) {
    homingStallDetected = false;
    if (isHoming()) {
      Serial.println("ERROR: Stall detected during homing search (motor commanded to move but not turning) - aborting");
      stepper->forceStop();
      stepper->setAcceleration(stepperAccelConfig);
      stepper->setSpeedInHz(stepperSpeedConfig);
      homingState = HOMING_ERROR;
      homed = false;
      return;
    }
  }

  if (!isHoming() && homingState != HOMING_COMPLETE && homingState != HOMING_ERROR) {
    return;  // Not homing
  }

  unsigned long currentTime = millis();

  switch (homingState) {
  case HOMING_CHECK_SWITCH:
    // Check if switch is already triggered at startup
    // Based on actual hardware: HIGH = switch triggered, LOW = switch not triggered
    if (digitalRead(homingSwitchPin) == HIGH) {
      Serial.println("Homing switch triggered at bootup, will move off switch...");
      homingState = HOMING_MOVE_OFF_FORWARD;
      homingCounter = 0;
      homingStateTime = currentTime;
    } else {
      // Switch is clear, go directly to finding initial position
      Serial.println("Moving to find initial homing position...");
      homingState = HOMING_FIND_INITIAL;
      interruptTriggered = false;
      stepper->runBackward();
      tmcResetStallRampTimer();
      homingStateTime = currentTime;
    }
    break;

  case HOMING_MOVE_OFF_FORWARD:
    // Try moving forward to clear the switch
    // Based on actual hardware: HIGH = switch triggered, LOW = switch not triggered
    if (currentTime - homingStateTime >= 100) {  // Check every 100ms
      if (digitalRead(homingSwitchPin) == LOW) {
        // Switch cleared (pin went LOW), move a bit more to get fully off
        Serial.println("Forward worked! Moving clear of switch...");
        stepper->move(2500);
        tmcResetStallRampTimer();
        homingState = HOMING_WAIT_CLEAR_SWITCH;
        homingStateTime = currentTime;
      } else if (homingCounter < 25) {
        // Still on switch (pin still HIGH), keep moving forward
        stepper->moveTo(10 * homingCounter);
        tmcResetStallRampTimer();
        homingCounter++;
        homingStateTime = currentTime;
      } else {
        // Forward didn't work, try backward
        Serial.print("Didn't clear forward, trying backward... pos=");
        Serial.println(stepper->getCurrentPosition());
        homingCounter = 0;
        homingState = HOMING_MOVE_OFF_BACKWARD;
        homingStateTime = currentTime;
      }
    }
    break;

  case HOMING_MOVE_OFF_BACKWARD:
    // Try moving backward to clear the switch
    // Based on actual hardware: HIGH = switch triggered, LOW = switch not triggered
    if (currentTime - homingStateTime >= 100) {  // Check every 100ms
      if (digitalRead(homingSwitchPin) == LOW) {
        // Switch cleared (pin went LOW), move a bit more to get fully off
        Serial.println("Reverse worked! Moving clear of switch...");
        stepper->move(-2500);
        tmcResetStallRampTimer();
        homingState = HOMING_WAIT_CLEAR_SWITCH;
        homingStateTime = currentTime;
      } else if (homingCounter < 25) {
        // Still on switch (pin still HIGH), keep moving backward
        stepper->moveTo(-10 * homingCounter);
        tmcResetStallRampTimer();
        homingCounter++;
        homingStateTime = currentTime;
      } else {
        // Couldn't clear switch
        Serial.print("ERROR: Homing switch stuck! pos=");
        Serial.println(stepper->getCurrentPosition());
        stepper->forceStop();
        stepper->setAcceleration(stepperAccelConfig);
        stepper->setSpeedInHz(stepperSpeedConfig);
        homingState = HOMING_ERROR;
        homed = false;
      }
    }
    break;

  case HOMING_WAIT_CLEAR_SWITCH:
    // Wait for the move-off-switch movement to complete.
    printWaitClearDiag(currentTime);
    if (!stepper->isRunning()) {
      Serial.println("Cleared switch, starting homing search...");
      homingState = HOMING_FIND_INITIAL;
      interruptTriggered = false;

      // Reset to homing speed/accel before searching
      stepper->setAcceleration(stepperAccelHomingConfig);
      stepper->setSpeedInHz(stepperSpeedHomingConfig);

      Serial.print("Starting runBackward() with speed ");
      Serial.print(stepperSpeedHomingConfig);
      Serial.print(" Hz, accel ");
      Serial.print(stepperAccelHomingConfig);
      Serial.println(" Hz/s");

      stepper->runBackward();
      tmcResetStallRampTimer();

      Serial.print("Stepper isRunning: ");
      Serial.println(stepper->isRunning() ? "true" : "false");

      homingStateTime = currentTime;
      homingCounter = 0;
    } else if (currentTime - homingStateTime >= 10000) {  // 10 second timeout
      Serial.println("ERROR: Timed out moving off switch!");
      stepper->forceStop();
      stepper->setAcceleration(stepperAccelConfig);
      stepper->setSpeedInHz(stepperSpeedConfig);
      homingState = HOMING_ERROR;
      homed = false;
    }
    break;

  case HOMING_FIND_INITIAL:
    // Wait for interrupt or timeout
    if (currentTime - homingStateTime >= 500) {  // Print status every 500ms
      // Position (not just a dot) so a stuck/jammed search leaves a clear
      // record of which direction it was actually moving (or not moving at
      // all) - added after a real jam that couldn't be explained by the
      // periodic dot-print alone (2026-08-30); see TODO.md.
      Serial.print(". pos=");
      Serial.print(stepper->getCurrentPosition());
      Serial.print(" speed=");
      Serial.println(stepper->getCurrentSpeedInMilliHz() / 1000);
      homingStateTime = currentTime;
      homingCounter++;

      if (homingCounter > 60) {  // 30 second timeout
        Serial.println("\nERROR: Timed out finding initial position!");
        stepper->forceStop();
        stepper->setAcceleration(stepperAccelConfig);
        stepper->setSpeedInHz(stepperSpeedConfig);
        homingState = HOMING_ERROR;
        homed = false;
        return;
      }
    }

    if (interruptTriggered) {
      Serial.println("\nFound initial homing position");
      // Safe to relabel the pulse count immediately - setCurrentPosition()
      // is just bookkeeping, not a motion command, so it doesn't need to
      // wait for anything.
      stepper->setCurrentPosition(0);
      interruptTriggered = false;
      homingState = HOMING_SETTLE_AFTER_INITIAL;
      homingStateTime = currentTime;
    }
    break;

  case HOMING_SETTLE_AFTER_INITIAL:
    // Wait for the interrupt-triggered forceStop() to actually finish before
    // issuing the next move - forceStop() isn't instantaneous, and issuing
    // move(2500) immediately used to assume the motor was already at rest
    // when it could still be coasting/decelerating from the search. That
    // produced a real, visible decelerate-reverse-reaccelerate "pause" once
    // stepperAccelHomingConfig was lowered enough (from the earlier stall
    // fix) to make it perceptible instead of instantaneous (2026-08-30).
    printWaitClearDiag(currentTime);
    if (!stepper->isRunning()) {
      Serial.println("Moving off switch...");
      stepper->move(2500);  // Move forward 2500 steps
      tmcResetStallRampTimer();
      homingState = HOMING_MOVE_OFF_INITIAL;
      homingStateTime = currentTime;
      homingCounter = 0;
    } else if (currentTime - homingStateTime >= 2000) {  // shouldn't take long - safety net
      Serial.println("ERROR: Timed out waiting for stop to settle after finding initial position!");
      stepper->forceStop();
      stepper->setAcceleration(stepperAccelConfig);
      stepper->setSpeedInHz(stepperSpeedConfig);
      homingState = HOMING_ERROR;
      homed = false;
    }
    break;

  case HOMING_MOVE_OFF_INITIAL:
    // Wait for move to complete and switch to clear
    printWaitClearDiag(currentTime);
    if (!stepper->isRunning() && digitalRead(homingSwitchPin) == LOW) {
      Serial.println("Switch cleared, reversing...");
      stepper->runForward();
      tmcResetStallRampTimer();
      homingState = HOMING_FIND_OTHER_END;
      homingStateTime = currentTime;
      homingCounter = 0;
    } else if (currentTime - homingStateTime >= 10000) {  // 10 second timeout
      Serial.println("ERROR: Timed out moving off initial switch!");
      stepper->forceStop();
      stepper->setAcceleration(stepperAccelConfig);
      stepper->setSpeedInHz(stepperSpeedConfig);
      homingState = HOMING_ERROR;
      homed = false;
    }
    break;

  case HOMING_FIND_OTHER_END:
    // Wait for interrupt or timeout
    if (currentTime - homingStateTime >= 500) {  // Print status every 500ms
      // Position (not just a dot) - see the comment in HOMING_FIND_INITIAL.
      Serial.print(". pos=");
      Serial.print(stepper->getCurrentPosition());
      Serial.print(" speed=");
      Serial.println(stepper->getCurrentSpeedInMilliHz() / 1000);
      homingStateTime = currentTime;
      homingCounter++;

      if (homingCounter > 60) {  // 30 second timeout
        Serial.println("\nERROR: Timed out finding other end!");
        stepper->forceStop();
        stepper->setAcceleration(stepperAccelConfig);
        stepper->setSpeedInHz(stepperSpeedConfig);
        homingState = HOMING_ERROR;
        homed = false;
        return;
      }
    }

    if (interruptTriggered) {
      Serial.println();
      // Capture the position right away, at the moment the interrupt is
      // noticed - accurate to the actual trigger point. bottomPosition
      // must be computed from this now, not after waiting to settle below
      // (residual coasting would move it further before it's read).
      int endPosition = stepper->getCurrentPosition();
      Serial.print("Found other end at position ");
      Serial.println(endPosition);
      bottomPosition = endPosition / 2;
      interruptTriggered = false;
      homingState = HOMING_SETTLE_AFTER_OTHER_END;
      homingStateTime = currentTime;
    }
    break;

  case HOMING_SETTLE_AFTER_OTHER_END:
    // Wait for the interrupt-triggered forceStop() to actually finish -
    // see the comment in HOMING_SETTLE_AFTER_INITIAL for why.
    printWaitClearDiag(currentTime);
    if (!stepper->isRunning()) {
      Serial.println("Moving off switch...");
      stepper->move(-2500);  // Move backward 2500 steps
      tmcResetStallRampTimer();
      homingState = HOMING_MOVE_OFF_OTHER_END;
      homingStateTime = currentTime;
    } else if (currentTime - homingStateTime >= 2000) {  // shouldn't take long - safety net
      Serial.println("ERROR: Timed out waiting for stop to settle after finding other end!");
      stepper->forceStop();
      stepper->setAcceleration(stepperAccelConfig);
      stepper->setSpeedInHz(stepperSpeedConfig);
      homingState = HOMING_ERROR;
      homed = false;
    }
    break;

  case HOMING_MOVE_OFF_OTHER_END:
    // Wait for move to complete and switch to clear
    printWaitClearDiag(currentTime);
    if (!stepper->isRunning() && digitalRead(homingSwitchPin) == LOW) {
      Serial.println("Switch cleared, returning to home position");
      stepper->setAcceleration(stepperAccelConfig);
      stepper->setSpeedInHz(stepperSpeedConfig);
      stepper->moveTo(0);
      tmcResetStallRampTimer();
      homingState = HOMING_RETURN_TO_ZERO;
      homingStateTime = currentTime;
    } else if (currentTime - homingStateTime >= 10000) {  // 10 second timeout
      Serial.println("ERROR: Timed out moving off other end switch!");
      stepper->forceStop();
      stepper->setAcceleration(stepperAccelConfig);
      stepper->setSpeedInHz(stepperSpeedConfig);
      homingState = HOMING_ERROR;
      homed = false;
    }
    break;

  case HOMING_RETURN_TO_ZERO:
    // Wait for stepper to reach position 0
    // Check if stepper has stopped moving (more reliable than checking position exactly)
    if (!stepper->isRunning()) {
      int currentPos = stepper->getCurrentPosition();
      // Allow small tolerance (within a few steps of zero)
      if (abs(currentPos) <= 5) {
        Serial.println("Stepper is homed.");
        Serial.print("Final position: ");
        Serial.println(currentPos);
        homed = true;
        homingState = HOMING_COMPLETE;
      } else {
        // Stepper stopped but not at zero, try again
        Serial.print("Stepper stopped at ");
        Serial.print(currentPos);
        Serial.println(", moving to zero again...");
        stepper->moveTo(0);
        tmcResetStallRampTimer();
        homingStateTime = currentTime;  // Reset timeout
      }
    } else if (currentTime - homingStateTime >= 30000) {  // 30 second timeout
      Serial.println("ERROR: Timed out returning to zero!");
      Serial.print("Final position: ");
      Serial.println(stepper->getCurrentPosition());
      stepper->forceStop();
      homingState = HOMING_ERROR;
      homed = false;
    }
    break;

  case HOMING_COMPLETE:
    // Homing successful, return to idle
    homingState = HOMING_IDLE;
    break;

  case HOMING_ERROR:
    // Error occurred, return to idle
    homingState = HOMING_IDLE;
    break;

  default:
    break;
  }
}
