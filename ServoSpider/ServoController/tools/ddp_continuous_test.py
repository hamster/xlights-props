#!/usr/bin/env python3
"""
Reproduces DDPDebugger's actual continuous-loop triangle wave (not a
single up-down cycle like tuning_harness.py's send_triangle_wave()) so
real-world reversal bugs can be caught without needing a human to run
DDPDebugger by hand - built 2026-09-07 after two real firmware bugs
(a pidStopSettling wait using an unreliable isRunning() check, and the
PID overshoot-safety net re-triggering every tick on a benign few-step
excursion) only showed up against DDPDebugger's actual continuous
multi-cycle wave, not tuning_harness.py's single-cycle-per-run tests.

Matches DDPDebugger's DdpSender.java exactly: value = triangleValue(
elapsedSeconds % period / period), i.e. the wave genuinely repeats
continuously for as long as the test runs, not "one cycle then hold" -
see triangleValue() in DdpSender.java. Also matches its jitter
characteristics (value computed from real elapsed time at send, not a
schedule) rather than tuning_harness.py's cleaned-up scheduled-tick
sender - deliberately, since the point here is fidelity to the real
tool, not the cleanest possible signal.

Configuration and log retrieval both go over HTTP (GET /tunable, GET
/compact-log) - no serial connection at all, so this can run repeatedly
without any risk of resetting the device mid-test (opening a serial
connection toggles DTR/RTS - see CLAUDE.md).

Usage:
  python ddp_continuous_test.py --ddp-host 192.168.10.181 --period 8 \
      --duration 60 --config config_pid_ff_kp3.json
"""
import argparse
import json
import socket
import struct
import sys
import time
import urllib.request

DDP_PORT = 4048
DDP_HEADER_FMT = ">BBBBIH"
DDP_FLAGS_PUSH_V1 = 0x41
DDP_DATA_TYPE_RGB = 0x01
DDP_DESTINATION = 1


def http_get(host, path, timeout=8.0):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as resp:
        return resp.read().decode("utf-8", errors="replace")


def http_set_tunable(host, name, value):
    body = http_get(host, f"/tunable?name={name}&value={value}")
    result = json.loads(body)
    if not result.get("success"):
        raise RuntimeError(f"HTTP set {name}={value} failed: {result}")
    return result


def get_status(host):
    body = http_get(host, "/status-data")
    return json.loads(body)


def build_ddp_packet(seq, value, bits=8):
    """Stepper position is always at byte offset 0. 8-bit sends one byte;
    16-bit sends two, MSB first - matching ddp_handler.cpp's parse under
    control16BitConfig (see its byteOffsetInPacket block)."""
    if bits == 16:
        payload = bytes([(value >> 8) & 0xFF, value & 0xFF])
    else:
        payload = bytes([value & 0xFF])
    header = struct.pack(DDP_HEADER_FMT, DDP_FLAGS_PUSH_V1, seq, DDP_DATA_TYPE_RGB,
                          DDP_DESTINATION, 0, len(payload))
    return header + payload


def triangle_value_wrapped(elapsed_s, period_s, bits=8):
    """Matches DdpSender.java's triangleValue() exactly: wraps continuously
    via elapsed % period, not a single ramp that holds after `period`.

    The wave itself is identical at either bit depth - only the quantization
    of each sample changes (256 levels vs 65536). That distinction matters
    for more than position resolution: updatePidMode()'s velocity feedforward
    estimates rate from consecutive *target* changes, so at 8-bit it is
    differentiating a signal that only ever moves in ~60-step jumps on this
    device, which injects real noise into the commanded speed."""
    period = max(0.01, period_s)
    phase = (elapsed_s % period) / period  # [0, 1)
    full = 65535.0 if bits == 16 else 255.0
    if phase < 0.5:
        value = phase * 2.0 * full
    else:
        value = (1.0 - phase) * 2.0 * full
    return max(0, min(int(full), int(round(value))))


