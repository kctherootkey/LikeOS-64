/*
 * adduser / addgroup - add or manipulate users and groups
 *
 * A friendly front end over the account databases.  The program acts as
 * "addgroup" when invoked under that name (or with --group / two-argument
 * group operations).  Five modes are supported:
 *
 *   adduser USER              add a normal user
 *   adduser --system USER     add a system user
 *   adduser --group GROUP     add a group (same as addgroup GROUP)
 *   addgroup --system GROUP   add a system group
 *   adduser USER GROUP        add an existing user to an existing group
 *
 * Accounts are created in /etc/passwd, /etc/shadow and /etc/group; a home
 * directory is created (and seeded from /etc/skel) unless suppressed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>

#define VERSION_STRING "adduser (LikeOS adduser) 0.2"

/* Debian-policy defaults (from /etc/adduser.conf). */
#define FIRST_UID        1000
#define LAST_UID         59999
#define FIRST_GID        1000
#define LAST_GID         59999
#define FIRST_SYS_UID    100
#define LAST_SYS_UID     999
#define FIRST_SYS_GID    100
#define LAST_SYS_GID     999
#define DEF_HOME         "/home"
#define DEF_SHELL        "/bin/bash"
#define NOLOGIN_SHELL    "/usr/sbin/nologin"
#define DEF_SKEL         "/etc/skel"
#define DEF_DIR_MODE     0755
#define USERS_GID        100

static int opt_verbose = 1;   /* info messages on by default */
static int opt_quiet;
static int is_addgroup;       /* invoked as addgroup */

static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char *fmt, ...)
{
	if (opt_quiet || !opt_verbose)
		return;
	va_list ap;
	__builtin_va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	__builtin_va_end(ap);
}

