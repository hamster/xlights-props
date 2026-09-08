#ifndef STEPPER_H
#define STEPPER_H

#include <Arduino.h>
#include "FastAccelStepper.h"

// Stepper pin definitions
#define stepperDirectionPin D2
#define stepperEnablePin D3
#define stepperStepPin D1
#define homingSwitchPin D10

// Stepper default values
#define stepperAccel 20000
// Was 1000000 (reaches 4000Hz cruise in ~4ms - functionally an instant jump
// to speed, not a ramp). Confirmed on the bench (2026-08-30) to intermittently
// stall the motor right at the start of a homing move - buzzes in place for
// a moment, sometimes self-recovers, sometimes needs a manual nudge or grinds
// indefinitely (the step counter keeps incrementing fictitiously while the
// motor isn't actually turning, since it has no way to detect a stall).
// 20000 (matching normalAccel) plus a nonzero JUMP_START_STEPS (see below)
// tested clean across multiple homing cycles, including back-to-back
// immediate re-homes, which is exactly the scenario that reliably
// reproduced the stall at the old value.
#define stepperAccelHoming 20000
#define stepperSpeed 6500
#define stepperSpeedHoming 6000

// Ramp step to jump to from standstill (FastAccelStepper's setJumpStart()):
// one deliberately larger first step (speed = sqrt(2*accel*jump_step), so 20
// steps at stepperAccelHoming=20000 gives roughly an 894Hz starting kick)
// instead of ramping from true zero - added 2026-08-30 after bench testing
// found the motor could intermittently stall right at the very first step of
// a move (buzzes in place briefly, then self-recovers or needs a nudge), a
// classic stepper starting-torque symptom a lower cruise acceleration alone
// didn't fully fix.
//
// Was a Settings-UI/NVS-configurable "Jump Start" field until 2026-09-07,
// when a Wave 4 UI-cleanup pass hardcoded it: disabling it (0) at
// homeAccel=20000 showed no observed difference across 3 back-to-back
// homing cycles (see TODO.md), and it was never independently tested for
// Direct-mode's ordinary DDP moveTo() calls either - not worth a Settings
// field for a value nobody has shown changes behavior. Kept non-zero rather
// than dropped to 0 purely because that's what's been running without
// incident; revisit in code, not the UI, if it's ever worth re-testing.
#define JUMP_START_STEPS 20

// Homing states. Simplified 2026-08-30 from an earlier 14-state version that
// inserted a fixed-distance "move off the switch, wait, re-verify clear"
// detour between finding each end and starting the next search/return. That
// detour was never actually necessary - once the switch has tripped and
// we're reversing direction, we're moving away from it by definition, and
// the trigger is a RISING edge that physically can't fire again until the
// far end. It also directly caused a real, reported bug: a visible pause
// right after moving off the switch, since the detour was itself a discrete
// move that fully decelerated to a stop before the next command was issued.
// Removing it fixes that pause at the root instead of tuning around it, and
// matches the mechanical procedure as actually described (find initial ->
// immediately reverse and count -> find other end -> half the count is the
// bottom -> return to zero), with no separate "clear the switch" leg in
// between except the one genuinely needed at boot if the switch already
// reads triggered.
enum HomingState {
  HOMING_IDLE,
  HOMING_CHECK_SWITCH,
  HOMING_CLEAR_STUCK_SWITCH,  // Switch already reads triggered at boot, so which direction actually
                              // moves off it is unknown - try one direction with a short timeout, and
                              // if it doesn't clear, try the other; if neither clears, the switch
                              // itself is stuck (HOMING_ERROR). One continuous slow run per direction,
                              // not a series of tiny incremental moves.
  HOMING_FIND_INITIAL,        // Search toward the switch (runBackward()) until it trips.
  HOMING_SETTLE,              // Wait for a genuine stop - forceStop() isn't instantaneous, so
                               // starting the next move immediately can still be fighting the tail
                               // end of the previous move's deceleration. Reused for both ends;
                               // homingSettleAction says what to do once actually stopped.
  HOMING_FIND_OTHER_END,       // Reverse (runForward()) and search until the switch trips again.
  HOMING_RETURN_TO_ZERO,
  HOMING_COMPLETE,
  HOMING_ERROR
};

