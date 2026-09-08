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
// Deferred flash write for protocolDebugConfig, same reasoning as
// stepperSettingsPendingSave (stepper_handler.h) - GET /protocol-debug
// (the Debug tab's instant-apply checkbox) updates protocolDebugConfig in
// RAM immediately so it takes effect right away, but must not call
// preferences.putBool() synchronously from that handler: a real flash
// write briefly disables the flash cache, and doing so while the stepper
// is actively stepping (e.g. mid-homing, exactly when this tab tends to
// get used) risks crashing if any interrupt that isn't fully IRAM-resident
// fires during that window - see CLAUDE.md's homing-switch-ISR note and
// persist_log.cpp's top comment for the same underlying hazard hitting
// this project twice already. persistProtocolDebugIfPending() actually
// writes it, gated on the stepper being idle, same pattern as
// persistStepperSettingsIfPending().
extern bool protocolDebugPendingSave;
void persistProtocolDebugIfPending();  // Call every loop() iteration - no-op unless a save is pending
// Temporary tuning aid: one compact CSV line per processed DDP position
// command (ms,ddpVal,cmdPos,curPos,delta,lag,profile,curSpeedHz,
// targetSpeedHz,encoderCount,sgResult,switchTripped), independent of
// protocolDebugConfig's verbose per-packet dump - for capturing motion
// data to diagnose tracking-profile tuning issues. Runtime-only, not
// persisted. See the position-handling block in main.cpp's loop().
// Surfaced live in the browser via the Debug tab (built 2026-09-08,
// GET /compact-log) - no longer a removal candidate, it's the tab's
// primary content now.
extern bool compactLogEnabled;
// Retrieval for the buffer above (2026-09-06) - mirrors ddp_handler.h's
// getDdpRxLog()/clearDdpRxLog() exactly, and for the same reason: reading
// it over HTTP instead of Serial means a real xLights/DDPDebugger-driven
// session (already talking to the device over the network) can be
// observed live without opening a competing serial connection, which
// would reset the ESP32 via DTR/RTS and kill whatever show is running.
// See html_handler.cpp's GET /compact-log.
String getCompactLog();
void clearCompactLog();

// Blank time settings (used by DDP)
extern int ledBlankTimeConfig;      // Seconds before LEDs turn off (0 = disabled)
extern int stepperBlankTimeConfig;  // Seconds before stepper goes to zero (0 = disabled)
extern unsigned long lastProtocolUpdateTime;  // Timestamp of last protocol update

// Shared position request (written by active protocol handler)
extern uint16_t positionRequest;

#endif