#ifndef TMC_HANDLER_H
#define TMC_HANDLER_H

#include <Arduino.h>
#include <TMCStepper.h>

// TMC2209 UART pin definitions. These modules use single-wire UART: both
// wires are expected to land on the driver's PDN_UART pad, so running the
// ESP32's RX and TX to it separately (as wired here) is normal - TMCStepper
// handles the resulting echo/half-duplex behavior internally.
#define tmcUartRxPin D7
#define tmcUartTxPin D6
#define tmcUartBaud 115200

// Driver instance - NULL until initTmc() runs and succeeds
extern TMC2209Stepper *tmcDriver;
extern bool tmcConnected;  // true once test_connection() succeeds

// Configuration (loaded from / saved to Preferences)
extern bool tmcEnabledConfig;             // master enable for the UART link
extern float tmcRSenseConfig;             // sense resistor, ohms (module-specific)
extern uint8_t tmcAddressConfig;          // UART address from MS1/MS2 strapping, 0-3
extern uint16_t tmcRunCurrentConfig;      // RMS run current, mA
extern uint8_t tmcHoldPercentConfig;      // hold current as % of run current
extern bool tmcStealthChopConfig;         // true = StealthChop, false = SpreadCycle
extern bool tmcStallEnabledConfig;        // enable stall-detection safety cutoff
extern uint16_t tmcStallThresholdConfig;  // raw SG_RESULT trip point (0-1023, lower = more loaded)

// Live diagnostics snapshot, refreshed periodically by updateTmc()
struct TmcStatus {
  bool commOk = false;
  bool overTempWarning = false;
  bool overTempShutdown = false;
  bool shortToGroundA = false;
  bool shortToGroundB = false;
  bool openLoadA = false;
  bool openLoadB = false;
  bool uartCrcError = false;
  bool resetSinceLastCheck = false;  // driver reported a power-on/brownout reset
  uint16_t stallGuardResult = 0;
  bool stalled = false;  // latched - stays true until clearTmcStall()
  unsigned long lastUpdateMillis = 0;
};
extern TmcStatus tmcStatus;

void initTmc();           // Call from setup(), after initializeStepper()
void updateTmc();         // Call every loop() iteration
void applyTmcSettings();  // Push current/chopper-mode config to the driver
void clearTmcStall();     // Acknowledge a latched stall fault

#endif
