#ifndef CHIME_H
#define CHIME_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Play a chime for `event` if all policy checks allow it
 *
 * Checks, in order: board_hal_has_speaker() (no-op on boards without one),
 * the master speaker mode (off / battery+mains / mains-only, gated on
 * board_hal_is_usb_connected() for mains-only), quiet hours, this event's
 * own enable flag, and a per-event/per-boot fire-count cap (see chime.c) -
 * then maps the event to a board_hal_chime_kind_t severity and plays it.
 * Safe to call unconditionally from any hook site; every gate is a no-op
 * skip, never an error.
 */
void chime_play_if_enabled(chime_event_t event);

/**
 * @brief Repeat-until-resolved gate for "actionable" events
 *
 * Call this with the event's current "is the underlying problem still
 * happening" state every time that's checked (once per wake/render).
 * Returns true (and the caller should then call chime_play_if_enabled())
 * up to CHIME_REPEAT_MAX times while `condition_active` stays true, then
 * false until the condition actually clears - at which point the counter
 * resets, so a later recurrence of the same problem repeats again from 1.
 * Only meaningful for CHIME_EVENT_LOW_BATTERY/CRITICAL_ERROR/AGENDA_DUE -
 * see config_manager_get/set_chime_repeat_count()'s comment.
 */
bool chime_repeat_gate(chime_event_t event, bool condition_active);

#ifdef __cplusplus
}
#endif

#endif  // CHIME_H
