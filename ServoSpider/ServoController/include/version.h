#ifndef VERSION_H
#define VERSION_H

// Version information
// Format: MAJOR.MINOR.SUBREV
// - MAJOR: Significant changes, breaking changes (manually increment)
// - MINOR: New features, non-breaking changes (manually increment)
// - SUBREV: Bug fixes, small updates (AUTO-INCREMENTED on each build)
//
// NOTE: VERSION_SUBREV is automatically incremented by increment_version.py
//       before each build. Do not manually edit unless resetting the counter.
//       To reset, set to 0. To manually increment MAJOR or MINOR, also reset SUBREV to 0.

#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define VERSION_SUBREV 321

// Helper macros to convert version numbers to strings
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// Full version string
#define VERSION_STRING TOSTRING(VERSION_MAJOR) "." TOSTRING(VERSION_MINOR) "." TOSTRING(VERSION_SUBREV)

// Build date and time (automatically set at compile time)
#define BUILD_DATE __DATE__
#define BUILD_TIME __TIME__

#endif
