#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <ESPmDNS.h>
#include "main.h"
#include "version.h"
#include "stepper_handler.h"
#include "wifi_handler.h"
#include "html_handlers.h"
#include "ddp_handler.h"
#include "led_handler.h"
#include "partition_utils.h"
#include "protocol_common.h"
#include "tmc_handler.h"
#include "tuning_handler.h"
#include "encoder_handler.h"
#include "encoder_diag.h"
#include "core0_task.h"
#include "persist_log.h"

// Watchdog timeout in seconds
#define WDT_TIMEOUT 10

// Preferences for storing configuration
Preferences preferences;

// Variables for position tracking
uint16_t oldPositionRequest = 0;
float position = 0;
float lastCommandedTargetPosition = 0;  // Previous DDP-commanded target, steps - see tracking-profile note in loop()

// Blank time tracking
bool stepperBlanked = false;  // Track if stepper has been blanked

// --- TRACK_MODE_COALESCE state ---
unsigned long coalesceLastCommitMs = 0;
float coalescePendingTarget = 0;
bool coalesceHasPending = false;

// --- TRACK_MODE_STREAMING state ---
struct StreamHistoryPoint { unsigned long ms; float pos; };
const int STREAM_HISTORY_SIZE = 6;
StreamHistoryPoint streamHistory[STREAM_HISTORY_SIZE];
int streamHistoryCount = 0;
unsigned long lastDdpCommandMs = 0;
float streamRawTarget = 0;
bool streamSettled = true;
int streamCurrentDirection = 0;  // -1, 0, 1
unsigned long lastStreamUpdateMs = 0;

// --- TRACK_MODE_LOOKAHEAD state ---
float lookaheadRawTarget = 0;       // true commanded position (calcPosition() output) - NOT the
                                     // artificially-extended moveTo() target actually sent to the
                                     // stepper; this is what logging/metrics should show as "commanded"
int lookaheadDirection = 0;         // -1, 0, 1 - direction implied by the most recent real change in target
bool lookaheadSettled = true;
unsigned long lastLookaheadCommandMs = 0;

// --- TRACK_MODE_PID state --- (see that enum value's declaration comment, stepper_handler.h)
bool pidSettled = true;
unsigned long lastPidUpdateMs = 0;
float pidIntegral = 0;
long pidLastMeasuredPos = 0;   // for derivative-on-measurement, not derivative-on-error - see updatePidMode()
// Time-budget-aware velocity feedforward (2026-09-07 - see the user's own
// original framing: PID had no notion of how much time it actually has to
// get from one commanded point to the next, so it always drove at full
// reactive speed even for small moves paced by a real, much slower show
// timeline). Self-measures the real rate the incoming *target* has been
// changing at - not an assumed DDP fps - so it adapts to whatever a show
// was actually authored at (25fps, 40fps, anything) with nothing
// hardcoded. See updatePidMode()'s feedforward block for the full design.
float pidFeedforwardLastTarget = 0;        // target value as of the last observed real change
unsigned long pidFeedforwardLastChangeMs = 0;  // when that change was observed
float pidFeedforwardVelocity = 0;          // current feedforward speed estimate, steps/s (signed)
// If the target hasn't actually changed for this long, stop coasting on a
// stale rate estimate - a real gap this size (several multiples of any
// normal 25-40Hz DDP cadence) means the source paused or stopped, not that
// it's still moving at whatever rate was last measured.
static const unsigned long PID_FEEDFORWARD_MAX_GAP_MS = 200;
bool pidStopSettling = false;  // mirrors streamStopSettling - wait for a forceStop() to actually finish before restarting
int pidCurrentDirection = 0;   // -1, 0, 1
// Throttle for re-issuing runForward()/runBackward() after FastAccelStepper
// silently drops a "keep running" request without ever filling the step
// queue - see stepper_handler.cpp's retryMoveIfDied() for the first,
// already-proven instance of this (homing's continuous-run searches).
// Found here the hard way (2026-09-06): PID's first bench test reissued a
// fresh runForward() on every single 20ms tick with no throttle at all
// while dead, right after departing position 0 (still very close to the
// homing switch) - the ramp generator reported starting (isRunning()==true,
// isRampGeneratorActive()==true) immediately after every single call, yet
// was already dead again by the very next tick, for 1.5+ seconds straight,
// never once producing a real, sustained run (confirmed independently by
// the encoder - real, if small, physical motion happened, then genuinely
// stopped). A clean test starting well away from the switch converged
// correctly, isolating this to a cold continuous-run start right at the
// switch, not the PID control law itself - throttling retries the same way
// retryMoveIfDied() already does is the same proven fix, applied here.
// A second, related gap found later the same day (during a PID damping
// sweep): the retry condition itself was plain !isRunning(), which a
// dead-but-nonempty queue satisfies without producing any steps (same
// root cause as the hysteresis band's genuinelyRunning fix below) - so
// once direction stopped changing, a dead run was never retried at all,
// not just under-throttled. Fixed by using the same genuinelyRunning
// check here too - see updatePidMode()'s call site.
unsigned long lastPidRunRetryMs = 0;
// Latches "already issued the clean forceStop() for the direction change
// in progress" across the pidStopSettling wait (which re-enters
// updatePidMode() from the top on every tick while waiting - without this,
// the still-true directionChanged on the settled-and-continuing tick would
// just trigger another forceStop() forever, never actually reaching the
// runForward()/runBackward() call). Cleared the instant that call is
// actually issued - see updatePidMode()'s direction-change handling.
bool pidDirectionSwitchPending = false;
// Same throttled-retry pattern, for the hysteresis band's moveTo() calls
// (see stepperPidReengageThresholdConfig's declaration comment) instead of
// runForward()/runBackward(). Found necessary the same way, one level up
// (2026-09-06): a 16s triangle wave left the trolley frozen at position 0
// for 2+ seconds despite the commanded target moving smoothly away, then
// caught up in one big jump once the drift finally exceeded the whole
// reengage band. Direct evidence via temporary diagnostics: isRunning()
// read true (via a non-empty queue) while isRampGeneratorActive() was
// false and current speed was 0 - a moveTo() had gone "dead" the exact
// same way runForward()/runBackward() do, just via !isQueueEmpty() instead
// of isQueueRunning()/isRampGeneratorActive(). The isRunning()-only gate
// this branch used never re-issues in that state, since a dead-but-
// nonempty queue still reads as running.
unsigned long lastPidHystRetryMs = 0;

// Applies the tracking-vs-normal profile decision (see stepperTrackThresholdConfig's
// declaration comment) for a move toward newTarget, given the previous target this
// decision was based on. Updates FastAccelStepper's live speed/accel. Returns true
// if the tracking profile was used; writes the Hz that was applied to targetSpeedHzOut.
bool applyTrackingProfileDecision(float newTarget, float lastCommittedTarget, int& targetSpeedHzOut) {
  int curPos = stepper->getCurrentPosition();
  float commandDelta = fabs(newTarget - lastCommittedTarget);
  float lagFromActual = fabs(newTarget - (float)curPos);

  bool useTracking = stepperTrackEnabledConfig
                      && commandDelta > 0
                      && commandDelta <= stepperTrackThresholdConfig
                      && lagFromActual <= stepperTrackMaxLagConfig;

  targetSpeedHzOut = useTracking ? stepperTrackSpeedConfig : stepperSpeedConfig;
  if (useTracking) {
    stepper->setSpeedInHz(stepperTrackSpeedConfig);
    stepper->setAcceleration(stepperTrackAccelConfig);
  } else {
    stepper->setSpeedInHz(stepperSpeedConfig);
    stepper->setAcceleration(stepperAccelConfig);
  }
  return useTracking;
}

// In-RAM buffer for the Compact Motion Log, retrievable via GET
// /compact-log - see protocol_common.h's getCompactLog()/clearCompactLog()
// declaration comment for why this exists alongside (not instead of) the
// Serial output below: a real xLights/DDPDebugger session already talks to
// the device over the network, and opening a serial connection to watch
// the log would reset the ESP32 (DTR/RTS) and kill that session.
//
// Fixed-size ring buffer, NOT a growing/trimming String (2026-09-07,
// found the hard way): the original version appended to a String and,
// once over a byte cap, called substring() to drop the oldest lines -
// each of those was a full reallocation-and-copy of a buffer that can be
// well over 100KB. That was fine for the short bench sessions it was
// first tested against, but a real multi-minute DDPDebugger session
// (sustained appends, the cap getting hit and trimmed repeatedly) caused
// real, severe corruption - over half the bytes in one real capture came
// back as embedded NULs instead of the data that was actually logged.
// This ring buffer never reallocates after startup: writes wrap in
// place, oldest bytes are simply overwritten once full, no String
// concat/substring churn at all.
// 96KB - sized for a real, sustained DDPDebugger/xLights session, not just
// a short bench burst (2026-09-07, found the hard way): at ~50 lines/sec
// x ~52 bytes/line during active tracking, the original 32KB only held
// ~13s of continuous motion - a real multi-cycle test wrapped it and left
// only trailing idle time by the time it was fetched. RAM headroom checked
// (43% used before this change, comfortable margin at this size).
static char compactLogRing[98304];
static size_t compactLogHead = 0;  // next write position
static size_t compactLogLen = 0;   // valid bytes currently stored, <= sizeof(compactLogRing)

static void ringAppendChar(char c) {
  compactLogRing[compactLogHead] = c;
  compactLogHead = (compactLogHead + 1) % sizeof(compactLogRing);
  if (compactLogLen < sizeof(compactLogRing)) compactLogLen++;
}

