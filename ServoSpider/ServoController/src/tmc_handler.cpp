#include "tmc_handler.h"
#include "stepper_handler.h"
#include "persist_log.h"
#include <Preferences.h>

extern Preferences preferences;

TMC2209Stepper *tmcDriver = nullptr;
bool tmcConnected = false;

bool tmcEnabledConfig = false;
float tmcRSenseConfig = 0.11f;
uint8_t tmcAddressConfig = 0;
uint16_t tmcRunCurrentConfig = 800;
uint8_t tmcHoldPercentConfig = 50;
bool tmcStealthChopConfig = true;
bool tmcStallEnabledConfig = false;
uint16_t tmcStallThresholdConfig = 50;
uint16_t tmcMicrostepsConfig = 16;
uint8_t tmcHstrtConfig = 0;
uint8_t tmcHendConfig = 0;
uint8_t tmcPwmRegConfig = 4;
uint8_t tmcPwmLimConfig = 12;
bool tmcPwmAutogradConfig = true;

TmcStatus tmcStatus;

// GSTAT bit positions (TMC2209 datasheet, register 0x01). Clear-on-write.
static const uint8_t GSTAT_RESET_BIT = 1 << 0;
static const uint8_t GSTAT_DRVERR_BIT = 1 << 1;
static const uint8_t GSTAT_UVCP_BIT = 1 << 2;
static const uint8_t GSTAT_ALL_BITS = GSTAT_RESET_BIT | GSTAT_DRVERR_BIT | GSTAT_UVCP_BIT;

// DRV_STATUS bit positions (TMC2209 datasheet, register 0x6F)
static const uint32_t DRVSTATUS_OTPW_BIT = 1UL << 0;
static const uint32_t DRVSTATUS_OT_BIT = 1UL << 1;
static const uint32_t DRVSTATUS_S2GA_BIT = 1UL << 2;
static const uint32_t DRVSTATUS_S2GB_BIT = 1UL << 3;
static const uint32_t DRVSTATUS_OLA_BIT = 1UL << 6;
static const uint32_t DRVSTATUS_OLB_BIT = 1UL << 7;

static const unsigned long DIAG_POLL_INTERVAL_MS = 2000;
static const unsigned long STALL_POLL_INTERVAL_MS = 100;
static const unsigned long STALL_RAMP_GRACE_MS = 300;  // let SG_RESULT settle after a move starts
static const uint8_t STALL_DEBOUNCE_COUNT = 3;  // consecutive low readings before we trust a stall

static unsigned long lastDiagPollMillis = 0;
static unsigned long lastStallPollMillis = 0;
static unsigned long runStartMillis = 0;
static bool wasRunning = false;
static uint8_t stallDebounceCount = 0;

void initTmc() {
  if (!tmcEnabledConfig) {
    Serial.println("TMC2209 UART control disabled (enable it in Settings to use it)");
    return;
  }

  Serial1.begin(tmcUartBaud, SERIAL_8N1, tmcUartRxPin, tmcUartTxPin);
  tmcDriver = new TMC2209Stepper(&Serial1, tmcRSenseConfig, tmcAddressConfig);
  tmcDriver->begin();

  Serial.print("TMC2209: opening UART on RX=D7(GPIO");
  Serial.print(tmcUartRxPin);
  Serial.print(") TX=D6(GPIO");
  Serial.print(tmcUartTxPin);
  Serial.print("), address ");
  Serial.print(tmcAddressConfig);
  Serial.print(", RSense ");
  Serial.println(tmcRSenseConfig, 3);

  uint8_t connResult = tmcDriver->test_connection();
  uint32_t rawDrvStatus = tmcDriver->DRV_STATUS();
  tmcConnected = (connResult == 0);
  tmcStatus.commOk = tmcConnected;

  if (tmcConnected) {
    Serial.println("TMC2209 UART link established");
    applyTmcSettings();
    // Clear the power-on-reset flag now that we've taken over configuration
    tmcDriver->GSTAT(GSTAT_ALL_BITS);
  } else {
    Serial.print("TMC2209 UART link FAILED - test_connection()=");
    Serial.print(connResult);
    Serial.print(" (1=no reply/read-all-1s, 2=read-all-0s), raw DRV_STATUS=0x");
    Serial.println(rawDrvStatus, HEX);
    Serial.println("Check: RX/TX both land on the driver's single PDN_UART pad (not two separate pins), driver is powered (VCC_IO/VM present), and MS1/MS2 give the expected address.");
  }
}

