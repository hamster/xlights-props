#include "stepper_handler.h"
#include <Preferences.h>

extern Preferences preferences;

// Stepper global variables
FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *stepper = NULL;
volatile bool interruptTriggered = false;
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
int jumpStartConfig = 0;
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
  // than a silently-discarded one: the trolley shouldn't reach the switch
  // outside of homing at all, so if it does, something has drifted -
  // stop and require a fresh re-home rather than pretending nothing
  // happened.
  if (pendingForceStop) {
    pendingForceStop = false;
    stepper->forceStop();
    if (!isHoming() && homed) {
      Serial.println("WARNING: Homing switch tripped outside of homing - stopping and marking system as not homed");
      homed = false;
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
        homingState = HOMING_WAIT_CLEAR_SWITCH;
        homingStateTime = currentTime;
      } else if (homingCounter < 25) {
        // Still on switch (pin still HIGH), keep moving forward
        stepper->moveTo(10 * homingCounter);
        homingCounter++;
        homingStateTime = currentTime;
      } else {
        // Forward didn't work, try backward
        Serial.println("Didn't clear forward, trying backward...");
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
        homingState = HOMING_WAIT_CLEAR_SWITCH;
        homingStateTime = currentTime;
      } else if (homingCounter < 25) {
        // Still on switch (pin still HIGH), keep moving backward
        stepper->moveTo(-10 * homingCounter);
        homingCounter++;
        homingStateTime = currentTime;
      } else {
        // Couldn't clear switch
        Serial.println("ERROR: Homing switch stuck!");
        stepper->forceStop();
        stepper->setAcceleration(stepperAccelConfig);
        stepper->setSpeedInHz(stepperSpeedConfig);
        homingState = HOMING_ERROR;
        homed = false;
      }
    }
    break;

  case HOMING_WAIT_CLEAR_SWITCH:
    // Wait for the move-off-switch movement to complete
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
      Serial.print(".");
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
      // Motor is already stopped by interrupt
      stepper->setCurrentPosition(0);
      interruptTriggered = false;

      // Move off the switch before reversing
      Serial.println("Moving off switch...");
      stepper->move(2500);  // Move forward 2500 steps
      homingState = HOMING_MOVE_OFF_INITIAL;
      homingStateTime = currentTime;
      homingCounter = 0;
    }
    break;

  case HOMING_MOVE_OFF_INITIAL:
    // Wait for move to complete and switch to clear
    if (!stepper->isRunning() && digitalRead(homingSwitchPin) == LOW) {
      Serial.println("Switch cleared, reversing...");
      stepper->runForward();
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
      Serial.print(".");
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
      int endPosition = stepper->getCurrentPosition();
      Serial.print("Found other end at position ");
      Serial.println(endPosition);

      // Motor is already stopped by interrupt
      bottomPosition = endPosition / 2;
      interruptTriggered = false;

      // Move off the switch before returning to center
      Serial.println("Moving off switch...");
      stepper->move(-2500);  // Move backward 2500 steps
      homingState = HOMING_MOVE_OFF_OTHER_END;
      homingStateTime = currentTime;
    }
    break;

  case HOMING_MOVE_OFF_OTHER_END:
    // Wait for move to complete and switch to clear
    if (!stepper->isRunning() && digitalRead(homingSwitchPin) == LOW) {
      Serial.println("Switch cleared, returning to home position");
      stepper->setAcceleration(stepperAccelConfig);
      stepper->setSpeedInHz(stepperSpeedConfig);
      stepper->moveTo(0);
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
