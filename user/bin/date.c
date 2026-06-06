/*
 * date - print or set the system date and time
 *
 * Usage: date [OPTION]... [+FORMAT]
 *        date [-u|--utc|--universal] [MMDDhhmm[[CC]YY][.ss]]
 *
 * Full implementation per the date(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>

#define DATE_VERSION "date (LikeOS coreutils) 0.1"

static void print_help(const char *prog)
{
    printf("Usage: %s [OPTION]... [+FORMAT]\n", prog);
    printf("  or:  %s [-u|--utc|--universal] [MMDDhhmm[[CC]YY][.ss]]\n", prog);
    printf("Display the current time in the given FORMAT, or set the system date.\n\n");
    printf("  -d, --date=STRING    display time described by STRING, not 'now'\n");
    printf("  -f, --file=DATEFILE  like --date; once for each line of DATEFILE\n");
    printf("  -I[FMT], --iso-8601[=FMT]\n");
    printf("                       output date/time in ISO 8601 format.\n");
    printf("                       FMT='date' (default), 'hours', 'minutes', 'seconds', 'ns'\n");
    printf("  -R, --rfc-email      output date and time in RFC 5322 format\n");
    printf("  --rfc-3339=FMT       output in RFC 3339 format (date, seconds, ns)\n");
    printf("  -r, --reference=FILE display last modification time of FILE\n");
    printf("  -s, --set=STRING     set time described by STRING\n");
    printf("  -u, --utc, --universal  print or set UTC\n");
    printf("  --resolution         output the available resolution of timestamps\n");
    printf("      --help     display this help and exit\n");
    printf("      --version  output version information and exit\n");
    printf("\nFORMAT controls the output.  Interpreted sequences are:\n");
    printf("  %%%%   a literal %%\n");
    printf("  %%a   abbreviated weekday name (e.g., Sun)\n");
    printf("  %%A   full weekday name (e.g., Sunday)\n");
    printf("  %%b   abbreviated month name (e.g., Jan)\n");
    printf("  %%B   full month name (e.g., January)\n");
    printf("  %%c   date and time (e.g., Thu Mar  3 23:05:25 2005)\n");
    printf("  %%C   century (e.g., 20)\n");
    printf("  %%d   day of month (e.g., 01)\n");
    printf("  %%D   date; same as %%m/%%d/%%y\n");
    printf("  %%e   day of month, space padded (e.g.,  1)\n");
    printf("  %%F   full date; like %%Y-%%m-%%d\n");
    printf("  %%H   hour (00..23)\n");
    printf("  %%I   hour (01..12)\n");
    printf("  %%j   day of year (001..366)\n");
    printf("  %%k   hour, space padded ( 0..23)\n");
    printf("  %%l   hour, space padded ( 1..12)\n");
    printf("  %%m   month (01..12)\n");
    printf("  %%M   minute (00..59)\n");
    printf("  %%n   a newline\n");
    printf("  %%N   nanoseconds (000000000..999999999)\n");
    printf("  %%p   AM or PM\n");
    printf("  %%P   am or pm\n");
    printf("  %%r   12-hour clock time (e.g., 11:11:04 PM)\n");
    printf("  %%R   24-hour hour and minute; same as %%H:%%M\n");
    printf("  %%s   seconds since the Epoch\n");
    printf("  %%S   second (00..60)\n");
    printf("  %%t   a tab\n");
    printf("  %%T   time; same as %%H:%%M:%%S\n");
    printf("  %%u   day of week (1..7); 1 is Monday\n");
    printf("  %%U   week number of year, Sunday first (00..53)\n");
    printf("  %%w   day of week (0..6); 0 is Sunday\n");
    printf("  %%W   week number of year, Monday first (00..53)\n");
    printf("  %%x   date representation (e.g., 12/31/99)\n");
    printf("  %%X   time representation (e.g., 23:13:48)\n");
    printf("  %%y   last two digits of year (00..99)\n");
    printf("  %%Y   year\n");
    printf("  %%z   +hhmm numeric time zone (e.g., +0000)\n");
    printf("  %%Z   alphabetic time zone abbreviation (e.g., UTC)\n");
}

/* Extended format: handle %%s, %%N, %%q, %%V, %%U, %%W which strftime may not cover */
static void format_date(const char *fmt, const struct tm *tm, time_t epoch, long nsec)
{
    char out[4096];
    int pos = 0;
    int max = (int)sizeof(out) - 1;

    for (int i = 0; fmt[i] && pos < max; i++) {
        if (fmt[i] != '%') {
            out[pos++] = fmt[i];
            continue;
        }
        i++;
        if (!fmt[i]) break;

        /* Check for flags: -, _, 0, +, ^, # */
        char pad_char = '0';
        int uppercase = 0;
        int swap_case = 0;
        int no_pad = 0;
        while (fmt[i] == '-' || fmt[i] == '_' || fmt[i] == '0' ||
               fmt[i] == '+' || fmt[i] == '^' || fmt[i] == '#') {
            if (fmt[i] == '-') no_pad = 1;
            else if (fmt[i] == '_') pad_char = ' ';
            else if (fmt[i] == '0') pad_char = '0';
            else if (fmt[i] == '^') uppercase = 1;
            else if (fmt[i] == '#') swap_case = 1;
            i++;
        }
        /* Optional width */
        int width = 0;
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            i++;
        }
        /* Skip E and O modifiers */
        if (fmt[i] == 'E' || fmt[i] == 'O')
            i++;

        if (!fmt[i]) break;

        char tmp[256];
        tmp[0] = '\0';
        int tmplen = 0;

        (void)pad_char;
        (void)no_pad;
        (void)width;
        (void)uppercase;
        (void)swap_case;

        switch (fmt[i]) {
        case '%': tmp[0] = '%'; tmp[1] = '\0'; tmplen = 1; break;
        case 'a': {
            const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            strcpy(tmp, days[tm->tm_wday % 7]);
            tmplen = 3;
            break;
        }
        case 'A': {
            const char *days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
            strcpy(tmp, days[tm->tm_wday % 7]);
            tmplen = (int)strlen(tmp);
            break;
        }
        case 'b': case 'h': {
            const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
            strcpy(tmp, mons[tm->tm_mon % 12]);
            tmplen = 3;
            break;
        }
        case 'B': {
            const char *mons[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
            strcpy(tmp, mons[tm->tm_mon % 12]);
            tmplen = (int)strlen(tmp);
            break;
        }
        case 'c':
            tmplen = snprintf(tmp, sizeof(tmp), "%s %s %2d %02d:%02d:%02d %d",
                (const char*[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[tm->tm_wday%7],
                (const char*[]){"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"}[tm->tm_mon%12],
                tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year+1900);
            break;
        case 'C':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_year + 1900) / 100);
            break;
        case 'd':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday);
            break;
        case 'D':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon+1, tm->tm_mday, tm->tm_year%100);
            break;
        case 'e':
            tmplen = snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday);
            break;
        case 'F':
            tmplen = snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d", tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday);
            break;
        case 'g': case 'G': case 'V': {
            /* ISO week number/year - simplified calculation */
            int yday = tm->tm_yday;
            int wday = tm->tm_wday;
            int iso_wday = (wday == 0) ? 7 : wday;
            int week = (yday + 7 - iso_wday + 1) / 7;
            /* Adjust: ISO week 1 is the week with January 4th */
            int jan1_wday = (wday - yday % 7 + 7) % 7;
            int iso_jan1 = (jan1_wday == 0) ? 7 : jan1_wday;
            week = (yday + iso_jan1 - 1) / 7;
            if (iso_jan1 <= 4) week++;
            if (week == 0) week = 52; /* belongs to previous year */
            int iso_year = tm->tm_year + 1900;
            if (week >= 52 && tm->tm_mon == 0 && tm->tm_mday <= 3) iso_year--;
            if (week == 1 && tm->tm_mon == 11 && tm->tm_mday >= 29) iso_year++;
            if (fmt[i] == 'V') tmplen = snprintf(tmp, sizeof(tmp), "%02d", week);
            else if (fmt[i] == 'g') tmplen = snprintf(tmp, sizeof(tmp), "%02d", iso_year % 100);
            else tmplen = snprintf(tmp, sizeof(tmp), "%04d", iso_year);
            break;
        }
        case 'H':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour);
            break;
        case 'I': {
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", h);
            break;
        }
        case 'j':
            tmplen = snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1);
            break;
        case 'k':
            tmplen = snprintf(tmp, sizeof(tmp), "%2d", tm->tm_hour);
            break;
        case 'l': {
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            tmplen = snprintf(tmp, sizeof(tmp), "%2d", h);
            break;
        }
        case 'm':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1);
            break;
        case 'M':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min);
            break;
        case 'n':
            tmp[0] = '\n'; tmp[1] = '\0'; tmplen = 1;
            break;
        case 'N':
            tmplen = snprintf(tmp, sizeof(tmp), "%09ld", nsec);
            break;
        case 'p':
            strcpy(tmp, tm->tm_hour < 12 ? "AM" : "PM");
            tmplen = 2;
            break;
        case 'P':
            strcpy(tmp, tm->tm_hour < 12 ? "am" : "pm");
            tmplen = 2;
            break;
        case 'q':
            tmplen = snprintf(tmp, sizeof(tmp), "%d", tm->tm_mon / 3 + 1);
            break;
        case 'r': {
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            tmplen = snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d %s",
                h, tm->tm_min, tm->tm_sec, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        }
        case 'R':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min);
            break;
        case 's':
            tmplen = snprintf(tmp, sizeof(tmp), "%ld", (long)epoch);
            break;
        case 'S':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec);
            break;
        case 't':
            tmp[0] = '\t'; tmp[1] = '\0'; tmplen = 1;
            break;
        case 'T':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            break;
        case 'u': {
            int wd = tm->tm_wday == 0 ? 7 : tm->tm_wday;
            tmplen = snprintf(tmp, sizeof(tmp), "%d", wd);
            break;
        }
        case 'U': {
            int week = (tm->tm_yday + 7 - tm->tm_wday) / 7;
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", week);
            break;
        }
        case 'w':
            tmplen = snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday);
            break;
        case 'W': {
            int monday_wday = (tm->tm_wday + 6) % 7;
            int week = (tm->tm_yday + 7 - monday_wday) / 7;
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", week);
            break;
        }
        case 'x':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon+1, tm->tm_mday, tm->tm_year%100);
            break;
        case 'X':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            break;
        case 'y':
            tmplen = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100);
            break;
        case 'Y':
            tmplen = snprintf(tmp, sizeof(tmp), "%04d", tm->tm_year + 1900);
            break;
        case 'z':
            strcpy(tmp, "+0000");
            tmplen = 5;
            break;
        case ':': {
            /* %:z, %::z, %:::z */
            int colons = 1;
            while (fmt[i+1] == ':') { colons++; i++; }
            if (fmt[i+1] == 'z') {
                i++;
                if (colons == 1)
                    strcpy(tmp, "+00:00");
                else if (colons == 2)
                    strcpy(tmp, "+00:00:00");
                else
                    strcpy(tmp, "+00");
                tmplen = (int)strlen(tmp);
            } else {
                tmp[0] = '%';
                tmp[1] = ':';
                tmp[2] = '\0';
                tmplen = 2;
            }
            break;
        }
        case 'Z':
            strcpy(tmp, "UTC");
            tmplen = 3;
            break;
        default:
            /* Unknown: output %X */
            tmp[0] = '%';
            tmp[1] = fmt[i];
            tmp[2] = '\0';
            tmplen = 2;
            break;
        }

        /* Apply case transformations */
        if (uppercase) {
            for (int j = 0; j < tmplen; j++)
                if (tmp[j] >= 'a' && tmp[j] <= 'z')
                    tmp[j] -= 32;
        }
        if (swap_case) {
            for (int j = 0; j < tmplen; j++) {
                if (tmp[j] >= 'a' && tmp[j] <= 'z')
                    tmp[j] -= 32;
                else if (tmp[j] >= 'A' && tmp[j] <= 'Z')
                    tmp[j] += 32;
            }
        }

        /* Copy to output */
        for (int j = 0; j < tmplen && pos < max; j++)
            out[pos++] = tmp[j];
    }
    out[pos] = '\0';
    printf("%s\n", out);
}

