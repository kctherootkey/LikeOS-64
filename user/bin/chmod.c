/*
 * chmod - change file mode bits
 *
 * Full implementation per the chmod(1) manual page: octal modes
 * (1-4 digits, optionally prefixed with +/-/=) and symbolic modes
 * ([ugoa]*([-+=]([rwxXst]*|[ugo]))+, comma-separated), plus the options
 * -c/--changes, -f/--silent/--quiet, -v/--verbose, -R/--recursive,
 * --reference=RFILE, --preserve-root/--no-preserve-root, --help, --version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "chmod"

/* permission / special bits (avoid relying on every macro existing) */
#ifndef S_ISUID
#define S_ISUID 04000
#endif
#ifndef S_ISGID
#define S_ISGID 02000
#endif
#ifndef S_ISVTX
#define S_ISVTX 01000
#endif

static int opt_changes = 0; /* -c: report only changes              */
static int opt_silent = 0; /* -f: suppress most error messages     */
static int opt_verbose = 0; /* -v: report every file                */
static int opt_recursive = 0; /* -R                                   */
static int opt_preserve_root = 0;
static const char *ref_file = NULL; /* --reference=RFILE               */

static int exit_status = 0;

/* ---- symbolic mode representation ---- */
/* A parsed mode is a sequence of clauses applied left to right. */
typedef struct {
	int is_octal;
	/* octal form */
	char octal_op; /* '=', '+', '-' (0 means plain set)        */
	unsigned octal_val;
	/* symbolic form: store the original string, parsed on apply         */
	const char *sym;
} mode_spec_t;

static void usage(void)
{
	printf("Usage: " PROGRAM_NAME " [OPTION]... MODE[,MODE]... FILE...\n"
	       "  or:  " PROGRAM_NAME " [OPTION]... OCTAL-MODE FILE...\n"
	       "  or:  " PROGRAM_NAME " [OPTION]... --reference=RFILE FILE...\n"
	       "Change the mode of each FILE to MODE.\n"
	       "With --reference, change the mode of each FILE to that of RFILE.\n"
	       "\n"
	       "  -c, --changes          like verbose but report only when a change is made\n"
	       "  -f, --silent, --quiet  suppress most error messages\n"
	       "  -v, --verbose          output a diagnostic for every file processed\n"
	       "      --no-preserve-root  do not treat '/' specially (the default)\n"
	       "      --preserve-root    fail to operate recursively on '/'\n"
	       "      --reference=RFILE  use RFILE's mode instead of MODE values\n"
	       "  -R, --recursive        change files and directories recursively\n"
	       "      --help     display this help and exit\n"
	       "      --version  output version information and exit\n"
	       "\n"
	       "Each MODE is of the form '[ugoa]*([-+=]([rwxXst]*|[ugo]))+|[-+=][0-7]+'.\n");
}

static void version(void)
{
	printf(PROGRAM_NAME " (LikeOS coreutils) " VERSION "\n");
}

/* Parse the MODE argument.  Returns 0 on success. */
static int parse_mode(const char *s, mode_spec_t *out)
{
	out->sym = s;
	/* Detect octal: optional [-+=] then only octal digits. */
	const char *p = s;
	char op = 0;
	if (*p == '+' || *p == '-' || *p == '=') {
		op = *p;
		p++;
	}
	if (*p) {
		const char *q = p;
		int all_octal = 1;
		for (; *q; q++)
			if (*q < '0' || *q > '7') {
				all_octal = 0;
				break;
			}
		if (all_octal && q != p) {
			out->is_octal = 1;
			out->octal_op = op;
			out->octal_val = (unsigned)strtoul(p, NULL, 8) & 07777u;
			return 0;
		}
	}
	/* Otherwise it must be symbolic; validate roughly. */
	if (op == 0) {
		/* symbolic must contain an operator somewhere */
		int has_op = 0;
		for (const char *r = s; *r; r++)
			if (*r == '+' || *r == '-' || *r == '=') {
				has_op = 1;
				break;
			}
		if (!has_op)
			return -1;
	}
	out->is_octal = 0;
	return 0;
}

/* Apply one symbolic clause string to `cur` (12 low bits) for a file whose
 * directory-ness is `is_dir`.  Returns the new 12-bit mode, or sets *err. */
