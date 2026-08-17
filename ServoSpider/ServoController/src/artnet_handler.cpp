#include "artnet_handler.h"
#include "led_handler.h"
#include "protocol_common.h"

// ArtNet objects and settings
ArtnetWiFiReceiver artnet;
const int startUniverse = 0;
int artnetUniverseConfig = startUniverse;
int artnetChannelsPerUniverseConfig = 512;
// Stepper is always on channel 1 (8-bit) or channels 1-2 (16-bit)

// ArtNet statistics
unsigned long artnetPacketsReceived = 0;

void onDmxFrame(const uint8_t* data, uint16_t size, const ArtDmxMetadata& metadata, const ArtNetRemoteInfo& remote) {
  artnetPacketsReceived++;

  if (protocolDebugConfig) {
    Serial.print("Artnet: src ");
    Serial.print(remote.ip);
    Serial.print(":");
    Serial.print(remote.port);
    Serial.print(", size = ");
    Serial.print(size);
    Serial.print(", universe = ");
    Serial.print(metadata.universe);
    Serial.print(" (");
    Serial.print(metadata.sequence);
    Serial.print(") :");
    for (size_t i = 0; i < size; ++i) {
      Serial.print(data[i]);
      if (i < size - 1) {
        Serial.print(",");
      }
    }
    Serial.println();
  }

  // Update the protocol timestamp for blank time tracking
  lastProtocolUpdateTime = millis();

  // LED data always starts at channel 3 (byte offset 2) for RGB alignment
  const int LED_START_OFFSET = 2;

  // Process stepper control if enabled
  if (stepperControlEnabled) {
    // Stepper is always on channel 1 (index 0) for 8-bit, or channels 1-2 (index 0-1) for 16-bit
    if (control16BitConfig) {
      // 16-bit mode: read two consecutive channels (MSB first)
      if (size > 1) {
        positionRequest = ((uint16_t)data[0] << 8) | data[1];
      }
    } else {
      // 8-bit mode: read single channel
      if (size > 0) {
        positionRequest = data[0];
      }
    }
  }

  // Update LEDs with channel data
  // LED data starts at channel 3 (byte offset 2) for RGB pixel alignment
  if (size > LED_START_OFFSET) {
    updatePixelLeds((uint8_t*)data, size, LED_START_OFFSET);
  }
}

void initializeArtNet() {
  artnet.begin();
  // Subscribe to configured universe - onDmxFrame will execute every time a packet is received by the ESP32
  artnet.subscribeArtDmxUniverse(artnetUniverseConfig, onDmxFrame);

  Serial.print("ArtNet listening on universe: ");
  Serial.println(artnetUniverseConfig);
}