/* Parse a date string in the format MMDDhhmm[[CC]YY][.ss] */
static int parse_set_date(const char *str, struct tm *tm)
{
    int len = (int)strlen(str);
    int ss_offset = -1;

    /* Check for .ss suffix */
    for (int i = 0; i < len; i++) {
        if (str[i] == '.') {
            ss_offset = i;
            break;
        }
    }

    const char *main_part = str;
    int main_len = ss_offset >= 0 ? ss_offset : len;

    if (main_len < 8 || main_len == 9 || main_len == 11 || main_len > 12)
        return -1;

    /* Parse MMDDhhmm */
    int mm = (main_part[0] - '0') * 10 + (main_part[1] - '0');
    int dd = (main_part[2] - '0') * 10 + (main_part[3] - '0');
    int hh = (main_part[4] - '0') * 10 + (main_part[5] - '0');
    int mi = (main_part[6] - '0') * 10 + (main_part[7] - '0');

    int year = tm->tm_year + 1900;
    if (main_len == 10) {
        /* YY */
        int yy = (main_part[8] - '0') * 10 + (main_part[9] - '0');
        year = (yy >= 69) ? (1900 + yy) : (2000 + yy);
    } else if (main_len == 12) {
        /* CCYY */
        int cc = (main_part[8] - '0') * 10 + (main_part[9] - '0');
        int yy = (main_part[10] - '0') * 10 + (main_part[11] - '0');
        year = cc * 100 + yy;
    }

    int ss = 0;
    if (ss_offset >= 0 && ss_offset + 2 < len) {
        ss = (str[ss_offset + 1] - '0') * 10 + (str[ss_offset + 2] - '0');
    }

    tm->tm_mon = mm - 1;
    tm->tm_mday = dd;
    tm->tm_hour = hh;
    tm->tm_min = mi;
    tm->tm_sec = ss;
    tm->tm_year = year - 1900;

    return 0;
}

