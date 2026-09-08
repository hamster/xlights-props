#include "html_handlers.h"
#include "html.h"
#include "version.h"
#include "wifi_handler.h"
#include "stepper_handler.h"
#include "ddp_handler.h"
#include "ota_handler.h"
#include "led_handler.h"
#include "protocol_common.h"
#include "tmc_handler.h"
#include "encoder_handler.h"
#include "persist_log.h"
#include "tuning_handler.h"
#include "config_handler.h"
#include "main.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <ESPmDNS.h>

// Web server
WebServer server(80);

// External preferences object
extern Preferences preferences;

// Wraps handleRoot() for server.onNotFound() specifically to log what was
// actually requested (2026-09-08, added while debugging a real "page loads
// blank over the AP" report) - the WebServer library's own
// log_e("request handler not found") right before this fires gives no clue
// what URI/method it was, only that some request matched no exact route.
// Every such request still falls through to this - same behavior as before,
// just with real visibility into what's landing here now.
void handleNotFound() {
  Serial.print("No exact route matched (");
  Serial.print(server.method() == HTTP_GET ? "GET" : server.method() == HTTP_POST ? "POST" : "other");
  Serial.print(") ");
  Serial.print(server.uri());
  Serial.print(" from ");
  Serial.print(server.client().remoteIP());
  Serial.println(" - serving the main page as fallback.");
  handleRoot();
}

