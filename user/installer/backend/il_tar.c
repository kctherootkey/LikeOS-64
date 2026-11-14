/* libinstaller -- the tar format: headers, numbers, pax records.
 *
 * See il_tar.h for what lives here.  Nothing in this file touches the file
 * system; every function works on memory the extractor hands it.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il_tar.h"

_Static_assert(sizeof(struct il_tar_hdr_raw) == IL_TAR_BLOCK,
	       "a tar header is one 512-byte block");
_Static_assert(sizeof(struct il_tar_sparse_ext) == IL_TAR_BLOCK,
	       "a sparse extension header is one 512-byte block");

/* Records a pax header may carry before it is taken as damaged. */
#define IL_PAX_MAX_RECORDS  100000
#define IL_PAX_MAX_SPARSE   (1u << 22)

/* ---- blocks ------------------------------------------------------- */

int il_tar_is_zero_block(const uint8_t *blk)
{
	const uint64_t *w = (const uint64_t *)(const void *)blk;

	for (unsigned i = 0; i < IL_TAR_BLOCK / 8; i++)
		if (w[i])
			return 0;
	return 1;
}

int il_tar_checksum_ok(const uint8_t *blk)
{
	const struct il_tar_hdr_raw *h = (const void *)blk;
	uint64_t stored;
	uint64_t usum = 0;
	int64_t ssum = 0;

	if (il_tar_unumber(h->chksum, sizeof(h->chksum), &stored) != 0)
		return 0;
	for (unsigned i = 0; i < IL_TAR_BLOCK; i++) {
		uint8_t c = (i >= 148 && i < 156) ? (uint8_t)' ' : blk[i];

		usum += c;
		ssum += (int8_t)c;
	}
	if (stored == usum)
		return 1;
	/* Some early implementations summed signed chars. */
	return (int64_t)stored == ssum;
}

void il_tar_checksum_set(uint8_t *blk)
{
	struct il_tar_hdr_raw *h = (void *)blk;
	unsigned sum = 0;

	memset(h->chksum, ' ', sizeof(h->chksum));
	for (unsigned i = 0; i < IL_TAR_BLOCK; i++)
		sum += blk[i];
	snprintf(h->chksum, sizeof(h->chksum), "%06o", sum & 0777777u);
	h->chksum[7] = ' ';
}

int il_tar_magic(const struct il_tar_hdr_raw *h)
{
	if (memcmp(h->magic, "ustar", 5) == 0 && h->magic[5] == '\0')
		return IL_TAR_MAGIC_USTAR;
	if (memcmp(h->magic, "ustar ", 6) == 0)
		return IL_TAR_MAGIC_GNU;
	return IL_TAR_MAGIC_V7;
}

/* ---- numbers ------------------------------------------------------ */

int il_tar_number(const char *field, size_t len, int64_t *out)
{
	const uint8_t *f = (const uint8_t *)field;
	size_t i = 0;
	uint64_t v = 0;

	if (len == 0) {
		*out = 0;
		return 0;
	}
	if (f[0] & 0x80) {
		/* Base-256: big-endian two's complement, the top bit of the
		 * first byte being the marker (0x80 positive, 0xff negative). */
		int neg = f[0] == 0xff;

		if (!neg && f[0] != 0x80)
			return -EINVAL;
		/* Bytes that would not fit in 64 bits must be sign fill. */
		for (i = 1; i + 8 < len; i++)
			if (f[i] != (neg ? 0xff : 0x00))
				return -EINVAL;
		v = neg ? ~0ULL : 0;
		for (; i < len; i++)
			v = (v << 8) | f[i];
		if (!neg && (int64_t)v < 0)
			return -EINVAL;
		*out = (int64_t)v;
		return 0;
	}
	while (i < len && (f[i] == ' ' || f[i] == '\0'))
		i++;
	if (i == len) {
		*out = 0;
		return 0;
	}
	for (; i < len && f[i] >= '0' && f[i] <= '7'; i++) {
		if (v >> 60)
			return -EINVAL;
		v = (v << 3) | (uint64_t)(f[i] - '0');
	}
	/* What follows the digits must be blanks or NULs. */
	for (; i < len; i++)
		if (f[i] != ' ' && f[i] != '\0')
			return -EINVAL;
	*out = (int64_t)v;
	return 0;
}

int il_tar_unumber(const char *field, size_t len, uint64_t *out)
{
	int64_t v;
	int rc = il_tar_number(field, len, &v);

	if (rc)
		return rc;
	if (v < 0)
		return -EINVAL;
	*out = (uint64_t)v;
	return 0;
}

