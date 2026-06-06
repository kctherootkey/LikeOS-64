/*
 * kill - send a signal to a process
 *
 * Usage: kill [-s sigspec] [-sigspec] pid ...
 *        kill -l [sigspec]
 *        kill -L
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

/* Signal name table */
static const struct {
    int         num;
    const char *name;
} sig_table[] = {
    {  1, "HUP"    },
    {  2, "INT"    },
    {  3, "QUIT"   },
    {  4, "ILL"    },
    {  5, "TRAP"   },
    {  6, "ABRT"   },
    {  6, "IOT"    },
    {  7, "BUS"    },
    {  8, "FPE"    },
    {  9, "KILL"   },
    { 10, "USR1"   },
    { 11, "SEGV"   },
    { 12, "USR2"   },
    { 13, "PIPE"   },
    { 14, "ALRM"   },
    { 15, "TERM"   },
    { 16, "STKFLT" },
    { 17, "CHLD"   },
    { 18, "CONT"   },
    { 19, "STOP"   },
    { 20, "TSTP"   },
    { 21, "TTIN"   },
    { 22, "TTOU"   },
    { 23, "URG"    },
    { 24, "XCPU"   },
    { 25, "XFSZ"   },
    { 26, "VTALRM" },
    { 27, "PROF"   },
    { 28, "WINCH"  },
    { 29, "IO"     },
    { 29, "POLL"   },
    { 30, "PWR"    },
    { 31, "SYS"    },
    {  0, NULL     }
};

/* Convert signal name to number.  Accepts "HUP", "SIGHUP", "1", etc. */
static int sig_from_name(const char *s)
{
    if (!s || !*s)
        return -1;

    /* Numeric? */
    if (isdigit((unsigned char)*s) || (*s == '-' && isdigit((unsigned char)s[1]))) {
        char *end;
        long n = strtol(s, &end, 10);
        if (*end != '\0')
            return -1;
        if (n < 0 || n > 64)
            return -1;
        return (int)n;
    }

    /* Strip leading "SIG" if present */
    const char *name = s;
    if (name[0] == 'S' && name[1] == 'I' && name[2] == 'G')
        name += 3;

    /* Check RT signals: RTMIN, RTMIN+n, RTMAX, RTMAX-n */
    if (strncmp(name, "RTMIN", 5) == 0) {
        if (name[5] == '\0')
            return SIGRTMIN;
        if (name[5] == '+') {
            int off = atoi(name + 6);
            int val = SIGRTMIN + off;
            if (val > SIGRTMAX)
                return -1;
            return val;
        }
        return -1;
    }
    if (strncmp(name, "RTMAX", 5) == 0) {
        if (name[5] == '\0')
            return SIGRTMAX;
        if (name[5] == '-') {
            int off = atoi(name + 6);
            int val = SIGRTMAX - off;
            if (val < SIGRTMIN)
                return -1;
            return val;
        }
        return -1;
    }

    for (int i = 0; sig_table[i].name; i++) {
        if (strcmp(name, sig_table[i].name) == 0)
            return sig_table[i].num;
    }
    return -1;
}

/* Convert signal number to canonical name (without SIG prefix) */
static const char *sig_to_name(int sig)
{
    static char buf[32];
    if (sig >= SIGRTMIN && sig <= SIGRTMAX) {
        if (sig == SIGRTMIN) {
            return "RTMIN";
        } else if (sig == SIGRTMAX) {
            return "RTMAX";
        } else if (sig - SIGRTMIN <= (SIGRTMAX - SIGRTMIN) / 2) {
            snprintf(buf, sizeof(buf), "RTMIN+%d", sig - SIGRTMIN);
        } else {
            snprintf(buf, sizeof(buf), "RTMAX-%d", SIGRTMAX - sig);
        }
        return buf;
    }
    for (int i = 0; sig_table[i].name; i++) {
        if (sig_table[i].num == sig)
            return sig_table[i].name;
    }
    snprintf(buf, sizeof(buf), "%d", sig);
    return buf;
}

/* -l [sigspec]: list signal names.
 * If sigspec is a number, print the name.
 * If sigspec is a name, print the number.
 * If no argument, list all standard signals. */
