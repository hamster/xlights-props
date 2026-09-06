#include "stepper_handler.h"
#include "tmc_handler.h"
#include "encoder_handler.h"
#include "persist_log.h"
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
bool homingErrorLatched = false;
HomingState homingState = HOMING_IDLE;
unsigned long homingStateTime = 0;
int homingCounter = 0;
HomingSettleAction homingSettleAction = SETTLE_THEN_FIND_INITIAL;

bool homingTravelValid = false;
long homingTravelSteps = 0;
unsigned long homingTravelMs = 0;
// When the HOMING_FIND_OTHER_END leg actually started (runForward() issued
// out of HOMING_SETTLE) - see homingTravelMs's declaration comment.
static unsigned long otherEndSearchStartMs = 0;

// Overall homing watchdog (2026-08-30 simplification, replacing a pile of
// separate 10s/30s/2s per-state timeouts that could stack to 90+ seconds
// worst case before erroring out - exactly what happened during a real
// bench session). Reference point, from the actual hardware: at 6500 Hz the
// trolley makes a full down-and-up round trip in about 5 seconds, so ~15s
// (3x that) at 6500 Hz is a reasonable "something is wrong" cutoff. A
// slower configured homing speed takes proportionally longer to cover the
// same physical distance, so the timeout scales inversely with speed
// (computed fresh in startHoming(), from whatever stepperSpeedHomingConfig
// actually is - not the compiled default) rather than staying a fixed
// constant that would false-trip at a deliberately gentler homing speed.
static const float HOMING_TIMEOUT_REFERENCE_SPEED_HZ = 6500.0f;
static const unsigned long HOMING_TIMEOUT_REFERENCE_MS = 15000;
static const unsigned long HOMING_TIMEOUT_MIN_MS = 8000;  // floor, in case speed is configured very high
static unsigned long homingOverallTimeoutMs = HOMING_TIMEOUT_REFERENCE_MS;
static unsigned long homingStartTime = 0;

// Short, fixed budget for clearing a switch that already reads triggered at
// boot - this is a tiny fraction of full travel (a handful of seconds at
// most), not related to a full round-trip, so it isn't scaled the way the
// main search watchdog above is.
static const unsigned long HOMING_CLEAR_STUCK_TIMEOUT_MS = 2000;

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
  // Explicitly pinned to Core 1 (was plain engine.init() - unpinned,
  // xTaskCreate() with tskNO_AFFINITY, left to the scheduler). FastAccelStepper
  // runs its own background task here (queue/ramp-refill housekeeping - the
  // actual step pulses are hardware/MCPWM-generated, independent of any
  // core) and, on this chip, its MCPWM+PCNT backend installs its own raw
  // PCNT interrupt via esp_intr_alloc() too - both core-affine to wherever
  // this function runs, which is setup(), Core 1. Making that explicit
  // rather than implicit, and keeping it on Core 1 alongside loop()/DDP/web,
  // now that Core 0 is being used deliberately for other things (see
  // core0_task.h) instead of left to chance.
  engine.init(1);
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
  homingErrorLatched = false;  // Give this fresh attempt a clean slate - isHoming() covers the UI meanwhile
  homingState = HOMING_CHECK_SWITCH;
  homingStateTime = millis();
  homingStartTime = homingStateTime;
  homingCounter = 0;
  interruptTriggered = false;
  pendingForceStop = false;  // Defensive: never let a stale flag from before this attempt fire mid-sequence

  stepper->setAcceleration(stepperAccelHomingConfig);
  stepper->setSpeedInHz(stepperSpeedHomingConfig);

  // Scale the overall watchdog to the speed we're actually about to home
  // at - see the comment above homingOverallTimeoutMs.
  float speedHz = (stepperSpeedHomingConfig > 0) ? (float)stepperSpeedHomingConfig : HOMING_TIMEOUT_REFERENCE_SPEED_HZ;
  homingOverallTimeoutMs = (unsigned long)(HOMING_TIMEOUT_REFERENCE_MS * (HOMING_TIMEOUT_REFERENCE_SPEED_HZ / speedHz));
  if (homingOverallTimeoutMs < HOMING_TIMEOUT_MIN_MS) homingOverallTimeoutMs = HOMING_TIMEOUT_MIN_MS;
  Serial.print("Homing watchdog: ");
  Serial.print(homingOverallTimeoutMs);
  Serial.print(" ms (homing speed ");
  Serial.print(stepperSpeedHomingConfig);
  Serial.println(" Hz)");
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