// What HOMING_SETTLE should do once the motor has genuinely come to a stop -
// it's reused at every "come to a stop, then do something specific" point
// in the sequence, which each need a different next action.
enum HomingSettleAction {
  SETTLE_THEN_CLEAR_BACKWARD,   // Forward didn't clear a stuck-at-boot switch - try backward instead.
  SETTLE_THEN_FIND_INITIAL,     // Cleared a stuck switch (or it wasn't stuck) - search for the initial position.
  SETTLE_THEN_FIND_OTHER_END,   // Just found the initial position - reverse and search for the other end.
  SETTLE_THEN_RETURN_TO_ZERO,   // Just found the other end - bottomPosition is known, return to 0.
};
extern HomingSettleAction homingSettleAction;

// Stepper global variables
extern FastAccelStepperEngine engine;
extern FastAccelStepper *stepper;
// Which way a continuous-run (runForward()/runBackward(), NOT moveTo())
// tracking mode currently intends to be moving: 0 = no continuous run in
// flight (moveTo()-based motion, or genuinely idle), 1 = forward commanded,
// -1 = backward commanded. Set by TRACK_MODE_STREAMING and TRACK_MODE_PID
// (main.cpp) immediately alongside every runForward()/runBackward() call,
// and cleared back to 0 whenever that continuous run ends (forceStop(),
// settling into moveTo() at the deadband, or leaving the mode).
//
// Exists so updateHoming()'s switch-trip "was this just contact bounce"
// filter has something reliable to check for continuous-run motion.
// stepper->targetPos() - the filter's primary signal - is not kept
// meaningful by FastAccelStepper during a "keep running" continuous move
// (see updateHoming()'s own comment), so without this, every legitimate
// bounce edge on liftoff from the switch (departing position 0, which is
// exactly the case a continuous-run mode has to depart from repeatedly
// under normal DDP operation) got misread as a genuine trip and
// forceStop()'d the move again within one tick of it starting - it could
// never actually get away from the switch. Found on the bench (2026-09-06)
// via TRACK_MODE_PID's very first real test: commanded to depart position 0
// for a large positive target, but the motor never reached measurable
// speed, the switch never cleared, and updateRammedIntoStopCheck() (working
// exactly as designed) caught the resulting "stuck at the switch" condition.
extern volatile int continuousRunDirection;
extern volatile bool interruptTriggered;
// Set by tmc_handler.cpp's stall-detection cutoff when a real stall (motor
// commanded to move but the rotor isn't actually turning, per TMC2209
// StallGuard) happens *during* an active homing search - distinct from
// pendingForceStop, which is specifically about the homing switch. Added
// after a real incident (2026-08-30): a homing search ran the stepper
// continuously for 30+ seconds at a perfectly steady commanded speed
// without ever triggering the switch - the motor was jammed against the
// mechanical stop the whole time, but nothing detected it (the switch
// wasn't tripping, and the old stall check explicitly excluded homing).
// Consumed unconditionally near the top of updateHoming(), same pattern as
// pendingForceStop, so a stall aborts the current search immediately
// (HOMING_ERROR) instead of grinding until the state's own ~30s timeout.
extern volatile bool homingStallDetected;
extern int bottomPosition;
extern bool homed;
extern HomingState homingState;
extern unsigned long homingStateTime;
extern int homingCounter;
// Latches true whenever a homing attempt ends in HOMING_ERROR (stuck switch,
// stall, timeout, etc.) - HOMING_ERROR itself is transient (updateHoming()
// falls through it back to HOMING_IDLE the same/next loop iteration), so
// this is what the web UI actually reads to show a persistent "Homing
// Error" indicator instead of silently reverting to a plain "Not Homed"
// the instant the error state is left. Cleared at the start of every fresh
// startHoming() attempt (isHoming() covers the UI during the attempt
// itself); set in updateHoming()'s HOMING_ERROR case, so every error path
// in the state machine is covered from one place instead of needing to be
// set at each individual "homingState = HOMING_ERROR" site.
extern bool homingErrorLatched;

