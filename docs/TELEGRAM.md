# Telegram Bot Integration

A rotation mode that receives images directly via the [Telegram Bot API](https://core.telegram.org/bots/api),
independent of the existing SD-card and URL-fetch rotation modes. Unlike the companion
[esp32-photoframe-server](https://github.com/aitjcize/esp32-photoframe-server)'s Telegram source
(which relays through a separate server), this integration talks to Telegram directly from the
firmware — no additional server required.

## How it works

1. On every deep-sleep wake (or auto-rotate timer), the device long-polls Telegram's `getUpdates`
   endpoint for new messages.
2. Images sent as a **photo** or as a **file/document** are accepted, in any format the firmware
   already supports.
3. Telegram re-encodes compressed photos as **progressive JPEG**, which the firmware's decoder
   cannot read. The bot automatically falls back through Telegram's other resolutions of the same
   photo (largest → smallest) until one succeeds, or asks for the image as a file instead.
4. A caption sent with the image is drawn onto the photo as a text overlay (unless the caption
   itself is a `/`-command).
5. A preview thumbnail is generated from the raw download first (true colors, not the e-paper
   palette), then the image is converted to a processed, display-ready PNG (deleting the raw
   original by default), exactly like a manual Web UI upload — so it shows up correctly in the
   gallery and is a normal, rotatable album image, not just a one-off push. Optionally, the raw
   original can be kept instead of deleted (`/keep_originals`, see below).
6. `last_update_id` is persisted in NVS (+1 offset) so restarts never re-process old messages, and
   an allowlisted chat ID filters out unsolicited senders.
7. Filenames are short and collision-safe: `img_<unix-timestamp>.<ext>`.
8. If a poll doesn't result in a new image being displayed (nothing new arrived, or everything in
   the batch was a command / still waiting for its pairing partner), the device falls back to
   normal album rotation instead of leaving the previous image up indefinitely — the frame always
   changes image on every rotation-triggering wake, same as the non-Telegram modes. This fallback
   itself is togglable - see [Fallback rotation](#fallback-rotation) below.

**Large document uploads and the task watchdog**: a very large JPEG sent as a file/document (many
megapixels) can take long enough to JPEG-decode that it trips the ESP-IDF task watchdog
(`CONFIG_ESP_TASK_WDT_TIMEOUT_S`, currently 15s) — confirmed on real hardware for a ~4 MB,
3072×4080 px document upload. This isn't fatal in this project's configuration
(`CONFIG_ESP_TASK_WDT_PANIC` is off, so it only logs a warning and the decode finishes right after),
but it is a real, reproducible risk if panic-on-timeout is ever enabled. Not fixed here — see the
comment above `decode_jpg_buffer()` in `main/image_processor.c` for why (either unsubscribing the
calling task from the watchdog for the duration of the decode, or raising the timeout further,
would work, but each has its own trade-off worth weighing deliberately rather than doing by
default).

**Wake-up processing order** (fixed, so behavior is predictable across timer, button, and
Telegram-triggered wakes):

```
WiFi connect → poll Telegram → emergency-reset scan → download & display newest image
  (or fall back to album rotation) → run queued "/" commands → persist last_update_id → deep sleep
```

## Emergency reset

`/telegram_reset` is checked **before** anything else in the queue. It immediately clears the
whole pending-image and pending-command queue and puts the device back to sleep, bypassing normal
processing — a safety valve if the queue gets flooded or stuck.

## Multi-image orientation pairing

If the display is in portrait orientation (from `display_rotation_deg`) but the frame's default
layout is landscape (or vice versa), a single portrait image would normally be letterboxed. When
**Pairing** (`/pairing`) is enabled, two images of complementary orientation are combined
side-by-side into one composed image instead. Unpaired images wait in a small persistent queue
(NVS-backed) until a matching partner arrives; the composed result is saved to the album so
nothing is lost. Within a single batch of several images, only the newest ready-to-display result
(a plain image or a freshly-composed pair) ends up on screen — everything else is still saved to
the album and available for later rotation.

The same pairing idea is also available for **normal auto-rotation** (not just Telegram receives)
— see [Auto-rotate orientation pairing](#auto-rotate-orientation-pairing) below.

## Display history (no-repeat rotation)

Random album rotation (Telegram's own album included, since downloaded images become normal album
files) tracks every image it has shown by path in a persisted history file, so it cycles through
every image in the active album(s) once before repeating, instead of just avoiding the single
immediately-previous pick. Once everything has been shown, the history clears itself and a new
cycle starts automatically. `/clear_history` clears it manually (and resets the sequential-mode
rotation cursor too).

## Commands

| Command | Effect |
|---|---|
| `/status` | Firmware, reset reason, battery, WiFi, storage %, heap %, rotation schedule, and all toggle states |
| `/clear` | Clears the display to white |
| `/restart` | Restarts the device |
| `/pairing` | Toggles portrait/landscape combining for Telegram receives |
| `/list_albums` | Lists every album, with its active/inactive state |
| `/active_albums` | Lists only the active albums |
| `/enable_album <name>` | Activates an existing album for rotation |
| `/clear_history` | Clears the display history and restarts the no-repeat cycle |
| `/rotate_cron <M H Weekday>` | Sets the auto-rotate schedule as a cron expression |
| `/deep_sleep on\|off` | Enables/disables deep sleep |
| `/auto_rotate on\|off` | Enables/disables the auto-rotate timer |
| `/wake_notify on\|off` | Toggles a status ping sent on every wake-up |
| `/error_overlay on\|off` | Toggles an on-display warning banner after repeated WiFi failures |
| `/wifi_perf on\|off` | Toggles the WiFi performance mode (see below) |
| `/rotation_pairing on\|off` | Toggles auto-rotate orientation pairing (random mode only) |
| `/fallback_rotation on\|off` | Whether a wake with no new Telegram image still changes the display (on, default) or leaves it unchanged (off) |
| `/fallback_rotation_on_error on\|off` | Only matters while the above is off: whether a failed/unconfigured poll still falls back to album rotation (on, default) or also leaves the display unchanged (off) |
| `/rotation_notify on\|off` | Sends a thumbnail when a wake displays an image via fallback rotation |
| `/keep_originals on\|off` | Keeps a copy of each photo as received, before e-paper processing |
| `/exif_date on\|off` | Shows a photo's EXIF capture date as caption when it has none (experimental) |
| `/help` | Lists all commands |
| `/telegram_reset` | **Emergency**: clears the queue immediately, highest priority |

Images can also be sent with a caption starting with `/` — the caption is treated as a command
instead of being drawn on the image.

### `/status` and message formatting

All bot replies use a consistent, scannable, plain-ASCII layout:

- `/status` groups related fields (firmware/reset, battery/WiFi, storage/heap, schedule,
  settings) with blank lines instead of one dense block, and reports storage and heap as both
  absolute values and percentages (e.g. `62.3/128.0 MB free (48%)`).
- `/status` lists the on/off state of every toggle as `[x]` / `[ ]`.
- Every reply is prefixed with `[OK]`, `[ERROR]`, `[!]`, or `[i]` so success, failure, warning,
  and usage-hint messages are visually distinct at a glance.
- `/help` is grouped into Status / Display / Albums / Settings / Emergency sections.

Text stays plain ASCII (no Markdown parse mode, no accented characters/emoji) by design —
Telegram's `parse_mode` would require escaping user-controlled text like SSIDs and cron
expressions to avoid silently failing to send.

## Low battery & wake notifications

- If the battery drops below 20%, a one-time warning is sent via Telegram even if there were no
  new messages to process (debounced — fires once per discharge cycle, clears again above 25%).
- Optional wake-up ping (`/wake_notify`): sends a full `/status`-style report to Telegram on every
  wake, so you can confirm the device is alive without opening the web UI.

## Battery history

A "Battery History" tab in the Web UI plots battery percentage over time (a plain SVG chart, no
external charting library), marking charging/USB periods separately from on-battery readings. One
reading is recorded after every displayed image, to a small persisted log
(`/storage/.battery_history`) that resets automatically once the battery reaches 95% (a fresh full
charge) or after 180 days, whichever comes first.

An estimate of days remaining until 20% - based on the drain rate observed since the last charge -
is shown next to the Web UI chart, in `/status`, and in the optional wake-up notification.

## Weather + headline overlays

A separate, on-device weather line and news headlines can be drawn across the top of any
rotation-triggered display (Storage/SD rotation and Telegram-received images alike) - see
[docs/OVERLAYS.md](OVERLAYS.md) for setup, the `/weather`/`/headlines` commands, and its own
appearance settings (colors, language, line layout).

## Settings (Web UI + Telegram)

All default to preserving existing behavior for users who don't configure Telegram at all.

| Setting | Default | Purpose |
|---|---|---|
| Telegram bot token / chat ID | empty | Enables the Telegram rotation mode when both are set |
| Pairing | on | Combine mismatched-orientation Telegram receives instead of letterboxing |
| Deep Sleep | on | Existing setting, now also controllable via `/deep_sleep` |
| Auto-Rotate | on | Existing setting, now also controllable via `/auto_rotate` |
| Wake notify | off | Status ping to Telegram on every wake |
| Error overlay | off | On-display warning banner after persistent WiFi failures |
| WiFi performance mode | off | See below |
| Home Assistant integration | **off** | Master switch for all HA features (see below) |
| OTA auto-check | on | Automatic update check on cold boot |
| Auto-rotate orientation pairing | **off** | See [below](#auto-rotate-orientation-pairing) - random mode only |
| Fallback rotation | on | See [below](#fallback-rotation) |
| Fallback rotation on connection error | on | See [below](#fallback-rotation) - only matters while the above is off |
| Fallback-rotation notification | **off** | See [below](#fallback-rotation-notification) |
| Thumbnail gallery (Web UI) | **off** | Client-side toggle; large galleries slow down the device's HTTP server |
| On-device image format | **EPDGZ** | See [below](#on-device-image-format) |
| Duplicate detection | **off** | See [below](#duplicate-detection) |

### WiFi performance mode

WiFi power-save is normally tiered: the radio only stays in full-receive mode while someone is
actively looking at the web UI, and drops to a power-saving mode otherwise. Enabling this toggle
forces full performance at all times, trading battery life for a consistently faster web UI —
useful for always-on / Home Assistant setups.

### Home Assistant master switch

All Home Assistant integration code checks a single `ha_enabled` flag before doing anything
(`ha_is_configured()` requires both the existing HA URL config **and** this flag). It defaults to
**off**, but for anyone who already had HA configured before this change, the migration path keeps
their integration working — the flag isn't silently sprung on existing users.

### OTA auto-check toggle

Disabling automatic OTA checks (`ota_check_enabled = false`) skips the cold-boot update check
entirely — useful for dev builds where a `dev-<commit>` version string otherwise causes spurious
"update available" comparisons.

### Auto-rotate orientation pairing

Extends the same portrait/landscape combining idea to **normal auto-rotation**, not just Telegram
receives. When enabled and the randomly-picked next image doesn't match the panel's orientation,
the device immediately looks for another mismatched image in the active album(s) and combines
them instead of showing one letterboxed. The combined result is saved permanently in the album
(with a thumbnail); both source images are kept too, still independently rotatable later.

**Random rotation mode only** — sequential mode's deterministic image-order cursor is deliberately
left untouched, so this setting has no effect there. The Web UI shows a warning if the toggle is
on while Sequential mode is selected.

### Fallback rotation

**On by default** (preserves the original behavior): when a Telegram-mode wake polls and finds no
new image (nothing new arrived, or everything in the batch was a command / still waiting for its
pairing partner), the device falls back to normal album rotation instead of leaving the previous
image up — the frame changes on every rotation-triggering wake, same as the non-Telegram rotation
modes.

Disable it (Web UI: Settings → Telegram → "Change display on a wake with no new photo", or
`/fallback_rotation off`) to make the display change **only** on a wake that actually receives a new
Telegram photo. Useful if you want the frame to act purely as a Telegram photo feed, with no
interleaved album content, and don't mind it going a while without changing between photos.

There are actually two distinct ways a wake can end up with "no new Telegram image": the poll
genuinely succeeds but finds nothing new (covered above), and the poll **fails outright** - Telegram
is unreachable, or the bot isn't configured at all. A **second, related toggle** decides what the
disabled state above means for that second case specifically:

- **"Still fall back to album rotation if the Telegram connection fails"** (Web UI, nested under the
  toggle above; or `/fallback_rotation_on_error on|off`) - **on by default**, and only has any effect
  while the main toggle above is off (with it on, a connection failure already always falls back,
  unconditionally, exactly as it always has). On: a connection failure is treated as an exception to
  the strict "Telegram-photos-only" policy and still falls back to normal album rotation - useful if
  you want the frame to keep showing *something* even during an extended outage or before the bot
  token is ever configured. Off: a connection failure is folded into the same strict policy as "no
  new photo" - the display stays completely untouched on every wake until Telegram is reachable
  again **and** actually delivers a new photo.

Independent of [Fallback-rotation notification](#fallback-rotation-notification) below, which only
controls whether a fallback display change (while the main toggle is on) also gets announced to the
chat - turning fallback rotation off makes that notification moot (it never fires with nothing to
notify about), so the Web UI disables that toggle while this one is off.

### Fallback-rotation notification

When a wake falls back to normal album rotation (no new Telegram image that cycle), optionally
sends a thumbnail of whatever got displayed instead - so the chat still shows what's currently on
the frame even when nothing was pushed to it. Only fires when the display actually changed (not
when rotation was a no-op, e.g. no enabled albums). Off by default; toggle via Web UI or
`/rotation_notify on|off`.

### Keep originals

Each incoming Telegram photo is normally converted straight to a display-ready image (PNG or EPDGZ,
see below) and the raw download is deleted. When enabled, a copy of the raw file is instead kept
under `Telegram/Originals` on the SD card — a plain archive path, not an album, so it's invisible to
the gallery and rotation. Off by default; toggle via Web UI or `/keep_originals on|off`.

### On-device image format

Which format the device itself produces when converting an incoming Telegram photo for the album:

- **EPDGZ** (default, recommended) — already stores the resolved 4-bit palette index,
  gzip-compressed. Every future display of that photo is then just a gzip-inflate and a direct
  nibble read — no per-pixel color re-matching, unlike reading a "processed" PNG back (which still
  needs a full decode plus a fresh nearest-palette lookup per pixel every time). Smaller file, faster
  display, same as [process-cli](../process-cli/README.md)'s own recommended default.
- **PNG** — kept for compatibility/inspection (e.g. opening the file directly on a computer).

This same setting also governs a **composed orientation pair** (see
[Multi-image orientation pairing](#multi-image-orientation-pairing) and
[Auto-rotate orientation pairing](#auto-rotate-orientation-pairing) above) — one format preference
covers every Telegram-originated display file, paired or not. If EPDGZ's ~260 KB of deflate state
can't be allocated at that moment, the write quietly falls back to PNG for that one file, same as
everywhere else this format choice applies.

### Duplicate detection

Off by default. When enabled, the device checks each incoming photo or file against Telegram's own
**`file_unique_id`** — a content-based identifier Telegram assigns that stays the same for identical
file content across re-sends and forwards, unlike `file_id` (which can vary even for the same
content). This is checked *before* downloading anything, so a detected duplicate costs no bandwidth
or processing time — just a short reply ("Duplicate photo/file - already received before, skipped")
instead of the usual "Saved" confirmation.

The device remembers the last 30 received items (oldest dropped first), persisted across deep sleep.
No image hashing is involved — the identifier comes from Telegram's own API response, so this adds
no meaningful CPU or memory cost. A "photo" message's identity is taken from its largest available
size (each resolution Telegram offers is technically a distinct file with its own id, so the largest
is used as a stand-in for "this photo" — the same convention most Telegram bots use for this); a
document/file upload has just one id of its own.

This only catches an *exact* re-send/forward of the same underlying file — cropping, re-compressing,
or re-exporting a photo elsewhere before sending it again produces a new, different
`file_unique_id`, so it won't be caught (nor should it be, since it's genuinely different file
content).

If EPDGZ encoding can't get the ~260 KB of memory it needs at that moment, the device falls back to
PNG for that photo automatically (logged as a warning) — this is a graceful degradation, not a
failure, and doesn't require re-sending the photo. Configure via Web UI (Telegram settings); no
Telegram command for this one, since it's an infrastructure choice rather than a per-use toggle.

For "photo" messages (not files sent as a document), the archived copy is always re-fetched at
Telegram's largest available size for that photo — even if the size actually used for the display
conversion above had to fall back to a smaller one because the largest turned out to be a
progressive JPEG the firmware's decoder can't read. Decoding isn't required to archive raw bytes, so
the best quality available is kept regardless of that display-side limitation.

### EXIF capture date as fallback caption (experimental)

When a received photo has no caption of its own, optionally falls back to its EXIF
"DateTimeOriginal" tag (the camera's capture date) as the caption instead — shown as
`YYYY-MM-DD HH:MM`. If the photo has no EXIF data (or the tag is missing), no caption is shown at
all; nothing is guessed or approximated. Off by default; toggle via Web UI or `/exif_date on|off`.

**Experimental / Telegram-only**: parsing is a small hand-written JPEG/TIFF reader (no external
library), tested against a limited set of real-world camera/phone JPEGs — atypical EXIF encodings
may not parse. Only applies to Telegram photos: the original JPEG (with EXIF intact) reaches the
device directly, whereas Storage/album images are processed client-side in the browser before
upload, so no EXIF data is ever available for those on the device side.

### Error overlay test

Settings includes a "Test Error Overlay" button that triggers the overlay immediately
(`POST /api/error-overlay/test`), regardless of the setting above, to preview it without waiting
for 3 consecutive WiFi failures. If there's no current image to overlay onto (fresh boot, or after
`/clear`), it generates a blank canvas instead of failing, so the preview - and the real
WiFi-failure case on a device that's never displayed anything yet - always has something to show.

## Security

- **Redacted logging**: the bot token is never logged. All Telegram HTTP calls log through
  `redact_url_for_log()`, which strips the token before anything reaches the log.
- **Chat allowlist**: only messages from the configured chat ID are ever processed.
- Keep your bot token private — anyone with it can send commands to your device, including
  `/telegram_reset` and `/restart`.

## Setup

1. Talk to [@BotFather](https://t.me/BotFather) on Telegram, create a bot, and copy the token it
   gives you.
2. Message your new bot once (or add it to a group) so you have a chat ID; the simplest way to
   find it is to send a message and check `https://api.telegram.org/bot<TOKEN>/getUpdates`.
3. In the PhotoFrame web UI, go to **Settings → Telegram**, enter the bot token and chat ID, and
   enable the integration.
4. Send the bot a photo. It will be processed on the next wake (or trigger one immediately,
   depending on your rotation-timer settings).

### Using the bot in a group: disable Privacy Mode

If the bot is added to a **group** chat (rather than messaged 1:1), Telegram's own
[Privacy Mode](https://core.telegram.org/bots/features#privacy-mode) applies: by default, a bot in
a group only receives `/`-commands, not regular messages - **including photos**. This is entirely
on Telegram's side (the message never reaches the device at all, so nothing is logged about it) and
looks exactly like "the bot responds to commands but ignores photos".

Fix via [@BotFather](https://t.me/BotFather): `/mybots` → select your bot → **Bot Settings** →
**Group Privacy** → turn it **off**. This lets the bot see every message in groups it's a member
of, not just commands. (Alternative: message the bot 1:1 instead of via a group - Privacy Mode only
applies to groups.)
