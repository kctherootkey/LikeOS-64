/* Unit tests of the tar format helpers (il_tar.c) and the ACL encoder
 * (il_acl.c): numbers, time stamps, base64, URL decoding, pax records,
 * sparse maps, header checksums, ACL values.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il_tar.h"

static int failures;
static int checks;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			__LINE__, #cond); \
	} \
} while (0)

static void test_numbers(void)
{
	int64_t v;
	uint64_t u;
	char f8[8], f12[12];

	CHECK(il_tar_number("0000755", 8, &v) == 0 && v == 0755);
	CHECK(il_tar_number("0000755 ", 8, &v) == 0 && v == 0755);
	CHECK(il_tar_number("   755\0\0", 8, &v) == 0 && v == 0755);
	CHECK(il_tar_number("\0\0\0\0\0\0\0", 8, &v) == 0 && v == 0);
	CHECK(il_tar_number("0007x55", 8, &v) == -EINVAL);
	CHECK(il_tar_number("14532073130", 12, &v) == 0 &&
	      v == 01453207313 * 8LL);
	/* base-256: 3000000000 */
	memset(f8, 0, sizeof(f8));
	f8[0] = (char)0x80;
	f8[4] = (char)0xb2;
	f8[5] = (char)0xd0;
	f8[6] = (char)0x5e;
	f8[7] = (char)0x00;
	CHECK(il_tar_unumber(f8, 8, &u) == 0 && u == 3000000000ULL);
	/* base-256 negative: -2 */
	memset(f12, 0xff, sizeof(f12));
	f12[11] = (char)0xfe;
	CHECK(il_tar_number(f12, 12, &v) == 0 && v == -2);
	CHECK(il_tar_unumber(f12, 12, &u) == -EINVAL);
	/* base-256 with a bad marker byte */
	f8[0] = (char)0x81;
	CHECK(il_tar_number(f8, 8, &v) == -EINVAL);
}

static void test_time(void)
{
	int64_t s;
	long ns;

	CHECK(il_tar_time("1700000000", 10, &s, &ns) == 0 && s == 1700000000 &&
	      ns == 0);
	CHECK(il_tar_time("1700000000.123456789", 20, &s, &ns) == 0 &&
	      s == 1700000000 && ns == 123456789);
	CHECK(il_tar_time("1.5", 3, &s, &ns) == 0 && s == 1 &&
	      ns == 500000000);
	CHECK(il_tar_time("1.1234567891234", 15, &s, &ns) == 0 && s == 1 &&
	      ns == 123456789);
	CHECK(il_tar_time("-1.25", 5, &s, &ns) == 0 && s == -2 &&
	      ns == 750000000);
	CHECK(il_tar_time("-3", 2, &s, &ns) == 0 && s == -3 && ns == 0);
	CHECK(il_tar_time("12a", 3, &s, &ns) == -EINVAL);
	CHECK(il_tar_time("", 0, &s, &ns) == -EINVAL);
}

static void test_base64(void)
{
	uint8_t out[64];
	long n;

	n = il_base64_decode("aGVsbG8=", 8, out);
	CHECK(n == 5 && memcmp(out, "hello", 5) == 0);
	n = il_base64_decode("aGVsbG8", 7, out);
	CHECK(n == 5 && memcmp(out, "hello", 5) == 0);
	n = il_base64_decode("AAEC/w==", 8, out);
	CHECK(n == 4 && out[0] == 0 && out[1] == 1 && out[2] == 2 &&
	      out[3] == 0xff);
	CHECK(il_base64_decode("a$==", 4, out) == -EINVAL);
	CHECK(il_base64_decode("", 0, out) == 0);
}

static void test_url(void)
{
	char *s = il_url_decode("user.a%3Db%25", 13);

	CHECK(s && strcmp(s, "user.a=b%") == 0);
	free(s);
	CHECK(il_url_decode("bad%2", 5) == NULL);
	s = il_xattr_keyword_decode("user.a%3Db%25c%41%", 18);
	CHECK(s && strcmp(s, "user.a=b%c%41%") == 0);
	free(s);
	CHECK(il_url_decode("nul%00", 6) == NULL);
}

