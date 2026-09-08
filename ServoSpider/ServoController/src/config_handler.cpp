#include "config_handler.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <string.h>
#include "wifi_handler.h"
#include "stepper_handler.h"
#include "protocol_common.h"
#include "tmc_handler.h"
#include "led_handler.h"

extern Preferences preferences;
extern WebServer server;

// One entry per persisted setting. `nvsKey` is the actual Preferences key
// (kept identical to every existing save handler's key, so this endpoint
// and the web UI's own per-page saves stay in sync with the same flash
// slots - see CLAUDE.md's 15-char NVS key limit note). `configKey` is the
// name used in the key=value text this endpoint reads/writes, which is
// allowed to be more descriptive since it isn't NVS-length-limited.
enum ConfigFieldType { CF_STRING, CF_INT, CF_BOOL, CF_FLOAT };

struct ConfigField {
  const char* configKey;
  ConfigFieldType type;
  bool sensitive;  // never included in GET output; optional, non-clobbering on POST
};

// Order matches the domain grouping in main.cpp's loadConfiguration()-style
// startup code and html_handler.cpp's handleSave*() functions.
static const ConfigField CONFIG_FIELDS[] = {
    {"ssid", CF_STRING, false},
    {"password", CF_STRING, true},
    {"hostname", CF_STRING, false},
    {"useStaticIp", CF_BOOL, false},
    {"staticIp", CF_STRING, false},
    {"staticGateway", CF_STRING, false},
    {"staticSubnet", CF_STRING, false},
    {"wifiRetryInt", CF_INT, false},
    {"apSsid", CF_STRING, false},
    {"apPassword", CF_STRING, true},
    {"apAppendMac", CF_BOOL, false},

    {"stepperSpeed", CF_INT, false},
    {"stepperAccel", CF_INT, false},
    {"autoHomeOnBoot", CF_BOOL, false},
    {"stepSpeedHome", CF_INT, false},
    {"stepAccelHome", CF_INT, false},
    {"stepTrackEn", CF_BOOL, false},
    {"stepTrackThresh", CF_INT, false},
    {"stepTrackSpeed", CF_INT, false},
    {"stepTrackAccel", CF_INT, false},
    {"stepTrackMaxLag", CF_INT, false},
    {"stepTrackMode", CF_INT, false},
    {"pidKp", CF_FLOAT, false},
    {"pidKd", CF_FLOAT, false},
    {"pidDFilterWt", CF_FLOAT, false},
    {"pidTickMs", CF_INT, false},
    {"pidLogMs", CF_INT, false},
    {"pidMaxSpeed", CF_INT, false},
    {"pidAccel", CF_INT, false},
    {"pidDeadband", CF_INT, false},
    {"pidReengageTh", CF_INT, false},
    {"pidFeedforward", CF_BOOL, false},
    {"pidFfWindowMs", CF_INT, false},
    {"pidLookaheadMs", CF_INT, false},

    {"protocol", CF_INT, false},
    {"stepperControl", CF_BOOL, false},
    {"control16Bit", CF_BOOL, false},
    {"protocolDebug", CF_BOOL, false},
    {"ledBlankTime", CF_INT, false},
    {"stepBlankTime", CF_INT, false},

    {"tmcRunCurrent", CF_INT, false},
    {"tmcHoldPercent", CF_INT, false},
    {"tmcStallEnabled", CF_BOOL, false},
    {"tmcStallThresh", CF_INT, false},
    {"tmcMicrosteps", CF_INT, false},

    {"ledPixelCount", CF_INT, false},
    {"ledColorOrder", CF_STRING, false},
    {"ledGamma", CF_FLOAT, false},
    {"ledBrightness", CF_INT, false},
    {"ledStartNull", CF_INT, false},
    {"ledEndNull", CF_INT, false},
};
static const int CONFIG_FIELD_COUNT = sizeof(CONFIG_FIELDS) / sizeof(CONFIG_FIELDS[0]);

