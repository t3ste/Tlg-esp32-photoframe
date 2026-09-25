# Bedside Alarm Clock feature — feasibility & effort assessment

**Status (2026-09-24): Phases 0-3 implemented and real-hardware-confirmed working**, including a
long KEY press straight from actual deep sleep - see `docs/DIFF.md`'s "Alarm Clock" entry for
shipped behavior and the project runbook's own section on this feature for the full incident
history (three rounds of live testing found and fixed: a fast-burst batch-press mode that didn't
work reliably and was removed per the user's call rather than tuned; a crash when entering the
setting UI from deep sleep, `assert failed: tcpip_send_msg_wait_sem ... Invalid mbox`, caused by an
earlier fix skipping WiFi driver init entirely instead of just the connection attempt; and a
press-duration measurement bug that silently required ~5.5-6s of holding instead of the intended
3s). Live-confirmed via the user's own serial captures (`Versuch4.log`/`Versuch5.log`/`Versuch6.log`):
deep-sleep KEY-wake correctly measuring ~3s, no crash, no unwanted WiFi connection or HTTP server
start on that path, the 10s inactivity timeout disarming correctly, a full button-driven
hour/minute/confirm cycle arming a rule correctly, **and the device correctly returning to deep
sleep within milliseconds of confirming, specifically when not USB-powered** (`Versuch5.log`:
"Alarm armed via button UI" immediately followed by "Preparing to enter deep sleep mode") - the one
previously-outstanding "can this even be observed" item, now closed. On USB power the device
correctly stays awake instead (also confirmed live), matching the deliberate
`!board_hal_is_usb_connected()` guard. Phases 0-3 have no known open issues at this point. Phase 6
(offline voice "Alarm off" detection) is still just design/research, not implemented. The rest of
this document is the original feasibility research, kept as-is for reference.

## 0. Build-time modularity (2026-09-23) — confirmed feasible, follows an existing pattern exactly

Since not every supported board has a speaker, buttons in the required shape, or (for voice) a
mic, the whole feature should only enter the firmware image when a new build parameter requests
it — this project already does exactly this for the Chimes feature, at two levels:
- **File-level exclusion**: `components/board_hal/CMakeLists.txt:22-36` conditionally
  `list(APPEND SRCS ...)`s each board's driver file based on a Kconfig `choice` — a board not
  selected gets none of that file's code at all.
- **Stub-on-`#ifdef`**: `components/board_hal/src/audio_chime.c:6-18` is compiled for every board,
  but without `CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73` defined it collapses to a two-line
  stub (`return false;` / `ESP_ERR_NOT_SUPPORTED`) — the real ES8311 driver and tone synthesis are
  fully dead-code-eliminated, not just hidden.
- **Build-flag plumbing already exists as a template**: `build.py`'s `build_firmware()`
  (`build.py:103-116`) joins `sdkconfig.defaults;boards/sdkconfig.defaults.<board>` and, only when
  `--debug` is passed, appends a further `sdkconfig.defaults.debug` overlay — the exact mechanism
  a new `--alarmclock` flag would reuse verbatim (append `sdkconfig.defaults.alarmclock`, which
  sets a new `CONFIG_ALARM_CLOCK_ENABLED=y`).

**Recommended shape**:
1. New Kconfig bool `ALARM_CLOCK_ENABLED` (default off), plus a new `build.py --alarmclock` flag
   appending a matching sdkconfig-defaults overlay — mirrors `--debug` exactly.
2. The feature's own new source files (`alarm_manager.c`, and later `voice_recognition.c`/
   `mfcc_dtw.c`/an ES7210 driver) are excluded at the `main/CMakeLists.txt` SOURCES-list level via
   `if(CONFIG_ALARM_CLOCK_ENABLED) ... endif()` — the board_hal driver-file pattern, not the
   single-file stub pattern, since this feature spans multiple new files and the stub approach
   would mean compiling real MFCC tables/DSP code just to discard it.