static void appendCompactLog(const char* line) {
  for (const char* p = line; *p; p++) ringAppendChar(*p);
  ringAppendChar('\n');
}

String getCompactLog() {
  String out;
  out.reserve(compactLogLen + 1);  // one allocation, no reallocation during the loop below
  // Oldest byte is at compactLogHead once the ring has wrapped (full);
  // otherwise everything written so far starts at index 0.
  size_t startIdx = (compactLogLen < sizeof(compactLogRing)) ? 0 : compactLogHead;
  for (size_t i = 0; i < compactLogLen; i++) {
    out += compactLogRing[(startIdx + i) % sizeof(compactLogRing)];
  }
  return out;
}
void clearCompactLog() { compactLogHead = 0; compactLogLen = 0; }

// Temporary compact CSV motion log, independent of protocolDebugConfig - see
// compactLogEnabled's declaration comment in protocol_common.h. Shared by all
// three TRACK_MODE_* strategies so the log stays useful regardless of mode.
void logCompactMotion(uint16_t ddpVal, int cmdPos, int curPos, int delta, int lag,
                       bool tracking, int32_t curSpeedMilliHz, int targetSpeedHz) {
  if (!compactLogEnabled) return;
  // Built via a fixed char buffer + snprintf(), NOT chained String
  // concatenation (2026-09-06, found the hard way): the original version
  // built this line via ~12 chained `String + String + ...` operators,
  // each allocating its own temporary on the heap - fine occasionally, but
  // this fires every 20-50ms during any active tracking/homing motion, and
  // sustained heap churn at that rate caused a real, reproducible firmware
  // crash (a full reboot mid-homing-search) plus data corruption in the
  // in-RAM log buffer itself (embedded NUL bytes where a newline should
  // have been - almost certainly a failed/partial allocation under
  // fragmentation). Matches this codebase's own existing convention for
  // hot-path buffers - see handleStatusData()'s static statusDataBuffer.
  static char lineBuf[160];
  int n = snprintf(lineBuf, sizeof(lineBuf), "%lu,%u,%d,%d,%d,%d,%c,%ld,%d,%d,%d,%d",
                    millis(), ddpVal, cmdPos, curPos, delta, lag, tracking ? 'T' : 'N',
                    (long)(curSpeedMilliHz / 1000), targetSpeedHz, getEncoderCount(),
                    // Live TMC2209 StallGuard reading (updateTmc() refreshes this
                    // every STALL_POLL_INTERVAL_MS - see tmc_handler.cpp) - added
                    // 2026-09-06 after a real bench session showed repeated
                    // stall-detection trips during DDP tracking-mode motion (3 of
                    // 5 baseline runs lost real steps), with no way to see
                    // *when*, at what speed/position, or how close to the
                    // threshold SG_RESULT was running the rest of the time. 0 if
                    // TMC UART isn't connected, same "always 0, still parses"
                    // convention as encoderCount.
                    tmcStatus.stallGuardResult,
                    // Raw homing switch state - added 2026-09-06 after running
                    // with StallGuard disabled (it was making false-positive-
                    // driven interruptions worse than the real stalls it's meant
                    // to catch, fighting attempts to get clean tracking-mode
                    // data) caused a real ram into the physical homing stop with
                    // no cutoff to catch it. Not wired into any real-time safety
                    // logic here on purpose - the user's own suggested check
                    // (motion commanded, encoder not advancing, switch reads
                    // triggered = stuck against the stop) is exactly the kind of
                    // thing that leans on the encoder, which isn't meant to
                    // outlive this tuning phase - so it's done as post-hoc
                    // analysis in tuning_harness.py instead, off this one raw
                    // logged bit, not as new production firmware logic.
                    isHomingSwitchTripped() ? 1 : 0);
  if (n < 0) return;  // encoding error - drop this row rather than log garbage
  Serial.println(lineBuf);
  appendCompactLog(lineBuf);
}

// Periodic compact-log tick, independent of the DDP/tracking-mode dispatch
// below - Direct and Coalesce only log once at the moment a move is
// committed, so a single large move (a big DDP jump, or a diagnostic
// moveTo() like $CHECKSTEPS's that never goes through this dispatch at
// all) previously produced only one data point instead of a full
// speed-over-time trace. Streaming and PID modes already log every ~20ms
// on their own (updateStreamingMode()/updatePidMode()), so this is skipped
// for both to avoid duplicate/conflicting rows. Call every loop()
// iteration; rate-limited internally.
unsigned long lastPeriodicLogMs = 0;
void logCompactMotionPeriodic() {
  if (!compactLogEnabled || stepperTrackModeConfig == TRACK_MODE_STREAMING ||
      stepperTrackModeConfig == TRACK_MODE_PID) return;
  if (stepper == NULL || !stepper->isRunning()) return;
  unsigned long now = millis();
  if (now - lastPeriodicLogMs < 50) return;
  lastPeriodicLogMs = now;

  int curPos = stepper->getCurrentPosition();
  // TRACK_MODE_LOOKAHEAD's actual stepper target is artificially extended
  // past the true commanded position (see StepperTrackMode's declaration
  // comment) - show the real commanded position here instead, or every
  // plot/metric derived from this log would show a commanded curve that
  // jumps ahead of and disagrees with what was actually asked for.
  int cmdPos = (stepperTrackModeConfig == TRACK_MODE_LOOKAHEAD)
                   ? (int)lookaheadRawTarget
                   : (int)stepper->targetPos();
  // Heuristic, display-only: tracking and normal accel are expected to
  // differ (that's the whole point of the tracking profile), so use
  // acceleration rather than speed to tell them apart - trackSpeed and
  // normalSpeed are commonly configured equal (bench-validated default),
  // which would make a speed-based comparison ambiguous.
  bool tracking = (stepper->getAcceleration() == (uint32_t)stepperTrackAccelConfig);
  logCompactMotion(oldPositionRequest, cmdPos, curPos, 0, cmdPos - curPos,
                    tracking, stepper->getCurrentSpeedInMilliHz(),
                    (int)(stepper->getSpeedInMilliHz() / 1000));
}

// TRACK_MODE_DIRECT: moveTo(newTarget) immediately, every DDP packet - the
// original/current behavior.
void handleDirectModeCommand(uint16_t ddpVal, float newTarget) {
  int curPosBeforeMove = stepper->getCurrentPosition();
  int32_t curSpeedBeforeMove = stepper->getCurrentSpeedInMilliHz();
  float commandDeltaForLog = fabs(newTarget - lastCommandedTargetPosition);
  float lagForLog = fabs(newTarget - (float)curPosBeforeMove);

  int targetSpeedHz;
  bool useTracking = applyTrackingProfileDecision(newTarget, lastCommandedTargetPosition, targetSpeedHz);
  lastCommandedTargetPosition = newTarget;

  if (protocolDebugConfig) {
    Serial.print("Moving to position ");
    Serial.print(ddpVal);
    Serial.print(" -> ");
    Serial.print((int)newTarget);
    Serial.println(useTracking ? " [tracking]" : " [normal]");
  }

  logCompactMotion(ddpVal, (int)newTarget, curPosBeforeMove, (int)commandDeltaForLog,
                    (int)lagForLog, useTracking, curSpeedBeforeMove, targetSpeedHz);

  stepper->moveTo((int)newTarget);
  stepperBlanked = false;
}

// TRACK_MODE_COALESCE: called every loop() iteration. Batches consecutive
// small DDP updates into one less-frequent, larger moveTo(), so each move
// has real distance to accelerate through before it needs to plan a stop -
// see stepperCoalesceMsConfig/stepperCoalesceStepsConfig's declaration.
void updateCoalesceMode() {
  if (!coalesceHasPending) return;

  unsigned long now = millis();
  bool timeElapsed = (now - coalesceLastCommitMs) >= (unsigned long)stepperCoalesceMsConfig;
  bool bigEnough = fabs(coalescePendingTarget - lastCommandedTargetPosition) >= stepperCoalesceStepsConfig;
  if (!timeElapsed && !bigEnough) return;

  int curPosBeforeMove = stepper->getCurrentPosition();
  int32_t curSpeedBeforeMove = stepper->getCurrentSpeedInMilliHz();
  float commandDeltaForLog = fabs(coalescePendingTarget - lastCommandedTargetPosition);
  float lagForLog = fabs(coalescePendingTarget - (float)curPosBeforeMove);

  int targetSpeedHz;
  bool useTracking = applyTrackingProfileDecision(coalescePendingTarget, lastCommandedTargetPosition, targetSpeedHz);
  lastCommandedTargetPosition = coalescePendingTarget;

  logCompactMotion(oldPositionRequest, (int)coalescePendingTarget, curPosBeforeMove,
                    (int)commandDeltaForLog, (int)lagForLog, useTracking, curSpeedBeforeMove, targetSpeedHz);

  stepper->moveTo((int)coalescePendingTarget);
  stepperBlanked = false;
  coalesceLastCommitMs = now;
  coalesceHasPending = false;
}

// TRACK_MODE_STREAMING: called every loop() iteration. While DDP updates
// keep arriving, runs continuously (runForward/runBackward) at a speed
// estimated from the recent rate of DDP position change, instead of
// aiming to stop at each tiny target; snaps to an exact moveTo() once
// updates go quiet for stepperStreamSettleMsConfig. The position clamp
// below is a hard safety net: a rate-based estimate has no built-in
// "never overshoot" guarantee the way moveTo() does - confirmed by
// simulation against real logged DDP data showing exactly this failure
// mode before this clamp was added.
// Set whenever this function calls forceStop() itself, so the restart logic
// below waits for that stop to genuinely finish before issuing a fresh
// runForward()/runBackward() - see the full explanation at the restart site.
static bool streamStopSettling = false;