void handleRoot() {
  // Debug logging (2026-09-08) - added while chasing a real "page loads
  // blank over the AP" report with no other lead: handleRoot() itself
  // never logged anything, so there was no way to tell from serial whether
  // it was even being reached for a real "/" request (vs. only ever seeing
  // background probe requests like /generate204), whether it completed, or
  // whether the device crashed/rebooted partway through building or
  // sending this ~74KB page - which would explain "background color
  // renders, body doesn't" exactly (the connection dying right after the
  // early <style> block but before the body content finishes).
  unsigned long handleRootStartMs = millis();
  Serial.print("handleRoot() called from ");
  Serial.print(server.client().remoteIP());
  Serial.print(", free heap ");
  Serial.println(ESP.getFreeHeap());

  // Root cause, found 2026-09-08 via temporary length-checkpoint logging
  // (removed once confirmed fixed): the very first *growing* replacement
  // (one where the
  // replacement text is longer than the {{PLACEHOLDER}} it replaces - the
  // WiFi section's {{WIFI_MODE}} -> "Access Point Mode" is the first one)
  // needs String::replace() to grow the buffer (WString.cpp's changeBuffer()
  // calls realloc() on the *existing* buffer) - fine if the allocator can
  // extend in place, but if it can't (heap fragmentation), realloc() has to
  // allocate an entirely new block and copy, needing the old ~70KB buffer
  // and a new, larger one alive simultaneously. That's fine over a normal
  // STA connection, but with the SoftAP + captive-portal DNS server +
  // (during a retry) a concurrent WiFi.begin() all also holding heap, free
  // contiguous space in AP mode was consistently too fragmented for that.
  // First attempt at fixing this (page.reserve() called *after*
  // `String page = String(htmlPage)` had already copied the full ~70KB in)
  // didn't help - it hit the exact same problem one line earlier, growing
  // an already-full buffer. The actual fix has to reserve capacity while
  // `page` is still empty (nothing allocated yet, so reserve() is a single
  // clean malloc() for the full target size - see WString.cpp's reserve(),
  // which short-circuits to a no-op once capacity() is already big enough),
  // *then* copy htmlPage's content in - String::copy()'s own internal
  // reserve() call becomes a no-op too, so the whole build never needs more
  // than one buffer alive at a time, at any point in this function.
  String page;
  page.reserve(strlen(htmlPage) + 8192);
  page = htmlPage;

  // WiFi status information
  if (WiFi.status() == WL_CONNECTED) {
    page.replace("{{STATUS_CLASS}}", "connected");
    page.replace("{{STATUS_TEXT}}", "Connected");
    page.replace("{{WIFI_MODE}}", "Client Mode");
    page.replace("{{WIFI_NETWORK}}", WiFi.SSID());
    page.replace("{{WIFI_IP}}", WiFi.localIP().toString());
  } else {
    page.replace("{{STATUS_CLASS}}", "disconnected");
    page.replace("{{STATUS_TEXT}}", "Not Connected");
    page.replace("{{WIFI_MODE}}", "Access Point Mode");
    page.replace("{{WIFI_NETWORK}}", getAPName());
    page.replace("{{WIFI_IP}}", WiFi.softAPIP().toString());
  }

  // Stepper status information
  if (homed) {
    page.replace("{{HOMED_CLASS}}", "homed");
    page.replace("{{HOMED_TEXT}}", "Homed");
  } else {
    page.replace("{{HOMED_CLASS}}", "not-homed");
    page.replace("{{HOMED_TEXT}}", "Not Homed");
  }

  int currentPosition = stepper->getCurrentPosition();
  float positionPercent = (bottomPosition > 0) ? ((float)currentPosition / (float)bottomPosition * 100.0) : 0;

  page.replace("{{CURRENT_POSITION}}", String(currentPosition));
  page.replace("{{POSITION_PERCENT}}", String(positionPercent, 1));
  page.replace("{{BOTTOM_POSITION}}", String(bottomPosition));
  if (homingTravelValid && homingTravelMs > 0) {
    float stepsPerSec = (float)homingTravelSteps * 1000.0f / (float)homingTravelMs;
    page.replace("{{HOMING_TRAVEL}}", String(homingTravelSteps) + " steps in " +
      String(homingTravelMs / 1000.0f, 1) + "s (~" + String(stepsPerSec, 0) + " steps/s)");
  } else {
    page.replace("{{HOMING_TRAVEL}}", "Not yet measured");
  }
  page.replace("{{AUTO_HOME_STATUS}}", autoHomeOnBootConfig ? "Enabled" : "Disabled");
  page.replace("{{ENCODER_COUNT}}", isEncoderInitialized() ?
    String(getEncoderCount()) + " (" + String(getMissedTransitionCount()) + " missed)" : "N/A");

  // Configuration values
  page.replace("{{HOSTNAME}}", hostname);
  page.replace("{{CURRENT_SSID}}", ssid);
  page.replace("{{DHCP_CHECKED}}", useStaticIp ? "" : "checked");
  page.replace("{{STATIC_CHECKED}}", useStaticIp ? "checked" : "");
  page.replace("{{STATIC_IP}}", staticIp);
  page.replace("{{STATIC_GATEWAY}}", staticGateway);
  page.replace("{{STATIC_SUBNET}}", staticSubnet);
  page.replace("{{WIFI_RETRY_INTERVAL}}", String(wifiRetryIntervalConfig));
  page.replace("{{AP_SSID}}", ap_ssid);
  page.replace("{{AP_PASSWORD}}", ap_password);
  page.replace("{{AP_APPEND_MAC_CHECKED}}", ap_append_mac ? "checked" : "");
  page.replace("{{STEPPER_SPEED}}", String(stepperSpeedConfig));
  page.replace("{{STEPPER_ACCEL}}", String(stepperAccelConfig));
  page.replace("{{AUTO_HOME_ON_BOOT_CHECKED}}", autoHomeOnBootConfig ? "checked" : "");
  page.replace("{{STEPPER_SPEED_HOMING}}", String(stepperSpeedHomingConfig));
  page.replace("{{STEPPER_ACCEL_HOMING}}", String(stepperAccelHomingConfig));
  page.replace("{{STEPPER_TRACK_ENABLED_CHECKED}}", stepperTrackEnabledConfig ? "checked" : "");
  page.replace("{{STEPPER_TRACK_THRESHOLD}}", String(stepperTrackThresholdConfig));
  page.replace("{{STEPPER_TRACK_SPEED}}", String(stepperTrackSpeedConfig));
  page.replace("{{STEPPER_TRACK_ACCEL}}", String(stepperTrackAccelConfig));
  page.replace("{{STEPPER_TRACK_MAX_LAG}}", String(stepperTrackMaxLagConfig));
  page.replace("{{TRACK_MODE_DIRECT_SEL}}", stepperTrackModeConfig == TRACK_MODE_DIRECT ? "selected" : "");
  page.replace("{{TRACK_MODE_PID_SEL}}", stepperTrackModeConfig == TRACK_MODE_PID ? "selected" : "");
  // Coalesce/Streaming/Lookahead removed entirely 2026-09-07 ("we just have
  // Direct mode and PID") - see TODO.md and StepperTrackMode's declaration
  // comment (stepper_handler.h) for the full history.

  // Protocol configuration values
  page.replace("{{STEPPER_CONTROL_CHECKED}}", stepperControlEnabled ? "checked" : "");
  page.replace("{{CONTROL_16BIT_CHECKED}}", control16BitConfig ? "checked" : "");
  page.replace("{{PROTOCOL_DEBUG_CHECKED}}", protocolDebugConfig ? "checked" : "");
  page.replace("{{LED_BLANK_TIME}}", String(ledBlankTimeConfig));
  page.replace("{{STEPPER_BLANK_TIME}}", String(stepperBlankTimeConfig));

  // TMC2209 configuration values. UART driver control, sense resistor,
  // address, and SpreadCycle hysteresis (hstrt/hend) are hardcoded as of
  // 2026-09-07 (Wave 4 UI pass) - see tmc_handler.cpp's declaration
  // comments - so those no longer have template placeholders here.
  page.replace("{{TMC_RUN_CURRENT}}", String(tmcRunCurrentConfig));
  page.replace("{{TMC_HOLD_PERCENT}}", String(tmcHoldPercentConfig));
  page.replace("{{TMC_STALL_ENABLED_CHECKED}}", tmcStallEnabledConfig ? "checked" : "");
  page.replace("{{TMC_STALL_THRESHOLD}}", String(tmcStallThresholdConfig));
  const uint16_t tmcMicrostepOptions[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
  for (uint16_t opt : tmcMicrostepOptions) {
    page.replace("{{TMC_USTEP_" + String(opt) + "}}", (opt == tmcMicrostepsConfig) ? "selected" : "");
  }

  // LED configuration values
  page.replace("{{LED_PIXEL_COUNT}}", String(ledPixelCount));
  page.replace("{{LED_ORDER_RGB}}", ledColorOrder == "RGB" ? "selected" : "");
  page.replace("{{LED_ORDER_RBG}}", ledColorOrder == "RBG" ? "selected" : "");
  page.replace("{{LED_ORDER_GRB}}", ledColorOrder == "GRB" ? "selected" : "");
  page.replace("{{LED_ORDER_GBR}}", ledColorOrder == "GBR" ? "selected" : "");
  page.replace("{{LED_ORDER_BRG}}", ledColorOrder == "BRG" ? "selected" : "");
  page.replace("{{LED_ORDER_BGR}}", ledColorOrder == "BGR" ? "selected" : "");
  page.replace("{{LED_GAMMA}}", String(ledGamma, 1));
  page.replace("{{LED_BRIGHTNESS}}", String(ledBrightness));
  page.replace("{{LED_START_NULL}}", String(ledStartNullPixels));
  page.replace("{{LED_END_NULL}}", String(ledEndNullPixels));

  // Version information
  page.replace("{{VERSION}}", VERSION_STRING);
  page.replace("{{BUILD_DATE}}", BUILD_DATE);
  page.replace("{{BUILD_TIME}}", BUILD_TIME);

  // Send with cache control headers to prevent browser caching
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  // Watchdog reset right before the actual network write (2026-09-08,
  // found via a real bench crash): a large page to a client that's
  // actively disconnecting can stall inside server.send()'s underlying
  // socket write for multiple WebServer HTTP_MAX_SEND_WAIT (5s each)
  // cycles waiting for ACKs that never come, with nothing in this call
  // path resetting the watchdog in between - loop()'s own reset happens
  // once per iteration, so if this one send() call alone eats close to or
  // past the 10s watchdog window, the device panics and reboots (observed
  // on the bench exactly this way, immediately after a large send, right
  // as a client disconnected from the AP mid-transfer). This alone doesn't
  // bound how long send() can actually take, but it does guarantee a full,
  // fresh 10s budget going into the one call site now known to be slow,
  // rather than whatever was left over from whenever loop() last reset it.
  esp_task_wdt_reset();
  Serial.print("handleRoot() sending ");
  Serial.print(page.length());
  Serial.print(" bytes, built in ");
  Serial.print(millis() - handleRootStartMs);
  Serial.println(" ms...");
  server.send(200, "text/html", page);
  // If this line never appears in the log right after the one above, the
  // device crashed/rebooted (or the connection died) inside server.send()
  // itself, not before it - genuinely useful to know which side of this
  // line things went wrong on.
  Serial.println("handleRoot() send() returned - response complete.");
}

void handleSaveWifi() {
  if (server.hasArg("ssid")) {
    // Save hostname
    if (server.hasArg("hostname")) {
      hostname = server.arg("hostname");
      preferences.putString("hostname", hostname);
      Serial.print("Hostname saved: ");
      Serial.println(hostname);
    }

    ssid = server.arg("ssid");
    // Standard "leave blank to keep unchanged" convention for a password
    // field, and a real fix (2026-09-08), not just a nicety: the field is
    // deliberately never pre-filled with the real value (so it's never
    // echoed into the page source), which means *every* WiFi settings save
    // that doesn't involve retyping the password used to submit it empty
    // and silently wipe the real one - found the hard way, testing an
    // unrelated field via a raw POST that didn't re-supply it, which broke
    // the actual saved network password on the bench device.
    if (server.arg("password").length() > 0) {
      password = server.arg("password");
    }

    // Handle static IP configuration
    if (server.hasArg("ipMode")) {
      String ipMode = server.arg("ipMode");
      useStaticIp = (ipMode == "static");

      if (useStaticIp) {
        staticIp = server.arg("staticIp");
        staticGateway = server.arg("staticGateway");
        staticSubnet = server.arg("staticSubnet");

        preferences.putString("staticIp", staticIp);
        preferences.putString("staticGateway", staticGateway);
        preferences.putString("staticSubnet", staticSubnet);
      }

      preferences.putBool("useStaticIp", useStaticIp);
    }

    if (server.hasArg("wifiRetryInterval")) {
      wifiRetryIntervalConfig = server.arg("wifiRetryInterval").toInt();
      preferences.putInt("wifiRetryInt", wifiRetryIntervalConfig);
    }

    // Save to preferences
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);

    Serial.println("WiFi configuration saved!");
    Serial.print("SSID: ");
    Serial.println(ssid);
    if (useStaticIp) {
      Serial.print("Static IP: ");
      Serial.println(staticIp);
    }

    // Send JSON response
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi settings saved! Click 'Connect Now' to connect.\"}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Error: Missing SSID\"}");
  }
}

