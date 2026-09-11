#include "calendar_ics.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "http_fetch.h"
#include "image_processor.h"

static const char *TAG = "calendar_ics";

#define ICS_HTTP_TIMEOUT_MS 10000
// A real Google Calendar export (many years of history, recurring series,
// categories/attendees on every VEVENT) was found live to exceed the
// original 96KB cap and get silently truncated mid-parse - confirmed live
// at ~800KB and still growing over time (the user's own calendar). Google's
// "secret address" ICS export has no query parameter to limit it to a date
// range, so there's no way to ask for less data up front. This is
// necessarily a "raise the ceiling" mitigation, not a permanent fix - a
// truly unbounded-size-safe fix needs the parser to work incrementally on
// the HTTP response stream (RFC 5545 line-unfolding included) rather than
// buffering the whole body first, which is a real rewrite deliberately not
// attempted under time pressure here. The response buffer lives in PSRAM
// (http_fetch_get()), which this project has megabytes of headroom in, so
// there's no reason to be stingy with the cap in the meantime - only the
// fixed-size ics_event_list_t output (ICS_MAX_EVENTS entries) is actually
// bounded by anything else.
#define ICS_MAX_RESPONSE_BYTES (2 * 1024 * 1024)
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

// RRULE-lite: FREQ=DAILY/WEEKLY only, optional INTERVAL (default 1) and
// COUNT. Anything else in the rule (BYDAY, EXDATE, UNTIL, BYMONTHDAY,
// WKST, an unrecognized FREQ, ...) makes `supported` false - the caller
// then skips the whole event rather than risk showing a wrong occurrence.
typedef struct {
    bool supported;
    bool weekly;   // false = daily
    int interval;  // >= 1
    bool has_count;
    int count;
} ics_rrule_t;

