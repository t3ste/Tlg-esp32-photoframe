#include "alarm_manager.h"

#include <time.h>

#include "board_hal.h"
#include "config.h"
#include "config_manager.h"
#include "cron.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "alarm_manager";

#ifndef CONFIG_ALARM_CLOCK_ENABLED

bool alarm_manager_is_compiled_in(void)
{
    return false;
}

bool alarm_manager_is_enabled(void)
{
    return false;
}

bool alarm_manager_wake_matches_now(void)
{
    return false;
}

int alarm_manager_seconds_until_next_wake(void)
{
    return CRON_FALLBACK_SEC;
}

bool alarm_manager_is_ringing(void)
{
    return false;
}

void alarm_manager_run(void)
{
    ESP_LOGW(TAG, "alarm_manager_run() called on a build without CONFIG_ALARM_CLOCK_ENABLED");
}

#else

static volatile bool s_ringing = false;

bool alarm_manager_is_compiled_in(void)
{
    return true;
}

bool alarm_manager_is_enabled(void)
{
    return config_manager_get_alarm_cron_rule_count() > 0;
}

bool alarm_manager_wake_matches_now(void)
{
    cron_rule_t rules[MAX_CRON_RULES];
    int n = config_manager_get_compiled_alarm_cron_rules(rules, MAX_CRON_RULES);
    if (n == 0) {
        return false;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    for (int i = 0; i < n; i++) {
        if (cron_match(&rules[i], &timeinfo)) {
            return true;
        }
    }
    return false;
}

int alarm_manager_seconds_until_next_wake(void)
{
    cron_rule_t rules[MAX_CRON_RULES];
    int n = config_manager_get_compiled_alarm_cron_rules(rules, MAX_CRON_RULES);
    if (n == 0) {
        return CRON_FALLBACK_SEC;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    return cron_seconds_until_next(&timeinfo, rules, n);
}

// Polled by board_hal_play_alarm() between notes/silence chunks - tracks a
// continuous KEY (rotate button) hold locally, entirely within this one
// function, rather than signaling across tasks: the button_task that
// normally handles KEY presses is NOT running during a timer wake
// (main.c dispatches WAKEUP_SOURCE_TIMER straight to deep_sleep_wake_task
// and returns from app_main() without ever creating button_task), so there
// is no separate task to receive a press from here anyway.
static bool key_long_press_requests_stop(void)
{
    static int64_t press_start_us = -1;

    if (BOARD_HAL_ROTATE_KEY == GPIO_NUM_NC) {
        return false;
    }

    bool pressed = (gpio_get_level(BOARD_HAL_ROTATE_KEY) == 0);  // active low, same as button_task
    int64_t now_us = esp_timer_get_time();

    if (!pressed) {
        press_start_us = -1;
        return false;
    }
    if (press_start_us < 0) {
        press_start_us = now_us;
        return false;
    }
    return (now_us - press_start_us) >=
           3000000;  // 3s, matches BOOT's existing long-press threshold
}

bool alarm_manager_is_ringing(void)
{
    return s_ringing;
}

void alarm_manager_run(void)
{
    if (!board_hal_has_speaker()) {
        ESP_LOGW(TAG, "Alarm due, but this board has no speaker - nothing to ring");
        return;
    }

    uint16_t duration_sec = config_manager_get_alarm_ring_duration_sec();
    ESP_LOGI(TAG, "Alarm ringing for up to %u second(s) (long-press KEY to stop early)",
             (unsigned) duration_sec);

    s_ringing = true;
    esp_err_t err =
        board_hal_play_alarm((uint8_t) config_manager_get_chime_volume(),
                             (uint32_t) duration_sec * 1000u, key_long_press_requests_stop);
    s_ringing = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Alarm playback failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Alarm finished ringing");
    }
}

#endif  // CONFIG_ALARM_CLOCK_ENABLED
