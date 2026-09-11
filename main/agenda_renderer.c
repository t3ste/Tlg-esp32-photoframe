#include "agenda_renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "config_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

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

// Resolves the shared ToDo+Calendar background setting (config_manager's
// agenda_bg_color, a plain name like "black" or "yellow") to actual RGB for
// the current hardware profile. An unrecognized name, or one that only
// applies to the other profile (e.g. "yellow" stored while running on a
// grayscale board - possible if a config was copied between boards), falls
// back to white rather than erroring, matching this project's fail-soft
// style for stored settings that don't quite fit the running hardware.
static void agenda_background_color(bool grayscale, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const char *name = config_manager_get_agenda_bg_color();
    if (grayscale) {
        if (strcmp(name, "black") == 0) {
            *r = *g = *b = 0;
        } else if (strcmp(name, "gray25") == 0) {
            *r = *g = *b = 64;
        } else if (strcmp(name, "gray50") == 0) {
            *r = *g = *b = 128;
        } else if (strcmp(name, "gray75") == 0) {
            *r = *g = *b = 191;
        } else {
            *r = *g = *b = 255;  // "white" or anything unrecognized
        }
        return;
    }
    if (strcmp(name, "black") == 0) {
        *r = 0;
        *g = 0;
        *b = 0;
    } else if (strcmp(name, "yellow") == 0) {
        *r = 255;
        *g = 255;
        *b = 0;
    } else if (strcmp(name, "red") == 0) {
        *r = 255;
        *g = 0;
        *b = 0;
    } else if (strcmp(name, "blue") == 0) {
        *r = 0;
        *g = 0;
        *b = 255;
    } else if (strcmp(name, "green") == 0) {
        *r = 0;
        *g = 255;
        *b = 0;
    } else {
        *r = *g = *b = 255;  // "white" or anything unrecognized
    }
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
// *that* fill regardless of the page background. The day divider and both
// column headers are a different case: they always draw a fill, but which
// polarity (dark-on-light vs. light-on-dark) they use is deliberately
// derived from the page background too - see draw_day_divider() and the
// header-drawing code in draw_todo_column()/draw_calendar_column().
static void agenda_avoid_bg_collision(uint8_t bg_r, uint8_t bg_g, uint8_t bg_b, uint8_t *fr,
                                      uint8_t *fg, uint8_t *fb)
{
    if (agenda_colors_equal(*fr, *fg, *fb, bg_r, bg_g, bg_b)) {
        agenda_safe_text_color(bg_r, bg_g, bg_b, fr, fg, fb);
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

static void add_run(image_processor_text_run_t *runs, int *count, int start, int length,
                    uint8_t r, uint8_t g, uint8_t b)
{
    if (*count >= AGENDA_MAX_RUNS_PER_LINE || length <= 0) {
        return;
    }
    runs[*count] = (image_processor_text_run_t) {start, length, r, g, b};
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
            priority_color(item->priority, grayscale, bg_r, bg_g, bg_b, &fr, &fg, &fb, &has_bg,
                           &br, &bg, &bb);
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
            due_color(overdue, today, grayscale, bg_r, bg_g, bg_b, &fr, &fg, &fb, &has_bg, &br,
                     &bg, &bb);
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
    uint8_t fr, fg, fb;
    bool has_bg;
    uint8_t br, bgg, bb;
} agenda_event_line_t;

// One event's color depends only on which of the two calendars it came
// from (not on today/later, unlike the row-based scheme this replaced).
// Originally this drew a full chip (white text on a source hue) on a
// light page background and plain colored text on a dark one - live
// testing found the chip visually noisy against the actual event rows
// ("Die Schrift-Hintergrundfarbe ... stört bei den Termineinträgen"), so
// both backgrounds now use the same plain-colored-text, no-fill
// treatment, using each source's user-configurable hue
// (config_manager_get_agenda_cal_a_color()/_cal_b_color()) with the usual
// collision-avoidance fallback if that hue happens to match the page
// background. Grayscale boards have no spare hue for this at all (same
// reasoning as priority_color()'s grayscale fallback) and fall back to
// plain body-colored text.
static void calendar_source_color(int calendar_index, bool grayscale, uint8_t bg_r, uint8_t bg_g,
                                  uint8_t bg_b, uint8_t *fr, uint8_t *fg, uint8_t *fb, bool *has_bg)
{
    *has_bg = false;
    if (calendar_index == 0) {
        resolve_plain_color(config_manager_get_agenda_cal_a_color(), grayscale, 0, 0, 255, bg_r,
                            bg_g, bg_b, fr, fg, fb);
    } else {
        resolve_plain_color(config_manager_get_agenda_cal_b_color(), grayscale, 0, 255, 0, bg_r,
                            bg_g, bg_b, fr, fg, fb);
    }
}

// Builds one event's display text - "HH:MM " (omitted for an all-day
// event) followed by the summary - and resolves the single color (plus
// optional chip) the whole row draws in, per calendar_source_color()
// above. No date/day-of-week here: draw_calendar_column() shows that once
// per day group via draw_day_divider(), not repeated on every event.
static void build_event_line(const ics_event_t *ev, int calendar_index, bool grayscale,
                             uint8_t bg_r, uint8_t bg_g, uint8_t bg_b, agenda_event_line_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    size_t cap = sizeof(out->text) - 1;

    if (!ev->all_day) {
        struct tm start_tm;
        localtime_r(&ev->start, &start_tm);
        int n = snprintf(out->text + pos, cap - pos + 1, "%02d:%02d ", start_tm.tm_hour,
                         start_tm.tm_min);
        if (n > 0) {
            size_t written = ((size_t) n <= cap - pos) ? (size_t) n : cap - pos;
            pos += written;
        }
    }

    size_t slen = strlen(ev->summary);
    if (slen > cap - pos) {
        slen = cap - pos;
    }
    memcpy(out->text + pos, ev->summary, slen);
    pos += slen;
    out->text[pos] = '\0';

    calendar_source_color(calendar_index, grayscale, bg_r, bg_g, bg_b, &out->fr, &out->fg, &out->fb,
                          &out->has_bg);
}

// Draws a day-separator row: a dashed horizontal line with a chip showing
// the weekday + day number, plus (if `weather_mode` is on and this
// particular day has one) a second chip with that day's forecast. Inverts
// polarity with the page background (dark-on-light fill/dashes normally,
// light-on-dark when the background is dark) rather than staying
// black-fixed, so it's never invisible against a dark chosen background -
// `fill_r/g/b` is whichever of black/white agenda_is_light() picked for
// the CURRENT background (i.e. body_r/g/b from the caller), and the label
// text is simply the opposite of that.
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
static void draw_day_divider(uint8_t *rgb, int width, int height, agenda_rect_t rect, int y,
                             const char *label, bool weather_mode, bool weather_right_aligned,
                             const char *weather_text, uint8_t fill_r, uint8_t fill_g,
                             uint8_t fill_b)
{
    uint8_t text_r, text_g, text_b;
    agenda_safe_text_color(fill_r, fill_g, fill_b, &text_r, &text_g, &text_b);

    int total_w = rect.w - 2 * AGENDA_PADDING;
    int line_y = y + IMAGE_PROCESSOR_FONT_HEIGHT / 2 - 1;
    const int dash_len = 4, gap_len = 3, dash_h = 2;

    if (!weather_mode) {
        int label_w = (int) strlen(label) * IMAGE_PROCESSOR_FONT_WIDTH;
        if (label_w > total_w) {
            label_w = total_w;  // pathologically narrow column - clip rather than overflow
        }
        int label_x = rect.x + AGENDA_PADDING + (total_w - label_w) / 2;

        for (int x = rect.x + AGENDA_PADDING; x + dash_len <= label_x; x += dash_len + gap_len) {
            image_processor_fill_rect(rgb, width, height, x, line_y, dash_len, dash_h, fill_r,
                                      fill_g, fill_b);
        }
        int right_start = label_x + label_w;
        int right_end = rect.x + AGENDA_PADDING + total_w;
        for (int x = right_start; x + dash_len <= right_end; x += dash_len + gap_len) {
            image_processor_fill_rect(rgb, width, height, x, line_y, dash_len, dash_h, fill_r,
                                      fill_g, fill_b);
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

    // No forecast entry for this specific day: weather_w stays 0 and
    // weather_x defaults to the row's right edge, which collapses the two
    // dash runs below into a single continuous one spanning the whole gap
    // after the label - not two runs that happen to line up.
    int weather_w = 0;
    int weather_x = right_end;
    char weather_clipped[WEATHER_DAY_LINE_MAX_LEN] = "";
    if (weather_text && weather_text[0] != '\0') {
        // Clip the weather chip to whatever's left after the label plus a
        // minimum gap, rather than letting it overlap - a narrow column
        // with a long day label (unlikely, but the label itself is
        // already clipped above for the same reason) is the only case
        // this ever triggers.
        int max_weather_chars =
            (total_w - label_w - (dash_len + gap_len)) / IMAGE_PROCESSOR_FONT_WIDTH;
        if (max_weather_chars < 0) {
            max_weather_chars = 0;
        }
        strncpy(weather_clipped, weather_text, sizeof(weather_clipped) - 1);
        weather_clipped[sizeof(weather_clipped) - 1] = '\0';
        if ((int) strlen(weather_clipped) > max_weather_chars) {
            weather_clipped[max_weather_chars] = '\0';
        }
        weather_w = (int) strlen(weather_clipped) * IMAGE_PROCESSOR_FONT_WIDTH;
        weather_x = weather_right_aligned ? (right_end - weather_w)
                                          : (rect.x + AGENDA_PADDING + (total_w - weather_w) / 2);
        int min_weather_x = label_x + label_w + dash_len;
        if (weather_w > 0 && weather_x < min_weather_x) {
            weather_x = min_weather_x;
        }
    }

    for (int x = label_x + label_w; x + dash_len <= weather_x; x += dash_len + gap_len) {
        image_processor_fill_rect(rgb, width, height, x, line_y, dash_len, dash_h, fill_r, fill_g,
                                  fill_b);
    }
    int right_start = weather_x + weather_w;
    for (int x = right_start; x + dash_len <= right_end; x += dash_len + gap_len) {
        image_processor_fill_rect(rgb, width, height, x, line_y, dash_len, dash_h, fill_r, fill_g,
                                  fill_b);
    }

    if (label_w > 0) {
        image_processor_fill_rect(rgb, width, height, label_x - 2, y, label_w + 4,
                                  IMAGE_PROCESSOR_FONT_HEIGHT, fill_r, fill_g, fill_b);
        image_processor_draw_text(rgb, width, height, label_x, y, label, text_r, text_g, text_b);
    }
    if (weather_w > 0) {
        image_processor_fill_rect(rgb, width, height, weather_x - 2, y, weather_w + 4,
                                  IMAGE_PROCESSOR_FONT_HEIGHT, fill_r, fill_g, fill_b);
        image_processor_draw_text(rgb, width, height, weather_x, y, weather_clipped, text_r,
                                  text_g, text_b);
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
// used only by the opt-in "compact multi-day" display below. A 1-day event
// (the overwhelming majority) returns 1.
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
#define AGENDA_MAX_TAGGED_EVENTS (ICS_MAX_EVENTS * 2)  // events_a + events_b, worst case both full

// One event plus the pre-resolved line/color build_event_line() computed
// for it (which already baked in which calendar it came from) - merging
// events_a/events_b into one array of these is what lets the rest of this
// function treat "two calendars" as "one sorted list" without caring which
// source any given entry came from again.
typedef struct {
    const ics_event_t *ev;
    const agenda_event_line_t *line;
} agenda_tagged_event_t;

// One calendar source's header display name plus the color swatch to draw
// before it - see draw_calendar_column()'s header-drawing comment. `show`
// and `has_swatch` are independent: a source with events always shows its
// name, but only gets a swatch on a color-capable board (grayscale has no
// spare hue to legend at all - calendar_source_color() already gives every
// source the same plain black there).
typedef struct {
    bool show;
    bool has_swatch;
    const char *name;
    uint8_t r, g, b;
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
// that day - including a multi-day event, which is deliberately repeated
// under each day it spans rather than shown once under its start day
// only. The day list is derived from the merged events but clipped to the
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

static void draw_calendar_column(uint8_t *rgb, int width, int height, agenda_rect_t rect,
                                 time_t now, int lookahead_days, uint8_t body_r, uint8_t body_g,
                                 uint8_t body_b, const agenda_tagged_event_t *tagged,
                                 int tagged_count, const weather_forecast_t *cal_weather,
                                 agenda_cal_name_tag_t name_a, agenda_cal_name_tag_t name_b)
{
    uint8_t header_text_r, header_text_g, header_text_b;
    agenda_safe_text_color(body_r, body_g, body_b, &header_text_r, &header_text_g,
                           &header_text_b);

    int header_h = IMAGE_PROCESSOR_FONT_HEIGHT + 2 * AGENDA_PADDING;
    image_processor_fill_rect(rgb, width, height, rect.x, rect.y, rect.w, header_h, body_r, body_g,
                              body_b);

    // Each shown calendar's name gets a small colored "swatch" immediately
    // before it - a single blank character cell filled with that source's
    // actual event text color (name_a/name_b.r/g/b, already resolved by
    // the caller including any background-collision fallback) - so the
    // header doubles as a color legend for which name maps to which
    // color, without any new visible glyph. Skipped on grayscale (the
    // caller passes show=false there, since calendar_source_color()
    // already gives every source the same plain black there - no color to
    // legend in the first place).
    int hx = rect.x + AGENDA_PADDING;
    int hy = rect.y + AGENDA_PADDING;
    if (name_a.show) {
        if (name_a.has_swatch) {
            image_processor_fill_rect(rgb, width, height, hx, hy, IMAGE_PROCESSOR_FONT_WIDTH,
                                      IMAGE_PROCESSOR_FONT_HEIGHT, name_a.r, name_a.g, name_a.b);
            hx += IMAGE_PROCESSOR_FONT_WIDTH;
        }
        image_processor_draw_text(rgb, width, height, hx, hy, name_a.name, header_text_r,
                                  header_text_g, header_text_b);
        hx += (int) strlen(name_a.name) * IMAGE_PROCESSOR_FONT_WIDTH;
    }
    if (name_a.show && name_b.show) {
        image_processor_draw_text(rgb, width, height, hx, hy, ", ", header_text_r, header_text_g,
                                  header_text_b);
        hx += 2 * IMAGE_PROCESSOR_FONT_WIDTH;
    }
    if (name_b.show) {
        if (name_b.has_swatch) {
            image_processor_fill_rect(rgb, width, height, hx, hy, IMAGE_PROCESSOR_FONT_WIDTH,
                                      IMAGE_PROCESSOR_FONT_HEIGHT, name_b.r, name_b.g, name_b.b);
            hx += IMAGE_PROCESSOR_FONT_WIDTH;
        }
        image_processor_draw_text(rgb, width, height, hx, hy, name_b.name, header_text_r,
                                  header_text_g, header_text_b);
        hx += (int) strlen(name_b.name) * IMAGE_PROCESSOR_FONT_WIDTH;
    }

    struct tm now_tm;
    localtime_r(&now, &now_tm);
    // Oversized vs. the typical ~20-char result - GCC's format-truncation
    // checker can't prove tm_year+1900 always fits in 4 digits from this
    // call site alone (same reason todo_item_role() widened its own date
    // buffer earlier in this feature).
    char datetime[40];
    snprintf(datetime, sizeof(datetime), " - %02d.%02d.%04d %02d:%02d", now_tm.tm_mday,
             now_tm.tm_mon + 1, now_tm.tm_year + 1900, now_tm.tm_hour, now_tm.tm_min);
    image_processor_draw_text(rgb, width, height, hx, hy, datetime, header_text_r, header_text_g,
                              header_text_b);

    int row_h = IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING;
    int content_top = rect.y + header_h + AGENDA_PADDING;
    int content_h = rect.h - header_h - AGENDA_PADDING;
    int max_rows = (content_h > 0) ? content_h / row_h : 0;
    int text_width = rect.w - 2 * AGENDA_PADDING;

    time_t win_day_start = day_start(now);
    time_t win_day_end = day_start(now + (time_t) lookahead_days * 86400 - 1);

    time_t days[AGENDA_MAX_CAL_DAYS];
    int day_count = 0;
    for (time_t d = win_day_start; d <= win_day_end && day_count < AGENDA_MAX_CAL_DAYS;
         d += 86400) {
        bool has_event = false;
        for (int k = 0; k < tagged_count && !has_event; k++) {
            has_event = event_touches_day(tagged[k].ev, d);
        }
        if (has_event) {
            days[day_count++] = d;
        }
    }

    // Opt-in: a multi-day event is shown only once, on the first day of the
    // *visible* window it touches, with an "N/M: " prefix (N = its position
    // within its own full span, M = that span's total length) instead of
    // being repeated under every day it spans. Precomputed once per tagged
    // event rather than re-derived per day, since both the budgeting pass
    // and the render pass below need the same answer.
    bool compact_multiday = config_manager_get_agenda_cal_compact_multiday();
    bool is_multiday[AGENDA_MAX_TAGGED_EVENTS];
    int first_visible_idx[AGENDA_MAX_TAGGED_EVENTS];
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
            if (compact_multiday && is_multiday[k] && di != first_visible_idx[k]) {
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

    int rows_used = 0;
    int instances_shown = 0;
    for (int di = 0; di < day_count && rows_used < budget; di++) {
        struct tm day_tm;
        localtime_r(&days[di], &day_tm);
        char label[16];
        snprintf(label, sizeof(label), "%s %d.", weather_weekday_abbr(day_tm.tm_wday, german),
                day_tm.tm_mday);
        char weather_buf[WEATHER_DAY_LINE_MAX_LEN] = "";
        const weather_day_t *wday;
        if (find_weather_for_day(cal_weather, days[di], &wday)) {
            int tmin = (int) lroundf(wday->temp_min_c);
            int tmax = (int) lroundf(wday->temp_max_c);
            const char *cond = weather_condition_text(wday->weather_code, german);
            snprintf(weather_buf, sizeof(weather_buf), "[%d/%d %s]", tmin, tmax, cond);
        }
        draw_day_divider(rgb, width, height, rect, content_top + rows_used * row_h, label,
                         weather_mode, weather_right_aligned, weather_buf[0] ? weather_buf : NULL,
                         body_r, body_g, body_b);
        rows_used++;

        for (int i = 0; i < tagged_count && rows_used < budget; i++) {
            if (!event_touches_day(tagged[i].ev, days[di])) {
                continue;
            }
            if (compact_multiday && is_multiday[i] && di != first_visible_idx[i]) {
                continue;
            }
            const agenda_event_line_t *line = tagged[i].line;

            char prefix[16] = "";
            int prefix_len = 0;
            if (compact_multiday && is_multiday[i]) {
                prefix_len = snprintf(prefix, sizeof(prefix), "%d/%d: ",
                                      event_day_index(tagged[i].ev, days[di]),
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
                    image_processor_draw_text(rgb, width, height, x, y, prefix, body_r, body_g,
                                              body_b);
                    x += prefix_len * IMAGE_PROCESSOR_FONT_WIDTH;
                }
                int visible_len = (int) strlen(wrapped[0]);
                if (line->has_bg) {
                    image_processor_fill_rect(rgb, width, height, x, y,
                                              visible_len * IMAGE_PROCESSOR_FONT_WIDTH,
                                              IMAGE_PROCESSOR_FONT_HEIGHT, line->br, line->bgg,
                                              line->bb);
                }
                image_processor_draw_text(rgb, width, height, x, y, wrapped[0], line->fr,
                                          line->fg, line->fb);
                rows_used++;
                instances_shown++;
            }
        }
    }

    if (instances_shown < total_event_instances && rows_used < max_rows) {
        char more[32];
        snprintf(more, sizeof(more), "+%d more", total_event_instances - instances_shown);
        int y = content_top + rows_used * row_h;
        image_processor_draw_text(rgb, width, height, rect.x + AGENDA_PADDING, y, more, body_r,
                                  body_g, body_b);
    }
}

// Same header/row-budget/"+N more" behavior as draw_column() above, but for
// ToDo rows: each row's colors come from the per-element runs
// build_todo_line() already computed, clipped to whatever
// image_processor_wrap_text() actually kept if the row had to be truncated
// (a run starting past the truncation cutoff is simply never reached; one
// straddling the cutoff is shortened to match).
static void draw_todo_column(uint8_t *rgb, int width, int height, agenda_rect_t rect, time_t now,
                             uint8_t body_r, uint8_t body_g, uint8_t body_b,
                             const agenda_line_t *todo_lines, int line_count)
{
    uint8_t header_text_r, header_text_g, header_text_b;
    agenda_safe_text_color(body_r, body_g, body_b, &header_text_r, &header_text_g,
                           &header_text_b);

    struct tm now_tm;
    localtime_r(&now, &now_tm);
    char header[64];  // oversized for the same -Wformat-truncation reason as the Calendar header
    snprintf(header, sizeof(header), "TODO - %02d.%02d.%04d %02d:%02d", now_tm.tm_mday,
             now_tm.tm_mon + 1, now_tm.tm_year + 1900, now_tm.tm_hour, now_tm.tm_min);

    int header_h = IMAGE_PROCESSOR_FONT_HEIGHT + 2 * AGENDA_PADDING;
    image_processor_fill_rect(rgb, width, height, rect.x, rect.y, rect.w, header_h, body_r, body_g,
                              body_b);
    image_processor_draw_text(rgb, width, height, rect.x + AGENDA_PADDING, rect.y + AGENDA_PADDING,
                              header, header_text_r, header_text_g, header_text_b);

    int row_h = IMAGE_PROCESSOR_FONT_HEIGHT + AGENDA_PADDING;
    int content_top = rect.y + header_h + AGENDA_PADDING;
    int content_h = rect.h - header_h - AGENDA_PADDING;
    int max_rows = (content_h > 0) ? content_h / row_h : 0;

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
            image_processor_fill_rect(rgb, width, height, x + run->start * IMAGE_PROCESSOR_FONT_WIDTH,
                                      y, len * IMAGE_PROCESSOR_FONT_WIDTH,
                                      IMAGE_PROCESSOR_FONT_HEIGHT, run->r, run->g, run->b);
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
                                 const ics_event_list_t *events_b,
                                 const weather_forecast_t *cal_weather, int lookahead_days,
                                 const char *output_path, image_format_t out_format)
{
    if (!output_path) {
        return ESP_ERR_INVALID_ARG;
    }
    bool show_todo = todo && todo->count > 0;
    bool have_a = events_a && events_a->count > 0;
    bool have_b = events_b && events_b->count > 0;
    bool show_cal = have_a || have_b;
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
    agenda_background_color(grayscale, &bg_r, &bg_g, &bg_b);
    image_processor_fill_rect(rgb, width, height, 0, 0, width, height, bg_r, bg_g, bg_b);

    // The one color every "plain, no own fill" text element falls back to
    // (body text, "+N more", any role whose usual color happens to collide
    // with the chosen background) - computed once here rather than
    // separately in each column, since it only depends on the background.
    uint8_t body_r = 0, body_g = 0, body_b = 0;
    agenda_avoid_bg_collision(bg_r, bg_g, bg_b, &body_r, &body_g, &body_b);

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

    if (show_todo) {
        agenda_line_t *todo_lines =
            heap_caps_malloc((size_t) todo->count * sizeof(agenda_line_t), MALLOC_CAP_SPIRAM);
        if (todo_lines) {
            for (int i = 0; i < todo->count; i++) {
                build_todo_line(&todo->items[i], now, grayscale, bg_r, bg_g, bg_b,
                               &todo_lines[i]);
            }
            draw_todo_column(rgb, width, height, todo_rect, now, body_r, body_g, body_b,
                             todo_lines, todo->count);
        } else {
            ESP_LOGW(TAG, "Failed to allocate ToDo render scratch buffers - skipping ToDo column");
        }
        heap_caps_free(todo_lines);
    }

    if (show_cal) {
        int count_a = have_a ? events_a->count : 0;
        int count_b = have_b ? events_b->count : 0;

        agenda_event_line_t *lines_a =
            heap_caps_malloc((size_t) (count_a > 0 ? count_a : 1) * sizeof(agenda_event_line_t),
                            MALLOC_CAP_SPIRAM);
        agenda_event_line_t *lines_b =
            heap_caps_malloc((size_t) (count_b > 0 ? count_b : 1) * sizeof(agenda_event_line_t),
                            MALLOC_CAP_SPIRAM);
        agenda_tagged_event_t *tagged = heap_caps_malloc(
            AGENDA_MAX_TAGGED_EVENTS * sizeof(agenda_tagged_event_t), MALLOC_CAP_SPIRAM);

        if (lines_a && lines_b && tagged) {
            int tagged_count = 0;
            for (int i = 0; i < count_a && tagged_count < AGENDA_MAX_TAGGED_EVENTS; i++) {
                build_event_line(&events_a->events[i], 0, grayscale, bg_r, bg_g, bg_b, &lines_a[i]);
                tagged[tagged_count].ev = &events_a->events[i];
                tagged[tagged_count].line = &lines_a[i];
                tagged_count++;
            }
            for (int i = 0; i < count_b && tagged_count < AGENDA_MAX_TAGGED_EVENTS; i++) {
                build_event_line(&events_b->events[i], 1, grayscale, bg_r, bg_g, bg_b, &lines_b[i]);
                tagged[tagged_count].ev = &events_b->events[i];
                tagged[tagged_count].line = &lines_b[i];
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
            // hasn't set a custom display name for that source. Each
            // shown name's color swatch reuses lines_a[0]/lines_b[0]'s
            // already-resolved event text color (fr/fg/fb) rather than
            // re-deriving the hue, so it's guaranteed to exactly match
            // what that source's events are actually drawn in this cycle,
            // collision-avoidance fallback included - skipped on
            // grayscale, where that color is always plain black anyway
            // (see calendar_source_color()) and so carries no legend value.
            const char *name_a = config_manager_get_agenda_cal_name();
            if (!name_a || name_a[0] == '\0') {
                name_a = "Calendar A";
            }
            const char *name_b = config_manager_get_agenda_cal_name2();
            if (!name_b || name_b[0] == '\0') {
                name_b = "Calendar B";
            }
            agenda_cal_name_tag_t tag_a = {.show = have_a, .has_swatch = have_a && !grayscale,
                                           .name = name_a};
            if (tag_a.has_swatch) {
                tag_a.r = lines_a[0].fr;
                tag_a.g = lines_a[0].fg;
                tag_a.b = lines_a[0].fb;
            }
            agenda_cal_name_tag_t tag_b = {.show = have_b, .has_swatch = have_b && !grayscale,
                                           .name = name_b};
            if (tag_b.has_swatch) {
                tag_b.r = lines_b[0].fr;
                tag_b.g = lines_b[0].fg;
                tag_b.b = lines_b[0].fb;
            }
            draw_calendar_column(rgb, width, height, cal_rect, now, lookahead_days, body_r,
                                 body_g, body_b, tagged, tagged_count, cal_weather, tag_a, tag_b);
        } else {
            ESP_LOGW(TAG, "Failed to allocate Calendar render scratch buffers - skipping column");
        }
        heap_caps_free(lines_a);
        heap_caps_free(lines_b);
        heap_caps_free(tagged);
    }

    image_format_t actual_format = out_format;
    esp_err_t err = image_processor_write_rgb_to_fmt(rgb, width, height, output_path, out_format,
                                                     &actual_format);
    heap_caps_free(rgb);
    return err;
}
