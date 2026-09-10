# Agenda ToDo Color Scheme

The full-screen Agenda ToDo/Calendar mode (see the main project plan) renders
[todo.txt](https://github.com/todotxt/todo.txt)-format items on the panel, styled after
[webstonehq/tuxedo](https://github.com/webstonehq/tuxedo)'s color-coded terminal look. This
firmware targets several very different e-paper panels — some with a handful of fixed colors, some
grayscale-only — so tuxedo's palette can only be an *inspiration*, not a literal copy: it has to be
re-derived per panel type from the colors actually available, while staying true to tuxedo's
intent (which element gets emphasized, and how strongly).

This document records that derivation: tuxedo's own colors, what each supported board can actually
display, and the resulting per-board assignment for the firmware's ToDo rendering.

## 1. Tuxedo's own color scheme

Source: [`src/theme.rs`](https://github.com/webstonehq/tuxedo/blob/main/src/theme.rs) in the
tuxedo repo. Tuxedo ships five built-in themes (`Muted Slate`, `Dawn`, `Nord`, `Matrix`,
`Terminal`); **`Muted Slate` is the default** (index 0, selected whenever no `theme =` line is
set in the user's config). The table below is `Muted Slate`, restricted to the fields relevant to
rendering a todo.txt line (tuxedo also defines UI-chrome colors — panel background, borders,
status bar, selection highlight, search-match highlight — which have no equivalent in this
firmware's single-purpose ToDo screen and are omitted).

| todo.txt element | Tuxedo field | Color |
|---|---|---|
| Priority `(A)` | `pri_a` | `#E07A7A` (muted red) |
| Priority `(B)` | `pri_b` | `#D4B06A` (muted gold/amber) |
| Priority `(C)` | `pri_c` | `#7AA67A` (muted green) |
| Priority `(D)` | `pri_d` | `#7A9EC9` (muted blue) |
| Priority `(E)`–`(Z)` | `pri_other` | `#9A8FC4` (muted purple) |
| `+Project` | `project` | `#7FB3A8` (teal/seafoam) |
| `@Context` | `context` | `#C89A6E` (tan/orange-brown) |
| `due:` (future, not yet overdue) | `due` | `#D4B06A` (same as `pri_b`) |
| Overdue | `overdue` | `#E07A7A` (same as `pri_a`) |
| Due today | `today` | `#E07A7A` (same as `overdue`/`pri_a`) |
| Completed (`x `) | `done` | `#5A6270` (dim gray) |
| Plain body text | `fg` | `#C8CCD4` (light gray) |
| Screen background | `bg` | `#1A1D23` (near-black) |

Two things worth noting, since they directly inform the hardware-constrained assignment below:

- **Tuxedo itself reuses colors across closely-related fields**: `overdue`, `today`, and `pri_a`
  are the identical red; `due` and `pri_b` are the identical amber. Tuxedo doesn't treat every
  concept as needing its own unique hue — it groups by *urgency meaning*. This firmware's
  palette is far more constrained than a 24-bit terminal, so the same grouping strategy is used
  deliberately below, not as a fallback compromise.
- Tuxedo is a **dark theme** (light text on a near-black background). This firmware's overlay
  conventions are the opposite (black text on a white background, matching every existing overlay
  — weather bar, captions, battery badge). Colors are re-picked for legibility on white, not
  transplanted by RGB value.

## 2. Hardware color capability

From [`boards/boards.json`](../boards/boards.json) and `BOARD_HAL_DISPLAY_TYPE`
(`components/board_hal/include/*.h`). Seven boards are supported, but they reduce to exactly two
distinct color capability profiles — every board in a profile is driven through the identical
palette/dithering code in `image_processor.c`, so there is no per-board variation to document
within a profile.

| Profile | Boards | `BOARD_HAL_DISPLAY_TYPE` | Available colors |
|---|---|---|---|
| **Spectra6 (6-color)** | `waveshare_photopainter_73`, `seeedstudio_xiao_ee02`, `seeedstudio_xiao_ee04`, `seeedstudio_reterminal_e1002`, `seeedstudio_reterminal_e1004` | `spectra6` | Black, White, **Yellow**, **Red**, **Blue**, **Green** — 6 fixed, undithered colors (`palette[7]` in `image_processor.c`; index 4 is reserved/unused) |
| **Grayscale (16-level)** | `seeedstudio_xiao_ee03`, `seeedstudio_reterminal_e1003` | `gc16` | 16 gray levels, `0` (black) to `255` (white) in steps of 17 (`gray_theoretical[16]`) — no chromatic hue at all |

Every other board field (resolution, storage, panel size) varies per board, but has no bearing on
color — only these two profiles matter for the tables below. `board_is_grayscale()` (and the
Agenda renderer's own copy, `agenda_board_is_grayscale()`) distinguish them at runtime via
`strncmp(BOARD_HAL_DISPLAY_TYPE, "gc", 2)`.

## 3. Current firmware assignment (as implemented today)

The Agenda renderer (`agenda_renderer.c`) currently assigns **one color to an entire rendered
row** (the whole formatted todo.txt line, e.g. `[A] Fix the printer (due 2026-09-05)`), picked by
`todo_item_role()`'s single highest-priority match — not per todo.txt element. This is the
"row-based, not element-based" behavior flagged for review (see §5).

| Role (`agenda_color_role_t`) | Trigger | Spectra6 color | Grayscale level |
|---|---|---|---|
| `URGENT` | Overdue **or** priority `A` | Red `(255,0,0)` | `2×17=34` (dark gray) |
| `WARNING` | Due today **or** priority `B` | Yellow `(255,255,0)` | `8×17=136` (mid gray) |
| `INFO` | Future due date **or** has a `+Project` | Blue `(0,0,255)` | `11×17=187` (light gray) |
| `DEFAULT` | Everything else (incl. priority `C`–`Z`, `@Context`-only items, plain text) | Black `(0,0,0)` | `0×17=0` (black) |

Two concrete issues this document's redesign (§4) addresses:

- **`WARNING` is pure yellow text on the default white background** — exactly the low-contrast
  case flagged: `(255,255,0)` on `(255,255,255)` has very little luminance difference and is hard
  to read, especially on a reflective e-paper surface rather than a backlit screen.
- `@Context` has no dedicated treatment at all — it's silently absorbed into whichever other role
  wins (usually `DEFAULT`), even though todo.txt treats context as one of its ["3 axes"](https://github.com/todotxt/todo.txt#the-3-axes-of-an-effective-todo-list)
  alongside priority and project.

## 4. Proposed tuxedo-aligned assignment, per hardware

Design principle carried over from tuxedo's own reuse pattern (§1): **group by urgency meaning**,
and where a hue must be reused for two unrelated axes (priority tier vs. project/context tag), pick
hues that are separated by other cues (position in the line, or which axis is even present on a
given row) rather than colliding on the *same* axis. Backgrounds are used specifically to avoid
the yellow-on-white legibility trap and to give the top urgency tier (overdue) the strongest
possible visual weight — mirroring tuxedo's own choice to make `overdue`/`today` the *same*, most
alarming color as the top priority tier.

### Spectra6 (color) boards

| todo.txt element | Foreground | Background | Rationale |
|---|---|---|---|
| Overdue | White | **Red** | Strongest possible treatment for the top urgency tier — an inverted "alert" chip, not just red text, mirrors tuxedo collapsing `overdue`/`today`/`pri_a` into one alarming color. |
| Priority `(A)` (not overdue) | Red | White | Same hue as overdue, plain text — one step down in visual weight, matching `pri_a` = `overdue` color in tuxedo but without the inversion. |
| Due today | Black | **Yellow** | Avoids yellow-on-white entirely (the flagged legibility problem) by using yellow only as a fill, with black text on top — same "highlighter" idiom as the overdue chip, one tier down. |
| Priority `(B)` (not due today) | Black | Yellow | Same fill as due-today, for the same reason tuxedo ties `pri_b` to `due` (both amber). |
| Priority `(C)` | Green | White | Distinct hue, calmer/plain treatment — matches tuxedo's `pri_c` being a plain (non-reused) color. |
| Priority `(D)` | Blue | White | Distinct hue; also the "no more colors" ceiling — see next row. |
| Priority `(E)`–`(Z)` | Black | White | Spectra6 has no 5th chromatic hue for tuxedo's `pri_other` (purple); falls back to plain body text rather than reusing an already-claimed color and creating a false association. |
| `+Project` | Blue | White | Reuses priority `D`'s hue, but on a *different axis* (a project tag vs. a priority marker never occupy the same visual slot in a per-token renderer — see §5) — closest cool hue to tuxedo's teal `project`. |
| `@Context` | Green | White | Reuses priority `C`'s hue, same reasoning as `+Project`/`D` above — closest hue to tuxedo's tan `context` that isn't already claimed by red/yellow. |
| Plain body text | Black | White | Matches every other overlay in this firmware (weather bar, captions) — deliberately the most neutral, highest-baseline-contrast choice, so colored elements stand out *against* it. |
| Completed (`x `) | — | — | Not rendered — `todo_parse()` excludes completed items by default (`TODO_LINE_MAX_LEN`/parser design), so no color is needed. If ever surfaced, Spectra6 has no gray to dim it with (see grayscale column for the equivalent idea). |

### Grayscale (16-level) boards

No chromatic hue exists at all, so emphasis has to come from **contrast/inversion**, not hue —
translating tuxedo's "most urgent = most visually loud" into the one dimension grayscale actually
has.

| todo.txt element | Foreground | Background | Rationale |
|---|---|---|---|
| Overdue | White | **Black** (level 0) | Full inversion — the strongest possible statement in a 1-channel palette, echoing the Spectra6 red-chip treatment. |
| Priority `(A)` (not overdue) | Black (level 0) | White | Plain full-black text — still the darkest possible foreground, one step down from full inversion. |
| Due today | Black (level 0) | **Mid gray** (level 8 = 136) | A visible "highlighted" fill without full inversion — same one-tier-down relationship as the color-board yellow chip. |
| Priority `(B)` (not due today) | Black (level 0) | Light-mid gray (level 11 = 187) | A lighter fill than due-today's, keeping `B` visually a notch below "due today" the same way Spectra6 keeps plain-priority-`B` a notch below the due-today chip (same fill hue, implied by context/position rather than a second fill tone if a simpler 2-tone scheme is preferred). |
| Priority `(C)` | Black (level 0) | White | Plain text, no fill — matches Spectra6's "calmer, non-reused treatment" for `C`. |
| Priority `(D)` | Black (level 0) | White | Same as `C` — grayscale has no extra channel to separate `C` from `D` the way Spectra6's green/blue can; both fall back to plain black-on-white, distinguished only by the priority letter itself. |
| Priority `(E)`–`(Z)` | Black (level 0) | White | Plain text — matches Spectra6's fallback. |
| `+Project` | Black (level 0) | White | Plain text — same reasoning as priority `C`/`D`: no spare channel, relies on the `+Project` token itself for identification. |
| `@Context` | Black (level 0) | White | Plain text, same as `+Project`. |
| Plain body text | Black (level 0) | White | Matches every other overlay on grayscale boards. |
| Completed (`x `) | Light gray (level 11 ≈ 187) | White | Not currently rendered (see Spectra6 note), but *if* it were, a light-but-still-legible gray is the direct grayscale analog of tuxedo's dim `done` color — deliberately faded, which is the whole point of "done," so the usual light-on-white caution doesn't apply here. |

> Compared to the currently-implemented mapping in §3, this proposal trades some hue variety
> (`C`/`D`/other-priority and `+Project`/`@Context` all collapse to plain black-on-white on
> grayscale boards, since there's only one channel to work with) for **no low-contrast text
> anywhere** and a consistent "more urgent → more background fill" language shared by both
> hardware profiles.

## 5. Open gap: row-based vs. element-based coloring

Both tables above describe an *intended per-element* mapping — but today's renderer
(`agenda_renderer.c`'s `draw_column()` → `image_processor_draw_text()`) can only paint **one solid
color for an entire line** in a single call; there is no primitive yet for a "colored run" (e.g.
painting just the `(A)` marker red, `+GarageSale` blue, and the rest of the line black, all within
one drawn string). That's exactly the behavior flagged: a row's color is decided once, by whichever
signal (`todo_item_role()`) wins overall, not per todo.txt element as the
[format's own 3 axes](https://github.com/todotxt/todo.txt#the-3-axes-of-an-effective-todo-list)
(priority / project / context) would suggest.

Closing this gap is a rendering-capability change, not a color-choice change — it needs
`image_processor_draw_text()` (or a new sibling function) to accept multiple colored substrings per
line instead of one color for the whole call. This was already flagged as a deferred item in the
original Agenda feature plan ("optional, explicitly deferred: per-token inline `+project`/
`@context` coloring"); this document's per-element tables are what that future implementation
should target once it's built.