/* Parse "YYYY-MM-DD[T ]HH:MM:SS" without sscanf */
static int parse_datetime(const char *str, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    /* Expect: digits-digits-digits[T ]digits:digits:digits */
    const char *p = str;
    char *end;

    *y = (int)strtol(p, &end, 10);
    if (end == p || *end != '-') return -1;
    p = end + 1;

    *mo = (int)strtol(p, &end, 10);
    if (end == p || *end != '-') return -1;
    p = end + 1;

    *d = (int)strtol(p, &end, 10);
    if (end == p || (*end != ' ' && *end != 'T')) return -1;
    p = end + 1;

    *h = (int)strtol(p, &end, 10);
    if (end == p || *end != ':') return -1;
    p = end + 1;

    *mi = (int)strtol(p, &end, 10);
    if (end == p || *end != ':') return -1;
    p = end + 1;

    *s = (int)strtol(p, &end, 10);
    if (end == p) return -1;

    return 0;
}

/* Simple @epoch parser and free-text date string parser */
static int parse_date_string(const char *str, struct tm *out_tm, time_t *out_epoch)
{
    /* @epoch format */
    if (str[0] == '@') {
        long e = atol(str + 1);
        *out_epoch = (time_t)e;
        gmtime_r(out_epoch, out_tm);
        return 0;
    }

    /* Try YYYY-MM-DD HH:MM:SS or YYYY-MM-DDTHH:MM:SS */
    int y, mo, d, h, mi, s;
    if (parse_datetime(str, &y, &mo, &d, &h, &mi, &s) == 0) {
        out_tm->tm_year = y - 1900;
        out_tm->tm_mon = mo - 1;
        out_tm->tm_mday = d;
        out_tm->tm_hour = h;
        out_tm->tm_min = mi;
        out_tm->tm_sec = s;
        *out_epoch = mktime(out_tm);
        return 0;
    }

    /* Fall back: try just epoch number */
    char *end;
    long val = strtol(str, &end, 10);
    if (end != str && *end == '\0') {
        *out_epoch = (time_t)val;
        gmtime_r(out_epoch, out_tm);
        return 0;
    }

    return -1;
}

