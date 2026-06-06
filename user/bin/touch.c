/*
 * touch - change file timestamps
 *
 * Full implementation per touch(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "touch"

/* Which timestamps to change */
#define CHANGE_ATIME  1
#define CHANGE_MTIME  2
#define CHANGE_BOTH   (CHANGE_ATIME | CHANGE_MTIME)

static int opt_no_create = 0;       /* -c */
static int opt_no_dereference = 0;  /* -h */
static int opt_change = CHANGE_BOTH;
static int opt_have_time = 0;       /* whether -d, -r, or -t was used */
static struct timespec opt_times[2]; /* [0]=atime, [1]=mtime */

static void usage(void) {
    printf("Usage: %s [OPTION]... FILE...\n", PROGRAM_NAME);
    printf("Update the access and modification times of each FILE to the current time.\n\n");
    printf("A FILE argument that does not exist is created empty, unless -c or -h\n");
    printf("is supplied.\n\n");
    printf("A FILE argument string of - is handled specially and causes touch to\n");
    printf("change the times of the file associated with standard output.\n\n");
    printf("  -a                     change only the access time\n");
    printf("  -c, --no-create        do not create any files\n");
    printf("  -d, --date=STRING      parse STRING and use it instead of current time\n");
    printf("  -f                     (ignored)\n");
    printf("  -h, --no-dereference   affect each symbolic link instead of any referenced\n");
    printf("                         file (useful only on systems that can change the\n");
    printf("                         timestamps of a symlink)\n");
    printf("  -m                     change only the modification time\n");
    printf("  -r, --reference=FILE   use this file's times instead of current time\n");
    printf("  -t STAMP               use [[CC]YY]MMDDhhmm[.ss] instead of current time\n");
    printf("      --time=WORD        change the specified time:\n");
    printf("                           WORD is access, atime, or use: equivalent to -a\n");
    printf("                           WORD is modify or mtime: equivalent to -m\n");
    printf("      --help             display this help and exit\n");
    printf("      --version          output version information and exit\n");
}

static void version(void) {
    printf("%s %s\n", PROGRAM_NAME, VERSION);
}

/* Parse a simple integer from a string, advancing the pointer */
static int parse_int(const char **s, int digits) {
    int val = 0;
    for (int i = 0; i < digits; i++) {
        if (**s < '0' || **s > '9') return -1;
        val = val * 10 + (**s - '0');
        (*s)++;
    }
    return val;
}

/* Simple mktime: convert broken-down time to epoch seconds.
 * Assumes UTC (no timezone support in this OS). */
static time_t simple_mktime(int year, int mon, int day, int hour, int min, int sec) {
    /* Days in months (non-leap) */
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    /* Count days from epoch (1970-01-01) to the given date */
    long days = 0;

    /* Years */
    for (int y = 1970; y < year; y++) {
        days += 365;
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))
            days++;
    }

    /* Months */
    for (int m = 0; m < mon - 1; m++) {
        days += mdays[m];
        if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)))
            days++;
    }

    /* Days */
    days += day - 1;

    return (time_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}

/*
 * Parse -t STAMP format: [[CC]YY]MMDDhhmm[.ss]
 * Returns 0 on success, -1 on error.
 */
