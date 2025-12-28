#include "html_handlers.h"
#include "html.h"
#include "version.h"
#include "wifi_handler.h"
#include "stepper_handler.h"
#include "artnet_handler.h"
#include "ddp_handler.h"
#include "ota_handler.h"
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

  // ArtNet configuration values
  page.replace("{{ARTNET_ENABLED_CHECKED}}", artnetEnabledConfig ? "checked" : "");
  page.replace("{{ARTNET_UNIVERSE}}", String(artnetUniverseConfig));
  page.replace("{{ARTNET_CHANNEL}}", String(artnetChannelConfig + 1));  // Display as 1-based
  page.replace("{{ARTNET_DEBUG_CHECKED}}", artnetDebugConfig ? "checked" : "");

  // DDP configuration values
  page.replace("{{DDP_ENABLED_CHECKED}}", ddpEnabledConfig ? "checked" : "");
  page.replace("{{DDP_SERVO_CHANNEL}}", String(ddpServoChannelConfig));  // Already 1-based
  page.replace("{{DDP_DEBUG_CHECKED}}", ddpDebugConfig ? "checked" : "");

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
    String response = "{\"success\":true,\"message\":\"WiFi settings saved! Click 'Connect Now' to connect.\"}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Error: Missing SSID\"}";
    server.send(400, "application/json", response);
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
    String response = "{\"success\":true,\"message\":\"Access Point settings saved! Changes will take effect after reboot.\"}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Error: Missing AP SSID or password\"}";
    server.send(400, "application/json", response);
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
    String response = "{\"success\":true,\"message\":\"Stepper settings saved and applied immediately!\"}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Error: Missing stepper parameters\"}";
    server.send(400, "application/json", response);
  }
}

void handleSaveArtnet() {
  if (server.hasArg("artnetUniverse") && server.hasArg("artnetChannel")) {
    int newUniverse = server.arg("artnetUniverse").toInt();
    int newChannel = server.arg("artnetChannel").toInt();
    bool newEnabled = server.hasArg("artnetEnabled");
    bool newDebug = server.hasArg("artnetDebug");

    // Convert from 1-based (user) to 0-based (internal)
    artnetUniverseConfig = newUniverse;
    artnetChannelConfig = newChannel - 1;  // User enters 1-512, we store 0-511
    artnetEnabledConfig = newEnabled;
    artnetDebugConfig = newDebug;

    preferences.putInt("artnetUniverse", artnetUniverseConfig);
    preferences.putInt("artnetChannel", artnetChannelConfig);
    preferences.putBool("artnetEnabled", artnetEnabledConfig);
    preferences.putBool("artnetDebug", artnetDebugConfig);

    Serial.println("ArtNet configuration saved!");
    Serial.print("Enabled: ");
    Serial.println(artnetEnabledConfig ? "Yes" : "No");
    Serial.print("Universe: ");
    Serial.println(artnetUniverseConfig);
    Serial.print("Channel: ");
    Serial.print(artnetChannelConfig + 1);  // Display as 1-based
    Serial.print(" (internal: ");
    Serial.print(artnetChannelConfig);
    Serial.println(")");
    Serial.print("Debug: ");
    Serial.println(artnetDebugConfig ? "Enabled" : "Disabled");

    // Send JSON response
    String response = "{\"success\":true,\"message\":\"ArtNet settings saved! NOTE: Reboot required for universe change.\"}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Error: Missing required parameters\"}";
    server.send(400, "application/json", response);
  }
}

void handleSaveDDP() {
  if (server.hasArg("ddpServoChannel")) {
    int newServoChannel = server.arg("ddpServoChannel").toInt();
    bool newEnabled = server.hasArg("ddpEnabled");
    bool newDebug = server.hasArg("ddpDebug");

    // Validate values (1-based input, store as 1-based)
    if (newServoChannel < 1 || newServoChannel > 512) {
      String response = "{\"success\":false,\"message\":\"Servo channel must be between 1 and 512\"}";
      server.send(400, "application/json", response);
      return;
    }

    // Store as 1-based internally
    ddpServoChannelConfig = newServoChannel;
    ddpEnabledConfig = newEnabled;
    ddpDebugConfig = newDebug;

    preferences.putInt("ddpServoChannel", ddpServoChannelConfig);
    preferences.putBool("ddpEnabled", ddpEnabledConfig);
    preferences.putBool("ddpDebug", ddpDebugConfig);

    // Send JSON response FIRST before reinitializing DDP
    String response = "{\"success\":true,\"message\":\"DDP settings saved and applied!\"}";
    server.send(200, "application/json", response);

    Serial.println("DDP configuration saved!");
    Serial.print("Enabled: ");
    Serial.println(ddpEnabledConfig ? "Yes" : "No");
    Serial.print("Servo Channel: ");
    Serial.println(ddpServoChannelConfig);  // Already 1-based
    Serial.print("Debug: ");
    Serial.println(ddpDebugConfig ? "Enabled" : "Disabled");

    // Reinitialize DDP with new settings AFTER sending response
    initDDP();
  } else {
    String response = "{\"success\":false,\"message\":\"Error: Missing required parameters\"}";
    server.send(400, "application/json", response);
  }
}