/* Appends one pax record "len key=value\n" (len counts itself). */
static size_t rec(char *buf, size_t pos, const char *key, const char *val,
		  size_t vlen)
{
	size_t body = 1 + strlen(key) + 1 + vlen + 1;  /* " k=v\n" */
	size_t len = body + 1;
	char num[24];

	while ((size_t)snprintf(num, sizeof(num), "%zu", len) + body != len)
		len = (size_t)snprintf(num, sizeof(num), "%zu", len) + body;
	pos += (size_t)sprintf(buf + pos, "%s %s=", num, key);
	memcpy(buf + pos, val, vlen);
	pos += vlen;
	buf[pos++] = '\n';
	return pos;
}

static void test_pax(void)
{
	struct il_tar_pax p;
	char err[128];
	char rec_buf[1024];
	size_t L = 0;
	int rc;

	L = rec(rec_buf, L, "mtime", "1700000000.123456789", 20);
	L = rec(rec_buf, L, "path", "dir/long/name.c", 15);
	L = rec(rec_buf, L, "uid", "1000", 4);
	L = rec(rec_buf, L, "SCHILY.xattr.user.k", "v", 1);
	L = rec(rec_buf, L, "LIBARCHIVE.xattr.user.b%20c", "AAEC", 4);
	L = rec(rec_buf, L, "GNU.sparse.major", "1", 1);
	L = rec(rec_buf, L, "GNU.sparse.minor", "0", 1);
	L = rec(rec_buf, L, "GNU.sparse.realsize", "99", 2);
	L = rec(rec_buf, L, "SCHILY.acl.access",
		"user::rw-,group::r--,other::r--", 31);

	il_tar_pax_init(&p);
	rc = il_tar_pax_parse(&p, rec_buf, L, err, sizeof(err));
	if (rc)
		fprintf(stderr, "pax: %s\n", err);
	CHECK(rc == 0);
	CHECK((p.have & IL_PAX_MTIME) && p.mtime_sec == 1700000000 &&
	      p.mtime_nsec == 123456789);
	CHECK((p.have & IL_PAX_PATH) && strcmp(p.path, "dir/long/name.c") == 0);
	CHECK((p.have & IL_PAX_UID) && p.uid == 1000);
	CHECK(p.nxattrs == 2);
	CHECK(p.nxattrs == 2 && strcmp(p.xattrs[0].name, "user.k") == 0 &&
	      p.xattrs[0].len == 1 && p.xattrs[0].value[0] == 'v');
	CHECK(p.nxattrs == 2 && strcmp(p.xattrs[1].name, "user.b c") == 0 &&
	      p.xattrs[1].len == 3 && p.xattrs[1].value[2] == 2);
	CHECK(p.sp_major == 1 && p.sp_minor == 0 && p.sp_realsize == 99);
	CHECK(p.acl_access && strstr(p.acl_access, "group::r--"));
	il_tar_pax_free(&p);

	/* bad length */
	il_tar_pax_init(&p);
	CHECK(il_tar_pax_parse(&p, "99 path=x\n", 10, err, sizeof(err)) ==
	      -EINVAL);
	il_tar_pax_free(&p);
	/* a binary value with a newline inside */
	{
		const char bin[] = "21 SCHILY.xattr.user.n=\n\n";

		il_tar_pax_init(&p);
		CHECK(il_tar_pax_parse(&p, bin, sizeof(bin) - 1, err,
				       sizeof(err)) == -EINVAL);
		il_tar_pax_free(&p);
	}
	{
		char bin[64];
		size_t bl = rec(bin, 0, "SCHILY.xattr.user.n", "\n", 1);

		il_tar_pax_init(&p);
		CHECK(il_tar_pax_parse(&p, bin, bl, err, sizeof(err)) == 0);
		CHECK(p.nxattrs == 1 && p.xattrs[0].len == 1 &&
		      p.xattrs[0].value[0] == '\n');
		il_tar_pax_free(&p);
	}
	/* 0.0 sparse records and the 0.1 map */
	{
		char s0[256];
		size_t sl = 0;

		sl = rec(s0, sl, "GNU.sparse.offset", "0", 1);
		sl = rec(s0, sl, "GNU.sparse.numbytes", "5", 1);
		sl = rec(s0, sl, "GNU.sparse.offset", "100", 3);
		sl = rec(s0, sl, "GNU.sparse.numbytes", "7", 1);
		sl = rec(s0, sl, "GNU.sparse.map", "1,2,30,40", 9);
		il_tar_pax_init(&p);
		CHECK(il_tar_pax_parse(&p, s0, sl, err, sizeof(err)) == 0);
		CHECK(p.nmap == 4 && p.map[1].offset == 100 &&
		      p.map[1].len == 7 && p.map[3].offset == 30 &&
		      p.map[3].len == 40);
		il_tar_pax_free(&p);
	}
}

