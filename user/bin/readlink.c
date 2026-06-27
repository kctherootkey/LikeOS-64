/*
 * readlink - print resolved symbolic links or canonical file names
 *
 * Full implementation per the readlink(1) manual page.  Default prints the
 * target of a symbolic link; -f/-e/-m canonicalize by following every
 * symlink in every component (differing in existence requirements).  Also
 * supports -n/--no-newline, -z/--zero, -q/-s (quiet, default) and
 * -v/--verbose, plus --help/--version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "readlink"

static int opt_canon = 0; /* 0 none, 'f', 'e', 'm'                  */
static int opt_no_newline = 0;
static int opt_zero = 0;
static int opt_verbose = 0; /* report errors (default: silent)        */

static void usage(void)
{
	printf("Usage: " PROGRAM_NAME " [OPTION]... FILE...\n"
	       "Print value of a symbolic link or canonical file name\n"
	       "\n"
	       "  -f, --canonicalize            canonicalize by following every symlink in\n"
	       "                                  every component; all but the last must exist\n"
	       "  -e, --canonicalize-existing   all components must exist\n"
	       "  -m, --canonicalize-missing    no requirements on components' existence\n"
	       "  -n, --no-newline              do not output the trailing delimiter\n"
	       "  -q, --quiet\n"
	       "  -s, --silent                  suppress most error messages (default)\n"
	       "  -v, --verbose                 report error messages\n"
	       "  -z, --zero                    end each output line with NUL, not newline\n"
	       "      --help     display this help and exit\n"
	       "      --version  output version information and exit\n");
}

static void version(void)
{
	printf(PROGRAM_NAME " (LikeOS coreutils) " VERSION "\n");
}

/* Canonicalize `name` into `resolved` per mode ('f','e','m'). 0 on success. */
static int canonicalize(const char *name, int mode, char *resolved, size_t cap)
{
	char rpath[4096];
	char rest[8192];
	int links = 0;

	if (name[0] == '/') {
		rpath[0] = '/';
		rpath[1] = '\0';
		snprintf(rest, sizeof(rest), "%s", name + 1);
	} else {
		if (!getcwd(rpath, sizeof(rpath)))
			return -1;
		snprintf(rest, sizeof(rest), "%s", name);
	}

	while (rest[0]) {
		char comp[1024];
		int ci = 0;
		char *s = rest;
		while (*s == '/')
			s++;
		while (*s && *s != '/') {
			if (ci < 1023)
				comp[ci++] = *s;
			s++;
		}
		comp[ci] = '\0';
		char remaining[8192];
		snprintf(remaining, sizeof(remaining), "%s", s);
		int is_last = (remaining[0] == '\0');

		if (ci == 0) {
			snprintf(rest, sizeof(rest), "%s", remaining);
			continue;
		}
		if (strcmp(comp, ".") == 0) {
			snprintf(rest, sizeof(rest), "%s", remaining);
			continue;
		}
		if (strcmp(comp, "..") == 0) {
			char *sl = strrchr(rpath, '/');
			if (sl && sl != rpath)
				*sl = '\0';
			else {
				rpath[0] = '/';
				rpath[1] = '\0';
			}
			snprintf(rest, sizeof(rest), "%s", remaining);
			continue;
		}

		size_t rl = strlen(rpath);
		if (rl + 1 + (size_t)ci + 1 >= sizeof(rpath)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		if (rl == 0 || rpath[rl - 1] != '/')
			rpath[rl++] = '/';
		memcpy(rpath + rl, comp, (size_t)ci + 1);

		struct stat st;
		if (lstat(rpath, &st) != 0) {
			if (errno == ENOENT &&
			    (mode == 'm' || (mode == 'f' && is_last))) {
				if (remaining[0]) {
					size_t pl = strlen(rpath);
					snprintf(rpath + pl, sizeof(rpath) - pl,
						 "%s", remaining);
				}
				rest[0] = '\0';
				break;
			}
			return -1;
		}
		if (S_ISLNK(st.st_mode)) {
			if (++links > 40) {
				errno = ELOOP;
				return -1;
			}
			char target[4096];
			int tl = (int)readlink(rpath, target,
					       sizeof(target) - 1);
			if (tl < 0)
				return -1;
			target[tl] = '\0';
			char *sl = strrchr(rpath, '/');
			if (sl && sl != rpath)
				*sl = '\0';
			else {
				rpath[0] = '/';
				rpath[1] = '\0';
			}
			char newrest[8192];
			if (target[0] == '/') {
				rpath[0] = '/';
				rpath[1] = '\0';
				snprintf(newrest, sizeof(newrest), "%s%s",
					 target + 1, remaining);
			} else {
				snprintf(newrest, sizeof(newrest), "%s%s",
					 target, remaining);
			}
			snprintf(rest, sizeof(rest), "%s", newrest);
			continue;
		}
		snprintf(rest, sizeof(rest), "%s", remaining);
	}
	snprintf(resolved, cap, "%s", rpath);
	return 0;
}

static int handle(const char *file)
{
	char out[4096];
	if (opt_canon) {
		if (canonicalize(file, opt_canon, out, sizeof(out)) != 0) {
			if (opt_verbose)
				fprintf(stderr, PROGRAM_NAME ": %s: %s\n", file,
					strerror(errno));
			return 1;
		}
	} else {
		int n = (int)readlink(file, out, sizeof(out) - 1);
		if (n < 0) {
			if (opt_verbose)
				fprintf(stderr, PROGRAM_NAME ": %s: %s\n", file,
					strerror(errno));
			return 1;
		}
		out[n] = '\0';
	}
	fputs(out, stdout);
	if (!opt_no_newline)
		fputc(opt_zero ? '\0' : '\n', stdout);
	return 0;
}

int main(int argc, char **argv)
{
	static struct option long_opts[] = {
		{ "canonicalize", no_argument, 0, 'f' },
		{ "canonicalize-existing", no_argument, 0, 'e' },
		{ "canonicalize-missing", no_argument, 0, 'm' },
		{ "no-newline", no_argument, 0, 'n' },
		{ "quiet", no_argument, 0, 'q' },
		{ "silent", no_argument, 0, 's' },
		{ "verbose", no_argument, 0, 'v' },
		{ "zero", no_argument, 0, 'z' },
		{ "help", no_argument, 0, 1 },
		{ "version", no_argument, 0, 2 },
		{ 0, 0, 0, 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "femnqsvz", long_opts, NULL)) !=
	       -1) {
		switch (c) {
		case 'f':
			opt_canon = 'f';
			break;
		case 'e':
			opt_canon = 'e';
			break;
		case 'm':
			opt_canon = 'm';
			break;
		case 'n':
			opt_no_newline = 1;
			break;
		case 'q':
		case 's':
			opt_verbose = 0;
			break;
		case 'v':
			opt_verbose = 1;
			break;
		case 'z':
			opt_zero = 1;
			break;
		case 1:
			usage();
			return 0;
		case 2:
			version();
			return 0;
		default:
			fprintf(stderr, "Try '" PROGRAM_NAME
					" --help' for more information.\n");
			return 1;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, PROGRAM_NAME ": missing operand\n");
		fprintf(stderr, "Try '" PROGRAM_NAME
				" --help' for more information.\n");
		return 1;
	}
	int status = 0;
	for (int i = optind; i < argc; i++)
		if (handle(argv[i]) != 0)
			status = 1;
	return status;
}
