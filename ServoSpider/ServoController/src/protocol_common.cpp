#include "protocol_common.h"
#include "stepper_handler.h"  // for the `stepper` global - see persistProtocolDebugIfPending()
#include <Preferences.h>

extern Preferences preferences;  // defined in main.cpp

// Protocol settings
protocolType protocolConfig = PROTOCOL_DDP;
bool stepperControlEnabled = true;  // Enable stepper control (stepper uses channel 1, or 1-2 for 16-bit)
bool control16BitConfig = false;    // 16-bit stepper control (only used if stepperControlEnabled)
bool protocolDebugConfig = false;
bool protocolDebugPendingSave = false;
bool compactLogEnabled = false;

// See the declaration comment in protocol_common.h for why this can't just
// happen synchronously inside the /protocol-debug handler.
void persistProtocolDebugIfPending() {
  if (!protocolDebugPendingSave) {
    return;
  }
  if (stepper != NULL && stepper->isRunning()) {
    return;  // wait for a quiet moment - the value is already live in RAM
  }
  preferences.putBool("protocolDebug", protocolDebugConfig);
  protocolDebugPendingSave = false;
}

// Blank time settings (used by DDP)
int ledBlankTimeConfig = 0;      // Seconds before LEDs turn off (0 = disabled)
int stepperBlankTimeConfig = 0;  // Seconds before stepper goes to zero (0 = disabled)
unsigned long lastProtocolUpdateTime = 0;  // Timestamp of last protocol update

// Shared position request (written by active protocol handler)
uint16_t positionRequest = 0;
