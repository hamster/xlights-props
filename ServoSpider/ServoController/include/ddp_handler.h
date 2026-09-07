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

// Bench-diagnostic tunables (RAM-only, $SET/$GET ddpRxLog / ddpAck) added
// 2026-09-06 to directly instrument DDP reception quality - independent of
// any tracking mode or stepper motion (the device doesn't even need to be
// homed for these to be useful; positionRequest and these logs update
// regardless of homed state). See tools/README.md's DDP reception test
// section for the full methodology.
//
// ddpRxLogConfig: records one clean, single-line, CSV-parseable entry per
// DDP packet - "<ms>,DRX,<seq>,<value>" for an accepted/parsed packet,
// "<ms>,DDPREJ,<seq>,<lastAcceptedSeq>" for one rejected by the sequence
// check, plus a throttled "<ms>,RSSI,<dbm>" - into an in-RAM buffer
// retrievable via GET /ddp-rx-log (?clear=1 to wipe), instead of over
// Serial like protocolDebugConfig's verbose format. Deliberately NOT
// Serial-based: a reception test needs zero serial I/O during its timed
// portion, both to rule out USB-serial contention as a confound and to
// match real production conditions (FPP/xLights never has a serial
// connection open) - see tools/ddp_reception_test.py.
extern bool ddpRxLogConfig;
// Appends one line (no trailing newline needed) to the buffer above -
// used by ddp_handler.cpp's own call sites; exposed here only because
// html_handler.cpp's retrieval endpoint needs the read/clear pair.
String getDdpRxLog();
void clearDdpRxLog();
// ddpAckConfig: immediately echoes a small UDP ACK packet (the received
// sequence number, 1 byte) back to the sender's own IP/port - captured
// straight from WiFiUDP's remoteIP()/remotePort() for whatever packet was
// just received - so the sender can directly measure round-trip time and
// detect genuine loss (an unacked send), decoupled from this firmware's
// own sequence-acceptance logic (the ack fires for every packet that
// reaches the UDP layer, accepted or rejected).
extern bool ddpAckConfig;

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
