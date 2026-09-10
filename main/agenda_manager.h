#ifndef AGENDA_MANAGER_H
#define AGENDA_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Whether the agenda (ToDo + Calendar) full-screen mode has
 * anything to do at all: at least one of ToDo/Calendar is enabled AND at
 * least one agenda cron rule is configured. Cheap/pure - safe to call on
 * every wake before touching WiFi.
 */
bool agenda_manager_is_enabled(void);

/**
 * @brief Does the current wall-clock time match any configured agenda
 * cron rule? Mirrors get_seconds_until_next_wakeup()'s own rule-compiling
 * pattern (utils.c), but checks a match against "now" rather than finding
 * the next one - this is what deep_sleep_wake_main() uses to decide
 * whether THIS wake is an agenda wake (skip the photo pipeline entirely)
 * or a normal rotate wake.
 */
bool agenda_manager_wake_matches_now(void);

/**
 * @brief Seconds from now until the next agenda cron match - same shape
 * and horizon/fallback behavior as get_seconds_until_next_wakeup(), just
 * against the agenda rule set. power_manager_enter_sleep() takes the
 * minimum of this and the rotate schedule's own next-wake to decide the
 * actual sleep duration.
 */
int agenda_manager_seconds_until_next_wake(void);

/**
 * @brief Runs one agenda cycle: fetches whichever of ToDo/Calendar is
 * enabled (each independently fail-soft - one source failing doesn't blank
 * the other), renders the full-screen grid, and displays it. Always a
 * fresh network fetch, no on-device caching, so whatever the configured
 * URL(s) serve at this exact moment is what gets shown.
 *
 * Caller (deep_sleep_wake_main()) is responsible for having WiFi already
 * connected and for going back to sleep afterward - this function neither
 * manages WiFi nor sleeps.
 */
esp_err_t agenda_manager_run(void);

#endif