# Rows the device's 96KB compact-log ring buffer holds before it wraps, at
# ~52 bytes/row. Runs must fit inside this, because the alternative - draining
# mid-run - blocks loop() for 0.5-1.0s while serving the response, during which
# a continuous-run stepper keeps cruising completely unsupervised. That is not a
# theoretical concern: it drove the trolley 2400-3700 steps past the bottom on
# every single run that used mid-run draining, and caused a real step loss when
# one of those blind windows happened to coincide with the top of travel.
COMPACT_LOG_ROW_CAPACITY = 1890
COMPACT_LOG_ROWS_PER_SEC = 50.0


def verify_zero(host, home_timeout, label, rate_hz=40.0, bits=8):
    """Command position 0 via /set-position (NOT DDP/PID), wait for the
    trolley to genuinely stop, then check the raw homing switch state
    directly - not through /verify-and-rehome, see below for why.

    /verify-and-rehome (startStepCheck(0, autoRehomeOnTrip=true)) is built for
    a different situation: confirming calibration from a position ALREADY
    believed to be near zero (its own comment: "so FPP can call one HTTP
    request before/between shows"). From there, a trip is unexpected and
    really does mean drift. Called after a full-travel test, though, the
    trolley starts a whole travel away from zero - reaching it necessarily
    drives into the switch, so a trip is the NORMAL, correct outcome of
    successfully getting there, not evidence of anything wrong. Measured this
    directly (2026-09-07): a run with independently-clean encoder-vs-counter
    agreement (drift -1 step) still got flagged "STEPS WERE LOST" by that
    endpoint and forced into an unnecessary ~20s re-home, every single time it
    was called this way.

    A first version of this held DDP value 0 (i.e. drove to zero through
    TRACK_MODE_PID, the very system under test) and read the switch once
    position looked close enough. That coupled verification to whatever
    pidKp/pidKd/pidFfWindowMs the run under test was using: a Kd sweep run
    where the final micro-approach to the switch settled a little
    differently (still well within the position deadband, but not always
    hard against the switch) got flagged "steps were lost" on 3 of 3
    candidates - right after 5 consecutive clean verifies at the one config
    that happened to be tuned for exactly this. That is not a step-loss
    signature, it is the test contaminating itself.

    /set-position (handleSetPosition(), html_handler.cpp) is a plain
    moveTo() on stepperSpeedConfig/stepperAccelConfig - it does not touch
    trackMode or any PID state, so the approach to zero is now identical
    regardless of which gains are under test. Waits for isRunning() to
    clear (a real stop) rather than a position tolerance, since a moveTo()
    finishes exactly where it's told rather than settling somewhere nearby.
    Returns (ok, rehomed).
    """
    print(f"  [{label}] drive to zero, confirm switch trips...")

    # Force Direct mode for the verify move itself, then always restore
    # whatever mode was active. Necessary, not just tidy: TRACK_MODE_PID's
    # updatePidMode() runs on EVERY loop() iteration regardless of what
    # triggered motion (see stepper_handler.h's dispatch comment - "no
    # per-command handler"). Issuing /set-position while trackMode stays 4
    # would get immediately overridden on the next tick by PID reacting to
    # whatever positionRequest last held from the just-finished wave -
    # /set-position's moveTo() would never even get a chance to run to
    # completion. Direct mode has no background updater (it only reacts to
    # an actual DDP packet), so once switched, nothing contends with the
    # manual move.
    try:
        orig_mode = json.loads(http_get(host, "/tunable?name=trackMode")).get("value")
    except Exception as e:
        print(f"    WARNING: could not read trackMode: {e}")
        return (False, False)

    try:
        http_set_tunable(host, "trackMode", 0)
        try:
            http_get(host, "/set-position?position=0", timeout=10.0)
        except Exception as e:
            print(f"    WARNING: /set-position failed: {e}")
            return (False, False)

        # status-data has no isRunning/isMoving field, so "stopped" is
        # inferred from position going unchanged between polls, not a flag.
        # A minimum elapsed-time floor before that check can pass avoids
        # reading "unchanged" at t=0, before the move has even started.
        MIN_MOVE_S = 1.0
        deadline = time.monotonic() + 30.0
        move_start = time.monotonic()
        st = get_status(host)
        last_pos = st.get("position")
        settled = False
        while time.monotonic() < deadline:
            time.sleep(0.4)
            st = get_status(host)
            pos = st.get("position")
            if time.monotonic() - move_start >= MIN_MOVE_S and pos == last_pos:
                settled = True
                break
            last_pos = pos
        if not settled:
            print(f"    WARNING: stepper never stopped (at {st.get('position')}) - skipping switch check")
            return (False, False)

        if st.get("homingSwitchTripped"):
            print(f"    ok - switch tripped as expected, pos={st.get('position')} "
                  f"enc={st.get('encoderCount')}")
            return (True, False)

        print(f"    *** AT POSITION 0 BUT SWITCH NOT TRIPPED - real drift. Re-homing. ***")
        http_get(host, "/home")
        home_deadline = time.monotonic() + home_timeout
        while time.monotonic() < home_deadline:
            time.sleep(1.0)
            st = get_status(host)
            if st.get("homed") and not st.get("isHoming"):
                print(f"    re-homed - pos={st.get('position')} enc={st.get('encoderCount')}")
                return (True, True)
        print("    WARNING: re-home did not complete in time")
        return (False, True)
    finally:
        if orig_mode is not None:
            try:
                http_set_tunable(host, "trackMode", orig_mode)
            except Exception as e:
                print(f"    WARNING: could not restore trackMode={orig_mode}: {e}")


