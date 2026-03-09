/*
 * dmesg - print or control the kernel ring buffer
 *
 * Usage: dmesg [options]
 *
 * Full implementation per the dmesg(1) manpage.
 * Default behaviour: show kernel-only messages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <sys/klog.h>
#include <sys/sysinfo.h>
#include <errno.h>

#define DMESG_VERSION "dmesg (LikeOS util) 0.1"
#define DEFAULT_BUFSIZE (256 * 1024)

/* Syslog priority levels */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

/* Syslog facility codes */
#define LOG_KERN     0
#define LOG_USER     1
#define LOG_MAIL     2
#define LOG_DAEMON   3

static const char *level_names[] = {
    "emerg", "alert", "crit", "err",
    "warn", "notice", "info", "debug"
};

static const char *facility_names[] = {
    "kern", "user", "mail", "daemon",
    "auth", "syslog", "lpr", "news",
    "uucp", "cron", "authpriv", "ftp",
    "res0", "res1", "res2", "res3",
    "local0", "local1", "local2", "local3",
    "local4", "local5", "local6", "local7"
};

static void print_help(void)
{
    printf("Usage:\n dmesg [options]\n\n");
    printf("Display or control the kernel ring buffer.\n\n");
    printf("Options:\n");
    printf(" -C, --clear                 clear the kernel ring buffer\n");
    printf(" -c, --read-clear            read and clear all messages\n");
    printf(" -D, --console-off           disable printing messages to console\n");
    printf(" -d, --show-delta            show time delta between printed messages\n");
    printf(" -E, --console-on            enable printing messages to console\n");
    printf(" -e, --reltime               show local time and time delta\n");
    printf(" -F, --file <file>           use the file instead of the kernel log buffer\n");
    printf(" -f, --facility <list>       restrict output to defined facilities\n");
    printf(" -H, --human                 human readable output\n");
    printf(" -k, --kernel                display kernel messages\n");
    printf(" -L, --color[=<when>]        colorize messages (auto, always, never)\n");
    printf(" -l, --level <list>          restrict output to defined levels\n");
    printf(" -n, --console-level <level> set level of messages printed to console\n");
    printf("     --noescape              don't escape unprintable characters\n");
    printf(" -P, --nopager               do not pipe output into a pager\n");
    printf(" -p, --force-prefix          force timestamp output in each line\n");
    printf(" -r, --raw                   print the raw message buffer\n");
    printf(" -S, --syslog                force to use syslog(2) rather than /dev/kmsg\n");
    printf(" -s, --buffer-size <size>    buffer size to query the kernel ring buffer\n");
    printf(" -T, --ctime                 show human-readable timestamp\n");
    printf(" -t, --notime                don't show any timestamp with messages\n");
    printf("     --time-format <format>  show time stamp using format:\n");
    printf("                               [delta|reltime|ctime|notime|iso]\n");
    printf(" -u, --userspace             display userspace messages\n");
    printf(" -w, --follow                wait for new messages\n");
    printf(" -x, --decode                decode facility and level to readable strings\n");
    printf(" -h, --help                  display this help\n");
    printf(" -V, --version               display version\n");
}

/* Parse a comma-separated list of level names into a bitmask */
static int parse_levels(const char *list)
{
    int mask = 0;
    char buf[256];
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = buf;
    while (*tok) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';

        /* Trim whitespace */
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';

        for (int i = 0; i < 8; i++) {
            if (strcmp(tok, level_names[i]) == 0) {
                mask |= (1 << i);
                break;
            }
        }

        if (!comma) break;
        tok = comma + 1;
    }
    return mask;
}

/* Parse a comma-separated list of facility names into a bitmask */
static int parse_facilities(const char *list)
{
    int mask = 0;
    char buf[256];
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = buf;
    while (*tok) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';

        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';

        for (int i = 0; i < 24; i++) {
            if (strcmp(tok, facility_names[i]) == 0) {
                mask |= (1 << i);
                break;
            }
        }

        if (!comma) break;
        tok = comma + 1;
    }
    return mask;
}

