#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

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

/* No timezone support — localtime is the same as gmtime */
struct tm *localtime_r(const time_t *timep, struct tm *result)
{
	return gmtime_r(timep, result);
}

struct tm *localtime(const time_t *timep)
{
	return gmtime_r(timep, &_gmtime_buf);
}

time_t mktime(struct tm *tm)
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
	t = t * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;

	/* Fill in derived fields */
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
		case 'z':
			n = _fmt_str(s + pos, max - pos, "+0000");
			pos += n;
			break;
		case 'Z':
			n = _fmt_str(s + pos, max - pos, "UTC");
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
	/* No timezone database: localtime == gmtime, name is fixed. */
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
