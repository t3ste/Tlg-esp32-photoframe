# Changelog

All notable changes to this fork are documented here. See [README.md → Changes from Upstream](README.md#changes-from-upstream) for the full running list of everything this fork adds on top of [aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe); this file covers per-release deltas only.

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
- Opt-in checkbox to include credentials (Telegram bot token, AI API keys, access token, custom auth header) in an exported config backup — off by default, since an export is a plaintext JSON file. WiFi password and Calendar/ToDo URLs can never be included, as the device never returns them at all

### Fixed

- WiFi captive-portal setup page's network scan could return zero SSIDs — the AP→APSTA mode switch hadn't actually settled before the scan started; added a short settle delay plus an automatic retry on the setup page
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
- 4 additional correctness bugs and 3 performance issues (redundant cron re-parsing, duplicated color-selection logic, excessive NVS commit calls) found and fixed during a full-branch code audit

### Security

- `agenda_todo_url` could be logged/persisted in a way that leaked it in plaintext — now treated with the same write-only handling as the WiFi password and Calendar URLs
- Exporting a device's config now also strips the Telegram bot token, AI API keys, access token, and custom auth header (previously only WiFi password and Calendar/ToDo URLs were excluded) — unless the new opt-in "include credentials" checkbox is used

---

## [v218.0.0] - 2026-08-28

First independent release of this fork. See [README.md → Changes from Upstream](README.md#changes-from-upstream) for the full feature list at this point.

[v218.1.0]: https://github.com/t3ste/Tlg-esp32-photoframe/compare/v218.0.0...v218.1.0
[v218.0.0]: https://github.com/t3ste/Tlg-esp32-photoframe/releases/tag/v218.0.0