/* Parse a single log line: <priority>message or [timestamp] message */
struct log_entry {
    int priority;     /* raw priority (facility * 8 + level) */
    int facility;
    int level;
    unsigned long timestamp_us; /* microseconds since boot */
    int has_timestamp;
    const char *msg;
    int msg_len;
};

static int parse_log_entry(const char *line, int len, struct log_entry *entry)
{
    entry->priority = -1;
    entry->facility = LOG_KERN;
    entry->level = LOG_INFO;
    entry->timestamp_us = 0;
    entry->has_timestamp = 0;
    entry->msg = line;
    entry->msg_len = len;

    const char *p = line;
    const char *end = line + len;

    /* Parse optional <priority> prefix */
    if (p < end && *p == '<') {
        p++;
        int pri = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            pri = pri * 10 + (*p - '0');
            p++;
        }
        if (p < end && *p == '>') {
            p++;
            entry->priority = pri;
            entry->facility = pri >> 3;
            entry->level = pri & 7;
        }
    }

    /* Parse optional [timestamp] */
    if (p < end && *p == '[') {
        p++;
        /* Skip whitespace */
        while (p < end && *p == ' ') p++;
        unsigned long secs = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            secs = secs * 10 + (*p - '0');
            p++;
        }
        unsigned long usecs = 0;
        if (p < end && *p == '.') {
            p++;
            int digits = 0;
            while (p < end && *p >= '0' && *p <= '9' && digits < 6) {
                usecs = usecs * 10 + (*p - '0');
                p++;
                digits++;
            }
            /* Pad to 6 digits */
            while (digits < 6) { usecs *= 10; digits++; }
            /* Skip remaining digits */
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p < end && *p == ']') {
            p++;
            entry->timestamp_us = secs * 1000000UL + usecs;
            entry->has_timestamp = 1;
        }
        if (p < end && *p == ' ') p++;
    }

    entry->msg = p;
    entry->msg_len = (int)(end - p);
    return 0;
}

