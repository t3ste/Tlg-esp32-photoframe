#include "calendar_ics.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "http_fetch.h"
#include "image_processor.h"

static const char *TAG = "calendar_ics";

#define ICS_HTTP_TIMEOUT_MS 10000
#define ICS_MAX_RESPONSE_BYTES (96 * 1024)
#define ICS_LINE_MAX_LEN 600  // a folded SUMMARY can legitimately run long

// Un-folds ICS line-folding in place: a line that starts with a single
// space or tab is a continuation of the previous line (RFC 5545 §3.1) - so
// that continuation's leading whitespace plus the line break before it are
// removed, joining it onto the previous logical line. Unfolding only ever
// removes bytes, so this can safely compact the buffer in place. Every
// remaining (non-folded) line break becomes a single '\n', regardless of
// whether the input used bare LF or CRLF. Returns the new length
// (NUL-terminated at that point too).
static size_t ics_unfold(char *body, size_t len)
{
    size_t src = 0, dst = 0;
    while (src < len) {
        while (src < len && body[src] != '\n' && body[src] != '\r') {
            body[dst++] = body[src++];
        }
        if (src >= len) {
            break;
        }
        if (body[src] == '\r') {
            src++;
        }
        if (src < len && body[src] == '\n') {
            src++;
        }
        if (src < len && (body[src] == ' ' || body[src] == '\t')) {
            src++;  // fold continuation - swallow the break, keep appending
            continue;
        }
        body[dst++] = '\n';
    }
    body[dst] = '\0';
    return dst;
}

// Splits an unfolded property line "NAME[;PARAM=VAL;...]:VALUE" into the
// property name (e.g. "DTSTART") and the raw value after the first colon.
// Returns false if no colon is present (not a property line, or malformed).
static bool split_ics_property(const char *line, size_t line_len, const char **name,
                               size_t *name_len, const char **value, size_t *value_len)
{
    const char *colon = memchr(line, ':', line_len);
    if (!colon) {
        return false;
    }
    const char *semi = memchr(line, ';', (size_t) (colon - line));
    *name = line;
    *name_len = semi ? (size_t) (semi - line) : (size_t) (colon - line);
    *value = colon + 1;
    *value_len = line_len - (size_t) (colon - line) - 1;
    return true;
}

static bool name_is(const char *name, size_t name_len, const char *literal)
{
    size_t lit_len = strlen(literal);
    return name_len == lit_len && strncmp(name, literal, lit_len) == 0;
}

// Parses a "YYYYMMDD" or "YYYYMMDDTHHMMSS[Z]" ICS date-time value into a
// broken-down time. Doesn't validate the date/time fields are in-range
// (e.g. month 13) - the eventual time_t conversion just produces a
// nonsensical-but-non-crashing result for genuinely malformed input,
// matching this project's fail-soft parsing philosophy.
static bool parse_ics_datetime(const char *value, size_t value_len, struct tm *out_tm,
                               bool *out_all_day, bool *out_utc)
{
    if (value_len < 8) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char) value[i])) {
            return false;
        }
    }
    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[2] - '0') * 10 +
                      (value[3] - '0') - 1900;
    out_tm->tm_mon = (value[4] - '0') * 10 + (value[5] - '0') - 1;
    out_tm->tm_mday = (value[6] - '0') * 10 + (value[7] - '0');

    if (value_len == 8) {
        *out_all_day = true;
        *out_utc = false;
        return true;
    }
    *out_all_day = false;
    if (value_len < 15 || value[8] != 'T') {
        return false;
    }
    for (int i = 9; i < 15; i++) {
        if (!isdigit((unsigned char) value[i])) {
            return false;
        }
    }
    out_tm->tm_hour = (value[9] - '0') * 10 + (value[10] - '0');
    out_tm->tm_min = (value[11] - '0') * 10 + (value[12] - '0');
    out_tm->tm_sec = (value[13] - '0') * 10 + (value[14] - '0');
    *out_utc = (value_len >= 16 && value[15] == 'Z');
    return true;
}

