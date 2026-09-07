#!/usr/bin/env python3
"""
DDP reception quality test - pure network/reception characterization, no
stepper motion at all (the device is never homed, so nothing can move
regardless of what DDP commands say - see CLAUDE.md/TODO.md's TRACK_MODE_PID
section for why this matters: the goal here is isolating "how well does DDP
reception track a live, continuous, 40fps-cadence stream" from any question
about tracking-mode/stepper behavior).

Sends a continuous triangle wave at a real show's typical cadence (default
40 packets/sec, matching the user's own sequencing frame rate) for a
configurable duration, while:
  - Recording every packet this script actually sent (time, seq, value) -
    "how smooth is sending."
  - Capturing the device's own per-packet receive log (new $SET ddpRxLog
    tunable - see ddp_handler.h) - "how smooth is reception," independent
    of anything this script assumes about its own sends.
  - Optionally (--ack) asking the device to echo a tiny ACK back for every
    packet it receives at the UDP layer (new $SET ddpAck tunable), letting
    this script directly measure round-trip time and detect genuine loss
    (a send that never gets acked) - decoupled from the firmware's own
    DDP sequence-acceptance logic, which only tells you about packets that
    both arrived AND passed that check.
  - Sampling the device's own WiFi signal strength (RSSI) throughout.

Usage:
  python ddp_reception_test.py --port COM7 --ddp-host 192.168.1.50 \
      --duration 60 --rate-hz 40 --ack

Output: a summary printed to the console, a raw .log of the device's
captured serial output, and a .png with four panels (sent vs. received
value over time, per-packet RTT over time, RTT histogram, RSSI over time).
"""
import argparse
import os
import select
import socket
import sys
import time
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tuning_harness import DeviceLink, triangle_value, build_ddp_packet

DDP_PORT = 4048


def run_test(args):
    link = DeviceLink(args.port, args.baud)
    try:
        status = link.get_status()
        print("Initial status:", status)
        if status and status.get("homed") == 1:
            print("NOTE: device is currently homed - this test never sends $HOME and never")
            print("      relies on trackMode, so it won't command any motion regardless, but")
            print("      if you want to be extra sure nothing can move, power-cycle first so")
            print("      it comes up unhomed.")

        print(link.set_tunable("protocolDebug", 0))
        print(link.set_tunable("compactLog", 0))
        print(link.set_tunable("ddpRxLog", 1))
        print(link.set_tunable("ddpAck", 1 if args.ack else 0))

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setblocking(False)

        # Per-seq FIFO of (value, send_time) for matching an incoming ACK
        # back to the specific send it belongs to - seq numbers cycle 1-15,
        # so this is scoped per-seq to avoid matching against a much older
        # send that happened to reuse the same number.
        pending = {s: deque() for s in range(1, 16)}
        acks = []  # (recv_time, seq, rtt_s)

        def drain_acks():
            # Non-blocking, called inline from the send loop below (not a
            # separate thread) - deliberately, after an earlier version
            # using a background thread produced RTT readings with a
            # spurious ~400ms cluster that turned out to be a measurement
            # artifact, not a real network/device delay: the device's own
            # per-packet receive timestamps (independent of anything this
            # script measures) stayed clean throughout - gaps mostly
            # 2-77ms, matching the ~25ms send interval, with only a
            # handful of brief outliers, no sustained plateau - in both
            # the run that showed the spurious cluster and the run that
            # didn't. Most likely cause: the ACK-receiver thread not
            # getting scheduled promptly against the tight-timed send
            # loop (GIL contention and/or general OS scheduling jitter on
            # this machine, not anything this test is actually trying to
            # measure). Single-threaded + non-blocking + polled every loop
            # iteration removes that whole class of confound.
            while True:
                try:
                    data, _addr = sock.recvfrom(64)
                except BlockingIOError:
                    return
                except OSError:
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

        sent = []  # (send_time, seq, value)
        interval = 1.0 / args.rate_hz
        total_ticks = max(1, round(args.duration * args.rate_hz))
        start = time.monotonic()
        seq = 0
        print(f"\nSending {args.duration:.0f}s of continuous {args.rate_hz:.0f}fps triangle wave "
              f"to {args.ddp_host}:{DDP_PORT} ({'with' if args.ack else 'without'} ACK)...")
        for tick in range(total_ticks + 1):
            target_t = tick * interval
            # Wait for this tick's scheduled time, but poll for ACKs at a
            # fine grain (capped sub-sleeps) rather than one long sleep -
            # keeps ACK receive timestamps close to when data actually
            # arrived instead of only being noticed at the next send.
            while True:
                now = time.monotonic()
                behind_s = now - (start + target_t)
                if behind_s >= 0:
                    break
                wait_s = min(0.002, -behind_s)
                if args.ack:
                    drain_acks()
                    select.select([sock], [], [], wait_s)  # already waits up to wait_s either way
                else:
                    time.sleep(wait_s)
            if behind_s > 1.0:
                # Real stall, not ordinary jitter - resync rather than flood
                # a catch-up burst. See send_triangle_wave()'s identical
                # reasoning in tuning_harness.py.
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

        # Let in-flight ACKs (and any real device-side lag) settle before
        # stopping capture, draining whatever arrives during that window.
        settle_until = time.monotonic() + 1.0
        while time.monotonic() < settle_until:
            if args.ack:
                drain_acks()
            time.sleep(0.01)

        rows = link.stop_capture()
        print(f"Captured {len(rows)} device log rows")
        log_path = args.out.rsplit(".", 1)[0] + ".log" if "." in args.out else args.out + ".log"
        with open(log_path, "w") as f:
            for r in rows:
                f.write(r + "\n")
        print(f"Wrote {log_path}")

        analyze(sent, rows, acks, args)
    finally:
        link.close()


