#include "agenda_renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "agenda_color_profile.h"
#include "board_hal.h"
#include "config_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "weather_icons_data.h"

static const char *TAG = "agenda_renderer";

// Holds one row's fully-assembled text (priority + body + tags + due-date
// suffix for ToDo, or time prefix + summary for Calendar) *before*
// image_processor_wrap_text() truncates it to fit the actual column width -
// wrap_text() only ever shortens further, so undersizing this buffer can't
// overflow, but it can silently drop the due-date suffix (and its color
// highlight) off the end of the string before wrap_text ever gets a chance
// to decide what's actually worth keeping. Kept with real headroom above
// TODO_LINE_MAX_LEN (200) rather than matching it exactly, since a
// body-text-only line already uses the ToDo source's full budget, leaving
// nothing for anything appended after it.
#define AGENDA_ROW_BUF_LEN 256
#define AGENDA_PADDING 4

typedef struct {
    int x, y, w, h;
} agenda_rect_t;

// Mirrors wants_portrait_frame() (display_manager.c) / wants_portrait_frame_now()
// (telegram_bot.c) - kept as a third small duplicate rather than a
// cross-module dependency, matching this codebase's own stated preference
// for that trade-off (see either of those functions' own comment).
static bool agenda_wants_portrait_frame(void)
{
    int rot = config_manager_get_display_rotation_deg() % 360;
    if (rot < 0) {
        rot += 360;
    }
    return (rot == 90 || rot == 270);
}

// Mirrors board_is_grayscale() (image_processor.c, file-private there) -
// same one-line duplication convention as agenda_wants_portrait_frame() above.
static bool agenda_board_is_grayscale(void)
{
    return strncmp(BOARD_HAL_DISPLAY_TYPE, "gc", 2) == 0;
}

// The ToDo column's fixed plain page background - always black-on-white
// (see config.h's comment on the removed agenda_bg_color setting). The
// Calendar column no longer shares this: its own background comes from the
// active color profile instead (see agenda_color_profile.h).
static void agenda_background_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = *g = *b = 255;
}

static bool agenda_colors_equal(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2,
                                uint8_t b2)
{
    return r1 == r2 && g1 == g2 && b1 == b2;
}

// Simple luminance split between this project's two safest colors
// (black/white) - enough since every background this renderer can be
// asked to use is one of a small, known set of solid palette colors, never
// something genuinely in between. Used both for the collision-avoidance
// fallback below and for deciding which "polarity" the day divider, column
// headers, and per-calendar-source event colors should render in.
static bool agenda_is_light(uint8_t r, uint8_t g, uint8_t b)
{
    // Weights sum to 1000 (per-mille, not scaled to 0..255000000), so the
    // formula's actual range is [0, 255*1000] = [0, 255000] - the
    // threshold has to sit at its midpoint (127500), not at 500000. That
    // stale threshold (roughly double the formula's own maximum) made
    // this always return false, misclassifying pure white as "dark" -
    // caught live: a black chosen background correctly turned the day
    // divider/header fill white, but their text stayed white-on-white
    // instead of flipping to black, since agenda_safe_text_color() always
    // took its "not light" branch regardless of input.
    int luminance = (int) r * 299 + (int) g * 587 + (int) b * 114;
    return luminance > 127500;
}

static void agenda_safe_text_color(uint8_t bg_r, uint8_t bg_g, uint8_t bg_b, uint8_t *r, uint8_t *g,
                                   uint8_t *b)
{
    if (agenda_is_light(bg_r, bg_g, bg_b)) {
        *r = *g = *b = 0;
    } else {
        *r = *g = *b = 255;
    }
}

// Applies the swap-on-collision rule uniformly: if (*fr,*fg,*fb) exactly
// matches the current background, replace it with the safe fallback so the
// text never disappears into the page. Deliberately only applied to plain
// (no-own-fill) text colors - the priority/due chips already draw their
// own local background first, so their text is guaranteed readable against
// *that* fill regardless of the page background. ToDo's own header/divider
// deliberately derive their polarity from the page background this way too
// (draw_todo_column()); the Calendar column no longer does - its colors
// come from the active color profile instead, drawn literally with no
// runtime auto-fix except where the profile genuinely has no field to
// specify an ink from (the "mark" fill's own text - see draw_day_cell()).
static void agenda_avoid_bg_collision(uint8_t bg_r, uint8_t bg_g, uint8_t bg_b, uint8_t *fr,
                                      uint8_t *fg, uint8_t *fb)
{
    if (agenda_colors_equal(*fr, *fg, *fb, bg_r, bg_g, bg_b)) {
        agenda_safe_text_color(bg_r, bg_g, bg_b, fr, fg, fb);
    }
}

// ----------------------------------------------------------------------------
// Calendar-view color resolution, driven entirely by the active imported
// color profile (agenda_color_profile.h) - see that header's own comment
// for the field-by-field schema. Verified 1:1 against profile-editor.html's
// own computeBase()/buildStateRows()/render() logic (colors + mode + mark),
// including its exact marking rules:
//   - "markColorsHeader" is mutually exclusive: a marked day recolors
//     EITHER its header OR its whole body area, never both.
//   - Color-mode Calendar event ink NEVER changes with marked state (only
//     its own chip background does, switching to the single "mark" color
//     on a body-marked day) - the profile author is expected to have
//     already verified every ink/background pairing looks right in the
//     tool's own live preview, so this file never re-derives Calendar
//     event ink against a marked background the way the old
//     agenda_ensure_contrast() heuristic used to.
//   - Mono-mode marking is always a plain page/ink polarity swap for
//     whichever area (header or body) is being marked, regardless of the
//     profile's specific hex colors (which mono ignores entirely) - see
//     agenda_mono_pair() below.
//   - Only the "+N more"/multi-day-prefix text and a header-marked day's
//     header ink have no dedicated profile field at all (there is no
//     "markInk" in the schema) - those two are the only remaining
//     auto-derived colors, via agenda_safe_text_color() against whatever
//     they're actually drawn on, exactly mirroring the tool's own
//     contrastInk().
// ----------------------------------------------------------------------------

// A color-mode profile can't be shown in its authored hues on a grayscale
// panel - fall back to the mono model, picking normal vs. invert from
// whichever polarity the profile's own page background leans toward, so a
// profile authored with a dark page (e.g. a "black on yellow" profile)
// degrades to mono-invert automatically rather than defaulting to
// mono-normal regardless of the author's intent. A profile that already
// declares a mono mode is left untouched - it's already board-agnostic.
static void agenda_color_profile_degrade_for_grayscale(agenda_color_profile_t *p)
{
    if (p->mono) {
        return;
    }
    p->mono = true;
    p->mono_invert = !agenda_is_light(p->text_bg.r, p->text_bg.g, p->text_bg.b);
}

// Mono page/ink pair for one area (header, or body) on one specific day -
// `invert_area` is whether marking currently applies to THIS area on THIS
// day (see draw_day_cell()'s mark_colors_header split); flips the
// profile's own base polarity when true, exactly matching
// profile-editor.html's mono() + markBg computation (mono marking is
// always a plain black/white swap, never a specific hue).
static void agenda_mono_pair(bool base_invert, bool invert_area, uint8_t *bg_r, uint8_t *bg_g,
                             uint8_t *bg_b, uint8_t *fg_r, uint8_t *fg_g, uint8_t *fg_b)
{
    bool inv = invert_area ? !base_invert : base_invert;
    if (inv) {
        *bg_r = *bg_g = *bg_b = 0;
        *fg_r = *fg_g = *fg_b = 255;
    } else {
        *bg_r = *bg_g = *bg_b = 255;
        *fg_r = *fg_g = *fg_b = 0;
    }
}

// Resolves one of the 11 user-configurable per-role color names
// (config_manager_get_agenda_*_color(), Spectra6/color boards only - see
// config.h's AGENDA_ROLE_COLOR_MAX_LEN comment) to its exact Spectra6 RGB
// triple. Never free RGB, only ever one of these 4 exact hues - an
// off-palette value dithers into visual noise on real hardware (see
// priority_color()'s comment below for the full story). An unrecognized or
// empty name (including on a fresh device that's never saved this setting)
// falls back to whichever default the caller passes in - normally the
// role's own original hardcoded color from before this setting existed.
static void role_hue(const char *name, uint8_t default_r, uint8_t default_g, uint8_t default_b,
                     uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (name && strcmp(name, "red") == 0) {
        *r = 255;
        *g = 0;
        *b = 0;
    } else if (name && strcmp(name, "yellow") == 0) {
        *r = 255;
        *g = 255;
        *b = 0;
    } else if (name && strcmp(name, "blue") == 0) {
        *r = 0;
        *g = 0;
        *b = 255;
    } else if (name && strcmp(name, "green") == 0) {
        *r = 0;
        *g = 255;
        *b = 0;
    } else {
        *r = default_r;
        *g = default_g;
        *b = default_b;
    }
}

// Which text color reads legibly on top of a given chip hue - hardware
// truth, not a formula: live testing on real Spectra6 hardware found the
// panel's actual green ink prints much darker than the sRGB (0,255,0)
// preview suggests (see priority_color()'s original comment on Priority C),
// so a luminance-based contrast pick (agenda_is_light()) would get green
// wrong. Yellow is the only hue light enough for black text; red/blue/green
// all need white.
static void hue_contrast_text(uint8_t hue_r, uint8_t hue_g, uint8_t hue_b, uint8_t *tr, uint8_t *tg,
                              uint8_t *tb)
{
    if (hue_r == 255 && hue_g == 255 && hue_b == 0) {
        *tr = *tg = *tb = 0;  // yellow -> black text
    } else {
        *tr = *tg = *tb = 255;  // red/blue/green -> white text
    }
}

// Resolves a user-configurable *chip* role (a role that draws its own
// background fill): looks up `role_name`'s hue (role_hue()), then either
// fills a chip with contrasting text (hue_contrast_text() - the common
// case), or, if that hue would exactly match the page background and the
// chip would otherwise vanish, falls back to a plain (no-fill) safe-
// contrast text color instead - the same idea agenda_avoid_bg_collision()
// applies to plain text, just for a chip's own fill. Shared by every
// chip role (priority A-D, due-overdue, due-today) so this "resolve hue,
// check collision, chip-or-plain" sequence exists in exactly one place
// rather than being repeated at every call site.
static void resolve_chip_color(const char *role_name, uint8_t default_r, uint8_t default_g,
                               uint8_t default_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                               uint8_t *fr, uint8_t *fg, uint8_t *fb, bool *has_bg, uint8_t *br,
                               uint8_t *bgg, uint8_t *bb)
{
    uint8_t hue_r, hue_g, hue_b;
    role_hue(role_name, default_r, default_g, default_b, &hue_r, &hue_g, &hue_b);
    if (agenda_colors_equal(hue_r, hue_g, hue_b, bg_r, bg_g, bg_b)) {
        *has_bg = false;
        agenda_safe_text_color(bg_r, bg_g, bg_b, fr, fg, fb);
        return;
    }
    *has_bg = true;
    hue_contrast_text(hue_r, hue_g, hue_b, fr, fg, fb);
    *br = hue_r;
    *bgg = hue_g;
    *bb = hue_b;
}

// Resolves a user-configurable *plain* role (no own fill): grayscale
// always renders it as plain black (no spare hue to assign - see
// priority_color()'s grayscale comment for why); a color board resolves
// the role's hue then swaps it for a safe contrasting color if it happens
// to exactly match the page background (agenda_avoid_bg_collision()).
// Shared by every no-chip role (due-later, +project, @context, and each
// Calendar source) so this "grayscale-or-hue, then collision check"
// sequence exists in exactly one place.
static void resolve_plain_color(const char *role_name, bool grayscale, uint8_t default_r,
                                uint8_t default_g, uint8_t default_b, uint8_t bg_r, uint8_t bg_g,
                                uint8_t bg_b, uint8_t *fr, uint8_t *fg, uint8_t *fb)
{
    if (grayscale) {
        *fr = *fg = *fb = 0;
    } else {
        role_hue(role_name, default_r, default_g, default_b, fr, fg, fb);
    }
    agenda_avoid_bg_collision(bg_r, bg_g, bg_b, fr, fg, fb);
}

// Used by the per-element ToDo coloring below: compares `due_date`
// ("YYYY-MM-DD") against `now`'s calendar date. All three flags are left
// false if there's no due date at all.
static void due_status(const char *due_date, time_t now, bool *overdue, bool *today, bool *future)
{
    *overdue = *today = *future = false;
    if (!due_date || due_date[0] == '\0') {
        return;
    }
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    // Oversized vs. the exact "YYYY-MM-DD" (10 chars) it normally holds -
    // silences -Wformat-truncation, which (correctly) can't prove
    // tm_year+1900 always fits in 4 digits from this call site alone. Only
    // the first 10 chars are ever compared below.
    char today_str[32];
    snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d", now_tm.tm_year + 1900,
             now_tm.tm_mon + 1, now_tm.tm_mday);
    int cmp = strncmp(due_date, today_str, 10);
    *overdue = cmp < 0;
    *today = (cmp == 0);
    *future = cmp > 0;
}

// ----------------------------------------------------------------------------
// Per-element ToDo coloring (docs/AGENDA_COLORS.html has the full derivation
// and rationale for every value below - priority, +project/@context, and
// due-date urgency are colored independently of each other within one row,
// rather than the row picking a single dominant color as event_role() above
// still does for the Calendar column).
// ----------------------------------------------------------------------------

#define AGENDA_MAX_RUNS_PER_LINE 20

typedef struct {
    char text[AGENDA_ROW_BUF_LEN];
    image_processor_text_run_t fg_runs[AGENDA_MAX_RUNS_PER_LINE];
    int fg_run_count;
    image_processor_text_run_t bg_runs[AGENDA_MAX_RUNS_PER_LINE];
    int bg_run_count;
} agenda_line_t;

static void add_run(image_processor_text_run_t *runs, int *count, int start, int length, uint8_t r,
                    uint8_t g, uint8_t b)
{
    if (*count >= AGENDA_MAX_RUNS_PER_LINE || length <= 0) {
        return;
    }
    runs[*count] = (image_processor_text_run_t){start, length, r, g, b};
    (*count)++;
}

