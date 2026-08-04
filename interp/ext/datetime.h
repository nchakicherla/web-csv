#ifndef WC_DATETIME_H
#define WC_DATETIME_H

/* datetime.h - civil-calendar math backing COL_DATE: parsing an ISO 8601
 * date (optionally with a time-of-day) into epoch seconds, and formatting
 * epoch seconds back into a calendar part for date_part()/emit(). No libc
 * time functions (mktime/timegm/strftime): those are calendar-*and*-
 * timezone aware in ways that vary by libc and aren't reliably UTC-safe
 * under Emscripten, where this all needs to agree with plain
 * `new Date(...)` arithmetic on the JS side (see parse.js's date
 * detection). The civil<->days conversion below is Howard Hinnant's
 * well-known proleptic-Gregorian algorithm (public domain math, see
 * http://howardhinnant.github.io/date_algorithms.html) - correct for any
 * year, not just a libc's supported range.
 *
 * Dates are stored as epoch *seconds* (double), UTC, matching
 * Date.parse(...)/1000 on the JS side - see column.h's COL_DATE.
 */

#include "common.h"

/* Parses "YYYY-MM-DD" or "YYYY-MM-DD[T ]HH:MM[:SS]" (UTC, no timezone
 * offset support - matches the subset of ISO 8601 parse.js's date
 * detection accepts). Returns 0 and writes epoch seconds to *out on
 * success, nonzero on a malformed string or an out-of-range month/day. */
int wcParseDate(const char *s, double *out);

/* True for the unit names date_part() accepts: "year", "month", "day",
 * "weekday". */
bool wcIsValidDatePartUnit(const char *unit);

/* Formats epoch seconds into buf (at least 16 bytes) per unit:
 *   "year"    -> "YYYY"
 *   "month"   -> "YYYY-MM"
 *   "day"     -> "YYYY-MM-DD"
 *   "weekday" -> "Sun".."Sat"
 * Falls back to "day"'s format for any other unit string - callers that
 * haven't already validated with wcIsValidDatePartUnit() get a sensible
 * result rather than an empty buffer. */
void wcFormatDatePart(double epoch_seconds, const char *unit, char *buf, size_t buf_len);

#endif // WC_DATETIME_H
