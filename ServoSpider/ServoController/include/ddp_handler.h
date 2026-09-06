#ifndef DDP_HANDLER_H
#define DDP_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// DDP Configuration
// Stepper is always on channel 1 (8-bit) or channels 1-2 (16-bit)

// DDP Statistics
extern unsigned long ddpPacketsReceived;
// Counts packets discarded by the out-of-order/duplicate check below - see
// handleDDP()'s sequence-number comment. Real, nonzero values here confirm
// UDP reordering is actually happening on the network, not just a
// theoretical concern.
extern unsigned long ddpPacketsRejectedOutOfOrder;

// DDP Protocol Constants
#define DDP_PORT 4048
#define DDP_HEADER_SIZE 10
// Max payload bytes handled per packet (standard Ethernet MTU 1500 - 20
// bytes IP - 8 bytes UDP = 1472). Real DDP senders fragment into packets
// at or under this, so this is a hard safety cap, not a normal-case limit -
// see handleDDP()'s comment on why the buffer must never be sized directly
// from the packet's own (untrusted) header.dataLen field.
#define DDP_MAX_DATA_SIZE 1472

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
