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
    print(f"Sent {sent} packets over {time.monotonic()-start:.1f}s")

    time.sleep(1.5)  # let the last few PID ticks settle before fetching
    log_text = http_get(args.ddp_host, "/compact-log", timeout=15.0)
    lines = [l for l in log_text.splitlines() if l]
    print(f"Fetched {len(lines)} compact-log rows over HTTP")

    out_path = args.out.rsplit(".", 1)[0] + ".log"
    with open(out_path, "w") as f:
        f.write(log_text)
    print(f"Wrote {out_path}")

    analyze(lines, args)


def analyze(lines, args):
    from tuning_harness import parse_row
    rows = [r for r in (parse_row(l) for l in lines) if r is not None]
    print(f"\nParsed {len(rows)} valid rows")
    if not rows:
        return

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
    parser.add_argument("--label", default="ddp_continuous")
    parser.add_argument("--out", default="tuning_runs/ddp_continuous_test.png")
    args = parser.parse_args()
    run(args)


if __name__ == "__main__":
    main()