/* A decimal number of a pax record (no sign). */
static int il_dec_u64(const char *s, size_t len, uint64_t *out)
{
	uint64_t v = 0;
	size_t i = 0;

	if (len == 0)
		return -EINVAL;
	for (; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return -EINVAL;
		if (v > (UINT64_MAX - 9) / 10)
			return -EINVAL;
		v = v * 10 + (uint64_t)(s[i] - '0');
	}
	*out = v;
	return 0;
}

int il_tar_time(const char *s, size_t len, int64_t *sec, long *nsec)
{
	int neg = 0;
	size_t i = 0, dot;
	uint64_t whole;
	long frac = 0;
	int digits = 0;

	if (len && (s[0] == '-' || s[0] == '+')) {
		neg = s[0] == '-';
		i = 1;
	}
	for (dot = i; dot < len && s[dot] != '.'; dot++)
		;
	if (dot == i) {
		if (dot >= len)
			return -EINVAL;
		whole = 0;
	} else if (il_dec_u64(s + i, dot - i, &whole) != 0) {
		return -EINVAL;
	}
	if (whole > (uint64_t)INT64_MAX - 1)
		return -EINVAL;
	if (dot < len) {
		for (size_t k = dot + 1; k < len; k++) {
			if (s[k] < '0' || s[k] > '9')
				return -EINVAL;
			if (digits < 9) {
				frac = frac * 10 + (s[k] - '0');
				digits++;
			}
		}
	}
	while (digits < 9) {
		frac *= 10;
		digits++;
	}
	if (!neg) {
		*sec = (int64_t)whole;
		*nsec = frac;
	} else if (frac == 0) {
		*sec = -(int64_t)whole;
		*nsec = 0;
	} else {
		/* -1.25 is 2 seconds before the epoch plus 0.75. */
		*sec = -(int64_t)whole - 1;
		*nsec = 1000000000L - frac;
	}
	return 0;
}

char *il_tar_field_dup(const char *field, size_t len)
{
	size_t n = strnlen(field, len);
	char *s = malloc(n + 1);

	if (!s)
		return NULL;
	memcpy(s, field, n);
	s[n] = '\0';
	return s;
}

/* ---- decoding helpers ------------------------------------------- */

static int il_b64_value(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+' || c == '-')
		return 62;
	if (c == '/' || c == '_')
		return 63;
	return -1;
}

long il_base64_decode(const char *in, size_t len, uint8_t *out)
{
	uint32_t acc = 0;
	int bits = 0;
	long n = 0;
	int pad = 0;

	for (size_t i = 0; i < len; i++) {
		int c = (unsigned char)in[i];
		int v;

		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			continue;
		if (c == '=') {
			pad++;
			continue;
		}
		if (pad)
			return -EINVAL;  /* data after the padding */
		v = il_b64_value(c);
		if (v < 0)
			return -EINVAL;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[n++] = (uint8_t)(acc >> bits);
		}
	}
	if (bits >= 6)
		return -EINVAL;  /* a lone sextet carries no byte */
	return n;
}

static int il_hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

char *il_url_decode(const char *in, size_t len)
{
	char *out = malloc(len + 1);
	size_t o = 0;

	if (!out)
		return NULL;
	for (size_t i = 0; i < len; i++) {
		if (in[i] == '%') {
			int hi, lo;

			if (i + 2 >= len) {
				free(out);
				return NULL;
			}
			hi = il_hexval((unsigned char)in[i + 1]);
			lo = il_hexval((unsigned char)in[i + 2]);
			if (hi < 0 || lo < 0) {
				free(out);
				return NULL;
			}
			out[o++] = (char)(hi << 4 | lo);
			i += 2;
		} else {
			out[o++] = in[i];
		}
	}
	out[o] = '\0';
	if (strlen(out) != o) {
		/* An attribute name never holds a NUL. */
		free(out);
		return NULL;
	}
	return out;
}

/* ---- pax ----------------------------------------------------------- */

void il_tar_pax_init(struct il_tar_pax *p)
{
	memset(p, 0, sizeof(*p));
}

void il_tar_pax_free(struct il_tar_pax *p)
{
	free(p->path);
	free(p->linkpath);
	free(p->sp_name);
	free(p->acl_access);
	free(p->acl_default);
	for (size_t i = 0; i < p->nxattrs; i++) {
		free(p->xattrs[i].name);
		free(p->xattrs[i].value);
	}
	free(p->xattrs);
	free(p->map);
	memset(p, 0, sizeof(*p));
}

