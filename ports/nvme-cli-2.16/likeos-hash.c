/*
 * likeos-hash.c - SHA-256, HMAC-SHA256 and MD5 for nvme-cli's RPMB commands.
 *
 * See likeos-hash.h.  Straightforward one-shot implementations: the inputs
 * are RPMB frames (a few hundred bytes) and 4-byte nonce seeds, so there is
 * no streaming interface.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <string.h>

#include "likeos-hash.h"

/* ---- SHA-256 (FIPS 180-4) --------------------------------------------- */

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

struct sha256_ctx {
	uint32_t h[8];
	uint8_t buf[64];
	size_t used;
	uint64_t total;
};

static uint32_t ror32(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct sha256_ctx *c, const uint8_t *p)
{
	uint32_t w[64], a, b, d, e, f, g, h, cc;

	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		       (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
		uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
		uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
		uint32_t t2 = S0 + maj;

		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(struct sha256_ctx *c)
{
	static const uint32_t h0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(c->h, h0, sizeof(h0));
	c->used = 0;
	c->total = 0;
}

static void sha256_update(struct sha256_ctx *c, const void *data, size_t len)
{
	const uint8_t *p = data;

	c->total += len;
	while (len) {
		size_t n = 64 - c->used;

		if (n > len)
			n = len;
		memcpy(c->buf + c->used, p, n);
		c->used += n;
		p += n;
		len -= n;
		if (c->used == 64) {
			sha256_block(c, c->buf);
			c->used = 0;
		}
	}
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
	uint64_t bits = c->total * 8;
	uint8_t pad = 0x80, zero = 0, len[8];

	sha256_update(c, &pad, 1);
	while (c->used != 56)
		sha256_update(c, &zero, 1);
	for (int i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_update(c, len, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

void likeos_sha256(const void *data, size_t len, uint8_t out[LIKEOS_SHA256_SIZE])
{
	struct sha256_ctx c;

	sha256_init(&c);
	sha256_update(&c, data, len);
	sha256_final(&c, out);
}

/* ---- HMAC-SHA256 (RFC 2104) ------------------------------------------- */

void likeos_hmac_sha256(const void *key, size_t keylen, const void *data,
			size_t len, uint8_t out[LIKEOS_SHA256_SIZE])
{
	uint8_t k[64], ipad[64], opad[64], inner[32];
	struct sha256_ctx c;

	memset(k, 0, sizeof(k));
	if (keylen > 64)
		likeos_sha256(key, keylen, k);
	else if (keylen)
		memcpy(k, key, keylen);
	for (int i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	sha256_init(&c);
	sha256_update(&c, ipad, 64);
	sha256_update(&c, data, len);
	sha256_final(&c, inner);
	sha256_init(&c);
	sha256_update(&c, opad, 64);
	sha256_update(&c, inner, 32);
	sha256_final(&c, out);
}

/* ---- MD5 (RFC 1321) ---------------------------------------------------- */

static const uint32_t md5_t[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
	0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
	0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
	0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
	0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
	0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
	0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
	0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
	0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t md5_s[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(uint32_t h[4], const uint8_t *p)
{
	uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];

	for (int i = 0; i < 16; i++)
		m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 |
		       (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
	for (int i = 0; i < 64; i++) {
		uint32_t f;
		int g;

		if (i < 16) {
			f = (b & c) | (~b & d);
			g = i;
		} else if (i < 32) {
			f = (d & b) | (~d & c);
			g = (5 * i + 1) & 15;
		} else if (i < 48) {
			f = b ^ c ^ d;
			g = (3 * i + 5) & 15;
		} else {
			f = c ^ (b | ~d);
			g = (7 * i) & 15;
		}
		f += a + md5_t[i] + m[g];
		a = d;
		d = c;
		c = b;
		b += (f << md5_s[i]) | (f >> (32 - md5_s[i]));
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

void likeos_md5(const void *data, size_t len, uint8_t out[LIKEOS_MD5_SIZE])
{
	uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
	const uint8_t *p = data;
	uint8_t tail[128];
	size_t rest, tlen;
	uint64_t bits = (uint64_t)len * 8;

	while (len >= 64) {
		md5_block(h, p);
		p += 64;
		len -= 64;
	}
	rest = len;
	memset(tail, 0, sizeof(tail));
	memcpy(tail, p, rest);
	tail[rest] = 0x80;
	tlen = rest + 1 + 8 <= 64 ? 64 : 128;
	for (int i = 0; i < 8; i++)
		tail[tlen - 8 + i] = (uint8_t)(bits >> (8 * i));
	md5_block(h, tail);
	if (tlen == 128)
		md5_block(h, tail + 64);
	for (int i = 0; i < 4; i++) {
		out[4 * i] = (uint8_t)h[i];
		out[4 * i + 1] = (uint8_t)(h[i] >> 8);
		out[4 * i + 2] = (uint8_t)(h[i] >> 16);
		out[4 * i + 3] = (uint8_t)(h[i] >> 24);
	}
}
