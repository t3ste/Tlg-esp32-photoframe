# Changelog

All notable changes to this fork are documented here. See [README.md → Changes from Upstream](README.md#changes-from-upstream) for the full running list of everything this fork adds on top of [aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe); this file covers per-release deltas only.

## [Unreleased]

### Added

- **Stop the ringing alarm by voice** (Alarm Clock firmware on boards with speaker **and** microphone - currently only `waveshare_photopainter_73`): teach the frame a short word (Alarm tab, up to 5 examples), switch "Stop the ringing alarm with the stop word" on, and the ringing alarm listens in the 5 s pauses between its notes and stops when it hears the word; the KEY button keeps working, and without a taught word or when the microphone can't be opened the alarm rings as before. While a note sounds (plus 250 ms echo) the audio is muted for the recogniser, so the alarm can't trigger itself. New endpoints `PUT /api/kws/settings` (`alarm_stop`) and `POST/GET/DELETE /api/alarm/test` (ring now / state and how the last ring ended / stop) with a "Ring now" button in the Alarm tab for trying it. Host-tested (`alarm_pattern`, alarm-shaped audio streams in `test_kws`).
- **Voice features belong to the Alarm Clock firmware**: the microphone monitor, level meter, self-test and stop-word recognition are compiled in only with `CONFIG_ALARM_CLOCK_ENABLED` on a board with speaker and microphone (`BOARD_HAL_VOICE_ENABLED`); every other firmware contains none of it (checked: no such symbol in a regular `waveshare_photopainter_73` build). A board with a speaker but no microphone would get the plain alarm (none exists today). `microphone_available` in the config is replaced by `voice_available`; the Web UI shows the microphone tools and the voice-stop settings in the Alarm tab (formerly Maintenance) only when it is true.
- **Stop-word recognition** (first steps, now used by the alarm): the frame learns a word from a few spoken examples and recognises it again. Small self-contained C module (`main/kws.c`, host-tested): voice-activity segmentation, MFCC features, DTW template matching (absorbs different speaking speeds), threshold derived from how much the enrolments vary; nothing but the feature templates is stored (on the storage), no audio. Device side `main/kws_service.c` with `POST /api/kws/enroll`, `POST /api/kws/test`, `GET /api/kws/status`, `DELETE /api/kws/templates` and `scripts/kws_tool.py`. Speaker-dependent by design (enrol with your own voice on the frame). Tested against text-to-speech samples of the word at several speeds plus ten other words, incl. the hard look-alikes "Stock", "Spott" and "Stoppuhr" (`scripts/generate_kws_test_audio.ps1` regenerates the samples). Also verified end to end on a frame with the PC playing the samples through its speakers (`scripts/kws_playback_test.py`: enrol 4 examples, then 10 words): every "Stopp" variant accepted in most runs (the fastest one is occasionally missed by a hair) and no other word ever accepted; 30 s of silence produce no events. Known limits: enrolment and use should happen in a similarly quiet room; a minimal pair such as "Stock" is rejected with a modest margin (distance about 7.7+ against a threshold around 6.5).
- **Adjustable microphone sensitivity with a live level meter** (Alarm tab; Alarm Clock firmware on a board with a microphone): a "Live level" switch makes the frame listen and shows both microphones as bars with markers for the running **noise floor** (blue) and the **threshold** (red), so the threshold can be chosen from what the room actually looks like. The threshold is either automatic (noise floor + 20 dB, at least -45 dBFS) or a fixed value (slider, -90 to 0 dBFS); stored on the device (`GET/PUT /api/mic/settings`) and used by the speaker/microphone self-test and any other listener. `GET /api/mic/level` reports the live levels, floor and threshold; `DELETE` stops a running monitor.
- **Microphone level monitor** (`waveshare_photopainter_73`, first step towards voice control): the onboard microphones sit on an **ES7210** ADC (not on the ES8311), which the firmware now initialises the same way Waveshare's stock example does. `POST /api/mic/level?seconds=N` (or the Alarm tab's "Log microphone level" button) prints the input level to the console / debug log five times a second as a bar plus RMS and peak in dBFS; `GET /api/mic/level` reports the last level. Nothing is recorded or stored. `voice_available` in the config tells the UI whether this firmware has the voice features.
- **OTA release channel + firmware variant** in the Web UI's Updates tab: "Stable" (default) or "Include pre-releases", and — on boards with a speaker — an "Alarm Clock firmware" option to switch a device between the regular and the Alarm Clock build (same version is offered when only the variant differs; an older release is never offered as a "switch"). Stored on the device (`GET/PUT /api/ota/options`); the update status reports `latest_prerelease` and `variant_switch`.
- **Web Flasher**: a **Pre-release** option (the newest published pre-release that is newer than the stable release; its firmware is hosted on the Pages site, since release assets can't be fetched cross-origin), also with the Alarm Clock option. A published pre-release no longer shows up as "Stable" in the flasher.

---

## [Unreleased]

### Added

- **Speaker + microphone self-test** (`waveshare_photopainter_73`): plays a tone sequence (four beeps with pauses, 100 % volume) on the speaker while the microphone listens and counts the bursts against the measured noise floor (at least 20 dB and -45 dBFS above it). Both onboard microphones are read now (left = MIC1, right = MIC2; Waveshare's stock MIC1 + MIC3 pairing left the right slot silent). Same frame (`POST /api/mic/level?seconds=8&tones=1`, also an Alarm-tab button) or two frames (`POST /api/mic/tones` on the speaker frame while the other one listens). `GET /api/mic/level` now reports the result; `scripts/mic_selftest.py <mic-host> [--speaker <host>]` runs either variant and exits 0/1.

---

## [v218.5.0] - 2026-09-25

### Changed

- Alarm Clock: a **short KEY press** now stops a ringing alarm (was a 3 s hold), and that press no longer also changes the picture or opens the alarm-setting menu

### Fixed

- Battery/Climate history backups no longer overwrite an earlier backup file that covers the same date range (a `_2`, `_3`, ... suffix is used instead)

### Added

- Web Flasher: an **Alarm Clock** option (shown for boards that support it, currently `waveshare_photopainter_73`) flashes the alarm firmware variant. Dev builds always offer it; Stable appears once a full (non-pre-)release ships the alarm binary. Driven by a new `alarmclock` flag in `boards/boards.json` and `generate_manifests.py --variant alarmclock`.

---

## [v218.4.0] - 2026-09-25

### Added

- Upstream integration: all 25 commits from `aitjcize/esp32-photoframe` since the last sync are now in this fork (15 as-is, 7 with documented adaptations, 3 already present) — includes optional HTTP-API password protection with brute-force lockout, IANA time zone picker, bounded WiFi connect time on a bad network, network-failure backoff between unattended wakes, fetch fixes (ETag only after the picture is shown, no retry on 4xx, keep the current picture when the URL fetch fails), and several e-paper/battery fixes. The fork's own cold-boot WiFi credential-protection logic was deliberately kept.
- **M5Stack M5Paper v1.1** board support (plain ESP32, SHT3x sensor) — 8 supported boards
- **Alarm Clock** (opt-in), previously only buildable from source, now also released as a ready-made binary: `photoframe-firmware-waveshare_photopainter_73-alarmclock-merged.bin` for the Waveshare PhotoPainter (the only supported board with a speaker). All other boards ship without the feature. See [README](README.md#features).
- Everything else from the `alarmclock` branch: Chimes/Climate hardware-capability build gating, 7-day Agenda grid with C/D/E header badges, importable Calendar color profiles, optional self-signed HTTPS web UI, offline AP hotspot, "reprovision on WiFi failure" safety toggle

### Changed

- OTA updates on an Alarm Clock build fetch the matching `esp32-photoframe-<board>-alarmclock.bin` release asset so a self-update never silently drops the feature

### Fixed

- Config import: one invalid field (e.g. a bad password or time zone) no longer discards every other field of the same request

---

## [v218.3.0] - 2026-09-24

### Fixed

- GitHub Pages was never enabled for this fork's repository, so the README's "Try the Interactive Demo"/Web Flasher link was dead even though CI had been correctly building and pushing the site to the `gh-pages` branch all along — enabled
- The demo/web-flasher site itself would have failed to load once Pages was enabled: its Vite build had upstream's repo name (`esp32-photoframe`) hardcoded as the asset base path instead of this fork's (`Tlg-esp32-photoframe`), which would 404 every asset and firmware manifest
- Several links across the demo page, README, and `process-cli`'s README still pointed at the upstream repository instead of this fork (GitHub/License/Firmware links, the release-version fallback fetch, the interactive-demo link, the algorithm-comparison sample images, clone instructions) — corrected; a stale, unreferenced `docs/index.html` left over from before this fork's own demo build existed was removed
- `scripts/launch_demo.py` downloaded "stable" firmware from upstream's releases unconditionally instead of whichever repo the local `git remote` actually points at
- Removed `.github/FUNDING.yml` (pointed sponsorship at the upstream author)

### Added

- [docs/CHIMES_CLIMATE_OVERHEAD.md](docs/CHIMES_CLIMATE_OVERHEAD.md): measured flash-size cost of the Chimes/Climate features and how much of it this fork's build-modularity gating actually saves vs. pre-existing hardware-driver gating

---

## [v218.2.0] - 2026-09-16

### Added

- **Chimes** (`waveshare_photopainter_73` only — onboard ES8311 codec + NS4150B speaker amp, previously unused): short synthesized beep feedback for 7 events — photo rotated, Telegram photo received, low battery, WiFi reprovisioning, an overdue/due-today Agenda ToDo item, a successful firmware update, and WiFi/internet lost — each independently toggleable from a new **Chimes** settings tab
  - 3-way speaker mode (Off/Battery + mains/Mains-USB only), optional daily quiet hours, a 0-100% volume slider
  - The three "actionable" events (low battery, Agenda due, WiFi/internet critical error) repeat up to 5 times while their condition stays true, then reset once it resolves; the rest remain one-shot
  - See [docs/DIFF.md](docs/DIFF.md) for the full ES8311/NS4150B hardware bring-up notes
- **Climate**: temperature/humidity monitoring via the onboard SHTC3 sensor — generic, available on any board whose sensor actually answers, not tied to one specific board
  - 5 preset room-type comfort profiles (Living Room/Office, Bedroom, Bathroom, Kitchen, Basement), classifying temperature and humidity independently as Bad/Good/Super; Celsius or Fahrenheit display
  - Optional top-right photo-overlay badges and an Agenda-header readout, both colored by category
  - New **Climate History** chart (alongside Battery History), with Y-axis gridlines and a manual reset
  - A reading is logged on every wake (not only when an image is actually displayed) plus every ~6 minutes while the device stays continuously awake, throttled to at most once every 5 minutes
  - User-settable calibration offset (°C/°F and percentage points) for hardware that reads consistently high/low
- Config-backup export can now optionally include the write-only ToDo/Calendar URLs (which can carry an embedded credential) alongside the existing credentials checkbox — previously these could never be recovered after a restore
- Battery/Climate History chart x-axis ticks now show a time (or day+time) instead of just a date once the visible history is short enough that a bare date would repeat across every tick

### Fixed

- Settings timezone field silently destroyed a DST-aware POSIX timezone (e.g. `CET-1CEST,M3.5.0/2,M10.5.0/3` for Amsterdam/Berlin) back to a fixed `UTC0` on *any* Settings save, not just an edit to timezone itself — the raw POSIX string is now the only source of truth, edited via a combobox (presets + free text) instead of a lossy numeric-offset field; the device's own time-keeping was never at fault
- Selecting a preset from that same new timezone combobox could silently fail to apply (typing a plain string worked fine) — Vuetify's combobox inconsistently emitted an object instead of a string on selection, which the backend silently rejected
- Gallery per-thumbnail delete button was undiscoverable (an unmarked 48px hover hotspot) and unusable on touch screens at all — now reveals on hovering/focusing the whole thumbnail and stays visible on touch devices
- Settings-save confirmation next to the Save button disappeared after 3 seconds, often too quickly to read — doubled to 6 seconds
- `build.py`/`CMakeLists.txt` could silently fail on a stock Windows + official ESP-IDF installer setup with no separate `python3` shim on PATH (resolving to the Microsoft Store stub) — version detection and partition-table generation now use the interpreter `idf.py` already provides; `build.py` also now prefers a real `idf.py.exe` wrapper when present
- Two latent bugs found while building the Climate feature above: an NVS key one character over its 15-character limit silently never persisted (a debounce timestamp resetting to 0 on every reboot instead of surviving it), and the HTTP server's handler-registration limit was exactly exhausted by the new endpoints, silently dropping the last-registered one
- Both the climate photo-overlay badges and the Agenda-header climate chip drew white text on the Good category's Yellow background, illegible on the actual e-paper panel — black text now used for that one case
- The climate overlay badges could visually overwrite the tail of the weather/headlines overlay bar instead of leaving room for it — the bar now reserves space and truncates with "…" ahead of the badges
- Enabling "Show thumbnails" on a large album could make the entire Web UI unreachable for a long time — a per-image `stat()` syscall used to check for a thumbnail blocked the device's single-threaded HTTP server for as long as a large album's directory scan took; now checked via one directory read collected into memory instead. The gallery grid also now renders a bounded batch of images at a time (with a "Load more" button) instead of every image in the album at once
- The external RTC read back a time exactly one hour ahead of true time whenever daylight saving time was active, self-correcting only until the next reboot — `mktime()` was being forced to assume standard time instead of working it out from the date; also affected local (non-UTC) Agenda Calendar event times

---

## [v218.1.0] - 2026-09-12

### Added

- **Agenda Mode**: full-screen ToDo (todo.txt-format URL) + Calendar (up to two ICS/iCal URLs, merged and sorted) mode on its own independent cron schedule — renders directly to the panel and skips the normal photo pipeline entirely for that wake
  - Per-element ToDo coloring (priority, `+project`/`@context` tags, due-date urgency) and per-calendar-source coloring (Calendar A/B each get their own color)
  - Day-grouped Calendar view with a divider per day; multi-day events shown compactly under every day they span
  - Configurable shared background (white/black/any hardware-supported color) with automatic fallback if a text color would otherwise match it
  - Landscape layout choice between stacked and side-by-side ToDo/Calendar columns; adjustable Calendar lookahead window (1-3 days)
  - Full per-role Web UI color picker for every ToDo/Calendar color (Spectra6/color boards)
  - Opt-in weather forecast annotation on each Calendar day divider (e.g. "Fri 11. [18/25 cloudy]"), independent toggle reusing the existing photo-overlay weather settings
  - See [docs/AGENDA_COLORS.html](docs/AGENDA_COLORS.html) for the full color scheme reference
  - Conditional GET (ETag) caching for both the ToDo and Calendar fetches — an unchanged source skips re-downloading on the next agenda wake and re-parses a small on-device cache instead
- Opt-in checkbox to include credentials (Telegram bot token, AI API keys, access token, custom auth header) in an exported config backup — off by default, since an export is a plaintext JSON file. WiFi password and Calendar/ToDo URLs can never be included, as the device never returns them at all

### Fixed

- WiFi captive-portal setup page's network scan could return zero SSIDs — root-caused via live log analysis to switching AP-only→APSTA auto-triggering a stale connection attempt that blocked the scan outright (`ESP_ERR_WIFI_STATE`); now explicitly cancelled before scanning (an earlier settle-delay-only fix had just masked this by lucky timing)
- A Calendar event's `VALARM` reminder block could overwrite the real event's title if the alarm itself carried its own `SUMMARY`
- TLS fetch failure against calendars whose certificate chain terminates at a cross-signed root (affects Google Calendar's current chain) — enabled cross-signed root verification in the mbedTLS certificate bundle
- A batched Agenda-settings save could race with a concurrent NVS write and drop part of the update — now saved as a single atomic batch
- `agenda_manager_run()` allocated its ToDo/Calendar working buffers on the deep-sleep-wake task's stack, overflowing it on every single Agenda wake — moved to PSRAM heap allocations
- Calendar day-divider weekday abbreviation ignored the configured overlay language setting
- Unsynchronized concurrent access to shared in-memory state in the display-history, per-album-enabled, and OTA periodic-check managers — each is now mutex-protected (or, for the album-list read path, snapshotted under a lock before parsing) against a genuine race between an HTTP request and a background task
- Three Settings-panel actions (save settings, save palette, factory reset) referenced the wrong variable name in their error handler, throwing an unhandled `ReferenceError` on any real failure instead of surfacing it
- Rotation-notify Telegram upload always failed for EPDGZ-format album images
- `process-cli`'s `/image` endpoint accepted client-supplied dimension headers without an upper bound, unlike the already-clamped `/thumbnail` endpoint — now clamped identically, preventing an oversized native canvas allocation
- `process-cli`'s face-crop engine could round a crop rectangle up to 1px past the source image's edge when independently rounding x/y/w/h — now re-clamped against the image bounds after rounding
- Agenda Mode only ever fired from the deep-sleep timer-wake path, so it silently never ran with Deep Sleep disabled (Home Assistant / always-on use) — the always-on rotation task now runs Agenda on its own independent schedule too
- A single failed WiFi connection attempt during the always-on cold-boot path immediately erased the saved SSID/password and restarted into captive-portal provisioning, even for a merely transient failure (router mid-reboot, brief congestion) rather than genuinely wrong credentials — now retries up to 3 times unless the AP's disconnect reason explicitly confirms rejected credentials (failed 4-way handshake/MIC failure/auth-fail), which still clears after one attempt as before
- Factory reset erased the Agenda ETag cache validators from NVS but left the matching `.agenda_*_cache.*` files behind on storage — now removed too
- The in-memory "current displayed image" path buffer (and a second local copy of it) were only 64 bytes, too small for a real absolute path with a long filename — confirmed live truncating one, which made the on-display error-overlay feature silently fall back to a blank canvas instead of overlaying onto the actual photo; both resized to 256 bytes
- 4 additional correctness bugs and 3 performance issues (redundant cron re-parsing, duplicated color-selection logic, excessive NVS commit calls) found and fixed during a full-branch code audit

### Security

- `agenda_todo_url` could be logged/persisted in a way that leaked it in plaintext — now treated with the same write-only handling as the WiFi password and Calendar URLs
- Exporting a device's config now also strips the Telegram bot token, AI API keys, access token, and custom auth header (previously only WiFi password and Calendar/ToDo URLs were excluded) — unless the new opt-in "include credentials" checkbox is used

---

## [v218.0.0] - 2026-08-28

First independent release of this fork. See [README.md → Changes from Upstream](README.md#changes-from-upstream) for the full feature list at this point.

[v218.2.0]: https://github.com/t3ste/Tlg-esp32-photoframe/compare/v218.1.0...v218.2.0
[v218.1.0]: https://github.com/t3ste/Tlg-esp32-photoframe/compare/v218.0.0...v218.1.0
[v218.0.0]: https://github.com/t3ste/Tlg-esp32-photoframe/releases/tag/v218.0.0