def run(args):
    # Per-run protocol (2026-09-07, at the user's suggestion): each run is
    # self-contained and verified at BOTH ends, so a run that loses steps is
    # caught immediately instead of silently corrupting the zero reference for
    # every run that follows it - which is exactly how one bad run earlier
    # tonight invalidated the clean-looking run right after it.
    #
    #   1. wait for genuine idle (a previous run's verify/re-home can still be
    #      in flight - `homed` stays true throughout one, so a plain homed
    #      check does not catch it)
    #   2. verify zero: drive to 0, confirm the switch trips, re-home if not
    #      (this doubles as pre-positioning - every wave here starts at value
    #      0, so there is no separate startup transient to trim)
    #   3. run the wave - NO mid-run log access, see COMPACT_LOG_ROW_CAPACITY
    #      above for why: draining mid-run blocks loop() for up to ~1s, and a
    #      continuous-run stepper left unsupervised for that long can - and
    #      did - cruise straight past the bottom of travel
    #   4. fetch the log
    #   5. verify zero again, re-home if needed - this is what actually
    #      catches a run that lost steps, not the arithmetic drift check
    #      this replaced (see verify_zero()'s own comment for why a direct
    #      switch read beats /verify-and-rehome for this use pattern)
    print(f"Applying config to {args.ddp_host}...")
    if args.config:
        with open(args.config) as f:
            config = json.load(f)
        print(f"Applying {len(config)} tunable(s) via HTTP...")
        for name, value in config.items():
            print(f"  {name} = {value}: {http_set_tunable(args.ddp_host, name, value)}")

    http_set_tunable(args.ddp_host, "compactLog", 1)

    print("Waiting for device to go idle (no check/home in flight)...")
    idle_deadline = time.monotonic() + 120.0
    while time.monotonic() < idle_deadline:
        st = get_status(args.ddp_host)
        if not st.get("isChecking") and not st.get("isHoming"):
            break
        time.sleep(1.0)
    else:
        print("  WARNING: device still busy after 120s")

    print("Pre-run zero verification:")
    verify_zero(args.ddp_host, args.home_timeout, "pre", args.rate_hz, args.bits)

    http_get(args.ddp_host, "/compact-log?clear=1")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"\nSending {args.duration:.0f}s continuous triangle wave, period={args.period:.1f}s, "
          f"{args.rate_hz:.0f}fps, to {args.ddp_host}:{DDP_PORT} (real-elapsed-time value, "
          f"matching DDPDebugger's own sender - not a cleaned-up schedule)...")

    interval = 1.0 / args.rate_hz
    start = time.monotonic()
    seq = 0
    sent = 0
    while True:
        now = time.monotonic()
        elapsed = now - start
        if elapsed >= args.duration:
            break
        value = triangle_value_wrapped(elapsed, args.period, args.bits)
        seq = (seq % 15) + 1
        sock.sendto(build_ddp_packet(seq, value, args.bits), (args.ddp_host, DDP_PORT))
        sent += 1
        time.sleep(interval)
    sock.close()
    print(f"Sent {sent} packets over {time.monotonic()-start:.1f}s")

    time.sleep(1.5)  # let the last few PID ticks settle before fetching
    log_text = http_get(args.ddp_host, "/compact-log", timeout=15.0)
    lines = [l for l in log_text.splitlines() if l]
    expected = int(args.duration * COMPACT_LOG_ROWS_PER_SEC)
    print(f"Fetched {len(lines)} compact-log rows over HTTP (~{expected} expected at 20ms ticks)")
    if len(lines) < expected * 0.85:
        print(f"  WARNING: got {100.0*len(lines)/expected:.0f}% of expected rows - "
              f"coverage may still be incomplete, treat 'no stalls' with caution")

    out_path = args.out.rsplit(".", 1)[0] + ".log"
    with open(out_path, "w") as f:
        f.write(log_text)
    print(f"Wrote {out_path}")

    print("\nPost-run zero verification:")
    ok, rehomed = verify_zero(args.ddp_host, args.home_timeout, "post", args.rate_hz, args.bits)
    if rehomed:
        print("  *** This run's zero reference drifted (switch did not trip where "
              "expected) - its numbers may be measured against a corrupted "
              "reference. Treat them with caution. ***")

    analyze(lines, args)


