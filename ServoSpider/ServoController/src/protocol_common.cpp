#include "protocol_common.h"

// Protocol settings
protocolType protocolConfig = PROTOCOL_DDP;
bool stepperControlEnabled = true;  // Enable stepper control (stepper uses channel 1, or 1-2 for 16-bit)
bool control16BitConfig = false;    // 16-bit stepper control (only used if stepperControlEnabled)
bool protocolDebugConfig = false;
bool compactLogEnabled = false;

// Blank time settings (used by DDP)
int ledBlankTimeConfig = 0;      // Seconds before LEDs turn off (0 = disabled)
int stepperBlankTimeConfig = 0;  // Seconds before stepper goes to zero (0 = disabled)
unsigned long lastProtocolUpdateTime = 0;  // Timestamp of last protocol update

// Shared position request (written by active protocol handler)
uint16_t positionRequest = 0;