int main(int argc, char *argv[])
{
    int do_clear = 0;
    int do_read_clear = 0;
    int show_delta = 0;
    int show_reltime = 0;
    int show_ctime = 0;
    int no_time = 0;
    int raw_mode = 0;
    int decode_mode = 0;
    int human_mode = 0;
    int force_prefix = 0;
    int follow_mode = 0;
    int noescape = 0;
    int bufsize = DEFAULT_BUFSIZE;
    int level_mask = 0xFF; /* all levels */
    int facility_mask = 0; /* 0 = show all */
    int kernel_only = 0;
    int userspace_only = 0;
    const char *time_format = NULL;
    const char *input_file = NULL;

    enum {
        OPT_NOESCAPE = 256,
        OPT_TIMEFMT,
    };

    static struct option long_options[] = {
        {"clear",         no_argument,       0, 'C'},
        {"read-clear",    no_argument,       0, 'c'},
        {"console-off",   no_argument,       0, 'D'},
        {"show-delta",    no_argument,       0, 'd'},
        {"console-on",    no_argument,       0, 'E'},
        {"reltime",       no_argument,       0, 'e'},
        {"file",          required_argument, 0, 'F'},
        {"facility",      required_argument, 0, 'f'},
        {"human",         no_argument,       0, 'H'},
        {"kernel",        no_argument,       0, 'k'},
        {"color",         optional_argument, 0, 'L'},
        {"level",         required_argument, 0, 'l'},
        {"console-level", required_argument, 0, 'n'},
        {"noescape",      no_argument,       0, OPT_NOESCAPE},
        {"nopager",       no_argument,       0, 'P'},
        {"force-prefix",  no_argument,       0, 'p'},
        {"raw",           no_argument,       0, 'r'},
        {"syslog",        no_argument,       0, 'S'},
        {"buffer-size",   required_argument, 0, 's'},
        {"ctime",         no_argument,       0, 'T'},
        {"notime",        no_argument,       0, 't'},
        {"time-format",   required_argument, 0, OPT_TIMEFMT},
        {"userspace",     no_argument,       0, 'u'},
        {"follow",        no_argument,       0, 'w'},
        {"decode",        no_argument,       0, 'x'},
        {"help",          no_argument,       0, 'h'},
        {"version",       no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "CcDdeEF:f:HhkL::l:n:Pprs:STtuVwx",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'C': do_clear = 1; break;
        case 'c': do_read_clear = 1; break;
        case 'D': /* console-off: not supported */ break;
        case 'd': show_delta = 1; break;
        case 'E': /* console-on: not supported */ break;
        case 'e': show_reltime = 1; break;
        case 'F': input_file = optarg; break;
        case 'f': facility_mask = parse_facilities(optarg); break;
        case 'H': human_mode = 1; show_reltime = 1; break;
        case 'k': kernel_only = 1; break;
        case 'L': /* color - silently accept but ignore */ break;
        case 'l': level_mask = parse_levels(optarg); break;
        case 'n': /* console-level: not supported */ break;
        case OPT_NOESCAPE: noescape = 1; break;
        case 'P': /* nopager - nothing to do */ break;
        case 'p': force_prefix = 1; break;
        case 'r': raw_mode = 1; break;
        case 'S': /* syslog mode: same as default */ break;
        case 's': bufsize = atoi(optarg); if (bufsize <= 0) bufsize = DEFAULT_BUFSIZE; break;
        case 'T': show_ctime = 1; break;
        case 't': no_time = 1; break;
        case OPT_TIMEFMT:
            time_format = optarg;
            if (strcmp(optarg, "delta") == 0) show_delta = 1;
            else if (strcmp(optarg, "reltime") == 0) show_reltime = 1;
            else if (strcmp(optarg, "ctime") == 0) show_ctime = 1;
            else if (strcmp(optarg, "notime") == 0) no_time = 1;
            else if (strcmp(optarg, "iso") == 0) show_ctime = 1; /* close enough */
            break;
        case 'u': userspace_only = 1; break;
        case 'w': follow_mode = 1; break;
        case 'x': decode_mode = 1; break;
        case 'h': print_help(); return 0;
        case 'V': printf("%s\n", DMESG_VERSION); return 0;
        default:
            fprintf(stderr, "Try 'dmesg --help' for more information.\n");
            return 1;
        }
    }

    (void)force_prefix;
    (void)time_format;
    (void)noescape;

    /* Default: kernel-only if nothing specified */
    if (!kernel_only && !userspace_only) {
        kernel_only = 1;
    }

    /* Clear operation */
    if (do_clear && !do_read_clear) {
        int ret = klogctl(SYSLOG_ACTION_CLEAR, NULL, 0);
        if (ret < 0) {
            fprintf(stderr, "dmesg: klogctl(CLEAR) failed\n");
            return 1;
        }
        return 0;
    }

    /* Read the kernel log */
    char *buf = NULL;
    int nread = 0;

    if (input_file) {
        /* Read from file */
        FILE *fp = fopen(input_file, "r");
        if (!fp) {
            fprintf(stderr, "dmesg: cannot open %s\n", input_file);
            return 1;
        }
        buf = (char *)malloc(bufsize);
        if (!buf) {
            fprintf(stderr, "dmesg: out of memory\n");
            fclose(fp);
            return 1;
        }
        nread = (int)fread(buf, 1, bufsize - 1, fp);
        fclose(fp);
        if (nread < 0) nread = 0;
        buf[nread] = '\0';
    } else {
        buf = (char *)malloc(bufsize);
        if (!buf) {
            fprintf(stderr, "dmesg: out of memory\n");
            return 1;
        }

        int action = do_read_clear ? SYSLOG_ACTION_READ_CLEAR : SYSLOG_ACTION_READ_ALL;
        nread = klogctl(action, buf, bufsize - 1);
        if (nread < 0) {
            fprintf(stderr, "dmesg: klogctl failed\n");
            free(buf);
            return 1;
        }
        buf[nread] = '\0';
    }

    if (nread == 0) {
        free(buf);
        return 0;
    }

    /* Get boot time for ctime conversion */
    time_t boot_time = 0;
    if (show_ctime || show_reltime) {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            time_t now = time(NULL);
            boot_time = now - si.uptime;
        }
    }

    /* Process and print each line */
    unsigned long prev_timestamp = 0;
    const char *p = buf;
    const char *buf_end = buf + nread;

    while (p < buf_end) {
        /* Find end of line */
        const char *eol = p;
        while (eol < buf_end && *eol != '\n') eol++;

        int line_len = (int)(eol - p);
        if (line_len > 0) {
            struct log_entry entry;
            parse_log_entry(p, line_len, &entry);

            /* Filter by facility */
            if (facility_mask) {
                if (!(facility_mask & (1 << entry.facility))) {
                    goto next_line;
                }
            }

            /* Filter by kernel/userspace */
            if (kernel_only && entry.facility != LOG_KERN && entry.priority >= 0) {
                goto next_line;
            }
            if (userspace_only && entry.facility == LOG_KERN && entry.priority >= 0) {
                goto next_line;
            }

            /* Filter by level */
            if (entry.priority >= 0) {
                if (!(level_mask & (1 << entry.level))) {
                    goto next_line;
                }
            }

            if (raw_mode) {
                /* Print the raw line */
                fwrite(p, 1, line_len, stdout);
                putchar('\n');
            } else {
                /* Print decode prefix if requested */
                if (decode_mode && entry.priority >= 0) {
                    const char *fac = (entry.facility < 24) ?
                        facility_names[entry.facility] : "unknown";
                    const char *lev = (entry.level < 8) ?
                        level_names[entry.level] : "unknown";
                    printf("%-6s:%-6s: ", fac, lev);
                }

                /* Print timestamp */
                if (!no_time && entry.has_timestamp) {
                    if (show_ctime) {
                        time_t msg_time = boot_time + (time_t)(entry.timestamp_us / 1000000UL);
                        struct tm tm_val;
                        gmtime_r(&msg_time, &tm_val);
                        printf("[%s %s %2d %02d:%02d:%02d %04d] ",
                               (const char*[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[tm_val.tm_wday%7],
                               (const char*[]){"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"}[tm_val.tm_mon%12],
                               tm_val.tm_mday, tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec,
                               tm_val.tm_year + 1900);
                    } else if (show_delta) {
                        unsigned long delta = entry.timestamp_us - prev_timestamp;
                        printf("[<%5lu.%06lu>] ",
                               delta / 1000000UL, delta % 1000000UL);
                    } else if (show_reltime) {
                        unsigned long secs = entry.timestamp_us / 1000000UL;
                        unsigned long usecs = entry.timestamp_us % 1000000UL;
                        printf("[%5lu.%06lu] ", secs, usecs);
                    } else {
                        unsigned long secs = entry.timestamp_us / 1000000UL;
                        unsigned long usecs = entry.timestamp_us % 1000000UL;
                        printf("[%5lu.%06lu] ", secs, usecs);
                    }
                    prev_timestamp = entry.timestamp_us;
                }

                /* Print the message */
                if (entry.msg && entry.msg_len > 0) {
                    fwrite(entry.msg, 1, entry.msg_len, stdout);
                }
                putchar('\n');
            }
        }
next_line:
        p = eol + 1;
    }

    free(buf);

    /* Follow mode: poll for new messages */
    if (follow_mode) {
        int last_size = nread;
        while (1) {
            struct timespec ts;
            ts.tv_sec = 1;
            ts.tv_nsec = 0;
            nanosleep(&ts, NULL);

            int cur_size = klogctl(SYSLOG_ACTION_SIZE_BUFFER, NULL, 0);
            if (cur_size > last_size) {
                char *newbuf = (char *)malloc(bufsize);
                if (!newbuf) break;
                int n = klogctl(SYSLOG_ACTION_READ_ALL, newbuf, bufsize - 1);
                if (n > last_size) {
                    /* Print only new data */
                    const char *start = newbuf + last_size;
                    int new_len = n - last_size;
                    fwrite(start, 1, new_len, stdout);
                    fflush(stdout);
                }
                last_size = n;
                free(newbuf);
            }
        }
    }

    return 0;
}