// Real physical speed limit, one-way (0-100%) - derived from the last
// completed HOMING_FIND_OTHER_END leg (the continuous run from the initial
// switch trip at position 0 to the second trip at endPosition, covering the
// same "full down-and-up round trip" homingOverallTimeoutMs's reference
// point is based on - stepper_handler.cpp), halved. That leg is one
// continuous run at constant commanded speed with no stop/reversal at its
// own midpoint (the trolley's direction change there is a passive
// mechanical consequence of the rope re-wrapping, not anything the stepper
// does), so halving both its distance and elapsed time is a well-justified
// one-way figure, not the round trip itself. Meant to answer "how fast can
// this prop actually move between two positions" for whoever is timing
// cues to music - see TODO.md. Persists across homing attempts (not
// cleared at startHoming()) so the web UI can keep showing the last known
// figure; homingTravelValid is false only before the very first successful
// homing since boot.
extern bool homingTravelValid;
extern long homingTravelSteps;         // one-way (0-100%) distance - same value as bottomPosition
extern unsigned long homingTravelMs;   // one-way (0-100%) wall-clock elapsed time

// Stepper configuration variables
extern int stepperSpeedConfig;
extern int stepperAccelConfig;
extern bool autoHomeOnBootConfig;
extern int stepperSpeedHomingConfig;   // Homing speed, Hz - tune per microstep setting (6000 tuned for 16 microsteps)
extern int stepperAccelHomingConfig;   // Homing acceleration, steps/s^2

// Small-move "tracking" profile: used instead of stepperSpeedConfig/
// stepperAccelConfig when a new position command is within
// stepperTrackThresholdConfig steps of the current position, so a stream
// of small incremental DDP updates (e.g. xLights slowly panning a value)
// blends into smooth continuous motion instead of a torque-spiking
// accelerate/decelerate cycle repeated on every packet. See updateHoming-
// adjacent position-handling code in main.cpp's loop().
extern bool stepperTrackEnabledConfig;
extern int stepperTrackThresholdConfig;  // steps - deltas at/below this use the tracking profile
extern int stepperTrackSpeedConfig;      // Hz
extern int stepperTrackAccelConfig;      // steps/s^2 - keep well below stepperAccelConfig; peak torque
                                          // demand at the start of a move scales with this, not with
                                          // the (lower) target speed, so it must be independently gentle
                                          // or the motor can stall/skip steps starting from near-rest.
// The tracking-vs-normal decision is based on the size of the *commanded*
// increment (new target vs. previous target), not on how far the stepper's
// actual position currently is from that target - using actual-position
// distance caused a spurious burst of full-speed motion at the ends of
// travel, where a lagging stepper (still catching up in the old direction
// as DDP starts commanding the reverse direction) sees a large gap to the
// new target even though each individual DDP increment is still small.
// This is a separate, much larger safety threshold: if the stepper's
// actual position ever falls this far behind the commanded target
// (genuine sustained lag, not a momentary reversal artifact), the normal
// profile is used regardless, to resync rather than drift indefinitely.
extern int stepperTrackMaxLagConfig;     // steps

