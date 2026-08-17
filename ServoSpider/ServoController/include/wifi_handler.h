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

// DNS server for captive portal
extern DNSServer dnsServer;
extern const byte DNS_PORT;

// WiFi functions
boolean connectToWifi();
String getAPName();
void startAccessPoint();
void checkWifiConnection();

#endif
