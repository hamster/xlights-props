# PID Feedforward + Test Protocol Session — 2026-09-07

Goal: chase the remaining PID-vs-Direct-mode smoothness gap under a new objective the user set at the start of this session — *"tuning for absolute speed isn't useful if it is going to be jerky... we can be behind a handful of frames and it will still look good... but we always need to be smooth."* Along the way, found and fixed a dead encoder, a real out-of-bounds runaway, and a test harness that was blinding itself mid-run. Full detail lives in the main project `TODO.md`; this is the graphical record.

## Final config

`Kp=4, Kd=0.3, pidAccel=50000, pidFeedforward=1, pidFfWindowMs=200`, 16-bit DDP, `bottomPosition≈15500`.

## Root cause: the velocity feedforward estimator, not the P/D terms

Every prior smoothness attempt (derivative filtering, a shelved trajectory-reference layer, a correction-term slew limiter built and discarded this session) targeted the P or D terms. Direct measurement showed the feedforward term was contributing ~350-400 Hz/tick of jerk by itself — more than P and D combined — because its old estimator divided a fine numerator (target change) by a denominator quantized to the 20ms tick, aliasing against a 40fps source's 25ms packet interval. Replaced with a least-squares slope over a fixed time window; confirmed independent of DDP bit depth.

## A real, serious bug found mid-session: the encoder was dead

Discovered the hard way: the rotary encoder silently stopped counting partway through the previous session, and ~35 tuning runs were reported "zero stalls" on the strength of the firmware's own step counter — which keeps incrementing while a stalled motor sits still, so it reads clean during exactly the failure it needs to catch. A real runaway drove the trolley ~11,900 steps past the top switch. See `TODO.md` and the new persistent memory note ("tuning requires encoder for second source of truth") for the full story. The user fixed the mechanical issue; every run below has live, ratio-verified encoder data (10.00 steps/count throughout).

## A second bug found live: mid-run log draining blinds the control loop

The device's compact-log ring buffer (96KB, ~1890 rows) can't hold a full 60s run at 50 rows/sec, so the harness was draining it mid-run over HTTP. Serving that response blocks `loop()` for 0.5-1.0s, and a continuous-run stepper left unsupervised for that long just keeps cruising - traced directly to a 1.0s gap in one log where the encoder confirmed +3,660 steps of real, unsupervised travel past the bottom of travel. This is flagged as a real firmware gap in `TODO.md` (a stale-tick watchdog in `updatePidMode()` is the fix, not yet implemented) and fixed on the test-harness side per the user's own suggested protocol:

1. wait for genuine idle
2. **verify zero** - drive to position 0 via DDP, confirm the homing switch actually trips there (doubles as pre-positioning, since every wave here starts at value 0 - no startup transient to trim)
3. run the wave with **zero mid-run HTTP calls**
4. fetch the log once
5. **verify zero again**, re-home if the switch doesn't trip where expected

Runs are now sized in whole triangle-wave "trips" (`--trips`) rather than a flat duration, scaled per period to fit inside the log's buffer without needing to drain it.

## Final sweep - 5 periods, verified at both ends, real ground truth

| period | trips | rms_error | max_error | jerk | corner mean | pre-verify | post-verify |
|---|---|---|---|---|---|---|---|
| 5s | 4 | 723.3 | 1797 | 289.5 | 1194.2 | ok | **mismatch - see note** |
| 8s | 4 | 302.1 | 655 | 236.5 | 537.8 | ok | ok |
| 12s | 3 | 204.3 | 643 | 141.5 | 385.0 | ok | ok |
| 16s | 2 | 147.4 | 273 | 101.0 | 239.6 | ok | ok |
| 20s | 1 | 117.2 | 222 | 90.6 | 184.0 | ok | ok |

Zero encoder-verified stalls at every period; 10.00 steps/count throughout. Jerk falls smoothly from 290 at p5 to 91 at p20. At p12 jerk (141.5) essentially matches Direct mode's best-ever number (143) while tracking meaningfully tighter (rms 204 vs Direct's 274, corner 385 vs 475); at p16/p20 it's well under Direct's floor.

**p5 note**: within the logged run itself, everything checked out clean (net drift +99 counter vs +130 encoder-equivalent, every negative excursion small and switch-adjacent). The post-run verify's mismatch happened in the ~1.5s gap *after* logging stopped, not during the measured run - most likely `verify_zero`'s ±200-step settle tolerance being looser than "physically touching the switch," rather than genuine drift, but not confirmed either way. p5 is also the known physical speed-ceiling case (a 5s full-range cycle needs ~6200Hz average against the 7000Hz cap), so its numbers are the worst of the sweep regardless.

### 5s - the physical speed-ceiling case

![5s period](tuning_runs/final5_p5.png)

### 8s

![8s period](tuning_runs/final5_p8.png)

### 12s - jerk matches Direct mode's best-ever number while tracking tighter

![12s period](tuning_runs/final5_p12.png)

### 16s

![16s period](tuning_runs/final5_p16.png)

### 20s - smoothest and tightest of the sweep

![20s period](tuning_runs/final5_p20.png)

## Open items

- The p5 post-verify mismatch above - worth tightening `verify_zero()`'s settle criteria (smaller tolerance, or a two-poll confirm before checking the switch) and re-running to see if it reproduces.
- `TODO.md`'s new stale-tick watchdog item - the actual firmware fix for the blind-loop hazard the harness protocol above works around. Not yet implemented.
- The compact-log ring buffer (96KB / ~1890 rows) is a chosen size, not a hard RAM ceiling - current usage is 63.2% (207,180 / 327,680 bytes), so there's headroom to grow it if the per-period trip-scaling becomes annoying.
