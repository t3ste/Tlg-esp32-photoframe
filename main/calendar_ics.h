#ifndef CALENDAR_ICS_H
#define CALENDAR_ICS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"

#define ICS_MAX_EVENTS 24
#define ICS_SUMMARY_MAX_LEN 160

typedef struct {
    time_t start;
    time_t end;  // == start if no duration could be determined
    bool all_day;
    char summary[ICS_SUMMARY_MAX_LEN];
} ics_event_t;

typedef struct {
    int count;
    ics_event_t events[ICS_MAX_EVENTS];
} ics_event_list_t;

/**
 * @brief Fetches `url` (an iCalendar/ICS feed - e.g. a Google Calendar
 * "secret address in iCal format", a plain authless HTTPS GET) and
 * extracts non-recurring VEVENTs overlapping [window_start, window_end).
 *
 * Recurring events (a VEVENT with an RRULE) are not expanded by this pass
 * - see calendar_ics.c's RRULE-lite extension for DAILY/WEEKLY support.
 * An RRULE'd event with none of the supported forms is silently skipped
 * (fail-soft: better to omit one event than show a wrong occurrence).
 *
 * If `cache_path` is non-NULL, this is a conditional GET: `etag_in` (may be
 * NULL/empty) is sent as If-None-Match, and on a 304 reply the body cached
 * at `cache_path` from the last successful 200 is re-parsed instead of
 * re-downloading - the parse itself is still redone every call, since which
 * events fall in [window_start, window_end) shifts day to day even when the
 * feed's content hasn't changed at all. `etag_out`/`etag_out_len` receive
 * the validator to persist for next time (already carries forward `etag_in`
 * if this response didn't repeat an ETag) - the caller owns actually
 * persisting it (see config_manager.h's agenda ETag getters/setters). Pass
 * cache_path/etag_in/etag_out as NULL to skip conditional-GET entirely and
 * always fetch unconditionally.
 *
 * Best-effort: a fetch failure returns an error and leaves *out zeroed
 * (count = 0).
 */
esp_err_t calendar_ics_fetch(const char *url, int timeout_ms, time_t window_start,
                             time_t window_end, const char *cache_path, const char *etag_in,
                             char *etag_out, size_t etag_out_len, ics_event_list_t *out);

/**
 * @brief Pure parsing logic behind calendar_ics_fetch(), split out so it's
 * host-testable without a real HTTP fetch: parses `body` (an ICS feed
 * already in memory, `body_len` bytes - modified in place during
 * line-unfolding) directly. See calendar_ics_fetch() for the extraction
 * rules.
 *
 * A DTSTART/DTEND with no trailing "Z" (bare or TZID-qualified) is
 * interpreted as the device's own local time (via mktime(), the same
 * local-time convention every other wake/schedule computation in this
 * firmware already relies on) rather than real IANA timezone conversion -
 * accurate for the common case where the calendar's own timezone matches
 * the device's configured timezone, approximate otherwise.
 */
esp_err_t calendar_ics_parse(char *body, size_t body_len, time_t window_start, time_t window_end,
                             ics_event_list_t *out);

#endif
