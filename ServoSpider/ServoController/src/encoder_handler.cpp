#include "encoder_handler.h"
#include "stepper_handler.h"
#include <driver/rmt.h>
#include <freertos/ringbuf.h>

// See encoder_handler.h's file comment for the full history/reasoning.
// This implementation: RMT RX on channel A only, hardware glitch-filtered,
// continuously captured into a ring buffer that updateEncoder() drains
// periodically; direction for each drained batch comes from the stepper's
// own currently commanded direction rather than reading channel B.

// Picked from the high end of RMT_CHANNEL_0..7 to minimize collision risk
// with FastLED's or Arduino's own dynamic RMT channel allocation, which
// both tend to start from channel 0 upward - only relevant if LEDs are
// ever re-enabled while this diagnostic code is still in place.
static const rmt_channel_t ENCODER_RMT_CHANNEL = RMT_CHANNEL_7;

// clk_div=80 against an 80MHz APB clock gives a 1MHz tick (1 tick = 1us) -
// convenient for reasoning about the thresholds below in microseconds.
static const uint8_t RMT_CLK_DIV = 80;

// Hardware glitch filter: any pulse shorter than this many ticks/us is
// discarded by the RMT peripheral before it's ever recorded - real bounce/
// noise rejection in hardware, not a software guess. 255 is this field's
// max (8 bits); real quadrature edges here are milliseconds apart even at
// max stepper speed, so the generous end of the range costs nothing.
static const uint8_t RMT_FILTER_TICKS_THRESH = 255;

// How long (ticks/us) with no edge before RMT considers the current "run"
// complete and flushes it to the ring buffer. 20ms is generous relative to
// real motion (edges every few ms at most) but still short enough that a
// run flushes promptly once real motion actually stops.
static const uint16_t RMT_IDLE_THRESHOLD_TICKS = 20000;

// Software ring buffer size (bytes) - generous headroom against the actual
// edge rate here (well under 200Hz total, so under 100Hz on channel A
// alone). The real protection against overflow is updateEncoder() draining
// everything pending on every call (see its own comment) rather than this
// size alone - a fixed buffer, however large, still fills eventually if
// nothing is ever taking items back out of it.
static const size_t RMT_RX_BUF_SIZE = 4096;

// See getMissedTransitionCount()'s declaration comment.
static const size_t ENCODER_BACKLOG_THRESHOLD = 64;

static RingbufHandle_t rmtRingBuf = NULL;
static volatile int32_t cumulativeCount = 0;
static volatile uint32_t missedTransitionCount = 0;
static bool encoderInitialized = false;
// Falls back to whatever direction was last actually seen if the stepper
// reads exactly 0 speed at the instant a batch is drained (e.g. right at a
// full stop) - better than arbitrarily defaulting to +1 every time.
static int lastKnownDirection = 1;

// updateEncoder() is now genuinely called from two different cores - the
// Core 0 task's periodic drain, and a direct, synchronous call from
// encoder_diag.cpp right when a leg completes, before the next (possibly
// opposite-direction) move is issued (see that call site's comment for why
// this is necessary, found on the bench 2026-09-06: entire legs' worth of
// data were getting attributed to the wrong direction when real captured
// data was still sitting in the ring buffer at the moment a reversal
// happened, because nothing forced a drain before the direction changed).
// This spinlock protects the shared counters against that genuine
// cross-core concurrency, which didn't exist back when this function only
// ever ran from one task.
static portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;