// Priority letter -> background chip + contrasting flat text color. Every
// letter gets exactly the same treatment (a full-strength palette hue as
// fill, text color picked for contrast against it) rather than mixing
// "plain colored text" and "chip" styles per letter - besides being more
// uniform, this is also the only style that's safe against every palette
// hue: a Spectra6 board dithers any RGB value that isn't one of its 6 exact
// colors (find_closest_color() + error diffusion in image_processor.c), so
// a "lighter"/blended shade picked for a calmer look (as an earlier version
// of this function did for priority C/@context) can render as visual noise
// or even vanish into the surrounding white - only the 6 exact palette
// values are guaranteed solid. Each letter's hue is user-configurable
// (config_manager_get_agenda_pri_*_color()) - see role_hue()'s comment;
// hue_contrast_text() then picks the text color for whichever hue actually
// ended up assigned, so a user reassigning e.g. priority A to green still
// gets legible white text automatically. If a chosen hue exactly matches
// the page background, the chip would otherwise vanish entirely - falls
// back to a plain (no-fill) safe text color in that case, same idea as
// agenda_avoid_bg_collision() but for a chip's own fill rather than plain
// text. Grayscale boards have no bug to fix here (black text was always
// used) and have no per-role picker at all - see docs/AGENDA_COLORS.html
// for why grayscale doesn't have enough distinguishable fill levels to give
// every priority its own chip without them blurring together.
static void priority_color(char priority, bool grayscale, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                           uint8_t *fr, uint8_t *fg, uint8_t *fb, bool *has_bg, uint8_t *br,
                           uint8_t *bgg, uint8_t *bb)
{
    *has_bg = false;
    *fr = *fg = *fb = 0;  // black text is the safe default for every case below
    if (grayscale) {
        agenda_avoid_bg_collision(bg_r, bg_g, bg_b, fr, fg, fb);
        return;  // plain black, no fill - matches every other unhighlighted role
    }
    switch (priority) {
    case 'A':
        resolve_chip_color(config_manager_get_agenda_pri_a_color(), 255, 0, 0, bg_r, bg_g, bg_b, fr,
                           fg, fb, has_bg, br, bgg, bb);
        return;
    case 'B':
        resolve_chip_color(config_manager_get_agenda_pri_b_color(), 255, 255, 0, bg_r, bg_g, bg_b,
                           fr, fg, fb, has_bg, br, bgg, bb);
        return;
    case 'C':
        resolve_chip_color(config_manager_get_agenda_pri_c_color(), 0, 255, 0, bg_r, bg_g, bg_b, fr,
                           fg, fb, has_bg, br, bgg, bb);
        return;
    case 'D':
        resolve_chip_color(config_manager_get_agenda_pri_d_color(), 0, 0, 255, bg_r, bg_g, bg_b, fr,
                           fg, fb, has_bg, br, bgg, bb);
        return;
    default:
        agenda_avoid_bg_collision(bg_r, bg_g, bg_b, fr, fg, fb);
        return;  // no priority letter beyond D: plain black text, no fill
    }
}

// Due-date urgency -> foreground + optional background chip. Independent of
// priority_color() above - an item can carry both a priority marker and a
// due date, each colored on its own.
static void due_color(bool overdue, bool today, bool grayscale, uint8_t bg_r, uint8_t bg_g,
                      uint8_t bg_b, uint8_t *fr, uint8_t *fg, uint8_t *fb, bool *has_bg,
                      uint8_t *br, uint8_t *bgg, uint8_t *bb)
{
    *has_bg = false;
    if (overdue) {
        if (grayscale) {
            *has_bg = true;
            *fr = *fg = *fb = 255;
            *br = *bgg = *bb = 0;  // full inversion
            return;
        }
        resolve_chip_color(config_manager_get_agenda_due_overdue_color(), 255, 0, 0, bg_r, bg_g,
                           bg_b, fr, fg, fb, has_bg, br, bgg, bb);
        return;
    }
    if (today) {
        if (grayscale) {
            *has_bg = true;
            *fr = *fg = *fb = 0;
            *br = *bgg = *bb = 136;  // level 8
            return;
        }
        resolve_chip_color(config_manager_get_agenda_due_today_color(), 255, 255, 0, bg_r, bg_g,
                           bg_b, fr, fg, fb, has_bg, br, bgg, bb);
        return;
    }
    // Future due date, or no due date at all (caller only invokes this when
    // there is one): plain, no fill. Grayscale has no spare channel left
    // after overdue/today claim the two inversion levels, so it falls back
    // to plain body-text black - see docs/AGENDA_COLORS.html.
    resolve_plain_color(config_manager_get_agenda_due_later_color(), grayscale, 0, 0, 255, bg_r,
                        bg_g, bg_b, fr, fg, fb);
}

// Builds the full display line for one ToDo item - priority marker, body
// text, +project/@context tags (never shown before this change; todo.c
// strips them out of item->text into separate arrays), and a due-date
// suffix - each tracked as its own colored run rather than one color for
// the whole row. Pure logic, no drawing: image_processor_wrap_text() (called
// by the caller once this returns) may still truncate out->text with "...",
// which naturally drops or shortens whichever runs land past the cutoff
// since draw_text_runs() never draws past the string's actual length.
static void build_todo_line(const todo_item_t *item, time_t now, bool grayscale, uint8_t bg_r,
                            uint8_t bg_g, uint8_t bg_b, agenda_line_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    size_t cap = sizeof(out->text) - 1;  // leave room for the NUL

    if (item->priority != 0) {
        int start = (int) pos;
        int n = snprintf(out->text + pos, cap - pos + 1, "(%c) ", item->priority);
        if (n > 0) {
            size_t written = ((size_t) n <= cap - pos) ? (size_t) n : cap - pos;
            pos += written;
            uint8_t fr, fg, fb, br, bg, bb;
            bool has_bg;
            priority_color(item->priority, grayscale, bg_r, bg_g, bg_b, &fr, &fg, &fb, &has_bg, &br,
                           &bg, &bb);
            // Exclude the trailing separator space from both runs so the
            // chip (if any) doesn't visually merge into the body text.
            int len = (int) written > 0 ? (int) written - 1 : 0;
            add_run(out->fg_runs, &out->fg_run_count, start, len, fr, fg, fb);
            if (has_bg) {
                add_run(out->bg_runs, &out->bg_run_count, start, len, br, bg, bb);
            }
        }
    }

    size_t text_len = strlen(item->text);
    if (text_len > cap - pos) {
        text_len = cap - pos;
    }
    memcpy(out->text + pos, item->text, text_len);
    pos += text_len;
    // No run added: falls through to draw_text_runs()'s default (body) color.

    uint8_t proj_r, proj_g, proj_b;
    resolve_plain_color(config_manager_get_agenda_project_color(), grayscale, 0, 0, 255, bg_r, bg_g,
                        bg_b, &proj_r, &proj_g, &proj_b);
    for (int i = 0; i < item->project_count && pos < cap; i++) {
        int n = snprintf(out->text + pos, cap - pos + 1, " +%s", item->projects[i]);
        if (n <= 0) {
            continue;
        }
        size_t written = ((size_t) n <= cap - pos) ? (size_t) n : cap - pos;
        // Color from the '+' onward, not the separating space.
        add_run(out->fg_runs, &out->fg_run_count, (int) pos + 1, (int) written - 1, proj_r, proj_g,
                proj_b);
        pos += written;
    }

    // Exact Spectra6 palette hue, not a softer/blended shade - see the
    // comment on priority_color() above for why anything off-palette risks
    // dithering into visual noise (or vanishing) rather than rendering solid.
    uint8_t ctx_r, ctx_g, ctx_b;
    resolve_plain_color(config_manager_get_agenda_context_color(), grayscale, 0, 255, 0, bg_r, bg_g,
                        bg_b, &ctx_r, &ctx_g, &ctx_b);
    for (int i = 0; i < item->context_count && pos < cap; i++) {
        int n = snprintf(out->text + pos, cap - pos + 1, " @%s", item->contexts[i]);
        if (n <= 0) {
            continue;
        }
        size_t written = ((size_t) n <= cap - pos) ? (size_t) n : cap - pos;
        add_run(out->fg_runs, &out->fg_run_count, (int) pos + 1, (int) written - 1, ctx_r, ctx_g,
                ctx_b);
        pos += written;
    }

    if (item->due_date[0] != '\0' && pos < cap) {
        int start = (int) pos + 1;  // skip the separating space
        int n = snprintf(out->text + pos, cap - pos + 1, " (due %s)", item->due_date);
        if (n > 0) {
            size_t written = ((size_t) n <= cap - pos) ? (size_t) n : cap - pos;
            bool overdue, today, future;
            due_status(item->due_date, now, &overdue, &today, &future);
            uint8_t fr, fg, fb, br, bg, bb;
            bool has_bg;
            due_color(overdue, today, grayscale, bg_r, bg_g, bg_b, &fr, &fg, &fb, &has_bg, &br, &bg,
                      &bb);
            int len = (int) written > 0 ? (int) written - 1 : 0;
            add_run(out->fg_runs, &out->fg_run_count, start, len, fr, fg, fb);
            if (has_bg) {
                add_run(out->bg_runs, &out->bg_run_count, start, len, br, bg, bb);
            }
            pos += written;
        }
    }

    out->text[pos] = '\0';
}

// ----------------------------------------------------------------------------
// Per-element Calendar coloring: events are grouped by day under a dashed
// divider (weekday + day number, e.g. "Fr 11.") rather than repeating a full
// date on every line - the previous one-role-per-row scheme (today=yellow
// text, later=blue text) is gone entirely, since (a) yellow-on-white had the
// exact same low-contrast problem the ToDo column's due-today role had, and
// (b) with day-grouping "today" is already obvious (it's the first group
// after the header) without needing a per-row color for it. Time, date
// divider, and event text now each have their own fixed color instead.
// ----------------------------------------------------------------------------

typedef struct {
    char text[AGENDA_ROW_BUF_LEN];
    int calendar_index;  // 0=A..4=E - which color-profile cal_ink/cal_bg
                         // entry this line uses; resolved at DRAW time
                         // (draw_day_cell()/draw_calendar_column()'s day
                         // loop), not here, since a mono-mode profile's
                         // event ink can depend on whether the specific day
                         // it lands on is shift-marked (see
                         // agenda_color_profile_t's mono_invert comment) -
                         // a color-mode profile's ink never varies by day,
                         // but resolving both cases the same way (always at
                         // draw time) keeps this file down to one code path
                         // instead of two.
} agenda_event_line_t;

// Formats a duration in whole minutes as a compact bracket-free token:
// under an hour "45m", an exact number of hours "1h"/"2h", otherwise
// "1h30m" - matches the granularity ICS events actually have (minutes),
// without ever needing more than a handful of characters next to the
// "HH:MM " prefix it follows.
static void format_duration_compact(int total_minutes, char *out, size_t out_len)
{
    if (total_minutes < 60) {
        snprintf(out, out_len, "%dm", total_minutes);
    } else if (total_minutes % 60 == 0) {
        snprintf(out, out_len, "%dh", total_minutes / 60);
    } else {
        snprintf(out, out_len, "%dh%02dm", total_minutes / 60, total_minutes % 60);
    }
}

// Builds one event's display text - the start time (omitted for an all-day
// event), in one of three forms depending on
// config_manager_get_agenda_cal_time_display_mode() (NVS_AGENDA_CAL_SHOW_DURATION_KEY):
// plain "HH:MM " (off, default), "HH:MM [duration] " (duration), or
// "HH:MM-HH:MM " (range, only when a real end time is known - falls back to
// plain "HH:MM " otherwise, same as duration mode already does) - then the
// summary. Its color-profile source index is stashed for later - see
// agenda_event_line_t's own comment for why resolution happens at draw
// time instead of here. No date/day-of-week here: draw_calendar_column()
// shows that once per day group via draw_day_divider(), not repeated on
// every event.
static void build_event_line(const ics_event_t *ev, int calendar_index, agenda_event_line_t *out)
{
    memset(out, 0, sizeof(*out));
    out->calendar_index = calendar_index;
    size_t pos = 0;
    size_t cap = sizeof(out->text) - 1;

    if (!ev->all_day) {
        struct tm start_tm;
        localtime_r(&ev->start, &start_tm);
        agenda_time_display_mode_t time_mode = config_manager_get_agenda_cal_time_display_mode();
        int duration_min = (ev->end > ev->start) ? (int) ((ev->end - ev->start) / 60) : 0;

        int n;
        if (time_mode == AGENDA_TIME_DISPLAY_RANGE && duration_min > 0) {
            struct tm end_tm;
            localtime_r(&ev->end, &end_tm);
            n = snprintf(out->text + pos, cap - pos + 1, "%02d:%02d-%02d:%02d ", start_tm.tm_hour,
                         start_tm.tm_min, end_tm.tm_hour, end_tm.tm_min);
        } else {
            n = snprintf(out->text + pos, cap - pos + 1, "%02d:%02d ", start_tm.tm_hour,
                         start_tm.tm_min);
        }
        if (n > 0) {
            size_t written = ((size_t) n <= cap - pos) ? (size_t) n : cap - pos;
            pos += written;
        }

        if (time_mode == AGENDA_TIME_DISPLAY_DURATION && duration_min > 0) {
            char dur[16];
            format_duration_compact(duration_min, dur, sizeof(dur));
            int dn = snprintf(out->text + pos, cap - pos + 1, "[%s] ", dur);
            if (dn > 0) {
                size_t dwritten = ((size_t) dn <= cap - pos) ? (size_t) dn : cap - pos;
                pos += dwritten;
            }
        }
    }

    size_t slen = strlen(ev->summary);
    if (slen > cap - pos) {
        slen = cap - pos;
    }
    memcpy(out->text + pos, ev->summary, slen);
    pos += slen;
    out->text[pos] = '\0';
}

