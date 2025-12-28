#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>
#include <WebServer.h>

// OTA update flag - set during firmware upload to disable other handlers
extern bool otaInProgress;

// External web server reference
extern WebServer server;

// OTA handler functions
void handleOTAUpdate();
void handleOTAUpdateComplete();

#endif
