/*
 * sleep - delay for a specified amount of time
 *
 * Usage: sleep NUMBER[SUFFIX]...
 *   SUFFIX may be 's' for seconds (default), 'm' for minutes,
 *   'h' for hours, or 'd' for days.
 *   Multiple arguments are summed.
 *   Fractional numbers are supported (e.g. sleep 0.5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "sleep"
#define VERSION      "1.0"

static void usage(int status)
{
    if (status != EXIT_SUCCESS) {
        fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
    } else {
        printf("Usage: %s NUMBER[SUFFIX]...\n"
               "  or:  %s OPTION\n"
               "Pause for NUMBER seconds.  SUFFIX may be 's' for seconds (the default),\n"
               "'m' for minutes, 'h' for hours or 'd' for days.  NUMBER need not be an\n"
               "integer.  Given two or more arguments, pause for the amount of time\n"
               "specified by the sum of their values.\n\n"
               "      --help     display this help and exit\n"
               "      --version  output version information and exit\n",
               PROGRAM_NAME, PROGRAM_NAME);
    }
    exit(status);
}

static void version(void)
{
    printf("%s (%s) %s\n", PROGRAM_NAME, "LikeOS coreutils", VERSION);
    exit(EXIT_SUCCESS);
}

/*
 * Parse a decimal number string manually (integer + fractional part)
 * Returns seconds and nanoseconds.
 * Returns 0 on success, -1 on failure.
 */
static int parse_duration(const char *s, long *sec_out, long *nsec_out)
{
    if (!s || !*s)
        return -1;

    int negative = 0;
    if (*s == '-') {
        negative = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    /* Parse integer part */
    long sec = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        sec = sec * 10 + (*p - '0');
        p++;
    }

    long nsec = 0;
    if (*p == '.' || *p == ',') {
        p++;
        /* Parse fractional part - up to 9 digits for nanoseconds */
        long frac = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 9) {
            frac = frac * 10 + (*p - '0');
            digits++;
            p++;
        }
        /* Skip remaining digits */
        while (*p >= '0' && *p <= '9')
            p++;
        /* Pad to 9 digits */
        while (digits < 9) {
            frac *= 10;
            digits++;
        }
        nsec = frac;
    }

    /* Check suffix */
    long multiplier = 1;
    if (*p == 's' || *p == '\0') {
        multiplier = 1;
        if (*p == 's') p++;
    } else if (*p == 'm') {
        multiplier = 60;
        p++;
    } else if (*p == 'h') {
        multiplier = 3600;
        p++;
    } else if (*p == 'd') {
        multiplier = 86400;
        p++;
    } else {
        return -1;
    }

    if (*p != '\0')
        return -1;

    if (negative)
        return -1; /* sleep doesn't accept negative */

    /* Apply multiplier */
    /* nsec * multiplier might overflow 9-digit range, handle carry */
    long total_nsec = nsec * multiplier;
    long extra_sec = total_nsec / 1000000000L;
    total_nsec = total_nsec % 1000000000L;

    *sec_out = sec * multiplier + extra_sec;
    *nsec_out = total_nsec;
    return 0;
}

int main(int argc, char **argv)
{
    /* Handle --help and --version before anything else */
    static struct option long_options[] = {
        {"help",    no_argument, 0, 'H'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0}
    };

    int c;
    /* Reset getopt */
    optind = 1;
    while ((c = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (c) {
        case 'H':
            usage(EXIT_SUCCESS);
            break;
        case 'V':
            version();
            break;
        default:
            usage(EXIT_FAILURE);
            break;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "%s: missing operand\n", PROGRAM_NAME);
        usage(EXIT_FAILURE);
    }

    /* Sum all duration arguments */
    long total_sec = 0;
    long total_nsec = 0;

    for (int i = optind; i < argc; i++) {
        long s, ns;
        if (parse_duration(argv[i], &s, &ns) != 0) {
            fprintf(stderr, "%s: invalid time interval '%s'\n", PROGRAM_NAME, argv[i]);
            return EXIT_FAILURE;
        }
        total_sec += s;
        total_nsec += ns;
        if (total_nsec >= 1000000000L) {
            total_sec += total_nsec / 1000000000L;
            total_nsec = total_nsec % 1000000000L;
        }
    }

    struct timespec req;
    req.tv_sec = total_sec;
    req.tv_nsec = total_nsec;

    while (nanosleep(&req, &req) == -1) {
        if (errno != EINTR)
            break;
        /* Interrupted by signal, continue sleeping the remainder */
    }

    return EXIT_SUCCESS;
}
