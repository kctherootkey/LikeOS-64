/*
 * sh - LikeOS-64 userland shell
 *
 * Built-in commands: cd, help, history, export, alias, unalias, time, exit
 *
 * Supported operators:
 *   |    Pipe stdout to stdin
 *   >    Redirect stdout (overwrite)
 *   >>   Redirect stdout (append)
 *   <    Redirect stdin from file
 *   <<   Here-document input delimiter
 *   <<<  Here-string input
 *   2>   Redirect stderr (overwrite)
 *   2>>  Redirect stderr (append)
 *   2>&1 Redirect stderr to stdout
 *   &>   Redirect stdout+stderr (overwrite)
 *   &>>  Redirect stdout+stderr (append)
 *   |&   Pipe stdout+stderr
 *   >&   Redirect stdout and stderr (older form)
 *   <>   Open file for read/write
 *   >|   Force overwrite (noclobber override)
 *   ;    Command separator
 *   &&   Run next if previous succeeds
 *   ||   Run next if previous fails
 *   &    Run command in background
 *   \|   Escaped pipe literal
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <termios.h>
#include <netdb.h>

#define SHELL_MAX_LINE 1024
#define SHELL_MAX_ARGS 64
#define MAX_REDIRECTS  16
#define MAX_PIPELINE   32
#define MAX_COMMANDS   64

/* ------------------------------------------------------------------ */
/* Background job tracking                                              */
/* ------------------------------------------------------------------ */
#define MAX_BG_JOBS 64

typedef struct {
    pid_t pid;          /* PID of background process (or last PID in pipeline) */
    int   job_number;   /* Job number [N] */
    char  cmd[128];     /* Command string for display */
} bg_job_t;

static bg_job_t bg_jobs[MAX_BG_JOBS];
static int      bg_job_count;
static int      next_job_number = 1;

static int bg_add_job(pid_t pid, const char *cmd) {
    if (bg_job_count >= MAX_BG_JOBS) return 0;
    bg_job_t *j = &bg_jobs[bg_job_count++];
    j->pid = pid;
    j->job_number = next_job_number++;
    strncpy(j->cmd, cmd, sizeof(j->cmd) - 1);
    j->cmd[sizeof(j->cmd) - 1] = '\0';
    return j->job_number;
}

static void bg_check_jobs(void) {
    int i = 0;
    while (i < bg_job_count) {
        int status = 0;
        pid_t ret = waitpid(bg_jobs[i].pid, &status, WNOHANG);
        if (ret > 0) {
            /* Job finished */
            if (WIFEXITED(status)) {
                printf("[%d]+  Done                    %s\n",
                       bg_jobs[i].job_number, bg_jobs[i].cmd);
            } else {
                printf("[%d]+  Terminated              %s\n",
                       bg_jobs[i].job_number, bg_jobs[i].cmd);
            }
            /* Remove from table */
            bg_jobs[i] = bg_jobs[--bg_job_count];
            /* Don't increment i — re-check the swapped entry */
        } else {
            i++;
        }
    }
    if (bg_job_count == 0) next_job_number = 1;
}

/* ------------------------------------------------------------------ */
/* Command history                                                      */
/* ------------------------------------------------------------------ */
#define HIST_MAX     500
#define HIST_FILE    "/.sh_history"

static char  hist_buf[HIST_MAX][SHELL_MAX_LINE];
static int   hist_count;
static int   hist_start;

static int hist_real(int i) { return (hist_start + i) % HIST_MAX; }

static void hist_add(const char *line) {
    if (hist_count < HIST_MAX) {
        strncpy(hist_buf[hist_count], line, SHELL_MAX_LINE - 1);
        hist_buf[hist_count][SHELL_MAX_LINE - 1] = '\0';
        hist_count++;
    } else {
        strncpy(hist_buf[hist_start], line, SHELL_MAX_LINE - 1);
        hist_buf[hist_start][SHELL_MAX_LINE - 1] = '\0';
        hist_start = (hist_start + 1) % HIST_MAX;
    }
}

static void hist_load(void) {
    FILE *f = fopen(HIST_FILE, "r");
    if (!f) return;
    char tmp[SHELL_MAX_LINE];
    while (fgets(tmp, sizeof(tmp), f)) {
        size_t l = strlen(tmp);
        if (l > 0 && tmp[l - 1] == '\n') tmp[l - 1] = '\0';
        if (tmp[0]) hist_add(tmp);
    }
    fclose(f);
}

static void hist_save(void) {
    FILE *f = fopen(HIST_FILE, "w");
    if (!f) return;
    for (int i = 0; i < hist_count; i++) {
        fputs(hist_buf[hist_real(i)], f);
        fputc('\n', f);
    }
    fclose(f);
}

static void hist_append(void) {
    FILE *f = fopen(HIST_FILE, "a");
    if (!f) return;
    if (hist_count > 0) {
        int idx = hist_real(hist_count - 1);
        fputs(hist_buf[idx], f);
        fputc('\n', f);
    }
    fclose(f);
}

static void hist_reread(void) {
    hist_count = 0;
    hist_start = 0;
    hist_load();
}

static void hist_delete(int offset) {
    if (offset < 1 || offset > hist_count) {
        fprintf(stderr, "history: %d: invalid offset\n", offset);
        return;
    }
    int idx = offset - 1;
    for (int i = idx; i < hist_count - 1; i++) {
        int ri = hist_real(i);
        int ri1 = hist_real(i + 1);
        strcpy(hist_buf[ri], hist_buf[ri1]);
    }
    hist_count--;
}

/* ------------------------------------------------------------------ */
/* Alias management                                                     */
/* ------------------------------------------------------------------ */
#define MAX_ALIASES  256
#define ALIAS_NAME   128
#define ALIAS_VALUE  512
#define ALIAS_FILE   "/.aliases"

typedef struct {
    char name[ALIAS_NAME];
    char value[ALIAS_VALUE];
} alias_entry_t;

static alias_entry_t alias_table[MAX_ALIASES];
static int alias_count;

static int alias_find(const char *name) {
    for (int i = 0; i < alias_count; i++)
        if (strcmp(alias_table[i].name, name) == 0)
            return i;
    return -1;
}

static void alias_set(const char *name, const char *value) {
    int idx = alias_find(name);
    if (idx >= 0) {
        strncpy(alias_table[idx].value, value, ALIAS_VALUE - 1);
        alias_table[idx].value[ALIAS_VALUE - 1] = '\0';
        return;
    }
    if (alias_count >= MAX_ALIASES) {
        fprintf(stderr, "alias: too many aliases\n");
        return;
    }
    strncpy(alias_table[alias_count].name,  name,  ALIAS_NAME  - 1);
    alias_table[alias_count].name[ALIAS_NAME - 1] = '\0';
    strncpy(alias_table[alias_count].value, value, ALIAS_VALUE - 1);
    alias_table[alias_count].value[ALIAS_VALUE - 1] = '\0';
    alias_count++;
}

static int alias_remove(const char *name) {
    int idx = alias_find(name);
    if (idx < 0) return -1;
    alias_table[idx] = alias_table[--alias_count];
    return 0;
}

static void alias_remove_all(void) {
    alias_count = 0;
}

static void alias_print_all(void) {
    for (int i = 0; i < alias_count; i++)
        printf("alias %s='%s'\n", alias_table[i].name, alias_table[i].value);
}

