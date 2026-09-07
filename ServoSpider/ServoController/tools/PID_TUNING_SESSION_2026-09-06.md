# PID Control Session Summary — 2026-09-06

Goal: implement closed-loop PID control for DDP position tracking (replacing the binary normal/tracking-profile switch the other modes use), characterize how much acceleration/speed/current the mechanism can actually take without stalling, tune the PID gains from that data, then validate against a real DDP stream. Full blow-by-blow is in the main project `TODO.md`; this is the distilled version.

## TL;DR

- Implemented `TRACK_MODE_PID` — a real P+I+D controller (derivative-on-measurement, conditional anti-windup) that reads `positionRequest` directly every tick instead of reacting only to discrete DDP commands. **Explicit design constraint: no encoder dependency** — the encoder is bench-only, used here purely to independently verify results, never read by the controller itself.
- First bench test drove the wrong direction entirely. Root cause: `setJumpStart()`'s startup kick — correctly signed for `moveTo()`-based moves — fires backward when issued through the continuous-run API PID uses. Fixed by disabling jumpStart during continuous-run and restoring it for the settle-to-target snap.
- Characterized acceleration, cruise speed, and TMC run current independently on real hardware. Acceleration and current turned out **not** to be the binding constraint anywhere tested — the real, sharply asymmetric limit is cruise speed, and even that boundary needed a human ear to catch what the encoder-slip metric missed.
- Tuned Kp then Kd via step-response sweeps: zero overshoot up to Kp=8 (~1020ms response), real overshoot from Kp=10 up; adding Kd=0.7 at Kp=15 gets *faster and cleaner* than the best pure-P result.
- First real DDP triangle-wave test (not just single-step response) found two more real bugs — PID's own settling behavior near the switch was tripping the "rammed into stop" safety check, and the first fix for that reintroduced Direct mode's own "constantly re-aiming never accelerates" jerkiness. Both fixed; a full 5-duration sweep is now clean end to end.

## Direction bug: `setJumpStart()` fires backward in continuous-run mode

PID's very first move — departing the homing switch for a large positive target — drove *backward* instead, tripping the newly-built ramming detector. Isolated with a controlled A/B test: identical move starting well away from the switch converged perfectly; starting glued to the switch failed every time; zeroing `jumpStart` before engaging PID fixed it even right at the switch. The PID control law and direction handling were correct throughout — confirmed independently by the bench encoder, which tracked every failed attempt's real physical motion in lockstep with the buggy position count.

**Fix**: disable jumpStart immediately before every `runForward()`/`runBackward()` call, restore it before the `moveTo()`-based settle at the deadband. Same fix applied to Streaming mode, which has the identical exposure.

## Characterization sweeps

Same methodology throughout: `$CHECKSTEPS` (direct `moveTo()`, independent of DDP/tracking-mode) for a large round trip clear of both physical ends, bench encoder as ground truth, steps/count ratio measured fresh from each run's own known-good baseline leg (~9.9–10.2 steps/count, matching the documented 1:10 worm ratio).

### Acceleration (speed fixed at 6500 Hz)

Swept 2500 → 50,000 steps/s² (20× the prior default). **No measurable slip anywhere, either direction, the entire range.** At 6500 Hz cruise, even 50,000 accel only takes 0.13s to ramp — the whole tested range was already cruise-dominated. Acceleration was never the real constraint at this speed.

### Cruise speed (accel fixed at 50,000, the clean value above)

| speed (Hz) | down (gravity-assisted) | up (gravity-opposed) |
|---|---|---|
| 8000 | clean | clean |
| 8500 | clean | clean |
| 9000 | clean (0.34%) | **51% slip — real stall** |
| 9500 | **39% slip — real stall** | (not tested — already failed at 9000) |

Real, sharply asymmetric boundary: **up (winding in against gravity) safe to ~8500 Hz, fails by 9000; down (paying out, gravity-assisted) safe to ~9000 Hz, fails by 9500.** Matches physical intuition — winding in demands more torque.