void updateStreamingMode() {
  if (stepper->isRunning()) {
    int curPos = stepper->getCurrentPosition();
    // Strictly past the boundary, not at-or-past: sitting exactly at 0 or
    // bottomPosition is the normal resting position at either extreme (every
    // run starts there), not an overshoot. Using <=/>= here was confirmed on
    // the bench (2026-08-30) to force-stop the instant a move starts from
    // position 0. Only genuinely overshooting past either boundary should
    // trip the clamp.
    if (curPos < 0 || curPos > bottomPosition) {
      stepper->forceStop();
      streamStopSettling = true;
      continuousRunDirection = 0;
    }
  }

  unsigned long now = millis();
  if (now - lastStreamUpdateMs < 20) return;  // rate-limit this control loop
  lastStreamUpdateMs = now;

  unsigned long quietMs = now - lastDdpCommandMs;
  if (!streamSettled && quietMs >= (unsigned long)stepperStreamSettleMsConfig) {
    // Gone quiet - land exactly on the final commanded position.
    int curPosBeforeMove = stepper->getCurrentPosition();
    int32_t curSpeedBeforeMove = stepper->getCurrentSpeedInMilliHz();
    float commandDeltaForLog = fabs(streamRawTarget - lastCommandedTargetPosition);
    float lagForLog = fabs(streamRawTarget - (float)curPosBeforeMove);

    int targetSpeedHz;
    bool useTracking = applyTrackingProfileDecision(streamRawTarget, lastCommandedTargetPosition, targetSpeedHz);
    lastCommandedTargetPosition = streamRawTarget;

    logCompactMotion(oldPositionRequest, (int)streamRawTarget, curPosBeforeMove,
                      (int)commandDeltaForLog, (int)lagForLog, useTracking, curSpeedBeforeMove, targetSpeedHz);

    // Restore the configured jumpStart for this moveTo()-based settle - see
    // the runForward()/runBackward() call site below for why it's disabled
    // while actually streaming (same fix as TRACK_MODE_PID, same root cause).
    stepper->setJumpStart(jumpStartConfig);
    stepper->moveTo((int)streamRawTarget);
    stepperBlanked = false;
    streamSettled = true;
    streamCurrentDirection = 0;
    continuousRunDirection = 0;  // settling into moveTo() - not a continuous run anymore
    return;
  }

  if (streamSettled || streamHistoryCount < 2) return;  // nothing to stream yet

  StreamHistoryPoint &oldest = streamHistory[0];
  StreamHistoryPoint &newest = streamHistory[streamHistoryCount - 1];
  long dtMs = (long)(newest.ms - oldest.ms);
  float estRateHzSigned = (dtMs > 0) ? (newest.pos - oldest.pos) / (dtMs / 1000.0f) : 0;

  float maxTrackSpeed = (float)stepperTrackSpeedConfig;
  if (estRateHzSigned > maxTrackSpeed) estRateHzSigned = maxTrackSpeed;
  if (estRateHzSigned < -maxTrackSpeed) estRateHzSigned = -maxTrackSpeed;

  int newDirection = (estRateHzSigned > 5) ? 1 : (estRateHzSigned < -5 ? -1 : 0);
  uint32_t speedHz = (uint32_t)fabs(estRateHzSigned);
  if (speedHz < 50) speedHz = 50;  // floor so runForward/runBackward always gets a sane nonzero speed

  stepper->setAcceleration(stepperTrackAccelConfig);
  stepper->setSpeedInHz(speedHz);

  if (streamStopSettling) {
    // A forceStop() was issued (above, or below on a prior tick) and hasn't
    // been confirmed finished yet. FastAccelStepper's forceStop() is not
    // guaranteed complete within a single tick (the library tracks this
    // internally as an "incomplete immediate stop"), and re-issuing
    // runForward()/runBackward() before the ramp generator has actually
    // settled back to idle skips the ramp-up entirely - the new command
    // just continues whatever speed the generator's internal state still
    // reflects, commanding full target speed with no built-up momentum.
    // Confirmed on the bench (2026-08-30): this is what actually stalled
    // the motor ("moving too fast to get it started... no momentum built
    // up yet so it could not actually move"), not just a log/UI artifact.
    // So: once we've asked for a stop, wait for isRunning() to actually go
    // false before allowing any restart below.
    if (stepper->isRunning()) return;
    streamStopSettling = false;
  }

  if (newDirection != streamCurrentDirection || !stepper->isRunning()) {
    if (newDirection != 0) {
      // Root-caused on the bench (2026-09-06, via TRACK_MODE_PID): setJumpStart()'s
      // configured burst applies in the *wrong* direction when issued through
      // runForward()/runBackward() instead of moveTo() - disabled here, restored
      // for the moveTo()-based settle above.
      stepper->setJumpStart(0);
    }
    if (newDirection > 0) {
      stepper->runForward();
    } else if (newDirection < 0) {
      stepper->runBackward();
    } else {
      stepper->forceStop();
      streamStopSettling = true;
    }
    streamCurrentDirection = newDirection;
    continuousRunDirection = newDirection;  // see stepper_handler.h's declaration comment
  } else {
    // Already running in the same direction - per FastAccelStepper's own
    // docs, setSpeedInHz()/setAcceleration() above only take effect after
    // move/moveTo/runForward/runBackward/applySpeedAcceleration(), NOT on
    // their own. Without this, only the very first speed estimate (the one
    // in effect when runForward()/runBackward() was actually called) ever
    // reached the motor - confirmed on the bench (2026-08-30): actual speed
    // rode the ramp up to the configured max once and then never changed
    // again for the rest of a 12s run, ignoring every subsequent (lower)
    // rate estimate, producing a huge, growing position error.
    stepper->applySpeedAcceleration();
  }

  logCompactMotion(oldPositionRequest, (int)streamRawTarget, stepper->getCurrentPosition(), 0,
                    (int)fabs(streamRawTarget - stepper->getCurrentPosition()), true,
                    stepper->getCurrentSpeedInMilliHz(), (int)speedHz);
}

