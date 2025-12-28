# ServoController

This project aims to allow you to control a prop along a linear rail from xlights.  It can listen to ArtNet (DMX) commands or DDP commands.  The servo is addressed as a single 8 bit channel, with values from 0-255 to coorespond to 0-100% of travel.

It is expected to be used with the Pixel Cat servo controller

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
   - Default Password: `Spiders1234`

2. **Configure WiFi**
   - Navigate to `http://192.168.4.1`
   - Go to Settings tab → WiFi Client Settings
   - Enter your network credentials
   - Click "Save WiFi Settings" then "Connect Now"
   - Most likely, you will be 

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

The controller uses a non-blocking state machine for homing.  The hardware design is a single wheel with a rope running to the prop trolley.  We attempt to wind the wheel so that we find both 'ends', allowing the rope to wrap around backwards.  The middle point is then the 'bottom', and actual usage of the controller will only wind the rope in one direction.

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