static unsigned apply_symbolic(unsigned cur, int is_dir, const char *spec,
			       int *err)
{
	unsigned mode = cur & 07777u;
	const char *p = spec;
	while (*p) {
		/* who set */
		unsigned who = 0; /* bit0=o, bit1=g, bit2=u (category mask)  */
		int who_given = 0;
		while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
			who_given = 1;
			if (*p == 'u')
				who |= 4;
			else if (*p == 'g')
				who |= 2;
			else if (*p == 'o')
				who |= 1;
			else
				who |= 7;
			p++;
		}
		if (!who_given)
			who = 7; /* default 'a' (umask applied below)   */

		if (*p != '+' && *p != '-' && *p != '=') {
			*err = 1;
			return cur;
		}

		/* one or more (op perms) groups */
		while (*p == '+' || *p == '-' || *p == '=') {
			char op = *p++;
			unsigned permbits =
				0; /* rwx pattern 0..7                 */
			unsigned special =
				0; /* setuid/setgid/sticky             */
			int copy_cat =
				-1; /* u/g/o copy                       */

			if (*p == 'u' || *p == 'g' || *p == 'o') {
				copy_cat = (*p == 'u') ? 2 :
					   (*p == 'g') ? 1 :
							 0;
				p++;
			} else {
				while (*p == 'r' || *p == 'w' || *p == 'x' ||
				       *p == 'X' || *p == 's' || *p == 't') {
					switch (*p) {
					case 'r':
						permbits |= 4;
						break;
					case 'w':
						permbits |= 2;
						break;
					case 'x':
						permbits |= 1;
						break;
					case 'X':
						if (is_dir || (mode & 0111))
							permbits |= 1;
						break;
					case 's':
						if (who & 4)
							special |= S_ISUID;
						if (who & 2)
							special |= S_ISGID;
						break;
					case 't':
						special |= S_ISVTX;
						break;
					}
					p++;
				}
			}

			/* If copying a category, derive permbits from current mode. */
			if (copy_cat >= 0) {
				unsigned src = (mode >> (copy_cat * 3)) & 7u;
				permbits = src;
			}

			/* Build the value across the affected categories. */
			unsigned val = 0;
			if (who & 4)
				val |= permbits << 6;
			if (who & 2)
				val |= permbits << 3;
			if (who & 1)
				val |= permbits << 0;
			val |= special;

			unsigned effective_who = who;
			if (!who_given) {
				/* "a" implied: bits set in umask are not affected. */
				mode_t um = umask(0);
				umask(um);
				val &= ~((unsigned)um);
			}

			unsigned whomask = 0;
			if (effective_who & 4)
				whomask |= 0700;
			if (effective_who & 2)
				whomask |= 0070;
			if (effective_who & 1)
				whomask |= 0007;
			if (effective_who & 4)
				whomask |= S_ISUID;
			if (effective_who & 2)
				whomask |= S_ISGID;
			whomask |=
				S_ISVTX; /* 't' belongs to 'o'/all conceptually  */

			if (op == '+')
				mode |= val;
			else if (op == '-')
				mode &= ~val;
			else { /* '=' */
				mode &= ~whomask;
				mode |= val;
			}
		}

		if (*p == ',')
			p++;
		else if (*p) {
			*err = 1;
			return cur;
		}
	}
	return mode & 07777u;
}

/* Compute the new 12-bit mode for a file given the parsed spec. */
static unsigned compute_mode(const mode_spec_t *spec, unsigned cur, int is_dir,
			     int *err)
{
	*err = 0;
	if (spec->is_octal) {
		if (spec->octal_op == '+')
			return (cur | spec->octal_val) & 07777u;
		else if (spec->octal_op == '-')
			return (cur & ~spec->octal_val) & 07777u;
		else
			return spec->octal_val & 07777u; /* '=' or plain */
	}
	return apply_symbolic(cur, is_dir, spec->sym, err);
}

static void report_change(const char *path, unsigned oldm, unsigned newm)
{
	if (opt_verbose || (opt_changes && oldm != newm)) {
		printf("mode of '%s' changed from %04o to %04o\n", path, oldm,
		       newm);
	}
}

/* Recursively apply to `path`.  is_cmdline: a top-level argument (follow a
 * symlink to its target); during recursion symlinks are ignored. */