void initEncoder() {
  pinMode(encoderPinB, INPUT);  // still wired, just unused by this implementation - see file comment

  rmt_config_t config = {};
  config.rmt_mode = RMT_MODE_RX;
  config.channel = ENCODER_RMT_CHANNEL;
  config.gpio_num = (gpio_num_t)encoderPinA;
  config.clk_div = RMT_CLK_DIV;
  config.mem_block_num = 1;
  config.flags = 0;
  config.rx_config.idle_threshold = RMT_IDLE_THRESHOLD_TICKS;
  config.rx_config.filter_ticks_thresh = RMT_FILTER_TICKS_THRESH;
  config.rx_config.filter_en = true;

  esp_err_t err = rmt_config(&config);
  if (err == ESP_OK) err = rmt_driver_install(ENCODER_RMT_CHANNEL, RMT_RX_BUF_SIZE, 0);
  if (err == ESP_OK) err = rmt_get_ringbuf_handle(ENCODER_RMT_CHANNEL, &rmtRingBuf);
  if (err == ESP_OK) err = rmt_rx_start(ENCODER_RMT_CHANNEL, true);

  if (err != ESP_OK) {
    Serial.print("Encoder: RMT init FAILED (err=");
    Serial.print((int)err);
    Serial.println(") - encoder disabled");
    encoderInitialized = false;
    return;
  }

  cumulativeCount = 0;
  encoderInitialized = true;
  Serial.print("Encoder: RMT RX quadrature (channel A only, X2) initialized on A=D0(GPIO");
  Serial.print(encoderPinA);
  Serial.println(")");
}

// Drains whatever RMT has captured since the last call and folds it into
// cumulativeCount - see encoder_handler.h's declaration comment for why
// this needs to be called periodically (unlike the earlier GPIO-ISR
// version). Non-blocking (0 tick wait) - a normal task-context call, not an
// ISR, so xRingbufferReceive()/vRingbufferReturnItem() are safe to use
// directly here.
void updateEncoder() {
  if (!encoderInitialized || rmtRingBuf == NULL) {
    return;
  }

  // Drains *everything* currently pending, not just one chunk - a real bug
  // in the first version of this function only pulled a single chunk per
  // call, so any stretch where the driver pushed chunks even slightly
  // faster than this was being polled let a backlog build up monotonically
  // until the ring buffer filled completely and the driver itself started
  // discarding real captured data ("RX buffer too small"/"RMT RX BUFFER
  // FULL" - confirmed on the bench, 2026-09-06: clean for the first ~20s of
  // continuous motion, then errors for the rest of the run once the buffer
  // filled, with encoderCount never advancing again). Looping until
  // xRingbufferReceive() returns NULL means each call fully catches up
  // regardless of how long it's been since the last one.
  size_t rxSize = 0;
  rmt_item32_t* items;
  while ((items = (rmt_item32_t*)xRingbufferReceive(rmtRingBuf, &rxSize, 0)) != NULL) {
    size_t itemCount = rxSize / sizeof(rmt_item32_t);

    portENTER_CRITICAL(&encoderMux);
    if (itemCount > ENCODER_BACKLOG_THRESHOLD) {
      // See getMissedTransitionCount()'s declaration comment - not a lost
      // edge itself, just evidence this drain call fell behind schedule.
      missedTransitionCount++;
    }

    // Direction for this whole drained batch comes from the stepper's own
    // currently commanded direction - see encoder_handler.h's file comment
    // for why this is a deliberate scope reduction vs. independently
    // reading channel B.
    if (stepper != NULL) {
      int32_t speedMilliHz = stepper->getCurrentSpeedInMilliHz();
      if (speedMilliHz > 0) {
        lastKnownDirection = 1;
      } else if (speedMilliHz < 0) {
        lastKnownDirection = -1;
      }
      // else: stopped (or between samples) - keep whatever direction was last known
    }

    int32_t delta = 0;
    for (size_t i = 0; i < itemCount; i++) {
      if (items[i].duration0 > 0) delta += lastKnownDirection;
      if (items[i].duration1 > 0) delta += lastKnownDirection;
    }
    cumulativeCount += delta;
    portEXIT_CRITICAL(&encoderMux);

    vRingbufferReturnItem(rmtRingBuf, (void*)items);
  }
}

int32_t getEncoderCount() {
  portENTER_CRITICAL(&encoderMux);
  int32_t value = cumulativeCount;
  portEXIT_CRITICAL(&encoderMux);
  return value;
}

uint32_t getMissedTransitionCount() {
  portENTER_CRITICAL(&encoderMux);
  uint32_t value = missedTransitionCount;
  portEXIT_CRITICAL(&encoderMux);
  return value;
}

void resetEncoderCount() {
  portENTER_CRITICAL(&encoderMux);
  cumulativeCount = 0;
  portEXIT_CRITICAL(&encoderMux);
}

bool isEncoderInitialized() {
  return encoderInitialized;
}