void handleConnect() {
  String response = "<html><body><h1>Attempting to connect...</h1>";
  response += "<p>Please wait...</p>";
  response += "<script>setTimeout(function(){ window.location.href='/'; }, 5000);</script>";
  response += "</body></html>";

  server.send(200, "text/html", response);

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
  String response = "<html><body><h1>Rebooting...</h1>";
  response += "<p>Device will restart in 1 second.</p>";
  response += "</body></html>";

  server.send(200, "text/html", response);

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

void handleHoming() {
  startHoming();

  // Return immediately so browser doesn't wait
  String response = "{\"success\":true,\"message\":\"Homing started. Check status for progress.\"}";
  server.send(200, "application/json", response);
}

void handleLocate() {
  if (server.hasArg("enable")) {
    String enableArg = server.arg("enable");
    if (enableArg == "true") {
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
    String response = "{\"locateMode\":";
    response += locateMode ? "true" : "false";
    response += "}";
    server.send(200, "application/json", response);
  }
}

void handleStatusData() {
  // WiFi status
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  String wifiMode = wifiConnected ? "Client Mode" : "Access Point Mode";
  String wifiNetwork = wifiConnected ? WiFi.SSID() : String(ap_ssid);
  String wifiIp = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String wifiGateway = wifiConnected ? WiFi.gatewayIP().toString() : WiFi.softAPIP().toString();
  String wifiSubnet = wifiConnected ? WiFi.subnetMask().toString() : "255.255.255.0";
  String ipType = wifiConnected ? (useStaticIp ? "Static IP" : "DHCP") : "N/A";

  // Calculate uptime
  unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
  unsigned long uptimeDays = uptimeSeconds / 86400;
  unsigned long uptimeHours = (uptimeSeconds % 86400) / 3600;
  unsigned long uptimeMins = (uptimeSeconds % 3600) / 60;
  unsigned long uptimeSecs = uptimeSeconds % 60;

  // Stepper status
  int currentPosition = stepper->getCurrentPosition();
  float positionPercent = (bottomPosition > 0) ? ((float)currentPosition / (float)bottomPosition * 100.0) : 0;
  bool homingSwitchTripped = isHomingSwitchTripped();

  // ArtNet last command
  float artnetPercent = (lastReceivedPosition / 255.0) * 100.0;

  // DDP last command
  float ddpPercent = (ddpLastReceivedPosition / 255.0) * 100.0;

  String json = "{";
  json += "\"wifiConnected\":" + String(wifiConnected ? "true" : "false") + ",";
  json += "\"wifiMode\":\"" + wifiMode + "\",";
  json += "\"wifiNetwork\":\"" + wifiNetwork + "\",";
  json += "\"wifiIp\":\"" + wifiIp + "\",";
  json += "\"wifiGateway\":\"" + wifiGateway + "\",";
  json += "\"wifiSubnet\":\"" + wifiSubnet + "\",";
  json += "\"ipType\":\"" + ipType + "\",";
  json += "\"uptimeDays\":" + String(uptimeDays) + ",";
  json += "\"uptimeHours\":" + String(uptimeHours) + ",";
  json += "\"uptimeMins\":" + String(uptimeMins) + ",";
  json += "\"uptimeSecs\":" + String(uptimeSecs) + ",";
  json += "\"homed\":" + String(homed ? "true" : "false") + ",";
  json += "\"isHoming\":" + String(isHoming() ? "true" : "false") + ",";
  json += "\"homingSwitchTripped\":" + String(homingSwitchTripped ? "true" : "false") + ",";
  json += "\"position\":" + String(currentPosition) + ",";
  json += "\"positionPercent\":" + String(positionPercent, 1) + ",";
  json += "\"bottomPosition\":" + String(bottomPosition) + ",";
  json += "\"artnetEnabled\":" + String(artnetEnabledConfig ? "true" : "false") + ",";
  json += "\"artnetUniverse\":" + String(artnetUniverseConfig) + ",";
  json += "\"artnetChannel\":" + String(artnetChannelConfig + 1) + ",";  // Display as 1-based
  json += "\"artnetPacketsReceived\":" + String(artnetPacketsReceived) + ",";
  json += "\"artnetPacketsActedOn\":" + String(artnetPacketsActedOn) + ",";
  json += "\"artnetLastCommand\":" + String(lastReceivedPosition) + ",";
  json += "\"artnetLastCommandPercent\":" + String(artnetPercent, 1) + ",";
  json += "\"ddpEnabled\":" + String(ddpEnabledConfig ? "true" : "false") + ",";
  json += "\"ddpServoChannel\":" + String(ddpServoChannelConfig) + ",";  // Already 1-based
  json += "\"ddpPacketsReceived\":" + String(ddpPacketsReceived) + ",";
  json += "\"ddpPacketsActedOn\":" + String(ddpPacketsActedOn) + ",";
  json += "\"ddpLastCommand\":" + String(ddpLastReceivedPosition) + ",";
  json += "\"ddpLastCommandPercent\":" + String(ddpPercent, 1) + ",";
  json += "\"locateMode\":" + String(locateMode ? "true" : "false") + ",";
  json += "\"autoHomeOnBoot\":" + String(autoHomeOnBootConfig ? "true" : "false");
  json += "}";

  server.send(200, "application/json", json);
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
  server.on("/save-artnet", HTTP_POST, handleSaveArtnet);  // ArtNet settings
  server.on("/save-ddp", HTTP_POST, handleSaveDDP);       // DDP settings

  // Status and control endpoints
  server.on("/status-data", HTTP_GET, handleStatusData);  // JSON status data
  server.on("/move", HTTP_GET, handleMove);               // Move relative steps
  server.on("/set-position", HTTP_GET, handleSetPosition); // Move to absolute position

  // Connect now
  server.on("/connect", HTTP_GET, handleConnect);

  // Reboot
  server.on("/reboot", HTTP_GET, handleReboot);

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

