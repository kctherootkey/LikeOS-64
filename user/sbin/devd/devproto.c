// devd protocol helpers: value encoding, tokens, reply lines.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>

#include "devproto.h"

static const char g_hex[] = "0123456789ABCDEF";

static int needs_escape(unsigned char c)
{
	return c < 0x21 || c > 0x7e || c == '%' || c == '=';
}

size_t devp_encode(char *dst, size_t cap, const char *src, size_t n)
{
	size_t need = 0, out = 0;

	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)src[i];
		size_t w = needs_escape(c) ? 3 : 1;

		if (dst && out + w < cap) {
			if (w == 3) {
				dst[out] = '%';
				dst[out + 1] = g_hex[c >> 4];
				dst[out + 2] = g_hex[c & 15];
			} else {
				dst[out] = (char)c;
			}
			out += w;
		}
		need += w;
	}
	if (dst && cap)
		dst[out < cap ? out : cap - 1] = '\0';
	return need;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

long devp_decode(char *dst, size_t cap, const char *src)
{
	size_t out = 0;

	if (!cap)
		return -1;
	for (size_t i = 0; src[i]; i++) {
		unsigned char c = (unsigned char)src[i];
		int v;

		if (c == '%') {
			int h = hexval(src[i + 1]);
			int l = h < 0 ? -1 : hexval(src[i + 2]);

			if (h < 0 || l < 0)
				return -1;
			v = h * 16 + l;
			i += 2;
		} else if (needs_escape(c)) {
			return -1;
		} else {
			v = c;
		}
		if (out + 1 >= cap)
			return -1;
		dst[out++] = (char)v;
	}
	dst[out] = '\0';
	return (long)out;
}

int devp_split(char *line, char **tok, int max)
{
	int n = 0;
	char *p = line;

	if (!*p)
		return 0;
	for (;;) {
		char *start = p;

		while (*p && *p != ' ') {
			if ((unsigned char)*p < 0x20 || *p == 0x7f)
				return -1;
			p++;
		}
		if (p == start || n >= max)
			return -1; /* empty token or too many */
		tok[n++] = start;
		if (!*p)
			break;
		*p++ = '\0';
		if (!*p)
			return -1; /* trailing space */
	}
	return n;
}

const char *devp_find(char *const *tok, int n, int first, const char *key,
		      int *dup)
{
	size_t kl = strlen(key);
	const char *found = NULL;

	for (int i = first; i < n; i++) {
		if (strncmp(tok[i], key, kl) == 0 && tok[i][kl] == '=') {
			if (found && dup)
				*dup = 1;
			if (!found)
				found = tok[i] + kl + 1;
		}
	}
	return found;
}

static void line_put(struct devp_line *l, const char *s, size_t n)
{
	if (l->overflow)
		return;
	/* one byte stays free for the '\n' */
	if (l->len + n + 1 > sizeof(l->buf)) {
		l->overflow = 1;
		return;
	}
	memcpy(l->buf + l->len, s, n);
	l->len += n;
}

void devp_line_begin(struct devp_line *l, const char *word)
{
	l->len = 0;
	l->overflow = 0;
	line_put(l, word, strlen(word));
}

void devp_line_word(struct devp_line *l, const char *word)
{
	line_put(l, " ", 1);
	line_put(l, word, strlen(word));
}

void devp_line_enc(struct devp_line *l, const char *value)
{
	char tmp[DEVP_LINE_MAX];
	size_t need = devp_encode(tmp, sizeof(tmp), value, strlen(value));

	if (need >= sizeof(tmp)) {
		l->overflow = 1;
		return;
	}
	line_put(l, " ", 1);
	line_put(l, tmp, need);
}

void devp_line_kv(struct devp_line *l, const char *key, const char *value)
{
	char tmp[DEVP_LINE_MAX];
	size_t need = devp_encode(tmp, sizeof(tmp), value, strlen(value));

	if (need >= sizeof(tmp)) {
		l->overflow = 1;
		return;
	}
	line_put(l, " ", 1);
	line_put(l, key, strlen(key));
	line_put(l, "=", 1);
	line_put(l, tmp, need);
}

static size_t fmt_u64(char *out, uint64_t v)
{
	char t[24];
	size_t n = 0, i;

	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	for (i = 0; i < n; i++)
		out[i] = t[n - 1 - i];
	out[n] = '\0';
	return n;
}

void devp_line_kv_u64(struct devp_line *l, const char *key, uint64_t v)
{
	char num[24];

	fmt_u64(num, v);
	devp_line_kv(l, key, num);
}

void devp_line_kv_int(struct devp_line *l, const char *key, long long v)
{
	char num[26];

	if (v < 0) {
		num[0] = '-';
		fmt_u64(num + 1, (uint64_t)0 - (uint64_t)v);
	} else {
		fmt_u64(num, (uint64_t)v);
	}
	/* '-' and digits never need escaping */
	devp_line_kv(l, key, num);
}

long devp_line_end(struct devp_line *l)
{
	if (l->overflow || l->len + 1 > sizeof(l->buf))
		return -1;
	l->buf[l->len++] = '\n';
	return (long)l->len;
}

const char *devp_errname(int err)
{
	switch (err) {
	case EPERM: return "EPERM";
	case ENOENT: return "ENOENT";
	case EIO: return "EIO";
	case ENXIO: return "ENXIO";
	case EAGAIN: return "EAGAIN";
	case ENOMEM: return "ENOMEM";
	case EACCES: return "EACCES";
	case EBUSY: return "EBUSY";
	case EEXIST: return "EEXIST";
	case ENODEV: return "ENODEV";
	case ENOTDIR: return "ENOTDIR";
	case EISDIR: return "EISDIR";
	case EINVAL: return "EINVAL";
	case ENOSPC: return "ENOSPC";
	case EROFS: return "EROFS";
	case ENAMETOOLONG: return "ENAMETOOLONG";
	case ENOSYS: return "ENOSYS";
	case ENOTSUP: return "ENOTSUP";
	case ECANCELED: return "ECANCELED";
	case ENOMEDIUM: return "ENOMEDIUM";
	case EUCLEAN: return "EUCLEAN";
	default: return "EIO";
	}
}

int devp_mount_parse(char *const *tok, int n, int *recover)
{
	if (n < 2 || n > 3 || strcmp(tok[0], "MOUNT") != 0)
		return -1;
	if (n == 3 && strcmp(tok[2], "recover") != 0)
		return -1;
	*recover = n == 3;
	return 0;
}

int devp_cancel_parse(char *const *tok, int n)
{
	if (n != 2 || strcmp(tok[0], "CANCEL") != 0)
		return -1;
	return 0;
}

int devp_parse_u64(const char *s, uint64_t max, uint64_t *out)
{
	uint64_t v = 0;

	if (!s || !*s)
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		if (v > (max - (uint64_t)(*s - '0')) / 10)
			return -1;
		v = v * 10 + (uint64_t)(*s - '0');
	}
	*out = v;
	return 0;
}

int devp_automount(const struct devp_flags *f)
{
	if (f->system || f->internal || !f->supported || f->ejected)
		return 0;
	return f->optical || (f->usb && f->removable);
}
