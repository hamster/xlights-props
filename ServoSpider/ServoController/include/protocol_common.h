#ifndef PROTOCOL_COMMON_H
#define PROTOCOL_COMMON_H

#include <cstdint>
#include <Arduino.h>

typedef enum {
  PROTOCOL_NONE,
  PROTOCOL_DDP
} protocolType;

// Protocol settings
extern protocolType protocolConfig;
extern bool stepperControlEnabled;  // Enable stepper control (stepper uses channel 1, or 1-2 for 16-bit)
extern bool control16BitConfig;     // 16-bit stepper control (only used if stepperControlEnabled)
extern bool protocolDebugConfig;
// Temporary tuning aid: one compact CSV line per processed DDP position
// command (ms,ddpVal,cmdPos,curPos,delta,lag,profile,curSpeedHz,
// targetSpeedHz), independent of protocolDebugConfig's verbose per-packet
// dump - for capturing motion data to diagnose tracking-profile tuning
// issues. Runtime-only, not persisted. See the position-handling block in
// main.cpp's loop(). Candidate for removal/replacement once the planned
// Debug tab exists.
extern bool compactLogEnabled;

// Blank time settings (used by DDP)
extern int ledBlankTimeConfig;      // Seconds before LEDs turn off (0 = disabled)
extern int stepperBlankTimeConfig;  // Seconds before stepper goes to zero (0 = disabled)
extern unsigned long lastProtocolUpdateTime;  // Timestamp of last protocol update

// Shared position request (written by active protocol handler)
extern uint16_t positionRequest;

#endif