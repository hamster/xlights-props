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
long pidLastMeasuredPos = 0;   // for derivative-on-measurement, not derivative-on-error - see updatePidMode()
// Low-pass-filtered version of the raw measured rate, used for the
// derivative term instead of the raw value (2026-09-07 - chasing the
// remaining smoothness/jerk gap vs Direct mode, after the feedforward
// work above already closed the accuracy/corner-tightness gap the other
// way). Derivative-on-measurement of a quantized, noisy position signal
// is a classic PID roughness source - the raw rate jitters tick to tick
// even during genuinely smooth motion, since position only ever changes
// in whole steps and the target itself advances in DDP-quantized jumps.
float pidFilteredRate = 0;
// NOTE (2026-09-07): tried at two filter strengths (0.5/0.5, then 0.85/0.15
// - a much heavier filter) and jerk_per_sample barely moved either time
// (513.5 baseline -> 496.9 -> 506.8, within run-to-run noise) - the
// derivative term is NOT the dominant jerk source. Left in (a real, if
// small, accuracy/corner-tightness improvement - 246.5->228.8 rms_error,
// 595.5->515.2 corner mean at the heavier setting).


// Latches the overshoot safety net so it fires once per genuine excursion
// outside [0, bottomPosition] rather than on every tick the trolley is
// parked outside it - cleared only once curPos is genuinely back inside.
// See the net itself (top of updatePidMode()) for the 3.5s freeze this
// fixes.
bool pidOvershootLatched = false;
// Time-budget-aware velocity feedforward (2026-09-07 - see the user's own
// original framing: PID had no notion of how much time it actually has to
// get from one commanded point to the next, so it always drove at full
// reactive speed even for small moves paced by a real, much slower show
// timeline). Self-measures the real rate the incoming *target* has been
// changing at - not an assumed DDP fps - so it adapts to whatever a show
// was actually authored at (25fps, 40fps, anything) with nothing
// hardcoded. See updatePidMode()'s feedforward block for the full design,
// including why this measures over a fixed time window rather than
// between consecutive target changes.
struct PidFfSample {
  float target;
  unsigned long ms;
};
// Sized for time coverage, not sample count, since both scale with
// stepperPidTickMsConfig (2026-09-07 evening - grown from 16 alongside
// that becoming configurable). At 128 entries: 2560ms of history at the
// original 20ms tick, still 640ms at a 5x-faster 4ms tick - comfortably
// longer than any pidFfWindowMs/pidLookaheadMs worth configuring at any
// tick rate this project would plausibly test, so neither window is ever
// silently truncated by hitting the buffer's own edge. Also now shared by
// the lookahead/reference-smoothing average (see pTermTarget below), which
// didn't exist when this was first sized at 16. Costs ~2KB total RAM for
// both this and pidFfWork below - checked against a build (63.2% used
// before this change), trivial at this size.
static const int PID_FF_HIST_LEN = 128;
PidFfSample pidFfHist[PID_FF_HIST_LEN];
// Scratch for the regression below - a file-scope array rather than a
// stack one purely to keep updatePidMode()'s frame small; it holds no
// state between calls.
struct PidFfWorkPoint { float t; float y; };
PidFfWorkPoint pidFfWork[PID_FF_HIST_LEN];
int pidFfHistIdx = 0;
int pidFfHistCount = 0;
float pidFeedforwardVelocity = 0;          // current feedforward speed estimate, steps/s (signed)
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

