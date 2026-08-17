#include "html_handlers.h"
#include "html.h"
#include "version.h"
#include "wifi_handler.h"
#include "stepper_handler.h"
#include "artnet_handler.h"
#include "ddp_handler.h"
#include "ota_handler.h"
#include "led_handler.h"
#include "protocol_common.h"
#include "main.h"
#include <WiFi.h>
#include <Preferences.h>

// Web server
WebServer server(80);

// External preferences object
extern Preferences preferences;

void handleRoot() {
  String page = String(htmlPage);

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
  page.replace("{{AUTO_HOME_STATUS}}", autoHomeOnBootConfig ? "Enabled" : "Disabled");

  // Configuration values
  page.replace("{{HOSTNAME}}", hostname);
  page.replace("{{CURRENT_SSID}}", ssid);
  page.replace("{{DHCP_CHECKED}}", useStaticIp ? "" : "checked");
  page.replace("{{STATIC_CHECKED}}", useStaticIp ? "checked" : "");
  page.replace("{{STATIC_IP}}", staticIp);
  page.replace("{{STATIC_GATEWAY}}", staticGateway);
  page.replace("{{STATIC_SUBNET}}", staticSubnet);
  page.replace("{{AP_SSID}}", ap_ssid);
  page.replace("{{AP_PASSWORD}}", ap_password);
  page.replace("{{AP_APPEND_MAC_CHECKED}}", ap_append_mac ? "checked" : "");
  page.replace("{{STEPPER_SPEED}}", String(stepperSpeedConfig));
  page.replace("{{STEPPER_ACCEL}}", String(stepperAccelConfig));
  page.replace("{{JUMP_START}}", String(jumpStartConfig));
  page.replace("{{AUTO_HOME_ON_BOOT_CHECKED}}", autoHomeOnBootConfig ? "checked" : "");

  // Protocol configuration values
  page.replace("{{DDP_SELECTED}}", protocolConfig == PROTOCOL_DDP ? "selected" : "");
  page.replace("{{ARTNET_SELECTED}}", protocolConfig == PROTOCOL_ARTNET ? "selected" : "");
  page.replace("{{ARTNET_UNIVERSE}}", String(artnetUniverseConfig));
  page.replace("{{ARTNET_CHANNELS_PER_UNIVERSE}}", String(artnetChannelsPerUniverseConfig));
  page.replace("{{STEPPER_CONTROL_CHECKED}}", stepperControlEnabled ? "checked" : "");
  page.replace("{{CONTROL_16BIT_CHECKED}}", control16BitConfig ? "checked" : "");
  page.replace("{{PROTOCOL_DEBUG_CHECKED}}", protocolDebugConfig ? "checked" : "");
  page.replace("{{LED_BLANK_TIME}}", String(ledBlankTimeConfig));
  page.replace("{{STEPPER_BLANK_TIME}}", String(stepperBlankTimeConfig));

  // Status page protocol values
  page.replace("{{PROTOCOL_TYPE}}", protocolConfig == PROTOCOL_DDP ? "DDP" : "ArtNet");

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
  server.send(200, "text/html", page);
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
    password = server.arg("password");

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
  if (server.hasArg("stepperSpeed") && server.hasArg("stepperAccel") && server.hasArg("jumpStart")) {
    stepperSpeedConfig = server.arg("stepperSpeed").toInt();
    stepperAccelConfig = server.arg("stepperAccel").toInt();
    jumpStartConfig = server.arg("jumpStart").toInt();
    autoHomeOnBootConfig = server.hasArg("autoHomeOnBoot");

    preferences.putInt("stepperSpeed", stepperSpeedConfig);
    preferences.putInt("stepperAccel", stepperAccelConfig);
    preferences.putInt("jumpStart", jumpStartConfig);
    preferences.putBool("autoHomeOnBoot", autoHomeOnBootConfig);

    // Apply the new settings immediately
    stepper->setSpeedInHz(stepperSpeedConfig);
    stepper->setAcceleration(stepperAccelConfig);
    stepper->setJumpStart(jumpStartConfig);

    Serial.println("Stepper configuration saved!");
    Serial.print("Speed: ");
    Serial.print(stepperSpeedConfig);
    Serial.print(" Hz, Acceleration: ");
    Serial.print(stepperAccelConfig);
    Serial.print(" Hz/s, Jump Start: ");
    Serial.print(jumpStartConfig);
    Serial.print(" steps, Auto Home on Boot: ");
    Serial.println(autoHomeOnBootConfig ? "Enabled" : "Disabled");

    // Send JSON response
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Stepper settings saved and applied immediately!\"}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Error: Missing stepper parameters\"}");
  }
}

