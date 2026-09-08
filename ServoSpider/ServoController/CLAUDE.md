# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ServoController is an ESP32-based lighting prop controller for xLights. It controls a stepper motor along a linear rail and optional WS2812 LED pixels, receiving both position and pixel data via DDP over WiFi — a standalone stepper + pixel controller with no separate LED controller needed on the prop.

Target hardware: Seeed XIAO ESP32-S3. The board was originally prototyped on the pin-compatible XIAO ESP32-C3, but the C3/C6 are single-core RISC-V parts and couldn't reliably run the stepper and WS2812 output together — that's why the project moved to the dual-core S3. See [TODO.md](TODO.md) for the work still needed to actually make use of the second core.

ArtNet support was deliberately removed (previously in `artnet_handler`/`ArtnetWiFiReceiver`) — DDP already scales to the project's ~500-pixel goal via its fragmented-packet handling, while ArtNet would have needed multi-universe support to get there, and wasn't worth the added surface area for this device. DDP is the only supported protocol now.

The stepper driver is a TMC2209, with its UART link (RX/TX, separate from STEP/DIR/EN) always used for digital current control and driver diagnostics, and optionally for StallGuard-based jam detection — see `tmc_handler` below. This is independent of FastAccelStepper, which only ever drives STEP/DIR/EN and has no awareness of the driver chip.

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

### Dual-Core Split (LED work completed 2026-09-08 — see TODO.md's Priority 1 for the full design history)
- **Core 1 (Main)**: WiFi, web server, stepper control (including FastAccelStepper's own background task, explicitly pinned via `engine.init(1)`), DDP parsing, TMC UART, serial commands
- **Core 0**: a dedicated task (`core0_task.h`/`.cpp`) hosting both the rotary encoder's timer-poll (`encoder_handler.h`) and, as of 2026-09-08, every actual FastLED hardware call — `addLeds()`/`setBrightness()`/`setCorrection()`/`show()` (`led_handler.h`'s `initLedCore0()`/`serviceLedCore0()`) — so neither contends with FastAccelStepper's own Core-1-affine MCPWM+PCNT work. Core 1's pixel-write call sites (`ddp_handler.cpp`'s `updatePixelLedsFragmented()`, `led_handler.cpp`'s own `blankPixelLeds()`/`updateLedTestMode()`) still write directly into the shared `leds[]` array (no double-buffer — an accepted, self-correcting tradeoff, see TODO.md) and signal Core 0 via a binary semaphore (`signalLedShow()`) instead of calling `FastLED.show()` themselves; a config change instead calls `requestLedReinit()` instead of touching `FastLED.addLeds()` directly. `FastLED.addLeds()` itself moving to Core 0 (not just `show()`) matters because it's what binds the RMT completion interrupt to whichever core calls it.
- The move to ESP32-S3 was originally understood as splitting stepper/protocol handling from WS2812 output because `FastLED.show()` blocks its calling core for the WS2812 transmission time (~30µs/pixel) — checking the actual vendored driver source first suggested that's mostly not true on this hardware (async), but a closer check of the actual *compiled* driver (not just the source's `#if` guard) found the opposite: this toolchain's bundled framework builds FastLED against the legacy, more blocking-style IDF4 RMT driver, not the async IDF5 one (see TODO.md's Priority 1 section) — so the original blocking concern does hold here, and is exactly why moving `show()`'s call site off Core 1 was worth doing, alongside the explicit-core-allocation goal (nothing on Core 1 — stepper dispatch, DDP, web, TMC UART — contends with anything else).
- One known, benign side effect: `FastLED.addLeds()` (RMT driver install) takes on the order of ~300-400ms the first time it runs each boot — a real, measured one-time cost on Core 0's own task, self-contained (doesn't touch Core 1, and the encoder's own `encoderMissed` counter stays 0 through it), not a bug.

### Key Data Flow
```
DDP packet → ddp_handler → positionRequest (shared variable) → main loop → stepper movement
                         → updatePixelLedsFragmented() → leds[] write → signalLedShow() → (Core 0 task) → FastLED.show()
```

### Channel Layout
- Stepper control: byte offset 0 (8-bit) or bytes 0-1 (16-bit MSB-first) when enabled
- LED data: Always starts at byte offset 2 (channel 3) for RGB pixel alignment
- This ensures LEDs always begin on an RGB boundary regardless of stepper mode, or even if stepper control is disabled entirely

