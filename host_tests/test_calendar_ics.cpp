#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

extern "C" {
#include "calendar_ics.h"
}

namespace
{

// All tests run with TZ forced to UTC0, matching test_cron.cpp's own
// precedent - makes a bare/TZID-qualified ("local time") DTSTART directly
// comparable to a "Z"-suffixed (UTC) one via plain mktime(), since with
// TZ=UTC0 local time *is* UTC.
class CalendarIcs : public ::testing::Test
{
   protected:
    void SetUp() override
    {
#if defined(_WIN32)
        _putenv_s("TZ", "UTC0");
        _tzset();
#else
        setenv("TZ", "UTC0", 1);
        tzset();
#endif
    }
};

time_t make_utc(int year, int mon, int day, int hour, int minute, int sec)
{
    struct tm tm {
    };
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

ics_event_list_t parse(const char *ics_text, time_t window_start, time_t window_end)
{
    std::vector<char> buf(ics_text, ics_text + strlen(ics_text) + 1);
    ics_event_list_t out;
    calendar_ics_parse(buf.data(), strlen(ics_text), window_start, window_end, &out);
    return out;
}

}  // namespace

TEST_F(CalendarIcs, SingleUtcEventWithinWindow)
{
    const char *ics =
        "BEGIN:VCALENDAR\n"
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "DTEND:20240115T100000Z\n"
        "SUMMARY:Team Meeting\n"
        "END:VEVENT\n"
        "END:VCALENDAR\n";

    time_t window_start = make_utc(2024, 1, 15, 0, 0, 0);
    time_t window_end = make_utc(2024, 1, 16, 0, 0, 0);
    ics_event_list_t out = parse(ics, window_start, window_end);

    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 15, 9, 0, 0));
    EXPECT_EQ(out.events[0].end, make_utc(2024, 1, 15, 10, 0, 0));
    EXPECT_FALSE(out.events[0].all_day);
    EXPECT_STREQ(out.events[0].summary, "Team Meeting");
}

TEST_F(CalendarIcs, LineFoldingReassemblesSummary)
{
    // The fold point has TWO leading spaces on the continuation line: one
    // natural word-separator (kept) and one fold-indicator (removed by
    // unfolding, per RFC 5545) - so the reassembled text reads with a
    // single space, not glued or double-spaced.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "SUMMARY:This is a long summary that\n"
        "  continues on the next line\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "This is a long summary that continues on the next line");
}

TEST_F(CalendarIcs, MissingDtendAllDayDefaultsToOneDay)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART;VALUE=DATE:20240115\n"
        "SUMMARY:Holiday\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_TRUE(out.events[0].all_day);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 15, 0, 0, 0));
    EXPECT_EQ(out.events[0].end, make_utc(2024, 1, 16, 0, 0, 0));
}

TEST_F(CalendarIcs, MissingDtendTimedDefaultsToZeroDuration)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "SUMMARY:Quick call\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.events[0].start, out.events[0].end);
}

TEST_F(CalendarIcs, TzidQualifiedTimestampParsed)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART;TZID=Europe/Berlin:20240115T090000\n"
        "SUMMARY:Local meeting\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    // With TZ forced to UTC0 for this test process, mktime()'s local-time
    // interpretation coincides numerically with UTC.
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 15, 9, 0, 0));
}

TEST_F(CalendarIcs, NonVeventBlocksIgnored)
{
    const char *ics =
        "BEGIN:VCALENDAR\n"
        "BEGIN:VTIMEZONE\n"
        "TZID:Europe/Berlin\n"
        "BEGIN:STANDARD\n"
        "DTSTART:19701025T030000\n"
        "END:STANDARD\n"
        "END:VTIMEZONE\n"
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "SUMMARY:Real event\n"
        "BEGIN:VALARM\n"
        "TRIGGER:-PT15M\n"
        "END:VALARM\n"
        "END:VEVENT\n"
        "END:VCALENDAR\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "Real event");
}

// A VALARM (RFC 5545 §3.6.6) is a sub-block nested inside VEVENT that can
// carry its own SUMMARY (e.g. an EMAIL-action alarm's message subject,
// distinct from the event's own title). Without tracking "inside VALARM"
// separately from "inside VEVENT", the alarm's SUMMARY line would overwrite
// the event's real one, since both share the same property name.
TEST_F(CalendarIcs, ValarmSummaryDoesNotOverwriteEventSummary)
{
    const char *ics =
        "BEGIN:VCALENDAR\n"
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "SUMMARY:Real event title\n"
        "BEGIN:VALARM\n"
        "ACTION:EMAIL\n"
        "TRIGGER:-P1D\n"
        "SUMMARY:Reminder email subject\n"
        "DESCRIPTION:Don't forget tomorrow\n"
        "END:VALARM\n"
        "END:VEVENT\n"
        "END:VCALENDAR\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "Real event title");
}

