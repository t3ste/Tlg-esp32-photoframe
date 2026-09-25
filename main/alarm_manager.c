#include "alarm_manager.h"

#include <time.h>

#include "board_hal.h"
#include "config.h"
#include "config_manager.h"
#include "cron.h"
#include "driver/gpio.h"
#include "esp_attr.h"
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

bool alarm_manager_key_pressed(void)
{
    return false;
}

bool alarm_manager_key_swallowed(int key_level)
{
    (void) key_level;
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

// A short KEY press stops the ringing alarm. Three pieces of state:
//  - s_key_edge: latched by a GPIO interrupt on KEY's falling edge, so even a
//    quick tap between two polls of the ring loop (up to ~300ms apart while a
//    note plays) is not lost.
//  - s_stop_requested: the ring loop should end.
//  - s_swallow_key: the press that stopped the alarm belongs to the alarm, not
//    to button_task - it must trigger neither the image rotation on release
//    nor the >=3s alarm-setting entry. Cleared once the key is released, see
//    alarm_manager_key_swallowed().
// button_task only exists in always-on operation; during a deep-sleep timer
// wake nothing but this module sees the key.
static volatile bool s_key_edge = false;
static volatile bool s_stop_requested = false;
static volatile bool s_swallow_key = false;
static bool s_key_seen_released = false;

static void IRAM_ATTR key_isr(void *arg)
{
    s_key_edge = true;
}

bool alarm_manager_key_pressed(void)
{
    if (!s_ringing) {
        return false;
    }
    s_stop_requested = true;
    s_swallow_key = true;
    return true;
}

bool alarm_manager_key_swallowed(int key_level)
{
    if (!s_swallow_key) {
        return false;
    }
    if (key_level == 1) {
        s_swallow_key = false;  // this is the release of the swallowed press
    }
    return true;
}

// Polled by board_hal_play_alarm() between notes/silence chunks.
static bool key_press_requests_stop(void)
{
    if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC && !s_stop_requested) {
        // Level check as a fallback for the interrupt; only counts a press that
        // followed a release, so a key already held when the ring started
        // doesn't stop it at once.
        int level = gpio_get_level(BOARD_HAL_ROTATE_KEY);
        if (level == 1) {
            s_key_seen_released = true;
        }
        if (s_key_edge || (level == 0 && s_key_seen_released)) {
            s_key_edge = false;
            s_stop_requested = true;
            s_swallow_key = true;
        }
    }
    return s_stop_requested;
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
    ESP_LOGI(TAG, "Alarm ringing for up to %u second(s) (press KEY to stop early)",
             (unsigned) duration_sec);

    s_stop_requested = false;
    s_swallow_key = false;
    s_key_edge = false;
    s_key_seen_released =
        (BOARD_HAL_ROTATE_KEY == GPIO_NUM_NC) || gpio_get_level(BOARD_HAL_ROTATE_KEY) == 1;
    bool isr_added = false;
    if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
        // The ISR service may already be installed by another component.
        esp_err_t isr_err = gpio_install_isr_service(0);
        if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
            gpio_set_intr_type(BOARD_HAL_ROTATE_KEY, GPIO_INTR_NEGEDGE);
            isr_added = gpio_isr_handler_add(BOARD_HAL_ROTATE_KEY, key_isr, NULL) == ESP_OK;
        }
        if (!isr_added) {
            ESP_LOGW(TAG, "KEY interrupt unavailable - falling back to polling the key level");
        }
    }

    s_ringing = true;
    esp_err_t err = board_hal_play_alarm((uint8_t) config_manager_get_chime_volume(),
                                         (uint32_t) duration_sec * 1000u, key_press_requests_stop);
    s_ringing = false;

    if (isr_added) {
        gpio_isr_handler_remove(BOARD_HAL_ROTATE_KEY);
        gpio_set_intr_type(BOARD_HAL_ROTATE_KEY, GPIO_INTR_DISABLE);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Alarm playback failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Alarm finished ringing");
    }
}

#endif  // CONFIG_ALARM_CLOCK_ENABLED
