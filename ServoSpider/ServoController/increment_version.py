Import("env")
import os
import re

def increment_build_number(source, target, env):
    """Increment the SUBREV (build number) in version.h before each build"""

    version_file = os.path.join(env.get("PROJECT_DIR"), "include", "version.h")

    if not os.path.exists(version_file):
        print("Warning: version.h not found, skipping build number increment")
        return

    # Read the current version.h file
    with open(version_file, 'r') as f:
        content = f.read()

    # Find and increment VERSION_SUBREV
    pattern = r'(#define\s+VERSION_SUBREV\s+)(\d+)'
    match = re.search(pattern, content)

    if match:
        current_subrev = int(match.group(2))
        new_subrev = current_subrev + 1

        # Replace the old value with the new one
        new_content = re.sub(pattern, f'\\g<1>{new_subrev}', content)

        # Write the updated content back to the file
        with open(version_file, 'w') as f:
            f.write(new_content)

        print(f"Build number incremented: {current_subrev} -> {new_subrev}")
    else:
        print("Warning: VERSION_SUBREV not found in version.h")

# Register the pre-build action
env.AddPreAction("buildprog", increment_build_number)
env.AddPreAction("upload", increment_build_number)
