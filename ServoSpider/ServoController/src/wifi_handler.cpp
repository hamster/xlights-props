#include <WiFi.h>
#include <esp_task_wdt.h>
#include <ESPmDNS.h>
#include "wifi_handler.h"
#include "led_handler.h"
#include "partition_utils.h"
#include "html_handlers.h"  // for `server` - see connectToWifi()'s preserveAp wait loop

// DNS server for captive portal
DNSServer dnsServer;
const byte DNS_PORT = 53;

// WiFi credentials
String ssid = "";
String password = "";
String hostname = "";

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

// See declaration comment (wifi_handler.h). Was a hardcoded 30s CHECK_INTERVAL
// local to checkWifiConnection() until 2026-09-07, when it became a
// configurable, persisted setting - default kept at 20s per the user's
// request. The existing 3-consecutive-failed-checks debounce before actually
// reconnecting was left untouched rather than also shortened/removed
// alongside this - this exact function is already suspected (not confirmed)
// as the source of a real, unexplained ~20-30s WiFi/HTTP unresponsiveness
// observed twice on the bench (see TODO.md, Wave 3) at the old 30s/3-strike
// settings; making reconnects trigger-happier at the same time as adding
// this config felt like the wrong moment to also loosen that guard.
int wifiRetryIntervalConfig = 20;

