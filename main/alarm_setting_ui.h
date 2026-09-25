#ifndef ALARM_SETTING_UI_H
#define ALARM_SETTING_UI_H

#include <stdbool.h>
#include <stdint.h>

// Button-driven alarm time-setting UI (docs/ALARMCLOCK_FEASIBILITY.md's
// Phase 3 - only meaningful on a build compiled with
// CONFIG_ALARM_CLOCK_ENABLED, harmless no-ops everywhere else so main.c's
// button_task never needs its own #ifdef). Long-press KEY (>=3s) enters this
// mode from idle, and confirms+exits it back to idle - the same physical
// gesture, disambiguated purely by whether this module is currently active.
// While active, short-press BOOT rolls the hour (0-23, wrapping) and
// short-press KEY rolls the minute in 10-minute steps (0-5) - both support a
// "several fast presses in a row" batch mode that defers the confirmation
// beep until the burst settles, matching the slow-vs-fast distinction
// described in the feasibility doc. An inactivity timeout auto-discards the
// in-progress edit back to a disarmed alarm.

// True while the button-driven setting UI owns BOOT/KEY input. main.c's
// button_task checks this to redirect BOOT's normal long-press (hotspot
// toggle) and KEY's normal short-press (rotation trigger) to this module's
// hour/minute-increment meaning instead, while active.
bool alarm_setting_ui_is_active(void);

// Call once per button_task loop tick (~50ms), regardless of button state -
// drives the inactivity-timeout auto-discard and the deferred "batch of fast
// presses" confirmation beep for whichever counter was mid-burst.
void alarm_setting_ui_tick(void);

// BOOT released after a 50-3000ms press. No-op if not active (button_task
// keeps its own "reset sleep timer" behavior for that case either way).
void alarm_setting_ui_handle_boot_short_press(void);

// BOOT held >=3000ms and released while alarm_setting_ui_is_active() -
// reserved for the future offline voice-enrollment entry gesture; not
// implemented yet (logs and does nothing). button_task calls this INSTEAD
// OF the normal hotspot toggle while setting mode is active, so the two
// can't collide.
void alarm_setting_ui_handle_boot_long_press(void);

// KEY released after a 50-3000ms press. No-op if not active (button_task
// keeps its own rotation-trigger behavior for that case either way).
void alarm_setting_ui_handle_key_short_press(void);

// KEY held >=3000ms and released - always called by button_task regardless
// of alarm_setting_ui_is_active(), since this is what toggles between idle
// and setting mode in the first place. A no-op if the alarm is currently
// ringing (alarm_manager_is_ringing()): that press already stopped the ring
// via alarm_manager_run()'s own check, so it shouldn't also toggle setting
// mode - only reachable when the alarm rang via the always-on active loop,
// since button_task never runs during a deep-sleep timer wake at all.
void alarm_setting_ui_handle_key_long_press(void);

// KEY currently held for `held_ms` so far (not yet released) - call every
// button_task tick while KEY reads pressed, so the "you've held it long
// enough, let go now" confirmation cue can fire while still held rather
// than only after release.
void alarm_setting_ui_on_key_held(uint32_t held_ms);

#endif  // ALARM_SETTING_UI_H
