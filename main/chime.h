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

#ifdef __cplusplus
}
#endif

#endif  // CHIME_H