int il_tar_pax_add_xattr(struct il_tar_pax *p, const char *name,
			 const uint8_t *value, size_t len)
{
	uint8_t *v;
	size_t i;

	if (!name[0])
		return -EINVAL;
	v = malloc(len ? len : 1);
	if (!v)
		return -ENOMEM;
	if (len)
		memcpy(v, value, len);
	for (i = 0; i < p->nxattrs; i++) {
		if (strcmp(p->xattrs[i].name, name) == 0) {
			free(p->xattrs[i].value);
			p->xattrs[i].value = v;
			p->xattrs[i].len = len;
			return 0;
		}
	}
	if (p->nxattrs == p->xattr_cap) {
		size_t cap = p->xattr_cap ? p->xattr_cap * 2 : 8;
		struct il_tar_xattr *nx = realloc(p->xattrs, cap * sizeof(*nx));

		if (!nx) {
			free(v);
			return -ENOMEM;
		}
		p->xattrs = nx;
		p->xattr_cap = cap;
	}
	p->xattrs[p->nxattrs].name = strdup(name);
	if (!p->xattrs[p->nxattrs].name) {
		free(v);
		return -ENOMEM;
	}
	p->xattrs[p->nxattrs].value = v;
	p->xattrs[p->nxattrs].len = len;
	p->nxattrs++;
	return 0;
}

int il_tar_pax_add_sparse(struct il_tar_pax *p, uint64_t off, uint64_t len)
{
	if (p->nmap >= IL_PAX_MAX_SPARSE)
		return -E2BIG;
	if (p->nmap == p->map_cap) {
		size_t cap = p->map_cap ? p->map_cap * 2 : 16;
		struct il_tar_sparse *nm = realloc(p->map, cap * sizeof(*nm));

		if (!nm)
			return -ENOMEM;
		p->map = nm;
		p->map_cap = cap;
	}
	p->map[p->nmap].offset = off;
	p->map[p->nmap].len = len;
	p->nmap++;
	p->have |= IL_PAX_SP_MAP;
	return 0;
}

static int il_set_str(char **dst, const char *v, size_t len)
{
	char *s = malloc(len + 1);

	if (!s)
		return -ENOMEM;
	memcpy(s, v, len);
	s[len] = '\0';
	free(*dst);
	*dst = s;
	return 0;
}

static void il_copy_name(char *dst, size_t cap, const char *v, size_t len)
{
	size_t n = len < cap - 1 ? len : cap - 1;

	memcpy(dst, v, n);
	dst[n] = '\0';
}

int il_tar_sparse_parse_list(const char *s, struct il_tar_pax *p)
{
	const char *q = s;
	int rc;

	while (*q) {
		uint64_t off, len;
		const char *c = strchr(q, ',');

		if (!c || il_dec_u64(q, (size_t)(c - q), &off) != 0)
			return -EINVAL;
		q = c + 1;
		c = strchr(q, ',');
		if (!c)
			c = q + strlen(q);
		if (il_dec_u64(q, (size_t)(c - q), &len) != 0)
			return -EINVAL;
		rc = il_tar_pax_add_sparse(p, off, len);
		if (rc)
			return rc;
		q = *c ? c + 1 : c;
	}
	return 0;
}

int il_tar_sparse_parse_old(const char (*pairs)[24], int n,
			    struct il_tar_pax *p)
{
	for (int i = 0; i < n; i++) {
		uint64_t off, len;
		int rc;

		if (pairs[i][0] == '\0' && pairs[i][12] == '\0')
			break;
		if (il_tar_unumber(pairs[i], 12, &off) != 0 ||
		    il_tar_unumber(pairs[i] + 12, 12, &len) != 0)
			return -EINVAL;
		rc = il_tar_pax_add_sparse(p, off, len);
		if (rc)
			return rc;
	}
	return 0;
}

/* An attribute name of a SCHILY.xattr record.  A pax keyword cannot hold
 * '=', so GNU tar writes '=' as "%3D" and, to keep that unambiguous, '%' as
 * "%25"; no other sequence is encoded (star writes names as they are, and
 * a '%' followed by anything else stays). */
char *il_xattr_keyword_decode(const char *in, size_t len)
{
	char *out = malloc(len + 1);
	size_t o = 0;

	if (!out)
		return NULL;
	for (size_t i = 0; i < len; i++) {
		if (in[i] == '%' && i + 2 < len) {
			if (in[i + 1] == '3' && (in[i + 2] == 'D' ||
						 in[i + 2] == 'd')) {
				out[o++] = '=';
				i += 2;
				continue;
			}
			if (in[i + 1] == '2' && in[i + 2] == '5') {
				out[o++] = '%';
				i += 2;
				continue;
			}
		}
		out[o++] = in[i];
	}
	out[o] = '\0';
	return out;
}

