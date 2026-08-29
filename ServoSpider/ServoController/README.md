# ServoController

This project drives an xLights prop that moves along a linear rail and (optionally) carries its own WS2812 pixels — a stepper mover and a pixel controller combined into one standalone ESP32 device. It listens for DDP commands over WiFi: one byte (or two, in 16-bit mode) sets the rail position, and the remaining bytes drive the pixel string directly. No separate pixel controller is needed on the prop.

It is expected to be used with the Pixel Cat servo controller (Seeed XIAO ESP32-S3).

DDP is the only supported protocol — ArtNet was deliberately dropped (see [TODO.md](TODO.md) for why). See TODO.md for the current roadmap and known gaps (notably: the stepper/LED core split is not yet done).

### Pin Configuration

| Pin | Function |
|-----|----------|
| D1  | Stepper Step |
| D2  | Stepper Direction |
| D3  | Stepper Enable |
| D4  | WS2812 LED Data |
| D6  | TMC2209 UART TX |
| D7  | TMC2209 UART RX |
| D8  | Status LED |
| D10 | Homing Switch |

## Software Requirements

- **PlatformIO** - Build system and IDE
- **ESP32 Arduino Core** - Framework
- **Libraries**:
  - FastAccelStepper (^1.2.7)
  - FastLED (^3.7.0)
  - TMCStepper (^0.7.3) — optional TMC2209 UART driver control

## Installation

1. **Clone the repository**
   ```bash
   git clone <repository-url>
   cd ServoController
   ```

2. **Open in PlatformIO**
   - Open the project folder in VS Code with PlatformIO extension
   - Or use PlatformIO CLI: `pio run`

3. **Build and Upload**
   ```bash
   pio run --target upload
   ```

4. **Monitor Serial Output**
   ```bash
   pio device monitor
   ```

## First-Time Setup

1. **Connect to AP**
   - Default SSID: `ServoController-XXXX` (where XXXX is MAC address)
   - Default Password: `Spiders1234`

2. **Configure WiFi**
   - Navigate to `http://192.168.4.1`
   - Go to Settings tab → WiFi Client Settings
   - Enter your network credentials
   - Click "Save WiFi Settings" then "Connect Now"

3. **Configure Motor**
   - Navigate to Settings tab → Stepper Configuration
   - Set appropriate speed, acceleration, and jump start values
   - Enable "Auto Home on Bootup" if desired

4. **Configure Channels**
   - Navigate to Settings tab → Channel Configuration
   - Enable/disable stepper control and 16-bit mode as needed
   - Servo position is fixed at DDP byte 1 (or bytes 1-2 in 16-bit mode)

5. **Configure Pixels (optional)**
   - Navigate to Settings tab → LED Configuration
   - Set pixel count, color order, gamma, brightness, and any null/spacer pixels
   - Pixel data always starts at channel 3 (byte offset 2), leaving channel 2 unused as padding — this keeps the RGB boundary aligned regardless of 8-bit/16-bit stepper mode or whether stepper control is enabled at all

## Usage

### Web Interface

Access the device via its IP address (or `http://<hostname>.local`) in a web browser:

**Status Tab**
- View real-time motor position and homing status
- Monitor network connectivity and uptime
- Track DDP packet statistics
- Live LED preview (fetched separately from `/led-preview` to save bandwidth)
- Manual motor control (position and incremental movement)
- Locate mode (blinks the status LED in an SOS pattern to help find the device)

**Settings Tab**
- WiFi configuration (client and AP modes, DHCP or static IP)
- Stepper motor parameters
- Channel settings, including 8-bit/16-bit stepper mode
- LED/pixel configuration (count, color order, gamma, brightness, null pixels)
- Stepper driver (TMC2209 UART): current control, StealthChop/SpreadCycle, StallGuard jam-detection cutoff, diagnostics — optional, off by default
- Blank-time timeouts (turn off LEDs / return stepper to zero after N seconds without a command)
- Firmware update (OTA)
- Reset all settings to defaults

### Control Methods

#### DDP Control
- Protocol: DDP (Distributed Display Protocol)
- Port: 4048 UDP
- Stepper channel: byte 1 (8-bit) or bytes 1-2 MSB-first (16-bit) — 0-255/0-65535 maps to 0-100% of travel
- LED data: starts at byte 3, 3 bytes (RGB) per pixel
- Handles fragmented/multi-packet DDP data, so large pixel counts are supported without a universe-style limit
- Requires homing before accepting stepper commands (LEDs work regardless of homing state)

#### Web Interface Control
- Direct position control (steps or percentage)
- Incremental movement (forward/backward)
- Manual homing trigger

### Homing Process

The controller uses a non-blocking state machine for homing. The hardware design is a single wheel with a rope running to the prop trolley. We attempt to wind the wheel so that we find both 'ends', allowing the rope to wrap around backwards. The middle point is then the 'bottom', and actual usage of the controller will only wind the rope in one direction.

