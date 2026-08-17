#include "ddp_handler.h"
#include "artnet_handler.h"
#include "led_handler.h"
#include <Preferences.h>
#include "protocol_common.h"

// DDP Configuration
// Stepper is always on channel 1 (8-bit) or channels 1-2 (16-bit)

// DDP Statistics
unsigned long ddpPacketsReceived = 0;

// UDP object
WiFiUDP ddpUdp;
bool ddpServerStarted = false;

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

  // Read all channel data into a buffer for debug output and processing
  uint8_t channelData[header.dataLen];
  uint16_t bytesRead = 0;
  while (ddpUdp.available() && bytesRead < header.dataLen) {
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

  // LED data always starts at channel 3 for RGB alignment
  const int LED_START_OFFSET = 3;

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
