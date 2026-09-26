#include "alarm_setting_ui.h"

#include <stdio.h>

#include "alarm_manager.h"
#include "board_hal.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "power_manager.h"

static const char *TAG = "alarm_setting_ui";

#ifndef CONFIG_ALARM_CLOCK_ENABLED

bool alarm_setting_ui_is_active(void)
{
    return false;
}

void alarm_setting_ui_tick(void) {}

void alarm_setting_ui_handle_boot_short_press(void) {}

void alarm_setting_ui_handle_boot_long_press(void) {}

void alarm_setting_ui_handle_key_short_press(void) {}

void alarm_setting_ui_handle_key_long_press(void) {}

void alarm_setting_ui_on_key_held(uint32_t held_ms)
{
    (void) held_ms;
}

#else

// Tuning constants - see docs/ALARMCLOCK_FEASIBILITY.md's Phase 3 section.
// (A "several fast presses batched into one silent count" mode was tried
// and removed after live testing on 2026-09-24 - it didn't work reliably
// in practice, so every press now confirms immediately, matching the
// original spec's slow-press behavior only.)
#define ENTRY_HOLD_MS 3000
#define INACTIVITY_TIMEOUT_MS 10000

#define BEEP_MS 120
#define BEEP_GAP_MS 90
#define LONG_MS 900
#define CONFIRM_SHORT_MS 150
#define CONFIRM_LONG_MS 700

// Five distinct pitches (AM hour / PM hour / minute / midnight-long /
// full-hour-long) plus two more for the armed/disarmed confirmation
// sequences - see confirm_hour()/confirm_minute()'s own comments for which
// is which and why.
#define AM_HOUR_FREQ_HZ 1000.0f
#define PM_HOUR_FREQ_HZ 600.0f
#define MINUTE_FREQ_HZ 1400.0f
#define MIDNIGHT_LONG_FREQ_HZ 300.0f
#define FULL_HOUR_LONG_FREQ_HZ 1800.0f
#define HOLD_CUE_FREQ_HZ 1100.0f
#define ENTERED_FREQ_HZ 900.0f
#define ARM_LOW_FREQ_HZ 500.0f
#define ARM_HIGH_FREQ_HZ 1700.0f

typedef enum {
    ALARM_UI_IDLE,
    ALARM_UI_SETTING,
} alarm_ui_state_t;

static alarm_ui_state_t s_state = ALARM_UI_IDLE;
static int s_hour = 0;         // 0-23, always starts at midnight on entry
static int s_minute_step = 0;  // 0-5, *10 = minutes

static int64_t s_last_activity_us = 0;
static bool s_hold_cue_played = false;  // guards the "still held" cue against repeating

static uint8_t volume(void)
{
    return (uint8_t) config_manager_get_alarm_volume();
}

static void play_beeps(float freq_hz, int count)
{
    if (count <= 0) {
        return;
    }
    // 12 beeps + 11 gaps between them = 23 entries, comfortably covers the
    // largest count this UI ever plays (12, for an hour ending in :00/:12).
    board_hal_note_t notes[23];
    int n = 0;
    for (int i = 0; i < count && n < 22; i++) {
        if (i > 0) {
            notes[n++] = (board_hal_note_t){0, BEEP_GAP_MS};
        }
        notes[n++] = (board_hal_note_t){freq_hz, BEEP_MS};
    }
    board_hal_play_notes(notes, n, volume());
}

static void play_long_tone(float freq_hz)
{
    board_hal_note_t note = {freq_hz, LONG_MS};
    board_hal_play_notes(&note, 1, volume());
}

// Midnight (hour 0) gets its own long tone instead of a 12-count beep
// sequence - matches the original spec's "Mitternacht hat einen langen
// erkennbaren Ton" requirement rather than the general count formula below.
// For every other hour, the beep count cycles 1-12 for the AM half of the
// day (hours 1-11) and again 1-12 for the PM half (hours 12-23) - pitch is
// the only thing that tells the two halves apart.
static void confirm_hour(int hour)
{
    if (hour == 0) {
        play_long_tone(MIDNIGHT_LONG_FREQ_HZ);
        return;
    }
    int count = ((hour - 1) % 12) + 1;
    float freq = (hour < 12) ? AM_HOUR_FREQ_HZ : PM_HOUR_FREQ_HZ;
    play_beeps(freq, count);
}

// The full hour (:00) gets its own long tone, at a different pitch than
// confirm_hour()'s own midnight long tone so the two are distinguishable -
// every other 10-minute step (1-5, i.e. :10-:50) plays that many beeps.
static void confirm_minute(int minute_step)
{
    if (minute_step == 0) {
        play_long_tone(FULL_HOUR_LONG_FREQ_HZ);
        return;
    }
    play_beeps(MINUTE_FREQ_HZ, minute_step);
}

// Armed = low-short-then-high-long; disarmed = the exact inverse
// (high-short-then-low-long) - deliberately symmetric opposites so the two
// outcomes are easy to tell apart by ear without having to count anything.
static void play_armed_tone(void)
{
    board_hal_note_t notes[2] = {
        {ARM_LOW_FREQ_HZ, CONFIRM_SHORT_MS},
        {ARM_HIGH_FREQ_HZ, CONFIRM_LONG_MS},
    };
    board_hal_play_notes(notes, 2, volume());
}

static void play_disarmed_tone(void)
{
    board_hal_note_t notes[2] = {
        {ARM_HIGH_FREQ_HZ, CONFIRM_SHORT_MS},
        {ARM_LOW_FREQ_HZ, CONFIRM_LONG_MS},
    };
    board_hal_play_notes(notes, 2, volume());
}

