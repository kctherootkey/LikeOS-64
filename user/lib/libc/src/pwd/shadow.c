/*
 * shadow.c - /etc/shadow database access for LikeOS libc.
 *
 * Each line is "name:hash:lstchg:min:max:warn:inact:expire:flag".  Empty
 * numeric fields decode to -1.  /etc/shadow is mode 0600, so these calls only
 * succeed for a privileged (root) reader.
 */
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SHADOW_PATH "/etc/shadow"
#define SP_BUFSZ    1024

static long sp_num(const char *s)
{
	if (!s || s[0] == '\0')
		return -1;
	return strtol(s, NULL, 10);
}

/* Parse `buf` (a NUL-terminated shadow line, modified in place). */
static int parse_sp_line(char *buf, struct spwd *sp)
{
	char *p = buf;
	char *name, *pwd, *lstchg, *min, *max, *warn, *inact, *expire, *flag;

	name   = strsep(&p, ":");
	pwd    = strsep(&p, ":");
	lstchg = strsep(&p, ":");
	min    = strsep(&p, ":");
	max    = strsep(&p, ":");
	warn   = strsep(&p, ":");
	inact  = strsep(&p, ":");
	expire = strsep(&p, ":");
	flag   = strsep(&p, ":");

	if (!name || !pwd)
		return -1;

	sp->sp_namp   = name;
	sp->sp_pwdp   = pwd;
	sp->sp_lstchg = sp_num(lstchg);
	sp->sp_min    = sp_num(min);
	sp->sp_max    = sp_num(max);
	sp->sp_warn   = sp_num(warn);
	sp->sp_inact  = sp_num(inact);
	sp->sp_expire = sp_num(expire);
	sp->sp_flag   = (flag && flag[0]) ? strtoul(flag, NULL, 10) : ~0UL;
	return 0;
}

struct spwd *sgetspent(const char *s)
{
	static struct spwd sp;
	static char buf[SP_BUFSZ];
	size_t len;

	if (!s)
		return NULL;
	len = strlen(s);
	if (len + 1 > sizeof(buf)) {
		errno = ERANGE;
		return NULL;
	}
	memcpy(buf, s, len + 1);
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';
	if (parse_sp_line(buf, &sp) != 0)
		return NULL;
	return &sp;
}

static int read_sp(FILE *stream, struct spwd *sp, char *buf, size_t buflen,
                   struct spwd **result)
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
		if (parse_sp_line(buf, sp) != 0)
			continue;
		*result = sp;
		return 0;
	}
	free(line);
	return ENOENT;
}

struct spwd *fgetspent(FILE *stream)
{
	static struct spwd sp;
	static char buf[SP_BUFSZ];
	struct spwd *res;
	if (read_sp(stream, &sp, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

int getspnam_r(const char *name, struct spwd *spbuf, char *buf, size_t buflen,
               struct spwd **spbufp)
{
	FILE *fp;
	int rc;

	*spbufp = NULL;
	if (!name) {
		errno = EINVAL;
		return EINVAL;
	}
	fp = fopen(SHADOW_PATH, "r");
	if (!fp)
		return errno;
	while ((rc = read_sp(fp, spbuf, buf, buflen, spbufp)) == 0) {
		if (strcmp(spbuf->sp_namp, name) == 0) {
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);
	*spbufp = NULL;
	return (rc == ENOENT) ? 0 : rc;
}

struct spwd *getspnam(const char *name)
{
	static struct spwd sp;
	static char buf[SP_BUFSZ];
	struct spwd *res;
	if (getspnam_r(name, &sp, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

/* ---- iterator ---- */
static FILE *sp_iter;

void setspent(void)
{
	if (sp_iter)
		rewind(sp_iter);
	else
		sp_iter = fopen(SHADOW_PATH, "r");
}

void endspent(void)
{
	if (sp_iter) {
		fclose(sp_iter);
		sp_iter = NULL;
	}
}

struct spwd *getspent(void)
{
	static struct spwd sp;
	static char buf[SP_BUFSZ];
	struct spwd *res;
	if (!sp_iter) {
		sp_iter = fopen(SHADOW_PATH, "r");
		if (!sp_iter)
			return NULL;
	}
	if (read_sp(sp_iter, &sp, buf, sizeof(buf), &res) != 0)
		return NULL;
	return res;
}

static int sp_put_num(FILE *stream, long v)
{
	if (v < 0)
		return fputc(':', stream) == EOF ? -1 : 0;
	return fprintf(stream, "%ld:", v) < 0 ? -1 : 0;
}

int putspent(const struct spwd *p, FILE *stream)
{
	if (!p || !stream) {
		errno = EINVAL;
		return -1;
	}
	if (fprintf(stream, "%s:%s:", p->sp_namp, p->sp_pwdp) < 0)
		return -1;
	if (sp_put_num(stream, p->sp_lstchg) || sp_put_num(stream, p->sp_min) ||
	    sp_put_num(stream, p->sp_max) || sp_put_num(stream, p->sp_warn) ||
	    sp_put_num(stream, p->sp_inact) || sp_put_num(stream, p->sp_expire))
		return -1;
	if (p->sp_flag != ~0UL) {
		if (fprintf(stream, "%lu", p->sp_flag) < 0)
			return -1;
	}
	if (fputc('\n', stream) == EOF)
		return -1;
	return 0;
}