void handleSaveAP() {
  if (server.hasArg("apSsid") && server.hasArg("apPassword")) {
    ap_ssid = server.arg("apSsid");
    ap_password = server.arg("apPassword");
    ap_append_mac = server.hasArg("apAppendMac");

    // Save to preferences
    preferences.putString("apSsid", ap_ssid);
    preferences.putString("apPassword", ap_password);
    preferences.putBool("apAppendMac", ap_append_mac);

    Serial.println("AP configuration saved!");
    Serial.print("AP SSID: ");
    Serial.println(ap_ssid);
    Serial.print("Append MAC: ");
    Serial.println(ap_append_mac ? "Yes" : "No");

    // Send JSON response
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Access Point settings saved! Changes will take effect after reboot.\"}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Error: Missing AP SSID or password\"}");
  }
}

void handleSaveStepper() {
  if (server.hasArg("stepperSpeed") && server.hasArg("stepperAccel")) {
    stepperSpeedConfig = server.arg("stepperSpeed").toInt();
    stepperAccelConfig = server.arg("stepperAccel").toInt();
    autoHomeOnBootConfig = server.hasArg("autoHomeOnBoot");
    if (server.hasArg("stepperSpeedHoming")) {
      stepperSpeedHomingConfig = server.arg("stepperSpeedHoming").toInt();
    }
    if (server.hasArg("stepperAccelHoming")) {
      stepperAccelHomingConfig = server.arg("stepperAccelHoming").toInt();
    }
    stepperTrackEnabledConfig = server.hasArg("stepperTrackEnabled");
    if (server.hasArg("stepperTrackThreshold")) {
      stepperTrackThresholdConfig = server.arg("stepperTrackThreshold").toInt();
    }
    if (server.hasArg("stepperTrackSpeed")) {
      stepperTrackSpeedConfig = server.arg("stepperTrackSpeed").toInt();
    }
    if (server.hasArg("stepperTrackAccel")) {
      stepperTrackAccelConfig = server.arg("stepperTrackAccel").toInt();
    }
    if (server.hasArg("stepperTrackMaxLag")) {
      stepperTrackMaxLagConfig = server.arg("stepperTrackMaxLag").toInt();
    }
    if (server.hasArg("stepperTrackMode")) {
      stepperTrackModeConfig = server.arg("stepperTrackMode").toInt();
    }

    // Config variables are live immediately - the DDP position-handling
    // loop reads them fresh on every packet. The actual flash write is
    // deferred to persistStepperSettingsIfPending() (called from loop())
    // rather than done here synchronously: a Preferences write briefly
    // disables the flash cache, and FastAccelStepper's step-generation
    // interrupt fires continuously while the motor is moving, making a
    // crash likely if this were written immediately mid-move (see the
    // header comment on stepperSettingsPendingSave). Deliberately NOT
    // calling stepper->setSpeedInHz()/setAcceleration() here either -
    // forcing the "normal" profile onto an actively-moving stepper that
    // might currently be cruising in tracking mode would itself be a jerk;
    // the next DDP update (or a manual move, which sets its own profile)
    // picks the right one naturally.
    stepperSettingsPendingSave = true;

    Serial.println("Stepper configuration updated (will save to flash once the motor is idle)");
    Serial.print("Speed: ");
    Serial.print(stepperSpeedConfig);
    Serial.print(" Hz, Acceleration: ");
    Serial.print(stepperAccelConfig);
    Serial.print(" Hz/s, Auto Home on Boot: ");
    Serial.println(autoHomeOnBootConfig ? "Enabled" : "Disabled");
    Serial.print("Homing Speed: ");
    Serial.print(stepperSpeedHomingConfig);
    Serial.print(" Hz, Homing Acceleration: ");
    Serial.print(stepperAccelHomingConfig);
    Serial.println(" Hz/s (takes effect on next homing run)");

    // Send JSON response
    if (stepper->isRunning()) {
      server.send(200, "application/json", "{\"success\":true,\"message\":\"Stepper settings applied! Will finish saving to flash once the motor is idle.\"}");
    } else {
      server.send(200, "application/json", "{\"success\":true,\"message\":\"Stepper settings saved and applied immediately!\"}");
    }
  } else {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Error: Missing stepper parameters\"}");
  }
}