// TRACK_MODE_PID: called every loop() iteration - see that enum value's
// declaration comment (stepper_handler.h) for the full design. Reads
// positionRequest directly rather than being fed through the "only reacts
// to a new DDP value" dispatch switch the other modes use, so there's no
// separate "on command received" handler for this mode - this is the
// whole thing.
void updatePidMode() {
  if (stepper->isRunning()) {
    int curPos = stepper->getCurrentPosition();
    // Same hard safety net TRACK_MODE_STREAMING uses - a continuously-
    // driven PID output has no built-in "never overshoot" guarantee the
    // way moveTo() does.
    if (curPos < 0 || curPos > bottomPosition) {
      stepper->forceStop();
      pidStopSettling = true;
      continuousRunDirection = 0;
    }
  }

  unsigned long now = millis();
  if (now - lastPidUpdateMs < 20) return;  // rate-limit, matches Streaming's own cadence
  float dt = (lastPidUpdateMs == 0) ? 0.02f : (now - lastPidUpdateMs) / 1000.0f;
  lastPidUpdateMs = now;

  long currentPos = stepper->getCurrentPosition();
  float target = calcPosition(positionRequest, control16BitConfig);
  float error = target - (float)currentPos;

  // Feedforward measurement - runs every tick regardless of which branch
  // below ends up firing, so the rate estimate stays continuous and isn't
  // reset by time spent settled/hysteresis-snapping. Measures how far the
  // *target* actually moved between the last two real changes and over
  // how long, not how often packets arrive - correctly self-corrects for
  // 8-bit DDP quantization (a target that only advances once every 2-3
  // packets at 40fps still yields the right real-world rate, since both
  // the distance and the elapsed time reflect that).
  if (target != pidFeedforwardLastTarget) {
    if (pidFeedforwardLastChangeMs != 0) {
      unsigned long sinceLastChange = now - pidFeedforwardLastChangeMs;
      if (sinceLastChange > 0 && sinceLastChange <= PID_FEEDFORWARD_MAX_GAP_MS) {
        float instVelocity = (target - pidFeedforwardLastTarget) / (sinceLastChange / 1000.0f);
        // Light EMA - a single inter-change gap can be jittery (network
        // timing, or landing just before/after a quantization boundary);
        // average a few together rather than trusting one sample.
        pidFeedforwardVelocity = (pidFeedforwardVelocity == 0)
                                      ? instVelocity
                                      : (0.5f * pidFeedforwardVelocity + 0.5f * instVelocity);
      }
      // A gap longer than the timeout: leave pidFeedforwardVelocity as-is
      // for this tick (the "stale, zero it out" handling below covers
      // that) rather than computing a bogus instVelocity from it.
    }
    pidFeedforwardLastChangeMs = now;
    pidFeedforwardLastTarget = target;
  } else if (pidFeedforwardLastChangeMs != 0 &&
             now - pidFeedforwardLastChangeMs > PID_FEEDFORWARD_MAX_GAP_MS) {
    pidFeedforwardVelocity = 0;  // stale - source paused/stopped, don't keep coasting
  }

  if (fabs(error) <= (float)stepperPidDeadbandConfig) {
    if (!pidSettled) {
      int curPosBeforeMove = (int)currentPos;
      int32_t curSpeedBeforeMove = stepper->getCurrentSpeedInMilliHz();
      stepper->setAcceleration(stepperPidAccelConfig);
      // Restore the configured jumpStart for this moveTo()-based settle -
      // see the runForward()/runBackward() call site below for why it's
      // disabled while actually tracking.
      stepper->setJumpStart(jumpStartConfig);
      stepper->moveTo((int32_t)target);
      stepperBlanked = false;
      lastCommandedTargetPosition = target;
      logCompactMotion(positionRequest, (int)target, curPosBeforeMove, 0, (int)error,
                        true, curSpeedBeforeMove, 0);
      pidSettled = true;
      pidIntegral = 0;
      pidCurrentDirection = 0;
      // In sync with pidCurrentDirection's reset - if a pending direction
      // switch's forceStop()/settle got interrupted by the target landing
      // in the deadband before it finished, don't leave this latched true:
      // that would make the *next* real engagement skip its own clean
      // stop-and-settle (see pidDirectionSwitchPending's declaration
      // comment) since directionChanged && !pidDirectionSwitchPending
      // would read false.
      pidDirectionSwitchPending = false;
      continuousRunDirection = 0;  // settling into moveTo() - not a continuous run anymore
    } else {
      // Already settled, nothing to do motion-wise - but still log every
      // tick a real DDP value change occurs (2026-09-06), so the Compact
      // Motion Log's ddpVal trace stays a complete, gap-free record of
      // positionRequest, not just a sparse sample only taken when
      // something actually moves. Without this, a real DDP stream could
      // update positionRequest smoothly many times while sitting fully
      // idle here and none of it would appear in the log - the next row
      // logged (whenever error next exceeds deadband) would then show an
      // artificially large ddpVal jump that looks exactly like a dropped
      // packet or WiFi hiccup but is actually just a logging gap. Confirmed
      // this was happening on the bench: a run's log showed ~26% of ddpVal
      // transitions jumping by 2+ units, while a parallel protocolDebug
      // capture of the same kind of run showed zero dropped/out-of-order
      // packets at the firmware level - the "jumpiness" was 100% a logging
      // artifact, not a reception one.
      //
      // BUT throttled to at most once per 250ms when NOTHING changed
      // (2026-09-06, found the hard way): genuinely idle time - the
      // trolley sitting settled between shows, potentially for minutes -
      // was flooding the bounded GET /compact-log buffer (see its
      // declaration comment) at 50 lines/sec with nothing-happened rows,
      // evicting real motion data from an actual xLights session before
      // it could be fetched over HTTP. A real DDP value change still logs
      // immediately regardless of this throttle, so the gap-free guarantee
      // above is preserved - only true stretches of "nothing changed at
      // all" are now heartbeat-rate instead of full tick-rate.
      static uint16_t lastLoggedDdpVal = 0;
      static unsigned long lastIdleLogMs = 0;
      bool ddpChanged = (positionRequest != lastLoggedDdpVal);
      if (ddpChanged || (now - lastIdleLogMs >= 250)) {
        logCompactMotion(positionRequest, (int)target, (int)currentPos, 0, (int)error,
                          true, stepper->getCurrentSpeedInMilliHz(), 0);
        lastLoggedDdpVal = positionRequest;
        lastIdleLogMs = now;
      }
    }
    pidLastMeasuredPos = currentPos;
    return;
  }

  // Hysteresis band (2026-09-06 - see stepperPidReengageThresholdConfig's
  // declaration comment): a small target shift while already settled just
  // gets another one-shot moveTo() snap, not full re-engagement into
  // continuous-run mode.
  //
  // Only issues a fresh moveTo() once the *previous* one is no longer
  // genuinely running (see genuinelyRunning below) - an early version of
  // this called moveTo() unconditionally on every ~20ms tick while the
  // target crept through this band (e.g. a slow triangle wave departing
  // position 0), which kept resetting FastAccelStepper's ramp generator
  // before it ever built real speed - the exact "constantly re-aiming
  // never accelerates" jerkiness Direct mode is already known for, which
  // PID's continuous-run design exists to avoid; this band had
  // reintroduced it. A second version gated on plain !isRunning() (wait for
  // the previous move to finish, don't interrupt it) - better, but still
  // left the trolley stuck at position 0 for 2+ seconds on a slow wave:
  // the moveTo() itself can go "dead" (queue holds a request but the ramp
  // generator never actually starts producing steps) while still reading
  // isRunning()==true via a nonempty queue, so a plain isRunning() check
  // waits forever on a request that's never coming. genuinelyRunning below
  // catches that and retries (throttled) instead of waiting on it forever.
  //
  // A separate, rejected version compared the new target against
  // lastCommandedTargetPosition (only re-snapping if it had moved more
  // than a threshold) instead of an isRunning()-style gate - that created a
  // real dead zone (the trolley sitting still through however wide the
  // gate was) and measurably hurt tracking accuracy on the bench. This
  // fix doesn't have that problem: it always reacts to the *current*
  // target once idle/dead, it just doesn't interrupt a genuinely
  // in-flight move to do so.
  if (pidSettled && fabs(error) <= (float)stepperPidReengageThresholdConfig) {
    // A moveTo() can go "dead" the same way runForward()/runBackward() do
    // (see lastPidHystRetryMs's declaration comment) - isRunning() alone
    // doesn't detect it, since a dead-but-nonempty queue still satisfies
    // isRunning() via !isQueueEmpty(). Treat "running" here as genuinely
    // producing motion. NOTE (2026-09-06, found during a PID damping
    // sweep): originally also OR'd in isRampGeneratorActive() here, on
    // the theory that it was a second, independent way to catch real
    // motion - but a real 2.4s mid-run stall (a direction reversal that
    // never recovered despite this exact check) proved isRampGeneratorActive()
    // itself can report true for seconds at a stretch while actual speed
    // stays genuinely 0, making it worse than useless as a trust signal
    // here. getCurrentSpeedInMilliHz() alone has never been wrong in any
    // observed case - dropped the OR-clause.
    bool genuinelyRunning = stepper->isRunning() && stepper->getCurrentSpeedInMilliHz() != 0;
    if (!genuinelyRunning && (now - lastPidHystRetryMs >= 100)) {
      lastPidHystRetryMs = now;
      stepper->setAcceleration(stepperPidAccelConfig);
      stepper->setJumpStart(jumpStartConfig);
      stepper->moveTo((int32_t)target);
      stepperBlanked = false;
      lastCommandedTargetPosition = target;
    }
    // Log every tick regardless of whether a fresh moveTo() was actually
    // issued this time, so the Compact Motion Log's ddpVal trace stays a
    // complete, gap-free record of positionRequest at PID's own tick rate -
    // see the deadband-settled branch above for the full "this was
    // mistaken for a DDP reception problem" story.
    logCompactMotion(positionRequest, (int)target, (int)currentPos, 0, (int)error,
                      true, stepper->getCurrentSpeedInMilliHz(), 0);
    pidLastMeasuredPos = currentPos;
    return;  // stays settled - pidSettled untouched
  }

  pidSettled = false;

  if (pidStopSettling) {
    // See TRACK_MODE_STREAMING's identical wait - FastAccelStepper's
    // forceStop() isn't guaranteed complete within one tick; restarting
    // before it's actually settled skips the ramp-up entirely.
    //
    // Checks genuinely-still-running (speed != 0), not plain isRunning()
    // (2026-09-07, found live testing feedforward against real
    // DDPDebugger data): a dead-but-nonempty FastAccelStepper queue
    // satisfies isRunning() without producing any steps - the exact same
    // hazard already fixed for the continuous-run retry and hysteresis
    // band checks elsewhere in this function, just missed here. Caught a
    // real ~700ms stall exactly at the bottom-of-travel reversal this
    // way: curSpeedHz read exactly 0 the whole time (a real, complete
    // stop) while isRunning() apparently kept this wait blocked far past
    // the ~50ms a decel at stepperPidAccelConfig should take.
    bool genuinelyStillRunning = stepper->isRunning() && stepper->getCurrentSpeedInMilliHz() != 0;
    if (genuinelyStillRunning) return;
    pidStopSettling = false;
  }

  // Derivative on measurement, not on error - avoids a derivative "kick"
  // every time the DDP-commanded target jumps (which would otherwise look
  // like an enormous, instantaneous error derivative rather than a real
  // change in how fast the stepper itself is moving).
  float measuredRate = (dt > 0) ? (float)(currentPos - pidLastMeasuredPos) / dt : 0;
  pidLastMeasuredPos = currentPos;

  float pTerm = stepperPidKpConfig * error;
  float dTerm = -stepperPidKdConfig * measuredRate;
  // Feedforward carries the "known" bulk of the motion (see the
  // measurement block above); toggleable so it stays A/B-testable against
  // pure reactive PID with the same sweep tooling as every other change
  // this session.
  float ffTerm = stepperPidFeedforwardConfig ? pidFeedforwardVelocity : 0;

  // Anti-windup: only accumulate the integral term while the combined
  // output isn't already saturated at the speed cap - otherwise a
  // sustained large error (e.g. right after a big DDP jump) winds the
  // integral up far past what's useful and causes a real overshoot once
  // error finally starts shrinking back toward zero. Includes feedforward
  // in the saturation check - it's part of the real commanded output too.
  float provisional = ffTerm + pTerm + dTerm + stepperPidKiConfig * pidIntegral;
  bool saturated = fabs(provisional) >= (float)stepperPidMaxSpeedConfig;
  if (!saturated) {
    pidIntegral += error * dt;
  }
  float iTerm = stepperPidKiConfig * pidIntegral;

  float speed = ffTerm + pTerm + iTerm + dTerm;
  if (speed > stepperPidMaxSpeedConfig) speed = (float)stepperPidMaxSpeedConfig;
  if (speed < -stepperPidMaxSpeedConfig) speed = -(float)stepperPidMaxSpeedConfig;

  int newDirection = (speed > 0) ? 1 : (speed < 0 ? -1 : 0);
  uint32_t speedHz = (uint32_t)fabs(speed);
  if (speedHz < 50) speedHz = 50;  // floor so runForward/runBackward always gets a sane nonzero speed, matches Streaming

  stepper->setAcceleration(stepperPidAccelConfig);
  stepper->setSpeedInHz(speedHz);

  // Same "genuinely running" check as the hysteresis band above, and for
  // the same reason (found live on the bench, 2026-09-06, during a PID
  // damping sweep): a dead-but-nonempty FastAccelStepper queue satisfies
  // plain isRunning() without ever producing a step. Right at the homing
  // switch (position ~0, the state every run starts in), a runForward()
  // call can go dead this way - and since pidCurrentDirection then never
  // changes again on its own, the old plain-isRunning() check below this
  // comment never saw a reason to retry, leaving curSpeedHz pinned at 0
  // for 4+ real seconds while the commanded target raced ahead, then
  // catching up in one big burst once something else (e.g. a later
  // direction change) finally forced a fresh runForward()/runBackward()
  // call. Reproduced directly: curSpeedHz read exactly 0 for ~228
  // consecutive 20ms ticks with isRunning()==true throughout.
  //
  // First fix attempt OR'd in isRampGeneratorActive() (matching the
  // hysteresis band's own check at the time) - insufficient. A follow-up
  // bench run hit a 2.4s stall exactly at a direction reversal (mid-run,
  // not the cold-start case above) that this exact check failed to
  // recover from, proving isRampGeneratorActive() can itself report true
  // for seconds straight with zero actual speed - worse than useless as
  // a trust signal. Dropped; getCurrentSpeedInMilliHz() alone has never
  // been wrong in any case observed so far.
  bool genuinelyRunning = stepper->isRunning() && stepper->getCurrentSpeedInMilliHz() != 0;
  bool directionChanged = (newDirection != pidCurrentDirection);

  // Root-caused on the bench (2026-09-06, the same session as the fixes
  // above): calling runForward()/runBackward() for a *new* direction
  // while the stepper is still actively producing steps in the *old*
  // direction (mid-deceleration, genuinelyRunning still true) is what was
  // actually wedging FastAccelStepper at every direction reversal - not a
  // throttling or detection problem at all. The old code issued that call
  // immediately and unconditionally the moment newDirection changed, often
  // while curSpeedHz was still a few thousand Hz in the old direction;
  // once it coasted down to 0 on its own a moment later, it simply never
  // restarted, and every throttled retry after that called the exact same
  // wedged API the exact same way. Reproduced directly: a full 4s stall
  // starting right at a triangle wave's peak, both the commanded position
  // and raw DDP trace updating cleanly the whole time (ruling out any
  // network/reception cause - see TODO.md).
  //
  // First fix attempt only guarded this when genuinelyRunning was also
  // true (i.e. actively moving the old direction right now) - insufficient
  // on its own. A follow-up bench run hit the exact same multi-second
  // stall pattern engaging a *fresh* direction from an apparently-idle
  // stepper (not mid-reversal - this happened at the very start of a
  // triangle-wave test, after the harness's own direct-moveTo() skipped-
  // step check had just finished, i.e. not the boot-time cold-start case
  // either), proving the race isn't specific to "still visibly moving" -
  // something about engaging runForward()/runBackward() too soon after
  // *any* other stepper activity (a moveTo(), a just-finished PID run,
  // even boot) can wedge it the same way. Widened to unconditional: force
  // a clean stop-and-settle - the exact same forceStop()+pidStopSettling
  // pattern already used for the overshoot safety net above - before
  // *every* fresh direction engagement, not just ones that were visibly
  // still moving. A forceStop() on an already-idle stepper is a harmless
  // no-op (pidStopSettling's own isRunning() check passes through
  // immediately next tick), so this costs at most one extra ~20ms tick of
  // latency on the normal case.
  //
  // pidDirectionSwitchPending guards against re-triggering this forever:
  // pidStopSettling's own wait (checked near the top of this function)
  // re-enters updatePidMode() from scratch every tick while waiting, and
  // directionChanged is still true on the very tick the wait finally
  // clears - without this latch, that tick would just see directionChanged
  // and issue *another* forceStop(), looping forever and never reaching
  // the actual runForward()/runBackward() call below.
  if (directionChanged && !pidDirectionSwitchPending) {
    stepper->forceStop();
    pidStopSettling = true;
    pidDirectionSwitchPending = true;
    continuousRunDirection = 0;
    return;  // re-enters next tick; pidStopSettling's existing wait-for-idle
             // handling above will finish the engagement once truly stopped
  }

  if (directionChanged || !genuinelyRunning) {
    // directionChanged here means the pending stop above just settled -
    // issue the new direction's run immediately, unthrottled. Otherwise
    // (same direction as last tick) this is a retry after FastAccelStepper
    // silently dropped the previous runForward()/runBackward() request
    // (see lastPidRunRetryMs's declaration comment) - throttled exactly
    // like retryMoveIfDied() does.
    if (directionChanged || (now - lastPidRunRetryMs >= 100)) {
      lastPidRunRetryMs = now;
      // Root-caused on the bench (2026-09-06): setJumpStart()'s configured
      // burst (a fixed kick at full speed to overcome static friction,
      // meant for moveTo()-based moves) applies in the *wrong* direction
      // when issued through runForward()/runBackward()'s continuous-run
      // API instead - every PID engagement was taking a small step the
      // wrong way before the real (correctly-directioned) ramp ever had a
      // chance to start, which was especially visible - and especially
      // damaging - starting right at the homing switch. Disabled here,
      // restored for the moveTo()-based settle at the deadband above.
      stepper->setJumpStart(0);
      if (newDirection > 0) {
        stepper->runForward();
      } else if (newDirection < 0) {
        stepper->runBackward();
      }
      pidCurrentDirection = newDirection;
      pidDirectionSwitchPending = false;
      continuousRunDirection = newDirection;  // see stepper_handler.h's declaration comment
    }
  } else {
    // Already running the right way - per FastAccelStepper's own docs,
    // setSpeedInHz()/setAcceleration() only take effect after a following
    // move/moveTo/runForward/runBackward/applySpeedAcceleration() call,
    // not on their own - see TRACK_MODE_STREAMING's identical, hard-won
    // fix for the real bug this caused there.
    stepper->applySpeedAcceleration();
  }

  stepperBlanked = false;
  logCompactMotion(positionRequest, (int)target, (int)currentPos, 0, (int)error,
                    true, stepper->getCurrentSpeedInMilliHz(), (int)speed);
}

