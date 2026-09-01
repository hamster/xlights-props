#include "encoder_handler.h"

// Deliberately plain digitalRead() polling, no ESP32 PCNT peripheral at all
// (2026-09-02) - two separate PCNT-based attempts (first interrupt-driven
// with pcnt_isr_service_install() + watch points, then a rewrite using pure
// polling for the 16-bit overflow but still configuring the PCNT
// peripheral on these two pins) both produced the same severe regression on
// the bench: TMC2209 stall detection firing continuously, and the motor
// unstoppable, repeatedly restarting and driving into the homing stop even
// with stall detection disabled. Root cause never conclusively pinned down,
// but a real, board-level fact surfaced while investigating: D9 (GPIO8) is
// this board's default SPI MISO pin (see XIAO_ESP32S3's pins_arduino.h),
// and D0 (GPIO1) doubles as ADC channel A0 - these pins may not be as free
// of alternate-peripheral entanglement as originally assumed. Given two
// PCNT variants both failed the same way, not worth trying a third -
// digitalRead() is the exact same underlying mechanism this firmware
// already uses reliably for the homing switch, with zero new peripheral or
// interrupt footprint.
//
// Software quadrature decode via a standard 2-bit-state transition lookup
// table: index = (previous state << 2) | current state, where each state
// is (A<<1 | B). A legitimate quadrature signal only ever changes one bit
// at a time; the four transitions belonging to each direction of travel
// map to +1 or -1, and everything else (no change, or an illegal two-bit
// jump - almost certainly a missed edge, i.e. the poll rate wasn't fast
// enough to catch an intermediate state) maps to 0 and is conservatively
// ignored rather than guessed at.
//
// Signed so that increasing count matches stepper_handler's convention
// (increasing position = moving away from the switch) - confirmed on the
// bench (2026-09-02) that the raw wiring produced the opposite sign, so
// this table is the physical wiring's table negated, not the "natural"
// one - see resetEncoderCount()'s calibration note if the physical
// mounting ever changes and this needs re-checking.
static const int8_t QUAD_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

static uint8_t lastQuadState = 0;
static int32_t cumulativeCount = 0;
static bool encoderInitialized = false;

void initEncoder() {
  // External 10k pull-ups to 3.3V are already present on both channels
  // (open-collector-style encoder output, common wired to ground) - plain
  // INPUT here, no need for the internal pull-up too.
  pinMode(encoderPinA, INPUT);
  pinMode(encoderPinB, INPUT);

  lastQuadState = (digitalRead(encoderPinA) << 1) | digitalRead(encoderPinB);
  cumulativeCount = 0;
  encoderInitialized = true;
  Serial.print("Encoder: digitalRead() quadrature polling initialized on A=D0(GPIO");
  Serial.print(encoderPinA);
  Serial.print(") B=D9(GPIO");
  Serial.print(encoderPinB);
  Serial.println(")");
}

// Call every loop() iteration - must run often enough that the pulley can't
// advance more than one quadrature step between polls, or an intermediate
// state gets missed (silently ignored per the table above, not
// miscounted - but still lost). digitalRead() is cheap; no throttling here.
void updateEncoder() {
  if (!encoderInitialized) return;
  uint8_t newState = (digitalRead(encoderPinA) << 1) | digitalRead(encoderPinB);
  if (newState != lastQuadState) {
    uint8_t index = (lastQuadState << 2) | newState;
    cumulativeCount += QUAD_TABLE[index];
    lastQuadState = newState;
  }
}

int32_t getEncoderCount() {
  return cumulativeCount;
}

void resetEncoderCount() {
  cumulativeCount = 0;
}

bool isEncoderInitialized() {
  return encoderInitialized;
}