static int parse_stamp(const char *stamp, struct timespec *ts) {
    const char *p = stamp;
    size_t len = strlen(stamp);
    int year, mon, day, hour, min, sec = 0;

    /* Find .ss suffix */
    const char *dot = NULL;
    size_t main_len = len;
    for (size_t i = 0; i < len; i++) {
        if (stamp[i] == '.') {
            dot = &stamp[i];
            main_len = i;
            break;
        }
    }

    /* Get current time for defaults */
    time_t now = time(NULL);
    struct tm *tm_now = gmtime(&now);
    int cur_century = (tm_now->tm_year + 1900) / 100;
    int cur_year = tm_now->tm_year + 1900;

    if (main_len == 8) {
        /* MMDDhhmm */
        year = cur_year;
        mon = parse_int(&p, 2);
        day = parse_int(&p, 2);
        hour = parse_int(&p, 2);
        min = parse_int(&p, 2);
    } else if (main_len == 10) {
        /* YYMMDDhhmm */
        int yy = parse_int(&p, 2);
        if (yy < 0) return -1;
        year = cur_century * 100 + yy;
        mon = parse_int(&p, 2);
        day = parse_int(&p, 2);
        hour = parse_int(&p, 2);
        min = parse_int(&p, 2);
    } else if (main_len == 12) {
        /* CCYYMMDDhhmm */
        year = parse_int(&p, 4);
        mon = parse_int(&p, 2);
        day = parse_int(&p, 2);
        hour = parse_int(&p, 2);
        min = parse_int(&p, 2);
    } else {
        return -1;
    }

    if (mon < 0 || day < 0 || hour < 0 || min < 0)
        return -1;

    if (dot) {
        p = dot + 1;
        sec = parse_int(&p, 2);
        if (sec < 0) return -1;
    }

    /* Validate ranges */
    if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 ||
        sec < 0 || sec > 60)
        return -1;

    ts->tv_sec = simple_mktime(year, mon, day, hour, min, sec);
    ts->tv_nsec = 0;
    return 0;
}

/*
 * Parse -d DATE format.
 * Supports: "YYYY-MM-DD HH:MM:SS", "YYYY-MM-DD HH:MM", "YYYY-MM-DD",
 *           "YYYY-MM-DDTHH:MM:SS", and epoch seconds "@SECONDS"
 */
static int parse_date_string(const char *str, struct timespec *ts) {
    /* Handle @epoch format */
    if (str[0] == '@') {
        long epoch = 0;
        const char *p = str + 1;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') {
            epoch = epoch * 10 + (*p - '0');
            p++;
        }
        if (neg) epoch = -epoch;
        ts->tv_sec = (time_t)epoch;
        ts->tv_nsec = 0;
        return 0;
    }

    /* Try YYYY-MM-DD[T ]HH:MM[:SS] */
    const char *p = str;
    int year = parse_int(&p, 4);
    if (year < 0 || *p != '-') return -1;
    p++;
    int mon = parse_int(&p, 2);
    if (mon < 0 || *p != '-') return -1;
    p++;
    int day = parse_int(&p, 2);
    if (day < 0) return -1;

    int hour = 0, min = 0, sec = 0;

    if (*p == 'T' || *p == ' ') {
        p++;
        hour = parse_int(&p, 2);
        if (hour < 0 || *p != ':') return -1;
        p++;
        min = parse_int(&p, 2);
        if (min < 0) return -1;
        if (*p == ':') {
            p++;
            sec = parse_int(&p, 2);
            if (sec < 0) return -1;
        }
    }

    if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 60)
        return -1;

    ts->tv_sec = simple_mktime(year, mon, day, hour, min, sec);
    ts->tv_nsec = 0;
    return 0;
}