// TRACK_MODE_LOOKAHEAD: called on every DDP command that changes the
// commanded position - see StepperTrackMode's declaration comment for the
// full rationale. Aims moveTo() at the true commanded position plus a
// lookahead buffer further in the current direction of travel, instead of
// the literal commanded position, so the ramp generator has no reason to
// plan a decelerate-to-stop while updates keep arriving - unlike Direct
// mode, which aims at the literal (nearby) target every time and is
// therefore almost always within its own stopping distance of it.
void handleLookaheadModeCommand(uint16_t ddpVal, float newTarget) {
  if (newTarget > lookaheadRawTarget + 0.5f) {
    lookaheadDirection = 1;
  } else if (newTarget < lookaheadRawTarget - 0.5f) {
    lookaheadDirection = -1;
  }
  // else: no real change in commanded position (or a sub-step rounding
  // wobble) - keep whatever direction was already in effect.

  float previousRawTarget = lookaheadRawTarget;
  lookaheadRawTarget = newTarget;
  lastLookaheadCommandMs = millis();
  lookaheadSettled = false;

  int curPosBeforeMove = stepper->getCurrentPosition();
  int32_t curSpeedBeforeMove = stepper->getCurrentSpeedInMilliHz();
  float commandDeltaForLog = fabs(newTarget - previousRawTarget);
  float lagForLog = fabs(newTarget - (float)curPosBeforeMove);

  int targetSpeedHz;
  bool useTracking = applyTrackingProfileDecision(newTarget, previousRawTarget, targetSpeedHz);
  lastCommandedTargetPosition = newTarget;

  logCompactMotion(ddpVal, (int)newTarget, curPosBeforeMove, (int)commandDeltaForLog,
                    (int)lagForLog, useTracking, curSpeedBeforeMove, targetSpeedHz);

  float extendedTarget = newTarget + (float)(lookaheadDirection * stepperLookaheadStepsConfig);
  if (extendedTarget < 0) extendedTarget = 0;
  if (extendedTarget > bottomPosition) extendedTarget = (float)bottomPosition;

  // moveTo() is authoritative about its own target - it will never drive
  // the stepper past whatever position it's given, so clamping the target
  // itself here is sufficient. No external clamp-and-forceStop() safety net
  // needed the way Streaming's runForward()/runBackward() approach required
  // (that clamp's own bug is what stalled the motor earlier this session).
  stepper->moveTo((int32_t)extendedTarget);
  stepperBlanked = false;
}