TEST_F(CalendarIcs, EventFullyOutsideWindowExcluded)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240110T090000Z\n"
        "DTEND:20240110T100000Z\n"
        "SUMMARY:Too early\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    EXPECT_EQ(out.count, 0);
}

TEST_F(CalendarIcs, EventSpanningWindowStartIncluded)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240114T220000Z\n"
        "DTEND:20240115T020000Z\n"
        "SUMMARY:Overnight\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "Overnight");
}

TEST_F(CalendarIcs, MissingSummaryHandledGracefully)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "(untitled)");
}

TEST_F(CalendarIcs, DailyRruleExpandsWithinWindow)
{
    // DTSTART is well before the window; a 3-day window should surface
    // exactly the occurrences that land inside it.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T090000Z\n"
        "DTEND:20240101T093000Z\n"
        "SUMMARY:Daily standup\n"
        "RRULE:FREQ=DAILY\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 18, 0, 0, 0));
    ASSERT_EQ(out.count, 3);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 15, 9, 0, 0));
    EXPECT_EQ(out.events[0].end, make_utc(2024, 1, 15, 9, 30, 0));
    EXPECT_EQ(out.events[1].start, make_utc(2024, 1, 16, 9, 0, 0));
    EXPECT_EQ(out.events[2].start, make_utc(2024, 1, 17, 9, 0, 0));
    for (int i = 0; i < out.count; i++) {
        EXPECT_STREQ(out.events[i].summary, "Daily standup");
    }
}

// Regression test for a real bug: expand_rrule()'s occurrence-count safety
// cap used to be a fixed "8", correct only as long as every caller stuck to
// the original 1-3 day rotation/agenda lookahead window. A wider window
// (e.g. the 30-day expansion agenda_manager.c's extra ICS sources use to
// build their flat cache, so large files don't need re-parsing on every
// wake) would have silently truncated a DAILY recurrence to its first ~8
// occurrences and dropped the rest.
TEST_F(CalendarIcs, DailyRruleExpandsAcrossWideThirtyDayWindow)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T090000Z\n"
        "DTEND:20240101T093000Z\n"
        "SUMMARY:Daily reminder\n"
        "RRULE:FREQ=DAILY\n"
        "END:VEVENT\n";

    time_t window_start = make_utc(2024, 1, 15, 0, 0, 0);
    time_t window_end = window_start + 30 * 86400;  // 30-day expansion window
    ics_event_list_t out = parse(ics, window_start, window_end);
    // ICS_MAX_EVENTS caps the list at 24, so a genuinely daily event across
    // 30 days hits that cap rather than reaching all 30 - the point of this
    // test is that it reaches the cap (not silently stopping at ~8).
    EXPECT_EQ(out.count, 24);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 15, 9, 0, 0));
    EXPECT_EQ(out.events[23].start, make_utc(2024, 2, 7, 9, 0, 0));
}

TEST_F(CalendarIcs, DailyRruleClosedFormJumpFromFarPastDtstart)
{
    // DTSTART is years before the window - this specifically exercises the
    // closed-form jump to the first in-window occurrence rather than a
    // slow (or wrong) day-by-day walk from DTSTART.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20200101T080000Z\n"
        "SUMMARY:Ancient daily reminder\n"
        "RRULE:FREQ=DAILY\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 15, 8, 0, 0));
}

TEST_F(CalendarIcs, WeeklyRruleExpandsOnCorrectDays)
{
    // DTSTART is a Monday (2024-01-01); weekly occurrences should land on
    // Mondays only, one per 7-day period.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T100000Z\n"
        "SUMMARY:Weekly sync\n"
        "RRULE:FREQ=WEEKLY\n"
        "END:VEVENT\n";

    // Window covers 2024-01-08 (Mon) through 2024-01-21 (Sun) - two weekly
    // occurrences (Jan 8 and Jan 15) should fall inside it.
    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 8, 0, 0, 0), make_utc(2024, 1, 22, 0, 0, 0));
    ASSERT_EQ(out.count, 2);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 8, 10, 0, 0));
    EXPECT_EQ(out.events[1].start, make_utc(2024, 1, 15, 10, 0, 0));
}