void handleSaveProtocol() {
  // DDP is the only supported protocol; this form covers channel/stepper settings.
  bool newStepperControl = server.hasArg("stepperControl");
  bool new16Bit = server.hasArg("control16Bit");
  bool newDebug = server.hasArg("protocolDebug");

  stepperControlEnabled = newStepperControl;
  control16BitConfig = new16Bit;
  protocolDebugConfig = newDebug;

  preferences.putBool("stepperControl", stepperControlEnabled);
  preferences.putBool("control16Bit", control16BitConfig);
  preferences.putBool("protocolDebug", protocolDebugConfig);

  // Save blank time settings
  if (server.hasArg("ledBlankTime")) {
    ledBlankTimeConfig = server.arg("ledBlankTime").toInt();
    preferences.putInt("ledBlankTime", ledBlankTimeConfig);
  }
  if (server.hasArg("stepperBlankTime")) {
    stepperBlankTimeConfig = server.arg("stepperBlankTime").toInt();
    // "stepperBlankTime" is 16 chars, over NVS's 15-char key limit - this
    // setting silently never persisted across a reboot until this fix.
    preferences.putInt("stepBlankTime", stepperBlankTimeConfig);
  }

  Serial.println("Protocol configuration saved!");
  Serial.print("Stepper Control: ");
  Serial.println(stepperControlEnabled ? "Enabled" : "Disabled");
  if (stepperControlEnabled) {
    Serial.print("16-bit Control: ");
    Serial.println(control16BitConfig ? "Yes" : "No");
  }
  Serial.print("Debug: ");
  Serial.println(protocolDebugConfig ? "Enabled" : "Disabled");
  Serial.print("LED Blank Time: ");
  Serial.print(ledBlankTimeConfig);
  Serial.println(" seconds");
  Serial.print("Stepper Blank Time: ");
  Serial.print(stepperBlankTimeConfig);
  Serial.println(" seconds");

  // Send JSON response
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Protocol settings saved and applied immediately!\"}");

  // Reinitialize DDP
  initDDP();
}

void handleSaveLed() {
  if (server.hasArg("ledPixelCount")) {
    // Blank LEDs with old configuration before changing settings
    // This ensures we clear all pixels that may be beyond the new pixel count
    blankPixelLeds();

    ledPixelCount = server.arg("ledPixelCount").toInt();
    ledColorOrder = server.arg("ledColorOrder");
    ledGamma = server.arg("ledGamma").toFloat();
    ledBrightness = server.arg("ledBrightness").toInt();
    ledStartNullPixels = server.arg("ledStartNullPixels").toInt();
    ledEndNullPixels = server.arg("ledEndNullPixels").toInt();

    preferences.putInt("ledPixelCount", ledPixelCount);
    preferences.putString("ledColorOrder", ledColorOrder);
    preferences.putFloat("ledGamma", ledGamma);
    preferences.putInt("ledBrightness", ledBrightness);
    preferences.putInt("ledStartNull", ledStartNullPixels);
    preferences.putInt("ledEndNull", ledEndNullPixels);

    Serial.println("LED configuration saved!");
    Serial.print("Pixel Count: ");
    Serial.println(ledPixelCount);
    Serial.print("Color Order: ");
    Serial.println(ledColorOrder);
    Serial.print("Gamma: ");
    Serial.println(ledGamma);
    Serial.print("Brightness: ");
    Serial.print(ledBrightness);
    Serial.println("%");
    Serial.print("Start Null Pixels: ");
    Serial.println(ledStartNullPixels);
    Serial.print("End Null Pixels: ");
    Serial.println(ledEndNullPixels);

    // Reinitialize LEDs with new settings immediately
    initPixelLeds();
    Serial.println("LEDs reinitialized with new settings");

    // Send JSON response
    server.send(200, "application/json", "{\"success\":true,\"message\":\"LED settings saved and applied immediately!\"}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Error: Missing LED parameters\"}");
  }
}

void handleSaveTmc() {
  if (isHoming()) {
    // Two independent reasons to refuse this while homing is in progress:
    // 1) Changing microsteps/current mid-search corrupts the step-count
    //    math homing depends on (it assumes a constant distance per step
    //    throughout the whole search).
    // 2) This handler makes several Preferences.putX() flash writes plus
    //    TMC UART round-trips; flash writes briefly disable cache, and
    //    doing that repeatedly while the homing-switch interrupt is live
    //    widens the window for a cache-disabled-access crash if the
    //    switch trips at the wrong moment (see handleHomingInterrupt).
    server.send(409, "application/json", "{\"success\":false,\"message\":\"Cannot change driver settings while homing is in progress - wait for it to finish.\"}");
    return;
  }

  // tmcEnabled/tmcRSense/tmcAddress/tmcHstrt/tmcHend are hardcoded as of
  // 2026-09-07 (see tmc_handler.cpp) and no longer have form fields to read
  // here - the UART link itself is always established at boot and never
  // needs re-initializing from this handler anymore, so this only ever
  // re-applies registers on an already-connected driver.
  uint16_t newRunCurrent = server.hasArg("tmcRunCurrent") ? (uint16_t)server.arg("tmcRunCurrent").toInt() : tmcRunCurrentConfig;
  uint8_t newHoldPercent = server.hasArg("tmcHoldPercent") ? (uint8_t)server.arg("tmcHoldPercent").toInt() : tmcHoldPercentConfig;
  bool newStallEnabled = server.hasArg("tmcStallEnabled");
  uint16_t newStallThreshold = server.hasArg("tmcStallThreshold") ? (uint16_t)server.arg("tmcStallThreshold").toInt() : tmcStallThresholdConfig;
  uint16_t newMicrosteps = server.hasArg("tmcMicrosteps") ? (uint16_t)server.arg("tmcMicrosteps").toInt() : tmcMicrostepsConfig;

  // A fresh homing run is needed if microstepping changes, since bottomPosition
  // is measured in actual steps and the physical distance per step just changed.
  bool microstepsChanged = (newMicrosteps != tmcMicrostepsConfig);

  tmcRunCurrentConfig = newRunCurrent;
  tmcHoldPercentConfig = newHoldPercent;
  tmcStallEnabledConfig = newStallEnabled;
  tmcStallThresholdConfig = newStallThreshold;
  tmcMicrostepsConfig = newMicrosteps;

  preferences.putInt("tmcRunCurrent", tmcRunCurrentConfig);
  preferences.putInt("tmcHoldPercent", tmcHoldPercentConfig);
  preferences.putBool("tmcStallEnabled", tmcStallEnabledConfig);
  preferences.putInt("tmcStallThresh", tmcStallThresholdConfig);
  preferences.putInt("tmcMicrosteps", tmcMicrostepsConfig);

  if (microstepsChanged) {
    Serial.println("TMC2209 microstepping changed - re-home to recalculate bottomPosition!");
  }

  Serial.println("TMC2209 configuration saved!");
  applyTmcSettings();

  if (microstepsChanged) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"TMC2209 settings saved! Microstepping changed - re-home to recalculate travel.\"}");
  } else {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"TMC2209 settings saved and applied immediately!\"}");
  }
}

