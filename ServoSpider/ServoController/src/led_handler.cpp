#include "led_handler.h"
#include <Preferences.h>
#include "protocol_common.h"

// External preferences object
extern Preferences preferences;

// LED Configuration variables
int ledPixelCount = 0;
String ledColorOrder = "GRB";
float ledGamma = 1.0;
int ledBrightness = 100;
int ledStartNullPixels = 0;
int ledEndNullPixels = 0;

// LED pixel array
CRGB leds[MAX_LEDS];
bool ledsInitialized = false;
bool ledsBlanked = false;

// LED statistics
int ledPixelsReceived = 0;
int ledMaxPixelsReceived = 0;

// LED test pattern state (see setLedTestMode/updateLedTestMode)
bool ledTestModeActive = false;
static unsigned long ledTestLastUpdateMillis = 0;
static int ledTestPhase = 0;
static const unsigned long LED_TEST_INTERVAL_MS = 1000;

// Status LED state
bool statusLedState = false;
unsigned long statusLedBlinkInterval = 1000;  // Default 1 second for connected state
bool locateMode = false;

// Timer for status LED blinking
hw_timer_t *ledTimer = NULL;
volatile unsigned long timerCounter = 0;

// Morse code SOS pattern: ... --- ...
// Using simple on/off states, each element is 50ms (5 ticks of 10ms timer)
// Dot = 1 unit on, 1 unit off
// Dash = 3 units on, 1 unit off
// Letter gap = 3 units off
// Word gap = 7 units off
const bool morsePattern[] = {
  // S (...)
  1,0,  1,0,  1,0,     // 3 dots
  0,0,0,               // letter gap
  // O (---)
  1,1,1,0,  1,1,1,0,  1,1,1,0,  // 3 dashes
  0,0,0,               // letter gap
  // S (...)
  1,0,  1,0,  1,0,     // 3 dots
  0,0,0,0,0,0,0        // word gap
};
const int morsePatternLength = sizeof(morsePattern) / sizeof(morsePattern[0]);
volatile int morseIndex = 0;
volatile int morseUnitCounter = 0;

// Status LED timer interrupt handler
void IRAM_ATTR onLedTimer() {
  if (locateMode) {
    // Morse code SOS pattern
    // Each unit is 100ms (10 ticks of 10ms timer)
    morseUnitCounter++;
    if (morseUnitCounter >= 10) {  // 100ms elapsed
      morseUnitCounter = 0;

      // Set LED based on current pattern
      digitalWrite(STATUS_LED_PIN, morsePattern[morseIndex] ? HIGH : LOW);

      // Move to next element
      morseIndex++;
      if (morseIndex >= morsePatternLength) {
        morseIndex = 0;
      }
    }
  } else {
    // Normal blinking mode
    timerCounter++;
    unsigned long ticksPerInterval = statusLedBlinkInterval / 10;
    if (timerCounter >= ticksPerInterval) {
      statusLedState = !statusLedState;
      digitalWrite(STATUS_LED_PIN, statusLedState ? HIGH : LOW);
      timerCounter = 0;
    }
  }
}

void initStatusLed() {
  // Initialize status LED pin
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  statusLedState = false;

  // Configure timer for LED blinking (Timer 1, prescaler 80 for 1MHz, interrupt every 10ms)
  // Using Timer 1 to avoid conflicts with WiFi stack which may use Timer 0
  ledTimer = timerBegin(1, 80, true);  // Timer 1, prescaler 80, count up
  timerAttachInterrupt(ledTimer, &onLedTimer, true);  // Attach interrupt handler
  timerAlarmWrite(ledTimer, 10000, true);  // Trigger every 10ms (10000 microseconds)
  timerAlarmEnable(ledTimer);  // Enable the timer
}