// Resolves the TOP shared header bar's bg/ink - never marked (there's only
// one top bar for the whole column, not one per day, unlike the per-day
// header below) and never affected by headerFollowsEntries either (that
// switch only ever touches the per-day header - confirmed against
// profile-editor.html's own buildStateRows(), whose "Oberster Header" row
// comment says exactly this). Mono uses the "header pair" - the OPPOSITE
// polarity from the day body (agenda_day_body_colors()'s own default) -
// matching profile-editor.html's `topBg = monoV ? m.headBg : hex(c.topBg)`
// (mono()'s headBg/headInk are always the inverse of its own page/ink).
static void agenda_top_header_colors(const agenda_color_profile_t *p, uint8_t *bg_r, uint8_t *bg_g,
                                     uint8_t *bg_b, uint8_t *fg_r, uint8_t *fg_g, uint8_t *fg_b)
{
    if (p->mono) {
        agenda_mono_pair(p->mono_invert, true, bg_r, bg_g, bg_b, fg_r, fg_g, fg_b);
        return;
    }
    *bg_r = p->top_bg.r;
    *bg_g = p->top_bg.g;
    *bg_b = p->top_bg.b;
    *fg_r = p->top_text.r;
    *fg_g = p->top_text.g;
    *fg_b = p->top_text.b;
}

// Resolves one day's HEADER bg/ink - a direct, case-by-case port of
// profile-editor.html's render() per-day header logic (its own if/else-if
// chain over markColorsHeader/headerFollowsEntries(Color)), verified
// against its exact precedence:
//   1. markColorsHeader && marked: header takes the mark color, with an
//      auto-derived contrasting ink (no dedicated "mark ink" field).
//   2. headerFollowsEntries(Color) && marked (and case 1 didn't already
//      apply, i.e. markColorsHeader is off): header ALSO takes the mark
//      color, with the day's plain body ink (`text`) verbatim by default
//      (headerFollowsEntriesColorSafeInk swaps that literal ink for an
//      auto-derived contrast against `mark`, same math as case 1's, when
//      the literal choice would otherwise be unreadable - see this
//      profile field's own doc comment in agenda_color_profile.h) - color
//      mode only; mono always ends up identical to case 4 regardless (see
//      below).
//   3. headerFollowsEntries(Color) && NOT marked: header abandons its own
//      header_text/header_bg pair and shows the day body's own pair
//      (`text`/`text_bg`) instead - "the header follows the entries".
//   4. Neither applies: header always shows its own header_text/header_bg
//      pair, unaffected by marking - the original, still-default behavior.
// Mono simplifies remarkably: profile-editor.html's mono() always makes
// markBg/markInk (mono) exactly equal headBg/headInk (the "header pair",
// opposite of the body's own "page pair") - so BOTH case 1 and case 2
// resolve to the plain header pair in mono, identical to case 4. The one
// mono case that actually differs is case 3 (headerFollowsEntries on, day
// NOT marked), which takes the body's own "page pair" instead. Net effect:
// in mono, marking a header is *never visually distinguishable* - a
// limitation inherited directly from the tool's own math, not something
// this firmware adds.
static void agenda_day_header_colors(const agenda_color_profile_t *p, bool day_marked,
                                     uint8_t *bg_r, uint8_t *bg_g, uint8_t *bg_b, uint8_t *fg_r,
                                     uint8_t *fg_g, uint8_t *fg_b)
{
    bool marked = day_marked && p->has_mark;
    bool header_follows = p->mono ? p->header_follows_entries : p->header_follows_entries_color;

    if (p->mono) {
        // header_follows && !marked -> body's own "page" pair; every other
        // combination -> the header pair (see this function's own comment
        // for why marking never actually changes it in mono).
        bool use_body_pair = header_follows && !marked;
        agenda_mono_pair(p->mono_invert, !use_body_pair, bg_r, bg_g, bg_b, fg_r, fg_g, fg_b);
        return;
    }

    if (p->mark_colors_header && marked) {
        *bg_r = p->mark.r;
        *bg_g = p->mark.g;
        *bg_b = p->mark.b;
        agenda_safe_text_color(*bg_r, *bg_g, *bg_b, fg_r, fg_g, fg_b);
        return;
    }
    if (header_follows && marked) {
        *bg_r = p->mark.r;
        *bg_g = p->mark.g;
        *bg_b = p->mark.b;
        if (p->header_follows_entries_color_safe_ink) {
            agenda_safe_text_color(*bg_r, *bg_g, *bg_b, fg_r, fg_g, fg_b);
        } else {
            *fg_r = p->text.r;
            *fg_g = p->text.g;
            *fg_b = p->text.b;
        }
        return;
    }
    if (header_follows) {
        *bg_r = p->text_bg.r;
        *bg_g = p->text_bg.g;
        *bg_b = p->text_bg.b;
        *fg_r = p->text.r;
        *fg_g = p->text.g;
        *fg_b = p->text.b;
        return;
    }
    *bg_r = p->header_bg.r;
    *bg_g = p->header_bg.g;
    *bg_b = p->header_bg.b;
    *fg_r = p->header_text.r;
    *fg_g = p->header_text.g;
    *fg_b = p->header_text.b;
}

// Resolves one day's BODY/page bg/ink (the plain page its events sit on,
// and the ink "+N more"/multi-day-prefix text uses) - mirrors
// agenda_day_header_colors() above but body marking only applies when
// markColorsHeader is false.
static void agenda_day_body_colors(const agenda_color_profile_t *p, bool day_marked, uint8_t *bg_r,
                                   uint8_t *bg_g, uint8_t *bg_b, uint8_t *fg_r, uint8_t *fg_g,
                                   uint8_t *fg_b)
{
    bool marked_here = day_marked && p->has_mark && !p->mark_colors_header;
    if (p->mono) {
        agenda_mono_pair(p->mono_invert, marked_here, bg_r, bg_g, bg_b, fg_r, fg_g, fg_b);
        return;
    }
    if (marked_here) {
        *bg_r = p->mark.r;
        *bg_g = p->mark.g;
        *bg_b = p->mark.b;
        agenda_safe_text_color(*bg_r, *bg_g, *bg_b, fg_r, fg_g, fg_b);
        return;
    }
    *bg_r = p->text_bg.r;
    *bg_g = p->text_bg.g;
    *bg_b = p->text_bg.b;
    *fg_r = p->text.r;
    *fg_g = p->text.g;
    *fg_b = p->text.b;
}

// Resolves one Calendar event's own ink and optional chip background for
// `calendar_index` (0=A..4=E). Color-mode ink never itself changes with
// marked state (only its chip does, see below) - the profile author is
// expected to have already verified the pairing looks right in the tool's
// own live preview, matching this section's own top comment. `body_marked`
// mirrors agenda_day_body_colors()'s own marked_here.
static void agenda_event_colors(const agenda_color_profile_t *p, int calendar_index,
                                bool body_marked, uint8_t body_ink_r, uint8_t body_ink_g,
                                uint8_t body_ink_b, bool *has_chip, uint8_t *chip_r,
                                uint8_t *chip_g, uint8_t *chip_b, uint8_t *ink_r, uint8_t *ink_g,
                                uint8_t *ink_b)
{
    if (p->mono) {
        // No distinct per-source chip in mono - the letter/name text is
        // the only disambiguator, matching the pre-profile grayscale
        // behavior this replaces.
        *has_chip = false;
        *ink_r = body_ink_r;
        *ink_g = body_ink_g;
        *ink_b = body_ink_b;
        return;
    }
    *ink_r = p->cal_ink[calendar_index].r;
    *ink_g = p->cal_ink[calendar_index].g;
    *ink_b = p->cal_ink[calendar_index].b;
    if (body_marked) {
        // The whole day area (including every event's own chip) already
        // became one continuous "mark" fill - drawing another chip in the
        // same color on top would be a no-op, so skip it and let the ink
        // sit directly on that fill.
        *has_chip = false;
        return;
    }
    *has_chip = true;
    *chip_r = p->cal_bg[calendar_index].r;
    *chip_g = p->cal_bg[calendar_index].g;
    *chip_b = p->cal_bg[calendar_index].b;
}

// Resolves the weather icon's own background - deliberately never derived
// from the day header's bg (which used to make a same-colored icon
// invisible, including the two-color case where the shift model's mark
// happens to match a header/mark color the icon's fixed traffic-light hue
// also uses - reported live on both a color and a monochrome display).
// Color mode: the profile's own literal `icon_bg`/`icon_bg_marked` depending
// on `marked` (the author is expected to have picked something that doesn't
// collide with any of their traffic-light hues, same "verify in the
// preview" expectation as every other literal color here). Mono mode:
// always the exact opposite of `head_bg`, the actual resolved header
// background for THIS specific day (which itself can be either polarity
// depending on marking) - with only two colors possible in mono, always
// taking the other one is a structural guarantee against collision, not a
// guess; `icon_bg_marked` has no effect there, same as `icon_bg`.
static void agenda_icon_colors(const agenda_color_profile_t *p, bool marked, uint8_t head_bg_r,
                               uint8_t head_bg_g, uint8_t head_bg_b, uint8_t *bg_r, uint8_t *bg_g,
                               uint8_t *bg_b)
{
    if (p->mono) {
        bool header_is_white = head_bg_r > 127;
        uint8_t v = header_is_white ? 0 : 255;
        *bg_r = *bg_g = *bg_b = v;
        return;
    }
    const agenda_rgb_t *icon_bg = (marked && p->has_mark) ? &p->icon_bg_marked : &p->icon_bg;
    *bg_r = icon_bg->r;
    *bg_g = icon_bg->g;
    *bg_b = icon_bg->b;
}

// True if `c` is a reserved weather-icon marker byte (see
// WEATHER_ICON_MARKER_BASE's doc comment in weather.h) - mirrors
// image_processor.c's identical file-private helper, needed here too since
// draw_day_divider() below has to split a weather chip's bracket text from
// its trailing icon to give the icon its own independent background (see
// agenda_icon_colors() above).
static bool agenda_is_weather_icon_marker(char c)
{
    unsigned char b = (unsigned char) c;
    return b >= WEATHER_ICON_MARKER_BASE && b < WEATHER_ICON_MARKER_BASE + WEATHER_ICON_COUNT;
}