static int do_list(const char *arg)
{
    if (!arg) {
        /* List all standard signals */
        for (int s = 1; s < 32; s++) {
            const char *n = sig_to_name(s);
            if (n) {
                if (s > 1) putchar(' ');
                printf("%s", n);
            }
        }
        putchar('\n');
        return 0;
    }

    /* Numeric argument => print name */
    if (isdigit((unsigned char)*arg)) {
        int num = atoi(arg);
        /* Strip exit-status encoding: if > 128, signal = num - 128 */
        if (num > 128)
            num -= 128;
        if (num <= 0 || num > SIGRTMAX) {
            fprintf(stderr, "kill: %s: invalid signal specification\n", arg);
            return 1;
        }
        printf("%s\n", sig_to_name(num));
        return 0;
    }

    /* Name argument => print number */
    int n = sig_from_name(arg);
    if (n < 0) {
        fprintf(stderr, "kill: %s: invalid signal specification\n", arg);
        return 1;
    }
    printf("%d\n", n);
    return 0;
}

/* -L: print a table of signal numbers and names */
static void do_table(void)
{
    int col = 0;
    for (int s = 1; s <= SIGRTMAX; s++) {
        const char *n = sig_to_name(s);
        printf("%2d %-10s", s, n);
        col++;
        if (col == 4) {
            putchar('\n');
            col = 0;
        }
    }
    if (col)
        putchar('\n');
}

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "  kill [-s sigspec | -sigspec] pid ...\n"
        "  kill -l [sigspec]\n"
        "  kill -L\n");
}

int main(int argc, char **argv)
{
    int signo = SIGTERM;  /* default signal */
    int i = 1;
    int ret = 0;

    if (argc < 2) {
        usage();
        return 1;
    }

    /* Parse options */
    while (i < argc && argv[i][0] == '-') {
        const char *opt = argv[i];

        /* -- ends option parsing */
        if (opt[1] == '-' && opt[2] == '\0') {
            i++;
            break;
        }

        /* -l [sigspec] */
        if (opt[1] == 'l' && opt[2] == '\0') {
            i++;
            if (i < argc) {
                /* Process remaining args as sigspecs */
                while (i < argc) {
                    if (do_list(argv[i]))
                        ret = 1;
                    i++;
                }
            } else {
                ret = do_list(NULL);
            }
            return ret;
        }

        /* -L : table */
        if (opt[1] == 'L' && opt[2] == '\0') {
            do_table();
            return 0;
        }

        /* -s sigspec */
        if (opt[1] == 's' && opt[2] == '\0') {
            i++;
            if (i >= argc) {
                fprintf(stderr, "kill: -s requires an argument\n");
                return 1;
            }
            signo = sig_from_name(argv[i]);
            if (signo < 0) {
                fprintf(stderr, "kill: %s: invalid signal specification\n", argv[i]);
                return 1;
            }
            i++;
            continue;
        }

        /* -s<sigspec> (no space) */
        if (opt[1] == 's' && opt[2] != '\0') {
            signo = sig_from_name(opt + 2);
            if (signo < 0) {
                fprintf(stderr, "kill: %s: invalid signal specification\n", opt + 2);
                return 1;
            }
            i++;
            continue;
        }

        /* -q value (queue signal with sigqueue, but we just use kill) */
        if (opt[1] == 'q' && opt[2] == '\0') {
            /* Accept and ignore the value argument (sigqueue not implemented) */
            i += 2;
            continue;
        }

        /* -<sigspec> e.g. -9, -HUP, -SIGTERM */
        {
            int s = sig_from_name(opt + 1);
            if (s >= 0) {
                signo = s;
                i++;
                continue;
            }
        }

        fprintf(stderr, "kill: invalid option: %s\n", opt);
        usage();
        return 1;
    }

    if (i >= argc) {
        fprintf(stderr, "kill: no pid specified\n");
        usage();
        return 1;
    }

    /* Send signal to each specified PID */
    while (i < argc) {
        char *end;
        long pid = strtol(argv[i], &end, 10);
        if (*end != '\0') {
            fprintf(stderr, "kill: %s: arguments must be process or job IDs\n", argv[i]);
            ret = 1;
            i++;
            continue;
        }
        if (kill((pid_t)pid, signo) != 0) {
            fprintf(stderr, "kill: (%ld) - %s\n", pid,
                    errno == ESRCH ? "No such process" :
                    errno == EPERM ? "Operation not permitted" :
                    "Unknown error");
            ret = 1;
        }
        i++;
    }

    return ret;
}
