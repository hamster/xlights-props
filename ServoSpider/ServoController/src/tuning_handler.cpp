#include "tuning_handler.h"
#include "stepper_handler.h"
#include "protocol_common.h"

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
  return false;
}

static const char* ALL_TUNABLE_NAMES[] = {
  "normalSpeed", "normalAccel", "jumpStart", "trackEnabled", "trackThreshold",
  "trackSpeed", "trackAccel", "trackMaxLag", "trackMode", "coalesceMs",
  "coalesceSteps", "streamRateWindow", "streamSettle", "compactLog", "protocolDebug",
  "homeSpeed", "homeAccel", "lookaheadSteps", "lookaheadSettle"
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
  if (name == "trackMode") { stepperTrackModeConfig = v; return true; }
  if (name == "coalesceMs") { stepperCoalesceMsConfig = v; return true; }
  if (name == "coalesceSteps") { stepperCoalesceStepsConfig = v; return true; }
  if (name == "streamRateWindow") { stepperStreamRateWindowMsConfig = v; return true; }
  if (name == "streamSettle") { stepperStreamSettleMsConfig = v; return true; }
  if (name == "compactLog") { compactLogEnabled = (v != 0); return true; }
  if (name == "protocolDebug") { protocolDebugConfig = (v != 0); return true; }
  // Applied fresh by startHoming()/HOMING_WAIT_CLEAR_SWITCH each time a
  // search actually begins, so just updating the config var here is enough -
  // no immediate stepper call needed (matches every other tunable above).
  if (name == "homeSpeed") { stepperSpeedHomingConfig = v; return true; }
  if (name == "homeAccel") { stepperAccelHomingConfig = v; return true; }
  if (name == "lookaheadSteps") { stepperLookaheadStepsConfig = v; return true; }
  if (name == "lookaheadSettle") { stepperLookaheadSettleMsConfig = v; return true; }
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
    Serial.println(stepperTrackModeConfig);
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

  Serial.print("ERR unrecognized command: ");
  Serial.println(line);
}