static unsigned long lastHomingDiagPrintMs = 0;

// Retry throttle, shared by every homing state that waits on a continuous
// runForward()/runBackward() search (HOMING_CLEAR_STUCK_SWITCH,
// HOMING_FIND_INITIAL, HOMING_FIND_OTHER_END) - see retryMoveIfDied()'s
// comment for what this is working around. Reset (to currentTime) at every
// place that issues a fresh runForward()/runBackward() for a new search,
// so a retry can't fire on the very same iteration the original request
// was just made. Sharing one variable across all three states is safe -
// they're mutually exclusive within a single homing attempt.
static unsigned long lastRunRetryMs = 0;

// FastAccelStepper's own ramp generator has, more than once now on the
// bench (2026-09-01), silently abandoned a just-accepted runForward()/
// runBackward() request before it ever filled the step queue or produced a
// single step: MOVE_OK was returned and isRunning() read true immediately
// after the call, but isRampGeneratorActive() then cleared itself on its
// own within ~100-200ms with the queue still completely empty
// (isQueueEmpty() stayed true throughout) - and with no forceStop() call
// anywhere in this codebase's own logging involved (confirmed via the
// qRunning/qEmpty/rampActive breakdown added for exactly this
// investigation). This looks like a genuine FastAccelStepper-internal
// timing issue, not anything decided at the application level - not
// something fixable from here. First seen only in HOMING_CLEAR_STUCK_SWITCH,
// but then hit HOMING_FIND_OTHER_END's runForward() too on the very next
// real reproduction, burning the entire ~24s overall watchdog before
// erroring out (that state had no comparable recovery) - so this is
// shared by every state that issues a continuous run, not special-cased to
// just one of them.
//
// Detects the dead signature as isRunning()==false while the caller still
// expects to be actively running (i.e., not because anything here asked it
// to stop) and just retries the identical runForward()/runBackward() call,
// throttled to once per 100ms via lastRunRetryMs, rather than silently
// burning a much longer timeout waiting on a move that's already dead -
// this resolves in well under 200ms typically. Returns true if a retry was
// just issued.
static bool retryMoveIfDied(bool forward, unsigned long currentTime) {
  if (stepper->isRunning() || (currentTime - lastRunRetryMs < 100)) {
    return false;
  }
  lastRunRetryMs = currentTime;
  Serial.println("Move died before producing any motion - retrying...");
  if (forward) {
    stepper->runForward();
  } else {
    stepper->runBackward();
  }
  tmcResetStallRampTimer();
  return true;
}

