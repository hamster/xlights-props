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

// Temporary compact CSV motion log, independent of protocolDebugConfig - see
// compactLogEnabled's declaration comment in protocol_common.h. Shared by all
// three TRACK_MODE_* strategies so the log stays useful regardless of mode.
void logCompactMotion(uint16_t ddpVal, int cmdPos, int curPos, int delta, int lag,
                       bool tracking, int32_t curSpeedMilliHz, int targetSpeedHz) {
  if (!compactLogEnabled) return;
  Serial.print(millis());
  Serial.print(",");
  Serial.print(ddpVal);
  Serial.print(",");
  Serial.print(cmdPos);
  Serial.print(",");
  Serial.print(curPos);
  Serial.print(",");
  Serial.print(delta);
  Serial.print(",");
  Serial.print(lag);
  Serial.print(",");
  Serial.print(tracking ? "T" : "N");
  Serial.print(",");
  Serial.print(curSpeedMilliHz / 1000);
  Serial.print(",");
  Serial.print(targetSpeedHz);
  Serial.print(",");
  // Ground-truth encoder position alongside the step-counted curPos above -
  // 0 if no encoder is wired/initialized, so existing logs without an
  // encoder still parse the same way, just with this column always 0.
  Serial.println(getEncoderCount());
}

// Periodic compact-log tick, independent of the DDP/tracking-mode dispatch
// below - Direct and Coalesce only log once at the moment a move is
// committed, so a single large move (a big DDP jump, or a diagnostic
// moveTo() like $CHECKSTEPS's that never goes through this dispatch at
// all) previously produced only one data point instead of a full
// speed-over-time trace. Streaming mode already logs every ~20ms on its
// own (updateStreamingMode()), so this is skipped there to avoid
// duplicate/conflicting rows. Call every loop() iteration; rate-limited
// internally.
unsigned long lastPeriodicLogMs = 0;
void logCompactMotionPeriodic() {
  if (!compactLogEnabled || stepperTrackModeConfig == TRACK_MODE_STREAMING) return;
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

    stepper->moveTo((int)streamRawTarget);
    stepperBlanked = false;
    streamSettled = true;
    streamCurrentDirection = 0;
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
    if (newDirection > 0) {
      stepper->runForward();
    } else if (newDirection < 0) {
      stepper->runBackward();
    } else {
      stepper->forceStop();
      streamStopSettling = true;
    }
    streamCurrentDirection = newDirection;
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
      Serial.println("$  - Extended tuning-harness command (see tuning_handler.h)");
      Serial.println("================================\n");
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
