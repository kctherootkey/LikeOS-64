/*
 * deluser / delgroup - remove users and groups from the system
 *
 * The counterpart to adduser(8): it acts as "delgroup" when invoked under that
 * name (or with --group / a two-argument group operation).  Five modes:
 *
 *   deluser USER             remove a normal user
 *   deluser --system USER    remove a user only if it is a system user
 *   deluser --group GROUP    remove a group (same as delgroup GROUP)
 *   delgroup --system GROUP  remove a system group
 *   deluser USER GROUP       remove a user from a group
 *
 * Only the superuser may remove accounts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>

#define VERSION_STRING "deluser (LikeOS adduser) 0.2"

#define FIRST_SYS_UID 100
#define LAST_SYS_UID  999
#define FIRST_SYS_GID 100
#define LAST_SYS_GID  999
#define MAILSPOOL_DIR "/var/mail"

static int opt_verbose = 1;
static int opt_quiet;
static int is_delgroup;

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

static int is_sys_uid(uid_t u) { return u >= FIRST_SYS_UID && u <= LAST_SYS_UID; }
static int is_sys_gid(gid_t g) { return g >= FIRST_SYS_GID && g <= LAST_SYS_GID; }

/* Rewrite a colon-separated database, dropping the line whose first field is
 * `name`.  Returns 0 (removed), 1 (not found) or -1 (error). */
static int db_drop(const char *path, const char *name, mode_t mode)
{
	FILE *in = fopen(path, "r");
	if (!in)
		return -1;
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s+", path);
	int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	FILE *out = ofd >= 0 ? fdopen(ofd, "w") : NULL;
	if (!out) {
		if (ofd >= 0)
			close(ofd);
		fclose(in);
		return -1;
	}
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	int found = 0;
	while ((n = getline(&line, &cap, in)) >= 0) {
		char field[128];
		size_t i = 0;
		while (line[i] && line[i] != ':' && i < sizeof(field) - 1)
			field[i] = line[i], i++;
		field[i] = '\0';
		if (strcmp(field, name) == 0) {
			found = 1;
			continue; /* drop */
		}
		fputs(line, out);
	}
	free(line);
	fclose(in);
	fclose(out);
	chmod(tmp, mode);
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return found ? 0 : 1;
}

/* Remove `user` from group member lists in /etc/group.  If `only_group` is
 * non-NULL, only that group is modified; otherwise every group is. */
static int strip_user_from_groups(const char *user, const char *only_group)
{
	FILE *in = fopen("/etc/group", "r");
	if (!in)
		return -1;
	int ofd = open("/etc/group+", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	FILE *out = ofd >= 0 ? fdopen(ofd, "w") : NULL;
	if (!out) {
		if (ofd >= 0)
			close(ofd);
		fclose(in);
		return -1;
	}
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, in)) >= 0) {
		if (n > 0 && line[n - 1] == '\n')
			line[--n] = '\0';
		/* name:passwd:gid:members */
		char gname[128];
		size_t gi = 0;
		while (line[gi] && line[gi] != ':' && gi < sizeof(gname) - 1)
			gname[gi] = line[gi], gi++;
		gname[gi] = '\0';
		if (only_group && strcmp(gname, only_group) != 0) {
			fprintf(out, "%s\n", line); /* leave other groups intact */
			continue;
		}
		char *third = line;
		int colons = 0;
		for (char *p = line; *p; p++)
			if (*p == ':' && ++colons == 3) {
				third = p + 1;
				break;
			}
		if (colons < 3) {
			fprintf(out, "%s\n", line);
			continue;
		}
		char head[256];
		int hlen = (int)(third - line); /* includes the 3rd ':' */
		if (hlen > (int)sizeof(head) - 1)
			hlen = sizeof(head) - 1;
		memcpy(head, line, hlen);
		head[hlen] = '\0';

		char out_mem[512];
		out_mem[0] = '\0';
		char *save = third, *tok;
		int first = 1;
		while ((tok = strsep(&save, ",")) != NULL) {
			if (!*tok || strcmp(tok, user) == 0)
				continue;
			if (!first)
				strncat(out_mem, ",", sizeof(out_mem) - strlen(out_mem) - 1);
			strncat(out_mem, tok, sizeof(out_mem) - strlen(out_mem) - 1);
			first = 0;
		}
		fprintf(out, "%s%s\n", head, out_mem);
	}
	free(line);
	fclose(in);
	fclose(out);
	chmod("/etc/group+", 0644);
	if (rename("/etc/group+", "/etc/group") != 0) {
		unlink("/etc/group+");
		return -1;
	}
	return 0;
}

