#include "persist_log.h"
#include <SPIFFS.h>
#include <Preferences.h>
#include <esp_system.h>
#include <stdarg.h>

static const char* LOG_PATH = "/persist.log";

// Compacted (not wiped) once the file passes this size - see flushPersistLogNow().
static const size_t PERSIST_LOG_MAX_BYTES = 32768;
static const size_t PERSIST_LOG_COMPACT_KEEP_BYTES = 16384;  // keep this many trailing bytes when compacting

// Real flash access (SPIFFS.open(..., FILE_APPEND) etc.) briefly disables
// the flash cache, exactly like the Preferences.putX() writes CLAUDE.md
// already documents as a hazard here - if any interrupt that isn't fully
// IRAM-resident fires during that window, it crashes (confirmed on the
// bench 2026-09-06: a real Guru Meditation Error/PANIC reset landed right
// after a homing-search breadcrumb, i.e. while the stepper was actively
// stepping - FastAccelStepper's own step-generation ISR is the leading
// suspect, not this project's own homing-switch ISR, which is already
// IRAM-safe). This applies equally to a flash *read*, not just a write -
// confirmed 2026-09-08 the hard way: adding the Debug tab's live
// GET /persist-log viewer (which previously read straight from SPIFFS on
// every request) let a page left open with auto-refresh crash the board
// mid-homing, the same failure mode as the write-side hazard below, just
// never hit before because nothing polled this endpoint automatically
// pre-Debug-tab. So persistLog() itself never touches flash - it only
// appends to this in-RAM buffer, which is always safe regardless of what
// else is running. Actually writing to SPIFFS only happens in
// flushPersistLogNow(), which callers must only invoke when nothing is
// stepping (main.cpp's loop() does this, gated on stepper->isRunning()).
// readPersistLog() below never touches SPIFFS live either, for the same
// reason - see flushedMirror.
static const size_t PENDING_BUFFER_MAX_BYTES = 4096;
static String pendingBuffer;

// RAM mirror of what's actually been written to the flash file, kept in
// sync only from safe contexts (initPersistLog() at boot, appendToFile()
// which only ever runs from flushPersistLogNow()). readPersistLog() reads
// this instead of SPIFFS directly, so a live GET /persist-log request can
// never touch flash, no matter when it lands.
static String flushedMirror;
// clearPersistLog() can be called from an HTTP handler at any time,
// including mid-homing - it clears the RAM state immediately (safe) and
// defers the actual SPIFFS.remove() to the next flushPersistLogNow() call,
// which callers already only invoke when the stepper is confirmed idle.
static bool pendingClear = false;

static bool spiffsReady = false;
static uint32_t bootCount = 0;
static unsigned long bootStartMillis = 0;  // millis() is already boot-relative, but named here for clarity at call sites

static const char* resetReasonString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "POWERON (normal power-on)";
    case ESP_RST_EXT:       return "EXT (external pin reset)";
    case ESP_RST_SW:        return "SW (software reset - esp_restart(), or a new serial connection's DTR/RTS toggle)";
    case ESP_RST_PANIC:     return "PANIC (crash - Guru Meditation Error)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog timeout - an ISR or interrupts-disabled section ran too long)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog timeout - a task didn't yield/reset in time)";
    case ESP_RST_WDT:       return "WDT (other watchdog timeout)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (woke from deep sleep - not used by this firmware)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage sagged below the safe threshold - a real electrical event, e.g. a hard mechanical stop spiking motor current)";
    case ESP_RST_SDIO:      return "SDIO";
    // Note: newer IDF versions add ESP_RST_USB/JTAG/EFUSE/PWR_GLITCH/
    // CPU_LOCKUP - not present in this project's toolchain version, so
    // those fall through to UNKNOWN rather than failing to compile. A
    // serial-connection-triggered reset on this board reports as SW, not
    // a distinct USB reason, in this IDF version.
    default:                return "UNKNOWN";
  }
}

