/*
 * sh - LikeOS-64 userland shell
 *
 * Built-in commands: cd, help, history, export, exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>

#define SHELL_MAX_LINE 256
#define SHELL_MAX_ARGS 16

/* ------------------------------------------------------------------ */
/* Command history                                                      */
/* ------------------------------------------------------------------ */
#define HIST_MAX     500
#define HIST_FILE    "/.sh_history"

static char  hist_buf[HIST_MAX][SHELL_MAX_LINE];
static int   hist_count;      /* total entries stored */
static int   hist_start;      /* ring-buffer start index */

static int hist_real(int i) { return (hist_start + i) % HIST_MAX; }

/* Add a line to the history ring buffer. */
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

/* Read history from HIST_FILE (one entry per line). */
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

/* Write entire history to HIST_FILE. */
static void hist_save(void) {
    FILE *f = fopen(HIST_FILE, "w");
    if (!f) return;
    for (int i = 0; i < hist_count; i++) {
        fputs(hist_buf[hist_real(i)], f);
        fputc('\n', f);
    }
    fclose(f);
}

/* Append new entries to HIST_FILE. */
static void hist_append(void) {
    FILE *f = fopen(HIST_FILE, "a");
    if (!f) return;
    /* Just append the last entry. */
    if (hist_count > 0) {
        int idx = hist_real(hist_count - 1);
        fputs(hist_buf[idx], f);
        fputc('\n', f);
    }
    fclose(f);
}

/* Re-read HIST_FILE (load new entries from other sessions). */
static void hist_reread(void) {
    hist_count = 0;
    hist_start = 0;
    hist_load();
}

/* Delete entry at offset (1-based). */
static void hist_delete(int offset) {
    if (offset < 1 || offset > hist_count) {
        fprintf(stderr, "history: %d: invalid offset\n", offset);
        return;
    }
    int idx = offset - 1;
    /* Shift entries down */
    for (int i = idx; i < hist_count - 1; i++) {
        int ri = hist_real(i);
        int ri1 = hist_real(i + 1);
        strcpy(hist_buf[ri], hist_buf[ri1]);
    }
    hist_count--;
}

/* history builtin handler.
 *   history           - show all
 *   history N         - show last N
 *   history -c        - clear
 *   history -d OFF    - delete entry at offset
 *   history -a        - append new to file
 *   history -n        - re-read file
 *   history -r        - read file (replace)
 *   history -w        - write to file
 *   history -s args.. - add args as single entry
 */
