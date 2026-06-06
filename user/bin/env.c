/*
 * env - run a program in a modified environment
 *
 * Full implementation per env(1) manpage.
 * Supports: -i, -0, -u, -C, -S, -v, --help, --version,
 *           --block-signal, --default-signal, --ignore-signal,
 *           --list-signal-handling, NAME=VALUE, COMMAND [ARG...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>

#define PROGRAM_NAME "env"
#define VERSION      "1.0"

/* Maximum environment variables and unset entries */
#define MAX_UNSET    64
#define MAX_SETENV   128
#define MAX_SPLIT    256

/* Signal name → number mapping */
struct sigmap {
    const char *name;
    int         num;
};

static const struct sigmap signal_table[] = {
    { "HUP",    SIGHUP   },
    { "INT",    SIGINT   },
    { "QUIT",   SIGQUIT  },
    { "ILL",    SIGILL   },
    { "TRAP",   SIGTRAP  },
    { "ABRT",   SIGABRT  },
    { "BUS",    SIGBUS   },
    { "FPE",    SIGFPE   },
    { "KILL",   SIGKILL  },
    { "USR1",   SIGUSR1  },
    { "SEGV",   SIGSEGV  },
    { "USR2",   SIGUSR2  },
    { "PIPE",   SIGPIPE  },
    { "ALRM",   SIGALRM  },
    { "TERM",   SIGTERM  },
    { "CHLD",   SIGCHLD  },
    { "CONT",   SIGCONT  },
    { "STOP",   SIGSTOP  },
    { "TSTP",   SIGTSTP  },
    { "TTIN",   SIGTTIN  },
    { "TTOU",   SIGTTOU  },
    { NULL,     0        }
};

/* Max known signal number */
#define MAX_SIG 31

static int verbose = 0;

static void print_version(void) {
    printf("%s (%s) %s\n", PROGRAM_NAME, PROGRAM_NAME, VERSION);
}

static void print_help(void) {
    printf("Usage: %s [OPTION]... [-] [NAME=VALUE]... [COMMAND [ARG]...]\n", PROGRAM_NAME);
    printf("Set each NAME to VALUE in the environment and run COMMAND.\n\n");
    printf("Mandatory arguments to long options are mandatory for short options too.\n");
    printf("  -i, --ignore-environment  start with an empty environment\n");
    printf("  -0, --null           end each output line with NUL, not newline\n");
    printf("  -u, --unset=NAME     remove variable from the environment\n");
    printf("  -C, --chdir=DIR      change working directory to DIR\n");
    printf("  -S, --split-string=S process and split S into separate arguments;\n");
    printf("                         used to pass multiple arguments on shebang lines\n");
    printf("  --block-signal[=SIG]    block delivery of SIG signal(s) to COMMAND\n");
    printf("  --default-signal[=SIG]  reset handling of SIG signal(s) to the default\n");
    printf("  --ignore-signal[=SIG]   set handling of SIG signal(s) to do nothing\n");
    printf("  --list-signal-handling   list non default signal handling to stderr\n");
    printf("  -v, --debug          print verbose information for each processing step\n");
    printf("      --help           display this help and exit\n");
    printf("      --version        output version information and exit\n");
    printf("\nA mere - implies -i.  If no COMMAND, print the resulting environment.\n");
    printf("\nExit status:\n");
    printf(" 125  if the env command itself fails\n");
    printf(" 126  if COMMAND is found but cannot be invoked\n");
    printf(" 127  if COMMAND cannot be found\n");
    printf(" -    the exit status of COMMAND otherwise\n");
}

/* Parse a signal specification: name, number, or comma-separated list.
 * Calls 'action' for each signal found. If sig_spec is NULL, applies to all. */
static int parse_signals(const char *sig_spec,
                         void (*action)(int signo, const char *desc)) {
    if (!sig_spec || sig_spec[0] == '\0') {
        /* Apply to all signals */
        for (int i = 1; i <= MAX_SIG; i++) {
            if (i == SIGKILL || i == SIGSTOP) continue;
            action(i, NULL);
        }
        return 0;
    }

    /* Comma-separated list */
    char buf[256];
    strncpy(buf, sig_spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = buf;
    while (*saveptr) {
        /* Find next comma */
        char *token = saveptr;
        char *comma = strchr(saveptr, ',');
        if (comma) {
            *comma = '\0';
            saveptr = comma + 1;
        } else {
            saveptr = token + strlen(token);
        }

        /* Skip empty tokens */
        if (token[0] == '\0') continue;

        /* Try as number */
        char *end;
        long num = strtol(token, &end, 10);
        if (*end == '\0' && num >= 1 && num <= MAX_SIG) {
            action((int)num, token);
            continue;
        }

        /* Try as name (with or without SIG prefix) */
        const char *name = token;
        if (name[0] == 'S' && name[1] == 'I' && name[2] == 'G')
            name += 3;

        int found = 0;
        for (const struct sigmap *s = signal_table; s->name; s++) {
            if (strcmp(s->name, name) == 0) {
                action(s->num, token);
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "%s: invalid signal '%s'\n", PROGRAM_NAME, token);
            return -1;
        }
    }
    return 0;
}

static void do_block_signal(int signo, const char *desc) {
    (void)desc;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signo);
    sigprocmask(SIG_BLOCK, &set, NULL);
    if (verbose)
        fprintf(stderr, "%s: blocking signal %d\n", PROGRAM_NAME, signo);
}

