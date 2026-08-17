#ifndef ARTNET_HANDLER_H
#define ARTNET_HANDLER_H

#include <Arduino.h>
#include <ArtnetWiFi.h>

// ArtNet objects and settings
extern ArtnetWiFiReceiver artnet;
extern const int startUniverse;
extern int artnetUniverseConfig;
extern int artnetChannelsPerUniverseConfig;
// Stepper is always on channel 1 (8-bit) or channels 1-2 (16-bit)

// ArtNet statistics
extern unsigned long artnetPacketsReceived;

// ArtNet functions
void onDmxFrame(const uint8_t* data, uint16_t size, const ArtDmxMetadata& metadata, const ArtNetRemoteInfo& remote);
void initializeArtNet();

#endif