def analyze(sent, rows, acks, args):
    print("\n=== SENT (this script's own send loop) ===")
    print(f"  packets sent: {len(sent)}")
    deltas = [sent[i][2] - sent[i - 1][2] for i in range(1, len(sent))]
    jumpy = sum(1 for d in deltas if abs(d) > 1)
    print(f"  value transitions with |delta|>1: {jumpy}/{len(deltas)} "
          f"({100.0 * jumpy / max(1, len(deltas)):.1f}%)")

    drx = []   # (device_ms, seq, value)
    ddprej = []  # (device_ms, seq, last_accepted_seq)
    rssi = []  # (device_ms, dbm)
    for line in rows:
        text = line[2:] if line.startswith("# ") else line
        parts = text.split(",")
        if len(parts) < 3:
            continue
        try:
            ms = int(parts[0])
        except ValueError:
            continue
        tag = parts[1]
        if tag == "DRX" and len(parts) >= 4:
            drx.append((ms, int(parts[2]), int(parts[3])))
        elif tag == "DDPREJ" and len(parts) >= 4:
            ddprej.append((ms, int(parts[2]), int(parts[3])))
        elif tag == "RSSI" and len(parts) >= 3:
            rssi.append((ms, int(parts[2])))

    print("\n=== RECEIVED (device's own per-packet log, $SET ddpRxLog) ===")
    print(f"  accepted+parsed (DRX): {len(drx)}")
    print(f"  rejected as out-of-order/duplicate (DDPREJ): {len(ddprej)}")
    total_seen = len(drx) + len(ddprej)
    print(f"  total packets the device's UDP layer reports seeing: {total_seen} "
          f"(vs. {len(sent)} sent - {100.0 * total_seen / max(1, len(sent)):.1f}%)")
    if drx:
        rvals = [d[2] for d in drx]
        rdeltas = [rvals[i] - rvals[i - 1] for i in range(1, len(rvals))]
        rjumpy = sum(1 for d in rdeltas if abs(d) > 1)
        print(f"  received value transitions with |delta|>1: {rjumpy}/{len(rdeltas)} "
              f"({100.0 * rjumpy / max(1, len(rdeltas)):.1f}%)")

    if args.ack:
        print("\n=== ROUND-TRIP TIME (ACK-based, $SET ddpAck) ===")
        print(f"  acks received: {len(acks)} / {len(sent)} sent "
              f"({100.0 * len(acks) / max(1, len(sent)):.1f}% - unacked sends are either "
              f"lost outbound, lost on the ack's return trip, or still in flight)")
        if acks:
            rtts_ms = sorted(a[2] * 1000.0 for a in acks)
            n = len(rtts_ms)
            print(f"  RTT (ms): min={rtts_ms[0]:.1f} p50={rtts_ms[n//2]:.1f} "
                  f"p95={rtts_ms[int(n*0.95)]:.1f} max={rtts_ms[-1]:.1f} "
                  f"mean={sum(rtts_ms)/n:.1f}")

    if rssi:
        vals = [r[1] for r in rssi]
        print(f"\n=== RSSI (dBm, sampled every ~500ms) ===")
        print(f"  min={min(vals)} max={max(vals)} mean={sum(vals)/len(vals):.1f} "
              f"(closer to 0 is stronger; below ~-75 is typically getting marginal)")

    make_plot(sent, drx, acks, rssi, args)


