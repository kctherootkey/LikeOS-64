// devd: names that come from devices and users, made safe.
//
// A label is whatever bytes the medium carries -- written by any system, or
// by someone who wants to break this one.  It is shown to people (made valid
// UTF-8 without control characters) and it names the mount directory (one
// path component that cannot climb out of /media/<user>).  Pure functions:
// tested on the build machine.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdio.h>
#include <string.h>

#include "devd.h"

size_t strlcpy_d(char *dst, const char *src, size_t cap)
{
	size_t n = strlen(src);

	if (cap) {
		size_t c = n < cap - 1 ? n : cap - 1;
		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return n;
}

/* Length of the valid UTF-8 character at s (at most `avail' bytes), 0 when
 * the bytes there are not one (surrogates and overlong forms included). */
static size_t utf8_char(const uint8_t *s, size_t avail, uint32_t *cp)
{
	uint32_t c;
	size_t len;

	if (avail == 0)
		return 0;
	if (s[0] < 0x80) {
		*cp = s[0];
		return 1;
	}
	if (s[0] >= 0xC2 && s[0] <= 0xDF) {
		len = 2;
		c = s[0] & 0x1F;
	} else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
		len = 3;
		c = s[0] & 0x0F;
	} else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
		len = 4;
		c = s[0] & 0x07;
	} else {
		return 0;
	}
	if (len > avail)
		return 0;
	for (size_t i = 1; i < len; i++) {
		if ((s[i] & 0xC0) != 0x80)
			return 0;
		c = (c << 6) | (s[i] & 0x3F);
	}
	if ((len == 3 && c < 0x800) || (len == 4 && c < 0x10000) ||
	    c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
		return 0;
	*cp = c;
	return len;
}

/* C0 and C1 controls and DEL */
static int is_control(uint32_t cp)
{
	return cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f);
}

int utf8_text_ok(const char *s, size_t n)
{
	size_t i = 0;

	while (i < n) {
		uint32_t cp;
		size_t l = utf8_char((const uint8_t *)s + i, n - i, &cp);

		if (l == 0 || is_control(cp))
			return 0;
		i += l;
	}
	return 1;
}

void label_display(char *dst, size_t cap, const uint8_t *src, size_t n)
{
	size_t out = 0, i = 0, end = 0;

	if (!cap)
		return;
	/* stop at the first NUL, drop trailing blanks */
	while (end < n && src[end])
		end++;
	while (end > 0 && src[end - 1] == ' ')
		end--;
	while (i < end) {
		uint32_t cp;
		size_t l = utf8_char(src + i, end - i, &cp);

		if (l == 0 || is_control(cp)) {
			if (out + 1 >= cap)
				break;
			dst[out++] = '?';
			i += l ? l : 1;
			continue;
		}
		if (out + l >= cap)
			break;
		memcpy(dst + out, src + i, l);
		out += l;
		i += l;
	}
	while (out > 0 && dst[out - 1] == ' ')
		out--;
	dst[out] = '\0';
}

/* One candidate made into a directory name; 0 when nothing usable is left. */
static int mountname_one(char *dst, size_t cap, const char *src)
{
	char tmp[DEVD_LABEL_MAX];
	size_t out = 0, i = 0, n;
	size_t lim = cap - 1 < DEVD_MNTNAME_MAX ? cap - 1 : DEVD_MNTNAME_MAX;

	if (!src || !*src || cap < 2)
		return 0;
	/* valid UTF-8 without control characters first */
	label_display(tmp, sizeof(tmp), (const uint8_t *)src, strlen(src));
	n = strlen(tmp);
	/* no leading dots or blanks: never hidden, never "." or ".." */
	while (i < n && (tmp[i] == '.' || tmp[i] == ' '))
		i++;
	while (i < n) {
		uint32_t cp;
		size_t l = utf8_char((const uint8_t *)tmp + i, n - i, &cp);

		if (l == 0)
			l = 1; /* cannot happen after label_display */
		if (out + l > lim)
			break;
		if (l == 1 && (tmp[i] == '/' || tmp[i] == '\\' ||
			       (unsigned char)tmp[i] < 0x20 || tmp[i] == 0x7f))
			dst[out] = '_';
		else
			memcpy(dst + out, tmp + i, l);
		out += l;
		i += l;
	}
	while (out > 0 && dst[out - 1] == ' ')
		out--;
	dst[out] = '\0';
	if (out == 0 || strcmp(dst, ".") == 0 || strcmp(dst, "..") == 0)
		return 0;
	return 1;
}

void mountname_choose(char *dst, size_t cap, const char *label,
		      const char *uuid, const char *devname)
{
	if (mountname_one(dst, cap, label) || mountname_one(dst, cap, uuid) ||
	    mountname_one(dst, cap, devname))
		return;
	strlcpy_d(dst, "disk", cap);
}

void mountname_suffix(char *dst, size_t cap, const char *base, unsigned n)
{
	char num[16];
	size_t nl, bl = strlen(base), lim;

	snprintf(num, sizeof(num), "%u", n);
	nl = strlen(num);
	lim = DEVD_MNTNAME_MAX;
	if (cap - 1 < lim)
		lim = cap - 1;
	if (bl + nl > lim) {
		bl = lim > nl ? lim - nl : 0;
		/* never cut a character in half */
		while (bl > 0 && ((unsigned char)base[bl] & 0xC0) == 0x80)
			bl--;
	}
	memcpy(dst, base, bl);
	memcpy(dst + bl, num, nl);
	dst[bl + nl] = '\0';
}

int username_ok(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n >= DEVD_USERDIR_MAX || s[0] == '.' || s[0] == '-')
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' ||
		      c == '.'))
			return 0;
	}
	return 1;
}

int devname_ok(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n >= DEVD_NAME_MAX || s[0] == '.' || s[0] == '-')
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' ||
		      c == '.' || c == ':'))
			return 0;
	}
	return 1;
}
