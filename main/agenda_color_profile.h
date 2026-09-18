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
