/*
 * pwd.c - /etc/passwd database access for LikeOS libc.
 *
 * Each line is "name:passwd:uid:gid:gecos:dir:shell".  The reentrant helpers
 * parse a line in place inside a caller-supplied buffer; the classic entry
 * points wrap them with static storage.
 */
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PASSWD_PATH "/etc/passwd"
#define PW_BUFSZ    1024

/* Split `line` in place on ':' and point the struct passwd fields at it. */
static int parse_pw_line(char *line, struct passwd *pwd)
{
	char *p = line, *tok;
	char *name, *passwd, *uid, *gid, *gecos, *dir, *shell;

	name   = strsep(&p, ":");
	passwd = strsep(&p, ":");
	uid    = strsep(&p, ":");
	gid    = strsep(&p, ":");
	gecos  = strsep(&p, ":");
	dir    = strsep(&p, ":");
	shell  = strsep(&p, ":");

	if (!name || !passwd || !uid || !gid)
		return -1;

	pwd->pw_name   = name;
	pwd->pw_passwd = passwd;
	pwd->pw_uid    = (uid_t)strtoul(uid, &tok, 10);
	pwd->pw_gid    = (gid_t)strtoul(gid, &tok, 10);
	pwd->pw_gecos  = gecos ? gecos : (char *)"";
	pwd->pw_dir    = dir   ? dir   : (char *)"";
	pwd->pw_shell  = shell ? shell : (char *)"";
	return 0;
}

int fgetpwent_r(FILE *stream, struct passwd *pwd, char *buf, size_t buflen,
                struct passwd **result)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;

	*result = NULL;
	if (!stream) {
		errno = EINVAL;
		return EINVAL;
	}

	while ((n = getline(&line, &cap, stream)) >= 0) {
		char *s = line;
		/* strip trailing newline */
		if (n > 0 && s[n - 1] == '\n')
			s[--n] = '\0';
		if (s[0] == '\0' || s[0] == '#')
			continue; /* blank or comment */
		if ((size_t)n + 1 > buflen) {
			free(line);
			errno = ERANGE;
			return ERANGE;
		}
		memcpy(buf, s, (size_t)n + 1);
		free(line);
		if (parse_pw_line(buf, pwd) != 0)
			continue; /* malformed - skip */
		*result = pwd;
		return 0;
	}
	free(line);
	return ENOENT; /* end of file */
}

struct passwd *fgetpwent(FILE *stream)
{
	static struct passwd pw;
	static char buf[PW_BUFSZ];
	struct passwd *res;
	if (fgetpwent_r(stream, &pw, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result)
{
	FILE *fp;
	int rc;

	*result = NULL;
	if (!name) {
		errno = EINVAL;
		return EINVAL;
	}
	fp = fopen(PASSWD_PATH, "r");
	if (!fp)
		return errno;
	while ((rc = fgetpwent_r(fp, pwd, buf, buflen, result)) == 0) {
		if (strcmp(pwd->pw_name, name) == 0) {
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);
	*result = NULL;
	return (rc == ENOENT) ? 0 : rc; /* not found is not an error for _r */
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result)
{
	FILE *fp;
	int rc;

	*result = NULL;
	fp = fopen(PASSWD_PATH, "r");
	if (!fp)
		return errno;
	while ((rc = fgetpwent_r(fp, pwd, buf, buflen, result)) == 0) {
		if (pwd->pw_uid == uid) {
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);
	*result = NULL;
	return (rc == ENOENT) ? 0 : rc;
}

struct passwd *getpwnam(const char *name)
{
	static struct passwd pw;
	static char buf[PW_BUFSZ];
	struct passwd *res;
	if (getpwnam_r(name, &pw, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

struct passwd *getpwuid(uid_t uid)
{
	static struct passwd pw;
	static char buf[PW_BUFSZ];
	struct passwd *res;
	if (getpwuid_r(uid, &pw, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

/* ---- iterator ---- */
static FILE *pw_iter;

void setpwent(void)
{
	if (pw_iter)
		rewind(pw_iter);
	else
		pw_iter = fopen(PASSWD_PATH, "r");
}

void endpwent(void)
{
	if (pw_iter) {
		fclose(pw_iter);
		pw_iter = NULL;
	}
}

struct passwd *getpwent(void)
{
	static struct passwd pw;
	static char buf[PW_BUFSZ];
	struct passwd *res;
	if (!pw_iter) {
		pw_iter = fopen(PASSWD_PATH, "r");
		if (!pw_iter)
			return NULL;
	}
	if (fgetpwent_r(pw_iter, &pw, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

int putpwent(const struct passwd *pwd, FILE *stream)
{
	if (!pwd || !stream) {
		errno = EINVAL;
		return -1;
	}
	if (fprintf(stream, "%s:%s:%u:%u:%s:%s:%s\n",
	            pwd->pw_name, pwd->pw_passwd, pwd->pw_uid, pwd->pw_gid,
	            pwd->pw_gecos ? pwd->pw_gecos : "",
	            pwd->pw_dir ? pwd->pw_dir : "",
	            pwd->pw_shell ? pwd->pw_shell : "") < 0)
		return -1;
	return 0;
}