void applyTmcSettings() {
  if (!tmcConnected || tmcDriver == nullptr) return;

  // TMCStepper caches CHOPCONF in RAM and rewrites the whole register on any
  // write to it (e.g. from rms_current()'s vsense bit below) - any field we
  // never explicitly set defaults to 0 in that cache. TOFF=0 disables the
  // driver's output stage entirely, so it must be set explicitly every time
  // we touch CHOPCONF, or the motor silently stops driving. Values match
  // TMCStepper's own reference example (toff=4, blank_time=24).
  tmcDriver->toff(4);
  tmcDriver->blank_time(24);

  // TMC2209Stepper::begin() calls mstep_reg_select(true), which switches
  // microstep resolution from the MS1/MS2 pins over to this UART-writable
  // MRES field - which falls into the exact same "defaults to 0" trap as
  // TOFF above, and MRES=0 means 256 microsteps (the finest/slowest
  // setting). Re-home after changing this - bottomPosition is measured in
  // actual steps, so it self-corrects on the next homing run, but any
  // previously-tuned Stepper Speed (Hz) will now feel different since the
  // physical distance per step just changed.
  tmcDriver->microsteps(tmcMicrostepsConfig);

  // SpreadCycle hysteresis start/end - only meaningful when SpreadCycle is
  // active, but harmless to set unconditionally. Datasheet-recommended
  // constraint: hstrt + hend should stay <= 15, though it's not enforced
  // here (this is exploratory tuning territory, not a hard safety limit).
  tmcDriver->hstrt(tmcHstrtConfig);
  tmcDriver->hend(tmcHendConfig);

  float holdMultiplier = tmcHoldPercentConfig / 100.0f;
  tmcDriver->rms_current(tmcRunCurrentConfig, holdMultiplier);

  // PWMCONF fields never set elsewhere also default to 0 in the same way -
  // pwm_reg/pwm_lim bound StealthChop's autoscale step size/amplitude, so
  // leaving them at 0 would silently cripple StealthChop the moment it's
  // selected. Defaults match TMC's documented factory-default reset state.
  tmcDriver->pwm_autograd(tmcPwmAutogradConfig);
  tmcDriver->pwm_reg(tmcPwmRegConfig);
  tmcDriver->pwm_lim(tmcPwmLimConfig);

  if (tmcStealthChopConfig) {
    tmcDriver->en_spreadCycle(false);
    tmcDriver->pwm_autoscale(true);
  } else {
    tmcDriver->en_spreadCycle(true);
    tmcDriver->pwm_autoscale(false);
  }

  Serial.print("TMC2209 settings applied: ");
  Serial.print(tmcRunCurrentConfig);
  Serial.print("mA run / ");
  Serial.print((int)(holdMultiplier * tmcRunCurrentConfig));
  Serial.print("mA hold, ");
  Serial.println(tmcStealthChopConfig ? "StealthChop" : "SpreadCycle");
}

void clearTmcStall() {
  tmcStatus.stalled = false;
  stallDebounceCount = 0;
}

void tmcResetStallRampTimer() {
  runStartMillis = millis();
  stallDebounceCount = 0;
  // Also update wasRunning's baseline so updateTmc()'s own transition
  // detection doesn't immediately re-fire this same reset from a stale
  // false->true edge it hasn't processed yet.
  wasRunning = (stepper != NULL) && stepper->isRunning();
}

void updateTmc() {
  if (!tmcEnabledConfig || !tmcConnected || tmcDriver == nullptr) return;

  unsigned long now = millis();

  // Track fresh move starts so a just-begun acceleration ramp doesn't get
  // mistaken for a stall (SG_RESULT is unreliable at very low step rates).
  bool isRunning = stepper->isRunning();
  if (isRunning && !wasRunning) {
    runStartMillis = now;
  }
  wasRunning = isRunning;

  // Periodic diagnostics poll
  if (now - lastDiagPollMillis >= DIAG_POLL_INTERVAL_MS) {
    lastDiagPollMillis = now;

    uint8_t gstat = tmcDriver->GSTAT();
    tmcStatus.resetSinceLastCheck = gstat & GSTAT_RESET_BIT;
    tmcStatus.uartCrcError = tmcDriver->CRCerror;

    uint32_t drvStatus = tmcDriver->DRV_STATUS();
    tmcStatus.overTempWarning = drvStatus & DRVSTATUS_OTPW_BIT;
    tmcStatus.overTempShutdown = drvStatus & DRVSTATUS_OT_BIT;
    tmcStatus.shortToGroundA = drvStatus & DRVSTATUS_S2GA_BIT;
    tmcStatus.shortToGroundB = drvStatus & DRVSTATUS_S2GB_BIT;
    tmcStatus.openLoadA = drvStatus & DRVSTATUS_OLA_BIT;
    tmcStatus.openLoadB = drvStatus & DRVSTATUS_OLB_BIT;
    tmcStatus.commOk = true;
    tmcStatus.lastUpdateMillis = now;

    if (gstat & GSTAT_ALL_BITS) {
      tmcDriver->GSTAT(GSTAT_ALL_BITS);  // clear latched flags now that we've read them
    }
  }

  // Stall-detection safety cutoff: while actually moving, in either normal
  // operation or an active homing search. Originally excluded homing
  // entirely ("never fight the homing state machine's own switch-based
  // logic") - but a real incident (2026-08-30) showed exactly why that's
  // not safe to skip: a homing search ran the stepper at a perfectly
  // steady commanded speed for 30+ seconds without the switch ever
  // tripping - genuinely jammed against the mechanical stop the whole
  // time, with nothing watching for it. During homing, a detected stall
  // sets homingStallDetected instead of stopping directly here - the
  // homing state machine (updateHoming()) consumes that flag and aborts
  // the current search (HOMING_ERROR) using its own normal-profile
  // speed/accel restore, rather than this code reaching into homing's
  // state directly.
  if (tmcStallEnabledConfig && isRunning &&
      (now - runStartMillis) > STALL_RAMP_GRACE_MS &&
      (now - lastStallPollMillis) >= STALL_POLL_INTERVAL_MS) {
    lastStallPollMillis = now;

    tmcStatus.stallGuardResult = tmcDriver->SG_RESULT();

    if (tmcStatus.stallGuardResult <= tmcStallThresholdConfig) {
      stallDebounceCount++;
      if (stallDebounceCount >= STALL_DEBOUNCE_COUNT) {
        Serial.print("TMC2209: stall detected (SG_RESULT=");
        Serial.print(tmcStatus.stallGuardResult);
        Serial.println(")");
        persistLog("TMC stall detected SG_RESULT=%d pos=%ld homing=%d", tmcStatus.stallGuardResult,
                   (long)stepper->getCurrentPosition(), isHoming() ? 1 : 0);
        if (isHoming()) {
          homingStallDetected = true;
        } else {
          Serial.println("Stopping motor");
          stepper->forceStop();
          tmcStatus.stalled = true;
        }
      }
    } else {
      stallDebounceCount = 0;
    }
  }
}