// Reads a field's CURRENT in-RAM value (not NVS directly - the in-RAM
// config vars are always kept in sync with NVS by every existing save
// handler, so this reflects reality without a second round of flash
// reads). Returns "" for an unrecognized key (callers only call this for
// known keys from the table above, so that shouldn't happen in practice).
static String getFieldValue(const char* key) {
  if (!strcmp(key, "ssid")) return ssid;
  if (!strcmp(key, "password")) return password;
  if (!strcmp(key, "hostname")) return hostname;
  if (!strcmp(key, "useStaticIp")) return useStaticIp ? "1" : "0";
  if (!strcmp(key, "staticIp")) return staticIp;
  if (!strcmp(key, "staticGateway")) return staticGateway;
  if (!strcmp(key, "staticSubnet")) return staticSubnet;
  if (!strcmp(key, "wifiRetryInt")) return String(wifiRetryIntervalConfig);
  if (!strcmp(key, "apSsid")) return ap_ssid;
  if (!strcmp(key, "apPassword")) return ap_password;
  if (!strcmp(key, "apAppendMac")) return ap_append_mac ? "1" : "0";

  if (!strcmp(key, "stepperSpeed")) return String(stepperSpeedConfig);
  if (!strcmp(key, "stepperAccel")) return String(stepperAccelConfig);
  if (!strcmp(key, "autoHomeOnBoot")) return autoHomeOnBootConfig ? "1" : "0";
  if (!strcmp(key, "stepSpeedHome")) return String(stepperSpeedHomingConfig);
  if (!strcmp(key, "stepAccelHome")) return String(stepperAccelHomingConfig);
  if (!strcmp(key, "stepTrackEn")) return stepperTrackEnabledConfig ? "1" : "0";
  if (!strcmp(key, "stepTrackThresh")) return String(stepperTrackThresholdConfig);
  if (!strcmp(key, "stepTrackSpeed")) return String(stepperTrackSpeedConfig);
  if (!strcmp(key, "stepTrackAccel")) return String(stepperTrackAccelConfig);
  if (!strcmp(key, "stepTrackMaxLag")) return String(stepperTrackMaxLagConfig);
  if (!strcmp(key, "stepTrackMode")) return String(stepperTrackModeConfig);
  if (!strcmp(key, "pidKp")) return String(stepperPidKpConfig, 4);
  if (!strcmp(key, "pidKd")) return String(stepperPidKdConfig, 4);
  if (!strcmp(key, "pidDFilterWt")) return String(stepperPidDFilterWeightConfig, 4);
  if (!strcmp(key, "pidTickMs")) return String(stepperPidTickMsConfig);
  if (!strcmp(key, "pidLogMs")) return String(stepperPidLogMsConfig);
  if (!strcmp(key, "pidMaxSpeed")) return String(stepperPidMaxSpeedConfig);
  if (!strcmp(key, "pidAccel")) return String(stepperPidAccelConfig);
  if (!strcmp(key, "pidDeadband")) return String(stepperPidDeadbandConfig);
  if (!strcmp(key, "pidReengageTh")) return String(stepperPidReengageThresholdConfig);
  if (!strcmp(key, "pidFeedforward")) return stepperPidFeedforwardConfig ? "1" : "0";
  if (!strcmp(key, "pidFfWindowMs")) return String(stepperPidFfWindowMsConfig);
  if (!strcmp(key, "pidLookaheadMs")) return String(stepperPidLookaheadMsConfig);

  if (!strcmp(key, "protocol")) return String((int)protocolConfig);
  if (!strcmp(key, "stepperControl")) return stepperControlEnabled ? "1" : "0";
  if (!strcmp(key, "control16Bit")) return control16BitConfig ? "1" : "0";
  if (!strcmp(key, "protocolDebug")) return protocolDebugConfig ? "1" : "0";
  if (!strcmp(key, "ledBlankTime")) return String(ledBlankTimeConfig);
  if (!strcmp(key, "stepBlankTime")) return String(stepperBlankTimeConfig);

  if (!strcmp(key, "tmcRunCurrent")) return String(tmcRunCurrentConfig);
  if (!strcmp(key, "tmcHoldPercent")) return String(tmcHoldPercentConfig);
  if (!strcmp(key, "tmcStallEnabled")) return tmcStallEnabledConfig ? "1" : "0";
  if (!strcmp(key, "tmcStallThresh")) return String(tmcStallThresholdConfig);
  if (!strcmp(key, "tmcMicrosteps")) return String(tmcMicrostepsConfig);

  if (!strcmp(key, "ledPixelCount")) return String(ledPixelCount);
  if (!strcmp(key, "ledColorOrder")) return ledColorOrder;
  if (!strcmp(key, "ledGamma")) return String(ledGamma, 3);
  if (!strcmp(key, "ledBrightness")) return String(ledBrightness);
  if (!strcmp(key, "ledStartNull")) return String(ledStartNullPixels);
  if (!strcmp(key, "ledEndNull")) return String(ledEndNullPixels);

  return "";
}

