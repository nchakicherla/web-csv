#include "datetime.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Howard Hinnant's days_from_civil / civil_from_days, days counted from
 * 1970-01-01 (epoch day 0). See datetime.h's header comment for the
 * reference. */
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
	int64_t era;
	unsigned yoe, doy, doe;

	y -= (m <= 2);
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400); /* [0, 399] */
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; /* [0, 146096] */
	return era * 146097 + (int64_t)doe - 719468;
}

static void civilFromDays(int64_t z, int *y, unsigned *m, unsigned *d) {
	int64_t era, yy;
	unsigned doe, yoe, doy, mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097); /* [0, 146096] */
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
	yy = (int64_t)yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100); /* [0, 365] */
	mp = (5 * doy + 2) / 153; /* [0, 11] */
	*d = doy - (153 * mp + 2) / 5 + 1; /* [1, 31] */
	*m = mp + (mp < 10 ? 3 : -9); /* [1, 12] */
	*y = (int)(yy + (*m <= 2));
}

int wcParseDate(const char *s, double *out) {
	int y, mo, d, h = 0, mi = 0, se = 0;
	int n;

	n = sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se);
	if (n < 3) {
		h = mi = se = 0;
		n = sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se);
	}
	if (n < 3 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23
	    || mi < 0 || mi > 59 || se < 0 || se > 60) {
		return 1;
	}

	*out = (double)daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400.0
	       + h * 3600.0 + mi * 60.0 + se;
	return 0;
}

bool wcIsValidDatePartUnit(const char *unit) {
	return 0 == strcmp(unit, "year") || 0 == strcmp(unit, "month")
	       || 0 == strcmp(unit, "day") || 0 == strcmp(unit, "weekday");
}

void wcFormatDatePart(double epoch_seconds, const char *unit, char *buf, size_t buf_len) {
	int64_t days;
	int y;
	unsigned m, d;

	/* floor(), not truncation: a negative epoch_seconds that isn't an
	 * exact multiple of 86400 (e.g. -1, one second before the epoch)
	 * must land on 1969-12-31, not round toward zero into 1970-01-01. */
	days = (int64_t)floor(epoch_seconds / 86400.0);
	civilFromDays(days, &y, &m, &d);

	if (0 == strcmp(unit, "year")) {
		snprintf(buf, buf_len, "%04d", y);
	} else if (0 == strcmp(unit, "month")) {
		snprintf(buf, buf_len, "%04d-%02d", y, m);
	} else if (0 == strcmp(unit, "weekday")) {
		static const char *names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
		/* 1970-01-01 (days=0) was a Thursday (index 4); +4 aligns day 0
		 * to that index, and the extra +7 keeps the operand of % non-
		 * negative for any days this function is called with (int64_t %
		 * in C can otherwise return a negative remainder). */
		unsigned wd = (unsigned)(((days % 7) + 7 + 4) % 7);
		snprintf(buf, buf_len, "%s", names[wd]);
	} else {
		snprintf(buf, buf_len, "%04d-%02d-%02d", y, m, d); /* "day", and the fallback for any other unit */
	}
}
