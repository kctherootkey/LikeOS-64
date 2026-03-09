/*
 * uptime - Tell how long the system has been running
 *
 * Usage: uptime [options]
 *
 * Full implementation per the uptime(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <sys/sysinfo.h>

#define UPTIME_VERSION "uptime (LikeOS procps) 0.1"

static void print_help(void)
{
    printf("Usage:\n uptime [options]\n\n");
    printf("Options:\n");
    printf(" -p, --pretty   show uptime in pretty format\n");
    printf(" -h, --help     display this help and exit\n");
    printf(" -s, --since    system up since, in yyyy-mm-dd HH:MM:SS format\n");
    printf(" -V, --version  output version information and exit\n");
}

static void print_pretty_uptime(long uptime_secs)
{
    int days = (int)(uptime_secs / 86400);
    uptime_secs %= 86400;
    int hours = (int)(uptime_secs / 3600);
    uptime_secs %= 3600;
    int minutes = (int)(uptime_secs / 60);

    printf("up ");
    int printed = 0;

    if (days > 0) {
        printf("%d day%s", days, days != 1 ? "s" : "");
        printed = 1;
    }
    if (hours > 0) {
        if (printed) printf(", ");
        printf("%d hour%s", hours, hours != 1 ? "s" : "");
        printed = 1;
    }
    if (minutes > 0) {
        if (printed) printf(", ");
        printf("%d minute%s", minutes, minutes != 1 ? "s" : "");
    }
    if (!printed && minutes == 0) {
        printf("0 minutes");
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    int pretty_mode = 0;
    int since_mode = 0;

    static struct option long_options[] = {
        {"pretty",  no_argument, 0, 'p'},
        {"since",   no_argument, 0, 's'},
        {"help",    no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "pshV", long_options, NULL)) != -1) {
        switch (opt) {
        case 'p': pretty_mode = 1; break;
        case 's': since_mode = 1; break;
        case 'h': print_help(); return 0;
        case 'V': printf("%s\n", UPTIME_VERSION); return 0;
        default:
            fprintf(stderr, "Try 'uptime --help' for more information.\n");
            return 1;
        }
    }

    struct sysinfo info;
    if (sysinfo(&info) < 0) {
        fprintf(stderr, "uptime: cannot get system information\n");
        return 1;
    }

    if (since_mode) {
        /* Show boot time */
        time_t now = time(NULL);
        time_t boot_time = now - info.uptime;
        struct tm tm_val;
        gmtime_r(&boot_time, &tm_val);
        printf("%04d-%02d-%02d %02d:%02d:%02d\n",
               tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
               tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
        return 0;
    }

    if (pretty_mode) {
        print_pretty_uptime(info.uptime);
        return 0;
    }

    /* Default output: current time, uptime, users, load averages */
    time_t now = time(NULL);
    struct tm tm_val;
    gmtime_r(&now, &tm_val);

    /* Current time */
    printf(" %02d:%02d:%02d ", tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);

    /* Uptime */
    long up = info.uptime;
    int days = (int)(up / 86400);
    up %= 86400;
    int hours = (int)(up / 3600);
    up %= 3600;
    int mins = (int)(up / 60);

    printf("up ");
    if (days > 0) {
        printf("%d day%s, ", days, days != 1 ? "s" : "");
    }
    if (hours > 0) {
        printf("%2d:%02d, ", hours, mins);
    } else {
        printf("%d min, ", mins);
    }

    /* Number of users (always 1 in this OS) */
    printf(" %d user, ", 1);

    /* Load averages (stored as fixed-point with 16-bit fraction) */
    double load1 = (double)info.loads[0] / 65536.0;
    double load5 = (double)info.loads[1] / 65536.0;
    double load15 = (double)info.loads[2] / 65536.0;

    printf(" load average: %.2f, %.2f, %.2f\n", load1, load5, load15);

    return 0;
}
