/*
 * id - print real and effective user and group IDs
 *
 * Usage: id [OPTION]... [USER]...
 * Print user and group information for each specified USER, or (when USER is
 * omitted) for the current process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>

#define VERSION_STRING "id (LikeOS coreutils) 0.2"

static const char *progname = "id";

/* Selected "print only" mode. */
enum only { ONLY_NONE, ONLY_USER, ONLY_GROUP, ONLY_GROUPS };

static int opt_name;   /* -n: names instead of numbers */
static int opt_real;   /* -r: real instead of effective */
static int opt_zero;   /* -z: NUL-delimit */
static char delim = '\n';

static void usage(int status) __attribute__((noreturn));
static void usage(int status)
{
	FILE *out = status == 0 ? stdout : stderr;
	fprintf(out, "Usage: %s [OPTION]... [USER]...\n", progname);
	fprintf(out,
	    "Print user and group information for each specified USER,\n"
	    "or (when USER omitted) for the current process.\n\n"
	    "  -a             ignore, for compatibility with other versions\n"
	    "  -Z, --context  print only the security context of the process\n"
	    "  -g, --group    print only the effective group ID\n"
	    "  -G, --groups   print all group IDs\n"
	    "  -n, --name     print a name instead of a number, for -ugG\n"
	    "  -r, --real     print the real ID instead of the effective ID, with -ugG\n"
	    "  -u, --user     print only the effective user ID\n"
	    "  -z, --zero     delimit entries with NUL characters, not whitespace;\n"
	    "                 not permitted in default format\n"
	    "      --help     display this help and exit\n"
	    "      --version  output version information and exit\n");
	exit(status);
}

/* In "print only" modes (-g/-G) print just the number, or just the name with
 * -n.  The "gid(name)" form is used only by the default full format. */
static void print_group(gid_t g, int names)
{
	if (names) {
		struct group *gr = getgrgid(g);
		if (gr)
			printf("%s", gr->gr_name);
		else
			printf("%u", (unsigned)g);
	} else {
		printf("%u", (unsigned)g);
	}
}

static void print_user(uid_t u, int names)
{
	if (names) {
		struct passwd *pw = getpwuid(u);
		if (pw)
			printf("%s", pw->pw_name);
		else
			printf("%u", (unsigned)u);
	} else {
		printf("%u", (unsigned)u);
	}
}

/* Build the group list for a named user from /etc/group + the primary gid. */
static int user_group_list(const char *name, gid_t primary, gid_t *groups,
                           int max)
{
	struct group *gr;
	int n = 0, i;

	if (n < max)
		groups[n++] = primary;
	setgrent();
	while ((gr = getgrent()) != NULL) {
		if (gr->gr_gid == primary)
			continue;
		for (char **m = gr->gr_mem; m && *m; m++) {
			if (strcmp(*m, name) == 0) {
				int dup = 0;
				for (i = 0; i < n; i++)
					if (groups[i] == gr->gr_gid)
						dup = 1;
				if (!dup && n < max)
					groups[n++] = gr->gr_gid;
				break;
			}
		}
	}
	endgrent();
	return n;
}