// Parses one RRULE value ("FREQ=DAILY;INTERVAL=2;COUNT=10"-style,
// ';'-separated "KEY=VALUE" components, order not significant per RFC
// 5545). Bails out (supported = false) the moment any component isn't one
// of the handful this project chose to support - see ics_rrule_t's own
// comment for the rationale.
static bool parse_rrule(const char *value, size_t value_len, ics_rrule_t *out)
{
    memset(out, 0, sizeof(*out));
    out->interval = 1;
    bool have_freq = false;

    size_t i = 0;
    while (i < value_len) {
        size_t part_start = i;
        while (i < value_len && value[i] != ';') {
            i++;
        }
        size_t part_len = i - part_start;
        if (i < value_len) {
            i++;  // skip ';'
        }
        if (part_len == 0) {
            continue;
        }

        const char *part = value + part_start;
        const char *eq = memchr(part, '=', part_len);
        if (!eq) {
            return false;  // malformed component - fail closed
        }
        size_t key_len = (size_t) (eq - part);
        const char *val_ptr = eq + 1;
        size_t val_len = part_len - key_len - 1;

        if (key_len == 4 && strncmp(part, "FREQ", 4) == 0) {
            have_freq = true;
            if (val_len == 5 && strncmp(val_ptr, "DAILY", 5) == 0) {
                out->weekly = false;
            } else if (val_len == 6 && strncmp(val_ptr, "WEEKLY", 6) == 0) {
                out->weekly = true;
            } else {
                return false;  // MONTHLY/YEARLY/HOURLY/... not supported
            }
        } else if (key_len == 8 && strncmp(part, "INTERVAL", 8) == 0) {
            char buf[16];
            size_t n = val_len < sizeof(buf) - 1 ? val_len : sizeof(buf) - 1;
            memcpy(buf, val_ptr, n);
            buf[n] = '\0';
            int iv = atoi(buf);
            out->interval = (iv < 1) ? 1 : iv;
        } else if (key_len == 5 && strncmp(part, "COUNT", 5) == 0) {
            char buf[16];
            size_t n = val_len < sizeof(buf) - 1 ? val_len : sizeof(buf) - 1;
            memcpy(buf, val_ptr, n);
            buf[n] = '\0';
            out->count = atoi(buf);
            out->has_count = true;
        } else {
            // BYDAY, EXDATE, UNTIL, BYMONTHDAY, WKST, BYSETPOS, ... - none
            // of these are safe to just ignore (they'd change which
            // occurrences are actually valid), so the whole rule is
            // unsupported rather than silently wrong.
            return false;
        }
    }

    out->supported = have_freq;
    return out->supported;
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

    bool has_rrule;
    ics_rrule_t rrule;
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

// Expands a supported RRULE (see ics_rrule_t) into whichever occurrences
// overlap [window_start, window_end), via a closed-form jump straight to
// the first candidate rather than walking forward one period at a time
// from DTSTART - important since a long-running recurring event's DTSTART
// can be years in the past, and the lookahead window here is only ever a
// few days wide. Whole-day period arithmetic is done directly in epoch
// seconds (period_secs is always an exact multiple of 86400) rather than
// via calendar-date components - equivalent as long as local-time-of-day
// shifts by exactly 86400s/day, which holds under this project's existing
// fixed-UTC-offset timezone model (no DST transitions to account for).
static void expand_rrule(const ics_rrule_t *rule, time_t base_start, time_t duration, bool all_day,
                         const char *summary, time_t window_start, time_t window_end,
                         ics_event_list_t *out)
{
    long period_secs = (long) (rule->weekly ? 7 : 1) * rule->interval * 86400;
    if (period_secs <= 0) {
        period_secs = 86400;  // defensive - interval is already clamped >= 1 by parse_rrule()
    }

    time_t diff = window_start - base_start;
    long k0 = (diff <= 0) ? 0 : (long) ((diff + period_secs - 1) / period_secs);
    // Back up one occurrence: the ceiling division above finds the first
    // occurrence whose START is >= window_start, but an occurrence that
    // started just before window_start can still overlap it if `duration`
    // carries its end past window_start (e.g. an in-progress multi-day
    // recurring event, "today" falling inside a multi-day trip that
    // started yesterday) - without this, that occurrence is silently
    // skipped even though it's actively ongoing. time_overlaps_window()
    // below correctly rejects this extra candidate if it doesn't actually
    // overlap, so this is always safe to check.
    if (k0 > 0) {
        k0--;
    }

    // Hard safety cap regardless of inputs: a window of at most a few days
    // can never legitimately need more than window_days+1 occurrences at
    // this period granularity, so 8 is a correctness backstop, not a real
    // constraint.
    for (long k = k0; k < k0 + 8; k++) {
        if (rule->has_count && k >= rule->count) {
            break;
        }
        time_t occ_start = base_start + (time_t) (k * period_secs);
        if (occ_start >= window_end) {
            break;  // start only increases with k - nothing further can matter
        }
        time_t occ_end = occ_start + duration;
        if (time_overlaps_window(occ_start, occ_end, window_start, window_end)) {
            add_event(out, occ_start, occ_end, all_day, summary);
        }
    }
}

// Finalizes one VEVENT block (called at "END:VEVENT"): fills in missing
// DTEND per the documented defaults, then either expands a supported RRULE
// (see expand_rrule()) or includes the single event if it overlaps the
// requested window. A present-but-unsupported RRULE means the event is
// skipped entirely, fail-soft - showing it once as if non-recurring would
// be actively misleading.
static void finalize_vevent(const ics_vevent_state_t *st, time_t window_start, time_t window_end,
                            ics_event_list_t *out)
{
    if (!st->have_dtstart) {
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

    const char *summary = st->have_summary ? st->raw_summary : NULL;

    if (st->has_rrule) {
        if (!st->rrule.supported) {
            return;
        }
        expand_rrule(&st->rrule, start, end - start, st->all_day, summary, window_start, window_end,
                     out);
        return;
    }

    if (!time_overlaps_window(start, end, window_start, window_end)) {
        return;
    }
    add_event(out, start, end, st->all_day, summary);
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
    // A VALARM is a sub-block *inside* VEVENT (RFC 5545 §3.6.6) that can
    // carry its own SUMMARY/DESCRIPTION/TRIGGER properties (e.g. a reminder
    // text distinct from the event's own title). Without tracking this
    // separately, a VALARM's SUMMARY line would overwrite the real event's
    // SUMMARY below, since both share the same property name and this
    // parser otherwise only distinguishes "inside VEVENT" from "outside".
    bool in_alarm = false;
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
            in_alarm = false;
            reset_vevent_state(&st);
            continue;
        }
        if (line_len >= 10 && strncmp(line, "END:VEVENT", 10) == 0) {
            if (in_event) {
                finalize_vevent(&st, window_start, window_end, out);
            }
            in_event = false;
            in_alarm = false;
            continue;
        }
        if (line_len >= 12 && strncmp(line, "BEGIN:VALARM", 12) == 0) {
            in_alarm = true;
            continue;
        }
        if (line_len >= 10 && strncmp(line, "END:VALARM", 10) == 0) {
            in_alarm = false;
            continue;
        }
        if (!in_event || in_alarm) {
            continue;  // VCALENDAR/VTIMEZONE header noise, or a VALARM's own properties - ignored
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
            st.has_rrule = true;
            parse_rrule(value, value_len, &st.rrule);  // rrule.supported tells finalize_vevent()
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