// Portable UTC "timegm()" equivalent (Howard Hinnant's days-from-civil
// algorithm - pure integer arithmetic, no libc timezone dependency), used
// for a "Z"-suffixed (explicitly UTC) DTSTART/DTEND so its interpretation
// never depends on the host/device's own local TZ setting - unlike a bare
// or TZID-qualified timestamp, which deliberately does go through
// mktime() (see calendar_ics.h's doc comment on that approximation).
static time_t ics_timegm(const struct tm *tm)
{
    long y = tm->tm_year + 1900;
    int m = tm->tm_mon + 1;
    int d = tm->tm_mday;
    y -= (m <= 2) ? 1 : 0;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned) (y - era * 400);
    unsigned doy = (153 * (unsigned) (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned) d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long) doe - 719468;  // days since 1970-01-01
    return (time_t) days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

static time_t ics_datetime_to_time(const struct tm *tm, bool utc)
{
    if (utc) {
        return ics_timegm(tm);
    }
    struct tm local = *tm;
    return mktime(&local);
}

// Decodes the small set of RFC 5545 §3.3.11 text escapes. Anything else is
// copied through as literal text - image_processor_sanitize_ascii()
// downstream will drop what it can't render anyway.
static void decode_ics_text(const char *in, size_t in_len, char *out, size_t out_len)
{
    size_t o = 0, i = 0;
    while (i < in_len && o + 1 < out_len) {
        if (in[i] == '\\' && i + 1 < in_len) {
            char next = in[i + 1];
            if (next == 'n' || next == 'N') {
                out[o++] = ' ';
                i += 2;
                continue;
            }
            if (next == ',' || next == ';' || next == '\\') {
                out[o++] = next;
                i += 2;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

typedef struct {
    bool have_dtstart;
    struct tm dtstart_tm;
    bool dtstart_utc;
    bool all_day;

    bool have_dtend;
    struct tm dtend_tm;
    bool dtend_utc;

    char raw_summary[ICS_SUMMARY_MAX_LEN];
    bool have_summary;

    bool unsupported_rrule;  // an RRULE present but not FREQ=DAILY/WEEKLY-only
} ics_vevent_state_t;

static void reset_vevent_state(ics_vevent_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

static bool time_overlaps_window(time_t start, time_t end, time_t window_start, time_t window_end)
{
    return start < window_end && end > window_start;
}

static void add_event(ics_event_list_t *out, time_t start, time_t end, bool all_day,
                      const char *summary)
{
    if (out->count >= ICS_MAX_EVENTS) {
        return;
    }
    ics_event_t *e = &out->events[out->count++];
    e->start = start;
    e->end = end;
    e->all_day = all_day;
    strncpy(e->summary, summary && summary[0] != '\0' ? summary : "(untitled)",
            ICS_SUMMARY_MAX_LEN - 1);
    e->summary[ICS_SUMMARY_MAX_LEN - 1] = '\0';
}

// Finalizes one VEVENT block (called at "END:VEVENT"): fills in missing
// DTEND per the documented defaults, and includes the event if it
// overlaps the requested window. RRULE handling (Phase 6) hooks in here
// too, once added - a block flagged unsupported_rrule is skipped outright.
static void finalize_vevent(const ics_vevent_state_t *st, time_t window_start, time_t window_end,
                            ics_event_list_t *out)
{
    if (!st->have_dtstart || st->unsupported_rrule) {
        return;
    }
    time_t start = ics_datetime_to_time(&st->dtstart_tm, st->dtstart_utc);

    time_t end;
    if (st->have_dtend) {
        end = ics_datetime_to_time(&st->dtend_tm, st->dtend_utc);
    } else {
        // Spec is ambiguous when DTEND is absent: a timed event defaults
        // to zero duration; an all-day VALUE=DATE event's single DTSTART
        // day means exactly one day.
        end = st->all_day ? start + 86400 : start;
    }

    if (!time_overlaps_window(start, end, window_start, window_end)) {
        return;
    }
    add_event(out, start, end, st->all_day, st->have_summary ? st->raw_summary : NULL);
}

static int compare_events_by_start(const void *a, const void *b)
{
    const ics_event_t *ea = (const ics_event_t *) a;
    const ics_event_t *eb = (const ics_event_t *) b;
    if (ea->start < eb->start) {
        return -1;
    }
    if (ea->start > eb->start) {
        return 1;
    }
    return 0;
}

esp_err_t calendar_ics_parse(char *body, size_t body_len, time_t window_start, time_t window_end,
                             ics_event_list_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!body || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    body_len = ics_unfold(body, body_len);

    bool in_event = false;
    ics_vevent_state_t st;
    reset_vevent_state(&st);

    const char *pos = body;
    const char *body_end = body + body_len;
    while (pos < body_end) {
        const char *nl = memchr(pos, '\n', (size_t) (body_end - pos));
        const char *line_end = nl ? nl : body_end;
        size_t line_len = (size_t) (line_end - pos);
        if (line_len > ICS_LINE_MAX_LEN) {
            line_len = ICS_LINE_MAX_LEN;  // tolerate, just don't overrun scratch buffers below
        }
        const char *line = pos;
        pos = nl ? nl + 1 : body_end;

        if (line_len == 0) {
            continue;
        }

        if (line_len >= 12 && strncmp(line, "BEGIN:VEVENT", 12) == 0) {
            in_event = true;
            reset_vevent_state(&st);
            continue;
        }
        if (line_len >= 10 && strncmp(line, "END:VEVENT", 10) == 0) {
            if (in_event) {
                finalize_vevent(&st, window_start, window_end, out);
            }
            in_event = false;
            continue;
        }
        if (!in_event) {
            continue;  // VCALENDAR/VTIMEZONE/VALARM/etc. header noise - ignored
        }

        const char *name, *value;
        size_t name_len, value_len;
        if (!split_ics_property(line, line_len, &name, &name_len, &value, &value_len)) {
            continue;
        }

        if (name_is(name, name_len, "DTSTART")) {
            if (parse_ics_datetime(value, value_len, &st.dtstart_tm, &st.all_day,
                                   &st.dtstart_utc)) {
                st.have_dtstart = true;
            }
        } else if (name_is(name, name_len, "DTEND")) {
            bool all_day_unused;
            if (parse_ics_datetime(value, value_len, &st.dtend_tm, &all_day_unused,
                                   &st.dtend_utc)) {
                st.have_dtend = true;
            }
        } else if (name_is(name, name_len, "SUMMARY")) {
            char raw[ICS_LINE_MAX_LEN];
            size_t copy_len = (value_len < sizeof(raw) - 1) ? value_len : sizeof(raw) - 1;
            memcpy(raw, value, copy_len);
            raw[copy_len] = '\0';

            char unescaped[ICS_LINE_MAX_LEN];
            decode_ics_text(raw, copy_len, unescaped, sizeof(unescaped));

            char ascii[ICS_SUMMARY_MAX_LEN];
            image_processor_sanitize_ascii(unescaped, ascii, sizeof(ascii));

            strncpy(st.raw_summary, ascii, ICS_SUMMARY_MAX_LEN - 1);
            st.raw_summary[ICS_SUMMARY_MAX_LEN - 1] = '\0';
            st.have_summary = true;
        } else if (name_is(name, name_len, "RRULE")) {
            // Phase 6 (RRULE-lite) parses this properly; until then, any
            // recurring event is skipped rather than shown once as if it
            // were a one-off (which would be actively misleading).
            st.unsupported_rrule = true;
        }
    }

    if (out->count > 1) {
        qsort(out->events, (size_t) out->count, sizeof(out->events[0]), compare_events_by_start);
    }

    return ESP_OK;  // an empty (no matching events) feed is not an error
}

esp_err_t calendar_ics_fetch(const char *url, int timeout_ms, time_t window_start,
                             time_t window_end, ics_event_list_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char *body = NULL;
    size_t body_len = 0;
    bool truncated = false;
    esp_err_t err = http_fetch_get(url, timeout_ms > 0 ? timeout_ms : ICS_HTTP_TIMEOUT_MS,
                                   ICS_MAX_RESPONSE_BYTES, &body, &body_len, &truncated, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Calendar fetch failed: %s", esp_err_to_name(err));
        return err;
    }
    if (truncated) {
        ESP_LOGW(TAG, "Calendar response truncated at %d bytes - parsing what was captured",
                 ICS_MAX_RESPONSE_BYTES);
    }

    err = calendar_ics_parse(body, body_len, window_start, window_end, out);
    free(body);
    return err;
}
