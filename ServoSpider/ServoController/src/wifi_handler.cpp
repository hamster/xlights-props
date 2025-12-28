#include <WiFi.h>
#include "wifi_handler.h"
#include "main.h"
#include "partition_utils.h"

// DNS server for captive portal
DNSServer dnsServer;
const byte DNS_PORT = 53;

// WiFi credentials
String ssid = "";
String password = "";

// Static IP configuration
bool useStaticIp = false;
String staticIp = "";
String staticGateway = "";
String staticSubnet = "255.255.255.0";

// AP settings
String ap_ssid = DEFAULT_AP_SSID;
String ap_password = DEFAULT_AP_PASSWORD;
bool ap_append_mac = DEFAULT_AP_APPEND_MAC;

// Connection timeout
const unsigned long WIFI_TIMEOUT = 10000; // 10 seconds

// connect to wifi – returns true if successful or false if not
boolean connectToWifi() {
  WiFi.mode(WIFI_STA);

  // Configure static IP if enabled
  if (useStaticIp && staticIp.length() > 0) {
    IPAddress ip, gateway, subnet;

    if (ip.fromString(staticIp) && gateway.fromString(staticGateway) && subnet.fromString(staticSubnet)) {
      if (WiFi.config(ip, gateway, subnet)) {
        Serial.println("Static IP configured");
      } else {
        Serial.println("Failed to configure static IP, using DHCP");
      }
    } else {
      Serial.println("Invalid static IP settings, using DHCP");
    }
  }

  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();
  ledBlinkInterval = 100;  // Rapid blink during connection attempt (timer interrupt handles it)

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > WIFI_TIMEOUT) {
      Serial.println("WiFi connection timeout!");
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  // Set to slow blink for connected state (timer interrupt handles it)
  ledBlinkInterval = 1000;

  return true;
}

String getAPName() {
  String apName = ap_ssid;
  if (ap_append_mac) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[5];
    sprintf(macStr, "%02X%02X", mac[4], mac[5]);
    apName += "-";
    apName += macStr;
  }
  return apName;
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  String fullAPName = getAPName();
  WiFi.softAP(fullAPName.c_str(), ap_password.c_str());

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP Mode - Connect to: ");
  Serial.println(fullAPName);
  Serial.print("IP address: ");
  Serial.println(IP);

  // Set rapid blink for AP mode
  ledBlinkInterval = 100;

  // Start DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("DNS server started for captive portal");
}


