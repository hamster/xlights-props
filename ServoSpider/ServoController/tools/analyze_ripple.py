#!/usr/bin/env python3
"""Standalone post-hoc analysis for the Kd/Kp damping sweep (2026-09-06).

Computes two things a plain compute_metrics() run doesn't give cleanly:
  1. "dead-start" contamination - whether this run got hit by the
     intermittent dead-runForward()-at-the-switch bug (see main.cpp's
     lastPidRunRetryMs / genuinelyRunning comments) - detected as actual
     position staying within a few steps of its start value for an
     unreasonably long real-time stretch right after motion should have
     begun. Contaminated runs have huge, spurious rms_error/max_error
     that reflects this transient, not the gain choice under test - not
     useful for comparing candidates.
  2. Steady-cruise ripple: peak-to-peak period and RMS amplitude of
     curSpeedHz within a clean mid-run cruise window (well after the
     dead-start/accel region, well before the next reversal) - the
     actual signal this sweep cares about.

Usage: python analyze_ripple.py <log_file> [<log_file> ...]
"""
import sys
import numpy as np
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from tuning_harness import parse_row


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            r = parse_row(line.strip())
            if r:
                rows.append(r)
    return rows


def analyze(path):
    rows = load(path)
    # Drop a leading malformed/truncated row if present - seen twice testing
    # pidTickMs below 20 (2026-09-07: once as "0,0", once as "08,663,0,0" -
    # different shapes, so this checks for an implausible timestamp rather
    # than matching either exact pattern). A genuine first row's ms is
    # millis() since boot at whatever point /compact-log?clear=1 landed,
    # comfortably into 4+ digits by the time a run starts - not the sub-1000
    # value parse_row()'s trailing-field padding produces from a short,
    # truncated line. Confirmed tick-rate-correlated, not yet root-caused:
    # 0 occurrences across ~25 runs at the original 20ms tick tonight, 2 of 3
    # runs at 10/5ms. Harmless to the main analyze() tool's aggregate metrics
    # (one row in ~1900 barely moves an average) but broke this file's
    # dead-start detection, which anchors t0 to rows[0] - with t0 wrongly
    # pinned near 0, dead_start_ms came out as the real first row's raw
    # device-uptime millis() (hundreds of thousands of ms), large enough to
    # trip the >700ms contamination flag on an otherwise-clean run. See
    # TODO.md's stepperPidTickMsConfig section for the open item.
    while len(rows) > 1 and rows[0]["ms"] < 1000 and rows[1]["ms"] - rows[0]["ms"] > 5000:
        print(f"  NOTE: dropped a leading malformed row (ms={rows[0]['ms']}) from {path}")
        rows = rows[1:]
    if len(rows) < 20:
        return None
    t0 = rows[0]["ms"]
    total_s = (rows[-1]["ms"] - t0) / 1000.0

    # Dead-start detection: find how long curPos stays within 40 steps of
    # its starting value before ever exceeding it - a normal run should
    # break out within ~150-300ms (accel ramp); anything past 700ms is
    # the dead-start signature.
    start_pos = rows[0]["curPos"]
    dead_start_ms = 0
    for r in rows:
        if abs(r["curPos"] - start_pos) > 60:
            dead_start_ms = r["ms"] - t0
            break
    else:
        dead_start_ms = rows[-1]["ms"] - t0
    contaminated = dead_start_ms > 700

    # Steady-cruise window: skip the first 30% and last 15% of the run
    # (start transient + turnaround), sample what's left.
    lo = t0 + total_s * 1000 * 0.30
    hi = t0 + total_s * 1000 * 0.85
    window = [r for r in rows if lo <= r["ms"] <= hi]
    ripple_rms = float("nan")
    ripple_period_ms = float("nan")
    if len(window) > 10:
        speeds = np.array([r["curSpeedHz"] for r in window], dtype=float)
        times = np.array([(r["ms"] - t0) / 1000.0 for r in window])
        # detrend with a short moving average, then RMS of the residual
        k = 5
        kernel = np.ones(k) / k
        padded = np.pad(speeds, (k // 2, k // 2), mode="edge")
        smooth = np.convolve(padded, kernel, mode="valid")[: len(speeds)]
        residual = speeds - smooth
        ripple_rms = float(np.sqrt(np.mean(residual**2)))
        mean_speed = np.mean(speeds)
        peaks = [times[i] for i in range(1, len(speeds) - 1)
                 if speeds[i] > speeds[i - 1] and speeds[i] >= speeds[i + 1] and speeds[i] > mean_speed]
        if len(peaks) > 3:
            ripple_period_ms = float(np.mean(np.diff(peaks)) * 1000)

    lags = [abs(r["lag"]) for r in rows]
    rms_error = float(np.sqrt(np.mean(np.array(lags, dtype=float) ** 2)))

    return {
        "file": path,
        "contaminated": contaminated,
        "dead_start_ms": dead_start_ms,
        "rms_error": round(rms_error, 1),
        "ripple_rms_hz": round(ripple_rms, 1),
        "ripple_period_ms": round(ripple_period_ms, 1),
    }


if __name__ == "__main__":
    for path in sys.argv[1:]:
        result = analyze(path)
        if result is None:
            print(f"{path}: not enough rows")
            continue
        flag = " *** DEAD-START CONTAMINATED ***" if result["contaminated"] else ""
        print(f"{path}: rms_error={result['rms_error']} ripple_rms={result['ripple_rms_hz']}Hz "
              f"ripple_period={result['ripple_period_ms']}ms dead_start={result['dead_start_ms']}ms{flag}")
