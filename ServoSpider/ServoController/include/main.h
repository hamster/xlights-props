#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>

// LED pin definition
#define statusLedPin D8

// LED state tracking
extern bool ledState;
extern unsigned long ledBlinkInterval;
extern bool locateMode;

// Timer for LED blinking
extern hw_timer_t *ledTimer;

// LED timer interrupt handler
void IRAM_ATTR onLedTimer();
void printStatus();
float calcPosition(float, bool);

#endif
