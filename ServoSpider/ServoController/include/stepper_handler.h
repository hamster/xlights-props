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

// Stepper functions
void IRAM_ATTR handleHomingInterrupt();
void startHoming();             // Start non-blocking homing
void updateHoming();            // Call from loop() to update homing state
bool isHoming();                // Returns true if homing is in progress
bool isHomingSwitchTripped();   // Returns true if homing switch is currently triggered
void initializeStepper();

#endif
