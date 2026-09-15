#include "chime.h"

#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "config_manager.h"

// Per-event, per-boot fire cap - a generic backstop against any future bug
// that calls chime_play_if_enabled() repeatedly for one event in a tight
// loop within a single boot, independent of the (NVS-persisted, survives
// across boots) repeat-until-resolved cap in chime_repeat_gate() below.
// Reset on every boot/deep-sleep wake (this array is a plain static, never
// persisted). Matches CHIME_REPEAT_MAX so the two caps agree.
#define CHIME_MAX_FIRES_PER_BOOT CHIME_REPEAT_MAX
static int chime_fire_count[CHIME_EVENT_COUNT] = {0};

// "HH:MM" -> minutes since midnight. Returns false (caller treats quiet
// hours as off) for anything malformed rather than guessing.
static bool parse_hhmm(const char *s, int *out_minutes)
{
    if (!s || strlen(s) < 5 || s[2] != ':') {
        return false;
    }
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    *out_minutes = h * 60 + m;
    return true;
}

static bool chime_in_quiet_hours(void)
{
    if (!config_manager_get_chime_quiet_enabled()) {
        return false;
    }
    int start_min, end_min;
    if (!parse_hhmm(config_manager_get_chime_quiet_start(), &start_min) ||
        !parse_hhmm(config_manager_get_chime_quiet_end(), &end_min) || start_min == end_min) {
        return false;
    }

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int now_min = tm_now.tm_hour * 60 + tm_now.tm_min;

    if (start_min < end_min) {
        return now_min >= start_min && now_min < end_min;
    }
    return now_min >= start_min || now_min < end_min;  // window wraps past midnight
}

// Severity mapping - not a per-event sound, just which of the 3 built-in
// beep patterns fits: SUCCESS for "something good/neutral happened",
// WARNING for "needs attention soon", ERROR for "needs attention now".
static board_hal_chime_kind_t chime_kind_for_event(chime_event_t event)
{
    switch (event) {
    case CHIME_EVENT_LOW_BATTERY:
    case CHIME_EVENT_AGENDA_DUE:
        return BOARD_HAL_CHIME_WARNING;
    case CHIME_EVENT_WIFI_REPROVISION:
    case CHIME_EVENT_CRITICAL_ERROR:
        return BOARD_HAL_CHIME_ERROR;
    case CHIME_EVENT_ROTATION:
    case CHIME_EVENT_TELEGRAM_PHOTO:
    case CHIME_EVENT_OTA_SUCCESS:
    default:
        return BOARD_HAL_CHIME_SUCCESS;
    }
}

void chime_play_if_enabled(chime_event_t event)
{
    if (event < 0 || event >= CHIME_EVENT_COUNT || !board_hal_has_speaker()) {
        return;
    }

    chime_speaker_mode_t mode = config_manager_get_chime_speaker_mode();
    if (mode == CHIME_SPEAKER_OFF) {
        return;
    }
    if (mode == CHIME_SPEAKER_MAINS_ONLY && !board_hal_is_usb_connected()) {
        return;
    }
    if (!config_manager_get_chime_event_enabled(event)) {
        return;
    }
    if (chime_in_quiet_hours()) {
        return;
    }
    if (chime_fire_count[event] >= CHIME_MAX_FIRES_PER_BOOT) {
        return;
    }

    chime_fire_count[event]++;
    board_hal_play_beep_pattern(chime_kind_for_event(event),
                                (uint8_t) config_manager_get_chime_volume());
}

bool chime_repeat_gate(chime_event_t event, bool condition_active)
{
    if (event < 0 || event >= CHIME_EVENT_COUNT) {
        return false;
    }
    if (!condition_active) {
        if (config_manager_get_chime_repeat_count(event) != 0) {
            config_manager_set_chime_repeat_count(event, 0);
        }
        return false;
    }
    int count = config_manager_get_chime_repeat_count(event);
    if (count >= CHIME_REPEAT_MAX) {
        return false;
    }
    config_manager_set_chime_repeat_count(event, count + 1);
    return true;
}
