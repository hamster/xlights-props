#!/usr/bin/env python3
"""
Drives simultaneous stepper motion + randomized LED pixel data over DDP -
built 2026-09-08 because LED support had gone unexercised by any bench
tool for a while (every existing tuning tool here only ever drove
position; LED_START_OFFSET/updatePixelLedsFragmented() in the firmware
had no repeatable automated test at all). Exercises the combined-channel
DDP path end to end: bytes 0-1 = 16-bit stepper position (MSB-first),
bytes 2+ = 3 bytes/pixel RGB, in the SAME packet - matching how a real
show actually drives this device (one DDP stream carrying both channels
together, not two separate senders), per CLAUDE.md's Channel Layout.

HTTP-only for configuration/verification, UDP for the DDP stream itself -
same reasoning as ddp_continuous_test.py: opening a serial connection
resets the ESP32 via DTR/RTS (see CLAUDE.md), so this can run repeatedly
with zero risk of interrupting or resetting an in-progress bench session.

At 150 pixels the payload is 2 + 150*3 = 452 bytes, comfortably under
DDP_MAX_DATA_SIZE (1472, see ddp_handler.h) - this deliberately stays
single-packet (no DDP offset-field fragmentation) for that pixel count.
Push past ~480 pixels (2 + pixels*3 > 1472) and this script's packets
would need real fragmentation to stay correct - not implemented here,
since 150 was the ask; raise it if a larger pixel count is ever needed.

Usage:
  # One-time device setup (16-bit stepper control + 150 pixels), then run:
  python led_stress_test.py --ddp-host 192.168.10.181 --configure \
      --pixels 150 --duration 30

  # Subsequent runs, device already configured:
  python led_stress_test.py --ddp-host 192.168.10.181 --pixels 150 \
      --duration 30 --pattern chase
"""
import argparse
import json
import random
import re
import socket
import struct
import sys
import threading
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