### Stepper Tracking Modes
`StepperTrackMode` (`stepper_handler.h`) selects how DDP position updates become stepper motion: Direct (`moveTo()` on every packet — simple, and the compiled-default fallback for any device that's never saved a `trackMode`) and PID. Three intermediate strategies - Coalesce, Streaming (shelved — unstable, not recommended), and Lookahead - were built and bench-compared against Direct earlier in the project (Coalesce performed nearly identically to Direct; Streaming was found unstable; Lookahead was never actually bench-tested), then **removed entirely** 2026-09-07 ("we just have Direct mode and PID") once PID proved out as the real answer to the tracking-jerkiness question - no code, UI, NVS keys, or `$SET`/`GET /tunable` names for any of the three remain; a device with an old saved `trackMode=1/2/3` falls back to Direct at boot with a logged warning. See TODO.md for the full history if this decision ever needs revisiting. **PID (`trackMode=4`) is the mode under active tuning, and is now persisted as the real production default on the bench device** (saved via `POST /config`, verified surviving a genuine reboot — see TODO.md's Wave 3 writeup): closed-loop control where a velocity feedforward (self-measures the real rate `positionRequest` has been changing at via a least-squares slope over a fixed window) carries the bulk of the commanded motion, with Kp/Kd trimming only the residual error. The control tick itself is configurable (`pidTickMsConfig`, default 5ms, decoupled from Compact Motion Log rate via `pidLogMsConfig`) — raising it turned out to be the real lever against a persistent speed ripple, because that ripple is a sampled-control-loop dynamic (the loop's own sample delay), not a noise source any output filter could remove. Deliberately does not read the encoder (bench-only ground truth for verifying tuning, never fed back into control). Current tuning trades some moment-to-moment smoothness for materially tighter tracking/corner accuracy versus Direct mode — matches the user's own priority: a real prop can tolerate a few frames of lag but must never look jerky. Full control law: `TRACK_MODE_PID`'s enum comment (`stepper_handler.h`). Full multi-session tuning history (gain sweeps, the feedforward design, the tick-rate investigation): `TODO.md`. Note the web UI still has no PID-specific tuning fields (Kp/Kd/etc. stay `$SET`/serial-only) — building that Settings section is a still-open Wave 4 item.

Direct mode's own "Small-Move Tracking" (a gentler speed/accel profile for small position updates, `stepperTrackEnabledConfig` + threshold/max-lag/speed/accel) is unrelated to PID's control law and still genuinely live/default-on for Direct - PID replaces the need for it with its own closed loop instead. Its tuning fields live behind a "Small-Move Tracking Settings" collapsible in Stepper Configuration now (Wave 4), collapsed by default since Direct-mode bench work needs them far less often than the top-level speed/accel/mode fields.

### Module Responsibilities

| Module | Purpose |
|--------|---------|
| `protocol_common` | Shared protocol settings and `positionRequest` variable |
| `ddp_handler` | DDP receiver on port 4048, handles fragmented packets (the only supported protocol) |
| `led_handler` | FastLED direct updates, gamma correction, color order mapping |
| `stepper_handler` | FastAccelStepper control, non-blocking homing state machine |
| `tmc_handler` | TMC2209 UART link (current control, StallGuard cutoff, diagnostics) - always enabled as of 2026-09-07 (was opt-in; no Settings checkbox left, since this project has only ever run one board/wiring that always has it). SpreadCycle only - StealthChop was tried and dropped 2026-09-07 (dramatically insufficient torque at this project's normal operating settings, not a subtle edge case) |
| `wifi_handler` | WiFi client/AP modes, captive portal DNS, mDNS |
| `html_handler` | Web API endpoints, serves HTML from `html.h` |
| `ota_handler` | HTTP OTA firmware updates with chunked upload |
| `persist_log` | Reboot-surviving diagnostic log (SPIFFS-backed) - logs each boot's `esp_reset_reason()` automatically, plus manual breadcrumbs (homing search progress, TMC stall events). RAM-buffered; only flushes to flash when the stepper is confirmed idle (a real flash write during active stepping crashed the device once - see TODO.md). Retrieval: `GET /persist-log` (`?clear=1` to wipe), not serial - checking it shouldn't itself trigger the reset it's explaining |

### Homing State Machine
The trolley hangs on a rope wound onto a pulley the stepper drives through a worm gear - winding pulls it **up**, letting rope out lets gravity pull it **down**. There is exactly **one** homing switch, at the **top** of the stroke only (no second switch at the bottom). Homing lets the trolley down until that switch trips, then keeps turning the pulley the *same* direction - once the rope is fully paid out it starts winding back on from the other side, pulling the trolley back up until it trips the *same* switch a second time. Halving that second trigger's step count gives `bottomPosition` (the real bottom of the rod), since the down-then-up trip is symmetric around it - no second switch or separate "find the bottom" step needed. Full sequence lives in the `HomingState` enum/`updateHoming()` (`stepper_handler.h`/`.cpp`); call `updateHoming()` every loop iteration. See README.md's "Homing Process" section for the numbered state list.

### Configuration Storage
All settings stored in ESP32 Preferences (NVS flash):
- `wifi-config` namespace
- Keys: `ssid`, `password`, `hostname`, `wifiRetryInt`, `protocol`, `stepperControl`, `control16Bit`, `stepSpeedHome`, `stepAccelHome`, `stepBlankTime`, `tmcRunCurrent`, `tmcHoldPercent`, `tmcStallEnabled`, `tmcStallThresh`, `tmcMicrosteps`, etc. All ≤15 chars (see NVS note below). `jumpStart`, `tmcEnabled`, `tmcRSense`, `tmcAddress`, `tmcHstrt`, `tmcHend` were removed 2026-09-07 (Wave 4) - all six are now hardcoded in firmware, not NVS-backed; any of those keys already sitting in an existing device's flash is now just an orphaned, unread value.

## Pin Configuration

| Pin | Function |
|-----|----------|
| D1 | Stepper Step |
| D2 | Stepper Direction |
| D3 | Stepper Enable |
| D4 | WS2812 LED Data |
| D6 | TMC2209 UART TX |
| D7 | TMC2209 UART RX |
| D8 | Status LED |
| D10 | Homing Switch |

## Web Interface

HTML is embedded in `include/html.h` as a raw string literal with template placeholders (`{{VARIABLE}}`). JavaScript fetches `/status-data` for live updates and `/led-preview` separately (optional, saves bandwidth). Source lives in `web/` — see [WEB_DEVELOPMENT.md](WEB_DEVELOPMENT.md); never hand-edit `include/html.h`.

## Serial Commands

Type in serial monitor (115200 baud):
- `?` - Help menu
- `h` - Start homing
- `r` - Reboot
- `s` - Print status (full diagnostics: WiFi, memory, DDP, LED, TMC2209, uptime)
- `w` - Reconnect WiFi
- `a` - Switch to AP mode
- `p` - Print stepper position
- `f`/`b` - Move forward/backward 10 steps
- `n` - Print connection status (brief)

A separate `$`-prefixed extended protocol (`tuning_handler.h`/`.cpp`) exists alongside these single-character commands, for the bench-tuning harness (`tools/tuning_harness.py`): `$SET`/`$GET`/`$GET ALL`/`$STATUS`/`$HOME`/`$CHECKSTEPS`. See `tuning_handler.h` and `tools/README.md` for the full protocol.

## Important Patterns

- The DDP handler updates `lastProtocolUpdateTime` for blank timeout tracking
- OTA sets `otaInProgress` flag to pause protocol handling during updates
- LED updates are called directly from the DDP handler (no queue/task)
- WiFi connection monitoring runs every 30s with auto-reconnect
- `Preferences` (NVS) key names are capped at 15 characters - a longer key fails to write silently (no compiler or runtime error), so the setting just never persists across a reboot. Keep new keys ≤15 chars. Found and fixed two real instances of this: `stepSpeedHome`/`stepAccelHome` (originally `stepperSpeedHoming`/`stepperAccelHoming`) and `stepBlankTime` (originally `stepperBlankTime`, a pre-existing bug unrelated to this session) - see TODO.md.
- Every `*Config` variable loads as `preferences.getInt(key, compiledDefault)` (or `getBool`) at boot - **a value already saved in NVS always wins over the compiled default**, on this device and any other that's ever had that key written. Changing a compiled default (e.g. `stepperAccelHoming` in `stepper_handler.h`) only helps a device that has never saved that key - it silently does nothing for a device that already has an explicit (possibly stale/bad) value in flash. Confirmed the hard way: lowering `stepperAccelHoming`'s compiled default to fix a real motor-stall bug (see TODO.md) had no effect on the bench device until the corrected value was actually POSTed to `/save-stepper` and verified via a real reboot that `$GET`/`$GET ALL` now reported it from flash. When fixing a bad default that bites real hardware, also push the corrected value to any already-provisioned device (web UI save, or a direct `/save-stepper` POST) - don't assume a firmware update alone propagates it.
- The homing-switch ISR (`handleHomingInterrupt`) only ever sets flags, never calls into non-`IRAM_ATTR` code (e.g. `FastAccelStepper::forceStop()`, which lives in regular cached flash) - doing so can crash if the ISR fires while flash cache is briefly disabled by a `Preferences.putX()` write elsewhere. `updateHoming()` (always normal task context) consumes those flags instead. Two separate flags on purpose: `pendingForceStop` is consumed exactly once per edge (mirrors the original synchronous-stop behavior); `interruptTriggered` is a level flag only the specific `HOMING_*` state waiting for that edge clears. Don't collapse these into one flag with a naive `if (flag) forceStop()` at the top of `updateHoming()` - see TODO.md for the regression that caused. **`pendingForceStop` must be checked/cleared unconditionally, before any "not currently homing" early return** - gating it behind homing-state caused a second bug (also in TODO.md): a switch trip during normal operation left it stale until the *next* homing attempt, where it fired as an unrelated `forceStop()` racing the new attempt's first move command.
- Use `extern` declarations in headers, definitions in `.cpp` files
