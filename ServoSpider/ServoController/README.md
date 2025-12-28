# ServoController

ESP32-C3 based stepper motor controller with web interface, designed for DMX lighting control integration via ArtNet and DDP protocols.

## Features

### Hardware Control
- **FastAccelStepper Integration** - High-performance stepper motor control with acceleration profiles
- **Automated Homing** - Non-blocking homing routine with limit switch detection
- **Position Tracking** - Absolute positioning with percentage-based control
- **Jump Start Support** - Configurable jump start for high-torque applications

### Network Protocols
- **ArtNet (DMX over Ethernet)** - Industry-standard lighting control protocol
- **DDP (Distributed Display Protocol)** - UDP-based pixel/servo control
- **WiFi Client & Access Point** - Dual-mode operation with automatic failover
- **Static IP Support** - Optional static IP configuration for reliable network addressing

### Web Interface
- **Modern Responsive UI** - Clean, mobile-friendly interface
- **Real-time Status Updates** - AJAX-based live monitoring (1Hz update rate)
- **OTA Firmware Updates** - Over-the-air updates with 4KB buffered writes
- **Configuration Management** - Web-based settings for all parameters
- **Locate Mode** - SOS LED pattern for device identification

### Safety & Reliability
- **Watchdog Timer** - 10-second timeout protection
- **WiFi Recovery** - Automatic reconnection and power management
- **Partition Table Diagnostics** - Boot-time partition health checks
- **Version Tracking** - Auto-incrementing build versioning

## Hardware Requirements

- **Microcontroller**: Seeed XIAO ESP32-C3
- **Stepper Driver**: Compatible with FastAccelStepper library
- **Power**: Suitable for your stepper motor requirements
- **Homing Switch**: NO/NC limit switch for position calibration

### Pin Configuration

| Pin | Function |
|-----|----------|
| D1  | Stepper Step |
| D2  | Stepper Direction |
| D3  | Stepper Enable |
| D8  | Status LED |
| D10 | Homing Switch |

## Software Requirements

- **PlatformIO** - Build system and IDE
- **ESP32 Arduino Core** - Framework
- **Libraries**:
  - FastAccelStepper (^0.33.9)
  - ArtnetWiFi (^0.4.1)

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
   - Default Password: `12345678`

2. **Configure WiFi**
   - Navigate to `http://192.168.4.1`
   - Go to Settings tab → WiFi Client Settings
   - Enter your network credentials
   - Click "Save WiFi Settings" then "Connect Now"

3. **Configure Motor**
   - Navigate to Settings tab → Stepper Configuration
   - Set appropriate speed, acceleration, and jump start values
   - Enable "Auto Home on Bootup" if desired

4. **Configure Protocols**
   - **ArtNet**: Set Universe and Channel (1-based)
   - **DDP**: Set Servo Channel (1-based, typically 1-512)

## Usage

### Web Interface

Access the device via its IP address in a web browser:

**Status Tab**
- View real-time motor position and homing status
- Monitor network connectivity and uptime
- Track ArtNet/DDP packet statistics
- Manual motor control (position and incremental movement)

**Settings Tab**
- WiFi configuration (client and AP modes)
- Stepper motor parameters
- ArtNet/DDP protocol settings
- Firmware update (OTA)

### Control Methods

#### ArtNet Control
- Protocol: ArtNet (DMX over Ethernet)
- Port: 6454 UDP
- Channel value: 0-255 maps to 0-100% of motor range
- Requires homing before accepting commands

#### DDP Control
- Protocol: DDP (Distributed Display Protocol)
- Port: 4048 UDP
- Channel value: 0-255 maps to 0-100% of motor range
- Requires homing before accepting commands

#### Web Interface Control
- Direct position control (steps or percentage)
- Incremental movement (forward/backward)
- Manual homing trigger

### Homing Process

The controller uses a non-blocking state machine for homing:

1. **Initial Check** - Verifies switch state at startup
2. **Clear Switch** - Moves off switch if initially triggered
3. **Find Initial Position** - Moves backward until switch triggers
4. **Find Opposite End** - Moves forward until switch triggers again
5. **Calculate Center** - Determines midpoint as "bottom position"
6. **Return to Zero** - Moves to initial switch position

**Homing States**:
- Auto-homing on boot (if enabled)
- Manual homing via web interface
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
- **Auto Home on Boot**: Enable/disable automatic homing

### Protocol Settings

**ArtNet**
- Universe: 0-32767
- Channel: 1-512 (1-based indexing)
- Enable/Disable toggle
- Debug mode

**DDP**
- Servo Channel: 1-512 (1-based indexing)
- Enable/Disable toggle
- Debug mode

## Building Firmware

The project uses automated version management and build artifact generation:

### Version Management
- **MAJOR.MINOR.SUBREV** format (e.g., 1.0.82)
- SUBREV auto-increments on each build
- Manually increment MAJOR/MINOR and reset SUBREV for releases

### Build Artifacts
Compiled firmware is automatically copied to:
```
build/ServoController-<version>.bin
```

Example: `build/ServoController-1.0.82.bin`

### Build Commands
```bash
# Standard build
pio run

# Build and upload
pio run --target upload

# Clean build
pio run --target clean
```

## Serial Commands

When connected via serial monitor (115200 baud):
- Type any character to print status information
- Status includes:
  - Connection status (WiFi/AP mode)
  - IP address and signal strength
  - Partition table
  - OTA partition information

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
- WiFi power management optimized during upload
- Automatic partition switching (app0 ↔ app1)
- Device reboots automatically on success

## Troubleshooting

### Device Won't Connect to WiFi
- Check credentials in serial output
- Verify signal strength (RSSI)
- Try static IP if DHCP fails
- Device automatically creates AP on connection failure

### Motor Not Responding
- Verify device is homed (check status page)
- Check "Homing Switch" indicator
- Ensure protocol is enabled (ArtNet/DDP)
- Verify correct Universe/Channel configuration

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
├── include/              # Header files
│   ├── version.h        # Version definitions
│   ├── html.h           # Web interface
│   ├── main.h           # Main declarations
│   ├── *_handler.h      # Protocol handlers
│   └── partition_utils.h
├── src/                 # Source files
│   ├── main.cpp         # Main program
│   ├── *_handler.cpp    # Protocol handlers
│   └── partition_utils.cpp
├── increment_version.py # Pre-build script
├── copy_firmware.py     # Post-build script
├── partitions.csv       # Partition table
├── platformio.ini       # Build configuration
└── README.md           # This file
```

### Adding New Features
1. Update version.h (MAJOR or MINOR) if significant change
2. Implement feature in appropriate handler file
3. Update web interface in html.h if needed
4. Build and test thoroughly before deployment
5. Document changes in commit messages

### Code Style
- Use descriptive variable names
- Comment complex logic
- Follow existing patterns for consistency
- Keep functions focused and modular

## Technical Specifications

- **CPU**: ESP32-C3 @ 160MHz (single-core RISC-V)
- **Flash**: Dual OTA partitions (1.9MB each)
- **RAM**: 400KB SRAM
- **WiFi**: 2.4GHz 802.11 b/g/n
- **Protocols**: ArtNet v4, DDP
- **Update Rate**: 1Hz status updates
- **Watchdog**: 10-second timeout
- **Serial Baud**: 115200

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
- Check serial output for diagnostic information
- Review partition table and OTA status

## Acknowledgments

- **FastAccelStepper** by gin66
- **ArtnetWiFi** by hideakitai
- ESP32 Arduino Core by Espressif
