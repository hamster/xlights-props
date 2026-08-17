#ifndef HTML_HANDLERS_H
#define HTML_HANDLERS_H

#include <Arduino.h>
#include <WebServer.h>

// Web server
extern WebServer server;

// Uptime tracking
extern unsigned long bootTime;

// OTA update flag
extern bool otaInProgress;

// Web handler functions
void handleRoot();
void handleSaveWifi();
void handleSaveAP();
void handleSaveStepper();
void handleSaveProtocol();
void handleSaveLed();
void handleConnect();
void handleReboot();
void handleResetSettings();
void handleHoming();
void handleLocate();
void handleStatusData();
void handleMove();
void handleSetPosition();

// Web server initialization
void startWebServer();

#endif