void initPixelLeds() {
  // Load LED configuration from preferences
  ledPixelCount = preferences.getInt("ledPixelCount", 0);
  ledColorOrder = preferences.getString("ledColorOrder", "GRB");
  ledGamma = preferences.getFloat("ledGamma", 1.0);
  ledBrightness = preferences.getInt("ledBrightness", 100);
  ledStartNullPixels = preferences.getInt("ledStartNull", 0);
  ledEndNullPixels = preferences.getInt("ledEndNull", 0);

  Serial.println("LED Configuration:");
  Serial.print("  Pixel Count: ");
  Serial.println(ledPixelCount);
  Serial.print("  Color Order: ");
  Serial.println(ledColorOrder);
  Serial.print("  Gamma: ");
  Serial.println(ledGamma);
  Serial.print("  Brightness: ");
  Serial.print(ledBrightness);
  Serial.println("%");
  Serial.print("  Start Null Pixels: ");
  Serial.println(ledStartNullPixels);
  Serial.print("  End Null Pixels: ");
  Serial.println(ledEndNullPixels);

  // Initialize FastLED if we have pixels configured
  if (ledPixelCount > 0) {
    int totalPixels = ledStartNullPixels + ledPixelCount + ledEndNullPixels;
    if (totalPixels > MAX_LEDS) {
      totalPixels = MAX_LEDS;
    }

    // Initialize FastLED - RGB order (no hardware reordering)
    // CRGB stores as RGB internally, we handle color order via ledColorOrder config
    FastLED.addLeds<WS2812B, LED_DATA_PIN, RGB>(leds, totalPixels);
    FastLED.setBrightness(ledBrightness * 255 / 100);
    FastLED.setCorrection(TypicalLEDStrip);

    // Clear all LEDs initially
    fill_solid(leds, totalPixels, CRGB::Black);
    FastLED.show();

    ledsInitialized = true;
    ledsBlanked = true;  // Start in blanked state

    Serial.print("  FastLED initialized with ");
    Serial.print(totalPixels);
    Serial.println(" total pixels");
  }
}

// Apply gamma correction to a single value
uint8_t applyGamma(uint8_t value, float gamma) {
  if (gamma == 1.0) {
    return value;
  }
  return (uint8_t)(pow((float)value / 255.0, gamma) * 255.0 + 0.5);
}

// Update pixel LEDs with new data (called directly from protocol handlers)
void updatePixelLeds(uint8_t* data, uint16_t size, int stepperChannels) {
  if (ledTestModeActive) {
    return;  // Test mode owns the strip; ignore DDP pixel data until disabled
  }
  if (!ledsInitialized || ledPixelCount == 0) {
    return;
  }

  // Data layout: [stepper channel(s)] [LED RGB data...]
  // Skip the stepper channels at the start
  int ledDataOffset = stepperChannels;

  // Calculate how many LED bytes we have (3 bytes per pixel for RGB)
  int availableLedBytes = size - ledDataOffset;
  if (availableLedBytes <= 0) {
    return;
  }

  // Calculate how many complete pixels we received
  int availablePixels = availableLedBytes / 3;
  ledPixelsReceived = availablePixels;  // Track for status display

  // Update max pixel count
  if (availablePixels > ledMaxPixelsReceived) {
    ledMaxPixelsReceived = availablePixels;
  }

  int pixelsToUpdate = min(availablePixels, ledPixelCount);

  if (pixelsToUpdate <= 0) {
    return;
  }

  // Update pixels, accounting for null pixels at start
  for (int i = 0; i < pixelsToUpdate; i++) {
    int dataIndex = ledDataOffset + (i * 3);
    int ledIndex = ledStartNullPixels + i;

    if (ledIndex >= MAX_LEDS) {
      break;
    }

    uint8_t r, g, b;

    // Extract RGB values based on color order configuration
    if (ledColorOrder == "RGB") {
      r = data[dataIndex];
      g = data[dataIndex + 1];
      b = data[dataIndex + 2];
    } else if (ledColorOrder == "RBG") {
      r = data[dataIndex];
      b = data[dataIndex + 1];
      g = data[dataIndex + 2];
    } else if (ledColorOrder == "GRB") {
      g = data[dataIndex];
      r = data[dataIndex + 1];
      b = data[dataIndex + 2];
    } else if (ledColorOrder == "GBR") {
      g = data[dataIndex];
      b = data[dataIndex + 1];
      r = data[dataIndex + 2];
    } else if (ledColorOrder == "BRG") {
      b = data[dataIndex];
      r = data[dataIndex + 1];
      g = data[dataIndex + 2];
    } else if (ledColorOrder == "BGR") {
      b = data[dataIndex];
      g = data[dataIndex + 1];
      r = data[dataIndex + 2];
    } else {
      // Default to GRB
      g = data[dataIndex];
      r = data[dataIndex + 1];
      b = data[dataIndex + 2];
    }

    // Apply gamma correction
    r = applyGamma(r, ledGamma);
    g = applyGamma(g, ledGamma);
    b = applyGamma(b, ledGamma);

    // Set LED color - use member assignment for explicit control
    leds[ledIndex].r = r;
    leds[ledIndex].g = g;
    leds[ledIndex].b = b;

  }

  FastLED.show();
  ledsBlanked = false;

  if (protocolDebugConfig) {
    Serial.print("LED: Updated ");
    Serial.print(pixelsToUpdate);
    Serial.println(" pixels");
  }
}