1. **Initial Check** - Verifies switch state at startup
2. **Clear Switch** - Moves off switch if initially triggered
3. **Find Initial Position** - Moves backward until switch triggers
4. **Find Opposite End** - Moves forward until switch triggers again
5. **Calculate Center** - Determines midpoint as "bottom position"
6. **Return to Zero** - Moves to initial switch position

**Homing States**:
- Auto-homing on boot (if enabled)
- Manual homing via web interface or `h` serial command
- Status indicator shows "Homing..." during process

## Configuration Options

### WiFi Settings
- **SSID/Password** - Network credentials
- **IP Mode** - DHCP or Static IP
- **AP Settings** - Custom AP name and password
- **MAC Append** - Optionally append MAC to AP name

### Stepper Settings
- **Speed** (Hz): 100-50,000 (default: 6,500)
- **Acceleration** (steps/s²): 0-1,000,000 (default: 20,000)
- **Jump Start** (steps): 0-10,000 (initial power boost)
- **Homing Speed** (Hz) / **Homing Acceleration** (steps/s²): separate from the values above, used only during the homing search (default: 6,000 Hz / 1,000,000 steps/s²). This is a step-pulse rate, not a physical speed — it doesn't automatically scale with the driver's microstep setting. If you change **Microsteps per Full Step** under the TMC2209 driver settings, re-tune this too: fewer microsteps means more physical distance per step, so the same Hz now moves faster and can overshoot the homing switch.
- **Auto Home on Boot**: Enable/disable automatic homing

