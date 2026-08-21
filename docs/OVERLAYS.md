# Weather + Headline Overlays

An on-device alternative to the companion
[esp32-photoframe-server](https://github.com/aitjcize/esp32-photoframe-server)'s weather overlay
and smart collage — no separate server required. Two independently toggleable overlays, drawn as a
short text bar across the **top** of the image whenever a wake rotates to a new photo:

- **Weather**: a 3-day forecast (today + next 2 days) — weekday, condition, and min/max
  temperature per day, e.g. `Wed sunny 16/24 | Thu partly cloudy 17/25 | Fri rain -5/3` — from
  [Open-Meteo](https://open-meteo.com/) — free, no API key, no signup.
- **Headlines**: up to 3 news headline titles, from any RSS or Atom feed you configure — no API
  key, no rate limit, works with essentially any news outlet.

Both share two appearance settings (Web UI only):

- **Colors**: black bar/white text (default) or inverted (white bar/black text). A separate
  checkbox extends this to Telegram photo captions too (off by default — captions keep their
  fixed look unless explicitly opted in).
- **Language**: English (default) or German — selects both the weekday abbreviations
  (`Mon..Sun` / `Mo..So`) and the weather condition wording.

### Line-width limits

This firmware targets several boards with very different panel widths (see `boards/boards.json`) -
from 800px (Waveshare PhotoPainter, Seeed XIAO EE04, reTerminal E1002) up to 1872px (Seeed XIAO
EE03, reTerminal E1003). The overlay text uses a fixed-width 17px-per-character bitmap font
regardless of panel size, so the character budget scales with it: roughly `(panel_width_px - 8) /
17` characters per line - about **46** on the narrowest (800px) boards, ~70 on the 1200px boards,
and ~109 on the widest (1872px) ones.

A single day's weather segment (`Wed sunny 16/24`) fits comfortably on every supported board, even
in the worst case (a long condition word plus 3-digit negative temperatures on both ends, e.g.
`Wed hvy.storm+hail -10/-11` ≈ 27 characters). **Three days combined onto one line** is the case
that varies by board: on the widest boards (~109 chars) every combination fits; on the narrowest
800px boards (~46 chars) it generally does not, even after abbreviating condition words as far as
reasonably legible (`Gewitter` → `Gew.`, `hvy. storm + hail` → `hvy.storm+hail`, etc.) - worst case
there is still well over twice the line budget. There is no way to guarantee all three days on one
line with text alone on the narrower boards; the options are: accept that some combinations
truncate on the single-line layout, or use the 3-line layout below, which always fits regardless of
panel width. (Small fixed-size icons instead of condition words would also help on narrow boards,
but weren't implemented - the 3-line layout already solves the fitting problem completely there, at
much lower cost than authoring a new bitmap icon set.)

- **Weather as 3 lines**: when the headlines overlay is off, an option renders the weather as one
  line per day instead of one combined line — reliably fits every combination. Not offered while
  headlines are also on (not enough vertical room for both).
- **Headline word-wrap**: when exactly 1 headline is selected, an option wraps it across 2 or 3
  display lines instead of hard-truncating it to one line with `…`.

Smart collage (combining two mismatched-orientation photos instead of showing one letterboxed) is
a separate, already-existing feature — see [Multi-image orientation
pairing](TELEGRAM.md#multi-image-orientation-pairing) and [Auto-rotate orientation
pairing](TELEGRAM.md#auto-rotate-orientation-pairing) in the Telegram docs.

## How it works

1. Both overlays are off by default. Enable either independently via the Web UI (Settings →
   Power → Weather + Headline Overlays) or a Telegram command (`/weather on|off`,
   `/headlines on|off`).
2. On every rotation-triggered display (Storage/SD rotation or a Telegram-received image), if
   either overlay is enabled, the frame fetches fresh weather/headline data and draws it as one
   bar across the top of the image **before** the single per-wake panel refresh — never a second
   refresh, and the original saved album file on disk is never modified (the overlay is drawn onto
   a throwaway scratch copy, re-created every time).
3. If a fetch fails (no network, feed unreachable, etc.), that overlay is silently skipped for the
   cycle — never blocks the normal image display.
4. **Refresh cadence is tied to your existing rotation schedule** — there's no separate wake timer
   for overlays (the e-paper panel has no partial-refresh capability, so waking more often than
   your chosen rotation interval just to refresh text would cost extra full panel refreshes and
   battery for no real benefit). A sparse rotation schedule (e.g. once a day, or less) means
   correspondingly stale weather/headlines — refreshing only as often as the frame already wakes is
   the deliberate trade-off here.
5. **Not available in URL rotation mode**: that mode streams pixels row-by-row straight to the
   panel and never produces a processed image file to draw an overlay onto (the same reason the
   error-overlay feature can't use it either). Storage and Telegram rotation modes are unaffected.

## Weather setup

Configure a location one of two ways:

- **Location name** (e.g. "Berlin"): geocoded once via Open-Meteo's free geocoding API on first
  use (in the overlay language, so results match German or English place names), then cached —
  subsequent wakes reuse the cached coordinates and only re-geocode if you change the name.
  Geocoding always goes through Open-Meteo, regardless of which forecast provider (below) is
  selected.
- **Latitude/Longitude**: set both directly to skip geocoding entirely.

Day boundaries (which calendar day each forecast entry belongs to) use Open-Meteo's
`timezone=auto`, which resolves the correct local timezone from the coordinates server-side —
independent of the device's own configured timezone (a POSIX TZ string like `UTC0`, not directly
usable as an Open-Meteo timezone parameter).

### Weather data source

Three free, keyless providers are available (Web UI: Settings → Auto Rotate → Weather + Headline
Overlays → "Weather data source"):

- **Open-Meteo** (default) — the source described above.
- **[wttr.in](https://wttr.in/)** — no setup, pulls from a mix of free weather sources in the
  background.
- **[yr.no](https://api.met.no/)** (MET Norway) — globally accurate, widely used as a fallback by
  other open-source projects; requests must include an identifying User-Agent, which the firmware
  sends automatically.

wttr.in and yr.no exist as manual alternatives to switch to if Open-Meteo isn't reachable or
reliable for your network/region — there's no automatic runtime fallback between the three, so
only one is ever queried per cycle. Each provider's own condition codes are approximated into the
same short condition vocabulary described above, so the displayed text looks the same regardless of
source; day-boundary handling differs slightly per provider (yr.no in particular buckets by UTC
calendar date rather than local time, since it has no per-location timezone parameter — see
`weather.c`'s `fetch_yrno()` for specifics).

## Headlines setup

Paste any RSS or Atom feed URL (e.g. `https://www.tagesschau.de/xml/rss2/`,
`http://feeds.bbci.co.uk/news/rss.xml`, or your outlet of choice). Choose how many headlines to
show (1-3, default 3). Titles are extracted with a small purpose-built parser (not a full
XML/RSS library) - CDATA and the common HTML entities are handled, and everything is
transliterated to plain ASCII to match the display font.

## Commands

| Command | Effect |
|---|---|
| `/weather on\|off` | Toggles the weather overlay (configure location in the Web UI) |
| `/headlines on\|off` | Toggles the headlines overlay (configure the RSS feed in the Web UI) |
