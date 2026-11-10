/*
 * mntent.c - mount table access: setmntent, getmntent(_r), addmntent,
 * endmntent, hasmntopt.  The file format is described in <mntent.h>.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

FILE *setmntent(const char *filename, const char *type)
{
	return fopen(filename, type);
}

int endmntent(FILE *stream)
{
	if (stream)
		fclose(stream);
	return 1; /* always 1, whatever happened */
}

/* Undo the octal escapes of a field in place: \ooo with exactly three octal
 * digits becomes that byte; any other backslash is kept as it is. */
static void unescape(char *s)
{
	char *w = s;

	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' &&
		    s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			*w++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) |
				      (s[3] - '0'));
			s += 4;
		} else {
			*w++ = *s++;
		}
	}
	*w = '\0';
}

static int is_blank(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Cut the next blank-separated field out of *cursor; NULL when none is left. */
static char *next_field(char **cursor)
{
	char *p = *cursor;
	char *start;

	while (*p && is_blank(*p))
		p++;
	if (!*p) {
		*cursor = p;
		return NULL;
	}
	start = p;
	while (*p && !is_blank(*p))
		p++;
	if (*p)
		*p++ = '\0';
	*cursor = p;
	return start;
}

struct mntent *getmntent_r(FILE *stream, struct mntent *mnt, char *buf,
			   int buflen)
{
	if (!stream || !mnt || !buf || buflen <= 1) {
		errno = EINVAL;
		return NULL;
	}

	for (;;) {
		char *cursor, *field;
		size_t len;

		if (!fgets(buf, buflen, stream))
			return NULL;

		len = strlen(buf);
		if (len > 0 && buf[len - 1] != '\n' && !feof(stream)) {
			/* The line did not fit: skip the rest of it, as the
			 * record cannot be returned whole. */
			int c;

			while ((c = fgetc(stream)) != EOF && c != '\n')
				;
			continue;
		}

		cursor = buf;
		while (*cursor && is_blank(*cursor))
			cursor++;
		if (*cursor == '\0' || *cursor == '#')
			continue;

		mnt->mnt_fsname = next_field(&cursor);
		mnt->mnt_dir = next_field(&cursor);
		mnt->mnt_type = next_field(&cursor);
		mnt->mnt_opts = next_field(&cursor);
		if (!mnt->mnt_dir)
			mnt->mnt_dir = cursor;
		if (!mnt->mnt_type)
			mnt->mnt_type = cursor;
		if (!mnt->mnt_opts)
			mnt->mnt_opts = cursor;

		field = next_field(&cursor);
		mnt->mnt_freq = field ? atoi(field) : 0;
		field = next_field(&cursor);
		mnt->mnt_passno = field ? atoi(field) : 0;

		unescape(mnt->mnt_fsname);
		unescape(mnt->mnt_dir);
		unescape(mnt->mnt_type);
		unescape(mnt->mnt_opts);
		return mnt;
	}
}

struct mntent *getmntent(FILE *stream)
{
	static struct mntent mnt;
	static char buf[4096];

	return getmntent_r(stream, &mnt, buf, (int)sizeof(buf));
}

/* Write one field, escaping the characters that would split or end it. */
static int put_field(FILE *stream, const char *s)
{
	if (!s || !*s)
		s = "none";
	for (; *s; s++) {
		int r;

		if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\\')
			r = fprintf(stream, "\\%03o", (unsigned char)*s);
		else
			r = fputc((unsigned char)*s, stream);
		if (r < 0)
			return -1;
	}
	return 0;
}

int addmntent(FILE *stream, const struct mntent *mnt)
{
	if (!stream || !mnt) {
		errno = EINVAL;
		return 1;
	}
	if (fseek(stream, 0, SEEK_END) < 0)
		return 1;
	if (put_field(stream, mnt->mnt_fsname) < 0 || fputc(' ', stream) < 0 ||
	    put_field(stream, mnt->mnt_dir) < 0 || fputc(' ', stream) < 0 ||
	    put_field(stream, mnt->mnt_type) < 0 || fputc(' ', stream) < 0 ||
	    put_field(stream, mnt->mnt_opts) < 0 ||
	    fprintf(stream, " %d %d\n", mnt->mnt_freq, mnt->mnt_passno) < 0)
		return 1;
	return fflush(stream) == 0 ? 0 : 1;
}

/* The option `opt' as a whole word of the option list: at the start or after a
 * comma, and followed by the end, a comma or '='.  Returns where it starts. */
char *hasmntopt(const struct mntent *mnt, const char *opt)
{
	size_t n;
	char *p;

	if (!mnt || !mnt->mnt_opts || !opt)
		return NULL;
	n = strlen(opt);
	if (n == 0)
		return NULL;

	p = mnt->mnt_opts;
	while ((p = strstr(p, opt)) != NULL) {
		if ((p == mnt->mnt_opts || p[-1] == ',') &&
		    (p[n] == '\0' || p[n] == ',' || p[n] == '='))
			return p;
		p += n;
	}
	return NULL;
}
