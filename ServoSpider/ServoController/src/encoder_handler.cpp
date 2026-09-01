#include "encoder_handler.h"
#include "driver/pcnt.h"

// PCNT unit 0 - not used elsewhere in this firmware.
static const pcnt_unit_t ENCODER_PCNT_UNIT = PCNT_UNIT_0;

// No interrupt/watch-point handling here on purpose (2026-09-02) - an
// earlier version used pcnt_isr_service_install() + H_LIM/L_LIM watch
// points to catch the 16-bit hardware counter's overflow. That was the one
// genuinely new interrupt source added to this firmware's interrupt
// landscape, and it coincided with a real, severe regression on the bench
// (stepper stall detection firing continuously, the motor unstoppable and
// driving repeatedly into the homing stop even with stall detection
// disabled) - never conclusively root-caused, but not worth risking again
// when a simpler, interrupt-free technique does the same job just as well:
// read the raw counter periodically and take the delta as 16-bit signed
// subtraction. Two's-complement arithmetic handles the wraparound
// correctly on its own (as long as the true delta between two consecutive
// reads never exceeds +/-32767, which any plausible pulley RPM guarantees
// at a reasonable poll rate) - no watch points, no ISR, no new interrupt
// source at all.
static int16_t lastRawCount = 0;
static int32_t cumulativeCount = 0;
static bool encoderInitialized = false;

void initEncoder() {
  // Counter limits set to the full 16-bit signed range - not used as watch
  // points (no events are ever enabled below), just satisfying the
  // pcnt_config_t struct's required fields.
  const int16_t PCNT_FULL_RANGE_H_LIM = 32767;
  const int16_t PCNT_FULL_RANGE_L_LIM = -32768;

  // Standard two-channel X4 quadrature setup: each channel counts its own
  // pulse pin's edges, with the *other* channel's pin as its control input
  // deciding count direction (that's what makes this X4 - both edges of
  // both signals are counted, each direction-qualified by the other
  // signal's current level). Same configuration pattern as Espressif's own
  // PCNT rotary-encoder example.
  pcnt_config_t cfgA = {};
  cfgA.pulse_gpio_num = encoderPinA;
  cfgA.ctrl_gpio_num = encoderPinB;
  cfgA.channel = PCNT_CHANNEL_0;
  cfgA.unit = ENCODER_PCNT_UNIT;
  cfgA.pos_mode = PCNT_COUNT_DEC;
  cfgA.neg_mode = PCNT_COUNT_INC;
  cfgA.lctrl_mode = PCNT_MODE_REVERSE;
  cfgA.hctrl_mode = PCNT_MODE_KEEP;
  cfgA.counter_h_lim = PCNT_FULL_RANGE_H_LIM;
  cfgA.counter_l_lim = PCNT_FULL_RANGE_L_LIM;

  if (pcnt_unit_config(&cfgA) != ESP_OK) {
    Serial.println("Encoder: pcnt_unit_config (channel A) failed - not initialized");
    return;
  }

  pcnt_config_t cfgB = cfgA;
  cfgB.pulse_gpio_num = encoderPinB;
  cfgB.ctrl_gpio_num = encoderPinA;
  cfgB.channel = PCNT_CHANNEL_1;
  cfgB.pos_mode = PCNT_COUNT_INC;
  cfgB.neg_mode = PCNT_COUNT_DEC;

  if (pcnt_unit_config(&cfgB) != ESP_OK) {
    Serial.println("Encoder: pcnt_unit_config (channel B) failed - not initialized");
    return;
  }

  // Glitch filter - ignores pulses shorter than ~12.5us (1000 APB clock
  // cycles @ 80MHz), the value used in Espressif's own rotary-encoder
  // example. Real encoder edges at any plausible pulley RPM are far
  // longer than this; it's here purely to reject electrical noise.
  pcnt_set_filter_value(ENCODER_PCNT_UNIT, 1000);
  pcnt_filter_enable(ENCODER_PCNT_UNIT);

  // Deliberately no pcnt_event_enable() / pcnt_isr_service_install() /
  // pcnt_isr_handler_add() here - see the comment above lastRawCount.

  pcnt_counter_pause(ENCODER_PCNT_UNIT);
  pcnt_counter_clear(ENCODER_PCNT_UNIT);
  pcnt_counter_resume(ENCODER_PCNT_UNIT);

  lastRawCount = 0;
  cumulativeCount = 0;
  encoderInitialized = true;
  Serial.print("Encoder: PCNT quadrature decode initialized on A=D0(GPIO");
  Serial.print(encoderPinA);
  Serial.print(") B=D9(GPIO");
  Serial.print(encoderPinB);
  Serial.println("), polling mode (no dedicated interrupt)");
}

// Call every loop() iteration - pcnt_get_counter_value() is a cheap
// register read, no need to throttle it. Must be called often enough that
// the true delta between two consecutive calls never exceeds the 16-bit
// signed range (+/-32767) - trivially true for a mechanical pulley at any
// realistic RPM even at typical loop() rates.
void updateEncoder() {
  if (!encoderInitialized) return;
  int16_t raw = 0;
  pcnt_get_counter_value(ENCODER_PCNT_UNIT, &raw);
  int16_t delta = raw - lastRawCount;  // 16-bit signed subtraction - wraps correctly on its own
  cumulativeCount += delta;
  lastRawCount = raw;
}

int32_t getEncoderCount() {
  return cumulativeCount;
}

void resetEncoderCount() {
  if (!encoderInitialized) return;
  pcnt_counter_clear(ENCODER_PCNT_UNIT);
  lastRawCount = 0;
  cumulativeCount = 0;
}

bool isEncoderInitialized() {
  return encoderInitialized;
}
