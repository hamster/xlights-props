#ifndef DDP_HANDLER_H
#define DDP_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// DDP Configuration
// Stepper is always on channel 1 (8-bit) or channels 1-2 (16-bit)

// DDP Statistics
extern unsigned long ddpPacketsReceived;

// DDP Protocol Constants
#define DDP_PORT 4048
#define DDP_HEADER_SIZE 10

// DDP Packet Header Structure
struct DDPHeader {
  uint8_t flags;
  uint8_t sequenceNum;
  uint8_t dataType;
  uint8_t destination;
  uint32_t dataOffset;
  uint16_t dataLen;
};

// Functions
void initDDP();
void handleDDP();

#endif
