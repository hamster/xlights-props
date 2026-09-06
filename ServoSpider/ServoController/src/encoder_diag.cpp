#include "encoder_diag.h"
#include "stepper_handler.h"
#include "tmc_handler.h"
#include "encoder_handler.h"

// Encoder diagnostic sweep state - see startEncoderDiag()/updateEncoderDiag()
// and encoder_diag.h's declaration comment.
EncDiagState encDiagState = ENCDIAG_IDLE;
static const int ENCDIAG_FREQS[4] = {3000, 4000, 5000, 6500};
static const int ENCDIAG_RUNS_PER_FREQ = 8;
static int encDiagFreqIndex = 0;
static int encDiagRunIndex = 0;      // 1-based, which of the 8 runs at the current frequency
static bool encDiagHeadingTo100 = true;  // What the just-issued move was targeting - true=100%, false=0%
static int encDiagSavedSpeed = 0;
static int encDiagSavedAccel = 0;
// A settle-wait state (waiting out RMT_IDLE_THRESHOLD_TICKS before trusting
// the encoder count after a stop) was tried here 2026-09-06 to fix the
// direction-at-reversal bug - every bench run that included it crashed
// (Core 0 interrupt wdt timeout), regardless of RMT channel/mem_block_num/
// idle_threshold or WiFi on/off, while the one run that predates it did
// not crash. Reverted back to the original immediate-drain-on-stop
// behavior pending a real root-cause of that crash - see TODO.md. The
// direction-at-reversal bug this was meant to fix is real and still
// unfixed.

bool isEncoderDiagRunning() {
  return encDiagState != ENCDIAG_IDLE;
}

// Prints one CSV row for the leg that was just completed. phase is 100 or
// 0, matching whichever target encDiagHeadingTo100 said we were heading to.
static void printEncDiagRow(int freqHz, int run, int phase) {
  Serial.print("ENCDIAG,");
  Serial.print(freqHz);
  Serial.print(",");
  Serial.print(run);
  Serial.print(",");
  Serial.print(phase);
  Serial.print(",");
  Serial.print(stepper->getCurrentPosition());
  Serial.print(",");
  Serial.print(getEncoderCount());
  Serial.print(",");
  Serial.println(getMissedTransitionCount());
}

// Issues the next move in the sweep - to bottomPosition if headingTo100,
// else to 0 - and remembers which one so updateEncoderDiag() knows what was
// just reached once the stepper stops.
static void encDiagIssueMove(bool headingTo100) {
  encDiagHeadingTo100 = headingTo100;
  long target = headingTo100 ? bottomPosition : 0;
  stepper->moveTo(target);
  tmcResetStallRampTimer();
}

// Begins the full sweep: startEncoderDiag()/updateEncoderDiag()'s
// declaration comment (encoder_diag.h) has the overall design. Refuses
// under the same conditions $CHECKSTEPS does - this is just as much a
// direct diagnostic move, bypassing DDP/tracking-mode entirely.
void startEncoderDiag() {
  if (isHoming() || isStepChecking() || isEncoderDiagRunning() || stepper->isRunning()) {
    Serial.println("ERR ENCDIAG refused - homing/checking/moving/already running");
    return;
  }
  if (!homed) {
    Serial.println("ERR ENCDIAG refused - not homed, run $HOME first");
    return;
  }

  encDiagSavedSpeed = stepperSpeedConfig;
  encDiagSavedAccel = stepperAccelConfig;

  encDiagFreqIndex = 0;
  encDiagRunIndex = 1;

  Serial.println("ENCDIAG_START");
  Serial.println("freqHz,run,phase,stepperPos,encoderCount,missedTotal");

  stepper->setSpeedInHz(ENCDIAG_FREQS[encDiagFreqIndex]);
  stepper->setAcceleration(stepperAccelConfig);
  encDiagIssueMove(true);
  encDiagState = ENCDIAG_MOVING;
}

// Call from loop() (see encoder_diag.h's declaration comment for why
// ordering relative to updateHoming() doesn't matter here).
void updateEncoderDiag() {
  if (encDiagState != ENCDIAG_MOVING) {
    return;
  }
  if (stepper->isRunning()) {
    return;  // Still moving (or still coasting to a stop after a switch trip) - wait
  }

  // Force a drain right here, before deciding/issuing the next (likely
  // opposite-direction) move - see updateEncoder()'s declaration comment
  // (encoder_handler.h) for why this matters: the stepper reads 0 speed at
  // this exact instant (just stopped), so this flushes any real data still
  // sitting in the ring buffer under the *correct*, just-finished
  // direction, before anything about to change could make it wrong. (Real
  // residual risk here - the RMT hardware itself may not have flushed this
  // run's final tail yet, since that only happens after
  // RMT_IDLE_THRESHOLD_TICKS of true silence - see that constant's comment
  // in encoder_handler.cpp. An explicit wait for that was tried and
  // reverted; see this file's top-of-function history note.)
  updateEncoder();

  int freqHz = ENCDIAG_FREQS[encDiagFreqIndex];
  printEncDiagRow(freqHz, encDiagRunIndex, encDiagHeadingTo100 ? 100 : 0);

  if (encDiagHeadingTo100) {
    // Just reached 100% - head back to 0% to complete this run.
    encDiagIssueMove(false);
    return;
  }

  // Just reached 0% - this run is complete.
  encDiagRunIndex++;
  if (encDiagRunIndex <= ENCDIAG_RUNS_PER_FREQ) {
    encDiagIssueMove(true);
    return;
  }

  // Done with this frequency - move to the next one, or finish entirely.
  encDiagFreqIndex++;
  if (encDiagFreqIndex < 4) {
    encDiagRunIndex = 1;
    stepper->setSpeedInHz(ENCDIAG_FREQS[encDiagFreqIndex]);
    encDiagIssueMove(true);
    return;
  }

  // Full sweep complete - restore the stepper's original profile.
  stepper->setSpeedInHz(encDiagSavedSpeed);
  stepper->setAcceleration(encDiagSavedAccel);
  encDiagState = ENCDIAG_IDLE;
  Serial.println("ENCDIAG_DONE");
}