void handleSaveProtocol() {
  if (server.hasArg("protocol")) {
    String newProtocol = server.arg("protocol");
    bool newStepperControl = server.hasArg("stepperControl");
    bool new16Bit = server.hasArg("control16Bit");
    bool newDebug = server.hasArg("protocolDebug");

    // Save protocol selection
    stepperControlEnabled = newStepperControl;
    control16BitConfig = new16Bit;
    protocolDebugConfig = newDebug;

    if(newProtocol == "artnet") {
      preferences.putInt("protocol", PROTOCOL_ARTNET);
    } else {
      preferences.putInt("protocol", PROTOCOL_DDP);
    }

    preferences.putBool("stepperControl", stepperControlEnabled);
    preferences.putBool("control16Bit", control16BitConfig);
    preferences.putBool("protocolDebug", protocolDebugConfig);

    // Save ArtNet-specific settings if present
    if (server.hasArg("artnetUniverse")) {
      artnetUniverseConfig = server.arg("artnetUniverse").toInt();
      preferences.putInt("artnetUniverse", artnetUniverseConfig);
    }
    if (server.hasArg("artnetChannelsPerUniverse")) {
      artnetChannelsPerUniverseConfig = server.arg("artnetChannelsPerUniverse").toInt();
      preferences.putInt("artnetChansPerUni", artnetChannelsPerUniverseConfig);
    }

    // Save blank time settings
    if (server.hasArg("ledBlankTime")) {
      ledBlankTimeConfig = server.arg("ledBlankTime").toInt();
      preferences.putInt("ledBlankTime", ledBlankTimeConfig);
    }
    if (server.hasArg("stepperBlankTime")) {
      stepperBlankTimeConfig = server.arg("stepperBlankTime").toInt();
      preferences.putInt("stepperBlankTime", stepperBlankTimeConfig);
    }

    Serial.println("Protocol configuration saved!");
    Serial.print("Protocol: ");
    if( protocolConfig == PROTOCOL_DDP ) {
      Serial.println("DDP");
    } else {
      Serial.println("ArtNet");
    }
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

    if (protocolConfig == PROTOCOL_ARTNET) {
      Serial.print("ArtNet Universe: ");
      Serial.println(artnetUniverseConfig);
      Serial.print("Channels per Universe: ");
      Serial.println(artnetChannelsPerUniverseConfig);
    }

    // Send JSON response
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Protocol settings saved! Reboot may be required for changes to take effect.\"}");

    // Reinitialize protocols
    initDDP();
    initializeArtNet();
  } else {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Error: Missing protocol parameter\"}");
  }
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

void handleConnect() {
  server.send(200, "text/html",
    "<html><body><h1>Attempting to connect...</h1>"
    "<p>Please wait...</p>"
    "<script>setTimeout(function(){ window.location.href='/'; }, 5000);</script>"
    "</body></html>");

  delay(1000);

  Serial.println("Manual connection attempt...");

  if (connectToWifi()) {
    Serial.println("Successfully connected! Disabling AP mode...");
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  } else {
    Serial.println("Connection failed. Staying in AP mode.");
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

// Static buffer for status data response (avoids heap allocation)
static char statusDataBuffer[1200];

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
  unsigned long packetsReceived = (protocolConfig == PROTOCOL_DDP) ? ddpPacketsReceived : artnetPacketsReceived;
  int totalChannels = 2 + (ledPixelCount * 3) + 1;

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
  // Use enums: wifiMode: 0=AP, 1=Client, ipType: 0=N/A, 1=DHCP, 2=Static, protocol: 0=ArtNet, 1=DDP
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
    "\"homingSwitchTripped\":%s,"
    "\"position\":%d,"
    "\"positionPercent\":%d,"
    "\"bottomPosition\":%d,"
    "\"protocol\":%d,"
    "\"control16Bit\":%s,"
    "\"protocolPacketsReceived\":%lu,"
    "\"protocolLastCommand\":%u,"
    "\"protocolLastCommandPercent\":%d,"
    "\"totalChannels\":%d,"
    "\"locateMode\":%s,"
    "\"autoHomeOnBoot\":%s,"
    "\"ledPixelCount\":%d,"
    "\"ledMaxPixelsReceived\":%d,"
    "\"ledsBlanked\":%s"
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
    homingSwitchTripped ? "true" : "false",
    currentPosition,
    positionPercent,
    bottomPosition,
    protocolConfig == PROTOCOL_DDP ? 1 : 0,  // protocol: 0=ArtNet, 1=DDP
    control16BitConfig ? "true" : "false",
    packetsReceived,
    currentPositionRequest,
    lastCommandPercent,
    totalChannels,
    locateMode ? "true" : "false",
    autoHomeOnBootConfig ? "true" : "false",
    ledPixelCount,
    ledMaxPixelsReceived,
    ledsBlanked ? "true" : "false"
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

  if (server.hasArg("position")) {
    int position = server.arg("position").toInt();
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
  server.onNotFound(handleRoot);

  server.begin();
  Serial.println("Web server started");
}