// Which motion strategy handles DDP position updates. All three still
// respect stepperTrackThresholdConfig/stepperTrackMaxLagConfig to pick
// tracking vs. normal speed/accel; they differ in *how* new targets get
// committed to the stepper. Added after bench data showed the jerkiness
// wasn't really an accel/speed tuning problem: moveTo() always plans to
// decelerate to a full stop at whatever target it's given, and since a
// tracking-mode target is only ~60-120 steps further than the last one,
// the motor is almost always within its own stopping distance of the
// current target - so it's constantly "in the process of stopping,"
// never truly cruising, regardless of how gentle the accel is tuned.
enum StepperTrackMode {
  TRACK_MODE_DIRECT = 0,     // Current/original behavior: moveTo(new target) on every DDP packet.
  TRACK_MODE_COALESCE = 1,   // Batch several close updates into one less-frequent, larger moveTo(),
                              // so each move has real distance to accelerate through before planning a stop.
  TRACK_MODE_STREAMING = 2,  // While updates keep arriving, run continuously (runForward/runBackward) at a
                              // speed estimated from the recent rate of DDP change, instead of aiming to
                              // stop at each tiny target; snaps to an exact moveTo() once updates go quiet.
                              // Clamped to [0, bottomPosition] every step as a hard safety net - a rate-
                              // based estimate has no built-in "never overshoot" guarantee the way
                              // moveTo() does, confirmed by simulation showing exactly this failure mode.
                              // SHELVED (2026-08-30): repeatedly stalled/froze the trolley on the bench
                              // across two separate confirmed-and-fixed bugs, plus a third unresolved
                              // instability (the stepper command queue appearing to wedge for 15+ seconds
                              // at a time) that survived both fixes. Not recommended - see TODO.md.
  TRACK_MODE_LOOKAHEAD = 3,  // Like Direct - still dispatches via moveTo(), the same well-tested path
                              // Direct/Coalesce use, not Streaming's separate runForward()/runBackward()
                              // path - but instead of aiming at the literal commanded position, aims at
                              // that position plus stepperLookaheadStepsConfig further in the current
                              // direction of travel (clamped to [0, bottomPosition], so moveTo() itself
                              // never overshoots - no external clamp-and-forceStop() safety net needed,
                              // unlike Streaming). Because the target is always artificially far ahead,
                              // the ramp generator has no reason to plan a decelerate-to-stop while
                              // updates keep arriving - it just keeps accelerating/cruising. Snaps to an
                              // exact moveTo() at the true commanded position once updates go quiet
                              // (stepperLookaheadSettleMsConfig), same idea as Streaming's settle, without
                              // Streaming's separate code path or its unresolved instability.
  TRACK_MODE_PID = 4         // Added 2026-09-06, replacing the discrete normal/tracking-profile switch
                              // with real closed-loop control: error = commanded position (read fresh
                              // every tick from positionRequest, not cached) minus getCurrentPosition()
                              // drives a PID loop whose output is the stepper's target speed
                              // (magnitude+direction), applied via the same continuous
                              // runForward()/runBackward()/applySpeedAcceleration() plumbing
                              // TRACK_MODE_STREAMING already uses (including its hard-won fixes: wait
                              // for forceStop() to actually finish before restarting; re-apply
                              // speed/accel every tick since FastAccelStepper only picks up new values
                              // on the next move/moveTo/runForward/runBackward/applySpeedAcceleration()
                              // call). Snaps to an exact moveTo() once |error| <= stepperPidDeadbandConfig,
                              // rather than needing a "gone quiet" timer the way Streaming/Lookahead do -
                              // PID's own error naturally shrinks to that point as it converges, it
                              // doesn't need to infer "probably done" from elapsed time.
                              //
                              // Because it reads positionRequest directly every tick instead of only
                              // reacting to a *new* DDP value, this mode is structurally immune to the
                              // forceStop()-then-stuck bug documented in TODO.md's 2026-09-06 entry
                              // (stepper->targetPos() staleness after a switch trip or stall during
                              // normal operation) - any forceStop(), from whatever cause, just shows up
                              // as "not moving yet" on the very next tick, and PID recomputes fresh
                              // error and resumes on its own; no separate corrective moveTo() needed.
                              //
                              // Deliberately does NOT use the encoder for feedback - only
                              // getCurrentPosition() (this device's own step-pulse bookkeeping), so it
                              // works identically on every real device, not just the tuning bench where
                              // an encoder happens to be wired (the encoder is bench-only ground truth
                              // for verifying tuning results, never read by the controller itself).
                              //
                              // Current state (2026-09-07, see TODO.md for the full tuning history):
                              // a velocity feedforward (self-measures the real rate positionRequest has
                              // been changing at, via a least-squares slope over a fixed time window -
                              // stepperPidFfWindowMsConfig) carries the bulk of the commanded motion, so
                              // Kp/Kd only trim the residual error rather than driving reactively from
                              // scratch. The control tick itself is configurable
                              // (stepperPidTickMsConfig, default 5ms) - found to be the most effective
                              // lever against a persistent ~120-150ms-period speed ripple, since it's a
                              // sampled-control-loop dynamic rather than a noise source any output filter
                              // could remove. Logging cadence is deliberately independent of tick rate
                              // (stepperPidLogMsConfig) so a fast control tick doesn't overflow the
                              // Compact Motion Log's ring buffer. Still measurably less smooth
                              // moment-to-moment than TRACK_MODE_DIRECT, in exchange for meaningfully
                              // tighter tracking and corner accuracy - see the user's own framing
                              // (TODO.md, 2026-09-07): a real prop can tolerate a few frames of lag but
                              // must never look jerky, which is what this mode is tuned against.
};
extern int stepperTrackModeConfig;

