# TODO / Roadmap

## Goal

Turn this into a standalone stepper-mover **+** pixel controller: one ESP32-S3 device on a moving prop that gets both its position and its pixel data over the network via DDP, with no separate LED controller needed on the prop. Target capacity is up to ~500 WS2812 pixels, though most real props will be 50-150. Whether WiFi is reliable enough for combined position + 500-pixel data at a usable frame rate is an open question this roadmap is meant to answer.

Note: checked `git branch -a` / `git stash list` / `git log --all` — there is no leftover branch, stash, or commit anywhere in this repo with prior dual-core work. If there was earlier progress on splitting stepper/LED work across cores, it never made it into git, so treat this as a fresh design rather than something to dig up.

## Open TODOs - curated, reviewed 2026-09-07

This is the live tracking list. Everything below this section is the
dated session history that produced these items - kept for the full
backstory/rationale, but no longer the place to look for "what's still
open." 29 stale/resolved/duplicate items were closed out (marked `[x]`
in place, historical record preserved) during this review; see the
individual entries below for what each one was.

### Wave 1 - safety-relevant, still genuinely open

- [x] **Stale-tick watchdog in `updatePidMode()`** - implemented and
  bench-verified 2026-09-07 (see the "Needed: a stale-tick watchdog"
  section below for the original design). Reproduced the exact hazard
  directly: a genuinely large (98KB, 1.7s) `/compact-log` fetch during
  active continuous-run motion now freezes position exactly across that
  window instead of coasting, then settles cleanly afterward.
- [ ] **Root-cause the Kd=0.1 permanent freeze/corruption bug** - a real,
  serious, reproduced-once issue (2026-09-07 early), still not understood.
  Distinct from the "rammed into stop" item below (removed) - this one is
  unambiguous and severe: `curSpeedHz` stuck at exactly 0 for 40+ seconds
  while `targetSpeedHz` correctly kept wanting motion, never self-recovered,
  and corruption persisted across subsequent config changes until a full
  reboot. **Reviewed 2026-09-08, kept open** (not dismissed as a one-off
  like the item below) - genuinely reproducible-in-principle danger zone,
  not an ambiguous/inconclusive trip, and it's now reachable through UI the
  user didn't have when this was first found: the PID Tuning section's Kd
  field (built this session) allows any value down to 0 with no guard.
  **Consolidated the "~20-reboot flurry" item into this one** rather than
  tracking it separately - it coincided with the same testing window
  (reset reason UNKNOWN for all but one) and was never confirmed as a
  distinct cause; investigating it independently of the freeze itself
  isn't likely to be productive.
  **Mitigated, not root-caused, 2026-09-08**: added a live (non-blocking)
  warning under the Kd field (`web/index.html`/`script.js`,
  `checkPidKdWarning()`) that appears whenever Kd drops below 0.15,
  checked on every keystroke and once on page load so an already-saved
  dangerous value doesn't sit silently unwarned-about. Deliberately not a
  hard block - bench work chasing this bug down for real will need to go
  back into this range deliberately at some point. Actually reproducing
  the freeze again to root-cause it is real, separate work involving
  deliberately re-triggering an unrecovered hang on real hardware - not
  attempted this pass.
- [x] **StallGuard / jam-detection - decided, 2026-09-08.** Stays as a
  user-selectable option (`tmcStallEnabledConfig`, Settings -> TMC2209
  Config -> "Enable Stall Detection Safety Cutoff"), **default off** - no
  code change needed, that was already both the compiled default
  (`tmc_handler.cpp`) and the NVS-load fallback (`main.cpp`), and the bench
  device's own saved value already matched (`tmcStallEnabled=0` via
  `GET /config`). Left disabled by default because it's still
  false-positive-prone at real bench-observed `SG_RESULT` values
  (`min_sg_result` bottomed at 2-4 even in the improved config) and
  `updateRammedIntoStopCheck()` already covers the jam-at-the-homing-switch
  case independently. A mid-travel jam away from the switch (what
  StallGuard originally caught, 2026-08-30) is not covered by anything
  while StallGuard is off - accepted as a known gap rather than something
  needing a replacement mechanism right now ("hardware can't hurt itself,
  it just sounds terrible" - the user, an earlier session). Anyone who
  wants the cutoff can still enable and tune it per-device via Settings;
  it's just not the shipped default.
- [x] **Dropped, 2026-09-08 - decided not worth chasing.** Was: figure out
  what actually happened during the Kd=0 sweep's "rammed into stop" trip.
  Confirmed a genuine one-off before dropping it, not just declared one:
  happened exactly once, during a single sweep run, at `Kd=0` (pure P) -
  a candidate gain that was never adopted (the compiled default landed on
  `Kd=0.3`, later re-swept and kept there against `Kp=3`) and every
  subsequent verification sweep at the gains actually shipped (`Kd=0.3`
  and others) ran clean across dozens of runs and 10+ direction reversals
  with zero repeats. No buzz/grinding was heard at the time either
  (argues against a genuine mechanical ram), and `updateRammedIntoStopCheck()`
  hasn't false-tripped since in any of this project's much more extensive
  subsequent testing. Left as an unexplained single data point rather than
  a standing investigation - the signal-to-noise on chasing a single,
  never-repeated event at an abandoned gain setting isn't worth the bench
  time.
- [x] **Re-validated and re-tuned, 2026-09-07.** Made independently
  configurable (`pidDFilterWeightConfig`) and swept 0.15 (as it was left) /
  0.04 (effective time constant restored to the original ~123ms) / 1.0
  (off) against the standard p8 wave. 0.04 won on every metric
  simultaneously (rms_error, jerk, corner tightness, ripple) - not a
  tradeoff - confirming the filter still earns its place and that
  restoring its original smoothing window helps further. New compiled
  default: 0.04 (was, unknowingly, 0.15 at the wrong tick rate).

- [x] **Debug tab could crash the board mid-homing - found live by the user
  2026-09-08, fixed same day.** Opening the new Debug tab (built earlier
  this same session) while the trolley was actively moving crashed the
  ESP32. Root cause: `GET /persist-log` (`readPersistLog()`,
  `persist_log.cpp`) did a live `SPIFFS.open(..., FILE_READ)` on every
  request, and the Debug tab's auto-refresh hits that endpoint immediately
  on open, and every 3s after - the exact same flash-cache-disable hazard
  CLAUDE.md already documents for the homing-switch ISR (FastAccelStepper's
  step-generation ISR isn't IRAM-safe; any real flash access while it's
  live risks a crash), just never previously hit because nothing polled
  this endpoint automatically before the Debug tab existed. Fixed by
  giving `persist_log.cpp` a RAM mirror of the flash file's content
  (`flushedMirror`), loaded once at boot and kept in sync only from
  already-safe contexts (`initPersistLog()`, `flushPersistLogNow()`) -
  `readPersistLog()` now reads the mirror only and never touches SPIFFS
  live. `clearPersistLog()` got the same treatment: clears RAM state
  instantly, defers the actual `SPIFFS.remove()` to the next safe
  (stepper-idle) flush via a new `pendingClear` flag.
  **While fixing this, found and closed a second instance of the same bug
  class introduced by the same Debug tab work**: the new
  `GET /protocol-debug` toggle (Serial Debug checkbox) called
  `preferences.putBool()` synchronously on every click, with no idle
  gating - same hazard, different flash API. Given the same deferred-write
  treatment as `stepperSettingsPendingSave` (`stepper_handler.h`): new
  `protocolDebugPendingSave` flag/`persistProtocolDebugIfPending()`
  (`protocol_common.h`/`.cpp`), applied in RAM immediately, actually
  written to flash only once the stepper is confirmed idle. Also routed
  the pre-existing `/config POST protocolDebug=...` write path
  (`config_handler.cpp`) through the same deferred flag while in there -
  it had the identical unguarded-write hazard already, just less likely to
  get hit by accident than a tab full of auto-refreshing controls.
  **Left alone, deliberately**: the broader "audit every other
  `Preferences.putX()` call site for the same unguarded-during-stepping
  hazard" item is still open (see the historical Bring-up findings section
  further down) - this entry closes the two paths this session's own new
  Debug tab work actually introduced/exposed, not a general sweep of
  every existing Settings-save handler.

### Wave 2 - cheap, mechanical, no design risk

- [x] **Audited, 2026-09-07 - no other instances in a real hot path.**
  Every remaining `String + String`/`+=` chain in `src/` is boot-time
  (`initDDP()`, `WiFi` AP-name setup), page-load-time (`handleRoot()`'s
  full settings-page render), or explicit human/script-triggered
  (`POST /config`, `$GET ALL`, OTA error messages) - all far below the
  20-50ms hot-path rate that actually caused the crash. `stepper_handler.cpp`
  and `led_handler.cpp` (the two files that run every `loop()` iteration
  during real motion/LED updates) have zero matches at all.
- [ ] **Consolidate the two remaining near-identical throttled-retry
  implementations** - homing's `retryMoveIfDied()` (`stepper_handler.cpp`,
  `static`/file-local) and PID's hand-rolled equivalent
  (`lastPidRunRetryMs`/`lastPidHystRetryMs` in `main.cpp`). (A third,
  Streaming's own less-hardened version, was removed along with the rest of
  Streaming mode, 2026-09-07 - see the Coalesce/Streaming/Lookahead removal
  entry below.) Real duplication, and worth re-examining together given how
  much was learned this project about `isRunning()`/
  `isRampGeneratorActive()` both being unreliable trust signals.
  **Reclassified out of "cheap and mechanical," 2026-09-07**: on closer
  look this touches battle-tested, delicately-fixed code in two different
  call patterns (homing's search-continuation, PID's direction-aware
  continuous-run retry) - the TODO history below is full of "found the hard
  way" bugs in exactly this logic. Real risk of a silent regression, not a
  quick refactor - treat as Wave 3/design-level work, not Wave 2.
- [ ] `pidReengageThreshold`/`RAMMED_STEP_TOLERANCE` (both 150) were chosen
  by feel (matched to each other, and to the DDP-quantization math) rather
  than an independent sweep. Partial progress, 2026-09-07: `pidReengageThreshold=150`
  has since been exercised across ~20+ real bench runs this project (every
  reversal repeatedly re-entering and leaving the hysteresis band) with
  zero false re-engagements and zero stuck-in-band cases - real evidence it
  works, though not a sweep against alternatives, so "is 150 actually
  optimal" stays open. `RAMMED_STEP_TOLERANCE` is a hardcoded
  `static const` in `stepper_handler.cpp` (not currently exposed as a
  tunable at all), so it hasn't been touched this pass - would need new
  plumbing to sweep, not just an existing knob. Low priority: no false
  StallGuard/ram-detection trips observed at the current value across the
  same extensive testing.
- [x] **Fixed, 2026-09-07.** Added the same throttled `genuinelyRunning`
  retry the hysteresis band already had, for consistency (the practical
  risk was always low here - a dead move just leaves the trolley exactly
  where it already was, still within deadband - but the asymmetry is now
  closed). Bench-verified no regression (p8: rms_error 381.2, jerk 196.8,
  zero stalls, matching the derivative-filter baseline within noise).
- [ ] The malformed-leading-log-row artifact (seen at `pidTickMs` below
  20ms, three different corrupted shapes, never at the original 20ms
  across ~25+ runs) is real and tick-rate-correlated but not root-caused.
  Confirmed harmless to every metric `ddp_continuous_test.py` computes,
  worked around in both it and `analyze_ripple.py`. Worth understanding if
  tick rate is ever pushed further - possibly a race in the Compact Motion
  Log ring buffer's read vs. write path becoming likelier at a higher
  write rate. **Already tried once, 2026-09-07**: a dedicated serial-vs-HTTP
  capture comparison was attempted specifically to root-cause this, but
  ran into unrelated serial-connection reliability problems on this board
  (DTR needed explicit assertion, then a USB-CDC buffering quirk truncated
  the capture) before it could isolate the actual artifact. Real
  investigation effort already spent without an answer - not a quick
  re-attempt.
- [ ] `$CHECKSTEPS` only catches step loss in the *overshoot* direction
  (switch fires early) - can't distinguish "no drift" from "drift the
  other way" (undershoot, never reaching the switch). Worth a second check
  toward the far end if undershoot-direction loss turns out to matter.
  **Reclassified, 2026-09-07**: this is a new capability (a second check
  mode), not cleanup - needs a design decision first (run both checks
  always, or only when configured; what target for the far-end check).
  Wave 3-shaped, not Wave 2.

### Wave 3 - real decisions needed before more code (closed out 2026-09-08)

- [x] **Decided and done, 2026-09-07 - PID is now the real production
  default.** User confirmed via explicit choice. Persisted via
  `POST /config` (`stepTrackMode=4`, the existing NVS key), verified
  surviving a genuine reboot (not just a RAM check) - `GET /tunable?name=trackMode`
  read back `4` immediately after a real power-cycle-equivalent reboot.
- [x] **Investigated via code review, 2026-09-07 - two of three suspected
  asymmetries ruled out, one remains genuinely open.** Traced both search
  legs in `updateHoming()` (`stepper_handler.cpp`): (1) **configured
  profile** - `startHoming()` sets `stepperAccelHomingConfig`/
  `stepperSpeedHomingConfig` exactly once; neither `HOMING_FIND_INITIAL`
  nor `HOMING_FIND_OTHER_END` calls `setSpeedInHz()`/`setAcceleration()`
  again before their search - both legs run at the identical commanded
  values. (2) **starting velocity state** - leg 1 starts from confirmed
  rest (nothing has run since boot); leg 2 passes through `HOMING_SETTLE`,
  which explicitly waits for `!stepper->isRunning()` (a genuine complete
  stop, built 2026-08-30 for an unrelated pause bug) before reversing - so
  leg 2 *also* accelerates from true rest, not mid-cruise. Both suspected
  code-level asymmetries are ruled out. (3) **gravity assisting one
  direction, resisting the other** - NOT resolved by this review; even
  with identical commanded parameters and starting conditions, the
  *achieved* motion profile can still differ physically. Would need a real
  bench comparison (encoder-timed ramp profile, each direction) to close
  out fully - not attempted.
- [x] **Tested at p16, 2026-09-07 - real trade confirmed, default kept at
  200ms.** Swept 200/300/400ms: jerk improves substantially at a slower
  period with a longer window (131 -> 88 -> 84) but corner tightness gets
  markedly worse (231 -> 312 -> 387, a 68% increase at 400ms) and frame
  lag starts creeping toward the hard cap (max 6.8 -> 25.0 -> 48.0). Given
  the user's own explicit priority (corner tracking matters), not worth
  trading away for the jerk gain - 200ms is already a good compromise at
  both extremes tested (p8 and p16). A genuinely optimal answer would need
  a per-period tunable, which doesn't exist and isn't worth building for
  this. Not changing the default.
- [x] `control16Bit` is persisted to NVS on the bench device (real
  accuracy win) - standing operational caveat, not a task, closing it out
  as such rather than a to-do: the DDP source must match, or position will
  be wrong after a reboot. Revert with `POST /config` body
  `control16Bit=0` if the source isn't switching too.
