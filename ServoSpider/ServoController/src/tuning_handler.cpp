#include "tuning_handler.h"
#include "stepper_handler.h"
#include "protocol_common.h"
#include "encoder_handler.h"
#include "encoder_diag.h"
#include "core0_task.h"
#include "tmc_handler.h"

// Splits `line` on spaces into up to 3 tokens (command, name, value).
// Returns the number of tokens found. Good enough for this simple protocol -
// no quoting/escaping needed since values are always plain numbers.
static int tokenize(const String& line, String tokens[3]) {
  int count = 0;
  int start = 0;
  int len = line.length();
  while (start < len && count < 3) {
    while (start < len && line[start] == ' ') start++;
    if (start >= len) break;
    int end = start;
    while (end < len && line[end] != ' ') end++;
    tokens[count++] = line.substring(start, end);
    start = end;
  }
  return count;
}

// Returns true and writes the value if `name` matches a known tunable;
// false (with no output) if the name is unrecognized.
static bool getTunable(const String& name, String& valueOut) {
  if (name == "normalSpeed") { valueOut = String(stepperSpeedConfig); return true; }
  if (name == "normalAccel") { valueOut = String(stepperAccelConfig); return true; }
  if (name == "jumpStart") { valueOut = String(jumpStartConfig); return true; }
  if (name == "trackEnabled") { valueOut = String(stepperTrackEnabledConfig ? 1 : 0); return true; }
  if (name == "trackThreshold") { valueOut = String(stepperTrackThresholdConfig); return true; }
  if (name == "trackSpeed") { valueOut = String(stepperTrackSpeedConfig); return true; }
  if (name == "trackAccel") { valueOut = String(stepperTrackAccelConfig); return true; }
  if (name == "trackMaxLag") { valueOut = String(stepperTrackMaxLagConfig); return true; }
  if (name == "trackMode") { valueOut = String(stepperTrackModeConfig); return true; }
  if (name == "coalesceMs") { valueOut = String(stepperCoalesceMsConfig); return true; }
  if (name == "coalesceSteps") { valueOut = String(stepperCoalesceStepsConfig); return true; }
  if (name == "streamRateWindow") { valueOut = String(stepperStreamRateWindowMsConfig); return true; }
  if (name == "streamSettle") { valueOut = String(stepperStreamSettleMsConfig); return true; }
  if (name == "compactLog") { valueOut = String(compactLogEnabled ? 1 : 0); return true; }
  if (name == "protocolDebug") { valueOut = String(protocolDebugConfig ? 1 : 0); return true; }
  if (name == "homeSpeed") { valueOut = String(stepperSpeedHomingConfig); return true; }
  if (name == "homeAccel") { valueOut = String(stepperAccelHomingConfig); return true; }
  if (name == "lookaheadSteps") { valueOut = String(stepperLookaheadStepsConfig); return true; }
  if (name == "lookaheadSettle") { valueOut = String(stepperLookaheadSettleMsConfig); return true; }
  // TRACK_MODE_PID - Kp/Ki/Kd are floats, unlike every other tunable here
  // (4 decimal places - these are small per-step/per-second gains where
  // integer precision would be far too coarse to tune usefully).
  if (name == "pidKp") { valueOut = String(stepperPidKpConfig, 4); return true; }
  if (name == "pidKi") { valueOut = String(stepperPidKiConfig, 4); return true; }
  if (name == "pidKd") { valueOut = String(stepperPidKdConfig, 4); return true; }
  if (name == "pidMaxSpeed") { valueOut = String(stepperPidMaxSpeedConfig); return true; }
  if (name == "pidAccel") { valueOut = String(stepperPidAccelConfig); return true; }
  if (name == "pidDeadband") { valueOut = String(stepperPidDeadbandConfig); return true; }
  // RAM-only live current control for bench sweeps (current-vs-speed
  // characterization) - see setTunable()'s handling of this name for why
  // it's kept separate from /save-tmc's full-settings path.
  if (name == "tmcRunCurrent") { valueOut = String(tmcRunCurrentConfig); return true; }
  // Same rationale as tmcRunCurrent - a plain RAM boolean gate (no register
  // write needed, unlike current/microsteps/etc.), so toggling it here is
  // trivially safe and avoids /save-tmc's full-form reconstruction risk -
  // added after that risk turned from theoretical to real (2026-09-06): a
  // web UI save re-enabled StallGuard as a side effect of an unrelated
  // settings change, silently reintroducing the false-positive stalls it
  // was deliberately disabled for earlier this session.
  if (name == "tmcStallEnabled") { valueOut = String(tmcStallEnabledConfig ? 1 : 0); return true; }
  // Read-only (no setTunable() branch - a fake position command bypassing
  // real DDP parsing would be misleading, not useful). Added 2026-09-06 for
  // verifying a UDP DDP packet actually landed from a bench test script -
  // DDP has no ack, so a lost packet otherwise silently leaves this stale,
  // corrupting whatever step-response test assumed it just changed. Lighter
  // than polling /status-data's equivalent (protocolLastCommand) over HTTP,
  // which competes with the same single-threaded WebServer everything else
  // on the device shares.
  if (name == "positionRequest") { valueOut = String(positionRequest); return true; }
  return false;
}