// connect to wifi - returns true if successful or false if not.
//
// preserveAp (2026-09-07): if true, uses WIFI_AP_STA instead of plain
// WIFI_STA, so an already-running AP (softAP + captive-portal DNS) stays
// reachable for the whole connection attempt instead of dropping the
// instant this function is called - the ESP32 genuinely supports
// concurrent AP+STA on one radio, this isn't a hack. Used by
// checkWifiConnection()'s AP-fallback retry (see there): the entire point
// of that retry is *added* recovery safety, so a failed attempt must never
// leave the device with neither the real network nor its local AP
// reachable, which is exactly what would happen if this just force-set
// WIFI_STA like the plain boot-time call does. On success in this mode,
// the AP is explicitly torn back down and the radio dropped to plain STA
// once we're actually back on the real network - this doesn't stay in
// dual mode indefinitely once it no longer needs to.
boolean connectToWifi(bool preserveAp) {
  WiFi.mode(preserveAp ? WIFI_AP_STA : WIFI_STA);

  if (preserveAp) {
    // WiFi.mode() switching AP_STA<->AP has been unreliable in some
    // ESP32 Arduino core versions about keeping a previously-announced
    // softAP actually broadcasting - re-assert it explicitly rather than
    // assume it survived the mode change, since silently losing the
    // fallback AP here would defeat the entire point of preserveAp.
    String fullAPName = getAPName();
    WiFi.softAP(fullAPName.c_str(), ap_password.c_str());
  }

  // Set hostname
  if (hostname.length() > 0) {
    WiFi.setHostname(hostname.c_str());
    Serial.print("Hostname set to: ");
    Serial.println(hostname);
  }

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
  statusLedBlinkInterval = 100;  // Rapid blink during connection attempt (timer interrupt handles it)

  while (WiFi.status() != WL_CONNECTED) {
    // Feed watchdog to prevent reset during connection
    esp_task_wdt_reset();

    if (millis() - startTime > WIFI_TIMEOUT) {
      Serial.println("WiFi connection timeout!");
      if (preserveAp) {
        // Drop the failed STA association attempt cleanly and fall back to
        // plain AP - don't leave the radio sitting in AP_STA with a dead
        // STA half, and don't leave statusLedBlinkInterval changed since
        // AP mode already uses this same 100ms rapid blink.
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
      }
      return false;
    }

    if (preserveAp) {
      // Found the hard way (2026-09-08): keeping the AP *associable* during
      // a retry isn't the same as keeping it *usable* - this whole loop
      // blocks the caller (checkWifiConnection(), called from loop()), so
      // without this, server.handleClient()/dnsServer.processNextRequest()
      // never ran for the entire up-to-WIFI_TIMEOUT duration of every
      // retry attempt, and a phone connected to the AP trying to load the
      // captive portal page had a real chance of its request landing in
      // that dead window and just hanging - exactly what was reported.
      // Serviced in short bursts (every 50ms) rather than once per 500ms
      // dot below, so a page load doesn't have to wait up to half a second
      // for its first byte.
      for (int i = 0; i < 10; i++) {
        server.handleClient();
        dnsServer.processNextRequest();
        delay(50);
      }
    } else {
      delay(500);
    }
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  if (preserveAp) {
    // Genuinely back on the real network now - drop the fallback AP rather
    // than staying in AP_STA indefinitely, matching what a normal
    // successful boot-time connect looks like. The captive-portal DNS
    // server only makes sense while the AP is up.
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.println("AP-fallback retry succeeded - dropped the fallback AP, now client-only.");
  }

  // Disable WiFi modem sleep (power-save) - the ESP32 Arduino core's default
  // for station mode. Never touched before 2026-09-06, when a dedicated DDP
  // reception test (tools/ddp_reception_test.py) caught a real, sustained
  // round-trip-time jump - a healthy <50ms baseline shifting to a flat
  // ~400ms plateau partway through a 60s run, with 100% packet delivery and
  // strong, stable RSSI throughout (ruling out signal quality or loss).
  // That signature - a fixed added latency once triggered, not packet
  // loss, not signal-strength-correlated - is the classic symptom of modem
  // sleep: the radio periodically sleeps between AP beacon/DTIM intervals,
  // queueing inbound traffic (including DDP) until the next wake window
  // instead of processing it immediately. A real-time control protocol
  // being queued for up to a DTIM interval at a time is exactly the kind
  // of jitter this project can't tolerate - disabling it trades a small
  // amount of extra power draw (not a concern - this is mains-powered
  // prop hardware, not battery) for consistently low, real-time latency.
  WiFi.setSleep(false);

  // Set to slow blink for connected state (timer interrupt handles it)
  statusLedBlinkInterval = 1000;

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
  statusLedBlinkInterval = 100;

  // Start DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("DNS server started for captive portal");
}

// Check WiFi connection and attempt reconnect if needed
// Call this periodically from main loop
void checkWifiConnection() {
  static unsigned long lastCheckTime = 0;
  static int disconnectCount = 0;
  const int MAX_DISCONNECT_COUNT = 3;  // Reconnect after 3 failed checks

  // 0 = feature disabled entirely - see wifiRetryIntervalConfig's
  // declaration comment (wifi_handler.h).
  if (wifiRetryIntervalConfig <= 0) {
    return;
  }
  unsigned long checkIntervalMs = (unsigned long)wifiRetryIntervalConfig * 1000UL;

  unsigned long currentTime = millis();

  // Only check periodically
  if (currentTime - lastCheckTime < checkIntervalMs) {
    return;
  }

  lastCheckTime = currentTime;

  // Nothing to retry with in any mode.
  if (ssid.length() == 0) {
    return;
  }

  // AP fallback (2026-09-07): a device that failed to connect at boot (or
  // lost its connection and, via some other path, ended up AP-only) sat
  // here forever before this - nothing ever tried the client connection
  // again short of a manual "Connect Now" or a reboot. Selective on purpose:
  // only fires with real saved credentials (the ssid check above), on the
  // same configurable interval as the already-connected case below, and via
  // connectToWifi(true) - see its declaration comment for why that keeps
  // the fallback AP reachable through a failed attempt rather than
  // trading "stuck in AP forever" for "occasionally unreachable in both
  // modes at once." No debounce here, unlike the already-connected branch
  // below - there's no "currently working" state to protect against a
  // false trip, every check while genuinely AP-only is a real, unambiguous
  // "still not on the real network" reading.
  if (WiFi.getMode() == WIFI_AP) {
    // Skip this cycle entirely if someone's actually connected to the AP
    // right now (2026-09-08, found on the bench) - a client connection
    // attempt needs the radio to briefly sync the AP's channel to whatever
    // it's scanning/connecting to, which degrades or drops packets for
    // anyone using the AP at that exact moment. server.handleClient()
    // staying serviced during the attempt (see connectToWifi()'s
    // preserveAp wait loop) keeps small requests working, but a real
    // person is most likely on the AP specifically to load the Settings
    // page and fix the credentials that are presumably the reason this
    // fallback is active in the first place - the last thing that moment
    // needs is the retry itself corrupting a large page load out from
    // under them. Retrying while genuinely unattended is the whole point
    // of this feature; retrying while someone's actively there working on
    // it is counterproductive. Just waits for the next interval instead -
    // if they're still connected then, same story, skip again.
    if (WiFi.softAPgetStationNum() > 0) {
      Serial.println("AP fallback: skipping retry, a client is connected to the AP");
      return;
    }
    Serial.println("AP fallback: retrying client connection...");
    if (connectToWifi(true)) {
      disconnectCount = 0;  // fresh start now that we're genuinely reconnected
      // Matches setup()'s own post-connect mDNS start - didn't have this
      // until 2026-09-08, so hostname.local never worked after recovering
      // via this path, only after a fresh boot.
      if (MDNS.begin(hostname.c_str())) {
        Serial.print("mDNS responder started: ");
        Serial.print(hostname);
        Serial.println(".local");
        MDNS.addService("http", "tcp", 80);
      } else {
        Serial.println("Error starting mDNS");
      }
    }
    return;
  }

  // Check connection status
  if (WiFi.status() != WL_CONNECTED) {
    disconnectCount++;
    Serial.print("WiFi disconnected (count: ");
    Serial.print(disconnectCount);
    Serial.print(", status: ");
    Serial.print(WiFi.status());
    Serial.print(", free heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes)");

    if (disconnectCount >= MAX_DISCONNECT_COUNT) {
      Serial.println("Multiple disconnect detections, attempting reconnect...");

      // Reset the WiFi connection
      WiFi.disconnect();
      delay(100);

      if (connectToWifi()) {
        Serial.println("WiFi reconnected successfully");
        disconnectCount = 0;
      } else {
        Serial.print("WiFi reconnect failed, will retry in ");
        Serial.print(wifiRetryIntervalConfig);
        Serial.println(" seconds");
      }
    }
  } else {
    // Connected, reset disconnect counter
    if (disconnectCount > 0) {
      Serial.println("WiFi connection restored");
      disconnectCount = 0;
    }
  }
}