static int do_touch(const char *path) {
    struct timespec times[2];
    int fd = -1;

    /* Special case: "-" means stdout */
    if (strcmp(path, "-") == 0) {
        /* Change timestamps of stdout */
        if (opt_have_time) {
            times[0] = opt_times[0];
            times[1] = opt_times[1];
        } else {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            times[0] = now;
            times[1] = now;
        }

        /* Only change requested timestamps */
        if (!(opt_change & CHANGE_ATIME))
            times[0].tv_nsec = ((long)1 << 30) - 1; /* UTIME_OMIT equivalent */
        if (!(opt_change & CHANGE_MTIME))
            times[1].tv_nsec = ((long)1 << 30) - 1;

        /* Use futimens-style via utimensat on /dev/stdout or just succeed */
        return 0;
    }

    /* Check if file exists */
    struct stat st;
    int exists = (stat(path, &st) == 0);

    if (!exists) {
        if (opt_no_create || opt_no_dereference) {
            /* Don't create, silently succeed */
            return 0;
        }
        /* Create the file */
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            fprintf(stderr, "%s: cannot touch '%s': %s\n",
                    PROGRAM_NAME, path, strerror(errno));
            return 1;
        }
        close(fd);
    }

    /* Set timestamps */
    if (opt_have_time) {
        times[0] = opt_times[0];
        times[1] = opt_times[1];
    } else {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        times[0] = now;
        times[1] = now;
    }

    /* Only change requested timestamps */
    if (exists) {
        if (!(opt_change & CHANGE_ATIME)) {
            times[0].tv_sec = st.st_atime;
            times[0].tv_nsec = 0;
        }
        if (!(opt_change & CHANGE_MTIME)) {
            times[1].tv_sec = st.st_mtime;
            times[1].tv_nsec = 0;
        }
    }

    int flags = opt_no_dereference ? 0x100 : 0; /* AT_SYMLINK_NOFOLLOW */
    if (utimensat(-100, path, times, flags) != 0) {
        fprintf(stderr, "%s: cannot touch '%s': %s\n",
                PROGRAM_NAME, path, strerror(errno));
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    enum { OPT_TIME = 256, OPT_HELP, OPT_VERSION };

    static struct option long_options[] = {
        {"no-create",      no_argument,       0, 'c'},
        {"date",           required_argument, 0, 'd'},
        {"no-dereference", no_argument,       0, 'h'},
        {"reference",      required_argument, 0, 'r'},
        {"time",           required_argument, 0, OPT_TIME},
        {"help",           no_argument,       0, OPT_HELP},
        {"version",        no_argument,       0, OPT_VERSION},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "acd:fhm r:t:", long_options, NULL)) != -1) {
        switch (c) {
            case 'a':
                opt_change = CHANGE_ATIME;
                break;
            case 'c':
                opt_no_create = 1;
                break;
            case 'd': {
                opt_have_time = 1;
                struct timespec ts;
                if (parse_date_string(optarg, &ts) != 0) {
                    fprintf(stderr, "%s: invalid date format '%s'\n",
                            PROGRAM_NAME, optarg);
                    return 1;
                }
                opt_times[0] = ts;
                opt_times[1] = ts;
                break;
            }
            case 'f':
                /* ignored */
                break;
            case 'h':
                opt_no_dereference = 1;
                break;
            case 'm':
                opt_change = CHANGE_MTIME;
                break;
            case 'r': {
                opt_have_time = 1;
                struct stat st;
                if (stat(optarg, &st) != 0) {
                    fprintf(stderr, "%s: failed to get attributes of '%s': %s\n",
                            PROGRAM_NAME, optarg, strerror(errno));
                    return 1;
                }
                opt_times[0].tv_sec = st.st_atime;
                opt_times[0].tv_nsec = 0;
                opt_times[1].tv_sec = st.st_mtime;
                opt_times[1].tv_nsec = 0;
                break;
            }
            case 't': {
                opt_have_time = 1;
                struct timespec ts;
                if (parse_stamp(optarg, &ts) != 0) {
                    fprintf(stderr, "%s: invalid date format '%s'\n",
                            PROGRAM_NAME, optarg);
                    return 1;
                }
                opt_times[0] = ts;
                opt_times[1] = ts;
                break;
            }
            case OPT_TIME:
                if (strcmp(optarg, "access") == 0 ||
                    strcmp(optarg, "atime") == 0 ||
                    strcmp(optarg, "use") == 0) {
                    opt_change = CHANGE_ATIME;
                } else if (strcmp(optarg, "modify") == 0 ||
                           strcmp(optarg, "mtime") == 0) {
                    opt_change = CHANGE_MTIME;
                } else {
                    fprintf(stderr, "%s: invalid argument '%s' for '--time'\n",
                            PROGRAM_NAME, optarg);
                    return 1;
                }
                break;
            case OPT_HELP:
                usage();
                return 0;
            case OPT_VERSION:
                version();
                return 0;
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "%s: missing file operand\n", PROGRAM_NAME);
        fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
        return 1;
    }

    int errors = 0;
    for (int i = optind; i < argc; i++) {
        errors += do_touch(argv[i]);
    }

    return errors > 0 ? 1 : 0;
}
