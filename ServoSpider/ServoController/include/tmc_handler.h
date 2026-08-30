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
extern uint16_t tmcMicrostepsConfig;      // microsteps per full step (1,2,4,8,16,32,64,128,256)
extern uint8_t tmcHstrtConfig;            // SpreadCycle hysteresis start, raw register value 0-7
extern uint8_t tmcHendConfig;             // SpreadCycle hysteresis end, raw register value 0-15
extern uint8_t tmcPwmRegConfig;           // StealthChop autoscale max PWM amplitude step, 1-15
extern uint8_t tmcPwmLimConfig;           // StealthChop autoscale amplitude limit, 0-15
extern bool tmcPwmAutogradConfig;         // StealthChop automatic gradient adaptation

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

// Explicitly restarts the stall-detection ramp grace period (see
// STALL_RAMP_GRACE_MS in tmc_handler.cpp) right at the moment a genuinely
// new move/direction begins. updateTmc() also infers this from an
// isRunning() false->true transition on its own, but that heuristic can
// miss a real restart if the stepper never reads as fully idle in between
// two back-to-back commands (e.g. one small move finishing and a new
// runBackward()/runForward() starting in the same loop() iteration, or
// close enough together that isRunning() stays continuously true) - in
// that case the grace period ends up measured from whenever the *earlier*
// move started, letting a stall check fire on the new move almost
// immediately even though its own ramp has barely begun. Found on the
// bench (2026-08-30): the WAIT_CLEAR_SWITCH -> FIND_INITIAL transition
// reported a "stall" (SG_RESULT=0-2) just ~110-130ms after runBackward()
// was called - well under the 300ms grace period - and raising jumpStart
// (which should help a *real* torque stall) made the reading more extreme,
// not better, pointing at a false trigger from this exact gap rather than
// a genuine stall. Call this right after any runBackward()/runForward()/
// moveTo() that starts a search or move stall-detection should judge
// independently of whatever came immediately before it.
void tmcResetStallRampTimer();

#endif
