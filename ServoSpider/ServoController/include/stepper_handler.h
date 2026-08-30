#ifndef STEPPER_H
#define STEPPER_H

#include <Arduino.h>
#include "FastAccelStepper.h"

// Stepper pin definitions
#define stepperDirectionPin D2
#define stepperEnablePin D3
#define stepperStepPin D1
#define homingSwitchPin D10

// Stepper default values
#define stepperAccel 20000
#define stepperAccelHoming 1000000
#define stepperSpeed 6500
#define stepperSpeedHoming 6000

// Homing states
enum HomingState {
  HOMING_IDLE,
  HOMING_CHECK_SWITCH,
  HOMING_MOVE_OFF_FORWARD,
  HOMING_MOVE_OFF_BACKWARD,
  HOMING_WAIT_CLEAR_SWITCH,
  HOMING_FIND_INITIAL,
  HOMING_MOVE_OFF_INITIAL,      // Move off the switch after finding initial position
  HOMING_FIND_OTHER_END,
  HOMING_MOVE_OFF_OTHER_END,    // Move off the switch after finding other end
  HOMING_RETURN_TO_ZERO,
  HOMING_COMPLETE,
  HOMING_ERROR
};

// Stepper global variables
extern FastAccelStepperEngine engine;
extern FastAccelStepper *stepper;
extern volatile bool interruptTriggered;
extern int bottomPosition;
extern bool homed;
extern HomingState homingState;
extern unsigned long homingStateTime;
extern int homingCounter;

// Stepper configuration variables
extern int stepperSpeedConfig;
extern int stepperAccelConfig;
extern int jumpStartConfig;
extern bool autoHomeOnBootConfig;
extern int stepperSpeedHomingConfig;   // Homing speed, Hz - tune per microstep setting (6000 tuned for 16 microsteps)
extern int stepperAccelHomingConfig;   // Homing acceleration, steps/s^2

// Small-move "tracking" profile: used instead of stepperSpeedConfig/
// stepperAccelConfig when a new position command is within
// stepperTrackThresholdConfig steps of the current position, so a stream
// of small incremental DDP updates (e.g. xLights slowly panning a value)
// blends into smooth continuous motion instead of a torque-spiking
// accelerate/decelerate cycle repeated on every packet. See updateHoming-
// adjacent position-handling code in main.cpp's loop().
extern bool stepperTrackEnabledConfig;
extern int stepperTrackThresholdConfig;  // steps - deltas at/below this use the tracking profile
extern int stepperTrackSpeedConfig;      // Hz
extern int stepperTrackAccelConfig;      // steps/s^2 - keep well below stepperAccelConfig; peak torque
                                          // demand at the start of a move scales with this, not with
                                          // the (lower) target speed, so it must be independently gentle
                                          // or the motor can stall/skip steps starting from near-rest.

// Stepper functions
void IRAM_ATTR handleHomingInterrupt();
void startHoming();             // Start non-blocking homing
void updateHoming();            // Call from loop() to update homing state
bool isHoming();                // Returns true if homing is in progress
bool isHomingSwitchTripped();   // Returns true if homing switch is currently triggered
void initializeStepper();

#endif
