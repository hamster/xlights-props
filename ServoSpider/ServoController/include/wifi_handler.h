#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>

// Default AP configuration
#define DEFAULT_AP_SSID "ServoController"
#define DEFAULT_AP_PASSWORD "Spiders1234"
#define DEFAULT_AP_APPEND_MAC true

// WiFi credentials
extern String ssid;
extern String password;
extern String hostname;

// Static IP configuration
extern bool useStaticIp;
extern String staticIp;
extern String staticGateway;
extern String staticSubnet;

// AP settings
extern String ap_ssid;
extern String ap_password;
extern bool ap_append_mac;

// Connection timeout
extern const unsigned long WIFI_TIMEOUT;

// How often checkWifiConnection() polls connection status while in station
// mode with saved credentials, seconds. 0 disables the periodic check
// entirely (a dropped connection then only recovers via a manual
// "Connect Now" or a reboot). Matches this codebase's existing "0 =
// disabled" convention (stepperBlankTimeConfig, ledBlankTimeConfig).
extern int wifiRetryIntervalConfig;

// DNS server for captive portal
extern DNSServer dnsServer;
extern const byte DNS_PORT;

// WiFi functions
// preserveAp: see connectToWifi()'s declaration comment (wifi_handler.cpp) -
// defaults false so every existing no-arg call site (boot, manual
// "Connect Now", the already-STA-then-dropped reconnect path) is unchanged.
boolean connectToWifi(bool preserveAp = false);
String getAPName();
void startAccessPoint();
void checkWifiConnection();

#endif