/* Return 1 and copy the owner name if `gid` is some user's primary group. */
static int gid_is_primary(gid_t gid, char *owner, size_t osz)
{
	struct passwd *pw;
	setpwent();
	int found = 0;
	while ((pw = getpwent()) != NULL) {
		if (pw->pw_gid == gid) {
			snprintf(owner, osz, "%s", pw->pw_name);
			found = 1;
			break;
		}
	}
	endpwent();
	return found;
}

static int group_member_count(const char *group)
{
	struct group *g = getgrnam(group);
	int n = 0;
	if (g && g->gr_mem)
		while (g->gr_mem[n])
			n++;
	return n;
}

/* Recursively remove a directory tree. */
static void rmrf(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				if (!strcmp(de->d_name, ".") ||
				    !strcmp(de->d_name, ".."))
					continue;
				char child[1024];
				snprintf(child, sizeof(child), "%s/%s", path,
				         de->d_name);
				rmrf(child);
			}
			closedir(d);
		}
		rmdir(path);
	} else {
		unlink(path);
	}
}

/* Remove every file/directory owned by `uid`, walking from `root`.  Pseudo
 * filesystems are skipped. */
static void remove_owned(const char *root, uid_t uid)
{
	if (!strcmp(root, "/dev") || !strcmp(root, "/proc") ||
	    !strcmp(root, "/sys"))
		return;
	DIR *d = opendir(root);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char child[1024];
		snprintf(child, sizeof(child), "%s/%s",
		         strcmp(root, "/") ? root : "", de->d_name);
		struct stat st;
		if (lstat(child, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			remove_owned(child, uid);
			if (st.st_uid == uid)
				rmdir(child);
		} else if (st.st_uid == uid) {
			unlink(child);
		}
	}
	closedir(d);
}

/* ---- minimal uncompressed ustar backup of a home directory ---- */
static void octal(char *dst, int len, unsigned long v)
{
	dst[len - 1] = '\0';
	for (int i = len - 2; i >= 0; i--) {
		dst[i] = '0' + (v & 7);
		v >>= 3;
	}
}

static void tar_write_entry(int out, const char *arcname, struct stat *st)
{
	char hdr[512];
	memset(hdr, 0, sizeof(hdr));
	snprintf(hdr, 100, "%s", arcname);
	octal(hdr + 100, 8, st->st_mode & 07777);
	octal(hdr + 108, 8, st->st_uid);
	octal(hdr + 116, 8, st->st_gid);
	octal(hdr + 124, 12, S_ISDIR(st->st_mode) ? 0 : (unsigned long)st->st_size);
	octal(hdr + 136, 12, 0); /* mtime */
	memset(hdr + 148, ' ', 8); /* checksum field spaces during calc */
	hdr[156] = S_ISDIR(st->st_mode) ? '5' : '0';
	memcpy(hdr + 257, "ustar", 5);
	hdr[263] = '0';
	hdr[264] = '0';
	unsigned int sum = 0;
	for (int i = 0; i < 512; i++)
		sum += (unsigned char)hdr[i];
	octal(hdr + 148, 7, sum);
	hdr[155] = ' ';
	write(out, hdr, 512);

	if (!S_ISDIR(st->st_mode)) {
		int in = open(arcname, O_RDONLY);
		if (in >= 0) {
			char buf[512];
			ssize_t r;
			while ((r = read(in, buf, sizeof(buf))) > 0) {
				if (r < 512)
					memset(buf + r, 0, 512 - r);
				write(out, buf, 512);
			}
			close(in);
		}
	}
}

static void tar_walk(int out, const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return;
	tar_write_entry(out, path, &st);
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (!d)
			return;
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			char child[1024];
			snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
			tar_walk(out, child);
		}
		closedir(d);
	}
}

static int backup_home(const char *home, const char *user, const char *dir)
{
	char dest[512];
	snprintf(dest, sizeof(dest), "%s/%s.tar", dir && *dir ? dir : ".", user);
	int out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		fprintf(stderr, "deluser: cannot create backup %s: %s\n", dest,
		        strerror(errno));
		return -1;
	}
	tar_walk(out, home);
	char zero[1024];
	memset(zero, 0, sizeof(zero));
	write(out, zero, sizeof(zero)); /* two zero blocks = end of archive */
	close(out);
	info("Backing up files to be removed to %s ...\n", dest);
	return 0;
}