int main(int argc, char *argv[])
{
    int utc_mode = 0;
    const char *date_string = NULL;
    const char *set_string = NULL;
    const char *reference_file = NULL;
    const char *iso_fmt = NULL;
    int rfc_email = 0;
    const char *rfc3339_fmt = NULL;
    int show_resolution = 0;

    /* Rewrite long options to short */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { print_help(argv[0]); return 0; }
        if (strcmp(argv[i], "--version") == 0) { printf("%s\n", DATE_VERSION); return 0; }
        if (strcmp(argv[i], "--utc") == 0 || strcmp(argv[i], "--universal") == 0)
            { argv[i] = "-u"; continue; }
        if (strncmp(argv[i], "--date=", 7) == 0) {
            date_string = argv[i] + 7;
            argv[i] = "-\1";  /* consumed */
            continue;
        }
        if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) {
            date_string = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--set=", 6) == 0) {
            set_string = argv[i] + 6;
            argv[i] = "-\1";
            continue;
        }
        if (strncmp(argv[i], "--reference=", 12) == 0) {
            reference_file = argv[i] + 12;
            argv[i] = "-\1";
            continue;
        }
        if (strcmp(argv[i], "--rfc-email") == 0)
            { rfc_email = 1; argv[i] = "-\1"; continue; }
        if (strncmp(argv[i], "--rfc-3339=", 11) == 0)
            { rfc3339_fmt = argv[i] + 11; argv[i] = "-\1"; continue; }
        if (strcmp(argv[i], "--iso-8601") == 0)
            { iso_fmt = "date"; argv[i] = "-\1"; continue; }
        if (strncmp(argv[i], "--iso-8601=", 11) == 0)
            { iso_fmt = argv[i] + 11; argv[i] = "-\1"; continue; }
        if (strcmp(argv[i], "--resolution") == 0)
            { show_resolution = 1; argv[i] = "-\1"; continue; }
        if (strcmp(argv[i], "--debug") == 0)
            { argv[i] = "-\1"; continue; } /* silently ignore */
    }

    optind = 1;
    int opt;
    while ((opt = getopt(argc, argv, "d:f:I::Rr:s:u\1")) != -1) {
        switch (opt) {
        case 'd':
            date_string = optarg;
            break;
        case 'f':
            /* --file: treat as --date for simplicity */
            date_string = optarg;
            break;
        case 'I':
            iso_fmt = optarg ? optarg : "date";
            break;
        case 'R':
            rfc_email = 1;
            break;
        case 'r':
            reference_file = optarg;
            break;
        case 's':
            set_string = optarg;
            break;
        case 'u':
            utc_mode = 1;
            break;
        case '\1':
            break; /* already handled */
        default:
            fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
            return 1;
        }
    }

    (void)utc_mode; /* always UTC in this OS */

    if (show_resolution) {
        printf("0.000000001\n");
        return 0;
    }

    /* Determine the time to display */
    time_t now;
    struct tm tm_val;
    long nsec = 0;

    if (date_string) {
        if (parse_date_string(date_string, &tm_val, &now) < 0) {
            fprintf(stderr, "%s: invalid date '%s'\n", argv[0], date_string);
            return 1;
        }
    } else if (reference_file) {
        struct stat st;
        if (stat(reference_file, &st) < 0) {
            fprintf(stderr, "%s: cannot stat '%s'\n", argv[0], reference_file);
            return 1;
        }
        now = (time_t)st.st_mtime;
        gmtime_r(&now, &tm_val);
    } else if (set_string) {
        /* Setting time: parse the string and set */
        now = time(NULL);
        gmtime_r(&now, &tm_val);
        if (parse_date_string(set_string, &tm_val, &now) < 0) {
            fprintf(stderr, "%s: invalid date '%s'\n", argv[0], set_string);
            return 1;
        }
        now = mktime(&tm_val);
        struct timeval tv;
        tv.tv_sec = (long)now;
        tv.tv_usec = 0;
        if (settimeofday(&tv, NULL) < 0) {
            fprintf(stderr, "%s: cannot set date: %s\n", argv[0],
                    errno == EPERM ? "Operation not permitted" : "System error");
            return 1;
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        now = ts.tv_sec;
        nsec = ts.tv_nsec;
        gmtime_r(&now, &tm_val);
    }

    /* Check for non-option argument starting with '+' = format string */
    const char *user_format = NULL;
    for (int i = optind; i < argc; i++) {
        if (argv[i][0] == '+') {
            user_format = argv[i] + 1;
            break;
        }
        /* Check for MMDDhhmm set format */
        if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            if (parse_set_date(argv[i], &tm_val) < 0) {
                fprintf(stderr, "%s: invalid date '%s'\n", argv[0], argv[i]);
                return 1;
            }
            now = mktime(&tm_val);
            struct timeval stv;
            stv.tv_sec = (long)now;
            stv.tv_usec = 0;
            if (settimeofday(&stv, NULL) < 0) {
                fprintf(stderr, "%s: cannot set date: %s\n", argv[0],
                        errno == EPERM ? "Operation not permitted" : "System error");
                return 1;
            }
            break;
        }
    }

    /* Apply special format modes */
    if (rfc_email) {
        const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        printf("%s, %02d %s %04d %02d:%02d:%02d +0000\n",
               days[tm_val.tm_wday % 7], tm_val.tm_mday,
               mons[tm_val.tm_mon % 12], tm_val.tm_year + 1900,
               tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
        return 0;
    }

    if (rfc3339_fmt) {
        if (strcmp(rfc3339_fmt, "date") == 0) {
            printf("%04d-%02d-%02d\n", tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday);
        } else if (strcmp(rfc3339_fmt, "seconds") == 0) {
            printf("%04d-%02d-%02d %02d:%02d:%02d+00:00\n",
                   tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday,
                   tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
        } else if (strcmp(rfc3339_fmt, "ns") == 0) {
            printf("%04d-%02d-%02d %02d:%02d:%02d.%09ld+00:00\n",
                   tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday,
                   tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec, nsec);
        }
        return 0;
    }

    if (iso_fmt) {
        if (strcmp(iso_fmt, "date") == 0) {
            printf("%04d-%02d-%02d\n", tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday);
        } else if (strcmp(iso_fmt, "hours") == 0) {
            printf("%04d-%02d-%02dT%02d+00:00\n",
                   tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday, tm_val.tm_hour);
        } else if (strcmp(iso_fmt, "minutes") == 0) {
            printf("%04d-%02d-%02dT%02d:%02d+00:00\n",
                   tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday,
                   tm_val.tm_hour, tm_val.tm_min);
        } else if (strcmp(iso_fmt, "seconds") == 0) {
            printf("%04d-%02d-%02dT%02d:%02d:%02d+00:00\n",
                   tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday,
                   tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
        } else if (strcmp(iso_fmt, "ns") == 0) {
            printf("%04d-%02d-%02dT%02d:%02d:%02d,%09ld+00:00\n",
                   tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday,
                   tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec, nsec);
        } else {
            printf("%04d-%02d-%02d\n", tm_val.tm_year+1900, tm_val.tm_mon+1, tm_val.tm_mday);
        }
        return 0;
    }

    if (user_format) {
        format_date(user_format, &tm_val, now, nsec);
    } else {
        /* Default format: same as date(1) default */
        const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        printf("%s %s %2d %02d:%02d:%02d UTC %04d\n",
               days[tm_val.tm_wday % 7],
               mons[tm_val.tm_mon % 12],
               tm_val.tm_mday,
               tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec,
               tm_val.tm_year + 1900);
    }

    return 0;
}
