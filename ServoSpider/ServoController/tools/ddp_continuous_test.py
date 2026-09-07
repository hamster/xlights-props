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
import threading
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


def ensure_homed(host, timeout=90.0):
    status = get_status(host)
    if status.get("homed"):
        return True
    http_get(host, "/home")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(1.0)
        status = get_status(host)
        if status.get("homed"):
            return True
        if not status.get("isHoming"):
            # Homing ended without success (and isn't running) - real failure.
            return False
    return False


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


def run(args):
    print(f"Ensuring {args.ddp_host} is homed...")
    if not ensure_homed(args.ddp_host, timeout=args.home_timeout):
        print("ERROR: homing did not complete", file=sys.stderr)
        sys.exit(1)

    if args.config:
        with open(args.config) as f:
            config = json.load(f)
        print(f"Applying {len(config)} tunable(s) via HTTP...")
        for name, value in config.items():
            print(f"  {name} = {value}: {http_set_tunable(args.ddp_host, name, value)}")

    http_set_tunable(args.ddp_host, "compactLog", 1)

    # Make each run independent of the one before it (2026-09-07).
    #
    # Two ways runs were contaminating each other, both of which only became
    # visible once mid-run log draining exposed the full 60s instead of the
    # last 40s:
    #
    # 1. The previous run's post-check (/verify-and-rehome, which drives to 0
    #    and may trigger a full re-home) could still be in flight when the next
    #    wave started. ensure_homed() does not catch this - `homed` stays true
    #    throughout a verify - so the new run's opening seconds were measured
    #    while the device was still executing the old run's cleanup.
    # 2. Even idle, the trolley sits wherever the previous wave's final value
    #    left it, while every wave starts at value 0. That one-off full-range
    #    dash was landing in the metrics as a ~15,000-step max_error and
    #    wrecking rms_error and corner tightness.
    #
    # Fix both at the source rather than trimming afterwards: wait for the
    # device to be genuinely idle, then hold the wave's own t=0 value until the
    # trolley actually gets there, and only then clear the log and start. The
    # captured run then begins from the correct position with no transient, so
    # no warm-up trimming is needed and every candidate starts identically.
    print("Waiting for device to go idle (no check/home in flight)...")
    idle_deadline = time.monotonic() + 120.0
    while time.monotonic() < idle_deadline:
        st = get_status(args.ddp_host)
        if not st.get("isChecking") and not st.get("isHoming"):
            break
        time.sleep(1.0)
    else:
        print("  WARNING: device still busy after 120s")

    start_value = triangle_value_wrapped(0.0, args.period, args.bits)
    pre_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"Pre-positioning to the wave's t=0 value ({start_value})...")
    pre_seq = 0
    pos_deadline = time.monotonic() + 30.0
    settled_reads = 0
    while time.monotonic() < pos_deadline:
        for _ in range(10):  # keep the stream alive; PID needs a live target
            pre_seq = (pre_seq % 15) + 1
            pre_sock.sendto(build_ddp_packet(pre_seq, start_value, args.bits),
                            (args.ddp_host, DDP_PORT))
            time.sleep(1.0 / args.rate_hz)
        st = get_status(args.ddp_host)
        target_pos = st.get("bottomPosition", 0) * start_value / (65535.0 if args.bits == 16 else 255.0)
        if abs(st.get("position", 0) - target_pos) <= 200:
            settled_reads += 1
            if settled_reads >= 2:
                print(f"  in position ({st.get('position')} vs target {target_pos:.0f})")
                break
        else:
            settled_reads = 0
    else:
        st = get_status(args.ddp_host)
        print(f"  WARNING: did not reach start position (at {st.get('position')})")
    pre_sock.close()

    http_get(args.ddp_host, "/compact-log?clear=1")

    # Baseline for the drift gate below - taken after config is applied and
    # the device is settled, immediately before any motion.
    STEPS_PER_COUNT = 10.0
    _pre = get_status(args.ddp_host)
    pre_pos = _pre.get("position", 0)
    pre_enc = _pre.get("encoderCount", 0)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"\nSending {args.duration:.0f}s continuous triangle wave, period={args.period:.1f}s, "
          f"{args.rate_hz:.0f}fps, to {args.ddp_host}:{DDP_PORT} (real-elapsed-time value, "
          f"matching DDPDebugger's own sender - not a cleaned-up schedule)...")

    # Drain the device's Compact Motion Log DURING the run, not just once at
    # the end (2026-09-07 evening, found the hard way). The device-side ring
    # buffer is 96KB, which at ~50 rows/sec x ~52 bytes holds only about 40
    # seconds of active tracking - so a single fetch after a 60s run silently
    # returned only the LAST ~40s, with the first ~20s already overwritten.
    # Every "zero stalls" result from a >40s run was therefore scoped to the
    # tail of that run without saying so, which is exactly where a real
    # early-run freeze would hide.
    #
    # Runs on its own thread so the DDP send loop keeps its cadence - a
    # blocking HTTP fetch on the send thread would drop packets and corrupt
    # the very test being measured. Fetch-then-clear loses whatever few rows
    # land between the two calls (a ~10ms window, so 1-2 rows per drain);
    # that is a far better trade than losing a third of the run.
    drained = []
    drain_stop = threading.Event()

    def drain_loop():
        while not drain_stop.is_set():
            if drain_stop.wait(args.drain_interval):
                break
            try:
                chunk = http_get(args.ddp_host, "/compact-log", timeout=15.0)
                http_get(args.ddp_host, "/compact-log?clear=1", timeout=10.0)
                if chunk:
                    drained.append(chunk)
            except Exception as e:  # a failed drain must not kill the run
                print(f"  (drain failed, continuing: {e})")

    drain_thread = threading.Thread(target=drain_loop, daemon=True)
    drain_thread.start()

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
    print(f"Sent {sent} packets over {time.monotonic()-start:.1f}s")

    drain_stop.set()
    drain_thread.join(timeout=20.0)

    # Encoder-vs-counter drift gate (2026-09-07). A mid-travel stall makes
    # FastAccelStepper's counter diverge from physical reality permanently -
    # the zero reference is then wrong for every LATER run in the sweep too,
    # which is exactly how one bad p5 run silently invalidated the p8 run that
    # followed it (p5 drifted +3768 steps; p8 then tracked to -1 step but did
    # it all ~3700 steps past where the firmware believed it was, running the
    # trolley past the bottom and part way up the back of the pulley).
    # Neither firmware guard catches this: one keys off the counter, which is
    # the thing that lies, and the other off the top switch, which a mid-travel
    # stall never touches. So verify here and re-home before continuing rather
    # than carrying a corrupt reference into the next run.
    post = get_status(args.ddp_host)
    drift = None
    try:
        d_pos = post["position"] - pre_pos
        d_enc = (post["encoderCount"] - pre_enc) * STEPS_PER_COUNT
        drift = d_enc - d_pos
        print(f"\nEncoder/counter drift this run: {drift:+.0f} steps "
              f"(counter {d_pos:+d}, encoder {d_enc:+.0f})")
    except (KeyError, TypeError):
        print("\nWARNING: could not read encoder drift - status fields missing")



    time.sleep(1.5)  # let the last few PID ticks settle before the final fetch
    log_text = "".join(drained) + http_get(args.ddp_host, "/compact-log", timeout=15.0)
    lines = [l for l in log_text.splitlines() if l]
    expected = int(args.duration * 50)
    print(f"Fetched {len(lines)} compact-log rows over HTTP "
          f"({len(drained)} mid-run drain(s); ~{expected} expected at 20ms ticks)")
    if len(lines) < expected * 0.85:
        print(f"  WARNING: got {100.0*len(lines)/expected:.0f}% of expected rows - "
              f"coverage may still be incomplete, treat 'no stalls' with caution")

    out_path = args.out.rsplit(".", 1)[0] + ".log"
    with open(out_path, "w") as f:
        f.write(log_text)
    print(f"Wrote {out_path}")

    # Physical verification, not an arithmetic threshold: command the trolley
    # back to position 0 and confirm the homing switch actually trips there.
    # GET /verify-and-rehome is startStepCheck(0, autoRehomeOnTrip=true) - it
    # drives to zero, watches the switch, and triggers a full re-home if the
    # switch says the counter was wrong.
    #
    # Preferred over comparing encoder counts against the step counter because
    # it measures the one thing that cannot drift: a physical switch at a known
    # physical place. It needs no encoder, no ratio constant, and no tolerance
    # tuning - the switch either trips where the counter predicted or it does
    # not. The encoder drift figure above is kept as an independent diagnostic
    # (it localises WHEN sync was lost, which the switch check cannot), but the
    # switch is what gates whether the next run starts from a valid reference.
    print("\nVerifying zero reference (drive to 0, confirm switch trips)...")
    try:
        # /verify-and-rehome refuses with 409 while the stepper is still
        # winding down from the wave (its own guard: "Stepper is moving").
        # Wait it out rather than skipping the check - skipping is how a
        # corrupted reference reaches the next run.
        resp = None
        for attempt in range(10):
            try:
                resp = json.loads(http_get(args.ddp_host, "/verify-and-rehome"))
                break
            except Exception as e:
                if "409" in str(e):
                    time.sleep(2.0)
                    continue
                raise
        if resp is None:
            print("  WARNING: stepper never went idle - verification skipped")
        elif not resp.get("success"):
            print(f"  verify refused: {resp.get('message')}")
        else:
            deadline = time.monotonic() + args.home_timeout
            saw_check = False
            while time.monotonic() < deadline:
                time.sleep(1.0)
                st = get_status(args.ddp_host)
                if st.get("isChecking"):
                    saw_check = True
                    continue
                if st.get("isHoming"):
                    # A trip was found - the check auto-triggered a full re-home.
                    saw_check = True
                    print("  *** SWITCH TRIP AT ZERO CHECK - this run LOST STEPS. ***")
                    print("  *** Its numbers are measured against a corrupted zero "
                          "reference. Auto-re-homing... ***")
                    continue
                if saw_check and st.get("homed"):
                    post2 = get_status(args.ddp_host)
                    print(f"  verified - pos={post2.get('position')} "
                          f"enc={post2.get('encoderCount')} homed={post2.get('homed')}")
                    break
            else:
                print("  WARNING: verification did not complete in time")
    except Exception as e:
        print(f"  WARNING: verification failed to run: {e}")

    analyze(lines, args)


def analyze(lines, args):
    from tuning_harness import parse_row
    rows = [r for r in (parse_row(l) for l in lines) if r is not None]
    print(f"\nParsed {len(rows)} valid rows")
    if not rows:
        return

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
    parser.add_argument("--duration", type=float, default=60.0, help="Total test duration in seconds")
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
    parser.add_argument("--drift-limit", type=float, default=300.0,
                        help="Max tolerated encoder-vs-counter divergence (steps) "
                             "across a run before the run is declared to have lost "
                             "steps and the device is re-homed. ~300 is a few times "
                             "normal quantisation noise and well under a real stall.")
    parser.add_argument("--drain-interval", type=float, default=20.0,
                        help="Seconds between mid-run drains of the device's compact "
                             "motion log. Must be short enough that the device-side "
                             "96KB ring buffer (~40s of active tracking) cannot wrap "
                             "between drains.")
    parser.add_argument("--label", default="ddp_continuous")
    parser.add_argument("--out", default="tuning_runs/ddp_continuous_test.png")
    args = parser.parse_args()
    run(args)


if __name__ == "__main__":
    main()