static const char* ALL_TUNABLE_NAMES[] = {
  "normalSpeed", "normalAccel", "jumpStart", "trackEnabled", "trackThreshold",
  "trackSpeed", "trackAccel", "trackMaxLag", "trackMode", "coalesceMs",
  "coalesceSteps", "streamRateWindow", "streamSettle", "compactLog", "protocolDebug",
  "homeSpeed", "homeAccel", "lookaheadSteps", "lookaheadSettle",
  "pidKp", "pidKi", "pidKd", "pidMaxSpeed", "pidAccel", "pidDeadband", "tmcRunCurrent",
  "tmcStallEnabled", "positionRequest"
};
static const int ALL_TUNABLE_COUNT = sizeof(ALL_TUNABLE_NAMES) / sizeof(ALL_TUNABLE_NAMES[0]);

// Returns true if `name` was recognized and applied; false (with no side
// effect) if unrecognized. RAM-only - deliberately does not touch
// Preferences/flash, so rapid iterative tuning doesn't wear the flash and
// doesn't require the motor to be idle (unlike the web UI's save path).
static bool setTunable(const String& name, const String& valueStr) {
  long v = valueStr.toInt();
  if (name == "normalSpeed") { stepperSpeedConfig = v; stepper->setSpeedInHz(stepperSpeedConfig); return true; }
  if (name == "normalAccel") { stepperAccelConfig = v; stepper->setAcceleration(stepperAccelConfig); return true; }
  if (name == "jumpStart") { jumpStartConfig = v; stepper->setJumpStart(jumpStartConfig); return true; }
  if (name == "trackEnabled") { stepperTrackEnabledConfig = (v != 0); return true; }
  if (name == "trackThreshold") { stepperTrackThresholdConfig = v; return true; }
  if (name == "trackSpeed") { stepperTrackSpeedConfig = v; return true; }
  if (name == "trackAccel") { stepperTrackAccelConfig = v; return true; }
  if (name == "trackMaxLag") { stepperTrackMaxLagConfig = v; return true; }
  // Reset regardless of which mode we're leaving/entering - a stale
  // nonzero value left over from Streaming/PID would make updateHoming()'s
  // switch-trip bounce filter (see continuousRunDirection's declaration
  // comment) wrongly excuse a real trip in whatever mode comes next.
  if (name == "trackMode") { stepperTrackModeConfig = v; continuousRunDirection = 0; return true; }
  if (name == "coalesceMs") { stepperCoalesceMsConfig = v; return true; }
  if (name == "coalesceSteps") { stepperCoalesceStepsConfig = v; return true; }
  if (name == "streamRateWindow") { stepperStreamRateWindowMsConfig = v; return true; }
  if (name == "streamSettle") { stepperStreamSettleMsConfig = v; return true; }
  if (name == "compactLog") { compactLogEnabled = (v != 0); return true; }
  if (name == "protocolDebug") { protocolDebugConfig = (v != 0); return true; }
  // Applied fresh by startHoming()/HOMING_SETTLE each time a search
  // actually begins, so just updating the config var here is enough - no
  // immediate stepper call needed (matches every other tunable above).
  if (name == "homeSpeed") { stepperSpeedHomingConfig = v; return true; }
  if (name == "homeAccel") { stepperAccelHomingConfig = v; return true; }
  if (name == "lookaheadSteps") { stepperLookaheadStepsConfig = v; return true; }
  if (name == "lookaheadSettle") { stepperLookaheadSettleMsConfig = v; return true; }
  // TRACK_MODE_PID - Kp/Ki/Kd parsed as floats (toInt()'s `v` above would
  // truncate a gain like 1.5 to 1), everything else uses the shared int `v`.
  if (name == "pidKp") { stepperPidKpConfig = valueStr.toFloat(); return true; }
  if (name == "pidKi") { stepperPidKiConfig = valueStr.toFloat(); return true; }
  if (name == "pidKd") { stepperPidKdConfig = valueStr.toFloat(); return true; }
  if (name == "pidMaxSpeed") { stepperPidMaxSpeedConfig = v; return true; }
  if (name == "pidAccel") { stepperPidAccelConfig = v; return true; }
  if (name == "pidDeadband") { stepperPidDeadbandConfig = v; return true; }
  // Deliberately bypasses /save-tmc entirely: that handler writes all 14 TMC
  // Preferences keys to flash on every call (real wear across a long sweep)
  // and reconstructs every other TMC field from HTTP form args, where an
  // absent checkbox arg (tmcEnabled/tmcStallEnabled/tmcPwmAutograd) reads as
  // false - a script posting only tmcRunCurrent would silently disable the
  // UART link and StallGuard as a side effect. Setting the config global
  // directly and calling applyTmcSettings() re-applies the *current*
  // in-memory value of every other TMC field (untouched here) alongside the
  // new current - same "RAM-only, no flash wear, no side effects on
  // anything else" contract every other tunable in this file already has.
  if (name == "tmcRunCurrent") { tmcRunCurrentConfig = v; applyTmcSettings(); return true; }
  // Plain RAM boolean gate (checked directly in tmc_handler.cpp's stall
  // cutoff, no register write involved) - safe to flip with no other side
  // effects, unlike a /save-tmc round trip. Added 2026-09-06 after a web UI
  // save for an unrelated setting silently re-enabled StallGuard (an absent
  // checkbox arg there means false, so it must have been checked when that
  // form was submitted) - reintroducing the false-positive stalls this was
  // deliberately disabled for earlier the same session.
  if (name == "tmcStallEnabled") { tmcStallEnabledConfig = (v != 0); return true; }
  return false;
}