// Draws a day-separator row: a dashed horizontal line with a chip showing
// the weekday + day number, plus (if `weather_mode` is on and this
// particular day has one) a second chip with that day's forecast.
// `fill_r/g/b`/`text_r/g/b` are the caller's already-resolved header
// bg/ink pair for this specific day (the active color profile's plain
// header colors, or its "mark" color + an auto-derived contrasting ink
// when this day is header-marked - see draw_day_cell()/draw_calendar_column()
// for how that's picked) - this function just draws with them literally,
// no further contrast adjustment of its own.
//
// `weather_mode` reflects whether the weather annotation feature is on and
// actually returned data this cycle - it is NOT the same thing as whether
// `weather_text` happens to be set for *this* day. With weather_mode off,
// the day label is always centered on the line (dashes both sides) -
// unchanged from before this feature. With weather_mode on, every day
// uses the same left-aligned-label layout, even a day with no forecast
// entry of its own (most commonly a 4th calendar day, since
// WEATHER_FORECAST_DAYS is 3) - it just gets one continuous dashed run
// instead of a second chip, rather than reverting to the centered layout
// for that one day, which would look inconsistent against its neighbors.
// `weather_right_aligned` (only consulted when weather_mode is on) flips
// the forecast chip's placement within its reserved space from centered
// to flush against the row's right edge - purely where it sits, not how
// much of it fits, since max_weather_chars below reserves the same amount
// of space either way.
// `divider_filled` (the color profile's "headerDividerFilled") replaces the
// dashed line's usual gaps - which otherwise show the page background
// through them, visually reading as "two different colors" in one header
// even though the dashes and the day label use the identical fill/text
// pair - with one continuous fill_r/g/b bar spanning the whole row. That
// bar spans the row's full width/height (`rect.w`, `row_h` worth of
// height - not just the inset `total_w`/`FONT_HEIGHT` every other fill in
// this function uses for the label/dash positioning), matching the
// gutter-to-gutter fill the top header already uses - anything narrower
// leaves this row's own outer padding margins showing whatever the caller
// painted underneath beforehand (typically the day list's single ambient
// page color) rather than this specific day's `fill_r/g/b`, which reads as
// a stray colored frame around any header whose own color diverges from
// that ambient one - reported live (2026-09-20) as a black partial frame
// around a marked (white) header sitting in an otherwise black column.
// `icon_bg_r/g/b` is the weather icon's own independent background (see
// agenda_icon_colors()) - never fill_r/g/b, so a same-colored header
// (including a shift-marked one) can never swallow the icon.
// `leader_line` (the color profile's "dayheadLeaderLine") draws a decorative
// dashed rule, in text_r/g/b (ink), between the day label and the weather
// chip - independent of `divider_filled`, since it's a foreground accent on
// top of whatever background resulted, not a background gap/fill choice
// (matches profile-editor.html's own `.leader{border-bottom:2px dashed
// currentColor}`, which is ink-colored regardless of the day's background).
// Only meaningful when `weather_mode` is on, matching the tool's own scope
// - it has no equivalent for the centered-label (`!weather_mode`) layout.
static void draw_day_divider(uint8_t *rgb, int width, int height, agenda_rect_t rect, int y,
                             const char *label, bool weather_mode, bool weather_right_aligned,
                             const char *weather_text, uint8_t fill_r, uint8_t fill_g,
                             uint8_t fill_b, uint8_t text_r, uint8_t text_g, uint8_t text_b,
                             bool divider_filled, uint8_t icon_bg_r, uint8_t icon_bg_g,
                             uint8_t icon_bg_b, bool leader_line)
{
    int total_w = rect.w - 2 * AGENDA_PADDING;
    int line_y = y + IMAGE_PROCESSOR_FONT_HEIGHT / 2 - 1;
    const int dash_len = 4, gap_len = 3, dash_h = 2;

    if (divider_filled) {
        // Full gutter-to-gutter width and the whole row_h-worth of height
        // (IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING, matching every
        // caller's own row_h) - see this function's doc comment above for
        // why the narrower total_w/FONT_HEIGHT inset every other fill here
        // uses would leave a colored frame around this specific fill.
        image_processor_fill_rect(rgb, width, height, rect.x, y, rect.w,
                                  IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING, fill_r, fill_g,
                                  fill_b);
    }

    if (!weather_mode) {
        int label_w = (int) strlen(label) * IMAGE_PROCESSOR_FONT_WIDTH;
        if (label_w > total_w) {
            label_w = total_w;  // pathologically narrow column - clip rather than overflow
        }
        int label_x = rect.x + AGENDA_PADDING + (total_w - label_w) / 2;

        if (!divider_filled) {
            for (int x = rect.x + AGENDA_PADDING; x + dash_len <= label_x;
                 x += dash_len + gap_len) {
                image_processor_fill_rect(rgb, width, height, x, line_y, dash_len, dash_h, fill_r,
                                          fill_g, fill_b);
            }
            int right_start = label_x + label_w;
            int right_end = rect.x + AGENDA_PADDING + total_w;
            for (int x = right_start; x + dash_len <= right_end; x += dash_len + gap_len) {
                image_processor_fill_rect(rgb, width, height, x, line_y, dash_len, dash_h, fill_r,
                                          fill_g, fill_b);
            }
        }

        if (label_w > 0) {
            image_processor_fill_rect(rgb, width, height, label_x - 2, y, label_w + 4,
                                      IMAGE_PROCESSOR_FONT_HEIGHT, fill_r, fill_g, fill_b);
            image_processor_draw_text(rgb, width, height, label_x, y, label, text_r, text_g,
                                      text_b);
        }
        return;
    }

    int label_w = (int) strlen(label) * IMAGE_PROCESSOR_FONT_WIDTH;
    if (label_w > total_w) {
        label_w = total_w;
    }
    int label_x = rect.x + AGENDA_PADDING;  // left-aligned
    int right_end = rect.x + AGENDA_PADDING + total_w;

    // Splitting the weather chip's trailing icon (if any) from its bracket
    // text needs a fixed pixel gap between the two independent boxes, not a
    // text-column space - see the drawing code below. Counted into
    // weather_w up front so centering/right-align math (which only ever
    // sees one combined width) stays correct either way.
    const int icon_gap = 4;

    // No forecast entry for this specific day: weather_w stays 0 and
    // weather_x defaults to the row's right edge, which collapses the two
    // dash runs below into a single continuous one spanning the whole gap
    // after the label - not two runs that happen to line up.
    int weather_w = 0;
    int weather_x = right_end;
    bool weather_has_icon = false;
    char weather_clipped[WEATHER_DAY_LINE_MAX_LEN] = "";
    if (weather_text && weather_text[0] != '\0') {
        // Clip the weather chip to whatever's left after the label plus a
        // minimum gap, rather than letting it overlap - a narrow column
        // with a long day label (unlikely, but the label itself is
        // already clipped above for the same reason) is the only case
        // this ever triggers.
        int max_weather_width = total_w - label_w - (dash_len + gap_len);
        if (max_weather_width < 0) {
            max_weather_width = 0;
        }
        strncpy(weather_clipped, weather_text, sizeof(weather_clipped) - 1);
        weather_clipped[sizeof(weather_clipped) - 1] = '\0';
        // Trim from the end until it fits - a plain char-count cutoff would
        // be wrong once a weather-icon marker byte (wider than a normal
        // character) is embedded; this string is short (one day's forecast
        // chip, well under WEATHER_DAY_LINE_MAX_LEN), so re-measuring the
        // whole string per trimmed byte is negligible cost, once per
        // calendar day per render. The icon_gap only actually applies once
        // a trailing icon marker survives clipping - re-checked every pass
        // since clipping can trim the marker itself away first under real
        // space pressure.
        while (weather_clipped[0] != '\0') {
            size_t clen = strlen(weather_clipped);
            bool has_icon = agenda_is_weather_icon_marker(weather_clipped[clen - 1]);
            int w = image_processor_measure_text_width(weather_clipped) + (has_icon ? icon_gap : 0);
            if (w <= max_weather_width) {
                break;
            }
            weather_clipped[clen - 1] = '\0';
        }
        size_t clipped_len = strlen(weather_clipped);
        weather_has_icon =
            clipped_len > 0 && agenda_is_weather_icon_marker(weather_clipped[clipped_len - 1]);
        weather_w =
            image_processor_measure_text_width(weather_clipped) + (weather_has_icon ? icon_gap : 0);
        weather_x = weather_right_aligned ? (right_end - weather_w)
                                          : (rect.x + AGENDA_PADDING + (total_w - weather_w) / 2);
        int min_weather_x = label_x + label_w + dash_len;
        if (weather_w > 0 && weather_x < min_weather_x) {
            weather_x = min_weather_x;
        }
    }

    if (leader_line) {
        for (int x = label_x + label_w; x + dash_len <= weather_x; x += dash_len + gap_len) {
            image_processor_fill_rect(rgb, width, height, x, line_y, dash_len, dash_h, text_r,
                                      text_g, text_b);
        }
    }
    if (!divider_filled) {
        int right_start = weather_x + weather_w;
        for (int x = right_start; x + dash_len <= right_end; x += dash_len + gap_len) {
            image_processor_fill_rect(rgb, width, height, x, line_y, dash_len, dash_h, fill_r,
                                      fill_g, fill_b);
        }
    }

    if (label_w > 0) {
        image_processor_fill_rect(rgb, width, height, label_x - 2, y, label_w + 4,
                                  IMAGE_PROCESSOR_FONT_HEIGHT, fill_r, fill_g, fill_b);
        image_processor_draw_text(rgb, width, height, label_x, y, label, text_r, text_g, text_b);
    }
    if (weather_w > 0) {
        if (weather_has_icon) {
            size_t clipped_len = strlen(weather_clipped);
            char bracket_part[WEATHER_DAY_LINE_MAX_LEN];
            memcpy(bracket_part, weather_clipped, clipped_len - 1);
            bracket_part[clipped_len - 1] = '\0';
            int bracket_w = image_processor_measure_text_width(bracket_part);
            if (bracket_w > 0) {
                image_processor_fill_rect(rgb, width, height, weather_x - 2, y, bracket_w + 4,
                                          IMAGE_PROCESSOR_FONT_HEIGHT, fill_r, fill_g, fill_b);
                image_processor_draw_text(rgb, width, height, weather_x, y, bracket_part, text_r,
                                          text_g, text_b);
            }
            // The icon gets its own box in icon_bg_r/g/b - never fill_r/g/b
            // - with an ink auto-derived from THAT background, since the
            // icon's own fixed traffic-light hue already carries meaning
            // and can't be reassigned (see agenda_icon_colors()'s comment).
            int icon_x = weather_x + bracket_w + icon_gap;
            image_processor_fill_rect(rgb, width, height, icon_x - 2, y, WEATHER_ICON_WIDTH + 4,
                                      IMAGE_PROCESSOR_FONT_HEIGHT, icon_bg_r, icon_bg_g, icon_bg_b);
            char icon_str[2] = {weather_clipped[clipped_len - 1], '\0'};
            uint8_t icon_ink_r, icon_ink_g, icon_ink_b;
            agenda_safe_text_color(icon_bg_r, icon_bg_g, icon_bg_b, &icon_ink_r, &icon_ink_g,
                                   &icon_ink_b);
            image_processor_draw_text_on_bg(rgb, width, height, icon_x, y, icon_str, icon_ink_r,
                                            icon_ink_g, icon_ink_b, icon_bg_r, icon_bg_g,
                                            icon_bg_b);
        } else {
            image_processor_fill_rect(rgb, width, height, weather_x - 2, y, weather_w + 4,
                                      IMAGE_PROCESSOR_FONT_HEIGHT, fill_r, fill_g, fill_b);
            image_processor_draw_text(rgb, width, height, weather_x, y, weather_clipped, text_r,
                                      text_g, text_b);
        }
    }
}

// Local midnight containing `t`.
static time_t day_start(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

// True if calendar day `day` (a day_start()-aligned timestamp) falls within
// [ev->start, ev->end) - so a multi-day event (a multi-day all-day event, or
// even an ordinary timed event that happens to cross midnight) is found on
// every day it touches, not just the day its DTSTART falls on. `end` is
// exclusive per RFC 5545 (an all-day DTEND is the day *after* the last day),
// so the last day considered is day_start(end - 1 second).
static bool event_touches_day(const ics_event_t *ev, time_t day)
{
    time_t ev_day_start = day_start(ev->start);
    time_t last_instant = (ev->end > ev->start) ? ev->end - 1 : ev->start;
    time_t ev_day_end = day_start(last_instant);
    return day >= ev_day_start && day <= ev_day_end;
}

// Total whole days `ev` spans (inclusive of both its first and last day) -
// used by the COMPACT/REPEAT_NUMBERED multi-day display modes below. A
// 1-day event (the overwhelming majority) returns 1.
static int event_total_days(const ics_event_t *ev)
{
    time_t ev_day_start = day_start(ev->start);
    time_t last_instant = (ev->end > ev->start) ? ev->end - 1 : ev->start;
    time_t ev_day_end = day_start(last_instant);
    return (int) ((ev_day_end - ev_day_start) / 86400) + 1;
}

// 1-based position of `day` within `ev`'s own full span, counting from its
// actual start date regardless of whether that start is inside the visible
// lookahead window - e.g. an 8-day event whose day 4 is the first day
// visible in a 3-day window still reports "4", not "1".
static int event_day_index(const ics_event_t *ev, time_t day)
{
    return (int) ((day - day_start(ev->start)) / 86400) + 1;
}

#define AGENDA_MAX_CAL_DAYS 8  // generous vs. the 1-4 calendar days a 1-3 day lookahead can touch
#define AGENDA_MAX_TAGGED_EVENTS (ICS_MAX_EVENTS * 5)  // events_a/b/c/d/e, worst case all five full

// One event plus the pre-resolved line/color build_event_line() computed
// for it (which already baked in which calendar it came from) - merging
// events_a/events_b into one array of these is what lets the rest of this
// function treat "two calendars" as "one sorted list" without caring which
// source any given entry came from again.
typedef struct {
    const ics_event_t *ev;
    const agenda_event_line_t *line;
} agenda_tagged_event_t;

// One calendar source's header display name - see draw_calendar_column()'s
// header-drawing comment. Drawn directly in that source's own
// profile.cal_ink[] color (or plain top-header ink on a mono profile), no
// separate swatch box - matches profile-editor.html's own top-header
// legend, which colors the name text itself rather than a preceding chip.
typedef struct {
    bool show;
    const char *name;
} agenda_cal_name_tag_t;

static int compare_tagged_by_start(const void *a, const void *b)
{
    const agenda_tagged_event_t *ta = (const agenda_tagged_event_t *) a;
    const agenda_tagged_event_t *tb = (const agenda_tagged_event_t *) b;
    if (ta->ev->start < tb->ev->start) {
        return -1;
    }
    if (ta->ev->start > tb->ev->start) {
        return 1;
    }
    return 0;
}

// Draws the Calendar column: a header bar (title plus a "last updated"
// timestamp - no day-count parenthetical, see the Web UI/README for that),
// then one draw_day_divider() per distinct calendar day touched by any
// event from either source, each followed by every event that touches
// that day - including a multi-day event, which by default (REPEAT mode)
// is repeated under each day it spans rather than shown once under its
// start day only; see agenda_multiday_mode_t for the other two modes. The
// day list is derived from the merged events but clipped to the
// lookahead window explicitly too, since a multi-day event's own span can
// extend past the window on either side even though it overlaps it. Stops
// once the column runs out of vertical room, reserving a row for "+N more"
// only if not everything actually fits.
// Finds `day`'s forecast entry (matched by "YYYY-MM-DD", same format
// ics_event_t/todo_item_t dates already use elsewhere in this file), if
// `weather` is present and actually covers that date. WEATHER_FORECAST_DAYS
// is 3, so a day past that (most commonly a 4th calendar day reached late
// in the evening - see calendar_ics.c) simply has no entry, same as
// `weather` being NULL outright.
static bool find_weather_for_day(const weather_forecast_t *weather, time_t day,
                                 const weather_day_t **out)
{
    if (!weather || !weather->valid) {
        return false;
    }
    struct tm tm;
    localtime_r(&day, &tm);
    char date_str[11];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm);
    for (int i = 0; i < weather->count; i++) {
        if (strcmp(weather->days[i].date, date_str) == 0) {
            *out = &weather->days[i];
            return true;
        }
    }
    return false;
}

// Colors mirror image_processor_draw_climate_badges()'s convention exactly
// (Bad=Red, Good=Yellow standing in for orange - this board's real palette
// has no true orange, Super=Green; a single black chip on grayscale
// boards, no color distinction possible there). No background-collision
// avoidance is needed for the box itself (always high-contrast by
// construction - same reasoning as the day-count "N/N:" prefix and the
// per-source name swatches elsewhere in this file) - but the TEXT color
// still needs to pick white vs. black per background, see
// climate_chip_text_color() below.
static void climate_chip_colors(climate_category_t category, bool grayscale, uint8_t *bg_r,
                                uint8_t *bg_g, uint8_t *bg_b)
{
    if (grayscale) {
        *bg_r = *bg_g = *bg_b = 0;
        return;
    }
    switch (category) {
    case CLIMATE_CATEGORY_BAD:
        *bg_r = 255;
        *bg_g = 0;
        *bg_b = 0;
        break;
    case CLIMATE_CATEGORY_SUPER:
        *bg_r = 0;
        *bg_g = 255;
        *bg_b = 0;
        break;
    case CLIMATE_CATEGORY_GOOD:
    default:
        *bg_r = 255;
        *bg_g = 255;
        *bg_b = 0;
        break;
    }
}

// White text reads fine on Red/Green/the grayscale chip's Black, but not on
// Good's Yellow background - too little contrast to read on the actual
// e-paper panel (confirmed live, same issue as the photo-overlay badges'
// climate_badge_text_color()). Black text instead, only for that one case.
static void climate_chip_text_color(climate_category_t category, bool grayscale, uint8_t *fg_r,
                                    uint8_t *fg_g, uint8_t *fg_b)
{
    if (!grayscale && category == CLIMATE_CATEGORY_GOOD) {
        *fg_r = *fg_g = *fg_b = 0;
        return;
    }
    *fg_r = *fg_g = *fg_b = 255;
}

