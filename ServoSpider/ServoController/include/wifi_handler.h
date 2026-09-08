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

// WiFi mode selector (2026-09-08) - makes explicit what used to be
// implicit boot-time behavior. Default (0) is byte-for-byte the same as
// every device already running: try client if credentials exist, fall
// back to AP on failure (and, as of today's AP-fallback retry work, keep
// periodically retrying as a client while in that fallback). The other
// two are real, named alternatives:
//   1 (Client Only) - never starts an AP, on boot or on a failed retry.
//     A device with wrong/missing credentials in this mode has *no*
//     network access at all (not even a recovery AP) until either the
//     credentials are fixed some other way or the mode is changed back -
//     the serial 'a' command ("Switch to AP mode") is the deliberate,
//     documented recovery path for that case. Choosing this mode is an
//     explicit acceptance of that tradeoff, not a bug.
//   2 (AP Only) - never attempts a client connection at all, regardless
//     of saved credentials; always boots straight into AP mode. Useful
//     for setup/bench work or a device that's deliberately not meant to
//     join a network.
#define WIFI_MODE_AP_FALLBACK 0
#define WIFI_MODE_CLIENT_ONLY 1
#define WIFI_MODE_AP_ONLY 2
extern int wifiModeConfig;

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
