/* libgpt -- GUIDs.
 *
 * A GUID is kept in canonical order (the order of its text form).  On disk
 * the first three groups (4, 2 and 2 bytes) are stored little-endian and
 * the last two groups (2 and 6 bytes) as they are written, so the EFI
 * system partition type c12a7328-f81f-11d2-ba4b-00a0c93ec93b is stored as
 * 28 73 2a c1 1f f8 d2 11 ba 4b 00 a0 c9 3e c9 3b.
 *
 * Random GUIDs are version 4 (RFC 9562): 122 random bits, the version
 * nibble 4 and the variant bits 10.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "gpt.h"

#if defined(__GLIBC__)
#include <sys/random.h>
#endif

void gpt_guid_to_disk(const struct gpt_guid *g, uint8_t out[16])
{
	const uint8_t *b = g->b;
	uint8_t tmp[16];

	tmp[0] = b[3];
	tmp[1] = b[2];
	tmp[2] = b[1];
	tmp[3] = b[0];
	tmp[4] = b[5];
	tmp[5] = b[4];
	tmp[6] = b[7];
	tmp[7] = b[6];
	memcpy(tmp + 8, b + 8, 8);
	memcpy(out, tmp, 16);
}

void gpt_guid_from_disk(const uint8_t in[16], struct gpt_guid *g)
{
	uint8_t tmp[16];

	/* The swap is its own inverse. */
	tmp[0] = in[3];
	tmp[1] = in[2];
	tmp[2] = in[1];
	tmp[3] = in[0];
	tmp[4] = in[5];
	tmp[5] = in[4];
	tmp[6] = in[7];
	tmp[7] = in[6];
	memcpy(tmp + 8, in + 8, 8);
	memcpy(g->b, tmp, 16);
}

static int gpt_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int gpt_guid_parse(const char *s, struct gpt_guid *out)
{
	/* Where the dashes go in "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx". */
	static const int dash_at[4] = { 8, 13, 18, 23 };
	struct gpt_guid g;
	size_t len;
	int braces = 0, nb = 0;

	if (!s || !out)
		return -EINVAL;
	while (*s == ' ' || *s == '\t')
		s++;
	len = strlen(s);
	while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
		       s[len - 1] == '\n' || s[len - 1] == '\r'))
		len--;
	if (len && s[0] == '{') {
		if (s[len - 1] != '}')
			return -EINVAL;
		braces = 1;
		s++;
		len -= 2;
	}
	(void)braces;
	if (len != 36)
		return -EINVAL;
	for (int i = 0; i < 4; i++)
		if (s[dash_at[i]] != '-')
			return -EINVAL;
	for (size_t i = 0; i < 36;) {
		int hi, lo;

		if (s[i] == '-') {
			i++;
			continue;
		}
		hi = gpt_hexval(s[i]);
		lo = gpt_hexval(s[i + 1]);
		if (hi < 0 || lo < 0 || s[i + 1] == '-')
			return -EINVAL;
		if (nb >= 16)
			return -EINVAL;
		g.b[nb++] = (uint8_t)(hi << 4 | lo);
		i += 2;
	}
	if (nb != 16)
		return -EINVAL;
	*out = g;
	return 0;
}

static void gpt_guid_format_with(const struct gpt_guid *g,
				 char out[GPT_GUID_STR_LEN], const char *hex)
{
	int o = 0;

	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[o++] = '-';
		out[o++] = hex[g->b[i] >> 4];
		out[o++] = hex[g->b[i] & 15];
	}
	out[o] = '\0';
}

void gpt_guid_format(const struct gpt_guid *g, char out[GPT_GUID_STR_LEN])
{
	gpt_guid_format_with(g, out, "0123456789abcdef");
}

void gpt_guid_format_upper(const struct gpt_guid *g,
			   char out[GPT_GUID_STR_LEN])
{
	gpt_guid_format_with(g, out, "0123456789ABCDEF");
}

int gpt_random_bytes(void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t got = 0;

	while (got < n) {
		ssize_t r = getrandom(p + got, n - got, 0);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	if (got < n) {
		int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

		if (fd < 0)
			return -(errno ? errno : EIO);
		while (got < n) {
			ssize_t r = read(fd, p + got, n - got);

			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0) {
				close(fd);
				return -EIO;
			}
			got += (size_t)r;
		}
		close(fd);
	}
	return 0;
}

int gpt_guid_random(struct gpt_guid *out)
{
	struct gpt_guid g;
	int rc;

	if (!out)
		return -EINVAL;
	do {
		rc = gpt_random_bytes(g.b, sizeof(g.b));
		if (rc)
			return rc;
		g.b[6] = (uint8_t)((g.b[6] & 0x0f) | 0x40); /* version 4 */
		g.b[8] = (uint8_t)((g.b[8] & 0x3f) | 0x80); /* variant 10 */
	} while (gpt_guid_is_zero(&g));
	*out = g;
	return 0;
}

int gpt_guid_is_zero(const struct gpt_guid *g)
{
	for (int i = 0; i < 16; i++)
		if (g->b[i])
			return 0;
	return 1;
}

int gpt_guid_equal(const struct gpt_guid *a, const struct gpt_guid *b)
{
	return memcmp(a->b, b->b, 16) == 0;
}
