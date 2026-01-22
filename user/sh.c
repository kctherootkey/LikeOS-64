#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>

#define SHELL_MAX_LINE 256
#define SHELL_MAX_ARGS 16

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

static int tokenize(char* line, char** argv, int max_args) {
    int argc = 0;
    char* p = line;
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

static void show_help(void) {
    printf("LikeOS-64 Shell (userland)\n");
    printf("  cd <dir>       - Change directory\n");
    printf("  help           - Show this help\n");
    printf("  ls, cat, pwd, stat are external commands in /bin\n");
    printf("  <cmd> [args]   - Execute program via PATH\n");
}

int main(void) {
    setenv("PATH", "/bin:/", 1);

    char line[SHELL_MAX_LINE];
    char* argv[SHELL_MAX_ARGS + 1];

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

        int argc = tokenize(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) {
            continue;
        }

        if (strcmp(argv[0], "help") == 0) {
            show_help();
            continue;
        }
        if (strcmp(argv[0], "cd") == 0) {
            if (argc < 2) {
                printf("Usage: cd <dir>\n");
                continue;
            }
            if (chdir(argv[1]) != 0) {
                printf("cd: failed (%d)\n", errno);
            }
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            execvp(argv[0], argv);
            printf("exec: not found: %s\n", argv[0]);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        } else {
            printf("fork failed\n");
        }
    }
}