// Draws one right-aligned chip (colored box + text) in a column header,
// anchored so its right edge sits at `right_edge_x` - returns the x the
// next (further left) chip should use as its own right edge, same
// chaining shape as image_processor_draw_climate_badges()'s photo-overlay
// counterpart.
static int draw_header_climate_chip(uint8_t *rgb, int width, int height, int right_edge_x, int y,
                                    const char *text, climate_category_t category, bool grayscale)
{
    int text_len = (int) strlen(text);
    int chip_w = text_len * IMAGE_PROCESSOR_FONT_WIDTH + IMAGE_PROCESSOR_FONT_WIDTH / 2;
    int x = right_edge_x - chip_w;
    if (x < 0) {
        x = 0;
    }
    uint8_t bg_r, bg_g, bg_b;
    climate_chip_colors(category, grayscale, &bg_r, &bg_g, &bg_b);
    image_processor_fill_rect(rgb, width, height, x, y, chip_w, IMAGE_PROCESSOR_FONT_HEIGHT, bg_r,
                              bg_g, bg_b);
    uint8_t fg_r, fg_g, fg_b;
    climate_chip_text_color(category, grayscale, &fg_r, &fg_g, &fg_b);
    image_processor_draw_text(rgb, width, height, x + IMAGE_PROCESSOR_FONT_WIDTH / 4, y, text, fg_r,
                              fg_g, fg_b);
    return x - IMAGE_PROCESSOR_FONT_WIDTH / 2;
}

// Draws the optional climate readout (see agenda_climate_t) right-aligned
// in a column header bar - shared by both draw_todo_column() and
// draw_calendar_column(), which otherwise leave this space empty after
// their own header text (confirmed free in both). No-op (climate is NULL:
// feature off, both sensor reads failed, or this specific header was
// de-duplicated away - see agenda_renderer_render()'s own comment) or drew
// nothing at all either way, returns the right edge (`rect.x+rect.w-
// AGENDA_PADDING`, i.e. unclaimed) - callers use the return value to place
// their own right-aligned header content (the timestamp) immediately to
// its left, so it's never drawn underneath/behind the climate chips.
static int draw_header_climate(uint8_t *rgb, int width, int height, agenda_rect_t rect,
                               const agenda_climate_t *climate)
{
    int right_edge_x = rect.x + rect.w - AGENDA_PADDING;
    if (!climate) {
        return right_edge_x;
    }
    bool grayscale = agenda_board_is_grayscale();
    int y = rect.y + AGENDA_PADDING;
    if (climate->has_hum) {
        right_edge_x =
            draw_header_climate_chip(rgb, width, height, right_edge_x, y, climate->hum_text,
                                     climate->hum_category, grayscale);
    }
    if (climate->has_temp) {
        right_edge_x =
            draw_header_climate_chip(rgb, width, height, right_edge_x, y, climate->temp_text,
                                     climate->temp_category, grayscale);
    }
    return right_edge_x;
}

