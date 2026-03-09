/*
 * time - run programs and summarize system resource usage
 *
 * Usage: time [options] command [arguments...]
 *
 * Full implementation per the time(1) manpage (GNU time).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>

#define TIME_VERSION "time (LikeOS) 0.1"

/* Default format (POSIX): real, user, sys */
#define POSIX_FORMAT "real %e\nuser %U\nsys %S\n"
/* GNU verbose format */
#define VERBOSE_FORMAT \
    "\tCommand being timed: \"%C\"\n" \
    "\tUser time (seconds): %U\n" \
    "\tSystem time (seconds): %S\n" \
    "\tElapsed (wall clock) time (h:mm:ss or m:ss): %E\n" \
    "\tMaximum resident set size (kbytes): %M\n" \
    "\tMajor (requiring I/O) page faults: %F\n" \
    "\tMinor (reclaiming a frame) page faults: %R\n" \
    "\tVoluntary context switches: %w\n" \
    "\tInvoluntary context switches: %c\n" \
    "\tPage size (bytes): %Z\n" \
    "\tExit status: %x\n"
/* Default GNU format */
#define GNU_DEFAULT_FORMAT "%Uuser %Ssystem %Eelapsed %PCPU (%Xtext+%Ddata %Mmax)k\n%Iinputs+%Ooutputs (%Fmajor+%Rminor)pagefaults %Wswaps\n"

static void print_help(void)
{
    printf("Usage: time [options] command [arguments...]\n\n");
    printf("Run COMMAND, then print system resource usage.\n\n");
    printf("Options:\n");
    printf("  -a, --append          (with -o) append instead of overwrite\n");
    printf("  -f FORMAT, --format FORMAT\n");
    printf("                        use FORMAT as the output format string\n");
    printf("  -o FILE, --output FILE\n");
    printf("                        write result to FILE instead of stderr\n");
    printf("  -p, --portability     use POSIX output format\n");
    printf("  -q, --quiet           do not print anything to stderr for the resource usage\n");
    printf("  -v, --verbose         use verbose output format\n");
    printf("  -V, --version         display version and exit\n");
    printf("      --help            display this help and exit\n");
    printf("\nFormat specifiers:\n");
    printf("  %%C  Name and command-line arguments\n");
    printf("  %%D  Average unshared data size (kbytes)\n");
    printf("  %%E  Elapsed real time ([hours:]minutes:seconds)\n");
    printf("  %%e  Elapsed real time (seconds)\n");
    printf("  %%F  Major page faults\n");
    printf("  %%I  File system inputs\n");
    printf("  %%K  Average total memory use (kbytes)\n");
    printf("  %%M  Maximum resident set size (kbytes)\n");
    printf("  %%O  File system outputs\n");
    printf("  %%P  Percent of CPU this job got\n");
    printf("  %%R  Minor page faults\n");
    printf("  %%S  System (kernel) time (seconds)\n");
    printf("  %%U  User time (seconds)\n");
    printf("  %%W  Times process was swapped\n");
    printf("  %%X  Average shared text size (kbytes)\n");
    printf("  %%Z  System's page size (bytes)\n");
    printf("  %%c  Involuntary context switches\n");
    printf("  %%e  Elapsed real (wall clock) time in seconds\n");
    printf("  %%k  Signals delivered\n");
    printf("  %%p  Average unshared stack size (kbytes)\n");
    printf("  %%r  Socket messages received\n");
    printf("  %%s  Socket messages sent\n");
    printf("  %%t  Average resident set size (kbytes)\n");
    printf("  %%w  Voluntary context switches\n");
    printf("  %%x  Exit status of command\n");
}

/* Format elapsed time as [h:]mm:ss or m:ss.ss */
static void format_elapsed(char *buf, int bufsz, double elapsed)
{
    int total_secs = (int)elapsed;
    int hours = total_secs / 3600;
    int mins = (total_secs % 3600) / 60;
    int secs = total_secs % 60;
    int hundredths = (int)((elapsed - (double)total_secs) * 100.0);

    if (hours > 0)
        snprintf(buf, bufsz, "%d:%02d:%02d", hours, mins, secs);
    else
        snprintf(buf, bufsz, "%d:%02d.%02d", mins, secs, hundredths);
}

