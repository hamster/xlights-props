#include "FastAccelStepper.h"

// As in StepperDemo for Motor 1 on ESP32
#define dirPinStepper 19
#define enablePinStepper 23
#define stepPinStepper 21

FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *stepper = NULL;

void setup() {
  engine.init();
  stepper = engine.stepperConnectToPin(stepPinStepper);
  if (stepper) {
    stepper->setDirectionPin(dirPinStepper);
    stepper->setEnablePin(enablePinStepper);
    stepper->setAutoEnable(true);

    // If auto enable/disable need delays, just add (one or both):
    // stepper->setDelayToEnable(50);
    // stepper->setDelayToDisable(1000);

    //stepper->setSpeedInUs(2);  // the parameter is us/step !!!
    stepper->setSpeedInHz(6000);
    stepper->setAcceleration(12000);
    
  }
}

void loop() {

  // for(int i = 0; i < 5; i++){
  //   stepper->setSpeedInUs(i * 5); 

    stepper->move(30000, true);
    delay(250);
    stepper->move(-30000, true);
    delay(250);

    // delay(500);
//  }

}
