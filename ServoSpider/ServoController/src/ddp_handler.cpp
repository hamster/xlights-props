#include "ddp_handler.h"
#include "led_handler.h"
#include <Preferences.h>
#include "protocol_common.h"

// DDP Configuration
// Stepper is always on channel 1 (8-bit) or channels 1-2 (16-bit)

// DDP Statistics
unsigned long ddpPacketsReceived = 0;
unsigned long ddpPacketsRejectedOutOfOrder = 0;

// See declaration comments (ddp_handler.h).
bool ddpRxLogConfig = false;
bool ddpAckConfig = false;

// In-RAM buffer for ddpRxLogConfig's output - see its declaration comment
// for why this is HTTP-retrievable (GET /ddp-rx-log) rather than printed
// to Serial.
//
// Fixed-size ring buffer, NOT a growing/trimming String (2026-09-07) -
// see main.cpp's compactLogRing for the full story: a String that grows
// via += and periodically shrinks via substring() once over a byte cap
// caused real, severe data corruption (embedded NULs) during a real
// multi-minute session, from the repeated large reallocations that
// pattern requires. This buffer never reallocates after startup - writes
// wrap in place, oldest bytes are simply overwritten once full.
static char ddpRxLogRing[32768];
static size_t ddpRxLogHead = 0;  // next write position
static size_t ddpRxLogLen = 0;   // valid bytes currently stored, <= sizeof(ddpRxLogRing)

static void ddpRxRingAppendChar(char c) {
  ddpRxLogRing[ddpRxLogHead] = c;
  ddpRxLogHead = (ddpRxLogHead + 1) % sizeof(ddpRxLogRing);
  if (ddpRxLogLen < sizeof(ddpRxLogRing)) ddpRxLogLen++;
}

static void appendDdpRxLog(const char* line) {
  for (const char* p = line; *p; p++) ddpRxRingAppendChar(*p);
  ddpRxRingAppendChar('\n');
}

String getDdpRxLog() {
  String out;
  out.reserve(ddpRxLogLen + 1);
  size_t startIdx = (ddpRxLogLen < sizeof(ddpRxLogRing)) ? 0 : ddpRxLogHead;
  for (size_t i = 0; i < ddpRxLogLen; i++) {
    out += ddpRxLogRing[(startIdx + i) % sizeof(ddpRxLogRing)];
  }
  return out;
}

void clearDdpRxLog() {
  ddpRxLogHead = 0;
  ddpRxLogLen = 0;
}

// UDP object
WiFiUDP ddpUdp;
bool ddpServerStarted = false;

// Out-of-order/duplicate rejection - added 2026-09-06 after tuning-harness
// plots showed real spikes/dips in commanded position that didn't match
// the smooth synthetic wave being sent. Traced (via a new raw-DDP-value
// plot panel in tools/tuning_harness.py) back to real out-of-order UDP
// delivery, not a firmware-side artifact: positionRequest/cmdPos is a
// direct, unfiltered decode of whatever arrived, and this handler
// previously accepted every packet unconditionally regardless of the DDP
// header's own sequence field, which it already parsed but never used
// beyond a debug print. DDP's sequence field is 4 bits (1-15, cycling; 0
// is the documented "sequence numbering not in use" convention some
// senders rely on, and must always be accepted rather than treated as
// stale).
static uint8_t lastAcceptedDdpSeq = 0;

// True if `seq` is newer than `lastSeq` in DDP's 4-bit rolling sequence
// space (1-15 - 0 is the "unused" sentinel, handled by the caller before
// this is reached). Treats the 15 non-zero values as a circular sequence;
// "newer" means the forward distance from lastSeq to seq falls in the
// closer half of the cycle - the standard scheme for a small wrapping
// counter, so a genuine wraparound (15 -> 1) is still accepted while a
// stale/reordered packet arriving late is rejected.
static bool isNewerDdpSeq(uint8_t seq, uint8_t lastSeq) {
  int diff = ((int)seq - 1) - ((int)lastSeq - 1);  // map 1..15 -> 0..14 first
  if (diff < 0) diff += 15;
  return diff != 0 && diff <= 7;
}

