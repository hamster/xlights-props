#!/usr/bin/env python3
"""
Automated bench-tuning harness for ServoController's stepper tracking modes.

Connects to the device over serial (the same connection used for the
Compact Motion Log), configures motion tunables via the '$'-prefixed
extended command protocol (see include/tuning_handler.h), then drives the
trolley with a synthetic 8-bit triangle-wave DDP position curve at several
different full-cycle durations, capturing the Compact Motion Log for each
run and analyzing/plotting commanded-vs-actual smoothness.

Typical iteration loop:
  1. Edit a small JSON config of tunables (see tools/example_config.json).
  2. python tuning_harness.py --port COM5 --ddp-host 192.168.1.50 \
       --config my_config.json --label iter1
  3. Look at tuning_runs/iter1_*.png and tuning_runs/summary.csv.
  4. Adjust the JSON config, re-run with a new --label, repeat.
  5. python tuning_harness.py --reanalyze-dir tuning_runs   # re-plot/re-
     summarize everything captured so far, e.g. after tweaking metrics.

Does NOT persist anything to the device's flash - $SET only changes the
live RAM config, matching how DDP-driven tracking already reads it fresh
every packet. Use the web UI's Stepper Configuration to actually save a
setting you've settled on.
"""
import argparse
import csv
import datetime
import glob
import json
import os
import queue
import re
import socket
import struct
import sys
import threading
import time

DDP_HEADER_FMT = ">BBBBIH"  # flags, seq, dataType, destination, dataOffset(u32), dataLen(u16)
DDP_FLAGS_PUSH_V1 = 0x41
DDP_DATA_TYPE_RGB = 0x01
DDP_DESTINATION = 0x01

CSV_ROW_RE = re.compile(r"^\d+,")
STATUS_KV_RE = re.compile(r"(\w+)=(-?\d+)")


