# Tuning tools

`tuning_harness.py` automates bench-testing the stepper tracking modes
(`StepperTrackMode` in `stepper_handler.h`): it configures the device over
serial, drives the trolley with a synthetic DDP triangle wave, captures the
Compact Motion Log, and produces plots/metrics - so a tuning iteration is
"edit a config file, run one command, look at a plot" instead of manual
button-pushing and eyeballing a serial terminal.

For a worked example of using this harness for a real tuning session -
findings, comparison tables, graphs, bugs found along the way - see
`TUNING_SESSION_2026-09-06.md` in this directory (Direct-mode tracking
tuning) and `PID_TUNING_SESSION_2026-09-06.md` (implementing and tuning
`TRACK_MODE_PID` - acceleration/speed/current characterization, Kp/Kd
gain tuning, and validating against a real DDP stream).

## Compact Motion Log columns

One CSV line per processed DDP command (Direct mode) or ~every 50ms while
moving (the periodic tick - PID self-logs on its own schedule instead, see
`pidLogMsConfig`): `ms,ddpVal,cmdPos,curPos,delta,lag,
profile,curSpeedHz,targetSpeedHz,encoderCount,sgResult,switchTripped`.
(Coalesce/Streaming/Lookahead modes existed earlier in this project and are
mentioned in some of the dated session logs in this directory, but were
removed entirely 2026-09-07 - Direct and PID are the only modes left.)

- `ddpVal` - the raw DDP-decoded value (0-255, or 0-65535 in 16-bit mode)
  as received - added to confirm/rule out network-layer causes for a
  glitch elsewhere in the pipeline (see the note on `cmdPos` below).
- `cmdPos` - **not** a direct decode of `ddpVal`: for the once-per-command
  rows it's the scaled target just sent to `moveTo()`, but for the
  periodic tick in between it's FastAccelStepper's own `stepper->targetPos()`
  - which can briefly report "wherever the stepper just stopped" rather
  than "what was actually last commanded" right after a `forceStop()`
  during normal operation (a homing-switch trip near position 0, or a
  StallGuard-detected stall anywhere along the travel) - see TODO.md's
  2026-09-06 entry. A real, currently-unfixed source of spikes in the
  "Commanded" plot trace, and of the stepper genuinely pausing until the
  next DDP command happens to carry a different value.
- `encoderCount`/`sgResult` - ground-truth encoder position and live
  TMC2209 StallGuard reading, both 0 if not available (no encoder wired,
  or TMC UART not connected) - real values, not literal zero readings, so
  metrics that use these (`min_sg_result`, etc.) exclude zero rather than
  treating it as data.
- `switchTripped` - raw, live homing-switch state (`isHomingSwitchTripped()`),
  not a latched/edge flag - feeds `is_stuck_at_switch()`'s post-hoc
  "commanded to move, switch already reads triggered, encoder not
  advancing" detector (a red-shaded span on the position plot, plus a
  `stuck_at_switch_count`/`_ms` metric and a per-run warning).

`plot_run()`'s figure always includes Commanded/Actual position (with
stuck-at-switch shading), raw DDP value, actual speed, and error; it adds
an SG_RESULT panel only if the run actually saw a nonzero reading (TMC
connected and StallGuard producing real data).

