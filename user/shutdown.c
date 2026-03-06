/*
 * shutdown - Halt, power off or reboot the machine
 *
 * Usage: shutdown [OPTIONS...] [TIME] [WALL...]
 *
 * Implements the shutdown(8) command as per systemd manpage.
 *
 * TIME may be:
 *   "now"   — immediate shutdown (alias for "+0")
 *   "+m"    — m minutes from now
 *   "hh:mm" — absolute wall-clock time (24 h format, today or tomorrow)
 *
 * If no TIME is given, "+1" is implied.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/reboot.h>

/* Pending shutdown state file */
#define SCHEDULED_FILE "/tmp/.shutdown_scheduled"

enum action { ACTION_POWEROFF, ACTION_HALT, ACTION_REBOOT };

static const char *action_name(enum action a)
{
    switch (a) {
    case ACTION_HALT:     return "halt";
    case ACTION_REBOOT:   return "reboot";
    default:              return "poweroff";
    }
}

static const char *action_past(enum action a)
{
    switch (a) {
    case ACTION_HALT:     return "halted";
    case ACTION_REBOOT:   return "rebooted";
    default:              return "powered off";
    }
}

static void print_help(void)
{
    printf("shutdown [OPTIONS...] [TIME] [WALL...]\n\n");
    printf("Shut down the system.\n\n");
    printf("Options:\n");
    printf("     --help      Show this help\n");
    printf("  -H, --halt     Halt the machine\n");
    printf("  -P, --poweroff Power-off the machine (default)\n");
    printf("  -r, --reboot   Reboot the machine\n");
    printf("  -h             Equivalent to --poweroff unless overridden by --halt\n");
    printf("  -k             Don't halt/power-off/reboot, just write wall message\n");
    printf("     --no-wall   Don't send wall message before halt/power-off/reboot\n");
    printf("  -c             Cancel a pending shutdown\n");
    printf("     --show      Show pending shutdown action and time\n");
}

/*
 * Parse a time string and return the number of seconds from now.
 *   "now"   →  0
 *   "+N"    →  N * 60
 *   "hh:mm" → seconds until that wall-clock time (wraps to tomorrow if past)
 * Returns seconds >= 0 on success, -1 on error.
 */
static long parse_time(const char *s)
{
    if (!s)
        return 60; /* default: +1 minute */

    if (strcmp(s, "now") == 0)
        return 0;

    /* "+N" minutes from now */
    if (s[0] == '+') {
        long m = 0;
        for (const char *p = s + 1; *p; p++) {
            if (*p < '0' || *p > '9')
                return -1;
            m = m * 10 + (*p - '0');
        }
        return m * 60;
    }

    /* "hh:mm" absolute wall-clock time */
    {
        int hh = 0, mm = 0;
        const char *p = s;
        while (*p >= '0' && *p <= '9') {
            hh = hh * 10 + (*p - '0');
            p++;
        }
        if (*p != ':')
            return -1;
        p++;
        while (*p >= '0' && *p <= '9') {
            mm = mm * 10 + (*p - '0');
            p++;
        }
        if (*p != '\0')
            return -1;
        if (hh > 23 || mm > 59)
            return -1;

        /* Get current wall-clock time */
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        /* Build target time for today */
        struct tm tm_target = tm_now;
        tm_target.tm_hour = hh;
        tm_target.tm_min  = mm;
        tm_target.tm_sec  = 0;
        time_t target = mktime(&tm_target);

        /* If the target time has already passed today, schedule for tomorrow */
        if (target <= now) {
            tm_target.tm_mday += 1;
            target = mktime(&tm_target); /* mktime normalises overflow */
        }

        long diff = (long)(target - now);
        return diff > 0 ? diff : 0;
    }
}

/*
 * Write schedule file so "--show" and "-c" can work.
 * Format: "action=<name>\nwhen=<epoch>\n"
 */
static void write_schedule(enum action a, time_t when)
{
    int fd = open(SCHEDULED_FILE, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "action=%s\nwhen=%ld\n",
                     action_name(a), (long)when);
    if (n > 0)
        write(fd, buf, (size_t)n);
    close(fd);
}

/*
 * Read the schedule file and print its contents for "--show".
 */
static void show_schedule(void)
{
    int fd = open(SCHEDULED_FILE, 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        printf("No scheduled shutdown.\n");
        return;
    }
    char buf[256];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("Shutdown scheduled (details unavailable).\n");
        return;
    }
    buf[n] = '\0';

    /* Parse "action=<str>\nwhen=<epoch>\n" */
    const char *act = "unknown";
    long when_epoch = 0;

    char *line = buf;
    while (*line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        if (strncmp(line, "action=", 7) == 0) {
            act = line + 7;
        } else if (strncmp(line, "when=", 5) == 0) {
            const char *q = line + 5;
            int neg = 0;
            if (*q == '-') { neg = 1; q++; }
            while (*q >= '0' && *q <= '9')
                when_epoch = when_epoch * 10 + (*q++ - '0');
            if (neg) when_epoch = -when_epoch;
        }

        if (!eol) break;
        line = eol + 1;
    }

    /* Format the scheduled time */
    time_t wt = (time_t)when_epoch;
    struct tm tm_when;
    localtime_r(&wt, &tm_when);

    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S UTC", &tm_when);

    time_t now = time(NULL);
    long remaining = (long)(wt - now);

    printf("Shutdown scheduled, use 'shutdown -c' to cancel.\n");
    printf("   Action: %s\n", act);
    printf("   When:   %s\n", timebuf);
    if (remaining > 0) {
        long rm = remaining / 60;
        long rs = remaining % 60;
        printf("   In:     %ld min %ld sec\n", rm, rs);
    } else {
        printf("   In:     imminent\n");
    }
}

