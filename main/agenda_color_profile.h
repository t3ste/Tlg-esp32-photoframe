#ifndef AGENDA_COLOR_PROFILE_H
#define AGENDA_COLOR_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} agenda_rgb_t;

// A fully-resolved Calendar-view color profile, ready to render with no
// further lookup - see agenda_color_profile_load_active() below. Field
// names mirror profile-editor.html's exported "spectra6-firmware-profile"
// JSON schema 1:1 (see that file's buildProfilePayload()/COLOR_KEYS) so the
// two stay easy to cross-check.
typedef struct {
    bool active;       // false = no profile selected; every field below is still
                       // filled with a sane built-in black-on-white default, so
                       // callers never need a separate no-profile code path.
    bool mono;         // profile "mode" starts with "mono-"
    bool mono_invert;  // mode == "mono-invert" (vs. "mono-normal")

    bool has_mark;            // "mark" != "none"
    agenda_rgb_t mark;        // resolved marker color - color mode only; mono
                              // marking is always a plain page/ink invert instead
                              // (matches profile-editor.html's own mono() model)
    bool mark_colors_header;  // "markColorsHeader": true = the marker paints
                              // the day HEADER; false = it paints the day
                              // BODY/events instead

    agenda_rgb_t text, text_bg;           // day body ink / page background
    agenda_rgb_t header_text, header_bg;  // per-day header bar
    agenda_rgb_t top_text, top_bg;        // shared top header bar

    // "headerFollowsEntries"/"headerFollowsEntriesColor" (mono/color modes
    // respectively - only the one matching the profile's own mode applies,
    // exactly like profile-editor.html's own `monoV ? headerFollowsEntries :
    // headerFollowsEntriesColor` selector): when on, an UNMARKED day's
    // header abandons its own header_text/header_bg pair and instead shows
    // the same colors as the day's body (`text`/`text_bg`) - and a MARKED
    // day's header (even if `markColorsHeader` is off) shows the mark color
    // too, with color-mode ink taken literally from `text` rather than
    // auto-derived. See agenda_day_header_colors()'s own comment for the
    // full 4-case precedence this interacts with.
    bool header_follows_entries;
    bool header_follows_entries_color;

    // "headerFollowsEntriesColorSafeInk" (optional, default false; color
    // mode only - mono's marked-header ink always equals its unmarked ink
    // regardless, see agenda_day_header_colors()'s own comment, so this
    // flag has no effect there): the marked-day case above takes `text`
    // literally, which can end up equal to `mark` (e.g. both "white") and
    // render invisible - reported live (2026-09-20) via a profile with
    // mark=white, colors.text=white. When on, that literal ink is replaced
    // with an auto-derived black/white contrast against `mark`, exactly
    // like `markColorsHeader`'s own ink already does - but scoped to ONLY
    // the header's ink, unlike `markColorsHeader`, which also stops the
    // mark color from applying to the day's body at all. Opt-in rather than
    // always-on so an existing profile that intentionally relies on the
    // literal (possibly custom, non-black/white) ink keeps its exact look.
    bool header_follows_entries_color_safe_ink;

    // "dayheadLeaderLine" (optional, default false): whether the day
    // header shows a decorative dashed rule, drawn in the header's own ink
    // color, between the day label and the weather forecast chip (only
    // meaningful when the weather annotation is on) - profile-editor.html's
    // own preview draws this as a `border-bottom:2px dashed currentColor`
    // rule, i.e. ink-colored, independent of whichever background fill is
    // active (see agenda_color_profile_t's header_divider_filled comment -
    // the two are unrelated: one is a decorative foreground line, the other
    // is a background gap/fill choice).
    bool dayhead_leader_line;

    // "headerDividerFilled" (optional, default false): the per-day header's
    // dashed separator normally leaves the page background showing through
    // its gaps, which can visually read as "two different colors" in the
    // same header even though both the dashes and the day label use the
    // identical header bg/ink pair - true fills the whole divider row
    // solidly in that pair instead, with no gaps at all.
    bool header_divider_filled;

    // "iconBg" (optional, default white - see agenda_color_profile.c's
    // fill_default()): the per-day weather icon's OWN background, drawn
    // independently of header_bg/mark so a same-colored header (including a
    // shift-marked one) can never make the icon invisible - its ink is
    // always auto-derived from this background (agenda_safe_text_color()),
    // since the icon's own fixed traffic-light hue already carries meaning
    // and can't be reassigned. On a mono profile this field is ignored
    // entirely - the icon bg is instead always the exact opposite of
    // whatever this specific day's header bg resolved to, which structurally
    // guarantees no collision with only two colors available.
    agenda_rgb_t icon_bg;

    // "iconBgMarked" (optional, default = icon_bg's own value - see
    // agenda_color_profile.c's fill_default()/parse_payload()): the same
    // weather icon background as icon_bg above, but used on a MARKED day
    // instead - requested live (2026-09-20) so a profile can pick the icon's
    // background independently for the marked vs. unmarked case, the same
    // way header/body colors already can. Defaulting to icon_bg's own value
    // when unset means a profile that never sets this renders identically
    // to before this field existed. Ignored on a mono profile, exactly like
    // icon_bg - see that field's own comment for why.
    agenda_rgb_t icon_bg_marked;

    agenda_rgb_t cal_ink[5], cal_bg[5];  // Calendar A-E, index 0=A..4=E
} agenda_color_profile_t;

// Writes the absolute file path for `slot` (1..AGENDA_COLOR_PROFILE_SLOTS)
// into buf; an out-of-range slot yields an empty string. Does not check
// whether the file actually exists.
void agenda_color_profile_path(int slot, char *buf, size_t buf_len);

// Parses `json_text` (a NUL-terminated spectra6-firmware-profile document)
// and checks it carries every field the firmware needs to render the
// Calendar view. Returns true on success; on failure returns false and, if
// err_out is non-NULL, writes a short human-readable reason (truncated to
// fit). If name_out is non-NULL, the profile's "name" field is copied there
// on success (truncated to fit). Used both to validate an incoming HTTP
// import before it's saved, and internally by
// agenda_color_profile_load_active() below.
bool agenda_color_profile_validate(const char *json_text, char *name_out, size_t name_out_len,
                                   char *err_out, size_t err_out_len);

// Reads just the "name" field of the profile stored in `slot` - returns
// false if the slot is empty, unreadable, or not valid JSON. Cheap enough
// to call once per slot for a Web UI listing.
bool agenda_color_profile_slot_name(int slot, char *name_out, size_t name_out_len);

// Loads whichever slot config_manager_get_agenda_color_profile_active()
// currently names into *out, resolving every color-string field (a named
// Spectra6 hue or a "#rrggbb" hex string) to concrete RGB. Always succeeds:
// no active slot, a missing file, or a parse/validation failure all fall
// back to a literal built-in black-on-white default (out->active = false)
// rather than requiring the caller to handle a separate error case.
void agenda_color_profile_load_active(agenda_color_profile_t *out);

#endif
