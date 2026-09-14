#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>

/* Days per month (non-leap, then leap) */
static const int _mon_days[2][12] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

static const char *_wday_abbr[] = { "Sun", "Mon", "Tue", "Wed",
				    "Thu", "Fri", "Sat" };

static const char *_wday_full[] = { "Sunday",    "Monday",   "Tuesday",
				    "Wednesday", "Thursday", "Friday",
				    "Saturday" };

static const char *_mon_abbr[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
				   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static const char *_mon_full[] = { "January", "February", "March",
				   "April",   "May",      "June",
				   "July",    "August",   "September",
				   "October", "November", "December" };

static int _is_leap(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static struct tm _gmtime_buf;

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
	time_t t = *timep;
	int days, rem;

	if (t < 0) {
		/* Minimal handling: clamp to epoch */
		t = 0;
	}

	days = (int)(t / 86400);
	rem = (int)(t % 86400);

	result->tm_hour = rem / 3600;
	rem %= 3600;
	result->tm_min = rem / 60;
	result->tm_sec = rem % 60;

	/* Jan 1, 1970 was a Thursday (wday=4) */
	result->tm_wday = (days + 4) % 7;
	if (result->tm_wday < 0)
		result->tm_wday += 7;

	/* Compute year and day-of-year */
	int year = 1970;
	while (1) {
		int yd = _is_leap(year) ? 366 : 365;
		if (days < yd)
			break;
		days -= yd;
		year++;
	}

	result->tm_year = year - 1900;
	result->tm_yday = days;

	int leap = _is_leap(year);
	int mon;
	for (mon = 0; mon < 11; mon++) {
		if (days < _mon_days[leap][mon])
			break;
		days -= _mon_days[leap][mon];
	}
	result->tm_mon = mon;
	result->tm_mday = days + 1;
	result->tm_isdst = 0;

	return result;
}

struct tm *gmtime(const time_t *timep)
{
	return gmtime_r(timep, &_gmtime_buf);
}


/* ===================================================================
 * Time zones
 *
 * There is no zoneinfo database on this system, so the zone cannot be read
 * from /usr/share/zoneinfo the way it is elsewhere.  What there is instead:
 *
 *   - the POSIX TZ string, which describes a zone completely in a few
 *     characters -- "CET-1CEST,M3.5.0,M10.5.0/3" is central Europe, standard
 *     time one hour east of UTC, summer time from the last Sunday in March to
 *     the last Sunday in October at 03:00.  That is the format POSIX itself
 *     specifies for TZ and it is parsed in full below;
 *
 *   - a table of the zone NAMES people actually set, because TZ on this
 *     system holds an Olson name ("Europe/Berlin").  It has to: ICU, which
 *     is what the browser's Date and Intl ask, accepts nothing else, and one
 *     variable has to serve both.  The table maps a name to its POSIX rule.
 *
 * A name that is not in the table leaves the clock on UTC rather than
 * guessing, and /etc/profile's comment says where to change it.  The rules
 * here are the current ones; a zone that changes its law needs the line
 * updated, which is the price of not carrying a database.
 * =================================================================== */

struct tz_when {
	int mode; /* 0 none, 1 = Mm.w.d, 2 = Jn (no Feb 29), 3 = n (0-365) */
	int m, w, d;
	int n;
	long secs; /* seconds after local midnight */
};

static char g_tz_std[16] = "UTC";
static char g_tz_dst[16] = "";
static long g_tz_std_east; /* local = UTC + this */
static long g_tz_dst_east;
static struct tz_when g_tz_start, g_tz_end;
static int g_tz_has_dst;
static int g_tz_done;

static const struct {
	const char *name;
	const char *rule;
} g_tz_table[] = {
	{ "UTC", "UTC0" },
	{ "Etc/UTC", "UTC0" },
	{ "GMT", "UTC0" },
	{ "Etc/GMT", "UTC0" },
	/* central Europe */
	{ "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Luxembourg", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Zagreb", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Bratislava", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Ljubljana", "CET-1CEST,M3.5.0,M10.5.0/3" },
	/* western Europe */
	{ "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0" },
	{ "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
	/* eastern Europe */
	{ "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Kiev", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Riga", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Tallinn", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Istanbul", "<+03>-3" },
	{ "Europe/Moscow", "MSK-3" },
	/* the Americas */
	{ "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Toronto", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
	{ "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
	{ "America/Phoenix", "MST7" },
	{ "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Vancouver", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
	{ "America/Sao_Paulo", "<-03>3" },
	{ "America/Mexico_City", "CST6" },
	{ "America/Bogota", "<-05>5" },
	{ "America/Buenos_Aires", "<-03>3" },
	{ "America/Argentina/Buenos_Aires", "<-03>3" },
	/* Asia and Oceania */
	{ "Asia/Tokyo", "JST-9" },
	{ "Asia/Seoul", "KST-9" },
	{ "Asia/Shanghai", "CST-8" },
	{ "Asia/Hong_Kong", "HKT-8" },
	{ "Asia/Singapore", "<+08>-8" },
	{ "Asia/Kolkata", "IST-5:30" },
	{ "Asia/Calcutta", "IST-5:30" },
	{ "Asia/Dubai", "<+04>-4" },
	{ "Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0" },
	{ "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Brisbane", "AEST-10" },
	{ "Australia/Perth", "AWST-8" },
	{ "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
	{ "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
	{ "Africa/Johannesburg", "SAST-2" },
	{ "Africa/Lagos", "WAT-1" },
};

/* A zone name ("CET"), or a quoted one ("<+04>"). */
static const char *tz_parse_name(const char *p, char *out, size_t cap)
{
	size_t i = 0;

	if (*p == '<') {
		p++;
		while (*p && *p != '>') {
			if (i + 1 < cap)
				out[i++] = *p;
			p++;
		}
		if (*p == '>')
			p++;
	} else {
		while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
			if (i + 1 < cap)
				out[i++] = *p;
			p++;
		}
	}
	out[i] = '\0';
	return p;
}

/* [+|-]hh[:mm[:ss]].  POSIX states the value to ADD TO LOCAL to reach UTC,
 * so the sign is flipped to get the offset east of UTC that everything
 * below wants. */
static const char *tz_parse_offset(const char *p, long *east)
{
	int sign = 1;
	long v = 0, part;

	if (*p == '+')
		p++;
	else if (*p == '-') {
		sign = -1;
		p++;
	}
	part = 0;
	while (*p >= '0' && *p <= '9')
		part = part * 10 + (*p++ - '0');
	v = part * 3600;
	if (*p == ':') {
		p++;
		part = 0;
		while (*p >= '0' && *p <= '9')
			part = part * 10 + (*p++ - '0');
		v += part * 60;
		if (*p == ':') {
			p++;
			part = 0;
			while (*p >= '0' && *p <= '9')
				part = part * 10 + (*p++ - '0');
			v += part;
		}
	}
	*east = -(sign * v);
	return p;
}

static const char *tz_parse_when(const char *p, struct tz_when *w)
{
	long part;

	w->mode = 0;
	w->secs = 2 * 3600; /* POSIX default: 02:00 local */
	if (*p == 'M') {
		p++;
		w->mode = 1;
		part = 0;
		while (*p >= '0' && *p <= '9')
			part = part * 10 + (*p++ - '0');
		w->m = (int)part;
		if (*p == '.')
			p++;
		part = 0;
		while (*p >= '0' && *p <= '9')
			part = part * 10 + (*p++ - '0');
		w->w = (int)part;
		if (*p == '.')
			p++;
		part = 0;
		while (*p >= '0' && *p <= '9')
			part = part * 10 + (*p++ - '0');
		w->d = (int)part;
	} else if (*p == 'J' || (*p >= '0' && *p <= '9')) {
		w->mode = (*p == 'J') ? 2 : 3;
		if (*p == 'J')
			p++;
		part = 0;
		while (*p >= '0' && *p <= '9')
			part = part * 10 + (*p++ - '0');
		w->n = (int)part;
	} else {
		return p;
	}
	if (*p == '/') {
		long east;
		p++;
		/* the same syntax as an offset, but a plain time of day:
		 * parse it and undo the sign flip */
		p = tz_parse_offset(p, &east);
		w->secs = -east;
	}
	return p;
}

static void tz_parse(const char *tz)
{
	const char *p = tz;

	g_tz_has_dst = 0;
	g_tz_dst[0] = '\0';
	g_tz_start.mode = g_tz_end.mode = 0;
	p = tz_parse_name(p, g_tz_std, sizeof(g_tz_std));
	if (!g_tz_std[0]) {
		strcpy(g_tz_std, "UTC");
		g_tz_std_east = 0;
		return;
	}
	p = tz_parse_offset(p, &g_tz_std_east);
	if (*p && *p != ',') {
		p = tz_parse_name(p, g_tz_dst, sizeof(g_tz_dst));
		if (g_tz_dst[0]) {
			g_tz_has_dst = 1;
			if (*p && *p != ',')
				p = tz_parse_offset(p, &g_tz_dst_east);
			else
				g_tz_dst_east = g_tz_std_east + 3600;
		}
	}
	if (g_tz_has_dst && *p == ',') {
		p++;
		p = tz_parse_when(p, &g_tz_start);
		if (*p == ',') {
			p++;
			p = tz_parse_when(p, &g_tz_end);
		}
		if (!g_tz_start.mode || !g_tz_end.mode) {
			/* a summer-time name with no rule cannot be applied */
			g_tz_has_dst = 0;
		}
	} else if (g_tz_has_dst) {
		/* POSIX says a missing rule means the US rule; without one
		 * stated, standard time all year is the safe reading. */
		g_tz_has_dst = 0;
	}
}

static time_t tz_days_from_epoch(int year, int mon0, int mday)
{
	time_t days = 0;
	int y;

	for (y = 1970; y < year; y++)
		days += _is_leap(y) ? 366 : 365;
	for (y = 0; y < mon0; y++)
		days += _mon_days[_is_leap(year)][y];
	return days + (mday - 1);
}

/* The instant a rule fires, in UTC, for a given year.  `east' is the offset
 * in force just BEFORE the change, which is what the rule's time of day is
 * expressed in. */
static time_t tz_transition(int year, const struct tz_when *w, long east)
{
	time_t days;

	if (w->mode == 1) {
		int leap = _is_leap(year);
		int mon0 = w->m - 1;
		int mlen = _mon_days[leap][mon0];
		time_t first = tz_days_from_epoch(year, mon0, 1);
		int wday_first = (int)((first + 4) % 7); /* 1970-01-01 = Thu */
		int shift = (w->d - wday_first + 7) % 7;
		int mday = 1 + shift + (w->w - 1) * 7;
		while (mday > mlen)
			mday -= 7;
		days = tz_days_from_epoch(year, mon0, mday);
	} else if (w->mode == 2) {
		/* Jn: day n of the year, February 29 never counted */
		int n = w->n;
		int leap = _is_leap(year);
		if (leap && n >= 60)
			n++;
		days = tz_days_from_epoch(year, 0, 1) + (n - 1);
	} else {
		days = tz_days_from_epoch(year, 0, 1) + w->n;
	}
	return days * 86400 + w->secs - east;
}

static int tz_is_dst(time_t t)
{
	struct tm probe;
	time_t start, end;
	int year;

	if (!g_tz_has_dst)
		return 0;
	gmtime_r(&t, &probe);
	year = probe.tm_year + 1900;
	start = tz_transition(year, &g_tz_start, g_tz_std_east);
	end = tz_transition(year, &g_tz_end, g_tz_dst_east);
	if (start <= end)
		return t >= start && t < end; /* northern hemisphere */
	return t >= start || t < end; /* southern: summer spans new year */
}

static void tz_ensure(void)
{
	if (!g_tz_done)
		tzset();
}

/* What a broken-down time says, read as UTC.  mktime() and timegm() differ
 * only in what they do with the answer. */
static time_t tz_tm_to_utc(const struct tm *tm)
{
	int year = tm->tm_year + 1900;
	int mon = tm->tm_mon;
	time_t t = 0;

	for (int y = 1970; y < year; y++)
		t += _is_leap(y) ? 366 : 365;
	int leap = _is_leap(year);
	for (int m = 0; m < mon; m++)
		t += _mon_days[leap][m];
	t += tm->tm_mday - 1;
	return t * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
	time_t t = *timep;
	int dst;
	time_t local;

	tz_ensure();
	dst = tz_is_dst(t);
	local = t + (dst ? g_tz_dst_east : g_tz_std_east);
	gmtime_r(&local, result);
	result->tm_isdst = dst;
	return result;
}

struct tm *localtime(const time_t *timep)
{
	return localtime_r(timep, &_gmtime_buf);
}

/* mktime(3): the broken-down time is LOCAL, and the answer is UTC. */
time_t mktime(struct tm *tm)
{
	time_t as_utc, res;
	int dst;

	tz_ensure();
	as_utc = tz_tm_to_utc(tm);
	/* Which offset applied depends on the answer, and the answer depends
	 * on the offset.  Resolve it the usual way: assume standard time,
	 * then ask what was actually in force at the instant that gives.  A
	 * caller that already knows says so through tm_isdst. */
	if (tm->tm_isdst > 0)
		dst = g_tz_has_dst;
	else if (tm->tm_isdst == 0)
		dst = 0;
	else
		dst = tz_is_dst(as_utc - g_tz_std_east);
	res = as_utc - (dst ? g_tz_dst_east : g_tz_std_east);
	if (tm->tm_isdst < 0) {
		/* One correction pass: a time inside the spring-forward gap
		 * or the autumn overlap lands on the other side otherwise. */
		int again = tz_is_dst(res);
		if (again != dst) {
			dst = again;
			res = as_utc - (dst ? g_tz_dst_east : g_tz_std_east);
		}
	}

	/* The derived fields describe the LOCAL time the caller gave. */
	struct tm check;
	gmtime_r(&as_utc, &check);
	tm->tm_wday = check.tm_wday;
	tm->tm_yday = check.tm_yday;
	tm->tm_isdst = dst;

	return res;
}

/* timegm(3): mktime with the broken-down time taken as UTC.  Portable code
 * uses the pair to learn the zone offset -- WTF's date handling computes
 * exactly timegm(&t) - mktime(&t) where the platform has no tm_gmtoff --
 * so the two must differ by the offset, and this one applies none. */
time_t timegm(struct tm *tm)
{
	time_t t = tz_tm_to_utc(tm);
	struct tm check;

	gmtime_r(&t, &check);
	tm->tm_wday = check.tm_wday;
	tm->tm_yday = check.tm_yday;
	tm->tm_isdst = 0;
	return t;
}

/* Helper: append string, return chars written */
static size_t _fmt_str(char *buf, size_t rem, const char *s)
{
	size_t len = strlen(s);
	if (len > rem)
		len = rem;
	memcpy(buf, s, len);
	return len;
}

/* Helper: format a number with zero-padding to width */
static size_t _fmt_num(char *buf, size_t rem, int val, int width)
{
	char tmp[16];
	int neg = 0;
	unsigned int uv;
	if (val < 0) {
		neg = 1;
		uv = (unsigned int)(-val);
	} else {
		uv = (unsigned int)val;
	}
	int pos = 0;
	do {
		tmp[pos++] = '0' + (uv % 10);
		uv /= 10;
	} while (uv > 0);
	/* Pad */
	while (pos < width - neg)
		tmp[pos++] = '0';
	if (neg)
		tmp[pos++] = '-';
	/* Reverse into buf */
	size_t n = 0;
	for (int i = pos - 1; i >= 0 && n < rem; i--)
		buf[n++] = tmp[i];
	return n;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
	size_t pos = 0;

	if (max == 0)
		return 0;
	max--; /* Reserve space for NUL */

	while (*format && pos < max) {
		if (*format != '%') {
			s[pos++] = *format++;
			continue;
		}
		format++; /* skip '%' */
		if (*format == '\0')
			break;

		size_t n = 0;
		switch (*format) {
		case '%':
			s[pos++] = '%';
			break;
		case 'a':
			n = _fmt_str(s + pos, max - pos,
				     _wday_abbr[tm->tm_wday % 7]);
			pos += n;
			break;
		case 'A':
			n = _fmt_str(s + pos, max - pos,
				     _wday_full[tm->tm_wday % 7]);
			pos += n;
			break;
		case 'b':
		case 'h':
			n = _fmt_str(s + pos, max - pos,
				     _mon_abbr[tm->tm_mon % 12]);
			pos += n;
			break;
		case 'B':
			n = _fmt_str(s + pos, max - pos,
				     _mon_full[tm->tm_mon % 12]);
			pos += n;
			break;
		case 'c': {
			/* Locale date-time: "Thu Jan  1 00:00:00 1970" */
			char tmp[64];
			snprintf(tmp, sizeof(tmp),
				 "%s %s %2d %02d:%02d:%02d %d",
				 _wday_abbr[tm->tm_wday % 7],
				 _mon_abbr[tm->tm_mon % 12], tm->tm_mday,
				 tm->tm_hour, tm->tm_min, tm->tm_sec,
				 tm->tm_year + 1900);
			n = _fmt_str(s + pos, max - pos, tmp);
			pos += n;
			break;
		}
		case 'C':
			n = _fmt_num(s + pos, max - pos,
				     (tm->tm_year + 1900) / 100, 2);
			pos += n;
			break;
		case 'd':
			n = _fmt_num(s + pos, max - pos, tm->tm_mday, 2);
			pos += n;
			break;
		case 'D': {
			char tmp[16];
			snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d",
				 tm->tm_mon + 1, tm->tm_mday,
				 tm->tm_year % 100);
			n = _fmt_str(s + pos, max - pos, tmp);
			pos += n;
			break;
		}
		case 'e':
			n = _fmt_num(s + pos, max - pos, tm->tm_mday, 1);
			if (n == 1 && pos + 1 < max) {
				s[pos + 1] = s[pos];
				s[pos] = ' ';
				n = 2;
			}
			pos += n;
			break;
		case 'F': {
			char tmp[16];
			snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d",
				 tm->tm_year + 1900, tm->tm_mon + 1,
				 tm->tm_mday);
			n = _fmt_str(s + pos, max - pos, tmp);
			pos += n;
			break;
		}
		case 'H':
			n = _fmt_num(s + pos, max - pos, tm->tm_hour, 2);
			pos += n;
			break;
		case 'I': {
			int h = tm->tm_hour % 12;
			if (h == 0)
				h = 12;
			n = _fmt_num(s + pos, max - pos, h, 2);
			pos += n;
			break;
		}
		case 'j':
			n = _fmt_num(s + pos, max - pos, tm->tm_yday + 1, 3);
			pos += n;
			break;
		case 'k':
			n = _fmt_num(s + pos, max - pos, tm->tm_hour, 1);
			if (n == 1 && pos + 1 < max) {
				s[pos + 1] = s[pos];
				s[pos] = ' ';
				n = 2;
			}
			pos += n;
			break;
		case 'l': {
			int h = tm->tm_hour % 12;
			if (h == 0)
				h = 12;
			n = _fmt_num(s + pos, max - pos, h, 1);
			if (n == 1 && pos + 1 < max) {
				s[pos + 1] = s[pos];
				s[pos] = ' ';
				n = 2;
			}
			pos += n;
			break;
		}
		case 'm':
			n = _fmt_num(s + pos, max - pos, tm->tm_mon + 1, 2);
			pos += n;
			break;
		case 'M':
			n = _fmt_num(s + pos, max - pos, tm->tm_min, 2);
			pos += n;
			break;
		case 'n':
			s[pos++] = '\n';
			break;
		case 'p':
			n = _fmt_str(s + pos, max - pos,
				     tm->tm_hour < 12 ? "AM" : "PM");
			pos += n;
			break;
		case 'P':
			n = _fmt_str(s + pos, max - pos,
				     tm->tm_hour < 12 ? "am" : "pm");
			pos += n;
			break;
		case 'r': {
			int h = tm->tm_hour % 12;
			if (h == 0)
				h = 12;
			char tmp[16];
			snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d %s", h,
				 tm->tm_min, tm->tm_sec,
				 tm->tm_hour < 12 ? "AM" : "PM");
			n = _fmt_str(s + pos, max - pos, tmp);
			pos += n;
			break;
		}
		case 'R': {
			char tmp[8];
			snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour,
				 tm->tm_min);
			n = _fmt_str(s + pos, max - pos, tmp);
			pos += n;
			break;
		}
		case 'S':
			n = _fmt_num(s + pos, max - pos, tm->tm_sec, 2);
			pos += n;
			break;
		case 't':
			s[pos++] = '\t';
			break;
		case 'T': {
			char tmp[12];
			snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d",
				 tm->tm_hour, tm->tm_min, tm->tm_sec);
			n = _fmt_str(s + pos, max - pos, tmp);
			pos += n;
			break;
		}
		case 'u': {
			int wd = tm->tm_wday == 0 ? 7 : tm->tm_wday;
			n = _fmt_num(s + pos, max - pos, wd, 1);
			pos += n;
			break;
		}
		case 'w':
			n = _fmt_num(s + pos, max - pos, tm->tm_wday, 1);
			pos += n;
			break;
		case 'x': {
			char tmp[16];
			snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d",
				 tm->tm_mon + 1, tm->tm_mday,
				 tm->tm_year % 100);
			n = _fmt_str(s + pos, max - pos, tmp);
			pos += n;
			break;
		}
		case 'X': {
			char tmp[12];
			snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d",
				 tm->tm_hour, tm->tm_min, tm->tm_sec);
			n = _fmt_str(s + pos, max - pos, tmp);
			pos += n;
			break;
		}
		case 'y':
			n = _fmt_num(s + pos, max - pos, tm->tm_year % 100, 2);
			pos += n;
			break;
		case 'Y':
			n = _fmt_num(s + pos, max - pos, tm->tm_year + 1900, 4);
			pos += n;
			break;
		case 'z': {
			/* The zone the broken-down time is in, which its own
			 * tm_isdst says; there is no tm_gmtoff to read. */
			char zb[8];
			long off;
			long a;
			tz_ensure();
			off = (tm->tm_isdst > 0 && g_tz_has_dst) ? g_tz_dst_east :
								  g_tz_std_east;
			a = off < 0 ? -off : off;
			zb[0] = off < 0 ? '-' : '+';
			zb[1] = (char)('0' + (a / 36000) % 10);
			zb[2] = (char)('0' + (a / 3600) % 10);
			zb[3] = (char)('0' + ((a % 3600) / 600) % 10);
			zb[4] = (char)('0' + ((a % 3600) / 60) % 10);
			zb[5] = '\0';
			n = _fmt_str(s + pos, max - pos, zb);
			pos += n;
			break;
		}
		case 'Z':
			tz_ensure();
			n = _fmt_str(s + pos, max - pos,
				     (tm->tm_isdst > 0 && g_tz_has_dst) ? g_tz_dst :
									  g_tz_std);
			pos += n;
			break;
		default:
			/* Unknown specifier: output as-is */
			if (pos + 1 < max) {
				s[pos++] = '%';
				s[pos++] = *format;
			}
			break;
		}
		format++;
	}

	s[pos] = '\0';
	return pos;
}

/* asctime / ctime - format a struct tm or a time_t into a 26-byte
 * "Wed Jun 30 21:49:08 1993\n\0" string. tzset is a no-op since we
 * track no local timezone. */

static const char _wday_name[7][4] = { "Sun", "Mon", "Tue", "Wed",
				       "Thu", "Fri", "Sat" };
static const char _mon_name[12][4] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void _put2(char *p, int n)
{
	p[0] = '0' + (n / 10) % 10;
	p[1] = '0' + (n % 10);
}

char *asctime_r(const struct tm *tm, char *buf)
{
	if (!tm || !buf)
		return 0;
	int wday = tm->tm_wday & 7;
	int mon = tm->tm_mon & 15;
	if (wday >= 7)
		wday = 0;
	if (mon >= 12)
		mon = 0;
	int year = tm->tm_year + 1900;
	/* "Www Mmm dd hh:mm:ss yyyy\n\0" - 26 bytes */
	buf[0] = _wday_name[wday][0];
	buf[1] = _wday_name[wday][1];
	buf[2] = _wday_name[wday][2];
	buf[3] = ' ';
	buf[4] = _mon_name[mon][0];
	buf[5] = _mon_name[mon][1];
	buf[6] = _mon_name[mon][2];
	buf[7] = ' ';
	_put2(&buf[8], tm->tm_mday);
	buf[10] = ' ';
	_put2(&buf[11], tm->tm_hour);
	buf[13] = ':';
	_put2(&buf[14], tm->tm_min);
	buf[16] = ':';
	_put2(&buf[17], tm->tm_sec);
	buf[19] = ' ';
	buf[20] = '0' + ((year / 1000) % 10);
	buf[21] = '0' + ((year / 100) % 10);
	buf[22] = '0' + ((year / 10) % 10);
	buf[23] = '0' + (year % 10);
	buf[24] = '\n';
	buf[25] = '\0';
	return buf;
}

char *asctime(const struct tm *tm)
{
	static char buf[26];
	return asctime_r(tm, buf);
}

char *ctime_r(const time_t *t, char *buf)
{
	struct tm tm;
	if (!t || !buf)
		return 0;
	if (!localtime_r(t, &tm))
		return 0;
	return asctime_r(&tm, buf);
}

char *ctime(const time_t *t)
{
	static char buf[26];
	return ctime_r(t, buf);
}

char *tzname[2] = { (char *)"UTC", (char *)"UTC" };
long timezone = 0;
int daylight = 0;

void tzset(void)
{
	const char *tz = getenv("TZ");
	const char *rule = NULL;

	char fromfile[64];

	g_tz_done = 1;
	if (!tz || !*tz) {
		/* Nothing in the environment.  /etc/timezone holds the zone
		 * name for the whole system, and reading it here means a
		 * process that never saw a login shell -- a daemon, a
		 * program started by init -- still keeps local time. */
		FILE *f = fopen("/etc/timezone", "r");
		if (f) {
			if (fgets(fromfile, (int)sizeof(fromfile), f)) {
				size_t n = strlen(fromfile);
				while (n && (fromfile[n - 1] == '\n' ||
					     fromfile[n - 1] == '\r' ||
					     fromfile[n - 1] == ' ' ||
					     fromfile[n - 1] == '\t'))
					fromfile[--n] = '\0';
				if (fromfile[0])
					tz = fromfile;
			}
			fclose(f);
		}
	}
	if (!tz || !*tz) {
		rule = "UTC0";
	} else if (strchr(tz, ',') || strchr(tz, '<') ||
		   (!strchr(tz, '/') && *tz != ':')) {
		/* already a POSIX rule */
		rule = tz;
	} else {
		if (*tz == ':')
			tz++;
		for (size_t i = 0; i < sizeof(g_tz_table) / sizeof(g_tz_table[0]); i++) {
			if (strcmp(g_tz_table[i].name, tz) == 0) {
				rule = g_tz_table[i].rule;
				break;
			}
		}
		if (!rule)
			rule = "UTC0"; /* a name with no rule here stays UTC */
	}
	tz_parse(rule);
	tzname[0] = g_tz_std;
	tzname[1] = g_tz_has_dst ? g_tz_dst : g_tz_std;
	timezone = -g_tz_std_east;
	daylight = g_tz_has_dst;
}

/* ===================================================================
 * difftime / strptime
 * =================================================================== */

/* difftime(): the difference between two times, in seconds.
 *
 * Computed in double rather than by subtracting time_t values and converting,
 * because the standard defines the result as a double and the subtraction of
 * two distant times can overflow a signed integer -- which is undefined
 * behaviour, not merely a wrong answer. */
double difftime(time_t time1, time_t time0)
{
	return (double)time1 - (double)time0;
}

/* --- strptime ---------------------------------------------------------- */

static int _sp_isspace(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
	       c == '\r';
}

static int _sp_tolower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Case-insensitive prefix match; returns the length matched, or 0. */
static size_t _sp_match(const char *s, const char *word)
{
	size_t i = 0;
	while (word[i]) {
		if (!s[i] || _sp_tolower((unsigned char)s[i]) !=
				     _sp_tolower((unsigned char)word[i]))
			return 0;
		i++;
	}
	return i;
}

/* Read a bounded number: at most `maxdig` digits, result within [from,to].
 *
 * The stopping rule matters and is the reference's: another digit is consumed
 * only while the value would STAY within `to`.  That is not the same as
 * reading maxdig digits and range-checking afterwards -- given "%H" and the
 * input "97", reading two digits yields 97 and fails, while stopping early
 * yields 9 and leaves "7" for the next specifier.  Real formats like "%H%M%S"
 * against unpadded input depend on the early stop, so a reader that fails
 * instead would reject strings the caller expects to parse.
 *
 * Only spaces are skipped, not every kind of whitespace -- again matching the
 * reference, so that a tab in the input is a mismatch rather than padding.
 *
 * Returns the new position, or NULL if there is no digit or the value is out
 * of range. */
static const char *_sp_num(const char *s, int from, int to, int maxdig,
			   int *out)
{
	int val = 0;
	int n = maxdig;

	while (*s == ' ')
		s++;
	if (*s < '0' || *s > '9')
		return NULL;
	do {
		val *= 10;
		val += *s++ - '0';
	} while (--n > 0 && val * 10 <= to && *s >= '0' && *s <= '9');
	if (val < from || val > to)
		return NULL;
	*out = val;
	return s;
}

/* Cumulative days before each month, for common and leap years. */
static const unsigned short _sp_mon_yday[2][13] = {
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
};

static int _sp_isleap(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

/* Derive tm_wday and tm_yday from tm_year/tm_mon/tm_mday.
 *
 * strptime() is expected to fill these in once it has a date, even though the
 * format never mentions them -- code that parses "%Y-%m-%d" and then reads
 * tm_wday is common, and leaving it zero silently means "Sunday".  The
 * arithmetic is the reference implementation's, deliberately: it is what
 * callers have been tested against, including for the odd inputs (a parsed
 * month with no day gives tm_mday 0, and the answer that falls out of the same
 * formula is the one they expect). */
static void _sp_fill_wday(struct tm *tm)
{
	int corr_year = 1900 + tm->tm_year - (tm->tm_mon < 2);
	int wday = (-473 + (365 * (tm->tm_year - 70)) + (corr_year / 4) -
		    ((corr_year / 4) / 25) + ((corr_year / 4) % 25 < 0) +
		    (((corr_year / 4) / 25) / 4) + _sp_mon_yday[0][tm->tm_mon] +
		    tm->tm_mday - 1);
	tm->tm_wday = ((wday % 7) + 7) % 7;
}

static void _sp_fill_yday(struct tm *tm)
{
	tm->tm_yday = _sp_mon_yday[_sp_isleap(1900 + tm->tm_year)][tm->tm_mon] +
		      (tm->tm_mday - 1);
}

/*
 * strptime(): parse a time string according to a format.
 *
 * Returns a pointer to the first character not consumed, or NULL if the input
 * does not match.  Fields the format does not mention are left ALONE -- the
 * caller's struct tm is updated, not initialised, which is what lets several
 * calls build up one time and why this must not helpfully zero anything.
 *
 * Two-digit years follow POSIX: 69-99 mean 1969-1999, 00-68 mean 2000-2068.
 */
char *strptime(const char *s, const char *format, struct tm *tm)
{
	const char *p = format;
	int i, v;
	int century = -1, year2 = -1;
	int pm = -1;
	int have_date = 0; /* a year, month or day was parsed */
	int have_wday = 0; /* %a/%A/%w/%u gave the weekday explicitly */

	if (!s || !format || !tm)
		return NULL;

	while (*p) {
		if (_sp_isspace((unsigned char)*p)) {
			/* Whitespace in the format matches any amount of
			 * whitespace in the input, including none. */
			while (_sp_isspace((unsigned char)*s))
				s++;
			p++;
			continue;
		}
		if (*p != '%') {
			if (*s != *p)
				return NULL;
			s++;
			p++;
			continue;
		}

		p++; /* consume '%' */
		if (*p == 'E' || *p == 'O')
			p++; /* locale-alternative forms: same as the base here */

		switch (*p) {
		case '%':
			if (*s != '%')
				return NULL;
			s++;
			break;

		case 'n':
		case 't':
			while (_sp_isspace((unsigned char)*s))
				s++;
			break;

		case 'a':
		case 'A': {
			size_t n = 0;
			for (i = 0; i < 7; i++) {
				/* Full name first: "Sunday" also starts with
				 * "Sun", so trying the abbreviation first
				 * would stop after three characters and leave
				 * "day" unconsumed. */
				n = _sp_match(s, _wday_full[i]);
				if (!n)
					n = _sp_match(s, _wday_abbr[i]);
				if (n) {
					tm->tm_wday = i;
					have_wday = 1;
					s += n;
					break;
				}
			}
			if (!n)
				return NULL;
			break;
		}

		case 'b':
		case 'B':
		case 'h': {
			size_t n = 0;
			for (i = 0; i < 12; i++) {
				n = _sp_match(s, _mon_full[i]);
				if (!n)
					n = _sp_match(s, _mon_abbr[i]);
				if (n) {
					tm->tm_mon = i;
					have_date = 1;
					s += n;
					break;
				}
			}
			if (!n)
				return NULL;
			break;
		}

		case 'd':
		case 'e':
			s = _sp_num(s, 1, 31, 2, &v);
			if (!s)
				return NULL;
			tm->tm_mday = v;
			have_date = 1;
			break;

		case 'm':
			s = _sp_num(s, 1, 12, 2, &v);
			if (!s)
				return NULL;
			tm->tm_mon = v - 1;
			have_date = 1;
			break;

		case 'y':
			s = _sp_num(s, 0, 99, 2, &v);
			if (!s)
				return NULL;
			year2 = v;
			tm->tm_year = (v >= 69) ? v : v + 100;
			have_date = 1;
			break;

		case 'Y':
			s = _sp_num(s, 0, 9999, 4, &v);
			if (!s)
				return NULL;
			tm->tm_year = v - 1900;
			year2 = -1;
			have_date = 1;
			break;

		case 'C':
			s = _sp_num(s, 0, 99, 2, &v);
			if (!s)
				return NULL;
			century = v;
			have_date = 1;
			break;

		case 'H':
			s = _sp_num(s, 0, 23, 2, &v);
			if (!s)
				return NULL;
			tm->tm_hour = v;
			break;

		case 'I':
			s = _sp_num(s, 1, 12, 2, &v);
			if (!s)
				return NULL;
			tm->tm_hour = v;
			break;

		case 'M':
			s = _sp_num(s, 0, 59, 2, &v);
			if (!s)
				return NULL;
			tm->tm_min = v;
			break;

		case 'S':
			/* Up to 61: leap seconds, and the reference has always
			 * allowed two of them in one minute. */
			s = _sp_num(s, 0, 61, 2, &v);
			if (!s)
				return NULL;
			tm->tm_sec = v;
			break;

		case 'j':
			s = _sp_num(s, 1, 366, 3, &v);
			if (!s)
				return NULL;
			tm->tm_yday = v - 1;
			break;

		case 'w':
			s = _sp_num(s, 0, 6, 1, &v);
			if (!s)
				return NULL;
			tm->tm_wday = v;
			have_wday = 1;
			break;

		case 'u':
			s = _sp_num(s, 1, 7, 1, &v);
			if (!s)
				return NULL;
			tm->tm_wday = (v == 7) ? 0 : v;
			have_wday = 1;
			break;

		case 'U':
		case 'W':
		case 'V':
			/* Week numbers are parsed and discarded: they cannot
			 * set a date on their own, and silently inventing one
			 * would be worse than ignoring the field. */
			s = _sp_num(s, 0, 53, 2, &v);
			if (!s)
				return NULL;
			break;

		case 'p':
		case 'P': {
			size_t n = _sp_match(s, "AM");
			if (n) {
				pm = 0;
			} else if ((n = _sp_match(s, "PM")) != 0) {
				pm = 1;
			} else {
				return NULL;
			}
			s += n;
			break;
		}

		case 'Z': {
			/* Time-zone names are accepted and ignored: there is no
			 * zone database here, so recording one would imply a
			 * conversion this system cannot perform. */
			while ((*s >= 'A' && *s <= 'Z') ||
			       (*s >= 'a' && *s <= 'z'))
				s++;
			break;
		}

		case 'z': {
			/* Four forms, as the reference accepts them:
			 *   Z          UTC
			 *   +hh        hours only
			 *   +hhmm      hours and minutes
			 *   +hh:mm     the ISO 8601 spelling
			 *
			 * The colon form is not optional to support: it is what
			 * appears in ISO 8601 timestamps, so a browser parsing
			 * a date like "2026-07-31T10:20:30+01:30" needs it.
			 * Omitting it did not fail loudly -- the offset simply
			 * stopped after "+01", leaving ":30" to be mismatched
			 * by whatever came next in the format.
			 *
			 * The value is parsed and discarded: this struct tm has
			 * no tm_gmtoff to record it in, and there is no zone
			 * database to convert with.  Consuming exactly the
			 * right characters is what the rest of the format
			 * depends on. */
			if (*s == 'Z') {
				s++;
			} else if (*s == '+' || *s == '-') {
				int nd = 0;
				s++;
				while (nd < 2 && *s >= '0' && *s <= '9') {
					s++;
					nd++;
				}
				if (nd != 2)
					return NULL;
				if (*s == ':') {
					/* +hh:mm -- the minutes are required
					 * once the colon is there. */
					const char *c = s + 1;
					int md = 0;
					while (md < 2 && *c >= '0' &&
					       *c <= '9') {
						c++;
						md++;
					}
					if (md != 2)
						return NULL;
					s = c;
				} else if (*s >= '0' && *s <= '9') {
					/* +hhmm */
					int md = 0;
					while (md < 2 && *s >= '0' &&
					       *s <= '9') {
						s++;
						md++;
					}
					if (md != 2)
						return NULL;
				}
			} else {
				return NULL;
			}
			break;
		}

		case 'D': /* %m/%d/%y */
		case 'x':
		case 'F': /* %Y-%m-%d */
		case 'T': /* %H:%M:%S */
		case 'X':
		case 'R': /* %H:%M */
		case 'r': /* %I:%M:%S %p */
		case 'c': {
			const char *sub;
			char *r;
			switch (*p) {
			case 'D':
			case 'x':
				sub = "%m/%d/%y";
				break;
			case 'F':
				sub = "%Y-%m-%d";
				break;
			case 'T':
			case 'X':
				sub = "%H:%M:%S";
				break;
			case 'R':
				sub = "%H:%M";
				break;
			case 'r':
				sub = "%I:%M:%S %p";
				break;
			default:
				sub = "%a %b %e %H:%M:%S %Y";
				break;
			}
			r = strptime(s, sub, tm);
			if (!r)
				return NULL;
			/* The sub-format carries date and weekday fields of
			 * its own; %T and %R are the only purely-time ones. */
			if (*p != 'T' && *p != 'X' && *p != 'R' && *p != 'r')
				have_date = 1;
			if (*p == 'c')
				have_wday = 1;
			s = r;
			break;
		}

		case 's': {
			/* Seconds since the epoch (an extension, but Duktape
			 * and much else expect it). */
			long long acc = 0;
			int nd = 0;
			/* At least one digit, and no sign: the reference's
			 * %s extension takes an unsigned count of seconds. */
			while (*s >= '0' && *s <= '9') {
				acc = acc * 10 + (*s - '0');
				s++;
				nd++;
			}
			if (!nd)
				return NULL;
			{
				time_t t = (time_t)acc;
				struct tm tmp;
				/* Local time, matching the reference: there is
				 * no zone database here, so this is UTC in
				 * practice, but the choice is recorded rather
				 * than accidental. */
				if (!localtime_r(&t, &tmp))
					return NULL;
				*tm = tmp;
			}
			break;
		}

		default:
			/* An unknown specifier is a format error, not something
			 * to skip: skipping would consume the wrong input and
			 * report success with a wrong time. */
			return NULL;
		}
		p++;
	}

	/* %C and %y combine into a full year; either alone has already been
	 * applied above. */
	if (century >= 0) {
		int yy = (year2 >= 0) ? year2 : 0;
		tm->tm_year = century * 100 + yy - 1900;
	}

	/* %p only means something together with %I. */
	if (pm >= 0) {
		int h = tm->tm_hour % 12;
		tm->tm_hour = pm ? h + 12 : h;
	}

	/* Derive the weekday and day-of-year once a date has been seen.  The
	 * weekday is NOT overwritten when the format supplied one: a caller
	 * that parsed "%A" asked for that value, and recomputing it from an
	 * otherwise-empty struct tm would replace it with a weekday for the
	 * year 1900. */
	if (have_date) {
		if (!have_wday)
			_sp_fill_wday(tm);
		_sp_fill_yday(tm);
	}

	return (char *)s;
}