// TRACK_MODE_PID parameters - see that enum value's comment for the control
// law. Kp/Kd are floats (unlike every other tunable here) - $SET/$GET
// special-case both, see tuning_handler.cpp.
//
// There is deliberately no Ki (removed 2026-09-07): it was never once set
// nonzero in any bench session, so iTerm was always exactly 0 while its
// anti-windup guard cost a saturation check and an accumulator every tick.
// Integral action has no obvious job here either - the deadband's
// moveTo() snap already closes any steady-state P/D gap, and feedforward
// now supplies the bulk velocity that an integrator would otherwise have
// had to wind up to. Re-add it deliberately (with a real sustained-
// tracking test) if a systematic bias ever shows up; don't restore it
// speculatively.
extern float stepperPidKpConfig;      // Hz per step of error (proportional gain)
extern float stepperPidKdConfig;      // Hz per (step/second) of measured-position rate (derivative gain, applied to -d(measured)/dt)
// Weight (0-1) in the per-tick EMA that smooths the raw derivative-on-
// measurement rate before it drives dTerm - see updatePidMode()'s own
// declaration comment (main.cpp) for the exact formula and history. A
// per-SAMPLE weight, not per-unit-time, so its effective smoothing time
// constant (tau = -tickMs / ln(1-w)) scales inversely with
// stepperPidTickMsConfig - the two are coupled even though they're
// separate tunables. Lower = more smoothing (longer tau) at a given tick
// rate; 1.0 = no filtering (raw derivative passes straight through).
extern float stepperPidDFilterWeightConfig;
// ms - minimum interval between updatePidMode() ticks (main.cpp gates on
// this instead of a hardcoded 20). A sampled control loop's phase margin
// generally improves at a faster sample rate for the same continuous-time
// gains, which is a real, untried lever against the ~120-150ms speed
// ripple characterized in stepperPidKpConfig's declaration comment - unlike
// Kp, it doesn't ask for less gain, it asks the loop to react sooner.
// dt is still computed from real elapsed time each call (self-correcting
// if a tick occasionally overruns), so this is purely a floor, not a fixed
// period. Two real, disclosed interactions to watch when lowering this:
// stepperPidDFilterWeightConfig's EMA weight is per-SAMPLE, not per unit
// time, so a faster tick shortens its effective time constant (weaker
// filtering per wall-clock second) unless retuned - see that tunable's
// own declaration comment; and the Compact Motion Log's row rate rises
// proportionally, shrinking how much wall-clock time the 96KB ring buffer
// can hold before wrapping.
extern int stepperPidTickMsConfig;
// ms - minimum interval between Compact Motion Log rows written from
// updatePidMode()'s active-tracking log calls. Independent of
// stepperPidTickMsConfig on purpose (see logCompactMotionPidThrottled's
// declaration comment, main.cpp) - tying it to the tick rate silently
// hides exactly the improvement a faster tick is meant to show, since
// jerk/ripple analysis works from consecutive logged rows. Default 20ms
// keeps the original ~37s safe capture window at the default tick rate;
// lower it (down to matching pidTickMs) for a short diagnostic capture
// that needs full control-rate resolution.
extern int stepperPidLogMsConfig;
extern int stepperPidMaxSpeedConfig;  // Hz - hard clamp on PID output magnitude
extern int stepperPidAccelConfig;     // Hz/s - ramp rate FastAccelStepper uses when the PID output speed changes; the value the planned acceleration-characterization sweep is meant to inform
extern int stepperPidDeadbandConfig;  // steps - |error| at or below this snaps to an exact moveTo() and stops driving via PID, instead of continuing to output a tiny, chattery nonzero speed forever
// steps - hysteresis band around a settled target: while already settled
// (pidSettled), a new target within this radius gets another one-shot
// moveTo() snap instead of dropping back into full continuous-run PID
// control. Must be >= stepperPidDeadbandConfig to do anything. Added
// 2026-09-06: DDP's 8-bit position quantization can shift the computed
// target by tens to (on a large-bottomPosition device) over a hundred
// steps between adjacent commanded values - right at position 0 (always
// switch-triggered, and a completely normal DDP endpoint, not just a
// homing reference), that was enough to repeatedly kick pidSettled back to
// false, re-engaging continuous-run mode (runForward()/runBackward()) for
// a correction of only a few tens of steps - each re-engagement produced a
// small amount of real motion right at the switch, which
// updateRammedIntoStopCheck() correctly read as "stuck against the stop"
// even though nothing was actually wrong. Found via the first full
// DDP-triangle-wave test against PID mode - see TODO.md.
extern int stepperPidReengageThresholdConfig;
// Time-budget-aware velocity feedforward toggle (2026-09-07) - see
// updatePidMode()'s feedforward block (main.cpp) for the full design.
// Kept toggleable so it stays A/B-testable against pure reactive PID with
// the same sweep tooling used for every other change this session.
extern bool stepperPidFeedforwardConfig;

