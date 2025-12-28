Import("env")
import os
import shutil
import re

# Extract version from version.h
def get_version():
    version_file = os.path.join(env.get("PROJECT_DIR"), "include", "version.h")

    if not os.path.exists(version_file):
        print("Warning: version.h not found, using version 0.0.0")
        return "0.0.0"

    major = minor = subrev = "0"

    with open(version_file, 'r') as f:
        content = f.read()

        # Extract VERSION_MAJOR
        match = re.search(r'#define\s+VERSION_MAJOR\s+(\d+)', content)
        if match:
            major = match.group(1)

        # Extract VERSION_MINOR
        match = re.search(r'#define\s+VERSION_MINOR\s+(\d+)', content)
        if match:
            minor = match.group(1)

        # Extract VERSION_SUBREV
        match = re.search(r'#define\s+VERSION_SUBREV\s+(\d+)', content)
        if match:
            subrev = match.group(1)

    return f"{major}.{minor}.{subrev}"

def copy_firmware_after_build(source, target, env):
    # Get version
    version = get_version()

    # Get project directory
    project_dir = env.get("PROJECT_DIR")

    # Create build directory at top level
    build_dir = os.path.join(project_dir, "build")
    if not os.path.exists(build_dir):
        os.makedirs(build_dir)
        print(f"Created build directory: {build_dir}")

    # Source firmware file
    firmware_source = str(target[0])

    # Destination filename with version
    firmware_name = f"ServoController-{version}.bin"
    firmware_dest = os.path.join(build_dir, firmware_name)

    # Copy the firmware
    try:
        shutil.copy2(firmware_source, firmware_dest)
        print(f"✓ Firmware copied to: {firmware_dest}")
        print(f"  Version: {version}")

        # Get file size
        file_size = os.path.getsize(firmware_dest)
        print(f"  Size: {file_size:,} bytes ({file_size/1024:.1f} KB)")
    except Exception as e:
        print(f"Error copying firmware: {e}")

# Register the callback
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware_after_build)
