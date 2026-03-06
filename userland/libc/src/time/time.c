#include "../../include/time.h"
#include "../../include/string.h"
#include "../../include/stdio.h"
#include <stddef.h>

/* Days per month (non-leap, then leap) */
static const int _mon_days[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

static const char *_wday_abbr[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *_wday_full[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

static const char *_mon_abbr[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *_mon_full[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

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
    rem  = (int)(t % 86400);

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
    result->tm_mon  = mon;
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
    int mon  = tm->tm_mon;
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
    if (len > rem) len = rem;
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

    if (max == 0) return 0;
    max--; /* Reserve space for NUL */

    while (*format && pos < max) {
        if (*format != '%') {
            s[pos++] = *format++;
            continue;
        }
        format++; /* skip '%' */
        if (*format == '\0') break;

        size_t n = 0;
        switch (*format) {
        case '%':
            s[pos++] = '%';
            break;
        case 'a':
            n = _fmt_str(s + pos, max - pos, _wday_abbr[tm->tm_wday % 7]);
            pos += n;
            break;
        case 'A':
            n = _fmt_str(s + pos, max - pos, _wday_full[tm->tm_wday % 7]);
            pos += n;
            break;
        case 'b':
        case 'h':
            n = _fmt_str(s + pos, max - pos, _mon_abbr[tm->tm_mon % 12]);
            pos += n;
            break;
        case 'B':
            n = _fmt_str(s + pos, max - pos, _mon_full[tm->tm_mon % 12]);
            pos += n;
            break;
        case 'c': {
            /* Locale date-time: "Thu Jan  1 00:00:00 1970" */
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s %s %2d %02d:%02d:%02d %d",
                     _wday_abbr[tm->tm_wday % 7],
                     _mon_abbr[tm->tm_mon % 12],
                     tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec,
                     tm->tm_year + 1900);
            n = _fmt_str(s + pos, max - pos, tmp);
            pos += n;
            break;
        }
        case 'C':
            n = _fmt_num(s + pos, max - pos, (tm->tm_year + 1900) / 100, 2);
            pos += n;
            break;
        case 'd':
            n = _fmt_num(s + pos, max - pos, tm->tm_mday, 2);
            pos += n;
            break;
        case 'D': {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d",
                     tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100);
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
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
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
            if (h == 0) h = 12;
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
            if (h == 0) h = 12;
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
            n = _fmt_str(s + pos, max - pos, tm->tm_hour < 12 ? "AM" : "PM");
            pos += n;
            break;
        case 'P':
            n = _fmt_str(s + pos, max - pos, tm->tm_hour < 12 ? "am" : "pm");
            pos += n;
            break;
        case 'r': {
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d %s",
                     h, tm->tm_min, tm->tm_sec,
                     tm->tm_hour < 12 ? "AM" : "PM");
            n = _fmt_str(s + pos, max - pos, tmp);
            pos += n;
            break;
        }
        case 'R': {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min);
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
                     tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100);
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