// TRACK_MODE_LOOKAHEAD: called every loop() iteration. handleLookaheadModeCommand()
// above already keeps the stepper aimed at an extended target while updates
// are arriving; this just watches for a quiet period (no new DDP command)
// and then snaps to an exact moveTo() at the true final commanded position,
// same idea as Streaming's settle logic.
void updateLookaheadMode() {
  if (lookaheadSettled) return;
  unsigned long quietMs = millis() - lastLookaheadCommandMs;
  if (quietMs < (unsigned long)stepperLookaheadSettleMsConfig) return;

  int curPosBeforeMove = stepper->getCurrentPosition();
  int32_t curSpeedBeforeMove = stepper->getCurrentSpeedInMilliHz();
  float lagForLog = fabs(lookaheadRawTarget - (float)curPosBeforeMove);

  int targetSpeedHz;
  bool useTracking = applyTrackingProfileDecision(lookaheadRawTarget, lastCommandedTargetPosition, targetSpeedHz);
  lastCommandedTargetPosition = lookaheadRawTarget;

  logCompactMotion(oldPositionRequest, (int)lookaheadRawTarget, curPosBeforeMove, 0,
                    (int)lagForLog, useTracking, curSpeedBeforeMove, targetSpeedHz);

  stepper->moveTo((int32_t)lookaheadRawTarget);
  stepperBlanked = false;
  lookaheadSettled = true;
  lookaheadDirection = 0;
}

// Uptime tracking
unsigned long bootTime = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configure watchdog timer
  esp_task_wdt_init(WDT_TIMEOUT, true);  // 10 second timeout, panic on timeout
  esp_task_wdt_add(NULL); 
  Serial.print("Watchdog timer enabled with ");
  Serial.print(WDT_TIMEOUT);
  Serial.println(" second timeout");

  // Record boot time
  bootTime = millis();

  // Mounts SPIFFS and logs this boot's reset reason (POWERON/SW/PANIC/
  // BROWNOUT/etc.) to the reboot-surviving diagnostic log - as early as
  // possible, before anything else has a chance to fail first. See
  // persist_log.h's file comment for why this exists.
  initPersistLog();

  // Initialize status LED
  initStatusLed();

  Serial.println("\n=== Servo Controller Bootup ===");
  Serial.print("Version: ");
  Serial.println(VERSION_STRING);
  Serial.print("Build Date: ");
  Serial.print(BUILD_DATE);
  Serial.print(" ");
  Serial.println(BUILD_TIME);
  Serial.println();

  // Print partition table and OTA info
  printPartitionTable();
  printOTAInfo();

  // Initialize preferences
  preferences.begin("wifi-config", false);

  // Generate default hostname with MAC address
  uint8_t mac[6];
  WiFi.macAddress(mac);
  String macSuffix = String(mac[4], HEX) + String(mac[5], HEX);
  macSuffix.toUpperCase();
  String defaultHostname = "ServoController-" + macSuffix;

  // Load saved credentials
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  hostname = preferences.getString("hostname", defaultHostname);

  // Load static IP configuration
  useStaticIp = preferences.getBool("useStaticIp", false);
  staticIp = preferences.getString("staticIp", "");
  staticGateway = preferences.getString("staticGateway", "");
  staticSubnet = preferences.getString("staticSubnet", "255.255.255.0");

  // Load saved AP configuration
  ap_ssid = preferences.getString("apSsid", DEFAULT_AP_SSID);
  ap_password = preferences.getString("apPassword", DEFAULT_AP_PASSWORD);
  ap_append_mac = preferences.getBool("apAppendMac", DEFAULT_AP_APPEND_MAC);

  // Load saved stepper configuration
  stepperSpeedConfig = preferences.getInt("stepperSpeed", stepperSpeed);
  stepperAccelConfig = preferences.getInt("stepperAccel", stepperAccel);
  jumpStartConfig = preferences.getInt("jumpStart", 0);
  autoHomeOnBootConfig = preferences.getBool("autoHomeOnBoot", true);
  // NVS keys are capped at 15 chars - keep these short (see matching note in
  // html_handler.cpp's handleSaveStepper()).
  stepperSpeedHomingConfig = preferences.getInt("stepSpeedHome", stepperSpeedHoming);
  stepperAccelHomingConfig = preferences.getInt("stepAccelHome", stepperAccelHoming);
  stepperTrackEnabledConfig = preferences.getBool("stepTrackEn", true);
  stepperTrackThresholdConfig = preferences.getInt("stepTrackThresh", 2000);
  stepperTrackSpeedConfig = preferences.getInt("stepTrackSpeed", stepperSpeed);
  stepperTrackAccelConfig = preferences.getInt("stepTrackAccel", stepperAccel / 4);
  stepperTrackMaxLagConfig = preferences.getInt("stepTrackMaxLag", 3000);
  stepperTrackModeConfig = preferences.getInt("stepTrackMode", TRACK_MODE_DIRECT);
  stepperCoalesceMsConfig = preferences.getInt("stepCoalesceMs", 250);
  stepperCoalesceStepsConfig = preferences.getInt("stepCoalesceSt", 400);
  stepperStreamRateWindowMsConfig = preferences.getInt("stepStreamRateW", 250);
  stepperStreamSettleMsConfig = preferences.getInt("stepStreamSettl", 150);
  stepperLookaheadStepsConfig = preferences.getInt("stepLookaheadSt", 5000);
  stepperLookaheadSettleMsConfig = preferences.getInt("stepLookaheadMs", 150);

  // Load protocol configuration. Sanitize against stale NVS values from
  // before ArtNet was removed, when the enum was NONE=0/ARTNET=1/DDP=2 - a
  // device previously flashed with that firmware and saved as DDP(2) would
  // load an unrecognized value here (today's enum only has NONE=0/DDP=1),
  // silently fail the PROTOCOL_DDP check in loop(), and never process DDP
  // at all despite packets arriving fine at the socket. DDP is the only
  // protocol this firmware supports, so anything other than an explicit
  // NONE(0) is treated as DDP.
  int storedProtocol = preferences.getInt("protocol", PROTOCOL_DDP);
  protocolConfig = (storedProtocol == PROTOCOL_NONE) ? PROTOCOL_NONE : PROTOCOL_DDP;
  if (storedProtocol != PROTOCOL_NONE && storedProtocol != PROTOCOL_DDP) {
    Serial.print("NOTE: stored protocol value (");
    Serial.print(storedProtocol);
    Serial.println(") doesn't match this firmware's protocol enum (likely stale from before ArtNet removal) - defaulting to DDP");
  }
  stepperControlEnabled = preferences.getBool("stepperControl", true);
  control16BitConfig = preferences.getBool("control16Bit", false);
  protocolDebugConfig = preferences.getBool("protocolDebug", false);

  // Load blank time configuration
  ledBlankTimeConfig = preferences.getInt("ledBlankTime", 0);
  // See matching note in html_handler.cpp's handleSaveProtocol() - old key
  // exceeded NVS's 15-char limit and never actually persisted.
  stepperBlankTimeConfig = preferences.getInt("stepBlankTime", 0);

  // Load TMC2209 UART configuration
  tmcEnabledConfig = preferences.getBool("tmcEnabled", false);
  tmcRSenseConfig = preferences.getFloat("tmcRSense", 0.11f);
  tmcAddressConfig = (uint8_t)preferences.getInt("tmcAddress", 0);
  tmcRunCurrentConfig = (uint16_t)preferences.getInt("tmcRunCurrent", 800);
  tmcHoldPercentConfig = (uint8_t)preferences.getInt("tmcHoldPercent", 50);
  tmcStealthChopConfig = preferences.getBool("tmcStealthChop", true);
  tmcStallEnabledConfig = preferences.getBool("tmcStallEnabled", false);
  tmcStallThresholdConfig = (uint16_t)preferences.getInt("tmcStallThresh", 50);
  tmcMicrostepsConfig = (uint16_t)preferences.getInt("tmcMicrosteps", 16);
  tmcHstrtConfig = (uint8_t)preferences.getInt("tmcHstrt", 0);
  tmcHendConfig = (uint8_t)preferences.getInt("tmcHend", 0);
  tmcPwmRegConfig = (uint8_t)preferences.getInt("tmcPwmReg", 4);
  tmcPwmLimConfig = (uint8_t)preferences.getInt("tmcPwmLim", 12);
  tmcPwmAutogradConfig = preferences.getBool("tmcPwmAutograd", true);

  // Initialize stepper
  initializeStepper();

  // Starts the Core 0 task, which calls initEncoder() itself (not here) so
  // the encoder's GPIO interrupt ends up Core-0-affine, physically
  // separated from FastAccelStepper's own Core-1-affine PCNT interrupt -
  // see core0_task.h.
  startCore0Task();

  // Initialize TMC2209 UART link (no-op if tmcEnabledConfig is false)
  initTmc();

  // Initialize pixel LEDs (loads config from preferences)
  initPixelLeds();

  // Determine if we should connect to wifi or start a host access point
  if(ssid.length() > 0) {
    Serial.println("Attempting to connect to saved WiFi...");
    Serial.print("SSID: ");
    Serial.println(ssid);
    if(connectToWifi()) {
      Serial.println("Successfully connected to WiFi!");

      // Start mDNS
      if (MDNS.begin(hostname.c_str())) {
        Serial.print("mDNS responder started: ");
        Serial.print(hostname);
        Serial.println(".local");
        MDNS.addService("http", "tcp", 80);
      } else {
        Serial.println("Error starting mDNS");
      }
    }
    else {
      Serial.print("Could not connect to WiFi network '");
      Serial.print(ssid);
      Serial.println("', starting Access Point mode...");
      startAccessPoint();
    }
  }
  else {
    // Failed to connect or no credentials, start AP mode
    Serial.println("No saved network to connect to, starting Access Point mode...");
    startAccessPoint();
  }

  startWebServer();
  initDDP();

  // Start non-blocking homing if enabled (will complete in loop)
  if (autoHomeOnBootConfig) {
    Serial.println("Auto-homing enabled, starting homing sequence...");
    startHoming();
  } else {
    Serial.println("Auto-homing disabled, skipping homing sequence");
  }
}