// Actually writes `text` to the log file, compacting first if it's already
// grown past PERSIST_LOG_MAX_BYTES. Only ever called from
// flushPersistLogNow() (safe context) or initPersistLog()'s own boot line
// (also always safe - nothing has moved yet at boot). Works off
// flushedMirror rather than re-reading the file, so this never needs a
// live SPIFFS read - only a write, and only from those same safe contexts.
static void appendToFile(const String& text) {
  if (!spiffsReady) return;

  if (pendingClear) {
    SPIFFS.remove(LOG_PATH);
    flushedMirror = "";
    pendingClear = false;
  }

  flushedMirror += text;

  if (flushedMirror.length() > PERSIST_LOG_MAX_BYTES) {
    // Keep only the trailing PERSIST_LOG_COMPACT_KEEP_BYTES of the mirror,
    // then rewrite the file with just that content. A full rewrite, not a
    // true ring buffer - simple and correct, and only actually runs once
    // every ~16KB of accumulated log (rare, given this is meant for
    // occasional breadcrumbs).
    size_t dropLen = flushedMirror.length() - PERSIST_LOG_COMPACT_KEEP_BYTES;
    int cut = flushedMirror.indexOf('\n', dropLen);
    String tail = (cut >= 0) ? flushedMirror.substring(cut + 1) : flushedMirror.substring(dropLen);
    flushedMirror = "[log compacted - earlier entries discarded]\n" + tail;

    File wf = SPIFFS.open(LOG_PATH, FILE_WRITE);  // truncates
    if (wf) {
      wf.print(flushedMirror);
      wf.close();
    }
    return;
  }

  if (text.length() == 0) return;  // pendingClear-only flush, nothing new to append
  File af = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (af) {
    af.print(text);
    af.close();
  }
}

void initPersistLog() {
  bootStartMillis = millis();
  spiffsReady = SPIFFS.begin(true);  // true = format on mount failure (first-ever boot, or corruption)
  if (!spiffsReady) {
    Serial.println("PersistLog: SPIFFS mount FAILED - persistent logging disabled");
    return;
  }

  // Load whatever's already on flash into the RAM mirror once, here at
  // boot - the only place besides appendToFile() (also always a safe,
  // stepper-idle context) this file ever touches SPIFFS for reading.
  // readPersistLog() reads flushedMirror from here on, never SPIFFS itself.
  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  if (f) {
    flushedMirror = f.readString();
    f.close();
  }

  Preferences bootPrefs;
  bootPrefs.begin("diaglog", false);
  bootCount = bootPrefs.getUInt("bootCount", 0) + 1;
  bootPrefs.putUInt("bootCount", bootCount);
  bootPrefs.end();

  esp_reset_reason_t reason = esp_reset_reason();
  persistLog("=== Boot #%u - reset reason: %s ===", bootCount, resetReasonString(reason));
  // Flushed immediately, unlike every other entry - safe here specifically
  // because nothing has started moving yet this boot, and this is the
  // single most valuable line in the whole log (see this file's header
  // comment), worth guaranteeing it survives even a near-instant repeat
  // crash rather than waiting on loop()'s normal idle-gated flush.
  flushPersistLogNow();
}

void persistLog(const char* fmt, ...) {
  if (!spiffsReady) return;

  char msg[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  char line[224];
  snprintf(line, sizeof(line), "[boot %u +%lums] %s\n", bootCount, millis() - bootStartMillis, msg);

  // RAM-only append - see this file's top comment for why this never
  // touches flash directly. Bounded: if a burst of entries piles up faster
  // than flushPersistLogNow() gets a safe (stepper-idle) chance to run,
  // drop the oldest rather than growing without limit.
  pendingBuffer += line;
  if (pendingBuffer.length() > PENDING_BUFFER_MAX_BYTES) {
    size_t excess = pendingBuffer.length() - PENDING_BUFFER_MAX_BYTES;
    int cut = pendingBuffer.indexOf('\n', excess);
    pendingBuffer = (cut >= 0) ? pendingBuffer.substring(cut + 1) : "";
  }
}

bool hasPendingPersistLog() {
  // pendingClear also needs a safe-context flush to actually take effect
  // (see appendToFile()'s SPIFFS.remove()), even if no new entries have
  // been logged since - otherwise a clear requested mid-homing would sit
  // deferred forever if nothing logs again afterward.
  return pendingBuffer.length() > 0 || pendingClear;
}

void flushPersistLogNow() {
  if (pendingBuffer.length() == 0 && !pendingClear) return;
  appendToFile(pendingBuffer);
  pendingBuffer = "";
}

String readPersistLog() {
  if (!spiffsReady) return "(SPIFFS not mounted - persistent logging unavailable)";
  // Never touches SPIFFS live - see flushedMirror's declaration comment for
  // why (a live flash read here crashed the board mid-homing once the
  // Debug tab started polling this automatically). Includes whatever
  // hasn't been flushed to flash yet, so a live read (e.g. GET
  // /persist-log while a homing search is still in progress) sees the
  // most current data instead of waiting for the next safe flush.
  String content = flushedMirror + pendingBuffer;
  return content.length() ? content : "(no log yet)";
}

void clearPersistLog() {
  pendingBuffer = "";
  flushedMirror = "";
  if (!spiffsReady) return;
  // Actual SPIFFS.remove() deferred to the next flushPersistLogNow() call
  // (safe, stepper-idle-gated) - see this flag's declaration comment.
  pendingClear = true;
}