def analyze(lines, args):
    from tuning_harness import parse_row
    rows = [r for r in (parse_row(l) for l in lines) if r is not None]
    print(f"\nParsed {len(rows)} valid rows")
    if not rows:
        return

    # Drop a leading malformed/truncated row if present - see
    # analyze_ripple.py's matching fix for the full story (seen at
    # pidTickMs below 20, in at least three different corrupted shapes -
    # "0,0", "08,663,0,0", "06,53586,..." - never at the original 20ms tick
    # across ~25 runs). A genuine first row's ms is comfortably into 4+
    # digits by the time a run starts; a truncated line's padded/mis-parsed
    # ms is not. Most of this file's own metrics (jerk, frame lag, encoder
    # ground truth, real-stall detection) don't depend on ms at all and
    # were already unaffected, but corner-tightness's reversal-window
    # bucketing does use ms, so this is worth doing here too, not just in
    # analyze_ripple.py.
    while len(rows) > 1 and rows[0]["ms"] < 1000 and rows[1]["ms"] - rows[0]["ms"] > 5000:
        print(f"  NOTE: dropped a leading malformed row (ms={rows[0]['ms']})")
        rows = rows[1:]

    # Discard the startup transient (2026-09-07). Each run begins with the
    # trolley wherever the PREVIOUS run's wave happened to leave it, while the
    # new wave always starts at value 0 - so the first moments are a one-off
    # full-range dash that has nothing to do with tracking quality. It shows up
    # as a max_error of roughly a whole travel (~15,000 steps) and badly skews
    # rms_error and corner tightness.
    #
    # This was invisible until mid-run log draining was added: the old
    # single-fetch-at-the-end only returned the last ~40s of a 60s run, which
    # silently excluded the transient. Now that the full run is captured, it has
    # to be excluded deliberately instead of by accident.
    warmup_ms = args.warmup * 1000.0
    t0 = rows[0]["ms"]
    kept = [r for r in rows if r["ms"] - t0 >= warmup_ms]
    if kept and len(kept) < len(rows):
        print(f"  (dropped {len(rows)-len(kept)} warm-up rows, first {args.warmup:.0f}s "
              f"- trolley starting from wherever the last run parked it)")
        rows = kept

    # --- Encoder ground truth ------------------------------------------------
    # Runs FIRST, and loudly, because every other metric below is computed from
    # the firmware's own step counter - which keeps incrementing while a stalled
    # motor sits still, so it reads clean during exactly the failure it most
    # needs to catch. The encoder is the only independent physical measurement.
    #
    # A dead encoder does not error; it just returns a constant (all zeros, or
    # frozen at some stale value). On 2026-09-07 it died silently and ~35 runs
    # were reported "zero stalls" on the strength of a counter that could not
    # see stalls, while a real runaway drove the trolley ~11,900 steps past the
    # top switch. Hence: constant encoder == results are not trustworthy, say so
    # rather than printing clean-looking numbers.
    STEPS_PER_COUNT = 10.0  # documented worm ratio; measured 9.9-10.2 on the bench
    encs = [r.get("encoderCount", 0) for r in rows]
    enc_distinct = len(set(encs))
    enc_span = max(encs) - min(encs)
    pos_span = max(r["curPos"] for r in rows) - min(r["curPos"] for r in rows)
    print("\n=== ENCODER GROUND TRUTH ===")
    if enc_distinct <= 2 and enc_span < 10:
        print(f"  *** ENCODER DEAD - {enc_distinct} distinct value(s), span {enc_span} ***")
        print(f"  *** Every metric below is step-counter-only and CANNOT detect a stall. ***")
        print(f"  *** Fix the encoder before trusting any result from this run. ***")
    else:
        ratio = (pos_span / enc_span) if enc_span else float("nan")
        print(f"  live: {enc_distinct} distinct values, span {enc_span} counts "
              f"vs {pos_span} steps -> {ratio:.2f} steps/count (expect ~10)")
        if not (8.5 <= ratio <= 12.0):
            print(f"  WARNING: steps/count {ratio:.2f} is outside the expected band - "
                  f"real step loss, or an encoder reading only part of the travel")

        # Real stall detection: the step counter advanced but the encoder did
        # not. This is the check that was structurally impossible without a live
        # encoder, and the one that would have caught the 2026-09-07 runaway.
        WIN = 10           # ~200ms at 20ms ticks
        MIN_STEPS = 400    # only judge windows where real motion was claimed
        stalls = []
        for i in range(WIN, len(rows)):
            d_pos = abs(rows[i]["curPos"] - rows[i - WIN]["curPos"])
            d_enc = abs(encs[i] - encs[i - WIN])
            if d_pos >= MIN_STEPS and d_enc * STEPS_PER_COUNT < d_pos * 0.5:
                stalls.append((rows[i - WIN]["ms"], d_pos, d_enc))
        merged = []
        for ms, d_pos, d_enc in stalls:
            if merged and ms - merged[-1][0] <= 400:
                continue
            merged.append((ms, d_pos, d_enc))
        print(f"  REAL stalls (counter moved, encoder did not): {len(merged)}")
        for ms, d_pos, d_enc in merged[:8]:
            print(f"      at ms={ms}: counter +{d_pos} steps but encoder only "
                  f"+{d_enc} counts (~{d_enc*STEPS_PER_COUNT:.0f} steps) in {WIN*20}ms")

    ts = [r["ms"] for r in rows]
    ooo = sum(1 for i in range(1, len(ts)) if ts[i] < ts[i - 1])
    if ooo:
        print(f"  WARNING: {ooo} out-of-order timestamp(s) - buffer may have wrapped mid-fetch")

    # Real "dead" stall detector: curSpeedHz==0 for a real stretch while a
    # real nonzero target speed is commanded - the exact signature both
    # bugs this tool was built to catch produce (pidStopSettling hang,
    # overshoot-check retrigger loop).
    stall_events = []
    run_start = None
    for i, r in enumerate(rows):
        stalled_tick = (r["curSpeedHz"] == 0 and r["targetSpeedHz"] != 0)
        if stalled_tick:
            if run_start is None:
                run_start = i
        else:
            if run_start is not None:
                span_ms = rows[i - 1]["ms"] - rows[run_start]["ms"]
                if span_ms > 150:
                    stall_events.append((rows[run_start]["ms"], span_ms, rows[run_start]["curPos"]))
                run_start = None
    print(f"\n=== STALL EVENTS (curSpeedHz==0 while targetSpeedHz!=0, >150ms) ===")
    print(f"  count: {len(stall_events)}")
    for ms, span, pos in stall_events:
        print(f"    at device ms={ms}, lasted {span}ms, pos={pos}")

    neg_excursions = [r for r in rows if r["curPos"] < 0]
    over_excursions = [r for r in rows if r["curPos"] > 20000]  # generous, device-agnostic sanity bound
    print(f"\n=== BOUNDS ===")
    print(f"  rows with curPos < 0: {len(neg_excursions)}"
          + (f" (min {min(r['curPos'] for r in neg_excursions)})" if neg_excursions else ""))
    print(f"  rows with curPos > 20000: {len(over_excursions)}")

    lags = [abs(r["lag"]) for r in rows]
    print(f"\n=== TRACKING ===")
    print(f"  rms_error: {(sum(x*x for x in lags)/len(lags))**0.5:.1f}  max_error: {max(lags)}")

    # Jerk/roughness: mean |consecutive curSpeedHz delta| - same metric
    # tuning_harness.py's compute_metrics() uses, for direct comparability
    # against every prior sweep this project has run.
    speeds = [r["curSpeedHz"] for r in rows]
    jerk = sum(abs(speeds[i] - speeds[i - 1]) for i in range(1, len(speeds))) / max(1, len(speeds) - 1)
    print(f"  jerk_per_sample: {jerk:.1f}")

    # Frame lag: how many 40fps frames "behind" the commanded position
    # actual currently is, estimated as |error| / (local commanded speed
    # in steps/frame) - i.e. "how long, at the rate the target is actually
    # moving right now, would it take actual to reach where commanded
    # already is". Undefined/skipped at very low commanded speed (near a
    # reversal or hold) - lag in steps there doesn't mean lag in *time*.
    FRAME_S = 1.0 / 40.0
    frame_lags = []
    for r in rows:
        speed_steps_per_s = abs(r["targetSpeedHz"])
        if speed_steps_per_s < 200:  # near-zero commanded rate - steps/frame not meaningful
            continue
        steps_per_frame = speed_steps_per_s * FRAME_S
        frame_lags.append(abs(r["lag"]) / steps_per_frame)
    if frame_lags:
        frame_lags.sort()
        n = len(frame_lags)
        print(f"\n=== FRAME LAG (at 40fps, only while target is actually moving) ===")
        print(f"  mean: {sum(frame_lags)/n:.1f}  p95: {frame_lags[int(n*0.95)]:.1f}  max: {frame_lags[-1]:.1f}  "
              f"(budget: prefer <5, hard cap 20)")
        over_budget = sum(1 for f in frame_lags if f > 20)
        print(f"  samples over the 20-frame hard cap: {over_budget}/{n} ({100.0*over_budget/n:.1f}%)")

    # Corner tightness: max |error| within +/-0.75s of each detected
    # reversal (a local min or max in ddpVal) - the specific "how tight are
    # the corners" question, separate from overall rms_error which mixes
    # in the (usually cleaner) straight-leg tracking too.
    # Real reversals only - a genuine triangle-wave peak/trough sits AT or
    # very near the DDP value's own boundary (0 or 255), not just any
    # local wiggle in the interior (a naive tight-window min/max detector
    # picked up 126 "reversals" from ordinary sender jitter in a 5-cycle
    # test - not useful). De-duplicated by time so one real reversal
    # (which can span several ticks sitting near the boundary) counts once.
    # Scale the boundary test to the actual DDP width in use (8- vs 16-bit)
    # rather than hardcoding 8-bit's 0..255 - inferred from the data so an
    # existing 8-bit log still analyses identically.
    ddp_full = 65535 if max(r["ddpVal"] for r in rows) > 255 else 255
    ddp_lo = ddp_full * 3 // 255
    ddp_hi = ddp_full - ddp_lo
    ddp_boundary_idxs = [i for i, r in enumerate(rows) if r["ddpVal"] <= ddp_lo or r["ddpVal"] >= ddp_hi]
    reversal_ms = []
    last_ms = None
    for i in ddp_boundary_idxs:
        ms = rows[i]["ms"]
        if last_ms is None or ms - last_ms > 1000:  # new reversal, not the same one still near the boundary
            reversal_ms.append(ms)
        last_ms = ms
    corner_errors = []
    for t_center in reversal_ms:
        window_rows = [r for r in rows if abs(r["ms"] - t_center) <= 750]
        if window_rows:
            corner_errors.append(max(abs(r["lag"]) for r in window_rows))
    if corner_errors:
        print(f"\n=== CORNER TIGHTNESS (max |error| within +/-0.75s of each of {len(reversal_ms)} real reversals) ===")
        print(f"  mean: {sum(corner_errors)/len(corner_errors):.1f}  max: {max(corner_errors)}")
        print(f"  per-corner: {[round(c) for c in corner_errors]}")

    make_plot(rows, args)


