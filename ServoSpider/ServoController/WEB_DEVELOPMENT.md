# Web Development Workflow

The web interface is now split into separate, editable files for better development experience.

## File Structure

```
web/
  ├── index.html    - Main HTML structure (edit this!)
  ├── style.css     - All CSS styles (edit this!)
  └── script.js     - All JavaScript (edit this!)

include/
  └── html.h        - Generated file (DO NOT EDIT - auto-generated)
```

## Development Workflow

### 1. Edit the Web Files

Edit any of the three files in the `web/` directory:
- **web/index.html** - HTML structure and layout
- **web/style.css** - All styling and colors
- **web/script.js** - All JavaScript functionality

### 2. Preview Your Changes

Simply open `web/index.html` in your browser to see your changes immediately!

**Note**: Some features like AJAX status updates won't work in the preview, but you can see the layout and styling.

### 3. Build and Flash

When you're ready to test on the ESP32:

```bash
# Build - build_web.py runs automatically
pio run

# Or build and upload
pio run --target upload
```

The `build_web.py` script automatically:
1. Reads your three web files
2. Inlines the CSS and JavaScript into the HTML
3. Generates `include/html.h` with proper C++ format

### 4. Manual Rebuild (Optional)

If you want to regenerate `html.h` without doing a full build:

```bash
python test_build_web.py
```

## How It Works

### Build Process

The build is configured in [platformio.ini](platformio.ini):

```ini
extra_scripts =
	pre:build_web.py        # Runs before build
	pre:increment_version.py
	post:copy_firmware.py
```

### build_web.py

This script:
1. Reads `web/index.html`, `web/style.css`, `web/script.js`
2. Replaces `<link rel="stylesheet" href="style.css">` with inlined `<style>` tag
3. Replaces `<script src="script.js"></script>` with inlined `<script>` tag
4. Wraps everything in a C++ raw string literal
5. Writes to `include/html.h`

## Template Variables

The HTML uses template variables that get replaced at runtime:

- `{{HOSTNAME}}` - Device hostname
- `{{STATUS_CLASS}}` - WiFi connection status class
- `{{WIFI_IP}}` - Current IP address
- And many more...

These are replaced by the ESP32 web server when serving the page.

## Tips

### Color Scheme

Current dark theme colors:
- Background gradient: `#0f1929` to `#1a2332`
- Content boxes: `#1a2332`
- Accent color: `#00d4ff` (cyan)
- Text: `#e0e0e0`, `#b0c0d0`, `#c0c0c0`
- Inputs: `#0f1929` background, `#2a3a4a` borders

### Responsive Design

The layout uses CSS Grid for desktop (min-width: 1024px) and stacks on mobile.

### JavaScript API Endpoints

The script.js uses these endpoints:
- `/status-data` - Main status JSON
- `/led-preview` - Optional LED preview data
- `/set-wifi` - WiFi configuration
- `/set-stepper` - Stepper settings
- `/ota-update` - Firmware upload

## Troubleshooting

### Build fails with "html.h not found"

Run the test build script:
```bash
python test_build_web.py
```

### Changes not appearing on ESP32

1. Make sure you edited files in `web/` directory (not `include/html.h`)
2. Rebuild the project to regenerate `html.h`
3. Flash the new firmware to ESP32

### Can't preview in browser

Just open `web/index.html` directly in any browser. Some dynamic features won't work, but layout and styling will be visible.