static void builtin_history(int argc, char **argv) {
    if (argc == 1) {
        /* Print all history with line numbers */
        for (int i = 0; i < hist_count; i++)
            printf(" %4d  %s\n", i + 1, hist_buf[hist_real(i)]);
        return;
    }

    /* history N */
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

    /* Option flags */
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
            /* Remaining args joined as single history entry */
            char entry[SHELL_MAX_LINE] = "";
            for (int j = a + 1; j < argc; j++) {
                if (j > a + 1) strncat(entry, " ", sizeof(entry) - strlen(entry) - 1);
                strncat(entry, argv[j], sizeof(entry) - strlen(entry) - 1);
            }
            if (entry[0]) hist_add(entry);
            return;
        } else if (strcmp(argv[a], "-p") == 0) {
            /* -p: print expansion of args without storing */
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

/*
 * export [-fn] [-p] [name[=value] ...]
 *   -p         display all exported variables
 *   -n         remove export property (unsetenv)
 *   -f         ignored (no function support)
 *   name=value set and export
 *   name       export existing variable
 */
static void builtin_export(int argc, char **argv) {
    int opt_print = 0;
    int opt_unset = 0;

    /* Parse leading option flags */
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (argv[i][0] != '-') break;
        for (const char *p = &argv[i][1]; *p; p++) {
            switch (*p) {
            case 'p': opt_print = 1; break;
            case 'n': opt_unset = 1; break;
            case 'f': break; /* ignored - no functions */
            default:
                fprintf(stderr, "export: -%c: invalid option\n", *p);
                return;
            }
        }
    }

    /* If -p or no arguments, print all exported variables */
    if (opt_print || i >= argc) {
        int cookie = 0;
        const char *name, *value;
        while (env_iter(&cookie, &name, &value)) {
            printf("declare -x %s=\"%s\"\n", name, value);
        }
        return;
    }

    /* Process name[=value] arguments */
    for (; i < argc; i++) {
        if (opt_unset) {
            /* -n: remove from environment */
            unsetenv(argv[i]);
            continue;
        }
        char *eq = strchr(argv[i], '=');
        if (eq) {
            /* name=value: set and export */
            *eq = '\0';
            setenv(argv[i], eq + 1, 1);
            *eq = '=';
        } else {
            /* Just a name: if already set, it's already "exported" (in env).
             * If not set, export as empty. */
            if (!getenv(argv[i])) {
                setenv(argv[i], "", 1);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Prompt and tokenizer                                                */
/* ------------------------------------------------------------------ */

static void print_prompt(void) {
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strcpy(cwd, "/");
    }
    if (strcmp(cwd, "/") == 0) {
        printf("/ # ");
    } else {
        printf("%s # ", cwd);
    }
    fflush(stdout);
    ioctl(STDIN_FILENO, TIOCSGUARD, NULL);
}

static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    argv[argc] = NULL;
    return argc;
}

/* ------------------------------------------------------------------ */
/* Help                                                                 */
/* ------------------------------------------------------------------ */

static void show_help(void) {
    printf("LikeOS-64 Shell (userland)\n");
    printf("Built-in commands:\n");
    printf("  cd <dir>       - Change directory\n");
    printf("  export [-fnp]  - Set/display exported variables\n");
    printf("  history [-cdanrwsp] - Command history\n");
    printf("  help           - Show this help\n");
    printf("  exit [N]       - Exit the shell with status N\n");
    printf("External commands (in /bin or /usr/local/bin):\n");
    printf("  ls             - List directory contents\n");
    printf("  cat <file>     - Display file contents\n");
    printf("  pwd            - Print working directory\n");
    printf("  stat <file>    - Show file status\n");
    printf("  clear          - Clear terminal screen\n");
    printf("  env            - Run program in modified environment\n");
    printf("  more <file>    - View file contents (pager)\n");
    printf("  less <file>    - View file contents (advanced pager)\n");
    printf("  touch <file>   - Create empty file\n");
    printf("  cp src dst     - Copy files\n");
    printf("  mv src dst     - Move/rename files\n");
    printf("  rm <file>      - Remove files\n");
    printf("  mkdir <dir>    - Create directory\n");
    printf("  rmdir <dir>    - Remove empty directory\n");
    printf("  uname [-a]     - System information\n");
    printf("  ps [aux]       - List processes\n");
    printf("  shutdown/reboot/poweroff/halt\n");
    printf("Keyboard shortcuts:\n");
    printf("  Ctrl+D         - Debug dump (threads, tasks, IRQs, memory)\n");
    printf("  Ctrl+C         - Interrupt current process\n");
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    setenv("PATH", "/bin:/usr/local/bin", 1);

    /* Load history from file */
    hist_load();

    char line[SHELL_MAX_LINE];
    char *argv[SHELL_MAX_ARGS + 1];

    while (1) {
        print_prompt();
        if (!fgets(line, sizeof(line), stdin)) {
            continue;
        }
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        if (line[0] == '\0') {
            continue;
        }

        /* Save original line for history before tokenize mutilates it */
        char hist_line[SHELL_MAX_LINE];
        strncpy(hist_line, line, SHELL_MAX_LINE - 1);
        hist_line[SHELL_MAX_LINE - 1] = '\0';

        int argc = tokenize(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) {
            continue;
        }

        /* Add to history (skip duplicates of the last entry) */
        if (hist_count == 0 ||
            strcmp(hist_buf[hist_real(hist_count - 1)], hist_line) != 0) {
            hist_add(hist_line);
        }

        /* Built-in: help */
        if (strcmp(argv[0], "help") == 0) {
            show_help();
            continue;
        }

        /* Built-in: exit */
        if (strcmp(argv[0], "exit") == 0) {
            int code = 0;
            if (argc > 1) code = (int)strtol(argv[1], NULL, 10);
            hist_save();
            _exit(code);
        }

        /* Built-in: cd */
        if (strcmp(argv[0], "cd") == 0) {
            if (argc < 2) {
                /* cd with no args → go home (or /) */
                const char *home = getenv("HOME");
                if (!home) home = "/";
                if (chdir(home) != 0)
                    printf("cd: %s: %s\n", home, strerror(errno));
            } else {
                if (chdir(argv[1]) != 0)
                    printf("cd: %s: %s\n", argv[1], strerror(errno));
            }
            continue;
        }

        /* Built-in: history */
        if (strcmp(argv[0], "history") == 0) {
            builtin_history(argc, argv);
            continue;
        }

        /* Built-in: export */
        if (strcmp(argv[0], "export") == 0) {
            builtin_export(argc, argv);
            continue;
        }

        /* External command */
        pid_t pid = fork();
        if (pid == 0) {
            /* Set child as foreground process group to receive Ctrl+C */
            setpgid(0, 0);
            tcsetpgrp(STDIN_FILENO, getpid());

            execvp(argv[0], argv);
            fprintf(stderr, "%s: command not found\n", argv[0]);
            _exit(127);
        } else if (pid > 0) {
            /* Set child as foreground process group (also from parent side) */
            setpgid(pid, pid);
            tcsetpgrp(STDIN_FILENO, pid);

            int status = 0;
            waitpid(pid, &status, 0);

            /* Restore shell as foreground process group */
            tcsetpgrp(STDIN_FILENO, getpgrp());
        } else {
            printf("fork failed\n");
        }
    }
}
