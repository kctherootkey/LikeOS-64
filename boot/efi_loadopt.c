/* efi_loadopt -- UEFI device paths, boot load options and BOOTX64.CSV.
 *
 * See efi_loadopt.h.  Freestanding on purpose: no headers, no library
 * calls, every multi-byte field read and written byte by byte.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "efi_loadopt.h"

#define ELO_NULL ((void *)0)

/* ---- little-endian fields --------------------------------------------- */

static elo_u16 rd16(const elo_u8 *p)
{
	return (elo_u16)(p[0] | (p[1] << 8));
}

static elo_u32 rd32(const elo_u8 *p)
{
	return (elo_u32)p[0] | ((elo_u32)p[1] << 8) | ((elo_u32)p[2] << 16) |
	       ((elo_u32)p[3] << 24);
}

static elo_u64 rd64(const elo_u8 *p)
{
	return (elo_u64)rd32(p) | ((elo_u64)rd32(p + 4) << 32);
}

static void wr16(elo_u8 *p, elo_u16 v)
{
	p[0] = (elo_u8)v;
	p[1] = (elo_u8)(v >> 8);
}

static void wr32(elo_u8 *p, elo_u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (elo_u8)(v >> (8 * i));
}

static void wr64(elo_u8 *p, elo_u64 v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (elo_u8)(v >> (8 * i));
}

static void bytes_copy(elo_u8 *dst, const elo_u8 *src, elo_size n)
{
	/* Element by element on purpose (see the header): no library. */
	for (elo_size i = 0; i < n; i++)
		dst[i] = src[i];
}