void loop() {
  // Feed the watchdog timer to prevent reset during normal operation
  esp_task_wdt_reset();

  // Check WiFi connection and reconnect if needed
  checkWifiConnection();

  // Commit any RAM-buffered persistent-log entries to flash - only safe to
  // do while nothing is stepping (see persist_log.h's flushPersistLogNow()
  // comment for why). Skipped entirely, not just deferred, whenever the
  // stepper is running - the next idle moment picks up whatever
  // accumulated in the meantime.
  if (hasPendingPersistLog() && (stepper == NULL || !stepper->isRunning())) {
    flushPersistLogNow();
  }

  // Periodic compact-log sample, so any significant move (a single large
  // DDP jump, or a diagnostic moveTo() like $CHECKSTEPS's that bypasses
  // this DDP dispatch entirely) gets a full speed-over-time trace instead
  // of just one data point at commit time. No-op unless compactLogEnabled.
  logCompactMotionPeriodic();

  // Update the skipped-step diagnostic move, if one is in progress - must
  // run before updateHoming() since both react to the same ISR-deferred
  // pendingForceStop flag, and this one needs first refusal on it (see
  // updateStepCheck()'s comment in stepper_handler.cpp).
  updateStepCheck();

  // Update the encoder diagnostic sweep, if one is in progress ($ENCDIAG) -
  // no first-refusal ordering requirement like updateStepCheck() above (see
  // its declaration comment, stepper_handler.h), just grouped with the
  // other diagnostic update calls for consistency.
  updateEncoderDiag();

  // Update non-blocking homing state machine
  updateHoming();

  // Detect being physically rammed into the homing stop during normal
  // (non-homing) operation - see stepper_handler.h's declaration comment.
  // Deliberately independent of the encoder, so this is real, permanent
  // safety logic, not something that needs to be pared off later.
  updateRammedIntoStopCheck();

  // Poll TMC2209 diagnostics / stall detection (no-op if not enabled)
  updateTmc();

  // Finish saving Stepper Configuration to flash once the motor is idle,
  // if a save is pending (no-op otherwise) - see stepperSettingsPendingSave.
  persistStepperSettingsIfPending();

  server.handleClient();
  handleSerialCommands();

  // Skip DDP and DNS handling during OTA update to prevent interference
  if (!otaInProgress) {
    if (protocolConfig == PROTOCOL_DDP) {
      handleDDP();
    }

    // Process DNS requests for captive portal (only in AP mode)
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
      dnsServer.processNextRequest();
    }
  }

  // Handle position requests from DDP
  uint16_t currentPositionRequest;
  currentPositionRequest = positionRequest;

  if (currentPositionRequest != oldPositionRequest) {
    oldPositionRequest = currentPositionRequest;

    // Only move stepper if homed and not currently homing
    if (!homed) {
      if (protocolDebugConfig) {
        Serial.print("Ignoring position command - system not homed");
      }
    } else if (isHoming()) {
      if (protocolDebugConfig) {
        Serial.print("Ignoring command - homing in progress");
      }
    } else {
      position = calcPosition(currentPositionRequest, control16BitConfig);

      // Which motion strategy handles this - see StepperTrackMode's
      // declaration comment for why there's more than one.
      switch (stepperTrackModeConfig) {
        case TRACK_MODE_COALESCE:
          coalescePendingTarget = position;
          coalesceHasPending = true;
          break;
        case TRACK_MODE_STREAMING:
          streamRawTarget = position;
          lastDdpCommandMs = millis();
          streamSettled = false;
          if (streamHistoryCount < STREAM_HISTORY_SIZE) {
            streamHistory[streamHistoryCount].ms = lastDdpCommandMs;
            streamHistory[streamHistoryCount].pos = position;
            streamHistoryCount++;
          } else {
            for (int i = 1; i < STREAM_HISTORY_SIZE; i++) streamHistory[i - 1] = streamHistory[i];
            streamHistory[STREAM_HISTORY_SIZE - 1].ms = lastDdpCommandMs;
            streamHistory[STREAM_HISTORY_SIZE - 1].pos = position;
          }
          break;
        case TRACK_MODE_LOOKAHEAD:
          handleLookaheadModeCommand(currentPositionRequest, position);
          break;
        case TRACK_MODE_PID:
          // Deliberately no per-command handler - updatePidMode() (called
          // every loop() iteration below) reads positionRequest directly
          // itself. See TRACK_MODE_PID's declaration comment.
          break;
        case TRACK_MODE_DIRECT:
        default:
          handleDirectModeCommand(currentPositionRequest, position);
          break;
      }
      stepperBlanked = false;  // Reset blank state when we receive a command
    }
  }

  // Per-loop-iteration processing for the batching/streaming/lookahead modes
  // (no-op unless that mode is active and homed/not-homing) - see StepperTrackMode.
  if (homed && !isHoming()) {
    if (stepperTrackModeConfig == TRACK_MODE_COALESCE) {
      updateCoalesceMode();
    } else if (stepperTrackModeConfig == TRACK_MODE_STREAMING) {
      updateStreamingMode();
    } else if (stepperTrackModeConfig == TRACK_MODE_LOOKAHEAD) {
      updateLookaheadMode();
    } else if (stepperTrackModeConfig == TRACK_MODE_PID) {
      updatePidMode();
    }
  }

  // Advance the local LED test pattern, if enabled (no-op otherwise)
  updateLedTestMode();

  // Handle blank time timeouts
  if (lastProtocolUpdateTime > 0 && !otaInProgress) {
    unsigned long timeSinceUpdate = millis() - lastProtocolUpdateTime;

    // LED blank timeout - skip while the test pattern owns the strip
    if (!ledTestModeActive && ledBlankTimeConfig > 0 && timeSinceUpdate > (unsigned long)ledBlankTimeConfig * 1000) {
      blankPixelLeds();
    }

    // Stepper blank timeout (only if homed and not currently homing)
    if (stepperBlankTimeConfig > 0 && homed && !isHoming() && !stepperBlanked) {
      if (timeSinceUpdate > (unsigned long)stepperBlankTimeConfig * 1000) {
        if (protocolDebugConfig) {
          Serial.println("Stepper blank timeout - moving to position 0");
        }
        stepper->moveTo(0);
        stepperBlanked = true;
      }
    }
  }
}

float calcPosition(float positionRequest, bool is16Bit) {
  float maxValue = is16Bit ? 65535.0 : 255.0;
  return (float)bottomPosition * ((float)positionRequest / maxValue);
}

// Buffers a '$'-prefixed line for the extended tuning-harness command
// protocol (see tuning_handler.h) - everything else stays the normal
// single-character commands below, read one at a time as before.
static String extendedCommandBuffer = "";
static bool inExtendedCommand = false;

void handleSerialCommands() {
  // Check for serial input
  if (Serial.available() > 0) {
    char c = Serial.read();

    if (inExtendedCommand) {
      if (c == '\n' || c == '\r') {
        if (extendedCommandBuffer.length() > 0) {
          handleExtendedSerialCommand(extendedCommandBuffer);
          extendedCommandBuffer = "";
        }
        inExtendedCommand = false;
      } else {
        extendedCommandBuffer += c;
        if (extendedCommandBuffer.length() > 200) {  // safety cap against a runaway line
          Serial.println("ERR line too long");
          extendedCommandBuffer = "";
          inExtendedCommand = false;
        }
      }
      return;
    }

    if (c == '$') {
      inExtendedCommand = true;
      extendedCommandBuffer = "";
      return;
    }

    switch (c) {
    case '\r':
    case '?':
      Serial.println("\n=== Available Serial Commands ===");
      Serial.println("?  - Show this help menu");
      Serial.println("h  - Start homing sequence");
      Serial.println("r  - Reboot device");
      Serial.println("s  - Print status");
      Serial.println("n  - Print connection status (brief)");
      Serial.println("w  - Attempt WiFi reconnection");
      Serial.println("a  - Switch to Access Point mode");
      Serial.println("p  - Print current stepper position");
      Serial.println("f  - Move forward 10 steps");
      Serial.println("b  - Move backward 10 steps");
      Serial.println("l  - Print the persistent (reboot-surviving) diagnostic log - see persist_log.h; GET /persist-log is the normal way to fetch this over WiFi instead");
      Serial.println("$  - Extended tuning-harness command (see tuning_handler.h)");
      Serial.println("================================\n");
      break;
    case 'l':
      Serial.println(readPersistLog());
      break;
    case 'h':
      Serial.println("Starting homing...");
      startHoming();
      break;
    case 'r':
      Serial.print("Rebooting...");
      ESP.restart();
      break;
    case 's':
      printFullStatus();
      break;
    case 'n':
      printStatus();
      break;
    case 'w':
      Serial.println("Attempting to reconnect to WiFi...");
      if (ssid.length() > 0) {
        if (connectToWifi()) {
          Serial.println("Successfully connected! Disabling AP mode...");
          WiFi.softAPdisconnect(true);
          WiFi.mode(WIFI_STA);

          // Start mDNS
          if (MDNS.begin(hostname.c_str())) {
            Serial.print("mDNS responder started: ");
            Serial.print(hostname);
            Serial.println(".local");
            MDNS.addService("http", "tcp", 80);
          } else {
            Serial.println("Error starting mDNS");
          }
        }
        else {
          Serial.println("Connection failed. AP mode remains active.");
        }
      }
      else {
        Serial.println("No SSID configured. Cannot connect to WiFi.");
      }
      break;
    case 'a':
      Serial.println("Switching to Access Point mode...");
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Disconnecting from WiFi...");
        WiFi.disconnect(true);
      }
      startAccessPoint();
      Serial.println("Access Point mode activated");
      Serial.print("AP SSID: ");
      Serial.println(getAPName());
      Serial.print("AP Password: ");
      Serial.println(ap_password);
      Serial.print("AP IP: ");
      Serial.println(WiFi.softAPIP());
      break;
    case 'p':
      Serial.print("Current position: ");
      Serial.println(stepper->getCurrentPosition());
      break;
    case 'f':
      if (isHoming()) {
        Serial.println("Cannot move while homing is in progress");
      }
      else {
        Serial.println("Moving forward 10 steps");
        stepper->setAcceleration(stepperAccelHomingConfig);
        stepper->move(10);
        stepper->setAcceleration(stepperAccelConfig);
      }
      break;
    case 'b':
      if (isHoming()) {
        Serial.println("Cannot move while homing is in progress");
      }
      else {
        Serial.println("Moving backward 10 steps");
        stepper->setAcceleration(stepperAccelHomingConfig);
        stepper->move(-10);
        stepper->setAcceleration(stepperAccelConfig);
      }
      break;
    }
  }
}