def make_plot(sent, drx, acks, rssi, args):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available - skipping plot")
        return

    fig, axes = plt.subplots(4, 1, figsize=(12, 12), sharex=False)

    t0 = sent[0][0] if sent else 0
    axes[0].plot([s[0] - t0 for s in sent], [s[2] for s in sent], label="Sent", linewidth=0.8)
    if drx:
        # Device timestamps are in a different clock domain (device millis()
        # vs. this script's time.monotonic()) - normalize each to start at 0
        # so the two curves are visually comparable in shape, not absolute
        # offset.
        d0 = drx[0][0]
        axes[0].plot([(d[0] - d0) / 1000.0 for d in drx], [d[2] for d in drx],
                     label="Received (device)", linewidth=0.8, alpha=0.7)
    axes[0].set_ylabel("DDP value")
    axes[0].set_xlabel("Time (s, each trace zeroed to its own start)")
    axes[0].set_title(f"{args.label} - sent vs. received value")
    axes[0].legend()

    if acks:
        t0a = acks[0][0]
        axes[1].plot([a[0] - t0a for a in acks], [a[2] * 1000.0 for a in acks],
                     ".", markersize=2)
    axes[1].set_ylabel("RTT (ms)")
    axes[1].set_xlabel("Time (s)")
    axes[1].set_title("Round-trip time over the run")

    if acks:
        axes[2].hist([a[2] * 1000.0 for a in acks], bins=50)
    axes[2].set_xlabel("RTT (ms)")
    axes[2].set_ylabel("count")
    axes[2].set_title("RTT histogram")

    if rssi:
        r0 = rssi[0][0]
        axes[3].plot([(r[0] - r0) / 1000.0 for r in rssi], [r[1] for r in rssi])
    axes[3].set_ylabel("RSSI (dBm)")
    axes[3].set_xlabel("Time (s)")
    axes[3].set_title("WiFi signal strength over the run")

    fig.tight_layout()
    fig.savefig(args.out, dpi=110)
    print(f"\nWrote {args.out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--ddp-host", required=True)
    parser.add_argument("--duration", type=float, default=60.0, help="seconds of continuous triangle wave")
    parser.add_argument("--rate-hz", type=float, default=40.0, help="send rate, matches show fps")
    parser.add_argument("--ack", action="store_true", help="enable device ACK-based RTT/loss measurement")
    parser.add_argument("--label", default="ddp_reception")
    parser.add_argument("--out", default="tuning_runs/ddp_reception_test.png")
    args = parser.parse_args()
    run_test(args)


if __name__ == "__main__":
    main()
