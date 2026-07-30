/*
 * tty - print the file name of the terminal connected to standard input
 *
 * POSIX tty(1).  Two things distinguish it from a one-line wrapper around
 * ttyname(): the EXIT STATUS is the point of the program as often as the
 * output is, and -s asks for the status alone.  Scripts use
 *
 *     if tty -s; then ... ; fi
 *
 * to decide whether they are interactive, and
 *
 *     tty=$(tty)
 *
 * to find out which terminal they are on -- startx does the second to work out
 * whether it was started from a virtual console.
 *
 * Exit status:
 *   0  standard input is a terminal
 *   1  standard input is not a terminal
 *   2  invalid option
 *   3  a write error occurred
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#define PROGRAM_NAME "tty"
#define VERSION "1.0"

static void usage(int status) __attribute__((noreturn));

static void usage(int status)
{
	FILE *out = status == 0 ? stdout : stderr;

	fprintf(out,
		"Usage: tty [OPTION]...\n"
		"Print the file name of the terminal connected to standard input.\n\n"
		"  -s, --silent, --quiet   print nothing, only return an exit status\n"
		"      --help              display this help and exit\n"
		"      --version           output version information and exit\n\n"
		"Exit status is 0 if standard input is a terminal, 1 if not,\n"
		"2 for an invalid option and 3 on a write error.\n");
	exit(status);
}

int main(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "silent", no_argument, 0, 's' },
		{ "quiet", no_argument, 0, 's' },
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'v' },
		{ 0, 0, 0, 0 }
	};
	int silent = 0;
	int c;
	char *name;

	while ((c = getopt_long(argc, argv, "s", longopts, NULL)) != -1) {
		switch (c) {
		case 's':
			silent = 1;
			break;
		case 'h':
			usage(0);
		case 'v':
			printf("%s %s\n", PROGRAM_NAME, VERSION);
			return 0;
		default:
			/* 2, not 1: an invalid option must not be mistaken for
			 * the ordinary "not a terminal" answer. */
			usage(2);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "tty: extra operand '%s'\n", argv[optind]);
		usage(2);
	}

	name = ttyname(STDIN_FILENO);

	if (!silent) {
		if (name)
			puts(name);
		else
			/* Reported on stdout, not stderr: this is the answer to
			 * the question asked, not a failure of the program. */
			puts("not a tty");

		/* A closed or full stdout has to be reported.  Without this a
		 * `tty > /full/disk` would exit 0 having written nothing. */
		if (fflush(stdout) != 0 || ferror(stdout)) {
			fprintf(stderr, "tty: write error: %s\n",
				strerror(errno));
			return 3;
		}
	}

	return name ? 0 : 1;
}
