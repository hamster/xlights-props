# TODO / Roadmap

## Goal

Turn this into a standalone stepper-mover **+** pixel controller: one ESP32-S3 device on a moving prop that gets both its position and its pixel data over the network via DDP, with no separate LED controller needed on the prop. Target capacity is up to ~500 WS2812 pixels, though most real props will be 50-150. Whether WiFi is reliable enough for combined position + 500-pixel data at a usable frame rate is an open question this roadmap is meant to answer.

Note: checked `git branch -a` / `git stash list` / `git log --all` — there is no leftover branch, stash, or commit anywhere in this repo with prior dual-core work. If there was earlier progress on splitting stepper/LED work across cores, it never made it into git, so treat this as a fresh design rather than something to dig up.

## Fixed/done this session

- [x] **DDP LED channel-offset bug** — `ddp_handler.cpp` used byte offset `3` for where LED data starts; the documented channel layout (and the now-removed ArtNet handler) used byte offset `2` (channel 3). This meant LED colors landed one byte later — and therefore looked different/shifted — than intended. Now uses offset `2`.
- [x] **`totalChannels` status miscalculation** — `html_handler.cpp`'s status JSON added a stray `+1` to the reserved-channels + LED-channels count. Removed; it now reports `2 + ledPixelCount*3`.
- [x] **Docs**: CLAUDE.md's pin table said D9 for WS2812 data; code has always used D4. Corrected to match the code. *(Confirm against the actual board/schematic if you get a chance — the doc was fixed to match firmware, not the other way around.)*
- [x] **ArtNet support removed entirely.** Decision: DDP already scales to the 500-pixel goal for free via its fragmented-packet handling (`header.dataOffset` spans multiple UDP packets with no universe-style ceiling), while ArtNet would have needed real work — subscribing N consecutive universes, per-universe pixel-offset math, UI for universe count — to reach the same target, and every prop in xLights can already have its own protocol per output, so keeping this device DDP-only doesn't constrain the rest of the show. Not worth the ongoing test/debug surface for a capability DDP already has.
  - Removed `artnet_handler.h`/`.cpp`, the `hideakitai/ArtNet` lib dependency, `PROTOCOL_ARTNET` from `protocolType`, the protocol `<select>` and ArtNet config fields from the web UI (settings tab is now "Channel Configuration", DDP-only), and the `artnetChannelsPerUniverseConfig` dead setting.
  - `protocolConfig`/`protocolType` (now just `PROTOCOL_NONE`/`PROTOCOL_DDP`) was left in place as cheap scaffolding in case another protocol (e.g. sACN/E1.31) is ever wanted — but nothing currently exposes a way to pick anything other than DDP.
  - If you ever do want ArtNet or sACN back (e.g. to match other controllers in the show), the multi-universe design notes from the previous version of this doc are in git history (see the commit that introduced this TODO.md) — worth a re-read rather than starting from scratch, since the offset math is the same problem DDP's fragmented handler already solves.

## Priority 1 — Split stepper/protocol handling from LED output across both cores

This is the reason the project moved from the C3 to the S3 in the first place, and it's never actually been implemented — everything (WiFi, web server, DDP receive, stepper dispatch, `FastLED.show()`) still runs serially in one `loop()` on Core 1. See CLAUDE.md's Architecture section.

**Why it matters at 500 pixels:** `FastLED.show()` blocks its calling core for the WS2812 transmission time, roughly 30µs/pixel — about 15ms for 500 pixels, physically required by the WS2812 protocol regardless of which core runs it. Today that 15ms happens inline inside the DDP packet handler, so for that window the device can't parse the next incoming packet, dispatch a stepper move, or serve a web request. At 500 pixels and a typical 20-40Hz update rate from xLights, that's a meaningful fraction of every frame period spent blocked.

Stepper motion itself is *not* blocked by this today — FastAccelStepper generates step pulses from a hardware timer/RMT peripheral independent of `loop()` — so the actual physical movement should stay smooth even now. The real win from splitting cores is keeping protocol *reception* responsive during the LED push, not motion smoothness.

**Suggested approach (not yet designed in detail — worth a dedicated planning pass before coding):**
- Run WiFi + web server + DDP receive + stepper move dispatch on Core 1 (current behavior, unchanged).
- Move the pixel buffer → `FastLED.show()` push to a FreeRTOS task pinned to Core 0.
- The DDP handler writes into the `leds[]` buffer as it does now, then just signals/queues a "show" request instead of calling `FastLED.show()` inline; the Core 0 task performs the actual push.
- Watch for double-buffering vs. tearing: decide whether a partially-updated `leds[]` array mid-push (e.g., a DDP frame arriving while Core 0 is still transmitting the previous frame) is acceptable, or whether you need a double buffer swapped atomically between cores.
- Re-verify the watchdog (`esp_task_wdt`) setup once there's a second task — right now `esp_task_wdt_add(NULL)` only registers the main loop's task.

## Priority 2 — Validate WiFi actually holds up at scale

This is explicitly an open question, not an assumption. Before investing further in Priority 1, worth doing a cheap real-world test:
- Point xLights at the device with a ~150-pixel and a ~500-pixel test model at a normal show frame rate and watch for dropped frames, visible stutter in position moves, or flicker in the pixel output, especially on a busy WiFi network (i.e. actual show night conditions, not a quiet bench).
- Check whether `WiFiUDP`'s default receive buffering is enough at higher sustained packet rates, or whether packets get dropped silently before `ddpUdp.parsePacket()` even sees them.
- This will tell you whether Priority 1 is sufficient, or whether frame-rate/pixel-count guidance needs to be added to the docs (e.g. "500 pixels works but cap update rate to X Hz over WiFi").

## Smaller/follow-up items

- [ ] Web UI: pixel count input already allows up to 1000 (`MAX_LEDS`), no change needed there, but consider adding a hint/warning in the LED settings section once Priority 2 establishes real practical limits over WiFi.
- [ ] Consider whether `stepperControlEnabled = false` (pixel-only prop) should be a first-class documented use case — it already works today (channels 1-2 stay reserved/unused, LEDs still start at channel 3), just isn't called out anywhere.
