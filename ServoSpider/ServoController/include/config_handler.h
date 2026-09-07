#ifndef CONFIG_HANDLER_H
#define CONFIG_HANDLER_H

// Full persisted-configuration export/import, added 2026-09-07 at the
// user's request ("make it easier to mess with the config") - distinct
// from tuning_handler.cpp's GET /tunable, which only covers the RAM-only
// bench tunables (PID gains, trackAccel, etc. - never persisted). This
// covers everything actually stored in NVS (Preferences): WiFi, AP,
// stepper, protocol/channel, TMC2209, and LED settings - see CLAUDE.md's
// Configuration Storage section for the underlying key list.
//
// Deliberately NOT JSON - a flat "key=value" line format instead (same on
// read and write), needing no parsing library: trivial to hand-edit, and
// the GET response can be POSTed straight back unmodified for a true
// round trip.
//
//   GET /config            - one "key=value" line per persisted setting.
//                             The WiFi/AP passwords are NEVER included
//                             here (they're write-only, like the
//                             existing WiFi settings form).
//   POST /config            - body is the same key=value format, plain
//                             text. Partial: only keys present are
//                             changed, everything else is left exactly
//                             as it was - this is how the password
//                             fields work (optional on write, never
//                             clobbered by omitting them) and also just
//                             a sane general default for hand-edited
//                             partial configs. Unrecognized keys are
//                             ignored (reported back in the response,
//                             not silently dropped). Stepper-related
//                             keys go through the existing deferred-save
//                             mechanism (stepperSettingsPendingSave) so a
//                             flash write can't land mid-move; everything
//                             else writes immediately, matching each
//                             domain's existing save-handler precedent.
//                             A reboot is recommended afterward for
//                             settings whose live effect depends on
//                             re-initialization (WiFi, TMC UART, LEDs) -
//                             this endpoint updates NVS and the in-RAM
//                             values but doesn't replicate every
//                             existing handler's own re-init dance.
void handleConfigGet();
void handleConfigPost();

#endif
