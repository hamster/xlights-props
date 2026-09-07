#!/usr/bin/env python3
"""
Combines the DDP reception instrumentation (ddp_reception_test.py) with
real PID motion - the natural follow-up to that test's clean, no-motion
result. Homes the device, engages TRACK_MODE_PID with the tuned gains,
then sends the same continuous 40fps triangle wave while capturing BOTH
the Compact Motion Log (tracking behavior: commanded/actual position,
speed) AND the DDP reception diagnostics (per-packet reception, RTT via
ACK, RSSI) simultaneously - looking for whether a freeze/stall in tracking
correlates with a real reception/RTT anomaly at the same moment, which
would confirm real motor motion is coupling into DDP reception (electrical
interference or resource contention) rather than this being a general
WiFi/protocol issue (already ruled out by the no-motion test).

Configuration (tunables) and DDP-reception-log retrieval both go over the
device's HTTP API (GET /tunable, GET /ddp-rx-log) rather than the serial
$SET/$GET protocol - this keeps serial I/O off the timed portion of the
test entirely except for the Compact Motion Log capture itself (still
serial-only; there's no HTTP equivalent for that stream), matching the
round-trip-plus-processing measurement the user asked for and avoiding
USB-serial contention as a confound.

Usage:
  python pid_motion_reception_test.py --port COM7 --ddp-host 192.168.10.181 \
      --duration 60 --rate-hz 40 --ack
"""
import argparse
import json
import os
import select
import socket
import sys
import time
import urllib.request
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tuning_harness import DeviceLink, triangle_value, build_ddp_packet, parse_row

DDP_PORT = 4048


def http_get(host, path, timeout=5.0):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as resp:
        return resp.read().decode("utf-8", errors="replace")


def http_set_tunable(host, name, value):
    """Sets a RAM-only tunable via GET /tunable?name=<n>&value=<v> - the
    HTTP equivalent of DeviceLink.set_tunable(), added specifically so this
    test doesn't need to interleave serial $SET commands with the Compact
    Motion Log capture."""
    body = http_get(host, f"/tunable?name={name}&value={value}")
    result = json.loads(body)
    if not result.get("success"):
        raise RuntimeError(f"HTTP set {name}={value} failed: {result}")
    return result


def http_get_ddp_rx_log(host):
    return http_get(host, "/ddp-rx-log", timeout=10.0)


def http_clear_ddp_rx_log(host):
    http_get(host, "/ddp-rx-log?clear=1")