TEST_F(CalendarIcs, MultiDayWeeklyOccurrenceInProgressAtWindowStartIncluded)
{
    // DTSTART is a Monday, each occurrence spans 2 days (09:00 Mon to 09:00
    // Wed). The window starts mid-occurrence (Tuesday), after the first
    // occurrence's own start but before its end - the closed-form k0 jump
    // (which only finds the first occurrence whose START is >= window
    // start) must back up one period to still find this in-progress
    // occurrence, not silently skip it.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T090000Z\n"
        "DTEND:20240103T090000Z\n"
        "SUMMARY:Multi-day trip\n"
        "RRULE:FREQ=WEEKLY\n"
        "END:VEVENT\n";

    ics_event_list_t out = parse(ics, make_utc(2024, 1, 2, 0, 0, 0), make_utc(2024, 1, 4, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 1, 9, 0, 0));
    EXPECT_EQ(out.events[0].end, make_utc(2024, 1, 3, 9, 0, 0));
}

TEST_F(CalendarIcs, IntervalTwoLandsOnCorrectBoundary)
{
    // DTSTART 2024-01-01, INTERVAL=2 (every other day): occurrences on
    // Jan 1, 3, 5, 7, 9, 11, 13, 15, 17... A window covering just Jan 16
    // should NOT include the Jan-15 or Jan-17 occurrences (off-by-one check
    // on the ceiling-division math that picks the first in-window k).
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T120000Z\n"
        "SUMMARY:Every other day\n"
        "RRULE:FREQ=DAILY;INTERVAL=2\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 16, 0, 0, 0), make_utc(2024, 1, 17, 0, 0, 0));
    EXPECT_EQ(out.count, 0);

    ics_event_list_t out2 =
        parse(ics, make_utc(2024, 1, 17, 0, 0, 0), make_utc(2024, 1, 18, 0, 0, 0));
    ASSERT_EQ(out2.count, 1);
    EXPECT_EQ(out2.events[0].start, make_utc(2024, 1, 17, 12, 0, 0));
}

TEST_F(CalendarIcs, CountExcludesOccurrenceBeyondLimit)
{
    // DTSTART 2024-01-01, DAILY, COUNT=3 -> only Jan 1/2/3 ever occur. A
    // window covering Jan 10 should see nothing, even though the naive
    // (COUNT-less) pattern would otherwise land there.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T090000Z\n"
        "SUMMARY:Three-day trial\n"
        "RRULE:FREQ=DAILY;COUNT=3\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 10, 0, 0, 0), make_utc(2024, 1, 13, 0, 0, 0));
    EXPECT_EQ(out.count, 0);

    // But the window covering the original 3 days should still see them.
    ics_event_list_t out2 =
        parse(ics, make_utc(2024, 1, 1, 0, 0, 0), make_utc(2024, 1, 4, 0, 0, 0));
    EXPECT_EQ(out2.count, 3);
}

TEST_F(CalendarIcs, RruleUntilBoundsActiveSeriesMidWindow)
{
    // DTSTART 2024-01-01 (Mon), WEEKLY, UNTIL 2024-01-15 (inclusive per RFC
    // 5545) - occurrences on Jan 1/8/15 are valid, Jan 22 is not. A window
    // spanning Jan 1 through Jan 29 should see exactly the first three.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T100000Z\n"
        "SUMMARY:Limited-run class\n"
        "RRULE:FREQ=WEEKLY;UNTIL=20240115T100000Z\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 1, 0, 0, 0), make_utc(2024, 1, 29, 0, 0, 0));
    ASSERT_EQ(out.count, 3);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 1, 10, 0, 0));
    EXPECT_EQ(out.events[1].start, make_utc(2024, 1, 8, 10, 0, 0));
    EXPECT_EQ(out.events[2].start, make_utc(2024, 1, 15, 10, 0, 0));
}

TEST_F(CalendarIcs, RruleUntilInPastYieldsNoOccurrencesButStaysSupported)
{
    // UNTIL entirely before the query window - a series that has simply
    // ended, not an unsupported rule. Distinguishes this from the old
    // behavior (before UNTIL was supported) where the whole rule, and any
    // still-relevant part of it, would have been rejected outright by
    // fall-through to the generic unsupported-component branch.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20200101T100000Z\n"
        "SUMMARY:Long-ended series\n"
        "RRULE:FREQ=WEEKLY;UNTIL=20200201T100000Z\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 1, 0, 0, 0), make_utc(2024, 1, 8, 0, 0, 0));
    EXPECT_EQ(out.count, 0);
}

