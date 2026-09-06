# DDP Tuning Session Summary — 2026-09-06

Goal: get the trolley moving smoothly (not jerky) across a range of DDP-commanded speeds, using `tools/tuning_harness.py` (see `tools/README.md` for the tool itself) to gather real bench data instead of eyeballing it.

This file is a distilled summary. The full blow-by-blow (every bug, every dead end) is in the main project `TODO.md`. Raw data for everything below lives in `tuning_runs/` — `summary.csv` (one row per run, every session ever) and `all_rows.csv` (every raw sample), plus per-run `.log`/`.json`/`.png`.

## TL;DR

- Found and fixed a **real firmware crash** (not related to tuning itself) caused by a new diagnostic feature added mid-session — see "Crash found and fixed" below.
- Found that **TMC2209 run current + tracking acceleration** matter a lot: raising current 1200→1400mA and dropping `trackAccel` 5000→2500 eliminated real step loss entirely on the slower, tracking-dominated runs, and meaningfully tightened tracking error.
- Found a **second, still-open bug**: after any `forceStop()` (a switch touch or a StallGuard stall during normal operation), the stepper is left stopped with no corrective move — it just sits until the next DDP packet happens to carry a genuinely different value. This shows up as real, physical pauses, and as visible spikes in the "commanded position" trace. **Not yet fixed.**
- StallGuard is currently **disabled** for continued bench tuning (per explicit decision — it was fighting the data collection worse than the real stalls it's meant to catch). This is fine for bench work but needs a real decision before any unattended/production use.

## Tunable sweep results

All runs: `trackMode=Direct`, `trackThreshold=2000`, `trackMaxLag=3000`, triangle wave at 5/8/12/16/20s full-cycle durations. RMS error and step-loss numbers below are for the 12/16/20s runs (tracking-mode-dominated); the 5s run is mostly normal-profile motion and behaves differently (see "5s run" note below).

| Config | Current | trackAccel | StallGuard | 12s RMS | 16s RMS | 20s RMS | Real step loss (12/16/20s) | Stuck-at-switch events |
|---|---|---|---|---|---|---|---|---|
| Baseline | 1200mA | 5000 | On (thresh 50) | 1768 | 2003 | 1791 | 3 / 10 / 4 steps | n/a (not tracked yet) |
| **Best overall** | 1400mA | **2500** | Off | **1378** | **1147** | **752** | **0 / 0 / 0** | **0 / 0 / 0** |
| Too conservative | 1400mA | 1500 | Off | 1889 | 1596 | 1059 | 0 / 0 / 0 | 1 / 2 / 0 |
| Tightest tracking | 1400mA | 3500 | Off | 1419 | 743 | 486 | 0 / 0 / 0 | 1 / 0 / 1 |
| Lower current | 1300mA | 2500 | Off | 1335 | 1023 | 648 | 0 / 0 / 0 | 1 / 0 / 0 |

**Reading this table:**
- Going from the old baseline (1200mA/5000) to 1400mA/2500 was a real, substantial win — tighter tracking *and* zero real step loss, where the baseline was losing 3-13 steps almost every run.
- **1500 is confirmed worse, not safer** — more conservative acceleration gave both worse tracking *and* more switch touches. The sweet spot is not "gentler."
- **3500 gives the tightest raw tracking numbers** (best 16s/20s of anything tested) but reintroduces occasional brief switch contact — a real tradeoff between tightness and clean endpoint behavior.
- **1300mA matched or slightly beat 1400mA** at the same acceleration — the current bump to 1400 may not have been strictly necessary, though this is only one run per config, so treat it as suggestive, not settled.
- **Recommendation: stick with trackAccel=2500.** It's the only config with zero stuck-at-switch events across all three tracking-dominated runs, and the RMS gap to 3500 is real but modest — 3500's occasional switch touches are exactly the kind of thing that reads as jerky.

### Before: baseline (1200mA, trackAccel=5000, StallGuard on)
![Baseline 20s run](tuning_runs/baseline_20260906_020438_dur20s_reanalyzed.png)

### After: best config (1400mA, trackAccel=2500, StallGuard off)
![Best config 20s run](tuning_runs/trackaccel2500_nosg_20260906_030041_dur20s_reanalyzed.png)

### Alternative: tightest tracking (1400mA, trackAccel=3500)
![trackAccel=3500 16s run](tuning_runs/trackaccel3500_20260906_030909_dur16s_reanalyzed.png)

### StallGuard readings while still enabled (thresh=25 test)
Shows the actual SG_RESULT panel — readings bottoming out near 0 (well below the 50/25 thresholds tried), confirming the motor really was running low on torque margin, not just tripping on a miscalibrated threshold.
![SG_RESULT bottoming out](tuning_runs/trackaccel2500_sg25_20260906_022439_dur16s_reanalyzed.png)

## Crash found and fixed

While chasing StallGuard interference, added a small SPIFFS-backed diagnostic log (`persist_log.h`/`.cpp`) that survives a reboot, specifically to answer "did the device actually crash, or was that just a new serial connection's DTR/RTS reset?" — an ambiguity that came up repeatedly this session.

The very first real use of it caught a genuine `PANIC (Guru Meditation Error)` crash, right after a homing-search breadcrumb. Root cause: the log's own writes went straight to flash, and a flash write briefly disables the flash cache — the same hazard already documented in `CLAUDE.md` for `Preferences.putX()` writes racing the homing-switch ISR, just hit here via a different flash API, and this time triggered by the stepper *actively stepping* when the write happened (FastAccelStepper's own step-generation ISR is the leading suspect for what wasn't IRAM-safe here).

**Fixed**: the log now only ever appends to an in-RAM buffer; a separate flush function commits it to flash, and is only ever called when the stepper is confirmed idle. Re-verified clean afterward — a full 5-run sweep completed with zero crashes.

Retrieval: `GET /persist-log` (device's IP) over WiFi — deliberately not serial, so checking it doesn't itself trigger the very reset it might be explaining. `?clear=1` wipes it for a fresh diagnostic session.

## The "commanded position spikes" investigation

Noticed while eyeballing the plots: the "Commanded" position trace has small spikes/dips that don't match the smooth synthetic triangle wave actually being sent. Traced step by step:

1. Added the raw DDP value as its own panel on the plots (`ddpVal` was already logged, just never plotted). **The raw DDP value is smooth** — confirms this isn't a network/UDP-reordering problem (also checked and fixed a latent gap here anyway: the firmware now validates DDP's rolling sequence number and discards genuinely out-of-order/duplicate packets, since nothing did before).
2. So the glitch is introduced somewhere between the raw DDP value and what gets logged as "Commanded." Checked the code: `cmdPos` in the periodic log tick comes from `stepper->targetPos()` (FastAccelStepper's own internal target), not a value this project tracks directly.
3. Found the real mechanism by matching a spike to the async event log captured alongside it: a homing-switch trip (`forceStop()`) had just fired, and the very next log row showed `cmdPos` had snapped to nearly `curPos` — i.e., `targetPos()` reports "wherever it just stopped," not "what was actually last commanded."
4. Checked a second, older run where this happens far more pervasively (spikes "the entire way," not just near the switch) — that run had StallGuard enabled, and StallGuard's own stall-during-normal-operation path calls `forceStop()` the exact same way, but a stall can be detected **anywhere** along the travel, not just near the switch. Confirmed nothing gates on the resulting `tmcStatus.stalled` flag — it's purely cosmetic (shown on the status page), so it doesn't block anything, but it also means nothing corrects the stepper's target either.

**Real, physical consequence, not just a logging artifact**: every time either of these `forceStop()` events fires during normal operation, the trolley genuinely stops dead and does not resume until the next DDP packet happens to carry a value different from before — the dispatch loop only reacts to *changes*. Right near a triangle wave's turnaround point, several consecutive samples often carry the same DDP byte value, so this can produce a real, extra pause exactly where the wave is slowest — compounding whatever else was making things feel jerky.

- [ ] **Not yet fixed**: the fix is straightforward — re-issue `stepper->moveTo(lastCommandedTargetPosition)` immediately after either `forceStop()` path (switch-trip-during-normal-operation, and stall-during-normal-operation), so the stepper resumes toward the real target right away instead of waiting on the next distinct DDP value. Also worth changing the periodic log's `cmdPos` source from `stepper->targetPos()` to the project's own `lastCommandedTargetPosition`, which isn't subject to this lag at all.

## Other open items

- **StallGuard is off.** Fine for continued bench tuning (nothing gets physically damaged by it — see TODO.md), but needs a real decision before any unattended/production use: re-enable at a threshold informed by the `min_sg_result` data (bottomed at 2-4 even in the improved config, so the old defaults would likely still false-trigger), or find a different real-time safety mechanism.
- **5s run regression**: the fastest run (mostly normal-profile motion, not tracking) got worse RMS-wise with the new settings, though with zero real stalling. Not investigated yet.
- **Sweep is sparse**: only one data point at each of 1500/2500/3500 `trackAccel` and 1300/1400mA current — enough to see the shape of the tradeoff, not enough to call 2500/1400 a confirmed optimum.
