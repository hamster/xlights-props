#ifndef TUNING_HANDLER_H
#define TUNING_HANDLER_H

#include <Arduino.h>

// Extended serial command protocol for the Python tuning harness
// (tools/tuning_harness.py). Lets a script configure motion parameters and
// query status over the same serial connection it reads the Compact
// Motion Log from (see compactLogEnabled in protocol_common.h), so a
// tuning run doesn't depend on WiFi/HTTP being part of the loop at all.
//
// Wire protocol: a line beginning with '$' is treated as an extended
// command instead of the normal single-character serial commands ('h',
// 'r', 's', etc., unchanged and still available). Everything after '$' up
// to '\n' is one command:
//
//   $SET <name> <value>   - sets a tunable live in RAM only (no flash
//                            write - this is for fast iterative tuning,
//                            not persisting a final choice; use the web
//                            UI's Stepper Configuration to actually save
//                            settings once you're happy with them).
//                            Responds "OK <name>=<value>" or an "ERR ..." line.
//   $GET <name>           - responds "VAL <name>=<value>" or an "ERR ..." line.
//   $GET ALL              - one "VAL name=value" line per tunable, then "OK ALL".
//   $STATUS               - one line: "STATUS homed=0/1 homing=0/1
//                            checking=0/1 pos=<int> bottom=<int>
//                            running=0/1 mode=0/1/2/3".
//   $HOME                 - triggers startHoming(); responds "OK HOME".
//   $CHECKSTEPS [target]  - ground-truth skipped-step check: commands a
//                            direct move to `target` (default 0, the
//                            initial-homing switch reference), bypassing
//                            DDP/tracking-mode entirely, and watches whether
//                            the physical homing switch fires *before* the
//                            step counter gets there - the only way to
//                            actually observe lost steps, since
//                            getCurrentPosition() is pulse-counting
//                            bookkeeping, not a position sensor. Requires
//                            homed=1 and the stepper idle first, or replies
//                            "ERR ...". Immediately replies
//                            "OK CHECKSTEPS started target=<n>", then - once
//                            the move finishes or the switch trips early -
//                            an asynchronous result line:
//                              CHECKSTEPS_RESULT tripped=1 tripPos=<n>
//                                target=<n> elapsedMs=<n>
//                            (switch fired early - tripPos is how many steps
//                            short of target it was, i.e. roughly how many
//                            steps were lost since the last homing/check),
//                            or:
//                              CHECKSTEPS_RESULT tripped=0 tripPos=<n>
//                                target=<n> elapsedMs=<n>
//                            (reached target cleanly with no early trip -
//                            tripPos here is just the final position, which
//                            should equal target). A trip also marks the
//                            system not-homed, same as any unexpected trip
//                            outside of homing - re-run $HOME before relying
//                            on position again.
//   $ENCDIAG               - encoder diagnostic sweep: repeats a
//                            0%->100%->0% cycle 8 times at each of four
//                            stepper speeds (3000/4000/5000/6500 Hz),
//                            direct moveTo() calls bypassing DDP/tracking-
//                            mode, same as $CHECKSTEPS. Requires homed=1 and
//                            the stepper idle first, or replies "ERR ...".
//                            Prints "ENCDIAG_START", then a CSV header
//                            (freqHz,run,phase,stepperPos,encoderCount,
//                            missedTotal) followed by one row per leg (16
//                            legs per frequency x 4 = 64 rows total: phase
//                            100 or 0 marks which end of that leg was just
//                            reached), then "ENCDIAG_DONE" once the whole
//                            sweep finishes and the stepper's original
//                            speed/accel are restored. Takes a few minutes
//                            end to end - it's a full-range move x 64.
//
// Tunable names (see tools/README.md for the matching Python-side names;
// this list has drifted stale before - ALL_TUNABLE_NAMES in
// tuning_handler.cpp is the actual source of truth):
//   normalSpeed, normalAccel, trackEnabled, trackThreshold,
//   trackSpeed, trackAccel, trackMaxLag, trackMode, compactLog, protocolDebug,
//   homeSpeed, homeAccel, pidKp,
//   pidKd, pidDFilterWeight, pidMaxSpeed, pidAccel, pidDeadband,
//   pidReengageThreshold, pidFeedforward, pidFfWindowMs, pidLookaheadMs,
//   pidTickMs, pidLogMs, tmcRunCurrent, tmcStallEnabled, positionRequest,
//   ddpRxLog, ddpAck
void handleExtendedSerialCommand(const String& line);

// HTTP equivalent of $SET/$GET above - see its own declaration comment in
// tuning_handler.cpp. GET /tunable?name=<n>[&value=<v>] or name=ALL.
void handleTunableHttp();

#endif