// Applies one field's new value to the in-RAM config variable and writes
// it to NVS immediately - except stepper-domain keys, which the caller
// (handleConfigPost) instead marks via stepperSettingsPendingSave, for
// the same flash-write-during-active-motion reason handleSaveStepper()
// defers those (see its declaration comment).
static bool setFieldValue(const char* key, const String& value, bool& isStepperField) {
  isStepperField = false;
  bool b = (value == "1" || value == "true" || value == "on");
  long i = value.toInt();
  float f = value.toFloat();

  if (!strcmp(key, "ssid")) { ssid = value; preferences.putString("ssid", ssid); return true; }
  if (!strcmp(key, "password")) { password = value; preferences.putString("password", password); return true; }
  if (!strcmp(key, "hostname")) { hostname = value; preferences.putString("hostname", hostname); return true; }
  if (!strcmp(key, "useStaticIp")) { useStaticIp = b; preferences.putBool("useStaticIp", useStaticIp); return true; }
  if (!strcmp(key, "staticIp")) { staticIp = value; preferences.putString("staticIp", staticIp); return true; }
  if (!strcmp(key, "staticGateway")) { staticGateway = value; preferences.putString("staticGateway", staticGateway); return true; }
  if (!strcmp(key, "staticSubnet")) { staticSubnet = value; preferences.putString("staticSubnet", staticSubnet); return true; }
  if (!strcmp(key, "wifiRetryInt")) { wifiRetryIntervalConfig = i; preferences.putInt("wifiRetryInt", wifiRetryIntervalConfig); return true; }
  if (!strcmp(key, "apSsid")) { ap_ssid = value; preferences.putString("apSsid", ap_ssid); return true; }
  if (!strcmp(key, "apPassword")) { ap_password = value; preferences.putString("apPassword", ap_password); return true; }
  if (!strcmp(key, "apAppendMac")) { ap_append_mac = b; preferences.putBool("apAppendMac", ap_append_mac); return true; }

  if (!strcmp(key, "stepperSpeed")) { stepperSpeedConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "stepperAccel")) { stepperAccelConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "autoHomeOnBoot")) { autoHomeOnBootConfig = b; isStepperField = true; return true; }
  if (!strcmp(key, "stepSpeedHome")) { stepperSpeedHomingConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "stepAccelHome")) { stepperAccelHomingConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "stepTrackEn")) { stepperTrackEnabledConfig = b; isStepperField = true; return true; }
  if (!strcmp(key, "stepTrackThresh")) { stepperTrackThresholdConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "stepTrackSpeed")) { stepperTrackSpeedConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "stepTrackAccel")) { stepperTrackAccelConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "stepTrackMaxLag")) { stepperTrackMaxLagConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "stepTrackMode")) { stepperTrackModeConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "pidKp")) { stepperPidKpConfig = f; isStepperField = true; return true; }
  if (!strcmp(key, "pidKd")) { stepperPidKdConfig = f; isStepperField = true; return true; }
  if (!strcmp(key, "pidDFilterWt")) { stepperPidDFilterWeightConfig = f; isStepperField = true; return true; }
  if (!strcmp(key, "pidTickMs")) { stepperPidTickMsConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "pidLogMs")) { stepperPidLogMsConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "pidMaxSpeed")) { stepperPidMaxSpeedConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "pidAccel")) { stepperPidAccelConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "pidDeadband")) { stepperPidDeadbandConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "pidReengageTh")) { stepperPidReengageThresholdConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "pidFeedforward")) { stepperPidFeedforwardConfig = b; isStepperField = true; return true; }
  if (!strcmp(key, "pidFfWindowMs")) { stepperPidFfWindowMsConfig = i; isStepperField = true; return true; }
  if (!strcmp(key, "pidLookaheadMs")) { stepperPidLookaheadMsConfig = i; isStepperField = true; return true; }

  if (!strcmp(key, "protocol")) { protocolConfig = (protocolType)i; preferences.putInt("protocol", (int)protocolConfig); return true; }
  if (!strcmp(key, "stepperControl")) { stepperControlEnabled = b; preferences.putBool("stepperControl", stepperControlEnabled); return true; }
  if (!strcmp(key, "control16Bit")) { control16BitConfig = b; preferences.putBool("control16Bit", control16BitConfig); return true; }
  if (!strcmp(key, "protocolDebug")) { protocolDebugConfig = b; preferences.putBool("protocolDebug", protocolDebugConfig); return true; }
  if (!strcmp(key, "ledBlankTime")) { ledBlankTimeConfig = i; preferences.putInt("ledBlankTime", ledBlankTimeConfig); return true; }
  if (!strcmp(key, "stepBlankTime")) { stepperBlankTimeConfig = i; preferences.putInt("stepBlankTime", stepperBlankTimeConfig); return true; }

  if (!strcmp(key, "tmcRunCurrent")) { tmcRunCurrentConfig = (uint16_t)i; preferences.putInt("tmcRunCurrent", tmcRunCurrentConfig); return true; }
  if (!strcmp(key, "tmcHoldPercent")) { tmcHoldPercentConfig = (uint8_t)i; preferences.putInt("tmcHoldPercent", tmcHoldPercentConfig); return true; }
  if (!strcmp(key, "tmcStallEnabled")) { tmcStallEnabledConfig = b; preferences.putBool("tmcStallEnabled", tmcStallEnabledConfig); return true; }
  if (!strcmp(key, "tmcStallThresh")) { tmcStallThresholdConfig = (uint16_t)i; preferences.putInt("tmcStallThresh", tmcStallThresholdConfig); return true; }
  if (!strcmp(key, "tmcMicrosteps")) { tmcMicrostepsConfig = (uint16_t)i; preferences.putInt("tmcMicrosteps", tmcMicrostepsConfig); return true; }

  if (!strcmp(key, "ledPixelCount")) { ledPixelCount = i; preferences.putInt("ledPixelCount", ledPixelCount); return true; }
  if (!strcmp(key, "ledColorOrder")) { ledColorOrder = value; preferences.putString("ledColorOrder", ledColorOrder); return true; }
  if (!strcmp(key, "ledGamma")) { ledGamma = f; preferences.putFloat("ledGamma", ledGamma); return true; }
  if (!strcmp(key, "ledBrightness")) { ledBrightness = i; preferences.putInt("ledBrightness", ledBrightness); return true; }
  if (!strcmp(key, "ledStartNull")) { ledStartNullPixels = i; preferences.putInt("ledStartNull", ledStartNullPixels); return true; }
  if (!strcmp(key, "ledEndNull")) { ledEndNullPixels = i; preferences.putInt("ledEndNull", ledEndNullPixels); return true; }

  return false;
}