3. The handful of integration points in existing files (`main.c`'s wake dispatch, `http_server.c`'s
   route registration, `config_manager.c`'s getters/setters) call unconditional thin wrapper
   functions declared in the alarm module's own header — exactly how `chime.c` already calls
   `board_hal_has_speaker()`/`board_hal_play_beep_pattern()` unconditionally today regardless of
   board, with the `#ifdef` living inside the callee, not at every call site.
4. New `alarm_clock_available` boolean on `GET /api/config`, mirroring the existing
   `chime_speaker_available`/`climate_sensor_available` fields, so the webapp can hide the entire
   Alarm settings tab on a build without the feature compiled in at all (not just individual
   controls within it).

**One real limitation to flag**: the webapp's JS bundle is built identically for every board today
— there is no existing mechanism to exclude part of the Vue bundle per feature/board (Chimes and
Climate both solve "not every board has this" by shipping their tab's code in every build and
hiding it at runtime via `v-if`, not by shrinking the bundle). Recommend following that same
established convention for the Alarm UI rather than introducing new webapp build-variant tooling —
the actual flash/RAM/compute savings this modularity is meant to protect live entirely on the
firmware (C) side, which the Kconfig approach above already fully addresses.

## Summary verdict

| Sub-feature | Feasible? | Effort | Notes |
|---|---|---|---|
| Alarm schedule(s), Web UI + Telegram config | Yes, straightforward | **Small** (~1 day) | Directly reuses the existing cron-rule pattern (`agenda_cron`) end to end |
| Alarm ringing (G4-C5-E5-C5 loop, adjustable duration, minimal wake) | Yes | **Small–Medium** (~1-2 days) | Reuses `audio_chime.c`'s tone/silence primitives almost as-is; the "no WiFi/rotation" wake path needs new, careful wiring |
| Button-based time-setting UI (long-press KEY enter/exit, BOOT=hours/KEY=minutes roll+batch, tone feedback) | Yes | **Medium** (~3-4 days incl. on-device tuning) | Finalized 2026-09-23: enter/exit/confirm/stop-ringing all use long-press KEY (genuinely free today), hours on short-press BOOT, minutes on short-press KEY, voice-enrollment entry on long-press BOOT while already in setting mode — **every gesture in the whole feature is now "one button, meaning depends on current state," no PMIC work and no simultaneous-chord detection needed anywhere** |
| Offline voice "Alarm off" detection (MFCC + DTW) | Feasible, but the biggest unknown | **Large** (1.5–3 weeks, open-ended) | The mic hardware (ES7210 ADC) exists on this board per the schematic, but **this firmware has zero driver code for it today** — this is a full new bring-up project, not a feature bolted onto existing code. Recommend treating as an optional, separate, later phase gated on a hardware-verification spike |
| Extension: show next alarm time as a display overlay | Yes | **Small** | Direct fit for `overlay_manager.c`'s existing badge-composition pattern (same convention as the climate temperature/humidity badges) |
| Build-time opt-in (whole feature only in firmware when requested) | Yes | **Small** (~half a day of scaffolding) | Directly reuses the exact Kconfig + `build.py`-flag + conditional-SOURCES pattern this project already ships for Chimes/per-board drivers — no new tooling concept needed |

Nothing here is a hard blocker. The scheduling and ringing pieces are cheap because they reuse two
already-mature subsystems (cron rules, tone synthesis). The button UI's remaining risk is now
purely in new state-machine logic (no hardware/PMIC gap left after the 2026-09-23 button
reassignment); the voice recognition is the one piece needing genuinely new hardware bring-up plus
a DSP pipeline this codebase has never had any equivalent of.

## 1. Alarm scheduling — reuse the existing cron infrastructure directly

`main/cron.c`'s `cron_rule_t` (minute/hour/day-of-week bitmasks, parsed from a 3-field string like
`"0 7 1-5"` = 07:00 Mon–Fri) already expresses exactly "one or more repeating times, on an
adjustable subset of weekdays" — the default-workdays requirement falls straight out of this
without any change to `cron.c` itself.

`config_manager.c` has this storage pattern implemented **twice already** (`rotate_cron` and
`agenda_cron` — matching fixed-size string arrays, NVS persistence, a memoized `cron_parse()`
cache invalidated on change, `config_manager_set_*_cron_rules()`/`get_compiled_*_cron_rules()`
pairs). A third `alarm_cron` set is a close-to-mechanical copy of that block: new NVS key(s), same
load/persist/get/set/compile functions, a new options array + cron-rule UI in
`SettingsPanel.vue` mirroring the existing Agenda schedule editor, and a new `PATCH /api/config`
field following the same pattern the config-import robustness fix (see `docs/DIFF.md`) just made
safe for. A Telegram command (`/alarm 07:00 Mon-Fri`, or similar) is a thin wrapper around the same
setter, following `telegram_bot.c`'s existing command-dispatch pattern (it already calls into
`album_manager_set_album_enabled()` etc. from a bot command handler today).