// Throttled wrapper for updatePidMode()'s own "log every tick" call sites
// (2026-09-07, added alongside stepperPidTickMsConfig becoming
// configurable). Every one of those call sites' own comments say "log
// every tick" meaning "every ~20ms," written back when PID's tick WAS a
// hardcoded 20 - decoupling tick rate from log rate silently broke that
// assumption: at pidTickMs=5, logging on literally every tick meant up to
// 200 rows/sec, which overflowed the 96KB Compact Motion Log ring buffer
// in under 11 seconds (confirmed on the bench - a 12s capture came back
// missing its first ~1.1s, silently truncated). Logging has no feedback
// into the control law, so decoupling its cadence costs nothing there.
//
// Independently configurable (stepperPidLogMsConfig), NOT just derived as
// max(20, tick) - found necessary the same evening: throttling the log to
// 20ms while validating pidTickMs=5 silently hid the very improvement
// being measured, since jerk_per_sample is computed from consecutive
// *logged* rows - a 20ms-throttled log downsamples a real 5ms-resolution
// improvement back to 20ms-equivalent before any analysis tool ever sees
// it. Default 20ms preserves original behavior/safe capture windows for
// normal use; set to match pidTickMs for a short diagnostic capture that
// needs full control-rate resolution, accepting a shorter safe window in
// trade (see COMPACT_LOG_ROW_CAPACITY in tools/ddp_continuous_test.py).
unsigned long lastPidLogMs = 0;
void logCompactMotionPidThrottled(uint16_t ddpVal, int cmdPos, int curPos, int delta, int lag,
                                    bool tracking, int32_t curSpeedMilliHz, int targetSpeedHz) {
  unsigned long now = millis();
  if (now - lastPidLogMs < (unsigned long)stepperPidLogMsConfig) return;
  lastPidLogMs = now;
  logCompactMotion(ddpVal, cmdPos, curPos, delta, lag, tracking, curSpeedMilliHz, targetSpeedHz);
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

    // Restore the hardcoded jumpStart for this moveTo()-based settle - see
    // the runForward()/runBackward() call site below for why it's disabled
    // while actually streaming (same fix as TRACK_MODE_PID, same root cause).
    stepper->setJumpStart(JUMP_START_STEPS);
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
  // Stale-tick watchdog (2026-09-07, see TODO.md's "Needed: a stale-tick
  // watchdog" entry for the full incident this fixes). In continuous-run
  // mode, runForward()/runBackward() mean "keep going indefinitely" - the
  // stepper does not stop on its own, only this function's own next tick
  // decides whether to keep going, reverse, or stop. If loop() doesn't get
  // back around to call this for a while (a large HTTP response - the
  // Compact Motion Log is up to 96KB - is the case that actually happened;
  // WiFi reconnect logic and SPIFFS writes are also blocking and run from
  // loop()), the stepper just cruises with nothing supervising it. Measured
  // directly: a real 1.0s gap in logged rows during which the trolley
  // travelled +3,660 steps completely unsupervised, confirmed by the
  // encoder as real physical motion, not a counter artifact.
  //
  // Neither the overshoot safety net below nor the directional containment
  // guards further down help here, because both only run when this
  // function runs - they cannot fire during exactly the gap where they're
  // needed. This check is what closes that gap: if it's been suspiciously
  // long since the last tick while a continuous run was in flight,
  // forceStop() immediately, before anything else in this function (which
  // would otherwise still be reasoning from now-stale assumptions) runs.
  //
  // Flat 200ms, not scaled to stepperPidTickMsConfig - this is detecting an
  // abnormal LOOP-LEVEL stall (the observed hazard was 500-1000ms+), not
  // normal tick-to-tick variance, so it doesn't need to track how fast the
  // control loop itself is configured to run. Comfortably longer than
  // normal jitter at any tick rate this project has tested (5-20ms) and
  // comfortably shorter than the real hazard windows that motivated this.
  {
    unsigned long nowWatchdog = millis();
    static const unsigned long STALE_TICK_THRESHOLD_MS = 200;
    if (continuousRunDirection != 0 && lastPidUpdateMs != 0 &&
        (nowWatchdog - lastPidUpdateMs) > STALE_TICK_THRESHOLD_MS) {
      stepper->forceStop();
      pidStopSettling = true;
      pidDirectionSwitchPending = false;  // abandon any in-flight reversal handshake - a fresh one starts clean next tick
      continuousRunDirection = 0;
      persistLog("PID stale-tick watchdog: %lums since last tick, forced stop", nowWatchdog - lastPidUpdateMs);
    }
  }

  if (stepper->isRunning()) {
    int curPos = stepper->getCurrentPosition();
    // Same hard safety net TRACK_MODE_STREAMING uses - a continuously-
    // driven PID output has no built-in "never overshoot" guarantee the
    // way moveTo() does.
    //
    // Small tolerance, and doesn't re-trigger while already settling
    // (2026-09-07, found live testing feedforward against real
    // DDPDebugger data): a real ~800ms stall at the bottom-of-travel
    // reversal traced to this firing on every single tick that curPos
    // read slightly negative (-32, -17, -2 steps - a real but entirely
    // benign excursion right at the switch reversal, nowhere near a
    // genuine safety violation) - each re-trigger called forceStop()
    // and re-armed pidStopSettling from scratch, never letting the
    // actual recovery/re-engage logic further down in this function run
    // to completion until the small excursion happened to resolve on its
    // own. Tolerance matches RAMMED_STEP_TOLERANCE's precedent
    // (stepper_handler.cpp) for the same class of judgment call.
    //
    // pidOvershootLatched (2026-09-07 evening) is what actually makes
    // "exactly once per excursion" true. The !pidStopSettling guard alone
    // doesn't: pidStopSettling clears as soon as the stepper genuinely
    // stops, but the trolley is then still parked outside bounds, so the
    // very next tick re-fires this check, forceStop()s an already-stopped
    // motor, and re-arms the wait - a loop that only ends when the
    // commanded target happens to travel back to wherever the trolley is
    // parked. Measured directly: a 3.5s freeze at curPos=15670 against
    // bottomPosition+tolerance=15652, with the control law correctly
    // asking to drive back down the whole time and never getting a tick
    // in which to do it. The same loop is what made the shorter (600-700ms)
    // top-of-travel stalls seen in earlier sweeps.
    //
    // Latching until curPos is genuinely back inside the bounds means the
    // safety net fires once on the way out, then hands control to the
    // normal control law to drive back in - which is what it was always
    // documented to do.
    static const int PID_OVERSHOOT_TOLERANCE = 150;
    bool outOfBounds = (curPos < -PID_OVERSHOOT_TOLERANCE ||
                        curPos > bottomPosition + PID_OVERSHOOT_TOLERANCE);
    if (!outOfBounds) {
      pidOvershootLatched = false;  // back inside - re-arm for a genuine future excursion
    } else if (!pidStopSettling && !pidOvershootLatched) {
      stepper->forceStop();
      pidStopSettling = true;
      pidOvershootLatched = true;
      continuousRunDirection = 0;
    }
  }

  unsigned long now = millis();
  // Configurable (2026-09-07 evening, see stepperPidTickMsConfig's
  // declaration comment) - was a hardcoded 20 matching Streaming's own
  // cadence. dt is still computed from real elapsed time regardless, so a
  // faster configured tick just means updatePidMode() gets more chances to
  // run per second; an occasional overrun (a slow loop() elsewhere) still
  // reads out correctly instead of assuming exactly stepperPidTickMsConfig
  // passed.
  if (now - lastPidUpdateMs < (unsigned long)stepperPidTickMsConfig) return;
  float dt = (lastPidUpdateMs == 0) ? (stepperPidTickMsConfig / 1000.0f)
                                     : (now - lastPidUpdateMs) / 1000.0f;
  lastPidUpdateMs = now;

  long currentPos = stepper->getCurrentPosition();
  float target = calcPosition(positionRequest, control16BitConfig);
  float error = target - (float)currentPos;

  // Velocity feedforward measurement - runs every tick regardless of which
  // branch below ends up firing, so the estimate stays continuous and isn't
  // reset by time spent settled or hysteresis-snapping.
  //
  // Measures the target's rate over a FIXED TIME WINDOW (the oldest sample
  // still inside stepperPidFfWindowMsConfig), not "distance since the last
  // observed change over the time since that change". That distinction is
  // the whole point, and it was worth real bench time to find:
  //
  // The old formulation divided a fine numerator by a coarse denominator.
  // This function only samples on its own ~20ms tick, so "time since the
  // last observed change" is quantized to whole ticks (20/40/60ms) while a
  // 40fps source actually delivers every 25ms - the two beat against each
  // other, and the resulting ratio swings hard even when the real motion is
  // perfectly smooth. Measured at ~350-400 Hz/tick of chop reaching the
  // commanded speed, making it the single largest jerk source in the loop -
  // larger than the P and D terms combined, which is why neither the
  // derivative filter nor a P-term-only slew limiter nor the shelved
  // trajectory-reference layer ever moved jerk. Confirmed independent of
  // DDP bit depth (8-bit and 16-bit both measured ~570-580 Hz/tick), which
  // is what ruled quantization out as the cause.
  //
  // Over a fixed window the denominator is a real elapsed time, so there is
  // no beat to alias - and unlike an EMA slow enough to suppress the same
  // noise, it adds only about half the window in lag rather than several
  // time constants. It also needs no separate machinery: no EMA, no
  // outlier clamp (a bounded-below denominator can't produce a wild
  // ratio), and no "stale, zero it out" timeout - if the target stops
  // moving, the window's own numerator goes to zero within one window,
  // which is exactly the correct answer.
  pidFfHist[pidFfHistIdx].target = target;
  pidFfHist[pidFfHistIdx].ms = now;
  pidFfHistIdx = (pidFfHistIdx + 1) % PID_FF_HIST_LEN;
  if (pidFfHistCount < PID_FF_HIST_LEN) pidFfHistCount++;

  {
    // Least-squares slope over every sample inside the window, not the
    // difference between its two endpoints (2026-09-07 evening, measured).
    // An endpoint difference over a longer baseline only rescales the
    // noise - it never averages it - which showed up exactly that way on
    // the bench: widening a two-point window improved corner tightness a
    // lot (the aliasing was gone) but left jerk high, while an EMA over
    // the old estimator did the reverse (averaged the noise, but lagged
    // real corners badly). A regression slope does both jobs with one
    // knob: it is unbiased on a constant-rate ramp, it averages all N
    // samples rather than trusting two, and its only cost at a genuine
    // corner is about half the window in lag - the same cost the endpoint
    // version already paid, for far better noise rejection.
    unsigned long windowMs = (unsigned long)stepperPidFfWindowMsConfig;
    int newestIdx = (pidFfHistIdx - 1 + PID_FF_HIST_LEN) % PID_FF_HIST_LEN;
    float sumT = 0, sumY = 0;
    int n = 0;
    for (int back = 0; back < pidFfHistCount; back++) {
      int idx = (newestIdx - back + PID_FF_HIST_LEN) % PID_FF_HIST_LEN;
      unsigned long age = now - pidFfHist[idx].ms;
      if (back > 0 && age > windowMs) break;
      pidFfWork[n].t = -(float)age / 1000.0f;  // seconds, relative to now (<= 0)
      pidFfWork[n].y = pidFfHist[idx].target;
      sumT += pidFfWork[n].t;
      sumY += pidFfWork[n].y;
      n++;
    }
    if (n < 2) {
      pidFeedforwardVelocity = 0;  // not enough history yet - no rate to report
    } else {
      float meanT = sumT / n, meanY = sumY / n;
      float num = 0, den = 0;
      for (int i = 0; i < n; i++) {
        float dt2 = pidFfWork[i].t - meanT;
        num += dt2 * (pidFfWork[i].y - meanY);
        den += dt2 * dt2;
      }
      pidFeedforwardVelocity = (den > 1e-9f) ? (num / den) : 0;
    }
  }

  // Lookahead / reference-smoothing for the P-term only (2026-09-07
  // evening). The Kp sweep that picked Kp=3 (see stepperPidKpConfig's
  // declaration comment) proved the P-term's reaction to the raw,
  // jumpy target is the dominant driver of the ~120-150ms speed ripple -
  // lowering Kp reduced ripple because it reduces how hard the loop reacts
  // to that jumpiness, not because the jumpiness itself went away. This
  // attacks the jumpiness at its source instead: pTerm reacts to a plain
  // moving average of the target over the last stepperPidLookaheadMsConfig
  // ms, rather than the single newest sample. `error`/`target` above stay
  // completely untouched - the deadband/hysteresis/settle logic and the
  // overshoot safety net all need the TRUE current commanded value, not a
  // lagged one, to know when the trolley has actually arrived.
  //
  // Reuses pidFfHist (the feedforward estimator's own ring buffer of real
  // (target, timestamp) samples, populated unconditionally every tick just
  // above) rather than keeping separate state - no new buffer needed.
  // Deliberately just an average, not a synthetic forward integration: this
  // is what makes it safe in a way the shelved pidSmoothTarget layer wasn't.
  // That one integrated a velocity/position forward from feedforward, which
  // could drift arbitrarily far from reality while a pidStopSettling wait
  // blocked this whole function - the longer the wait, the bigger the
  // eventual catch-up, and the catch-up itself could provoke another stall.
  // Averaging real, already-received samples has no such state: if the loop
  // pauses and resumes, the very next tick's average simply reflects
  // whatever real history is on hand - nothing to have drifted while away.
  //
  // The added latency is real (roughly half the window, the standard cost
  // of a moving-average/FIR filter) and is spent deliberately, not
  // accidentally: the user's own stated priority is "a handful of frames
  // behind is fine, but always be smooth" - this trades a bounded, small
  // amount of exactly that currency for a smoother reference. Off by
  // default (0ms) for a clean A/B against pure Kp=3.
  float pTermTarget = target;
  if (stepperPidLookaheadMsConfig > 0) {
    unsigned long lookMs = (unsigned long)stepperPidLookaheadMsConfig;
    int newestIdx = (pidFfHistIdx - 1 + PID_FF_HIST_LEN) % PID_FF_HIST_LEN;
    float sum = 0;
    int n = 0;
    for (int back = 0; back < pidFfHistCount; back++) {
      int idx = (newestIdx - back + PID_FF_HIST_LEN) % PID_FF_HIST_LEN;
      if (back > 0 && (now - pidFfHist[idx].ms) > lookMs) break;
      sum += pidFfHist[idx].target;
      n++;
    }
    if (n > 0) pTermTarget = sum / n;
  }
  float pTermError = pTermTarget - (float)currentPos;

  if (fabs(error) <= (float)stepperPidDeadbandConfig) {
    if (!pidSettled) {
      int curPosBeforeMove = (int)currentPos;
      int32_t curSpeedBeforeMove = stepper->getCurrentSpeedInMilliHz();
      stepper->setAcceleration(stepperPidAccelConfig);
      // Restore the hardcoded jumpStart for this moveTo()-based settle -
      // see the runForward()/runBackward() call site below for why it's
      // disabled while actually tracking.
      stepper->setJumpStart(JUMP_START_STEPS);
      stepper->moveTo((int32_t)target);
      stepperBlanked = false;
      lastCommandedTargetPosition = target;
      logCompactMotion(positionRequest, (int)target, curPosBeforeMove, 0, (int)error,
                        true, curSpeedBeforeMove, 0);
      pidSettled = true;
      pidFilteredRate = 0;  // don't carry a stale rate estimate into the next engagement
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
      // Already settled - but the settle moveTo() above can go dead the
      // same way runForward()/runBackward() and the hysteresis band's own
      // moveTo() can (see lastPidHystRetryMs's declaration comment below):
      // a nonempty queue alone satisfies isRunning() without the ramp
      // generator ever actually producing a step. Genuinely low
      // consequence in this specific branch - the target was already
      // within deadband of the current position when this was entered, so
      // a dead move just means "stayed exactly where it already was,
      // which is still within deadband" - but added 2026-09-07 for
      // consistency with the hysteresis band's own retry rather than
      // leaving this one path unguarded. Retrying on an already-arrived,
      // perfectly fine stepper (genuinelyRunning reads false there too,
      // since it's just sitting still) is harmless: moveTo() to the
      // position it's already at is a trivial no-op, same accepted
      // tradeoff the hysteresis band already makes.
      static unsigned long lastPidDeadbandRetryMs = 0;
      bool deadbandGenuinelyRunning = stepper->isRunning() && stepper->getCurrentSpeedInMilliHz() != 0;
      if (!deadbandGenuinelyRunning && (now - lastPidDeadbandRetryMs >= 100)) {
        lastPidDeadbandRetryMs = now;
        stepper->setAcceleration(stepperPidAccelConfig);
        stepper->setJumpStart(JUMP_START_STEPS);
        stepper->moveTo((int32_t)target);
        stepperBlanked = false;
        lastCommandedTargetPosition = target;
      }
      // Still log every tick a real DDP value change occurs (2026-09-06), so the Compact
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
      stepper->setJumpStart(JUMP_START_STEPS);
      stepper->moveTo((int32_t)target);
      stepperBlanked = false;
      lastCommandedTargetPosition = target;
    }
    // Log every tick regardless of whether a fresh moveTo() was actually
    // issued this time, so the Compact Motion Log's ddpVal trace stays a
    // complete, gap-free record of positionRequest at PID's own tick rate -
    // see the deadband-settled branch above for the full "this was
    // mistaken for a DDP reception problem" story. Throttled (see
    // logCompactMotionPidThrottled's declaration comment) - "every tick"
    // meant every ~20ms when this comment was written; pidTickMs can now
    // be faster than that.
    logCompactMotionPidThrottled(positionRequest, (int)target, (int)currentPos, 0, (int)error,
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
  // Low-pass filter before it drives dTerm (2026-09-07, see
  // pidFilteredRate's declaration comment). 0.5/0.5 first tried and barely
  // moved the jerk metric (496.9 vs 513.5 baseline, within run-to-run
  // noise) - too weak a filter for the ~80-100ms-period resonance
  // characterized that session, at the then-hardcoded 20ms tick. 0.85/0.15
  // instead.
  //
  // Made independently configurable (stepperPidDFilterWeightConfig,
  // 2026-09-07 later) after re-validation against the new tick rate: this
  // is a per-SAMPLE EMA weight, not a per-unit-time one, so its effective
  // time constant scales inversely with tick rate - at tick=5ms (4x faster
  // than when 0.15 was chosen) the same weight covered roughly a quarter
  // of the original wall-clock smoothing window, weakening the filter's
  // intended effect without anyone deciding that. Swept 0.15 (as-shipped,
  // mismatched) / ~0.04 (time-constant-matched to the original ~123ms
  // tau: w = 1-exp(-tick/tau)) / 1.0 (off - raw derivative, no filtering)
  // against the standard p8 wave. See its declaration comment
  // (stepper_handler.h) for the result and why the default changed or
  // didn't.
  pidFilteredRate = (1.0f - stepperPidDFilterWeightConfig) * pidFilteredRate
                     + stepperPidDFilterWeightConfig * measuredRate;

  float pTerm = stepperPidKpConfig * pTermError;
  float dTerm = -stepperPidKdConfig * pidFilteredRate;
  // Feedforward carries the "known" bulk of the motion (see the
  // measurement block above); toggleable so it stays A/B-testable against
  // pure reactive PID with the same sweep tooling as every other change
  // this session.
  float ffTerm = stepperPidFeedforwardConfig ? pidFeedforwardVelocity : 0;

  // Output is simply feedforward plus the P/D trim. A slew-rate limiter on
  // the correction term was built and measured here (2026-09-07 evening) on
  // the theory that pTerm's tick-to-tick chop was the jerk source, and
  // removed again when the data said otherwise: limiting it to well below
  // the P-term's own step size changed jerk by less than run-to-run noise
  // (295 vs 310) while costing real accuracy (rms_error 318 vs 291) and
  // corner tightness (611 vs 482). The jerk was in the feedforward
  // estimator all along - see its measurement block above.
  float speed = ffTerm + pTerm + dTerm;
  if (speed > stepperPidMaxSpeedConfig) speed = (float)stepperPidMaxSpeedConfig;
  if (speed < -stepperPidMaxSpeedConfig) speed = -(float)stepperPidMaxSpeedConfig;

  // --- Directional containment guards -------------------------------------
  //
  // Both exist because of a real, damaging incident (2026-09-07 evening): the
  // trolley was driven 11,911 steps past the top switch and wound up the back
  // side of the pulley, destroying the zero reference. Root cause was the
  // pidOvershootLatched change made earlier the same evening: it correctly
  // stopped the safety net from re-firing every tick (which had caused a 3.5s
  // freeze), but left nothing preventing the control law from then driving
  // *further* out of bounds. Firing once and handing back control is only safe
  // if "back" is the sole direction still permitted.
  //
  // Guard 1 - position bounds. While outside [0, bottomPosition], permit only
  // motion that reduces the excursion. Costs nothing in normal operation
  // (curPos is in range, so neither branch fires) and makes the runaway
  // structurally impossible rather than merely unlikely.
  if (currentPos < 0 && speed < 0) speed = 0;
  if (currentPos > bottomPosition && speed > 0) speed = 0;

  // Guard 2 - the homing switch is authoritative, the step counter is not.
  // This is the one that would have caught the incident above even with a
  // lying counter, and it is the more important of the two: once the motor
  // stalls against a hard stop, FastAccelStepper keeps emitting steps and
  // getCurrentPosition() keeps counting them, so curPos becomes fiction and
  // every bounds check derived from it (including Guard 1) is comparing
  // against a number that no longer describes the physical world. The switch
  // is a direct physical measurement and stays true regardless. The switch
  // sits at the top of travel (position 0), so negative speed is "toward the
  // switch" - if it reads tripped, refuse to drive further that way no matter
  // what the counter claims. The log from the incident shows the switch
  // reading tripped for 22 rows while the counter ran on down to -11911.
  if (isHomingSwitchTripped() && speed < 0) speed = 0;

  int newDirection = (speed > 0) ? 1 : (speed < 0 ? -1 : 0);
  uint32_t speedHz = (uint32_t)fabs(speed);
  if (speedHz < 50) speedHz = 50;  // floor so runForward/runBackward always gets a sane nonzero speed, matches Streaming

  // A guard above zeroed the command: stop rather than fall through to the
  // run/applySpeedAcceleration() path below, which has no way to express
  // "no motion" (runForward()/runBackward() both mean "keep going", and the
  // 50Hz floor above would quietly turn a refusal into slow creep in the
  // very direction just refused).
  if (newDirection == 0) {
    if (stepper->isRunning()) {
      stepper->forceStop();
      pidStopSettling = true;
    }
    pidCurrentDirection = 0;
    continuousRunDirection = 0;
    logCompactMotionPidThrottled(positionRequest, (int)target, (int)currentPos, 0, (int)error,
                      true, stepper->getCurrentSpeedInMilliHz(), 0);
    return;
  }

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
  // Throttled (see logCompactMotionPidThrottled's declaration comment) -
  // this is the main active-tracking log call, reached on essentially
  // every tick while continuously moving, so it's the dominant contributor
  // to log row rate. At pidTickMs=5 this alone confirmed overflowing the
  // 96KB ring buffer in ~11s of continuous motion before this fix.
  logCompactMotionPidThrottled(positionRequest, (int)target, (int)currentPos, 0, (int)error,
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

  // Load TMC2209 UART configuration. tmcEnabled/tmcRSense/tmcAddress/
  // tmcHstrt/tmcHend are hardcoded (tmc_handler.cpp, 2026-09-07) and no
  // longer read from NVS - any value a device saved for those keys before
  // that change is now just an orphaned, unused key.
  tmcRunCurrentConfig = (uint16_t)preferences.getInt("tmcRunCurrent", 800);
  tmcHoldPercentConfig = (uint8_t)preferences.getInt("tmcHoldPercent", 50);
  tmcStallEnabledConfig = preferences.getBool("tmcStallEnabled", false);
  tmcStallThresholdConfig = (uint16_t)preferences.getInt("tmcStallThresh", 50);
  tmcMicrostepsConfig = (uint16_t)preferences.getInt("tmcMicrosteps", 16);

  // Initialize stepper
  initializeStepper();

  // Starts the Core 0 task, which calls initEncoder() itself (not here) so
  // the encoder's GPIO interrupt ends up Core-0-affine, physically
  // separated from FastAccelStepper's own Core-1-affine PCNT interrupt -
  // see core0_task.h.
  startCore0Task();

  // Initialize TMC2209 UART link (always attempted - see tmc_handler.cpp;
  // simply won't connect if the driver isn't wired for UART)
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

  // TMC2209 driver info (UART control is always enabled - see tmc_handler.cpp)
  Serial.println("\n--- TMC2209 Driver Status ---");
  if (!tmcConnected) {
    Serial.println("UART control: link FAILED (check wiring)");
  } else {
    Serial.println("UART control: Connected");
    Serial.print("Run Current: ");
    Serial.print(tmcRunCurrentConfig);
    Serial.print(" mA, Hold: ");
    Serial.print(tmcHoldPercentConfig);
    Serial.println("%");
    Serial.println("Chopper Mode: SpreadCycle");
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