// Update pixel LEDs from fragmented packet data (for DDP multi-packet updates)
void updatePixelLedsFragmented(uint8_t* data, uint16_t size, uint32_t pixelOffset) {
  if (ledTestModeActive) {
    return;  // Test mode owns the strip; ignore DDP pixel data until disabled
  }
  if (!ledsInitialized || ledPixelCount == 0) {
    return;
  }

  // Calculate how many complete pixels we can update from this data
  int availablePixels = size / 3;
  if (availablePixels <= 0) {
    return;
  }

  // Make sure we don't exceed our configured pixel count
  int pixelsToUpdate = min(availablePixels, ledPixelCount - (int)pixelOffset);
  if (pixelsToUpdate <= 0) {
    return;
  }

  // Track total pixels received (accumulate across fragments)
  ledPixelsReceived = pixelOffset + pixelsToUpdate;

  // Update max pixel count
  if (ledPixelsReceived > ledMaxPixelsReceived) {
    ledMaxPixelsReceived = ledPixelsReceived;
  }

  // Update pixels starting from the specified offset
  for (int i = 0; i < pixelsToUpdate; i++) {
    int dataIndex = i * 3;
    int ledIndex = ledStartNullPixels + pixelOffset + i;

    if (ledIndex >= MAX_LEDS) {
      break;
    }

    uint8_t r, g, b;

    // Extract RGB values based on color order configuration
    if (ledColorOrder == "RGB") {
      r = data[dataIndex];
      g = data[dataIndex + 1];
      b = data[dataIndex + 2];
    } else if (ledColorOrder == "RBG") {
      r = data[dataIndex];
      b = data[dataIndex + 1];
      g = data[dataIndex + 2];
    } else if (ledColorOrder == "GRB") {
      g = data[dataIndex];
      r = data[dataIndex + 1];
      b = data[dataIndex + 2];
    } else if (ledColorOrder == "GBR") {
      g = data[dataIndex];
      b = data[dataIndex + 1];
      r = data[dataIndex + 2];
    } else if (ledColorOrder == "BRG") {
      b = data[dataIndex];
      r = data[dataIndex + 1];
      g = data[dataIndex + 2];
    } else if (ledColorOrder == "BGR") {
      b = data[dataIndex];
      g = data[dataIndex + 1];
      r = data[dataIndex + 2];
    } else {
      // Default to GRB
      g = data[dataIndex];
      r = data[dataIndex + 1];
      b = data[dataIndex + 2];
    }

    // Apply gamma correction
    r = applyGamma(r, ledGamma);
    g = applyGamma(g, ledGamma);
    b = applyGamma(b, ledGamma);

    // Set LED color - use member assignment for explicit control
    leds[ledIndex].r = r;
    leds[ledIndex].g = g;
    leds[ledIndex].b = b;
  }

  FastLED.show();
  ledsBlanked = false;

  if (protocolDebugConfig) {
    Serial.print("LED: Updated ");
    Serial.print(pixelsToUpdate);
    Serial.print(" pixels starting at offset ");
    Serial.println(pixelOffset);
  }
}