void handleClearTmcStall() {
  clearTmcStall();
  server.send(200, "application/json", "{\"success\":true}");
}

// Remote "verify calibration, self-heal if it's wrong" endpoint - added
// 2026-09-06 so FPP (or any external DDP-side scripting) can call one HTTP
// request before/between shows instead of needing its own logic to detect
// drift and separately trigger a re-home. Non-blocking, matching every
// other diagnostic move in this firmware (real checks/homing take seconds,
// far too long to hold a WebServer request handler open without starving
// esp_task_wdt_reset(), which only runs from loop()) - this only *starts*
// the check and returns immediately; poll /status-data's "isChecking"
// field, then "homed"/"isHoming" (a trip auto-triggers a full re-home) to
// see when it's done and what happened.
void handleVerifyAndRehome() {
  if (isHoming()) {
    server.send(409, "application/json", "{\"success\":false,\"message\":\"Homing already in progress\"}");
    return;
  }
  if (isStepChecking()) {
    server.send(409, "application/json", "{\"success\":false,\"message\":\"A check is already in progress\"}");
    return;
  }
  if (!homed) {
    // Nothing to verify against yet - go straight to a full homing cycle
    // instead of refusing the request outright.
    startHoming();
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Not homed yet - starting a full homing cycle\"}");
    return;
  }
  if (stepper->isRunning()) {
    server.send(409, "application/json", "{\"success\":false,\"message\":\"Stepper is moving - try again shortly\"}");
    return;
  }
  startStepCheck(0, true);  // true = auto-rehome if this finds real drift
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Verification started - poll /status-data (checking, then homed/isHoming) for the result\"}");
}

void handleConnect() {
  server.send(200, "text/html",
    "<html><body><h1>Attempting to connect...</h1>"
    "<p>Please wait...</p>"
    "<script>setTimeout(function(){ window.location.href='/'; }, 5000);</script>"
    "</body></html>");

  delay(1000);

  Serial.println("Manual connection attempt...");

  // Deliberately NOT preserveAp here (2026-09-08, second pass) - unlike
  // checkWifiConnection()'s unattended AP-fallback retry (which backs off
  // entirely while someone's connected to the AP, since nothing asked it
  // to run right now), this is a person explicitly clicking "Connect Now" -
  // disrupting their own AP connection is the expected, intended outcome
  // of that click, not something to protect them from. Beyond intent,
  // there's a real hardware reason too: the AP and a new STA connection
  // share one radio, and the chip generally won't shift the AP's channel
  // to match the target network while stations are actively associated to
  // it (that would silently disconnect them) - so preserveAp's AP_STA
  // approach can leave the attempt unable to actually complete while a
  // client (e.g. the very phone that just clicked this button) is
  // connected. Plain connectToWifi() forces WIFI_STA immediately,
  // dropping the AP (and any connected clients) up front, matching what
  // "connect now" should mean.
  //
  // Still doesn't strand anyone on failure, which was the real bug fixed
  // 2026-09-08 (first pass): if the just-saved credentials turn out wrong,
  // explicitly restart the AP afterward rather than leaving the device
  // sitting in a dead, disconnected STA state with nothing to fall back to.
  bool wasAccessPoint = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA);
  if (connectToWifi()) {
    Serial.println("Successfully connected!");
    // Matches setup()'s own post-connect mDNS start - this path (and
    // checkWifiConnection()'s AP-fallback retry) didn't have it before
    // 2026-09-08, so hostname.local never worked after recovering via
    // either of them, only after a fresh boot.
    if (MDNS.begin(hostname.c_str())) {
      Serial.print("mDNS responder started: ");
      Serial.print(hostname);
      Serial.println(".local");
      MDNS.addService("http", "tcp", 80);
    } else {
      Serial.println("Error starting mDNS");
    }
  } else {
    Serial.println("Connection failed.");
    if (wasAccessPoint) {
      Serial.println("Restarting AP...");
      startAccessPoint();
    }
  }
}

void handleReboot() {
  server.send(200, "text/html",
    "<html><body><h1>Rebooting...</h1>"
    "<p>Device will restart in 1 second.</p>"
    "</body></html>");

  Serial.println("Rebooting device...");

  // Feed watchdog and give time for response to be sent
  esp_task_wdt_reset();
  delay(100);
  esp_task_wdt_reset();
  delay(100);
  esp_task_wdt_reset();
  delay(100);
  esp_task_wdt_reset();

  ESP.restart();
}

void handleResetSettings() {
  server.send(200, "text/plain", "Settings reset to defaults. Rebooting...");

  Serial.println("Resetting all settings to defaults...");

  // Clear all preferences
  preferences.clear();

  Serial.println("Settings cleared. Rebooting device...");

  // Feed watchdog and give time for response to be sent
  esp_task_wdt_reset();
  delay(100);
  esp_task_wdt_reset();
  delay(100);
  esp_task_wdt_reset();
  delay(100);
  esp_task_wdt_reset();

  ESP.restart();
}