void printStatus() {
  Serial.println("\n=== Connection Status ===");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Mode: Connected to WiFi");
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    Serial.println("Mode: Access Point (AP Mode)");
    Serial.print("SSID: ");
    Serial.println(getAPName());
    Serial.print("Password: ");
    Serial.println(ap_password);
    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("Connected Clients: ");
    Serial.println(WiFi.softAPgetStationNum());
  } else {
    Serial.println("Mode: Disconnected");
  }

  Serial.println("========================\n");
}

void printFullStatus() {
  Serial.println("\n=== Status ===");

  // WiFi Status
  Serial.println("\n--- WiFi Status ---");
  Serial.print("WiFi Status Code: ");
  Serial.print(WiFi.status());
  Serial.print(" (");
  switch(WiFi.status()) {
    case WL_IDLE_STATUS: Serial.print("IDLE"); break;
    case WL_NO_SSID_AVAIL: Serial.print("NO_SSID_AVAIL"); break;
    case WL_SCAN_COMPLETED: Serial.print("SCAN_COMPLETED"); break;
    case WL_CONNECTED: Serial.print("CONNECTED"); break;
    case WL_CONNECT_FAILED: Serial.print("CONNECT_FAILED"); break;
    case WL_CONNECTION_LOST: Serial.print("CONNECTION_LOST"); break;
    case WL_DISCONNECTED: Serial.print("DISCONNECTED"); break;
    default: Serial.print("UNKNOWN"); break;
  }
  Serial.println(")");

  Serial.print("WiFi Mode: ");
  switch(WiFi.getMode()) {
    case WIFI_OFF: Serial.println("OFF"); break;
    case WIFI_STA: Serial.println("STATION"); break;
    case WIFI_AP: Serial.println("ACCESS_POINT"); break;
    case WIFI_AP_STA: Serial.println("AP+STATION"); break;
    default: Serial.println("UNKNOWN"); break;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("BSSID: ");
    Serial.println(WiFi.BSSIDstr());
    Serial.print("Channel: ");
    Serial.println(WiFi.channel());
    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("Subnet: ");
    Serial.println(WiFi.subnetMask());
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());

    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());
    Serial.print("Hostname: ");
    Serial.println(WiFi.getHostname());
  }

  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    Serial.println("\n--- AP Mode Info ---");
    Serial.print("AP SSID: ");
    Serial.println(getAPName());
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("Connected Clients: ");
    Serial.println(WiFi.softAPgetStationNum());
  }

  // Memory and system info
  Serial.println("\n--- System Resources ---");
  Serial.print("Free Heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
  Serial.print("Heap Size: ");
  Serial.print(ESP.getHeapSize());
  Serial.println(" bytes");
  Serial.print("Min Free Heap: ");
  Serial.print(ESP.getMinFreeHeap());
  Serial.println(" bytes");

  // Protocol info
  Serial.println("\n--- Protocol Status ---");
  Serial.print("Active Protocol: ");
  Serial.println(protocolConfig == PROTOCOL_DDP ? "DDP" : "Disabled");
  Serial.print("Last Protocol Update: ");
  if (lastProtocolUpdateTime > 0) {
    Serial.print(millis() - lastProtocolUpdateTime);
    Serial.println(" ms ago");
  } else {
    Serial.println("Never");
  }

  Serial.print("DDP Packets Received: ");
  Serial.println(ddpPacketsReceived);
  Serial.print("DDP Packets Rejected (out-of-order/duplicate): ");
  Serial.println(ddpPacketsRejectedOutOfOrder);

  // Stepper/homing status
  Serial.println("\n--- Stepper Status ---");
  Serial.print("Homed: ");
  Serial.println(homed ? "Yes" : "No");
  Serial.print("Homing In Progress: ");
  Serial.println(isHoming() ? "Yes" : "No");
  Serial.print("Current Position: ");
  Serial.println(stepper->getCurrentPosition());
  Serial.print("Bottom Position: ");
  Serial.println(bottomPosition);
  Serial.print("Full Travel: ");
  if (homingTravelValid && homingTravelMs > 0) {
    Serial.print(homingTravelSteps);
    Serial.print(" steps in ");
    Serial.print(homingTravelMs);
    Serial.print(" ms (~");
    Serial.print((float)homingTravelSteps * 1000.0f / (float)homingTravelMs, 0);
    Serial.println(" steps/s average)");
  } else {
    Serial.println("Not yet measured");
  }
  Serial.print("Encoder: ");
  if (isEncoderInitialized()) {
    Serial.print(getEncoderCount());
    Serial.print(" (missed transitions: ");
    Serial.print(getMissedTransitionCount());
    Serial.println(")");
    Serial.print("Core 0 task: max loop gap ");
    Serial.print(getCore0TaskMaxGapMs());
    Serial.print(" ms, min free stack ");
    Serial.print(getCore0TaskMinStackBytes());
    Serial.println(" bytes");
  } else {
    Serial.println("Not initialized");
  }

  // TMC2209 driver info
  Serial.println("\n--- TMC2209 Driver Status ---");
  if (!tmcEnabledConfig) {
    Serial.println("UART control: Disabled");
  } else if (!tmcConnected) {
    Serial.println("UART control: Enabled, but link FAILED (check wiring/RSense/address)");
  } else {
    Serial.println("UART control: Connected");
    Serial.print("Run Current: ");
    Serial.print(tmcRunCurrentConfig);
    Serial.print(" mA, Hold: ");
    Serial.print(tmcHoldPercentConfig);
    Serial.println("%");
    Serial.print("Chopper Mode: ");
    Serial.println(tmcStealthChopConfig ? "StealthChop" : "SpreadCycle");
    Serial.print("Over-Temp Warning: ");
    Serial.println(tmcStatus.overTempWarning ? "YES" : "No");
    Serial.print("Over-Temp Shutdown: ");
    Serial.println(tmcStatus.overTempShutdown ? "YES" : "No");
    Serial.print("Short to Ground (A/B): ");
    Serial.print(tmcStatus.shortToGroundA ? "YES" : "No");
    Serial.print(" / ");
    Serial.println(tmcStatus.shortToGroundB ? "YES" : "No");
    Serial.print("Open Load (A/B): ");
    Serial.print(tmcStatus.openLoadA ? "YES" : "No");
    Serial.print(" / ");
    Serial.println(tmcStatus.openLoadB ? "YES" : "No");
    Serial.print("UART CRC Errors: ");
    Serial.println(tmcStatus.uartCrcError ? "YES" : "No");
    Serial.print("Stall Detection: ");
    if (tmcStallEnabledConfig) {
      Serial.print("Enabled (threshold ");
      Serial.print(tmcStallThresholdConfig);
      Serial.print(", live SG_RESULT ");
      Serial.print(tmcStatus.stallGuardResult);
      Serial.println(")");
      if (tmcStatus.stalled) {
        Serial.println("*** STALL LATCHED - clear from the Status page ***");
      }
    } else {
      Serial.println("Disabled");
    }
  }

  // LED info
  Serial.println("\n--- LED Status ---");
  Serial.print("LEDs Initialized: ");
  Serial.println(ledsInitialized ? "Yes" : "No");
  Serial.print("LEDs Blanked: ");
  Serial.println(ledsBlanked ? "Yes" : "No");
  Serial.print("Configured Pixels: ");
  Serial.println(ledPixelCount);
  Serial.print("Max Pixels Received: ");
  Serial.println(ledMaxPixelsReceived);

  // Timer info
  Serial.println("\n--- Interrupt Status ---");
  Serial.print("LED Timer Active: ");
  Serial.println(ledTimer != NULL ? "Yes" : "No");
  Serial.print("Homing Interrupt Triggered: ");
  Serial.println(interruptTriggered ? "Yes" : "No");

  // Uptime
  Serial.println("\n--- Uptime ---");
  unsigned long uptimeMs = millis() - bootTime;
  unsigned long uptimeSecs = uptimeMs / 1000;
  unsigned long days = uptimeSecs / 86400;
  unsigned long hours = (uptimeSecs % 86400) / 3600;
  unsigned long minutes = (uptimeSecs % 3600) / 60;
  unsigned long seconds = uptimeSecs % 60;
  Serial.printf("Uptime: %lu days, %02lu:%02lu:%02lu\n", days, hours, minutes, seconds);

  Serial.println("\n========================\n");
}