TEST_F(CalendarIcs, RruleWithBydayUnsupportedSkippedEntirely)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T090000Z\n"
        "SUMMARY:Weekdays only\n"
        "RRULE:FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR\n"
        "END:VEVENT\n";

    ics_event_list_t out = parse(ics, make_utc(2024, 1, 1, 0, 0, 0), make_utc(2024, 1, 8, 0, 0, 0));
    EXPECT_EQ(out.count, 0);
}

TEST_F(CalendarIcs, RruleWithSingleBydayMatchingDtstartWeekdayAccepted)
{
    // DTSTART is a Monday (2024-01-01); a single BYDAY=MO matches it
    // exactly - the common case real calendar apps (Google/Outlook/Apple)
    // emit for a plain "repeat weekly" event even with no other special
    // pattern, so this is now accepted instead of failing closed like a
    // multi-value BYDAY does.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T100000Z\n"
        "SUMMARY:Weekly sync\n"
        "RRULE:FREQ=WEEKLY;BYDAY=MO\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 8, 0, 0, 0), make_utc(2024, 1, 22, 0, 0, 0));
    ASSERT_EQ(out.count, 2);
    EXPECT_EQ(out.events[0].start, make_utc(2024, 1, 8, 10, 0, 0));
    EXPECT_EQ(out.events[1].start, make_utc(2024, 1, 15, 10, 0, 0));
}

TEST_F(CalendarIcs, RruleWithWkstAndSingleBydayAccepted)
{
    // Real-world regression: an all-day weekly event exported from a real
    // calendar app (aCalendar, via Android) - DTSTART is a Wednesday
    // (2026-09-16), and the RRULE includes WKST alongside a single BYDAY.
    // WKST alone used to reject the whole rule even after BYDAY itself was
    // accepted, since it fell through to the generic unsupported-component
    // branch - confirmed live against the reporting user's actual .ics
    // export.
    const char *ics =
        "BEGIN:VEVENT\n"
        "SUMMARY:16:30 Lia Sport\n"
        "DTSTART;VALUE=DATE:20260916\n"
        "DTEND;VALUE=DATE:20260917\n"
        "RRULE:FREQ=WEEKLY;WKST=MO;BYDAY=WE\n"
        "END:VEVENT\n";

    // Window covers 2026-09-16 (Wed) through 2026-09-30 inclusive - three
    // Wednesdays (16th, 23rd, 30th).
    ics_event_list_t out =
        parse(ics, make_utc(2026, 9, 16, 0, 0, 0), make_utc(2026, 10, 1, 0, 0, 0));
    ASSERT_EQ(out.count, 3);
    EXPECT_EQ(out.events[0].start, make_utc(2026, 9, 16, 0, 0, 0));
    EXPECT_EQ(out.events[1].start, make_utc(2026, 9, 23, 0, 0, 0));
    EXPECT_EQ(out.events[2].start, make_utc(2026, 9, 30, 0, 0, 0));
}

TEST_F(CalendarIcs, RruleWithSingleBydayMismatchingDtstartWeekdaySkipped)
{
    // DTSTART is a Monday, but BYDAY names Wednesday instead - a genuinely
    // different pattern (occurrences that don't follow DTSTART's own
    // weekday) this project's simple weekly-with-interval model can't
    // represent, so the whole event is still skipped entirely (fail
    // closed), same as any other unsupported RRULE.
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T100000Z\n"
        "SUMMARY:Mismatched weekday\n"
        "RRULE:FREQ=WEEKLY;BYDAY=WE\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 8, 0, 0, 0), make_utc(2024, 1, 22, 0, 0, 0));
    EXPECT_EQ(out.count, 0);
}

TEST_F(CalendarIcs, RruleWithUnsupportedFreqSkippedEntirely)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240101T090000Z\n"
        "SUMMARY:Monthly report\n"
        "RRULE:FREQ=MONTHLY\n"
        "END:VEVENT\n";

    ics_event_list_t out = parse(ics, make_utc(2024, 1, 1, 0, 0, 0), make_utc(2024, 2, 1, 0, 0, 0));
    EXPECT_EQ(out.count, 0);
}

