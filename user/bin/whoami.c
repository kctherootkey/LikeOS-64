/*
 * whoami - print effective user name
 *
 * Usage: whoami [OPTION]...
 * Print the user name associated with the current effective user ID.
 * Same as id -un.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <pwd.h>

#define VERSION_STRING "whoami (LikeOS coreutils) 0.2"

int main(int argc, char **argv)
{
	static struct option long_opts[] = {
		{ "help", no_argument, 0, 1 },
		{ "version", no_argument, 0, 2 },
		{ 0, 0, 0, 0 }
	};
	int c;

	while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
		switch (c) {
		case 1:
			printf("Usage: %s [OPTION]...\n"
			       "Print the user name associated with the current"
			       " effective user ID.\nSame as id -un.\n\n"
			       "      --help     display this help and exit\n"
			       "      --version  output version information and exit\n",
			       argv[0]);
			return 0;
		case 2:
			printf("%s\n", VERSION_STRING);
			return 0;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n",
			        argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "%s: extra operand '%s'\n", argv[0],
		        argv[optind]);
		return 1;
	}

	uid_t euid = geteuid();
	struct passwd *pw = getpwuid(euid);
	if (!pw) {
		fprintf(stderr, "%s: cannot find name for user ID %u: %s\n",
		        argv[0], (unsigned)euid, strerror(errno ? errno : 0));
		return 1;
	}
	printf("%s\n", pw->pw_name);
	return 0;
}
