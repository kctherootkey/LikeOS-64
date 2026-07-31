/*
 * nproc - print the number of processing units available
 *
 * Usage: nproc [OPTION]...
 * Print the number of processing units available to the current process,
 * which may be fewer than the number of online processors.
 *
 * The count comes from sysconf(_SC_NPROCESSORS_ONLN), which derives it from
 * the scheduling affinity mask.  --all reports the configured count instead;
 * the two are the same here because processors are never taken offline at
 * runtime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>

#define VERSION_STRING "nproc (LikeOS coreutils) 0.2"

int main(int argc, char **argv)
{
	static struct option long_opts[] = { { "all", no_argument, 0, 'a' },
					     { "ignore", required_argument, 0,
					       'i' },
					     { "help", no_argument, 0, 1 },
					     { "version", no_argument, 0, 2 },
					     { 0, 0, 0, 0 } };
	int c;
	int all = 0;
	long ignore = 0;
	long n;

	while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
		switch (c) {
		case 'a':
			all = 1;
			break;
		case 'i': {
			char *end = NULL;
			errno = 0;
			ignore = strtol(optarg, &end, 10);
			if (errno != 0 || end == optarg || *end != '\0' ||
			    ignore < 0) {
				fprintf(stderr,
					"nproc: invalid number: '%s'\n",
					optarg);
				return 1;
			}
			break;
		}
		case 1:
			printf("Usage: %s [OPTION]...\n"
			       "Print the number of processing units available.\n\n"
			       "      --all         print the number of installed processors\n"
			       "      --ignore=N    exclude up to N processing units\n"
			       "      --help        display this help and exit\n"
			       "      --version     output version information and exit\n",
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

	n = sysconf(all ? _SC_NPROCESSORS_CONF : _SC_NPROCESSORS_ONLN);
	if (n < 1)
		n = 1;

	/* Never print 0: callers size thread pools and -j values by this, and
	 * a zero would be worse than an underestimate. */
	if (ignore >= n)
		n = 1;
	else
		n -= ignore;

	printf("%ld\n", n);
	return 0;
}
