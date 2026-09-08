# Kp=3 + PID Tick Rate Session — 2026-09-07 (continued)

Continues [TUNING_SESSION_2026-09-07.md](TUNING_SESSION_2026-09-07.md) - after the feedforward estimator fix in that document, this session chased the remaining ~120-150ms speed ripple the user was hearing/seeing as "actual speed being shaky." Full narrative in the main project `TODO.md`; this is the graphical record.

## Final locked config

`Kp=3, Kd=0.3, pidAccel=50000, pidFeedforward=1, pidFfWindowMs=200, pidTickMs=5, pidLogMs=20`, 16-bit DDP. All compiled defaults - verified live from a genuine fresh boot, not RAM overrides.

## What changed and why

Two ideas were tried against the ripple, at the user's suggestion:

- **A lookahead/reference-smoothing buffer** (`pidLookaheadMsConfig`) - worked exactly as designed (jerk and ripple fell monotonically with window size) but landed on the same trade curve as simply lowering `Kp` further, not a better one. Built, tested, kept as an off-by-default tunable, not adopted.
- **A faster PID control tick** (`pidTickMsConfig`, was a hardcoded 20) - the real lever. Unlike every other smoothing attempt this project has tried (derivative filtering, a correction slew limiter, `pidAccel` reduction, the lookahead buffer above), this bought real smoothness **with no measured accuracy cost**. The ripple's own period shrank faster than proportionally with the tick (141ms -> 71ms -> 22ms as tick went 20 -> 10 -> 5ms), which is the signature of a genuine sampled-control-loop dynamic (the loop's own sample delay interacting with the stepper's accel-limited response), not a physical resonance - explaining why no *output* filter tried earlier ever moved it much.

`pidTickMs=2` was tested and rejected: `protocolPacketsRejectedOutOfOrder` went from 0 (every single other run, all session) to 6 during that one run, and a pre-existing log-corruption artifact got measurably worse. Both point at Core 1's `loop()` running out of headroom, not the control law running out of benefit. Moving PID to Core 0 would be the way past that, but is real re-architecture (cross-core state, `FastAccelStepper`'s engine is pinned to Core 1) - explicitly declined for this session, recorded as a scoped future item in `TODO.md`.

**A real methodology trap along the way**: making the log rate follow the tick rate quadrupled log volume, overflowing the 96KB Compact Motion Log ring buffer in under 11 seconds of continuous motion at `pidTickMs=5`, silently truncating several runs. Fixed by decoupling log cadence from control cadence entirely (`pidLogMsConfig`, independent tunable) - logging has no feedback into the control law, so this costs nothing there, but it does mean **a 20ms-throttled log downsamples away the very improvement a faster tick provides**, which is why this document shows both a full-period sweep at the safe, throttled log rate, and a dedicated full-resolution comparison to actually see the smoothness the numbers claim.

## Full-resolution proof: this is what the tick rate actually buys

Captured at `pidLogMs` matching `pidTickMs` (before the throttle fix existed) - short runs, full control-rate resolution, so the *actual* achieved smoothness is visible rather than downsampled back to 20ms.

| tick | rms_error | jerk | ripple_rms | ripple_period | DDP rejected |
|---|---|---|---|---|---|
| 20 (old default) | 386.5 | 199.1 | 210.3Hz | 140.7ms | 0 |
| 10 | 392-395 | 131-132 | 117-121Hz | 71-72ms | 0 |
| **5 (locked in)** | **394.0** | **109.8** | **94.1Hz** | **21.9ms** | **0** |
| 2 (rejected) | 394.5 | 62.2 | 51.8Hz | 21.0ms | **6** |

### Actual speed at every Kp tested, same y-axis scale (the ripple this whole investigation is about)

![Kp sweep speed comparison](tuning_runs/kp_sweep_speed_comparison.png)

### Tick=5, full resolution, period=8s

![tick=5 full resolution, p8](tuning_runs/tick5_v1.png)

### Tick=5, full resolution, period=12s (generalization check)

![tick=5 full resolution, p12](tuning_runs/tick5_p12.png)

### Tick=10 for comparison (roughly double the ripple of tick=5)

![tick=10 full resolution](tuning_runs/tick10_v2.png)

## Full period sweep at the locked config - safe log rate, full corner/cycle coverage

Standard periods, floor moved to 6s this session (5s confirmed physically speed-limited and retired as a target). Each run verified at both ends (drive to zero, confirm the homing switch trips - not `/verify-and-rehome`, which was found to false-flag on this exact usage pattern, see `TODO.md`) plus encoder ground truth throughout.

| period | trips | rms_error | max_error | jerk* | corner mean | frame lag p95 | stalls |
|---|---|---|---|---|---|---|---|
| 6s | 4 | 538.9 | 934 | 275.9* | 700.8 | 4.7 | 0 |
| 8s | 4 | 392.8 | 498 | 200.8* | 475.9 | 4.5 | 0 |
| 12s | 3 | 271.8 | 533 | 135.1* | 399.7 | 4.8 | 0 |
| 16s | 2 | 202.4 | 382 | 100.7* | 289.8 | 4.6 | 0 |
| 20s | 1 | 159.9 | 252 | 87.4* | 213.0 | 4.6 | 0 |

*Jerk here is measured from a 20ms-throttled log (`pidLogMs=20`) - it reflects the same resolution every earlier lever in this project was compared at, but **understates** how smooth the actual 5ms-tick control loop is; see the full-resolution section above for the real number at each tick rate. Zero real (encoder-verified) stalls at every period; 10.00 steps/count throughout.

### 6s

![6s period](tuning_runs/final3_p6.png)

### 8s

![8s period](tuning_runs/final3_p8.png)

### 12s

![12s period](tuning_runs/final3_p12.png)

### 16s

![16s period](tuning_runs/final3_p16.png)

### 20s - smoothest and tightest of the sweep

![20s period](tuning_runs/final3_p20.png)

## Open items

- `pidLogMsConfig` defaults to 20ms (safe, full-coverage captures). Set it to match `pidTickMs` (5) for a short diagnostic capture needing full control-rate resolution - see `COMPACT_LOG_ROW_CAPACITY` in `tools/ddp_continuous_test.py` for the safe-duration math at a given log rate.
- A packed binary Compact Motion Log format was discussed but not built this session - re-encoding to CSV only at `GET /compact-log` fetch time (the device is always idle then) would give roughly 2.3x more rows for the same 96KB, without touching Serial output or any Python tooling. Real, contained follow-up work.
- A serial-vs-HTTP comparison capture (same run, both channels) was proposed to help root-cause the still-unexplained malformed-leading-log-row artifact (seen at `pidTickMs` below 20, never at 20ms) - not yet done.
- The lookahead buffer (`pidLookaheadMsConfig`) remains in the firmware, off by default, if a future situation wants that specific trade over Kp's.