void handleExtendedSerialCommand(const String& line) {
  String tokens[3];
  int n = tokenize(line, tokens);
  if (n == 0) {
    return;
  }
  String cmd = tokens[0];
  cmd.toUpperCase();

  if (cmd == "SET" && n == 3) {
    if (setTunable(tokens[1], tokens[2])) {
      String v;
      getTunable(tokens[1], v);
      Serial.print("OK ");
      Serial.print(tokens[1]);
      Serial.print("=");
      Serial.println(v);
    } else {
      Serial.print("ERR unknown name ");
      Serial.println(tokens[1]);
    }
    return;
  }

  if (cmd == "GET" && n == 2) {
    if (tokens[1] == "ALL") {
      for (int i = 0; i < ALL_TUNABLE_COUNT; i++) {
        String v;
        getTunable(String(ALL_TUNABLE_NAMES[i]), v);
        Serial.print("VAL ");
        Serial.print(ALL_TUNABLE_NAMES[i]);
        Serial.print("=");
        Serial.println(v);
      }
      Serial.println("OK ALL");
      return;
    }
    String v;
    if (getTunable(tokens[1], v)) {
      Serial.print("VAL ");
      Serial.print(tokens[1]);
      Serial.print("=");
      Serial.println(v);
    } else {
      Serial.print("ERR unknown name ");
      Serial.println(tokens[1]);
    }
    return;
  }

  if (cmd == "STATUS" && n == 1) {
    Serial.print("STATUS homed=");
    Serial.print(homed ? 1 : 0);
    Serial.print(" homing=");
    Serial.print(isHoming() ? 1 : 0);
    Serial.print(" checking=");
    Serial.print(isStepChecking() ? 1 : 0);
    Serial.print(" pos=");
    Serial.print(stepper->getCurrentPosition());
    Serial.print(" bottom=");
    Serial.print(bottomPosition);
    Serial.print(" running=");
    Serial.print(stepper->isRunning() ? 1 : 0);
    Serial.print(" mode=");
    Serial.print(stepperTrackModeConfig);
    Serial.print(" encoder=");
    Serial.print(getEncoderCount());
    Serial.print(" encoderMissed=");
    Serial.print(getMissedTransitionCount());
    Serial.print(" core0MaxGapMs=");
    Serial.print(getCore0TaskMaxGapMs());
    Serial.print(" core0MinStack=");
    Serial.println(getCore0TaskMinStackBytes());
    return;
  }

  if (cmd == "HOME" && n == 1) {
    startHoming();
    Serial.println("OK HOME");
    return;
  }

  if (cmd == "CHECKSTEPS" && n <= 2) {
    if (isHoming()) {
      Serial.println("ERR homing in progress");
      return;
    }
    if (isStepChecking()) {
      Serial.println("ERR check already in progress");
      return;
    }
    if (!homed) {
      Serial.println("ERR not homed - run $HOME first");
      return;
    }
    if (stepper->isRunning()) {
      Serial.println("ERR stepper is moving - wait for it to settle first");
      return;
    }
    long target = 0;
    if (n == 2) {
      target = tokens[1].toInt();
    }
    startStepCheck(target);
    Serial.print("OK CHECKSTEPS started target=");
    Serial.println(target);
    return;
  }

  if (cmd == "ENCDIAG" && n == 1) {
    startEncoderDiag();
    return;
  }

  Serial.print("ERR unrecognized command: ");
  Serial.println(line);
}
