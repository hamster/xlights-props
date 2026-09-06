#include "persist_log.h"
#include <SPIFFS.h>
#include <Preferences.h>
#include <esp_system.h>
#include <stdarg.h>

static const char* LOG_PATH = "/persist.log";

// Compacted (not wiped) once the file passes this size - see flushPersistLogNow().
static const size_t PERSIST_LOG_MAX_BYTES = 32768;
static const size_t PERSIST_LOG_COMPACT_KEEP_BYTES = 16384;  // keep this many trailing bytes when compacting

// Real flash writes (SPIFFS.open(..., FILE_APPEND) etc.) briefly disable
// the flash cache, exactly like the Preferences.putX() writes CLAUDE.md
// already documents as a hazard here - if any interrupt that isn't fully
// IRAM-resident fires during that window, it crashes (confirmed on the
// bench 2026-09-06: a real Guru Meditation Error/PANIC reset landed right
// after a homing-search breadcrumb, i.e. while the stepper was actively
// stepping - FastAccelStepper's own step-generation ISR is the leading
// suspect, not this project's own homing-switch ISR, which is already
// IRAM-safe). So persistLog() itself never touches flash - it only
// appends to this in-RAM buffer, which is always safe regardless of what
// else is running. Actually writing to SPIFFS only happens in
// flushPersistLogNow(), which callers must only invoke when nothing is
// stepping (main.cpp's loop() does this, gated on stepper->isRunning()).
static const size_t PENDING_BUFFER_MAX_BYTES = 4096;
static String pendingBuffer;

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
// (also always safe - nothing has moved yet at boot).
static void appendToFile(const String& text) {
  if (!spiffsReady) return;

  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  size_t currentSize = f ? f.size() : 0;
  if (f) f.close();

  if (currentSize > PERSIST_LOG_MAX_BYTES) {
    // Keep only the trailing PERSIST_LOG_COMPACT_KEEP_BYTES - read that
    // tail, then rewrite the file with just that content before appending
    // the new line. A full rewrite, not a true ring buffer - simple and
    // correct, and only actually runs once every ~16KB of accumulated log
    // (rare, given this is meant for occasional breadcrumbs).
    File rf = SPIFFS.open(LOG_PATH, FILE_READ);
    String tail;
    if (rf) {
      if (currentSize > PERSIST_LOG_COMPACT_KEEP_BYTES) {
        rf.seek(currentSize - PERSIST_LOG_COMPACT_KEEP_BYTES);
        // Skip a possibly-partial first line so the kept tail starts cleanly.
        rf.readStringUntil('\n');
      }
      tail = rf.readString();
      rf.close();
    }
    File wf = SPIFFS.open(LOG_PATH, FILE_WRITE);  // truncates
    if (wf) {
      wf.print("[log compacted - earlier entries discarded]\n");
      wf.print(tail);
      wf.close();
    }
  }

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
  return pendingBuffer.length() > 0;
}

void flushPersistLogNow() {
  if (pendingBuffer.length() == 0) return;
  appendToFile(pendingBuffer);
  pendingBuffer = "";
}

String readPersistLog() {
  if (!spiffsReady) return "(SPIFFS not mounted - persistent logging unavailable)";
  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  String content = f ? f.readString() : String();
  if (f) f.close();
  // Include whatever hasn't been flushed to flash yet, so a live read (e.g.
  // GET /persist-log while a homing search is still in progress) sees the
  // most current data instead of waiting for the next safe flush.
  content += pendingBuffer;
  return content.length() ? content : "(no log yet)";
}

void clearPersistLog() {
  pendingBuffer = "";
  if (!spiffsReady) return;
  SPIFFS.remove(LOG_PATH);
}
