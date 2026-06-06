/*
 * clear - clear the terminal screen
 *
 * Implementation per clear(1) manpage.
 * Clears the terminal screen and its scrollback buffer.
 * Uses ANSI/xterm escape sequences.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "clear"
#define VERSION      "1.0"

static void usage(void) {
    fprintf(stderr, "Usage: %s [-x] [-T terminal-type]\n", PROGRAM_NAME);
    fprintf(stderr, "       %s -V\n", PROGRAM_NAME);
}

int main(int argc, char *argv[]) {
    int opt_no_scrollback = 0;   /* -x: don't clear scrollback */
    const char *term_type = NULL; /* -T type */
    int opt_version = 0;         /* -V */

    static struct option long_options[] = {
        { NULL, 0, NULL, 0 }
    };

    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "xT:V", long_options, NULL)) != -1) {
        switch (c) {
        case 'x':
            opt_no_scrollback = 1;
            break;
        case 'T':
            term_type = optarg;
            break;
        case 'V':
            opt_version = 1;
            break;
        default:
            usage();
            return EXIT_FAILURE;
        }
    }

    if (opt_version) {
        printf("clear (%s) %s\n", PROGRAM_NAME, VERSION);
        return EXIT_SUCCESS;
    }

    /*
     * -T type: in a full terminfo implementation we'd look up capabilities.
     * For this system we always use ANSI/xterm sequences, so -T is accepted
     * but the type value is ignored (our console is always ANSI-compatible).
     * When -T is given, we also ignore LINES/COLUMNS environment variables
     * (as documented), but that's only relevant for terminfo sizing which
     * doesn't apply here.
     */
    (void)term_type;

    /*
     * Clear screen: ESC[2J moves cursor to home implicitly on many terminals,
     * but we also send ESC[H to be safe.
     *
     * Clear scrollback: ESC[3J (xterm extension, widely supported).
     */
    if (opt_no_scrollback) {
        /* Clear visible screen only */
        write(STDOUT_FILENO, "\033[H\033[2J", 7);
    } else {
        /* Clear visible screen + scrollback buffer */
        write(STDOUT_FILENO, "\033[H\033[2J\033[3J", 11);
    }

    return EXIT_SUCCESS;
}