void handleHoming() {
  startHoming();

  // Return immediately so browser doesn't wait
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Homing started. Check status for progress.\"}");
}

void handleLocate() {
  if (server.hasArg("enable")) {
    // Check first character of enable argument to avoid String allocation
    const String& enableArg = server.arg("enable");
    if (enableArg.length() > 0 && enableArg[0] == 't') {  // "true"
      locateMode = true;
      Serial.println("Locate mode enabled - LED showing SOS pattern");
      server.send(200, "application/json", "{\"success\":true,\"locateMode\":true}");
    } else {
      locateMode = false;
      Serial.println("Locate mode disabled");
      server.send(200, "application/json", "{\"success\":true,\"locateMode\":false}");
    }
  } else {
    // Return current locate mode state
    server.send(200, "application/json", locateMode ? "{\"locateMode\":true}" : "{\"locateMode\":false}");
  }
}

void handleLedTest() {
  if (server.hasArg("enable")) {
    // Check first character of enable argument to avoid String allocation
    const String& enableArg = server.arg("enable");
    bool enable = (enableArg.length() > 0 && enableArg[0] == 't');  // "true"
    setLedTestMode(enable);
    server.send(200, "application/json", enable ? "{\"success\":true,\"ledTestMode\":true}" : "{\"success\":true,\"ledTestMode\":false}");
  } else {
    // Return current LED test mode state
    server.send(200, "application/json", ledTestModeActive ? "{\"ledTestMode\":true}" : "{\"ledTestMode\":false}");
  }
}

// GET /compact-log?enable=true|false - toggles compactLogEnabled (original
// behavior, unchanged - what the Settings page checkbox calls).
// GET /compact-log?clear=1 - clears the in-RAM buffer below.
// GET /compact-log (no args) - retrieves the in-RAM Compact Motion Log
// buffer as plain text (added 2026-09-06) - see protocol_common.h's
// getCompactLog()/clearCompactLog() declaration comment for why this is
// HTTP, not Serial: a real xLights/DDPDebugger session already talks to
// the device over the network, and opening a serial connection to watch
// the log would reset the ESP32 (DTR/RTS) and kill that session.
void handleCompactLog() {
  if (server.hasArg("enable")) {
    const String& enableArg = server.arg("enable");
    bool enable = (enableArg.length() > 0 && enableArg[0] == 't');  // "true"
    compactLogEnabled = enable;
    if (enable) {
      Serial.println("ms,ddpVal,cmdPos,curPos,delta,lag,profile,curSpeedHz,targetSpeedHz,encoderCount,sgResult,switchTripped");
    }
    Serial.print("Compact motion log ");
    Serial.println(enable ? "enabled" : "disabled");
    server.send(200, "application/json", enable ? "{\"success\":true,\"compactLog\":true}" : "{\"success\":true,\"compactLog\":false}");
    return;
  }
  if (server.hasArg("clear")) {
    clearCompactLog();
    server.send(200, "text/plain", "cleared");
    return;
  }
  server.send(200, "text/plain", getCompactLog());
}

// Reboot-surviving diagnostic log (see persist_log.h) - primary retrieval
// path, no serial port needed. GET /persist-log?clear=1 clears it instead
// of returning it, for deliberately starting a fresh diagnostic session.
void handlePersistLog() {
  if (server.hasArg("clear")) {
    clearPersistLog();
    server.send(200, "text/plain", "cleared");
    return;
  }
  server.send(200, "text/plain", readPersistLog());
}

// GET /ddp-rx-log[?clear=1] - retrieves (or clears) the in-RAM DDP
// reception log $SET ddpRxLog fills - see ddp_handler.h's declaration
// comment for why this is HTTP, not Serial.
void handleDdpRxLog() {
  if (server.hasArg("clear")) {
    clearDdpRxLog();
    server.send(200, "text/plain", "cleared");
    return;
  }
  server.send(200, "text/plain", getDdpRxLog());
}

// Static buffer for status data response (avoids heap allocation)
static char statusDataBuffer[1900];