// Two quick same-pitch beeps, distinct from every other cue this module
// plays: the single-beep "still held, let go now" cue during the hold, and
// the two-DIFFERENT-pitch armed/disarmed sequences on the way back out -
// without a serial monitor attached, this is the only way to tell "you are
// now in the setting UI" apart from everything else happening around it.
static void play_entered_tone(void)
{
    play_beeps(ENTERED_FREQ_HZ, 2);
}

static void enter_setting_mode(void)
{
    s_state = ALARM_UI_SETTING;
    s_hour = 0;
    s_minute_step = 0;
    s_last_activity_us = esp_timer_get_time();
    play_entered_tone();
    ESP_LOGI(TAG, "Entered alarm-setting mode");
}

// Both exit paths below return the device to deep sleep afterward (if
// enabled and not USB-powered) rather than relying on the generic
// auto-sleep idle timer: that timer is deliberately never started at all
// for a WAKEUP_SOURCE_ROTATE_BUTTON wake (power_manager.c - "Skip auto-sleep
// timer if woken by ROTATE button", since that wake normally goes through
// its own dedicated deep_sleep_wake_task path with its own explicit sleep
// call instead) - main.c's long-press redirect diverts that same wake into
// this module's normal-awake path without ever restarting that timer, so
// without an explicit call here the device would stay awake indefinitely
// after finishing an alarm-setting session that started from a KEY-press
// deep-sleep wake.
static void return_to_sleep_if_appropriate(void)
{
    if (config_manager_get_deep_sleep_enabled() && !board_hal_is_usb_connected()) {
        power_manager_enter_sleep();
        // Does not return.
    }
}

// Confirming writes a single cron rule for Mon-Fri (workdays), matching the
// original spec's "Standardmaessig ... fuer alle Werktage" default - the Web
// UI/Telegram path (config_manager_set_alarm_cron_rules() called directly
// with a user-authored rule) is the only way to get a different weekday
// pattern or more than one rule; the physical button UI only ever manages
// one workdays-only rule, also per spec ("Per Knoepfe ... nur eine
// Weckzeit").
static void exit_setting_mode_confirm(void)
{
    char rule[32];
    snprintf(rule, sizeof(rule), "%d %d 1-5", s_minute_step * 10, s_hour);
    const char *one[1] = {rule};
    config_manager_set_alarm_cron_rules(one, 1);
    play_armed_tone();
    ESP_LOGI(TAG, "Alarm armed via button UI: %s (workdays)", rule);
    s_state = ALARM_UI_IDLE;
    return_to_sleep_if_appropriate();
}

static void exit_setting_mode_discard(void)
{
    config_manager_set_alarm_cron_rules(NULL, 0);
    play_disarmed_tone();
    ESP_LOGI(TAG, "Alarm-setting mode timed out - alarm disarmed");
    s_state = ALARM_UI_IDLE;
    return_to_sleep_if_appropriate();
}

bool alarm_setting_ui_is_active(void)
{
    return s_state == ALARM_UI_SETTING;
}

void alarm_setting_ui_tick(void)
{
    if (s_state != ALARM_UI_SETTING) {
        return;
    }
    int64_t now = esp_timer_get_time();

    if ((now - s_last_activity_us) >= (int64_t) INACTIVITY_TIMEOUT_MS * 1000) {
        exit_setting_mode_discard();
    }
}

void alarm_setting_ui_handle_boot_short_press(void)
{
    if (s_state != ALARM_UI_SETTING) {
        return;
    }
    s_hour = (s_hour + 1) % 24;
    s_last_activity_us = esp_timer_get_time();
    confirm_hour(s_hour);
}

void alarm_setting_ui_handle_boot_long_press(void)
{
    if (s_state != ALARM_UI_SETTING) {
        return;
    }
    // Reserved for the future offline voice-enrollment entry gesture
    // (docs/ALARMCLOCK_FEASIBILITY.md) - not implemented yet.
    ESP_LOGI(TAG, "Long BOOT press in alarm-setting mode - voice enrollment not implemented yet");
    s_last_activity_us = esp_timer_get_time();
}

void alarm_setting_ui_handle_key_short_press(void)
{
    if (s_state != ALARM_UI_SETTING) {
        return;
    }
    s_minute_step = (s_minute_step + 1) % 6;
    s_last_activity_us = esp_timer_get_time();
    confirm_minute(s_minute_step);
}

void alarm_setting_ui_handle_key_long_press(void)
{
    if (alarm_manager_is_ringing()) {
        // That long press already stopped the ringing alarm
        // (alarm_manager_run()'s own should_stop check) - don't also toggle
        // setting mode from the same physical action.
        return;
    }
    s_hold_cue_played = false;
    if (s_state == ALARM_UI_IDLE) {
        enter_setting_mode();
    } else {
        exit_setting_mode_confirm();
    }
}

void alarm_setting_ui_on_key_held(uint32_t held_ms)
{
    if (s_hold_cue_played || held_ms < ENTRY_HOLD_MS || alarm_manager_is_ringing()) {
        return;
    }
    // Fires once, while KEY is still held, so the user knows exactly when
    // the threshold has been reached and they can let go - the actual
    // enter/exit action only happens on release
    // (alarm_setting_ui_handle_key_long_press()).
    s_hold_cue_played = true;
    play_beeps(HOLD_CUE_FREQ_HZ, 1);
}

#endif  // CONFIG_ALARM_CLOCK_ENABLED
