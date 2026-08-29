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

// Watchdog timeout in seconds
#define WDT_TIMEOUT 10

// Preferences for storing configuration
Preferences preferences;

// Variables for position tracking
uint16_t oldPositionRequest = 0;
float position = 0;

// Uptime tracking
unsigned long bootTime = 0;

// Blank time tracking
bool stepperBlanked = false;  // Track if stepper has been blanked

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

  // Load protocol configuration
  protocolConfig = (protocolType)preferences.getInt("protocol", PROTOCOL_DDP);
  stepperControlEnabled = preferences.getBool("stepperControl", true);
  control16BitConfig = preferences.getBool("control16Bit", false);
  protocolDebugConfig = preferences.getBool("protocolDebug", false);

  // Load blank time configuration
  ledBlankTimeConfig = preferences.getInt("ledBlankTime", 0);
  stepperBlankTimeConfig = preferences.getInt("stepperBlankTime", 0);

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

  // Update non-blocking homing state machine
  updateHoming();

  // Poll TMC2209 diagnostics / stall detection (no-op if not enabled)
  updateTmc();

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

      // Only print position changes if protocol debug is enabled
      if (protocolDebugConfig) {
        float maxValue = control16BitConfig ? 65535.0 : 255.0;
        Serial.print("Moving to position ");
        Serial.print(currentPositionRequest);
        Serial.print(" (");
        Serial.print(int((float)((float)currentPositionRequest / maxValue) * 100));
        Serial.print("%) -> ");
        Serial.println((int)position);
      }

      stepper->moveTo((int)position);
      stepperBlanked = false;  // Reset blank state when we receive a command
    }
  }

  // Handle blank time timeouts
  if (lastProtocolUpdateTime > 0 && !otaInProgress) {
    unsigned long timeSinceUpdate = millis() - lastProtocolUpdateTime;

    // LED blank timeout
    if (ledBlankTimeConfig > 0 && timeSinceUpdate > (unsigned long)ledBlankTimeConfig * 1000) {
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

void handleSerialCommands() {
  // Check for serial input
  if (Serial.available() > 0) {
    char c = Serial.read();
    switch (c) {
    case '\r':
    case '?':
      Serial.println("\n=== Available Serial Commands ===");
      Serial.println("?  - Show this help menu");
      Serial.println("h  - Start homing sequence");
      Serial.println("r  - Reboot device");
      Serial.println("s  - Print connection status");
      Serial.println("n  - Network diagnostics (detailed)");
      Serial.println("w  - Attempt WiFi reconnection");
      Serial.println("a  - Switch to Access Point mode");
      Serial.println("p  - Print current stepper position");
      Serial.println("f  - Move forward 10 steps");
      Serial.println("b  - Move backward 10 steps");
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
      printStatus();
      break;
    case 'n':
      printNetworkDiagnostics();
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
        stepper->setAcceleration(stepperAccelHoming);
        stepper->move(10);
        stepper->setAcceleration(stepperAccel);
      }
      break;
    case 'b':
      if (isHoming()) {
        Serial.println("Cannot move while homing is in progress");
      }
      else {
        Serial.println("Moving backward 10 steps");
        stepper->setAcceleration(stepperAccelHoming);
        stepper->move(-10);
        stepper->setAcceleration(stepperAccel);
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

void printNetworkDiagnostics() {
  Serial.println("\n=== Network Diagnostics ===");

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