// Shared periodic position/speed print for every homing state that's
// waiting on something (a search interrupt, a genuine stop, a switch
// clearing) - added (2026-08-30) to directly see symptoms like "pauses
// partway through, then continues" or a real stall rather than guessing at
// them. Uses its own timer, separate from homingStateTime (which these
// states also use for their own timeouts).
static void printHomingDiag(unsigned long currentTime) {
  if (currentTime - lastHomingDiagPrintMs < 100) return;
  lastHomingDiagPrintMs = currentTime;
  Serial.print("  [homing] state=");
  Serial.print((int)homingState);
  Serial.print(" pos=");
  Serial.print(stepper->getCurrentPosition());
  Serial.print(" speed=");
  Serial.print(stepper->getCurrentSpeedInMilliHz() / 1000);
  Serial.print(" running=");
  Serial.print(stepper->isRunning() ? 1 : 0);
  // isRunning() is just isQueueRunning() || isRampGeneratorActive() ||
  // !isQueueEmpty() under the hood - broken out here to see which of the
  // three actually flips when a CLEAR_STUCK_SWITCH move goes from running=1
  // (confirmed immediately after runForward()/runBackward()) to running=0
  // a moment later with no forceStop() call anywhere in this codebase's own
  // logging (2026-09-01) - narrows down whether the ramp generator itself
  // is stopping, or the queue is (somehow) never getting filled.
  Serial.print(" qRunning=");
  Serial.print(stepper->isQueueRunning() ? 1 : 0);
  Serial.print(" qEmpty=");
  Serial.print(stepper->isQueueEmpty() ? 1 : 0);
  Serial.print(" rampActive=");
  Serial.println(stepper->isRampGeneratorActive() ? 1 : 0);
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

    // A trip while normal (non-homing) motion is headed *away* from the
    // switch can't be a genuine new arrival - moving away and arriving
    // somewhere are contradictory. Found on the bench (2026-08-30):
    // commanding a move away while resting right on the switch immediately
    // killed the move, because a normal DDP-commanded departure from that
    // position is exactly the case where switch contact bounce
    // (make-break-make as the actuator lifts off) is most likely, and every
    // one of those bounces is a RISING edge the ISR can't help but catch.
    //
    // Checked two ways, either one enough to call it a bounce:
    //  - stepper->targetPos() vs. tripPosition: where the move is *headed*,
    //    known the instant it's issued regardless of ramp state. This is
    //    the primary signal - a bounce right at the very start of a move
    //    (as seen on the bench: tripPosition just a few steps from where
    //    the move began) can fire before getCurrentSpeedInMilliHz() has
    //    any measurable speed to report yet, so target alone already
    //    catches what speed alone missed.
    //  - getCurrentSpeedInMilliHz() > 0: kept as a secondary check for the
    //    (probably rare) case where target isn't meaningful for some other
    //    reason but real motion is already measurably underway.
    // Restricted to non-homing operation - an active homing search must
    // still stop on any trip regardless of direction, since detecting the
    // trip *is* the search, and targetPos() isn't kept updated during a
    // continuous run() search anyway (see FastAccelStepper.h's own note on
    // "keep running" mode).
    if (!isHoming() && (stepper->targetPos() > tripPosition || stepper->getCurrentSpeedInMilliHz() > 0)) {
      Serial.print("Switch interrupt while moving away from switch (pos=");
      Serial.print(tripPosition);
      Serial.println(") - treating as contact bounce, not a real trip; move not stopped");
      return;
    }

    stepper->forceStop();
    if (homingState == HOMING_CLEAR_STUCK_SWITCH) {
      // The switch is already known HIGH throughout this state (that's why
      // we're here) - a RISING edge firing again this soon is suspicious,
      // not a real "found the far end" event. Cause not confirmed - could
      // be a spurious/noisy edge on the switch line, or a genuine stall
      // (right at the hard stop, starting torque demand can be higher than
      // normal, e.g. against switch-actuator preload). Seen once on the
      // bench (2026-08-30); flagged explicitly here instead of only being
      // inferable from the move never showing any speed in the diagnostic
      // log, so a recurrence gives more to go on.
      Serial.print("NOTE: Switch interrupt fired again during stuck-switch clearing (pos=");
      Serial.print(tripPosition);
      Serial.println(") - not a real trip (switch already known triggered). The just-issued move was force-stopped before it could start; this attempt will likely time out and retry the other direction.");
    }
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
      // HOMING_CLEAR_STUCK_SWITCH's own "try forward, then backward, then
      // give up" logic (see that case body) previously only fired on a
      // *timeout* (2s with no trigger) - a real stall is typically caught
      // by TMC2209 StallGuard in well under a second, so it almost always
      // won this race and fell straight through to the unconditional abort
      // below, never letting the second direction get tried at all (found
      // 2026-09-02, reasoning through a real report: "we'll end up stuck
      // on the far end of the pulley" if the first direction we happen to
      // try is the one that jams against it). Treat a stall on the first
      // attempt exactly like a timeout - try the other direction - and
      // only actually conclude something is genuinely wrong once *both*
      // directions have failed, whether by stall or timeout.
      if (homingState == HOMING_CLEAR_STUCK_SWITCH && homingCounter == 0) {
        Serial.println("Stall detected trying to clear the switch forward - trying backward...");
        stepper->forceStop();
        homingSettleAction = SETTLE_THEN_CLEAR_BACKWARD;
        homingState = HOMING_SETTLE;
        homingStateTime = millis();
        return;
      }
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

  // Periodic breadcrumb to the reboot-surviving diagnostic log - added
  // 2026-09-06 after a real bench session where a homing search
  // apparently got stuck for well over a minute (StallGuard disabled to
  // get clean tuning data, real mechanical resistance with nothing left to
  // catch it) with no way afterward to tell whether it had crashed, hung,
  // or was reset by something as mundane as a new serial connection
  // opening - see persist_log.h's file comment. Coarse (every 2s), not a
  // replacement for the live serial homing prints - just enough of a trail
  // to see where a stuck search actually was if the live session is gone
  // by the time anyone looks.
  static unsigned long lastHomingBreadcrumbMs = 0;
  if (isHoming() && (currentTime - lastHomingBreadcrumbMs >= 2000)) {
    lastHomingBreadcrumbMs = currentTime;
    persistLog("homing state=%d pos=%ld speed=%ld switch=%d", (int)homingState,
               stepper->getCurrentPosition(), (long)(stepper->getCurrentSpeedInMilliHz() / 1000),
               isHomingSwitchTripped() ? 1 : 0);
  }

  // Single overall homing watchdog - see homingOverallTimeoutMs's comment.
  // Replaces what used to be six separate per-state timeouts (10s/30s/2s
  // each) that could stack to 90+ seconds worst case.
  if (isHoming() && (currentTime - homingStartTime >= homingOverallTimeoutMs)) {
    Serial.print("ERROR: Homing timed out after ");
    Serial.print(currentTime - homingStartTime);
    Serial.println(" ms - aborting");
    stepper->forceStop();
    stepper->setAcceleration(stepperAccelConfig);
    stepper->setSpeedInHz(stepperSpeedConfig);
    homingState = HOMING_ERROR;
    homed = false;
    return;
  }

  switch (homingState) {
  case HOMING_CHECK_SWITCH:
    // Check if switch is already triggered at startup
    // Based on actual hardware: HIGH = switch triggered, LOW = switch not triggered
    Serial.print("CHECK_SWITCH: digitalRead=");
    Serial.print(digitalRead(homingSwitchPin) == HIGH ? "HIGH" : "LOW");
    Serial.print(" pos=");
    Serial.println(stepper->getCurrentPosition());
    if (digitalRead(homingSwitchPin) == HIGH) {
      Serial.println("Homing switch triggered at bootup, clearing it...");
      homingCounter = 0;  // 0 = trying forward first, 1 = trying backward (retry)
      // runForward()/runBackward() return a MoveResultCode (silently
      // discarded at every other call site in this file too) - logged here
      // since this exact transition has twice now produced zero motion for
      // the full CLEAR_STUCK_SWITCH timeout with no other explanation
      // (2026-08-31/09-01) - if the ramp generator itself refused the move
      // (MOVE_ERR_SPEED_IS_UNDEFINED / MOVE_ERR_ACCELERATION_IS_UNDEFINED /
      // MOVE_ERR_NO_DIRECTION_PIN), this is the only place that would show it.
      MoveResultCode runResult = stepper->runForward();
      Serial.print("runForward() result=");
      Serial.print((int)runResult);
      Serial.print(" isRunning=");
      Serial.print(stepper->isRunning() ? 1 : 0);
      Serial.print(" qRunning=");
      Serial.print(stepper->isQueueRunning() ? 1 : 0);
      Serial.print(" qEmpty=");
      Serial.print(stepper->isQueueEmpty() ? 1 : 0);
      Serial.print(" rampActive=");
      Serial.println(stepper->isRampGeneratorActive() ? 1 : 0);
      tmcResetStallRampTimer();
      homingState = HOMING_CLEAR_STUCK_SWITCH;
      homingStateTime = currentTime;
      lastRunRetryMs = currentTime;
    } else {
      // Switch is clear, go directly to finding initial position
      Serial.println("Moving to find initial homing position...");
      homingState = HOMING_FIND_INITIAL;
      interruptTriggered = false;
      stepper->runBackward();
      tmcResetStallRampTimer();
      homingStateTime = currentTime;
      lastRunRetryMs = currentTime;
    }
    break;

  case HOMING_CLEAR_STUCK_SWITCH:
    // Which direction actually moves off a switch that's already triggered
    // at boot isn't known in advance, so try one direction with a short
    // timeout, then the other, then give up - one continuous slow run per
    // direction (not a series of tiny incremental moves; that was more
    // complicated than this needs to be).
    printHomingDiag(currentTime);
    if (digitalRead(homingSwitchPin) == LOW) {
      Serial.println("Switch cleared.");
      stepper->forceStop();
      homingSettleAction = SETTLE_THEN_FIND_INITIAL;
      homingState = HOMING_SETTLE;
      homingStateTime = currentTime;
    } else if (currentTime - homingStateTime >= HOMING_CLEAR_STUCK_TIMEOUT_MS) {
      if (homingCounter == 0) {
        Serial.println("Didn't clear forward, trying backward...");
        stepper->forceStop();
        homingSettleAction = SETTLE_THEN_CLEAR_BACKWARD;
        homingState = HOMING_SETTLE;
        homingStateTime = currentTime;
      } else {
        Serial.println("ERROR: Homing switch stuck - didn't clear in either direction!");
        stepper->forceStop();
        stepper->setAcceleration(stepperAccelConfig);
        stepper->setSpeedInHz(stepperSpeedConfig);
        homingState = HOMING_ERROR;
        homed = false;
      }
    } else {
      retryMoveIfDied(homingCounter == 0, currentTime);
    }
    break;

  case HOMING_FIND_INITIAL:
    // Search toward the switch. Wait for the interrupt; the overall
    // watchdog above covers the "never triggers" case, and TMC2209 stall
    // detection (when enabled) catches a genuine jam almost instantly.
    printHomingDiag(currentTime);
    if (interruptTriggered) {
      Serial.println("Found initial homing position");
      // Safe to relabel the pulse count immediately - setCurrentPosition()
      // is just bookkeeping, not a motion command, so it doesn't need to
      // wait for anything. The pendingForceStop consumption block above
      // has already called forceStop() this same loop() iteration.
      stepper->setCurrentPosition(0);
      interruptTriggered = false;
      homingSettleAction = SETTLE_THEN_FIND_OTHER_END;
      homingState = HOMING_SETTLE;
      homingStateTime = currentTime;
    } else {
      // Always searches backward - see retryMoveIfDied()'s declaration
      // comment for why this check exists.
      retryMoveIfDied(false, currentTime);
    }
    break;

  case HOMING_FIND_OTHER_END:
    // Reverse and search until the switch trips again.
    printHomingDiag(currentTime);
    if (interruptTriggered) {
      // Capture the position right away, at the moment the interrupt is
      // noticed - accurate to the actual trigger point. bottomPosition
      // must be computed from this now, not after waiting to settle below
      // (residual coasting would move it further before it's read).
      int endPosition = stepper->getCurrentPosition();
      Serial.print("Found other end at position ");
      Serial.println(endPosition);
      bottomPosition = endPosition / 2;

      // Real physical speed limit, wanted as a one-way 0-100% figure (what
      // whoever is timing cues to music actually needs), not the full
      // down-and-up round trip this leg physically covers. This whole leg
      // is one continuous, unbroken runForward() at a constant commanded
      // speed - the trolley's direction reverses at the midpoint as a
      // passive consequence of the rope re-wrapping on the pulley, but the
      // stepper itself never stops, decelerates, or changes speed there -
      // so distance and elapsed time both split evenly at the midpoint,
      // and halving both of this leg's totals is a well-justified
      // approximation of the one-way figure, not a rough guess. (The small
      // ramp-up at the very start of the leg falls entirely within the
      // first half either way, so it doesn't skew this any more than it
      // already would.)
      unsigned long fullLegMs = currentTime - otherEndSearchStartMs;
      homingTravelSteps = endPosition / 2;  // same value as bottomPosition, computed the same way
      homingTravelMs = fullLegMs / 2;
      homingTravelValid = true;
      if (homingTravelMs > 0) {
        Serial.print("One-way (0-100%) travel: ");
        Serial.print(homingTravelSteps);
        Serial.print(" steps in ");
        Serial.print(homingTravelMs);
        Serial.print(" ms (~");
        Serial.print((float)homingTravelSteps * 1000.0f / (float)homingTravelMs, 0);
        Serial.println(" steps/s average)");
      }

      interruptTriggered = false;
      homingSettleAction = SETTLE_THEN_RETURN_TO_ZERO;
      homingState = HOMING_SETTLE;
      homingStateTime = currentTime;
    } else {
      // Always searches forward - see retryMoveIfDied()'s declaration
      // comment for why this check exists. This is the state that first
      // showed the dead-move bug can strike outside HOMING_CLEAR_STUCK_SWITCH
      // too (2026-09-01) - it burned the entire ~24s overall watchdog
      // before this retry existed here.
      retryMoveIfDied(true, currentTime);
    }
    break;

  case HOMING_SETTLE:
    // Wait for a genuine stop before issuing the next move -
    // forceStop() isn't instantaneous, and issuing the next command
    // immediately used to assume the motor was already at rest when it
    // could still be coasting/decelerating from the previous move. That
    // produced a real, visible decelerate-reverse-reaccelerate "pause"
    // once stepperAccelHomingConfig was lowered enough (from the earlier
    // stall fix) to make it perceptible instead of instantaneous
    // (2026-08-30). homingSettleAction (set by whichever state transitioned
    // in) says what to actually do once stopped.
    printHomingDiag(currentTime);
    if (!stepper->isRunning()) {
      switch (homingSettleAction) {
      case SETTLE_THEN_CLEAR_BACKWARD: {
        Serial.println("Trying backward...");
        homingCounter = 1;
        MoveResultCode runResult = stepper->runBackward();
        Serial.print("runBackward() result=");
        Serial.print((int)runResult);
        Serial.print(" isRunning=");
        Serial.print(stepper->isRunning() ? 1 : 0);
        Serial.print(" qRunning=");
        Serial.print(stepper->isQueueRunning() ? 1 : 0);
        Serial.print(" qEmpty=");
        Serial.print(stepper->isQueueEmpty() ? 1 : 0);
        Serial.print(" rampActive=");
        Serial.println(stepper->isRampGeneratorActive() ? 1 : 0);
        tmcResetStallRampTimer();
        homingState = HOMING_CLEAR_STUCK_SWITCH;
        lastRunRetryMs = currentTime;
        break;
      }
      case SETTLE_THEN_FIND_INITIAL:
        Serial.println("Searching for initial position...");
        interruptTriggered = false;
        stepper->runBackward();
        tmcResetStallRampTimer();
        homingState = HOMING_FIND_INITIAL;
        lastRunRetryMs = currentTime;
        break;
      case SETTLE_THEN_FIND_OTHER_END:
        Serial.println("Searching for other end...");
        stepper->runForward();
        tmcResetStallRampTimer();
        homingState = HOMING_FIND_OTHER_END;
        lastRunRetryMs = currentTime;
        otherEndSearchStartMs = currentTime;  // start of the full round-trip timing leg
        break;
      case SETTLE_THEN_RETURN_TO_ZERO:
        Serial.println("Returning to home position...");
        stepper->setAcceleration(stepperAccelConfig);
        stepper->setSpeedInHz(stepperSpeedConfig);
        stepper->moveTo(0);
        tmcResetStallRampTimer();
        homingState = HOMING_RETURN_TO_ZERO;
        break;
      }
      homingStateTime = currentTime;
    } else if (currentTime - homingStateTime >= 2000) {  // shouldn't take long - safety net
      Serial.println("ERROR: Timed out waiting for stop to settle!");
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
        // Resync the encoder's zero reference to the stepper's freshly-
        // confirmed position 0, now that it's actually trusted - otherwise
        // the encoder count keeps drifting from whatever it happened to
        // read at boot (or after a prior, possibly-imperfect homing).
        resetEncoderCount();
        homingState = HOMING_COMPLETE;
      } else {
        // Stepper stopped but not at zero, try again
        Serial.print("Stepper stopped at ");
        Serial.print(currentPos);
        Serial.println(", moving to zero again...");
        stepper->moveTo(0);
        tmcResetStallRampTimer();
      }
    }
    break;

  case HOMING_COMPLETE:
    // Homing successful, return to idle
    homingState = HOMING_IDLE;
    break;

  case HOMING_ERROR:
    // Error occurred, return to idle. Latch it first - this state itself is
    // transient (falls through to IDLE the same/next iteration), but the
    // web UI needs something persistent to show "Homing Error" instead of
    // silently reverting to a plain "Not Homed" the instant this is left.
    homingErrorLatched = true;
    homingState = HOMING_IDLE;
    break;

  default:
    break;
  }
}