// Blank all pixel LEDs (called directly from main loop)
void blankPixelLeds() {
  if (!ledsInitialized || ledsBlanked) {
    return;
  }

  int totalPixels = ledStartNullPixels + ledPixelCount + ledEndNullPixels;
  if (totalPixels > MAX_LEDS) {
    totalPixels = MAX_LEDS;
  }

  fill_solid(leds, totalPixels, CRGB::Black);
  FastLED.show();
  ledsBlanked = true;

  // Reset pixel tracking on blank
  ledPixelsReceived = 0;
  ledMaxPixelsReceived = 0;

  if (protocolDebugConfig) {
    Serial.println("LED: Blanked all pixels");
  }
}

// Enable/disable the local test pattern. Enabling resets to a fresh phase 0
// and forces an immediate render (rather than waiting up to a second for the
// next tick); disabling blanks the strip so a stale test frame doesn't sit
// on the pixels until the next real DDP update arrives.
void setLedTestMode(bool enable) {
  ledTestModeActive = enable;
  if (enable) {
    ledTestPhase = 0;
    ledTestLastUpdateMillis = 0;
    Serial.println("LED test mode enabled - DDP pixel updates ignored until disabled");
  } else {
    Serial.println("LED test mode disabled");
    blankPixelLeds();
  }
}

// Marching RGB test pattern: pixel i shows testColors[(i + phase) % 3].
// Phase increments once per second, so pixel 0 goes Red -> Green -> Blue ->
// Red... and the whole R/G/B sequence appears to march down the strip.
void updateLedTestMode() {
  if (!ledTestModeActive || !ledsInitialized || ledPixelCount == 0) {
    return;
  }

  unsigned long now = millis();
  if (now - ledTestLastUpdateMillis < LED_TEST_INTERVAL_MS) {
    return;
  }
  ledTestLastUpdateMillis = now;

  static const CRGB testColors[3] = {CRGB::Red, CRGB::Green, CRGB::Blue};

  for (int i = 0; i < ledPixelCount; i++) {
    int ledIndex = ledStartNullPixels + i;
    if (ledIndex >= MAX_LEDS) {
      break;
    }
    leds[ledIndex] = testColors[(i + ledTestPhase) % 3];
  }

  FastLED.show();
  ledsBlanked = false;

  ledTestPhase = (ledTestPhase + 1) % 3;
}

// Get LED preview as JSON array for web status display
// Shows all configured LEDs (or up to maxPixels if specified)
// Static buffer for LED preview (6 hex chars per pixel RGB + null terminator)
// Max size: 1000 pixels * 6 chars = 6000 chars + some overhead
static char ledPreviewBuffer[MAX_LEDS * 6 + 100];

const char* getLedPreviewJson(int maxPixels) {
  if (!ledsInitialized || ledPixelCount == 0) {
    return "\"\"";  // Return empty string in compact format
  }

  // Show all configured LEDs up to maxPixels limit
  int pixelsToShow = min(maxPixels, ledPixelCount);

  // Sanity check to prevent buffer overflow
  if (pixelsToShow > MAX_LEDS || pixelsToShow < 0) {
    Serial.print("[ERROR] getLedPreviewJson: Invalid pixelsToShow = ");
    Serial.print(pixelsToShow);
    Serial.print(" (maxPixels=");
    Serial.print(maxPixels);
    Serial.print(", ledPixelCount=");
    Serial.print(ledPixelCount);
    Serial.println(")");
    return "\"\"";
  }

  // Build compact hex string format: "RRGGBBRRGGBB..." (6 chars per pixel)
  char* ptr = ledPreviewBuffer;
  *ptr++ = '"';  // Opening quote

  for (int i = 0; i < pixelsToShow; i++) {
    int ledIndex = ledStartNullPixels + i;
    if (ledIndex >= MAX_LEDS) break;

    // Write RGB as 6 hex characters directly to buffer
    sprintf(ptr, "%02X%02X%02X", leds[ledIndex].r, leds[ledIndex].g, leds[ledIndex].b);
    ptr += 6;
  }

  *ptr++ = '"';  // Closing quote
  *ptr = '\0';   // Null terminator

  // Return pointer to static buffer (no heap allocation)
  return ledPreviewBuffer;
}
