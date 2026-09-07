# PID Control Session Summary — 2026-09-06

Goal: implement closed-loop PID control for DDP position tracking (replacing the binary normal/tracking-profile switch the other modes use), characterize how much acceleration/speed/current the mechanism can actually take without stalling, tune the PID gains from that data, then validate against a real DDP stream. Full blow-by-blow is in the main project `TODO.md`; this is the distilled version.

## TL;DR

- Implemented `TRACK_MODE_PID` — a real P+I+D controller (derivative-on-measurement, conditional anti-windup) that reads `positionRequest` directly every tick instead of reacting only to discrete DDP commands. **Explicit design constraint: no encoder dependency** — the encoder is bench-only, used here purely to independently verify results, never read by the controller itself.
- First bench test drove the wrong direction entirely. Root cause: `setJumpStart()`'s startup kick — correctly signed for `moveTo()`-based moves — fires backward when issued through the continuous-run API PID uses. Fixed by disabling jumpStart during continuous-run and restoring it for the settle-to-target snap.
- Characterized acceleration, cruise speed, and TMC run current independently on real hardware. Acceleration and current turned out **not** to be the binding constraint anywhere tested — the real, sharply asymmetric limit is cruise speed, and even that boundary needed a human ear to catch what the encoder-slip metric missed.
- Tuned Kp then Kd via step-response sweeps: zero overshoot up to Kp=8 (~1020ms response), real overshoot from Kp=10 up; adding Kd=0.7 at Kp=15 gets *faster and cleaner* than the best pure-P result.
- First real DDP triangle-wave test (not just single-step response) found two more real bugs — PID's own settling behavior near the switch was tripping the "rammed into stop" safety check, and the first fix for that reintroduced Direct mode's own "constantly re-aiming never accelerates" jerkiness. Both fixed.
- The raw DDP trace looked jumpy in most runs. Root cause was entirely host-side, not the device: the test harness's own sender computed each value from real elapsed time, so ordinary scheduling jitter on the sending machine produced real (if artificial) jumps. Fixed, and verified two independent ways that delivery itself was never the issue - zero packets lost, zero sequence gaps.
- Re-running the full sweep with the fixed sender exposed one more real bug (the hysteresis fix's own `moveTo()` could silently go "dead" the same way `runForward()`/`runBackward()` already had) plus a logging-completeness gap that had been making clean DDP reception look jumpy in the log. Both fixed; the full 5-duration sweep is now clean end to end, and it's the cleanest data of the whole session.
- A later re-run of the same clean sweep regressed hard - real multi-second freezes where `positionRequest` itself stopped updating. Investigated separately with dedicated instrumentation (see `TODO.md`); re-running the full sweep again after that work found no freezes at all, numbers matching the original clean baseline almost exactly - encouraging, but not proof the underlying cause is understood or fixed. Also visible in that final re-run: overall tracking is genuinely smooth (matches the user's own "jerky but smooth" read watching it live), but a real, bounded high-frequency ripple in actual speed is present underneath and still unexplained.

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

## The raw DDP trace looked jumpy — dropped packets, or WiFi? Neither.

Asked directly: was the jumpy-looking "Raw DDP value" panel in these plots a sign of dropped packets or a WiFi problem? Checked it properly rather than assuming either way.

**It wasn't reception at all - it was the test harness's own sender.** `send_triangle_wave()` computed each packet's value from real elapsed wall-clock time *at the moment of sending*, not from the schedule - so any ordinary send-loop delay (OS scheduling jitter, general system load) meant the next value legitimately reflected a bigger jump once the delayed send finally went out. Confirmed with a direct packet-count comparison against the device's own `protocolPacketsReceived` counter: **321 sent, 321 received, 0 rejected as out-of-order**, on a run whose trace still showed several multi-unit jumps. Every packet arrived; only the *value* each one carried was affected.

**Fixed**: the sender now computes each value from the *scheduled* tick time instead, so the value sequence is a perfectly smooth, deterministic ramp regardless of real-time jitter - and this now better matches how a real DDP source like FPP actually works (a fixed show timeline, not "whatever's correct for whenever the sender gets around to it"). Verified 100% clean (510/510 single-unit transitions, zero jumps) on a fresh run, up from 92.7% before.

**Re-running the full sweep with the fixed sender surfaced two more things**, both fixed:
1. The hysteresis band's own `moveTo()` (added for Bug 1 above) could silently go "dead" the same way `runForward()`/`runBackward()` already had earlier this session - a nonempty step queue alone satisfies `isRunning()`, even when the ramp generator never actually starts. The 16s run regressed hard (`rms_error` 234→1515) with the exact same "stuck at position 0, then a delayed catch-up" signature as Bug 2. Reproduced directly on the bench with targeted diagnostics (`isRunning=1, qEmpty=0, rampActive=0, speed=0`); fixed by treating "genuinely running" as ramp-active-or-nonzero-speed, not just a nonempty queue, and retrying (throttled) when it isn't.
2. `updatePidMode()` only wrote a log row when actually issuing a correction, not on every tick - so a real, smoothly-arriving DDP stream could update the commanded position many times while PID sat idle, with none of it logged; the next logged row would then show an artificial jump indistinguishable from a dropped packet. **Confirmed this was purely a logging gap, not a reception one**: a `protocolDebug` capture (prints every packet as it's parsed, independent of any tracking-mode's own logging) showed zero sequence gaps across 202 packets on a run whose compact log alone suggested ~26% of transitions were jumpy. Fixed by logging unconditionally on every tick.

### Before (all bugs fixed except the sender/dead-move/logging ones above) — stuck at zero, then a delayed catch-up

![Stuck at zero for 4+ seconds](tuning_runs/pid_tuned5_20260906_181138_dur16s.png)

### After every fix in this document — clean tracking, clean DDP trace, throughout

![Clean tracking, clean DDP trace](tuning_runs/pid_final_20260906_210511_dur16s.png)

### Before — visible oscillation on the "up" (gravity-opposed) leg only

![Oscillation on the up leg](tuning_runs/pid_tuned5_20260906_181138_dur8s.png)

### After — clean and symmetric in both directions

![Clean in both directions](tuning_runs/pid_final_20260906_210511_dur8s.png)

The "up-direction oscillation" that looked like it might be a real gravity-related asymmetry turned out to be a symptom of these bugs, not a separate real effect — it's gone in both directions once everything above was fixed.

### Full sweep, every fix applied — the cleanest data of the whole session

| duration | rms_error | max_error | near_stall_pct | tracking_pct |
|---|---|---|---|---|
| 5s | 747 | 1497 | 1.8% | 100% |
| 8s | 429 | 1103 | 0.2% | 100% |
| 12s | 279 | 564 | 0.1% | 100% |
| 16s | 213 | 442 | 0.2% | 100% |
| 20s | 173 | 352 | 0.5% | 100% |

Error scales down smoothly and predictably as duration increases — exactly the pattern every pre-PID mode showed in the very first characterization session — with no un-homing anywhere, no stuck episodes, and a `ddpVal` trace that's 94% clean single-unit transitions (the remaining ~6% is ordinary sampling-rate mismatch between PID's 20ms log tick and DDP's ~25ms send interval, not a real gap - confirmed visually smooth in the plot too).

## A new, unexplained anomaly - real DDP freezes mid-run (2026-09-06, `pid_observed_*`)

Re-ran the full sweep once more (same firmware/config as the clean `pid_final` run above) purely so the user could watch the trolley directly - and this run came back noticeably worse: 5s/8s/16s all regressed hard, and the 20s run's pre-check caught a real 12-step loss requiring an automatic re-home mid-sweep.

| duration | rms_error | max_error | near_stall_pct | notes |
|---|---|---|---|---|
| 5s | 4182 | 9631 | 2.2% | inherently the hardest wave (see below) |
| 8s | 1309 | 6340 | 1.3% | two multi-second freezes |
| 12s | 182 | 776 | 0.9% | clean during capture, froze right at its own end |
| 16s | 1049 | 5654 | 8.0% | inherited the 12s run's frozen end as its own start |
| 20s | 317 | 2412 | 0.2% | real step-loss trip before this run; clean after the recovery re-home |

### The real finding: `positionRequest` itself stops updating, not just the stepper

![8s run showing two multi-second freezes](tuning_runs/pid_observed_20260906_211427_dur8s.png)

The 8s run makes this unambiguous: **both** the "Commanded" position trace **and** the raw DDP value panel go flat together, for ~1.2s then ~3.4s, before both resume together. If this were a stepper/motion-control issue (like the two dead-`moveTo()` bugs fixed earlier today), the *commanded* trace - which is just `bottomPosition * receivedValue / 255`, nothing more - would keep updating even while actual position lagged behind. It doesn't. **`positionRequest` genuinely stops changing on the device for whole seconds at a time, then catches up.** This is a DDP reception-layer symptom, not anything downstream of it - none of today's `moveTo()`/hysteresis fixes are implicated.

The 12s/16s pair confirms the same thing a different way: the 12s run's own log ends frozen at the exact DDP value/position the 16s run then opens with - the freeze outlived one run's capture window and bled into the next, self-recovering only once the *next* wave's sending resumed.

**Two real candidate explanations, not yet distinguished:**
1. **Genuine WiFi packet loss**, real and sustained enough (many consecutive packets, not the odd single drop) that positionRequest has nothing new to parse for whole seconds. Different from the earlier, isolated sender-timing investigation (which proved 321/321 delivery on a short, clean bench test) - this could plausibly get worse over a long, hot, hours-into-testing session than in a short isolated check.
2. **A DDP sequence-validation edge case** (`isNewerDdpSeq()`'s 1-15 rolling window): if the device's `lastAcceptedDdpSeq` reference ever gets stuck relative to the sender's own count, every incoming packet is compared against a stale reference until the numbers happen to realign. Arithmetically this should self-resolve within about 15 packets (~375ms at 40Hz) for a normally-incrementing sequence, which is shorter than the 1.1-3.4s freezes actually observed - making this the less likely of the two, but not ruled out (e.g. if something also resets or double-consumes sequence numbers elsewhere).

**Not yet resolved**: the on-device counters that would settle this (`protocolPacketsReceived`, `protocolPacketsRejectedOutOfOrder`) reset with an intervening reboot (the harness closing its serial connection at the end of the run also toggles DTR/RTS, same as opening one) before they could be checked - so this run's own counters aren't recoverable after the fact. Confirming which explanation is right needs a live-monitored re-run: poll `/status-data`'s counters (HTTP, doesn't reboot anything) during a run long enough to catch a freeze in the act, or watch for `ddpPacketsRejectedOutOfOrder` climbing in lockstep with a freeze.

### The 5s run's poor numbers are a separate, already-understood, pre-existing limit

![5s run showing genuine speed-limited lag](tuning_runs/pid_observed_20260906_211427_dur5s.png)

Unlike the freezes above, the 5s run's error trace is smooth and continuous throughout - "Actual" is working the whole time, just can't keep up. A 5s full-cycle wave needs to cross the whole ~15,500-step range in 2.5s, requiring something like 6200Hz average speed against a 7000Hz cap - not much margin once acceleration/deceleration time is subtracted. The 5s run has been the worst of every duration tested all session, in every sweep - this is an inherent consequence of asking for a faster wave than the system's tuned speed ceiling supports, not a new regression.

## Re-run after the DDP reception instrumentation work (2026-09-06, `pid_v2_*`) — freeze did not recur, ripple visible but "smooth"

Same sweep again (`tuning_harness.py --config config_pid.json`, same gains/environment as `pid_final`/`pid_observed` above), run after this session's DDP reception instrumentation work (network-retrievable reception log, `WiFi.setSleep(false)`, and — earlier the same day — a dedicated combined motion+reception test that also came back clean; see `TODO.md`). The user watched this run directly and described it as "jerky but smooth."

| duration | rms_error | max_error | near_stall_pct | notes |
|---|---|---|---|---|
| 5s | 744 | 1501 | 1.5% | matches `pid_final`'s 747/1497 almost exactly |
| 8s | 437 | 1192 | 1.3% | matches `pid_final`'s 429/1103 |
| 12s | 283 | 752 | 0.9% | matches `pid_final`'s 279/564 |
| 16s | 209 | 297 | 0.0% | matches `pid_final`'s 213/442 |
| 20s | 169 | 230 | 0.0% | matches `pid_final`'s 173/352 |

**No freezes, no stuck episodes, no step loss, no re-homes needed anywhere in the sweep** — the skipped-step check before every run came back clean, and every number lines up with the original `pid_final` baseline almost digit-for-digit. The `pid_observed` regression (5s/8s/16s all badly degraded, a real 12-step loss, multi-second `positionRequest` freezes) did not reproduce.

### All 5 runs — same story every time: clean tracking, ripple underneath

![5s run](tuning_runs/pid_v2_20260906_220241_dur5s.png)

The 5s run is the one exception worth calling out separately: it's fast enough (full ~15,500-step range in 2.5s) that PID pegs at the 7000Hz speed cap for most of each leg (see the flat-topped **Actual Speed** trace) - genuinely speed-limited, not oscillating, the same pre-existing limit documented earlier in this doc. The ripple below shows up once the required speed drops back under the cap.

![8s run](tuning_runs/pid_v2_20260906_220241_dur8s.png)

![12s run](tuning_runs/pid_v2_20260906_220241_dur12s.png)

![16s run](tuning_runs/pid_v2_20260906_220241_dur16s.png)

![20s run](tuning_runs/pid_v2_20260906_220241_dur20s.png)

This is very likely what "jerky but smooth" is describing: the **Position** panel (top) tracks the commanded ramp cleanly with no visible discontinuity — genuinely smooth motion at the level a person watching the trolley would judge it. The **Actual Speed** panel (third) tells a different story underneath: a real, sustained oscillation riding on top of the average cruise speed, in both directions, the whole time it's moving (once below the 7000Hz cap - see the 5s note above). Small enough in position terms not to show up as visible jerkiness in the trace, but real enough to be audible/felt as roughness in the drivetrain (worm gear + motor) even while the net result looks smooth on a plot.

**Measured the ripple's period directly rather than eyeballing it** (peak-to-peak spacing in `curSpeedHz`, steady-cruise window, each run's own log):

| duration | mean cruise speed | ripple period |
|---|---|---|
| 5s | ~6200Hz | ~78ms |
| 8s | ~3800Hz | ~102ms |
| 12s | ~2600Hz | ~103ms |
| 16s | ~2000Hz | ~97ms |
| 20s | ~1600Hz | ~86ms |

**This settles what kind of bug it is.** The period stays pinned at ~80-100ms (roughly 10-13Hz) regardless of average cruise speed, even though cruise speed itself varies 4x across these runs. If the ripple were the DDP staircase (packets arriving every 25ms at 40fps, or 40ms at 25fps) bleeding through into motion, its period would lock to that packet-arrival interval or a clean multiple of it, and wouldn't care how fast the trolley happens to be moving. It does neither. A fixed ~10-13Hz buzz independent of commanded speed is the signature of a genuine **underdamped control-loop resonance** - `Kp=15`/`Kd=0.7` sampled every 20ms fighting the stepper's own accel-limited response - not the DDP frame rate leaking through into the motion.

**Practical conclusion: this is two separate problems, not one.**
1. **The ripple itself** is a gain/damping tuning problem, not a frame-rate problem - fix via a Kd/damping sweep (or a low-pass filter on the derivative estimate; derivative-on-measurement at a fixed 20ms tick is a classic noise amplifier) tested against this exact continuous triangle-wave signal, not the single-step-response test `Kp=15`/`Kd=0.7` were originally picked from (a genuinely different signal, as this session already learned once with Bugs 1-2 above).
2. **The original design-goal mismatch** (small moves always racing to target instead of pacing to the time actually available) is real, separate, and still needs the frame-rate/time-budget-adaptive feedforward idea flagged earlier - but that fix targets pacing philosophy, not this specific fixed-frequency resonance, so it's not expected to be a fix for the ripple on its own.

Recommended next step: a same-day Kd/Kp damping sweep against the triangle-wave test (cheap, reuses existing tooling and this exact data as a baseline) before taking on the bigger structural feedforward work.

**On the freeze bug specifically**: this clean result, plus the same-day combined motion+reception test (also clean — see `TODO.md`), is encouraging but still not proof it's fixed, since nothing that directly touches DDP receive/parse logic changed. `WiFi.setSleep(false)` remains the leading candidate explanation if it stays clean across more/longer runs.

## Where things landed

**Working PID gains**: `Kp=15, Ki=0, Kd=0.7, MaxSpeed=7000, Accel=50000, Deadband=30, ReengageThreshold=150`. **Environment**: `1200mA run current, 200,000 accel, 7000Hz cruise cap` (current and accel saved via the web UI; the 7000Hz speed cap is a live override only — see below).

## Still open

- **Multi-second `positionRequest` freeze — not reproduced since, but not confirmed fixed.** Two clean re-runs since (a dedicated combined motion+reception test, and the full duration sweep above) found zero freezes, but nothing that directly touches DDP receive/parse logic was changed - `WiFi.setSleep(false)` is the leading candidate if it stays clean across more/longer sessions. See `TODO.md`'s DDP reception instrumentation sections for full detail.
- A real, bounded, visible high-frequency ripple remains in actual speed during active tracking (~1000–4500Hz, ~0.2–0.3s period, both directions) — doesn't hurt overall accuracy and the position trace itself looks clean (see the `pid_v2` re-run above - user's own read watching it live was "jerky but smooth"), but isn't fully explained; likely Kd reacting to rate-estimate noise on a live, continuously-updating target rather than a clean single step.
- Ki hasn't been tuned — the deadband snap already closes any P/PD steady-state gap for a single step, so this needs a genuinely different test (sustained tracking of a moving target, not step response) to matter.
- `pidReengageThreshold` / the ram-check's tolerance (150, matched to each other) were chosen from the DDP-quantization math, not an independent sweep.
- `normalSpeed=7000Hz` is currently a live override only, not saved — a reboot reverts it to the saved 8000Hz until it's explicitly saved via the web UI or a different number is settled on.
- The speed-vs-current sweep was never re-run below 800mA, and StallGuard's re-enable/replace decision (flagged in the previous session's summary) is still open.
- The deadband-settled branch's own first `moveTo()` (the initial snap into `pidSettled`) doesn't have the same dead-move retry the hysteresis band now has - lower risk (it only fires once error is already inside the tight deadband, so a dead move there barely matters) but not verified clean the same rigorous way.