static void do_default_signal(int signo, const char *desc) {
    (void)desc;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(signo, &sa, NULL);
    if (verbose)
        fprintf(stderr, "%s: defaulting signal %d\n", PROGRAM_NAME, signo);
}

static void do_ignore_signal(int signo, const char *desc) {
    (void)desc;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(signo, &sa, NULL);
    if (verbose)
        fprintf(stderr, "%s: ignoring signal %d\n", PROGRAM_NAME, signo);
}

static void do_list_signal(int signo, const char *desc) {
    (void)desc;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    if (sigaction(signo, NULL, &sa) == 0) {
        if (sa.sa_handler == SIG_IGN) {
            /* Find signal name */
            const char *name = "???";
            for (const struct sigmap *s = signal_table; s->name; s++) {
                if (s->num == signo) { name = s->name; break; }
            }
            fprintf(stderr, "%s (%d): ignore\n", name, signo);
        }
    }
}

/* Split a string into tokens (for -S/--split-string).
 * Handles single/double quotes and backslash escapes. */
static int split_string(const char *str, char **out_argv, int max_args) {
    static char buf[4096];
    int argc = 0;
    const char *p = str;
    char *d = buf;
    char *dend = buf + sizeof(buf) - 1;

    while (*p && argc < max_args) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        out_argv[argc++] = d;

        /* Parse a token */
        while (*p && *p != ' ' && *p != '\t' && d < dend) {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': *d++ = '\n'; break;
                case 't': *d++ = '\t'; break;
                case '\\': *d++ = '\\'; break;
                case '\'': *d++ = '\''; break;
                case '"': *d++ = '"'; break;
                case ' ': *d++ = ' '; break;
                default: *d++ = *p; break;
                }
                p++;
            } else if (*p == '\'') {
                p++;
                while (*p && *p != '\'' && d < dend)
                    *d++ = *p++;
                if (*p == '\'') p++;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"' && d < dend) {
                    if (*p == '\\' && p[1]) {
                        p++;
                        switch (*p) {
                        case 'n': *d++ = '\n'; break;
                        case 't': *d++ = '\t'; break;
                        case '\\': *d++ = '\\'; break;
                        case '"': *d++ = '"'; break;
                        default: *d++ = '\\'; *d++ = *p; break;
                        }
                        p++;
                    } else {
                        *d++ = *p++;
                    }
                }
                if (*p == '"') p++;
            } else {
                *d++ = *p++;
            }
        }
        *d++ = '\0';
    }
    return argc;
}

static const char *signame(int signo) {
    for (const struct sigmap *s = signal_table; s->name; s++) {
        if (s->num == signo) return s->name;
    }
    return "???";
}

