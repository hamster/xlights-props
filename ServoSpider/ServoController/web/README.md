# Web Interface Source Files

This directory contains the editable source files for the ServoController web interface.

## Files

- **index.html** - HTML structure and layout
- **style.css** - All CSS styles and theming
- **script.js** - All JavaScript functionality

## Quick Start

### 1. Edit Files

Edit any of the three files in this directory using your favorite editor. Your IDE will provide full syntax highlighting and autocomplete.

### 2. Preview Changes

Open `index.html` in your browser to see your changes immediately:

```bash
# Windows
start index.html

# macOS
open index.html

# Linux
xdg-open index.html
```

**Note**: Some features like AJAX status updates won't work in the preview, but you can see layout and styling.

### 3. Build and Flash

When ready to test on the ESP32:

```bash
# Build (build_web.py runs automatically and combines files into html.h)
pio run --target upload
```

The build system automatically:
1. Reads these three files
2. Inlines CSS and JavaScript into the HTML
3. Generates `include/html.h` with proper C++ format
4. Compiles and uploads to the ESP32

## How It Works

During the PlatformIO build process:
- `build_web.py` runs as a pre-build script
- Reads `web/index.html`, `web/style.css`, `web/script.js`
- Replaces `<link rel="stylesheet" href="style.css">` with inlined `<style>` tag
- Replaces `<script src="script.js"></script>` with inlined `<script>` tag
- Generates `include/html.h` as a C++ raw string literal

## Template Variables

The HTML uses template variables replaced by the ESP32 at runtime:

- `{{HOSTNAME}}` - Device hostname
- `{{STATUS_CLASS}}` - WiFi status class
- `{{WIFI_IP}}` - IP address
- And many more...

These get replaced when the ESP32 serves the page.

## Important

**DO NOT** edit `include/html.h` directly - it's auto-generated!

Always edit the files in this `web/` directory instead.

## See Also

Read [WEB_DEVELOPMENT.md](../WEB_DEVELOPMENT.md) in the project root for complete documentation.
