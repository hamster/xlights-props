#include "encoder_handler.h"
#include "driver/gpio.h"

// See encoder_handler.h's file comment for the full history/reasoning.
// This implementation: a hardware timer ISR polls both quadrature channels
// at ENCODER_POLL_HZ and decodes X4 in software via QUAD_TABLE - no RMT, no
// ring buffer, no dependency on FreeRTOS task scheduling.

// ~20x oversampling over the real edge rate this project's own bench data
// implies (well under 200Hz even at the fastest tested stepper speed) - see
// encoder_handler.h's file comment for the full reasoning. 4kHz keeps the
// ISR itself cheap (two gpio_get_level() calls and a table lookup) while
// leaving enormous margin before a missed transition becomes possible.
static const uint32_t ENCODER_POLL_HZ = 4000;

// 80MHz APB / 80 = 1MHz tick (1us) - same reasoning as every other
// peripheral in this codebase that divides down APB for a convenient tick.
static const uint16_t TIMER_CLK_DIV = 80;

// Standard X4 quadrature decode table, indexed by (oldState<<2)|newState
// where each 2-bit state is packed (A<<1)|B. The four indices where
// oldState==newState (0,5,10,15) are "no motion" - expected almost every
// poll. The other two zero entries (3 and 12) are old/new pairs where
// *both* bits differ at once - impossible for a real single quadrature
// step, so a poll landing there means a real transition was skipped
// between samples (see getMissedTransitionCount()). Every other entry is
// a real, unambiguous single step, +1 or -1.
static const int8_t QUAD_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0,
};

static hw_timer_t* encoderTimer = NULL;
static volatile int32_t cumulativeCount = 0;
static volatile uint32_t missedTransitionCount = 0;
static volatile uint8_t lastQuadState = 0;
static bool encoderInitialized = false;

// Guards the shared counters against the timer ISR (which can fire on
// either core, though initEncoder() being called from core0Task() keeps it
// Core-0-affine in practice - see core0_task.cpp) racing a task-context
// reader/writer (getEncoderCount()/resetEncoderCount(), both callable from
// Core 1). portENTER_CRITICAL_ISR/portEXIT_CRITICAL_ISR are the ISR-context
// counterparts used inside onEncoderTimer() itself.
static portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;

// Runs at ENCODER_POLL_HZ, IRAM-resident since it's a real timer ISR that
// must stay valid even if flash cache is briefly disabled elsewhere (e.g. a
// Preferences.putX() write on the other core) - same reasoning CLAUDE.md
// documents for the homing-switch ISR. gpio_get_level() (not digitalRead())
// is the IRAM-safe way to read a pin from ISR context.
void IRAM_ATTR onEncoderTimer() {
  uint8_t a = gpio_get_level((gpio_num_t)encoderPinA);
  uint8_t b = gpio_get_level((gpio_num_t)encoderPinB);
  uint8_t newState = (a << 1) | b;
  if (newState == lastQuadState) {
    return;  // No change - the overwhelmingly common case at this poll rate
  }
  uint8_t index = (lastQuadState << 2) | newState;
  int8_t delta = QUAD_TABLE[index];
  portENTER_CRITICAL_ISR(&encoderMux);
  if (delta != 0) {
    cumulativeCount += delta;
  } else {
    missedTransitionCount++;
  }
  portEXIT_CRITICAL_ISR(&encoderMux);
  lastQuadState = newState;
}

void initEncoder() {
  pinMode(encoderPinA, INPUT);
  pinMode(encoderPinB, INPUT);

  uint8_t a = digitalRead(encoderPinA);
  uint8_t b = digitalRead(encoderPinB);
  lastQuadState = (a << 1) | b;

  encoderTimer = timerBegin(0, TIMER_CLK_DIV, true);
  if (encoderTimer == NULL) {
    Serial.println("Encoder: hardware timer init FAILED - encoder disabled");
    encoderInitialized = false;
    return;
  }
  timerAttachInterrupt(encoderTimer, &onEncoderTimer, true);
  uint64_t alarmTicks = 1000000UL / ENCODER_POLL_HZ;  // 250 @ 1MHz tick for 4kHz
  timerAlarmWrite(encoderTimer, alarmTicks, true);
  timerAlarmEnable(encoderTimer);

  cumulativeCount = 0;
  missedTransitionCount = 0;
  encoderInitialized = true;
  Serial.print("Encoder: hardware-timer quadrature poll (both channels, X4) initialized at ");
  Serial.print(ENCODER_POLL_HZ);
  Serial.println("Hz");
}

void updateEncoder() {
  // Intentionally empty - see encoder_handler.h's declaration comment.
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