# --------------------------------------------------------------------------
# Serial link to the device
# --------------------------------------------------------------------------
class DeviceLink:
    def __init__(self, port, baud=115200, timeout=1.0, wait_ready=True):
        import serial  # deferred import so --help works without pyserial installed
        self.ser = serial.Serial(port, baud, timeout=timeout)
        time.sleep(2.0)  # opening the port toggles DTR/RTS on most ESP32 boards
                         # (that's how flashing/monitor tools reset it without a
                         # physical button), which reboots the device - confirmed
                         # on this hardware: disconnecting/reconnecting the serial
                         # port causes a reboot and, if autoHomeOnBootConfig is
                         # set, a full auto-home. 2s covers the boot itself; it
                         # does NOT cover a subsequent auto-home, which can easily
                         # take 15s+ - see wait_until_ready() below, called by
                         # default unless wait_ready=False.
        self.ser.reset_input_buffer()
        self._write_lock = threading.Lock()
        self._capture = None
        self._capture_lock = threading.Lock()
        self._response_queue = queue.Queue()
        self._stop = False
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()
        if wait_ready:
            self.wait_until_ready()

    def wait_until_ready(self, boot_timeout=15.0, home_timeout=90.0):
        """Waits for $STATUS to actually respond (the reset-on-open reboot
        above can leave the device unresponsive for a couple seconds longer
        than the fixed sleep in __init__ accounts for), then - if it comes up
        already homing (autoHomeOnBootConfig) - waits for that to finish too,
        so callers don't send commands into a device that's still mid-boot or
        mid-homing and get confusing ERR/empty responses back."""
        deadline = time.monotonic() + boot_timeout
        status = None
        while time.monotonic() < deadline:
            status = self.get_status()
            if status:
                break
            time.sleep(0.3)
        if not status:
            print(f"WARNING: no response to $STATUS within {boot_timeout}s of connecting - "
                  f"device may still be booting, or port/baud is wrong", file=sys.stderr)
            return False
        if status.get("homing") == 1:
            print("Device came up already homing (auto-home on boot) - waiting for it to finish...")
            deadline = time.monotonic() + home_timeout
            while time.monotonic() < deadline:
                status = self.get_status()
                if status and status.get("homing", 1) == 0:
                    print(f"  auto-home finished: {status}")
                    return True
                time.sleep(1.0)
            print("WARNING: auto-home on boot didn't finish within timeout", file=sys.stderr)
            return False
        return True

    def _reader_loop(self):
        while not self._stop:
            try:
                raw = self.ser.readline()
            except Exception:
                continue
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if CSV_ROW_RE.match(line):
                with self._capture_lock:
                    if self._capture is not None:
                        self._capture.append(line)
            else:
                self._response_queue.put(line)
                # Also keep a tagged copy in the capture, if one's active -
                # added 2026-09-06 after a real bench slam into the homing
                # stop during a tuning run: async event lines (a switch trip,
                # a stall-detected message, TMC diagnostics) previously only
                # went to _response_queue, which nothing drains during a
                # duration run (send_triangle_wave() sends DDP packets, not
                # serial commands) - so they were silently discarded, and the
                # only sign anything happened was an unrelated skipped-step
                # check sometime later. "# " prefix keeps these out of
                # CSV_ROW_RE/parse_row (neither matches a line starting with
                # "#", so plotting/metrics are unaffected) while preserving
                # exactly where in the stream - relative to real position/
                # speed/SG_RESULT rows - the event happened, for reading the
                # .log file directly afterward.
                with self._capture_lock:
                    if self._capture is not None:
                        self._capture.append("# " + line)

    def start_capture(self):
        with self._capture_lock:
            self._capture = []

    def stop_capture(self):
        with self._capture_lock:
            rows = self._capture if self._capture is not None else []
            self._capture = None
            return rows

    def send_raw(self, text):
        with self._write_lock:
            self.ser.write((text + "\n").encode("utf-8"))

    def send_command(self, text, timeout=3.0):
        # Bounded by a real wall-clock deadline, not just "until empty" -
        # found necessary 2026-09-06 on the bench: a device stuck in a
        # homing search (spamming verbose "[homing] state=..." debug lines
        # continuously) can enqueue new lines via _reader_loop faster than
        # this loop drains them, so an unbounded "while not empty" here
        # could spin for as long as the flood continues - which, for a
        # genuinely stuck search, is indefinitely. That surfaced as the
        # whole harness hanging silently well past its own home_timeout,
        # with no error ever printed. 0.5s is generous headroom over how
        # long a legitimate backlog should ever take to drain.
        drain_deadline = time.monotonic() + 0.5
        while not self._response_queue.empty() and time.monotonic() < drain_deadline:
            try:
                self._response_queue.get_nowait()
            except queue.Empty:
                break
        self.send_raw(text)
        deadline = time.monotonic() + timeout
        lines = []
        want_all = text.strip().upper() == "$GET ALL"
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                line = self._response_queue.get(timeout=remaining)
            except queue.Empty:
                break
            lines.append(line)
            if want_all:
                if line.startswith("OK"):
                    break
            elif line.startswith(("OK", "VAL", "ERR", "STATUS")):
                break
        return lines

    def set_tunable(self, name, value):
        return self.send_command(f"$SET {name} {value}")

    def get_status(self):
        for line in self.send_command("$STATUS"):
            if line.startswith("STATUS"):
                return {k: int(v) for k, v in STATUS_KV_RE.findall(line)}
        return None

    def get_all_tunables(self):
        """Full live tunable state, not just whatever this session's config
        file happened to override - so every run's record is self-contained
        and reproducible later without needing to know what the *other*
        settings were at the time."""
        result = {}
        for line in self.send_command("$GET ALL", timeout=5.0):
            if line.startswith("VAL ") and "=" in line:
                body = line[4:]
                k, v = body.split("=", 1)
                result[k] = v
        return result

    def wait_until_idle(self, timeout=15.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            st = self.get_status()
            if st and st.get("running", 1) == 0:
                return True
            time.sleep(0.2)
        return False

    def home_and_wait(self, timeout=90.0):
        # Also detects a *failed* search (device's own ~24s internal
        # homingOverallTimeoutMs aborts into HOMING_ERROR, which reads as
        # homing=0/homed=0 from $STATUS - indistinguishable from "hasn't
        # started yet" unless we'd already seen homing=1 in between) and
        # returns False immediately rather than spinning for the full
        # external `timeout` waiting for a homed=1 that will never come -
        # found necessary 2026-09-06 on the bench: with StallGuard disabled,
        # a real stuck search failed internally in ~24s but the harness
        # kept polling uselessly for the rest of a 90s timeout regardless,
        # making a real failure look identical to a hang from the outside.
        self.send_command("$HOME")
        deadline = time.monotonic() + timeout
        seen_homing = False
        while time.monotonic() < deadline:
            st = self.get_status()
            if st:
                if st.get("homing") == 1:
                    seen_homing = True
                if st.get("homed") == 1 and st.get("homing", 1) == 0:
                    return True
                if seen_homing and st.get("homing") == 0 and st.get("homed") == 0:
                    return False
            time.sleep(1.0)
        return False

    def rehome_and_check(self, timeout=90.0):
        """Homes and reports how it went, including bottomPosition - the
        one *ground-truth* signal available for real mechanical step loss.
        getCurrentPosition() (curPos in the Compact Motion Log) is just
        FastAccelStepper's own pulse-counting bookkeeping - it has no way
        to know if the motor actually skipped steps, so it always looks
        internally consistent even if the trolley has drifted. bottomPosition
        is recalculated from scratch every homing run by actually counting
        steps between the two switch triggers, so if it changes meaningfully
        between successive homings in the same session, that's real drift,
        not a control-algorithm artifact."""
        start = time.monotonic()
        ok = self.home_and_wait(timeout=timeout)
        elapsed = time.monotonic() - start
        status = self.get_status() if ok else None
        return {
            "ok": ok,
            "elapsed_s": round(elapsed, 1),
            "bottom_position": status.get("bottom") if status else None,
        }

    def check_for_skipped_steps(self, target=0, timeout=60.0):
        """Ground-truth step-loss check: commands a direct move (bypassing
        DDP/tracking-mode entirely) to `target` and watches whether the
        physical homing switch fires *before* the step counter gets there -
        see the $CHECKSTEPS command in include/tuning_handler.h and
        startStepCheck()/updateStepCheck() in stepper_handler.cpp for why
        this actually observes lost steps where getCurrentPosition() alone
        cannot. Much faster than a full rehome_and_check() (one directed
        move vs. searching both ends of travel from scratch), so this is
        the right tool to run between every stress-test iteration; treat a
        full rehome as the (slower) recovery step once this flags a problem,
        not as the routine per-run check.

        A "tripped" result means the switch fired early - `trip_pos` is how
        far short of `target` the step counter still thought it was, i.e.
        roughly how many steps have been lost since the last homing/check.
        It also leaves the device not-homed (same as any unexpected trip
        outside of homing), so the caller should follow a tripped result
        with rehome_and_check() before trusting position again.
        """
        resp = self.send_command(f"$CHECKSTEPS {target}", timeout=5.0)
        if not resp or not any(line.upper().startswith("OK") for line in resp):
            return {"ok": False, "error": resp}
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self._response_queue.get(timeout=0.2)
            except queue.Empty:
                continue
            if line.startswith("CHECKSTEPS_RESULT"):
                kv = dict(STATUS_KV_RE.findall(line))
                return {
                    "ok": True,
                    "tripped": kv.get("tripped") == "1",
                    "trip_pos": int(kv["tripPos"]) if "tripPos" in kv else None,
                    "target": int(kv["target"]) if "target" in kv else target,
                    "elapsed_ms": int(kv["elapsedMs"]) if "elapsedMs" in kv else None,
                }
            # Anything else arriving here (stale/unrelated response line) is
            # discarded - we're only listening for the one async result line.
        return {"ok": False, "error": "timeout waiting for CHECKSTEPS_RESULT"}

    def send_direction_test(self, sock, ddp_host, ddp_port, pause_s=2.0, settle_s=1.5,
                             move_timeout=15.0):
        """One packet to each extreme (255, then 0), each followed by a real
        pause once the move settles - instead of a continuous triangle wave.
        Isolates each direction's actual achieved speed/behavior without the
        "still finishing the previous reversal" artifact a continuously
        reversing wave introduces right at each turnaround. Built to
        investigate the up/down speed asymmetry noted from triangle-wave
        runs (see TODO.md): is it a fixed mechanical/gravity characteristic
        of the prop, or an artifact of reversal/tracking-profile timing?

        A single large jump like this is far beyond stepperTrackThresholdConfig,
        so it drives the normal (not tracking) profile in Direct/Coalesce
        modes regardless of current settings - this deliberately is NOT
        representative of the tracking profile's own behavior, only of the
        normal profile's achieved speed each direction. Requires the new
        periodic Compact Motion Log tick (logCompactMotionPeriodic() in
        main.cpp) to get more than one data point per leg - a single
        DDP-triggered moveTo() only logs once at commit time otherwise.

        Returns the raw captured Compact Motion Log lines spanning both legs
        (the pause between them is visible in the timestamps).
        """
        self.start_capture()
        seq = 1
        sock.sendto(build_ddp_packet(seq, 255), (ddp_host, ddp_port))
        # UDP (WiFi) vs. $STATUS (direct serial) are two independent
        # transports - the serial round-trip can easily beat the DDP packet
        # to the device, so polling wait_until_idle() with zero delay can
        # see running=0 because the move hasn't *started* yet, not because
        # it already finished. Confirmed on the bench: without this, the
        # second leg's command went out ~2s into a ~2.7s first move, visibly
        # cutting it off mid-travel. A short mandatory settle guarantees the
        # device has actually received and begun the move before we start
        # asking whether it's done.
        time.sleep(0.2)
        self.wait_until_idle(timeout=move_timeout)
        time.sleep(pause_s)
        seq = (seq % 15) + 1
        sock.sendto(build_ddp_packet(seq, 0), (ddp_host, ddp_port))
        time.sleep(0.2)
        self.wait_until_idle(timeout=move_timeout)
        time.sleep(settle_s)
        return self.stop_capture()

    def close(self):
        self._stop = True
        try:
            self.ser.close()
        except Exception:
            pass


# --------------------------------------------------------------------------
# DDP triangle-wave sender
# --------------------------------------------------------------------------
def triangle_value(t, duration):
    """0 -> 255 over the first half of duration, 255 -> 0 over the second half."""
    if duration <= 0:
        return 0
    frac = t / duration
    value = 255.0 * (1.0 - abs(1.0 - 2.0 * frac))
    return max(0, min(255, int(round(value))))


def build_ddp_packet(seq, value):
    header = struct.pack(DDP_HEADER_FMT, DDP_FLAGS_PUSH_V1, seq, DDP_DATA_TYPE_RGB,
                          DDP_DESTINATION, 0, 1)
    return header + bytes([value])


def send_triangle_wave(sock, host, port, duration, rate_hz):
    interval = 1.0 / rate_hz
    start = time.monotonic()
    seq = 0
    tick = 0
    while True:
        now = time.monotonic()
        t = now - start
        if t >= duration:
            break
        value = triangle_value(t, duration)
        seq = (seq % 15) + 1
        sock.sendto(build_ddp_packet(seq, value), (host, port))
        tick += 1
        next_tick_time = start + tick * interval
        sleep_time = next_tick_time - time.monotonic()
        if sleep_time > 0:
            time.sleep(sleep_time)
    # Final packet to land exactly on 0
    seq = (seq % 15) + 1
    sock.sendto(build_ddp_packet(seq, 0), (host, port))


# --------------------------------------------------------------------------
# Log parsing / metrics / plotting
# --------------------------------------------------------------------------
ROW_FIELDS = ["ms", "ddpVal", "cmdPos", "curPos", "delta", "lag", "profile", "curSpeedHz",
              "targetSpeedHz", "encoderCount", "sgResult", "switchTripped"]


def parse_row(line):
    # Accepts the current 12-field format (trailing switchTripped added
    # 2026-09-06 for post-hoc stuck-against-the-switch detection - see
    # is_stuck_at_switch()), the 11-field format from just before it
    # (sgResult, no switchTripped), the 10-field format before that
    # (encoderCount, neither), and the original 9-field format (none of the
    # three) - so reanalyzing a long-lived --out-dir doesn't silently zero
    # out older runs just because the firmware's log format grew a column
    # since they were captured.
    parts = line.split(",")
    while len(parts) < len(ROW_FIELDS):
        parts.append("0")  # missing trailing column(s) in an older-format log - treat as 0 (unknown)
    if len(parts) != len(ROW_FIELDS):
        return None
    try:
        return {
            "ms": int(parts[0]), "ddpVal": int(parts[1]), "cmdPos": int(parts[2]),
            "curPos": int(parts[3]), "delta": int(parts[4]), "lag": int(parts[5]),
            "profile": parts[6], "curSpeedHz": int(parts[7]), "targetSpeedHz": int(parts[8]),
            "encoderCount": int(parts[9]), "sgResult": int(parts[10]), "switchTripped": int(parts[11]),
        }
    except ValueError:
        return None


# How many consecutive samples of "commanded to move, switch already
# triggered, encoder not advancing" before is_stuck_at_switch() calls it a
# real stuck condition rather than one noisy sample - see that function's
# docstring. At this project's ~50ms compact-log tick, 3 samples is ~150ms.
STUCK_DEBOUNCE_SAMPLES = 3
STUCK_SPEED_THRESHOLD_HZ = 50  # "commanded to move" - filters out rows where the stepper is essentially idle


def is_stuck_at_switch(rows):
    """Post-hoc detection of the condition the user described directly: the
    stepper is being commanded to move (curSpeedHz well above 0), the homing
    switch already reads triggered, and the encoder isn't advancing anyway -
    i.e. buzzing uselessly against the physical stop rather than actually
    moving. Deliberately done here, not as new real-time firmware logic -
    leans on the encoder, which isn't meant to outlive this tuning phase
    (see main.cpp's logCompactMotion() comment). Returns a list of (start_ms,
    end_ms, num_samples) spans, debounced by STUCK_DEBOUNCE_SAMPLES so one
    noisy sample right at a real switch release doesn't get flagged."""
    spans = []
    run_start_idx = None
    for i, r in enumerate(rows):
        stuck_sample = (r["switchTripped"] == 1 and abs(r["curSpeedHz"]) >= STUCK_SPEED_THRESHOLD_HZ)
        if stuck_sample:
            # Encoder must also show ~no motion since the last sample -
            # first sample of a run has nothing to compare against yet.
            if run_start_idx is None:
                run_start_idx = i
            elif i > 0 and abs(r["encoderCount"] - rows[i - 1]["encoderCount"]) > 1:
                # Real encoder motion despite the switch reading triggered -
                # not actually stuck (e.g. bouncing near the trip point) -
                # end this run without flagging it.
                run_start_idx = None
        else:
            if run_start_idx is not None and i - run_start_idx >= STUCK_DEBOUNCE_SAMPLES:
                spans.append((rows[run_start_idx]["ms"], rows[i - 1]["ms"], i - run_start_idx))
            run_start_idx = None
    if run_start_idx is not None and len(rows) - run_start_idx >= STUCK_DEBOUNCE_SAMPLES:
        spans.append((rows[run_start_idx]["ms"], rows[-1]["ms"], len(rows) - run_start_idx))
    return spans


def load_run_log(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if CSV_ROW_RE.match(line):
                r = parse_row(line)
                if r:
                    rows.append(r)
    return rows


def compute_metrics(rows):
    if not rows:
        return {"num_rows": 0}
    lags = [abs(r["lag"]) for r in rows]
    speeds = [r["curSpeedHz"] for r in rows]
    rms_error = (sum(x * x for x in lags) / len(lags)) ** 0.5
    max_error = max(lags)
    mean_abs_speed = sum(abs(s) for s in speeds) / len(speeds)
    jerk_total = sum(abs(speeds[i] - speeds[i - 1]) for i in range(1, len(speeds)))
    jerk_per_sample = jerk_total / max(1, len(speeds) - 1)
    near_stall = sum(1 for r in rows if abs(r["curSpeedHz"]) < 300 and abs(r["lag"]) > 200)
    near_stall_pct = 100.0 * near_stall / len(rows)
    tracking_rows = sum(1 for r in rows if r["profile"] == "T")
    # sgResult of 0 means "no TMC UART/not connected" (see logCompactMotion()'s
    # comment), not a real reading of 0 - exclude those from the min so an
    # unconnected TMC doesn't look identical to an actual stall-threshold trip.
    sg_readings = [r["sgResult"] for r in rows if r["sgResult"] > 0]
    min_sg_result = min(sg_readings) if sg_readings else None
    stuck_spans = is_stuck_at_switch(rows)
    stuck_ms_total = sum(end - start for start, end, _ in stuck_spans)
    return {
        "num_rows": len(rows),
        "duration_ms": rows[-1]["ms"] - rows[0]["ms"],
        "rms_error_steps": round(rms_error, 1),
        "max_error_steps": max_error,
        "mean_abs_speed_hz": round(mean_abs_speed, 1),
        "jerk_per_sample": round(jerk_per_sample, 1),
        "near_stall_pct": round(near_stall_pct, 1),
        "tracking_pct": round(100.0 * tracking_rows / len(rows), 1),
        "min_sg_result": min_sg_result,
        "stuck_at_switch_count": len(stuck_spans),
        "stuck_at_switch_ms": stuck_ms_total,
    }


def plot_run(rows, out_png, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    t0 = rows[0]["ms"]
    t = [(r["ms"] - t0) / 1000.0 for r in rows]
    cmd_pos = [r["cmdPos"] for r in rows]
    cur_pos = [r["curPos"] for r in rows]
    cur_speed = [r["curSpeedHz"] for r in rows]
    signed_error = [r["cmdPos"] - r["curPos"] for r in rows]
    sg_result = [r["sgResult"] for r in rows]
    has_sg = any(v > 0 for v in sg_result)

    fig, axes = plt.subplots(4 if has_sg else 3, 1, figsize=(10, 10 if has_sg else 8), sharex=True)
    axes[0].plot(t, cmd_pos, label="Commanded", linewidth=1)
    axes[0].plot(t, cur_pos, label="Actual", linewidth=1)
    axes[0].set_ylabel("Position (steps)")
    # Shade spans where the stepper was buzzing uselessly against the
    # homing switch (commanded to move, switch triggered, encoder not
    # advancing) - see is_stuck_at_switch(). Labeled once so the legend
    # doesn't get one entry per span.
    for i, (start_ms, end_ms, _) in enumerate(is_stuck_at_switch(rows)):
        axes[0].axvspan((start_ms - t0) / 1000.0, (end_ms - t0) / 1000.0, color="red", alpha=0.15,
                         label="Stuck at switch" if i == 0 else None)
    axes[0].legend(loc="upper right", fontsize=8)
    axes[0].set_title(title)

    axes[1].plot(t, cur_speed, color="tab:orange", linewidth=1)
    axes[1].set_ylabel("Actual Speed (Hz)")
    axes[1].axhline(0, color="gray", linewidth=0.5)

    axes[2].plot(t, signed_error, color="tab:red", linewidth=1)
    axes[2].axhline(0, color="gray", linewidth=0.5)
    axes[2].set_ylabel("Error (cmd - actual)")

    if has_sg:
        # 0 means "no TMC UART" (see logCompactMotion()) - plotted as-is
        # rather than hidden, since a run that's entirely 0 is itself useful
        # to notice (TMC link down for this run).
        axes[3].plot(t, sg_result, color="tab:purple", linewidth=1)
        axes[3].set_ylabel("SG_RESULT\n(lower = more loaded)")
        axes[3].set_xlabel("Time (s)")
    else:
        axes[2].set_xlabel("Time (s)")

    fig.tight_layout()
    fig.savefig(out_png, dpi=120)
    plt.close(fig)


def write_summary_row(summary_path, row):
    """Appends one row to a CSV that may already have accumulated rows from
    earlier sessions. If the file exists, keeps its existing header (so
    years-old data stays readable) and warns rather than silently dropping
    data if this row has keys the existing file doesn't - that only
    happens if the script's own field set changes over time (e.g. a new
    tunable added to the firmware)."""
    file_exists = os.path.isfile(summary_path)
    if file_exists:
        with open(summary_path, newline="") as f:
            fieldnames = next(csv.reader(f))
        extra = [k for k in row if k not in fieldnames]
        if extra:
            print(f"WARNING: {summary_path} doesn't have column(s) {extra} - "
                  f"they'll be dropped from this row. Rename/rotate the file to pick up new columns.",
                  file=sys.stderr)
    else:
        fieldnames = list(row.keys())
    with open(summary_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        if not file_exists:
            writer.writeheader()
        writer.writerow(row)


# --------------------------------------------------------------------------
# Main orchestration
# --------------------------------------------------------------------------
def run_hardware_session(args):
    durations = [float(x) for x in args.durations.split(",")]
    os.makedirs(args.out_dir, exist_ok=True)

    config = {}
    if args.config:
        with open(args.config) as f:
            config = json.load(f)

    link = DeviceLink(args.port, args.baud)
    try:
        print(f"Applying {len(config)} tunable(s) from {args.config}...")
        for name, value in config.items():
            resp = link.set_tunable(name, value)
            print(f"  {name} = {value}: {resp}")

        link.set_tunable("protocolDebug", 0)
        link.set_tunable("compactLog", 1)

        status = link.get_status()
        print("Status:", status)
        if not status:
            print("ERROR: no response to $STATUS - check port/baud/wiring", file=sys.stderr)
            sys.exit(1)

        # Always home at the start of a session by default, even if the
        # device already claims homed=1 - that flag could be left over from
        # a much earlier calibration, and this is exactly the kind of
        # session where trusting a stale reference silently would defeat
        # the point. Skippable for quick iteration once you trust the
        # current calibration.
        bottom_positions = []  # tracked across every homing this session - see rehome_and_check()
        if not args.skip_initial_rehome or status.get("homed") != 1:
            print("Homing now (this can take a while)...")
            result = link.rehome_and_check(timeout=args.home_timeout)
            print("  Homing result:", result)
            if not result["ok"]:
                print("ERROR: homing did not complete in time, aborting", file=sys.stderr)
                sys.exit(1)
            bottom_positions.append(result["bottom_position"])

        # Full live tunable state (not just whatever this session's config
        # file overrode) - every run's record below is self-contained and
        # reproducible later without needing to cross-reference what the
        # *other* settings were at the time.
        full_config = link.get_all_tunables()
        print("Full tunable snapshot for this session:", full_config)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        summary_path = os.path.join(args.out_dir, "summary.csv")
        all_rows_path = os.path.join(args.out_dir, "all_rows.csv")

        for duration in durations:
            print(f"\n=== Run: label={args.label} duration={duration}s ===")
            link.wait_until_idle(timeout=15.0)

            homing_result = None
            step_check_result = None
            if args.rehome_between_runs:
                # Slow but thorough: re-derive bottomPosition from scratch by
                # searching both ends of travel again.
                print("  Re-homing before this run (--rehome-between-runs)...")
                homing_result = link.rehome_and_check(timeout=args.home_timeout)
                print("  Homing result:", homing_result)
                if not homing_result["ok"]:
                    print("ERROR: homing did not complete in time, aborting", file=sys.stderr)
                    sys.exit(1)
                bottom_positions.append(homing_result["bottom_position"])
                if len(bottom_positions) >= 2 and homing_result["bottom_position"] is not None:
                    drift = homing_result["bottom_position"] - bottom_positions[-2]
                    if abs(drift) > 20:
                        print(f"  WARNING: bottomPosition drifted {drift:+d} steps since the last "
                              f"homing this session ({bottom_positions[-2]} -> {homing_result['bottom_position']}) "
                              f"- possible real step loss, not just a control-algorithm tracking artifact.")
            elif args.check_steps_between_runs:
                # Fast, targeted default: one direct move to the known switch
                # reference instead of a full both-ends search. See
                # DeviceLink.check_for_skipped_steps().
                print("  Checking for skipped steps before this run...")
                step_check_result = link.check_for_skipped_steps(target=0, timeout=args.home_timeout)
                print("  Skipped-step check:", step_check_result)
                if not step_check_result.get("ok"):
                    print(f"  WARNING: skipped-step check didn't complete cleanly "
                          f"({step_check_result.get('error')}) - continuing anyway", file=sys.stderr)
                elif step_check_result.get("tripped"):
                    print(f"  WARNING: homing switch tripped {step_check_result['trip_pos']} step(s) "
                          f"early - real step loss detected since the last homing/check. Re-homing to recover...")
                    homing_result = link.rehome_and_check(timeout=args.home_timeout)
                    print("  Recovery homing result:", homing_result)
                    if not homing_result["ok"]:
                        print("ERROR: recovery re-home did not complete in time, aborting", file=sys.stderr)
                        sys.exit(1)
                    bottom_positions.append(homing_result["bottom_position"])

            run_id = f"{args.label}_{timestamp}_dur{duration:g}s"
            link.start_capture()
            send_triangle_wave(sock, args.ddp_host, args.ddp_port, duration, args.rate_hz)
            time.sleep(args.settle)
            raw_rows = link.stop_capture()

            log_filename = os.path.join(args.out_dir, f"{run_id}.log")
            with open(log_filename, "w") as f:
                f.write(f"{args.label} - triangle wave duration {duration:g}s\n")
                f.write(f"overrides this session: {json.dumps(config)}\n")
                f.write(f"full config: {json.dumps(full_config)}\n")
                f.write(f"homing before this run: {json.dumps(homing_result)}\n")
                f.write(f"skipped-step check before this run: {json.dumps(step_check_result)}\n\n")
                for line in raw_rows:
                    f.write(line + "\n")
            print(f"  Captured {len(raw_rows)} rows -> {log_filename}")

            rows = [parse_row(l) for l in raw_rows]
            rows = [r for r in rows if r is not None]
            metrics = compute_metrics(rows)
            print("  Metrics:", metrics)
            if metrics.get("stuck_at_switch_count"):
                print(f"  WARNING: stuck against the homing switch {metrics['stuck_at_switch_count']} "
                      f"time(s) this run, {metrics['stuck_at_switch_ms']}ms total (commanded to move, "
                      f"switch reads triggered, encoder not advancing) - see is_stuck_at_switch()")

            if rows:
                png_filename = log_filename.replace(".log", ".png")
                plot_run(rows, png_filename, f"{args.label} - {duration:g}s triangle wave")
                print(f"  Plot -> {png_filename}")

            # Structured sidecar for programmatic datamining later (pandas/
            # jq/whatever) without needing to parse the .log header text.
            sidecar = {
                "run_id": run_id,
                "label": args.label,
                "duration_s": duration,
                "rate_hz": args.rate_hz,
                "timestamp": timestamp,
                "log_file": os.path.basename(log_filename),
                "overrides": config,
                "full_config": full_config,
                "homing_before_run": homing_result,
                "step_check_before_run": step_check_result,
                "metrics": metrics,
            }
            with open(log_filename.replace(".log", ".json"), "w") as f:
                json.dump(sidecar, f, indent=2)

            # Every raw sample, across every run this --out-dir has ever
            # seen, in one place - so later analysis isn't limited to
            # per-run summary metrics.
            for r in rows:
                write_summary_row(all_rows_path, {"run_id": run_id, "label": args.label,
                                                    "duration_s": duration, **r})

            write_summary_row(summary_path, {
                "run_id": run_id,
                "label": args.label,
                "duration_s": duration,
                "log_file": os.path.basename(log_filename),
                "bottom_position": homing_result["bottom_position"] if homing_result else None,
                "homing_elapsed_s": homing_result["elapsed_s"] if homing_result else None,
                "step_check_tripped": step_check_result.get("tripped") if step_check_result else None,
                "step_check_trip_pos": step_check_result.get("trip_pos") if step_check_result else None,
                **full_config,  # already reflects this session's overrides, fetched after applying them
                **metrics,
            })

        print(f"\nSummary appended to {summary_path}")
        print(f"All raw samples appended to {all_rows_path}")
    finally:
        link.close()


def run_direction_test_session(args):
    """--direction-test: one packet to each extreme (255, then 0), each
    followed by a real pause once it settles, instead of a continuous
    triangle wave - see DeviceLink.send_direction_test()'s docstring for why
    this isolates each direction's achieved speed better than reading it off
    a reversing wave. Reuses the same output conventions (log/json/png,
    summary.csv/all_rows.csv) as the regular duration runs, tagged with
    duration_s="dir" so it's easy to filter out of/into that data."""
    os.makedirs(args.out_dir, exist_ok=True)
    config = {}
    if args.config:
        with open(args.config) as f:
            config = json.load(f)

    link = DeviceLink(args.port, args.baud)
    try:
        print(f"Applying {len(config)} tunable(s) from {args.config}...")
        for name, value in config.items():
            resp = link.set_tunable(name, value)
            print(f"  {name} = {value}: {resp}")
        link.set_tunable("protocolDebug", 0)
        link.set_tunable("compactLog", 1)

        status = link.get_status()
        print("Status:", status)
        if not status:
            print("ERROR: no response to $STATUS - check port/baud/wiring", file=sys.stderr)
            sys.exit(1)
        if status.get("homed") != 1:
            print("Homing now (this can take a while)...")
            result = link.rehome_and_check(timeout=args.home_timeout)
            print("  Homing result:", result)
            if not result["ok"]:
                print("ERROR: homing did not complete in time, aborting", file=sys.stderr)
                sys.exit(1)

        full_config = link.get_all_tunables()
        print("Full tunable snapshot for this session:", full_config)
        print("NOTE: a single full-range jump like this is far beyond trackThreshold, so it "
              "exercises the NORMAL profile each direction, not the tracking profile.")

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")

        link.wait_until_idle(timeout=15.0)
        print("\n  Checking for skipped steps before this test...")
        step_check_result = link.check_for_skipped_steps(target=0, timeout=args.home_timeout)
        print("  Skipped-step check:", step_check_result)
        if step_check_result.get("tripped"):
            print("  Recovering with a full re-home...")
            homing_result = link.rehome_and_check(timeout=args.home_timeout)
            print("  Homing result:", homing_result)
            if not homing_result["ok"]:
                print("ERROR: recovery re-home did not complete in time, aborting", file=sys.stderr)
                sys.exit(1)

        run_id = f"{args.label}_{timestamp}_dirtest"
        print(f"\n=== Direction test: label={args.label} (up to 255, pause, back to 0) ===")
        raw_rows = link.send_direction_test(sock, args.ddp_host, args.ddp_port,
                                             pause_s=args.direction_pause, settle_s=args.settle)

        log_filename = os.path.join(args.out_dir, f"{run_id}.log")
        with open(log_filename, "w") as f:
            f.write(f"{args.label} - direction test (0->255, pause {args.direction_pause}s, 255->0)\n")
            f.write(f"overrides this session: {json.dumps(config)}\n")
            f.write(f"full config: {json.dumps(full_config)}\n")
            f.write(f"skipped-step check before this test: {json.dumps(step_check_result)}\n\n")
            for line in raw_rows:
                f.write(line + "\n")
        print(f"  Captured {len(raw_rows)} rows -> {log_filename}")

        rows = [parse_row(l) for l in raw_rows]
        rows = [r for r in rows if r is not None]
        metrics = compute_metrics(rows)
        print("  Metrics:", metrics)
        if metrics.get("stuck_at_switch_count"):
            print(f"  WARNING: stuck against the homing switch {metrics['stuck_at_switch_count']} "
                  f"time(s), {metrics['stuck_at_switch_ms']}ms total - see is_stuck_at_switch()")

        if rows:
            png_filename = log_filename.replace(".log", ".png")
            plot_run(rows, png_filename, f"{args.label} - direction test (up/pause/down)")
            print(f"  Plot -> {png_filename}")

        sidecar = {
            "run_id": run_id, "label": args.label, "duration_s": "dir",
            "timestamp": timestamp, "log_file": os.path.basename(log_filename),
            "overrides": config, "full_config": full_config,
            "step_check_before_test": step_check_result, "metrics": metrics,
        }
        with open(log_filename.replace(".log", ".json"), "w") as f:
            json.dump(sidecar, f, indent=2)

        all_rows_path = os.path.join(args.out_dir, "all_rows.csv")
        for r in rows:
            write_summary_row(all_rows_path, {"run_id": run_id, "label": args.label,
                                                "duration_s": "dir", **r})

        summary_path = os.path.join(args.out_dir, "summary.csv")
        write_summary_row(summary_path, {
            "run_id": run_id, "label": args.label, "duration_s": "dir",
            "log_file": os.path.basename(log_filename),
            "step_check_tripped": step_check_result.get("tripped"),
            "step_check_trip_pos": step_check_result.get("trip_pos"),
            **full_config, **metrics,
        })
        print(f"\nSummary appended to {summary_path}")
    finally:
        link.close()


def run_reanalyze(args):
    pattern = os.path.join(args.reanalyze_dir, "*.log")
    files = sorted(glob.glob(pattern))
    if not files:
        print(f"No .log files found in {args.reanalyze_dir}")
        return
    summary_path = os.path.join(args.reanalyze_dir, "summary_reanalyzed.csv")
    if os.path.exists(summary_path):
        os.remove(summary_path)
    for path in files:
        rows = load_run_log(path)
        if not rows:
            print(f"  {path}: no valid rows, skipping")
            continue
        metrics = compute_metrics(rows)
        print(f"  {os.path.basename(path)}: {metrics}")
        png_path = path.replace(".log", "_reanalyzed.png")
        plot_run(rows, png_path, os.path.basename(path))
        write_summary_row(summary_path, {"log_file": os.path.basename(path), **metrics})
    print(f"\nSummary written to {summary_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="Serial port (e.g. COM5)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--ddp-host", help="Device IP address for DDP UDP packets")
    parser.add_argument("--ddp-port", type=int, default=4048)
    parser.add_argument("--durations", default="5,8,12,16,20",
                         help="Comma-separated full-cycle (0->255->0) durations in seconds")
    parser.add_argument("--rate-hz", type=float, default=40.0, help="DDP send rate")
    parser.add_argument("--config", default=None, help="JSON file of {tunableName: value} to $SET before this run")
    parser.add_argument("--label", default=None, help="Short name for this iteration (used in output filenames)")
    parser.add_argument("--out-dir", default="tuning_runs")
    parser.add_argument("--settle", type=float, default=1.5, help="Extra capture seconds after DDP sending stops")
    parser.add_argument("--home-timeout", type=float, default=90.0)
    parser.add_argument("--skip-initial-rehome", action="store_true",
                         help="Don't home at session start if the device already reports homed=1. "
                              "Default is to always home first, since a stale prior calibration is "
                              "exactly what this tool exists to catch.")
    parser.add_argument("--rehome-between-runs", action="store_true",
                         help="Do a full re-home (both ends of travel, slow) before every duration run, "
                              "instead of the faster --check-steps-between-runs default.")
    parser.add_argument("--check-steps-between-runs", action=argparse.BooleanOptionalAction, default=True,
                         help="Before every duration run, do a fast targeted skipped-step check "
                              "(command to position 0, watch for an early homing-switch trip) and "
                              "auto-recover with a full re-home if it detects drift. On by default; "
                              "ignored if --rehome-between-runs is also set. Use --no-check-steps-between-runs "
                              "to skip both drift checks entirely.")
    parser.add_argument("--reanalyze-dir", default=None,
                         help="Skip hardware entirely; just re-parse/re-plot/re-summarize .log files in this directory")
    parser.add_argument("--direction-test", action="store_true",
                         help="Instead of the triangle-wave duration runs, send one packet to each "
                              "extreme (255, then 0) with a real pause in between once each settles - "
                              "isolates each direction's achieved speed without a continuous wave's "
                              "reversal artifacts. See DeviceLink.send_direction_test(). Ignores "
                              "--durations/--rate-hz.")
    parser.add_argument("--direction-pause", type=float, default=2.0,
                         help="--direction-test only: seconds to sit still between the two legs")
    args = parser.parse_args()

    if args.reanalyze_dir:
        run_reanalyze(args)
        return

    if not args.port or not args.ddp_host or not args.label:
        parser.error("--port, --ddp-host, and --label are required unless using --reanalyze-dir")

    if args.direction_test:
        run_direction_test_session(args)
        return

    run_hardware_session(args)


if __name__ == "__main__":
    main()
