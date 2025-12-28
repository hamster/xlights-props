#ifndef ARTNET_HANDLER_H
#define ARTNET_HANDLER_H

#include <Arduino.h>
#include <ArtnetWiFi.h>

// ArtNet objects and settings
extern ArtnetWiFiReceiver artnet;
extern const int startUniverse;
extern int artnetUniverseConfig;
extern bool artnetDebugConfig;
extern int artnetChannelConfig;
extern bool artnetEnabledConfig;

// ArtNet data
extern int previousDataLength;
extern char frame[256];
extern uint8_t positionRequest;
extern uint8_t lastReceivedPosition;

// ArtNet statistics
extern unsigned long artnetPacketsReceived;
extern unsigned long artnetPacketsActedOn;

// ArtNet functions
void onDmxFrame(const uint8_t* data, uint16_t size, const ArtDmxMetadata& metadata, const ArtNetRemoteInfo& remote);
void initializeArtNet();

#endif