static void alias_load_defaults(void) {
    alias_set("grep", "grep --color=auto");
    alias_set("l",    "ls -CF");
    alias_set("la",   "ls -A");
    alias_set("ll",   "ls -alF");
    alias_set("ls",   "ls --color=auto");
}

static void alias_load(void) {
    alias_count = 0;
    FILE *f = fopen(ALIAS_FILE, "r");
    if (!f) {
        alias_load_defaults();
        return;
    }
    char line[ALIAS_NAME + ALIAS_VALUE + 16];
    while (fgets(line, sizeof(line), f) && alias_count < MAX_ALIASES) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        /* Format: alias name='value' */
        if (strncmp(line, "alias ", 6) != 0) continue;
        char *p = line + 6;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '\'' && val[vlen - 1] == '\'') {
            val[vlen - 1] = '\0';
            val++;
        }
        alias_set(p, val);
    }
    fclose(f);
}

static void alias_save(void) {
    FILE *f = fopen(ALIAS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < alias_count; i++)
        fprintf(f, "alias %s='%s'\n", alias_table[i].name, alias_table[i].value);
    fclose(f);
}

/*
 * Expand aliases in the input line (before tokenizing).
 * Only the first word of each simple command is subject to alias expansion.
 * If the alias value ends with a space, the next word is also checked.
 * Protects against infinite recursion with a depth limit.
 */
/*
 * Expand an alias at position *pos within line (in-place).
 * Returns 1 if expansion happened, 0 otherwise.
 */
static int alias_expand_at(char *line, int maxlen, int pos,
                           char **seen, int *nseen) {
    char expanded[SHELL_MAX_LINE * 2];

    /* Skip whitespace at pos */
    char *p = line + pos;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;

    /* Extract the first word of this segment */
    char *word_start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '|' &&
           *p != ';' && *p != '>' && *p != '<' && *p != '&')
        p++;
    int word_len = (int)(p - word_start);
    if (word_len <= 0 || word_len >= ALIAS_NAME) return 0;

    char word[ALIAS_NAME];
    memcpy(word, word_start, (size_t)word_len);
    word[word_len] = '\0';

    /* Check if we already expanded this alias (avoid recursion) */
    for (int i = 0; i < *nseen; i++) {
        if (strcmp(seen[i], word) == 0) return 0;
    }

    int idx = alias_find(word);
    if (idx < 0) return 0;

    /* Expand: replace the word with the alias value */
    int vlen = (int)strlen(alias_table[idx].value);
    int rest_len = (int)strlen(p);
    int prefix_len = (int)(word_start - line);

    if (prefix_len + vlen + rest_len >= maxlen - 1) return 0;

    memcpy(expanded, line, (size_t)prefix_len);
    memcpy(expanded + prefix_len, alias_table[idx].value, (size_t)vlen);
    memcpy(expanded + prefix_len + vlen, p, (size_t)rest_len + 1);
    memcpy(line, expanded, (size_t)(prefix_len + vlen + rest_len + 1));

    if (*nseen < 32)
        seen[(*nseen)++] = alias_table[idx].name;
    return 1;
}

/*
 * Expand aliases for every command segment in the line.
 * Segments are separated by |, ;, &&, ||.
 */
static void alias_expand(char *line, int maxlen) {
    char *seen[32];
    int nseen = 0;

    /* Expand alias at position 0 (first command) */
    for (int passes = 0; passes < 16; passes++) {
        if (!alias_expand_at(line, maxlen, 0, seen, &nseen))
            break;
    }

    /* Walk through the line finding pipe/semicolon/&&/|| boundaries,
     * and expand the alias of the first word after each separator. */
    int i = 0;
    int in_sq = 0, in_dq = 0;  /* track quoting */
    while (line[i]) {
        if (line[i] == '\'' && !in_dq) { in_sq = !in_sq; i++; continue; }
        if (line[i] == '"'  && !in_sq) { in_dq = !in_dq; i++; continue; }
        if (in_sq || in_dq) { i++; continue; }

        int sep_len = 0;
        if (line[i] == '|' && line[i + 1] == '|') sep_len = 2;       /* || */
        else if (line[i] == '&' && line[i + 1] == '&') sep_len = 2;  /* && */
        else if (line[i] == '|') sep_len = 1;                         /* |  */
        else if (line[i] == ';') sep_len = 1;                         /* ;  */

        if (sep_len > 0) {
            int cmd_start = i + sep_len;
            nseen = 0;  /* reset recursion guard for new command */
            for (int passes = 0; passes < 16; passes++) {
                if (!alias_expand_at(line, maxlen, cmd_start, seen, &nseen))
                    break;
            }
            /* Re-scan from cmd_start since the line may have grown */
            i = cmd_start;
        } else {
            i++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Builtin: alias                                                       */
/* ------------------------------------------------------------------ */

static void builtin_alias(int argc, char **argv) {
    int opt_print = 0;
    int first_arg = 1;

    if (first_arg < argc && strcmp(argv[first_arg], "-p") == 0) {
        opt_print = 1;
        first_arg++;
    }

    if (opt_print || first_arg >= argc) {
        alias_print_all();
        return;
    }

    for (int i = first_arg; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            alias_set(argv[i], eq + 1);
            *eq = '=';
        } else {
            int idx = alias_find(argv[i]);
            if (idx >= 0)
                printf("alias %s='%s'\n", alias_table[idx].name,
                       alias_table[idx].value);
            else
                fprintf(stderr, "alias: %s: not found\n", argv[i]);
        }
    }
    alias_save();
}

/* ------------------------------------------------------------------ */
/* Builtin: unalias                                                     */
/* ------------------------------------------------------------------ */

static void builtin_unalias(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "unalias: usage: unalias [-a] name [name ...]\n");
        return;
    }

    int i = 1;
    if (strcmp(argv[1], "-a") == 0) {
        alias_remove_all();
        alias_save();
        return;
    }

    for (; i < argc; i++) {
        if (alias_remove(argv[i]) < 0)
            fprintf(stderr, "unalias: %s: not found\n", argv[i]);
    }
    alias_save();
}

/* ------------------------------------------------------------------ */
/* Builtin: time                                                        */
/* ------------------------------------------------------------------ */

static int builtin_time(int argc, char **argv) {
    int posix_fmt = 0;
    int arg_start = 1;

    /* Parse time's own options: only -p is defined for the builtin */
    if (arg_start < argc && strcmp(argv[arg_start], "-p") == 0) {
        posix_fmt = 1;
        arg_start++;
    }

    if (arg_start >= argc) {
        fprintf(stderr, "time: missing command\n");
        return 1;
    }

    struct timespec ts_start, ts_end;
    clock_gettime(0 /* CLOCK_MONOTONIC is 0 in our kernel */, &ts_start);

    /* Fork and exec the command */
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "time: cannot fork: %s\n", strerror(errno));
        return 127;
    }

    if (child == 0) {
        /* Child: exec the pipeline */
        setpgid(0, 0);
        tcsetpgrp(STDIN_FILENO, getpid());
        execvp(argv[arg_start], &argv[arg_start]);
        fprintf(stderr, "time: %s: %s\n", argv[arg_start], strerror(errno));
        _exit(errno == 2 /* ENOENT */ ? 127 : 126);
    }

    /* Parent: set process group and wait */
    setpgid(child, child);
    tcsetpgrp(STDIN_FILENO, child);

    int status = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    wait4(child, &status, 0, &ru);

    /* Restore shell as foreground */
    tcsetpgrp(STDIN_FILENO, getpgrp());

    clock_gettime(0, &ts_end);

    double real_secs = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                       (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000000000.0;
    double user_secs = (double)ru.ru_utime.tv_sec +
                       (double)ru.ru_utime.tv_usec / 1000000.0;
    double sys_secs  = (double)ru.ru_stime.tv_sec +
                       (double)ru.ru_stime.tv_usec / 1000000.0;

    int exit_status = 0;
    if (WIFEXITED(status))
        exit_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        exit_status = 128 + WTERMSIG(status);

    if (posix_fmt) {
        fprintf(stderr, "real %.2f\nuser %.2f\nsys %.2f\n",
                real_secs, user_secs, sys_secs);
    } else {
        /* Default shell format (like bash) */
        int r_min = (int)(real_secs / 60.0);
        double r_sec = real_secs - r_min * 60.0;
        int u_min = (int)(user_secs / 60.0);
        double u_sec = user_secs - u_min * 60.0;
        int s_min = (int)(sys_secs / 60.0);
        double s_sec = sys_secs - s_min * 60.0;
        fprintf(stderr, "\nreal\t%dm%.3fs\nuser\t%dm%.3fs\nsys\t%dm%.3fs\n",
                r_min, r_sec, u_min, u_sec, s_min, s_sec);
    }

    return exit_status;
}

