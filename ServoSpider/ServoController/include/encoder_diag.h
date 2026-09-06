#ifndef ENCODER_DIAG_H
#define ENCODER_DIAG_H

#include <Arduino.h>

// Encoder diagnostic sweep - added 2026-09-05 after a real bench chase (see
// TODO.md's encoder section) through mechanical slip, an over-aggressive
// software debounce, and finally a hardware RC-filter fix, all diagnosed
// from small, manually-run batches of 0-100%-0% cycles. Automates exactly
// that: repeats a 0%->100%->0% cycle 8 times at each of four stepper speeds
// (3000/4000/5000/6500 Hz), printing one CSV row per leg (stepper position,
// encoder count, cumulative missed-transition count) so a full sweep can be
// pasted somewhere and analyzed at once instead of transcribed by hand one
// run at a time. Direct moveTo() calls, same as $CHECKSTEPS - bypasses DDP/
// tracking-mode entirely. Restores the stepper's original speed/accel when
// done. See startEncoderDiag()/updateEncoderDiag() in encoder_diag.cpp and
// the $ENCDIAG command in tuning_handler.cpp.
//
// Deliberately kept in its own file, separate from both stepper_handler
// (production code) and encoder_handler (the encoder reading mechanism
// itself, which might outlive this) - this sweep exists purely to bench-
// tune the stepper and is expected to be removed once that's done. Keeping
// it isolated means it can be pared off cleanly later without touching
// either of those.
enum EncDiagState {
  ENCDIAG_IDLE,
  ENCDIAG_MOVING,
};
extern EncDiagState encDiagState;

bool isEncoderDiagRunning();
void startEncoderDiag();   // Begins the full sweep; refuses if homing/checking/moving/already running
// Call from loop(), anywhere after updateStepCheck() - unlike that
// function, this doesn't need first refusal on pendingForceStop (it only
// reacts to stepper->isRunning() going false, however that happened -
// natural completion or updateHoming()'s own forceStop() on a switch trip -
// not to the raw flag itself), but is kept alongside the other diagnostic
// update calls for consistent ordering.
void updateEncoderDiag();

#endif
