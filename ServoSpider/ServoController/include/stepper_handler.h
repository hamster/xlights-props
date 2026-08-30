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
// 20000 (matching normalAccel) plus a nonzero jumpStartConfig (see
// stepper_handler.cpp) tested clean across multiple homing cycles, including
// back-to-back immediate re-homes, which is exactly the scenario that
// reliably reproduced the stall at the old value.
#define stepperAccelHoming 20000
#define stepperSpeed 6500
#define stepperSpeedHoming 6000

// Homing states
enum HomingState {
  HOMING_IDLE,
  HOMING_CHECK_SWITCH,
  HOMING_MOVE_OFF_FORWARD,
  HOMING_MOVE_OFF_BACKWARD,
  HOMING_WAIT_CLEAR_SWITCH,
  HOMING_FIND_INITIAL,
  HOMING_MOVE_OFF_INITIAL,      // Move off the switch after finding initial position
  HOMING_FIND_OTHER_END,
  HOMING_MOVE_OFF_OTHER_END,    // Move off the switch after finding other end
  HOMING_RETURN_TO_ZERO,
  HOMING_COMPLETE,
  HOMING_ERROR
};

// Stepper global variables
extern FastAccelStepperEngine engine;
extern FastAccelStepper *stepper;
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

// Stepper configuration variables
extern int stepperSpeedConfig;
extern int stepperAccelConfig;
extern int jumpStartConfig;
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
  TRACK_MODE_LOOKAHEAD = 3   // Like Direct - still dispatches via moveTo(), the same well-tested path
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
};
extern int stepperTrackModeConfig;

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
void startStepCheck(long targetPosition);  // Begins the move; refuses if homing/checking/moving already
void updateStepCheck();                    // Call from loop(), before updateHoming()

// Stepper functions
void IRAM_ATTR handleHomingInterrupt();
void startHoming();             // Start non-blocking homing
void updateHoming();            // Call from loop() to update homing state
bool isHoming();                // Returns true if homing is in progress
bool isHomingSwitchTripped();   // Returns true if homing switch is currently triggered
void initializeStepper();

#endif