/* Print info for one identity. Returns 0 on success. */
static int print_identity(const char *user, enum only mode)
{
	uid_t ruid, euid;
	gid_t rgid, egid;
	gid_t groups[NGROUPS_MAX + 1];
	int ngroups = 0, i;
	const char *name = user;

	if (user) {
		struct passwd *pw = getpwnam(user);
		if (!pw) {
			/* Also accept a numeric uid. */
			char *end;
			long v = strtol(user, &end, 10);
			if (*end == '\0' && v >= 0)
				pw = getpwuid((uid_t)v);
			if (!pw) {
				fprintf(stderr, "%s: '%s': no such user\n",
				        progname, user);
				return 1;
			}
		}
		ruid = euid = pw->pw_uid;
		rgid = egid = pw->pw_gid;
		name = pw->pw_name;
		ngroups = user_group_list(name, pw->pw_gid, groups,
		                          NGROUPS_MAX + 1);
	} else {
		ruid = getuid();
		euid = geteuid();
		rgid = getgid();
		egid = getegid();
		int n = getgroups(NGROUPS_MAX, (int *)groups);
		if (n < 0)
			n = 0;
		ngroups = n;
		/* Ensure the effective gid appears in the list. */
		int have = 0;
		for (i = 0; i < ngroups; i++)
			if (groups[i] == egid)
				have = 1;
		if (!have && ngroups < NGROUPS_MAX)
			groups[ngroups++] = egid;
	}

	switch (mode) {
	case ONLY_USER:
		print_user(opt_real ? ruid : euid, opt_name);
		putchar(delim);
		break;
	case ONLY_GROUP:
		print_group(opt_real ? rgid : egid, opt_name);
		putchar(delim);
		break;
	case ONLY_GROUPS:
		for (i = 0; i < ngroups; i++) {
			if (i)
				putchar(opt_zero ? '\0' : ' ');
			print_group(groups[i], opt_name);
		}
		putchar(delim);
		break;
	default: /* full format */
		printf("uid=%u(", (unsigned)ruid);
		print_user(ruid, 1);
		printf(")");
		printf(" gid=%u(", (unsigned)rgid);
		print_group(rgid, 1);
		printf(")");
		if (!user && euid != ruid) {
			printf(" euid=%u(", (unsigned)euid);
			print_user(euid, 1);
			printf(")");
		}
		if (!user && egid != rgid) {
			printf(" egid=%u(", (unsigned)egid);
			print_group(egid, 1);
			printf(")");
		}
		printf(" groups=");
		for (i = 0; i < ngroups; i++) {
			if (i)
				putchar(',');
			printf("%u(", (unsigned)groups[i]);
			print_group(groups[i], 1);
			printf(")");
		}
		putchar(delim);
		break;
	}
	(void)name;
	return 0;
}

int main(int argc, char **argv)
{
	enum only mode = ONLY_NONE;
	int only_count = 0;
	int opt_context = 0;
	int c;

	static struct option long_opts[] = {
		{ "context", no_argument, 0, 'Z' },
		{ "group", no_argument, 0, 'g' },
		{ "groups", no_argument, 0, 'G' },
		{ "name", no_argument, 0, 'n' },
		{ "real", no_argument, 0, 'r' },
		{ "user", no_argument, 0, 'u' },
		{ "zero", no_argument, 0, 'z' },
		{ "help", no_argument, 0, 1 },
		{ "version", no_argument, 0, 2 },
		{ 0, 0, 0, 0 }
	};

	while ((c = getopt_long(argc, argv, "agGnruzZ", long_opts, NULL)) != -1) {
		switch (c) {
		case 'a':
			break; /* ignored, for compatibility */
		case 'g':
			mode = ONLY_GROUP;
			only_count++;
			break;
		case 'G':
			mode = ONLY_GROUPS;
			only_count++;
			break;
		case 'u':
			mode = ONLY_USER;
			only_count++;
			break;
		case 'n':
			opt_name = 1;
			break;
		case 'r':
			opt_real = 1;
			break;
		case 'z':
			opt_zero = 1;
			break;
		case 'Z':
			opt_context = 1;
			break;
		case 1:
			usage(0);
		case 2:
			printf("%s\n", VERSION_STRING);
			return 0;
		default:
			usage(1);
		}
	}

	if (opt_context) {
		fprintf(stderr,
		    "%s: --context (-Z) works only on a security-enabled kernel\n",
		    progname);
		return 1;
	}
	if (only_count > 1) {
		fprintf(stderr,
		    "%s: cannot print \"only\" of more than one choice\n",
		    progname);
		return 1;
	}
	if ((opt_name || opt_real) && mode == ONLY_NONE) {
		fprintf(stderr,
		    "%s: cannot print only names or real IDs in default format\n",
		    progname);
		return 1;
	}
	if (opt_zero && mode == ONLY_NONE) {
		fprintf(stderr,
		    "%s: option --zero not permitted in default format\n",
		    progname);
		return 1;
	}
	if (opt_zero)
		delim = '\0';

	int rc = 0;
	if (optind >= argc) {
		rc = print_identity(NULL, mode);
	} else {
		for (int i = optind; i < argc; i++)
			if (print_identity(argv[i], mode) != 0)
				rc = 1;
	}
	return rc;
}