- [x] **Closing out Wave 3 with this one honestly unresolved, 2026-09-08 -
  not blocking, watching for recurrence rather than actively chasing
  further.** The severe recurrence (below) got a full, confirmed root
  cause (a real bug, fixed), but that doesn't explain *this* original,
  milder observation - it happened before any of this session's WiFi
  config changes existed, so it can't be the same mechanism. Genuinely
  still unexplained. Hasn't recurred in this original mild form since;
  revisit only if it does.
  **Original note, 2026-09-07 - intermittent WiFi/HTTP unresponsiveness
  observed, independent of any flash cycle.** During Wave 3 bench work
  (no upload had just happened), the device went unreachable to both HTTP
  and ICMP ping for roughly 20-30s, then recovered on its own - `uptimeMins`
  confirmed no reboot occurred, ruling out a crash. Consistent with the
  documented 30s WiFi reconnect-monitoring cadence, but not confirmed as
  the cause. Recurred a second time moments later (a few failed attempts,
  then recovered). Not investigated further tonight - worth watching for
  recurrence, and worth checking WiFi signal strength/interference on the
  bench if it keeps happening.
  - **What looked like a recurrence, same session, later that night, was
    actually a real bug Claude introduced - not this item, and not an
    external network issue.** While bench-testing the new AP-fallback
    retry feature, the real network (`ssid="se"`) appeared completely
    unconnectable for 8+ minutes straight, surviving a fresh from-boot
    attempt and multiple retries alike, while the real router kept
    answering pings the whole time - at the time this was written up
    (wrongly) as a probable router-side flap-protection/interference
    theory. **Root-caused the next day, 2026-09-08, when the user reported
    the device "still won't connect, it should be":** a `/save-wifi` test
    POST run *earlier that same session* (verifying the new
    `wifiRetryInterval` field's round-trip) included `password=` (empty) -
    `handleSaveWifi()` unconditionally overwrote and persisted whatever was
    submitted, silently wiping the real saved WiFi password to blank. Every
    "unconnectable" observation afterward was just correct behavior against
    now-broken credentials, not flakiness of any kind - a bad password
    fails consistently regardless of retry strategy, AP+STA mode, or how
    many times you retry, which is exactly what was observed and
    misread as evidence of an external cause. Real fix (see below) plus a
    lesson worth keeping: check recent own write actions against
    persisted state before reaching for an external/environmental
    explanation for a sudden, total, and perfectly consistent failure.
  - [x] **Fixed, 2026-09-08**: `handleSaveWifi()` (`html_handler.cpp`) now
    only overwrites the stored password if a non-empty value was actually
    submitted - standard "leave blank to keep unchanged" convention for a
    password field. This wasn't just a fix for the test mistake above: the
    web UI's own password field is deliberately never pre-filled with the
    real value (so it's never echoed into page source), which means
    *every* real Settings-page WiFi save that didn't involve retyping the
    password - changing just the hostname, or static IP, or the new retry
    interval - had this exact same silent-wipe bug for any real user, not
    just this session's testing. The generic `/config` API's own password
    setter (`config_handler.cpp`) was deliberately left as plain
    set-whatever-is-given - it's an explicit, opt-in, single-key API (a
    caller must specifically write `password=<value>` to touch it at all),
    with none of the browser form's "every field gets submitted whether or
    not you meant to change it" accidental-inclusion risk.
- [x] **Tested, 2026-09-07 - jumpStart does not appear necessary at
  `homeAccel=20,000`.** Disabled it (`jumpStart=0`) and ran 3 back-to-back
  homing cycles: all clean, no stalls, no errors, identical ~11s timing to
  with-jumpStart, `bottomPosition` consistent (15502-15504). Makes sense
  given the history - `homeAccel` was already lowered from a much higher
  original value (1,000,000, "functionally instant") as part of the same
  original stall fix; jumpStart's own individual contribution may only
  have mattered at that old, steeper ramp. Re-enabled (`jumpStart=20`,
  safe default, no reason to leave off given the small sample size here -
  3 trials, not an exhaustive stress test).
- [x] **Investigated, 2026-09-07 - question's practical relevance has
  shifted now that PID is the production default.** Checked: PID's own
  continuous-run engagement (`main.cpp`, the `runForward()`/`runBackward()`
  call site) deliberately calls `setJumpStart(0)` before every engagement,
  by design - the documented 2026-09-06 fix for `setJumpStart()`'s burst
  firing in the wrong direction through the continuous-run API. So the
  mode actually running in production does *not* apply jumpStart to its
  own "first move after rest" path at all, on purpose - this was never an
  oversight to test, it's an intentional tradeoff already made. The
  original question (does jumpStart help a cold-start `moveTo()`) remains
  genuinely relevant only for Direct/Coalesce/Lookahead, which are no
  longer the default mode - lower priority than when this was written, not
  tested tonight.

### Wave 4 - web UI modernization (its own epic; merges ~15 scattered items)

- [x] Remove the **Compact Motion Log** checkbox from the UI (it was on the
  Status tab, not Settings - the TODO's file location was slightly stale)
  (`web/index.html`) - bench/tuning-only, shouldn't be user-facing. The
  `/compact-log` HTTP endpoint itself is untouched - `tools/*.py` control it
  directly and don't go through this checkbox.
- [x] **Revisited and resolved, 2026-09-07 (second pass).** The previous
  entry flagged real fleet-default risk in blindly deleting Jump Start and
  Small-Move Tracking; the user then made the actual calls directly:
  - [x] **Jump Start removed as a Settings/NVS field, hardcoded instead**
    (`JUMP_START_STEPS` in `stepper_handler.h`, still 20, the value already
    running). Nothing in this project's testing (the Wave 3 homing test, or
    ever) showed it changing behavior - not worth a per-device setting for
    a value nobody has shown matters. `jumpStartConfig` removed everywhere:
    NVS load/save, `/save-stepper`, `/config` GET/POST, `$SET`/`GET /tunable`.
  - [x] **Small-Move Tracking kept** (still real, still Direct-mode-default,
    per the previous entry's reasoning) but decluttered: its Threshold/Max
    Lag/Speed/Acceleration fields now live behind a "Small-Move Tracking
    Settings" collapsible under the enable checkbox, instead of four
    always-visible fields. Its description trimmed to one sentence plus an
    explicit "Direct mode only" note now that PID is a real alternative.
  - [x] **Tracking Motion Strategy dropdown simplified to Direct/PID only**
    ("we just have Direct mode and PID," the user's words) - Coalesce/
    Streaming/Lookahead options and their fields-groups/paragraphs removed
    from `web/index.html`. At this point (first pass, same day) the backend
    was left untouched - `TRACK_MODE_COALESCE/STREAMING/LOOKAHEAD` still
    fully implemented, still reachable via `$SET`/`GET /tunable
    trackMode=1/2/3`. **Superseded a few messages later the same session**:
    asked directly "is Coalesce/Streaming/Lookahead bench tested?", which
    surfaced that Lookahead never was (only Coalesce and Streaming had real
    bench evidence, one good, one bad) - given that, and now that Direct/PID
    cover the real use cases, the user asked to remove all three **from the
    code entirely**, not just the UI. Done: `updateCoalesceMode()`,
    `updateStreamingMode()`, `handleLookaheadModeCommand()`,
    `updateLookaheadMode()`, all their state variables, NVS keys
    (`stepCoalesceMs/St`, `stepStreamRateW/Settl`, `stepLookaheadSt/Ms`),
    `/config` GET/POST entries, and `$SET`/`GET /tunable` names all removed.
    The `StepperTrackMode` enum now only has `TRACK_MODE_DIRECT=0` and
    `TRACK_MODE_PID=4` (gap between them left alone on purpose - no
    renumbering, so an already-persisted `trackMode=4` on the bench device
    keeps meaning PID). A device with an old saved `trackMode=1/2/3` now
    gets an explicit boot-time fallback to Direct with a logged warning,
    instead of silently falling through the dispatch switch's default case
    with no explanation. Verified: full rebuild clean (no errors/warnings),
    see the bench re-verification note below for the on-hardware test.
    PID (`trackMode=4`) added to the dropdown for the first time
    (`{{TRACK_MODE_PID_SEL}}`) as part of the same pass - it was previously
    missing from the UI entirely despite being the production default since
    earlier in Wave 3.
  - [x] **PID tuning parameters section, built 2026-09-08.** All 12 `pid*`
    tunables (Kp, Kd, D-filter weight, tick ms, log ms, max speed, accel,
    deadband, reengage threshold, feedforward on/off, feedforward window
    ms, lookahead ms) now have a real Settings UI - a "PID Tuning"
    collapsible under Stepper Configuration - and NVS persistence via the
    existing deferred-write mechanism (`stepperSettingsPendingSave`), a new
    `/save-pid` endpoint, and `/config` GET/POST entries, all following the
    exact same pattern as every other stepper setting. All 12 new NVS keys
    verified ≤15 chars. Bench-verified round-trip through both `/save-pid`
    and generic `/config`, and confirmed surviving a real reboot via
    `$GET ALL`.
- [x] **TMC2209 UART enable, sense resistor, driver address, and SpreadCycle
  hysteresis (hstrt/hend) all hardcoded, 2026-09-07 (second pass) - "we
  never used them" / "this project has only ever run one board." No
  Settings fields, no NVS keys, no `/config` GET/POST entries, no
  `linkSettingsChanged`-triggered UART reinit in `/save-tmc` (no longer
  reachable - those values can't change at runtime anymore).
  `tmcEnabledConfig`/`tmcRSenseConfig`/`tmcAddressConfig`/`tmcHstrtConfig`/
  `tmcHendConfig` are `const` in `tmc_handler.cpp` now (values unchanged:
  true/0.11/0/0/0). Any of the five keys already saved in an existing
  device's NVS is now just an orphaned, unread value - harmless, never
  read again.
- [x] Remove the redundant **Stepper Control** section from Settings - it
  duplicated the Status page's own "Manual Stepper Control" collapsible
  (same `setPosition`/`moveSteps`/`homeServo` actions, just a second set of
  form fields). `homeServo()` in `script.js` is shared by both tabs and was
  kept; the Settings-tab-only `setPosition()`/`moveSteps()` wrapper
  functions were removed since nothing calls them anymore (the `/set-position`
  and `/move` HTTP endpoints themselves are untouched - tooling hits them
  directly).
- [x] Fold the **TMC2209 driver settings** panel into Stepper Configuration
  under a single "TMC2209 Config" collapsible (no nested "Advanced" level -
  flattened those fields into the main body). First pass (same day) made
  UART enable default-on but still a checkbox; second pass (see the
  hardcoding entry above) removed the checkbox entirely - superseded, not a
  separate outcome. Did **not** remove the StallGuard settings from the
  panel - this item was explicitly conditional on Wave 1's StallGuard
  decision coming back "remove," and it didn't: decided 2026-09-08 to keep
  it as a user-selectable option, default off (see Wave 1). Settings stay
  in the UI, unchanged.
- [x] **`#define`d off the encoder status/count display, 2026-09-08**
  (`SHOW_ENCODER_STATUS 0` in `encoder_handler.h`, gates the Status page's
  `#encoder-count-row` to `display:none`). Discussed with the user first,
  per the earlier note - PID tuning judged far enough along now. This only
  gates the Status-page row; the encoder subsystem itself
  (`encoder_handler.h`/`.cpp`, `updateEncoder()`) is completely untouched
  and still counts - the standing note that tuning needs it as ground
  truth still holds, it's just no longer surfaced in the normal UI. Flip
  the `#define` back to `1` if bench access to it is needed again.
- [x] **New "Debug" tab, built 2026-09-08 - first version.** Reconciled the
  two earlier half-specified "Live Log" / "Debug tab" proposals into one
  feature, per the note below. Turned out to be almost entirely a UI-only
  task: the backend pieces the design called for (a polling endpoint
  matching `/status-data`'s pattern; an in-RAM ring buffer) already
  existed from earlier bench-tooling work and just needed a browser front
  end - `GET /compact-log` (96KB RAM ring buffer, CSV position/speed/
  tracking data) and `GET /persist-log` (SPIFFS-backed, reboot-surviving
  breadcrumbs) were both already real HTTP endpoints, added for
  `tools/*.py` bench scripts to hit directly without opening serial (which
  resets this board - see the WiFi mode selector item above). New
  "Debug" tab: a Compact Motion Log viewer (Enable/Refresh Now/Clear,
  auto-refreshes every 3s only while the tab is actually open - these
  buffers can be tens of KB and there's no reason to poll them on
  `/status-data`'s 1s cadence) and a Persistent Diagnostic Log viewer
  (Refresh Now/Clear, same auto-refresh). Both boxes are plain
  `fetch().then(response => response.text())` into a scrollable
  `<pre>`, not JSON - matches what the endpoints already return.
  **Serial Debug checkbox moved here too**, out of Channel Configuration
  as planned - but rather than folding it into the existing `/save-protocol`
  form (which would've meant either restructuring that form across two
  tabs or risking silently clearing it on every unrelated Channel Settings
  save, since `handleSaveProtocol()` reads it via `hasArg()`), gave it its
  own instant-apply `GET /protocol-debug?enable=true|false` toggle -
  same pattern already used for `/compact-log`'s and `/led-test`'s enable
  toggles, applies immediately with no save/reboot round-trip.
  `handleSaveProtocol()` no longer touches `protocolDebugConfig` at all.
  **The redundant "Stepper Control" duplicate mention** referenced in the
  original item text was already resolved in an earlier Wave 4 pass (see
  the "Remove the redundant Stepper Control section from Settings" item
  above) - nothing left to do here.
  **Deliberately deferred to a later pass, not part of this first
  version**: `GET /ddp-rx-log` (a third existing RAM log, bench/tooling-
  only and off by default via `$SET ddpRxLog`) isn't surfaced in this tab
  - `tools/*.py` already hits it directly and it's a narrower, more
  specialist tool than the other two; a true live mirror of `Serial.print`/
  `println()` output (the original, more ambitious "Debug tab" proposal)
  was not attempted - the three existing RAM/flash logs already cover
  what actually gets used in practice, and wrapping every `Serial.print`
  call project-wide is real, separate, higher-risk work not worth doing
  speculatively.
  Bench-verified over HTTP end to end: `/protocol-debug?enable=true|false`
  toggles and persists correctly; `/compact-log?enable=` /
  fetch / `?clear=1` all round-trip; `/persist-log` fetch returns real
  reboot-surviving content (confirmed actual homing-search breadcrumbs
  from a prior boot); the served page (~84KB, no truncation - see the
  heap-fragmentation bug fixed earlier this wave) contains exactly one
  `id="protocolDebug"` checkbox, now inside the Debug tab, with the
  template placeholder correctly substituted rather than left literal.
- [x] **WiFi settings**: add an explicit mode selector (Client only / AP
  fallback / AP only) instead of today's implicit behavior (traced in
  `wifi_handler.cpp`/`main.cpp`: today it always tries client first, then
  falls back to AP on failure/no-saved-SSID - there's no way to force
  "AP only" or "client, no fallback"). **Built and bench-verified,
  2026-09-08.** New `wifiModeConfig` (`WIFI_MODE_AP_FALLBACK=0` (default,
  matches the previous implicit behavior byte-for-byte),
  `WIFI_MODE_CLIENT_ONLY=1`, `WIFI_MODE_AP_ONLY=2`), NVS key
  `wifiModeCfg` - deliberately *not* `wifiMode`, which collides with
  `/status-data`'s existing JSON field of that name (caught before it
  shipped). `setup()`'s boot logic and `checkWifiConnection()`'s AP-retry
  branch both gate on it (AP_ONLY skips ever trying a client connection at
  all, at boot or on retry; CLIENT_ONLY skips ever starting a fallback AP,
  logging instead). New Settings-page dropdown in WiFi Client Settings,
  with a warning paragraph on Client Only about there being no recovery AP
  if it can't connect (mentions the serial `'a'` command as the way back).
  **AP_FALLBACK and AP_ONLY confirmed working live on the bench**
  (multiple flash cycles, real client connections, real disconnect/retry
  cycles). **CLIENT_ONLY was deliberately not live-tested** - by this
  point in the session real time had already gone into the WiFi
  debugging saga below, and CLIENT_ONLY's code path is structurally
  identical to the other two modes (same `connectToWifi()`/boot-sequence
  machinery, just skipping the `startAccessPoint()` call), so it's high
  confidence by code review and analogy rather than direct observation.
  Worth a real bench pass before relying on it for a device that might
  need physical/serial recovery. This whole feature's implementation and
  bench pass ran straight into a confusing, unrelated testing-methodology
  artifact: closing (not just opening) a serial connection also resets
  this board, so a test script that opens serial, sends a command, and
  closes again silently undoes whatever that command just set before it
  could be verified over HTTP. Cost real debugging time before being
  correctly diagnosed as not a firmware bug (now saved as its own memory
  note for future sessions) - worth remembering why some of the
  live-testing narrative above took far longer than the actual bug fixes
  did.
  - [x] **Adjacent, smaller piece done separately, 2026-09-07**: the
    existing (already-implemented) "was connected as client, then dropped,
    retry" monitor (`checkWifiConnection()`) had its 30-second poll interval
    hardcoded - made configurable and persisted (`wifiRetryIntervalConfig`,
    NVS key `wifiRetryInt`, default 20s, 0=disabled), with a Settings-page
    field. Left the existing 3-consecutive-failed-checks debounce before
    actually reconnecting untouched - didn't want to also make reconnects
    more trigger-happy in the same change as shortening the poll interval,
    given this exact function is already suspected (not confirmed) in the
    unexplained WiFi/HTTP unresponsiveness noted earlier in Wave 3. This is
    a real, separate improvement from the mode-selector item above, not a
    substitute for it - it only ever helps a device that's already
    successfully connected as a client and then drops; it does nothing for
    a device stuck in AP fallback after a failed boot-time connect (which
    is exactly what the mode-selector/explicit-retry-while-AP behavior
    above would address).
  - [x] **The retry-while-AP piece itself, done right after** - the user
    confirmed this was in fact what they wanted (not just the
    already-connected case above). `connectToWifi()` gained a `preserveAp`
    parameter (default `false`, every existing call site unaffected):
    `true` uses `WIFI_AP_STA` instead of plain `WIFI_STA`, and explicitly
    re-asserts `WiFi.softAP()` (observed unreliable across a bare mode
    change on this ESP32 Arduino core version) so the fallback AP survives
    the whole attempt. `checkWifiConnection()`'s `WiFi.getMode() ==
    WIFI_AP` case, previously an unconditional skip, now calls
    `connectToWifi(true)` on the same `wifiRetryIntervalConfig` cadence,
    deliberately with **no** debounce (unlike the already-connected
    branch) - there's no "was working" state to protect against a false
    trip while genuinely AP-only, every check is a real "still not on the
    real network" reading. On failure, explicitly drops back to plain
    `WIFI_AP` (not left in a half-connected `AP_STA` limbo); on success,
    explicitly tears the AP back down (`softAPdisconnect`, `WiFi.mode(WIFI_STA)`)
    rather than staying dual-mode forever, matching what a normal
    successful boot connect looks like.
    **Bench-verified the failure/stability path extensively, not the
    success path** - a real, currently-unexplained WiFi outage (see the
    intermittent-unresponsiveness item above, recurred much more severely
    this same session) meant the real network was never reachable during
    testing, so the "retry succeeds, AP drops cleanly" branch is
    code-reviewed but not yet observed live. What *was* directly confirmed
    via serial across multiple long, uninterrupted capture windows: clean,
    correctly-timed retry cycles (~20s apart, each properly timing out
    around 10s) for 8+ minutes straight, zero crashes, zero watchdog
    resets, free heap staying healthy, and correct fallback to plain AP on
    every failure - never stranded in a broken dual-mode state. Worth a
    follow-up bench check once the real network is reliably reachable
    again, to watch the success/AP-teardown path fire for real.
  - [x] **Follow-up debugging session, 2026-09-08 - three more real bugs
    found and fixed, all through live user-in-the-loop troubleshooting
    (the user directly on the AP with a phone, reporting symptoms in real
    time):**
    1. **Root cause of "the real network suddenly became unreachable for
       8+ minutes" above, finally found**: not external at all. An earlier
       `/save-wifi` test POST (verifying the new retry-interval field)
       included `password=` (empty) - `handleSaveWifi()` unconditionally
       overwrote and persisted whatever was submitted, silently wiping the
       real saved password. Real, separate bug beyond the test mistake:
       the web UI's own password field is deliberately never pre-filled
       (so it's never echoed into page source), meaning *any* real
       Settings-page WiFi save that didn't involve retyping the password
       had this exact silent-wipe bug for a real user too. Fixed: only
       overwrite if a non-empty value was actually submitted.
    2. **"Page loads blank over the AP, background color right, body
       empty"** - genuinely the hardest bug this project has hit. Ruled
       out, in order: the retry's own radio disruption (confirmed skipped
       via its log line while it still happened), heap exhaustion (103KB
       free, comfortably above the ~74KB page), a captive-portal
       mini-browser (reproduced in a real browser app too). Root cause
       via targeted checkpoint logging bisecting `handleRoot()`'s ~68
       sequential `page.replace()` calls: the first *growing* replacement
       (`{{WIFI_MODE}}` -> `"Access Point Mode"`) needs Arduino's
       `String::replace()` to `realloc()` the ~70KB buffer, which needs
       the old and a new, larger buffer alive simultaneously if the
       allocator can't extend in place - reliably too much for the
       contiguous free heap available once the SoftAP + captive-portal
       DNS + (during a retry) a concurrent `WiFi.begin()` are all also
       holding memory. A failed reallocation there silently truncates the
       String rather than erroring. First fix attempt (`page.reserve()`
       called *after* the initial copy) didn't help - same problem, one
       line earlier, confirmed via the checkpoints regressing from 69902
       to failing at the very first checkpoint. Real fix: reserve capacity
       on the **empty** String first (a single clean `malloc()`, nothing
       old to keep alive), *then* copy `htmlPage`'s content in - every
       later `replace()`, including growing ones, reuses that one buffer
       and never reallocates again. User-confirmed fixed: full page loads
       over the AP now.
    3. **Watchdog crash right after a large send** (`task_wdt` abort,
       reboot) - hit once the page could finally be served at full size,
       apparently while a client was actively disconnecting from the AP
       mid-transfer. Likely cause: the underlying socket write can stall
       for multiple `HTTP_MAX_SEND_WAIT` (5s each) cycles waiting for ACKs
       from a client that's leaving, with nothing in that call path
       resetting the watchdog in between. Added `esp_task_wdt_reset()`
       immediately before `server.send()` in `handleRoot()` - guarantees a
       full fresh 10s budget going into the one call site now known to be
       slow. Doesn't bound how long `send()` itself can take, so not a
       complete guarantee, but a real, low-risk improvement over no reset
       at all there.
    4. **Not a bug, a real design correction from the user**: "Connect
       Now" (`handleConnect()`) had also been switched to
       `connectToWifi(true)` (`preserveAp`) as part of fixing the original
       stranding bug - but the user pointed out that's backwards for an
       *explicit* click: disrupting the clicker's own AP connection is the
       expected, intended outcome of "connect now," not something to
       protect them from (unlike the unattended automatic retry, which
       correctly *should* stay hands-off). There's a real hardware reason
       too: the AP and a new STA connection share one radio, and the chip
       generally won't shift the AP's channel while stations are actively
       associated (that would silently disconnect them) - so `preserveAp`
       could leave the connection attempt unable to complete at all while
       the very phone that clicked the button stays connected. Reverted to
       plain `connectToWifi()` (forces `WIFI_STA` immediately, drops the
       AP and any clients up front) but kept the real fix from the first
       pass: on failure, explicitly calls `startAccessPoint()` again
       rather than leaving the device stranded.
    All bench-verified on real hardware through the whole saga - multiple
    flash cycles, the user directly on the AP reporting each symptom live,
    each root cause confirmed via targeted logging rather than guessed at.
- [x] **Stepper Configuration**: remove the help-text paragraph above
  Homing Acceleration.
- [x] **Settings tab**: fold Channel Configuration and LED Configuration
  into one panel (now "Channel & LED Configuration" - two forms, one box,
  matching the pattern already used for WiFi/AP settings).
- [ ] Surface which tracking profile (normal vs. small-move) was used
  per-move somewhere in the UI - currently only visible via serial with
  Debug enabled (`[tracking]`/`[normal]`). Natural fit for the planned
  Debug tab above.
- [x] Changing Microsteps per Full Step requires a re-home afterward (the
  UI warns on save, but it's easy to miss) - the previously-tuned Stepper
  Speed (Hz) will feel like a different physical speed since distance per
  step changed. **Improved 2026-09-08**: the microsteps dropdown now
  fires a real `confirm()` dialog on actual change (`web/script.js`,
  captures the field's initial value on page load, only prompts if the
  new value differs), reverting the selection if the user cancels -
  harder to miss than the old passive save-time warning text alone
  (which is still shown too).
- [x] Web UI: hint/warning in the LED settings section about practical WiFi
  pixel-count limits (pixel count input still allows up to `MAX_LEDS`=1000,
  no code change needed - added the hint text next to the field).
- [x] Documented `stepperControlEnabled = false` (pixel-only prop) as a
  first-class supported use case in README.md's Channel Settings section -
  it already worked (channels 1-2 stay reserved/unused, LEDs still start at
  channel 3), just wasn't called out clearly before.

### Wave 5 - bigger, deliberately-deferred design work

- [ ] **Core 0 for PID** - explicitly declined for the 2026-09-07 tick-rate
  session (see that section below for the full reasoning and the concrete
  evidence for it: `pidTickMs=2` caused real DDP packet rejection, the
  first all session). Scope as real design work if ever picked up -
  resolve FastAccelStepper's cross-core call safety first, then design
  synchronization for `positionRequest` and PID's own internal state.
- [ ] **Dual-core LED offload** - the rest of the design (Push-flag-aware
  frame assembly, the LED semaphore/task, double-buffering if tearing
  turns out to matter) is still just design, not code. See the dedicated
  design section below for the full plan.
- [ ] Confirm whether FPP actually sets the Push flag on the final
  fragment of a multi-packet DDP frame (believed likely - standard
  practice - but not confirmed against real captured traffic).
  `DDPDebugger`'s Receiver tab (this repo) has a live flags-seen counter
  built for exactly this - point a duplicate FPP output at it during a
  real show.
- [ ] Frame assembly logic for multi-packet DDP frames - not yet
  implemented, pending the Push-flag confirmation above.



## PID tick rate found to be the real ripple lever - locked in at 5ms; a lookahead buffer tried and not adopted; Core 0 for PID explicitly declined for now (2026-09-07, late)

Continuing the "smooth out the actual speed" investigation after Kp=3 was locked in: the ~120-150ms-period speed ripple survived every filtering-based lever tried (Kd, Kp, pidAccel, pidFfWindowMs) because none of them touched its actual cause. Two new ideas were tried, at the user's suggestion.

### Lookahead / reference-smoothing buffer - built, works, not adopted

Added `pidLookaheadMsConfig` (0=off): a plain moving average of the target over the configured window, reusing the feedforward estimator's own ring buffer of real `(target, timestamp)` samples rather than new state. Only feeds the P-term (`pTermError`) - `error`/`target` stay raw for the deadband/hysteresis/settle/safety-net logic, which need the true current commanded value. Deliberately just an average of real received samples, not a synthetic forward integration - unlike the shelved `pidSmoothTarget` layer, there is no persistent velocity/position state that can drift while a `pidStopSettling` wait blocks the function.

Worked exactly as designed - jerk and ripple fell monotonically with window size (jerk 199->163->139->129, ripple 210->178->141->113Hz at 50/100/150ms) - but lined up against the earlier Kp sweep at matched ripple, it's the same trade curve, not a better one (Kp=2 alone: rms=513/jerk=160; lookahead=100 at Kp=3: rms=538/jerk=139). Makes sense in hindsight: both changes reduce the P-term's own output amplitude by roughly proportional means (attenuate the gain vs. attenuate the signal before the gain), so they cost accuracy similarly. `lookahead=100` also pushed frame lag to a 23.5-frame max at p8, over the 20-frame hard cap. Kept in the firmware (tunable, off by default) since it's a legitimate lever if ever wanted, but not part of the recommended config.

### PID tick rate - the real lever, locked in at 5ms

`updatePidMode()`'s rate-limit was a hardcoded 20, matching `TRACK_MODE_STREAMING`'s cadence since both were first built. Made configurable (`pidTickMsConfig`) and swept 20/10/5/2ms at Kp=3, p8, 16-bit, each verified via the full protocol (encoder ground truth, zero real stalls, DDP reception checked before/after):

| tick | rms_error | jerk | ripple_rms | ripple_period | DDP rejected |
|---|---|---|---|---|---|
| 20 (was default) | 386.5 | 199.1 | 210.3Hz | 140.7ms | 0 |
| 10 | 392-395 | 131-132 | 117-121Hz | 71-72ms | 0 |
| **5 (new default)** | **394.0** | **109.8** | **94.1Hz** | **21.9ms** | **0** |
| 2 (rejected) | 394.5 | 62.2 | 51.8Hz | 21.0ms | **6** |

Unlike every other lever tried this session, tick rate bought real smoothness with **no measured accuracy cost** - rms_error stayed flat across all four settings while jerk and ripple fell substantially. The ripple period shrinking faster than proportionally with the tick (141->71->22ms) is strong evidence this is a genuine sampled-control-loop dynamic (the loop's own sample delay interacting with the stepper's accel-limited response) rather than a physical resonance or noise source - which is exactly why no output filter tried earlier this session (derivative filtering, a correction slew limiter, the lookahead buffer above) ever moved it much: filtering the output doesn't change the loop's own sample delay.

**2ms was tested and rejected on real, not theoretical, grounds**: `protocolPacketsRejectedOutOfOrder` went 0->6 during that one run - the only nonzero reading across the entire session, at any tick rate tested. The malformed-log-row artifact (see below) also got measurably worse (a negative parsed timestamp vs. an implausibly-small one at 10/5ms). Both point at Core 1's `loop()` (DDP parsing, web server, WiFi housekeeping, TMC UART, now a 500Hz PID tick) genuinely running out of headroom - the control law was still improving right up to 2ms, so this is a CPU ceiling, not a diminishing-returns one.

**Applied**: `stepperPidTickMsConfig` compiled default 20->5ms. Verified live from a genuine fresh boot.

### Core 0 for PID - explicitly declined for now, not forgotten

The 2ms result is the concrete case for moving PID off Core 1: this project's own stated reason for the ESP32-S3 move is "explicit core allocation so nothing on Core 1 contends with anything else" (see CLAUDE.md), and the encoder already moved to Core 0 for exactly this kind of contention. PID hitting the same wall at 2ms is the same problem showing up again.

**Explicit decision: not attempted this session.** This is a real re-architecture, not a tunable - everything currently runs cooperatively single-threaded inside `loop()`, with no locking anywhere. Moving PID to Core 0 introduces genuine cross-core sharing of state that is currently unprotected: `positionRequest` (written by DDP parsing on Core 1, would be read by PID on Core 0), and PID's own internal state (`pidCurrentDirection`, `pidStopSettling`, `pidDirectionSwitchPending`, the feedforward ring buffer). Every hard-won bug fix from this and prior sessions (the direction-switch race, the overshoot-latch fix, the stale-tick watchdog flagged above) was found and fixed under a single-threaded mental model - cross-core reintroduces a bug class that model has no answer for.

There is also a real, unanswered question that has to be resolved BEFORE writing any of this, not discovered after: FastAccelStepper's own engine is explicitly pinned to Core 1 (`engine.init(1)`). Whether calling `stepper->moveTo()`/`runForward()`/`getCurrentPosition()` from a Core 0 task is safe against that library's internals, or needs a queue/mutex boundary instead of direct calls, is not currently known.

- [ ] If tick rates below ~5ms are wanted later, moving PID (or the whole stepper track-mode dispatch) to Core 0 is the way to get there - but scope it as real design work, not a quick follow-up: resolve FastAccelStepper's cross-core call safety first, then design synchronization for the shared state listed above.
- [ ] The malformed-leading-log-row artifact (seen at 10ms/5ms/2ms tick, never at the original 20ms across ~25 runs) is real and tick-rate-correlated but not root-caused. Confirmed harmless to every metric `ddp_continuous_test.py`'s `analyze()` computes (one row, dropped), and worked around in `analyze_ripple.py`. Worth understanding if tick rate is ever revisited - possibly a race in the Compact Motion Log's ring buffer read (`getCompactLog()`) vs. write (`appendCompactLog()`) path becoming likelier at a higher write rate, but not investigated.
- [ ] `pidFilteredRate`'s 0.85/0.15 derivative-filter EMA weight is per-SAMPLE, not per unit time - at tick=5ms (4x the original rate) its effective time constant is roughly 4x shorter than when it was tuned, weakening the filter's intended smoothing. Not yet re-tuned or re-validated against the new tick rate; flagged in stepperPidTickMsConfig's own declaration comment but not addressed.
- [ ] `pidLookaheadMsConfig` exists as a tested, working, off-by-default tunable if the lookahead approach is ever wanted for a different reason (e.g. a show with more headroom in its frame-lag budget than the bench tests here).

## Needed: a stale-tick watchdog in updatePidMode() for continuous-run modes (found 2026-09-07, not yet fixed)

Found while re-diagnosing why the trolley kept ending up past the bottom of travel and up the back of the pulley. Root-caused via the compact log itself: a ~1.0s gap in logged rows, with position and encoder agreeing the trolley travelled +3,660 steps during that gap alone (confirmed independently by the encoder, so this is real physical motion, not a counter artifact).

**The mechanism**: in `TRACK_MODE_PID`'s continuous-run state, `runForward()`/`runBackward()` mean "keep going indefinitely" - the stepper does not stop on its own. `updatePidMode()` is what re-evaluates the position, checks bounds, and decides whether to keep going, reverse, or stop. If `loop()` doesn't get back around to call it for a while (serving a large HTTP response is the case that was actually hit - the compact log is up to 96KB - but WiFi reconnect logic and SPIFFS writes are also blocking and run from `loop()`), the stepper just keeps cruising with nothing supervising it. Neither containment guard added earlier in this same session helps here, because **both guards only run when `updatePidMode()` runs** - they cannot fire during exactly the gap where they're needed.

This was hit repeatedly by the test harness's own mid-run log draining (since fixed - see the harness protocol rework below/elsewhere in this file), but HTTP is not the only thing that can stall `loop()`, and the user's own read is that the web server should be off the critical path for a real show anyway (FPP drives the show; HTTP is setup/diagnostics only) - which makes this a belt-and-suspenders fix rather than a load-bearing one for normal operation, but still worth having given how cheap it is and how bad the failure mode is (runs the trolley past a boundary with nothing watching).

**The fix**: `updatePidMode()` already computes `dt` (time since its own last tick) every call. Add a check near the top: if `dt` exceeds some threshold (~150-200ms - several multiples of `stepperPidTickMsConfig`, whatever that's currently set to, comfortably more than one HTTP request should ever block for) while the stepper is in continuous-run mode, `forceStop()` immediately rather than trusting a stale "keep going" command and letting the normal control-law path decide what to do next. Small, contained change - similar shape to the two guards already added. (Note: this was written when the tick was still a hardcoded 20ms; the tick is now configurable, default 5ms - the threshold should be relative to whatever `stepperPidTickMsConfig` is, not a fixed number.)

- [ ] Implement the stale-tick watchdog described above.
- [ ] Verify it actually catches the case it's meant to: force an artificial `loop()` stall (e.g. a slow debug endpoint) during continuous-run motion and confirm the stepper stops instead of coasting.

## The jerk was in the feedforward estimator all along - found, fixed, PID now matches Direct mode's smoothness (2026-09-07, evening)

User's reframing that started this: *"tuning for absolute speed isn't useful if it is going to be jerky... we can be behind a handful of frames and it will still look good - there is almost no instance that we need to be frame perfect, but we always need to be smooth."* That is a different objective from the one every previous session optimized against, and it changed the answer.

### Result

`pidFfWindowMs=200` + 16-bit DDP, with `Kp=4/Kd=0.3/pidAccel=50000` unchanged:

| period | rms_error | jerk | corner mean | frame lag p95 | stalls |
|---|---|---|---|---|---|
| 6s | 461.7 | 318.5 | 906.4 | 4.8 | 0 |
| 8s | 302.6 | 214.6 | 540.3 | 3.6 | 0 |
| 12s | 196.1 | 142.6 | 289.5 | 3.4 | 0 |

**At p12 jerk is 142.6 against Direct mode's 143 - the smoothness gap is closed outright** while tracking 1.4x tighter (rms 196 vs 274) with 1.6x tighter corners (290 vs 475). At p8, jerk is down 55% from the session's starting 481 (to 215, vs Direct's 153) while still tracking ~2x tighter than Direct on both rms_error and corner tightness.

### Root cause: the velocity feedforward's input was structurally aliased

Every prior smoothness attempt (derivative filtering, the shelved trajectory-reference layer, and a correction-term slew limiter built and discarded this session) targeted the P or D terms. Direct measurement showed why none of them ever moved the needle: with the correction slew-limited to 200 Hz/tick, the *commanded* speed still chopped ~550 Hz/tick. **The feedforward term was contributing ~350-400 Hz/tick by itself - more jerk than P and D combined.**

The old estimator computed `(target - lastTarget) / (time since last observed change)`. It only samples on `updatePidMode()`'s own 20ms tick while a 40fps source delivers every 25ms, so the *denominator* was quantized to whole ticks (20/40/60ms) and beat against the real arrival rate - a fine numerator over a coarse, beating denominator. **Verified independent of DDP bit depth**: 8-bit and 16-bit both measured ~570-580 Hz/tick of commanded-speed chop.

### Fix: least-squares slope over a fixed time window

Three estimators measured against the same p8 wave. Worth recording because the first two are the obvious ideas and both are wrong:

| estimator | jerk | rms_error | corner mean |
|---|---|---|---|
| EMA over consecutive changes (old), w=0.15 | 310 | 285 | 656 |
| Two-point difference over a fixed 160ms window | 748 (at 60ms) / 482 (at 160ms) | 284-302 | 398-536 |
| **Least-squares slope, 160ms window** | **310** | **291** | **482** |
| EMA, w=0.08 | 238 | 397 | 878 |
| **Least-squares slope, 240ms window** | **238** | **332** | **692** |

- A **heavier EMA** averages the noise but lags real corners badly (corner 656 then 878 as it tightens).
- A **two-point windowed difference** removes the aliasing (corners improve a lot - 398 at 60ms, the best measured anywhere) but only *rescales* noise rather than averaging it, so jerk got worse than baseline at short windows (748 at 60ms).
- A **least-squares slope** does both jobs with one knob: unbiased on a constant-rate ramp, averages all N samples instead of trusting two endpoints, and costs only ~half the window in lag. It strictly dominates the EMA at equal jerk (at jerk 310: corner 482 vs 656; at jerk 238: rms 332 vs 397 and corner 692 vs 878).

Window sweep (16-bit, p8, rms/jerk/corner): 100ms -> 295/377/466, 160ms -> 291/310/482, **200ms -> 319/216/619**, 240ms -> 332/238/692. 200ms is the knee, and is the new compiled default.

### 16-bit DDP: real, but not for the reason first assumed

Initially dismissed on the (correct) grounds that it does not change the trajectory, only the quantization of commanded position. It turned out to matter substantially - just for accuracy, not jerk: at p8 it took rms_error 249->216, corner mean 568->471, max_error 1024->592, and frame lag max 14.6->6.5. It did **not** improve jerk at all (510 vs 500), which is what ruled quantization out as the jerk cause and pointed at the estimator's denominator instead.

### Bug found and fixed: the overshoot safety net could freeze the trolley for seconds

`updatePidMode()`'s out-of-bounds `forceStop()` net re-fired on **every tick** the trolley sat parked outside `[0, bottomPosition]`. Its `!pidStopSettling` guard only holds until the motor stops - the trolley is then still out of bounds, so the next tick forceStop()s an already-stopped motor and re-arms the wait, a loop that only ends when the commanded target happens to travel back to where the trolley is parked. Measured directly: a **3.5s freeze at curPos=15670** against `bottomPosition+tolerance=15652`, with the control law correctly asking to drive back down the entire time and never getting a tick in which to do it.

This is pre-existing, not new - it is what the intermittent 600-700ms top-of-travel stalls in earlier sweeps were (all at curPos just past bottomPosition: 15677, 15725, 15650). **Fixed** with `pidOvershootLatched`: the net fires once on the way out, then hands control to the normal control law to drive back in - which is what its own comment always claimed it did. Zero stalls across every run since, at every period.

### Simplifications (all measured, none speculative)

- **Removed `Ki` entirely** (`pidKi`, `pidIntegral`, and the anti-windup saturation check). It was never once set nonzero in any bench session, so `iTerm` was always exactly 0 while its guard cost a saturation check and an accumulator every tick. The deadband's `moveTo()` snap already closes any steady-state P/D gap, and feedforward supplies the bulk velocity an integrator would have had to wind up to. Re-add deliberately, with a real sustained-tracking test, if a systematic bias ever appears.
- **Removed the shelved trajectory-reference layer** (`pidTrajFollow`/`pidTrajAccel`/`pidSmoothTarget`/`pidTrajVel`). Off by default, never worked (a 27-second freeze at one setting), and the velocity-domain approach supersedes its rationale entirely.
- **Built and then removed a correction-term slew limiter.** The idea was to rate-limit P+D while leaving feedforward sharp, so `pidAccel` would not have to filter both. Sound reasoning, wrong target: it changed jerk by less than run-to-run noise (295 vs 310) while costing real accuracy (rms 318 vs 291) and corner tightness (611 vs 482). Removed rather than left in "since it does not hurt."
- The old estimator's **outlier clamp and stale-target timeout are gone too** - a fixed window needs neither. A bounded-below denominator cannot produce a wild ratio, and if the target stops moving the window's numerator goes to zero within one window, which is the correct answer.

Net: `updatePidMode()` is meaningfully shorter than it was this morning despite gaining a better estimator.

### `pidAccel` re-swept downward first (superseded, but recorded)

Before any of the above, `pidAccel` was swept below the previously-characterized 15000 floor, since the old data had been scored against PID@50000 rather than against Direct mode. At p8 (8-bit, Kp=4/Kd=0.3, rms/jerk/corner): 6000 -> 758/129/1513, 8000 -> 554/158/1274, **10000 -> 448/190/1002**, 12000 -> 433/215/1079. `pidAccel=10000` beat Direct on accuracy *and* corners at 1.24x its jerk, versus 3.1x for the shipped config - a genuine improvement available with zero code. Superseded by the feedforward fix, which is strictly better (it filters only the noisy half), so `pidAccel` stays at 50000. Also confirmed the two levers are **substitutes, not complements**: LS-160ms + `pidAccel=20000` (338/264/682) is worse than LS-240ms alone (332/238/692).

### Tooling

`tools/ddp_continuous_test.py` gained `--bits {8,16}` (sends the stepper channel as one byte or two MSB-first, matching `ddp_handler.cpp`'s parse under `control16Bit`), and its corner detector now scales its reversal-boundary test to the observed DDP width instead of hardcoding 8-bit's 0..255 - existing 8-bit logs still analyse identically.

- [ ] **`control16Bit` is now persisted to NVS on the bench device.** The accuracy gain is real and worth keeping, but the DDP source must match - if xLights/FPP is still sending 8-bit, position will be wrong after a reboot. Revert with a `POST /config` body of `control16Bit=0` if the source is not switching too.
- [ ] `trackMode=4` is still a live-only override, unchanged from the previous session - flash-saved fallback remains Direct. Still wants an explicit decision before being persisted.
- [ ] `pidFfWindowMs` swept only at p8. p6/p12 were validated at the chosen 200ms and look right (jerk scales the way the wave does), but the *optimum* window may differ by period - a longer window may suit slow shows better.
- [ ] The derivative filter (`pidFilteredRate`, 0.85/0.15) is now of unclear value - it was kept because it modestly helped accuracy back when the feedforward was noisy. Worth one A/B run to see if it still earns its place, in the same spirit as the removals above.
- [x] **`Kp`/`Kd` re-swept against the clean feedforward - done later the same night.** See "PID tick rate found to be the real ripple lever" above: a Kd sweep (0.15-0.7) found Kd is not a real lever once feedforward carries the bulk of velocity; a Kp sweep found Kp=3 beats Kp=4 on jerk, ripple, corner tightness, and max_error, at a modest rms_error cost - now the compiled default.
- [ ] The device dropped off HTTP after 2 of ~6 flashes this session, both times recovered by immediately re-running the same `pio run --target upload` - the same already-documented flake, no new information, but it recurred often enough to be worth expecting.

## Time-budget PID velocity feedforward - implemented, two real bugs found/fixed, tuned, and beats Direct mode on accuracy (2026-09-07)

The user's own original framing, from earlier in this same marathon session: PID had no notion of *how much time it actually has* to get from one commanded point to the next, so it always drove at full reactive speed even for small moves paced by a much slower real show timeline. Direct mode (see the section below) turned out smoother for realistic pacing, but "I still think the PID mode could be better with some tweaking based on frame rate" - this section is that work.

### Design: `pidFeedforward` (new tunable, on by default)

Self-measures how fast the *target* has actually been changing in real time (not an assumed 25/40fps) and drives that velocity directly; PID's P/I/D terms then only trim the small residual error instead of doing all the work. See `updatePidMode()`'s feedforward block (`main.cpp`) for the exact implementation - tracks the last real target change and its timestamp, computes instantaneous velocity from the two, EMA-smooths it (weighted 0.7 toward the running estimate after tuning - a lighter 0.5/0.5 blend let single-sample sender jitter skew the estimate right at reversals), and zeroes it out if no real target change has happened in >200ms (source paused/stopped, stop coasting on a stale rate). A sanity clamp (±1.5x `pidMaxSpeedConfig`) keeps one wild instantaneous sample from skewing the EMA even before the final output's own clamp.

### Two real bugs found testing against actual DDPDebugger data (not just the bench triangle-wave tool)

Both were **new-to-this-session bugs in the reversal-freeze fix from the previous session** - real DDPDebugger playback exercises repeated continuous reversals in a way `tuning_harness.py`'s single-cycle-per-run tests never had:

1. **`pidStopSettling`'s wait used plain `isRunning()`**, not the `genuinelyRunning` check (`isRunning() && speed != 0`) already applied everywhere else in this function - missed when that fix went in. Caused a real ~700ms stall exactly at the bottom-of-travel (position 0) reversal: `curSpeedHz` read exactly 0 the whole time (a genuine, complete stop) while the wait stayed blocked far past the ~50ms a decel at `stepperPidAccelConfig` should take. Fixed to match.
2. **The overshoot safety check was re-triggering every tick** a benign few-step excursion (-32, -17, -2 observed) sat outside `[0, bottomPosition]`, calling `forceStop()`+re-arming `pidStopSettling` from scratch each time and never letting the actual recovery logic run to completion - this is what the user reported as "no stall at 0, but all kinds of stalls at the bottom" once the first bug was fixed. Added a 150-step tolerance (matching `RAMMED_STEP_TOLERANCE`'s precedent) and a `!pidStopSettling` guard so a genuine overshoot still stops exactly once per excursion.

Verified with a new tool, **`tools/ddp_continuous_test.py`** - reproduces DDPDebugger's actual continuous-loop triangle wave (`elapsed % period`, matched directly against `DdpSender.java` - including its real-elapsed-time value computation, deliberately not the cleaned-up scheduled-tick sender `tuning_harness.py` uses, since fidelity to the real tool was the point) entirely over HTTP (`GET /tunable`, `GET /compact-log` - no serial connection, so it can run repeatedly with zero risk of resetting the device mid-test). This tool means DDPDebugger-fidelity regression testing no longer needs a human running DDPDebugger by hand.

Also found and fixed the same night: the Compact Motion Log's HTTP ring buffer was too small (32KB only held ~13s of continuous active-tracking logging at ~50 lines/sec - a real multi-minute DDPDebugger session wrapped it, leaving only trailing idle time by the time it was fetched). Grown to 96KB.

### Gain sweep against feedforward - Kp=15 badly over-drives once feedforward carries the load

First test with the old Kp=15/Kd=0.3 (tuned for pure reactive PID) showed real sustained oscillation - expected, feedforward now supplies most of the velocity and Kp=15 double-drives on top of it. Swept Kp/Kd (all via `tools/ddp_continuous_test.py`, period=8s, 60s/~7 cycles per candidate):

| candidate | rms_error | jerk_per_sample | corner tightness (mean) | stalls | frame lag max |
|---|---|---|---|---|---|
| Kp=15 (unchanged) | real oscillation, not usable | | | | |
| Kp=3/Kd=0.3 | 315.9 | 455.2 | 591.4 | 1 (~600ms, top reversal) | 20.1 |
| Kp=3/Kd=0.5 | 472.6 | 473.7 | 600.1 | **0** | 23.2 |
| Kp=2/Kd=0.3 | 398.8 | 434.0 | 554.0 | 0 | **40.6 (over hard cap)** |
| **Kp=4/Kd=0.3 (chosen)** | **251.7** | 481.3 | 508.1 | **0** | **10.0** |
| Kp=3/Kd=0.1 | **catastrophic - see below** | | | | |

**Kp=3/Kd=0.1 caused a real, severe firmware bug, not just bad tuning**: the trolley froze completely (curPos stuck at one exact unchanging value, curSpeedHz reading exactly 0 for 40+ seconds despite targetSpeedHz correctly wanting -7000) and **never self-recovered** - unlike every other stall this session, which resolved on its own within a second. Worse: the corruption **persisted across subsequent config changes** - re-testing the already-known-good Kp=4/Kd=0.3 candidate immediately afterward, without a reboot, came back with a huge first-corner error (4036 steps) before "recovering" mid-test. A full device reboot fully restored normal behavior (re-verified Kp=4/Kd=0.3 matched its original clean numbers exactly). **Not root-caused - flagged as a real, serious, unresolved bug**: avoid very low Kd (below ~0.15) with feedforward enabled until this is understood. Also coincided with a flurry of ~20 unexplained reboots (`persist_log` showed boot numbers climbing rapidly, reset reason UNKNOWN, no serial connection open) - not confirmed as the same root cause, but suspicious timing.

**Kp=6 combined with a reduced `pidAccel` (20000, tried while chasing jerk - see below) also produced a real, severe instability**: rms_error 8061, 58% of samples over the 20-frame lag budget, obviously oscillating. A useful boundary to know - don't combine Kp much above 4 with a lowered accel.

### `pidAccel` as a jerk lever - a real but non-free tradeoff

Direct mode's own `trackAccel` (20000) is well below PID's `pidAccel` (50000) - tested whether lowering PID's accel would close some of the smoothness gap to Direct mode, since feedforward (not accel-driven reactive correction) now carries the primary "aiming" job:

| pidAccel | rms_error | jerk_per_sample | corner tightness (mean) |
|---|---|---|---|
| 15000 | 383.9 | 254.6 | 972.6 |
| 20000 | 317.7 | 299.5 | 806.6 |
| 35000 | 257.8 | 449.2 | 587.8 |
| **50000 (chosen)** | **251.7** | 481.3 | **508.1** |

The relationship isn't linear - jerk barely improves 50000→35000 but drops a lot below ~20000, suggesting `pidAccel` acts like a low-pass filter on the noisy Kp/Kd correction signal only once it's low enough to meaningfully rate-limit it. But every step down also loosens corner tightness and rms_error non-trivially. Given the corner-tightness/accuracy improvement over the pre-feedforward baseline (and over Direct mode - see below) was the user's explicit priority, kept `pidAccel=50000` - the smoothness gap to Direct mode remains open, flagged below.

### Final validation across realistic periods (6-15s) and vs. Direct mode

Kp=4/Kd=0.3/pidAccel=50000/pidFeedforward=1, verified clean (zero stalls, frame lag comfortably under the 20-frame budget except one single-sample outlier at p12) across periods 6, 8, 10, 12, 15s - see `tools/tuning_runs/final_comparison_summary.png` for the full graphed comparison against Direct mode (`trackAccel=20000`) at the same periods:

| period | PID+FF rms_error | PID+FF corner mean | PID+FF jerk | Direct rms_error | Direct corner mean | Direct jerk |
|---|---|---|---|---|---|---|
| 6 | 393.6 | 790.8 | 557.7 | - | - | - |
| 8 | 246.5 | 595.5 | 513.5 | 612.5 | 1161.1 | 153.0 |
| 10 | 189.4 | 397.6 | 418.6 | - | - | - |
| 12 | 167.4 | 397.1 | 265.3 | 273.8 | 475.3 | 142.8 |
| 15 | 155.8 | 275.3 | 350.1 | - | - | - |

**PID+feedforward wins decisively on accuracy and corner tightness at every period tested** (roughly 2.5x better rms_error and 2x tighter corners than Direct at p8; still clearly better, if by a smaller margin, at p12). **Direct mode is still smoother moment-to-moment** (jerk ~3x lower) - this is the one part of the user's stated goal ("smooth tracking like direct mode, but tighter corners... actual closer to commanded") not yet fully closed. Given the explicit priority order in the ask (corners tighter, actual closer to commanded, "OK to be a couple frames behind... don't be more than 20 frames"), and that PID+feedforward already meets the frame-lag budget comfortably, this was judged the better overall trade and made the new default.

**Applied**: `pidKp` compiled default 15.0→4.0, `pidFeedforward` compiled default false→true (`stepper_handler.cpp`) - both flashed and verified live from a genuine fresh boot. `trackMode` set to 4 (PID) *live* but **not saved to NVS** - the flash-saved fallback stays Direct mode, so an unplanned power cycle while unattended doesn't silently switch behavior without a human reviewing this first.

### Chasing the remaining smoothness gap: derivative filtering helped a little, a trajectory-reference layer found real bugs and was shelved (2026-09-07, later the same night)

User's question: "what do you think will help the smoothness?" Two candidates proposed, both tried in order as directed ("try #1, but if it doesn't get us close to Direct mode plan out and execute #4"):

**#1 - low-pass filter the derivative term's rate estimate** (`pidFilteredRate`, new state in `main.cpp`). Derivative-on-measurement of a quantized position signal is a classic PID roughness source. Tried at two filter strengths against the period=8s test: a light 0.5/0.5 EMA barely moved jerk (513.5→496.9, within run-to-run noise); a much heavier 0.85/0.15 EMA still barely moved it (→506.8) but did measurably tighten accuracy and corners as a side benefit (rms_error 246.5→228.8, corner mean 595.5→515.2). **Conclusion: the derivative term was never the dominant jerk source** - kept the 0.85/0.15 filter anyway (small, real, no-cost win), moved on to #4 per the instruction.

**#4 - an accel-limited trajectory reference the P-term tracks, instead of the raw DDP-quantized target directly** (`pidSmoothTarget`/`pidTrajVel`, gated by a new `pidTrajFollow` toggle, off by default). Reasoning for why this was the next candidate: neither feedforward's own smoothing nor the derivative filter touched jerk, but *lowering `pidAccel`* (tried earlier the same night chasing the same gap) did - a strong hint the P-term's own output was the real driver, since a lower `pidAccel` is really just an involuntary rate-limiter on how fast the stepper can react to pTerm's inherently choppy output (pTerm recomputes from the raw target every 20ms tick, exactly as jumpy as the target's own DDP-quantized arrival). Rather than blunt the *stepper's* reaction (the accuracy/corner-tightness cost already measured), this limits the *reference* pTerm reacts to instead - `pidSmoothTarget` slews toward the real target at a bounded rate/accel (`pidTrajAccelConfig`), so the target's own choppiness can't reach pTerm at all.

**Found two real, reproducible bugs, not just suboptimal tuning:**
- At `pidTrajAccel=30000` (the initial guess): real, sustained lag built up during fast (period=8s) motion - rms_error 246.5→840.5, two genuine overshoot stalls (one 1.1s).
- At `pidTrajAccel=80000`: a genuine **27-second freeze** (rms_error 7737, 68% of samples over the 20-frame lag budget).
- At `pidTrajAccel=150000` (tracks almost as tightly as the raw target): no failures, but the smoothing benefit vanished entirely - jerk got *worse* than not using this layer at all (558.5 vs 513.5 baseline).

**Not fully root-caused**, but the working theory: `pidSmoothTarget` keeps integrating its own position/velocity, but the whole trajectory computation lives *after* the `pidStopSettling` wait's early-return - so while a real `forceStop()` recovery is blocking this function (position frozen, waiting for the stepper to genuinely stop), the *real* DDP target keeps moving from ongoing packets, but `pidSmoothTarget` doesn't move at all. Once the wait clears, `pidSmoothTarget` has to catch up across a gap that grew for the whole stall duration - and if that catch-up itself provokes another overshoot (no anticipatory braking near the physical bounds was implemented), the cycle can repeat far longer than any stall this session prior to tonight.

**Decision: kept the code, gated off by default** (`pidTrajFollow`, off) rather than deleted - the core idea (limit the *reference* rather than the *stepper's reaction*) still seems like a reasonable framing, but a real fix needs (a) anticipatory braking as the reference nears `0`/`bottomPosition`, and (b) either freezing `pidSmoothTarget` in sync with `pidStopSettling`'s own wait, or resyncing it to the true target immediately once the wait clears, instead of replaying an accel-limited catch-up across whatever gap accumulated. Not attempted again tonight given the severity of what was already found (a device that won't move for 27 seconds is a real regression, not an acceptable tuning cost).

**Net result of this whole detour**: jerk is essentially unchanged from before this investigation (484.0 at p8 in final validation, vs. 513.5 originally) - the smoothness gap to Direct mode remains open. The one durable improvement is the derivative filter, kept as part of the recommended config.

- [x] **Closed the other way - a faster PID tick rate, not the trajectory-reference idea, turned out to be the real lever** (see "PID tick rate found to be the real ripple lever" above, 2026-09-07 late). At tick=5ms, jerk now beats Direct mode at p12+ periods. The trajectory-reference layer was never revisited - superseded, not attempted.
- [x] **Removed entirely 2026-09-07**, not just left off - see the tick-rate section above for why the underlying idea was superseded rather than fixed.

### Operational note: a DTR/RTS toggle sequence - or possibly just the first `pio.exe upload` after a device has been running a while - can leave the ESP32-S3 stuck in USB download/bootloader mode

Found live tonight: manually toggling DTR/RTS via raw pyserial (outside `pio.exe upload`'s own proven sequence) left the device completely unresponsive over HTTP *and* silent over serial - looked exactly like a hard crash. It wasn't: a careful serial reconnect showed `rst:0x15 (USB_UART_CHIP_RESET), boot:0x0 (DOWNLOAD(USB/UART0))` - the chip was sitting in the ROM bootloader waiting for `esptool`, not running any application code at all (explains the total silence, including no ESP-IDF boot banner). **Fix: just re-run `pio.exe run --target upload`** - a normal flash cycle's own reset sequence reliably returns it to run mode.

**Recurred several more times later the same night without any manual DTR/RTS involved** - just the plain `pio.exe run --target upload` sequence used all session, on what looked like the *first* upload attempt after the device had been running/being tested for a while (always recovered by simply re-running the exact same upload command a second time, immediately, no code changes). Not root-caused - could be a marginal reset-timing issue on this specific board/driver combination, unrelated to the original manual-DTR/RTS incident. Worth remembering either way: after any flash, verify HTTP connectivity before assuming success, and don't jump to "the new firmware crashed" - try one more plain re-upload first.

- [x] **Closed** - the tick-rate fix (2026-09-07 late) took jerk to 110-142 depending on period, at or below Direct's 140-155, with no accuracy cost. Ki is moot (removed).
- [ ] Root-cause the Kd=0.1 permanent freeze/corruption bug - a real, serious, reproduced-once issue, not yet understood. Until it is, treat Kd below ~0.15 as an unverified danger zone with feedforward enabled.
- [ ] Investigate the ~20-reboot flurry that coincided with the Kd=0.1 testing window - not confirmed as the same root cause, but the timing is suspicious and reset reason was UNKNOWN (a real crash signature) for all but one of them.
- [x] **Superseded** - the tick-rate fix is strictly better (no accuracy cost at all, where lowering `pidAccel` always traded some away). `pidAccel` stays 50000.
- [x] **Moot - Ki removed entirely 2026-09-07.** Never set nonzero in any session; see stepper_handler.h's declaration comment for why it was removed rather than left untested.
- [x] Superseded by the later, more thorough re-tune the same project - see the live curated TODO list at the top of this file for the current, still-open version of this decision.

## Direct-mode reversal lag investigation + a real crash bug found and fixed (2026-09-06/07)

Prompted by real-world testing: the user compared a clean reboot (no live overrides - device's true persisted defaults, which turned out to be `trackMode=0`/Direct, not PID) against real DDPDebugger/xLights playback at several cycle periods. **10-20s periods were "pretty darn smooth," 5s was jerky (mid-point), 4s clipped the travel ends without being jerky.** The 5s/4s cases are a genuine physical speed-ceiling limit, not a bug: a 5s full-range cycle needs ~6205 steps/s average against a 6500-7000Hz ceiling, essentially zero margin - confirmed by the user separately noting that changing normalSpeed (6000-8000Hz) didn't change that behavior at all, consistent with DDPDebugger's period sender using an eased/non-linear curve whose peak instantaneous velocity exceeds the simple average-speed math.

The 6-12s periods being smooth "other than the end reversal being kinda jerky" turned out to be a real, fixable issue. Captured real motion data live during an actual DDPDebugger session via the new `GET /compact-log` HTTP endpoint (see below) and found: **actual position lags commanded by a large, persistent margin** (up to roughly half the travel range) throughout each leg, cruising at a nearly-constant speed well under the tracking profile's own ceiling rather than tracking tightly - then snapping to the fast `normalSpeed`/`normalAccel` profile right at each reversal once the accumulated lag exceeds `trackMaxLag` (3000 steps). That hard snap is the "end reversal jerky" the user described. Root cause: Direct mode issues a fresh `moveTo()` on every single DDP command (`handleDirectModeCommand()`), and the tracking profile's `trackAccel` (5000 steps/s²) is too low to let the ramp generator build real speed between re-targets, converging to a steady-state cruise near the average incoming rate instead of the instantaneous commanded position.

**Swept `trackAccel` (5000 → 10000 → 20000 → 30000 → 50000), verifying no step loss at every level via `$CHECKSTEPS`:**

| trackAccel | rms_error | max_error | jerk_per_sample (roughness) |
|---|---|---|---|
| 5000 (was default) | 1511 | 3221 | 92 |
| 10000 | ~1112 | ~2385 | 98 |
| **20000 (now saved)** | **563** | **1224** | **196** |
| 30000 | 415 | 897 | 232 |
| 50000 | 302 | 619 | 461 (visible speed jitter on the plateau, same texture as PID's ripple) |

Higher `trackAccel` monotonically shrinks the lag (and therefore the reversal snap), but past a point trades it for a *different* roughness - small, rapid speed jitter during cruise, the same underdamped-resonance flavor PID's damping sweep found. `trackAccel=20000` looked like the knee of the curve (~63% error reduction, reversal no longer hits the speed ceiling) without the jitter showing up yet - **saved to flash** via `POST /save-stepper` (full-field payload, to avoid the endpoint's checkbox-omission-disables-it hazard - see CLAUDE.md's `/save-tmc` precedent). Widening `trackMaxLag` instead (tested at 6000) made things measurably *worse* (rms_error 2454 vs baseline 1511) - it just lets more lag accumulate before the same hard-snap correction, no benefit.

- [ ] `trackAccel` beyond 20000 (30000/50000) is available if the user wants tighter tracking and doesn't mind more jitter - not chosen by default, a subjective call.
- [ ] The 5s/4s speed-ceiling cases aren't addressed by any of this - would need either a higher safe cruise-speed ceiling (already found to sound close to stalling around 8000Hz) or accepting sub-~6s full-range cycles as outside the device's honest capability.

### Real firmware crash found and fixed: heap churn from chained String concatenation in hot-path logging

While running the `trackAccel` sweep, a homing attempt was interrupted mid-search by an unexpected reboot (`persist_log` showed a new boot number appearing ~2s into an active search, reset reason UNKNOWN). Root-caused to `logCompactMotion()` (`main.cpp`) building its CSV line via ~12 chained `String + String` operators, each allocating its own heap temporary - harmless occasionally, but this fires every 20-50ms during any active tracking/homing motion, and sustained heap churn at that rate caused a real, reproducible crash. The same mechanism had already produced a milder symptom earlier in the session: embedded NUL bytes in place of newlines in the `GET /compact-log` buffer (a genuine data-corruption bug, recovered post-hoc for analysis by treating NUL as an additional row separator).

**Fixed**: rewrote `logCompactMotion()` to build its line into a fixed 160-byte stack buffer via `snprintf()` instead, matching this codebase's own existing convention for hot-path buffers (`handleStatusData()`'s static `statusDataBuffer`). Applied the identical fix to `ddp_handler.cpp`'s DDP reception log (`appendDdpRxLog()` and its 4 call sites) - same chained-String pattern, same risk, not yet observed to crash but sharing the exact same hazard, and the whole point of that log is extended real-world monitoring - exactly the sustained load that triggers this bug class. Verified: re-ran the `trackAccel` sweep (including the exact candidate that crashed) with the fix in place, multiple clean homing cycles, no recurrence.

- [ ] Consider auditing other hot-path code for the same chained-String-concatenation pattern - this was found reactively (a live crash), not via a systematic review, so other instances may exist.

## Queued: web UI cleanup + a few real questions (2026-09-06, not yet started)

Batch of settings-page cleanup the user wants queued for a later session, not acted on now. Source of truth for the UI is `web/index.html`/`web/script.js` (see [WEB_DEVELOPMENT.md](WEB_DEVELOPMENT.md) - never hand-edit `include/html.h`).

- [ ] Remove the **Compact Motion Log** checkbox from the Settings page (`web/index.html:170`, `id="compactLog"` or similar - bench/tuning-only, shouldn't be user-facing).
- [ ] **StallGuard removal - needs a real decision, not just a UI trim.** `updateRammedIntoStopCheck()` (`stepper_handler.cpp`) only detects a jam *at* the homing switch (switch-state + position) - it does **not** cover a jam mid-travel away from the switch. StallGuard is what caught the real 2026-08-30 incident: a homing search ran the stepper at a steady commanded speed for 30+ seconds, genuinely jammed against the mechanical stop, with the switch never tripping (see `tmc_handler.cpp`'s stall-cutoff comment block). StallGuard is currently disabled anyway (false-positive-prone - see memory/TODO history), so removing its settings wouldn't change current behavior, but it would remove the *option* to fix it properly later without deciding whether some other mid-travel jam detection is needed first. If the decision is "yes, remove" - drop the TMC2209 stall-detection settings (threshold, enable) from Settings, and the "Stall Guard" status line (`web/index.html:136`, `id="tmc-stall-status"`) from the status page.
- [ ] On Settings: remove **Jump Start** field (`web/index.html:287-288`, `id="jumpStart"`), add the **PID tuning parameters** (currently RAM-only via `$SET`/`GET /tunable`, no Settings-page UI or Preferences persistence at all - list has grown across two sessions and is now `pidKp`/`pidKd`/`pidMaxSpeed`/`pidAccel`/`pidDeadband`/`pidReengageThreshold`/`pidFeedforward`/`pidFfWindowMs`/`pidLookaheadMs`/`pidTickMs`/`pidLogMs` - `pidKi` was removed 2026-09-07, never add it back to this list without checking `include/tuning_handler.h`'s own doc comment first, which is the current source of truth - this would be new work, not just moving existing fields), and remove the old **Small-Move Tracking** section (`web/index.html:305`, Direct-mode tracking fields: `trackThreshold`/`trackSpeed`/`trackAccel`/`trackMaxLag` - superseded by PID mode).
- [ ] Remove the **Stepper Control** section from Settings (`web/index.html:378`, `<h3>Stepper Control</h3>` - manual position/step controls) since it duplicates the status page's own "Manual Stepper Control" collapsible (`web/index.html:91-129`).
- [ ] Remove the **Stepper Driver (TMC2209 UART)** section from Settings (`web/index.html:415-416`) as a distinct section: fold **Run Current** (`tmcRunCurrent`) into the stepper configuration section instead, **enable UART driver control by default** (currently opt-in via `tmcEnabled`), and remove the StallGuard settings from it (contingent on the StallGuard decision above - don't remove if the decision is "keep, just needs a real threshold").
  - Remove **UART Link** status from the status page (`web/index.html:134`, `id="tmc-connected-status"`).
  - Decide if **Diagnostics** (`web/index.html:135`, `id="tmc-diag-status"` - GSTAT-derived driver diagnostics) is worth keeping; if so, fold it into the main Stepper Status fields (`web/index.html:79-89`) rather than the separate `tmc-status-box` sub-box it's currently in (note: that sub-box is already nested inside the Stepper Status `.status-box` div today, just visually/structurally separate - the ask is to flatten it into the same field list, not relocate it further).
- [ ] **Full travel measurement - open correctness question, not yet investigated.** The user's concern: the homing bounce (rope paid all the way out, then wound back on from the other side until the switch trips a second time - see CLAUDE.md's Homing Process) may not measure `bottomPosition` symmetrically if the return leg's speed profile differs from the outbound leg's (e.g., already at cruising `homeSpeed` when leaving the switch on the way back vs. accelerating from rest on the way out, or gravity assisting one direction and resisting the other changing effective ramp distance). Needs a real look at `updateHoming()`'s speed/accel setup for each leg before concluding whether this biases `bottomPosition` - not yet investigated.
- [ ] Be ready to `#define` off the encoder status/count (`web/index.html:88`, `id="encoder-count"`) rather than removing the encoder code outright, once tracking-mode tuning is done and the encoder is no longer needed for bench verification. (User is open to alternatives - removing vs. `#define`-gating - worth a quick discussion when it's time, not a unilateral call.)
- [ ] Remove the **Advanced Settings** section from Settings (need to identify its exact contents in `web/index.html` when this is picked up - not yet located precisely).
- [ ] In channel config, remove the **Serial Debug** checkbox (`web/index.html:548-550`, `id="protocolDebug"`) - folds into the new Live Log tab below instead.
- [ ] **New "Live Log" tab**: shows the live Compact Motion Log and/or the in-RAM diagnostic log (`persist_log`/the new `ddpRxLog` buffer - see this session's `GET /ddp-rx-log` work above) rather than requiring a serial connection. Needs a decision on transport - polling `GET /ddp-rx-log`-style endpoints on an interval is the simplest fit with what already exists; a push/streaming mechanism (WebSocket, SSE) would need new server-side work. Serial Debug (`protocolDebug`) either gets a tickbox on this tab, or - open question - just stays on permanently if it's confirmed not to cost anything (needs a quick check of its actual overhead before deciding).
- [x] **WiFi AP-mode retry timer - answered, not present.** Checked `checkWifiConnection()` (`wifi_handler.cpp:134-181`): it explicitly returns early whenever `WiFi.getMode() == WIFI_AP` (line 150), so there is currently **no** periodic attempt to fall back to station mode once the device drops into AP mode - it stays in AP until a manual reconnect (web UI/serial `w`/`a` commands) or reboot. Adding a periodic AP→STA retry (e.g., try reconnecting with saved credentials every N minutes while in AP mode, falling back to AP again on failure) is real, wanted work, not yet done.
- [x] **Full config export/import - done (2026-09-07).** `GET`/`POST /config` (`config_handler.h`/`.cpp`) covers everything actually persisted to NVS - WiFi, AP, stepper, protocol/channel, TMC2209, LED settings. Not JSON in the end (user: "doesn't have to be JSON if it's too much... just want it easier to mess with the config", then explicitly asked for the same format both ways so a GET response posts straight back unmodified) - plain `key=value` lines instead, no parsing library needed. WiFi/AP passwords excluded from GET (write-only, matching the existing WiFi form) but settable via POST without clobbering if omitted - and that's really just the general partial-update rule (only keys present in the POST body are touched) applied to every field, not a special case. Verified live: 52-field round trip byte-identical, partial single-field update leaves everything else untouched, unknown keys reported rather than silently dropped. Stepper fields go through the existing deferred-save mechanism; everything else writes to NVS immediately (a reboot is still recommended for WiFi/TMC/LED changes to fully re-initialize - this endpoint doesn't replicate each domain's own live re-init dance).

## Added: bench-tuning harness (`tools/tuning_harness.py`) + a ground-truth skipped-step check

Automates the manual "edit a setting, run a pan, eyeball the serial log" loop that produced the tracking-jerkiness findings above: a small `$`-prefixed extended serial command protocol (`tuning_handler.h`/`.cpp`) lets a Python script (`tools/tuning_harness.py`) apply tunables live (`$SET`, RAM-only - no flash writes/wear), query status (`$STATUS`, `$GET ALL`), trigger homing (`$HOME`), drive a synthetic 8-bit triangle-wave DDP position curve at several cycle durations while capturing the Compact Motion Log, then compute metrics (RMS/max error, jerk proxy, near-stall %, tracking vs. normal %) and plot commanded-vs-actual/speed/error per run. Everything accumulates to disk under `--out-dir` (raw `.log`, structured `.json` sidecar, per-run `.png`, plus running `summary.csv`/`all_rows.csv`) so a tuning history can be datamined later, not just eyeballed run-by-run. Full protocol/usage docs in `tools/README.md`.

**Also added - `$CHECKSTEPS`, a real ground-truth step-loss detector.** Prompted by the observation that there's no way to actually *see* skipped steps except by commanding the trolley back to a known reference point and watching whether the physical homing switch fires before the step counter believes it should - `getCurrentPosition()` is pure pulse-counting bookkeeping and cannot itself notice a commanded step that didn't physically happen. `$CHECKSTEPS [target]` (default target `0`) does exactly this: a direct `moveTo(target)` bypassing DDP/tracking-mode entirely, watching the existing ISR-deferred switch-trip flag (`pendingForceStop`, same one `updateHoming()` already reacts to) in real time via a new `updateStepCheck()` state machine (`stepper_handler.cpp`, called from `loop()` *before* `updateHoming()` so it gets first refusal on the flag while a check is running). An early trip means real steps were lost since the last homing/check - the residual step count is roughly the number lost - and it correctly leaves the device `homed = false`, same as any other unexpected trip outside of homing. `tuning_harness.py` runs this by default before every stress-test duration (`--check-steps-between-runs`, on by default; `--rehome-between-runs` does a slower full both-ends re-home instead), auto-recovering with a full re-home if it detects drift.

- [x] Firmware builds clean with all of this wired in (`updateStepCheck()`, `$CHECKSTEPS`, extended `$STATUS`).
- [x] Harness code is syntax/CLI-checked and the analysis/plotting path (`--reanalyze-dir`) is smoke-tested against real captured data (`log.log`).
- [x] **First real hardware run, 2026-08-30** (device on COM7): the whole pipeline works end-to-end - `$SET`/`$GET ALL`/`$STATUS`/`$HOME`/`$CHECKSTEPS` all confirmed live, a full session (apply config → home → check → send triangle wave → capture/plot/summarize) ran cleanly against the real device. Findings from this session:
  - **Reset-on-serial-open is real on this hardware**: opening a new serial connection reboots the device (standard ESP32 behavior - the USB-serial chip's DTR/RTS lines toggle EN/GPIO0, same mechanism flashing tools use), and with `autoHomeOnBootConfig` on, that also kicks off a full auto-home. `DeviceLink.__init__`'s old fixed 2s sleep didn't account for the auto-home (which took ~15s), so it's now `wait_until_ready()`: polls `$STATUS` until it actually responds, then if it comes up `homing=1`, waits for that to finish too, before returning control to the caller.
  - **`$CHECKSTEPS` round-trip test** (short 0→1000→0 move, full re-home between each of 5 repeats, so each is an independent measurement): the return-to-0 leg tripped early every single time, by a *remarkably consistent* 23-29 steps (26, 29, 23, 28, 29) - not growing, not random-walking. That consistency across independently-rehomed trials points toward a repeatable mechanical/approach-dynamics characteristic (the original homing search approaches the switch continuously at homing speed from a long run-up; this test approaches from a dead stop only 1000 steps away using the normal profile's accel, a very different impact speed/momentum against the same physical switch) rather than genuine random step loss - but this is a hypothesis, not confirmed. `bottomPosition` itself also varied by only a few steps across the 5 independent homings (15488-15494), consistent with ordinary mechanical repeatability.
    - [ ] Worth a follow-up test varying the approach speed/distance for the verification move itself (e.g. `$CHECKSTEPS` after a much longer runway, or at homing speed instead of normal speed) to see if the ~23-29 step offset shrinks - that would confirm it's approach-dynamics rather than loss.
  - **First real Direct-mode triangle wave** (8s full cycle, example_config.json defaults - `trackThreshold=2000`, `trackAccel=5000` vs. normal `20000`): confirms the already-diagnosed jerkiness root cause on actual hardware for the first time - a steady ~1700-step commanded/actual lag through most of the ramp, actual speed capped around 3900 Hz well below the configured 6500 Hz cruise target (consistent with nearly the whole move qualifying for the gentler tracking profile, whose lower acceleration is the limiting factor, compounded by Direct mode's constant re-planning). Plot/log: `tools/tuning_runs/smoketest_20260830_145234_dur8s.*`.
    - **New finding, not previously known**: peak achieved speed was clearly asymmetric between directions in this run (~3900 Hz one way vs. ~6500 Hz the other). Not yet explained - possibly a genuine mechanical/gravity asymmetry (rope-and-spool prop: one direction may be gravity-assisted, the other fighting it) rather than a software artifact. Worth deliberately investigating (e.g. compare achieved speed both directions at a fixed, generous accel/threshold with tracking disabled) before assuming it's a bug.
  - [x] **Found and fixed a real bug this surfaced immediately**: the first full-campaign attempt broke after its very first run (5s) with every subsequent run failing "ERR not homed". Root cause: normal DDP operation legitimately drives the trolley all the way down to real position 0 (0-255 maps onto `[0, bottomPosition]`, so 0 is an intended endpoint, not just a diagnostic reference) - but the earlier re-homing-drift fix treated *any* switch trip outside of homing as drift and force-unhomed the system, which fired the instant a normal run legitimately reached 0. Fixed in `stepper_handler.cpp`: only escalates to "not homed" when the trip happens more than 200 steps from position 0 (generous vs. the ~25-step legitimate-approach offset measured above) - `$CHECKSTEPS` itself is unchanged, since a trip during that deliberate verification move is always the signal being tested for.
  - [x] **Iteration 1 (`iter1_direct`, Direct mode, `example_config.json` defaults) run clean after the fix** - all 5 durations, every inter-run `$CHECKSTEPS` came back `tripped=false` (no drift detected across the whole session). Results (`tools/tuning_runs/iter1_direct_*`):
    | duration | rms_error | max_error | tracking_pct |
    |---|---|---|---|
    | 5s | 4219 | 5965 | 21% |
    | 8s | 1870 | 3577 | 94% |
    | 12s | 965 | 2106 | 100% |
    | 16s | 557 | 1206 | 100% |
    | 20s | 349 | 783 | 100% |

    Error scales down smoothly and predictably as duration increases, consistent with the trolley's known ~2.5s physical full-range limit (5s asks for a 2.5s half-cycle - right at that limit, hence the worst tracking and the only run where normal profile dominates over tracking). At 20s the actual-vs-commanded curves track closely with a small, steady ~300-400 step lag and no crashes/oscillation.

    **Two open questions from this data, not yet explained:**
    - The 5s run's actual speed pegs at the configured 6500 Hz target on the way up, but plateaus around **-8000 Hz** on the way down - exceeding the configured cap. Whether this is a real overshoot (FastAccelStepper's ramp generator briefly overshooting during a hard direction reversal) or a logging/measurement artifact in `curSpeedHz` isn't yet known.
    - Even the clean 20s run shows small recurring speed spikes during the "down" half (brief jumps from a ~1500 Hz cruise up toward ~700-1100 Hz) that aren't present on the "up" half - a smaller-scale version of the same directional asymmetry noted in the original 8s smoke test (there: ~3900 Hz one way vs. ~6500 Hz the other). Worth deliberately investigating this up/down asymmetry (mechanical/gravity vs. software) before the next iteration, since it shows up consistently across independent runs.
  - [x] **Up/down speed asymmetry investigated and resolved** - added a `--direction-test` harness mode (`DeviceLink.send_direction_test()`) that sends one packet to each extreme with a real pause once each settles, instead of a continuous triangle wave, specifically to isolate each direction's achieved speed from reversal artifacts. Needed a new firmware piece to be useful: `logCompactMotionPeriodic()` (`main.cpp`, called every `loop()` iteration) samples the Compact Motion Log every ~50ms whenever the stepper is running, regardless of DDP/mode dispatch - Direct/Coalesce previously only logged once at the moment a move was committed, so a single big jump (or a diagnostic move like `$CHECKSTEPS`'s) produced only one data point instead of a full trace. Skipped in Streaming mode, which already self-logs periodically.
    - **Result: no real asymmetry.** With a genuinely isolated move (full stop → pause → move), both directions hit the same ~6498 Hz cruise speed, matching the configured 6500 Hz target almost exactly (`tools/tuning_runs/directiontest2_*`). The asymmetry seen in triangle-wave runs (3900 vs. 6500 Hz in one run, 6500 vs. 8000 Hz in another - inconsistent between runs) was a reversal artifact of the continuous wave - the trolley still finishing the previous leg's deceleration/direction-change - not a fixed mechanical or gravity effect. (Gravity genuinely does help pull the trolley down given the up/down travel, but this data shows it isn't large enough to show up as a measurable speed difference.)
    - **Found and fixed a real harness bug while building this test**: the first attempt cut the "up" move off mid-travel (~2s into a ~2.7s move) before sending the "down" command. Root cause - `wait_until_idle()` was polled with zero delay right after `sock.sendto()`; the serial `$STATUS` round-trip (direct USB) can beat the DDP packet's WiFi round-trip to the device, so the very first poll could see `running=0` because the move hadn't *started* yet, not because it had already finished. Fixed with a mandatory 0.2s settle after each send before polling.
  - [x] **Iteration 2 (`iter2_coalesce`, Coalesce mode, same config otherwise)**: nearly identical results to Direct mode at every duration (e.g. 5s rms 4030 vs. Direct's 4219; 20s rms 422 vs. 349) - `coalesceMs=250`/`coalesceSteps=400` didn't meaningfully change behavior from Direct at these durations/rate. Not yet tried with more aggressive coalesce parameters.
  - [x] **Found and fixed a real, physical stall bug trying Streaming mode (iteration 3)**: all 5 durations produced *zero* Compact Motion Log rows - the trolley never moved. Investigated live and found `updateStreamingMode()`'s safety clamp (`curPos <= 0 || curPos >= bottomPosition`, meant to catch overshoot past the travel bounds) also fires when sitting exactly *at* 0 or `bottomPosition` - which is the normal resting position at either extreme, where every run starts. It force-stopped the instant a move began, and the restart logic just below it re-issues `runForward()`/`runBackward()` again next tick whenever `!isRunning()` (true right after that forceStop()) - the two together looped rapidly (start → force-stop before one clean ramp completes → restart → repeat), and this **actually stalled the motor on the bench** (reported by the user mid-session: "trolley is currently stuck due to moving too fast to get it started moving"). Recovered with a plain reboot - no mechanical jam, it re-homed cleanly afterward, confirming this was purely the stop/restart oscillation. Fixed by making the clamp strict (`< 0` / `> bottomPosition`) so it only trips on genuine overshoot, not on legitimately sitting at a boundary. Flashed; **not yet re-verified that Streaming mode actually works** - only that this specific stall mechanism is fixed by inspection. Next step: a short, closely-watched test (not another unattended multi-duration run) before trusting it.
  - [x] **Second real Streaming bug found and fixed, same session**: the retest above didn't stall, but actual speed rode the ramp up to a fixed value once and then never changed for the rest of the run, completely ignoring the live rate estimate afterward (near_stall_pct=66.6%). The user's own diagnosis nailed it precisely ("the speed commanded to the stepper did not ramp but went to full speed right away, but there was no momentum built up yet so it could not actually move" - describing the *mechanism*, not just the boundary-clamp trigger). Root cause confirmed against FastAccelStepper's own header docs: `setSpeedInHz()`/`setAcceleration()` only take effect after a following `move()`/`moveTo()`/`runForward()`/`runBackward()`/`applySpeedAcceleration()` call - **not on their own**. `updateStreamingMode()` called `setSpeedInHz()` every tick but only called `runForward()`/`runBackward()` on a direction change, so only the very first rate estimate (whatever was in effect when continuous running actually started) ever reached the motor. Fixed: call `stepper->applySpeedAcceleration()` explicitly when already running in the same direction (the common case). Re-tested: actual speed now visibly tracks the varying estimate through a full cycle; `near_stall_pct` dropped from 66.6% to 2.2% on the same 12s test.
  - [x] **New artifact noticed in that same retest, not yet explained** ~~commanded position (`streamRawTarget`, driven by the raw incoming DDP value) showed two sharp jumps (~4300 steps within ~100-200ms) partway through the cycle, which a clean triangle wave shouldn't do. Not yet clear whether this is a real DDP/WiFi packet-delivery hiccup (UDP has no delivery guarantee; this device is on WiFi, and heavy simultaneous serial polling from the harness could plausibly contend with DDP reception on the single core) or a Streaming-mode-specific logic issue - needs a dedicated look (e.g. capture with `protocolDebug` on to see raw incoming DDP values around the jump) before concluding either way.~~ **Root-caused and fixed for real, 2026-09-06 - see "Raw DDP trace jumpiness" section below.** Not a WiFi/delivery issue at all - `send_triangle_wave()` itself.
  - [x] **Iteration 3 (Streaming) re-run, full 5 durations, both bugs above fixed - Streaming mode shelved as a result.** The 5s run got the trolley stuck at the top (~15490) around t=3.5s and it never moved again for the rest of that run. Every `$CHECKSTEPS` pre-run check for the next three durations (8s/12s/16s) reported completing in ~20ms at position ~15490-15558 with `tripped=false` - meaning the library considered a `moveTo(0)` from ~15490 steps away "done" almost instantly, when that move should take ~2.7s minimum. That's not explainable by anything in `updateStreamingMode()`'s own logic (`$CHECKSTEPS` is a completely separate, direct `moveTo()` call) - it points at the stepper's own command queue/ramp generator getting wedged at a lower level, surviving for the rest of the session. The trolley stayed stuck at the top through the entire 20s run too - the user directly observed it "twitch" only in the last ~2 seconds, and the log confirms: flat at ~15490 from t=0 to t≈18s, then jittery, unstable negative speed (oscillating ±a few hundred Hz around -1500Hz, not smooth) for the remainder.
    - This is a real, physical instability - not just a bad log or metric - and it's the *third* distinct problem found in Streaming mode this session (after the boundary-clamp stall and the missing `applySpeedAcceleration()` fix), each requiring real hardware to discover. Given the user's own prior assessment ("the other algos did not appear to work very well, but they might have needed further tuning/testing") - Streaming has now had two real bugs fixed and re-tested, and still isn't stable, which is stronger evidence than "just needs more tuning."
    - **Decision: Streaming mode is shelved.** Marked "NOT RECOMMENDED" in the web UI's Tracking Motion Strategy dropdown and given a red warning note pointing back here (`web/index.html`). Direct and Coalesce (which performed nearly identically to Direct across all 5 durations - see iteration 1/2 above) are the validated, recommended choices, especially given the music-timing use case below needs predictable, physically-reliable motion more than maximum smoothness.
    - [ ] Root cause of the queue-wedge itself was never found - if Streaming is ever revisited, this needs a from-scratch investigation (possibly at the FastAccelStepper/RMT-queue level, not the mode-dispatch level) before it's trustworthy again.
  - [x] **Iteration 4 (`iter4_coalesce_wide`, coalesceSteps raised 400→2500, otherwise same as iteration 2)**: hypothesis was that `coalesceSteps=400` was triggering an early commit before the `coalesceMs=250` time window elapsed in most cases, defeating the point of batching. Confirmed correct by the plot - commanded position now shows a clear staircase (~250ms steps) instead of per-packet jitter, proving the wider threshold let the time window actually govern. **But it didn't reduce error** (12s: rms 1096 vs. iteration 2's 1007.5 and Direct's 965 - essentially unchanged). Each larger coalesced batch still triggers its own decelerate-to-a-stop-then-reaccelerate cycle (visible as a repeating sawtooth in actual speed, period matching the 250ms commit interval) - confirms Coalesce's batching alone doesn't escape `moveTo()`'s "always plans to fully stop at the target" behavior from the original root-cause writeup above, it just changes the granularity of the same underlying problem from fine jitter to a coarser oscillation. Not clearly better or worse for the music-timing use case - arguably a coarser, more mechanical-feeling oscillation is worse, not better, even at a similar RMS number.
  - [ ] Given both Coalesce variants tried land in the same place as Direct, further Coalesce parameter search is likely low-yield without a structural change (e.g. widening `coalesceMs` itself rather than the steps threshold, or a real look-ahead trajectory planner instead of point-to-point `moveTo()` commits). Lower priority than the still-open PID/closed-loop idea from earlier, or the planned rotary-encoder ground-truth position sensor - see below.

## Real tuning session (2026-09-06): StallGuard fighting real data, a genuine firmware crash found and fixed, and a real improvement

Resumed the harness-based tuning above once the encoder work (see its own section) was stable. First baseline run (current defaults, `trackAccel=5000`) immediately hit real trouble: repeated TMC2209 StallGuard trips during tracking-mode motion, `min_sg_result` bottoming out near 0 (well below the 50 threshold) on every run, and the ground-truth `$CHECKSTEPS`-style check between runs catching real step loss (3-13 steps) on the slower, tracking-dominated durations (12/16/20s) - notably *not* on the faster runs (5s/8s), which are mostly normal-profile motion despite steeper acceleration. Pointed at tracking mode's frequent small reversals demanding more instantaneous torque than available, not raw speed.

Added `sgResult` (live StallGuard reading) as a new column in the Compact Motion Log (`main.cpp`'s `logCompactMotion()`) and a matching plot panel/metric (`min_sg_result`) in `tuning_harness.py`, so this could actually be seen correlated with position/speed instead of inferred from isolated trip messages.

**Tried loosening the StallGuard threshold (50→25) to stop it interfering with getting clean data - made things worse, not better.** A real step-loss event caught mid-sweep came back at 57 steps - the worst of the whole session, far exceeding anything at threshold 50. Reasoning: a looser threshold lets a real stall run deeper before the safety cutoff engages, trading fewer interruptions for more severe loss per incident. Then, testing with StallGuard fully disabled, the user reported the trolley slammed hard into the physical homing stop - real step loss with nothing left to catch it before it became a real event, not just a metric. (Current settings this session also increased TMC run current 1200mA→1400mA via the web UI, for real headroom rather than just tolerance-shifting.)

**Found and fixed a real, reproducible firmware crash along the way**, root-caused with the help of a new persistent (reboot-surviving) diagnostic log:
- **Added `persist_log.h`/`.cpp`**: a small SPIFFS-backed log (the existing "spiffs" data partition, no partition table change needed) that survives a reboot, so a hung/crashed/reset device can be diagnosed afterward instead of just showing a fresh boot with no history. Logs the boot's `esp_reset_reason()` (POWERON/SW/PANIC/BROWNOUT/TASK_WDT/etc.) automatically on every boot - directly answers the "did it actually crash, or was this just a new serial connection's DTR/RTS reset?" ambiguity that came up repeatedly this session. Retrieval is `GET /persist-log` over WiFi (`?clear=1` to wipe it) - deliberately *not* serial, so checking it doesn't itself trigger the very reset it might be trying to explain. `l` serial command is a fallback for when WiFi is down.
- Wired in periodic homing-search breadcrumbs (state/position/speed/switch every 2s while `isHoming()`) and TMC stall-detected events, in stepper_handler.cpp/tmc_handler.cpp.
- **First real capture, while StallGuard was disabled**: a homing search breadcrumb, then `Boot #2 - reset reason: PANIC (crash - Guru Meditation Error)` - a genuine, reproducible firmware crash, not ambiguous at all. Root cause: `persistLog()`'s first version wrote to SPIFFS directly, and a real flash write briefly disables the flash cache - exactly the hazard CLAUDE.md already documents for `Preferences.putX()` writes racing the homing-switch ISR, just via a different flash API, and this time hit while the stepper was *actively stepping* (FastAccelStepper's own step-generation ISR is the leading suspect for what isn't IRAM-safe here, not this project's own homing-switch ISR, which already is). **Fixed**: `persistLog()` no longer touches flash at all - it only appends to an in-RAM buffer (`pendingBuffer`), which is always safe. A new `flushPersistLogNow()` actually commits to SPIFFS, called only from `loop()` gated on `stepper->isRunning() == false` (the boot/reset-reason line is the one exception - flushed immediately in `initPersistLog()`, safe there since nothing has moved yet). `readPersistLog()` includes any not-yet-flushed RAM entries too, so a live read still sees current data.
- Re-ran the exact same sweep after this fix: completed cleanly end-to-end, zero crashes, confirmed via the persist-log showing no PANIC/WDT/BROWNOUT entries anywhere in the session.

**Also found and fixed two real bugs in `tuning_harness.py` itself while chasing what first looked like the crash's cause (a `$HOME` call that "hung" for 20+ minutes) before the crash was root-caused**:
- `send_command()`'s pre-drain loop (`while not self._response_queue.empty(): get_nowait()`) had no time bound - a device flooding serial output (e.g. verbose per-tick homing debug prints during a long search) could in principle enqueue lines faster than this drains them, spinning indefinitely. Now bounded to a 0.5s wall-clock budget.
- `home_and_wait()` only recognized success (`homed==1`); a failed search (the device's own ~24s internal `homingOverallTimeoutMs` aborting into `HOMING_ERROR`) reads as `homing==0`/`homed==0` from `$STATUS` - indistinguishable from "hasn't started yet" - so the harness spun uselessly for the rest of its full external timeout (90s) instead of reporting the failure in ~24s. Now tracks whether `homing==1` was ever seen, and treats a return to `homing==0`/`homed==0` after that as an immediate, fast failure.
- (In the end, most of the actual multi-minute "hangs" that triggered this investigation turned out to be a mundane, unrelated cause: leftover ad-hoc serial probe scripts left running in the background from manual bench checks, holding the COM port open. Real lesson for future sessions: use `/persist-log` and `/status-data` over HTTP for out-of-band checks instead of spinning up extra serial connections mid-session, since each one also reboots the device.)

**Added a user-suggested stuck-at-switch detector, deliberately as post-hoc analysis, not new firmware logic**: since the encoder isn't meant to outlive this tuning phase, `is_stuck_at_switch()` (`tuning_harness.py`) detects "commanded to move, homing switch already reads triggered, encoder not advancing" from the already-logged `switchTripped` (new Compact Motion Log column, `isHomingSwitchTripped()`) and `encoderCount` columns, entirely in the harness - no new real-time firmware check tied to the encoder. Surfaced as a per-run warning, a metric (`stuck_at_switch_count`/`_ms`), and red-shaded spans on the position plot.

**Real result once all of the above was in place** (1400mA run current, `trackAccel` 5000→2500, StallGuard disabled): the 12-20s (tracking-dominated) runs improved substantially - RMS error dropped (1768→1378, 2003→1147, 1791→752) and **real step loss went to zero** on all three, where the old config lost 3-13 steps almost every run. The 5s run (mostly normal-profile, `near_stall_pct=0%`) got worse in RMS terms, a separate lower-priority thing to look at later, not a stalling problem. Data saved to `tuning_runs/summary.csv`/`all_rows.csv` (accumulating across every session) plus per-run `.log`/`.json`/`.png` as usual.

- [ ] StallGuard is currently disabled for tuning-data-collection purposes - needs a real decision before this ships: re-enable at a threshold informed by this session's `min_sg_result` data (bottomed at 2-4 even in the improved config, so a naive re-enable at the old defaults would likely still false-trigger), or find a different real-time safety mechanism. Not urgent per the user ("hardware can't hurt itself, it just sounds terrible") for continued bench tuning, but worth resolving before any unattended/production use.
- [x] **Explained by later sessions**: the 5s (and 4s) case is a genuine physical speed-ceiling limit, confirmed and re-confirmed across multiple later characterization sweeps - not a bug, not settings-dependent. The user's own later decision (2026-09-07) moved the project's tested floor to 6s for exactly this reason.

**Follow-up sweep, same session**: bracketed the 2500/1400mA result with three more configs (`trackAccel` 1500 and 3500 at 1400mA, and 2500 at 1300mA) to check it wasn't just "better than the one prior data point." Full comparison table and graphs: `tools/TUNING_SESSION_2026-09-06.md`. Findings:
- **1500 is confirmed worse, not safer** - more conservative acceleration gave both worse RMS error *and* more stuck-at-switch events than 2500. The sweet spot isn't toward gentler.
- **3500 gives the tightest raw tracking** (best 16s/20s RMS of anything tried) but reintroduces occasional brief stuck-at-switch contact that 2500 didn't have - a real tradeoff between tightness and clean endpoint behavior.
- **1300mA matched or slightly beat 1400mA** at the same acceleration - suggestive that the current bump wasn't strictly necessary, but only one run per config, so not confirmed against noise.
- Net recommendation: stick with `trackAccel=2500` - the only config with zero stuck-at-switch events on all three tracking-dominated runs, at a real (not best-in-class, but strong) RMS number.

**Found a second, real, still-open bug while investigating why "commanded position" spikes in the plots** (user noticed and asked about it directly): added the raw DDP value as its own plot panel (`tools/tuning_harness.py`) to check whether a glitch traced back to the network layer - it didn't; the raw DDP value is smooth. Root-caused instead to `stepper->targetPos()` (what the periodic Compact Motion Log tick uses for `cmdPos`) reporting "wherever the stepper just stopped" rather than "what was actually last commanded," specifically right after any `forceStop()` during normal (non-homing) operation - a homing-switch trip near position 0, or a StallGuard-detected stall anywhere along the travel (confirmed via a real captured run predating this session's StallGuard changes, where stalls were firing repeatedly at essentially random positions - matching "spikes the entire way", not just near the switch). Confirmed `tmcStatus.stalled` doesn't gate anything in the DDP dispatch path - it's purely cosmetic (status page display only).
- **Real physical consequence, not just a logging artifact**: after either `forceStop()` path, the stepper is left stopped with **no corrective move issued** - it just sits until the next DDP packet happens to carry a value different from before (the dispatch loop in `main.cpp`'s `loop()` only reacts to *changes* in `positionRequest`). Near a triangle wave's turnaround point, several consecutive samples often carry the same DDP byte value, so this can produce a real extra pause exactly where the wave is already slowest.
- [ ] **Not yet fixed**: re-issue `stepper->moveTo(lastCommandedTargetPosition)` immediately after either `forceStop()` path so the stepper resumes toward the real target right away, and switch `logCompactMotionPeriodic()`'s `cmdPos` source from `stepper->targetPos()` to `lastCommandedTargetPosition` (not subject to this lag) for accurate logging regardless.

**Also found and fixed while investigating this**: DDP's 4-bit rolling sequence number (`header.sequenceNum`, already parsed) was never actually validated - any packet was accepted regardless of arrival order. Added `isNewerDdpSeq()`/`lastAcceptedDdpSeq` (`ddp_handler.cpp`) to reject genuinely out-of-order or duplicate packets (seq==0 - "sequencing not in use" - always accepted, per the DDP convention), counted in a new `ddpPacketsRejectedOutOfOrder` stat (serial `s` status, `/status-data`). Turned out not to be the explanation for the spikes above (raw DDP was already smooth even before this fix), but a real, latent correctness gap worth closing regardless on an unreliable transport like WiFi/UDP.

- [x] **Superseded** - a later, more rigorous sweep ("Direct-mode reversal lag investigation") chose and saved `trackAccel=20000` from real multi-point data. The 2500/1400mA figures here were an early, coarser pass.
- [x] Done - `tools/TUNING_SESSION_2026-09-06.md` exists.

**Two real production features added the same session, in response to user questions about StallGuard's normal-operation false positives and remote calibration verification:**

- **"Rammed into the homing stop" detector** (`stepper_handler.cpp`'s `updateRammedIntoStopCheck()`, called from `loop()`) - deliberately independent of StallGuard (which the user correctly identified as too false-positive-prone during normal direction changes/starts to be the right tool here) *and* independent of the encoder (which isn't meant to outlive the tuning phase). Uses only the homing switch's raw state plus `stepper->getCurrentPosition()`: if the switch has read continuously triggered for 1.5s and the step count has kept changing since the trip started, that's a real jam - the stepper is still being driven, but the physical stop is denying it further travel, which is exactly the signature `getCurrentPosition()` (pure step-pulse bookkeeping, not a physical sensor) can show even with zero real motion. On detection: `forceStop()` + `homed = false`, matching the existing "trust nothing until a fresh re-home" pattern for real drift. Real, permanent firmware logic, not tuning-only code - bench-verified building/flashing cleanly.
- **`GET /verify-and-rehome`** (`html_handler.cpp`) - a remote self-healing calibration check for FPP/DDP-side scripting (run once before/between shows instead of trusting stale calibration silently). Non-blocking like every other diagnostic move here (a real check/rehome takes seconds, far too long to hold a WebServer handler open without starving `esp_task_wdt_reset()`, which only runs from `loop()`) - it only starts the check and returns immediately. If not yet homed, starts a full homing cycle; if homed, runs the existing `$CHECKSTEPS`-style ground-truth check (`startStepCheck(0, true)` - new `autoRehomeOnTrip` param, default `false` so `$CHECKSTEPS`'s own bench-diagnostic behavior is unchanged) and auto-triggers a full re-home if real drift is found. Poll `/status-data`'s new `isChecking` field, then `homed`/`isHoming`, to see the result. Bench-verified end to end on real hardware: both the "not yet homed" and "homed, clean check" branches confirmed working, no crashes.

## Fixed (for real): homing intermittently stalled the motor - root cause was `stepperAccelHoming=1000000`

This is the real root cause behind the "90+ second homing anomaly" section below, and behind the live "trolley stuck buzzing" / "slam into home, down, back up, slam into home" incidents during this same session. All of it traces to one setting.

**Diagnosis (from the user, confirmed by raw serial capture):** "If you're commanding the unit to run, and it is not opening the homing switch after a second or two, then we're jammed against the stop - either going the wrong direction or we've jumped into high step rate without accelerating first." Captured full raw serial (not periodic `$STATUS` polls) during a reproduction and found exactly this: `Starting runBackward() with speed 4000 Hz, accel 1000000 Hz/s` - reaching 4000Hz cruise in ~4ms. That's not a ramp, it's functionally an instant jump to speed. The motor stalled right at the start of that call and never got moving - the step counter kept incrementing fictitiously (matching the earlier +23314/-6845/-122110 "position" readings, none of them real physical travel) while the trolley wasn't actually turning, until either it self-recovered after a while, needed a manual nudge, or hit the ~30s per-state timeout.

A second, related symptom the user directly observed live: "It was part way down, and started buzzing like the accel went high before moving. Then it stopped and cleared on its own" - a brief stall specifically at the *first step* of a move, self-recovering. That's a classic stepper starting-torque symptom (the first step from a dead stop needs more torque than the same acceleration provides once already turning) - not fully explained by cruise acceleration alone.

**Fix - two changes, both needed:**
1. `stepperAccelHoming` default lowered from `1000000` to `20000` (`stepper_handler.h`) - a real ramp instead of an effectively-instant jump to speed.
2. `jumpStartConfig` default raised from `0` to `20` (`stepper_handler.cpp`) - FastAccelStepper's built-in fix for the starting-torque symptom specifically: one deliberately larger first step (speed = `sqrt(2*accel*jump_step)`, so ~894Hz kick at accel=20000) instead of ramping from true zero. Both are also now exposed as live-tunable (`homeSpeed`/`homeAccel` added to the `$SET`/`$GET` protocol - `tuning_handler.cpp`/`.h` - `jumpStart` already existed there).

Tested clean across multiple full homing cycles on the bench, including back-to-back immediate re-homes (the specific scenario that reliably reproduced the stall before) - 2/2 clean with both fixes together, vs. 2/2 failures at the old value.

**Important gotcha hit while verifying this fix**: changing the *compiled* default alone did NOT actually fix this device. After flashing it, the device still stalled/cycled erratically on boot - checked live and found `\$GET jumpStart` / `\$GET homeAccel` still reporting the *old* values (`0` / `1000000`). Cause: this device already had explicit values saved in NVS from some earlier session (`Preferences.getInt("jumpStart", 0)` / `Preferences.getInt("stepAccelHome", stepperAccelHoming)` - a saved NVS value always wins over the compiled fallback default, regardless of what the fallback is). **A compiled default change only helps a device that has never had that key saved** - any device already configured needs the fix applied and saved for real (web UI Stepper Configuration save, or the `/save-stepper` POST used here directly). Confirmed fixed on this device by POSTing the corrected full settings form to `/save-stepper` and verifying with a real reboot afterward that `\$GET ALL` now reports `jumpStart=20`/`homeAccel=20000` from flash, not RAM.

- [x] **Closed out** (user, 2026-09-07) - exercised by dozens of homing cycles across every subsequent bench session since.
- [ ] The starting-torque stall (jump-start symptom) could in principle also affect any *other* cold-start move, not just homing - e.g. the very first DDP-commanded move after a period of rest. `jumpStartConfig` is a global FastAccelStepper setting (applies to every move, not just homing), so this should already help there too, but hasn't been specifically tested for that case.

## Root-caused (see the section above): a homing search ran 90+ seconds and traveled well past the normal range

Noticed starting `iter4_coalesce_wide` (higher `coalesceSteps`, otherwise same as iteration 2): `rehome_and_check()` timed out at 90s. Checked live status mid-event: `homing=1, running=1, pos=23314` - the trolley was still actively searching, at a position well past the normal ~15490 full range, more than a minute past when homing normally completes (~16s). Stopped it with a reboot rather than let it continue (serial log showed it had just reached "Switch cleared, returning to home position" - i.e. it had actually found both switch triggers and was on its way back to zero when interrupted, so the search itself did eventually succeed, just took far longer and covered far more distance than normal).

A fresh homing immediately after the reboot completed normally in the usual ~16s and found `bottomPosition=15482` - right in line with every other reading this session (15482-15494). So the physical range hasn't shifted and there's no evidence of a lasting mechanical or calibration problem - this looks like one erratic search, not a new steady-state.

Since this happened on a freshly-booted device (opening a new serial connection always resets it - see the reset-on-open note above), it's unlikely to be leftover software state from the Streaming-mode incidents earlier in this session. Can't rule out that the twitching/repeated stall stress from that testing left the rope/mechanism momentarily disturbed (e.g. slightly off the spool) in a way that caused one bad search before self-correcting, but this is speculation, not confirmed.

- [x] Root cause identified and fixed - this was the `stepperAccelHoming=1000000` motor stall, see the section above. The "well past the normal range" position was never real physical travel - it was the step counter incrementing fictitiously while the motor was actually stalled in place, exactly as confirmed by the later raw-serial reproduction and the user's own live observations ("stuck buzzing", "slam into home... down and back up").
- [x] Turned out to be unrelated to the Streaming-mode incidents' mechanical stress theory - a much simpler, single-setting explanation, confirmed by fixing that one value and getting clean repeated homings afterward.

## Fixed: TMC2209 stall detection now catches stalls during homing searches too

Follow-up to the section above - the accel/jumpStart fix reduced but didn't eliminate homing stalls; it recurred multiple times later the same session, including jamming hard enough that the user directly observed it ("slams into the stop", "jammed"). A 30-second raw-serial capture during one occurrence showed the stepper running at a perfectly steady constant -4000Hz commanded speed the whole time, traveling to -114548 steps (7x+ the normal ~15490 range) without the switch ever tripping - the user confirmed "mechanically the unit is sound" (no rope/switch hardware failure), and then suggested the real fix directly: **the existing TMC2209 stall-detection feature (`tmcStallEnabledConfig`) was disabled during homing specifically** ("never fight the homing state machine's own switch-based logic" - reasonable-sounding when written, wrong in practice: it left genuine stalls during a search completely undetected, grinding blind for 30+ seconds or more).

Fixed: extended the stall check (`tmc_handler.cpp`) to also run while `isHoming()`, setting a new `homingStallDetected` flag (consumed unconditionally near the top of `updateHoming()`, same pattern as `pendingForceStop`) instead of stopping directly - lets the homing state machine handle the abort with its own profile-restore/`HOMING_ERROR` logic. Enabled and persisted `tmcStallEnabledConfig=true` on the bench device (was `false` - another case of the NVS-override lesson: had to POST it via `/save-tmc`, not just flip a compiled default).

**Confirmed working immediately**: the very next homing attempt stalled again, but this time was caught in ~130ms (`SG_RESULT=2` - deep into stall territory, not a borderline reading) with a clear `ERROR: Stall detected during homing search... aborting` instead of grinding for 30+ seconds. This is a real, validated safety win regardless of what's still causing the underlying stall - a bad search now fails fast and safely instead of physically grinding against a hard stop for half a minute or more.

**Still open: why the search stalls at all.** Catching it near-instantly (~130ms, essentially at the very first step) points toward a starting-torque/current-margin problem specific to this search (the `HOMING_WAIT_CLEAR_SWITCH` → `runBackward()` transition), not a "drives into the wrong stop over some real distance" issue - the earlier 30-second/-114548-step capture was very likely the same instant stall, just running blind the whole time before this fix existed, not evidence of a long wrong-direction journey. Also added position/speed logging to `HOMING_FIND_INITIAL`/`HOMING_FIND_OTHER_END`'s periodic prints and the `MOVE_OFF_FORWARD`/`BACKWARD` give-up messages, in case future evidence points elsewhere.

- [x] **Root-caused as a false positive, not a real stall - fixed for real.** Raising `jumpStart` to 100 made the reading *more* extreme (SG_RESULT 2 → 0), not better, which doesn't fit a genuine starting-torque stall. The real cause: `updateTmc()`'s stall-ramp grace period is anchored to an inferred `isRunning()` false→true transition, but several homing-state transitions start a new move right after a *previous* move stopped without producing a transition `updateTmc()` catches cleanly - so the 300ms grace period ends up measured from whenever an *earlier* move started, not the new one. Both confirmed false triggers fired 110-130ms after the new move began - well under the 300ms grace period that should have blocked them, and at two *different* transitions (`WAIT_CLEAR_SWITCH`→`FIND_INITIAL`, then `FIND_OTHER_END`'s interrupt→`MOVE_OFF_OTHER_END`), confirming it wasn't a one-off. Fixed with `tmcResetStallRampTimer()` (`tmc_handler.h`/`.cpp`), called explicitly right after all 12 move-issuing sites in the homing state machine plus `$CHECKSTEPS`'s diagnostic move, anchoring the grace period to each move's actual start instead of an inferred transition. **Confirmed on the bench: 3 consecutive full homing cycles, including two back-to-back immediate re-homes (the exact scenario that reliably failed before), all completed with no false triggers.**
- [x] **A second, related symptom the user reported was tracked down and fixed**: "when moving off the switch, we move about 10% of the way, then stop for a split second before continuing to move." Added position/speed diagnostic prints to all three "wait for move-off-switch to finish" states (`printWaitClearDiag()`) and caught it directly: `HOMING_FIND_INITIAL`/`HOMING_FIND_OTHER_END`'s interrupt handlers assumed "motor is already stopped by interrupt" and immediately issued the next move - but `forceStop()` isn't instantaneous, so the motor was often still coasting in the *old* direction when the new move queued, forcing it to decelerate the leftover motion, reverse, and re-accelerate (directly visible in a capture: commanded position went *more negative* before reversing back through zero). This was always true but invisible with the old `stepperAccelHoming=1000000` (the whole dance took a few ms and a handful of steps); lowering it to 20000 for the stall fix above made the same physical process take ~200-300ms and cover hundreds of steps, turning an imperceptible artifact into a visible pause - **the user correctly guessed this connection ("maybe changing the accel means we introduced the pause")**. Same root cause as "slams into the stop" below: `forceStop()` takes real time/distance to actually arrest momentum, not just software bookkeeping. Fixed with two new states (`HOMING_SETTLE_AFTER_INITIAL`/`HOMING_SETTLE_AFTER_OTHER_END`) that wait for `stepper->isRunning()` to genuinely go false before issuing the next move, instead of assuming `forceStop()` already finished (bookkeeping like `setCurrentPosition(0)` and the `bottomPosition` calculation still happen immediately - only the next *motion* command is deferred). Confirmed on the bench across two full cycles: both transitions now show a clean, monotonic acceleration with no reversal or dip.
- [x] **User confirmed on the bench (2026-08-30): "slamming seems to be resolved."** No code change specifically targeted the impact itself (that would have meant lowering `stepperSpeedHomingConfig` or adding a pre-switch decel zone, neither done) - most likely explanation is that the settle-state fix removed a *second*, compounding impact (the decelerate-reverse-reaccelerate dance was itself slamming the trolley back toward the switch area before this fix), which was making the primary impact feel/sound worse than it now does alone. Not fully explained, but real-world result is a genuine improvement either way.
- [x] **Same message, follow-up: "there is still a pause once it moves off the switch on each end."** Root cause: this was a *different* transition than the one just fixed above - not the pause *before* the move-off (already fixed), but the natural full-stop dwell *after* the discrete `move(2500)`/`move(-2500)` "move off the switch" step completes and before the next discrete search/return move starts. Rather than trying to shrink that inherent dwell, the whole state machine was reworked (2026-08-30, see the section below, "Homing state machine simplified") to remove the fixed-distance move-off detour entirely - after a trip and a genuine stop, the next search/return starts directly, with no intermediate move-off step to have a dwell around. **Not yet bench-verified** - implemented and build-verified only; next real homing cycle should confirm both this and the timeout rework below.

## Homing state machine simplified (2026-08-30): removed the move-off-switch detour, unified timeouts

Prompted by a request to review the whole homing procedure against how it was actually described (find initial -> reverse and count -> find other end -> half the count is the bottom -> return to zero, plus a stuck-switch-at-boot recovery: try one direction, reverse and try the other if it doesn't clear quickly, error if neither works) and check it wasn't overcomplicated. It was - the implementation had grown to 14 states, more than the description called for.

Reworked to 9 states (`stepper_handler.h`/`.cpp`):
- **Removed the fixed-distance move-off-switch detour** (`HOMING_MOVE_OFF_INITIAL`/`HOMING_MOVE_OFF_OTHER_END` and their settle states) between finding each end and continuing to the next phase. It was never actually necessary - reversing direction after a trip already moves away from the switch, and the RISING-edge interrupt can't fire again until the far end regardless of whether a fixed clearance move happens first. This is also the direct fix for the "pause after moving off the switch" item above - removing the detour removes the discrete stop-then-restart it required. A single generic `HOMING_SETTLE` state (dispatched via a new `HomingSettleAction` enum) still waits for a genuine stop before the next move, since `forceStop()` still isn't instantaneous - that part of the earlier fix was correct and is preserved.
- **Simplified stuck-switch-at-boot recovery** (`HOMING_MOVE_OFF_FORWARD`/`BACKWARD`) from 25 discrete 10-step `moveTo()` nibbles per direction (2.5s of repeated stop-start jitter each) to one continuous slow run per direction with a short (2s) timeout - matching the described procedure directly, with no incidental jitter of its own.
- **Replaced six separate per-state timeouts (10s/30s/2s each) with one overall homing watchdog**, scaled to the actual configured homing speed. The user's own reference point: a full down-and-up round trip takes ~5s at 6500 Hz, so ~15s (3x that) is the timeout at 6500 Hz - and since a slower configured homing speed genuinely takes longer to cover the same physical distance, the timeout is computed fresh in `startHoming()` as `15000 * (6500 / stepperSpeedHomingConfig)` (floored at 8s), not a fixed constant that would false-trip at a deliberately gentler speed. This also directly addresses the earlier 90+ second worst-case anomaly - the old per-state timeouts could stack; this can't.

Build verified clean. **Bench-tested (2026-08-30)**: normal case confirmed working well. The stuck-switch-at-boot recovery path was also exercised for real and mostly worked, but hit one real anomaly:

- **One occurrence: neither direction cleared the switch, ending in `HOMING_ERROR`** - the diagnostic log showed `pos=0 speed=0 running=0` for the *entire* 2s timeout window in both the forward and backward attempts, not a gradual stall after some real motion. That pattern points at the move never actually starting, not a mechanical jam. `handleHomingInterrupt()` fires unconditionally on any RISING edge regardless of homing state (by design, so a real switch trip is never missed), so if the switch's GPIO read another rising edge right as `runForward()`/`runBackward()` started, `pendingForceStop` would force-stop the just-issued move before it ever ramped up from zero - matching "stayed at literal zero the whole time" exactly. First guess was electrical noise from the stepper driver's current inrush coupling into the switch line, but that's speculative and not confirmed - a genuine stall (extra starting torque needed right against the switch actuator/hard stop) is at least as plausible and doesn't require assuming a noise-coupling path that hasn't been verified. Added a targeted diagnostic (`stepper_handler.cpp`'s `pendingForceStop` consumption block) that explicitly flags this specific scenario (`NOTE: Switch interrupt fired again during stuck-switch clearing...`) if it recurs, instead of only being inferable from the log staying flat.
  - **The system behaved safely** - it correctly reported the switch as stuck and stopped, exactly the intended fallback rather than doing anything dangerous or looping forever. Given this happened once out of several homing attempts, not treating it as a priority bug for now - the added diagnostic gives more to go on if it recurs, without committing to a specific unconfirmed cause. If it does recur, worth first checking whether it looks like a genuine stall (would show up as steadily-loaded then giving way, versus this occurrence's flat zero throughout) before reaching for an ISR debounce as the fix.
- [x] Whether the original instant-stall pattern was direction-specific (`runBackward()` vs `runForward()`) is now moot given it turned out to be a false positive at the code-transition level, not a direction-dependent mechanical one.
- [x] **Root-caused for real (2026-09-01), after several more real recurrences.** Neither of the two guesses above was it. Added targeted logging (`MoveResultCode` return value, plus a breakdown of `isQueueRunning()`/`isQueueEmpty()`/`isRampGeneratorActive()` - `isRunning()` is just an OR of those three) at the exact `runForward()`/`runBackward()` calls involved. Real capture of a failure:
  ```
  runForward() result=0 isRunning=1 qRunning=0 qEmpty=1 rampActive=1   <- accepted, MOVE_OK
  [homing] ... running=1 qRunning=0 qEmpty=1 rampActive=1              <- queue still never filled
  [homing] ... running=0 qRunning=0 qEmpty=1 rampActive=0              <- rampActive cleared itself
  ```
  `runForward()`/`runBackward()` returned `MOVE_OK` and `isRunning()` read true immediately, but FastAccelStepper's own ramp generator then silently abandoned the request within ~100-200ms - `isRampGeneratorActive()` cleared on its own with the step queue never once filled (`isQueueEmpty()` stayed true the whole time), and no `forceStop()` call anywhere in this codebase's own logging was involved. This is a genuine intermittent FastAccelStepper-internal timing issue, not anything decided at the application level - the call was made correctly and got a documented success code back, so it's not fixable by changing this codebase's logic directly.
  - **Mitigated rather than fixed at the source**: `HOMING_CLEAR_STUCK_SWITCH` now detects `isRunning()==false` while still within its own 2s timeout (i.e. not because anything here asked it to stop) and retries the identical `runForward()`/`runBackward()` call, throttled to at most once per 100ms. Recovers in well under 200ms typically instead of burning the full 2s timeout, trying the other direction, and erroring out.
  - **User found the same root cause independently, as a workaround**: jogging the trolley off the switch first (so `CHECK_SWITCH` takes the direct `FIND_INITIAL` path instead of ever entering `CLEAR_STUCK_SWITCH`) homes reliably every time - consistent with the bug being isolated specifically to `CLEAR_STUCK_SWITCH`'s `runForward()`/`runBackward()` calls, never the direct-search path's identical-looking calls.
  - [x] **Bench-verified working (2026-09-01)**: several real reproductions of the `CLEAR_STUCK_SWITCH` dead-move signature since, every one recovered via the retry within one or two throttle cycles and completed a normal homing cycle afterward.
  - [x] **Generalized after the very next reproduction hit `HOMING_FIND_OTHER_END` instead** - same dead-move signature, but that state had no retry logic, so it burned the *entire* ~24s overall watchdog before erroring out. Extracted the retry into a shared `retryMoveIfDied(forward, currentTime)` helper (with a shared throttle - safe, since `CLEAR_STUCK_SWITCH`/`FIND_INITIAL`/`FIND_OTHER_END` are mutually exclusive within one homing attempt) and applied it to `HOMING_FIND_INITIAL` (always retries backward) and `HOMING_FIND_OTHER_END` (always retries forward) too, not just `CLEAR_STUCK_SWITCH`. This is now genuinely "wherever a continuous search is running," not special-cased to the one state it was first noticed in.
  - [ ] The underlying FastAccelStepper timing issue itself is still not understood (why the ramp generator abandons a `keep_running` request without ever filling the queue, only sometimes). If this resurfaces somewhere the retry mitigation doesn't cover (e.g. a `moveTo()` call, which hasn't been directly observed hitting this yet), the same `qRunning`/`qEmpty`/`rampActive` breakdown technique is the way to characterize it again.

## Fixed: a stall on CLEAR_STUCK_SWITCH's first direction bypassed the "try the other direction" fallback

`HOMING_CLEAR_STUCK_SWITCH` already had "try forward, then backward, then give up" logic - but it only fired on a *timeout* (2s with no trigger). TMC2209 StallGuard typically catches a real stall in well under a second, so a genuine stall almost always won the race against that 2s timeout and fell straight through to the unconditional `homingStallDetected` abort (straight to `HOMING_ERROR`, regardless of which direction or attempt number). Raised directly by the user, reasoning through the mechanism correctly: "it's very likely to stall while trying to figure out which way moves off the switch" - if the first direction guessed happens to be the one that jams against the pulley's own wind limit, homing would abort immediately instead of ever trying the direction that would have worked, "we'll end up stuck on the far end of the pulley."

Fixed: a stall detected during `CLEAR_STUCK_SWITCH`'s first attempt (`homingCounter == 0`) is now treated exactly like a timeout - try the other direction - and only actually aborts to `HOMING_ERROR` once both directions have failed (stall or timeout either way).

- [x] **Bench-confirmed working immediately (2026-09-02)**: real reproduction showed the forward attempt stalling (`SG_RESULT=30` at ~1850 steps, with the initial buzz as it pushed into the stop the user directly observed), the new logic printed "trying backward...", backward attempt cleared the switch cleanly, and the rest of the cycle (`FIND_INITIAL` → `FIND_OTHER_END` → return to zero) completed normally.
- [ ] Deliberately not extended to `HOMING_FIND_INITIAL`/`HOMING_FIND_OTHER_END`'s own long searches - user confirmed this is correct as-is: "if we stall on the long search something is wrong" (unlike the short, symmetric clear-the-switch maneuver, reversing mid-search doesn't have an obviously safe interpretation - it would mean giving up on the direct route and taking the long way around the whole down-up cycle instead).

## Added: web UI Percent mode now explicitly supports 0-200%, for setting up homing test positions

Requested so the user could deliberately position the trolley past the bottom (rope wrapped the other way, right up near the far switch-trigger point) to exercise homing/stall-recovery logic on demand, rather than waiting to hit it by chance. Turned out to need no backend change at all - the position input never had a `max=100` restriction and the percent-to-steps conversion (`bottomPos * inputValue / 100`) has no clamp, so typing 200 already computed `2 * bottomPosition`, and `handleSetPosition()` calls `moveTo()` directly with no range restriction. Made the intent explicit instead of leaving it as an undocumented accident: `max="200"` on both position inputs (advisory only, same as Steps mode), relabeled "Percent" to "Percent (0-200%)", and added a tooltip explaining what the 100-200% range does.

## Fixed: commanding a move away from the switch, while resting on it, immediately killed the move

Reported (2026-08-30, `home.log`): homed and resting on the switch, commanding a DDP/manual move elsewhere immediately stopped again - log showed `Moving stepper to position: 3861` immediately followed by `Homing switch tripped at position -12 during normal operation - expected endpoint of travel, not treated as drift`. The "not treated as drift" message is correct (the existing near-zero tolerance worked as designed), but that's not the whole problem: `pendingForceStop`'s consumption block called `stepper->forceStop()` *unconditionally*, before ever reasoning about whether the trip made sense - so even a trip correctly judged "not drift" still killed whatever move was in progress.

Root cause: a switch's contact bounces (make-break-make) as it physically releases, and the ISR triggers on any RISING edge - so leaving a position where the switch is already triggered is exactly the scenario most likely to throw a spurious edge, and every one of those edges was being treated as a genuine new trip. A real trip and "currently moving away from the switch" are physically contradictory (arriving somewhere and moving away from it can't both be true), so that's now checked before deciding to `forceStop()` at all: if normal (non-homing) motion is already running with positive speed (away from the switch, per the established sign convention - see `stepperTrackModeConfig`'s direction notes elsewhere) when the interrupt fires, it's treated as bounce/noise and the move is left running, not stopped. Restricted to non-homing operation only - an active homing search still stops on any trip regardless of direction, since detecting the trip is the search's entire job.

- [x] **First fix (speed-sign check) was incomplete - recurred (2026-08-31)**: `Moving stepper to position: 3090` / `Homing switch tripped at position -9 ... not treated as drift` - the trip position (-9, barely moved) shows this fires right at the very start of the move, before `getCurrentSpeedInMilliHz()` necessarily has any measurable speed to report yet, so the speed-only check could read exactly 0 and fall through to the old force-stop behavior. Fixed by adding a second, more robust signal: `stepper->targetPos()` vs. the trip position - where the move is *headed*, known the instant it's issued regardless of ramp state, unlike instantaneous speed. Either signal now being true (target beyond the trip position, or already-measurable positive speed) is enough to call it a bounce. Still restricted to non-homing operation (`targetPos()` isn't kept updated during a continuous `run()` search per FastAccelStepper's own docs, which is fine since homing searches don't use this path at all).

Separately confirmed as **working as intended, not a bug**: tripping the switch by hand while the trolley is elsewhere on the rail correctly marks the system not-homed and the status page reflects it, while DDP/manual movement keeps working (matches the original design intent - drift should require a re-home before trusting position again, but shouldn't otherwise brick the device).

## Fixed: manual "Set Position" ignored not-homed, unlike DDP

Asked directly: "if I trip it and now it's not homed, shouldn't it no longer know where it is and not try to goto a requested position?" - a fair challenge, and the answer was a real inconsistency, not a documented spec. DDP position commands are correctly gated on `homed` (`main.cpp`), but `handleSetPosition()` (the web UI's manual "Set Position" control, `html_handler.cpp`) only checked `isHoming()`, never `homed` - exactly what `home.log` showed: absolute moves to 3861/0/3861 went through right after the drift warning, with no trusted zero reference at all.

Fixed: `handleSetPosition()` now also refuses (`400`, "Cannot move to position - system not homed") when `!homed`, matching DDP's own reasoning - an absolute target is meaningless without a trusted zero. **Deliberately left `handleMove()`'s relative jog (and the serial `f`/`b` commands) ungated** - that's the intended recovery tool for nudging the trolley back near the switch when not homed, and moving a relative step count doesn't depend on trusting `[0, bottomPosition]` the way an absolute target does. Also updated `script.js`'s Set Position handlers (Status tab and Settings tab both call the same endpoint) to actually surface a failure via the existing `showNotification()` helper instead of silently logging to the console - previously any non-200 response was ignored client-side.

**Also reported**: the earlier stuck-switch-at-boot / "won't re-home when off-switch" issue (see the sections above) is now not reproducing, with a guess that rebooting via the web UI (vs. power-cycling) might be interfering somehow. Worth noting: `handleReboot()` (`html_handler.cpp`) just calls `ESP.restart()` after a 1s delay - a genuine full hardware reset, functionally identical to the reset button or a power cycle, running `setup()` from scratch with no special state carried over. The web UI itself shouldn't be able to influence the boot sequence this way. More likely explanation for the inconsistent reproduction: the trolley's actual resting position differs between test attempts, and the leading theory for that bug (a switch read ambiguous specifically near its own actuation boundary) is inherently position-sensitive - it would only reproduce when the resting position happens to land in that narrow zone, not on every off-switch boot. The `CHECK_SWITCH: digitalRead=...` diagnostic added earlier is still in place for whenever it recurs.
- [ ] `tmcStallThresholdConfig` (currently 50, untuned) still hasn't been properly bench-characterized (compare SG_RESULT during genuine normal moves vs. a deliberately blocked one) - the extreme low readings seen here (0-6) were never close to the threshold being the deciding factor, but that's still worth doing before trusting the cutoff broadly during normal (non-homing) operation.

## Rotary encoder for ground-truth position - now motor-shaft-mounted, hardware-timer-poll-driven, on branch `encoder-work`

Raised while reviewing tuning data: `getCurrentPosition()` is pure step-count bookkeeping and can never itself notice a commanded step that didn't physically happen - `$CHECKSTEPS` and the `bottomPosition`-drift comparison are indirect workarounds for this, not a real fix. A quadrature encoder gives a real, continuous, independent position reading.

**Wired**: A = D0 (GPIO1), B = D9 (GPIO8) - standard two-wire quadrature. The originally-planned third pin (D5, an optional Z/index channel) wasn't wired - genuinely optional.

**Firmware went through six implementations before landing on the current one** (the first four below; RMT and the current hardware-timer poll are items 11-14 further down, after this list):
1. ESP32 PCNT peripheral, interrupt-driven (`pcnt_isr_service_install()` + H_LIM/L_LIM watch points for the 16-bit counter's overflow) - caused a severe regression on the bench: TMC2209 stall detection firing continuously, motor unstoppable and grinding repeatedly into the homing stop even with stall detection disabled.
2. PCNT peripheral again, but polling instead of interrupt-driven for the overflow handling - reproduced the *exact same* regression, ruling out the interrupt specifically as the cause and implicating the PCNT peripheral configuration itself (or these two pins) more broadly. A real, relevant fact surfaced while investigating: D9/GPIO8 is this board's default SPI MISO pin per the actual `XIAO_ESP32S3/pins_arduino.h` - these pins may not be as electrically free as first assumed, though the exact mechanism wasn't conclusively pinned down at the time.
3. Dropped the PCNT peripheral entirely for plain `digitalRead()` polling in `loop()` with a software quadrature decode (2-bit-state transition lookup table) - stable, bench-confirmed clean across many homing cycles, but with a known real gap: an intermediate quadrature state gets silently dropped (not miscounted, lost) if the pulley advances more than one step between `loop()` iterations - e.g. during a `FastLED.show()` call or a flash write blocking `loop()` for a stretch.
4. **Current**: genuine GPIO change-interrupts (`attachInterrupt(..., CHANGE)` on both A and B, `encoder_handler.cpp`'s `handleEncoderInterrupt()`) - the same category as `stepper_handler.cpp`'s `handleHomingInterrupt()`, already proven safe on this hardware. Not a peripheral at all, so it can't repeat the PCNT regression, and it catches every edge deterministically regardless of what `loop()` is doing, closing the gap #3 left open. Root-caused *why* PCNT broke things, not just confirmed that it did (2026-09-05, while investigating the core-split design - see the core-split section above): FastAccelStepper's own ESP32-S3 backend (the `DRIVER_DONT_CARE` default picks MCPWM+PCNT on this chip) uses PCNT hardware to count actual generated step pulses as ground truth, installing its own raw low-level PCNT interrupt handler via `esp_intr_alloc()` and poking peripheral-wide control registers directly - a second, independent PCNT configuration for the encoder was very likely corrupting that shared hardware/ISR state, not a coincidental pin-electrical-conflict as first suspected. This also confirms it was PCNT, not RMT, that caused the historical regression - RMT is FastLED's peripheral alone and was never actually involved.

**Bench-confirmed (2026-09-02, pulley-hub mount)**:
- Sign was initially inverted from `stepper_handler`'s convention (encoder went more negative as stepper position increased) - fixed by negating the quadrature lookup table so increasing encoder count matched increasing stepper position (moving down, away from the switch).
- Resolution was inherently coarse at the pulley hub (no step-up gearing, pulley only turns ~1.9 revolutions across the full 0-100% stroke) - measured ~154 counts end to end, consistent with ~20 PPR × 4 (quadrature) × 1.9 rev ≈ 152.
- **Worm gear ratio pinned down using this cross-check**: drivetrain is a 1.8°/step stepper (200 full steps/rev) through the worm to the pulley. The ratio was assumed to be 1:20 but the encoder data (~1.9 pulley revolutions for a ~15,500-step stroke) implied **1:10** instead - confirmed with the user and documented (README.md's "Drivetrain" note).

**Remounted on the motor shaft (2026-09-05)**, upstream of the worm reduction - purely mechanical, no firmware change needed (the interrupt-based decode doesn't care which shaft it's watching):
- **Sign false start**: a bench report right after the remount (count going more negative as commanded percentage increased) read as the sign having flipped again, so the quadrature table was re-negated a second time to match. A flashed test of that showed it was wrong - the *original* single-negated (pulley-hub-era) table was already correct on the motor shaft too; the second negation is what actually broke it. Reverted back to the single negation. Lesson: re-check a sign fix against an actual flashed/tested result, not just a description of the symptom, before committing to which direction to flip.
- **Encoder now resyncs to 0 automatically at the end of every successful homing** (`stepper_handler.cpp`'s `HOMING_RETURN_TO_ZERO` success branch calls `resetEncoderCount()` right alongside `homed = true`), so it recalibrates against the stepper's own trusted zero every cycle instead of drifting from whatever it read at boot.
- **Now shown on the web UI** too, not just serial/`$STATUS`/Compact Motion Log - new "Encoder Count" row on the Status tab (`web/index.html`/`script.js`, `encoderCount`/`encoderInitialized` in `/status-data`).

**Root-caused and fixed (2026-09-05): a real drift chased through slip, debounce, and finally landed on missing hardware filtering.** Repeated 0%→100%→0% cycles (ordinary DDP/manual moves, not re-homing between cycles, so nothing was correcting the encoder's reference) showed the encoder's reading at the physical 0/switch position drifting cycle to cycle, while the stepper's own return-to-switch position stayed perfectly repeatable every time. Chased down in stages:
1. **Mechanical slip suspected first** - the motor-shaft-to-encoder coupling is confirmed direct 1:1 (not a friction drive), so a monotonic ~100-150-count-per-cycle creep with a rock-steady span (~1150-1270 across cycles) looked exactly like real, consistent coupling slip. User reprinted the coupling with an inspection window to check directly, and added tape as a fix - but the *next* bench run (post-tape) showed the drift get *worse* and lose its smooth, monotonic character (span now varying ~27% cycle to cycle, zero-reference jumping irregularly including one large jump) - the opposite of what fixing real slip should do, and confirmed by the user's own visual inspection that there's no mechanical slip.
2. **Software debounce tried and rejected** - added and then removed the same day (see the two commits) after bench data showed a 1ms "ignore anything this soon after the last edge" window was far too aggressive (a ~1267-count unfiltered traverse dropped to 342-560, inconsistently, with the filter active) - the real inter-edge spacing during actual motion turned out to be nowhere near what a PPR/speed estimate predicted, and guessing a digital window blind was the wrong approach.
3. **Fixed for real: 0.1µF filter capacitors added from each channel (A and B) to ground**, forming a real analog RC low-pass with the existing 10k pull-ups - the missing piece the whole time (10k pull-ups alone, no capacitor, means nothing was ever filtering noise before the GPIO saw it). **Bench-confirmed clean**: 5 repeated 0-100% cycles gave spans of 1520/1521/1508/1502/1535 (tightest spread yet, ~2%) and a zero-reference wobbling only ±33 counts with no directional trend - consistent with ordinary measurement noise floor, not drift.
   - **This also resolves the earlier "8.2x, not 10x" worm-ratio discrepancy** flagged after the motor-shaft remount: the uncapacitor'd encoder was genuinely undercounting real transitions (signal integrity issue, not a wrong gear-ratio assumption) - 1267 counts uncapacitor'd vs. ~1517 average now with the caps in place. 1517/154 (the original pulley-hub count) ≈ 9.85 - right in line with the documented 1:10 ratio after all.
   - **Final resolution, motor-shaft mount, with filtering**: ~15488 steps / ~1517 counts ≈ **10.2 steps/count** - matching the original prediction almost exactly.
4. **Re-tested at a different stepper speed (5000Hz, down from 6500Hz) to confirm the fix generalizes**: 6 more cycles gave spans of 1514/1522/1527/1519/1519/1513 (~0.9% spread, tighter than the 6500Hz run) averaging ~1519 - essentially identical to the 6500Hz average, which is itself a good sign (resolution should be a purely mechanical/encoder property, independent of drive speed, and now it actually is). Zero-reference wobbled a bit more (down to -36) but stayed the same order of magnitude, no return of the old creep.
5. **Added a "missed transitions" diagnostic counter** (`encoder_handler.h`/`.cpp`'s `getMissedTransitionCount()`) after the user asked directly whether firmware might still be losing counts, given the intended use (tuning real trolley motion - the original reason for this whole effort) needs the encoder's noise floor to sit clearly below the hundreds-of-steps lag it's meant to catch, and the residual ±30-40 count wobble was worth investigating rather than accepting on faith. Counts every time the ISR sees an "illegal" 2-bit jump (both A and B appear to have changed between samples) - a real edge that was never sampled and is permanently unrecoverable, not just delayed; still dropped (guessing direction risks being wrong), but now visible instead of silently invisible. Surfaced in serial `s`, `$STATUS` (`encoderMissed=`), and the web UI's Encoder Count row.
6. **Added `$ENCDIAG`** (`tuning_handler.h`/`.cpp`, state machine originally in `stepper_handler.h`/`.cpp`, later moved to its own `encoder_diag.h`/`.cpp` - see below) to automate the manual batch-testing process itself - runs an 8-cycle 0%->100%->0% sweep at each of 3000/4000/5000/6500 Hz (64 direct `moveTo()` legs total, same bypass-DDP approach as `$CHECKSTEPS`), printing one CSV row per leg (`stepperPos`, `encoderCount`, `missedTotal`) instead of requiring cycles to be run and transcribed by hand one at a time. See `tools/README.md` for the full command reference.
7. **First real `$ENCDIAG` sweep (2026-09-05) confirms firmware-side loss is real, and points at a specific likely cause.** `missedTotal` climbed on every single one of 64 legs, never flat - roughly 10-13 missed transitions per leg regardless of stepper frequency (3000/4000/5000/6500 Hz all landed in the same ~10-13/leg range, not scaling up with speed), and roughly the same whether the leg tripped the switch or not. Both of those rule out the two obvious explanations (raw ISR throughput falling behind a faster encoder edge rate; something specific to the switch-trip/`forceStop()` moment) and point instead at something with a roughly fixed cost per *move*, most likely time-sliced contention with something else active on the same core throughout every move - and there's now a specific, well-supported suspect: FastAccelStepper's own MCPWM+PCNT backend installs its own raw PCNT interrupt handler via `esp_intr_alloc()` (see the PCNT root-cause entry above), core-affine to Core 1 because `initializeStepper()` runs from `setup()` there - the *exact same core* the encoder's GPIO interrupt is also on, for the same reason. Decided to fold the fix into the core-split work rather than a same-core ISR-priority fight (which risks the opposite problem - a too-aggressive encoder priority delaying the stepper instead): pin the encoder's interrupt handling to Core 0 alongside/instead of wherever LED work ends up, physically separating it from the stepper's Core-1-affine PCNT activity, rather than trying to out-prioritize it in place.
   - Also considered switching the underlying mechanism entirely, not just its core: RMT's RX/capture mode is a genuinely different approach (DMA-backed hardware edge timestamping, not CPU-serviced-per-edge, so it doesn't have the "was the ISR scheduled in time" problem at all) - ruled out for now while FastLED still owns RMT (same class of two-drivers-sharing-a-peripheral risk that broke PCNT), but reconsidered below once FastLED is temporarily disabled for the tuning work.
8. **File reorganization (2026-09-05)**: the `$ENCDIAG` state machine was moved out of `stepper_handler.h`/`.cpp` (production code) into its own `encoder_diag.h`/`.cpp`, matching `encoder_handler.h`/`.cpp`'s own separation - the whole point being that none of this encoder-tuning-support code is expected to stay in the codebase once stepper tuning is done, and keeping it in dedicated files (rather than mixed into `stepper_handler`) means it can be pared off cleanly later without touching production code. `resetEncoderCount()`'s single call site in `stepper_handler.cpp`'s `HOMING_RETURN_TO_ZERO` is the one remaining integration point still living in production code - small and easy to manually revert if/when the rest goes.
9. **Decided to temporarily disable FastLED entirely during this tuning phase** (set LED Pixel Count to 0 in the web UI - `initPixelLeds()` already skips `FastLED.addLeds()` at 0, no firmware change needed) - both to remove one more Core 1 activity source while diagnosing stepper/encoder timing, and because it frees RMT completely (FastLED is its only consumer in this firmware) - reopening the RMT-RX-capture idea above as a legitimate option now that it wouldn't be a second consumer of an already-claimed peripheral.

**Git housekeeping (2026-09-05)**: this branch accumulated real, unrelated fixes alongside the encoder work (the `CLEAR_STUCK_SWITCH` stall-fallback fix, the Percent-mode 0-200% UI change, etc.) that shouldn't wait on the encoder's own fate. Merged `encoder-work` into `main` as a whole, then brought `core-split`'s design doc in too - down to one active branch (`main`) instead of three.

10. **Core-pinning implemented, bench-tested, came back negative (2026-09-05)**: `engine.init(1)` pins FastAccelStepper's task explicitly to Core 1; a new `core0_task.h`/`.cpp` runs a dedicated Core-0 task whose first job was calling `initEncoder()` from within itself, moving the encoder's GPIO interrupt off Core 1. Two bench sweeps with `$ENCDIAG` - one with the encoder moved to Core 0 (missed/leg ~9-11, essentially unchanged from Core 1's ~10-13), one with the web status page's poll additionally removed from Core 0 (~8-9/leg, a marginal ~15% drop within normal run-to-run noise) - showed neither task/core placement change meaningfully reduced the loss. Ruled out "competing with a specific busy neighbor" as the cause.
11. **Switched to RMT capture (2026-09-05/06)**, since core-pinning came back negative and the real fix needed to remove the "was an interrupt serviced in time" question entirely, not just relocate it:
    - Checked whether upgrading the PlatformIO `espressif32` platform (7.0.1 -> 7.1.1, the latest available) would unlock the modern IDF5 channel-handle RMT API (`rmt_new_rx_channel()`, per-edge callbacks) FastLED's own source assumes - it didn't; the bundled `framework-arduinoespressif32` stayed at the same version (3.20017.241212) either way. Confirmed directly by checking the actual compiled object files: `idf5_rmt.cpp.o` is a near-empty 1228-byte stub (the `#if FASTLED_RMT5` guard evaluated false at compile time), while `idf4_rmt.cpp.o` is a real 385KB compiled unit - **FastLED is actually using the legacy IDF4-style RMT driver in this build, not the async IDF5 one** - correcting an earlier (wrong) claim in this same section that `FastLED.show()` is mostly non-blocking here via `drawAsync()`. Checked upstream too: recent arduino-esp32 has moved away from bundling a self-contained `tools/sdk` header tree altogether (now an ESP-IDF component dependency) - getting the modern RMT API would mean a real `framework=espidf`-style reconfiguration, not a version bump, so not pursued further.
    - Given only the older ring-buffer-based `driver/rmt.h` API is actually available, and RMT only watches one GPIO per channel, chose not to attempt full dual-channel (A+B) capture with a software-reconstructed merged timeline (real synchronization complexity, fragile, for code that isn't permanent). Instead: RMT RX on channel A only (`RMT_CHANNEL_7`, hardware glitch filter via `filter_ticks_thresh`, continuous capture into a ring buffer), X2 resolution (~20 steps/count, still far finer than the hundreds-of-steps lag this tool exists to catch) - direction for each drained batch comes from `stepper->getCurrentSpeedInMilliHz()`'s sign (the stepper's own currently commanded direction) rather than independently reading channel B. Accepted scope reduction: can no longer detect the shaft spinning opposite to what's commanded (never actually observed or suspected in this whole investigation) in exchange for removing the cross-channel timing-correlation problem entirely.
    - `encoder_handler.cpp` rewritten around this (same public API as before, plus a new `updateEncoder()` that must be called periodically - RMT's ring buffer is drained by application code, not delivered via a per-edge callback like the GPIO-ISR version was). `core0_task.cpp`'s loop now calls it every ~10ms instead of idling.
    - Side effect: bumped the project's PlatformIO `espressif32` platform 7.0.1 -> 7.1.1 while investigating (no functional change observed - same bundled framework version either way).
    - [x] **First bench run (2026-09-06) found a real bug**: `updateEncoder()` only pulled one chunk per call, so the ring buffer could fall behind and never catch up - the RMT driver itself started logging `RX buffer too small`/`RMT RX BUFFER FULL` and discarding real captured data once it filled, after which `encoderCount` never advanced again. Fixed by looping `xRingbufferReceive()` until it returns NULL each call, so every call fully catches up regardless of elapsed time; bumped the ring buffer 2048->4096 bytes too.
    - [x] **Second bench run found a second, more fundamental bug**: with the drain-loop fix in place, isolated legs still showed either a dropped delta (whole leg's worth of motion recorded as 0) or a flipped sign - 3 of 16 legs in one real sweep (~19%). Root cause: direction is inferred from the stepper's *currently* commanded direction at drain time, applied to the whole batch just pulled - if real data was still sitting in the ring buffer when a leg finished and nothing drained it before the *next* (reversed) move was issued, that stale batch got the new, wrong direction once it was finally drained. All the *other* legs landed within a couple counts of the expected ~778-780 (matching ~15456 steps / ~20 steps/count), confirming the raw edge-counting itself is solid - only direction-at-reversal is broken. Fixed for `$ENCDIAG`'s own case: `encoder_diag.cpp`'s `updateEncoderDiag()` now calls `updateEncoder()` synchronously the instant a leg is detected stopped, *before* issuing the next move - at that exact instant the stepper reads 0 speed, so the existing "keep last known direction" fallback correctly flushes any straggler under the old, correct direction before anything changes. Required making `updateEncoder()`/the shared counters genuinely thread-safe (a `portMUX_TYPE` spinlock) since it's now legitimately called from both Core 0 (periodic) and Core 1 (this synchronous flush) - see `encoder_handler.h`'s updated comments.
    - [ ] **This fix does not generalize to real tracking-mode tuning** - flagged directly by the user before it bit anyone. `$ENCDIAG`'s clean full-stop-between-legs pattern is what makes "flush when `isRunning()==false`" work; real DDP-driven tracking motion (`TRACK_MODE_DIRECT`/`COALESCE`/`LOOKAHEAD`) re-targets via `moveTo()` while potentially still running in the *other* direction, with no idle checkpoint to hang a flush off of. Not yet solved for that case - see the next item.
    - [ ] **Second mystery, not yet resolved**: even with the synchronous-flush fix, the misattributed legs were losing/flipping an *entire* ~4-second leg's worth of data, not just the 1-2 edges a mere ~10ms polling gap would suggest - implying the Core 0 task itself may be stalling for multi-second stretches under some condition, which would be a real, independent problem worth understanding regardless of the encoder. Added direct instrumentation to actually measure this instead of continuing to infer it: `core0_task.h`'s `getCore0TaskMaxGapMs()` (longest gap ever seen between consecutive loop iterations) and `getCore0TaskMinStackBytes()` (least free stack ever seen, via `uxTaskGetStackHighWaterMark()` - rules out a stack-overflow-adjacent cause if it stays healthy) - both high-water-marks since boot, surfaced in serial `s` and `$STATUS` (`core0MaxGapMs=`/`core0MinStack=`). Not yet bench-checked.
    - [ ] **Longer-term, the real fix for the general tuning case is likely giving direction independent hardware backing** (RMT on channel B too, accepting the cross-channel timeline-merge complexity originally avoided) rather than continuing to patch the single-channel/inferred-direction approach, since real tracking motion can reverse direction far more often and less predictably than `$ENCDIAG`'s sweep ever does. Decide after the Core 0 stall mystery above is understood - if that turns out to be the dominant error source, the direction-inference approach might be salvageable for infrequent reversals; if not, dual-channel is probably necessary.

12. **Core 0 stall mystery resolved: it wasn't Core 0 (2026-09-06).** Bench-tested the new instrumentation directly by driving the device over serial (homing + full `$ENCDIAG` sweep, repeated several times across the rest of this investigation): `getCore0TaskMaxGapMs()` sat flat at ~330ms across every run, matched exactly by a WiFi-disabled control run showing ~35-40ms instead - meaning that one-time gap is WiFi's own connection-time activity at boot, not a recurring stall, and it never grows during actual motion/moves. `getCore0TaskMinStackBytes()` stayed healthy throughout. The "whole leg's worth of data misattributed" symptom from item 11 was real, but Core 0 starvation was never the cause.

13. **RMT abandoned after real, repeated hardware crashes (2026-09-06).** Bench-tested a settle-wait fix for the direction-at-reversal race (wait past `RMT_IDLE_THRESHOLD_TICKS` after a stop before trusting a drain, instead of draining immediately) - the device hard-crashed (`Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)`, `Core 1 panic'ed (Unhandled debug exception)`) on the very next sweep, right after an `RMT RX BUFFER FULL` error. Methodically isolated every variable in turn, re-testing on real hardware after each change:
    - Raising `mem_block_num` (RMT's on-chip memory, to survive a long continuous move without an idle gap) made things *worse*, not better, on two different channels: channel 7 (memory blocks are claimed *upward* from a channel's own index, so `mem_block_num=4` on channel 7 silently requested blocks 7-10, which don't exist - result: `encoderCount` stuck at 0 for a full sweep, no error reported) and channel 4 (in-range, but still hard-crashed in the same overflow scenario it was meant to fix).
    - Reverting `mem_block_num`/channel back to original while keeping the settle-wait still crashed.
    - Disabling WiFi entirely (to test whether its Core-0-pinned background activity was starving the RMT ISR) made no difference - still crashed, ruling that out cleanly.
    - Reverting the settle-wait itself, back to byte-for-byte the same `encoder_diag.cpp`/`main.cpp` logic as the very first sweep of the day (which hadn't crashed) - **still crashed**, on the identical code. This was the key result: the crash isn't tied to any single knob tested (RMT channel, `mem_block_num`, `idle_threshold`, the settle-wait, WiFi on/off) - it's an intermittent, probabilistic instability in the legacy RMT RX driver's overflow-recovery path itself, and the one clean run at the start of the day was luck, not a stable baseline. 5 of 6 sweeps run this session crashed.
    - Decided with the user to abandon RMT for this entirely rather than keep patching an approach with a demonstrated, unexplained crash risk.

14. **Current (6th implementation): hardware timer poll, both channels, in software** (`encoder_handler.cpp`, rewritten). A `hw_timer_t` ISR (`onEncoderTimer()`, `IRAM_ATTR`) fires at `ENCODER_POLL_HZ` (4kHz - ~20x oversampling over the <200Hz real edge rate this project's own bench data implies), reads both channels via `gpio_get_level()`, and decodes X4 quadrature via the standard state-transition lookup table (`QUAD_TABLE`). No RMT, no ring buffer, no dependency on FreeRTOS task scheduling at all - just a periodic ISR, installed from `core0Task()` (keeping it Core-0-affine the same way `initEncoder()` always has been). `getMissedTransitionCount()` is now a *real* diagnostic (not RMT's backlog heuristic): it counts genuine skipped transitions - a poll landing on a state where both quadrature bits changed since the last sample, which a valid signal can't do in one real step. Direction is genuinely measured again (not inferred from the stepper's commanded direction, as the RMT version was reduced to) - this directly resolves item 11's open "does this generalize to real tracking-mode tuning" question, since direction no longer depends on there being an idle checkpoint at all.
    - **Bench-verified clean (2026-09-06)**: full homing + 64-leg `$ENCDIAG` sweep (all 4 frequencies) completed end-to-end with zero crashes, `missedTotal` staying at 0 for the entire sweep, and drift across all 64 legs totaling ~23 counts (1563 down to 1540) - tiny, monotonic, consistent with real mechanical backlash, not a counting error. `core0MaxGapMs`/`core0MinStack` stayed at their healthy boot-time values throughout, confirming item 12's finding held under the new implementation too.
- [x] **Done, extensively** - the entire 2026-09-07 PID tuning session used the encoder as ground truth against `curPos` this exact way (see "Tuning requires encoder ground truth" and every subsequent verified sweep).

## Implemented: display max travel-time (end-to-end speed) in the UI, from real homing data

Raised while tuning: the prop's motion will ultimately be timed to music, so the person sequencing it needs to know the device's real physical speed limit (how fast it can actually move between two positions) to keep their cues within what's physically achievable - not just "seems fast enough on the bench."

Implemented by timing the `HOMING_FIND_OTHER_END` leg - the continuous run from the initial switch trip (position 0) to the second trip (`endPosition`, i.e. `2*bottomPosition`), the "full down-and-up round trip" `homingOverallTimeoutMs`'s own reference point is based on (`stepper_handler.cpp`). `otherEndSearchStartMs` is stamped when `HOMING_SETTLE`'s `SETTLE_THEN_FIND_OTHER_END` case issues the `runForward()` call; elapsed time is computed against it the moment the second trip is detected, alongside the already-existing `endPosition` distance. Result stored in `homingTravelSteps`/`homingTravelMs`/`homingTravelValid` (`stepper_handler.h`/`.cpp`) - persists across homing attempts (not cleared at `startHoming()`) so the UI keeps showing the last known figure; `homingTravelValid` is only false before the very first successful homing since boot.

**Fixed (2026-09-05, user-reported)**: the first version reported that whole leg's totals directly - the full down-and-up round trip, not the one-way 0-100% figure actually wanted for timing cues. Fixed by halving both `endPosition` and the elapsed time before storing them. Justified, not just a convenient guess: this leg is one continuous, unbroken `runForward()` at constant commanded speed - the trolley's direction reverses at its physical midpoint only as a passive consequence of the rope re-wrapping on the pulley, not because the stepper itself stops, decelerates, or changes speed there - so distance and elapsed time both genuinely split evenly at that midpoint.

Surfaced in three places: a "One-way (0-100%) travel: N steps in M ms (~X steps/s average)" log line the moment it's measured, a "Full Travel" row on the Status tab (web UI, both the initial server-templated page load and the live `/status-data` poll), and the serial `s` full status report's Stepper Status section.

- [x] **Closed out** (user, 2026-09-07) - exercised by many subsequent real homing cycles.
- [ ] `$CHECKSTEPS` only catches step loss in the *overshoot* direction (switch fires early) - it can't distinguish "no drift" from "drift the other way" (steps lost such that the trolley undershoots and never reaches the switch at commanded position 0). Worth considering a second check toward the far end (`bottomPosition * 2`) if undershoot-direction loss turns out to matter in practice.

### Looking ahead (raised while building the harness, not yet started)

- [x] **Dropped by the user, 2026-09-07** - actual direction taken was the opposite (richer external Python tooling, `ddp_continuous_test.py`/`analyze_ripple.py`), not on-device automation.
- [x] **Done** - `TRACK_MODE_PID` exists, is extensively tuned, and is the mode under active use.

## Fixed: re-homing silently didn't move, after the trolley drifted off the switch during normal operation

Reported: trolley returns to "zero" after being in motion for a while but sits offset from the switch; commanding a re-home logs "Moving to find initial homing position..." (`HOMING_CHECK_SWITCH`'s else-branch) but the motor never actually moves. Any unrelated manual move (e.g. jogging 100 steps) "recovers" it, and re-homing works normally afterward.

Root cause, found by code inspection (not log evidence - `log2.log` turned out to be another Compact Motion Log CSV capture, not the serial text from this incident): `pendingForceStop` (set by the homing-switch ISR, see `handleHomingInterrupt`) was only ever consumed inside `updateHoming()`, and that function returned early, before ever reaching the check, whenever homing wasn't currently active. So if the switch ever tripped during *normal* (non-homing) operation - plausible after extended travel, if the trolley drifts near the switch zone - the flag got set and then sat there unconsumed indefinitely, since nothing looked at it while idle. The *next* time homing started, that stale flag was treated as "the switch just tripped right now," triggering a `forceStop()` in the same `loop()` iteration as `HOMING_CHECK_SWITCH`'s first `runBackward()` call for `HOMING_FIND_INITIAL` - racing against it in FastAccelStepper's own internal ramp-generator state and, per what was reported, sometimes silently swallowing that first move entirely. An unrelated manual move (which goes through a different call path) happened to reset enough internal state to unstick it, which is why jogging "recovered" it.

Fixed two ways in `stepper_handler.cpp`:
1. `pendingForceStop` is now checked and cleared unconditionally at the very top of `updateHoming()`, before the "not homing" early return - never leaves it stale across a homing/non-homing boundary.
2. A trip while *not* homing is no longer silently discarded either: it now force-stops (as it always did) and additionally marks the system `homed = false`, logging a warning - since the trolley reaching the switch outside of homing means something has genuinely drifted, and continuing to trust the old homed position would be wrong.
3. `startHoming()` now also defensively clears `pendingForceStop` itself, belt-and-suspenders against any other path that might leave it stale.

- [x] **Closed out** (user, 2026-09-07) - bench verification is considered adequate at this point given extensive subsequent real-world exercise.
- [x] **Root-caused later** - the 2026-09-07 PID session found and fixed the actual mechanism (a control-loop containment gap letting the trolley run unsupervised past a boundary during a blind window). See that session's "real bug found and fixed" section.

## Root-caused: tracking jerkiness, and three motion strategies added to compare on the bench

Follow-up to the "loses steps / drives into the end stop" report below - a compact CSV log (`compactLogEnabled`, see below) was captured across four real pans ("Fast" 2.5s, "Middle" 10s, "Very slow" 20s, "Kinda fast" 6s) and analyzed. Full log in `log.log` at the repo root (kept for reference; not meant to stay there long-term - it's a one-off capture, not something the build depends on).

**Findings from the real data (not simulation) - a script parsed all four sections:**
- No negative `curPos` and no `curPos` exceeding `bottomPosition` anywhere in this particular capture, and the `stepperTrackMaxLagConfig` safety net never had to fire incorrectly - the "buzzes against the end stop" symptom wasn't reproduced in this specific log (confirmed with the user), so that remains a separate, not-yet-reproduced-on-demand issue.
- **Root cause of the jerkiness**: `FastAccelStepper::moveTo()` always plans to decelerate to a full stop at whatever target it's given. Since tracking-mode targets are only ~60-120 steps apart, the motor is almost always within its own stopping distance of the current target - so it's constantly "in the process of stopping," never truly cruising, regardless of accel/speed tuning. Direct evidence: a stretch in the "Very slow" section (ms 305475-305850) where the commanded position moves in one constant direction with a constant ~60-step delta every packet - no curve easing, no reversal - yet actual speed (`curSpeedHz`) decelerates continuously from 6498 Hz to 100 Hz over ~10 packets. In that section, 87% of tracking-mode rows ran under 30% of the configured target speed.
- The "slows down at the ends when it shouldn't" observation is a related but distinct effect: it coincides with direction *reversals* (physically unavoidable - the motor has to pass through zero speed to reverse), which in a back-and-forth test pattern happen to sit at the rail's physical extremes. It isn't really "the ends" causing it; it's "every reversal," which just happens to align with the ends in this test.

**Simulation, and its limits**: before implementing, replayed the real command sequences from `log.log` through a simplified trapezoidal-motion physics model comparing three candidate strategies. A fidelity check against real logged `curSpeed` data found the model does **not** precisely reproduce the real device's deceleration curve, so its absolute numbers (jerk/near-stall/error metrics) shouldn't be trusted as precise predictions. What the simulation *did* usefully flag, even with an uncalibrated model: a naive velocity-streaming approach overshot past `[0, bottomPosition]` in a couple of sections - a real structural risk (rate-based control has no built-in "never overshoot" guarantee the way `moveTo()` does), not a fidelity artifact. That risk is exactly why the streaming mode below has a hard position clamp built in from the start rather than being added reactively.

**Implemented - `StepperTrackMode` (`stepper_handler.h`), selectable per the user's request to A/B test on real hardware rather than pick one blind:**
- **Direct** (mode 0, default - unchanged behavior): `moveTo()` on every DDP packet, as before.
- **Coalesce** (mode 1): batches consecutive small updates and only commits a `moveTo()` once `stepperCoalesceMsConfig` has elapsed or the accumulated delta exceeds `stepperCoalesceStepsConfig`, so each individual move has real distance to accelerate through before it needs to plan a stop.
- **Streaming** (mode 2): while updates keep arriving, runs continuously (`runForward()`/`runBackward()`) at a speed estimated from the recent rate of DDP position change over `stepperStreamRateWindowMsConfig`, instead of aiming to stop at each tiny target; snaps to an exact `moveTo()` once no new command arrives for `stepperStreamSettleMsConfig`. Hard-clamped to `[0, bottomPosition]` every loop iteration (calls `forceStop()` if breached) as the safety net the simulation showed was necessary.

All three still use the same tracking-vs-normal profile decision (`stepperTrackThresholdConfig`/`stepperTrackMaxLagConfig`) - they differ in *how* a chosen target gets committed to the stepper, not in when tracking vs. normal speed/accel applies. Selectable in Settings → Stepper Configuration → Tracking Motion Strategy; persists like the other stepper settings (deferred write while moving, per the earlier crash fix).

- [x] **Closed out** (user, 2026-09-07) - Direct and PID have both since been extensively bench-tested; Coalesce/Streaming remain lower priority (Streaming is shelved) but are no longer tracked as "blocking, untested."
- [ ] Streaming mode's rate estimate and the coalesce parameters (250ms/400 steps, 250ms/150ms defaults) are untuned guesses - expect to need adjustment per-prop like the other tracking parameters.
- [x] **Superseded** - the 2026-09-07 PID session's containment guards and encoder-verified stall detection directly address "driven past a boundary" scenarios with real, repeatable bench evidence, in more depth than this item asked for.

## Compact Motion Log (temporary diagnostic tool)

"Compact Motion Log" checkbox (DDP/LED Status box, `compactLogEnabled` in `protocol_common.h`): prints one CSV line per processed DDP command (Direct/Coalesce modes) or roughly every 20ms while actively streaming (Streaming mode - a different cadence than the other two, worth remembering when comparing logs across modes) - `ms,ddpVal,cmdPos,curPos,delta,lag,profile,curSpeedHz,targetSpeedHz`. Toggle via the checkbox or `GET /compact-log?enable=true/false`. Runtime-only, not persisted. **This is explicitly temporary** - remove it (or fold it into the planned Debug tab) once the tracking-mode comparison above is settled; it shouldn't linger as permanent UI clutter.

## Added: small-move "tracking" profile for smooth slow panning

Reported symptom: commanding small, frequent position changes (e.g. xLights slowly panning a DDP value, observed incrementing by ~1 unit / ~60 steps every 150-300ms) produced visibly jerky motion, not a smooth glide.

Root cause: at the configured Speed/Acceleration (6500 Hz / 20000 steps/s²), the ramp-up distance to reach cruise speed is `speed² / (2 × accel)` ≈ 1056 steps - far more than the ~60-step moves being commanded. Every small move was a tiny triangular spike that never reached cruise speed, and a new target kept arriving before the previous spike finished decelerating, forcing the ramp generator to recompute a fresh spike from whatever nonzero velocity it still had - repeated many times a second. That's the jerk.

First instinct (rejected, correctly, by the user before implementing): just lower the target speed for small moves while keeping the same acceleration. That doesn't work - peak torque demand at the start of a move scales with acceleration alone (steps/s²), not with what speed it's ramping toward. Keeping accel high and only lowering speed shortens the high-torque phase's *duration*, not its *magnitude*, so it doesn't reduce stall/skipped-step risk at all.

Implemented: a genuinely separate, independently-tunable "tracking" profile (`stepperTrack*Config` in `stepper_handler.h`/`.cpp`) - Enabled, Threshold (steps), Speed (Hz), Acceleration (steps/s²), Max Lag (steps, see below) - all exposed in Stepper Configuration.

**Bench-tested and validated** (defaults updated to match): same speed as the normal profile, but acceleration cut to roughly 1/4, threshold 2000 steps. What matters most for avoiding stalls/skipped steps is specifically the lower acceleration - target speed barely matters for small moves since they rarely get anywhere near cruise speed regardless.

Two follow-up bugs found and fixed after the initial implementation:

1. **End-of-travel speed burst.** The tracking-vs-normal decision originally measured move size as the delta between the new target and the stepper's *actual current position*. Near a reversal (e.g. a triangle wave), the stepper is still physically travelling in the old direction while DDP starts commanding the new direction, so that gap balloons even though each individual DDP increment is still small - pushing the decision into the fast/aggressive normal profile at exactly the moment the gentlest motion is needed, producing a jarring burst of speed/torque right at the ends of travel. Fixed: the decision now measures the delta between the new target and the *previous commanded target* instead, which stays small through a reversal since DDP's increment size doesn't spike there. Added `stepperTrackMaxLagConfig` as a separate, much larger safety net (default 3000 steps) - if the stepper's actual position ever falls genuinely far behind the commanded target (not just a momentary reversal artifact), it still falls back to the normal profile to resync rather than drift indefinitely.
2. **Reboot when changing tracking settings while the motor was moving** - same crash class as the earlier homing-ISR fix, but far more reproducible: `handleSaveStepper()` did 8-10 synchronous `Preferences.putX()` calls, and FastAccelStepper's step-generation interrupt fires continuously while the motor is stepping (vs. the homing switch's rare single edge), making a hit against a flash-cache-disabled window near-certain rather than occasional. Fixed by deferring the actual flash write: the save handler now updates the RAM config variables immediately (live behavior changes right away) and sets `stepperSettingsPendingSave`; a new `persistStepperSettingsIfPending()`, called every `loop()` iteration, only performs the actual `Preferences.putX()` calls once `stepper->isRunning()` is false. Also stopped `handleSaveStepper()` from unconditionally calling `setSpeedInHz()`/`setAcceleration()` with the normal profile on every save - that would have force-applied it mid-move even while the stepper was legitimately cruising in tracking mode, which is itself a jerk risk.

Also fixed as part of the initial implementation: `/move` and `/set-position` (manual web UI control) now explicitly reset to the normal Speed/Acceleration before moving, since without that they'd silently inherit whatever profile DDP's tracking logic last left the stepper in.

**This pattern (any Preferences save while a continuously-firing interrupt is active can crash) likely also affects other settings-save handlers** that don't currently guard against it - `/save-tmc`, `/save-led`, `/save-protocol`, `/save-wifi`, `/save-ap` all still do synchronous flash writes with no "is anything currently interrupt-heavy" check. `/save-tmc` has an `isHoming()` guard but not an `isRunning()` one, so it's still exposed to this exact scenario (saving TMC settings while the stepper is just normally moving, not homing). Worth a broader pass applying the same deferred-write pattern more systematically, rather than fixing each handler reactively as it's reported.

- [ ] Not yet done: surfacing which profile was used per-move anywhere in the UI (currently only visible via serial with Debug enabled, printed as `[tracking]`/`[normal]`) - could be a nice fit for the planned Debug tab.

## Fixed: DDP reception was silently broken on any device with pre-ArtNet-removal history

Reported as "I'm sending DDP data and it's being ignored." Two separate bugs found and fixed:

1. **Stale NVS protocol value.** Before ArtNet was removed, `protocolType` was `NONE=0, ARTNET=1, DDP=2`. After removal it's `NONE=0, DDP=1`. `loop()` only calls `handleDDP()` when `protocolConfig == PROTOCOL_DDP`. A device previously flashed with the old firmware and saved as DDP(2) - NVS survives a normal reflash - loads that stale `2` under the new enum, which no longer equals `PROTOCOL_DDP` (1), and `handleDDP()` silently never runs. The UDP socket stays open and receives packets fine; they just never get read. This bug was introduced by the ArtNet removal itself (an earlier session, before this session's work began) and only bites a device with that history - not something introduced this session, but only now discovered because live DDP hadn't been tested yet. Fixed in `main.cpp` `setup()`: any stored value other than an explicit `PROTOCOL_NONE` is now treated as `PROTOCOL_DDP`, since DDP is the only protocol this firmware supports; logs a note when it corrects a stale value.
2. **Unbounded stack buffer sized from untrusted packet data** (flagged separately, worth fixing regardless of whether it was the cause of the reported symptom): `ddp_handler.cpp`'s `handleDDP()` declared `uint8_t channelData[header.dataLen]` - a stack VLA sized directly from the packet's own header field, which is untrusted network input up to 65535. A malformed or truncated packet claiming a large `dataLen` while sending little or no actual data would blow the stack - a crash triggerable by any non-conforming packet, not just a display bug. Fixed: `channelData` is now a `static` buffer sized from a new `DDP_MAX_DATA_SIZE` (1472 bytes, standard MTU-safe UDP payload), and the actual read length is derived from `packetSize` (what was really received) rather than the packet's own claimed `dataLen`, which is now only used for a debug-mode mismatch warning.
   - Not further hardened this pass: DDP's `flags`/`dataType`/`destination` header fields aren't validated at all (this implementation always treats every packet as an immediate RGB push, regardless of what those fields claim) - a completeness gap, not a crash risk, so left alone for now. Worth a closer look if a stricter/more spec-compliant DDP implementation is ever wanted.

## UI/Settings redesign (requested, not yet implemented)

- [ ] **Settings tab**: remove the Stepper Control section - it's redundant with the equivalent info/controls already on the Status page.
- [ ] **WiFi settings**: add a mode selector - Client only, AP fallback (if not connected within 30s, fall back to AP mode - closest to today's actual behavior, just needs to be an explicit named choice rather than implicit), or AP mode only.
  - [ ] Related, reported separately: the boot-time client connection attempt doesn't seem to try hard enough before giving up. Confirmed in code - `wifi_handler.cpp`'s `WIFI_TIMEOUT` is hardcoded to 10 seconds, and `connectToWifi()` makes exactly one `WiFi.begin()` attempt with no retry; on timeout `setup()` falls straight to AP mode. 10s can be tight for a real router (WPA handshake + DHCP lease, especially on a busy network), so a device that would connect fine given a bit longer instead lands in AP mode every boot. Implementing the AP-fallback mode above with an explicit (and probably longer, e.g. the requested 30s) timeout should directly address this - worth doing both together rather than separately.
- [ ] **Stepper Configuration**: remove the help text paragraph above the Homing Acceleration field.
- [ ] **Stepper Configuration**: fold the TMC2209 driver settings panel into the Stepper Configuration panel, under a "TMC2209 Config" collapsible dropdown. Remove the existing "Advanced" collapsible inside the TMC panel - just show those fields directly whenever the TMC2209 Config dropdown itself is open, no second level of nesting.
- [ ] **Settings tab**: fold the Channel Configuration and LED Configuration panels together into one.
- [ ] **New "Debug" top-level tab**: show the serial console output live in the browser, and move the "Enable Serial Debug Output" checkbox there (currently in Channel Configuration). Feasible - the natural approach is a ring buffer in `main.cpp` that every `Serial.print`/`println` call also writes into (or a thin wrapper function replacing direct `Serial.print` calls where that's practical), served via a new endpoint the Debug tab polls (same pattern as `/status-data`) rather than a persistent WebSocket, to stay consistent with how the rest of this UI already works. Needs a design pass before implementing: how much history to buffer (memory is limited - 327KB RAM total), whether to intercept the `Serial` object globally (e.g. a thin wrapper class) vs. only capturing specific calls, and whether serial-over-USB output should also mirror to the ring buffer or the two become independent.

## Smaller additions (previous session)

- **LED test pattern**: Status tab checkbox drives a marching R/G/B pattern out the pixel strip and ignores incoming DDP pixel data while enabled, for bench-testing pixel wiring without a controller. Scoped to LED data only (stepper DDP control unaffected). Runtime-only, not persisted.
- **Serial command key swap**: `s` and `n` swapped meaning. `s` is now the full/detailed status report (formerly `printNetworkDiagnostics()`, renamed `printFullStatus()`, header now prints `=== Status ===` instead of `=== Network Diagnostics ===`); `n` is now the brief connection status (formerly bound to `s`, `printStatus()` unchanged internally). Done because the old detailed-diagnostics output was getting long and "network diagnostics" undersold what it now also covers (LED, TMC2209, uptime, etc.) - "status" is the more accurate name and better claims the more-used `s` key.

## Fixed/done this session

- [x] **DDP LED channel-offset bug** — `ddp_handler.cpp` used byte offset `3` for where LED data starts; the documented channel layout (and the now-removed ArtNet handler) used byte offset `2` (channel 3). This meant LED colors landed one byte later — and therefore looked different/shifted — than intended. Now uses offset `2`.
- [x] **`totalChannels` status miscalculation** — `html_handler.cpp`'s status JSON added a stray `+1` to the reserved-channels + LED-channels count. Removed; it now reports `2 + ledPixelCount*3`.
- [x] **Docs**: CLAUDE.md's pin table said D9 for WS2812 data; code has always used D4. Corrected to match the code. *(Confirm against the actual board/schematic if you get a chance — the doc was fixed to match firmware, not the other way around.)*
- [x] **ArtNet support removed entirely.** Decision: DDP already scales to the 500-pixel goal for free via its fragmented-packet handling (`header.dataOffset` spans multiple UDP packets with no universe-style ceiling), while ArtNet would have needed real work — subscribing N consecutive universes, per-universe pixel-offset math, UI for universe count — to reach the same target, and every prop in xLights can already have its own protocol per output, so keeping this device DDP-only doesn't constrain the rest of the show. Not worth the ongoing test/debug surface for a capability DDP already has.
  - Removed `artnet_handler.h`/`.cpp`, the `hideakitai/ArtNet` lib dependency, `PROTOCOL_ARTNET` from `protocolType`, the protocol `<select>` and ArtNet config fields from the web UI (settings tab is now "Channel Configuration", DDP-only), and the `artnetChannelsPerUniverseConfig` dead setting.
  - `protocolConfig`/`protocolType` (now just `PROTOCOL_NONE`/`PROTOCOL_DDP`) was left in place as cheap scaffolding in case another protocol (e.g. sACN/E1.31) is ever wanted — but nothing currently exposes a way to pick anything other than DDP.
  - If you ever do want ArtNet or sACN back (e.g. to match other controllers in the show), the multi-universe design notes from the previous version of this doc are in git history (see the commit that introduced this TODO.md) — worth a re-read rather than starting from scratch, since the offset math is the same problem DDP's fragmented handler already solves.

## Priority 1 — Split stepper/protocol handling from LED output across both cores (design worked through 2026-09-05, first step implemented)

This is the reason the project moved from the C3 to the S3 in the first place, and it's never actually been implemented — everything (WiFi, web server, DDP receive, stepper dispatch, `FastLED.show()`) still runs serially in one `loop()` on Core 1. See CLAUDE.md's Architecture section.

**The original justification above turned out to be outdated once actually checked against the vendored library source** (not a big deal - the plan below still gets built, just for a more precise reason):
- **Correction (2026-09-06)**: this originally claimed `FastLED.show()` is mostly non-blocking here via FastLED's async IDF5 RMT5 driver (`drawAsync()`/`led_strip_refresh_async()`), based on reading the source's `#if FASTLED_RMT5` selection logic. That was wrong for the *actual build* - checking the compiled object files directly (while investigating the encoder's own RMT options - see the encoder section) showed `idf5_rmt.cpp.o` is a near-empty 1228-byte stub (the guard evaluates false at compile time in this toolchain) while `idf4_rmt.cpp.o` is a real 385KB compiled unit. **FastLED is actually using the legacy, more blocking-style IDF4 RMT driver in this build.** So the original premise - `FastLED.show()` blocks its calling core for the WS2812 transmission time (~30µs/pixel) - stands after all, for a more mundane reason (this toolchain's bundled arduino-esp32 framework doesn't expose the modern async driver, not anything about how DDP calls it). `updatePixelLedsFragmented()` still calls `FastLED.show()` **once per received UDP fragment**, not once per logical frame (DDP's Push flag, parsed into `DDPHeader.flags` but never inspected - see the DDP Push-flag item below), which only compounds this.
- More importantly, checked what hardware peripheral **FastAccelStepper** actually uses on this chip, since that's what the C3→S3 migration was really trying to fix. `stepperConnectToPin()` is called with no explicit driver type (`stepper_handler.cpp`), which defaults to `DRIVER_DONT_CARE` - the library's allocator (`esp32_queue.cpp:431-445`) tries **MCPWM+PCNT first**, falling back to RMT only if unavailable. Checking FastAccelStepper's per-chip config (`pd_config_idf5.h`): on **ESP32-C3/C6** (the original prototype), `SUPPORT_ESP32_MCPWM_PCNT` isn't even defined - the stepper is **forced onto RMT**, the same peripheral FastLED uses for WS2812 - a real, literal hardware conflict on a single core, almost certainly the actual mechanism behind "couldn't reliably run stepper and WS2812 together" that motivated the S3 move. On **ESP32-S3**, both peripherals are available (4 queues each) and `DONT_CARE` lands the stepper on **MCPWM+PCNT** - a completely separate peripheral from FastLED's RMT. So the peripheral-level conflict that made the C3/C6 unworkable may already be gone on the S3 just from the chip swap + default driver selection, independent of ever writing this split.
- FastAccelStepper also runs its own background FreeRTOS task (`StepperTask`, `esp32_queue.cpp:451`, doing ramp/queue-refill housekeeping - the actual pulse output is hardware/MCPWM-driven and independent of it) and has a first-class API for pinning it to a core: `engine.init(cpu_core)` (`FastAccelStepperEngine.h:65-70`). Today's code calls plain `engine.init()` - unpinned, left to the scheduler.

**Actual goal, per discussion with the user**: not "keep DDP reception responsive during a blocking call" (the original framing), but explicit, deliberate resource allocation across the two cores so nothing - DDP receive/parse, the web server + status AJAX, stepper pulse dispatch, the end-stop ISR, TMC UART traffic, or LED output - can stall or delay anything else badly enough to lose data or trip the watchdog, given the real load (a few hundred LEDs at 25-40 FPS, stepper at 6500Hz).

**Design settled on:**
| Core | Owns |
|---|---|
| Core 1 (unchanged home of `loop()`) | DDP receive/parse, web server + AJAX status, stepper move dispatch, end-stop ISR, TMC UART, serial commands |
| Core 1, but now *explicit* | FastAccelStepper's `StepperTask`, pinned via `engine.init(1)` instead of today's unpinned `engine.init()` - deterministic instead of left to the scheduler, and guaranteed not to compete with the new Core 0 task |
| Core 0 (new) | A dedicated FreeRTOS task owning FastLED end-to-end - `initPixelLeds()`'s `FastLED.addLeds()` call moves here too (not just the `show()` trigger), so the RMT completion interrupt is Core-0-affine, not just the call site - otherwise only half the work actually moves |

DDP reception doesn't need its own task: packets already land in a lwIP-managed socket buffer (lwIP's own task is Core-0-affine by ESP-IDF default already, independent of this project's code) - the risk is only "`loop()` doesn't get back to draining it promptly if something else on Core 1 stalls," and today's biggest stall risk on Core 1 *is* FastLED's RMT work, which this split removes.

- Signal: a binary semaphore. Core 1's pixel-write call sites (`updatePixelLedsFragmented()`/`blankPixelLeds()`/`updateLedTestMode()`) call `xSemaphoreGive()` instead of `FastLED.show()` directly; Core 0's task blocks on `xSemaphoreTake(portMAX_DELAY)` and calls `show()` when woken. A give() while already pending is a no-op, so bursts of DDP fragments between wake-ups naturally coalesce to one `show()` of the latest state.
- Buffer: single shared `leds[]`, no double-buffer for the first cut - Core 0 reads while Core 1 writes, so a handful of pixels could show a transitional color for one frame during a write that overlaps a transmission; self-corrects next frame. Only worth real double-buffering if bench testing shows this is actually visible, not preemptively - see the earlier "watch for tearing" note this replaces.
- Watchdog: register the new task (`esp_task_wdt_add()`), reset it each iteration - closes the exact gap the old text below already flagged. Related finding while checking this: FastAccelStepper's own `StepperTask` isn't watchdog-registered either, and its one `esp_task_wdt_reset()` call is compiled out under IDF5 (`esp32_queue.cpp:455`, `#if ESP_IDF_VERSION_MAJOR == 4`) - flagged, not fixed, unless revisited.
- Priority: low (e.g. 1) for the new Core 0 task - it spends nearly all its time blocked on the semaphore, shouldn't need to preempt WiFi's own Core 0 tasks.
- TMC UART polling deliberately stays on Core 1 for this first cut, not moved preemptively - revisit only if bench testing shows it's a real stall source.

**Related, deliberately out of scope**: DDP's Timecode flag (forward-scheduled display via NTP-synced clocks) isn't worth building - the actual sender here is Falcon Player (FPP), which the user doesn't believe uses it, and this device has no NTP client. FPP's own native show-sync protocol (expects show data stored locally, syncs playback over the network) was also considered and ruled out - no SD card/memory budget for local show storage on this device. The DDP **Push flag** (marks "last fragment of a frame, display now") is a separate, smaller, still-open item - see below.

**Implemented so far (2026-09-05)**, scoped to what the current stepper-tuning priority actually needs rather than building the whole design at once:
- `engine.init(1)` - FastAccelStepper's `StepperTask` now explicitly pinned to Core 1 (`stepper_handler.cpp`'s `initializeStepper()`), instead of the previous unpinned `engine.init()`.
- New `core0_task.h`/`.cpp` - a real, watchdog-registered FreeRTOS task pinned to Core 0 via `xTaskCreatePinnedToCore()`. Its only job right now is calling `initEncoder()` from within itself, so the encoder's GPIO interrupt ends up Core-0-affine instead of Core-1-affine - directly testing the hypothesis above (the $ENCDIAG sweep's ~10-13-missed-transitions-per-leg result, not scaling with stepper speed, pointed at contention with FastAccelStepper's own Core-1-affine PCNT interrupt). `main.cpp`'s `setup()` now calls `startCore0Task()` instead of `initEncoder()` directly.
- FastLED's `addLeds()`/`show()` work deliberately **not** moved to this task yet - LEDs are disabled (pixel count 0) during this tuning phase anyway (see the encoder section's note on freeing RMT), so there's nothing to move yet. When that's revisited, its logic belongs in this same Core 0 task's loop (currently just an idle watchdog-reset loop), fed by the semaphore design above, not a second task.
- [x] **Closed out** (user, 2026-09-07) - bench verification considered adequate; the encoder has since been relied on extensively across the whole 2026-09-07 PID session with no indication of a missed-transition problem.
- [ ] The rest of the design (Push-flag-aware frame assembly, the LED semaphore/task, double-buffering if tearing turns out to matter) is still just design, not code.

## DDP Push flag is parsed but never checked - causes partial-frame display on multi-fragment updates (raised 2026-09-05, not yet implemented)

`ddp_handler.h` only defines the base 10-byte header (`DDP_HEADER_SIZE 10`, no Timecode support) and `DDPHeader.flags` is populated but never inspected in `ddp_handler.cpp` - every packet is treated as "display immediately," including the Push flag. A large frame (300-500px) arrives as multiple UDP fragments, and `updatePixelLedsFragmented()` calls `FastLED.show()` after **every** fragment, not once per logical frame - so the strip briefly shows a half-updated frame on every multi-fragment update, independent of anything about cores. Fixing this (accumulate fragments into `leds[]` without showing, only signal a show on the fragment with Push set, plus a short no-more-fragments fallback timeout for a sender that doesn't set it) would both fix that correctness gap and reduce `show()`-call frequency, complementing the Priority 1 split above.

- [ ] Not yet confirmed whether FPP actually sets Push on the final fragment of a multi-packet frame (the user believes it likely does, standard practice, but hasn't captured real traffic to confirm). `DDPDebugger` (a JavaFX tool in this repo - its Receiver tab has a live "Flags seen (push / query / reply / storage / timecode)" counter and a per-packet Flags column built for exactly this) is the way to check - point a duplicate/temporary FPP output at the machine running it and watch the counts while FPP plays whatever sequence drives this prop.
- [ ] Not yet implemented pending that confirmation.

## Priority 2 — Validate WiFi actually holds up at scale

This is explicitly an open question, not an assumption. Before investing further in Priority 1, worth doing a cheap real-world test:
- Point xLights at the device with a ~150-pixel and a ~500-pixel test model at a normal show frame rate and watch for dropped frames, visible stutter in position moves, or flicker in the pixel output, especially on a busy WiFi network (i.e. actual show night conditions, not a quiet bench).
- Check whether `WiFiUDP`'s default receive buffering is enough at higher sustained packet rates, or whether packets get dropped silently before `ddpUdp.parsePacket()` even sees them.
- This will tell you whether Priority 1 is sufficient, or whether frame-rate/pixel-count guidance needs to be added to the docs (e.g. "500 pixels works but cap update rate to X Hz over WiFi").

## Homing speed settings weren't persisting - NVS 15-char key limit, and a pre-existing instance found in the same audit

Reported: Homing Speed setting reverted after every reboot. Cause: ESP32's NVS (what `Preferences` writes to) silently caps key names at 15 characters - a longer key fails to write with no obvious error, so the value only ever lived in RAM for that boot and reverted to the `getInt()` default on the next one. The new keys `"stepperSpeedHoming"`/`"stepperAccelHoming"` were 18 characters each. Renamed to `"stepSpeedHome"`/`"stepAccelHome"` (13 chars) in both `main.cpp` (load) and `html_handler.cpp` (save).

While fixing it, audited every `Preferences` key in the codebase for the same limit (`grep` + `awk` sorted by length) and found one more, **pre-existing, unrelated to this session**: `"stepperBlankTime"` is 16 characters - the "Stepper Blank Time" setting (Channel Configuration) has silently never persisted across a reboot since it was originally written. Renamed to `"stepBlankTime"` (13 chars). After both fixes, the longest key anywhere in the codebase is exactly 15 characters (`tmcStallEnabled`), which is within the limit - confirmed no other violations remain.

**Worth remembering for any future `Preferences` key added to this codebase: keep it to 15 characters or fewer, and there's no compiler or runtime error to catch a violation - it just silently doesn't persist.**

## Fixed a regression the ISR crash fix introduced into homing itself

The `handleHomingInterrupt`/`updateHoming` crash fix (above) accidentally changed `forceStop()` from edge-triggered (called exactly once, synchronously, the instant the switch trips - the original ISR behavior) to level-triggered (called on *every* `loop()` iteration for as long as `interruptTriggered` stayed `true`). `HOMING_MOVE_OFF_FORWARD`/`HOMING_MOVE_OFF_BACKWARD` never touch `interruptTriggered` at all, so a single stray edge during the initial move-off-the-switch phase (plausible on a mechanical switch as it releases) left the flag latched. The very next state (`HOMING_WAIT_CLEAR_SWITCH`) then had its 2500-step "get clear" move force-stopped almost immediately by that stale flag, read the premature stop as "move finished," and pressed on into the search from right back at the switch boundary - re-tripping and re-stopping in a tight loop near the original position instead of actually traveling to search for the other end. Externally this looked like "moves off the switch, then never moves again, times out."

Fixed by splitting the ISR's signal into two flags: `pendingForceStop` (consumed and cleared the instant `updateHoming()` acts on it, restoring the original "exactly once per edge" behavior) and the existing `interruptTriggered` (left exactly as before - a level flag only the specific `HOMING_*` state waiting for that edge clears, on its own schedule). `stepper_handler.cpp`.

## Homing speed is now independently configurable (added this session)

Homing speed/acceleration (`stepperSpeedHomingConfig`/`stepperAccelHomingConfig`, in Stepper Configuration) used to be hardcoded (`stepperSpeedHoming`/`stepperAccelHoming`, 6000 Hz / 1000000 steps/s²) and only ever tuned against 16 microsteps. Found when dropping to 4 microsteps to speed up movement made homing 4x physically faster for the same Hz - fast enough that the motor slammed past the switch and stalled/buzzed against the mechanical end-stop before it could stop in time. Homing speed is a step-pulse rate, not a physical velocity, so it doesn't automatically scale with microstep resolution; deliberately made this a manual setting rather than auto-deriving it from `tmcMicrostepsConfig`, since `stepper_handler` has no dependency on `tmc_handler` (and shouldn't - TMC UART control is optional/independent) and homing needs to work correctly whether or not UART control is even enabled. Re-tune Homing Speed/Acceleration whenever the microstep setting changes.

While touching every `stepperAccelHoming`/`stepperSpeedHoming` call site, also fixed a small pre-existing bug: several homing-error/timeout recovery paths (and the `f`/`b` serial commands) reset speed/acceleration to the hardcoded defaults (`stepperAccel`/`stepperSpeed`) instead of the user's actually-configured values (`stepperAccelConfig`/`stepperSpeedConfig`) - meaning a custom Stepper Speed silently reverted to the firmware default after any homing error. Now uses the Config variants throughout.

## TMC2209 UART driver control (added this session)

Added `tmc_handler` (new module, `teemuatlut/TMCStepper` dependency) using the driver's UART link — wired to D6 (TX) / D7 (RX), separate from the STEP/DIR/EN pins FastAccelStepper drives. Disabled by default (`tmcEnabled` preference); enable it in Settings once the wiring is confirmed. Covers:
- Digital run/hold current control (replaces the board's Vref trimpot once enabled)
- StealthChop/SpreadCycle chopper mode toggle (under Advanced in the UI)
- A firmware-side stall-detection safety cutoff, polling live `SG_RESULT` and force-stopping the motor if it stays below a configured threshold while moving — deliberately *not* using the chip's internal SGTHRS/DIAG-pin comparator, since DIAG isn't wired; this is a simpler, firmware-side comparison so the live value is directly visible on the Status tab while tuning
- Diagnostics (over-temp, short-to-ground, open-load, UART CRC errors) surfaced on both the Status tab and the `s` serial command (was `n` - see the serial-command key swap noted later in this file)

**Bring-up findings (bench-tested this session):**
- [x] MS1/MS2 confirmed grounded on the actual board → UART address 0 matches the firmware default. Link connects.
- [x] **TMCStepper register-shadow trap, hit twice**: the library caches each register in RAM and rewrites the *whole* register on any write to it; any field never explicitly touched stays at its C++ default of 0 forever, regardless of the chip's own power-on-reset default for that field. Found and fixed two instances - `CHOPCONF.toff` (0 = driver output stage fully disabled, motor didn't move at all) and `CHOPCONF.mres` (0 = 256 microsteps, motor moved at ~1/16th intended speed after the toff fix). Also preemptively fixed `PWMCONF.pwm_reg`/`pwm_lim`/`pwm_autograd`, which would have silently crippled StealthChop's autotuning the moment it was selected, even though SpreadCycle was in use when this was found. **Any future field added to a TMC register write must be checked against this same trap** - cross-reference against `TMC2208_bitfields.h`'s struct layout before assuming an untouched field is safe at its hardware default.
- [x] `CHOPCONF.hstrt`/`hend` (SpreadCycle hysteresis) and microstep resolution are now exposed as Settings fields (under Advanced) instead of hardcoded, so they can be tuned on the bench without a rebuild - defaults unchanged (hstrt=0, hend=0, microsteps=16). Still worth listening for excess coil noise/ripple in SpreadCycle and adjusting hstrt/hend if so.
- [ ] Changing Microsteps per Full Step requires a re-home afterward (bottomPosition is measured in actual steps, so it self-corrects, but the previously-tuned Stepper Speed (Hz) will feel like a different physical speed since distance per step changed) - the UI warns about this on save, but it's easy to miss.
- [x] **Found and fixed a pre-existing crash, not new but newly exposed**: saving TMC settings while homing was in progress caused a reboot - `Guru Meditation Error: Core 1 panic'ed (Cache disabled but cached memory region accessed)`. Root cause: `handleHomingInterrupt()` (the homing-switch ISR, correctly marked `IRAM_ATTR`) called `stepper->forceStop()`, but `FastAccelStepper::forceStop()` itself is a regular (non-IRAM) function living in cached flash. `Preferences.putX()` briefly disables flash cache while it writes, and the TMC settings save does eight of those plus several UART round-trips in one HTTP request - if the switch tripped during that window, the ISR jumped into forceStop()'s flash-cached code while cache was disabled and panicked. This bug predates the TMC work; nothing previously wrote to flash that repeatedly while the homing interrupt was live, so it never got hit. Fixed by making the ISR set only a flag (`interruptTriggered`), with `updateHoming()` (always normal task context) calling the actual `forceStop()` as soon as it notices the flag - `stepper_handler.cpp`. Also added a `409` guard rejecting `/save-tmc` while `isHoming()` is true, both to close the exposure window and because changing microsteps/current mid-search would corrupt the homing math regardless (it assumes constant distance-per-step throughout the search).
  - [ ] Worth auditing whether any *other* code path can call `Preferences.putX()` (or otherwise trigger a flash write) while homing's interrupt is live and unguarded - the fix above only closes the specific TMC path that was actually hit. The interrupt-side fix (deferring `forceStop()` to `updateHoming()`) is the real belt-and-suspenders protection; the `/save-tmc` guard is closing one specific door, not the whole hallway.
  - [ ] Relevant to Priority 1 (splitting protocol/DDP handling and `FastLED.show()` across cores): any future interrupt or cross-core signaling needs the same IRAM-safety scrutiny - don't assume a library function is interrupt-safe just because it's called from inside an `IRAM_ATTR` function; check whether the *callee* is also IRAM-resident.
- [ ] Tune the stall-detection threshold: watch live `SG_RESULT` on the Status tab during normal moves vs. a deliberately blocked/jammed trolley, then pick a threshold with margin, before enabling the cutoff for real use.
- [x] **Resolved decisively, 2026-09-07 - StealthChop removed from the project entirely.** Tried it live at this project's normal operating settings: dramatically insufficient torque, audibly wrong, barely moving, the step counter running to position 95980 during a homing attempt against a real ~15500 travel before the search timed out. Not a subtle resonance edge case worth tuning around. SpreadCycle is now the only chopper mode the firmware supports - see `tmc_handler.h`'s declaration comment.

## TRACK_MODE_PID - closed-loop DDP tracking, no encoder dependency (added this session, 2026-09-06)

Added a fifth tracking mode (`StepperTrackMode` in `stepper_handler.h`) alongside Direct/Coalesce/Streaming/Lookahead: a proper PID controller (`updatePidMode()`, `main.cpp`) reading `positionRequest` directly every `loop()` iteration (no "only reacts to a new DDP value" dispatch - this mode *is* the whole thing) instead of the binary normal/tracking-profile switch every other mode uses. Proportional + integral + derivative, **derivative-on-measurement** (not on error, to avoid a derivative "kick" every time the DDP-commanded target jumps), **conditional anti-windup** (integral only accumulates while the combined output isn't already saturated at the speed cap). New tunables, all live over `$SET`/`$GET` (floats for the three gains - `toInt()` would truncate a gain like 1.5): `pidKp`, `pidKi`, `pidKd`, `pidMaxSpeed`, `pidAccel`, `pidDeadband`. **Explicit design constraint from the user**: must not depend on the rotary encoder - that's a bench-only tuning/evaluation tool, not something intended to ship on every device.

Drives via `runForward()`/`runBackward()` (continuous-run) while outside the deadband, same primitive Streaming mode uses, and settles into an exact `moveTo()` once inside `pidDeadbandConfig` of the target - structurally immune to the older `forceStop()`-then-`targetPos()`-goes-stale bug the other modes still have (see "Real bug found via graph inspection" further down), since PID never trusts `targetPos()` for anything; it always recomputes fresh from `positionRequest` and `getCurrentPosition()` every tick.

**Two real bugs found and fixed getting the first bench test to actually work** (both would have hit Streaming mode too - Streaming was patched to match where cheap to do so, though it's currently deprioritized/shelved):

1. **`setJumpStart()`'s configured burst applies in the wrong direction when issued through `runForward()`/`runBackward()` instead of `moveTo()`.** `jumpStartConfig` (default 20) is a fixed-size kick at full speed meant to overcome static friction at the start of a move - correctly signed for `moveTo()`-based motion (used successfully by Direct/Coalesce/Lookahead and `$CHECKSTEPS` for years), but issuing it through the continuous-run API instead sends that initial burst the *opposite* way. First bench test sent a large positive DDP target from right at the homing switch (position ~0-1, immediately after homing) and the trolley drove *backward* instead, tripping `updateRammedIntoStopCheck()`'s new safety net (working exactly as designed) and force-stopping with `homed=false`. Root-caused by a controlled A/B test: starting the identical PID move from well away from the switch (`$CHECKSTEPS 3000` first) converged perfectly; starting glued to the switch failed every time; `$SET jumpStart 0` before engaging PID fixed it even starting right at the switch, confirming jumpStart as the mechanism, not the PID control law or direction handling (both were correct all along - confirmed independently by the bench encoder, which tracked every failed attempt's real, if small, physical motion in lockstep with the buggy `curPos`). Fixed by explicitly disabling jumpStart (`stepper->setJumpStart(0)`) immediately before every `runForward()`/`runBackward()` call in both PID and Streaming mode, and restoring the configured value (`stepper->setJumpStart(jumpStartConfig)`) immediately before their respective settle-at-deadband `moveTo()` calls - not a global config change, since jumpStart's normal, correctly-signed behavior is still wanted for every `moveTo()`-based mode.
2. **PID's own retry-on-dead-move logic reissued `runForward()`/`runBackward()` on every single 20ms tick with no throttle**, unlike homing's already-proven `retryMoveIfDied()` (`stepper_handler.cpp`, throttled to once per 100ms) for the same class of FastAccelStepper flakiness (a `runForward()`/`runBackward()` request can report `MOVE_OK`/`isRunning()==true` immediately, then the ramp generator silently abandons it within ~100-200ms with the step queue never filled - see `retryMoveIfDied()`'s own declaration comment for the original 2026-09-01 finding). Fixed by adding the same 100ms throttle (`lastPidRunRetryMs`) to PID's retry path - a genuine new direction command still issues immediately/unthrottled; only same-direction retries after a dead move are throttled.

Also added, as part of chasing bug #1 (kept since it's a real, independent latent bug worth having fixed regardless): `continuousRunDirection` (`stepper_handler.h`/`.cpp`), a shared flag any continuous-run mode sets alongside every `runForward()`/`runBackward()` call. `updateHoming()`'s switch-trip "was this contact bounce, or a real trip" filter previously trusted only `stepper->targetPos()` (meaningless during continuous-run motion - FastAccelStepper doesn't keep it updated for "keep running" moves, as the filter's own comment already noted) and `getCurrentSpeedInMilliHz() > 0` (can still read 0 for the first tick or two while a ramp is just starting) - neither reliably recognizes "we intentionally just started moving away from the switch" for Streaming/PID. `continuousRunDirection > 0` now covers that gap directly. Did not turn out to be the fix for bug #1 above (that was jumpStart), but is real, defensible infrastructure the switch-trip filter needed regardless, and is now in place for whenever PID/Streaming next departs from position 0 under real DDP traffic.

**Verified working, 2026-09-06** (`tools/tuning_harness.py`-based ad hoc smoke test, not yet folded into the harness proper): PID mode converges cleanly on a real DDP command (30% target from a cold start right at the homing switch) - clean acceleration ramp matching `pidAccel`, correct direction, `homed` stays `true` throughout, encoder confirms real physical motion in the correct direction the whole way, modest proportional-only overshoot then clean settle (expected with `Ki=Kd=0` - no derivative damping yet). Conservative starting gains used for this smoke test: `Kp=1.5, Ki=0, Kd=0, MaxSpeed=6500, Accel=2500, Deadband=30`.

### Acceleration and cruise-speed characterization sweeps (2026-09-06)

Ad hoc scripts (not yet folded into `tuning_harness.py` proper - `accel_sweep.py`/`speed_sweep.py`, currently only in the scratch working area, not committed), same methodology throughout: `$CHECKSTEPS` (direct `moveTo()`, no DDP/tracking-mode involved) for a large round trip clear of both physical ends, bench encoder as ground truth, steps/count ratio measured fresh from each run's own known-good baseline leg (came out to ~9.9-10.2 steps/count both times - matches the documented 1:10 worm ratio). Down = increasing position = paying rope out = gravity-assisted; up = decreasing position = winding rope in = gravity-opposed.

- **Acceleration sweep (speed fixed at 6500 Hz)**: swept `normalAccel` 2500→50,000 (20x the then-current default) - **no measurable slip in either direction across the entire range**, steps/count ratio stayed pinned at ~9.9-10.0 throughout, no switch trips, stayed homed. Acceleration is not the binding constraint at 6500 Hz cruise - at that speed even 50,000 steps/s² only takes 0.13s to ramp (~3% of the test move), so the whole swept range was already cruise-dominated. This reframes the earlier StallGuard-fighting saga: those false stalls were more likely from continuous re-planning/direction changes (Direct/Coalesce modes) than a genuine open-loop acceleration ceiling.
- **Cruise-speed sweep (accel fixed at 50,000, the clean value from above)**: swept `normalSpeed` upward and found a real, sharply-asymmetric stall boundary:

  | speed (Hz) | down (gravity-assisted) slip | up (gravity-opposed) slip |
  |---|---|---|
  | 8000 | clean (~0-3%) | clean (0.07%) |
  | 8500 | clean (0.07%) | clean (0.07%) |
  | 9000 | clean (0.34%) | **51% - real stall** |
  | 9500 | **39% - real stall** | (not tested - already failed at 9000) |

  Real ceiling: **up (winding in, gravity-opposed) is safe to ~8500 Hz, fails by 9000 Hz; down (paying out, gravity-assisted) is safe to ~9000 Hz, fails by 9500 Hz** - up's ceiling is consistently ~500 Hz lower, matching the physical expectation that winding in against gravity demands more torque. The currently-used 6500 Hz cruise speed (both `normalSpeed`'s default and `pidMaxSpeedConfig`'s default) has a comfortable ~2000-2500 Hz (25-30%) margin below *either* boundary.
  - [ ] Not yet narrowed further than 500 Hz resolution - could tighten with a finer sweep (e.g. 8600/8700/8800 for the up-direction boundary specifically) if a more precise ceiling is ever needed; current resolution is almost certainly good enough for setting `pidMaxSpeedConfig` with real margin.
  - [ ] This was measured with StallGuard disabled and TMC run current at the 1400mA figure from the earlier tuning session - re-run if either changes, since both directly affect real torque margin.
- **Run-current sweep (accel fixed at 50,000, three speeds well inside the clean zone: 6000/7000/8000 Hz)** - added a new RAM-only `$SET`/`$GET tmcRunCurrent` tunable for this (`tuning_handler.cpp`; deliberately bypasses `/save-tmc`'s full-form HTTP handler, which writes 14 Preferences keys to flash on every call and reconstructs every other TMC field from form args - an absent checkbox arg there silently reads as `false`, so a script posting only run current would have disabled the UART link and StallGuard as a side effect; the new tunable only touches `tmcRunCurrentConfig` then calls the existing `applyTmcSettings()`, leaving every other in-memory TMC field untouched). **Result: zero real slip at every current tested, 1400mA all the way down to 800mA, at all three speeds, both directions.** Current isn't the limiting factor anywhere in this speed range - the mechanism has large torque margin even at little more than half the currently-configured 1400mA. Reinforces the accel sweep's conclusion: none of these static, single-clean-move parameters (accel, current) were ever the real constraint in the safe-speed range: the earlier StallGuard-fighting during actual DDP tracking was much more likely from the *dynamic* behavior (continuous re-planning, rapid direction reversals in Direct mode) than a simple torque/current/speed ceiling.
  - [ ] Not pushed below 800mA - the user's requested range was 1400→800mA; genuinely finding where current starts to matter (if it does above the stall-speed boundary at all) would need going lower still.

**First-pass decision: 1200mA run current, 8000 Hz max speed.** Not purely from the slip data above (which showed zero measurable slip at every current/speed combination tested) - the user directly heard the motor audibly straining on the first few (highest-current, 1300-1400mA) runs at 7000-8000 Hz during the current sweep, even though nothing showed up as measured slip. The bench motor was cold during all of this session's testing; a real show would run warmer, which erodes torque margin further - so the qualitative, audible signal was trusted over the quantitative one here, deliberately choosing a value with headroom rather than the highest current that happened to test clean. **This is a real limitation of encoder-slip as a stall metric worth remembering**: a motor can sound close to losing sync well before it actually measurably skips a step in a short, cold-motor bench test.
- The user saved these via the web UI themselves (real, persisted Preferences values, not a live-only override): `tmcRunCurrent=1200`, `normalSpeed=8000`, **and `normalAccel=200,000`** - the last one far beyond anything the accel sweep actually tested (that sweep only went to 50,000, already comfortably cruise-dominated there; 200,000 was the user's own choice, not derived from sweep data).
- `stepperPidMaxSpeedConfig`'s compiled default updated 6500→8000 to match (`stepper_handler.cpp`) - no Preferences key exists for this (RAM-only by design, like every PID/tracking tunable), so the compiled default is the only thing making it durable across a reboot.

**Immediately superseded: the 200,000 accel + 8000 Hz combination sounded close to stalling on the bench** (audibly, not via any measured slip) - backed off live to **7000 Hz** (`normalSpeed` and `pidMaxSpeed`, the latter's compiled default also updated to 7000). This makes sense in hindsight: the run-current sweep that found 1200mA clean at 8000Hz was done at `accel=50,000`, **not** the 200,000 the user separately saved afterward - much higher acceleration demands much higher instantaneous torque right at the start of every move, so that sweep's current margin doesn't necessarily carry over to the new accel value.
- `normalSpeed=7000` is currently only a *live* override on top of the user's saved `8000` - a reboot (including any future reflash) reverts it back to 8000 from Preferences. Re-apply `$SET normalSpeed 7000` after any reboot until/unless the user saves 7000 via the web UI for real, or the sweep below gives a different number to settle on.

**Re-ran the current-vs-speed sweep at `accel=200,000` (pass 2, `current_sweep_results_200kaccel.csv`) as the direct follow-up - result: identical to pass 1.** Zero measurable slip at every current (1400mA down to 800mA) and all three speeds (6000/7000/8000Hz), both directions, even at 4x the acceleration. The audible "close to stalling" impression did not show up in the slip data at *either* acceleration tested.
- **Read this honestly, don't dismiss the audible signal just because the metric came back clean again**: either what the user heard is a harmless torque-ripple/resonance sound at this current/speed combination that doesn't correspond to real intermittent stalling, or the encoder-slip methodology (one cold-motor test move, ~10 steps/count resolution) has a genuine blind spot for whatever's actually happening acoustically - consistent with the same limitation already flagged above (a motor can sound close to losing sync well before it measurably skips a step in a short bench test). This sweep does NOT resolve which of those it is - it only rules out "a coarse, sustained stall the encoder would clearly show," not a finer-grained or intermittent issue.
- [ ] Open question, not yet resolved: what's actually behind the audible impression, if not measurable slip? Possible next steps if it matters enough to chase further: listen again carefully at each individual current/speed combination during a future sweep (rather than only at the extremes) to isolate exactly which combination(s) sound different; or try a longer sustained run (this test's moves are single, few-second round trips - heat buildup over a longer run could matter, and wasn't tested here).
- **Narrowed further by direct listening during pass 2**: the pre-stall-sounding noise showed up at **every** current level (1400mA down to 800mA) specifically at the **last** speed tested, 8000 Hz - not just the highest-current runs there. This points at speed itself as the trigger rather than current: a real torque-margin problem should get *worse*, not stay uniform, as current drops, so uniform noise across the whole current range at one specific speed looks more like mechanical/acoustic resonance at that speed than a graduated stall symptom. 6000/7000 Hz were not flagged as having this issue. **Reinforces 7000 Hz (already the working value) as the right ceiling for now, independent of current** - 8000 Hz should stay off the table regardless of how much current margin the slip data shows there.

**jumpStart A/B-tested at the new 200,000 accel (2026-09-06, `jumpstart_test.py`, prompted by the user asking whether it's still doing anything)**: identical results with `jumpStart=20` vs `jumpStart=0` across 3 repeats each (same `step_delta`, same `encoder_delta`, zero slip either way) - the Compact Motion Log's first sample after each move start already shows `curSpeedHz` at the full 8000 Hz target, meaning the ramp itself (8000/200,000 = 40ms) completes faster than jumpStart's kick would even matter for. **jumpStart is real, proven-needed technology for the original starting-torque-stall symptom it was added for** (see the entry above this one in this file) - its kick speed is `sqrt(2*accel*jumpStart)`, which scales *with* acceleration, so raising accel doesn't make it redundant by shrinking the kick; rather, the kick has become moot because the *normal* ramp is now already just as fast on its own. Left at its default (20) for now since it's harmless at this accel and still doing real work for any future lower-accel context (e.g. `homeAccel` is still only 20,000, tested separately, not verified either way).
- [ ] Not yet re-verified: whether jumpStart matters at `homeAccel=20,000` (homing's own, separate, much lower accel setting) - the A/B test above was only done at `normalAccel=200,000`.

- [x] **Acceleration-characterization sweep done** (see above) - acceleration was not the constraint; done in the same session with a follow-up cruise-speed sweep instead, which found the real (asymmetric) boundary.
### PID gain tuning (2026-09-06)

Raised `pidAccel` from its 2500 placeholder to 50,000 (vetted clean by the accel sweep) so gain tuning tests the control loop itself, not an arbitrarily slow ramp. Step-response methodology (`pid_kp_sweep.py`, scratch area, not yet committed): command a single, sudden target change from a genuine rest position, capture the whole transient via the Compact Motion Log, compute overshoot/first-crossing-time/oscillation.

**Two real, serious test-methodology bugs found and fixed before this data could be trusted - both are useful precedent for any future DDP-based bench test, not just this one:**
1. **`positionRequest` is a persistent global, never reset between tests.** Repositioning via `$CHECKSTEPS` (direct `moveTo()`) moves the stepper but does *not* touch `positionRequest` - so the moment `trackMode` flips to `4` (PID), PID immediately starts driving toward whatever `positionRequest` was left at by the *previous* test (the same `TARGET_POS` every time in this sweep), well before the "real" step packet for the new test even arrives. The capture (started after that mode switch) could begin mid-response or after it had already overshot, producing wildly different, irreproducible results at the *same* Kp across repeated runs. Fixed by explicitly sending a "neutral" DDP packet at the start position before engaging PID each time, so its error is genuinely zero at the moment it engages.
2. **DDP is UDP - no ack, no retry - so a single send of that neutral packet (or the real step packet) is not reliable.** A silently-lost packet leaves `positionRequest` stale with no client-side error, corrupting the test the same way as bug #1 even after fixing it. Confirmed for real on the bench (not just theorized) via a new `$GET positionRequest` (RAM-only, read-only - added to `tuning_handler.cpp` for exactly this) showing the neutral packet genuinely hadn't landed. Fixed by verifying delivery (poll `$GET positionRequest`, matching over the same serial connection rather than `/status-data` over HTTP - the latter, tried first, produced real request timeouts fighting the same single-threaded WebServer everything else on the device shares) and resending with a fresh sequence number until confirmed.

**Also found while investigating bug #1's symptoms: StallGuard had been silently re-enabled** (`tmcStallEnabled=true` on the live device, confirmed via `/status-data`), contradicting this whole session's established "disabled" status - a real `TMC2209: stall detected` / `Stopping motor` event fired mid-move during an early pass of this exact sweep. Root cause: almost certainly a side effect of the user's earlier web UI settings save (that form reconstructs every TMC field including checkboxes; StallGuard's checkbox must have been checked in the browser at save time). Added a matching RAM-only `$SET`/`$GET tmcStallEnabled` tunable (same rationale as `tmcRunCurrent` - a plain boolean gate with no register write involved, so trivially safe to toggle without `/save-tmc`'s full-form risk) and explicitly force it off at the start of every bench script now rather than assume.

**Clean Kp sweep (Ki=Kd=0), once both bugs were fixed:**

| Kp | overshoot (steps) | first-crossing time (ms) | oscillation |
|---|---|---|---|
| 0.5 | - | never converged in 8s | - |
| 1.0 | 0 | 3553 | none |
| 1.5 | 0 | 3062 | none |
| 2.0 | 0 | 2360 | none |
| 3.0 | 0 | 1720 | none |
| 4.0 | 0 | 1420 | none |
| 6.0 | 0 | 1160 | none |
| 8.0 | 0 | 1020 | none |
| 10.0 | 81 | 960 | none |
| 15.0 | 333 | 1200 | one reversal |
| 20.0 | 408 | 1180 | one reversal |
| 30.0 | 462 | 921 | none |

Clean, monotonic, physically sensible: response time improves steadily with Kp up to 8.0 (zero overshoot the entire way), then real overshoot begins between Kp=8 and Kp=10 and grows from there, while first-crossing time barely improves further (1020ms at Kp=8 vs. 921ms at Kp=30, despite ~4x the gain) - **Kp≈6-8 is the P-only sweet spot**, real diminishing returns past 8.

**Kd sweep (2026-09-06, `pid_kd_sweep.py`, not yet committed) - fixed Kp=15 (which showed 333 steps overshoot, one oscillation, Kd=0), swept Kd:**

| Kd | overshoot (steps) | first-crossing time (ms) |
|---|---|---|
| 0.0 | 300 | 920 |
| 0.02 | 291 | 1160 |
| 0.05 | 272 | (didn't cross within capture) |
| 0.1 | 214 | (didn't cross within capture) |
| 0.2 | 216 | (didn't cross within capture) |
| 0.3 | 190 | 940 |
| 0.5 | 9 | 960 |
| 0.7 | 3 | 1020 |
| 1.0 | 8 | 1100 |
| 1.5 | 8 | 1201 |
| 2.0 | 16 | 1306 |

Overshoot drops steadily from 300 (Kd=0) to near-zero by Kd=0.5-0.7, with first-crossing time barely moving (920→1020ms) - real derivative damping, not just a slower response masquerading as less overshoot. Past Kd≈1.0, response time starts climbing again (over-damping) without further overshoot benefit. **Kp=15, Kd≈0.5-0.7 is a genuinely better operating point than the P-only sweet spot** (Kp=8, 0 overshoot, 1020ms) - comparable or better overshoot control, same speed, at nearly 2x the proportional gain (steeper response to a growing error, which should matter more once actually tracking a moving DDP target rather than just one step).
- Note: one Kd=0.5 run hit a real DDP delivery failure (neutral packet never confirmed after 10 retries, lost homing) - recovered automatically via the sweep's re-home fallback; the very next run (Kd=0.7) was clean, and this looks like an unrelated transient network/device hiccup, not a Kd-specific issue (nothing about a PID gain should affect UDP packet delivery) - noted rather than dismissed, in case it recurs.

**Working PID gains after this pass: `Kp=15, Ki=0, Kd=0.7, MaxSpeed=7000, Accel=50000, Deadband=30`** (live only - not yet saved anywhere durable; there's no Preferences path for any PID tunable, so nothing persists across a reboot regardless).

- [x] **Moot - Ki removed entirely 2026-09-07** (see stepper_handler.h's declaration comment).
- [x] **Superseded** - the later continuous-wave testing (triangle waves, both directions, every cycle) is a far more thorough test than this item asked for, and covers this concern as a side effect.
- [x] Tested against a real synthetic DDP stream (triangle wave via `tuning_harness.py`) - see the section immediately below. Found and fixed two more real bugs in the process; the "up"/gravity-opposed direction concern above turned out to be a symptom of one of them, not a separate real asymmetry (the second fix below made both directions clean).

### PID vs. a real DDP triangle wave (2026-09-06) - two more real bugs found and fixed

First full `tuning_harness.py` run against tuned PID (`config_pid.json`, `Kp=15/Kd=0.7/Accel=50000/MaxSpeed=7000`) completed its first 5s run cleanly (real tracking, `tracking_pct=100%`) but then every subsequent duration failed with `ERR not homed` - the device silently lost its calibration partway through the session and every later run was doomed from the start.

**Bug 1: `updateRammedIntoStopCheck()` false-triggered on PID's own legitimate settling behavior right at position 0.** Root-caused via the persist log (`GET /persist-log`, exactly the tool built for this): `Rammed into homing stop: -9 to -108 steps counted while switch held triggered for 1500ms` fired shortly after each triangle wave returned to its start (position 0 - always switch-triggered, and a completely normal DDP endpoint, not just a homing reference). Cause: PID's deadband check (`stepperPidDeadbandConfig=30`) is far tighter than DDP's 8-bit position quantization on this device (~60 steps per DDP unit, given `bottomPosition≈15500`) - near position 0, a single incoming DDP value could shift the computed target by more than the deadband, kicking `pidSettled` back to `false` and re-engaging full continuous-run control (`runForward()`/`runBackward()`) for what was really a trivial correction. Each re-engagement produced a small amount of real motion right at the switch - individually harmless, but repeated across a stream of DDP updates, added up to "position changing while switch triggered" for well over 1.5s.

  Fixed with two changes together (neither alone was sufficient - see below):
  - New `stepperPidReengageThresholdConfig` (default 150 steps, `$SET`/`$GET pidReengageThreshold`): a hysteresis band wider than the deadband - while already settled, a target shift within this band gets another one-shot `moveTo()` snap instead of dropping back into continuous-run mode.
  - New `RAMMED_STEP_TOLERANCE` (150 steps, matches the above) in `updateRammedIntoStopCheck()` - only fires on *net* drift since the trip began that exceeds this, not any nonzero change. Doesn't meaningfully weaken real-jam detection: `switchTrippedSinceMs`/`switchTrippedStartPos` are never reset while the trip continues, so a genuine sustained jam still accumulates past the tolerance given enough time, it just takes a little longer to confirm than a hard zero-tolerance check.
  - **Tried and rejected**: gating the hysteresis-band `moveTo()` on "has the target moved enough since `lastCommandedTargetPosition`" (instead of just checking `isRunning()`) - this created a real dead zone (the trolley sitting still through however wide that gate was before catching up in a jump) that measurably hurt tracking accuracy on the bench (rms_error jumped from ~840 to ~2600 for the same 5s run). Reverted in favor of the `isRunning()`-gated version below.

**Bug 2: the hysteresis band itself, as first implemented, reintroduced Direct mode's own well-documented "constantly re-aiming never accelerates" jerkiness.** Calling `moveTo()` unconditionally on every ~20ms tick while the target crept slowly through the reengage band (e.g. departing position 0 during a slow 16s wave) kept resetting FastAccelStepper's ramp generator before it ever built real speed. Confirmed directly on the bench: a 16s triangle wave test showed the trolley stuck dead at position 0 for the first 4+ seconds despite the commanded target ramping smoothly away to ~8000, then catching up in one large delayed snap once the drift finally exceeded the whole 150-step band (`near_stall_pct: 29.0` in that run's metrics, vastly higher than every other run's 0.3-2.4%). This is exactly the class of problem PID's continuous-run design exists to avoid - the new hysteresis band had quietly reintroduced it in the one narrow case it covers.

  Fixed by gating the hysteresis-band `moveTo()` on `!stepper->isRunning()` - only issue a fresh snap once the *previous* one has actually finished, letting each small correction complete before reacting to further drift, rather than interrupting it every tick.

**Result after both fixes, full 5-duration sweep (`pid_tuned6`), clean end to end:**

| duration | rms_error | max_error | near_stall_pct | tracking_pct |
|---|---|---|---|---|
| 5s | 908 | 1442 | 2.3% | 100% |
| 8s | 490 | 1109 | 1.7% | 100% |
| 12s | 299 | 746 | 1.0% | 100% |
| 16s | 234 | 526 | 1.0% | 100% |
| 20s | 181 | 401 | 0.3% | 100% |

Error scales down smoothly and monotonically with duration (exactly the expected pattern, matching how every pre-PID mode behaved in the very first characterization session), no un-homing anywhere, no stuck episodes, both directions clean (the earlier "up-direction oscillation" concern - visible in an intermediate, not-yet-fully-fixed pass - turned out to be a symptom of bug 1/2 above, not a separate real gravity-direction asymmetry; resolved once both were fixed).

### Bug 3: the hysteresis band's own `moveTo()` could go "dead" too, plus a real logging-completeness gap (2026-09-06, after fixing `send_triangle_wave()`)

After fixing the sender's own timing artifact (see the "Raw DDP trace jumpiness" section further down), re-ran the full sweep and the 16s run regressed hard - `rms_error=1515`, `max_error=5299`, `near_stall_pct=16.1%` (vs. `234`/`526`/`1.0%` before). The plot showed the exact same "stuck at position 0 for 2+ seconds, then one big catch-up jump" signature as bug 2 above, which was supposedly already fixed.

Added targeted diagnostics (`isRunning()`/`isQueueEmpty()`/`isRampGeneratorActive()`/current speed, printed whenever the hysteresis branch is waiting) and reproduced it directly: `isRunning=1 ... qEmpty=0 rampActive=0 speedHz=0`. **The hysteresis band's `moveTo()` can go "dead" the same way `runForward()`/`runBackward()` do** (see `retryMoveIfDied()`'s original 2026-09-01 finding) - a nonempty queue alone satisfies `isRunning()`, even when the ramp generator never actually starts producing steps. Bug 2's fix (wait for `!isRunning()` before re-issuing) never re-issues in this state, since a dead-but-nonempty queue reads as running forever.

**Fixed**: treat "genuinely running" as `isRunning() && (isRampGeneratorActive() || currentSpeed != 0)`, not just `isRunning()` alone, and retry (throttled to 100ms, same pattern as `retryMoveIfDied()`/`lastPidRunRetryMs`) when it isn't.

**While investigating this, also found and fixed a real logging-completeness gap** the user specifically asked to verify before running more tests: `updatePidMode()` only wrote a Compact Motion Log row when actually issuing a correction, not on every ~20ms tick - so a real, smoothly-received DDP stream could update `positionRequest` many times while PID sat fully idle/settled, and none of it would appear in the log; the next row logged would then show an artificially large `ddpVal` jump indistinguishable from a dropped packet. **Confirmed this was 100% a logging artifact, not a reception one**, two independent ways: (1) the sender-fix's own packet-count check (321 sent, 321 received, 0 rejected) already ruled out loss at the transport level; (2) a dedicated `protocolDebugConfig=1` capture (which prints every packet as it's parsed, independent of any tracking-mode logging) showed **zero sequence gaps across 202 packets** on a run whose Compact Motion Log alone would have suggested ~26% of transitions were "jumpy." Fixed by logging unconditionally on every tick regardless of branch (idle-settled, hysteresis-wait, hysteresis-commit), not just when a correction actually fires.

**Re-ran the full sweep with both fixes - cleanest result yet:**

| duration | rms_error | max_error | near_stall_pct |
|---|---|---|---|
| 5s | 747 | 1497 | 1.8% |
| 8s | 429 | 1103 | 0.2% |
| 12s | 279 | 564 | 0.1% |
| 16s | 213 | 442 | 0.2% |
| 20s | 173 | 352 | 0.5% |

No stuck episodes anywhere, `near_stall_pct` low and consistent throughout, `ddpVal` trace now 94% single-unit transitions (remaining ~6% explained by ordinary 20ms-log-tick-vs-~25ms-DDP-send-interval sampling mismatch, not any real gap - confirmed visually smooth in the plot too).

### Real design-goal mismatch, flagged by the user, NOT yet worked on (2026-09-06)

All of today's Kp/Kd tuning optimized for *fastest clean response to a single large step* (step-response testing, one big jump per test). That's the wrong objective for what PID is actually for in production: **real xLights/FPP sequences command position at a fixed cadence (the user sequences shows at 40fps) with typically *small* moves between consecutive frames**, not one big jump followed by a long idle gap. Tuned this way, PID has no notion of "how much time do I actually have until the next commanded position" - it always drives toward the current target as fast as Kp/Kd/MaxSpeed/Accel allow, which for a small move means snapping there almost instantly and then sitting idle until the next frame nudges it again - "jerk, jerk, jerk" between frames, exactly what PID was supposed to eliminate, rather than a smooth, continuous glide paced to the real 25ms-per-frame budget.

**Not yet worked on - explicitly deferred** in favor of the DDP reception investigation below, but a real, higher-priority item once that's resolved: needs something like a time-budget-aware speed target (e.g. derive a max speed from *distance-to-target / time-since-last-command* rather than a fixed `pidMaxSpeed`, or a real trajectory/velocity planner) rather than the current "always-fastest" step-response-tuned behavior. `trackRateWindow`/Streaming mode's rate-estimation approach is conceptually closer to this than pure step-response PID is - worth reconsidering that lineage, or a hybrid, once this is picked back up.

### Bug 4 (found, NOT yet fixed): real multi-second freezes where `positionRequest` itself stops updating

Re-ran the same sweep once more right after the above (same firmware, same config) purely so the user could watch the trolley directly, and it came back noticeably worse (5s/8s/16s regressed hard, and the 20s run's pre-check caught a real 12-step loss requiring an automatic re-home). The 8s run's plot makes the mechanism unambiguous: **both** the "Commanded" position trace **and** the raw DDP value panel go flat together for 1.2s then 3.4s before both resume together - if this were a stepper/motion issue (like bugs 2/3 above), the commanded trace (pure arithmetic on the received DDP value, nothing else) would keep updating even while actual position lagged. It doesn't. `positionRequest` genuinely stops changing on the device for whole seconds, confirming this is a **DDP reception-layer symptom**, not anything downstream of it - none of today's `moveTo()`/hysteresis fixes are implicated. The 12s/16s pair confirms it a second way: the 12s run's own log ends frozen at the exact value/position the 16s run then opens with - the freeze outlived one run's capture window and bled into the next.

Two real candidate explanations, not yet distinguished: (1) genuine sustained WiFi packet loss - different from the earlier, isolated sender-timing check (which proved 321/321 delivery on a short clean bench test; this could plausibly get worse over a long, hot, hours-into-testing session); (2) a `isNewerDdpSeq()` rolling-window edge case where `lastAcceptedDdpSeq` gets stuck relative to the sender, though the arithmetic (self-resolves within ~15 packets, ~375ms at 40Hz, for a normally-incrementing sequence) is shorter than the 1.1-3.4s freezes actually seen, making this the less likely of the two. The on-device counters that would settle it (`protocolPacketsReceived`, `protocolPacketsRejectedOutOfOrder`) reset with an intervening reboot (closing the harness's serial connection toggles DTR/RTS same as opening one) before they could be checked, so this specific run's counters aren't recoverable after the fact.

- [x] **Instrumented properly instead of guessing further** - see the dedicated section immediately below. Built a real DDP reception test tool; pure reception (no stepper motion at all) turns out to be completely clean, which narrows this down a lot without yet fully explaining it.

### Dedicated DDP reception instrumentation (2026-09-06) - pure reception is clean; still doesn't explain Bug 4 above

Built proper tooling rather than continuing to guess at Bug 4's cause: `tools/ddp_reception_test.py` sends a continuous, real-show-cadence (default 40fps, matching the user's own sequencing rate) triangle wave for a configurable duration, with the device **never homed** (positionRequest and all of this logging update regardless of homed state - confirmed earlier this session - so this tests reception in complete isolation from any stepper/tracking-mode behavior, zero motion risk). Three new pieces:
- `$SET ddpRxLog` (`ddp_handler.h`/`.cpp`) - one clean CSV line per packet (`<ms>,DRX,<seq>,<value>` accepted, `<ms>,DDPREJ,<seq>,<lastAcceptedSeq>` rejected) instead of `protocolDebug`'s verbose multi-line format, plus a throttled `<ms>,RSSI,<dbm>` line every ~500ms.
- `$SET ddpAck` (same files) - echoes a 1-byte UDP ACK (the received sequence number) back to the sender's own `remoteIP()`/`remotePort()` immediately upon receiving any packet at the UDP layer, regardless of the sequence-acceptance decision - lets the sender directly measure round-trip time and detect genuine loss (an unacked send), decoupled from this firmware's own sequence logic.
- The Python side records every packet it sends (for "how smooth is sending") and matches ACKs to sends per-sequence-number for RTT.

**Found and fixed a real measurement artifact in the test tool itself before trusting any of its RTT numbers**: the first version ran the ACK receiver on a background thread, and its RTT readings showed a spurious, sustained cluster around 380-440ms (looked exactly like a real, alarming network problem). Cross-checked against the device's own per-packet receive timestamps (`DRX` lines - independent of anything this script measures) and found reception was clean throughout both the run that showed the cluster and one that didn't (inter-packet gaps mostly 2-77ms, only a literal handful of brief outliers, no sustained plateau) - meaning the cluster wasn't real, it was the receiver thread not getting scheduled promptly against the tight-timed sender (GIL contention and/or general OS scheduling jitter on this machine, not the network or device). Rewrote the tool single-threaded with a non-blocking socket, polling for ACKs at a fine grain (~2ms) interleaved directly into the send loop instead of relying on a separate thread at all - the spurious cluster is gone, replaced by genuinely low, believable numbers.

**Along the way, also disabled WiFi modem sleep** (`WiFi.setSleep(false)` in `wifi_handler.cpp`) after the spurious-cluster reading looked at the time like a plausible power-save symptom (a step-change to a higher, flat latency plateau is the classic signature). Once the tool's own artifact was found and fixed, the "before" evidence for power-save specifically no longer holds up - but the change is being **kept anyway**: disabling modem sleep is correct, unambiguous best practice for any real-time WiFi control protocol regardless of whether it was the cause of anything observed here, and there's no real cost (this is mains-powered prop hardware, not battery).

**Final, trustworthy result (60s continuous, 40fps, `--ack`, `tools/tuning_runs/ddp_rx_60s_v3.png`): completely clean.** 2401/2401 packets sent, received, *and* acked (100% both ways, zero rejected), zero received-value transitions jumping by more than one unit, RTT mean 2.7ms / p95 16ms / max 94ms with no systematic pattern anywhere in the 60s window, RSSI stable and strong (-48 to -53dBm) throughout.

**This is a real, useful, if partial, answer**: pure DDP reception, with no stepper motion at all, is completely healthy on this hardware/network - ruling out a general WiFi/protocol/environment problem as the explanation for Bug 4's multi-second freezes. Bug 4 was only ever observed during runs that were *also* actively driving the stepper. **This points the remaining investigation specifically at something tied to real motor motion** - most plausibly either electrical/RF interference from the stepper driver disrupting the WiFi radio while actively switching, or CPU/interrupt resource contention on the single core juggling DDP + WiFi + stepper + TMC UART simultaneously - rather than a WiFi range/environment issue (which the clean RSSI here argues against) or a firmware DDP-parsing bug (which the clean reception here also argues against).

- [x] Re-ran with real motion - see the dedicated section immediately below. Bug 4 did **not** reproduce.
- [x] Standing contingency note, not an action item.

### Motion + reception combined test (2026-09-06) - Bug 4 did NOT reproduce; root cause still unconfirmed

Before re-running, two things changed in support of running this test with minimal serial dependency (per the user's own request - "set up the log to be over the network", "create a web endpoint for configuring the in-ram parameters"), both in `tuning_handler.cpp`/`.h` and `html_handler.cpp`:
- `ddpRxLogConfig`'s output moved off Serial entirely, into a bounded in-RAM buffer retrievable via `GET /ddp-rx-log` (`?clear=1` to wipe) - see `ddp_handler.h`'s declaration comment.
- A new `GET /tunable?name=<n>[&value=<v>]` HTTP endpoint mirrors the serial `$SET`/`$GET` protocol exactly (`name=ALL` reads everything as JSON).

`tools/pid_motion_reception_test.py` was rewritten to use both: all tunable configuration and DDP-rx-log retrieval now go over HTTP, with serial reserved only for the Compact Motion Log capture (no HTTP equivalent for that stream), status polling, and homing.

**Result (60s, 40fps, `--ack`, homed, real PID tracking, `tools/tuning_runs/pid_motion_reception_v2.png`): also completely clean.** 2401/2401 DDP packets sent, received (100%), and acked (100%), zero rejected, zero device-side inter-packet gaps over 200ms, RTT mean 3.9ms/p95 16ms/max 172ms (two outliers over 150ms, nothing sustained), RSSI stable (-54 to -51dBm). Compact Motion Log: 3056 rows, **zero freeze events** (the >500ms-unchanged-`curPos` detector that would have caught Bug 4 found nothing), tracking error stayed bounded (max 607 steps, mean 93.7 steps - in line with prior tuning results, not itself concerning).

**This does not prove Bug 4 is fixed** - a single clean 60s run against one earlier run that showed it is weak evidence either way, and nothing was changed that directly touches DDP receive/parse logic. The two things that *did* change since Bug 4 was observed, either of which could plausibly explain a clean result without the underlying issue being understood:
1. `WiFi.setSleep(false)` (added this session, see above) - motivated by a since-debunked artifact, but never ruled out as a genuine fix for something WiFi-power-save-related under real load.
2. Moving `ddpRxLogConfig`'s output off Serial - not relevant here since Bug 4 was originally observed on runs that did **not** have that diagnostic enabled at all (it didn't exist yet), so this can't be the explanation, but it does mean this test's own instrumentation is now lighter-weight than the original `protocolDebug`-based runs were.

- [x] **Closing pending recurrence** - never recurred across many subsequent long bench sessions, including the entire 2026-09-07 PID investigation (hours of continuous-wave testing). `WiFi.setSleep(false)` remains the leading candidate; downgrading from "open investigation" to "watch for recurrence."
- [x] The visible high-frequency ripple in actual speed during active tracking - measured (not just eyeballed) via a real damping sweep and found to be a genuine underdamped Kp/Kd resonance (period ~80-100ms, ~10-13Hz, independent of commanded speed - not DDP-frame-rate-locked), not input quantization. See "PID damping sweep" section below - `pidKd` lowered from 0.7 to 0.3, measurably reduces it at every duration.
- [x] **Moot - Ki removed entirely 2026-09-07.**
- [ ] `pidReengageThreshold`/`RAMMED_STEP_TOLERANCE` were both set to 150 somewhat by feel (matching each other for consistency) rather than from a systematic sweep - reasonable given the DDP-quantization math that motivated them (~60 steps/DDP-unit on this device), but not independently verified as optimal.
- [ ] The deadband-settled branch's own `moveTo()` (the very first snap into `pidSettled`) doesn't have the same dead-move retry as the hysteresis band - lower risk (that branch only fires once error is already within the tight deadband, so a dead move there barely matters) but not verified clean the same rigorous way.
- [ ] Streaming mode got the jumpStart fix but *not* the retry-throttle fix (bug #2) - it has the identical exposure (same untamed "reissue every tick while `!isRunning()`" pattern) but wasn't the mode under active test, so this is unverified there. Low priority given Streaming is already deprioritized/shelved, but worth doing before ever picking Streaming back up.
- [x] Merged into the live curated TODO list at the top of this file (retry-logic consolidation, Wave 2).

### Bug 5 (found AND fixed, 2026-09-06): PID freezes for seconds at every direction reversal - a real production show-stopper, found while chasing the speed ripple

Started as a Kd/Kp damping sweep (see the section below) aimed at the speed ripple noted above. The very first low-Kd candidate came back catastrophically worse (rms_error 7355 vs. the ~440 baseline) - not more ripple, a real multi-second freeze. Traced through **three fix attempts** before it was actually gone:

1. **First bug found**: PID's continuous-run retry check (`main.cpp`'s `updatePidMode()`) used plain `stepper->isRunning()` to decide whether to retry a dead `runForward()`/`runBackward()` call - but a dead-but-nonempty FastAccelStepper queue satisfies `isRunning()` without ever producing a step (the same hazard the hysteresis band's `moveTo()` retry was already fixed for, earlier this session - this branch just never got the same fix). Right at the homing switch (position ~0, the state every run starts in), a `runForward()` call could go dead this way and then never retry at all, since `pidCurrentDirection` had already latched to the new direction. Reproduced directly: `curSpeedHz` read exactly 0 for ~228 consecutive 20ms ticks. **Fix attempt 1**: added the same `genuinelyRunning` check (`isRunning() && (isRampGeneratorActive() || getCurrentSpeedInMilliHz() != 0)`) to this branch too.
2. **That wasn't enough**: a follow-up sweep run hit a full **2.4-second stall exactly at a triangle wave's peak** (a direction reversal, not a cold start) that fix attempt 1 failed to recover from - proving `isRampGeneratorActive()` can itself report `true` for seconds at a stretch with zero actual speed, making it actively misleading as a trust signal here, not just insufficient. **Fix attempt 2**: dropped `isRampGeneratorActive()` from the check entirely, trusting only `getCurrentSpeedInMilliHz() != 0` (never observed to be wrong in any case this session).
3. **Still not enough on its own**: the real root cause turned out to be something else entirely - calling `runForward()`/`runBackward()` for a *new* direction while the stepper was **still actively producing steps in the old direction** (mid-deceleration) wedges FastAccelStepper outright, regardless of how the retry condition is written, since every subsequent retry calls the same already-wedged API the same way. Confirmed with a full 4-second stall, **both the commanded position and the raw DDP trace updating cleanly the entire time** - the same "check whether the reception-layer trace freezes too" diagnostic used for Bug 4, here proving definitively this is a motion-control bug, not a network one (asked directly mid-session: "are we sure network issues aren't giving us grief?" - no, confirmed by this exact trace). **Fix attempt 3 (the one that actually worked)**: never call `runForward()`/`runBackward()` over an actively-moving opposite-direction run - force a clean `forceStop()` first and wait for it to genuinely settle (the same `pidStopSettling` pattern already used for the overshoot safety net), *then* issue the new direction. Needed a dedicated latch (`pidDirectionSwitchPending`) to avoid infinitely re-triggering the stop across the multi-tick wait.
4. **Even fix attempt 3 wasn't complete**: a subsequent full-sweep verification still hit one *more* multi-second freeze, this time engaging a **fresh** direction from an apparently-idle stepper (not mid-reversal, and not the boot-time cold start either - this was the start of a new triangle-wave test right after the harness's own direct-`moveTo()` skipped-step check had just finished). Proved the race isn't specific to "still visibly moving in the old direction" - engaging `runForward()`/`runBackward()` too soon after *any* other stepper activity can wedge it the same way. **Final fix**: widened the stop-and-settle guard to fire on *every* direction change unconditionally, not just ones where the stepper was still visibly moving. A `forceStop()` on an already-idle stepper is a harmless no-op, so this costs at most one extra ~20ms tick of latency on the normal case.

**Verified clean**: two full 5-duration sweeps (Kp=15/Kd=0.7 baseline, and Kp=15/Kd=0.3) on the final fixed firmware, zero contamination, zero freezes, zero unexpected trips, across all 10 direction reversals between them. See `tools/PID_TUNING_SESSION_2026-09-06.md`'s damping-sweep section for the full data.

This was a real, serious bug independent of the ripple/gain question that started the investigation - any actual show with PID tracking would have frozen at literally every direction reversal in the sequence. Worth being explicit that this was caught *by* the damping sweep, not despite it - a good example of why a broad, repeated, automated test sweep against real hardware surfaces things a single clean bench run doesn't.

- [ ] `retryMoveIfDied()`/PID's own retry logic/Streaming's equivalent (see the item above) should probably all be re-examined together now, given how much was learned here about `isRunning()`/`isRampGeneratorActive()` both being unreliable trust signals - Streaming still has the *original*, less-hardened retry pattern and was never covered by any of the four fixes above.

### PID damping sweep (2026-09-06) - lower Kd reduces the speed ripple; Kd defaulted to 0.3

With Bug 5 above actually fixed, re-ran the sweep properly. Built `tools/analyze_ripple.py` (steady-cruise-window ripple RMS/period, plus a dead-start/contamination detector) to get an objective comparison metric instead of eyeballing plots.

| candidate | 5s ripple | 8s ripple | 12s ripple | 16s ripple | 20s ripple | notes |
|---|---|---|---|---|---|---|
| Kp=15/Kd=0.7 (baseline) | 629Hz | 515Hz | 498Hz | 470Hz | 392Hz | original step-response-tuned gains |
| Kp=15/Kd=0.3 | 572Hz | 494Hz | 384Hz | 337Hz | 310Hz | **recommended - see below** |
| Kp=15/Kd=0.15 | - | - | - | 353Hz | - | similar to Kd=0.3, no clear further gain |
| Kp=15/Kd=0 (pure P) | - | 451Hz | - | - | - | best raw ripple number, but see caveat below |
| Kp=15/Kd=1.2 | - | 579Hz | - | - | - | worse than baseline - confirms higher Kd hurts |
| Kp=10/Kd=0.7 | - | - | - | 330Hz | - | lower Kp also helps ripple, but hurts rms_error more (312 vs 209 baseline) |
| Kp=8/Kd=0.5 | - | - | - | 213Hz | - | lowest ripple of any candidate, but rms_error 360 (worse trade than Kd alone) |

Kd=0.3 wins on both ripple *and* rms_error against baseline at every duration in a full, clean, contamination-free 5-duration sweep (rms_error 572/371/233/233/248 vs. baseline's 679/444/359/293/206). Lowering Kp instead of Kd also reduces ripple, but costs more tracking accuracy for the same benefit - Kd is the better lever.

**Kd=0 tested even better on ripple/rms_error but is NOT recommended pending a closer look**: during its full-sweep verification, the device tripped an ambiguous "rammed into homing stop" event (`persist_log`) partway through, ending the sweep early and requiring a re-home. The user directly observed the trolley during this - **no buzz or grinding sound, it simply stopped moving** - which argues against a real mechanical ram (a stepper straining against a hard stop typically whines/buzzes audibly) and toward this being a false positive from residual step-counter drift during some other still-unresolved dead-run edge case, rather than genuine overshoot into the switch. Not confirmed either way. Kd=0.3 gets most of the same ripple benefit without going anywhere near this edge case in any tested run, so it's the safer choice for now.

**Applied**: `pidKd`'s compiled default changed from `0.7` to `0.3` (`stepper_handler.cpp`) - like every PID tunable, there's no Preferences/NVS path for it, so the compiled default is the only thing making this durable across a reboot. Flashed and verified live (`GET /tunable?name=pidKd` reads back `0.3000` after a fresh boot).

- [ ] Figure out what actually happened during the Kd=0 sweep's "rammed into stop" trip - false positive from residual dead-run position drift (most likely, per the no-buzz observation) vs. a genuine, if quiet, overshoot. If it's a false positive, `updateRammedIntoStopCheck()`'s reliance on step-count drift as a proxy for real jamming may need revisiting the same way `isRunning()`/`isRampGeneratorActive()` did in Bug 5.
- [x] Ki: moot, removed entirely 2026-09-07. Kp re-sweep: done the same night - see "PID tick rate found to be the real ripple lever" above (Kp=3 chosen).
- [x] **Resolved** - this is exactly what the time-budget velocity feedforward ("Time-budget PID velocity feedforward" section) was built to fix.

### Raw DDP trace jumpiness - root-caused and fixed for real (2026-09-06)

The "commanded position looks jagged" symptom noticed multiple times across this whole tuning effort (first as an unexplained Streaming-mode artifact, then again in the PID triangle-wave plots above) turned out to have nothing to do with WiFi, dropped packets, or firmware at all - it was `tools/tuning_harness.py`'s own `send_triangle_wave()`.

**Root cause**: each packet's DDP value was computed from `time.monotonic()` at the *actual moment of sending*, not from the schedule. If the Python process's send loop fell behind by even a normal amount (ordinary OS scheduling jitter, general system load - nothing exotic), the next value legitimately reflected however far the wall clock had moved on by the time the delayed send finally went out - a real, correctly-computed value, just a bigger step than the nominal one-DDP-unit-per-tick progression. In a plot this looks identical to a dropped packet or a firmware bug.

**Confirmed definitively, not just inferred**: a direct packet-count comparison (Python-side send count vs. the device's own `protocolPacketsReceived` counter) on a fresh run showed **321 sent, 321 received, 0 rejected as out-of-order** - on a run whose DDP trace still showed several 2-5-unit jumps. Every packet arrived; the jumpiness was entirely in what value each packet *carried*, decided before it ever left the sending machine.

**Fixed**: `send_triangle_wave()` now computes each value from the *scheduled* tick time (`tick / rate_hz`) instead of real elapsed time - the value sequence is a perfectly smooth, deterministic ramp regardless of real-time send jitter. Ordinary jitter needs no special handling (the loop just sends without sleeping until it catches back up, each tick still carrying its own correct value); a new `max_stall_s` guard only matters for a genuine, large stall, where it resyncs to real time and logs it rather than flooding out a large backlog of queued ticks. This also better matches how a real DDP source like FPP actually works - driven by a fixed show timeline, not by whatever value happens to be correct for whenever the sender gets around to transmitting.

**Verified**: a fresh 20s run went from 92.7% clean single-unit transitions (7.3% jumps of 2-5 units) under the old code to **100% clean (510/510 transitions, zero jumps of any size)** under the fix.

- [x] Informational note, not an action item - no action was ever needed (says so itself: "none of them need to be redone").

## Smaller/follow-up items

- [ ] Web UI: pixel count input already allows up to 1000 (`MAX_LEDS`), no change needed there, but consider adding a hint/warning in the LED settings section once Priority 2 establishes real practical limits over WiFi.
- [ ] Consider whether `stepperControlEnabled = false` (pixel-only prop) should be a first-class documented use case — it already works today (channels 1-2 stay reserved/unused, LEDs still start at channel 3), just isn't called out anywhere.