void handleStatusData() {

  // WiFi status
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  // Calculate uptime
  unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
  unsigned long uptimeDays = uptimeSeconds / 86400;
  unsigned long uptimeHours = (uptimeSeconds % 86400) / 3600;
  unsigned long uptimeMins = (uptimeSeconds % 3600) / 60;
  unsigned long uptimeSecs = uptimeSeconds % 60;

  // Stepper status
  int currentPosition = stepper->getCurrentPosition();
  int positionPercent = (bottomPosition > 0) ? ((currentPosition * 100) / bottomPosition) : 0;
  bool homingSwitchTripped = isHomingSwitchTripped();

  // Protocol status
  float maxValue = control16BitConfig ? 65535.0 : 255.0;

  // Read positionRequest safely with critical section
  uint16_t currentPositionRequest;
  currentPositionRequest = positionRequest;

  int lastCommandPercent = (int)((currentPositionRequest / maxValue) * 100.0);
  unsigned long packetsReceived = ddpPacketsReceived;
  int totalChannels = 2 + (ledPixelCount * 3);

  // DDP state: Disabled while OTA is pausing protocol handling entirely,
  // Paused while the LED test pattern is ignoring DDP pixel data (stepper
  // DDP still works in that case), Enabled otherwise. OTA takes priority
  // over test mode if both were somehow true at once.
  int ddpState = otaInProgress ? 0 : (ledTestModeActive ? 2 : 1);  // 0=Disabled,1=Enabled,2=Paused
  bool ddpEverReceived = (lastProtocolUpdateTime > 0);
  unsigned long secsSinceLastDdp = ddpEverReceived ? (millis() - lastProtocolUpdateTime) / 1000 : 0;

  // Get IP addresses as strings
  char ipStr[16], gatewayStr[16], subnetStr[16];
  IPAddress ip = wifiConnected ? WiFi.localIP() : WiFi.softAPIP();
  IPAddress gateway = wifiConnected ? WiFi.gatewayIP() : WiFi.softAPIP();
  IPAddress subnet = wifiConnected ? WiFi.subnetMask() : IPAddress(255, 255, 255, 0);

  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  snprintf(gatewayStr, sizeof(gatewayStr), "%d.%d.%d.%d", gateway[0], gateway[1], gateway[2], gateway[3]);
  snprintf(subnetStr, sizeof(subnetStr), "%d.%d.%d.%d", subnet[0], subnet[1], subnet[2], subnet[3]);

  // Get SSID (safe copy to avoid String allocation issues)
  // Use saved ssid string instead of WiFi.SSID() to avoid String allocation every second
  char ssidStr[33];  // Max SSID length is 32 + null terminator
  const char* ssidSource = wifiConnected ? ssid.c_str() : ap_ssid.c_str();
  strncpy(ssidStr, ssidSource, sizeof(ssidStr) - 1);
  ssidStr[sizeof(ssidStr) - 1] = '\0';

  // Build JSON using snprintf in static buffer (no heap allocation)
  // Use enums: wifiMode: 0=AP, 1=Client, ipType: 0=N/A, 1=DHCP, 2=Static
  snprintf(statusDataBuffer, sizeof(statusDataBuffer),
    "{"
    "\"wifiConnected\":%s,"
    "\"wifiMode\":%d,"
    "\"wifiNetwork\":\"%s\","
    "\"wifiIp\":\"%s\","
    "\"wifiGateway\":\"%s\","
    "\"wifiSubnet\":\"%s\","
    "\"ipType\":%d,"
    "\"uptimeDays\":%lu,"
    "\"uptimeHours\":%lu,"
    "\"uptimeMins\":%lu,"
    "\"uptimeSecs\":%lu,"
    "\"homed\":%s,"
    "\"isHoming\":%s,"
    "\"isChecking\":%s,"
    "\"homingError\":%s,"
    "\"homingSwitchTripped\":%s,"
    "\"position\":%d,"
    "\"positionPercent\":%d,"
    "\"bottomPosition\":%d,"
    "\"homingTravelValid\":%s,"
    "\"homingTravelSteps\":%ld,"
    "\"homingTravelMs\":%lu,"
    "\"encoderInitialized\":%s,"
    "\"encoderCount\":%ld,"
    "\"encoderMissed\":%lu,"
    "\"control16Bit\":%s,"
    "\"protocolPacketsReceived\":%lu,"
    "\"protocolPacketsRejectedOutOfOrder\":%lu,"
    "\"protocolLastCommand\":%u,"
    "\"protocolLastCommandPercent\":%d,"
    "\"totalChannels\":%d,"
    "\"ddpState\":%d,"
    "\"ddpEverReceived\":%s,"
    "\"secsSinceLastDdp\":%lu,"
    "\"locateMode\":%s,"
    "\"autoHomeOnBoot\":%s,"
    "\"ledPixelCount\":%d,"
    "\"ledMaxPixelsReceived\":%d,"
    "\"ledsBlanked\":%s,"
    "\"ledTestMode\":%s,"
    "\"compactLog\":%s,"
    "\"tmcEnabled\":%s,"
    "\"tmcConnected\":%s,"
    "\"tmcOverTempWarning\":%s,"
    "\"tmcOverTempShutdown\":%s,"
    "\"tmcShortToGroundA\":%s,"
    "\"tmcShortToGroundB\":%s,"
    "\"tmcOpenLoadA\":%s,"
    "\"tmcOpenLoadB\":%s,"
    "\"tmcUartCrcError\":%s,"
    "\"tmcStallEnabled\":%s,"
    "\"tmcStallGuardResult\":%u,"
    "\"tmcStalled\":%s"
    "}",
    wifiConnected ? "true" : "false",
    wifiConnected ? 1 : 0,  // wifiMode: 0=AP, 1=Client
    ssidStr,
    ipStr,
    gatewayStr,
    subnetStr,
    wifiConnected ? (useStaticIp ? 2 : 1) : 0,  // ipType: 0=N/A, 1=DHCP, 2=Static
    uptimeDays, uptimeHours, uptimeMins, uptimeSecs,
    homed ? "true" : "false",
    isHoming() ? "true" : "false",
    isStepChecking() ? "true" : "false",
    homingErrorLatched ? "true" : "false",
    homingSwitchTripped ? "true" : "false",
    currentPosition,
    positionPercent,
    bottomPosition,
    homingTravelValid ? "true" : "false",
    homingTravelSteps,
    homingTravelMs,
    isEncoderInitialized() ? "true" : "false",
    (long)getEncoderCount(),
    (unsigned long)getMissedTransitionCount(),
    control16BitConfig ? "true" : "false",
    packetsReceived,
    ddpPacketsRejectedOutOfOrder,
    currentPositionRequest,
    lastCommandPercent,
    totalChannels,
    ddpState,
    ddpEverReceived ? "true" : "false",
    secsSinceLastDdp,
    locateMode ? "true" : "false",
    autoHomeOnBootConfig ? "true" : "false",
    ledPixelCount,
    ledMaxPixelsReceived,
    ledsBlanked ? "true" : "false",
    ledTestModeActive ? "true" : "false",
    compactLogEnabled ? "true" : "false",
    tmcEnabledConfig ? "true" : "false",
    tmcConnected ? "true" : "false",
    tmcStatus.overTempWarning ? "true" : "false",
    tmcStatus.overTempShutdown ? "true" : "false",
    tmcStatus.shortToGroundA ? "true" : "false",
    tmcStatus.shortToGroundB ? "true" : "false",
    tmcStatus.openLoadA ? "true" : "false",
    tmcStatus.openLoadB ? "true" : "false",
    tmcStatus.uartCrcError ? "true" : "false",
    tmcStallEnabledConfig ? "true" : "false",
    tmcStatus.stallGuardResult,
    tmcStatus.stalled ? "true" : "false"
  );

  server.send(200, "application/json", statusDataBuffer);

}

// Static buffer for LED preview response (avoids heap allocation)
static char ledPreviewResponseBuffer[MAX_LEDS * 6 + 200];