#define KEYIS(lit) (klen == sizeof(lit) - 1 && memcmp(key, lit, klen) == 0)
#define KEYPFX(lit) (klen > sizeof(lit) - 1 && \
		     memcmp(key, lit, sizeof(lit) - 1) == 0)

/* One record: key[klen] = val[vlen]. */
static int il_pax_record(struct il_tar_pax *p, const char *key, size_t klen,
			 const char *val, size_t vlen, char *err,
			 size_t errlen)
{
	uint64_t u;
	int rc = 0;

	if (KEYIS("path")) {
		if (vlen == 0) {
			p->have &= ~IL_PAX_PATH;
			return 0;
		}
		rc = il_set_str(&p->path, val, vlen);
		if (!rc)
			p->have |= IL_PAX_PATH;
	} else if (KEYIS("linkpath")) {
		if (vlen == 0) {
			p->have &= ~IL_PAX_LINKPATH;
			return 0;
		}
		rc = il_set_str(&p->linkpath, val, vlen);
		if (!rc)
			p->have |= IL_PAX_LINKPATH;
	} else if (KEYIS("size")) {
		if (il_dec_u64(val, vlen, &p->size) != 0)
			goto bad;
		p->have |= IL_PAX_SIZE;
	} else if (KEYIS("uid")) {
		if (il_dec_u64(val, vlen, &p->uid) != 0)
			goto bad;
		p->have |= IL_PAX_UID;
	} else if (KEYIS("gid")) {
		if (il_dec_u64(val, vlen, &p->gid) != 0)
			goto bad;
		p->have |= IL_PAX_GID;
	} else if (KEYIS("uname")) {
		il_copy_name(p->uname, sizeof(p->uname), val, vlen);
		p->have |= IL_PAX_UNAME;
	} else if (KEYIS("gname")) {
		il_copy_name(p->gname, sizeof(p->gname), val, vlen);
		p->have |= IL_PAX_GNAME;
	} else if (KEYIS("mtime")) {
		if (il_tar_time(val, vlen, &p->mtime_sec, &p->mtime_nsec) != 0)
			goto bad;
		p->have |= IL_PAX_MTIME;
	} else if (KEYIS("atime")) {
		if (il_tar_time(val, vlen, &p->atime_sec, &p->atime_nsec) != 0)
			goto bad;
		p->have |= IL_PAX_ATIME;
	} else if (KEYIS("ctime") || KEYIS("charset") || KEYIS("comment") ||
		   KEYIS("hdrcharset") || KEYIS("SCHILY.fflags") ||
		   KEYIS("SCHILY.dev") || KEYIS("SCHILY.ino") ||
		   KEYIS("SCHILY.nlink") || KEYIS("SCHILY.acl.ace") ||
		   KEYIS("LIBARCHIVE.creationtime") ||
		   KEYIS("LIBARCHIVE.symlinktype")) {
		/* Nothing a restored file system object needs. */
	} else if (KEYIS("SCHILY.devmajor")) {
		if (il_dec_u64(val, vlen, &p->devmajor) != 0)
			goto bad;
		p->have |= IL_PAX_DEVMAJOR;
	} else if (KEYIS("SCHILY.devminor")) {
		if (il_dec_u64(val, vlen, &p->devminor) != 0)
			goto bad;
		p->have |= IL_PAX_DEVMINOR;
	} else if (KEYIS("SCHILY.filetype")) {
		il_copy_name(p->filetype, sizeof(p->filetype), val, vlen);
		p->have |= IL_PAX_FILETYPE;
	} else if (KEYIS("SCHILY.acl.access")) {
		rc = il_set_str(&p->acl_access, val, vlen);
		if (!rc)
			p->have |= IL_PAX_ACL_ACC;
	} else if (KEYIS("SCHILY.acl.default")) {
		rc = il_set_str(&p->acl_default, val, vlen);
		if (!rc)
			p->have |= IL_PAX_ACL_DEF;
	} else if (KEYPFX("SCHILY.xattr.")) {
		char *name = il_xattr_keyword_decode(key + 13, klen - 13);

		if (!name)
			return -ENOMEM;
		rc = il_tar_pax_add_xattr(p, name, (const uint8_t *)val, vlen);
		free(name);
		if (rc == -EINVAL)
			goto bad;
	} else if (KEYPFX("LIBARCHIVE.xattr.")) {
		char *name = il_url_decode(key + 17, klen - 17);
		uint8_t *bin;
		long n;

		if (!name)
			goto bad;
		bin = malloc(vlen / 4 * 3 + 4);
		if (!bin) {
			free(name);
			return -ENOMEM;
		}
		n = il_base64_decode(val, vlen, bin);
		if (n < 0) {
			free(name);
			free(bin);
			goto bad;
		}
		rc = il_tar_pax_add_xattr(p, name, bin, (size_t)n);
		free(name);
		free(bin);
		if (rc == -EINVAL)
			goto bad;
	} else if (KEYIS("GNU.sparse.major")) {
		if (il_dec_u64(val, vlen, &u) != 0 || u > 100)
			goto bad;
		p->sp_major = (int)u;
		p->have |= IL_PAX_SP_MAJOR;
	} else if (KEYIS("GNU.sparse.minor")) {
		if (il_dec_u64(val, vlen, &u) != 0 || u > 100)
			goto bad;
		p->sp_minor = (int)u;
		p->have |= IL_PAX_SP_MINOR;
	} else if (KEYIS("GNU.sparse.name")) {
		rc = il_set_str(&p->sp_name, val, vlen);
		if (!rc)
			p->have |= IL_PAX_SP_NAME;
	} else if (KEYIS("GNU.sparse.realsize") || KEYIS("GNU.sparse.size")) {
		if (il_dec_u64(val, vlen, &p->sp_realsize) != 0)
			goto bad;
		p->have |= IL_PAX_SP_REAL;
	} else if (KEYIS("GNU.sparse.numblocks")) {
		if (il_dec_u64(val, vlen, &p->sp_numblocks) != 0)
			goto bad;
		p->have |= IL_PAX_SP_BLOCKS;
	} else if (KEYIS("GNU.sparse.offset")) {
		/* 0.0: offset and numbytes records alternate. */
		if (il_dec_u64(val, vlen, &p->map_pending_off) != 0)
			goto bad;
		p->map_pending = 1;
	} else if (KEYIS("GNU.sparse.numbytes")) {
		if (!p->map_pending || il_dec_u64(val, vlen, &u) != 0)
			goto bad;
		p->map_pending = 0;
		rc = il_tar_pax_add_sparse(p, p->map_pending_off, u);
		if (!rc) {
			/* 0.0 has no major/minor records of its own. */
			if (!(p->have & IL_PAX_SP_MAJOR)) {
				p->sp_major = 0;
				p->sp_minor = 0;
			}
		}
	} else if (KEYIS("GNU.sparse.map")) {
		char *s = malloc(vlen + 1);

		if (!s)
			return -ENOMEM;
		memcpy(s, val, vlen);
		s[vlen] = '\0';
		rc = il_tar_sparse_parse_list(s, p);
		free(s);
		if (rc == -EINVAL)
			goto bad;
	} else {
		p->unknown++;
	}
	return rc;
bad:
	snprintf(err, errlen, "bad pax record '%.*s'", (int)(klen > 64 ? 64 : klen),
		 key);
	return -EINVAL;
}