int main(int argc, char *argv[]) {
    int opt_ignore_env = 0;       /* -i */
    int opt_null_term = 0;        /* -0 */
    const char *opt_chdir = NULL; /* -C dir */
    int opt_list_signals = 0;

    const char *unset_names[MAX_UNSET];
    int unset_count = 0;

    /* Collected -S split args prepended before COMMAND */
    char *split_argv[MAX_SPLIT];
    int split_argc = 0;

    enum {
        OPT_HELP = 1000,
        OPT_VERSION,
        OPT_BLOCK_SIG,
        OPT_DEFAULT_SIG,
        OPT_IGNORE_SIG,
        OPT_LIST_SIG
    };

    static struct option long_options[] = {
        { "ignore-environment",   no_argument,       NULL, 'i'             },
        { "null",                 no_argument,       NULL, '0'             },
        { "unset",                required_argument, NULL, 'u'             },
        { "chdir",                required_argument, NULL, 'C'             },
        { "split-string",         required_argument, NULL, 'S'             },
        { "debug",                no_argument,       NULL, 'v'             },
        { "block-signal",         optional_argument, NULL, OPT_BLOCK_SIG   },
        { "default-signal",       optional_argument, NULL, OPT_DEFAULT_SIG },
        { "ignore-signal",        optional_argument, NULL, OPT_IGNORE_SIG  },
        { "list-signal-handling",  no_argument,      NULL, OPT_LIST_SIG    },
        { "help",                 no_argument,       NULL, OPT_HELP        },
        { "version",              no_argument,       NULL, OPT_VERSION     },
        { NULL,                   0,                 NULL, 0               }
    };

    /* Check for leading '-' as first non-option argument (implies -i) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            opt_ignore_env = 1;
            /* Remove the '-' from argv by shifting */
            for (int j = i; j < argc - 1; j++)
                argv[j] = argv[j + 1];
            argc--;
            break;
        }
        if (argv[i][0] != '-') break; /* stop at first non-option */
        if (strcmp(argv[i], "--") == 0) break;
    }

    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "+i0u:C:S:v", long_options, NULL)) != -1) {
        switch (c) {
        case 'i':
            opt_ignore_env = 1;
            break;
        case '0':
            opt_null_term = 1;
            break;
        case 'u':
            if (unset_count < MAX_UNSET)
                unset_names[unset_count++] = optarg;
            break;
        case 'C':
            opt_chdir = optarg;
            break;
        case 'S': {
            int n = split_string(optarg, &split_argv[split_argc],
                                 MAX_SPLIT - split_argc);
            split_argc += n;
            break;
        }
        case 'v':
            verbose = 1;
            break;
        case OPT_BLOCK_SIG:
            if (parse_signals(optarg, do_block_signal) < 0)
                return 125;
            break;
        case OPT_DEFAULT_SIG:
            if (parse_signals(optarg, do_default_signal) < 0)
                return 125;
            break;
        case OPT_IGNORE_SIG:
            if (parse_signals(optarg, do_ignore_signal) < 0)
                return 125;
            break;
        case OPT_LIST_SIG:
            opt_list_signals = 1;
            break;
        case OPT_HELP:
            print_help();
            return EXIT_SUCCESS;
        case OPT_VERSION:
            print_version();
            return EXIT_SUCCESS;
        default:
            fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
            return 125;
        }
    }

    /* Step 1: If -i, clear entire environment */
    if (opt_ignore_env) {
        if (verbose)
            fprintf(stderr, "%s: clearing environment\n", PROGRAM_NAME);
        clearenv();
    }

    /* Step 2: Unset specified variables */
    for (int i = 0; i < unset_count; i++) {
        if (verbose)
            fprintf(stderr, "%s: unsetting %s\n", PROGRAM_NAME, unset_names[i]);
        unsetenv(unset_names[i]);
    }

    /* Step 3: Process NAME=VALUE pairs from remaining args */
    int cmd_start = optind;
    while (cmd_start < argc) {
        char *eq = strchr(argv[cmd_start], '=');
        if (!eq) break; /* Not a NAME=VALUE, must be COMMAND */
        /* Set the variable */
        *eq = '\0';
        const char *name = argv[cmd_start];
        const char *value = eq + 1;
        if (verbose)
            fprintf(stderr, "%s: setenv %s=%s\n", PROGRAM_NAME, name, value);
        setenv(name, value, 1);
        *eq = '='; /* Restore for potential later use */
        cmd_start++;
    }

    /* Step 4: List signal handling if requested */
    if (opt_list_signals) {
        for (int i = 1; i <= MAX_SIG; i++) {
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            if (sigaction(i, NULL, &sa) == 0) {
                if (sa.sa_handler == SIG_IGN)
                    fprintf(stderr, "%s (%d): ignore\n", signame(i), i);
                else if (sa.sa_handler != SIG_DFL)
                    fprintf(stderr, "%s (%d): non-default\n", signame(i), i);
            }
        }
    }

    /* Step 5: Change directory if requested */
    if (opt_chdir) {
        if (verbose)
            fprintf(stderr, "%s: chdir '%s'\n", PROGRAM_NAME, opt_chdir);
        if (chdir(opt_chdir) != 0) {
            fprintf(stderr, "%s: cannot change directory to '%s': %s\n",
                    PROGRAM_NAME, opt_chdir, strerror(errno));
            return 125;
        }
    }

    /* Build the command to execute (split_argv + remaining argv) */
    int remaining = argc - cmd_start;
    int total_args = split_argc + remaining;

    if (total_args == 0) {
        /* No COMMAND: print the resulting environment */
        int cookie = 0;
        const char *name, *value;
        while (env_iter(&cookie, &name, &value)) {
            printf("%s=%s%c", name, value, opt_null_term ? '\0' : '\n');
        }
        return EXIT_SUCCESS;
    }

    /* Build argv for exec */
    char *exec_argv[MAX_SPLIT + 64];
    int ai = 0;
    for (int i = 0; i < split_argc && ai < MAX_SPLIT + 63; i++)
        exec_argv[ai++] = split_argv[i];
    for (int i = cmd_start; i < argc && ai < MAX_SPLIT + 63; i++)
        exec_argv[ai++] = argv[i];
    exec_argv[ai] = NULL;

    if (verbose) {
        fprintf(stderr, "%s: executing:", PROGRAM_NAME);
        for (int i = 0; exec_argv[i]; i++)
            fprintf(stderr, " %s", exec_argv[i]);
        fprintf(stderr, "\n");
    }

    /* Execute the command */
    execvp(exec_argv[0], exec_argv);

    /* If we get here, exec failed */
    int err = errno;
    fprintf(stderr, "%s: '%s': %s\n", PROGRAM_NAME, exec_argv[0], strerror(err));
    /* ENOENT → 127, other → 126 */
    return (err == ENOENT) ? 127 : 126;
}