static void builtin_history(int argc, char **argv) {
    if (argc == 1) {
        for (int i = 0; i < hist_count; i++)
            printf(" %4d  %s\n", i + 1, hist_buf[hist_real(i)]);
        return;
    }
    if (argv[1][0] != '-') {
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (*end == '\0' && n > 0) {
            int start = 0;
            if (n < hist_count) start = hist_count - (int)n;
            for (int i = start; i < hist_count; i++)
                printf(" %4d  %s\n", i + 1, hist_buf[hist_real(i)]);
        } else {
            fprintf(stderr, "history: %s: numeric argument required\n", argv[1]);
        }
        return;
    }
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "-c") == 0) {
            hist_count = 0;
            hist_start = 0;
        } else if (strcmp(argv[a], "-d") == 0) {
            if (a + 1 < argc) {
                a++;
                int off = (int)strtol(argv[a], NULL, 10);
                hist_delete(off);
            } else {
                fprintf(stderr, "history: -d: option requires an argument\n");
            }
        } else if (strcmp(argv[a], "-a") == 0) {
            hist_append();
        } else if (strcmp(argv[a], "-n") == 0) {
            hist_reread();
        } else if (strcmp(argv[a], "-r") == 0) {
            hist_count = 0;
            hist_start = 0;
            hist_load();
        } else if (strcmp(argv[a], "-w") == 0) {
            hist_save();
        } else if (strcmp(argv[a], "-s") == 0) {
            char entry[SHELL_MAX_LINE] = "";
            for (int j = a + 1; j < argc; j++) {
                if (j > a + 1) strncat(entry, " ", sizeof(entry) - strlen(entry) - 1);
                strncat(entry, argv[j], sizeof(entry) - strlen(entry) - 1);
            }
            if (entry[0]) hist_add(entry);
            return;
        } else if (strcmp(argv[a], "-p") == 0) {
            for (int j = a + 1; j < argc; j++) {
                if (j > a + 1) putchar(' ');
                printf("%s", argv[j]);
            }
            putchar('\n');
            return;
        } else {
            fprintf(stderr, "history: %s: invalid option\n", argv[a]);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* export builtin                                                       */
/* ------------------------------------------------------------------ */

static void builtin_export(int argc, char **argv) {
    int opt_print = 0;
    int opt_unset = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (argv[i][0] != '-') break;
        for (const char *p = &argv[i][1]; *p; p++) {
            switch (*p) {
            case 'p': opt_print = 1; break;
            case 'n': opt_unset = 1; break;
            case 'f': break;
            default:
                fprintf(stderr, "export: -%c: invalid option\n", *p);
                return;
            }
        }
    }
    if (opt_print || i >= argc) {
        int cookie = 0;
        const char *name, *value;
        while (env_iter(&cookie, &name, &value)) {
            printf("declare -x %s=\"%s\"\n", name, value);
        }
        return;
    }
    for (; i < argc; i++) {
        if (opt_unset) {
            unsetenv(argv[i]);
            continue;
        }
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            setenv(argv[i], eq + 1, 1);
            *eq = '=';
        } else {
            if (!getenv(argv[i])) {
                setenv(argv[i], "", 1);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Token types                                                          */
/* ------------------------------------------------------------------ */

enum token_type {
    TOK_WORD,              /* plain word / argument */
    TOK_PIPE,              /* | */
    TOK_PIPE_BOTH,         /* |& (pipe stdout+stderr) */
    TOK_REDIR_OUT,         /* > */
    TOK_REDIR_OUT_FORCE,   /* >| */
    TOK_REDIR_APPEND,      /* >> */
    TOK_REDIR_IN,          /* < */
    TOK_REDIR_READWRITE,   /* <> */
    TOK_HEREDOC,           /* << */
    TOK_HERESTRING,        /* <<< */
    TOK_REDIR_ERR,         /* 2> */
    TOK_REDIR_ERR_APPEND,  /* 2>> */
    TOK_REDIR_ERR_TO_OUT,  /* 2>&1 */
    TOK_REDIR_BOTH_OUT,    /* &> */
    TOK_REDIR_BOTH_APPEND, /* &>> */
    TOK_REDIR_STDOUT_ERR,  /* >& (legacy) */
    TOK_SEMI,              /* ; */
    TOK_AND,               /* && */
    TOK_OR,                /* || */
    TOK_BG,                /* & (background) */
    TOK_EOF
};

typedef struct {
    enum token_type type;
    char text[SHELL_MAX_LINE];
} token_t;

/* ------------------------------------------------------------------ */
/* Tokenizer                                                            */
/* ------------------------------------------------------------------ */

static int is_operator_char(char c) {
    return c == '|' || c == '>' || c == '<' || c == ';' || c == '&';
}

/*
 * Tokenize the input line into an array of tokens.
 * Handles quoting (single/double), backslash escapes, and all operators.
 * Returns the number of tokens produced.
 */
static int tokenize(const char *input, token_t *tokens, int max_tokens) {
    int ntok = 0;
    const char *p = input;

    while (*p && ntok < max_tokens - 1) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        /* --- Operator recognition (order matters) --- */

        /* 2>&1 */
        if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') {
            tokens[ntok++].type = TOK_REDIR_ERR_TO_OUT;
            p += 4;
            continue;
        }
        /* 2>> */
        if (p[0] == '2' && p[1] == '>' && p[2] == '>') {
            tokens[ntok++].type = TOK_REDIR_ERR_APPEND;
            p += 3;
            continue;
        }
        /* 2> */
        if (p[0] == '2' && p[1] == '>') {
            tokens[ntok++].type = TOK_REDIR_ERR;
            p += 2;
            continue;
        }
        /* &>> */
        if (p[0] == '&' && p[1] == '>' && p[2] == '>') {
            tokens[ntok++].type = TOK_REDIR_BOTH_APPEND;
            p += 3;
            continue;
        }
        /* &> */
        if (p[0] == '&' && p[1] == '>') {
            tokens[ntok++].type = TOK_REDIR_BOTH_OUT;
            p += 2;
            continue;
        }
        /* <<< (must check before <<) */
        if (p[0] == '<' && p[1] == '<' && p[2] == '<') {
            tokens[ntok++].type = TOK_HERESTRING;
            p += 3;
            continue;
        }
        /* << */
        if (p[0] == '<' && p[1] == '<') {
            tokens[ntok++].type = TOK_HEREDOC;
            p += 2;
            continue;
        }
        /* <> */
        if (p[0] == '<' && p[1] == '>') {
            tokens[ntok++].type = TOK_REDIR_READWRITE;
            p += 2;
            continue;
        }
        /* < */
        if (p[0] == '<') {
            tokens[ntok++].type = TOK_REDIR_IN;
            p += 1;
            continue;
        }
        /* >> */
        if (p[0] == '>' && p[1] == '>') {
            tokens[ntok++].type = TOK_REDIR_APPEND;
            p += 2;
            continue;
        }
        /* >| */
        if (p[0] == '>' && p[1] == '|') {
            tokens[ntok++].type = TOK_REDIR_OUT_FORCE;
            p += 2;
            continue;
        }
        /* >& (legacy stdout+stderr redirect) */
        if (p[0] == '>' && p[1] == '&') {
            tokens[ntok++].type = TOK_REDIR_STDOUT_ERR;
            p += 2;
            continue;
        }
        /* > */
        if (p[0] == '>') {
            tokens[ntok++].type = TOK_REDIR_OUT;
            p += 1;
            continue;
        }
        /* |& */
        if (p[0] == '|' && p[1] == '&') {
            tokens[ntok++].type = TOK_PIPE_BOTH;
            p += 2;
            continue;
        }
        /* || */
        if (p[0] == '|' && p[1] == '|') {
            tokens[ntok++].type = TOK_OR;
            p += 2;
            continue;
        }
        /* | */
        if (p[0] == '|') {
            tokens[ntok++].type = TOK_PIPE;
            p += 1;
            continue;
        }
        /* && */
        if (p[0] == '&' && p[1] == '&') {
            tokens[ntok++].type = TOK_AND;
            p += 2;
            continue;
        }
        /* & (background - must come after &> and &&) */
        if (p[0] == '&') {
            tokens[ntok++].type = TOK_BG;
            p += 1;
            continue;
        }
        /* ; */
        if (p[0] == ';') {
            tokens[ntok++].type = TOK_SEMI;
            p += 1;
            continue;
        }

        /* --- Word: collect characters with quoting and escape support --- */
        tokens[ntok].type = TOK_WORD;
        char *out = tokens[ntok].text;
        char *out_end = tokens[ntok].text + SHELL_MAX_LINE - 1;

        while (*p && out < out_end) {
            if (*p == '\\') {
                /* Backslash escape: literal next char */
                p++;
                if (*p) {
                    *out++ = *p++;
                }
                continue;
            }
            if (*p == '\'') {
                /* Single quote: everything literal until closing ' */
                p++;
                while (*p && *p != '\'' && out < out_end) {
                    *out++ = *p++;
                }
                if (*p == '\'') p++;
                continue;
            }
            if (*p == '"') {
                /* Double quote: allow backslash escapes for special chars */
                p++;
                while (*p && *p != '"' && out < out_end) {
                    if (*p == '\\' && p[1] &&
                        (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')) {
                        p++;
                        *out++ = *p++;
                    } else {
                        *out++ = *p++;
                    }
                }
                if (*p == '"') p++;
                continue;
            }
            /* Unquoted: stop at whitespace or operator chars */
            if (*p == ' ' || *p == '\t') break;
            if (is_operator_char(*p)) break;
            /* 2> 2>> 2>&1 at start of unquoted region */
            if (*p == '2' && p[1] == '>') break;

            *out++ = *p++;
        }
        *out = '\0';
        ntok++;
    }

    tokens[ntok].type = TOK_EOF;
    tokens[ntok].text[0] = '\0';
    return ntok;
}

/* ------------------------------------------------------------------ */
/* Redirect descriptor                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    enum token_type type;
    char filename[SHELL_MAX_LINE];
} redirect_t;

/* ------------------------------------------------------------------ */
/* Simple command: argv + redirects                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char *argv[SHELL_MAX_ARGS + 1];
    int   argc;
    redirect_t redirects[MAX_REDIRECTS];
    int   nredirects;
    int   pipe_stderr;  /* true if |& connects to next command */
} simple_cmd_t;

/* ------------------------------------------------------------------ */
/* Pipeline: sequence of simple commands connected by | or |&           */
/* ------------------------------------------------------------------ */

typedef struct {
    simple_cmd_t cmds[MAX_PIPELINE];
    int ncmds;
    int background;
} pipeline_t;

/* ------------------------------------------------------------------ */
/* Command list entry: pipeline + connector (;, &&, ||)                 */
/* ------------------------------------------------------------------ */

enum connector {
    CONN_NONE,
    CONN_SEMI,
    CONN_AND,
    CONN_OR
};

typedef struct {
    pipeline_t pipeline;
    enum connector conn;
} cmd_entry_t;

/* ------------------------------------------------------------------ */
/* Parser: tokens -> command list                                       */
/* ------------------------------------------------------------------ */

/*
 * Is this token type a redirect operator?
 */
static int is_redirect_token(enum token_type t) {
    return (t == TOK_REDIR_OUT || t == TOK_REDIR_OUT_FORCE ||
            t == TOK_REDIR_APPEND || t == TOK_REDIR_IN ||
            t == TOK_REDIR_READWRITE || t == TOK_HEREDOC ||
            t == TOK_HERESTRING || t == TOK_REDIR_ERR ||
            t == TOK_REDIR_ERR_APPEND || t == TOK_REDIR_ERR_TO_OUT ||
            t == TOK_REDIR_BOTH_OUT || t == TOK_REDIR_BOTH_APPEND ||
            t == TOK_REDIR_STDOUT_ERR);
}

/*
 * Parse a single simple command from the token stream starting at *pos.
 */
static int parse_simple_cmd(token_t *tokens, int ntok, int *pos,
                            simple_cmd_t *cmd) {
    memset(cmd, 0, sizeof(*cmd));
    int found = 0;

    while (*pos < ntok && tokens[*pos].type != TOK_EOF) {
        token_t *t = &tokens[*pos];

        /* Stop at pipeline/command-list operators */
        if (t->type == TOK_PIPE || t->type == TOK_PIPE_BOTH ||
            t->type == TOK_SEMI || t->type == TOK_AND ||
            t->type == TOK_OR   || t->type == TOK_BG) {
            break;
        }

        /* Redirect operators that need a filename/word argument */
        if (is_redirect_token(t->type) && t->type != TOK_REDIR_ERR_TO_OUT) {
            if (cmd->nredirects >= MAX_REDIRECTS) {
                fprintf(stderr, "sh: too many redirections\n");
                return -1;
            }
            redirect_t *r = &cmd->redirects[cmd->nredirects];
            r->type = t->type;
            (*pos)++;
            if (*pos >= ntok || tokens[*pos].type != TOK_WORD) {
                fprintf(stderr, "sh: syntax error near redirect\n");
                return -1;
            }
            strncpy(r->filename, tokens[*pos].text, SHELL_MAX_LINE - 1);
            r->filename[SHELL_MAX_LINE - 1] = '\0';
            cmd->nredirects++;
            (*pos)++;
            found = 1;
            continue;
        }

        /* 2>&1: no filename needed */
        if (t->type == TOK_REDIR_ERR_TO_OUT) {
            if (cmd->nredirects >= MAX_REDIRECTS) {
                fprintf(stderr, "sh: too many redirections\n");
                return -1;
            }
            redirect_t *r = &cmd->redirects[cmd->nredirects];
            r->type = TOK_REDIR_ERR_TO_OUT;
            r->filename[0] = '\0';
            cmd->nredirects++;
            (*pos)++;
            found = 1;
            continue;
        }

        /* Must be a word (argument) */
        if (t->type == TOK_WORD) {
            if (cmd->argc < SHELL_MAX_ARGS) {
                cmd->argv[cmd->argc++] = t->text;
            }
            (*pos)++;
            found = 1;
            continue;
        }

        break;
    }

    cmd->argv[cmd->argc] = NULL;
    return found;
}

/*
 * Parse a pipeline (cmd1 | cmd2 |& cmd3 ...) from token stream.
 */
static int parse_pipeline(token_t *tokens, int ntok, int *pos,
                          pipeline_t *pl) {
    memset(pl, 0, sizeof(*pl));

    int ret = parse_simple_cmd(tokens, ntok, pos, &pl->cmds[0]);
    if (ret <= 0) return ret;
    pl->ncmds = 1;

    while (*pos < ntok) {
        token_t *t = &tokens[*pos];
        if (t->type == TOK_PIPE || t->type == TOK_PIPE_BOTH) {
            int pipe_both = (t->type == TOK_PIPE_BOTH);
            (*pos)++;
            if (pl->ncmds >= MAX_PIPELINE) {
                fprintf(stderr, "sh: too many commands in pipeline\n");
                return -1;
            }
            ret = parse_simple_cmd(tokens, ntok, pos,
                                   &pl->cmds[pl->ncmds]);
            if (ret <= 0) {
                fprintf(stderr, "sh: syntax error near '|'\n");
                return -1;
            }
            /* Mark that the PREVIOUS command should send stderr too */
            if (pipe_both) {
                pl->cmds[pl->ncmds - 1].pipe_stderr = 1;
            }
            pl->ncmds++;
        } else {
            break;
        }
    }

    /* Check for trailing & (background) */
    if (*pos < ntok && tokens[*pos].type == TOK_BG) {
        pl->background = 1;
        (*pos)++;
    }

    return 1;
}

/*
 * Parse the full command list from the token stream.
 */
static int parse_command_list(token_t *tokens, int ntok,
                              cmd_entry_t *entries, int max_entries) {
    int nentries = 0;
    int pos = 0;
    enum connector next_conn = CONN_NONE;

    while (pos < ntok && tokens[pos].type != TOK_EOF) {
        if (nentries >= max_entries) {
            fprintf(stderr, "sh: too many commands\n");
            return -1;
        }

        entries[nentries].conn = next_conn;
        int ret = parse_pipeline(tokens, ntok, &pos,
                                 &entries[nentries].pipeline);
        if (ret < 0) return -1;
        if (ret == 0) {
            if (pos < ntok) { pos++; continue; }
            break;
        }
        nentries++;

        if (pos < ntok) {
            token_t *t = &tokens[pos];
            if (t->type == TOK_SEMI) {
                next_conn = CONN_SEMI;
                pos++;
            } else if (t->type == TOK_AND) {
                next_conn = CONN_AND;
                pos++;
            } else if (t->type == TOK_OR) {
                next_conn = CONN_OR;
                pos++;
            } else if (t->type == TOK_EOF) {
                break;
            }
        }
    }

    return nentries;
}

/* ------------------------------------------------------------------ */
/* Redirect application                                                 */
/* ------------------------------------------------------------------ */

static int apply_redirects(redirect_t *redirects, int nredirects) {
    for (int i = 0; i < nredirects; i++) {
        redirect_t *r = &redirects[i];
        int fd;

        switch (r->type) {
        case TOK_REDIR_OUT:
        case TOK_REDIR_OUT_FORCE:
            fd = open(r->filename, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->filename, strerror(errno));
                return -1;
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            break;

        case TOK_REDIR_APPEND:
            fd = open(r->filename, O_WRONLY | O_CREAT | O_APPEND);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->filename, strerror(errno));
                return -1;
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            break;

        case TOK_REDIR_IN:
            fd = open(r->filename, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->filename, strerror(errno));
                return -1;
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
            break;

        case TOK_REDIR_READWRITE:
            fd = open(r->filename, O_RDWR | O_CREAT);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->filename, strerror(errno));
                return -1;
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
            break;

        case TOK_REDIR_ERR:
            fd = open(r->filename, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->filename, strerror(errno));
                return -1;
            }
            dup2(fd, STDERR_FILENO);
            close(fd);
            break;

        case TOK_REDIR_ERR_APPEND:
            fd = open(r->filename, O_WRONLY | O_CREAT | O_APPEND);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->filename, strerror(errno));
                return -1;
            }
            dup2(fd, STDERR_FILENO);
            close(fd);
            break;

        case TOK_REDIR_ERR_TO_OUT:
            dup2(STDOUT_FILENO, STDERR_FILENO);
            break;

        case TOK_REDIR_BOTH_OUT:
        case TOK_REDIR_STDOUT_ERR:
            fd = open(r->filename, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->filename, strerror(errno));
                return -1;
            }
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            break;

        case TOK_REDIR_BOTH_APPEND:
            fd = open(r->filename, O_WRONLY | O_CREAT | O_APPEND);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->filename, strerror(errno));
                return -1;
            }
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            break;

        case TOK_HEREDOC: {
            int pfd[2];
            if (pipe(pfd) < 0) {
                fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
                return -1;
            }
            size_t hlen = strlen(r->filename);
            if (hlen > 0)
                write(pfd[1], r->filename, hlen);
            close(pfd[1]);
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            break;
        }

        case TOK_HERESTRING: {
            int pfd[2];
            if (pipe(pfd) < 0) {
                fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
                return -1;
            }
            size_t slen = strlen(r->filename);
            if (slen > 0)
                write(pfd[1], r->filename, slen);
            write(pfd[1], "\n", 1);
            close(pfd[1]);
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            break;
        }

        default:
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Here-document collection (interactive)                               */
/* ------------------------------------------------------------------ */

