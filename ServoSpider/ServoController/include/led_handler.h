#ifndef LED_HANDLER_H
#define LED_HANDLER_H

#include <Arduino.h>
#include <FastLED.h>

// Maximum number of LEDs supported
#define MAX_LEDS 1000

// LED data pin
#define LED_DATA_PIN D4

// LED Configuration
extern int ledPixelCount;
extern String ledColorOrder;
extern float ledGamma;
extern int ledBrightness;
extern int ledStartNullPixels;
extern int ledEndNullPixels;

// LED pixel array
extern CRGB leds[MAX_LEDS];
extern bool ledsInitialized;
extern bool ledsBlanked;

// LED statistics
extern int ledPixelsReceived;     // Current pixels received in last update
extern int ledMaxPixelsReceived;  // Maximum pixel index seen since last blank

// Status LED pin
#define STATUS_LED_PIN D8

// Status LED state
extern bool statusLedState;
extern unsigned long statusLedBlinkInterval;
extern bool locateMode;
extern hw_timer_t *ledTimer;

// Status LED functions
void initStatusLed();
void IRAM_ATTR onLedTimer();

// Pixel LED functions
void initPixelLeds();
void updatePixelLeds(uint8_t* data, uint16_t size, int stepperChannels);
void updatePixelLedsFragmented(uint8_t* data, uint16_t size, uint32_t pixelOffset);
void blankPixelLeds();

// LED preview for web status (returns compact hex string "RRGGBBRRGGBB...", up to maxPixels)
// Uses static buffer to avoid heap allocation, returns const char* (not String)
const char* getLedPreviewJson(int maxPixels);

#endif