DDP's 4-bit rolling sequence number is validated firmware-side as of
2026-09-06 - a genuinely out-of-order or duplicate packet is now silently
dropped (`ddp_handler.cpp`'s `isNewerDdpSeq()`), counted in
`ddpPacketsRejectedOutOfOrder` (serial `s` status, `/status-data`). Doesn't
change anything about this harness's own usage - it only matters if
something upstream (a real DDP sender, not this harness's own
`send_triangle_wave()`) is prone to reordering packets over a lossy link.

## Setup

```bash
pip install -r requirements.txt
```

## Firmware side: the `$` extended serial protocol

The harness talks to the device over the same serial connection used for
the Compact Motion Log, using a small line-based command protocol (see
`include/tuning_handler.h` for the authoritative spec). A few examples:

```
$SET trackSpeed 5000      -> OK trackSpeed=5000
$GET trackAccel           -> VAL trackAccel=5000
$GET ALL                  -> one VAL line per tunable, then OK ALL
$STATUS                   -> STATUS homed=1 homing=0 checking=0 pos=1234 bottom=15474 running=0 mode=0
$HOME                     -> OK HOME
$CHECKSTEPS [target]      -> OK CHECKSTEPS started target=0
                              ... then, async once the move finishes ...
                              CHECKSTEPS_RESULT tripped=0 tripPos=0 target=0 elapsedMs=1830
$ENCDIAG                  -> ENCDIAG_START
                              freqHz,run,phase,stepperPos,encoderCount,missedTotal
                              ... 64 CSV rows ...
                              ENCDIAG_DONE
```

### Detecting real step loss: `$CHECKSTEPS`

`getCurrentPosition()` (the `curPos` column in the Compact Motion Log) is
just FastAccelStepper's own pulse-counting bookkeeping - it has no way to
notice a step that was commanded but didn't actually happen, so it always
looks internally consistent even after real mechanical step loss. The one
ground-truth signal available is the physical homing switch: if the trolley
has lost steps moving *away* from a known reference point, the step counter
will have over-counted that travel, so commanding it back to that reference
overshoots physically before the counter's own idea of "there" is reached -
the switch fires *early*.

`$CHECKSTEPS [target]` (default target `0`, the initial-homing switch
reference) drives this directly: a plain `moveTo(target)` using the normal
speed/accel profile, completely bypassing DDP and whatever tracking mode is
under test, while watching for an early switch trip. It requires
`homed=1` and the stepper idle first. The result:

- `tripped=1 tripPos=<n>` - the switch fired early; `tripPos` is how many
  steps short of `target` the counter still thought it was, i.e. roughly
  how many steps have been lost since the last homing/check. The device is
  left `homed=0` (same as any unexpected trip outside of homing) - re-home
  before trusting position again.
- `tripped=0 tripPos=<n>` - reached `target` cleanly with no early trip
  (`tripPos` here is just the final position, which should equal `target`).
  This does **not** rule out the opposite direction of drift (steps lost
  such that the trolley undershoots and never reaches the switch at all) -
  it only catches overshoot-direction loss, which is the failure mode
  that's actually been observed on the bench.

This is much faster than a full re-home (one directed move vs. searching
both ends of travel from scratch), so `tuning_harness.py` uses it as the
default check between every stress-test run - see below - reserving a full
re-home for either session start or as the automatic recovery step once a
check reports `tripped=1`.

### Bulk encoder characterization: `$ENCDIAG`

Automates the manual "run a 0%->100%->0% cycle a handful of times, write
down the encoder readings" process used to chase down the encoder drift/
noise investigation (see TODO.md's encoder section for the whole saga).
Requires `homed=1` and the stepper idle first (same as `$CHECKSTEPS`).
Repeats a direct `moveTo()` 0%->100%->0% cycle 8 times at each of four
stepper speeds (3000/4000/5000/6500 Hz), printing one CSV row per leg
(`stepperPos`, `encoderCount`, and the lifetime `missedTotal` diagnostic
from `encoder_handler.h`'s `getMissedTransitionCount()`) so a full sweep
can be captured and analyzed at once instead of transcribed by hand one
run at a time. Restores the stepper's original speed/accel when done.
Takes a few minutes end to end (64 full-range moves).

`$SET` only changes the live in-RAM config - it does **not** write to
flash, so rapid iterative tuning doesn't wear the flash and doesn't
require the motor to be idle first (unlike the web UI's Stepper
Configuration save, which is flash-backed and deliberately deferred while
moving). Once you've found values you like, set them for real through the
web UI so they persist across a reboot.

Tunable names: see `include/tuning_handler.h`'s own doc comment for the
authoritative, current list - it's grown substantially since PID mode
gained feedforward, a configurable tick rate, and lookahead smoothing, and
duplicating that list here has already drifted stale once.

## Running a tuning iteration

```bash
python tuning_harness.py --port COM5 --ddp-host 192.168.1.50 \
    --config example_config.json --label iter1
```

This will:
1. Apply every tunable in the config file via `$SET`.
2. Force `protocolDebug` off and `compactLog` on, so the serial stream is
   clean CSV.
3. Home the trolley first, unless `--skip-initial-rehome` was passed *and*
   `$STATUS` already says `homed=1` - a fresh homing is the default even if
   the device claims it's already calibrated, since trusting a stale
   reference silently is exactly what this whole feature exists to catch.
4. For each duration in `--durations` (default `5,8,12,16,20`, matching the
   full 0->255->0 triangle-wave cycle time in seconds - note the trolley's
   fastest achievable end-to-end move is roughly 2.5s, so a 5s cycle is
   already close to its physical limit):
   - Before the run, check for step loss (see `$CHECKSTEPS` above) - on by
     default, disable with `--no-check-steps-between-runs`. A detected trip
     automatically triggers a full re-home to recover before continuing.
     Pass `--rehome-between-runs` to do a full re-home before every run
     instead (slower, but also re-derives `bottomPosition` from scratch
     rather than just checking it).
   - Send the triangle wave over DDP while capturing every Compact Motion
     Log line, then save it.

Note the DDP sender only ever touches the stepper channel (byte offset 0,
8-bit) - it doesn't send any pixel data, so LED behavior is untouched.

## What gets written to disk (`--out-dir`, default `tuning_runs/`)

Everything is meant to accumulate across every session you ever run
against the same `--out-dir`, so you can datamine the whole tuning history
later, not just the run you just did:

- `<label>_<timestamp>_dur<N>s.log` - the raw captured CSV lines for one
  run, with a header noting the label, the config overrides you passed,
  and the *full* resulting tunable snapshot (fetched via `$GET ALL` after
  applying overrides, so the record is self-contained even if you only
  changed one or two values that session).
- `<label>_<timestamp>_dur<N>s.json` - the same info as a structured
  sidecar (run metadata, full config, computed metrics) for easy loading
  with `pandas`/`jq`/whatever, without parsing the `.log` header text.
- `<label>_<timestamp>_dur<N>s.png` - three stacked plots for that run:
  commanded vs. actual position, actual speed, and commanded-minus-actual
  error, all vs. time.
- `summary.csv` - one row per run, ever, appended across every session:
  run id, label, duration, `bottom_position`/`homing_elapsed_s` (if a full
  re-home happened before this run), `step_check_tripped`/
  `step_check_trip_pos` (if the fast check ran instead), the full tunable
  snapshot, and the computed metrics (RMS error, max error, mean speed, a
  jerk proxy, near-stall %, % of samples in tracking vs. normal profile).
  This is the file to open in a spreadsheet to compare iterations side by
  side.
- `all_rows.csv` - every single raw sample from every run, ever, tagged
  with `run_id`/`label`/`duration_s`, for when the summary metrics aren't
  enough and you want to dig into the raw traces across many runs at once.

None of these ever get overwritten - each run's filename embeds the label
and a timestamp, and the two CSVs are always appended to, so `--out-dir`
is a durable, growing record of the whole tuning history.

## Isolating one direction's behavior: `--direction-test`

```bash
python tuning_harness.py --port COM5 --ddp-host 192.168.1.50 \
    --config example_config.json --label dirtest --direction-test
```

Instead of the triangle-wave duration runs, sends one packet to each extreme
(255, then 0), with a real pause (`--direction-pause`, default 2s) once each
move fully settles - no continuous back-and-forth. A reversing triangle wave
confounds each direction's own behavior with "still finishing the previous
direction's deceleration," which is exactly the ambiguity that made an
apparent up/down speed asymmetry look real in early triangle-wave runs (see
TODO.md) until this test isolated it and showed both directions actually
reach the same cruise speed. Uses the same log/plot/summary conventions as
the duration runs, tagged `duration_s="dir"`.

A single full-range jump like this is far beyond `trackThreshold`, so both
legs exercise the *normal* profile, not tracking - this test characterizes
normal-profile behavior per direction, not the tracking profile.

## Re-analyzing without touching hardware

```bash
python tuning_harness.py --reanalyze-dir tuning_runs
```

Re-parses every `.log` file in the directory and regenerates plots/a
summary (`summary_reanalyzed.csv`) - useful after tweaking the metrics/
plotting code itself, without re-running anything on the bench.

## Suggested iteration workflow

1. Copy `example_config.json`, adjust a couple of values you want to try.
2. Run the harness with a new `--label` for this attempt.
3. Look at `summary.csv` and the newest `.png` files.
4. Repeat with the next set of values. Nothing here decides *what* to try
   next automatically - that's still a human-in-the-loop judgment call,
   informed by the plots and metrics.

## `ddp_continuous_test.py` - HTTP-only, real-fidelity continuous wave

A second, independent tool, added 2026-09-07 and since become the primary
one for `TRACK_MODE_PID` tuning (see `TUNING_SESSION_2026-09-07.md` and
`TUNING_SESSION_2026-09-07_part2.md` for the sessions built entirely
around it). Different from `tuning_harness.py` in two load-bearing ways:

- **No serial connection at all** - configuration (`GET /tunable`), the
  DDP wave itself, and log retrieval (`GET /compact-log`) all go over
  HTTP/UDP. Opening a serial connection resets the ESP32 via DTR/RTS (see
  CLAUDE.md), so this can run repeatedly with zero risk of interrupting an
  in-progress bench session or corrupting state mid-sweep.
- **Continuously repeating wave, real sender timing** - `tuning_harness.py`
  sends one up-down cycle per run; this matches DDPDebugger's actual
  `DdpSender.java` exactly (`elapsed % period`, value computed from real
  elapsed time at send, not a clean schedule), so it reproduces reversal
  bugs and jitter characteristics a cleaner synthetic wave never would.

```bash
python ddp_continuous_test.py --ddp-host 192.168.10.181 --period 8 \
    --trips 4 --bits 16 --config config_final_locked.json --label p8 \
    --out tuning_runs/p8.png
```

Per-run protocol (settled on 2026-09-07 after two real methodology bugs -
see `TODO.md`'s "PID tick rate" and "Rework test harness protocol"
sections for the full story of what went wrong first):

1. Wait for genuine idle (a previous run's verify/re-home can still be in
   flight).
2. **Verify zero**: drive to position 0 via `/set-position` (forced to
   Direct mode for this one move, restored after - NOT through whatever
   `trackMode`/PID gains are under test, and deliberately not through
   `/verify-and-rehome`, which false-flags on this exact from-anywhere
   usage pattern), confirm the homing switch actually trips there,
   re-home if it doesn't. This also serves as pre-positioning, since every
   wave here starts at value 0.
3. Send the wave with **zero mid-run HTTP calls** - draining the log
   mid-run blocks `loop()` for up to ~1s, long enough for a continuous-run
   stepper to cruise unsupervised past a physical boundary (measured
   directly, not theoretical).
4. Fetch the log once.
5. Verify zero again.

**`--trips`** (full triangle cycles; duration = trips × period) replaces a
flat `--duration` for exactly this reason: `COMPACT_LOG_ROW_CAPACITY` caps
how much of a run the device's 96KB ring buffer can hold before wrapping,
and that cap is *not* a fixed duration - it scales with however fast rows
are actually being written (`pidLogMsConfig`, independent of
`pidTickMsConfig` - see `stepper_handler.h`). A run that requests more
than the buffer can hold prints a warning naming a safe `--trips` value at
that period; heed it; the alternative is a silent tail-only capture, which
this whole protocol exists to avoid.

**`--bits {8,16}`** must match the device's actual `control16Bit` setting
(set via the config file's `control16Bit` key or the web UI) - it only
controls how many bytes this sender puts in the DDP packet, not what the
device is configured to expect.

`analyze_ripple.py` is a companion, standalone tool for the specific
question a plain `analyze()` run doesn't answer well: steady-cruise speed
ripple period/amplitude, and a dead-start/contamination detector. Run it
directly against any `.log` this tool produces:

```bash
python analyze_ripple.py tuning_runs/p8.log
```

## `led_stress_test.py` - combined stepper motion + LED pixel stress test

Added 2026-09-08 - every other tool here only ever drove the stepper
channel; LED support (`led_handler.cpp`, `LED_START_OFFSET`/
`updatePixelLedsFragmented()`) had no repeatable automated bench test at
all. Drives both at once, in the same DDP packets, the way a real show
actually does: bytes 0-1 = 16-bit stepper position (a continuously
repeating triangle wave, same shape as `ddp_continuous_test.py`'s, so real
direction reversals happen throughout the run), bytes 2+ = 3 bytes/pixel
random or "chase" RGB data for `--pixels` LEDs (150 by default).

```bash
# One-time device setup (persists control16Bit + ledPixelCount, then
# reboots - see below for why the reboot is required):
python led_stress_test.py --ddp-host 192.168.10.181 --configure \
    --pixels 150 --duration 30

# Subsequent runs, device already configured:
python led_stress_test.py --ddp-host 192.168.10.181 --pixels 150 \
    --duration 30 --pattern chase
```

**`--configure` reboots the device.** `ledPixelCount` takes effect in
RAM/NVS immediately on `POST /config`, but `led_handler.cpp`'s
`FastLED.addLeds<...>(leds, totalPixels)` - what actually sizes the live
pixel buffer - only ever runs once, at boot, from whatever `ledPixelCount`
NVS held *then*. A device that has never had LEDs configured before is
still running with them completely uninitialized until an actual reboot
happens; the config write alone silently isn't enough (confirmed by
`handleConfigPost()`'s own "reboot recommended" response text). `--configure`
does the reboot and waits for the device to come back online before
continuing, so this is one command, not "configure, then remember to
reboot yourself first."

**Verification, after the run**, all via `/status-data`/`/led-preview`/
`/persist-log` (nothing serial): stepper position actually changed, LED
preview data actually changed across the run (not stuck),
`ledMaxPixelsReceived >= --pixels` (the full pixel payload reached the
firmware, not truncated), `ledsBlanked` stayed false (no gap in the DDP
stream large enough to trip `ledBlankTimeConfig`), and the persist log's
own boot markers are unchanged before/after (the same authoritative
no-reboot check used to verify the Debug tab's crash fix - see CLAUDE.md/
TODO.md on why comparing `uptimeSecs` instead would be misleading).

**Encoder ground truth, added 2026-09-08** - `position` alone is only
FastAccelStepper's own step-pulse bookkeeping (per
`encoder_handler.h`'s own header comment: it has no way to notice a
commanded step that didn't physically happen), so an earlier version of
this script that checked only `position` wasn't actually proving the
trolley moved, just that the firmware issued that many step pulses. Now
also checks, whenever `encoderInitialized` is true: the encoder count
changed at all; it ranged over a real amount (not a couple of stray
ticks); it agreed in direction with `position` across every comparable
poll interval (both should move the same way - confirmed bench convention,
not just assumed, per `encoder_handler.h`); and `encoderMissed` didn't
grow meaningfully relative to the real motion seen. A device with no
encoder wired/initialized falls back to a `[SKIP]` line instead of
silently passing on step-count bookkeeping alone.

**Why status/preview polling runs on its own thread (`StatusPoller`),
not inline in the send loop**: it wasn't, in an earlier version of this
script - and one slow HTTP round-trip (an 8s `urllib` timeout, hit for
real on the bench) stalled DDP sending long enough to trip the device's
own `ledBlankTimeConfig` safety blanking. That's correct firmware
behavior reacting to a real gap - just a gap this script had caused
itself, not the firmware being tested. Moving all HTTP polling to a
background thread means even a fully hung HTTP call can never delay or
gap the packet stream, matching `ddp_continuous_test.py`'s established
"zero HTTP calls in the hot send path" principle.

At 150 pixels the payload is 2 + 150×3 = 452 bytes, comfortably under
`DDP_MAX_DATA_SIZE` (1472, `ddp_handler.h`) - deliberately single-packet,
no DDP offset-field fragmentation. Push past ~480 pixels
(2 + pixels×3 > 1472) and this script's packets would need real
fragmentation to stay correct - not implemented, since 150 was the actual
ask.