int il_tar_pax_parse(struct il_tar_pax *p, const char *data, size_t len,
		     char *err, size_t errlen)
{
	size_t pos = 0;
	unsigned records = 0;

	while (pos < len) {
		uint64_t rlen = 0;
		size_t i = pos, eq;
		int rc;

		/* A run of NULs pads the end of the block. */
		if (data[pos] == '\0')
			break;
		while (i < len && data[i] >= '0' && data[i] <= '9') {
			if (rlen > 100000000ULL)
				goto bad;
			rlen = rlen * 10 + (uint64_t)(data[i] - '0');
			i++;
		}
		if (i == pos || i >= len || data[i] != ' ' || rlen > len - pos ||
		    rlen < (uint64_t)(i - pos) + 3)
			goto bad;
		/* The record is data[pos .. pos + rlen), ending in '\n'. */
		if (data[pos + rlen - 1] != '\n')
			goto bad;
		i++;
		for (eq = i; eq < pos + rlen - 1 && data[eq] != '='; eq++)
			;
		if (eq >= pos + rlen - 1 || eq == i)
			goto bad;
		rc = il_pax_record(p, data + i, eq - i, data + eq + 1,
				   (size_t)(pos + rlen - 1 - (eq + 1)), err,
				   errlen);
		if (rc)
			return rc;
		pos += (size_t)rlen;
		if (++records > IL_PAX_MAX_RECORDS)
			goto bad;
	}
	return 0;
bad:
	snprintf(err, errlen, "malformed pax extended header (record at byte "
		 "%zu)", pos);
	return -EINVAL;
}
