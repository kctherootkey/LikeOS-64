/*
 * grp.c - /etc/group database access for LikeOS libc.
 *
 * Each line is "name:passwd:gid:member,member,...".  The member list is parsed
 * into a NULL-terminated char* array packed into the tail of the caller buffer.
 */
#include <grp.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>

#define GROUP_PATH "/etc/group"
#define GR_BUFSZ   2048
#define GR_MAXMEM  64

static int parse_gr_line(char *buf, size_t used, size_t buflen,
                         struct group *grp)
{
	char *p = buf, *tok;
	char *name, *passwd, *gid, *members;
	char **mem;
	size_t off;
	int count = 0;

	name    = strsep(&p, ":");
	passwd  = strsep(&p, ":");
	gid     = strsep(&p, ":");
	members = p; /* remainder (may be NULL/empty) */

	if (!name || !passwd || !gid)
		return -1;

	/* Pointer array goes after the line text, pointer-aligned. */
	off = (used + sizeof(char *) - 1) & ~(sizeof(char *) - 1);
	if (off + sizeof(char *) > buflen)
		return -2; /* ERANGE */
	mem = (char **)(buf + off);

	if (members && members[0]) {
		char *m = members;
		char *item;
		while ((item = strsep(&m, ",")) != NULL) {
			if (item[0] == '\0')
				continue;
			if (count >= GR_MAXMEM)
				break;
			if (off + (size_t)(count + 2) * sizeof(char *) > buflen)
				return -2;
			mem[count++] = item;
		}
	}
	mem[count] = NULL;

	grp->gr_name   = name;
	grp->gr_passwd = passwd;
	grp->gr_gid    = (gid_t)strtoul(gid, &tok, 10);
	grp->gr_mem    = mem;
	return 0;
}

static int read_gr(FILE *stream, struct group *grp, char *buf, size_t buflen,
                   struct group **result)
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
		if (n > 0 && s[n - 1] == '\n')
			s[--n] = '\0';
		if (s[0] == '\0' || s[0] == '#')
			continue;
		if ((size_t)n + 1 > buflen) {
			free(line);
			errno = ERANGE;
			return ERANGE;
		}
		memcpy(buf, s, (size_t)n + 1);
		free(line);
		int pr = parse_gr_line(buf, (size_t)n + 1, buflen, grp);
		if (pr == -2) {
			errno = ERANGE;
			return ERANGE;
		}
		if (pr != 0)
			continue;
		*result = grp;
		return 0;
	}
	free(line);
	return ENOENT;
}

struct group *fgetgrent(FILE *stream)
{
	static struct group gr;
	static char buf[GR_BUFSZ];
	struct group *res;
	if (read_gr(stream, &gr, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result)
{
	FILE *fp;
	int rc;

	*result = NULL;
	if (!name) {
		errno = EINVAL;
		return EINVAL;
	}
	fp = fopen(GROUP_PATH, "r");
	if (!fp)
		return errno;
	while ((rc = read_gr(fp, grp, buf, buflen, result)) == 0) {
		if (strcmp(grp->gr_name, name) == 0) {
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);
	*result = NULL;
	return (rc == ENOENT) ? 0 : rc;
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result)
{
	FILE *fp;
	int rc;

	*result = NULL;
	fp = fopen(GROUP_PATH, "r");
	if (!fp)
		return errno;
	while ((rc = read_gr(fp, grp, buf, buflen, result)) == 0) {
		if (grp->gr_gid == gid) {
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);
	*result = NULL;
	return (rc == ENOENT) ? 0 : rc;
}

struct group *getgrnam(const char *name)
{
	static struct group gr;
	static char buf[GR_BUFSZ];
	struct group *res;
	if (getgrnam_r(name, &gr, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

struct group *getgrgid(gid_t gid)
{
	static struct group gr;
	static char buf[GR_BUFSZ];
	struct group *res;
	if (getgrgid_r(gid, &gr, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

/* ---- iterator ---- */
static FILE *gr_iter;

void setgrent(void)
{
	if (gr_iter)
		rewind(gr_iter);
	else
		gr_iter = fopen(GROUP_PATH, "r");
}

void endgrent(void)
{
	if (gr_iter) {
		fclose(gr_iter);
		gr_iter = NULL;
	}
}

struct group *getgrent(void)
{
	static struct group gr;
	static char buf[GR_BUFSZ];
	struct group *res;
	if (!gr_iter) {
		gr_iter = fopen(GROUP_PATH, "r");
		if (!gr_iter)
			return NULL;
	}
	if (read_gr(gr_iter, &gr, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

int putgrent(const struct group *grp, FILE *stream)
{
	int i;
	if (!grp || !stream) {
		errno = EINVAL;
		return -1;
	}
	if (fprintf(stream, "%s:%s:%u:", grp->gr_name, grp->gr_passwd,
	            grp->gr_gid) < 0)
		return -1;
	for (i = 0; grp->gr_mem && grp->gr_mem[i]; i++) {
		if (fprintf(stream, "%s%s", i ? "," : "", grp->gr_mem[i]) < 0)
			return -1;
	}
	if (fputc('\n', stream) < 0)
		return -1;
	return 0;
}

int initgroups(const char *user, gid_t group)
{
	int list[NGROUPS_MAX];
	int ngroups = 0;
	struct group *gr;
	int i;

	if (!user) {
		errno = EINVAL;
		return -1;
	}

	list[ngroups++] = (int)group; /* primary group first */

	setgrent();
	while ((gr = getgrent()) != NULL) {
		if (gr->gr_gid == group)
			continue; /* already included */
		for (i = 0; gr->gr_mem && gr->gr_mem[i]; i++) {
			if (strcmp(gr->gr_mem[i], user) == 0) {
				if (ngroups < NGROUPS_MAX)
					list[ngroups++] = (int)gr->gr_gid;
				break;
			}
		}
	}
	endgrent();

	return setgroups(ngroups, list);
}
