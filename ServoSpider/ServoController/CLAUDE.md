# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ServoController is an ESP32-based lighting prop controller for xLights. It controls a stepper motor along a linear rail and optional WS2812 LED pixels, receiving both position and pixel data via DDP over WiFi — a standalone stepper + pixel controller with no separate LED controller needed on the prop.

Target hardware: Seeed XIAO ESP32-S3. The board was originally prototyped on the pin-compatible XIAO ESP32-C3, but the C3/C6 are single-core RISC-V parts and couldn't reliably run the stepper and WS2812 output together — that's why the project moved to the dual-core S3. See [TODO.md](TODO.md) for the work still needed to actually make use of the second core.

ArtNet support was deliberately removed (previously in `artnet_handler`/`ArtnetWiFiReceiver`) — DDP already scales to the project's ~500-pixel goal via its fragmented-packet handling, while ArtNet would have needed multi-universe support to get there, and wasn't worth the added surface area for this device. DDP is the only supported protocol now.

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

### Single-Core Design (current — not yet using the S3's second core)
- **Core 1 (Main)**: WiFi, web server, stepper control, DDP parsing, LED updates (main loop)
- FastLED.show() is called directly from the DDP handler for immediate LED updates
- The move to ESP32-S3 was meant to split stepper/protocol handling from WS2812 output across both cores, since `FastLED.show()` blocks its calling core for the WS2812 transmission time (~30µs/pixel). That split has never been implemented — everything still runs serially in `loop()` on one core. Tracked in [TODO.md](TODO.md).

### Key Data Flow
```
DDP packet → ddp_handler → positionRequest (shared variable) → main loop → stepper movement
                         → updatePixelLeds() → FastLED.show()
```

### Channel Layout
- Stepper control: byte offset 0 (8-bit) or bytes 0-1 (16-bit MSB-first) when enabled
- LED data: Always starts at byte offset 2 (channel 3) for RGB pixel alignment
- This ensures LEDs always begin on an RGB boundary regardless of stepper mode, or even if stepper control is disabled entirely

### Module Responsibilities

| Module | Purpose |
|--------|---------|
| `protocol_common` | Shared protocol settings and `positionRequest` variable |
| `ddp_handler` | DDP receiver on port 4048, handles fragmented packets (the only supported protocol) |
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
| D4 | WS2812 LED Data |
| D8 | Status LED |
| D10 | Homing Switch |

## Web Interface

HTML is embedded in `include/html.h` as a raw string literal with template placeholders (`{{VARIABLE}}`). JavaScript fetches `/status-data` for live updates and `/led-preview` separately (optional, saves bandwidth). Source lives in `web/` — see [WEB_DEVELOPMENT.md](WEB_DEVELOPMENT.md); never hand-edit `include/html.h`.

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
- `n` - Print detailed network/system diagnostics

## Important Patterns

- The DDP handler updates `lastProtocolUpdateTime` for blank timeout tracking
- OTA sets `otaInProgress` flag to pause protocol handling during updates
- LED updates are called directly from the DDP handler (no queue/task)
- WiFi connection monitoring runs every 30s with auto-reconnect
- Use `extern` declarations in headers, definitions in `.cpp` files