### Run current (accel fixed at 50,000, three speeds inside the clean zone)

Swept 1400 mA down to 800 mA at 6000/7000/8000 Hz. **Zero measurable slip at any current tested, any speed, either direction.** Current wasn't the limiting factor either — large torque margin even at close to half the configured 1400 mA.

### Where the numbers and the bench disagreed

Both sweeps above came back clean at 8000 Hz — but while watching the current sweep run, the motor audibly sounded close to stalling on the first few (highest-current) passes at 7000–8000 Hz, uniformly across the whole current range at that speed (not just the high-current end, which argues for a speed/resonance effect rather than a graduated torque-margin one). **The slip metric never caught this at either accel value tested (50,000 or the later 200,000).** Backed off to a working ceiling of **1200 mA / 8000 Hz / 200,000 accel**, then further to **7000 Hz** once 8000 Hz repeatedly sounded wrong at the higher accel — settling below what the data alone would have justified. Real-world lesson: encoder-slip is a poor stand-alone proxy for acoustic stress; a motor can sound like it's losing sync well before a short, cold-motor bench test shows a missed step.

## PID gain tuning (step response)

Fixed `pidAccel` at 50,000 (vetted clean above) so tuning tested the control loop itself, not an arbitrary slow ramp. Methodology: command a single, sudden ~6000-step target change from rest, capture the full transient, measure overshoot / time-to-first-cross-deadband / oscillation.

**Two real test-methodology bugs found and fixed before this data could be trusted**, useful precedent for any future DDP-based bench test: `positionRequest` is a persistent global that never resets between tests, so switching into PID mode started driving toward the *previous* test's stale-but-identical target before the "real" test packet even arrived; and DDP is UDP with no ack, so a lost packet left that stale state uncaught. Fixed with a neutral reset packet plus a new `$GET positionRequest` to verify delivery and resend until confirmed.

### Kp sweep (Ki = Kd = 0)

| Kp | overshoot (steps) | first-crossing time (ms) |
|---|---|---|
| 0.5 | – | never converged in 8s |
| 1.0 – 8.0 | 0 | 3553 → 1020 (steady improvement) |
| 10.0 | 81 | 960 |
| 15.0 | 333 | 1200 |
| 20.0 | 408 | 1180 |
| 30.0 | 462 | 921 |

Clean and monotonic: response improves steadily with Kp through 8.0 with zero overshoot the whole way, then real overshoot appears from Kp=10 on with barely any further speed gain. **Kp≈6–8 is the pure-P sweet spot.**

### Kd sweep (Kp fixed at 15, which overshoots 333 steps at Kd=0)

| Kd | overshoot (steps) | first-crossing time (ms) |
|---|---|---|
| 0.0 | 300 | 920 |
| 0.3 | 190 | 940 |
| 0.5 | 9 | 960 |
| **0.7** | **3** | **1020** |
| 1.0 | 8 | 1100 |
| 2.0 | 16 | 1306 |

Overshoot drops steadily to near-zero by Kd≈0.5–0.7 while response time barely moves — genuine derivative damping, not just a slower response in disguise. **Kp=15/Kd=0.7 ends up both faster and cleaner than the best pure-P result.** Past Kd≈1.0, response time climbs again without further benefit (over-damping).

## Validating against a real DDP stream — two more bugs, fixed

Single-step response testing never exercises a live, continuously-updating DDP stream. The first full triangle-wave run (`tuning_harness.py`) completed its first 5s pass cleanly, then every later duration failed with `ERR not homed` — something silently un-homed the device mid-session.

**Bug 1 — the safety net misreading PID's own normal behavior.** `updateRammedIntoStopCheck()` (built earlier this session specifically to catch a trolley genuinely jammed against the switch) fired on PID's *legitimate* settling right at position 0 — always switch-triggered, and a completely ordinary DDP endpoint, not just a homing reference. DDP's 8-bit quantization (~60 steps per unit on this device) can exceed PID's tight 30-step deadband near there, repeatedly kicking it back into full continuous-run mode for what was really a trivial correction — each one real motion at the switch, cumulatively enough to look like a jam.

