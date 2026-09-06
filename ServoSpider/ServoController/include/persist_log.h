#ifndef PERSIST_LOG_H
#define PERSIST_LOG_H

#include <Arduino.h>

// A small diagnostic log that survives a reboot - added 2026-09-06 after a
// real bench session where a hung/stuck condition (StallGuard disabled to
// get clean tuning data, homing apparently getting stuck against real
// mechanical resistance with nothing left to catch it) left no way to tell,
// after the fact, whether the device had genuinely crashed, hung in a
// retry loop, or was reset by something as mundane as a new serial
// connection opening (which itself reboots this board via DTR/RTS - see
// tools/tuning_harness.py's DeviceLink comment) - every one of those looks
// identical from a fresh boot alone. Backed by SPIFFS (the existing
// "spiffs" data partition - see partition_utils.cpp's table, already
// present, no partition table change needed), so entries genuinely survive
// a reset, unlike anything kept in RAM.
//
// Deliberately NOT a high-frequency data logger - each call is a real
// flash write with real latency (open/append/close), so this is for
// occasional breadcrumbs (a boot happened, a homing search is still going
// after N seconds, a stall was detected), not per-loop-iteration tracing.
// The Compact Motion Log (main.cpp's logCompactMotion(), read live over
// serial) is still the right tool for high-rate position/speed/encoder
// data - this is for the things that only matter in hindsight, after
// something already went wrong and the live serial session is long gone.
//
// Retrieval: GET /persist-log (html_handler.cpp) is the primary way to
// pull this - no serial port contention, no reboot-on-connect, works over
// WiFi same as everything else. The 'l' serial command is a fallback for
// when WiFi is down.
void initPersistLog();  // Call early in setup(), before anything else might want to log - mounts SPIFFS and logs the boot/reset-reason line automatically

// printf-style; each entry is automatically prefixed with a boot-relative
// timestamp and this boot's sequence number (so entries from different
// power cycles are distinguishable even though millis() resets every boot).
// RAM-only - does NOT touch flash (see persist_log.cpp's top comment for
// why: a real flash write briefly disables the flash cache, and calling
// this while the stepper is actively stepping risks crashing if any
// interrupt that isn't fully IRAM-resident fires during that window -
// confirmed on the bench 2026-09-06). Call flushPersistLogNow() to
// actually commit pending entries to flash, only when nothing is stepping.
void persistLog(const char* fmt, ...);

bool hasPendingPersistLog();  // True if there are entries buffered in RAM not yet written to flash
// Writes any RAM-buffered entries to flash. Callers MUST only call this
// when the stepper is confirmed idle (main.cpp's loop() gates this on
// stepper->isRunning() == false) - see persist_log.cpp's top comment.
void flushPersistLogNow();

String readPersistLog();  // Full current log content, flash + any not-yet-flushed RAM entries (bounded - see PERSIST_LOG_MAX_BYTES in persist_log.cpp)
void clearPersistLog();   // Wipes it (flash and any pending RAM entries) - for starting a fresh diagnostic session deliberately

#endif