/* ---- name validation ---- */
static int valid_name(const char *n, int allow_bad)
{
	size_t len = strlen(n);
	if (len == 0 || len > 32)
		return 0;
	if (allow_bad)
		return 1; /* only the length/emptiness check */
	if (!(islower((unsigned char)n[0]) || n[0] == '_'))
		return 0;
	for (size_t i = 1; i < len; i++) {
		char c = n[i];
		if (i == len - 1 && c == '$')
			continue; /* trailing $ allowed (machine accounts) */
		if (!(islower((unsigned char)c) || isdigit((unsigned char)c) ||
		      c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

/* ---- id allocation ---- */
static int uid_used(uid_t u) { return getpwuid(u) != NULL; }
static int gid_used(gid_t g) { return getgrgid(g) != NULL; }

static long next_free_uid(long lo, long hi)
{
	for (long u = lo; u <= hi; u++)
		if (!uid_used((uid_t)u))
			return u;
	return -1;
}

static long next_free_gid(long lo, long hi)
{
	for (long g = lo; g <= hi; g++)
		if (!gid_used((gid_t)g))
			return g;
	return -1;
}

/* ---- file append helpers ---- */
static int append_line(const char *path, const char *line, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, mode);
	if (fd < 0) {
		fprintf(stderr, "adduser: cannot open %s: %s\n", path,
		        strerror(errno));
		return -1;
	}
	size_t len = strlen(line);
	if (write(fd, line, len) != (ssize_t)len) {
		fprintf(stderr, "adduser: write to %s failed: %s\n", path,
		        strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static long today_days(void)
{
	time_t t = time(NULL);
	return t > 0 ? (long)(t / 86400) : 0;
}

/* Add an existing user to an existing group's member list (rewrites /etc/group). */
static int add_member(const char *group, const char *user)
{
	FILE *in = fopen("/etc/group", "r");
	if (!in) {
		fprintf(stderr, "adduser: cannot open /etc/group: %s\n",
		        strerror(errno));
		return -1;
	}
	FILE *out = fopen("/etc/group+", "w");
	if (!out) {
		fprintf(stderr, "adduser: cannot create /etc/group+: %s\n",
		        strerror(errno));
		fclose(in);
		return -1;
	}
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	int found = 0, already = 0;
	while ((n = getline(&line, &cap, in)) >= 0) {
		if (n > 0 && line[n - 1] == '\n')
			line[--n] = '\0';
		/* name:passwd:gid:members */
		char namebuf[64];
		size_t i = 0;
		while (line[i] && line[i] != ':' && i < sizeof(namebuf) - 1)
			namebuf[i] = line[i], i++;
		namebuf[i] = '\0';
		if (strcmp(namebuf, group) == 0) {
			found = 1;
			char *members = strrchr(line, ':');
			members = members ? members + 1 : (char *)"";
			/* already a member? */
			char *dup = strdup(members), *save = dup, *tok;
			while ((tok = strsep(&save, ",")))
				if (*tok && strcmp(tok, user) == 0)
					already = 1;
			free(dup);
			if (already)
				fprintf(out, "%s\n", line);
			else if (members[0])
				fprintf(out, "%s,%s\n", line, user);
			else
				fprintf(out, "%s%s\n", line, user);
		} else {
			fprintf(out, "%s\n", line);
		}
	}
	free(line);
	fclose(in);
	fclose(out);
	if (!found) {
		unlink("/etc/group+");
		return 1;
	}
	chmod("/etc/group+", 0644);
	if (rename("/etc/group+", "/etc/group") != 0) {
		fprintf(stderr, "adduser: cannot replace /etc/group: %s\n",
		        strerror(errno));
		unlink("/etc/group+");
		return -1;
	}
	(void)already;
	return 0;
}

/* Copy the regular files in /etc/skel into a new home directory. */
static void seed_home(const char *home, uid_t uid, gid_t gid)
{
	DIR *d = opendir(DEF_SKEL);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char src[512], dst[512];
		snprintf(src, sizeof(src), "%s/%s", DEF_SKEL, de->d_name);
		snprintf(dst, sizeof(dst), "%s/%s", home, de->d_name);
		int in = open(src, O_RDONLY);
		if (in < 0)
			continue;
		int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			close(in);
			continue;
		}
		char buf[4096];
		ssize_t r;
		while ((r = read(in, buf, sizeof(buf))) > 0)
			write(out, buf, r);
		close(in);
		close(out);
		chown(dst, uid, gid);
	}
	closedir(d);
}

static int run_passwd(const char *user)
{
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "adduser: fork: %s\n", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		char *av[] = { (char *)"passwd", (char *)user, NULL };
		extern char **environ;
		execve("/bin/passwd", av, environ);
		_exit(127);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Prompt the interactive GECOS fields (Debian-style). */
static void prompt_gecos(char *out, size_t outsz)
{
	struct { const char *label; char val[128]; } f[] = {
		{ "Full Name", "" }, { "Room Number", "" },
		{ "Work Phone", "" }, { "Home Phone", "" }, { "Other", "" },
	};
	if (!isatty(0)) {
		out[0] = '\0';
		return;
	}
	printf("Enter the new value, or press ENTER for the default\n");
	for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
		printf("        %s []: ", f[i].label);
		fflush(stdout);
		if (fgets(f[i].val, sizeof(f[i].val), stdin)) {
			size_t l = strlen(f[i].val);
			if (l && f[i].val[l - 1] == '\n')
				f[i].val[l - 1] = '\0';
		}
	}
	snprintf(out, outsz, "%s,%s,%s,%s,%s", f[0].val, f[1].val, f[2].val,
	         f[3].val, f[4].val);
	/* Trim trailing commas for empty fields. */
	size_t e = strlen(out);
	while (e > 0 && out[e - 1] == ',')
		out[--e] = '\0';
}

static void usage(int status) __attribute__((noreturn));
static void usage(int status)
{
	FILE *o = status ? stderr : stdout;
	fprintf(o,
	    "Usage: adduser [options] USER          add a normal user\n"
	    "       adduser --system [options] USER add a system user\n"
	    "       adduser --group GROUP           add a group\n"
	    "       addgroup [options] GROUP        add a group\n"
	    "       adduser USER GROUP              add an existing user to a group\n\n"
	    "Options:\n"
	    "      --system               create a system account\n"
	    "      --group                create a group (or usergroup with --system)\n"
	    "      --uid ID               force the user id\n"
	    "      --gid ID               force the (primary) group id\n"
	    "      --ingroup GROUP        set the primary group by name\n"
	    "      --home DIR             set the home directory\n"
	    "      --shell SHELL          set the login shell\n"
	    "      --comment TEXT         set the GECOS/comment field\n"
	    "      --no-create-home       do not create the home directory\n"
	    "      --disabled-password    do not set a password\n"
	    "      --disabled-login       like --disabled-password and set nologin shell\n"
	    "      --add-extra-groups     add the user to the configured extra groups\n"
	    "      --firstuid ID  --lastuid ID   override the user id range\n"
	    "      --firstgid ID  --lastgid ID   override the group id range\n"
	    "      --allow-all-names / --allow-bad-names  relax name checking\n"
	    "      --quiet / --verbose / --debug\n"
	    "      --conf FILE            configuration file (accepted, unused)\n"
	    "      --help                 display this help\n"
	    "      --version              display version\n");
	exit(status);
}

/* ---- create a group ---- */
static int do_addgroup(const char *name, long forced_gid, int system,
                       long firstgid, long lastgid, int allow_bad)
{
	if (getgrnam(name)) {
		fprintf(stderr, "addgroup: group '%s' already exists.\n", name);
		return 1;
	}
	if (!valid_name(name, allow_bad)) {
		fprintf(stderr, "addgroup: '%s' is not a valid group name.\n",
		        name);
		return 1;
	}
	long gid = forced_gid;
	if (gid < 0) {
		if (system)
			gid = next_free_gid(FIRST_SYS_GID, LAST_SYS_GID);
		else
			gid = next_free_gid(firstgid, lastgid);
	} else if (gid_used((gid_t)gid)) {
		fprintf(stderr, "addgroup: gid %ld is already in use.\n", gid);
		return 1;
	}
	if (gid < 0) {
		fprintf(stderr, "addgroup: no group id available in range.\n");
		return 1;
	}
	char line[256];
	snprintf(line, sizeof(line), "%s:x:%ld:\n", name, gid);
	if (append_line("/etc/group", line, 0644) != 0)
		return 1;
	info("Adding group `%s' (GID %ld) ...\n", name, gid);
	info("Done.\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *prog = strrchr(argv[0], '/');
	prog = prog ? prog + 1 : argv[0];
	is_addgroup = (strcmp(prog, "addgroup") == 0);

	int system = 0, want_group = 0, no_create_home = 0;
	int disabled_pw = 0, disabled_login = 0, add_extra = 0;
	int allow_bad = 0;
	long forced_uid = -1, forced_gid = -1;
	long firstuid = FIRST_UID, lastuid = LAST_UID;
	long firstgid = FIRST_GID, lastgid = LAST_GID;
	const char *opt_home = NULL, *opt_shell = NULL, *opt_comment = NULL;
	const char *opt_ingroup = NULL;
	int c;

	enum {
		O_SYSTEM = 256, O_GROUP, O_UID, O_GID, O_INGROUP, O_HOME, O_SHELL,
		O_COMMENT, O_GECOS, O_NOHOME, O_DISPW, O_DISLOGIN, O_EXTRA,
		O_FUID, O_LUID, O_FGID, O_LGID, O_ALLOWALL, O_ALLOWBAD, O_CONF,
		O_DEBUG, O_QUIET, O_VERBOSE, O_HELP, O_VERSION, O_MSGLEVEL,
		O_ENCHOME
	};
	static struct option lo[] = {
		{ "system", no_argument, 0, O_SYSTEM },
		{ "group", no_argument, 0, O_GROUP },
		{ "uid", required_argument, 0, O_UID },
		{ "gid", required_argument, 0, O_GID },
		{ "ingroup", required_argument, 0, O_INGROUP },
		{ "home", required_argument, 0, O_HOME },
		{ "shell", required_argument, 0, O_SHELL },
		{ "comment", required_argument, 0, O_COMMENT },
		{ "gecos", required_argument, 0, O_GECOS },
		{ "no-create-home", no_argument, 0, O_NOHOME },
		{ "disabled-password", no_argument, 0, O_DISPW },
		{ "disabled-login", no_argument, 0, O_DISLOGIN },
		{ "add-extra-groups", no_argument, 0, O_EXTRA },
		{ "firstuid", required_argument, 0, O_FUID },
		{ "lastuid", required_argument, 0, O_LUID },
		{ "firstgid", required_argument, 0, O_FGID },
		{ "lastgid", required_argument, 0, O_LGID },
		{ "allow-all-names", no_argument, 0, O_ALLOWALL },
		{ "allow-bad-names", no_argument, 0, O_ALLOWBAD },
		{ "allow-badname", no_argument, 0, O_ALLOWBAD },
		{ "force-badname", no_argument, 0, O_ALLOWBAD },
		{ "conf", required_argument, 0, O_CONF },
		{ "debug", no_argument, 0, O_DEBUG },
		{ "quiet", no_argument, 0, O_QUIET },
		{ "verbose", no_argument, 0, O_VERBOSE },
		{ "encrypt-home", no_argument, 0, O_ENCHOME },
		{ "stdoutmsglevel", required_argument, 0, O_MSGLEVEL },
		{ "stderrmsglevel", required_argument, 0, O_MSGLEVEL },
		{ "logmsglevel", required_argument, 0, O_MSGLEVEL },
		{ "help", no_argument, 0, O_HELP },
		{ "version", no_argument, 0, O_VERSION },
		{ 0, 0, 0, 0 }
	};

	while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
		switch (c) {
		case O_SYSTEM: system = 1; break;
		case O_GROUP: want_group = 1; break;
		case O_UID: forced_uid = atol(optarg); break;
		case O_GID: forced_gid = atol(optarg); break;
		case O_INGROUP: opt_ingroup = optarg; break;
		case O_HOME: opt_home = optarg; break;
		case O_SHELL: opt_shell = optarg; break;
		case O_COMMENT:
		case O_GECOS: opt_comment = optarg; break;
		case O_NOHOME: no_create_home = 1; break;
		case O_DISPW: disabled_pw = 1; break;
		case O_DISLOGIN: disabled_login = 1; disabled_pw = 1; break;
		case O_EXTRA: add_extra = 1; break;
		case O_FUID: firstuid = atol(optarg); break;
		case O_LUID: lastuid = atol(optarg); break;
		case O_FGID: firstgid = atol(optarg); break;
		case O_LGID: lastgid = atol(optarg); break;
		case O_ALLOWALL:
		case O_ALLOWBAD: allow_bad = 1; break;
		case O_QUIET: opt_quiet = 1; break;
		case O_DEBUG:
		case O_VERBOSE: opt_verbose = 1; break;
		case O_CONF: case O_MSGLEVEL: case O_ENCHOME: break; /* accepted */
		case O_HELP: usage(0);
		case O_VERSION: printf("%s\n", VERSION_STRING); return 0;
		default: usage(1);
		}
	}

	if (geteuid() != 0) {
		fprintf(stderr, "%s: Only root may add users or groups.\n", prog);
		return 1;
	}

	int nargs = argc - optind;

	/* addgroup, or adduser --group with a single argument: group mode. */
	if ((is_addgroup || want_group) && nargs == 1 && !system) {
		return do_addgroup(argv[optind], forced_gid, 0, firstgid,
		                   lastgid, allow_bad);
	}
	if ((is_addgroup || want_group) && nargs == 1 && system) {
		return do_addgroup(argv[optind], forced_gid, 1, firstgid,
		                   lastgid, allow_bad);
	}

	/* adduser USER GROUP: add an existing user to an existing group. */
	if (nargs == 2) {
		const char *user = argv[optind], *group = argv[optind + 1];
		if (!getpwnam(user)) {
			fprintf(stderr, "adduser: user '%s' does not exist.\n",
			        user);
			return 1;
		}
		if (!getgrnam(group)) {
			fprintf(stderr, "adduser: group '%s' does not exist.\n",
			        group);
			return 1;
		}
		info("Adding user `%s' to group `%s' ...\n", user, group);
		int r = add_member(group, user);
		if (r != 0)
			return 1;
		info("Done.\n");
		return 0;
	}

	if (nargs != 1)
		usage(1);

	const char *name = argv[optind];

	/* ---- create a normal or system user ---- */
	if (getpwnam(name)) {
		fprintf(stderr, "adduser: user '%s' already exists.\n", name);
		return 1;
	}
	if (!valid_name(name, allow_bad)) {
		fprintf(stderr, "adduser: '%s' is not a valid user name.\n", name);
		return 1;
	}

	/* uid */
	long uid = forced_uid;
	if (uid < 0)
		uid = system ? next_free_uid(FIRST_SYS_UID, LAST_SYS_UID)
		             : next_free_uid(firstuid, lastuid);
	else if (uid_used((uid_t)uid)) {
		fprintf(stderr, "adduser: uid %ld is already in use.\n", uid);
		return 1;
	}
	if (uid < 0) {
		fprintf(stderr, "adduser: no user id available in range.\n");
		return 1;
	}

	/* primary group */
	long gid;
	int made_usergroup = 0;
	if (opt_ingroup) {
		struct group *g = getgrnam(opt_ingroup);
		if (!g) {
			fprintf(stderr, "adduser: group '%s' does not exist.\n",
			        opt_ingroup);
			return 1;
		}
		gid = g->gr_gid;
	} else if (forced_gid >= 0) {
		if (!gid_used((gid_t)forced_gid)) {
			fprintf(stderr, "adduser: gid %ld does not exist.\n",
			        forced_gid);
			return 1;
		}
		gid = forced_gid;
	} else if (!system || want_group) {
		/* usergroup: a new group named after the user, gid == uid if free */
		gid = (!gid_used((uid_t)uid)) ? uid
		    : next_free_gid(system ? FIRST_SYS_GID : firstgid,
		                    system ? LAST_SYS_GID : lastgid);
		if (gid < 0) {
			fprintf(stderr, "adduser: no group id available.\n");
			return 1;
		}
		char gline[256];
		snprintf(gline, sizeof(gline), "%s:x:%ld:\n", name, gid);
		if (append_line("/etc/group", gline, 0644) != 0)
			return 1;
		made_usergroup = 1;
	} else {
		/* system user without --group -> the shared "users" group */
		struct group *g = getgrnam("nogroup");
		gid = g ? g->gr_gid : USERS_GID;
	}

	/* home + shell */
	char homebuf[256];
	const char *home = opt_home;
	if (!home) {
		if (system)
			home = "/nonexistent";
		else {
			snprintf(homebuf, sizeof(homebuf), "%s/%s", DEF_HOME, name);
			home = homebuf;
		}
	}
	const char *shell = opt_shell;
	if (!shell)
		shell = (disabled_login || system) && !opt_shell
		            ? (system ? NOLOGIN_SHELL : NOLOGIN_SHELL)
		            : DEF_SHELL;
	if (!opt_shell && !disabled_login && !system)
		shell = DEF_SHELL;

	/* gecos */
	char gecos[256] = "";
	if (opt_comment)
		snprintf(gecos, sizeof(gecos), "%s", opt_comment);
	else if (!system)
		prompt_gecos(gecos, sizeof(gecos));

	info("Adding user `%s' ...\n", name);
	if (made_usergroup)
		info("Adding new group `%s' (%ld) ...\n", name, gid);
	info("Adding new user `%s' (%ld) with group `%ld' ...\n", name, uid,
	     gid);

	/* /etc/passwd entry */
	char pline[512];
	snprintf(pline, sizeof(pline), "%s:x:%ld:%ld:%s:%s:%s\n", name, uid, gid,
	         gecos, home, shell);
	if (append_line("/etc/passwd", pline, 0644) != 0)
		return 1;

	/* /etc/shadow entry (password locked until passwd sets it) */
	char sline[256];
	snprintf(sline, sizeof(sline), "%s:!:%ld:0:99999:7:::\n", name,
	         today_days());
	if (append_line("/etc/shadow", sline, 0600) != 0)
		return 1;
	chmod("/etc/shadow", 0600);

	/* home directory */
	if (!no_create_home && strcmp(home, "/nonexistent") != 0) {
		if (mkdir(home, DEF_DIR_MODE) != 0 && errno != EEXIST)
			fprintf(stderr, "adduser: cannot create %s: %s\n", home,
			        strerror(errno));
		else {
			info("Creating home directory `%s' ...\n", home);
			seed_home(home, (uid_t)uid, (gid_t)gid);
			if (chown(home, (uid_t)uid, (gid_t)gid) != 0)
				fprintf(stderr, "adduser: chown %s: %s\n", home,
				        strerror(errno));
		}
	}

	/* extra groups */
	if (add_extra) {
		const char *extras[] = { "users", NULL };
		for (int i = 0; extras[i]; i++)
			if (getgrnam(extras[i]))
				add_member(extras[i], name);
	}

	/* Set the password interactively for a normal, non-disabled account. */
	if (!system && !disabled_pw) {
		if (run_passwd(name) != 0)
			fprintf(stderr,
			    "adduser: warning: passwd did not complete for %s\n",
			    name);
	}
	info("Done.\n");
	return 0;
}