// Local noon (not midnight) for the given calendar date - deliberately
// avoids any DST-transition-day arithmetic surprise: a transition always
// happens at a small fixed hour (e.g. 2/3 AM), never at noon, so the gap
// between any two local noons is always an exact multiple of 86400 seconds
// regardless of DST, unlike midnight-to-midnight which can be 23h or 25h on
// the transition day itself.
static time_t agenda_local_noon(int year_1900, int mon0, int mday)
{
    struct tm tm = {0};
    tm.tm_year = year_1900;
    tm.tm_mon = mon0;
    tm.tm_mday = mday;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

// Resolves the 2-group rotation/"shift" model (agenda_shift_model_t,
// config.h) for one calendar day - returns 0 (group1) or 1 (group2), or -1
// if the model is NONE or `start_date_str` ("YYYY-MM-DD") is unset/
// unparseable (fail-soft: no coloring rather than a guess). Each preset's
// segment lengths sum to one 7-day "half"; which group holds the first
// segment of a half flips every OTHER half, giving the real
// alternating-fortnightly pattern real-world custody schedules use (e.g.
// "2-2-3": week 1 is group1/group1/group2/group2/group1/group1/group1,
// week 2 flips to group2/group2/group1/group1/group2/group2/group2) -
// anchored so `start_date_str` itself falls on the first day of the first
// (non-flipped) half. Well-defined for any `day` before or after
// start_date_str too (floor-style modulo on the signed day difference).
static int agenda_shift_group_for_day(agenda_shift_model_t model, const char *start_date_str,
                                      time_t day)
{
    if (model == AGENDA_SHIFT_MODEL_NONE || !start_date_str || start_date_str[0] == '\0') {
        return -1;
    }
    int start_y, start_m, start_d;
    if (sscanf(start_date_str, "%d-%d-%d", &start_y, &start_m, &start_d) != 3) {
        return -1;
    }

    static const int SEGMENTS_2_2_3[] = {2, 2, 3};
    static const int SEGMENTS_WEEK_WEEK[] = {7};
    static const int SEGMENTS_3_4[] = {3, 4};
    const int *segments;
    int segment_count;
    switch (model) {
    case AGENDA_SHIFT_MODEL_2_2_3:
        segments = SEGMENTS_2_2_3;
        segment_count = 3;
        break;
    case AGENDA_SHIFT_MODEL_WEEK_WEEK:
        segments = SEGMENTS_WEEK_WEEK;
        segment_count = 1;
        break;
    case AGENDA_SHIFT_MODEL_3_4:
        segments = SEGMENTS_3_4;
        segment_count = 2;
        break;
    default:
        return -1;
    }
    int cycle_len = 0;
    for (int i = 0; i < segment_count; i++) {
        cycle_len += segments[i];
    }

    struct tm day_tm;
    localtime_r(&day, &day_tm);
    time_t day_noon = agenda_local_noon(day_tm.tm_year, day_tm.tm_mon, day_tm.tm_mday);
    time_t start_noon = agenda_local_noon(start_y - 1900, start_m - 1, start_d);
    double diff_seconds = difftime(day_noon, start_noon);
    long days_since_start =
        (long) (diff_seconds >= 0 ? diff_seconds / 86400.0 + 0.5 : diff_seconds / 86400.0 - 0.5);

    long total_cycle = 2L * cycle_len;
    long pos = days_since_start % total_cycle;
    if (pos < 0) {
        pos += total_cycle;
    }
    int which_half = (int) (pos / cycle_len);
    int pos_in_half = (int) (pos % cycle_len);

    int segment_idx = 0, acc = 0;
    for (int i = 0; i < segment_count; i++) {
        acc += segments[i];
        if (pos_in_half < acc) {
            segment_idx = i;
            break;
        }
    }
    int group = segment_idx % 2;
    if (which_half == 1) {
        group = 1 - group;
    }
    return group;
}

// Draws one calendar source's header legend label (A/B's full name, or
// C/D/E's single letter) in that source's own profile.cal_ink[] color (or
// plain top-header ink on a mono profile) - no separate swatch/badge box,
// matching profile-editor.html's own top-header legend. Returns the x the
// next label (or following text) should start at.
static int draw_calendar_label(uint8_t *rgb, int width, int height, int x, int y, const char *label,
                               const agenda_color_profile_t *profile, int calendar_index,
                               uint8_t top_text_r, uint8_t top_text_g, uint8_t top_text_b)
{
    uint8_t r, g, b;
    if (profile->mono) {
        r = top_text_r;
        g = top_text_g;
        b = top_text_b;
    } else {
        r = profile->cal_ink[calendar_index].r;
        g = profile->cal_ink[calendar_index].g;
        b = profile->cal_ink[calendar_index].b;
    }
    image_processor_draw_text(rgb, width, height, x, y, label, r, g, b);
    return x + (int) strlen(label) * IMAGE_PROCESSOR_FONT_WIDTH;
}

// Draws one day's divider row plus as many of its events as fit within
// `cell` - the 7-day grid's per-cell counterpart to draw_calendar_column()'s
// shared-budget day loop. Each grid cell gets its own independent, FIXED
// budget derived purely from `cell.h` (never resized to the day's actual
// content) - a deliberate choice so every day-header lands on the same
// fixed grid row regardless of column, matching the plain calendar-grid
// look the two Layout_*.jpg concept images show; a day with little content
// just leaves blank space in its own cell rather than letting neighboring
// cells creep up to fill it (that's the explicitly-deferred future
// "dynamic grid", config.h's own comment). Header and body colors both come
// from `profile`, resolved for this specific `day_marked` state via
// agenda_day_header_colors()/agenda_day_body_colors() above - marking
// targets EITHER the header OR the whole body area (never both). The body
// fill is one continuous rect down to the cell's own bottom edge, including
// whatever's left over past the last drawn line, so an under-booked day
// never shows a plain gap inside an otherwise-colored cell.
static void draw_day_cell(uint8_t *rgb, int width, int height, agenda_rect_t cell, int day_index,
                          time_t day, const agenda_tagged_event_t *tagged, int tagged_count,
                          const bool *is_multiday, const int *first_visible_idx, bool skip_repeats,
                          bool show_prefix, const weather_forecast_t *cal_weather,
                          bool weather_mode, bool weather_right_aligned, bool german,
                          const agenda_color_profile_t *profile, bool day_marked)
{
    struct tm day_tm;
    localtime_r(&day, &day_tm);
    char label[16];
    snprintf(label, sizeof(label), "%s %d.", weather_weekday_abbr(day_tm.tm_wday, german),
             day_tm.tm_mday);

    char weather_buf[WEATHER_DAY_LINE_MAX_LEN] = "";
    const weather_day_t *wday;
    if (find_weather_for_day(cal_weather, day, &wday)) {
        int tmin = (int) lroundf(wday->temp_min_c);
        int tmax = (int) lroundf(wday->temp_max_c);
        const char *icon_set = config_manager_get_weather_icon_set();
        int icon_id =
            (strcmp(icon_set, "none") != 0) ? weather_code_to_icon_id(wday->weather_code) : -1;
        if (icon_id >= 0) {
            // Icon appended right after the closing bracket, not inside it
            // - the icon gets its own independent background box
            // (agenda_icon_colors()) rather than sharing the bracket text's
            // fill, so the two are drawn as separate chips by
            // draw_day_divider() (see its own comment on icon_gap).
            int prefix_len = snprintf(weather_buf, sizeof(weather_buf), "[%d/%d]", tmin, tmax);
            if (prefix_len > 0 && (size_t) prefix_len + 1 < sizeof(weather_buf)) {
                weather_buf[prefix_len] = (char) (WEATHER_ICON_MARKER_BASE + icon_id);
                weather_buf[prefix_len + 1] = '\0';
            }
        } else {
            const char *cond = weather_condition_text(wday->weather_code, german);
            snprintf(weather_buf, sizeof(weather_buf), "[%d/%d %s]", tmin, tmax, cond);
        }
    }

    uint8_t head_bg_r, head_bg_g, head_bg_b, head_fg_r, head_fg_g, head_fg_b;
    agenda_day_header_colors(profile, day_marked, &head_bg_r, &head_bg_g, &head_bg_b, &head_fg_r,
                             &head_fg_g, &head_fg_b);
    uint8_t icon_bg_r, icon_bg_g, icon_bg_b;
    agenda_icon_colors(profile, day_marked, head_bg_r, head_bg_g, head_bg_b, &icon_bg_r, &icon_bg_g,
                       &icon_bg_b);
    draw_day_divider(rgb, width, height, cell, cell.y, label, weather_mode, weather_right_aligned,
                     weather_buf[0] ? weather_buf : NULL, head_bg_r, head_bg_g, head_bg_b,
                     head_fg_r, head_fg_g, head_fg_b, profile->header_divider_filled, icon_bg_r,
                     icon_bg_g, icon_bg_b, profile->dayhead_leader_line);

    uint8_t body_bg_r, body_bg_g, body_bg_b, body_fg_r, body_fg_g, body_fg_b;
    agenda_day_body_colors(profile, day_marked, &body_bg_r, &body_bg_g, &body_bg_b, &body_fg_r,
                           &body_fg_g, &body_fg_b);
    bool body_marked = day_marked && profile->has_mark && !profile->mark_colors_header;

    int row_h = IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING;
    int text_width = cell.w - 2 * AGENDA_PADDING;
    int content_top = cell.y + IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING;
    int content_h = cell.h - IMAGE_PROCESSOR_FONT_HEIGHT - AGENDA_PADDING;
    // +AGENDA_PADDING before dividing: row_h bakes in one AGENDA_PADDING gap
    // *after* each row for separation from whatever follows, but the very
    // last row in the cell has nothing after it to separate from - counting
    // content_h/row_h straight would silently waste up to one whole
    // row_h-1 pixels at the bottom of every cell (confirmed live: a
    // visible blank gap above the next day's header even though its own
    // cell still had unused height).
    int max_rows = (content_h > 0) ? (content_h + AGENDA_PADDING) / row_h : 0;

    // The whole body area (everything below the header, down to this
    // cell's own bottom edge) gets one continuous fill up front, before any
    // text - including whatever's left over past the last drawn row, so
    // there's never a plain gap inside an otherwise-colored cell.
    if (content_h > 0) {
        image_processor_fill_rect(rgb, width, height, cell.x, content_top, cell.w, content_h,
                                  body_bg_r, body_bg_g, body_bg_b);
    }

    int total_instances = 0;
    for (int i = 0; i < tagged_count; i++) {
        if (!event_touches_day(tagged[i].ev, day)) {
            continue;
        }
        if (skip_repeats && is_multiday[i] && day_index != first_visible_idx[i]) {
            continue;
        }
        total_instances++;
    }
    int budget = (total_instances > max_rows) ? max_rows - 1 : max_rows;
    if (budget < 0) {
        budget = 0;
    }

    int rows_used = 0, instances_shown = 0;
    for (int i = 0; i < tagged_count && rows_used < budget; i++) {
        if (!event_touches_day(tagged[i].ev, day)) {
            continue;
        }
        if (skip_repeats && is_multiday[i] && day_index != first_visible_idx[i]) {
            continue;
        }
        const agenda_event_line_t *line = tagged[i].line;

        char prefix[16] = "";
        int prefix_len = 0;
        if (show_prefix && is_multiday[i]) {
            prefix_len =
                snprintf(prefix, sizeof(prefix), "%d/%d: ", event_day_index(tagged[i].ev, day),
                         event_total_days(tagged[i].ev));
        }
        int avail_width = text_width - prefix_len * IMAGE_PROCESSOR_FONT_WIDTH;
        if (avail_width < IMAGE_PROCESSOR_FONT_WIDTH) {
            avail_width = IMAGE_PROCESSOR_FONT_WIDTH;
        }

        char wrapped[1][OVERLAY_LINE_MAX_CHARS];
        int wrapped_count = image_processor_wrap_text(line->text, avail_width, 1, wrapped);
        if (wrapped_count > 0) {
            int y = content_top + rows_used * row_h;
            int x = cell.x + AGENDA_PADDING;
            if (prefix_len > 0) {
                image_processor_draw_text(rgb, width, height, x, y, prefix, body_fg_r, body_fg_g,
                                          body_fg_b);
                x += prefix_len * IMAGE_PROCESSOR_FONT_WIDTH;
            }
            int visible_len = (int) strlen(wrapped[0]);
            bool has_chip;
            uint8_t chip_r, chip_g, chip_b, event_r, event_g, event_b;
            agenda_event_colors(profile, line->calendar_index, body_marked, body_fg_r, body_fg_g,
                                body_fg_b, &has_chip, &chip_r, &chip_g, &chip_b, &event_r, &event_g,
                                &event_b);
            if (has_chip) {
                image_processor_fill_rect(rgb, width, height, x, y,
                                          visible_len * IMAGE_PROCESSOR_FONT_WIDTH,
                                          IMAGE_PROCESSOR_FONT_HEIGHT, chip_r, chip_g, chip_b);
            }
            image_processor_draw_text(rgb, width, height, x, y, wrapped[0], event_r, event_g,
                                      event_b);
            rows_used++;
            instances_shown++;
        }
    }

    if (instances_shown < total_instances && rows_used < max_rows) {
        char more[32];
        snprintf(more, sizeof(more), "+%d more", total_instances - instances_shown);
        int y = content_top + rows_used * row_h;
        image_processor_draw_text(rgb, width, height, cell.x + AGENDA_PADDING, y, more, body_fg_r,
                                  body_fg_g, body_fg_b);
    }
}

// Lays out the 7-day grid's fixed 4x2 cell skeleton (agenda_cal_layout_mode_t
// GRID_A/GRID_B, config.h) and draws each day into its cell via
// draw_day_cell() above. `template_b` selects which of the two today-gets-
// double-space arrangements to use - both give today exactly 2 of the 8
// cell-units, just split across width (GRID_A) or height (GRID_B); see
// config.h's enum comment and the two Layout_*.jpg concept images this was
// designed against. Every day-header lands on one of exactly 4 fixed row
// boundaries, shared by both columns, regardless of how much content any
// individual day has.
static void draw_calendar_grid(uint8_t *rgb, int width, int height, agenda_rect_t rect,
                               int content_top, int content_h, const time_t *days, int day_count,
                               const agenda_tagged_event_t *tagged, int tagged_count,
                               const weather_forecast_t *cal_weather,
                               const agenda_color_profile_t *profile, bool template_b)
{
    if (day_count <= 0) {
        return;
    }

    agenda_multiday_mode_t multiday_mode = config_manager_get_agenda_cal_multiday_mode();
    bool skip_repeats = (multiday_mode == AGENDA_MULTIDAY_COMPACT);
    bool show_prefix = (multiday_mode != AGENDA_MULTIDAY_REPEAT);

    // Same heap-not-stack reasoning as draw_calendar_column()'s own copy of
    // these two arrays - see its comment.
    bool *is_multiday =
        heap_caps_malloc(AGENDA_MAX_TAGGED_EVENTS * sizeof(bool), MALLOC_CAP_SPIRAM);
    int *first_visible_idx =
        heap_caps_malloc(AGENDA_MAX_TAGGED_EVENTS * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!is_multiday || !first_visible_idx) {
        ESP_LOGW(TAG, "Failed to allocate Calendar grid scratch buffers");
        heap_caps_free(is_multiday);
        heap_caps_free(first_visible_idx);
        return;
    }
    for (int k = 0; k < tagged_count; k++) {
        is_multiday[k] = event_total_days(tagged[k].ev) > 1;
        first_visible_idx[k] = -1;
        for (int di = 0; di < day_count; di++) {
            if (event_touches_day(tagged[k].ev, days[di])) {
                first_visible_idx[k] = di;
                break;
            }
        }
    }

    bool weather_mode = cal_weather && cal_weather->valid;
    bool weather_right_aligned = config_manager_get_agenda_cal_weather_right_aligned();
    bool german = (strcmp(config_manager_get_overlay_language(), "de") == 0);

    agenda_shift_model_t shift_model = config_manager_get_agenda_shift_model();
    const char *shift_start = config_manager_get_agenda_shift_start();

    int grid_row_h = content_h / 4;
    int grid_col_w = rect.w / 2;

    // Cell 0 is always "today". Cells 1-6 are the remaining days, laid out
    // column-major (matches Layout_A.jpg/Layout_B.jpg exactly) - GRID_A
    // fills the left column top-to-bottom then the right column; GRID_B's
    // left column already has "today" occupying its first 2 row-slots, so
    // only 2 more days fit there, and the right column takes the other 4.
    agenda_rect_t cells[7];
    if (!template_b) {
        cells[0] = (agenda_rect_t){rect.x, content_top, rect.w, grid_row_h};
        for (int i = 1; i <= 3 && i < day_count; i++) {
            cells[i] =
                (agenda_rect_t){rect.x, content_top + i * grid_row_h, grid_col_w, grid_row_h};
        }
        for (int i = 4; i <= 6 && i < day_count; i++) {
            cells[i] = (agenda_rect_t){rect.x + grid_col_w, content_top + (i - 3) * grid_row_h,
                                       grid_col_w, grid_row_h};
        }
    } else {
        cells[0] = (agenda_rect_t){rect.x, content_top, grid_col_w, grid_row_h * 2};
        for (int i = 1; i <= 2 && i < day_count; i++) {
            cells[i] =
                (agenda_rect_t){rect.x, content_top + (i + 1) * grid_row_h, grid_col_w, grid_row_h};
        }
        for (int i = 3; i <= 6 && i < day_count; i++) {
            cells[i] = (agenda_rect_t){rect.x + grid_col_w, content_top + (i - 3) * grid_row_h,
                                       grid_col_w, grid_row_h};
        }
    }

    for (int di = 0; di < day_count; di++) {
        // Group 1 of the alternating fortnightly cycle is "marked" - see
        // agenda_shift_group_for_day()'s own comment; group 0 (or no model
        // selected) just keeps the profile's plain, unmarked colors.
        bool day_marked = (shift_model != AGENDA_SHIFT_MODEL_NONE) &&
                          agenda_shift_group_for_day(shift_model, shift_start, days[di]) == 1;
        draw_day_cell(rgb, width, height, cells[di], di, days[di], tagged, tagged_count,
                      is_multiday, first_visible_idx, skip_repeats, show_prefix, cal_weather,
                      weather_mode, weather_right_aligned, german, profile, day_marked);
    }

    heap_caps_free(is_multiday);
    heap_caps_free(first_visible_idx);
}

static void draw_calendar_column(uint8_t *rgb, int width, int height, agenda_rect_t rect,
                                 time_t now, int lookahead_days,
                                 const agenda_color_profile_t *profile,
                                 const agenda_tagged_event_t *tagged, int tagged_count,
                                 const weather_forecast_t *cal_weather,
                                 agenda_cal_name_tag_t name_a, agenda_cal_name_tag_t name_b,
                                 bool show_c, bool show_d, bool show_e, bool calendar_only,
                                 const agenda_climate_t *climate)
{
    // The whole Calendar area gets the profile's plain page fill first -
    // the day cells/dividers below repaint their own areas on top of this
    // (including a full re-fill when marked), but this is what shows
    // through any padding/gap that never gets its own fill otherwise.
    uint8_t page_bg_r, page_bg_g, page_bg_b, page_fg_r, page_fg_g, page_fg_b;
    agenda_day_body_colors(profile, false, &page_bg_r, &page_bg_g, &page_bg_b, &page_fg_r,
                           &page_fg_g, &page_fg_b);
    image_processor_fill_rect(rgb, width, height, rect.x, rect.y, rect.w, rect.h, page_bg_r,
                              page_bg_g, page_bg_b);

    uint8_t header_bg_r, header_bg_g, header_bg_b, header_text_r, header_text_g, header_text_b;
    agenda_top_header_colors(profile, &header_bg_r, &header_bg_g, &header_bg_b, &header_text_r,
                             &header_text_g, &header_text_b);

    int header_h = IMAGE_PROCESSOR_FONT_HEIGHT + 2 * AGENDA_PADDING;
    image_processor_fill_rect(rgb, width, height, rect.x, rect.y, rect.w, header_h, header_bg_r,
                              header_bg_g, header_bg_b);

    // Each shown calendar's name/letter is drawn directly in that source's
    // own color (draw_calendar_label() - see its own comment for why no
    // separate swatch/badge box is used) - all five sources show whenever
    // they contributed events this cycle; a mono profile just draws every
    // one of them in the same plain header ink, since there's no color to
    // legend in the first place there.
    int hx = rect.x + AGENDA_PADDING;
    int hy = rect.y + AGENDA_PADDING;
    // A "," (no surrounding space) precedes every shown source after the
    // first, whatever kind it is (a named A/B or a lettered C/D/E) -
    // `any_shown` tracks that uniformly instead of an A-then-B-only
    // special case, which would leave later sources bunched together with
    // no separator at all.
    bool any_shown = false;
    if (name_a.show) {
        hx = draw_calendar_label(rgb, width, height, hx, hy, name_a.name, profile, 0, header_text_r,
                                 header_text_g, header_text_b);
        any_shown = true;
    }
    if (name_b.show) {
        if (any_shown) {
            image_processor_draw_text(rgb, width, height, hx, hy, ",", header_text_r, header_text_g,
                                      header_text_b);
            hx += IMAGE_PROCESSOR_FONT_WIDTH;
        }
        hx = draw_calendar_label(rgb, width, height, hx, hy, name_b.name, profile, 1, header_text_r,
                                 header_text_g, header_text_b);
        any_shown = true;
    }
    if (show_c) {
        if (any_shown) {
            image_processor_draw_text(rgb, width, height, hx, hy, ",", header_text_r, header_text_g,
                                      header_text_b);
            hx += IMAGE_PROCESSOR_FONT_WIDTH;
        }
        hx = draw_calendar_label(rgb, width, height, hx, hy, "C", profile, 2, header_text_r,
                                 header_text_g, header_text_b);
        any_shown = true;
    }
    if (show_d) {
        if (any_shown) {
            image_processor_draw_text(rgb, width, height, hx, hy, ",", header_text_r, header_text_g,
                                      header_text_b);
            hx += IMAGE_PROCESSOR_FONT_WIDTH;
        }
        hx = draw_calendar_label(rgb, width, height, hx, hy, "D", profile, 3, header_text_r,
                                 header_text_g, header_text_b);
        any_shown = true;
    }
    if (show_e) {
        if (any_shown) {
            image_processor_draw_text(rgb, width, height, hx, hy, ",", header_text_r, header_text_g,
                                      header_text_b);
            hx += IMAGE_PROCESSOR_FONT_WIDTH;
        }
        hx = draw_calendar_label(rgb, width, height, hx, hy, "E", profile, 4, header_text_r,
                                 header_text_g, header_text_b);
        any_shown = true;
    }

    struct tm now_tm;
    localtime_r(&now, &now_tm);
    // No dash, 2-digit year (tm_year % 100 is always 0-99) - and now
    // right-aligned, immediately left of the climate readout (if shown in
    // this header - see agenda_renderer_render()'s de-duplication comment)
    // instead of directly after the last shown calendar source, so it
    // stays in the same visual spot regardless of how many sources/how
    // long a custom name happens to be.
    char datetime[32];  // oversized - GCC's -Wformat-truncation can't prove
                        // a %02d field never exceeds 2 digits
    snprintf(datetime, sizeof(datetime), "%02d.%02d.%02d %02d:%02d", now_tm.tm_mday,
             now_tm.tm_mon + 1, now_tm.tm_year % 100, now_tm.tm_hour, now_tm.tm_min);
    int climate_edge = draw_header_climate(rgb, width, height, rect, climate);
    int datetime_w = (int) strlen(datetime) * IMAGE_PROCESSOR_FONT_WIDTH;
    int datetime_x = climate_edge - datetime_w;
    // Omit the timestamp entirely rather than overlapping the calendar
    // names/badges it would otherwise garble into - confirmed live in the
    // side-by-side dual layout's half-width Calendar column, where several
    // sources' names plus a "," between each can already reach past the
    // midpoint on their own. Matches this project's existing "clip/omit
    // rather than corrupt" convention for an over-narrow header elsewhere
    // (see draw_day_divider()'s own label clipping).
    if (datetime_x >= hx + AGENDA_PADDING) {
        image_processor_draw_text(rgb, width, height, datetime_x, hy, datetime, header_text_r,
                                  header_text_g, header_text_b);
    }

    int row_h = IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING;
    int content_top = rect.y + header_h + AGENDA_PADDING;
    int content_h = rect.h - header_h - AGENDA_PADDING;
    // +AGENDA_PADDING: row_h's trailing gap isn't needed after the very
    // last row - see draw_day_cell()'s identical fix for the full story.
    int max_rows = (content_h > 0) ? (content_h + AGENDA_PADDING) / row_h : 0;
    int text_width = rect.w - 2 * AGENDA_PADDING;

    // The 7-day grid layouts only take effect Calendar-only-fullscreen
    // (calendar_only, set by the caller from !both) - falls back to plain
    // list rendering otherwise (a half-width 7-day grid would be
    // unreadable), silently, rather than the Web UI's chosen layout
    // simply not working with no explanation.
    agenda_cal_layout_mode_t layout_mode = config_manager_get_agenda_cal_layout_mode();
    bool use_grid = calendar_only && layout_mode != AGENDA_CAL_LAYOUT_LIST;
    if (!use_grid) {
        // List mode always respects its own 1-3 day setting, regardless of
        // how many days agenda_manager.c actually fetched this cycle (it
        // fetches a full 7 whenever a grid layout is selected, even if
        // calendar_only then turns out false - see its own comment).
        int list_days = config_manager_get_agenda_cal_days();
        if (lookahead_days > list_days) {
            lookahead_days = list_days;
        }
    }

    time_t win_day_start = day_start(now);
    time_t win_day_end = day_start(now + (time_t) lookahead_days * 86400 - 1);

    // Every day in the visible window gets a divider row, even one with no
    // events at all - a day used to be dropped entirely when it had no
    // matching event (saved a row), but that silently hid that day's
    // weather annotation too (confirmed live: today's row, and its
    // forecast, vanished whenever today happened to have no events) -
    // surprising given the header right above already names today's date.
    time_t days[AGENDA_MAX_CAL_DAYS];
    int day_count = 0;
    for (time_t d = win_day_start; d <= win_day_end && day_count < AGENDA_MAX_CAL_DAYS;
         d += 86400) {
        days[day_count++] = d;
    }

    if (use_grid) {
        if (day_count > 7) {
            // AGENDA_MAX_CAL_DAYS=8's documented spillover day (see its own
            // comment above) - the grid is always exactly 7 cells.
            day_count = 7;
        }
        draw_calendar_grid(rgb, width, height, rect, content_top, content_h, days, day_count,
                           tagged, tagged_count, cal_weather, profile,
                           layout_mode == AGENDA_CAL_LAYOUT_GRID_B);
        return;
    }

    // Multi-day event display mode - see agenda_multiday_mode_t (config.h).
    // COMPACT shows the event only once, on the first day of the *visible*
    // window it touches; REPEAT_NUMBERED keeps repeating it under every day
    // like plain REPEAT but adds the same "N/M: " prefix COMPACT uses (N =
    // position within the event's own full span, M = that span's total
    // length). Precomputed once per tagged event rather than re-derived per
    // day, since both the budgeting pass and the render pass below need the
    // same answer.
    agenda_multiday_mode_t multiday_mode = config_manager_get_agenda_cal_multiday_mode();
    bool skip_repeats = (multiday_mode == AGENDA_MULTIDAY_COMPACT);
    bool show_prefix = (multiday_mode != AGENDA_MULTIDAY_REPEAT);
    // Heap, not stack locals - AGENDA_MAX_TAGGED_EVENTS scales with
    // ICS_MAX_EVENTS (48 per source x 5 sources = 240 today), and this
    // project has a documented history of stack-overflow bugs from
    // exactly this class of "array sized by a generous capacity constant"
    // local (see agenda_manager.c's own todo/events buffers, heap-
    // allocated for the same reason).
    bool *is_multiday =
        heap_caps_malloc(AGENDA_MAX_TAGGED_EVENTS * sizeof(bool), MALLOC_CAP_SPIRAM);
    int *first_visible_idx =
        heap_caps_malloc(AGENDA_MAX_TAGGED_EVENTS * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!is_multiday || !first_visible_idx) {
        ESP_LOGW(TAG, "Failed to allocate Calendar column scratch buffers");
        heap_caps_free(is_multiday);
        heap_caps_free(first_visible_idx);
        return;
    }
    for (int k = 0; k < tagged_count; k++) {
        is_multiday[k] = event_total_days(tagged[k].ev) > 1;
        first_visible_idx[k] = -1;
        for (int di = 0; di < day_count; di++) {
            if (event_touches_day(tagged[k].ev, days[di])) {
                first_visible_idx[k] = di;
                break;
            }
        }
    }

    // Two-pass budgeting, same "reserve a row for +N more" idea as
    // draw_todo_column() - here a single event can occupy more than one
    // row total (once per day it touches, unless compacted above), so the
    // total has to be counted up front rather than just compared against
    // tagged_count.
    int total_event_instances = 0;
    for (int di = 0; di < day_count; di++) {
        for (int k = 0; k < tagged_count; k++) {
            if (!event_touches_day(tagged[k].ev, days[di])) {
                continue;
            }
            if (skip_repeats && is_multiday[k] && di != first_visible_idx[k]) {
                continue;
            }
            total_event_instances++;
        }
    }
    int total_rows_needed = day_count + total_event_instances;
    int budget = (total_rows_needed > max_rows) ? max_rows - 1 : max_rows;
    if (budget < 0) {
        budget = 0;
    }

    // Whether the weather annotation feature is on and actually returned
    // data this cycle - decides the day divider's overall layout (see
    // draw_day_divider()'s comment), independent of whether any specific
    // day within the window happens to have its own forecast entry.
    bool weather_mode = cal_weather && cal_weather->valid;
    bool weather_right_aligned = config_manager_get_agenda_cal_weather_right_aligned();

    // Same overlay_language setting the weather annotation below already
    // used - the weekday abbreviation is generated by this firmware (it
    // has nothing to do with the ICS file, which only ever carries dates,
    // never a localized weekday name), so it needs the same language check
    // to actually follow the user's choice instead of staying German-only
    // regardless of it.
    bool german = (strcmp(config_manager_get_overlay_language(), "de") == 0);

    // List mode has no per-day cell to fill, so BODY marking (the whole-cell
    // fill grid mode does) has no analog here and stays off regardless of
    // the profile - but HEADER marking (markColorsHeader) is just a color
    // swap on an already-existing element, so it's resolved per day exactly
    // like grid mode does, fixing an earlier bug where a header-marking
    // profile's mark color only ever showed up in the 7-day grid (Calendar
    // shown alone) and silently reverted to plain header colors the moment
    // ToDo was shown alongside it (dual/stacked or side-by-side), even
    // though both views share the same active profile.
    agenda_shift_model_t list_shift_model = config_manager_get_agenda_shift_model();
    const char *list_shift_start = config_manager_get_agenda_shift_start();

    int rows_used = 0;
    int instances_shown = 0;
    for (int di = 0; di < day_count && rows_used < budget; di++) {
        struct tm day_tm;
        localtime_r(&days[di], &day_tm);
        char label[16];
        snprintf(label, sizeof(label), "%s %d.", weather_weekday_abbr(day_tm.tm_wday, german),
                 day_tm.tm_mday);
        bool day_marked =
            (list_shift_model != AGENDA_SHIFT_MODEL_NONE) &&
            agenda_shift_group_for_day(list_shift_model, list_shift_start, days[di]) == 1;
        uint8_t head_bg_r, head_bg_g, head_bg_b, head_fg_r, head_fg_g, head_fg_b;
        agenda_day_header_colors(profile, day_marked, &head_bg_r, &head_bg_g, &head_bg_b,
                                 &head_fg_r, &head_fg_g, &head_fg_b);
        char weather_buf[WEATHER_DAY_LINE_MAX_LEN] = "";
        const weather_day_t *wday;
        if (find_weather_for_day(cal_weather, days[di], &wday)) {
            int tmin = (int) lroundf(wday->temp_min_c);
            int tmax = (int) lroundf(wday->temp_max_c);
            const char *icon_set = config_manager_get_weather_icon_set();
            int icon_id =
                (strcmp(icon_set, "none") != 0) ? weather_code_to_icon_id(wday->weather_code) : -1;
            if (icon_id >= 0) {
                // Icon appended right after the closing bracket, not inside
                // it - see draw_day_cell()'s identical comment for why.
                int prefix_len = snprintf(weather_buf, sizeof(weather_buf), "[%d/%d]", tmin, tmax);
                if (prefix_len > 0 && (size_t) prefix_len + 1 < sizeof(weather_buf)) {
                    weather_buf[prefix_len] = (char) (WEATHER_ICON_MARKER_BASE + icon_id);
                    weather_buf[prefix_len + 1] = '\0';
                }
            } else {
                const char *cond = weather_condition_text(wday->weather_code, german);
                snprintf(weather_buf, sizeof(weather_buf), "[%d/%d %s]", tmin, tmax, cond);
            }
        }
        uint8_t icon_bg_r, icon_bg_g, icon_bg_b;
        agenda_icon_colors(profile, day_marked, head_bg_r, head_bg_g, head_bg_b, &icon_bg_r,
                           &icon_bg_g, &icon_bg_b);
        draw_day_divider(rgb, width, height, rect, content_top + rows_used * row_h, label,
                         weather_mode, weather_right_aligned, weather_buf[0] ? weather_buf : NULL,
                         head_bg_r, head_bg_g, head_bg_b, head_fg_r, head_fg_g, head_fg_b,
                         profile->header_divider_filled, icon_bg_r, icon_bg_g, icon_bg_b,
                         profile->dayhead_leader_line);
        rows_used++;

        for (int i = 0; i < tagged_count && rows_used < budget; i++) {
            if (!event_touches_day(tagged[i].ev, days[di])) {
                continue;
            }
            if (skip_repeats && is_multiday[i] && di != first_visible_idx[i]) {
                continue;
            }
            const agenda_event_line_t *line = tagged[i].line;

            char prefix[16] = "";
            int prefix_len = 0;
            if (show_prefix && is_multiday[i]) {
                prefix_len = snprintf(prefix, sizeof(prefix),
                                      "%d/%d: ", event_day_index(tagged[i].ev, days[di]),
                                      event_total_days(tagged[i].ev));
            }
            int avail_width = text_width - prefix_len * IMAGE_PROCESSOR_FONT_WIDTH;
            if (avail_width < IMAGE_PROCESSOR_FONT_WIDTH) {
                avail_width = IMAGE_PROCESSOR_FONT_WIDTH;
            }

            char wrapped[1][OVERLAY_LINE_MAX_CHARS];
            int wrapped_count = image_processor_wrap_text(line->text, avail_width, 1, wrapped);
            if (wrapped_count > 0) {
                int y = content_top + rows_used * row_h;
                int x = rect.x + AGENDA_PADDING;
                if (prefix_len > 0) {
                    image_processor_draw_text(rgb, width, height, x, y, prefix, page_fg_r,
                                              page_fg_g, page_fg_b);
                    x += prefix_len * IMAGE_PROCESSOR_FONT_WIDTH;
                }
                int visible_len = (int) strlen(wrapped[0]);
                bool has_chip;
                uint8_t chip_r, chip_g, chip_b, event_r, event_g, event_b;
                agenda_event_colors(profile, line->calendar_index, false, page_fg_r, page_fg_g,
                                    page_fg_b, &has_chip, &chip_r, &chip_g, &chip_b, &event_r,
                                    &event_g, &event_b);
                if (has_chip) {
                    image_processor_fill_rect(rgb, width, height, x, y,
                                              visible_len * IMAGE_PROCESSOR_FONT_WIDTH,
                                              IMAGE_PROCESSOR_FONT_HEIGHT, chip_r, chip_g, chip_b);
                }
                image_processor_draw_text(rgb, width, height, x, y, wrapped[0], event_r, event_g,
                                          event_b);
                rows_used++;
                instances_shown++;
            }
        }
    }

    if (instances_shown < total_event_instances && rows_used < max_rows) {
        char more[32];
        snprintf(more, sizeof(more), "+%d more", total_event_instances - instances_shown);
        int y = content_top + rows_used * row_h;
        image_processor_draw_text(rgb, width, height, rect.x + AGENDA_PADDING, y, more, page_fg_r,
                                  page_fg_g, page_fg_b);
    }

    heap_caps_free(is_multiday);
    heap_caps_free(first_visible_idx);
}

// Same header/row-budget/"+N more" behavior as draw_column() above, but for
// ToDo rows: each row's colors come from the per-element runs
// build_todo_line() already computed, clipped to whatever
// image_processor_wrap_text() actually kept if the row had to be truncated
// (a run starting past the truncation cutoff is simply never reached; one
// straddling the cutoff is shortened to match).
static void draw_todo_column(uint8_t *rgb, int width, int height, agenda_rect_t rect, time_t now,
                             uint8_t body_r, uint8_t body_g, uint8_t body_b,
                             const agenda_line_t *todo_lines, int line_count,
                             const agenda_climate_t *climate)
{
    uint8_t header_text_r, header_text_g, header_text_b;
    agenda_safe_text_color(body_r, body_g, body_b, &header_text_r, &header_text_g, &header_text_b);

    struct tm now_tm;
    localtime_r(&now, &now_tm);
    // No dash, 2-digit year (tm_year % 100 is always 0-99) - matches the
    // Calendar column's own top header exactly. Right-aligned, immediately
    // left of the climate readout (if shown in this header - see
    // agenda_renderer_render()'s de-duplication comment) rather than
    // directly after "TODO", so it lands in the same visual spot
    // (top-right, just left of any sensor chips) regardless of how long the
    // rest of the header's own content is.
    char timestamp[32];  // oversized - GCC's -Wformat-truncation can't prove
                         // a %02d field never exceeds 2 digits
    snprintf(timestamp, sizeof(timestamp), "%02d.%02d.%02d %02d:%02d", now_tm.tm_mday,
             now_tm.tm_mon + 1, now_tm.tm_year % 100, now_tm.tm_hour, now_tm.tm_min);

    int header_h = IMAGE_PROCESSOR_FONT_HEIGHT + 2 * AGENDA_PADDING;
    image_processor_fill_rect(rgb, width, height, rect.x, rect.y, rect.w, header_h, body_r, body_g,
                              body_b);
    image_processor_draw_text(rgb, width, height, rect.x + AGENDA_PADDING, rect.y + AGENDA_PADDING,
                              "TODO", header_text_r, header_text_g, header_text_b);
    int todo_label_end = rect.x + AGENDA_PADDING + 4 * IMAGE_PROCESSOR_FONT_WIDTH;  // "TODO"
    int climate_edge = draw_header_climate(rgb, width, height, rect, climate);
    int timestamp_w = (int) strlen(timestamp) * IMAGE_PROCESSOR_FONT_WIDTH;
    int timestamp_x = climate_edge - timestamp_w;
    // Omit rather than overlap "TODO" - see draw_calendar_column()'s
    // identical guard for why (narrow side-by-side columns are the
    // realistic trigger for the Calendar side; kept here too for the same
    // reason, since ToDo's column can be just as narrow side-by-side).
    if (timestamp_x >= todo_label_end + AGENDA_PADDING) {
        image_processor_draw_text(rgb, width, height, timestamp_x, rect.y + AGENDA_PADDING,
                                  timestamp, header_text_r, header_text_g, header_text_b);
    }

    int row_h = IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING;
    int content_top = rect.y + header_h + AGENDA_PADDING;
    int content_h = rect.h - header_h - AGENDA_PADDING;
    // +AGENDA_PADDING: row_h's trailing gap isn't needed after the very
    // last row - see draw_day_cell()'s identical fix for the full story.
    int max_rows = (content_h > 0) ? (content_h + AGENDA_PADDING) / row_h : 0;

    int rows_drawn = 0;
    int text_width = rect.w - 2 * AGENDA_PADDING;
    int budget = (line_count > max_rows) ? max_rows - 1 : max_rows;
    if (budget < 0) {
        budget = 0;
    }
    for (int i = 0; i < line_count && rows_drawn < budget; i++) {
        const agenda_line_t *line = &todo_lines[i];
        char wrapped[1][OVERLAY_LINE_MAX_CHARS];
        int wrapped_count = image_processor_wrap_text(line->text, text_width, 1, wrapped);
        if (wrapped_count <= 0) {
            continue;
        }
        int visible_len = (int) strlen(wrapped[0]);
        int y = content_top + rows_drawn * row_h;
        int x = rect.x + AGENDA_PADDING;

        // Backgrounds first (clipped to what's actually visible), then
        // glyphs on top - same layering as the header bar above.
        for (int r = 0; r < line->bg_run_count; r++) {
            const image_processor_text_run_t *run = &line->bg_runs[r];
            if (run->start >= visible_len) {
                continue;
            }
            int len = run->length;
            if (run->start + len > visible_len) {
                len = visible_len - run->start;
            }
            image_processor_fill_rect(rgb, width, height,
                                      x + run->start * IMAGE_PROCESSOR_FONT_WIDTH, y,
                                      len * IMAGE_PROCESSOR_FONT_WIDTH, IMAGE_PROCESSOR_FONT_HEIGHT,
                                      run->r, run->g, run->b);
        }
        image_processor_draw_text_runs(rgb, width, height, x, y, wrapped[0], line->fg_runs,
                                       line->fg_run_count, body_r, body_g, body_b);
        rows_drawn++;
    }
    if (line_count > rows_drawn && rows_drawn < max_rows) {
        char more[32];
        snprintf(more, sizeof(more), "+%d more", line_count - rows_drawn);
        int y = content_top + rows_drawn * row_h;
        image_processor_draw_text(rgb, width, height, rect.x + AGENDA_PADDING, y, more, body_r,
                                  body_g, body_b);
    }
}

esp_err_t agenda_renderer_render(const todo_list_t *todo, const ics_event_list_t *events_a,
                                 const ics_event_list_t *events_b, const ics_event_list_t *events_c,
                                 const ics_event_list_t *events_d, const ics_event_list_t *events_e,
                                 const weather_forecast_t *cal_weather, int lookahead_days,
                                 const char *output_path, image_format_t out_format,
                                 const agenda_climate_t *climate)
{
    if (!output_path) {
        return ESP_ERR_INVALID_ARG;
    }
    bool show_todo = todo && todo->count > 0;
    bool have_a = events_a && events_a->count > 0;
    bool have_b = events_b && events_b->count > 0;
    bool have_c = events_c && events_c->count > 0;
    bool have_d = events_d && events_d->count > 0;
    bool have_e = events_e && events_e->count > 0;
    bool show_cal = have_a || have_b || have_c || have_d || have_e;
    if (!show_todo && !show_cal) {
        return ESP_ERR_INVALID_STATE;
    }

    int width = BOARD_HAL_DISPLAY_WIDTH;
    int height = BOARD_HAL_DISPLAY_HEIGHT;
    size_t buf_size = (size_t) width * (size_t) height * 3;
    uint8_t *rgb = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "Failed to allocate %zu-byte agenda canvas", buf_size);
        return ESP_ERR_NO_MEM;
    }

    bool grayscale = agenda_board_is_grayscale();
    uint8_t bg_r, bg_g, bg_b;
    agenda_background_color(&bg_r, &bg_g, &bg_b);
    image_processor_fill_rect(rgb, width, height, 0, 0, width, height, bg_r, bg_g, bg_b);

    // The one color every ToDo "plain, no own fill" text element falls back
    // to (body text, "+N more", any role whose usual color happens to
    // collide with the fixed page background) - computed once here rather
    // than separately per role, since it only depends on that background.
    // The Calendar column no longer shares this: its own colors come from
    // the active color profile instead (see agenda_color_profile.h).
    uint8_t body_r = 0, body_g = 0, body_b = 0;
    agenda_avoid_bg_collision(bg_r, bg_g, bg_b, &body_r, &body_g, &body_b);

    agenda_color_profile_t profile;
    agenda_color_profile_load_active(&profile);
    if (grayscale) {
        agenda_color_profile_degrade_for_grayscale(&profile);
    }
    // A mono/mono-invert profile must keep weather icons plain black/white
    // too, even on a full-color panel where the device-wide "colored icons"
    // setting would otherwise still apply - see image_processor_set_mono_icon_mode().
    image_processor_set_mono_icon_mode(profile.mono);

    // Landscape only - portrait always stacks (a side-by-side split would
    // make each column too narrow to be useful there), matching
    // agenda_wants_portrait_frame()'s existing hardware-driven behavior.
    // Landscape defaults to stacked too (config_manager's
    // AGENDA_STACK_DEFAULT) with side-by-side as the user-selectable
    // alternative - stacked was chosen as the default because a half-width
    // column was found to hide too much content in practice.
    bool stack = agenda_wants_portrait_frame() || config_manager_get_agenda_stack_layout();

    bool both = show_todo && show_cal;
    agenda_rect_t todo_rect, cal_rect;
    if (!both) {
        agenda_rect_t full = {0, 0, width, height};
        todo_rect = full;
        cal_rect = full;
    } else if (stack) {
        todo_rect = (agenda_rect_t){0, 0, width, height / 2};
        cal_rect = (agenda_rect_t){0, height / 2, width, height - height / 2};
        image_processor_fill_rect(rgb, width, height, 0, height / 2 - 1, width, 2, body_r, body_g,
                                  body_b);
    } else {
        todo_rect = (agenda_rect_t){0, 0, width / 2, height};
        cal_rect = (agenda_rect_t){width / 2, 0, width - width / 2, height};
        image_processor_fill_rect(rgb, width, height, width / 2 - 1, 0, 2, height, body_r, body_g,
                                  body_b);
    }

    time_t now = time(NULL);

    // De-duplicate the climate readout when both columns are shown -
    // otherwise it would render identically in both headers (confirmed
    // live: "Sa 19."-style header duplication of the exact same chips).
    // Single column: unaffected either way. Both + stacked: only ToDo (the
    // fixed top half) shows it. Both + side-by-side: only Calendar (the
    // fixed right column) shows it instead, so the readout still ends up in
    // the screen's actual top-right corner either way.
    const agenda_climate_t *todo_climate = climate;
    const agenda_climate_t *cal_climate = climate;
    if (both) {
        if (stack) {
            cal_climate = NULL;
        } else {
            todo_climate = NULL;
        }
    }

    if (show_todo) {
        agenda_line_t *todo_lines =
            heap_caps_malloc((size_t) todo->count * sizeof(agenda_line_t), MALLOC_CAP_SPIRAM);
        if (todo_lines) {
            for (int i = 0; i < todo->count; i++) {
                build_todo_line(&todo->items[i], now, grayscale, bg_r, bg_g, bg_b, &todo_lines[i]);
            }
            draw_todo_column(rgb, width, height, todo_rect, now, body_r, body_g, body_b, todo_lines,
                             todo->count, todo_climate);
        } else {
            ESP_LOGW(TAG, "Failed to allocate ToDo render scratch buffers - skipping ToDo column");
        }
        heap_caps_free(todo_lines);
    }

    if (show_cal) {
        int count_a = have_a ? events_a->count : 0;
        int count_b = have_b ? events_b->count : 0;
        int count_c = have_c ? events_c->count : 0;
        int count_d = have_d ? events_d->count : 0;
        int count_e = have_e ? events_e->count : 0;

        agenda_event_line_t *lines_a = heap_caps_malloc(
            (size_t) (count_a > 0 ? count_a : 1) * sizeof(agenda_event_line_t), MALLOC_CAP_SPIRAM);
        agenda_event_line_t *lines_b = heap_caps_malloc(
            (size_t) (count_b > 0 ? count_b : 1) * sizeof(agenda_event_line_t), MALLOC_CAP_SPIRAM);
        agenda_event_line_t *lines_c = heap_caps_malloc(
            (size_t) (count_c > 0 ? count_c : 1) * sizeof(agenda_event_line_t), MALLOC_CAP_SPIRAM);
        agenda_event_line_t *lines_d = heap_caps_malloc(
            (size_t) (count_d > 0 ? count_d : 1) * sizeof(agenda_event_line_t), MALLOC_CAP_SPIRAM);
        agenda_event_line_t *lines_e = heap_caps_malloc(
            (size_t) (count_e > 0 ? count_e : 1) * sizeof(agenda_event_line_t), MALLOC_CAP_SPIRAM);
        agenda_tagged_event_t *tagged = heap_caps_malloc(
            AGENDA_MAX_TAGGED_EVENTS * sizeof(agenda_tagged_event_t), MALLOC_CAP_SPIRAM);

        if (lines_a && lines_b && lines_c && lines_d && lines_e && tagged) {
            int tagged_count = 0;
            for (int i = 0; i < count_a && tagged_count < AGENDA_MAX_TAGGED_EVENTS; i++) {
                build_event_line(&events_a->events[i], 0, &lines_a[i]);
                tagged[tagged_count].ev = &events_a->events[i];
                tagged[tagged_count].line = &lines_a[i];
                tagged_count++;
            }
            for (int i = 0; i < count_b && tagged_count < AGENDA_MAX_TAGGED_EVENTS; i++) {
                build_event_line(&events_b->events[i], 1, &lines_b[i]);
                tagged[tagged_count].ev = &events_b->events[i];
                tagged[tagged_count].line = &lines_b[i];
                tagged_count++;
            }
            // C/D/E: same tagging shape as A/B, just a different
            // calendar_index (2/3/4) so the color profile's cal_ink[]/
            // cal_bg[] picks each one's own entry - see
            // NVS_AGENDA_CAL_C_URL_KEY etc. in config.h for why these three
            // never auto-refresh.
            for (int i = 0; i < count_c && tagged_count < AGENDA_MAX_TAGGED_EVENTS; i++) {
                build_event_line(&events_c->events[i], 2, &lines_c[i]);
                tagged[tagged_count].ev = &events_c->events[i];
                tagged[tagged_count].line = &lines_c[i];
                tagged_count++;
            }
            for (int i = 0; i < count_d && tagged_count < AGENDA_MAX_TAGGED_EVENTS; i++) {
                build_event_line(&events_d->events[i], 3, &lines_d[i]);
                tagged[tagged_count].ev = &events_d->events[i];
                tagged[tagged_count].line = &lines_d[i];
                tagged_count++;
            }
            for (int i = 0; i < count_e && tagged_count < AGENDA_MAX_TAGGED_EVENTS; i++) {
                build_event_line(&events_e->events[i], 4, &lines_e[i]);
                tagged[tagged_count].ev = &events_e->events[i];
                tagged[tagged_count].line = &lines_e[i];
                tagged_count++;
            }
            if (tagged_count > 1) {
                qsort(tagged, (size_t) tagged_count, sizeof(agenda_tagged_event_t),
                      compare_tagged_by_start);
            }
            // Header shows the actual calendar source name(s) instead of a
            // generic "CALENDAR" label - just whichever one(s) actually
            // contributed events this cycle (have_a/have_b), not simply
            // whichever have a URL configured, since a configured-but-
            // currently-failed source contributes nothing to show a name
            // for. Falls back to "Calendar A"/"Calendar B" if the user
            // hasn't set a custom display name for that source.
            const char *name_a = config_manager_get_agenda_cal_name();
            if (!name_a || name_a[0] == '\0') {
                name_a = "Calendar A";
            }
            const char *name_b = config_manager_get_agenda_cal_name2();
            if (!name_b || name_b[0] == '\0') {
                name_b = "Calendar B";
            }
            agenda_cal_name_tag_t tag_a = {.show = have_a, .name = name_a};
            agenda_cal_name_tag_t tag_b = {.show = have_b, .name = name_b};
            draw_calendar_column(rgb, width, height, cal_rect, now, lookahead_days, &profile,
                                 tagged, tagged_count, cal_weather, tag_a, tag_b, have_c, have_d,
                                 have_e, !both, cal_climate);
        } else {
            ESP_LOGW(TAG, "Failed to allocate Calendar render scratch buffers - skipping column");
        }
        heap_caps_free(lines_a);
        heap_caps_free(lines_b);
        heap_caps_free(lines_c);
        heap_caps_free(lines_d);
        heap_caps_free(lines_e);
        heap_caps_free(tagged);
    }

    image_processor_set_mono_icon_mode(false);  // don't leak into an unrelated later render

    image_format_t actual_format = out_format;
    esp_err_t err = image_processor_write_rgb_to_fmt(rgb, width, height, output_path, out_format,
                                                     &actual_format);
    heap_caps_free(rgb);
    return err;
}