static void test_checksum(void)
{
	uint8_t blk[512];
	struct il_tar_hdr_raw *h = (void *)blk;

	memset(blk, 0, sizeof(blk));
	strcpy(h->name, "file.txt");
	memcpy(h->mode, "0000644", 8);
	memcpy(h->magic, "ustar", 6);
	memcpy(h->version, "00", 2);
	h->typeflag = '0';
	il_tar_checksum_set(blk);
	CHECK(il_tar_checksum_ok(blk));
	CHECK(il_tar_magic(h) == IL_TAR_MAGIC_USTAR);
	blk[0] ^= 1;
	CHECK(!il_tar_checksum_ok(blk));
	blk[0] ^= 1;
	/* a high byte: the signed sum differs from the unsigned one */
	h->name[50] = (char)0xe4;
	il_tar_checksum_set(blk);
	CHECK(il_tar_checksum_ok(blk));
	memset(blk, 0, sizeof(blk));
	CHECK(il_tar_is_zero_block(blk));
}

static uint16_t g16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t g32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static void test_acl(void)
{
	uint8_t *b;
	size_t n;
	int base;
	char err[128];

	CHECK(il_acl_from_text("user::rwx,user:1000:r-x,group::r-x,"
			       "mask::r-x,other::---", &b, &n, &base, err,
			       sizeof(err)) == 0);
	CHECK(n == 4 + 5 * 8 && !base);
	if (b && n == 44) {
		CHECK(g32(b) == 2);
		CHECK(g16(b + 4) == 0x01 && g16(b + 6) == 7 &&
		      g32(b + 8) == 0xffffffffu);
		CHECK(g16(b + 12) == 0x02 && g16(b + 14) == 5 &&
		      g32(b + 16) == 1000);
		CHECK(g16(b + 20) == 0x04 && g16(b + 22) == 5);
		CHECK(g16(b + 28) == 0x10 && g16(b + 30) == 5);
		CHECK(g16(b + 36) == 0x20 && g16(b + 38) == 0);
	}
	free(b);
	/* short tags, line ends, comments, the name:perm:id form, no mask */
	CHECK(il_acl_from_text("u::rw-\ng:wheel:r--:10 # comment\n"
			       "g::r--\no::r--\n", &b, &n, &base, err,
			       sizeof(err)) == 0);
	CHECK(n == 4 + 5 * 8 && !base);
	if (b && n == 44) {
		CHECK(g16(b + 12) == 0x04);
		CHECK(g16(b + 20) == 0x08 && g32(b + 24) == 10);
		CHECK(g16(b + 28) == 0x10 && g16(b + 30) == 4);
	}
	free(b);
	/* only the base entries */
	CHECK(il_acl_from_text("user::rw-,group::r--,other::r--", &b, &n,
			       &base, err, sizeof(err)) == 0);
	CHECK(base && n == 28);
	free(b);
	/* errors */
	CHECK(il_acl_from_text("user::rw-,group::r--", &b, &n, &base, err,
			       sizeof(err)) == -EINVAL);
	CHECK(il_acl_from_text("user:bob:rw-,user::rw-,group::r--,other::---",
			       &b, &n, &base, err, sizeof(err)) == -EINVAL);
	CHECK(il_acl_from_text("user::rwz,group::r--,other::---", &b, &n,
			       &base, err, sizeof(err)) == -EINVAL);
	CHECK(il_acl_from_text("", &b, &n, &base, err, sizeof(err)) == 0 &&
	      !b && base);
	/* As many entries as allowed, named ones without a mask: the mask
	 * that is added must still fit. */
	{
		size_t cap = 64 * 1024, o = 0;
		char *t = malloc(cap);

		CHECK(t != NULL);
		if (t) {
			o += (size_t)snprintf(t + o, cap - o,
					      "user::rwx,group::r-x,other::---");
			for (int i = 0; i < 1021; i++)
				o += (size_t)snprintf(t + o, cap - o,
						      ",user:%d:r--", 2000 + i);
			b = NULL;
			CHECK(il_acl_from_text(t, &b, &n, &base, err,
					       sizeof(err)) == 0);
			CHECK(b && n == 4 + 1025 * 8 && !base);
			free(b);
			free(t);
		}
	}
}

int main(void)
{
	test_numbers();
	test_time();
	test_base64();
	test_url();
	test_pax();
	test_checksum();
	test_acl();
	printf("tar_unit_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