static int do_path(const char *path, const mode_spec_t *spec)
{
	struct stat st;
	if (lstat(path, &st) != 0) {
		if (!opt_silent)
			fprintf(stderr,
				PROGRAM_NAME ": cannot access '%s': %s\n", path,
				strerror(errno));
		exit_status = 1;
		return -1;
	}

	/* Recurse first into directory contents (skip symlinks). */
	if (opt_recursive && S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d)) != NULL) {
				if (strcmp(e->d_name, ".") == 0 ||
				    strcmp(e->d_name, "..") == 0)
					continue;
				char child[4096];
				size_t pl = strlen(path);
				int need_slash =
					(pl > 0 && path[pl - 1] != '/');
				if (pl + strlen(e->d_name) + 2 >= sizeof(child))
					continue;
				snprintf(child, sizeof(child), "%s%s%s", path,
					 need_slash ? "/" : "", e->d_name);
				struct stat cst;
				if (lstat(child, &cst) == 0 &&
				    S_ISLNK(cst.st_mode))
					continue; /* ignore symlinks while recursing */
				do_path(child, spec);
			}
			closedir(d);
		} else if (!opt_silent) {
			fprintf(stderr,
				PROGRAM_NAME
				": cannot read directory '%s': %s\n",
				path, strerror(errno));
			exit_status = 1;
		}
	}

	/* chmod() follows symlinks, which is correct for a command-line symlink
     * (changes the target). */
	unsigned cur = st.st_mode & 07777u;
	int dir = S_ISDIR(st.st_mode);
	int err = 0;
	unsigned newm = compute_mode(spec, cur, dir, &err);
	if (err) {
		fprintf(stderr, PROGRAM_NAME ": invalid mode\n");
		exit(1);
	}
	if (newm != cur) {
		if (chmod(path, (mode_t)newm) != 0) {
			if (!opt_silent)
				fprintf(stderr,
					PROGRAM_NAME
					": changing permissions of '%s': %s\n",
					path, strerror(errno));
			exit_status = 1;
			return -1;
		}
	}
	report_change(path, cur, newm);
	return 0;
}

int main(int argc, char **argv)
{
	static struct option long_opts[] = {
		{ "changes", no_argument, 0, 'c' },
		{ "silent", no_argument, 0, 'f' },
		{ "quiet", no_argument, 0, 'f' },
		{ "verbose", no_argument, 0, 'v' },
		{ "recursive", no_argument, 0, 'R' },
		{ "reference", required_argument, 0, 1 },
		{ "no-preserve-root", no_argument, 0, 2 },
		{ "preserve-root", no_argument, 0, 3 },
		{ "help", no_argument, 0, 4 },
		{ "version", no_argument, 0, 5 },
		{ 0, 0, 0, 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "cfvR", long_opts, NULL)) != -1) {
		switch (c) {
		case 'c':
			opt_changes = 1;
			break;
		case 'f':
			opt_silent = 1;
			break;
		case 'v':
			opt_verbose = 1;
			break;
		case 'R':
			opt_recursive = 1;
			break;
		case 1:
			ref_file = optarg;
			break;
		case 2:
			opt_preserve_root = 0;
			break;
		case 3:
			opt_preserve_root = 1;
			break;
		case 4:
			usage();
			return 0;
		case 5:
			version();
			return 0;
		default:
			fprintf(stderr, "Try '" PROGRAM_NAME
					" --help' for more information.\n");
			return 1;
		}
	}

	mode_spec_t spec;
	int first_file = optind;

	if (ref_file) {
		struct stat rst;
		if (stat(ref_file, &rst) != 0) {
			fprintf(stderr,
				PROGRAM_NAME
				": failed to get attributes of '%s': %s\n",
				ref_file, strerror(errno));
			return 1;
		}
		spec.is_octal = 1;
		spec.octal_op = '=';
		spec.octal_val = rst.st_mode & 07777u;
	} else {
		if (optind >= argc) {
			fprintf(stderr, PROGRAM_NAME ": missing operand\n");
			fprintf(stderr, "Try '" PROGRAM_NAME
					" --help' for more information.\n");
			return 1;
		}
		if (parse_mode(argv[optind], &spec) != 0) {
			fprintf(stderr, PROGRAM_NAME ": invalid mode: '%s'\n",
				argv[optind]);
			return 1;
		}
		first_file = optind + 1;
	}

	if (first_file >= argc) {
		fprintf(stderr, PROGRAM_NAME ": missing operand after '%s'\n",
			argv[argc - 1]);
		return 1;
	}

	for (int i = first_file; i < argc; i++) {
		if (opt_recursive && opt_preserve_root &&
		    strcmp(argv[i], "/") == 0) {
			fprintf(stderr, PROGRAM_NAME
				": it is dangerous to operate recursively on '/'\n");
			fprintf(stderr, PROGRAM_NAME
				": use --no-preserve-root to override this failsafe\n");
			exit_status = 1;
			continue;
		}
		do_path(argv[i], &spec);
	}
	return exit_status;
}