def make_plot(rows, args):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available - skipping plot")
        return

    t0 = rows[0]["ms"]
    times = [(r["ms"] - t0) / 1000.0 for r in rows]

    fig, axes = plt.subplots(3, 1, figsize=(16, 10), sharex=True)
    axes[0].plot(times, [r["cmdPos"] for r in rows], label="Commanded", linewidth=0.8)
    axes[0].plot(times, [r["curPos"] for r in rows], label="Actual", linewidth=0.8)
    axes[0].axhline(0, color="gray", linewidth=0.5)
    axes[0].legend()
    axes[0].set_ylabel("Position (steps)")
    axes[0].set_title(f"{args.label} - continuous triangle, period={args.period:.1f}s")

    axes[1].plot(times, [r["curSpeedHz"] for r in rows], linewidth=0.6)
    axes[1].set_ylabel("Actual speed (Hz)")

    axes[2].plot(times, [r["ddpVal"] for r in rows], linewidth=0.6, color="green")
    axes[2].set_ylabel("Raw DDP value")
    axes[2].set_xlabel("Time (s)")

    fig.tight_layout()
    fig.savefig(args.out, dpi=110)
    print(f"\nWrote {args.out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ddp-host", required=True)
    parser.add_argument("--period", type=float, default=8.0, help="Triangle wave period in seconds")
    parser.add_argument("--duration", type=float, default=None,
                        help="Total test duration in seconds. Normally leave unset "
                             "and use --trips, which sizes the run to the period.")
    parser.add_argument("--trips", type=float, default=4.0,
                        help="Full triangle cycles to run. Duration = trips * period. "
                             "4 keeps a run inside the device log's ring buffer for "
                             "periods up to ~9s, so the log never has to be drained "
                             "mid-run (which blinds the control loop).")
    parser.add_argument("--rate-hz", type=float, default=40.0)
    parser.add_argument("--bits", type=int, choices=(8, 16), default=8,
                        help="DDP stepper channel width. Must match the device's "
                             "control16Bit setting (set it via the config file's "
                             "control16Bit key, or the web UI).")
    parser.add_argument("--config", default=None, help="JSON file of {tunableName: value} to apply via HTTP first")
    parser.add_argument("--home-timeout", type=float, default=90.0)
    parser.add_argument("--warmup", type=float, default=0.0,
                        help="Seconds of run start to exclude from metrics - the "
                             "one-off dash from wherever the previous run parked "
                             "the trolley to wherever this wave begins.")
    parser.add_argument("--label", default="ddp_continuous")
    parser.add_argument("--out", default="tuning_runs/ddp_continuous_test.png")
    args = parser.parse_args()
    if args.duration is None:
        args.duration = args.trips * args.period
    est_rows = args.duration * COMPACT_LOG_ROWS_PER_SEC
    if est_rows > COMPACT_LOG_ROW_CAPACITY:
        fits = COMPACT_LOG_ROW_CAPACITY / COMPACT_LOG_ROWS_PER_SEC
        print(f"WARNING: {args.duration:.0f}s (~{est_rows:.0f} rows) exceeds the device "
              f"log's ~{COMPACT_LOG_ROW_CAPACITY}-row buffer (~{fits:.0f}s). The run "
              f"will be captured from its TAIL only, hiding anything earlier. "
              f"Use --trips {max(1, int(fits/args.period))} or fewer at this period.")
    run(args)


if __name__ == "__main__":
    main()
