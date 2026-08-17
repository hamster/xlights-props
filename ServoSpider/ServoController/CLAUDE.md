# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ServoController is an ESP32-based lighting prop controller for xLights. It controls a stepper motor along a linear rail and optional WS2812 LED pixels, receiving commands via ArtNet (DMX) or DDP protocols over WiFi.

Target hardware: Seeed XIAO ESP32-S3 (also supports C3/C6 variants)

## Build Commands

```bash
# Build firmware
pio run

# Build and upload via USB
pio run --target upload

# Monitor serial output (115200 baud)
pio device monitor

# Clean build
pio run --target clean
```

Build artifacts are auto-copied to `build/ServoController-<version>.bin`. Version SUBREV auto-increments on each build.

## Architecture

### Single-Core Design
- **Core 1 (Main)**: WiFi, web server, stepper control, protocol parsing, LED updates (main loop)
- FastLED.show() is called directly from protocol handlers for immediate LED updates

### Key Data Flow
```
ArtNet/DDP packet → Protocol Handler → positionRequest (shared variable) → main loop → stepper movement
                                    → updatePixelLeds() → FastLED.show()
```

### Channel Layout
- Stepper control: Channel 1 (8-bit) or Channels 1-2 (16-bit MSB-first) when enabled
- LED data: Always starts at channel 3 (byte offset 2) for RGB pixel alignment
- This ensures LEDs always begin on an RGB boundary regardless of stepper mode

### Module Responsibilities

| Module | Purpose |
|--------|---------|
| `protocol_common` | Shared protocol settings and `positionRequest` variable |
| `artnet_handler` | ArtNet receiver, subscribes to configured universe |
| `ddp_handler` | DDP receiver on port 4048, handles fragmented packets |
| `led_handler` | FastLED direct updates, gamma correction, color order mapping |
| `stepper_handler` | FastAccelStepper control, non-blocking homing state machine |
| `wifi_handler` | WiFi client/AP modes, captive portal DNS, mDNS |
| `html_handler` | Web API endpoints, serves HTML from `html.h` |
| `ota_handler` | HTTP OTA firmware updates with chunked upload |

### Homing State Machine
The stepper uses a non-blocking state machine (`HomingState` enum) that finds both ends of travel by winding/unwinding a rope spool. The center point becomes `bottomPosition`. Call `updateHoming()` every loop iteration.

### Configuration Storage
All settings stored in ESP32 Preferences (NVS flash):
- `wifi-config` namespace
- Keys: `ssid`, `password`, `hostname`, `protocol`, `stepperControl`, `control16Bit`, etc.

## Pin Configuration

| Pin | Function |
|-----|----------|
| D1 | Stepper Step |
| D2 | Stepper Direction |
| D3 | Stepper Enable |
| D8 | Status LED |
| D9 | WS2812 LED Data |
| D10 | Homing Switch |

## Web Interface

HTML is embedded in `include/html.h` as a raw string literal with template placeholders (`{{VARIABLE}}`). JavaScript fetches `/status-data` for live updates and `/led-preview` separately (optional, saves bandwidth).

## Serial Commands

Type in serial monitor (115200 baud):
- `?` - Help menu
- `h` - Start homing
- `r` - Reboot
- `s` - Print status
- `w` - Reconnect WiFi
- `a` - Switch to AP mode
- `p` - Print stepper position
- `f`/`b` - Move forward/backward 10 steps

## Important Patterns

- Protocol handlers update `lastProtocolUpdateTime` for blank timeout tracking
- OTA sets `otaInProgress` flag to pause protocol handling during updates
- LED updates are called directly from protocol handlers (no queue/task)
- WiFi connection monitoring runs every 30s with auto-reconnect
- Use `extern` declarations in headers, definitions in `.cpp` files