TEST_F(CalendarIcs, CrlfLineEndingsTolerated)
{
    const char *ics =
        "BEGIN:VEVENT\r\n"
        "DTSTART:20240115T090000Z\r\n"
        "SUMMARY:CRLF event\r\n"
        "END:VEVENT\r\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "CRLF event");
}

TEST_F(CalendarIcs, MultipleEventsSortedByStart)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T150000Z\n"
        "SUMMARY:Later\n"
        "END:VEVENT\n"
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "SUMMARY:Earlier\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 2);
    EXPECT_STREQ(out.events[0].summary, "Earlier");
    EXPECT_STREQ(out.events[1].summary, "Later");
}

TEST_F(CalendarIcs, TextEscapesDecoded)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "SUMMARY:Comma\\, semicolon\\; and backslash\\\\ here\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "Comma, semicolon; and backslash\\ here");
}

TEST_F(CalendarIcs, NoMatchingEventsIsNotAnError)
{
    const char *ics = "BEGIN:VCALENDAR\nEND:VCALENDAR\n";
    ics_event_list_t out;
    std::vector<char> buf(ics, ics + strlen(ics) + 1);
    esp_err_t err = calendar_ics_parse(buf.data(), strlen(ics), make_utc(2024, 1, 15, 0, 0, 0),
                                       make_utc(2024, 1, 16, 0, 0, 0), &out);
    EXPECT_EQ(err, ESP_OK);
    EXPECT_EQ(out.count, 0);
}

TEST_F(CalendarIcs, NullBodyIsInvalidArg)
{
    ics_event_list_t out;
    esp_err_t err = calendar_ics_parse(nullptr, 0, 0, 0, &out);
    EXPECT_EQ(err, ESP_ERR_INVALID_ARG);
    EXPECT_EQ(out.count, 0);
}

TEST_F(CalendarIcs, NullOutIsInvalidArg)
{
    char body[] = "x";
    esp_err_t err = calendar_ics_parse(body, 1, 0, 0, nullptr);
    EXPECT_EQ(err, ESP_ERR_INVALID_ARG);
}

// calendar_ics_has_upcoming_event() - used by agenda_manager.c's extra ICS
// sources (holidays/school-holidays/etc.) to decide whether to inject a
// "this source is stale, please update it" reminder, since those three
// sources never refresh themselves.
TEST_F(CalendarIcs, HasUpcomingEventEmptyListIsFalse)
{
    ics_event_list_t list;
    memset(&list, 0, sizeof(list));
    EXPECT_FALSE(calendar_ics_has_upcoming_event(&list, make_utc(2024, 1, 15, 0, 0, 0)));
}

TEST_F(CalendarIcs, HasUpcomingEventAllInPastIsFalse)
{
    ics_event_list_t list;
    memset(&list, 0, sizeof(list));
    list.count = 2;
    list.events[0].start = make_utc(2020, 1, 1, 0, 0, 0);
    list.events[0].end = make_utc(2020, 1, 2, 0, 0, 0);
    list.events[1].start = make_utc(2021, 6, 1, 0, 0, 0);
    list.events[1].end = make_utc(2021, 6, 2, 0, 0, 0);
    EXPECT_FALSE(calendar_ics_has_upcoming_event(&list, make_utc(2024, 1, 15, 0, 0, 0)));
}

TEST_F(CalendarIcs, HasUpcomingEventOneFutureIsTrue)
{
    ics_event_list_t list;
    memset(&list, 0, sizeof(list));
    list.count = 2;
    list.events[0].start = make_utc(2020, 1, 1, 0, 0, 0);
    list.events[0].end = make_utc(2020, 1, 2, 0, 0, 0);
    list.events[1].start = make_utc(2030, 1, 1, 0, 0, 0);
    list.events[1].end = make_utc(2030, 1, 2, 0, 0, 0);
    EXPECT_TRUE(calendar_ics_has_upcoming_event(&list, make_utc(2024, 1, 15, 0, 0, 0)));
}

TEST_F(CalendarIcs, HasUpcomingEventEndExactlyAtNowIsFalse)
{
    // end > now is the exact rule (event.end == now is treated as fully
    // passed, matching event_touches_day()'s own end-is-exclusive
    // convention in agenda_renderer.c).
    ics_event_list_t list;
    memset(&list, 0, sizeof(list));
    list.count = 1;
    time_t now = make_utc(2024, 1, 15, 12, 0, 0);
    list.events[0].start = make_utc(2024, 1, 15, 11, 0, 0);
    list.events[0].end = now;
    EXPECT_FALSE(calendar_ics_has_upcoming_event(&list, now));
}

