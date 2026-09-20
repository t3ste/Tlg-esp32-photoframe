#include "agenda_color_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "esp_log.h"

static const char *TAG = "agenda_color_profile";

void agenda_color_profile_path(int slot, char *buf, size_t buf_len)
{
    const char *path;
    switch (slot) {
    case 1:
        path = AGENDA_COLOR_PROFILE_PATH_1;
        break;
    case 2:
        path = AGENDA_COLOR_PROFILE_PATH_2;
        break;
    case 3:
        path = AGENDA_COLOR_PROFILE_PATH_3;
        break;
    default:
        path = "";
        break;
    }
    strncpy(buf, path, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

// Parses a profile-editor.html color value: either one of the 6 exact
// Spectra6 palette names (matching this project's existing role_hue()
// convention elsewhere in agenda_renderer.c) or a "#rrggbb" hex string -
// the tool's own <input type=color> pickers always emit hex, while a hand-
// edited/legacy JSON may use the plain names instead. Both forms are seen
// in the wild in the same document (profil-SchwarzGelb.json mixes
// "yellow"/"black" for most fields with "#ffffff"/"#000000" for
// topText/topBg). A resulting RGB triple that isn't an exact Spectra6
// palette color still displays correctly - image_processor_write_rgb_to_fmt()'s
// own palette quantization (the same step every photo already goes
// through) snaps it to the nearest real panel color at final-output time,
// so no separate snapping is needed here.
static bool parse_color_value(const char *s, agenda_rgb_t *out)
{
    if (!s) {
        return false;
    }
    if (s[0] == '#') {
        unsigned int v;
        if (strlen(s) != 7 || sscanf(s + 1, "%6x", &v) != 1) {
            return false;
        }
        out->r = (uint8_t) ((v >> 16) & 0xFF);
        out->g = (uint8_t) ((v >> 8) & 0xFF);
        out->b = (uint8_t) (v & 0xFF);
        return true;
    }
    if (strcmp(s, "black") == 0) {
        *out = (agenda_rgb_t){0, 0, 0};
    } else if (strcmp(s, "white") == 0) {
        *out = (agenda_rgb_t){255, 255, 255};
    } else if (strcmp(s, "red") == 0) {
        *out = (agenda_rgb_t){255, 0, 0};
    } else if (strcmp(s, "yellow") == 0) {
        *out = (agenda_rgb_t){255, 255, 0};
    } else if (strcmp(s, "blue") == 0) {
        *out = (agenda_rgb_t){0, 0, 255};
    } else if (strcmp(s, "green") == 0) {
        *out = (agenda_rgb_t){0, 255, 0};
    } else {
        return false;
    }
    return true;
}

static void set_err(char *err_out, size_t err_out_len, const char *msg)
{
    if (err_out && err_out_len > 0) {
        strncpy(err_out, msg, err_out_len - 1);
        err_out[err_out_len - 1] = '\0';
    }
}

// Shared by every optional boolean switch field (markColorsHeader,
// headerDividerFilled, headerFollowsEntries/Color, dayheadLeaderLine) -
// missing or non-boolean defaults to false, matching this project's
// fail-soft style for a profile that predates one of these fields.
static bool parse_optional_bool(cJSON *root, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    return (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : false;
}

static void fill_default(agenda_color_profile_t *out)
{
    memset(out, 0, sizeof(*out));
    agenda_rgb_t black = {0, 0, 0}, white = {255, 255, 255};
    out->text = black;
    out->text_bg = white;
    out->header_text = white;
    out->header_bg = black;
    out->top_text = white;
    out->top_bg = black;
    out->icon_bg = white;
    out->icon_bg_marked = white;
    for (int i = 0; i < 5; i++) {
        out->cal_ink[i] = black;
        out->cal_bg[i] = white;
    }
}

// Matches profile-editor.html's own COLOR_KEYS array exactly (buildProfilePayload()/
// applyProfilePayload()) - order here doubles as the index mapping used
// below (0-5 = generic/header/top pairs, 6-10 = Calendar A-E ink, 11-15 =
// Calendar A-E background).
static const char *const COLOR_KEYS[16] = {
    "text", "textBg", "headerText", "headerBg", "topText", "topBg", "a",   "b",
    "c",    "d",      "e",          "aBg",      "bBg",     "cBg",   "dBg", "eBg"};

static bool parse_payload(cJSON *root, agenda_color_profile_t *out, char *name_out,
                          size_t name_out_len, char *err_out, size_t err_out_len)
{
    fill_default(out);

    cJSON *type_json = cJSON_GetObjectItem(root, "type");
    if (!type_json || !cJSON_IsString(type_json) ||
        strcmp(type_json->valuestring, "spectra6-firmware-profile") != 0) {
        set_err(err_out, err_out_len, "Not a spectra6-firmware-profile document");
        return false;
    }

    cJSON *mode_json = cJSON_GetObjectItem(root, "mode");
    const char *mode = (mode_json && cJSON_IsString(mode_json)) ? mode_json->valuestring : "color";
    out->mono = (strncmp(mode, "mono", 4) == 0);
    out->mono_invert = (strcmp(mode, "mono-invert") == 0);

    cJSON *mark_json = cJSON_GetObjectItem(root, "mark");
    const char *mark = (mark_json && cJSON_IsString(mark_json)) ? mark_json->valuestring : "none";
    out->has_mark = (strcmp(mark, "none") != 0);
    if (out->has_mark && !parse_color_value(mark, &out->mark)) {
        // "mark" only ever holds one of the tool's palette names, never hex
        // - an unrecognized value is treated as "no marking" rather than a
        // hard validation failure, matching this project's fail-soft style
        // for a cosmetic setting.
        out->has_mark = false;
    }

    out->mark_colors_header = parse_optional_bool(root, "markColorsHeader");
    out->header_divider_filled = parse_optional_bool(root, "headerDividerFilled");
    out->header_follows_entries = parse_optional_bool(root, "headerFollowsEntries");
    out->header_follows_entries_color = parse_optional_bool(root, "headerFollowsEntriesColor");
    out->header_follows_entries_color_safe_ink =
        parse_optional_bool(root, "headerFollowsEntriesColorSafeInk");
    out->dayhead_leader_line = parse_optional_bool(root, "dayheadLeaderLine");

    cJSON *colors = cJSON_GetObjectItem(root, "colors");
    if (!colors || !cJSON_IsObject(colors)) {
        set_err(err_out, err_out_len, "Missing 'colors' object");
        return false;
    }

    agenda_rgb_t parsed[16];
    char missing[128] = "";
    size_t missing_len = 0;
    for (size_t i = 0; i < 16; i++) {
        cJSON *v = cJSON_GetObjectItem(colors, COLOR_KEYS[i]);
        if (!v || !cJSON_IsString(v) || !parse_color_value(v->valuestring, &parsed[i])) {
            int n = snprintf(missing + missing_len, sizeof(missing) - missing_len, "%s%s",
                             missing_len ? "," : "", COLOR_KEYS[i]);
            if (n > 0 && (size_t) n < sizeof(missing) - missing_len) {
                missing_len += (size_t) n;
            }
        }
    }
    if (missing_len > 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Missing/invalid color(s): %s", missing);
        set_err(err_out, err_out_len, msg);
        return false;
    }

    out->text = parsed[0];
    out->text_bg = parsed[1];
    out->header_text = parsed[2];
    out->header_bg = parsed[3];
    out->top_text = parsed[4];
    out->top_bg = parsed[5];
    for (int i = 0; i < 5; i++) {
        out->cal_ink[i] = parsed[6 + i];
        out->cal_bg[i] = parsed[11 + i];
    }

    // "iconBg" is optional (not part of the required 16 above) so a profile
    // exported before this field existed still imports cleanly - falls back
    // to fill_default()'s white rather than failing validation.
    cJSON *icon_bg_json = cJSON_GetObjectItem(colors, "iconBg");
    if (icon_bg_json && cJSON_IsString(icon_bg_json)) {
        parse_color_value(icon_bg_json->valuestring, &out->icon_bg);
    }

    // "iconBgMarked" (optional): the weather icon's background on a MARKED
    // day, independently choosable from "iconBg" above (which becomes the
    // unmarked-day value once this is set). Defaults to the same value as
    // "iconBg" when absent - a profile that predates this field, or simply
    // never set it, keeps rendering identically on both marked and unmarked
    // days, exactly as before.
    out->icon_bg_marked = out->icon_bg;
    cJSON *icon_bg_marked_json = cJSON_GetObjectItem(colors, "iconBgMarked");
    if (icon_bg_marked_json && cJSON_IsString(icon_bg_marked_json)) {
        parse_color_value(icon_bg_marked_json->valuestring, &out->icon_bg_marked);
    }

    if (name_out && name_out_len > 0) {
        cJSON *name_json = cJSON_GetObjectItem(root, "name");
        const char *name = (name_json && cJSON_IsString(name_json)) ? name_json->valuestring : "";
        strncpy(name_out, name, name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
    }

    out->active = true;
    return true;
}

// Reads a whole small file into a heap buffer (NUL-terminated). Returns
// NULL if the file is missing, empty, or larger than
// AGENDA_COLOR_PROFILE_MAX_BYTES (profiles are always a small fixed-shape
// JSON document, never arbitrary user text).
static char *read_small_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > AGENDA_COLOR_PROFILE_MAX_BYTES) {
        fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t) sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t) sz, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

bool agenda_color_profile_validate(const char *json_text, char *name_out, size_t name_out_len,
                                   char *err_out, size_t err_out_len)
{
    if (!json_text || json_text[0] == '\0') {
        set_err(err_out, err_out_len, "Empty body");
        return false;
    }
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        set_err(err_out, err_out_len, "Invalid JSON");
        return false;
    }
    agenda_color_profile_t tmp;
    bool ok = parse_payload(root, &tmp, name_out, name_out_len, err_out, err_out_len);
    cJSON_Delete(root);
    return ok;
}

bool agenda_color_profile_slot_name(int slot, char *name_out, size_t name_out_len)
{
    char path[64];
    agenda_color_profile_path(slot, path, sizeof(path));
    if (path[0] == '\0') {
        return false;
    }
    char *buf = read_small_file(path);
    if (!buf) {
        return false;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return false;
    }
    cJSON *name_json = cJSON_GetObjectItem(root, "name");
    bool ok = name_json && cJSON_IsString(name_json);
    if (ok && name_out && name_out_len > 0) {
        strncpy(name_out, name_json->valuestring, name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
    }
    cJSON_Delete(root);
    return ok;
}

void agenda_color_profile_load_active(agenda_color_profile_t *out)
{
    fill_default(out);
    int slot = config_manager_get_agenda_color_profile_active();
    if (slot <= 0) {
        return;
    }
    char path[64];
    agenda_color_profile_path(slot, path, sizeof(path));
    if (path[0] == '\0') {
        return;
    }
    char *buf = read_small_file(path);
    if (!buf) {
        ESP_LOGW(TAG, "Active color profile slot %d has no readable file, using default", slot);
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Active color profile slot %d has invalid JSON, using default", slot);
        return;
    }
    char err[96];
    if (!parse_payload(root, out, NULL, 0, err, sizeof(err))) {
        ESP_LOGW(TAG, "Active color profile slot %d failed validation (%s), using default", slot,
                 err);
        fill_default(out);
    }
    cJSON_Delete(root);
}