### Channel Settings
- **Stepper Control**: Enable/disable stepper channel(s) entirely (pixel-only props don't need it)
- **16-bit Stepper Control**: Use two channels for finer position resolution
- **Debug mode**: Verbose serial logging of incoming DDP data
- **LED Blank Time**: Seconds of no DDP traffic before pixels turn off (0 = disabled)
- **Stepper Blank Time**: Seconds of no DDP traffic before the stepper returns to position 0 (0 = disabled)

### Stepper Driver Settings (TMC2209 UART, optional)
Requires the driver's UART (single-wire PDN_UART pad) wired to D6 (TX) / D7 (RX); leave "Enable UART Driver Control" off if it isn't. Off by default — enabling it does not change the motor's current/behavior until you also set Run Current, since the firmware won't apply digital current control until told to.
- **Run Current** (mA) / **Hold Current** (% of run) — replaces the board's physical Vref trimpot once enabled; the trimpot has no further effect after this is turned on
- **Stall Detection** — a firmware-side safety cutoff that watches the driver's live StallGuard reading (`SG_RESULT`) and force-stops the motor if it drops below a configured threshold while moving, e.g. a jammed rope. This needs bench tuning: watch the live `SG_RESULT` value on the Status tab under normal moves vs. a deliberately blocked one to pick a threshold, then enable the cutoff. It's independent of the physical homing switch and doesn't affect homing.
- **Advanced**:
  - **StealthChop / SpreadCycle** — quiet vs. higher-torque chopper mode
  - **Microsteps per Full Step** — 1 to 256 (default: 16). Changing this changes the physical distance covered per step, so **re-home afterward** and re-tune Homing Speed/Acceleration and normal Stepper Speed/Acceleration if movement feels too fast or slow.
  - **SpreadCycle Hysteresis Start/End** (`hstrt`/`hend`) — raw chopper tuning values, only affect SpreadCycle mode; default 0/0 (a conservative but valid starting point, not confirmed to need adjustment)
  - **StealthChop Autoscale Step Size/Amplitude Limit/Automatic Gradient Adaptation** (`pwm_reg`/`pwm_lim`/`pwm_autograd`) — governs StealthChop's self-tuning; only affect StealthChop mode
  - **Sense Resistor** / **Driver Address** — match your TMC2209 module's actual sense resistor (commonly 0.11Ω for SilentStepStick-style modules) and MS1/MS2 address strapping (0 for a single-driver setup, both pins grounded)

### LED/Pixel Settings
- **Pixel Count**: 0-1000 (typical props are 50-150; designed to comfortably handle up to ~500)
- **Color Order**: RGB, RBG, GRB, GBR, BRG, or BGR
- **Gamma**: 0.1-5.0 correction curve
- **Brightness**: 0-100%
- **Start/End Null Pixels**: Spacer pixels at the start/end of the physical strip that aren't driven by channel data

## Building Firmware

The project uses automated version management and build artifact generation:

### Version Management
- **MAJOR.MINOR.SUBREV** format (e.g., 1.0.230)
- SUBREV auto-increments on each build
- Manually increment MAJOR/MINOR and reset SUBREV for releases

### Build Artifacts
Compiled firmware is automatically copied to:
```
build/ServoController-<version>.bin
```

Example: `build/ServoController-1.0.230.bin`

### Build Commands
```bash
# Standard build
pio run

# Build and upload
pio run --target upload

# Clean build
pio run --target clean
```

### Editing the Web UI
The web interface source lives in `web/` (`index.html`, `style.css`, `script.js`) and is compiled into `include/html.h` automatically by `build_web.py` on every build. See [WEB_DEVELOPMENT.md](WEB_DEVELOPMENT.md) for the full workflow — never hand-edit `include/html.h` directly.

## Serial Commands

When connected via serial monitor (115200 baud), type `?` for the full menu:
- `?` - Help menu
- `h` - Start homing sequence
- `r` - Reboot device
- `s` - Print connection status
- `n` - Detailed network/system diagnostics (WiFi, memory, DDP stats, LED status, uptime)
- `w` - Attempt WiFi reconnection
- `a` - Switch to Access Point mode
- `p` - Print current stepper position
- `f`/`b` - Move forward/backward 10 steps

## LED Status Indicators

| Pattern | Meaning |
|---------|---------|
| Slow blink (1 Hz) | WiFi connected |
| Fast blink (5 Hz) | AP mode or connecting |
| SOS pattern | Locate mode active |

## OTA Updates

### Web-Based OTA
1. Navigate to Settings tab → OTA Update
2. Select `.bin` file from `build/` directory
3. Click "Update Firmware"
4. Wait for upload and reboot (~30 seconds)

### Technical Details
- 4KB buffered writes for optimal flash performance
- Automatic partition switching (app0 ↔ app1)
- Device reboots automatically on success
- DDP handling pauses during OTA to avoid interference

## Troubleshooting

### Device Won't Connect to WiFi
- Check credentials in serial output
- Verify signal strength (RSSI) — use `n` for detailed diagnostics
- Try static IP if DHCP fails
- Device automatically creates AP on connection failure

### Motor Not Responding
- Verify device is homed (check status page)
- Check "Homing Switch" indicator
- Ensure stepper control is enabled in Channel Configuration
- Verify DDP is reaching the device (check packet count on the status page)

### Motor Moves Too Fast/Slow, or Overshoots During Homing (TMC2209 UART enabled)
- If you changed **Microsteps per Full Step**, Homing Speed/Acceleration and normal Stepper Speed/Acceleration are step-pulse rates (Hz), not physical speeds — they don't automatically scale with microstep resolution. Re-tune them after any microstep change, and re-home afterward.
- If the motor barely moves after enabling UART Driver Control, check Run Current is set high enough for your motor's rated current — enabling digital current control replaces the board's physical Vref trimpot entirely, so whatever it was previously tuned to no longer applies.

### LEDs Not Responding
- Confirm Pixel Count is set > 0 and settings were saved
- Remember LED data starts at channel 3 (DDP byte offset 2) — check your xLights model's channel offset
- Use the Status tab's LED preview to confirm data is arriving even if the physical strip looks wrong (usually a color-order or wiring issue)

### OTA Upload Fails
- Check WiFi signal strength (need good RSSI)
- Ensure file is `.bin` format
- Try uploading smaller builds
- Check serial output for detailed error messages

### Homing Fails
- Verify homing switch is connected to D10
- Check switch polarity (HIGH = triggered)
- Ensure motor can reach both ends of travel
- Monitor serial output during homing process

## Development

### Project Structure
```
ServoController/
├── include/                # Header files
│   ├── version.h           # Version definitions
│   ├── html.h               # Generated web UI (DO NOT EDIT - see web/)
│   ├── main.h
│   ├── protocol_common.h    # Shared protocol/channel-layout state
│   ├── led_handler.h
│   ├── *_handler.h          # Other subsystem handlers (ddp, stepper, tmc, wifi, ota)
│   └── partition_utils.h
├── src/                     # Source files
│   ├── main.cpp              # Main program
│   ├── led_handler.cpp       # FastLED pixel updates, gamma/color-order
│   ├── *_handler.cpp         # Other subsystem handlers
│   └── partition_utils.cpp
├── web/                     # Editable web UI source (see WEB_DEVELOPMENT.md)
│   ├── index.html
│   ├── style.css
│   └── script.js
├── build_web.py             # Pre-build: compiles web/ into include/html.h
├── increment_version.py     # Pre-build: auto-increments SUBREV
├── copy_firmware.py         # Post-build: copies .bin into build/
├── platformio.ini           # Build configuration
├── TODO.md                  # Roadmap / open work
└── README.md                 # This file
```


## Version History

See [version.h](include/version.h) for current version.

Version format: `MAJOR.MINOR.SUBREV`
- **MAJOR**: Breaking changes, significant features
- **MINOR**: New features, non-breaking changes
- **SUBREV**: Bug fixes, small updates (auto-incremented)

## License

[Specify your license here]

## Support

For issues, questions, or contributions:
- Open an issue on GitHub
- Check serial output for diagnostic information (`n` for full diagnostics)
- Review partition table and OTA status
- See [TODO.md](TODO.md) for known gaps before assuming something is broken

## Acknowledgments

- **FastAccelStepper** by gin66
- **FastLED** by the FastLED community
- ESP32 Arduino Core by Espressif