TEST_F(CalendarIcs, HasUpcomingEventNullListIsFalse)
{
    EXPECT_FALSE(calendar_ics_has_upcoming_event(nullptr, make_utc(2024, 1, 15, 0, 0, 0)));
}

// calendar_ics_write_expanded_cache()/calendar_ics_read_expanded_cache() -
// the flat, already-expanded cache agenda_manager.c's extra ICS sources use
// to avoid re-parsing a large raw .ics file on every agenda wake.
class CalendarIcsExpandedCache : public CalendarIcs
{
   protected:
    const char *path = "test_expanded_cache_tmp.txt";

    void TearDown() override
    {
        remove(path);
    }
};

TEST_F(CalendarIcsExpandedCache, RoundTripPreservesFields)
{
    ics_event_list_t list;
    memset(&list, 0, sizeof(list));
    list.count = 2;
    list.events[0].start = make_utc(2024, 3, 1, 9, 0, 0);
    list.events[0].end = make_utc(2024, 3, 1, 10, 0, 0);
    list.events[0].all_day = false;
    strncpy(list.events[0].summary, "Team Meeting", sizeof(list.events[0].summary) - 1);
    list.events[1].start = make_utc(2024, 3, 2, 0, 0, 0);
    list.events[1].end = make_utc(2024, 3, 3, 0, 0, 0);
    list.events[1].all_day = true;
    strncpy(list.events[1].summary, "Public Holiday", sizeof(list.events[1].summary) - 1);

    ASSERT_EQ(calendar_ics_write_expanded_cache(path, &list), ESP_OK);

    ics_event_list_t out;
    ASSERT_EQ(calendar_ics_read_expanded_cache(path, &out), ESP_OK);
    ASSERT_EQ(out.count, 2);
    EXPECT_EQ(out.events[0].start, list.events[0].start);
    EXPECT_EQ(out.events[0].end, list.events[0].end);
    EXPECT_FALSE(out.events[0].all_day);
    EXPECT_STREQ(out.events[0].summary, "Team Meeting");
    EXPECT_EQ(out.events[1].start, list.events[1].start);
    EXPECT_EQ(out.events[1].end, list.events[1].end);
    EXPECT_TRUE(out.events[1].all_day);
    EXPECT_STREQ(out.events[1].summary, "Public Holiday");
}

TEST_F(CalendarIcsExpandedCache, EmbeddedTabAndNewlineSanitizedNotCorrupting)
{
    ics_event_list_t list;
    memset(&list, 0, sizeof(list));
    list.count = 1;
    list.events[0].start = make_utc(2024, 3, 1, 0, 0, 0);
    list.events[0].end = make_utc(2024, 3, 2, 0, 0, 0);
    list.events[0].all_day = true;
    strncpy(list.events[0].summary, "Weird\tTitle\nWith Newline",
            sizeof(list.events[0].summary) - 1);

    ASSERT_EQ(calendar_ics_write_expanded_cache(path, &list), ESP_OK);

    ics_event_list_t out;
    ASSERT_EQ(calendar_ics_read_expanded_cache(path, &out), ESP_OK);
    // A raw embedded tab/newline would otherwise be mistaken for a field
    // separator or a second line - write-side sanitization replaces them
    // with spaces, so this must round-trip as exactly one event.
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "Weird Title With Newline");
}

TEST_F(CalendarIcsExpandedCache, MissingFileIsNotFound)
{
    ics_event_list_t out;
    EXPECT_EQ(calendar_ics_read_expanded_cache("does_not_exist.txt", &out), ESP_ERR_NOT_FOUND);
    EXPECT_EQ(out.count, 0);
}

TEST_F(CalendarIcsExpandedCache, MalformedLineSkippedFailSoft)
{
    FILE *fp = fopen(path, "wb");
    ASSERT_NE(fp, nullptr);
    fprintf(fp, "not a valid line\n");
    fprintf(fp, "%lld\t%lld\t%d\t%s\n", (long long) make_utc(2024, 3, 1, 0, 0, 0),
            (long long) make_utc(2024, 3, 2, 0, 0, 0), 1, "Valid Entry");
    fclose(fp);

    ics_event_list_t out;
    ASSERT_EQ(calendar_ics_read_expanded_cache(path, &out), ESP_OK);
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.events[0].summary, "Valid Entry");
}