**Effort: small.** This is the lowest-risk piece of the whole feature and can be built and tested
independently of everything else (including the physical-button UI — the cron rule(s) can be fully
useful via Web UI/Telegram alone, with the button UI as an enhancement layered on top later).

## 2. Alarm ringing — mostly reuses `audio_chime.c`, needs a new "minimal wake" path

**Tone synthesis is already fully general.** `board_hal/src/audio_chime.c`'s `play_tone(freq_hz,
duration_ms, amplitude)` synthesizes an arbitrary-frequency sine wave on the fly (not fixed
melodies), and `i2s_write_silence()` writes an exact silent gap. G4 (392 Hz) – C5 (523 Hz) – E5
(659 Hz) – C5 (523 Hz), 300 ms each, then a 5 s gap, is a straightforward new orchestration
function sitting next to the existing `play_beep_pattern_tones()` (which only knows 3 fixed
severity patterns today) — **no new low-level audio code needed**, only a new sequencing loop.
- The project's own hard-won 250 ms PA-settle-delay fix (`audio_session_open()`) must be preserved
  — don't shorten it "to make the first alarm beep punchier."
- `audio_session_open()`/`_close()` currently opens/closes I2S+ES8311 per call; a minute-long
  repeating alarm should keep one session open for its whole duration (loop tones+silence inside
  it) rather than reopening every phrase.
- **Abort-on-long-KEY-press is new**: today's `board_hal_play_beep_pattern()` always runs to
  completion with no early-exit hook. The alarm loop needs to poll for (or be signaled by) a long
  KEY-press mid-playback — a small but real addition to the button task's existing responsibilities.
- Web-UI-adjustable duration (default 60 s) is a plain new config field, same shape as
  `chime_volume` (which is also directly reusable as the alarm's own volume, if desired).

**Minimal-wake (no WiFi/rotation/etc.) is an already-proven pattern, not a new concept** — WiFi
bring-up on wake is already conditional in `main.c` (skipped for a plain rotation-only wake in
several existing configurations). An `alarm_wake` boolean, threaded through `main.c` the same way
`agenda_wake` is today, needs to explicitly **not** set any of the flags that currently bring up
WiFi — this is the one place needing care, since `agenda_wake`'s existing behavior is the opposite
(it always wants WiFi for Calendar/ToDo/weather fetches) and is not a template to copy verbatim
here, only structurally similar.

**RTC wake precision**: the wake-time architecture (internal timer computed from
`cron_seconds_until_next()`, wall clock corrected from the external RTC every wake, an existing
5-second "woke up too early, go back to sleep" safety margin, and an existing >30s-drift-triggers-
resync policy) is already built for "fire close to a wall-clock target," and should comfortably
deliver "rings within about a minute of 07:00" without new timing infrastructure.

**Effort: small–medium.** Most of the audio work is reuse; the wake-path wiring is the part that
needs a careful read-through of `main.c`'s existing wake-reason branches to avoid regressing
rotation/agenda wakes while adding a third kind that deliberately skips network bring-up.

## 3. Button-based alarm-setting UI — finalized button assignment (2026-09-23)

**Buttons that exist today** (`board_waveshare_photopainter_73.h`):
- **BOOT** (`GPIO_NUM_0`) — 50–3000 ms press resets the auto-sleep timer (unchanged);
  ≥3000 ms press is already claimed by this session's own offline-hotspot toggle
  (`main.c:207-215`).
- **KEY**/rotate (`GPIO_NUM_4`) — 50–3000 ms press triggers rotation (unchanged); **≥3000 ms is
  completely unclaimed today** (confirmed by reading `main.c:227-235` — there is no `else if
  (duration >= 3000)` branch for KEY at all, unlike BOOT).
- **CLEAR** — not wired on this board at all (`GPIO_NUM_NC`).
- **PWR** — no board_hal GPIO exists for it at all; only reachable via unused AXP2101 PMIC IRQ
  plumbing (see below — no longer needed after the design decision below).

**Finalized design** (resolves every open question from the first draft of this document):
- **Enter / exit+confirm alarm-setting mode: long-press (3s) KEY**, context-dependent on whether
  the device is already in setting mode — mirrors exactly how BOOT's own long-press already means
  different things depending on state (nothing new architecturally, same pattern applied to a
  second button). This reuses KEY's genuinely free ≥3000 ms slot, so **there is no collision with
  the existing BOOT-hold hotspot toggle at all** — the original conflict is gone, not worked around.
- **Stop a ringing alarm: short press KEY** (changed 2026-09-25 from the original 3s long press —
  a tap is the natural gesture half asleep). A press that starts while the alarm rings is consumed
  entirely by the alarm module (`alarm_manager_key_pressed()`/`_key_swallowed()`): it triggers neither
  the normal image rotation on release nor the ≥3s alarm-setting entry. KEY's falling edge is
  latched by a GPIO interrupt so a quick tap between two polls of the ring loop is not missed
  (deep-sleep timer wakes have no `button_task`, only this module sees the key).
- **This removes the PWR-button PMIC-IRQ work item entirely** — the original design's "confirm via
  PWR" would have needed new AXP2101 IRQ enable/handling that doesn't exist today; using KEY for
  confirm instead means this feature needs **zero PMIC driver changes**. This is a genuine
  reduction in scope/risk, not just a renamed button.
- **Hour increment: short-press BOOT** (rolling single-step or rapid multi-press batch-count, per
  the original spec). **Minute increment: short-press KEY** (same rolling/batch behavior, in
  10-minute steps). Two independent counters, one per physical button, incremented in whatever
  order/interleaving the user taps them — this avoids needing any separate "confirm this field,
  advance to the next" gesture that a single shared increment-button would have required, which
  makes the overall state machine *simpler*, not just differently assigned.
- **Consequence needing an explicit guard**: while alarm-setting mode is active, BOOT's own
  existing ≥3000 ms hotspot-toggle action must be suppressed (checked against "is setting mode
  active" before dispatching that branch) — otherwise a rapid string of BOOT taps for hour-counting
  could occasionally be misread as a 3s hold and toggle the offline hotspot by accident. Small,
  contained fix at the same `main.c:207` branch point.
- **Rapid-vs-slow press counting** (needed independently on *both* BOOT-for-hours and KEY-for-
  minutes now) does not exist anywhere today and needs a genuinely new state machine — tracking
  inter-press intervals against a threshold that will need empirical, on-device tuning (as the
  original request itself anticipated) rather than a value guessable in advance. The existing
  50ms-poll/edge-detect button task already timestamps press/release, so this is additive logic,
  not a rewrite — just needed twice (once per button) instead of once.
- **The confirmation-beep-while-still-held requirement is unchanged by any of the above**: today's
  button task measures duration only **on release** (`main.c:201-202`, `227-228`), but the spec
  wants an audible cue at the moment the 3s threshold is *crossed*, while the button is still down,
  so the user knows exactly when to let go. This needs the poll loop to check elapsed hold time on
  every tick while a button is down (not just once on release) — a real, if small, change to the
  current measurement approach, independent of which button ends up using it.
- **AUFNAHME-MODUS entry — changed again (2026-09-23): long-press (3-5s) BOOT while already inside
  alarm-setting mode**, replacing the original two-button simultaneous KEY+BOOT chord. This reuses
  the exact slot the BOOT-hotspot-suppression guard above already frees up while inside setting
  mode — instead of that long BOOT hold being suppressed into doing nothing, it's given a real
  meaning specific to that context. Net effect: **the simultaneous-chord detection capability is no
  longer needed at all** — every gesture in the whole feature (KEY-long for enter/exit/stop-
  ringing, BOOT-long for hotspot-vs-enrollment) is now "one button, meaning dependent on current
  state," a pattern the existing BOOT-hotspot code already established, applied consistently
  everywhere instead of needing one special two-button case.
- **Follow-up detail worth deciding together with this**: the alarm-setting mode's own ~10s
  inactivity timeout (discard if no action) and the 3-5s hold plus 5s listening window for
  enrollment together consume nearly all of that budget — entering/returning from enrollment should
  reset the parent setting-mode's inactivity timer, or there may be too little time left to then
  also confirm the alarm. This applies equally regardless of which gesture triggers enrollment, so
  it isn't a consequence of today's change specifically, just newly worth deciding alongside it.
- **LED (ACT/green) conflict — resolved**: confirmed via a repo-wide grep that `BOARD_HAL_
  LED_ACTIVITY` (green) has exactly one consumer on this board, `power_manager.c`'s 200ms/10s
  auto-sleep countdown blink (`power_manager.c:219-221`) — no other file touches it. The decision
  to simply suppress that blink while voice-enrollment mode owns the LED is a single added
  condition at that one call site, fully resolving the conflict with no remaining unknowns.

**Effort: medium**, now entirely software (no PMIC/hardware IRQ work), dominated by (a) the new
multi-mode button state machine (setting-mode entry/exit, independent hour/minute roll-or-batch
counters, per-step tone feedback, timeout/discard), (b) the switch from release-measured to
live-during-hold duration tracking for the "beep at 3s while still held" cue, and (c) the new
simultaneous-chord (KEY+BOOT together) detection for voice enrollment — all of which will need real
on-device iteration (the press-speed threshold especially) rather than being purely a desk exercise.

## 4. Offline voice "Alarm off" detection — feasible, but a genuinely separate, large project

**Hardware**: the schematic shows a dedicated ES7210 4-channel mic-ADC chip (separate from the
ES8311 DAC used for chimes today) with mic bias/differential-pair wiring, and Waveshare's own
reference Arduino sketch (`01_Audio_Test.ino`, fetched earlier this project for the chime bring-up)
demonstrates a **working record path on this exact board** (`CodecPort_SetInfo("es8311 & es7210",
...)`, `CodecPort_SetMicGain()`, triggered by a double-press of BOOT in their example). The
schematic's own trace annotations suggest only one of the four ES7210 channels may actually be
populated with a physical mic element (the other three show "NC" on their coupling caps) — worth a
quick real-hardware confirmation (a multimeter continuity check or just trying a record + playback
round-trip) before committing engineering time, the same "verify against real hardware before
trusting a schematic/register-level assumption" lesson this project already learned once during the
ES8311 speaker bring-up.

**Software**: this firmware's `board_hal` has **zero ES7210 driver code today** — the I2S RX line
(`din`, wired in hardware per the pin table) is explicitly left `I2S_GPIO_UNUSED` in the current
I2S config. Implementing voice detection means:
1. A **new ES7210 I2C bring-up driver** (register sequence, mic gain, sample-rate/format config) —
   symmetric, comparable-sized new work to what `audio_chime.c` already did for the ES8311 speaker
   side, likely with the same "chip ACKs everything but produces nothing useful until the exact
   right register sequence is found" risk this project hit once already, i.e. cross-check against
   Waveshare's own working reference rather than a generic ES7210 datasheet sequence.
2. Enabling I2S RX (currently disabled) — the DMA-buffer-flush requirement the spec itself calls
   out (clearing residual chime audio from the buffer right before the 5 s listening window starts)
   is a real, correct concern for a shared I2S peripheral doing both TX (chime) and RX (mic) — needs
   explicit handling, not just enabling both directions and hoping.
3. A **new MFCC feature-extraction + DTW matching pipeline** — nothing like this exists in the
   codebase today (no audio/DSP dependency is currently vendored — `idf_component.yml` pulls in
   only `esp_jpeg`/`mdns`/`cjson`/`libpng`/`qrcode`/`littlefs`). Two realistic paths:
   - **Hand-rolled MFCC+DTW** exactly as specified: for a single ~1-2s utterance at 16kHz
     (~100-200 frames, 13-20 MFCCs/frame), this is comfortably within this board's compute/memory
     budget (dual-core 240MHz Xtensa LX7, 8MB PSRAM) — a few KB per stored template, tens of
     thousands of operations per DTW comparison, real-time with headroom. Espressif's `esp-dsp`
     component (optimized FFT/filter primitives for Xtensa) would meaningfully speed up the FFT/mel
     step and is a much smaller new dependency than the alternative below.
   - **`esp-sr`** (Espressif's speech-recognition framework: AFE front-end with noise
     reduction/VAD, WakeNet/MultiNet DNN models) is heavier, brings in a large new dependency, and
     is arguably overkill for a single fixed enrolled phrase — but its AFE/VAD could reduce false
     triggers from ambient noise more robustly than a from-scratch DTW threshold, at the cost of
     substantially more integration effort and flash footprint. Not recommended as a first attempt.
4. **Enrollment-mode UX**: the spec's "hold KEY+BOOT together for 2s to arm a 5s recording window,
   green LED solid during it" is buildable on the same button/audio infrastructure being built for
   items 2-3 above, but is additional new state-machine surface on top of the alarm-setting mode's
   own state machine — worth scoping as its own small deliverable once the underlying record/DTW
   pipeline works standalone (e.g. testable via a debug HTTP endpoint before any button wiring
   exists for it).

**Timing constraint check**: the spec's "listen only in the 5 s pauses between 4×300ms notes" is
compatible with a single-template DTW match (a 1-2s recognition pass fits inside a 5s window with
room to spare, even before optimizing), so the core timing idea is sound — the risk here is entirely
in getting reliable low-noise mic capture and enrollment quality, not in fitting the compute into
the time budget.

**Effort: large, and the least predictable estimate in this document** — realistically 1.5-3 weeks
of focused work including hardware bring-up, plus non-trivial risk that recognition accuracy in a
real bedside/night environment (fabric-muffled mic proximity, room echo, the enrolled speaker's
voice varying with grogginess right after waking up) needs iteration beyond the first working
version. Recommend scoping this as an explicitly optional, separately-shippable phase — the alarm
clock is fully useful (schedule + button/Web UI arm + ring + physical-button stop) without it.

## 5. Phase 4 design: show the next alarm time as a display overlay (researched 2026-09-24)

Idea: optionally show the armed alarm's time on the display, toggled from the Web UI, shown only
when an alarm is actually armed. The user specifically flagged that on their hardware, a photo
render with weather + climate badges already fills the entire top row — so this needs a real survey
of what's free in every render mode, not just "put it near the other badges." That survey (an
Explore-agent pass over `overlay_manager.c`/`image_processor.c`/`agenda_renderer.c`, exact
citations kept below) found two very different situations depending on render mode, so this needs
**two separate integration points**, not one shared element.

### Photo/rotation mode (`overlay_manager.c` + drawing code in `image_processor.c`)

Confirmed corner-by-corner (every draw call in `image_processor_add_overlay_to_file()` read):
- **Top-left**: low-battery badge (`image_processor_draw_battery_badge()`, boxed at `x=0,y=0`).
- **Top-right**: climate temp/humidity badges (`draw_one_climate_badge()`, right edge pinned to the
  display's right edge).
- **Top edge, full width**: weather/headlines bar (`image_processor_draw_overlay_bar()`,
  `anchor_top=true`) - already narrows its own `usable_width` by a `right_margin_px` reserved for
  whichever climate badges are about to draw, truncating its own text ahead of them
  (`image_processor.c` ~4286-4300) - this is the exact existing precedent for "reserve room, shift/
  truncate what's already there," and confirms the user's own observation: with both weather/
  headlines and climate enabled, the *entire* top edge is already claimed.
- **Bottom, full width, but only the drawn text is centered, not corner-anchored**: the EXIF/
  capture-date caption and the "no internet" error banner both go through the same bottom-anchored,
  centered `render_text_bar()` call.
- **Bottom-left and bottom-right are never touched by anything today, in any configuration** -
  confirmed by reading every draw call in this file, not inferred.

**Recommendation**: a new bottom-left badge, visually mirroring the existing top-left battery badge
(same box/font/padding shape, just the opposite bottom corner) - "next alarm 16:40" (or similar).
Since the caption/error-banner text is *centered across the full width* rather than corner-anchored,
a sufficiently long caption could still visually reach into the bottom-left corner even though
nothing is nominally drawn there - so this needs the same "reserve a margin, narrow the other
element's usable width" treatment the top edge already has, just mirrored to the left: the caption's
`render_text_bar()` call would need a `left_margin_px` alongside its existing right-margin support
(today `render_text_bar()`/`image_processor_draw_overlay_bar()`'s margin narrowing is only
implemented for the right side, for the top bar's own climate-badge accommodation - extending it to
also narrow from the left, and re-centering the caption's text within that narrower window rather
than the full width, is the one piece of that primitive that doesn't already exist and would need
building, not just reusing).

### Agenda mode (`agenda_renderer.c`)

This is the harder case, and the survey's most important finding: **unlike photo-overlay mode,
Agenda mode has no corner that is unconditionally free in every configuration.**
- **List mode** (Calendar-only or ToDo-only) and both **dual layouts** (stacked/side-by-side): the
  header (32px, full column width) already chains climate chip (top-right of that header) and
  timestamp (immediately left of it, omitted entirely if there's no room - `docs/DIFF.md` already
  documents this exact fallback). Below the header, the day/event list fills downward row by row -
  bottom corners are only free when the list happens to be *shorter* than its row budget, which is
  data-dependent, not something the layout guarantees.
- **7-day Grid mode** (Calendar-only, full screen): the content area below the header is tiled
  edge-to-edge by day-cells regardless of how much data each day has - confirmed **zero free space
  anywhere in the content area, ever**, independent of data. The grid's own header strip (same 32px
  bar as List mode) is unaffected by this and still exists identically above the grid.

**Recommendation**: don't try to find a spare corner in Agenda mode at all - extend the **existing
header chain** instead, the same one climate/timestamp already use
(`agenda_renderer.c`'s `draw_header_climate()` already returns its own left edge specifically so
"callers use the return value to place their own right-aligned header content... immediately to its
left" - a new alarm-time element is one more link in that same chain, e.g. climate chip → alarm time
→ timestamp, reusing the identical measure-width-then-subtract pattern every element there already
uses). This works uniformly across List, Grid, ToDo-only, and both dual layouts, because it only
ever touches the header strip (which exists in all five combinations) and never the content area
(which has no guaranteed free space in several of them, and none at all in Grid mode). Apply the
same two rules the climate chip/timestamp already use: shown in only one header at a time (the one
that already shows climate - top when stacked, Calendar's own header when side-by-side, matching
the existing de-duplication logic exactly), and omitted entirely (not truncated/overlapped) if
there's no room left once the other header content is placed.

### Cross-cutting notes

- **Board size**: `BOARD_HAL_DISPLAY_WIDTH`/`HEIGHT` are runtime values (`epaper_get_width/height()`),
  not a fixed 800x480 - confirmed other supported boards use 1200x1600 and ~1872x1404. Any new
  element must position itself from these, exactly like every existing overlay/header element
  already does - no board-specific hardcoding needed or present today.
- **Time format**: every existing on-device timestamp (Agenda header, config/log timestamps) uses
  24-hour format exclusively - there is no 12-hour/AM-PM rendering anywhere in the firmware today
  (only a client-side `hour12()` helper in the webapp's own cron-schedule *description* text, which
  never reaches the device's display). Recommend 24-hour ("16:40") for consistency with everything
  else already on screen, rather than introducing the device's first-ever AM/PM text rendering for
  just this one small element - happy to do 12-hour instead if preferred, but flagging that it would
  be a new, one-off convention.
- **Both toggles gate on the same conditions**: only visible when `alarm_clock_available` (the build
  actually has the feature) AND a new, separate Web UI switch (e.g. "Show next alarm on display,"
  meaningful only when the Alarm Clock tab itself is visible) is on AND at least one alarm is
  currently armed (`alarm_cron` non-empty) - showing nothing at all otherwise, matching the "nur
  falls eingestellt" framing of the request.

**Effort: small-medium** - reuses established primitives/conventions almost entirely; the one
genuinely new piece of plumbing is the caption's left-margin support for the photo-overlay case
(the Agenda-mode case needs no new plumbing at all, just one more chained header element). Two
separate, independent integration points (photo-overlay vs. Agenda header) rather than one shared
implementation, but both follow patterns this codebase already has working examples of.

## Energy budget

- **Arming/checking the alarm each wake**: identical cost class to the existing rotate/agenda wake
  cron check already running today — negligible additional energy over the status quo, since it's
  the same "wake, check cron table, decide to sleep or act" cycle, just with a third cron table
  consulted.
- **Ringing**: continuous ES8311+speaker playback for up to the configured duration (default 60s)
  is the same order of magnitude per second as an existing Chime event, just run continuously
  instead of once — a full duty-cycle draw for that duration, unavoidable for an audible alarm.
  Comparable to (roughly 60s ÷ a single chime's fraction-of-a-second duration) an existing chime
  firing continuously rather than once — worth an on-device current-draw measurement once
  implemented (this project already has the tooling/precedent for that from the original Chimes
  bring-up), but not expected to be a battery-life-defining feature on its own for an
  occasional/daily alarm.
- **Voice listening** (if implemented): I2S RX + MFCC/DTW compute only runs inside the alarm's own
  active-ringing window (mic is never listening while the device is otherwise asleep), so its
  energy cost is bounded to "however long the alarm rings for," not an always-on background drain.

## Compute/memory budget

- **Scheduling**: negligible — a few more bytes of NVS/RAM for one more small cron-rule array,
  reusing already-compiled cron-matching code.
- **Ringing**: negligible beyond what `audio_chime.c` already costs today (same I2S/ES8311 path,
  just a longer/looping sequence).
- **Button state machine**: negligible — a few more bytes of state, same 50ms poll loop.
- **Voice recognition** (if implemented): a few KB PSRAM per enrolled template, tens of KB flash
  for a hand-rolled MFCC+DTW implementation (more if `esp-sr` is used instead), real-time on one
  CPU core with the second core free for I2S/other work — comfortably within this board's 8MB
  PSRAM/240MHz-dual-core budget, not a limiting factor either way.

## Open design questions

1. ~~BOOT 3s-hold collision~~ — **resolved 2026-09-23**: enter/exit+confirm moved to long-press KEY
   (genuinely free today), leaving BOOT's hotspot toggle untouched outside setting mode.
2. ~~PWR button semantics~~ — **resolved 2026-09-23, and simplified further**: confirm/exit also
   moved to long-press KEY, so PWR/PMIC-IRQ work is no longer needed for this feature at all.
3. ~~Fast-vs-slow press threshold~~ — **resolved 2026-09-24, by removal**: real on-device testing
   showed the batch mode didn't work reliably, and the user asked for it to be dropped rather than
   tuned further. Every press now confirms immediately with its own beep(s), matching only the
   original spec's slow-press behavior.
4. ~~LED arbitration~~ — **resolved 2026-09-23**: confirmed the green LED has exactly one existing
   consumer (the auto-sleep countdown blink); suppressing it during voice-enrollment mode is a
   single added condition with no other interactions to consider.
5. **Mic hardware confirmation**: still open — verify a physical mic element actually responds on
   this specific board/batch before investing in the ES7210 driver + MFCC/DTW pipeline. The
   user's own proposed mic-loopback-and-level-meter test spike (record → play back through the
   speaker, and/or a live level meter on the serial console) is exactly the right-sized way to
   answer this — and doubles as the first working milestone of the larger ES7210 driver effort
   rather than being throwaway test code.
6. ~~BOOT-hotspot suppression while in alarm-setting mode~~ — **resolved** (Phase 3 implementation):
   `main.c`'s BOOT long-press branch checks `alarm_setting_ui_is_active()` before calling
   `toggle_ap_hotspot_mode()`, so a rapid string of hour-count taps can never be misread as the
   unrelated hotspot-toggle hold.
7. ~~Simultaneous KEY+BOOT chord detection~~ — **resolved 2026-09-23**: enrollment entry moved to a
   long-press BOOT while already inside alarm-setting mode (reusing the slot BOOT-hotspot-
   suppression already frees up there), so no cross-button simultaneous-press detection is needed
   anywhere in this feature at all.
8. **New: enrollment sub-mode should reset the parent alarm-setting mode's ~10s inactivity
   timeout** — the 3-5s hold plus 5s listening window otherwise consumes nearly the whole budget,
   leaving little time to confirm afterward.

## Recommended phasing

0. **Build-time scaffolding**: `ALARM_CLOCK_ENABLED` Kconfig option + `build.py --alarmclock` flag
   + empty `alarm_manager.c`/`.h` wired into `main/CMakeLists.txt`'s conditional SOURCES and one
   `alarm_clock_available` capability field — a thin skeleton with no real behavior yet, so every
   later phase is built *inside* the already-modular structure from day one instead of retrofitting
   it in afterward.
1. **Alarm schedule storage + Web UI + Telegram command** (reuses cron infra) — ships value alone.
2. **Alarm ringing + minimal-wake path**, armed/disarmed purely via Web UI/Telegram for now — the
   alarm clock is fully functional end-to-end at this point without touching button UI at all.
   Includes the long-press-KEY-stops-ringing gesture.
3. **Physical-button time-setting UI** (long-press KEY enter/exit, short-press BOOT=hours/
   KEY=minutes, BOOT-hotspot suppression guard, live-during-hold duration tracking for the
   confirmation beep, tone feedback) — the on-device physical convenience layer, now pure software.
   Include the long-press-BOOT-while-in-setting-mode hook point for enrollment here even if
   phase 6 hasn't landed yet, so the gesture has somewhere to attach later without revisiting this
   phase's state machine.
4. *(optional, small, independent of 3-5)* **Display overlay showing the next armed alarm time**
   — can land any time after phase 1's alarm-cron storage exists.
5. **Mic hardware verification spike** (record-and-playback loopback and/or a live serial level
   meter) — cheap, de-risks the decision to invest further before starting the full pipeline below.
6. **Offline voice "Alarm off" detection** (ES7210 driver, MFCC+DTW, enrollment mode, KEY+BOOT
   chord detection, LED arbitration) — optional, separate, gated on phase 5's result; can slip
   independently without blocking a shippable alarm clock.
