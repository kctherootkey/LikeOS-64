/* Host test for the libc's time zone handling (user/lib/libc/src/time/time.c).
 *
 * The file is compiled with its public names renamed (see test-tz.sh) so it
 * can sit beside the host's own libc, and the pieces under test are pure
 * arithmetic over a TZ string: what offset is in force at an instant, what
 * localtime() makes of it, and that mktime() is its exact inverse.
 *
 * The instants are chosen around the European and American transitions,
 * which is where an off-by-one week or an inverted sign shows up. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int lk_failures;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			lk_failures++;                                         \
		}                                                              \
	} while (0)

struct tm;
extern void lk_tzset(void);
extern struct tm *lk_localtime_r(const long *, struct tm *);
extern long lk_mktime(struct tm *);
extern long lk_timegm(struct tm *);
extern unsigned long lk_strftime(char *, unsigned long, const char *,
				 const struct tm *);
extern char *lk_tzname[2];
extern long lk_timezone;
extern int lk_daylight;

/* The layout the libc declares; kept here so the test does not drag in the
 * whole header set. */
struct tm {
	int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
	int tm_wday, tm_yday, tm_isdst;
};

static void set_tz(const char *v)
{
	setenv("TZ", v, 1);
	lk_tzset();
}

/* The offset the zone applies at a UTC instant, in seconds east. */
static long offset_at(long t)
{
	struct tm tm;
	lk_localtime_r(&t, &tm);
	return lk_timegm(&tm) - t;
}

int main(void)
{
	struct tm tm;
	char buf[64];

	/* --- no zone: UTC, and nothing shifts ------------------------- */
	set_tz("UTC");
	CHECK(offset_at(1700000000L) == 0, "UTC offset %ld", offset_at(1700000000L));
	CHECK(lk_timezone == 0 && lk_daylight == 0, "UTC globals");
	CHECK(strcmp(lk_tzname[0], "UTC") == 0, "UTC name %s", lk_tzname[0]);

	/* --- central Europe, by name ---------------------------------- */
	set_tz("Europe/Berlin");
	CHECK(lk_daylight == 1, "Berlin has summer time");
	CHECK(lk_timezone == -3600, "Berlin standard offset west %ld", lk_timezone);
	/* 2026-01-15 12:00:00 UTC: winter, CET, +1 */
	CHECK(offset_at(1768478400L) == 3600, "Berlin January %ld",
	      offset_at(1768478400L));
	/* 2026-07-15 12:00:00 UTC: summer, CEST, +2 */
	CHECK(offset_at(1784116800L) == 7200, "Berlin July %ld",
	      offset_at(1784116800L));
	/* The change is the last Sunday in March, 01:00 UTC.  2026-03-29
	 * 00:59:59 UTC is still CET; one second past 01:00 is CEST. */
	CHECK(offset_at(1774746000L - 1) == 3600, "Berlin before the change %ld",
	      offset_at(1774746000L - 1));
	CHECK(offset_at(1774746000L) == 7200, "Berlin after the change %ld",
	      offset_at(1774746000L));
	/* ...and back on the last Sunday in October (2026-10-25), 03:00 CEST = 01:00 UTC. */
	CHECK(offset_at(1792890000L - 1) == 7200, "Berlin before October %ld",
	      offset_at(1792890000L - 1));
	CHECK(offset_at(1792890000L) == 3600, "Berlin after October %ld",
	      offset_at(1792890000L));

	/* localtime() puts the clock forward, and names the zone ------- */
	{
		long t = 1784116800L; /* 2026-07-15 12:00 UTC */
		lk_localtime_r(&t, &tm);
		CHECK(tm.tm_hour == 14 && tm.tm_mday == 15 && tm.tm_mon == 6,
		      "Berlin July local %d:%02d on day %d", tm.tm_hour, tm.tm_min,
		      tm.tm_mday);
		CHECK(tm.tm_isdst == 1, "July is summer time");
		lk_strftime(buf, sizeof(buf), "%Z %z", &tm);
		CHECK(strcmp(buf, "CEST +0200") == 0, "July zone %s", buf);
		t = 1768478400L; /* 2026-01-15 12:00 UTC */
		lk_localtime_r(&t, &tm);
		CHECK(tm.tm_hour == 13, "Berlin January local hour %d", tm.tm_hour);
		lk_strftime(buf, sizeof(buf), "%Z %z", &tm);
		CHECK(strcmp(buf, "CET +0100") == 0, "January zone %s", buf);
	}

	/* mktime() is the inverse of localtime() ----------------------- */
	{
		long probes[] = { 1768478400L, 1784116800L, 1774746000L,
				  1792890000L, 1700000000L };
		for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
			struct tm back;
			lk_localtime_r(&probes[i], &back);
			back.tm_isdst = -1;
			long round = lk_mktime(&back);
			CHECK(round == probes[i], "round trip %ld -> %ld",
			      probes[i], round);
		}
	}

	/* timegm() applies no offset, which is how a caller with no
	 * tm_gmtoff derives one: WTF's calculateUTCOffset() zeroes tm_isdst,
	 * takes January 1, and subtracts mktime() from timegm().  Each call
	 * gets its own copy because both normalise the struct they are
	 * given. */
	{
		long t = 1768478400L; /* January: standard time */
		struct tm a, b;
		lk_localtime_r(&t, &a);
		a.tm_isdst = 0;
		b = a;
		CHECK(lk_timegm(&a) - lk_mktime(&b) == 3600,
		      "derived standard offset %ld",
		      lk_timegm(&a) - lk_mktime(&b));
	}

	/* --- a POSIX rule given directly ------------------------------ */
	set_tz("CET-1CEST,M3.5.0,M10.5.0/3");
	CHECK(offset_at(1784116800L) == 7200, "explicit rule July %ld",
	      offset_at(1784116800L));

	/* --- the American rule, a different month and week ------------ */
	set_tz("America/New_York");
	/* 2026-01-15 12:00 UTC -> EST, -5 */
	CHECK(offset_at(1768478400L) == -5 * 3600, "New York January %ld",
	      offset_at(1768478400L));
	/* 2026-07-15 12:00 UTC -> EDT, -4 */
	CHECK(offset_at(1784116800L) == -4 * 3600, "New York July %ld",
	      offset_at(1784116800L));

	/* --- southern hemisphere: summer spans the new year ----------- */
	set_tz("Australia/Sydney");
	CHECK(offset_at(1768478400L) == 11 * 3600, "Sydney January %ld",
	      offset_at(1768478400L));
	CHECK(offset_at(1784116800L) == 10 * 3600, "Sydney July %ld",
	      offset_at(1784116800L));

	/* --- a half-hour zone, and one with no summer time ------------ */
	set_tz("Asia/Kolkata");
	CHECK(offset_at(1784116800L) == 5 * 3600 + 1800, "Kolkata %ld",
	      offset_at(1784116800L));
	set_tz("Asia/Tokyo");
	CHECK(offset_at(1784116800L) == 9 * 3600, "Tokyo %ld",
	      offset_at(1784116800L));
	CHECK(lk_daylight == 0, "Tokyo has no summer time");

	/* --- a name the table does not carry stays on UTC ------------- */
	set_tz("Mars/Olympus_Mons");
	CHECK(offset_at(1784116800L) == 0, "unknown zone %ld",
	      offset_at(1784116800L));

	if (lk_failures) {
		printf("%d failure(s)\n", lk_failures);
		return 1;
	}
	printf("tz: all tests passed\n");
	return 0;
}