// ms - width of the fixed time window the velocity feedforward measures
// the target's rate over. The feedforward estimate is the dominant jerk
// source in the loop, and this is its smoothing knob: longer = smoother
// commanded speed, at the cost of roughly half the window in added lag
// following a genuine rate change (i.e. corner tightness). See
// updatePidMode()'s feedforward block (main.cpp) for why a window beats
// the EMA-over-consecutive-changes approach it replaced.
extern int stepperPidFfWindowMsConfig;

// ms - width of a plain moving-average window smoothing the target the
// P-term reacts to (NOT the same thing as TRACK_MODE_LOOKAHEAD below,
// which is a different mode entirely - this is PID-specific, hence the
// name). 0 = off (react to the raw, instantaneous target, original
// behavior). See updatePidMode()'s pTermTarget block (main.cpp) for the
// full design and why this is safe in a way the shelved pidSmoothTarget
// layer wasn't (no synthetic forward integration, so nothing to drift
// while paused - only ever averages real, already-received samples).
// Trades roughly half this value in added latency for a smoother
// reference; spend deliberately against the frame-lag budget.
extern int stepperPidLookaheadMsConfig;

// TRACK_MODE_LOOKAHEAD parameters
extern int stepperLookaheadStepsConfig;     // steps - how far beyond the commanded position to aim, in the
                                              // current direction of travel. Must exceed the worst-case
                                              // stopping distance at the tracking profile's speed/accel
                                              // (speed^2 / (2*accel)) or the ramp generator can still catch
                                              // up to the extended target and plan a decel anyway.