static void collect_heredocs(cmd_entry_t *entries, int nentries) {
    for (int e = 0; e < nentries; e++) {
        pipeline_t *pl = &entries[e].pipeline;
        for (int c = 0; c < pl->ncmds; c++) {
            simple_cmd_t *cmd = &pl->cmds[c];
            for (int r = 0; r < cmd->nredirects; r++) {
                if (cmd->redirects[r].type == TOK_HEREDOC) {
                    char delim[SHELL_MAX_LINE];
                    strncpy(delim, cmd->redirects[r].filename,
                            SHELL_MAX_LINE - 1);
                    delim[SHELL_MAX_LINE - 1] = '\0';

                    char content[SHELL_MAX_LINE * 4] = "";
                    char buf[SHELL_MAX_LINE];
                    while (1) {
                        printf("> ");
                        fflush(stdout);
                        if (!fgets(buf, sizeof(buf), stdin))
                            break;
                        size_t blen = strlen(buf);
                        if (blen > 0 && buf[blen - 1] == '\n')
                            buf[blen - 1] = '\0';
                        if (strcmp(buf, delim) == 0)
                            break;
                        strncat(content, buf,
                                sizeof(content) - strlen(content) - 2);
                        strncat(content, "\n",
                                sizeof(content) - strlen(content) - 1);
                    }
                    strncpy(cmd->redirects[r].filename, content,
                            SHELL_MAX_LINE - 1);
                    cmd->redirects[r].filename[SHELL_MAX_LINE - 1] = '\0';
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Built-in command check and execution                                 */
/* ------------------------------------------------------------------ */

static int is_builtin(const char *name) {
    return (strcmp(name, "cd") == 0 ||
            strcmp(name, "exit") == 0 ||
            strcmp(name, "help") == 0 ||
            strcmp(name, "history") == 0 ||
            strcmp(name, "export") == 0 ||
            strcmp(name, "alias") == 0 ||
            strcmp(name, "unalias") == 0 ||
            strcmp(name, "time") == 0);
}

static int run_builtin(int argc, char **argv) {
    if (strcmp(argv[0], "cd") == 0) {
        if (argc < 2) {
            const char *home = getenv("HOME");
            if (!home) home = "/";
            if (chdir(home) != 0) {
                fprintf(stderr, "cd: %s: %s\n", home, strerror(errno));
                return 1;
            }
        } else {
            if (chdir(argv[1]) != 0) {
                fprintf(stderr, "cd: %s: %s\n", argv[1], strerror(errno));
                return 1;
            }
        }
        return 0;
    }
    if (strcmp(argv[0], "help") == 0) {
        printf("LikeOS-64 Shell (userland)\n");
        printf("Built-in commands:\n");
        printf("  cd <dir>       - Change directory\n");
        printf("  alias [-p] [name[=value] ...] - Define/display aliases\n");
        printf("  unalias [-a] name ... - Remove alias definitions\n");
        printf("  time [-p] pipeline    - Time a pipeline\n");
        printf("  export [-fnp]  - Set/display exported variables\n");
        printf("  history [-cdanrwsp] - Command history\n");
        printf("  help           - Show this help\n");
        printf("  exit [N]       - Exit the shell with status N\n");
        printf("Operators:\n");
        printf("  |  >  >>  <  <<  <<<  2>  2>>  2>&1  &>  &>>\n");
        printf("  |&  >&  <>  >|  ;  &&  ||  &\n");
        printf("External commands (in /bin or /usr/local/bin):\n");
        printf("  ls cat pwd stat clear env more less touch cp mv rm\n");
        printf("  mkdir rmdir uname ps kill find df du hexdump\n");
        printf("  sort uniq cut tr yes true false time\n");
        printf("  grep head tail wc echo printf free uptime dmesg\n");
        printf("  which date sleep strings file top man nano tmux\n");
        printf("  shutdown reboot poweroff halt\n");
        printf("Networking commands:\n");
        printf("  ifconfig ping netstat route arp traceroute arping\n");
        printf("  dhclient dig nslookup host hostname nc\n");
        printf("Keyboard shortcuts:\n");
        printf("  Ctrl+D  - Debug dump\n");
        printf("  Ctrl+C  - Interrupt current process\n");
        return 0;
    }
    if (strcmp(argv[0], "history") == 0) {
        builtin_history(argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "export") == 0) {
        builtin_export(argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "alias") == 0) {
        builtin_alias(argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "unalias") == 0) {
        builtin_unalias(argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "time") == 0) {
        return builtin_time(argc, argv);
    }
    if (strcmp(argv[0], "exit") == 0) {
        int code = 0;
        if (argc > 1) code = (int)strtol(argv[1], NULL, 10);
        hist_save();
        _exit(code);
        return code;
    }
    return 127;
}

/* ------------------------------------------------------------------ */
/* Execute a simple command (fork + exec)                               */
/* ------------------------------------------------------------------ */

static int last_exit_status = 0;

/*
 * Fork and exec a single external command with process group control.
 * Used only for single (non-pipeline) commands.
 * Returns exit status.
 */
static int exec_single(simple_cmd_t *cmd, int background) {
    /* Snapshot tty state so we can restore it after the child exits.
     * A child that gets killed before resetting termios (e.g. SIGKILL of
     * nano) would otherwise leave the terminal in raw mode (OPOST off,
     * ECHO off, ICANON off), which causes shell output to staircase
     * (\n without \r) and silent unechoed input. */
    struct termios saved_tio;
    int tio_valid = (tcgetattr(STDIN_FILENO, &saved_tio) == 0);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: create own process group, become foreground */
        setpgid(0, 0);
        if (!background)
            tcsetpgrp(STDIN_FILENO, getpid());

        if (apply_redirects(cmd->redirects, cmd->nredirects) < 0)
            _exit(1);

        if (cmd->argc == 0) _exit(0);

        execvp(cmd->argv[0], cmd->argv);
        fprintf(stderr, "%s: command not found\n", cmd->argv[0]);
        _exit(127);
    } else if (pid < 0) {
        fprintf(stderr, "sh: fork: %s\n", strerror(errno));
        return 1;
    }

    /* Parent: also set child's process group (race-safe) */
    setpgid(pid, pid);

    if (background) {
        /* Build command string for job display */
        char cmdstr[128];
        cmdstr[0] = '\0';
        for (int i = 0; i < cmd->argc; i++) {
            if (i > 0) strncat(cmdstr, " ", sizeof(cmdstr) - strlen(cmdstr) - 1);
            strncat(cmdstr, cmd->argv[i], sizeof(cmdstr) - strlen(cmdstr) - 1);
        }
        int jn = bg_add_job(pid, cmdstr);
        printf("[%d] %d\n", jn, pid);
        return 0;
    }

    tcsetpgrp(STDIN_FILENO, pid);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Restore shell as foreground */
    tcsetpgrp(STDIN_FILENO, getpgrp());

    /* Restore termios in case the child exited without doing so. */
    if (tio_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (status & 0x7f);
}

/* ------------------------------------------------------------------ */
/* Execute a pipeline                                                   */
/* ------------------------------------------------------------------ */

static int exec_pipeline(pipeline_t *pl) {
    if (pl->ncmds == 0) return 0;

    /* Single command — check for builtins (only when not backgrounded) */
    if (pl->ncmds == 1 && !pl->background) {
        simple_cmd_t *cmd = &pl->cmds[0];
        if (cmd->argc > 0 && is_builtin(cmd->argv[0])) {
            /* Run builtin with optional redirections (save/restore fds) */
            int saved_in = -1, saved_out = -1, saved_err = -1;
            if (cmd->nredirects > 0) {
                saved_in  = dup(STDIN_FILENO);
                saved_out = dup(STDOUT_FILENO);
                saved_err = dup(STDERR_FILENO);
                if (apply_redirects(cmd->redirects, cmd->nredirects) < 0) {
                    if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
                    if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
                    if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
                    return 1;
                }
            }
            int ret = run_builtin(cmd->argc, cmd->argv);
            if (cmd->nredirects > 0) {
                if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
                if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
                if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
            }
            return ret;
        }
    }

    /* Single external command: use process groups (like original shell) */
    if (pl->ncmds == 1) {
        return exec_single(&pl->cmds[0], pl->background);
    }

    /*
     * Multi-command pipeline.
     * Children stay in the shell's process group to avoid races with
     * tcsetpgrp / setpgid that trigger kernel scheduler bugs.
     * All children are already in the foreground group so terminal
     * I/O works without SIGTTOU/SIGTTIN issues.
     */
    pid_t pids[MAX_PIPELINE];
    int prev_fd = -1;

    for (int i = 0; i < pl->ncmds; i++) {
        int pfd[2] = {-1, -1};
        int is_last = (i == pl->ncmds - 1);

        if (!is_last) {
            if (pipe(pfd) < 0) {
                fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
                return 1;
            }
        }

        int pipe_err = pl->cmds[i].pipe_stderr;
        pid_t pid = fork();
        if (pid == 0) {
            /* Child: NO setpgid — stay in shell's foreground group */

            /* Wire stdin from previous pipe */
            if (prev_fd != -1) {
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }

            /* Wire stdout to next pipe */
            if (!is_last) {
                close(pfd[0]);
                dup2(pfd[1], STDOUT_FILENO);
                if (pipe_err) {
                    dup2(pfd[1], STDERR_FILENO);
                }
                close(pfd[1]);
            }

            /* Apply file redirections */
            if (apply_redirects(pl->cmds[i].redirects,
                                pl->cmds[i].nredirects) < 0)
                _exit(1);

            if (pl->cmds[i].argc == 0) _exit(0);

            /* Run builtins inside the pipeline child process */
            if (is_builtin(pl->cmds[i].argv[0])) {
                int rc = run_builtin(pl->cmds[i].argc, pl->cmds[i].argv);
                fflush(stdout);
                fflush(stderr);
                _exit(rc);
            }

            execvp(pl->cmds[i].argv[0], pl->cmds[i].argv);
            fprintf(stderr, "%s: command not found\n", pl->cmds[i].argv[0]);
            _exit(127);
        } else if (pid < 0) {
            fprintf(stderr, "sh: fork: %s\n", strerror(errno));
            if (prev_fd != -1) close(prev_fd);
            if (!is_last) { close(pfd[0]); close(pfd[1]); }
            return 1;
        }

        pids[i] = pid;

        /* Parent: close used fds */
        if (prev_fd != -1) close(prev_fd);
        if (!is_last) {
            close(pfd[1]);
            prev_fd = pfd[0];
        } else {
            prev_fd = -1;
        }
    }

    if (pl->background) {
        /* Build command string for job display */
        char cmdstr[128];
        cmdstr[0] = '\0';
        for (int c = 0; c < pl->ncmds; c++) {
            if (c > 0) strncat(cmdstr, " | ", sizeof(cmdstr) - strlen(cmdstr) - 1);
            for (int a = 0; a < pl->cmds[c].argc; a++) {
                if (a > 0) strncat(cmdstr, " ", sizeof(cmdstr) - strlen(cmdstr) - 1);
                strncat(cmdstr, pl->cmds[c].argv[a], sizeof(cmdstr) - strlen(cmdstr) - 1);
            }
        }
        int jn = bg_add_job(pids[pl->ncmds - 1], cmdstr);
        printf("[%d] %d\n", jn, pids[pl->ncmds - 1]);
        return 0;
    }

    /* Wait for all children */
    int last_status = 0;
    for (int i = 0; i < pl->ncmds; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (i == pl->ncmds - 1) {
            if (WIFEXITED(status))
                last_status = WEXITSTATUS(status);
            else
                last_status = 128 + (status & 0x7f);
        }
    }

    return last_status;
}

/* ------------------------------------------------------------------ */
/* Execute the command list                                             */
/* ------------------------------------------------------------------ */

static void exec_command_list(cmd_entry_t *entries, int nentries) {
    for (int i = 0; i < nentries; i++) {
        cmd_entry_t *e = &entries[i];

        switch (e->conn) {
        case CONN_AND:
            if (last_exit_status != 0) continue;
            break;
        case CONN_OR:
            if (last_exit_status == 0) continue;
            break;
        case CONN_SEMI:
        case CONN_NONE:
            break;
        }

        last_exit_status = exec_pipeline(&e->pipeline);
    }
}

/* ------------------------------------------------------------------ */
/* Interactive line editor (readline-style)                             */
/* ------------------------------------------------------------------ */

/*
 * shell_readline - read a line with history and cursor editing
 *
 * Supports:
 *   Up/Down arrows   - browse command history
 *   Left/Right arrows - move cursor within line
 *   Home / End        - jump to beginning / end of line
 *   Backspace         - delete character before cursor
 *   Delete            - delete character at cursor
 *   Ctrl-A / Ctrl-E   - beginning / end of line
 *   Ctrl-U            - kill to beginning of line
 *   Ctrl-K            - kill to end of line
 *   Ctrl-W            - kill previous word
 *   Ctrl-L            - clear screen, redraw prompt+line
 *   Ctrl-D            - EOF if line is empty
 *
 * Returns number of chars in buf (>= 0), or -1 on EOF.
 */

static void rl_redraw(const char *prompt, int plen,
                       const char *buf, int len, int pos)
{
    /* Carriage return, print prompt, print buffer, clear to end, reposition */
    write(STDOUT_FILENO, "\r", 1);
    write(STDOUT_FILENO, prompt, plen);
    write(STDOUT_FILENO, buf, len);
    /* Clear everything to the right of the text */
    write(STDOUT_FILENO, "\033[K", 3);
    /* Move cursor back to the correct position */
    if (len - pos > 0) {
        char esc[32];
        int n = snprintf(esc, sizeof(esc), "\033[%dD", len - pos);
        write(STDOUT_FILENO, esc, n);
    }
}

static int shell_readline(const char *prompt, char *buf, int bufsz)
{
    struct termios orig, raw;
    int plen = (int)strlen(prompt);
    int len = 0;       /* current length of buf */
    int pos = 0;       /* cursor position in buf */
    int hist_idx = hist_count;  /* points one past the last entry (new line) */
    char saved_line[SHELL_MAX_LINE] = "";  /* saved current input when browsing history */

    /* Print prompt */
    write(STDOUT_FILENO, prompt, plen);

    /* Enter raw mode */
    tcgetattr(STDIN_FILENO, &orig);
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    buf[0] = '\0';

    while (1) {
        unsigned char c;
        int nread = read(STDIN_FILENO, &c, 1);
        if (nread <= 0) {
            /* EOF / error */
            tcsetattr(STDIN_FILENO, TCSANOW, &orig);
            return -1;
        }

        if (c == '\n' || c == '\r') {
            /* Accept line */
            write(STDOUT_FILENO, "\n", 1);
            buf[len] = '\0';
            tcsetattr(STDIN_FILENO, TCSANOW, &orig);
            return len;
        }

        if (c == 4) {
            /* Ctrl-D: EOF if line empty */
            if (len == 0) {
                tcsetattr(STDIN_FILENO, TCSANOW, &orig);
                return -1;
            }
            continue;
        }

        if (c == 127 || c == 8) {
            /* Backspace: delete char before cursor */
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos);
                pos--;
                len--;
                buf[len] = '\0';
                rl_redraw(prompt, plen, buf, len, pos);
            }
            continue;
        }

        if (c == 1) {
            /* Ctrl-A: go to beginning */
            pos = 0;
            rl_redraw(prompt, plen, buf, len, pos);
            continue;
        }

        if (c == 5) {
            /* Ctrl-E: go to end */
            pos = len;
            rl_redraw(prompt, plen, buf, len, pos);
            continue;
        }

        if (c == 21) {
            /* Ctrl-U: kill to beginning of line */
            if (pos > 0) {
                memmove(buf, buf + pos, len - pos);
                len -= pos;
                pos = 0;
                buf[len] = '\0';
                rl_redraw(prompt, plen, buf, len, pos);
            }
            continue;
        }

        if (c == 11) {
            /* Ctrl-K: kill to end of line */
            len = pos;
            buf[len] = '\0';
            rl_redraw(prompt, plen, buf, len, pos);
            continue;
        }

        if (c == 23) {
            /* Ctrl-W: kill previous word */
            if (pos > 0) {
                int old_pos = pos;
                /* Skip trailing spaces */
                while (pos > 0 && buf[pos - 1] == ' ') pos--;
                /* Skip word chars */
                while (pos > 0 && buf[pos - 1] != ' ') pos--;
                memmove(buf + pos, buf + old_pos, len - old_pos);
                len -= (old_pos - pos);
                buf[len] = '\0';
                rl_redraw(prompt, plen, buf, len, pos);
            }
            continue;
        }

        if (c == 12) {
            /* Ctrl-L: clear screen, redraw */
            write(STDOUT_FILENO, "\033[2J\033[H", 7);
            rl_redraw(prompt, plen, buf, len, pos);
            continue;
        }

        if (c == 27) {
            /* Escape sequence: read next chars */
            unsigned char seq[4];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;

            if (seq[0] == '[') {
                if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;

                switch (seq[1]) {
                case 'A':
                    /* Up arrow: previous history entry */
                    if (hist_idx > 0) {
                        /* Save current line if we're just starting to browse */
                        if (hist_idx == hist_count) {
                            strncpy(saved_line, buf, SHELL_MAX_LINE - 1);
                            saved_line[SHELL_MAX_LINE - 1] = '\0';
                        }
                        hist_idx--;
                        strncpy(buf, hist_buf[hist_real(hist_idx)], bufsz - 1);
                        buf[bufsz - 1] = '\0';
                        len = (int)strlen(buf);
                        pos = len;
                        rl_redraw(prompt, plen, buf, len, pos);
                    }
                    break;

                case 'B':
                    /* Down arrow: next history entry */
                    if (hist_idx < hist_count) {
                        hist_idx++;
                        if (hist_idx == hist_count) {
                            /* Restore the saved current line */
                            strncpy(buf, saved_line, bufsz - 1);
                            buf[bufsz - 1] = '\0';
                        } else {
                            strncpy(buf, hist_buf[hist_real(hist_idx)], bufsz - 1);
                            buf[bufsz - 1] = '\0';
                        }
                        len = (int)strlen(buf);
                        pos = len;
                        rl_redraw(prompt, plen, buf, len, pos);
                    }
                    break;

                case 'C':
                    /* Right arrow: move cursor right */
                    if (pos < len) {
                        pos++;
                        rl_redraw(prompt, plen, buf, len, pos);
                    }
                    break;

                case 'D':
                    /* Left arrow: move cursor left */
                    if (pos > 0) {
                        pos--;
                        rl_redraw(prompt, plen, buf, len, pos);
                    }
                    break;

                case 'H':
                    /* Home key */
                    pos = 0;
                    rl_redraw(prompt, plen, buf, len, pos);
                    break;

                case 'F':
                    /* End key */
                    pos = len;
                    rl_redraw(prompt, plen, buf, len, pos);
                    break;

                case '3':
                    /* Delete key: ESC [ 3 ~ */
                    {
                        unsigned char tilde;
                        if (read(STDIN_FILENO, &tilde, 1) > 0 && tilde == '~') {
                            if (pos < len) {
                                memmove(buf + pos, buf + pos + 1, len - pos - 1);
                                len--;
                                buf[len] = '\0';
                                rl_redraw(prompt, plen, buf, len, pos);
                            }
                        }
                    }
                    break;

                case '1':
                    /* Home key variant: ESC [ 1 ~ */
                    {
                        unsigned char tilde;
                        if (read(STDIN_FILENO, &tilde, 1) > 0 && tilde == '~') {
                            pos = 0;
                            rl_redraw(prompt, plen, buf, len, pos);
                        }
                    }
                    break;

                case '4':
                    /* End key variant: ESC [ 4 ~ */
                    {
                        unsigned char tilde;
                        if (read(STDIN_FILENO, &tilde, 1) > 0 && tilde == '~') {
                            pos = len;
                            rl_redraw(prompt, plen, buf, len, pos);
                        }
                    }
                    break;

                default:
                    break;
                }
            } else if (seq[0] == 'O') {
                /* ESC O H = Home, ESC O F = End (alternate sequences) */
                if (read(STDIN_FILENO, &seq[1], 1) > 0) {
                    if (seq[1] == 'H') {
                        pos = 0;
                        rl_redraw(prompt, plen, buf, len, pos);
                    } else if (seq[1] == 'F') {
                        pos = len;
                        rl_redraw(prompt, plen, buf, len, pos);
                    }
                }
            }
            continue;
        }

        /* Normal printable character: insert at cursor position */
        if (c >= 32 && len < bufsz - 1) {
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos] = (char)c;
            pos++;
            len++;
            buf[len] = '\0';
            rl_redraw(prompt, plen, buf, len, pos);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Prompt                                                               */
/* ------------------------------------------------------------------ */

static void get_prompt(char *prompt, int maxlen) {
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strcpy(cwd, "/");
    }
    if (strcmp(cwd, "/") == 0) {
        snprintf(prompt, maxlen, "/ # ");
    } else {
        snprintf(prompt, maxlen, "%s # ", cwd);
    }
    ioctl(STDIN_FILENO, TIOCSGUARD, NULL);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    setenv("PATH", "/bin:/usr/local/bin", 1);
    /* Default TERM so terminfo-using programs (tmux, less, ...) get a
     * usable capability list.  All consoles on LikeOS speak the same
     * ANSI/xterm sequences our ncurses-likeos stub emits, so xterm
     * is a safe baseline. */
    setenv("TERM", "xterm-256color", 0);
    /* Read /etc/resolv.conf and program the kernel resolver before
     * any DNS-using program (dig, ping, dhclient -x, ...) runs. DHCP
     * option 6 will overwrite per-interface settings later. */
    res_init();
    hist_load();
    alias_load();

    char line[SHELL_MAX_LINE];
    char prompt[512];
    static token_t tokens[128];
    static cmd_entry_t entries[MAX_COMMANDS];

    while (1) {
        bg_check_jobs();
        get_prompt(prompt, sizeof(prompt));
        int rc = shell_readline(prompt, line, sizeof(line));
        if (rc < 0) {
            /* EOF */
            continue;
        }
        if (line[0] == '\0') {
            continue;
        }

        /* Save original line for history */
        char hist_line[SHELL_MAX_LINE];
        strncpy(hist_line, line, SHELL_MAX_LINE - 1);
        hist_line[SHELL_MAX_LINE - 1] = '\0';

        /* Add to history (skip duplicates of the last entry) */
        if (hist_count == 0 ||
            strcmp(hist_buf[hist_real(hist_count - 1)], hist_line) != 0) {
            hist_add(hist_line);
        }

        /* Expand aliases on the command line */
        alias_expand(line, SHELL_MAX_LINE);

        /* Tokenize */
        int ntok = tokenize(line, tokens,
                            (int)(sizeof(tokens) / sizeof(tokens[0])));
        if (ntok == 0) continue;

        /* Parse into command list */
        int nentries = parse_command_list(tokens, ntok, entries, MAX_COMMANDS);
        if (nentries <= 0) continue;

        /* Collect here-documents interactively */
        collect_heredocs(entries, nentries);

        /* Execute */
        exec_command_list(entries, nentries);
    }
}
