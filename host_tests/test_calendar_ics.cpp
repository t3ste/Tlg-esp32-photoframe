#include <gtest/gtest.h>

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

TEST_F(CalendarIcs, RecurringEventWithRruleSkippedUntilPhase6)
{
    const char *ics =
        "BEGIN:VEVENT\n"
        "DTSTART:20240115T090000Z\n"
        "SUMMARY:Daily standup\n"
        "RRULE:FREQ=DAILY\n"
        "END:VEVENT\n";

    ics_event_list_t out =
        parse(ics, make_utc(2024, 1, 15, 0, 0, 0), make_utc(2024, 1, 16, 0, 0, 0));
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