extern int stepperLookaheadSettleMsConfig;  // ms - quiet period (no new DDP command) before snapping to
                                              // the exact final commanded position

// TRACK_MODE_COALESCE parameters
extern int stepperCoalesceMsConfig;     // ms - minimum time between committed moves
extern int stepperCoalesceStepsConfig;  // steps - accumulated delta that forces an early commit

// TRACK_MODE_STREAMING parameters
extern int stepperStreamRateWindowMsConfig;  // ms - window for estimating rate of DDP position change
extern int stepperStreamSettleMsConfig;      // ms - quiet period (no new DDP command) before snapping to the exact final position

// Set by handleSaveStepper() instead of writing to flash immediately, since
// a Preferences write briefly disables the flash cache and FastAccelStepper's
// step-generation interrupt fires continuously while moving - see
// persistStepperSettingsIfPending().
extern bool stepperSettingsPendingSave;
void persistStepperSettingsIfPending();  // Call every loop() iteration

// Skipped-step check: a deliberate diagnostic move (bypassing DDP/tracking-
// mode entirely) toward a known physical reference point, watching whether
// the homing switch fires *before* the step counter gets there. This is the
// only real ground-truth signal available for detecting lost steps -
// getCurrentPosition() is pulse-counting bookkeeping, not a position sensor,
// so it has no way to notice a step that was commanded but didn't physically
// happen. If earlier motion lost steps in the direction that moves *away*
// from this reference, the counter will have over-counted that travel, so
// commanding a return to the reference overshoots it physically before the
// counter's own idea of "there" is reached - the switch trips early. See
// startStepCheck()/updateStepCheck() in stepper_handler.cpp and the
// $CHECKSTEPS command in tuning_handler.cpp.
enum StepCheckState {
  STEPCHECK_IDLE,
  STEPCHECK_MOVING,
};
extern StepCheckState stepCheckState;
extern bool stepCheckTripped;        // Result of the most recently completed check
extern long stepCheckTripPosition;   // Counter value at the moment of an early trip, or final position if clean
extern long stepCheckTargetPosition; // What we asked it to move to
extern unsigned long stepCheckElapsedMs;

bool isStepChecking();
// Begins the move; refuses if homing/checking/moving already. autoRehomeOnTrip
// (default false, so $CHECKSTEPS's existing bench-diagnostic behavior is
// unchanged) - if true and this check finds real drift, a fresh homing
// cycle starts automatically instead of just marking homed=false and
// waiting for something else to notice. See handleVerifyAndRehome()
// (html_handler.cpp) - the /verify-and-rehome endpoint FPP/DDP scripting
// can call remotely to self-heal drift with a single request.
void startStepCheck(long targetPosition, bool autoRehomeOnTrip = false);
void updateStepCheck();                    // Call from loop(), before updateHoming()

// Stepper functions
void IRAM_ATTR handleHomingInterrupt();
void startHoming();             // Start non-blocking homing
void updateHoming();            // Call from loop() to update homing state
bool isHoming();                // Returns true if homing is in progress
bool isHomingSwitchTripped();   // Returns true if homing switch is currently triggered
void initializeStepper();

// Detects being physically rammed into the homing stop during *normal*
// (non-homing) operation - deliberately independent of the encoder, since
// getCurrentPosition() alone already shows the right signature: it's pure
// step-pulse bookkeeping, so it keeps changing even when the motor is
// mechanically blocked and nothing is really moving. If the switch has
// been continuously triggered for RAMMED_DETECT_MS and the step count has
// moved since the trip started, that's a real jam - the motor's still
// being driven, but the physical stop is denying it further travel. Call
// from loop() every iteration; no-op unless the switch is currently
// tripped. See stepper_handler.cpp for what it does once detected.
void updateRammedIntoStopCheck();

#endif
