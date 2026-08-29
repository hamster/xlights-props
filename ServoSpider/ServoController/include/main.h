#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>

void printStatus();
void printFullStatus();
float calcPosition(float, bool);
void handleSerialCommands();

#endif
