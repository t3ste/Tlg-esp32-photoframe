# Calendar RRULE support (Agenda Mode)

Which recurring-event (`RRULE`) values this fork's ICS parser (`main/calendar_ics.c`,
`parse_rrule()`/`expand_rrule()`) understands, and why the rest are rejected. An event whose
`RRULE` isn't fully supported is **skipped entirely** (fail-closed) rather than shown once as if
it weren't recurring - showing a wrong/misleading occurrence was judged worse than showing nothing
for that event.

This intentionally covers only a useful subset of RFC 5545, not the full spec - see
[Not planned](#not-planned-and-why) for the reasoning behind each gap.

## Supported

| `RRULE` part | Support | Notes |
|---|---|---|
| `FREQ=DAILY` | ✅ Full | |
| `FREQ=WEEKLY` | ✅ Full | |
| `INTERVAL=N` | ✅ Full | "every N days/weeks" |
| `COUNT=N` | ✅ Full | Stops after N occurrences |
| `UNTIL=<date>` | ✅ Full | Inclusive end bound - an occurrence starting after this is excluded. Accepts a bare date (`20260101`) or a date-time, with or without a trailing `Z` |
| `BYDAY=<single day>` | ✅ Conditional | Only when it names the **same weekday `DTSTART` already falls on** (e.g. `DTSTART` is a Wednesday, `BYDAY=WE`) - see below for why |
| `WKST=<day>` | ✅ Ignored (harmlessly) | Parsed but has no effect - see below for why that's safe |

A plain `FREQ=WEEKLY` with no `BYDAY` at all also works and always has - it's the presence of a
*single* `BYDAY` that used to be rejected until this was fixed.

### Why a single `BYDAY` is safe, but only when it matches `DTSTART`

Real calendar apps (Google Calendar, Outlook, Apple Calendar) almost always emit
`FREQ=WEEKLY;BYDAY=<day>` for a plain "repeat weekly" event, even one with no special pattern -
this is standard, not an edge case. When that single `BYDAY` names the same weekday `DTSTART`
already falls on, it's semantically identical to plain `FREQ=WEEKLY` (which already worked) - the
generated occurrences are exactly the same either way. If `BYDAY` names a *different* weekday than
`DTSTART`, that describes a genuinely different pattern (the parser can't know which one the
calendar author actually intended), so it still fails closed like any unsupported rule.

### Why `WKST` can just be ignored

`WKST` (week-start-day) only changes which occurrences are valid for patterns this parser doesn't
support anyway - `BYSETPOS`, `BYWEEKNO`, or multiple `BYDAY` values combined with `INTERVAL>1`. With
at most one `BYDAY` value (the only case ever accepted), `WKST` has no effect on the actual result,
so rejecting the whole rule over it would have been overly strict. (Confirmed live 2026-09-16: a
real calendar export's plain weekly Wednesday event, `FREQ=WEEKLY;WKST=MO;BYDAY=WE`, was being
dropped by `WKST` alone, even after `BYDAY` itself was accepted.)

## Not supported (and why)

| `RRULE` part | Why not | Could it be added later? |
|---|---|---|
| `FREQ=MONTHLY` | Months have variable length (28-31 days) - the current expander computes occurrences via fixed-seconds arithmetic (`period_secs = days * 86400`), which only works for DAILY/WEEKLY. A real calendar-arithmetic expansion (increment `tm_mon`, re-normalize) would be needed | Yes, but needs a separate expansion path, not a small tweak |
| `FREQ=YEARLY` | Same problem, worse (leap years: 365 vs. 366 days) | Yes - combined with `BYMONTHDAY`+`BYMONTH` this is the classic birthday/anniversary pattern, and was in fact the most common rejected rule found in real-world testing. Meaningfully more work than the `WKST`/`BYDAY`/`UNTIL` fixes, since it's a new expansion strategy rather than a new accepted parameter |
| `FREQ=HOURLY`/`MINUTELY`/`SECONDLY` | Not meaningful for a display that wakes at most every few minutes | No - out of scope for this project regardless of implementation cost |
| Multiple `BYDAY` values (e.g. `BYDAY=MO,WE,FR`) | A genuinely different pattern (multiple weekdays per week) - this parser's model represents "one occurrence every N periods," not "occurrences on several specific weekdays within each period" | Possible in principle, but a real architecture change (would need to generate several candidate weekdays per period and merge them) |
| `BYDAY` with an ordinal prefix (e.g. `BYDAY=1MO`, `BYDAY=-1FR` - "1st Monday", "last Friday") | Different meaning entirely ("Nth weekday of the period," used with `BYSETPOS` or `FREQ=MONTHLY`/`YEARLY`) - the current single-letter-code parser doesn't recognize the numeric prefix and fails closed on the malformed-looking value | Only meaningful together with `BYSETPOS`/`MONTHLY`/`YEARLY` support |
| `BYMONTHDAY`, `BYMONTH`, `BYWEEKNO`, `BYYEARDAY` | Only meaningful combined with `MONTHLY`/`YEARLY` (see above) | Same as `FREQ=YEARLY` above |
| `BYSETPOS` (e.g. "the 2nd Tuesday of the month") | Needs the full monthly/yearly occurrence set generated first, then picked by position - a materially different algorithm | Possible, but the most complex of the unsupported list; not seen in real-world testing so far |
| `EXDATE` (exclude specific dates from an otherwise-recurring series, e.g. "except this one Wednesday for a holiday") | Would need to parse and store a list of excluded dates (potentially spanning multiple `EXDATE` lines) and check every generated occurrence against it - real complexity, and this project's fixed-size buffers make an open-ended exception list awkward | Possible, meaningfully more work than everything above; not seen in real-world testing so far |
| Any unrecognized/malformed component | Fails closed - a value this parser doesn't understand at all could silently mean something that changes which occurrences are valid | N/A by design - the whole point of failing closed |

## Related, non-`RRULE` caveat: timezone handling

A `DTSTART`/`DTEND` with no trailing `Z` (a "floating" time, or one qualified with `TZID=...`) is
interpreted as the **device's own configured local timezone** (via `mktime()`) rather than actually
resolving the named IANA timezone from `TZID`. This matches how every other wake/schedule
computation in this firmware already works, and is accurate as long as the calendar's own
timezone matches the device's - which is true for the overwhelming majority of personal calendars,
but would be wrong for an event deliberately created in a different timezone than the device sits
in.

## Where to look in code

- `ics_rrule_t` / `parse_rrule()` (`main/calendar_ics.c`) - what's parsed from the raw `RRULE`
  value and why each rejected component is rejected.
- `expand_rrule()` (`main/calendar_ics.c`) - how accepted rules are turned into actual occurrence
  timestamps within a requested window.
- `finalize_vevent()` (`main/calendar_ics.c`) - the `BYDAY`-vs-`DTSTART`-weekday cross-check that
  only becomes possible once `DTSTART` is known (RRULE components can appear in any order within a
  `VEVENT` block per RFC 5545, so `parse_rrule()` alone can't validate this at parse time).
- `host_tests/test_calendar_ics.cpp` - one test per behavior documented above, including a
  regression test built from a real-world `.ics` export.