static int bytes_equal(const elo_u8 *a, const elo_u8 *b, elo_size n)
{
	for (elo_size i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* ---- UTF-8 ------------------------------------------------------------ */

/* The next code point of a NUL-terminated UTF-8 string: 1 with *cp set and
 * *ps advanced, 0 at the end, -1 for an invalid sequence (overlong forms,
 * surrogates and values beyond U+10FFFF included). */
static int utf8_next(const char **ps, elo_u32 *cp)
{
	const elo_u8 *s = (const elo_u8 *)*ps;
	elo_u32 c = s[0], min;
	int extra;

	if (c == 0)
		return 0;
	if (c < 0x80) {
		*cp = c;
		*ps += 1;
		return 1;
	}
	if ((c & 0xe0) == 0xc0) {
		extra = 1;
		c &= 0x1f;
		min = 0x80;
	} else if ((c & 0xf0) == 0xe0) {
		extra = 2;
		c &= 0x0f;
		min = 0x800;
	} else if ((c & 0xf8) == 0xf0) {
		extra = 3;
		c &= 0x07;
		min = 0x10000;
	} else {
		return -1;
	}
	for (int i = 1; i <= extra; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return -1;
		c = (c << 6) | (s[i] & 0x3f);
	}
	if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
		return -1;
	*cp = c;
	*ps += 1 + extra;
	return 1;
}

/* Bytes the UTF-8 form of cp takes (cp a valid scalar value). */
static elo_size utf8_len(elo_u32 cp)
{
	return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

static void utf8_put(char *o, elo_u32 cp)
{
	elo_u8 *p = (elo_u8 *)o;

	if (cp < 0x80) {
		p[0] = (elo_u8)cp;
	} else if (cp < 0x800) {
		p[0] = (elo_u8)(0xc0 | (cp >> 6));
		p[1] = (elo_u8)(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		p[0] = (elo_u8)(0xe0 | (cp >> 12));
		p[1] = (elo_u8)(0x80 | ((cp >> 6) & 0x3f));
		p[2] = (elo_u8)(0x80 | (cp & 0x3f));
	} else {
		p[0] = (elo_u8)(0xf0 | (cp >> 18));
		p[1] = (elo_u8)(0x80 | ((cp >> 12) & 0x3f));
		p[2] = (elo_u8)(0x80 | ((cp >> 6) & 0x3f));
		p[3] = (elo_u8)(0x80 | (cp & 0x3f));
	}
}

/* Append cp to out[*n] when it fits with room for the NUL; 0 when not.
 * Callers stop at the first character that does not fit, so a text is
 * only ever cut at a character boundary, at its end. */
static int utf8_append(char *out, elo_size cap, elo_size *n, elo_u32 cp)
{
	elo_size l = utf8_len(cp);

	if (*n + l + 1 > cap)
		return 0;
	utf8_put(out + *n, cp);
	*n += l;
	return 1;
}

elo_size elo_utf8_to_ucs2(const char *s, elo_u8 *out, elo_size cap)
{
	elo_size n = 0;
	elo_u32 cp;
	int r;

	if (!s || !out)
		return 0;
	while ((r = utf8_next(&s, &cp)) == 1) {
		if (cp > 0xffff || n + 2 > cap)
			return 0;
		wr16(out + n, (elo_u16)cp);
		n += 2;
	}
	if (r < 0 || n + 2 > cap)
		return 0;
	wr16(out + n, 0);
	return n + 2;
}

elo_size elo_ucs2_to_utf8(const elo_u8 *ucs2le, elo_size units, char *out,
			  elo_size cap)
{
	elo_size n = 0;

	if (!out || cap == 0)
		return 0;
	out[0] = 0;
	if (!ucs2le)
		return 0;
	for (elo_size i = 0; i < units; i++) {
		elo_u32 c = rd16(ucs2le + 2 * i);

		if (c == 0)
			break;
		if (c >= 0xd800 && c <= 0xdbff && i + 1 < units) {
			elo_u32 lo = rd16(ucs2le + 2 * (i + 1));

			if (lo >= 0xdc00 && lo <= 0xdfff) {
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				i++;
			} else {
				c = 0xfffd;
			}
		} else if (c >= 0xd800 && c <= 0xdfff) {
			c = 0xfffd;
		}
		if (!utf8_append(out, cap, &n, c))
			break;
	}
	out[n] = 0;
	return n;
}

/* ---- GUIDs ------------------------------------------------------------ */

static const char g_hex[] = "0123456789abcdef";

void elo_guid_to_text(const elo_u8 g[16], char out[37])
{
	/* Data1 (4 bytes LE), Data2, Data3 (2 bytes LE each), Data4 as is. */
	static const elo_u8 order[16] = { 3, 2, 1, 0, 5, 4, 7, 6,
					  8, 9, 10, 11, 12, 13, 14, 15 };
	int o = 0;

	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[o++] = '-';
		out[o++] = g_hex[g[order[i]] >> 4];
		out[o++] = g_hex[g[order[i]] & 15];
	}
	out[o] = 0;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int elo_guid_from_text(const char *s, elo_u8 g[16])
{
	static const elo_u8 order[16] = { 3, 2, 1, 0, 5, 4, 7, 6,
					  8, 9, 10, 11, 12, 13, 14, 15 };
	elo_u8 tmp[16];
	int brace = 0, pos = 0;

	if (!s)
		return -1;
	if (*s == '{') {
		brace = 1;
		s++;
	}
	for (int i = 0; i < 16; i++) {
		int hi, lo;

		if (i == 4 || i == 6 || i == 8 || i == 10) {
			if (s[pos] != '-')
				return -1;
			pos++;
		}
		hi = hex_value(s[pos]);
		lo = hi < 0 ? -1 : hex_value(s[pos + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		tmp[order[i]] = (elo_u8)(hi << 4 | lo);
		pos += 2;
	}
	if (brace) {
		if (s[pos] != '}')
			return -1;
		pos++;
	}
	if (s[pos] != 0)
		return -1;
	bytes_copy(g, tmp, 16);
	return 0;
}

/* ---- device path walking ------------------------------------------------ */

static int node_is_end_entire(const elo_u8 *n)
{
	return (n[0] & 0x7f) == ELO_DP_END && n[1] == ELO_DP_END_ENTIRE;
}

static int node_is_end_instance(const elo_u8 *n)
{
	return (n[0] & 0x7f) == ELO_DP_END && n[1] == ELO_DP_END_INSTANCE;
}

/* elo_dp_length, also giving the offset of the End Entire node. */
static elo_size dp_scan(const elo_u8 *dp, elo_size max, elo_size *end_off)
{
	elo_size off = 0;

	if (!dp)
		return 0;
	for (int i = 0; i < ELO_DP_MAX_NODES; i++) {
		elo_size nlen;

		if (max - off < 4)
			return 0;
		nlen = rd16(dp + off + 2);
		if (nlen < 4 || nlen > max - off)
			return 0;
		if (node_is_end_entire(dp + off)) {
			if (end_off)
				*end_off = off;
			return off + nlen;
		}
		off += nlen;
	}
	return 0;
}

elo_size elo_dp_length(const elo_u8 *dp, elo_size max)
{
	return dp_scan(dp, max, ELO_NULL);
}

int elo_dp_valid(const elo_u8 *dp, elo_size len)
{
	return len >= 4 && elo_dp_length(dp, len) == len;
}

/* Copy a Hard Drive node (at least ELO_HD_NODE_LEN bytes) into *hd. */
static void hd_decode(const elo_u8 *n, struct elo_hd *hd)
{
	hd->part_number = rd32(n + 4);
	hd->part_start = rd64(n + 8);
	hd->part_size = rd64(n + 16);
	bytes_copy(hd->signature, n + 24, 16);
	hd->mbr_type = n[40];
	hd->sig_type = n[41];
}

static int node_is_hd(const elo_u8 *n, elo_size nlen)
{
	return (n[0] & 0x7f) == ELO_DP_MEDIA && n[1] == ELO_MEDIA_HD &&
	       nlen >= ELO_HD_NODE_LEN;
}

static int node_is_file(const elo_u8 *n)
{
	return (n[0] & 0x7f) == ELO_DP_MEDIA && n[1] == ELO_MEDIA_FILE;
}

int elo_dp_find_hd(const elo_u8 *dp, elo_size len, struct elo_hd *hd,
		   elo_size *off_out)
{
	elo_size end = elo_dp_length(dp, len), off = 0;

	if (!end)
		return 0;
	while (off < end) {
		elo_size nlen = rd16(dp + off + 2);

		if (node_is_end_entire(dp + off))
			break;
		if (node_is_hd(dp + off, nlen)) {
			if (hd)
				hd_decode(dp + off, hd);
			if (off_out)
				*off_out = off;
			return 1;
		}
		off += nlen;
	}
	return 0;
}

static int same_partition(const struct elo_hd *a, const struct elo_hd *b)
{
	if (a->sig_type != b->sig_type || a->part_number != b->part_number)
		return 0;
	if (a->sig_type == ELO_HD_SIG_GUID)
		return bytes_equal(a->signature, b->signature, 16);
	if (a->sig_type == ELO_HD_SIG_MBR)
		return bytes_equal(a->signature, b->signature, 4);
	return 0;
}

/* A file path character for comparing: '/' as '\', ASCII folded. */
static elo_u32 path_fold(elo_u32 c)
{
	if (c == '/')
		return '\\';
	if (c >= 'A' && c <= 'Z')
		return c + ('a' - 'A');
	return c;
}

static int path_is_sep(elo_u32 c)
{
	return c == '/' || c == '\\';
}

/* The characters of the File nodes starting at dp[off] as one path, one at
 * a time across node boundaries; 0 after the last one.  Consecutive nodes
 * are joined with a separator, a leading separator is supplied when the
 * first node has none, and runs of separators read as a single '\'. */
struct file_iter {
	const elo_u8 *dp;
	elo_size end;      /* end of the path (its End node's offset) */
	elo_size node;     /* offset of the current File node */
	elo_size pos;      /* offset of the next unit */
	elo_size node_end;
	int sep_pending;   /* a node boundary (or the start) not yet joined */
	int last_sep;      /* the last character returned was a separator */
};

static int file_iter_enter(struct file_iter *it, elo_size off)
{
	elo_size nlen;

	if (off >= it->end || !node_is_file(it->dp + off))
		return 0;
	nlen = rd16(it->dp + off + 2);
	it->node = off;
	it->pos = off + 4;
	it->node_end = off + nlen;
	it->sep_pending = 1;
	return 1;
}

static elo_u32 file_iter_next(struct file_iter *it)
{
	for (;;) {
		if (it->pos + 2 <= it->node_end) {
			elo_u32 c = rd16(it->dp + it->pos);

			if (c != 0) {
				if (path_is_sep(c)) {
					it->pos += 2;
					it->sep_pending = 0;
					if (it->last_sep)
						continue;
					it->last_sep = 1;
					return '\\';
				}
				if (it->sep_pending) {
					/* join: the separator first, this
					 * unit on the next call */
					it->sep_pending = 0;
					if (!it->last_sep) {
						it->last_sep = 1;
						return '\\';
					}
				}
				it->pos += 2;
				it->last_sep = 0;
				return c;
			}
		}
		/* This node's string is done: on to the next File node. */
		if (!file_iter_enter(it, it->node_end))
			return 0;
	}
}

/* Do the File node(s) at dp[off] spell `path'?  Both sides normalised the
 * same way: a leading separator, '/' as '\', runs of separators as one,
 * ASCII case folded. */
static int files_match(const elo_u8 *dp, elo_size off, elo_size end,
		       const char *path)
{
	struct file_iter it;
	const char *s = path;
	int lead, wlast = 0;

	it.dp = dp;
	it.end = end;
	it.last_sep = 0;
	if (!file_iter_enter(&it, off))
		return 0;
	lead = !path_is_sep((elo_u8)*s);
	for (;;) {
		elo_u32 want, have = file_iter_next(&it);

		if (lead) {
			want = '\\';
			lead = 0;
			wlast = 1;
		} else {
			for (;;) {
				int r = utf8_next(&s, &want);

				if (r < 0)
					return 0;
				if (r == 0) {
					want = 0;
					break;
				}
				if (!path_is_sep(want)) {
					wlast = 0;
					break;
				}
				if (!wlast) {
					wlast = 1;
					break;
				}
			}
		}
		if (path_fold(want) != path_fold(have))
			return 0;
		if (want == 0)
			return 1;
	}
}

int elo_dp_matches(const elo_u8 *dp, elo_size len, const struct elo_hd *hd,
		   const char *path)
{
	elo_size end = 0, off = 0;

	if (!hd || !path || !*path || !dp_scan(dp, len, &end))
		return 0;
	/* Every HD node (a path list may hold several instances). */
	while (off < end) {
		elo_size nlen = rd16(dp + off + 2);
		struct elo_hd cand;

		if (node_is_hd(dp + off, nlen)) {
			hd_decode(dp + off, &cand);
			if (same_partition(&cand, hd) &&
			    files_match(dp, off + nlen, end, path))
				return 1;
		}
		off += nlen;
	}
	return 0;
}

elo_size elo_dp_hd_file(const struct elo_hd *hd, const char *path,
			elo_u8 *out, elo_size cap)
{
	elo_size off = ELO_HD_NODE_LEN, units = 0;
	const char *s = path;
	elo_u32 cp;
	int r;

	if (!hd || !path || !*path || !out || cap < ELO_HD_NODE_LEN + 4 + 4 + 4)
		return 0;
	/* Hard Drive node */
	out[0] = ELO_DP_MEDIA;
	out[1] = ELO_MEDIA_HD;
	wr16(out + 2, ELO_HD_NODE_LEN);
	wr32(out + 4, hd->part_number);
	wr64(out + 8, hd->part_start);
	wr64(out + 16, hd->part_size);
	bytes_copy(out + 24, hd->signature, 16);
	out[40] = hd->mbr_type;
	out[41] = hd->sig_type;
	/* File node: the path as UCS-2, NUL-terminated. */
	out[off] = ELO_DP_MEDIA;
	out[off + 1] = ELO_MEDIA_FILE;
	if (*s != '/' && *s != '\\') {
		wr16(out + off + 4, '\\');
		units = 1;
	}
	while ((r = utf8_next(&s, &cp)) == 1) {
		if (cp > 0xffff || units >= ELO_PATH_MAX_UNITS)
			return 0;
		if (off + 4 + 2 * units + 2 > cap)
			return 0;
		wr16(out + off + 4 + 2 * units, (elo_u16)(cp == '/' ? '\\' : cp));
		units++;
	}
	if (r < 0)
		return 0;
	/* the NUL, then the End node */
	if (off + 4 + 2 * units + 2 + 4 > cap)
		return 0;
	wr16(out + off + 4 + 2 * units, 0);
	units++;
	wr16(out + off + 2, (elo_u16)(4 + 2 * units));
	off += 4 + 2 * units;
	out[off] = ELO_DP_END;
	out[off + 1] = ELO_DP_END_ENTIRE;
	wr16(out + off + 2, 4);
	return off + 4;
}

/* ---- load options ---------------------------------------------------- */

elo_size elo_load_option_build(elo_u32 attrs, const char *desc,
			       const elo_u8 *dp, elo_size dp_len,
			       const elo_u8 *opt, elo_size opt_len,
			       elo_u8 *out, elo_size cap)
{
	elo_size d, total;

	if (!desc || !dp || !out || cap < 6 || dp_len == 0 || dp_len > 0xffff)
		return 0;
	if (opt_len && !opt)
		return 0;
	/* A list of one or more paths: the first must be well formed and the
	 * list must not end in the middle of a node. */
	{
		elo_size off = 0;

		while (off < dp_len) {
			elo_size l = elo_dp_length(dp + off, dp_len - off);

			if (!l)
				return 0;
			off += l;
		}
	}
	wr32(out, attrs);
	wr16(out + 4, (elo_u16)dp_len);
	d = elo_utf8_to_ucs2(desc, out + 6, cap - 6);
	if (!d)
		return 0;
	total = 6 + d;
	if (dp_len > cap - total || opt_len > cap - total - dp_len)
		return 0;
	bytes_copy(out + total, dp, dp_len);
	total += dp_len;
	if (opt_len)
		bytes_copy(out + total, opt, opt_len);
	return total + opt_len;
}

int elo_load_option_parse(const elo_u8 *buf, elo_size len,
			  struct elo_load_option *out)
{
	elo_size p, units = 0, fpl;

	if (!buf || !out || len < 6)
		return -1;
	fpl = rd16(buf + 4);
	/* the description: UCS-2 up to its NUL, inside the buffer */
	for (p = 6;; p += 2) {
		if (p + 2 > len)
			return -1;
		if (rd16(buf + p) == 0)
			break;
		units++;
	}
	p += 2;
	if (fpl == 0 || fpl > len - p)
		return -1;
	if (!elo_dp_length(buf + p, fpl))
		return -1;
	out->attributes = rd32(buf);
	out->dp_len = fpl;
	out->desc = buf + 6;
	out->desc_units = units;
	out->dp = buf + p;
	out->opt_len = len - p - fpl;
	out->opt = out->opt_len ? buf + p + fpl : ELO_NULL;
	return 0;
}

/* ---- text form ------------------------------------------------------- */

/* An snprintf-like sink: counts everything, stores what fits. */
struct sink {
	char *out;
	elo_size cap;
	elo_size n;
};

static void put_c(struct sink *k, char c)
{
	if (k->cap && k->n + 1 < k->cap)
		k->out[k->n] = c;
	k->n++;
}

static void put_s(struct sink *k, const char *s)
{
	while (*s)
		put_c(k, *s++);
}

static void put_hex(struct sink *k, elo_u64 v)
{
	char tmp[17];
	int i = 0;

	put_s(k, "0x");
	do {
		tmp[i++] = g_hex[v & 15];
		v >>= 4;
	} while (v);
	while (i)
		put_c(k, tmp[--i]);
}

static void put_dec(struct sink *k, elo_u64 v)
{
	char tmp[21];
	int i = 0;

	do {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (i)
		put_c(k, tmp[--i]);
}

static void put_hexbytes(struct sink *k, const elo_u8 *b, elo_size n)
{
	for (elo_size i = 0; i < n; i++) {
		put_c(k, g_hex[b[i] >> 4]);
		put_c(k, g_hex[b[i] & 15]);
	}
}

static void put_guid(struct sink *k, const elo_u8 *g)
{
	char t[37];

	elo_guid_to_text(g, t);
	put_s(k, t);
}

/* UCS-2 text of a node, cut at its NUL or end. */
static void put_ucs2(struct sink *k, const elo_u8 *p, elo_size bytes)
{
	char tmp[8];

	for (elo_size i = 0; i + 1 < bytes; i += 2) {
		elo_u32 c = rd16(p + i);
		elo_size l, j;

		if (c == 0)
			break;
		if (c >= 0xd800 && c <= 0xdbff && i + 3 < bytes) {
			elo_u32 lo = rd16(p + i + 2);

			if (lo >= 0xdc00 && lo <= 0xdfff) {
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			} else {
				c = 0xfffd;
			}
		} else if (c >= 0xd800 && c <= 0xdfff) {
			c = 0xfffd;
		}
		l = utf8_len(c);
		utf8_put(tmp, c);
		for (j = 0; j < l; j++)
			put_c(k, tmp[j]);
	}
}

/* Printable ASCII from a byte string (BBS descriptions, URIs). */
static void put_ascii(struct sink *k, const elo_u8 *p, elo_size n)
{
	for (elo_size i = 0; i < n && p[i]; i++)
		put_c(k, p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '?');
}

static void put_generic(struct sink *k, const elo_u8 *n, elo_size nlen)
{
	put_s(k, "Path(");
	put_dec(k, n[0]);
	put_c(k, ',');
	put_dec(k, n[1]);
	if (nlen > 4) {
		put_c(k, ',');
		put_hexbytes(k, n + 4, nlen - 4);
	}
	put_c(k, ')');
}

/* "Name(guid[,hex])" for the vendor-defined nodes. */
static void put_vendor(struct sink *k, const char *name, const elo_u8 *n,
		       elo_size nlen)
{
	put_s(k, name);
	put_c(k, '(');
	put_guid(k, n + 4);
	if (nlen > 20) {
		put_c(k, ',');
		put_hexbytes(k, n + 20, nlen - 20);
	}
	put_c(k, ')');
}

/* A compressed EISA id as "PNPxxxx" (or its hex value). */
static void put_eisa(struct sink *k, elo_u32 id)
{
	static const char upper[] = "0123456789ABCDEF";

	if ((id & 0xffff) == 0x41d0) {
		elo_u32 prod = id >> 16;

		put_s(k, "PNP");
		for (int s = 12; s >= 0; s -= 4)
			put_c(k, upper[(prod >> s) & 15]);
	} else {
		put_hex(k, id);
	}
}

static void put_ipv4(struct sink *k, const elo_u8 *a)
{
	for (int i = 0; i < 4; i++) {
		if (i)
			put_c(k, '.');
		put_dec(k, a[i]);
	}
}

static void put_ipv6(struct sink *k, const elo_u8 *a)
{
	for (int i = 0; i < 16; i += 2) {
		elo_u32 w = ((elo_u32)a[i] << 8) | a[i + 1];
		char t[5];
		int j = 0, z = 1;

		if (i)
			put_c(k, ':');
		for (int s = 12; s >= 0; s -= 4) {
			int d = (w >> s) & 15;

			if (d || !z || s == 0) {
				t[j++] = g_hex[d];
				z = 0;
			}
		}
		t[j] = 0;
		put_s(k, t);
	}
}

static int render_hw(struct sink *k, const elo_u8 *n, elo_size nlen)
{
	switch (n[1]) {
	case 0x01: /* PCI: function, device */
		if (nlen < 6)
			return 0;
		put_s(k, "Pci(");
		put_hex(k, n[5]);
		put_c(k, ',');
		put_hex(k, n[4]);
		put_c(k, ')');
		return 1;
	case 0x02: /* PC card */
		if (nlen < 5)
			return 0;
		put_s(k, "PcCard(");
		put_hex(k, n[4]);
		put_c(k, ')');
		return 1;
	case 0x03: /* memory mapped: type, start, end */
		if (nlen < 24)
			return 0;
		put_s(k, "MemoryMapped(");
		put_hex(k, rd32(n + 4));
		put_c(k, ',');
		put_hex(k, rd64(n + 8));
		put_c(k, ',');
		put_hex(k, rd64(n + 16));
		put_c(k, ')');
		return 1;
	case 0x04:
		if (nlen < 20)
			return 0;
		put_vendor(k, "VenHw", n, nlen);
		return 1;
	case 0x05:
		if (nlen < 8)
			return 0;
		put_s(k, "Ctrl(");
		put_hex(k, rd32(n + 4));
		put_c(k, ')');
		return 1;
	}
	return 0;
}

static int render_acpi(struct sink *k, const elo_u8 *n, elo_size nlen)
{
	if (n[1] == 0x01 && nlen >= 12) {
		elo_u32 hid = rd32(n + 4), uid = rd32(n + 8);

		if (hid == 0x0a0341d0)
			put_s(k, "PciRoot(");
		else if (hid == 0x0a0841d0)
			put_s(k, "PcieRoot(");
		else {
			put_s(k, "Acpi(");
			put_eisa(k, hid);
			put_c(k, ',');
		}
		put_hex(k, uid);
		put_c(k, ')');
		return 1;
	}
	if (n[1] == 0x03 && nlen >= 8) {
		put_s(k, "AcpiAdr(");
		for (elo_size o = 4; o + 4 <= nlen; o += 4) {
			if (o > 4)
				put_c(k, ',');
			put_hex(k, rd32(n + o));
		}
		put_c(k, ')');
		return 1;
	}
	return 0;
}

static int render_msg(struct sink *k, const elo_u8 *n, elo_size nlen)
{
	switch (n[1]) {
	case 0x01: /* ATAPI: primary/secondary, slave/master, LUN */
		if (nlen < 8)
			return 0;
		put_s(k, "Ata(");
		put_hex(k, n[4]);
		put_c(k, ',');
		put_hex(k, n[5]);
		put_c(k, ',');
		put_hex(k, rd16(n + 6));
		put_c(k, ')');
		return 1;
	case 0x02: /* SCSI: target, LUN */
		if (nlen < 8)
			return 0;
		put_s(k, "Scsi(");
		put_hex(k, rd16(n + 4));
		put_c(k, ',');
		put_hex(k, rd16(n + 6));
		put_c(k, ')');
		return 1;
	case 0x05: /* USB: parent port, interface */
		if (nlen < 6)
			return 0;
		put_s(k, "USB(");
		put_hex(k, n[4]);
		put_c(k, ',');
		put_hex(k, n[5]);
		put_c(k, ')');
		return 1;
	case 0x0a:
		if (nlen < 20)
			return 0;
		put_vendor(k, "VenMsg", n, nlen);
		return 1;
	case 0x0b: /* MAC address (32 bytes, padded), interface type */
		if (nlen < 37)
			return 0;
		put_s(k, "MAC(");
		put_hexbytes(k, n + 4, n[36] == 0x01 || n[36] == 0x00 ? 6 : 32);
		put_c(k, ',');
		put_hex(k, n[36]);
		put_c(k, ')');
		return 1;
	case 0x0c: /* IPv4: local, remote */
		if (nlen < 19)
			return 0;
		put_s(k, "IPv4(");
		put_ipv4(k, n + 8);
		put_c(k, ')');
		return 1;
	case 0x0d: /* IPv6: local, remote */
		if (nlen < 43)
			return 0;
		put_s(k, "IPv6(");
		put_ipv6(k, n + 20);
		put_c(k, ')');
		return 1;
	case 0x0e: /* UART: reserved, baud, data bits, parity, stop bits */
		if (nlen < 19)
			return 0;
		put_s(k, "Uart(");
		put_dec(k, rd64(n + 8));
		put_c(k, ',');
		put_dec(k, n[16]);
		put_c(k, ',');
		put_dec(k, n[17]);
		put_c(k, ',');
		put_dec(k, n[18]);
		put_c(k, ')');
		return 1;
	case 0x0f: /* USB class: vendor, product, class, subclass, protocol */
		if (nlen < 11)
			return 0;
		put_s(k, "UsbClass(");
		put_hex(k, rd16(n + 4));
		put_c(k, ',');
		put_hex(k, rd16(n + 6));
		put_c(k, ',');
		put_hex(k, n[8]);
		put_c(k, ',');
		put_hex(k, n[9]);
		put_c(k, ',');
		put_hex(k, n[10]);
		put_c(k, ')');
		return 1;
	case 0x10: /* USB WWID: interface, vendor, product, serial */
		if (nlen < 10)
			return 0;
		put_s(k, "UsbWwid(");
		put_hex(k, rd16(n + 6));
		put_c(k, ',');
		put_hex(k, rd16(n + 8));
		put_c(k, ',');
		put_hex(k, rd16(n + 4));
		if (nlen > 10) {
			put_c(k, ',');
			put_ucs2(k, n + 10, nlen - 10);
		}
		put_c(k, ')');
		return 1;
	case 0x12: /* SATA: HBA port, port multiplier port, LUN */
		if (nlen < 10)
			return 0;
		put_s(k, "Sata(");
		put_hex(k, rd16(n + 4));
		put_c(k, ',');
		put_hex(k, rd16(n + 6));
		put_c(k, ',');
		put_hex(k, rd16(n + 8));
		put_c(k, ')');
		return 1;
	case 0x17: /* NVMe: namespace id, EUI-64 */
		if (nlen < 16)
			return 0;
		put_s(k, "NVMe(");
		put_hex(k, rd32(n + 4));
		put_c(k, ',');
		for (int i = 0; i < 8; i++) {
			if (i)
				put_c(k, '-');
			put_hexbytes(k, n + 8 + i, 1);
		}
		put_c(k, ')');
		return 1;
	case 0x18: /* URI */
		put_s(k, "Uri(");
		put_ascii(k, n + 4, nlen - 4);
		put_c(k, ')');
		return 1;
	case 0x19: /* UFS: target, LUN */
		if (nlen < 6)
			return 0;
		put_s(k, "UFS(");
		put_hex(k, n[4]);
		put_c(k, ',');
		put_hex(k, n[5]);
		put_c(k, ')');
		return 1;
	case 0x1a: /* SD: slot */
	case 0x1d: /* eMMC: slot */
		if (nlen < 5)
			return 0;
		put_s(k, n[1] == 0x1a ? "SD(" : "eMMC(");
		put_hex(k, n[4]);
		put_c(k, ')');
		return 1;
	}
	return 0;
}

static int render_media(struct sink *k, const elo_u8 *n, elo_size nlen)
{
	switch (n[1]) {
	case ELO_MEDIA_HD: {
		struct elo_hd hd;

		if (nlen < ELO_HD_NODE_LEN)
			return 0;
		hd_decode(n, &hd);
		put_s(k, "HD(");
		put_dec(k, hd.part_number);
		put_c(k, ',');
		if (hd.sig_type == ELO_HD_SIG_GUID) {
			put_s(k, "GPT,");
			put_guid(k, hd.signature);
		} else if (hd.sig_type == ELO_HD_SIG_MBR) {
			char t[9];

			put_s(k, "MBR,0x");
			for (int i = 0; i < 8; i++)
				t[i] = g_hex[(rd32(hd.signature) >> (28 - 4 * i)) & 15];
			t[8] = 0;
			put_s(k, t);
		} else {
			put_hex(k, hd.sig_type);
			put_s(k, ",0");
		}
		put_c(k, ',');
		put_hex(k, hd.part_start);
		put_c(k, ',');
		put_hex(k, hd.part_size);
		put_c(k, ')');
		return 1;
	}
	case ELO_MEDIA_CDROM:
		if (nlen < 24)
			return 0;
		put_s(k, "CDROM(");
		put_hex(k, rd32(n + 4));
		put_c(k, ',');
		put_hex(k, rd64(n + 8));
		put_c(k, ',');
		put_hex(k, rd64(n + 16));
		put_c(k, ')');
		return 1;
	case ELO_MEDIA_VENDOR:
		if (nlen < 20)
			return 0;
		put_vendor(k, "VenMedia", n, nlen);
		return 1;
	case ELO_MEDIA_FILE:
		put_s(k, "File(");
		put_ucs2(k, n + 4, nlen - 4);
		put_c(k, ')');
		return 1;
	case ELO_MEDIA_PROTOCOL:
	case ELO_MEDIA_FV_FILE:
	case ELO_MEDIA_FV:
		if (nlen < 20)
			return 0;
		put_s(k, n[1] == ELO_MEDIA_PROTOCOL ? "Media(" :
			 n[1] == ELO_MEDIA_FV_FILE ? "FvFile(" : "Fv(");
		put_guid(k, n + 4);
		put_c(k, ')');
		return 1;
	case ELO_MEDIA_OFFSET:
		if (nlen < 24)
			return 0;
		put_s(k, "Offset(");
		put_hex(k, rd64(n + 8));
		put_c(k, ',');
		put_hex(k, rd64(n + 16));
		put_c(k, ')');
		return 1;
	}
	return 0;
}

static int render_bbs(struct sink *k, const elo_u8 *n, elo_size nlen)
{
	if (n[1] != 0x01 || nlen < 8)
		return 0;
	put_s(k, "BBS(");
	put_hex(k, rd16(n + 4));
	put_c(k, ',');
	put_ascii(k, n + 8, nlen - 8);
	put_c(k, ')');
	return 1;
}

elo_size elo_dp_to_text(const elo_u8 *dp, elo_size len, char *out,
			elo_size cap)
{
	struct sink k = { out, cap, 0 };
	elo_size off = 0;
	int first = 1;

	if (!dp)
		len = 0;
	for (int i = 0; i < ELO_DP_MAX_NODES; i++) {
		const elo_u8 *n;
		elo_size nlen;
		int done;

		if (len - off < 4) {
			if (off < len || off == 0)
				put_s(&k, first ? "<malformed>" : "/<malformed>");
			break;
		}
		n = dp + off;
		nlen = rd16(n + 2);
		if (nlen < 4 || nlen > len - off) {
			put_s(&k, first ? "<malformed>" : "/<malformed>");
			break;
		}
		if (node_is_end_entire(n))
			break;
		if (node_is_end_instance(n)) {
			put_c(&k, ',');
			first = 1;
			off += nlen;
			continue;
		}
		if (!first)
			put_c(&k, '/');
		first = 0;
		switch (n[0] & 0x7f) {
		case ELO_DP_HARDWARE:
			done = render_hw(&k, n, nlen);
			break;
		case ELO_DP_ACPI:
			done = render_acpi(&k, n, nlen);
			break;
		case ELO_DP_MESSAGING:
			done = render_msg(&k, n, nlen);
			break;
		case ELO_DP_MEDIA:
			done = render_media(&k, n, nlen);
			break;
		case ELO_DP_BBS:
			done = render_bbs(&k, n, nlen);
			break;
		default:
			done = 0;
			break;
		}
		if (!done)
			put_generic(&k, n, nlen);
		off += nlen;
	}
	if (cap)
		out[k.n < cap ? k.n : cap - 1] = 0;
	return k.n;
}

/* ---- BOOTX64.CSV -------------------------------------------------------- */

/* The file as a stream of code points. */
struct csv_reader {
	const elo_u8 *buf;
	elo_size len;
	elo_size pos;
	int utf16;
};

/* The next code point, or -1 at the end (or at a NUL). */
static long csv_next(struct csv_reader *r)
{
	if (r->utf16) {
		elo_u32 c;

		if (r->len - r->pos < 2)
			return -1;
		c = rd16(r->buf + r->pos);
		r->pos += 2;
		if (c == 0)
			return -1;
		if (c >= 0xd800 && c <= 0xdbff && r->len - r->pos >= 2) {
			elo_u32 lo = rd16(r->buf + r->pos);

			if (lo >= 0xdc00 && lo <= 0xdfff) {
				r->pos += 2;
				return 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
			}
		}
		if (c >= 0xd800 && c <= 0xdfff)
			return 0xfffd;
		return (long)c;
	} else {
		elo_u8 tmp[5] = { 0, 0, 0, 0, 0 };
		const char *s = (const char *)tmp;
		elo_size n = r->len - r->pos;
		elo_u32 cp;

		if (n == 0 || r->buf[r->pos] == 0)
			return -1;
		if (n > 4)
			n = 4;
		bytes_copy(tmp, r->buf + r->pos, n);
		if (utf8_next(&s, &cp) == 1) {
			r->pos += (elo_size)(s - (const char *)tmp);
			return (long)cp;
		}
		r->pos++;
		return 0xfffd;
	}
}

/* Drop trailing blanks of a UTF-8 field. */
static void trim_end(char *s, elo_size *n)
{
	while (*n && (s[*n - 1] == ' ' || s[*n - 1] == '\t'))
		(*n)--;
	s[*n] = 0;
}

int elo_csv_parse(const elo_u8 *buf, elo_size len, char *file, elo_size fcap,
		  char *label, elo_size lcap, char *opts, elo_size ocap)
{
	struct csv_reader r;
	char *fields[3];
	elo_size caps[3];

	if (!buf || !file || fcap == 0)
		return -1;
	fields[0] = file;
	fields[1] = label;
	fields[2] = opts;
	caps[0] = fcap;
	caps[1] = label ? lcap : 0;
	caps[2] = opts ? ocap : 0;
	for (int i = 0; i < 3; i++)
		if (caps[i])
			fields[i][0] = 0;

	r.buf = buf;
	r.len = len;
	r.pos = 0;
	r.utf16 = 0;
	if (len >= 2 && buf[0] == 0xff && buf[1] == 0xfe) {
		r.utf16 = 1;
		r.pos = 2;
	} else if (len >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) {
		r.pos = 3;
	} else if (len >= 2 && buf[0] != 0 && buf[1] == 0) {
		r.utf16 = 1; /* UTF-16LE without a byte order mark */
	}

	for (;;) {
		long c;
		elo_size n[3] = { 0, 0, 0 };
		int full[3] = { 0, 0, 0 };
		int field = 0, started = 0;

		/* Skip leading blanks; an empty line or a comment is skipped
		 * whole. */
		do {
			c = csv_next(&r);
		} while (c == ' ' || c == '\t' || c == 0xfeff);
		if (c < 0)
			return -1;
		if (c == '\r' || c == '\n')
			continue;
		if (c == '#') {
			while (c >= 0 && c != '\n')
				c = csv_next(&r);
			if (c < 0)
				return -1;
			continue;
		}
		/* The first usable line: up to three fields. */
		for (; c >= 0 && c != '\n'; c = csv_next(&r)) {
			if (c == '\r')
				continue;
			if (c == ',') {
				if (field < 3 && caps[field])
					trim_end(fields[field], &n[field]);
				field++;
				started = 0;
				continue;
			}
			if (!started && (c == ' ' || c == '\t'))
				continue;
			started = 1;
			if (field < 3 && caps[field] && !full[field]) {
				if (utf8_append(fields[field], caps[field],
						&n[field], (elo_u32)c))
					fields[field][n[field]] = 0;
				else
					full[field] = 1;
			}
		}
		if (field < 3 && caps[field])
			trim_end(fields[field], &n[field]);
		return n[0] ? 0 : -1;
	}
}
