#include "artnet_handler.h"

// ArtNet objects and settings
ArtnetWiFiReceiver artnet;
const int startUniverse = 0;
int artnetUniverseConfig = startUniverse;
bool artnetDebugConfig = false;
int artnetChannelConfig = 0;
bool artnetEnabledConfig = true;

// ArtNet data
uint8_t positionRequest = 0;
uint8_t lastReceivedPosition = 0;

// ArtNet statistics
unsigned long artnetPacketsReceived = 0;
unsigned long artnetPacketsActedOn = 0;

void onDmxFrame(const uint8_t* data, uint16_t size, const ArtDmxMetadata& metadata, const ArtNetRemoteInfo& remote){

  artnetPacketsReceived++;

  if(artnetDebugConfig){
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

  if(size > artnetChannelConfig){
    positionRequest = data[artnetChannelConfig];
    lastReceivedPosition = data[artnetChannelConfig];
  }

}

void initializeArtNet() {
  artnet.begin();
  // Subscribe to configured universe - onDmxFrame will execute every time a packet is received by the ESP32
  artnet.subscribeArtDmxUniverse(artnetUniverseConfig, onDmxFrame);

  Serial.print("ArtNet listening on universe: ");
  Serial.println(artnetUniverseConfig);
}