static void usage(int status) __attribute__((noreturn));
static void usage(int status)
{
	FILE *o = status ? stderr : stdout;
	fprintf(o,
	    "Usage: deluser [options] USER          remove a normal user\n"
	    "       deluser --system [options] USER remove a system user\n"
	    "       deluser --group GROUP           remove a group\n"
	    "       delgroup [options] GROUP        remove a group\n"
	    "       deluser USER GROUP              remove a user from a group\n\n"
	    "Options:\n"
	    "      --system              only remove a system user/group\n"
	    "      --group               remove a group\n"
	    "      --remove-home         remove the home directory and mailspool\n"
	    "      --remove-all-files    remove all files owned by the user\n"
	    "      --backup              back up files before removing them\n"
	    "      --backup-to DIR       place the backup archive in DIR (implies --backup)\n"
	    "      --backup-suffix STR   backup compression suffix (uncompressed tar only)\n"
	    "      --only-if-empty       only remove a group if it has no members\n"
	    "      --quiet / --verbose / --debug\n"
	    "      --conf FILE           configuration file (accepted, unused)\n"
	    "      --help                display this help\n"
	    "      --version             display version\n");
	exit(status);
}

/* ---- remove a group ---- */
static int do_delgroup(const char *name, int system, int only_if_empty)
{
	struct group *g = getgrnam(name);
	if (!g) {
		if (system)
			return 0; /* --system: absent is not an error */
		fprintf(stderr, "delgroup: group '%s' does not exist.\n", name);
		return 1;
	}
	if (g->gr_gid == 0) {
		fprintf(stderr, "delgroup: refusing to remove group with gid 0.\n");
		return 1;
	}
	if (system && !is_sys_gid(g->gr_gid)) {
		fprintf(stderr, "delgroup: '%s' is not a system group.\n", name);
		return 1;
	}
	char owner[64];
	if (gid_is_primary(g->gr_gid, owner, sizeof(owner))) {
		fprintf(stderr,
		    "delgroup: cannot remove group '%s': it is the primary "
		    "group of user '%s'.\n", name, owner);
		return 1;
	}
	if (only_if_empty && group_member_count(name) > 0) {
		fprintf(stderr, "delgroup: group '%s' is not empty.\n", name);
		return 1;
	}
	info("Removing group `%s' ...\n", name);
	if (db_drop("/etc/group", name, 0644) < 0) {
		fprintf(stderr, "delgroup: cannot update /etc/group: %s\n",
		        strerror(errno));
		return 1;
	}
	info("Done.\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *prog = strrchr(argv[0], '/');
	prog = prog ? prog + 1 : argv[0];
	is_delgroup = (strcmp(prog, "delgroup") == 0);

	int system = 0, want_group = 0, only_if_empty = 0;
	int remove_home = 0, remove_all = 0, do_backup = 0;
	const char *backup_to = NULL;
	int c;

	enum {
		O_SYSTEM = 256, O_GROUP, O_ONLYEMPTY, O_RMHOME, O_RMALL,
		O_BACKUP, O_BACKUPTO, O_BACKUPSUF, O_CONF, O_DEBUG, O_QUIET,
		O_VERBOSE, O_MSGLEVEL, O_HELP, O_VERSION
	};
	static struct option lo[] = {
		{ "system", no_argument, 0, O_SYSTEM },
		{ "group", no_argument, 0, O_GROUP },
		{ "only-if-empty", no_argument, 0, O_ONLYEMPTY },
		{ "remove-home", no_argument, 0, O_RMHOME },
		{ "remove-all-files", no_argument, 0, O_RMALL },
		{ "backup", no_argument, 0, O_BACKUP },
		{ "backup-to", required_argument, 0, O_BACKUPTO },
		{ "backup-suffix", required_argument, 0, O_BACKUPSUF },
		{ "conf", required_argument, 0, O_CONF },
		{ "debug", no_argument, 0, O_DEBUG },
		{ "quiet", no_argument, 0, O_QUIET },
		{ "verbose", no_argument, 0, O_VERBOSE },
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
		case O_ONLYEMPTY: only_if_empty = 1; break;
		case O_RMHOME: remove_home = 1; break;
		case O_RMALL: remove_all = 1; break;
		case O_BACKUP: do_backup = 1; break;
		case O_BACKUPTO: do_backup = 1; backup_to = optarg; break;
		case O_BACKUPSUF: break; /* only uncompressed tar is produced */
		case O_QUIET: opt_quiet = 1; break;
		case O_DEBUG: case O_VERBOSE: opt_verbose = 1; break;
		case O_CONF: case O_MSGLEVEL: break; /* accepted, unused */
		case O_HELP: usage(0);
		case O_VERSION: printf("%s\n", VERSION_STRING); return 0;
		default: usage(1);
		}
	}

	if (geteuid() != 0) {
		fprintf(stderr, "%s: Only root may remove users or groups.\n",
		        prog);
		return 1;
	}

	int nargs = argc - optind;

	/* group removal mode */
	if ((is_delgroup || want_group) && nargs == 1)
		return do_delgroup(argv[optind], system, only_if_empty);

	/* remove a user from a group */
	if (nargs == 2) {
		const char *user = argv[optind], *group = argv[optind + 1];
		struct passwd *pw = getpwnam(user);
		struct group *g = getgrnam(group);
		if (!pw) {
			fprintf(stderr, "deluser: user '%s' does not exist.\n",
			        user);
			return 1;
		}
		if (!g) {
			fprintf(stderr, "deluser: group '%s' does not exist.\n",
			        group);
			return 1;
		}
		if (pw->pw_gid == g->gr_gid) {
			fprintf(stderr,
			    "deluser: cannot remove user '%s' from their primary "
			    "group '%s'.\n", user, group);
			return 1;
		}
		int member = 0;
		for (char **m = g->gr_mem; m && *m; m++)
			if (strcmp(*m, user) == 0)
				member = 1;
		if (!member) {
			fprintf(stderr,
			    "deluser: user '%s' is not a member of group '%s'.\n",
			    user, group);
			return 1;
		}
		info("Removing user `%s' from group `%s' ...\n", user, group);
		if (strip_user_from_groups(user, group) < 0) {
			fprintf(stderr, "deluser: cannot update /etc/group: %s\n",
			        strerror(errno));
			return 1;
		}
		info("Done.\n");
		return 0;
	}

	if (nargs != 1)
		usage(1);

	/* ---- remove a user ---- */
	const char *name = argv[optind];
	struct passwd *pw = getpwnam(name);
	if (!pw) {
		if (system) {
			info("No system user `%s' to remove.\n", name);
			return 0;
		}
		fprintf(stderr, "deluser: user '%s' does not exist.\n", name);
		return 1;
	}
	if (pw->pw_uid == 0) {
		fprintf(stderr, "deluser: refusing to remove user with uid 0.\n");
		return 1;
	}
	if (system && !is_sys_uid(pw->pw_uid)) {
		fprintf(stderr, "deluser: '%s' is not a system user.\n", name);
		return 1;
	}

	/* Capture what we need before the databases change. */
	uid_t uid = pw->pw_uid;
	gid_t gid = pw->pw_gid;
	char home[256], uname[64];
	snprintf(home, sizeof(home), "%s", pw->pw_dir);
	snprintf(uname, sizeof(uname), "%s", pw->pw_name);

	info("Removing user `%s' ...\n", name);

	if (do_backup)
		backup_home(home, uname, backup_to);

	if (remove_all)
		remove_owned("/", uid);
	if (remove_home || remove_all) {
		char mail[512];
		snprintf(mail, sizeof(mail), "%s/%s", MAILSPOOL_DIR, uname);
		unlink(mail);
		if (home[0] && strcmp(home, "/") != 0 &&
		    strcmp(home, "/nonexistent") != 0) {
			info("Removing home directory `%s' ...\n", home);
			rmrf(home);
		}
	}

	if (db_drop("/etc/passwd", name, 0644) < 0 ||
	    db_drop("/etc/shadow", name, 0600) < 0) {
		fprintf(stderr, "deluser: cannot update account database: %s\n",
		        strerror(errno));
		return 1;
	}
	strip_user_from_groups(name, NULL);

	/* Remove the user's private group if it is now unused. */
	struct group *g = getgrnam(uname);
	char other[64];
	if (g && g->gr_gid == gid && group_member_count(uname) == 0 &&
	    !gid_is_primary(gid, other, sizeof(other))) {
		info("Removing group `%s' ...\n", uname);
		db_drop("/etc/group", uname, 0644);
	}

	info("Done.\n");
	return 0;
}