int main(int argc, char *argv[])
{
    enum action action = ACTION_POWEROFF;
    int just_wall = 0;    /* -k */
    int no_wall = 0;      /* --no-wall */
    int cancel = 0;       /* -c */
    int show = 0;         /* --show */
    int explicit_h = 0;   /* saw -h without prior --halt */
    int saw_halt = 0;
    const char *time_arg = NULL;
    char wall_msg[512] = {0};
    int wall_off = 0;

    /* Parse options and positional args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        } else if (strcmp(argv[i], "--halt") == 0) {
            action = ACTION_HALT;
            saw_halt = 1;
        } else if (strcmp(argv[i], "--poweroff") == 0) {
            action = ACTION_POWEROFF;
        } else if (strcmp(argv[i], "--reboot") == 0) {
            action = ACTION_REBOOT;
        } else if (strcmp(argv[i], "--no-wall") == 0) {
            no_wall = 1;
        } else if (strcmp(argv[i], "--show") == 0) {
            show = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '-') {
            for (const char *p = argv[i] + 1; *p; p++) {
                switch (*p) {
                case 'H': action = ACTION_HALT; saw_halt = 1; break;
                case 'P': action = ACTION_POWEROFF; break;
                case 'r': action = ACTION_REBOOT; break;
                case 'h': explicit_h = 1; break;
                case 'k': just_wall = 1; break;
                case 'c': cancel = 1; break;
                default:
                    fprintf(stderr, "shutdown: invalid option -- '%c'\n", *p);
                    fprintf(stderr, "Try 'shutdown --help' for more information.\n");
                    return 1;
                }
            }
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "shutdown: unrecognized option '%s'\n", argv[i]);
            return 1;
        } else {
            /* First non-option is TIME, rest is WALL message */
            if (time_arg == NULL) {
                time_arg = argv[i];
            } else {
                if (wall_off > 0 && wall_off < (int)sizeof(wall_msg) - 1)
                    wall_msg[wall_off++] = ' ';
                size_t len = strlen(argv[i]);
                if (wall_off + (int)len < (int)sizeof(wall_msg) - 1) {
                    memcpy(wall_msg + wall_off, argv[i], len);
                    wall_off += (int)len;
                }
            }
        }
    }
    wall_msg[wall_off] = '\0';

    /*
     * -h means poweroff, unless --halt was given before it.
     * "shutdown --halt -h" means halt.
     * "shutdown --reboot -h" means poweroff.
     */
    if (explicit_h && !saw_halt)
        action = ACTION_POWEROFF;

    /* --show */
    if (show) {
        show_schedule();
        return 0;
    }

    /* -c (cancel) */
    if (cancel) {
        if (access(SCHEDULED_FILE, 0) == 0) {
            unlink(SCHEDULED_FILE);
            if (!no_wall)
                printf("System shutdown has been cancelled.\n");
        } else {
            fprintf(stderr, "No scheduled shutdown to cancel.\n");
        }
        return 0;
    }

    /* Parse time argument (returns seconds from now, or -1 on error) */
    long delay_secs = parse_time(time_arg);
    if (delay_secs < 0) {
        fprintf(stderr, "shutdown: invalid time argument '%s'\n", time_arg);
        return 1;
    }

    /* Non-immediate: record schedule, print info, and wait */
    if (delay_secs > 0) {
        time_t when = time(NULL) + delay_secs;
        write_schedule(action, when);

        if (!no_wall) {
            struct tm tm_when;
            localtime_r(&when, &tm_when);
            char timebuf[64];
            strftime(timebuf, sizeof(timebuf), "%H:%M:%S UTC", &tm_when);
            printf("Shutdown scheduled for %s, use 'shutdown -c' to cancel.\n",
                   timebuf);
            if (wall_msg[0])
                printf("Wall message: %s\n", wall_msg);
        }

        /* Sleep until the target time */
        while (delay_secs > 0) {
            unsigned int chunk = delay_secs > 60 ? 60 : (unsigned int)delay_secs;
            sleep(chunk);
            delay_secs -= chunk;

            /* Check if cancelled (schedule file removed) */
            if (access(SCHEDULED_FILE, 0) != 0) {
                printf("Shutdown cancelled.\n");
                return 0;
            }
        }

        unlink(SCHEDULED_FILE);
    }

    /* Wall message */
    if (!no_wall) {
        printf("The system is going to be %s NOW!\n", action_past(action));
        if (wall_msg[0])
            printf("Wall message: %s\n", wall_msg);
    }

    /* -k: only wall message, don't actually shut down */
    if (just_wall)
        return 0;

    /* Perform the shutdown */
    sync();

    switch (action) {
    case ACTION_HALT:
        reboot(RB_HALT_SYSTEM);
        break;
    case ACTION_REBOOT:
        reboot(RB_AUTOBOOT);
        break;
    case ACTION_POWEROFF:
    default:
        reboot(RB_POWER_OFF);
        break;
    }

    /* Should not reach here */
    fprintf(stderr, "shutdown: reboot syscall failed\n");
    return 1;
}