def wait_for_idle(link, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        st = link.get_status()
        if st and st.get("running", 1) == 0:
            return True
        time.sleep(0.2)
    return False


def run_test(args):
    link = DeviceLink(args.port, args.baud)
    try:
        status = link.get_status()
        print("Initial status:", status)

        # Same tuned working config as config_pid.json.
        config = {
            "normalSpeed": 7000, "normalAccel": 200000,
            "tmcRunCurrent": 1200, "tmcStallEnabled": 0,
            "trackMode": 4,
            "pidKp": 15, "pidKi": 0, "pidKd": 0.7,
            "pidMaxSpeed": 7000, "pidAccel": 50000,
            "pidDeadband": 30, "pidReengageThreshold": 150,
        }
        print(f"Applying {len(config)} tunable(s) via HTTP...")
        for name, value in config.items():
            print(f"  {name} = {value}: {http_set_tunable(args.ddp_host, name, value)}")

        print(http_set_tunable(args.ddp_host, "protocolDebug", 0))
        # compactLog still goes to the serial-only Compact Motion Log - no
        # HTTP equivalent for that stream (see module docstring).
        print(http_set_tunable(args.ddp_host, "compactLog", 1))
        print(http_set_tunable(args.ddp_host, "ddpRxLog", 1))
        print(http_set_tunable(args.ddp_host, "ddpAck", 1 if args.ack else 0))
        http_clear_ddp_rx_log(args.ddp_host)

        if link.get_status().get("homed") != 1:
            print("Homing...")
            print(link.rehome_and_check(timeout=90))
        print("Status before test:", link.get_status())

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setblocking(False)

        pending = {s: deque() for s in range(1, 16)}
        acks = []  # (recv_time, seq, rtt_s)

        def drain_acks():
            while True:
                try:
                    data, _addr = sock.recvfrom(64)
                except (BlockingIOError, OSError):
                    return
                if not data:
                    continue
                recv_time = time.monotonic()
                seq = data[0]
                if seq < 1 or seq > 15:
                    continue
                q = pending[seq]
                if q:
                    _value, send_time = q.popleft()
                    acks.append((recv_time, seq, recv_time - send_time))

        link.start_capture()

        sent = []
        interval = 1.0 / args.rate_hz
        total_ticks = max(1, round(args.duration * args.rate_hz))
        start = time.monotonic()
        seq = 0
        print(f"\nSending {args.duration:.0f}s of continuous {args.rate_hz:.0f}fps triangle wave "
              f"to {args.ddp_host}:{DDP_PORT} while PID actively tracks "
              f"({'with' if args.ack else 'without'} ACK)...")
        for tick in range(total_ticks + 1):
            target_t = tick * interval
            while True:
                now = time.monotonic()
                behind_s = now - (start + target_t)
                if behind_s >= 0:
                    break
                wait_s = min(0.002, -behind_s)
                if args.ack:
                    drain_acks()
                    select.select([sock], [], [], wait_s)
                else:
                    time.sleep(wait_s)
            if behind_s > 1.0:
                new_tick = min(total_ticks, int((now - start) / interval))
                print(f"  WARNING: sender fell {behind_s:.2f}s behind schedule - "
                      f"skipping ticks {tick}..{new_tick - 1}", file=sys.stderr)
                tick = new_tick
                target_t = tick * interval

            if args.ack:
                drain_acks()
            value = triangle_value(min(target_t, args.duration), args.duration)
            seq = (seq % 15) + 1
            send_time = time.monotonic()
            sock.sendto(build_ddp_packet(seq, value), (args.ddp_host, DDP_PORT))
            sent.append((send_time, seq, value))
            if args.ack:
                pending[seq].append((value, send_time))

        settle_until = time.monotonic() + 1.5
        while time.monotonic() < settle_until:
            if args.ack:
                drain_acks()
            time.sleep(0.01)

        rows = link.stop_capture()
        print(f"Captured {len(rows)} serial log rows (Compact Motion Log)")
        log_path = args.out.rsplit(".", 1)[0] + ".log"
        with open(log_path, "w") as f:
            for r in rows:
                f.write(r + "\n")
        print(f"Wrote {log_path}")

        # DDP reception diagnostics (DRX/DDPREJ/RSSI) no longer go to
        # Serial at all - retrieve the in-RAM buffer over HTTP instead.
        rx_log_text = http_get_ddp_rx_log(args.ddp_host)
        rx_log_path = args.out.rsplit(".", 1)[0] + ".ddprxlog"
        with open(rx_log_path, "w") as f:
            f.write(rx_log_text)
        rx_log_lines = [ln for ln in rx_log_text.splitlines() if ln]
        print(f"Fetched {len(rx_log_lines)} DDP-rx-log rows over HTTP -> {rx_log_path}")

        http_set_tunable(args.ddp_host, "ddpRxLog", 0)
        http_set_tunable(args.ddp_host, "ddpAck", 0)

        final_status = link.get_status()
        print("Status after test:", final_status)

        analyze(sent, rows, rx_log_lines, acks, args, final_status)
    finally:
        link.close()


def analyze(sent, rows, rx_log_lines, acks, args, final_status):
    motion_rows = []
    for line in rows:
        text = line[2:] if line.startswith("# ") else line
        r = parse_row(text)
        if r is not None:
            motion_rows.append(r)

    drx, ddprej, rssi = [], [], []
    for text in rx_log_lines:
        parts = text.split(",")
        if len(parts) < 2:
            continue
        tag = parts[1]
        if tag == "DRX" and len(parts) >= 4:
            try:
                drx.append((int(parts[0]), int(parts[2]), int(parts[3])))
            except ValueError:
                pass
        elif tag == "DDPREJ" and len(parts) >= 4:
            try:
                ddprej.append((int(parts[0]), int(parts[2]), int(parts[3])))
            except ValueError:
                pass
        elif tag == "RSSI" and len(parts) >= 3:
            try:
                rssi.append((int(parts[0]), int(parts[2])))
            except ValueError:
                pass

    print("\n=== TRACKING (Compact Motion Log) ===")
    print(f"  rows: {len(motion_rows)}")
    if motion_rows:
        errors = [abs(r["lag"]) for r in motion_rows]
        print(f"  max |error|: {max(errors)}  mean |error|: {sum(errors)/len(errors):.1f}")
        near_zero_speed = sum(1 for r in motion_rows if r["curSpeedHz"] == 0)
        print(f"  rows with curSpeedHz==0: {near_zero_speed}/{len(motion_rows)} "
              f"({100.0*near_zero_speed/len(motion_rows):.1f}%)")
        # Flag any stretch where consecutive rows show identical curPos for
        # a long real-time span - the freeze signature from the earlier bug.
        freeze_events = []
        run_start_idx = 0
        for i in range(1, len(motion_rows)):
            if motion_rows[i]["curPos"] != motion_rows[i - 1]["curPos"]:
                span_ms = motion_rows[i - 1]["ms"] - motion_rows[run_start_idx]["ms"]
                if span_ms > 500:
                    freeze_events.append((motion_rows[run_start_idx]["ms"], span_ms,
                                           motion_rows[run_start_idx]["curPos"]))
                run_start_idx = i
        print(f"  freeze events (position unchanged >500ms): {len(freeze_events)}")
        for ms, span, pos in freeze_events:
            print(f"    at ms={ms}, lasted {span}ms, pos={pos}")

    print("\n=== DDP RECEPTION (device-side) ===")
    print(f"  DRX (accepted): {len(drx)}   DDPREJ (rejected): {len(ddprej)}")
    print(f"  sent: {len(sent)}   total device saw: {len(drx)+len(ddprej)} "
          f"({100.0*(len(drx)+len(ddprej))/max(1,len(sent)):.1f}%)")
    if drx:
        gaps = [drx[i][0] - drx[i-1][0] for i in range(1, len(drx))]
        big_gaps = [(drx[i][0], gaps[i-1]) for i in range(1, len(drx)) if gaps[i-1] > 200]
        print(f"  device-side inter-packet gaps > 200ms: {len(big_gaps)}")
        for ms, gap in big_gaps[:20]:
            print(f"    at device ms={ms}, gap={gap}ms")

    if args.ack:
        print("\n=== ROUND-TRIP TIME ===")
        print(f"  acks: {len(acks)}/{len(sent)} ({100.0*len(acks)/max(1,len(sent)):.1f}%)")
        if acks:
            rtts_ms = sorted(a[2] * 1000.0 for a in acks)
            n = len(rtts_ms)
            print(f"  RTT (ms): min={rtts_ms[0]:.1f} p50={rtts_ms[n//2]:.1f} "
                  f"p95={rtts_ms[int(n*0.95)]:.1f} max={rtts_ms[-1]:.1f} mean={sum(rtts_ms)/n:.1f}")
            big_rtt = [(a[0], a[2]*1000.0) for a in acks if a[2] > 0.15]
            print(f"  RTTs > 150ms: {len(big_rtt)}")

    if rssi:
        vals = [r[1] for r in rssi]
        print(f"\n=== RSSI ===  min={min(vals)} max={max(vals)} mean={sum(vals)/len(vals):.1f}")

    if final_status and final_status.get("homed") != 1:
        print("\n  WARNING: device ended this test NOT HOMED - a real trip/loss event occurred.")

    make_plot(sent, motion_rows, drx, acks, rssi, args)


def make_plot(sent, motion_rows, drx, acks, rssi, args):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available - skipping plot")
        return

    fig, axes = plt.subplots(5, 1, figsize=(12, 15), sharex=False)

    if motion_rows:
        t0 = motion_rows[0]["ms"]
        axes[0].plot([(r["ms"]-t0)/1000.0 for r in motion_rows], [r["cmdPos"] for r in motion_rows],
                     label="Commanded", linewidth=0.8)
        axes[0].plot([(r["ms"]-t0)/1000.0 for r in motion_rows], [r["curPos"] for r in motion_rows],
                     label="Actual", linewidth=0.8)
        axes[0].legend()
    axes[0].set_ylabel("Position (steps)")
    axes[0].set_title(f"{args.label} - PID tracking during continuous DDP stream")

    if motion_rows:
        t0 = motion_rows[0]["ms"]
        axes[1].plot([(r["ms"]-t0)/1000.0 for r in motion_rows], [r["curSpeedHz"] for r in motion_rows],
                     linewidth=0.6)
    axes[1].set_ylabel("Actual speed (Hz)")

    t0 = sent[0][0] if sent else 0
    axes[2].plot([s[0]-t0 for s in sent], [s[2] for s in sent], label="Sent", linewidth=0.8)
    if drx:
        d0 = drx[0][0]
        axes[2].plot([(d[0]-d0)/1000.0 for d in drx], [d[2] for d in drx],
                     label="Received (device)", linewidth=0.8, alpha=0.7)
    axes[2].set_ylabel("DDP value")
    axes[2].legend()
    axes[2].set_title("Sent vs. received DDP value")

    if acks:
        t0a = acks[0][0]
        axes[3].plot([a[0]-t0a for a in acks], [a[2]*1000.0 for a in acks], ".", markersize=2)
    axes[3].set_ylabel("RTT (ms)")
    axes[3].set_title("Round-trip time during motion")

    if rssi:
        r0 = rssi[0][0]
        axes[4].plot([(r[0]-r0)/1000.0 for r in rssi], [r[1] for r in rssi])
    axes[4].set_ylabel("RSSI (dBm)")
    axes[4].set_xlabel("Time (s)")
    axes[4].set_title("WiFi signal strength during motion")

    fig.tight_layout()
    fig.savefig(args.out, dpi=110)
    print(f"\nWrote {args.out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--ddp-host", required=True)
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--rate-hz", type=float, default=40.0)
    parser.add_argument("--ack", action="store_true")
    parser.add_argument("--label", default="pid_motion_reception")
    parser.add_argument("--out", default="tuning_runs/pid_motion_reception_test.png")
    args = parser.parse_args()
    run_test(args)


if __name__ == "__main__":
    main()