def http_post(host, path, body, timeout=8.0):
    # Content-Type matters here: ESP32WebServer only populates
    # server.arg("plain") - what handleConfigPost() reads - for a body
    # that isn't application/x-www-form-urlencoded or multipart/form-data
    # (those go through its own &-separated form parser instead, which our
    # newline-separated key=value body doesn't match, and it comes back as
    # "missing request body"). urllib defaults to
    # application/x-www-form-urlencoded whenever data is a plain str/bytes
    # payload on a POST - has to be overridden explicitly.
    req = urllib.request.Request(f"http://{host}{path}",
                                  data=body.encode("utf-8"), method="POST",
                                  headers={"Content-Type": "text/plain"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode("utf-8", errors="replace")


def get_status(host):
    return json.loads(http_get(host, "/status-data"))


def get_led_preview(host):
    return json.loads(http_get(host, "/led-preview"))


def get_boot_markers(host):
    """Every '=== Boot #N ===' line currently in the persistent log - the
    authoritative "did the device actually reset" signal, same one used to
    verify the Debug-tab crash fix: flushed unconditionally and
    immediately on every real boot (persist_log.cpp's initPersistLog()),
    so comparing this list before/after is reliable where comparing
    uptimeSecs is NOT (that field is only the seconds-of-the-current-
    minute component, not a running total - it wrapping looks exactly
    like a reset if you don't know that)."""
    text = http_get(host, "/persist-log", timeout=10.0)
    return re.findall(r"Boot #(\d+)", text)


def configure_device(host, pixels, timeout=60.0):
    print(f"Configuring device: stepperControl=1, control16Bit=1, "
          f"ledPixelCount={pixels}...")
    body = f"stepperControl=1\ncontrol16Bit=1\nledPixelCount={pixels}\n"
    result = http_post(host, "/config", body)
    print(f"  {result.strip()}")

    # ledPixelCount changes the RAM/NVS value immediately, but
    # led_handler.cpp's FastLED.addLeds<...>(leds, totalPixels) - what
    # actually sizes the live pixel buffer - only runs once, at boot, from
    # the value NVS had *then*. A device that has never been configured
    # with LEDs before (ledPixelCount==0 at last boot) is still running
    # with LEDs completely uninitialized until this reboot happens - the
    # config write alone is not enough, confirmed by handleConfigPost()'s
    # own "reboot recommended" response text.
    print("  Rebooting so the new LED pixel count actually takes effect...")
    try:
        http_get(host, "/reboot", timeout=3.0)
    except Exception:
        pass  # the device may drop the connection mid-response while rebooting - expected
    time.sleep(2.0)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            get_status(host)
            print("  Device back online after reboot.")
            return
        except Exception:
            time.sleep(1.0)
    raise RuntimeError(f"Device did not come back online within {timeout:.0f}s of reboot")


def ensure_homed(host, timeout=90.0):
    st = get_status(host)
    if st.get("homed"):
        print(f"Already homed (bottomPosition={st.get('bottomPosition')}).")
        return
    if st.get("isHoming"):
        print("Homing already in progress - waiting...")
    else:
        print("Not homed - starting homing...")
        result = json.loads(http_get(host, "/home"))
        if not result.get("success"):
            raise RuntimeError(f"Failed to start homing: {result}")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        st = get_status(host)
        if st.get("homed"):
            print(f"Homed. bottomPosition={st.get('bottomPosition')}")
            return
        if st.get("homingError"):
            raise RuntimeError("Homing reported an error - check /persist-log")
        time.sleep(1.0)
    raise RuntimeError(f"Homing did not complete within {timeout:.0f}s")


def build_packet(seq, position, pixel_bytes):
    payload = bytes([(position >> 8) & 0xFF, position & 0xFF]) + pixel_bytes
    header = struct.pack(DDP_HEADER_FMT, DDP_FLAGS_PUSH_V1, seq, DDP_DATA_TYPE_RGB,
                          DDP_DESTINATION, 0, len(payload))
    return header + payload


def triangle_position(elapsed, period, bits=16):
    """Same wave shape as ddp_continuous_test.py's triangle_value_wrapped()
    - continuously repeating, not a single ramp - so this exercises real
    direction reversals throughout the run, not just one."""
    period = max(0.01, period)
    phase = (elapsed % period) / period
    full = 65535.0 if bits == 16 else 255.0
    value = phase * 2.0 * full if phase < 0.5 else (1.0 - phase) * 2.0 * full
    return max(0, min(int(full), int(round(value))))


def pattern_random(pixels, frame, rng):
    """Fully independent random RGB per pixel, every frame - the simplest
    possible proof the firmware is decoding fresh per-pixel data each
    packet rather than showing something stale or aliased."""
    return bytes(rng.randrange(256) for _ in range(pixels * 3))


def pattern_chase(pixels, frame, rng, tail=6):
    """A single bright dot with a fading tail, advancing one pixel/frame,
    re-tinted each lap - easier to visually confirm as "a moving pattern"
    on real hardware than pure noise, while still changing every frame so
    the same before/after freshness check still applies."""
    head = frame % pixels
    out = bytearray(pixels * 3)
    hue_channel = (frame // max(1, pixels)) % 3
    for i in range(tail):
        idx = (head - i) % pixels
        level = int(255 * (1.0 - i / tail))
        px = [0, 0, 0]
        px[hue_channel] = level
        out[idx * 3:idx * 3 + 3] = bytes(px)
    return bytes(out)


PATTERNS = {"random": pattern_random, "chase": pattern_chase}


class StatusPoller(threading.Thread):
    """Polls /status-data and /led-preview on its own thread, decoupled
    from the DDP send loop's timing entirely. This matters: an earlier
    version of this script polled inline in the send loop, and one slow
    HTTP round-trip (an 8s urllib timeout, hit once on a real bench run)
    stalled DDP sending long enough to trip the device's own
    ledBlankTimeConfig safety blanking (5s of no protocol update) - a
    real, correct firmware behavior, but a self-inflicted false failure
    in this script's own verification, not an actual bug in the firmware.
    Running polls on a background thread means even a fully hung HTTP call
    here can never introduce a gap in the packet stream."""

    def __init__(self, host, interval=2.0):
        super().__init__(daemon=True)
        self.host = host
        self.interval = interval
        self._stop_evt = threading.Event()
        self.preview_samples = []
        # (position, encoderCount) at each poll - position alone is only
        # FastAccelStepper's own step-pulse bookkeeping (getCurrentPosition()
        # per encoder_handler.h's own header comment) and has no way to
        # notice a commanded step that didn't physically happen; the encoder
        # is real, independent ground truth. Recorded together so the final
        # verification can confirm the encoder is actually moving *with*
        # position, not just that either one alone is changing.
        self.motion_samples = []
        self.poll_errors = 0

    def run(self):
        while not self._stop_evt.is_set():
            try:
                preview = get_led_preview(self.host).get("ledPreview", "")
                s = get_status(self.host)
                self.preview_samples.append(preview)
                self.motion_samples.append((s.get("position"), s.get("encoderCount"),
                                             s.get("encoderMissed")))
                print(f"  [poll] position={s.get('position')}  "
                      f"encoderCount={s.get('encoderCount')}  "
                      f"encoderMissed={s.get('encoderMissed')}  "
                      f"ledMaxPixelsReceived={s.get('ledMaxPixelsReceived')}  "
                      f"homed={s.get('homed')}  ledsBlanked={s.get('ledsBlanked')}")
            except Exception as e:
                self.poll_errors += 1
                print(f"  [poll] WARNING: status/preview poll failed: {e}")
            self._stop_evt.wait(self.interval)

    def stop(self):
        self._stop_evt.set()


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ddp-host", required=True)
    parser.add_argument("--pixels", type=int, default=150)
    parser.add_argument("--duration", type=float, default=30.0, help="seconds")
    parser.add_argument("--rate-hz", type=float, default=30.0)
    parser.add_argument("--period", type=float, default=8.0,
                         help="stepper triangle-wave period, seconds")
    parser.add_argument("--pattern", choices=sorted(PATTERNS), default="random")
    parser.add_argument("--bits", type=int, choices=(8, 16), default=16)
    parser.add_argument("--configure", action="store_true",
                         help="POST /config to set stepperControl=1, "
                              "control16Bit=1, ledPixelCount=<--pixels> "
                              "before running")
    parser.add_argument("--seed", type=int, default=None)
    args = parser.parse_args()

    rng = random.Random(args.seed)

    if args.configure:
        configure_device(args.ddp_host, args.pixels)

    st = get_status(args.ddp_host)
    if not st.get("control16Bit") and args.bits == 16:
        print("WARNING: device reports control16Bit=false but --bits=16 - "
              "position bytes won't be interpreted the way this script "
              "intends. Pass --configure, or --bits 8, or fix the device's "
              "Channel Configuration first.")
    if st.get("ledPixelCount", 0) < args.pixels:
        print(f"WARNING: device ledPixelCount={st.get('ledPixelCount')} is "
              f"less than --pixels={args.pixels} - only the configured "
              f"count will actually render; the rest of each packet's "
              f"pixel data is still sent and parsed, just unused.")
    if not st.get("encoderInitialized"):
        print("WARNING: encoder not initialized on this device - motion "
              "verification will fall back to position (step-pulse "
              "bookkeeping) alone, which is NOT real ground truth (see "
              "encoder_handler.h: it can't tell a commanded step from one "
              "that actually happened physically).")

    ensure_homed(args.ddp_host)

    boots_before = get_boot_markers(args.ddp_host)
    print(f"Boot markers before run: ...{boots_before[-3:]}")

    st_before = get_status(args.ddp_host)
    pos_before = st_before.get("position")
    encoder_before = st_before.get("encoderCount")
    encoder_missed_before = st_before.get("encoderMissed", 0)
    preview_before = get_led_preview(args.ddp_host).get("ledPreview", "")

    pattern_fn = PATTERNS[args.pattern]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"\nSending {args.duration:.0f}s @ {args.rate_hz:.0f}fps to "
          f"{args.ddp_host}:{DDP_PORT} - {args.pixels} pixels, "
          f"pattern={args.pattern}, stepper triangle period={args.period:.1f}s, "
          f"bits={args.bits}...")

    poller = StatusPoller(args.ddp_host)
    poller.start()

    # Pure UDP sends only in this loop, deliberately - see StatusPoller's
    # docstring for why an HTTP call here is dangerous (it very nearly
    # ruined this exact tool's own bench verification, not a hypothetical).
    interval = 1.0 / args.rate_hz
    start = time.monotonic()
    seq = 0
    sent = 0
    frame = 0
    while True:
        now = time.monotonic()
        elapsed = now - start
        if elapsed >= args.duration:
            break
        position = triangle_position(elapsed, args.period, args.bits)
        pixel_bytes = pattern_fn(args.pixels, frame, rng)
        seq = (seq % 15) + 1
        sock.sendto(build_packet(seq, position, pixel_bytes),
                    (args.ddp_host, DDP_PORT))
        sent += 1
        frame += 1
        time.sleep(interval)
    sock.close()
    elapsed_total = time.monotonic() - start
    print(f"Sent {sent} packets over {elapsed_total:.1f}s "
          f"({sent/elapsed_total:.1f} actual fps)")

    poller.stop()
    poller.join(timeout=5.0)

    time.sleep(0.5)
    st_after = get_status(args.ddp_host)
    preview_after = get_led_preview(args.ddp_host).get("ledPreview", "")
    boots_after = get_boot_markers(args.ddp_host)
    preview_samples = [preview_before] + poller.preview_samples
    motion_samples = ([(pos_before, encoder_before, encoder_missed_before)]
                       + poller.motion_samples
                       + [(st_after.get("position"), st_after.get("encoderCount"),
                           st_after.get("encoderMissed"))])

    print("\n=== Verification ===")
    ok = True

    if boots_after == boots_before:
        print(f"[OK]   No reboot during the run (boot markers unchanged: "
              f"...{boots_after[-1:]})")
    else:
        print(f"[FAIL] Device rebooted during the run! before=...{boots_before[-2:]} "
              f"after=...{boots_after[-2:]}")
        ok = False

    pos_after = st_after.get("position")
    if pos_after != pos_before:
        print(f"[OK]   Stepper position changed ({pos_before} -> {pos_after}) - "
              f"trolley genuinely COMMANDED to move (this is FastAccelStepper's "
              f"own step-pulse bookkeeping, per encoder_handler.h - not proof "
              f"of real physical motion by itself; see the encoder checks below "
              f"for that)")
    else:
        print(f"[FAIL] Stepper position unchanged ({pos_before}) - trolley did "
              f"not move (not homed? stuck?)")
        ok = False

    # Real, independent ground truth - see encoder_handler.h's header
    # comment: position alone can't distinguish a commanded step from one
    # that didn't actually happen physically. Only meaningful if the
    # encoder was actually initialized (checked and warned about above).
    encoder_after = st_after.get("encoderCount")
    encoder_missed_after = st_after.get("encoderMissed", 0)
    if encoder_before is None or encoder_after is None:
        print("[SKIP] Encoder not available on this device - motion above "
              "is only verified via step-pulse bookkeeping, not real "
              "ground truth")
    else:
        if encoder_after != encoder_before:
            print(f"[OK]   Encoder count changed ({encoder_before} -> "
                  f"{encoder_after}) - real, physical ground-truth motion "
                  f"confirmed, not just commanded steps")
        else:
            print(f"[FAIL] Encoder count unchanged ({encoder_before}) despite "
                  f"position changing - stepper may be commanding motion "
                  f"that isn't actually happening physically (stalled/"
                  f"disconnected?)")
            ok = False

        encoder_range = (max(e for _, e, _ in motion_samples if e is not None)
                          - min(e for _, e, _ in motion_samples if e is not None))
        if encoder_range >= 50:
            print(f"[OK]   Encoder count ranged over {encoder_range} counts "
                  f"across the run - sustained real motion, not just a "
                  f"couple of stray ticks")
        else:
            print(f"[FAIL] Encoder count only ranged over {encoder_range} "
                  f"counts across the run - suspiciously little real motion "
                  f"for a {args.duration:.0f}s run with a {args.period:.1f}s "
                  f"triangle period")
            ok = False

        # Direction correlation: between consecutive samples, position and
        # encoderCount should move the same way (both this project's sign
        # convention, per encoder_handler.h: "increasing count = increasing
        # stepper position" - confirmed on the bench, not just assumed).
        # Small/noisy deltas near a reversal or a poll's own timing jitter
        # are excluded rather than treated as disagreements.
        agree, disagree = 0, 0
        for (p0, e0, _), (p1, e1, _) in zip(motion_samples, motion_samples[1:]):
            if None in (p0, e0, p1, e1):
                continue
            dp, de = p1 - p0, e1 - e0
            if abs(dp) < 20 or abs(de) < 20:
                continue  # too small to trust the sign of over one poll interval
            if (dp > 0) == (de > 0):
                agree += 1
            else:
                disagree += 1
        if agree + disagree == 0:
            print("[NOTE] Not enough distinct samples to check position/encoder "
                  "direction agreement (short run or coarse poll interval)")
        elif disagree == 0:
            print(f"[OK]   Position and encoder agreed on direction across "
                  f"all {agree} comparable sample intervals")
        else:
            print(f"[FAIL] Position and encoder DISAGREED on direction in "
                  f"{disagree}/{agree + disagree} sample intervals - possible "
                  f"sign/wiring/skipped-step issue")
            ok = False

        missed_delta = (encoder_missed_after or 0) - (encoder_missed_before or 0)
        if missed_delta <= 0:
            print(f"[OK]   No new missed quadrature transitions during the run "
                  f"(encoderMissed unchanged at {encoder_missed_after})")
        elif encoder_range > 0 and missed_delta / max(1, encoder_range) < 0.01:
            print(f"[NOTE] {missed_delta} new missed transition(s) during the "
                  f"run (encoderMissed {encoder_missed_before} -> "
                  f"{encoder_missed_after}) - small relative to {encoder_range} "
                  f"real counts, not flagged as a failure "
                  f"(encoder_handler.h says this should be ~0; worth a look "
                  f"if it keeps growing across runs)")
        else:
            print(f"[FAIL] {missed_delta} new missed transitions during the "
                  f"run, large relative to {encoder_range} real counts - the "
                  f"poll rate may not be keeping up (see encoder_handler.h)")
            ok = False

    distinct_previews = len(set(preview_samples + [preview_after]))
    if distinct_previews > 1:
        print(f"[OK]   LED preview changed across the run ({distinct_previews} "
              f"distinct samples of {len(preview_samples)+1} taken) - pixels "
              f"genuinely updating from DDP data")
    else:
        print(f"[FAIL] LED preview never changed - LEDs not receiving/rendering "
              f"fresh data")
        ok = False

    max_recv = st_after.get("ledMaxPixelsReceived", 0)
    if max_recv >= args.pixels:
        print(f"[OK]   ledMaxPixelsReceived={max_recv} >= --pixels={args.pixels} - "
              f"full pixel payload reached the firmware, not truncated")
    else:
        print(f"[FAIL] ledMaxPixelsReceived={max_recv} < --pixels={args.pixels} - "
              f"payload was truncated somewhere")
        ok = False

    if not st_after.get("homed"):
        print("[FAIL] Device no longer reports homed after the run")
        ok = False

    if st_after.get("ledsBlanked"):
        print("[FAIL] Device reports ledsBlanked=true right after the run - "
              "means a real gap over ledBlankTimeConfig seconds happened "
              "somewhere in the send loop itself (not this script's own "
              "polling - that runs on a separate thread precisely to rule "
              "this out)")
        ok = False
    else:
        print("[OK]   ledsBlanked=false - no gap in the DDP stream large "
              "enough to trip the blank timer")

    if poller.poll_errors:
        print(f"[NOTE] {poller.poll_errors} background status poll(s) failed "
              f"during the run (network hiccup, not fatal - the DDP send "
              f"loop itself is on a separate thread and unaffected)")

    print(f"\n{'PASS' if ok else 'FAIL - see above'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
