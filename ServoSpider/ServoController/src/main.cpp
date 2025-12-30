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
#include "artnet_handler.h"
#include "ddp_handler.h"
#include "partition_utils.h"

// Watchdog timeout in seconds
#define WDT_TIMEOUT 10

// Preferences for storing configuration
Preferences preferences;

// Variables for position tracking
uint16_t oldPositionRequest = 0;
uint16_t oldDdpPositionRequest = 0;
float position = 0;

// Uptime tracking
unsigned long bootTime = 0;

// LED state tracking
bool ledState = false;
unsigned long ledBlinkInterval = 1000;  // Default 1 second for connected state (in milliseconds)
bool locateMode = false;

// Timer for LED blinking
hw_timer_t *ledTimer = NULL;
volatile unsigned long timerCounter = 0;

// Morse code SOS pattern: ... --- ...
// Using simple on/off states, each element is 50ms (5 ticks of 10ms timer)
// Dot = 1 unit on, 1 unit off
// Dash = 3 units on, 1 unit off
// Letter gap = 3 units off
// Word gap = 7 units off
const bool morsePattern[] = {
  // S (...)
  1,0,  1,0,  1,0,     // 3 dots
  0,0,0,               // letter gap
  // O (---)
  1,1,1,0,  1,1,1,0,  1,1,1,0,  // 3 dashes
  0,0,0,               // letter gap
  // S (...)
  1,0,  1,0,  1,0,     // 3 dots
  0,0,0,0,0,0,0        // word gap
};
const int morsePatternLength = sizeof(morsePattern) / sizeof(morsePattern[0]);
volatile int morseIndex = 0;
volatile int morseUnitCounter = 0;

// LED timer interrupt handler
void IRAM_ATTR onLedTimer() {
  if (locateMode) {
    // Morse code SOS pattern
    // Each unit is 100ms (10 ticks of 10ms timer)
    morseUnitCounter++;
    if (morseUnitCounter >= 10) {  // 100ms elapsed
      morseUnitCounter = 0;

      // Set LED based on current pattern
      digitalWrite(statusLedPin, morsePattern[morseIndex] ? HIGH : LOW);

      // Move to next element
      morseIndex++;
      if (morseIndex >= morsePatternLength) {
        morseIndex = 0;
      }
    }
  } else {
    // Normal blinking mode
    timerCounter++;
    unsigned long ticksPerInterval = ledBlinkInterval / 10;
    if (timerCounter >= ticksPerInterval) {
      ledState = !ledState;
      digitalWrite(statusLedPin, ledState ? HIGH : LOW);
      timerCounter = 0;
    }
  }
}

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
  pinMode(statusLedPin, OUTPUT);
  digitalWrite(statusLedPin, LOW);
  ledState = false;

  // Configure timer for LED blinking (Timer 0, prescaler 80 for 1MHz, interrupt every 10ms)
  ledTimer = timerBegin(0, 80, true);  // Timer 0, prescaler 80, count up
  timerAttachInterrupt(ledTimer, &onLedTimer, true);  // Attach interrupt handler
  timerAlarmWrite(ledTimer, 10000, true);  // Trigger every 10ms (10000 microseconds)
  timerAlarmEnable(ledTimer);  // Enable the timer

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

  // Load saved ArtNet configuration
  artnetUniverseConfig = preferences.getInt("artnetUniverse", startUniverse);
  artnetChannelConfig = preferences.getInt("artnetChannel", 0);
  artnetEnabledConfig = preferences.getBool("artnetEnabled", true);
  artnetDebugConfig = preferences.getBool("artnetDebug", false);
  artnet16BitConfig = preferences.getBool("artnet16Bit", false);

  // Initialize stepper
  initializeStepper();

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
  initializeArtNet();
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

  // Update non-blocking homing state machine
  updateHoming();

  server.handleClient();

  // Skip ArtNet, DDP, and DNS handling during OTA update to prevent interference
  if (!otaInProgress) {
    artnet.parse();
    handleDDP();
    
    // Process DNS requests for captive portal (only in AP mode)
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
      dnsServer.processNextRequest();
    }
  }

  handleSerialCommands();

  if(positionRequest != oldPositionRequest) {
    // New position requested from ArtNet!
    oldPositionRequest = positionRequest;

    // Only act on ArtNet commands if enabled, homed, and not currently homing
    if (!artnetEnabledConfig) {
      // ArtNet disabled by user
      if (artnetDebugConfig) {
        Serial.println("Ignoring ArtNet command - ArtNet disabled");
      }
    } else if (!homed) {
      // Not homed, ignore the command
      if (artnetDebugConfig) {
        Serial.println("Ignoring ArtNet command - system not homed");
      }
    } else if (isHoming()) {
      // Currently homing, ignore the command
      if (artnetDebugConfig) {
        Serial.println("Ignoring ArtNet command - homing in progress");
      }
    } else {
      // Enabled, homed and not homing, execute the command
      artnetPacketsActedOn++;

      position = calcPosition(positionRequest, artnet16BitConfig);

      Serial.print("ArtNet: Moving to new position ");
      Serial.print(positionRequest);
      Serial.print(" - ");
      float maxValue = artnet16BitConfig ? 65535.0 : 255.0;
      Serial.print(int((float)((float)positionRequest / maxValue) * 100));
      Serial.print("% - ");
      Serial.println((int)position);

      stepper->moveTo((int)position);
    }
  }

  if(ddpPositionRequest != oldDdpPositionRequest) {
    // New position requested from DDP!
    oldDdpPositionRequest = ddpPositionRequest;

    // Only act on DDP commands if enabled, homed, and not currently homing
    if (!ddpEnabledConfig) {
      // DDP disabled by user
      if (ddpDebugConfig) {
        Serial.println("Ignoring DDP command - DDP disabled");
      }
    } else if (!homed) {
      // Not homed, ignore the command
      if (ddpDebugConfig) {
        Serial.println("Ignoring DDP command - system not homed");
      }
    } else if (isHoming()) {
      // Currently homing, ignore the command
      if (ddpDebugConfig) {
        Serial.println("Ignoring DDP command - homing in progress");
      }
    } else {
      // Enabled, homed and not homing, execute the command
      ddpPacketsActedOn++;

      position = calcPosition(ddpPositionRequest, ddp16BitConfig);

      Serial.print("DDP: Moving to new position ");
      Serial.print(ddpPositionRequest);
      Serial.print(" - ");
      float maxValue = ddp16BitConfig ? 65535.0 : 255.0;
      Serial.print(int((float)((float)ddpPositionRequest / maxValue) * 100));
      Serial.print("% - ");
      Serial.println((int)position);

      stepper->moveTo((int)position);
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
    case '?':
      Serial.println("\n=== Available Serial Commands ===");
      Serial.println("?  - Show this help menu");
      Serial.println("h  - Start homing sequence");
      Serial.println("r  - Reboot device");
      Serial.println("s  - Print connection status");
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
