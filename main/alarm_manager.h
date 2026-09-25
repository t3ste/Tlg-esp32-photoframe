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
 */
bool alarm_manager_is_ringing(void);

/**
 * @brief Called by button_task on a KEY (rotate button) press edge. If an alarm
 * is ringing, stops it and returns true: the caller must then ignore this
 * whole press (no rotation on release, no long-press action). Returns false
 * when nothing is ringing. Always false on a build without the Alarm Clock.
 */
bool alarm_manager_key_pressed(void);

/**
 * @brief Called by button_task every iteration with the current KEY level
 * (0 = pressed). Returns true while the key belongs to a press that stopped
 * the alarm - including the release event that ends it - so button_task can
 * skip all further handling of that press. Always false on a build without
 * the Alarm Clock.
 */
bool alarm_manager_key_swallowed(int key_level);

/**
 * @brief Rings the alarm: plays the repeating G4-C5-E5-C5 tone sequence
 * (docs/ALARMCLOCK_FEASIBILITY.md) for the configured ring duration, or
 * until a press of the KEY/rotate button is detected, whichever
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