void handleLedPreview() {

  // Show all configured LEDs (up to MAX_LEDS to prevent overflow)
  int maxPixels = min(ledPixelCount, MAX_LEDS);

  // Sanity check - refuse if pixel count is unreasonable
  if (maxPixels > MAX_LEDS || maxPixels < 0) {
    Serial.print("[ERROR] handleLedPreview: Invalid maxPixels = ");
    Serial.println(maxPixels);
    server.send(500, "application/json", "{\"error\":\"Invalid pixel count\"}");
    return;
  }

  // Build response directly in static buffer to avoid heap allocation
  const char* ledData = getLedPreviewJson(maxPixels);

  // Use snprintf to safely build JSON response in static buffer
  snprintf(ledPreviewResponseBuffer, sizeof(ledPreviewResponseBuffer),
           "{\"ledPreview\":%s,\"count\":%d}",
           ledData, maxPixels);

  server.send(200, "application/json", ledPreviewResponseBuffer);

}

void handleMove() {
  if (isHoming()) {
    server.send(400, "text/plain", "Cannot move - homing in progress");
    return;
  }

  if (server.hasArg("steps")) {
    int steps = server.arg("steps").toInt();
    // Manual moves always use the normal profile, not whatever DDP's
    // small-move tracking logic last left speed/accel set to.
    stepper->setSpeedInHz(stepperSpeedConfig);
    stepper->setAcceleration(stepperAccelConfig);
    stepper->move(steps);

    Serial.print("Moving stepper: ");
    Serial.print(steps);
    Serial.println(" steps");

    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing steps parameter");
  }
}

void handleSetPosition() {
  if (isHoming()) {
    server.send(400, "text/plain", "Cannot move - homing in progress");
    return;
  }
  // Was unconditionally gated on `homed` here (2026-08-30, after a
  // drift-triggered "not homed" still let Set Position drive to an
  // arbitrary absolute target immediately afterward) - relaxed back
  // (2026-09-01) after that made it impossible to move the trolley at all
  // for bench testing/verification while not homed, with no way to get it
  // moving again short of a physical nudge or a successful home (which may
  // itself be what's being diagnosed). A raw step count here is really the
  // same category of manual/human-operated control as handleMove()'s
  // relative jog (already ungated, for exactly this reason) - not
  // something DDP's automated position handling can fall back on, so it
  // doesn't need DDP's strict server-side enforcement. The one target this
  // endpoint receives that's genuinely meaningless without a trusted
  // bottomPosition - a *percent* position - is converted to steps
  // client-side before the request is even sent, so the server has no way
  // to tell steps and percent requests apart here anyway; the UI disables
  // the Percent option while not homed instead (see
  // updatePercentModeAvailability() in script.js).
  if (server.hasArg("position")) {
    int position = server.arg("position").toInt();
    // Manual moves always use the normal profile, not whatever DDP's
    // small-move tracking logic last left speed/accel set to.
    stepper->setSpeedInHz(stepperSpeedConfig);
    stepper->setAcceleration(stepperAccelConfig);
    stepper->moveTo(position);

    Serial.print("Moving stepper to position: ");
    Serial.println(position);

    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing position parameter");
  }
}

void startWebServer() {
  // Root page
  server.on("/", HTTP_GET, handleRoot);

  // Save configuration endpoints
  server.on("/save-wifi", HTTP_POST, handleSaveWifi);     // WiFi settings
  server.on("/save-ap", HTTP_POST, handleSaveAP);         // AP settings
  server.on("/save-stepper", HTTP_POST, handleSaveStepper); // Stepper settings
  server.on("/save-protocol", HTTP_POST, handleSaveProtocol);  // Protocol settings
  server.on("/save-led", HTTP_POST, handleSaveLed);            // LED settings
  server.on("/save-tmc", HTTP_POST, handleSaveTmc);             // TMC2209 driver settings

  // Status and control endpoints
  server.on("/status-data", HTTP_GET, handleStatusData);  // JSON status data
  server.on("/led-preview", HTTP_GET, handleLedPreview);  // LED preview data (separate to save bandwidth)
  server.on("/move", HTTP_GET, handleMove);               // Move relative steps
  server.on("/set-position", HTTP_GET, handleSetPosition); // Move to absolute position

  // Connect now
  server.on("/connect", HTTP_GET, handleConnect);

  // Reboot
  server.on("/reboot", HTTP_GET, handleReboot);

  // Reset Settings
  server.on("/reset-settings", HTTP_GET, handleResetSettings);

  // Homing
  server.on("/home", HTTP_GET, handleHoming);

  // Locate mode
  server.on("/locate", HTTP_GET, handleLocate);

  // LED test pattern (local bench testing without DDP)
  server.on("/led-test", HTTP_GET, handleLedTest);

  // Temporary compact motion CSV log toggle
  server.on("/compact-log", HTTP_GET, handleCompactLog);
  server.on("/persist-log", HTTP_GET, handlePersistLog);  // reboot-surviving diagnostic log - see persist_log.h
  server.on("/ddp-rx-log", HTTP_GET, handleDdpRxLog);      // DDP reception bench log - see ddp_handler.h
  server.on("/tunable", HTTP_GET, handleTunableHttp);      // $SET/$GET over HTTP - see tuning_handler.cpp
  server.on("/config", HTTP_GET, handleConfigGet);          // full persisted config as key=value - see config_handler.h
  server.on("/config", HTTP_POST, handleConfigPost);        // write it back, same format

  // TMC2209 stall fault acknowledgement
  server.on("/clear-tmc-stall", HTTP_GET, handleClearTmcStall);
  server.on("/verify-and-rehome", HTTP_GET, handleVerifyAndRehome);  // for FPP/DDP-side scripting - see handler comment

  // OTA Update endpoint
  server.on("/update", HTTP_POST, handleOTAUpdateComplete, handleOTAUpdate);

  // Captive portal detection endpoints
  server.on("/generate_204", HTTP_GET, handleRoot);  // Android
  server.on("/gen_204", HTTP_GET, handleRoot);       // Android
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);  // iOS
  server.on("/canonical.html", HTTP_GET, handleRoot);       // Firefox
  server.on("/success.txt", HTTP_GET, handleRoot);          // Firefox
  server.on("/ncsi.txt", HTTP_GET, handleRoot);             // Windows

  // Catch-all for any other requests
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Web server started");
}

