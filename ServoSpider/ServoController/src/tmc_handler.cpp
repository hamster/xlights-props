#include "tmc_handler.h"
#include "stepper_handler.h"
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

  tmcConnected = (tmcDriver->test_connection() == 0);
  tmcStatus.commOk = tmcConnected;

  if (tmcConnected) {
    Serial.println("TMC2209 UART link established");
    applyTmcSettings();
    // Clear the power-on-reset flag now that we've taken over configuration
    tmcDriver->GSTAT(GSTAT_ALL_BITS);
  } else {
    Serial.println("TMC2209 UART link FAILED - check RX/TX wiring to PDN_UART, and the RSense/address settings");
  }
}

void applyTmcSettings() {
  if (!tmcConnected || tmcDriver == nullptr) return;

  float holdMultiplier = tmcHoldPercentConfig / 100.0f;
  tmcDriver->rms_current(tmcRunCurrentConfig, holdMultiplier);

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

  // Stall-detection safety cutoff: only while actually moving under normal
  // (already-homed) operation, so it never fights the homing state machine's
  // own switch-based logic.
  if (tmcStallEnabledConfig && isRunning && homed && !isHoming() &&
      (now - runStartMillis) > STALL_RAMP_GRACE_MS &&
      (now - lastStallPollMillis) >= STALL_POLL_INTERVAL_MS) {
    lastStallPollMillis = now;

    tmcStatus.stallGuardResult = tmcDriver->SG_RESULT();

    if (tmcStatus.stallGuardResult <= tmcStallThresholdConfig) {
      stallDebounceCount++;
      if (stallDebounceCount >= STALL_DEBOUNCE_COUNT) {
        Serial.print("TMC2209: stall detected (SG_RESULT=");
        Serial.print(tmcStatus.stallGuardResult);
        Serial.println("), stopping motor");
        stepper->forceStop();
        tmcStatus.stalled = true;
      }
    } else {
      stallDebounceCount = 0;
    }
  }
}
