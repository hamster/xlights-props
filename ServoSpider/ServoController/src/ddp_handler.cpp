#include "ddp_handler.h"
#include <Preferences.h>

// DDP Configuration variables
int ddpServoChannelConfig = 0;
bool ddpEnabled = false;
bool ddpEnabledConfig = true;
bool ddpDebugConfig = false;

// DDP Data
uint8_t ddpPositionRequest = 0;
uint8_t ddpLastReceivedPosition = 0;

// DDP Statistics
unsigned long ddpPacketsReceived = 0;
unsigned long ddpPacketsActedOn = 0;

// UDP object
WiFiUDP ddpUdp;

// External preferences object
extern Preferences preferences;

void initDDP() {
  // Load configuration from preferences (stored as 1-based)
  ddpServoChannelConfig = preferences.getInt("ddpServoChannel", 1);
  ddpEnabledConfig = preferences.getBool("ddpEnabled", true);
  ddpDebugConfig = preferences.getBool("ddpDebug", false);
  ddpEnabled = ddpEnabledConfig && (ddpServoChannelConfig >= 1);

  Serial.println("DDP Configuration:");
  Serial.print("  Servo Channel: ");
  Serial.println(ddpServoChannelConfig);  // Already 1-based
  Serial.print("  Debug: ");
  Serial.println(ddpDebugConfig ? "Enabled" : "Disabled");
  Serial.print("  User Enabled: ");
  Serial.println(ddpEnabledConfig ? "Yes" : "No");
  Serial.print("  Active: ");
  Serial.println(ddpEnabled ? "Yes" : "No");

  if (ddpEnabled) {
    // Start or restart UDP listener
    ddpUdp.stop();
    if (ddpUdp.begin(DDP_PORT)) {
      Serial.print("DDP server started on port ");
      Serial.println(DDP_PORT);
    } else {
      Serial.println("Failed to start DDP server");
      ddpEnabled = false;
    }
  } else {
    Serial.println("DDP is disabled");
    ddpUdp.stop();
  }
}

void handleDDP() {
  if (!ddpEnabled) {
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
  if (ddpDebugConfig) {
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

  // Debug output - print all channel data
  if (ddpDebugConfig && bytesRead > 0) {
    Serial.print("  Channel data [");
    Serial.print(header.dataOffset);
    Serial.print("-");
    Serial.print(header.dataOffset + bytesRead - 1);
    Serial.print("]: ");
    for (uint16_t i = 0; i < bytesRead; i++) {
      if (i > 0) Serial.print(",");
      Serial.print(channelData[i]);
    }
    Serial.println();
  }

  // Calculate if our servo channel is in this packet
  // Servo channel is stored as 1-based, convert to 0-based byte offset
  uint32_t byteOffset = ddpServoChannelConfig - 1;
  uint32_t startByte = header.dataOffset;
  uint32_t endByte = startByte + bytesRead - 1;

  // Check if our servo channel falls within this packet's data range
  if (byteOffset >= startByte && byteOffset <= endByte) {
    // Calculate offset within this packet's data
    uint32_t byteOffsetInPacket = byteOffset - startByte;

    // Extract the servo position byte
    ddpPositionRequest = channelData[byteOffsetInPacket];
    ddpLastReceivedPosition = ddpPositionRequest;
    ddpPacketsActedOn++;

    if (ddpDebugConfig) {
      Serial.print("  -> Servo channel ");
      Serial.print(ddpServoChannelConfig);
      Serial.print(" (byte offset ");
      Serial.print(byteOffset);
      Serial.print(") value: ");
      Serial.println(ddpPositionRequest);
    }
  }
  else{
    if (ddpDebugConfig) {
      Serial.print("  -> Servo channel ");
      Serial.print(ddpServoChannelConfig);
      Serial.print(" (byte offset ");
      Serial.print(byteOffset);
      Serial.println(") not in this packet");
    }
  }

  // Flush any remaining packet data to ensure the buffer is clear for the next packet
  while (ddpUdp.available()) {
    ddpUdp.read();
  }
}