**Bug 2 — the fix for Bug 1 reintroduced a familiar problem.** The straightforward fix (a wider hysteresis band that just snaps to the new target) called `moveTo()` on every ~20ms tick while the target crept slowly through that band — which kept resetting the stepper's ramp generator before it ever built real speed. Confirmed directly on the bench: a slow 16s wave left the trolley stuck dead at position 0 for **4+ seconds** while the commanded target moved smoothly away, then caught up in one big delayed jump. This is exactly the "constantly re-aiming never accelerates" jerkiness Direct mode is already known for — PID's whole design exists to avoid it, and the naive hysteresis fix had quietly brought it back.

### Before (Bug 2 still present) — stuck at zero, then a delayed catch-up

![Stuck at zero for 4+ seconds](tuning_runs/pid_tuned5_20260906_181138_dur16s.png)

### After both fixes — clean tracking throughout

![Clean tracking after both fixes](tuning_runs/pid_tuned6_20260906_181600_dur16s.png)

### Before — visible oscillation on the "up" (gravity-opposed) leg only

![Oscillation on the up leg](tuning_runs/pid_tuned5_20260906_181138_dur8s.png)

### After — clean and symmetric in both directions

![Clean in both directions](tuning_runs/pid_tuned6_20260906_181600_dur8s.png)

The "up-direction oscillation" that looked like it might be a real gravity-related asymmetry turned out to be a symptom of these two bugs, not a separate real effect — it's gone in both directions once both were fixed.

**Fixes**: a new `pidReengageThreshold` (150 steps) hysteresis band — small target shifts while settled get another one-shot `moveTo()` snap instead of re-engaging continuous-run mode — combined with a matching small tolerance in the ram-check itself (net drift only; doesn't weaken real-jam detection, since the trip reference never resets while the trip continues). The hysteresis snap only fires once the *previous* small move has actually finished running, not on every tick.

### Full sweep after both fixes — clean end to end

| duration | rms_error | max_error | near_stall_pct | tracking_pct |
|---|---|---|---|---|
| 5s | 908 | 1442 | 2.3% | 100% |
| 8s | 490 | 1109 | 1.7% | 100% |
| 12s | 299 | 746 | 1.0% | 100% |
| 16s | 234 | 526 | 1.0% | 100% |
| 20s | 181 | 401 | 0.3% | 100% |

Error scales down smoothly and predictably as duration increases — exactly the pattern every pre-PID mode showed in the very first characterization session — with no un-homing anywhere and no stuck episodes.

## Where things landed

**Working PID gains**: `Kp=15, Ki=0, Kd=0.7, MaxSpeed=7000, Accel=50000, Deadband=30, ReengageThreshold=150`. **Environment**: `1200mA run current, 200,000 accel, 7000Hz cruise cap` (current and accel saved via the web UI; the 7000Hz speed cap is a live override only — see below).

## Still open

- A real, bounded, visible high-frequency ripple remains in actual speed during active tracking (~500–4500Hz, ~0.2–0.3s period, both directions) — doesn't hurt overall accuracy but isn't fully explained; likely Kd reacting to rate-estimate noise on a live, continuously-updating target rather than a clean single step.
- Ki hasn't been tuned — the deadband snap already closes any P/PD steady-state gap for a single step, so this needs a genuinely different test (sustained tracking of a moving target, not step response) to matter.
- `pidReengageThreshold` / the ram-check's tolerance (150, matched to each other) were chosen from the DDP-quantization math, not an independent sweep.
- `normalSpeed=7000Hz` is currently a live override only, not saved — a reboot reverts it to the saved 8000Hz until it's explicitly saved via the web UI or a different number is settled on.
- The speed-vs-current sweep was never re-run below 800mA, and StallGuard's re-enable/replace decision (flagged in the previous session's summary) is still open.
