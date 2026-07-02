/*
 * groups - print the groups a user is in
 *
 * Usage: groups [OPTION]... [USERNAME]...
 * Print group memberships for each USERNAME or, if none is specified, for the
 * current process.
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

#define VERSION_STRING "groups (LikeOS coreutils) 0.2"

static void print_group_name(gid_t g)
{
	struct group *gr = getgrgid(g);
	if (gr)
		printf("%s", gr->gr_name);
	else
		printf("%u", (unsigned)g);
}

/* Group list for a named user: primary group plus /etc/group memberships. */
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

static int groups_for_user(const char *user)
{
	gid_t groups[NGROUPS_MAX + 1];
	int n, i;

	struct passwd *pw = getpwnam(user);
	if (!pw) {
		fprintf(stderr, "groups: '%s': no such user\n", user);
		return 1;
	}
	n = user_group_list(pw->pw_name, pw->pw_gid, groups, NGROUPS_MAX + 1);
	printf("%s : ", pw->pw_name);
	for (i = 0; i < n; i++) {
		if (i)
			putchar(' ');
		print_group_name(groups[i]);
	}
	putchar('\n');
	return 0;
}

static void groups_for_self(void)
{
	gid_t groups[NGROUPS_MAX + 1];
	int n, i;
	gid_t egid = getegid();

	n = getgroups(NGROUPS_MAX, (int *)groups);
	if (n < 0)
		n = 0;
	int have = 0;
	for (i = 0; i < n; i++)
		if (groups[i] == egid)
			have = 1;
	if (!have && n < NGROUPS_MAX)
		groups[n++] = egid;

	for (i = 0; i < n; i++) {
		if (i)
			putchar(' ');
		print_group_name(groups[i]);
	}
	putchar('\n');
}

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
			printf("Usage: %s [OPTION]... [USERNAME]...\n"
			       "Print group memberships for each USERNAME or, if"
			       " none is specified,\nfor the current process.\n\n"
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

	if (optind >= argc) {
		groups_for_self();
		return 0;
	}

	int rc = 0;
	for (int i = optind; i < argc; i++)
		if (groups_for_user(argv[i]) != 0)
			rc = 1;
	return rc;
}