void handleConfigGet() {
  String body;
  body.reserve(1600);
  for (int i = 0; i < CONFIG_FIELD_COUNT; i++) {
    if (CONFIG_FIELDS[i].sensitive) continue;  // never export passwords
    body += CONFIG_FIELDS[i].configKey;
    body += '=';
    body += getFieldValue(CONFIG_FIELDS[i].configKey);
    body += '\n';
  }
  server.send(200, "text/plain", body);
}

void handleConfigPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"missing request body\"}");
    return;
  }
  String bodyText = server.arg("plain");

  int applied = 0;
  bool anyStepperField = false;
  String unknownKeys;
  int pos = 0;
  while (pos < (int)bodyText.length()) {
    int nl = bodyText.indexOf('\n', pos);
    if (nl < 0) nl = bodyText.length();
    String line = bodyText.substring(pos, nl);
    pos = nl + 1;
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();

    bool isStepperField = false;
    if (setFieldValue(key.c_str(), value, isStepperField)) {
      applied++;
      if (isStepperField) anyStepperField = true;
    } else {
      if (unknownKeys.length() > 0) unknownKeys += ",";
      unknownKeys += key;
    }
  }

  // Same deferred-write mechanism handleSaveStepper() uses - a Preferences
  // write briefly disables the flash cache, which is a real crash risk if
  // it lands while FastAccelStepper's step-generation interrupt is firing
  // (see stepperSettingsPendingSave's declaration comment).
  if (anyStepperField) {
    stepperSettingsPendingSave = true;
  }

  String msg = "{\"success\":true,\"applied\":" + String(applied) +
               ",\"unknownKeys\":\"" + unknownKeys + "\"" +
               ",\"message\":\"Config applied" +
               (anyStepperField ? " (stepper settings will finish saving to flash once the motor is idle)" : "") +
               " - reboot recommended so WiFi/TMC/LED changes fully re-initialize.\"}";
  server.send(200, "application/json", msg);
}
