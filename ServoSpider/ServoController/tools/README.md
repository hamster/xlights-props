# Tuning tools

`tuning_harness.py` automates bench-testing the stepper tracking modes
(`StepperTrackMode` in `stepper_handler.h`): it configures the device over
serial, drives the trolley with a synthetic DDP triangle wave, captures the
Compact Motion Log, and produces plots/metrics - so a tuning iteration is
"edit a config file, run one command, look at a plot" instead of manual
button-pushing and eyeballing a serial terminal.

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

`$SET` only changes the live in-RAM config - it does **not** write to
flash, so rapid iterative tuning doesn't wear the flash and doesn't
require the motor to be idle first (unlike the web UI's Stepper
Configuration save, which is flash-backed and deliberately deferred while
moving). Once you've found values you like, set them for real through the
web UI so they persist across a reboot.

Tunable names: `normalSpeed`, `normalAccel`, `jumpStart`, `trackEnabled`,
`trackThreshold`, `trackSpeed`, `trackAccel`, `trackMaxLag`, `trackMode`
(0=Direct, 1=Coalesce, 2=Streaming), `coalesceMs`, `coalesceSteps`,
`streamRateWindow`, `streamSettle`, `compactLog`, `protocolDebug`.

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
