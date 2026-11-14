/* libinstaller -- SHA-256 (FIPS 180-4).
 *
 * The system image is checked against its .sha256 file while it is being
 * unpacked, so the hash runs over the compressed stream as it is read; a
 * second pass over a multi-gigabyte archive would cost minutes.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "il_internal.h"

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ror(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static void il_sha256_block(struct il_sha256 *s, const uint8_t *p)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		       (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^
			      (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^
			      (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0];
	b = s->h[1];
	c = s->h[2];
	d = s->h[3];
	e = s->h[4];
	f = s->h[5];
	g = s->h[6];
	h = s->h[7];
	for (i = 0; i < 64; i++) {
		uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + K[i] + w[i];
		uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
	s->h[5] += f;
	s->h[6] += g;
	s->h[7] += h;
}

void il_sha256_init(struct il_sha256 *s)
{
	static const uint32_t H0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(s->h, H0, sizeof(H0));
	s->len = 0;
	s->nbuf = 0;
}

void il_sha256_update(struct il_sha256 *s, const void *data, size_t len)
{
	const uint8_t *p = data;

	s->len += len;
	if (s->nbuf) {
		size_t take = 64 - s->nbuf;

		if (take > len)
			take = len;
		memcpy(s->buf + s->nbuf, p, take);
		s->nbuf += take;
		p += take;
		len -= take;
		if (s->nbuf == 64) {
			il_sha256_block(s, s->buf);
			s->nbuf = 0;
		}
	}
	while (len >= 64) {
		il_sha256_block(s, p);
		p += 64;
		len -= 64;
	}
	if (len) {
		memcpy(s->buf, p, len);
		s->nbuf = len;
	}
}

void il_sha256_final(struct il_sha256 *s, uint8_t digest[32])
{
	uint64_t bits = s->len * 8;
	uint8_t pad[72];
	size_t padlen = (s->nbuf < 56) ? 56 - s->nbuf : 120 - s->nbuf;

	memset(pad, 0, sizeof(pad));
	pad[0] = 0x80;
	for (int i = 0; i < 8; i++)
		pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
	{
		uint64_t keep = s->len;

		il_sha256_update(s, pad, padlen + 8);
		s->len = keep;
	}
	for (int i = 0; i < 8; i++) {
		digest[4 * i] = (uint8_t)(s->h[i] >> 24);
		digest[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		digest[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		digest[4 * i + 3] = (uint8_t)s->h[i];
	}
	memset(s->buf, 0, sizeof(s->buf));
}

void il_sha256_hex(const uint8_t digest[32], char out[65])
{
	il_hex(digest, 32, out);
}

int il_sha256_file(const char *path, char out[65],
		   void (*progress)(double fraction, void *user), void *user,
		   const volatile int *cancel)
{
	struct il_sha256 s;
	struct stat st;
	uint8_t digest[32];
	uint64_t done = 0;
	uint64_t last_report = 0;
	size_t bufsize = 1024 * 1024;
	uint8_t *buf;
	int fd, rc = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) != 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	buf = malloc(bufsize);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	il_sha256_init(&s);
	for (;;) {
		ssize_t n = read(fd, buf, bufsize);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		if (n == 0)
			break;
		il_sha256_update(&s, buf, (size_t)n);
		done += (uint64_t)n;
		if (cancel && *cancel) {
			rc = -ECANCELED;
			break;
		}
		if (progress && st.st_size > 0 &&
		    done - last_report >= 16ULL * 1024 * 1024) {
			last_report = done;
			progress((double)done / (double)st.st_size, user);
		}
	}
	free(buf);
	close(fd);
	if (rc)
		return rc;
	il_sha256_final(&s, digest);
	il_sha256_hex(digest, out);
	if (progress)
		progress(1.0, user);
	return 0;
}