// External preferences object
extern Preferences preferences;

void initDDP() {
  Serial.println("DDP Configuration:");
  if (stepperControlEnabled) {
    Serial.println("  Stepper on channel 1" + String(control16BitConfig ? "-2 (16-bit)" : " (8-bit)"));
  } else {
    Serial.println("  Stepper control disabled");
  }
  Serial.println("  LED data starts at channel 3 (RGB aligned)");

  // Always start DDP server (we check protocol in handleDDP)
  ddpUdp.stop();
  if (ddpUdp.begin(DDP_PORT)) {
    Serial.print("DDP server started on port ");
    Serial.println(DDP_PORT);
    ddpServerStarted = true;
  } else {
    Serial.println("Failed to start DDP server");
    ddpServerStarted = false;
  }
}

void handleDDP() {
  if (!ddpServerStarted) {
    return;
  }

  int packetSize = ddpUdp.parsePacket();

  if (packetSize < DDP_HEADER_SIZE) {
    return;
  }

  ddpPacketsReceived++;

  // Read DDP header
  uint8_t headerBytes[DDP_HEADER_SIZE];
  ddpUdp.read(headerBytes, DDP_HEADER_SIZE);

  DDPHeader header;
  header.flags = headerBytes[0];
  header.sequenceNum = headerBytes[1];
  header.dataType = headerBytes[2];
  header.destination = headerBytes[3];
  header.dataOffset = (uint32_t)headerBytes[4] << 24 |
                      (uint32_t)headerBytes[5] << 16 |
                      (uint32_t)headerBytes[6] << 8 |
                      (uint32_t)headerBytes[7];
  header.dataLen = (uint16_t)headerBytes[8] << 8 | (uint16_t)headerBytes[9];

  // Bench diagnostic: echo a small ACK back to the sender immediately -
  // before the sequence-accept/reject decision, so this reflects raw UDP
  // arrival at this device regardless of app-level sequence handling. Uses
  // the SAME WiFiUDP object right after the read that already completed
  // above (remoteIP()/remotePort() stay valid until the next parsePacket()
  // call) - beginPacket()/write()/endPacket() on a WiFiUDP object mid-loop
  // like this is the standard Arduino UDP send pattern, safe to call from
  // inside the receive handler.
  if (ddpAckConfig) {
    ddpUdp.beginPacket(ddpUdp.remoteIP(), ddpUdp.remotePort());
    ddpUdp.write(header.sequenceNum);
    ddpUdp.endPacket();
  }

  // Bench diagnostic: signal strength, throttled (not worth a query on
  // every single packet at a 40Hz+ receive rate) and tagged distinctly
  // from the DRX/DDPREJ lines above so a script parsing this stream can
  // pull it out separately.
  if (ddpRxLogConfig) {
    static unsigned long lastRssiLogMs = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastRssiLogMs >= 500) {
      lastRssiLogMs = nowMs;
      // Fixed buffer + snprintf(), not chained String concatenation - see
      // main.cpp's logCompactMotion() for why (sustained heap churn from
      // per-line String temporaries caused a real firmware crash there).
      char logLine[48];
      snprintf(logLine, sizeof(logLine), "%lu,RSSI,%d", nowMs, WiFi.RSSI());
      appendDdpRxLog(logLine);
    }
  }

  // Debug output header
  if (protocolDebugConfig) {
    Serial.print("DDP: seq ");
    Serial.print(header.sequenceNum);
    Serial.print(", size: ");
    Serial.print(packetSize);
    Serial.print(", flags: 0x");
    Serial.print(header.flags, HEX);
    Serial.print(", datalen: ");
    Serial.print(header.dataLen);
    Serial.print(", dataoffset: ");
    Serial.println(header.dataOffset);
  }

  // Reject a packet that arrived out of order or as a duplicate - see the
  // isNewerDdpSeq()/lastAcceptedDdpSeq comment above. seq==0 always passes
  // (sender opted out of sequencing); lastAcceptedDdpSeq==0 means this is
  // the first sequenced packet since boot/reconnect, nothing to compare
  // against yet, so it's accepted unconditionally and becomes the new
  // baseline.
  if (header.sequenceNum != 0 && lastAcceptedDdpSeq != 0 &&
      !isNewerDdpSeq(header.sequenceNum, lastAcceptedDdpSeq)) {
    ddpPacketsRejectedOutOfOrder++;
    if (protocolDebugConfig) {
      Serial.print("DDP: rejecting out-of-order/duplicate packet, seq=");
      Serial.print(header.sequenceNum);
      Serial.print(" (last accepted seq=");
      Serial.print(lastAcceptedDdpSeq);
      Serial.println(")");
    }
    if (ddpRxLogConfig) {
      char logLine[48];
      snprintf(logLine, sizeof(logLine), "%lu,DDPREJ,%d,%d", millis(), header.sequenceNum, lastAcceptedDdpSeq);
      appendDdpRxLog(logLine);
    }
    while (ddpUdp.available()) {
      ddpUdp.read();
    }
    return;
  }
  if (header.sequenceNum != 0) {
    lastAcceptedDdpSeq = header.sequenceNum;
  }

  // Read all channel data into a buffer for debug output and processing.
  //
  // header.dataLen comes straight from the packet's own header bytes - it's
  // untrusted network input, not a fact about how much data actually
  // follows. A malformed or truncated packet can claim any 16-bit value
  // (up to 65535) there regardless of the real UDP payload size. This used
  // to size a stack-allocated VLA directly from that field, so a single
  // packet claiming a large dataLen while sending little or no data would
  // blow the stack - a crash triggerable by any non-conforming packet, not
  // just a display/logic bug. Fixed by using a static buffer sized from
  // DDP_MAX_DATA_SIZE and bounding the read length to what's actually
  // available in this packet (packetSize), never to the untrusted header
  // field.
  static uint8_t channelData[DDP_MAX_DATA_SIZE];
  int actualDataLen = packetSize - DDP_HEADER_SIZE;
  if (actualDataLen < 0) {
    actualDataLen = 0;
  }
  if (actualDataLen > DDP_MAX_DATA_SIZE) {
    actualDataLen = DDP_MAX_DATA_SIZE;
  }

  if (protocolDebugConfig && header.dataLen != actualDataLen) {
    Serial.print("DDP: header dataLen (");
    Serial.print(header.dataLen);
    Serial.print(") doesn't match actual packet payload (");
    Serial.print(packetSize - DDP_HEADER_SIZE);
    Serial.println(") - using actual packet size instead");
  }

  uint16_t bytesRead = 0;
  while (ddpUdp.available() && bytesRead < actualDataLen) {
    channelData[bytesRead++] = ddpUdp.read();
  }

  // // Debug output - print all channel data
  // if (protocolDebugConfig && bytesRead > 0) {
  //   Serial.print("  Channel data [");
  //   Serial.print(header.dataOffset);
  //   Serial.print("-");
  //   Serial.print(header.dataOffset + bytesRead - 1);
  //   Serial.print("]: ");
  //   for (uint16_t i = 0; i < bytesRead; i++) {
  //     if (i > 0) Serial.print(",");
  //     Serial.print(channelData[i]);
  //   }
  //   Serial.println();
  // }


  while (ddpUdp.available()) {
    ddpUdp.read();
  }


  // Update the protocol timestamp for blank time tracking
  lastProtocolUpdateTime = millis();

  // LED data always starts at channel 3 (byte offset 2) for RGB alignment.
  // Byte offset 0 = channel 1, matching header.dataOffset's own 0-based convention.
  const int LED_START_OFFSET = 2;

  // Process stepper control if enabled
  if (stepperControlEnabled) {
    // Calculate if our servo channel is in this packet
    // Stepper is always on channel 1 (byte offset 0)
    uint32_t byteOffset = 0;
    uint32_t startByte = header.dataOffset;
    uint32_t endByte = startByte + bytesRead - 1;

    if (control16BitConfig) {
      // 16-bit mode: check if both bytes are in this packet
      if (byteOffset >= startByte && (byteOffset + 1) <= endByte) {
        // Calculate offset within this packet's data
        uint32_t byteOffsetInPacket = byteOffset - startByte;

        // Extract the servo position (MSB first)
        positionRequest = ((uint16_t)channelData[byteOffsetInPacket] << 8) | channelData[byteOffsetInPacket + 1];

        if (protocolDebugConfig) {
          Serial.print("  -> Stepper (16-bit, byte offset 0-1) value: ");
          Serial.println(positionRequest);
        }
        if (ddpRxLogConfig) {
          char logLine[48];
          snprintf(logLine, sizeof(logLine), "%lu,DRX,%d,%d", millis(), header.sequenceNum, positionRequest);
          appendDdpRxLog(logLine);
        }
      } else {
        if (protocolDebugConfig) {
          Serial.println("  -> Stepper (16-bit, byte offset 0-1) not in this packet");
        }
      }
    } else {
      // 8-bit mode: check if our servo channel falls within this packet's data range
      if (byteOffset >= startByte && byteOffset <= endByte) {
        // Calculate offset within this packet's data
        uint32_t byteOffsetInPacket = byteOffset - startByte;

        // Extract the servo position byte
        positionRequest = channelData[byteOffsetInPacket];

        if (protocolDebugConfig) {
          Serial.print("  -> Stepper (8-bit, byte offset 0) value: ");
          Serial.println(positionRequest);
        }
        if (ddpRxLogConfig) {
          char logLine[48];
          snprintf(logLine, sizeof(logLine), "%lu,DRX,%d,%d", millis(), header.sequenceNum, positionRequest);
          appendDdpRxLog(logLine);
        }
      } else {
        if (protocolDebugConfig) {
          Serial.println("  -> Stepper (8-bit, byte offset 0) not in this packet");
        }
      }
    }
  }

  // Update LEDs with channel data from this packet
  // LED data starts at channel 3 for RGB pixel alignment
  uint32_t startByte = header.dataOffset;
  uint32_t endByte = startByte + bytesRead;

  // Check if this packet contains any LED data
  if (endByte >= LED_START_OFFSET && bytesRead > 0) {
    // Calculate where in the packet's data the LED data begins
    uint32_t ledDataStartInPacket = 0;
    uint32_t ledStartChannel = LED_START_OFFSET;

    if (startByte <= ledStartChannel) {
      // Packet starts before or at LED start, LED data begins at offset within packet
      ledDataStartInPacket = ledStartChannel - startByte;
    } else {
      // Packet starts after LED start, all data is LED data
      ledDataStartInPacket = 0;
    }

    // Calculate which LED pixel this packet starts at
    uint32_t ledByteOffset = (startByte > ledStartChannel) ? (startByte - ledStartChannel) : 0;
    uint32_t ledPixelOffset = ledByteOffset / 3;

    if (protocolDebugConfig) {
      Serial.print("  -> LED data: packet offset ");
      Serial.print(ledDataStartInPacket);
      Serial.print(", LED pixel offset ");
      Serial.print(ledPixelOffset);
      Serial.print(", bytes ");
      Serial.println(bytesRead - ledDataStartInPacket);
    }

    // Update LEDs starting from the correct pixel offset
    updatePixelLedsFragmented(&channelData[ledDataStartInPacket],
                              bytesRead - ledDataStartInPacket,
                              ledPixelOffset);
  }

  // Flush any remaining packet data to ensure the buffer is clear for the next packet
  while (ddpUdp.available()) {
    ddpUdp.read();
  }
}