static void print_format(FILE *out, const char *fmt,
                         const char *command_line,
                         double real_secs, double user_secs, double sys_secs,
                         struct rusage *ru, int exit_status)
{
    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] == '\\') {
            i++;
            switch (fmt[i]) {
            case 'n': fputc('\n', out); break;
            case 't': fputc('\t', out); break;
            case '\\': fputc('\\', out); break;
            default: fputc('\\', out); fputc(fmt[i], out); break;
            }
            continue;
        }
        if (fmt[i] != '%') {
            fputc(fmt[i], out);
            continue;
        }
        i++;
        if (!fmt[i]) break;

        char tmp[256];
        switch (fmt[i]) {
        case '%':
            fputc('%', out);
            break;
        case 'C':
            fputs(command_line, out);
            break;
        case 'D':
            fprintf(out, "0");
            break;
        case 'E':
            format_elapsed(tmp, sizeof(tmp), real_secs);
            fputs(tmp, out);
            break;
        case 'e':
            fprintf(out, "%.2f", real_secs);
            break;
        case 'F':
            fprintf(out, "%ld", ru->ru_majflt);
            break;
        case 'I':
            fprintf(out, "0"); /* not tracked */
            break;
        case 'K':
            fprintf(out, "0"); /* not tracked */
            break;
        case 'M':
            fprintf(out, "%ld", ru->ru_maxrss);
            break;
        case 'O':
            fprintf(out, "0"); /* not tracked */
            break;
        case 'P': {
            double total_cpu = user_secs + sys_secs;
            if (real_secs > 0.0) {
                int pct = (int)(total_cpu / real_secs * 100.0);
                fprintf(out, "%d%%", pct);
            } else {
                fprintf(out, "0%%");
            }
            break;
        }
        case 'R':
            fprintf(out, "%ld", ru->ru_minflt);
            break;
        case 'S':
            fprintf(out, "%.2f", sys_secs);
            break;
        case 'U':
            fprintf(out, "%.2f", user_secs);
            break;
        case 'W':
            fprintf(out, "0"); /* swaps not tracked */
            break;
        case 'X':
            fprintf(out, "0"); /* shared text not tracked */
            break;
        case 'Z':
            fprintf(out, "4096"); /* page size */
            break;
        case 'c':
            fprintf(out, "%ld", ru->ru_nivcsw);
            break;
        case 'k':
            fprintf(out, "0"); /* signals not tracked */
            break;
        case 'p':
            fprintf(out, "0"); /* stack not tracked */
            break;
        case 'r':
            fprintf(out, "0"); /* socket not tracked */
            break;
        case 's':
            fprintf(out, "0"); /* socket not tracked */
            break;
        case 't':
            fprintf(out, "0"); /* avg RSS not tracked */
            break;
        case 'w':
            fprintf(out, "%ld", ru->ru_nvcsw);
            break;
        case 'x':
            fprintf(out, "%d", exit_status);
            break;
        default:
            fputc('%', out);
            fputc(fmt[i], out);
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    const char *format = GNU_DEFAULT_FORMAT;
    const char *output_file = NULL;
    int append_mode = 0;
    int quiet_mode = 0;
    int portability = 0;
    int verbose = 0;

    /* We need to be careful: options before the command belong to time,
     * everything after belongs to the command. Stop at first non-option. */
    static struct option long_options[] = {
        {"append",      no_argument,       0, 'a'},
        {"format",      required_argument, 0, 'f'},
        {"output",      required_argument, 0, 'o'},
        {"portability", no_argument,       0, 'p'},
        {"quiet",       no_argument,       0, 'q'},
        {"verbose",     no_argument,       0, 'v'},
        {"version",     no_argument,       0, 'V'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "+af:o:pqvVh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'a': append_mode = 1; break;
        case 'f': format = optarg; break;
        case 'o': output_file = optarg; break;
        case 'p': portability = 1; break;
        case 'q': quiet_mode = 1; break;
        case 'v': verbose = 1; break;
        case 'V': printf("%s\n", TIME_VERSION); return 0;
        case 'h': print_help(); return 0;
        default:
            fprintf(stderr, "Try 'time --help' for more information.\n");
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "time: missing command\n");
        fprintf(stderr, "Try 'time --help' for more information.\n");
        return 1;
    }

    if (portability)
        format = POSIX_FORMAT;
    if (verbose)
        format = VERBOSE_FORMAT;

    /* Build the command line string for %C */
    char command_line[4096];
    command_line[0] = '\0';
    for (int i = optind; i < argc; i++) {
        if (i > optind) strcat(command_line, " ");
        strncat(command_line, argv[i], sizeof(command_line) - strlen(command_line) - 2);
    }

    /* Record start time */
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* Fork and exec */
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "time: cannot fork: %s\n", strerror(errno));
        return 127;
    }

    if (child == 0) {
        /* Child: exec the command */
        execvp(argv[optind], &argv[optind]);
        /* If execvp returns, it failed */
        int err = errno;
        fprintf(stderr, "time: cannot run %s: %s\n", argv[optind], strerror(err));
        _exit(err == ENOENT ? 127 : 126);
    }

    /* Parent: wait for child */
    int status = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));

    pid_t wpid = wait4(child, &status, 0, &ru);
    (void)wpid;

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    /* Calculate times */
    double real_secs = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                       (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000000000.0;
    double user_secs = (double)ru.ru_utime.tv_sec +
                       (double)ru.ru_utime.tv_usec / 1000000.0;
    double sys_secs = (double)ru.ru_stime.tv_sec +
                      (double)ru.ru_stime.tv_usec / 1000000.0;

    int exit_status = 0;
    if (WIFEXITED(status))
        exit_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        exit_status = 128 + WTERMSIG(status);

    /* Output the resource usage */
    if (!quiet_mode) {
        FILE *out = stderr;
        if (output_file) {
            out = fopen(output_file, append_mode ? "a" : "w");
            if (!out) {
                fprintf(stderr, "time: cannot open '%s': %s\n",
                        output_file, strerror(errno));
                out = stderr;
            }
        }

        print_format(out, format, command_line,
                     real_secs, user_secs, sys_secs, &ru, exit_status);

        if (out != stderr)
            fclose(out);
    }

    return exit_status;
}
