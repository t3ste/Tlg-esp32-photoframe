#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <stdbool.h>

/**
 * @brief Whether this firmware build was compiled with the alarm clock
 * feature at all (CONFIG_ALARM_CLOCK_ENABLED - see main/Kconfig,
 * `build.py --alarmclock`). Reported to the Web UI as "alarm_clock_available"
 * so it can hide the whole Alarm settings tab on a build without it, the
 * same way "chime_speaker_available"/"climate_sensor_available" already
 * work for hardware-dependent features. Always callable regardless of build
 * configuration - returns false as a plain no-op on a build without the
 * feature, exactly like board_hal_has_speaker() does on boards without a
 * speaker.
 */
bool alarm_manager_is_compiled_in(void);

/**
 * @brief Whether at least one alarm schedule is currently configured.
 * Mirrors agenda_manager_is_enabled()'s shape - cheap/pure, safe to call on
 * every wake before touching WiFi. There is no separate on/off toggle: an
 * alarm is "armed" purely by having at least one cron rule, and "permanently
 * disabled" purely by having none - matching how the physical button UI
 * (docs/ALARMCLOCK_FEASIBILITY.md) discards down to an empty schedule
 * instead of flipping a separate flag.
 */
bool alarm_manager_is_enabled(void);

/**
 * @brief Does the current wall-clock time match any configured alarm cron
 * rule? Mirrors agenda_manager_wake_matches_now()'s exact shape - this is
 * what deep_sleep_wake_main() uses to decide whether a timer wake should
 * ring the alarm.
 */
bool alarm_manager_wake_matches_now(void);

/**
 * @brief Seconds from now until the next configured alarm cron rule would
 * match, for power_manager.c's next-wake-time calculation. Mirrors
 * agenda_manager_seconds_until_next_wake()'s exact shape. Returns a large
 * fallback value (never the soonest candidate) when no alarm is configured.
 */
int alarm_manager_seconds_until_next_wake(void);

/**
 * @brief True while alarm_manager_run() is actively ringing.
 *
 * Used by the button-driven alarm-setting UI (docs/ALARMCLOCK_FEASIBILITY.md's
 * Phase 3) to avoid double-handling a long KEY press that already stopped the
 * ring via alarm_manager_run()'s own should_stop check - only relevant at all
 * when the alarm rings via the always-on active loop (power_manager.c's
 * rotation_timer_task), since button_task never runs during a deep-sleep
 * timer wake in the first place, so there's no button_task to double-handle
 * anything in that case.
 */
bool alarm_manager_is_ringing(void);

/**
 * @brief Rings the alarm: plays the repeating G4-C5-E5-C5 tone sequence
 * (docs/ALARMCLOCK_FEASIBILITY.md) for the configured ring duration, or
 * until a long (>=3s) press of the KEY/rotate button is detected, whichever
 * comes first. Blocks for the whole duration. Deliberately touches nothing
 * network/rotation/agenda-related - the caller (main.c's deep-sleep wake
 * dispatch) is responsible for skipping WiFi/rotation/agenda entirely for
 * an alarm wake; this function only owns the ringing itself.
 *
 * Must only be called after alarm_manager_wake_matches_now() returned true
 * for this wake. A no-op (logs and returns immediately) if this build has
 * no speaker or wasn't compiled with the feature at all.
 */
void alarm_manager_run(void);

#endif  // ALARM_MANAGER_H
