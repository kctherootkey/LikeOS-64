/* LikeOS - exFAT filesystem driver
 *
 * The upcase table, UTF-8 <-> UTF-16, name folding, NameHash.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/exfat/exfat.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/mount.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/bug.h>
#include <kernel/fs/exfat/exfat_int.h>

/* ===========================================================================
 * 10. Names: the upcase table, UTF-8 <-> UTF-16, folding, NameHash
 *
 * exFAT names are UTF-16, up to 255 code units, case-preserving and
 * case-insensitive: two names are the same when they are equal after every
 * code unit has gone through the volume's upcase table.  The table on disk
 * may be "compressed": the value 0xFFFF followed by a count N says that the
 * next N code units map to themselves.  It is expanded at mount into a full
 * 65536-entry table (128 KB), identity wherever it says nothing.
 *
 * UTF-16 names reach the VFS as UTF-8.  An unpaired surrogate (legal in an
 * exFAT name, not in Unicode) is carried as its three-byte encoding, so
 * every name on the volume can be listed AND opened again.
 * =========================================================================== */

/* The built-in table, for a volume without a usable one (mounted read-only
 * then): ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic and the
 * full-width Latin letters -- enough to find names, not to create them. */
void exfat_upcase_builtin(uint16_t *t)
{
	for (uint32_t c = 0; c < EXFAT_UPCASE_ENTRIES; c++)
		t[c] = (uint16_t)c;
	for (uint32_t c = 'a'; c <= 'z'; c++)
		t[c] = (uint16_t)(c - 0x20);
	for (uint32_t c = 0xE0; c <= 0xFE; c++)
		if (c != 0xF7)
			t[c] = (uint16_t)(c - 0x20);
	t[0xFF] = 0x178;
	t[0xB5] = 0x39C;
	/* Latin Extended-A: pairs, upper case even ... */
	for (uint32_t c = 0x100; c <= 0x137; c += 2)
		t[c + 1] = (uint16_t)c;
	t[0x131] = 'I';
	/* ... odd from 0x139 to 0x148 ... */
	for (uint32_t c = 0x139; c <= 0x147; c += 2)
		t[c + 1] = (uint16_t)c;
	/* ... even again from 0x14A to 0x177 ... */
	for (uint32_t c = 0x14A; c <= 0x177; c += 2)
		t[c + 1] = (uint16_t)c;
	/* ... odd from 0x179 to 0x17E. */
	for (uint32_t c = 0x179; c <= 0x17D; c += 2)
		t[c + 1] = (uint16_t)c;
	t[0x17F] = 'S';
	/* Greek. */
	for (uint32_t c = 0x3B1; c <= 0x3C9; c++)
		t[c] = (uint16_t)(c - 0x20);
	t[0x3C2] = 0x3A3;
	t[0x3AC] = 0x386;
	t[0x3AD] = 0x388;
	t[0x3AE] = 0x389;
	t[0x3AF] = 0x38A;
	t[0x3CC] = 0x38C;
	t[0x3CD] = 0x38E;
	t[0x3CE] = 0x38F;
	/* Cyrillic. */
	for (uint32_t c = 0x430; c <= 0x44F; c++)
		t[c] = (uint16_t)(c - 0x20);
	for (uint32_t c = 0x450; c <= 0x45F; c++)
		t[c] = (uint16_t)(c - 0x50);
	for (uint32_t c = 0x460; c <= 0x480; c += 2)
		t[c + 1] = (uint16_t)c;
	for (uint32_t c = 0x48A; c <= 0x4BE; c += 2)
		t[c + 1] = (uint16_t)c;
	/* Full-width Latin. */
	for (uint32_t c = 0xFF41; c <= 0xFF5A; c++)
		t[c] = (uint16_t)(c - 0x20);
}

/* Expand a table as stored (`n' bytes) into `t'.  0 on success, -1 when the
 * stored form is malformed (odd length, a compression count running past
 * the end of the table). */
int exfat_upcase_expand(const uint8_t *raw, unsigned long n,
			uint16_t *t)
{
	uint32_t idx = 0;

	for (uint32_t c = 0; c < EXFAT_UPCASE_ENTRIES; c++)
		t[c] = (uint16_t)c;
	if (n & 1)
		return -1;
	for (unsigned long i = 0; i + 1 < n && idx < EXFAT_UPCASE_ENTRIES;
	     i += 2) {
		uint16_t v = exfat_le16(raw + i);

		if (v == 0xFFFF && i + 3 < n) {
			uint32_t count = exfat_le16(raw + i + 2);

			if ((uint64_t)idx + count > EXFAT_UPCASE_ENTRIES)
				return -1;
			idx += count; /* identity: already there */
			i += 2;
			continue;
		}
		t[idx++] = v;
	}
	return 0;
}

static void exfat_fold(exfat_fs_t *fs, const uint16_t *in, unsigned n,
		       uint16_t *out)
{
	for (unsigned i = 0; i < n; i++)
		out[i] = exfat_upc(fs, in[i]);
}

/* Code units forbidden in a name (specification 7.7.3). */
static int exfat_char_forbidden(uint16_t c)
{
	if (c < 0x20)
		return 1;
	switch (c) {
	case '"':
	case '*':
	case '/':
	case ':':
	case '<':
	case '>':
	case '?':
	case '\\':
	case '|':
		return 1;
	}
	return 0;
}

/* UTF-8 (`len' bytes) -> UTF-16 in `out' (room for `max' units).  Code
 * points above U+FFFF become surrogate pairs; three-byte encodings of
 * surrogates are taken as they are (see above).  Returns the number of
 * units, ST_NAMETOOLONG, or ST_INVALID for malformed UTF-8. */
static int exfat_utf8_to_utf16(const char *s, unsigned long len, uint16_t *out,
			       unsigned max)
{
	const uint8_t *p = (const uint8_t *)s;
	unsigned long i = 0;
	unsigned n = 0;

	while (i < len) {
		uint32_t cp;
		unsigned extra;
		uint8_t c = p[i];

		if (c < 0x80) {
			cp = c;
			extra = 0;
		} else if ((c & 0xE0) == 0xC0) {
			cp = c & 0x1F;
			extra = 1;
		} else if ((c & 0xF0) == 0xE0) {
			cp = c & 0x0F;
			extra = 2;
		} else if ((c & 0xF8) == 0xF0) {
			cp = c & 0x07;
			extra = 3;
		} else {
			return ST_INVALID;
		}
		/* The continuation bytes must all be there. */
		if (extra && i + extra >= len)
			return ST_INVALID;
		for (unsigned k = 1; k <= extra; k++) {
			uint8_t cc = p[i + k];

			if ((cc & 0xC0) != 0x80)
				return ST_INVALID;
			cp = (cp << 6) | (cc & 0x3F);
		}
		/* Overlong forms and values past Unicode are malformed. */
		if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
		    (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
			return ST_INVALID;
		i += 1 + extra;
		if (cp >= 0x10000) {
			if (n + 2 > max)
				return ST_NAMETOOLONG;
			cp -= 0x10000;
			out[n++] = (uint16_t)(0xD800 | (cp >> 10));
			out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
		} else {
			if (n + 1 > max)
				return ST_NAMETOOLONG;
			out[n++] = (uint16_t)cp;
		}
	}
	return (int)n;
}

/* UTF-16 -> UTF-8 into `out' (`cap' bytes, NUL included).  Returns the
 * length without the NUL, or -1 when it does not fit. */
int exfat_utf16_to_utf8(const uint16_t *in, unsigned n, char *out,
			unsigned cap)
{
	unsigned o = 0;

	for (unsigned i = 0; i < n; i++) {
		uint32_t cp = in[i];

		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n &&
		    in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
			cp = 0x10000 + ((cp - 0xD800) << 10) +
			     (in[i + 1] - 0xDC00);
			i++;
		}
		if (cp < 0x80) {
			if (o + 1 >= cap)
				return -1;
			out[o++] = (char)cp;
		} else if (cp < 0x800) {
			if (o + 2 >= cap)
				return -1;
			out[o++] = (char)(0xC0 | (cp >> 6));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			if (o + 3 >= cap)
				return -1;
			out[o++] = (char)(0xE0 | (cp >> 12));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		} else {
			if (o + 4 >= cap)
				return -1;
			out[o++] = (char)(0xF0 | (cp >> 18));
			out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	if (o >= cap)
		return -1;
	out[o] = 0;
	return (int)o;
}

/* A path component as the volume would store it: UTF-16 in ctx->name,
 * folded copy in ctx->up, the folded UTF-8 dcache key in ctx->key, and the
 * NameHash.  `for_create' also rejects names exFAT cannot hold.  Returns the
 * length in units or an ST_ error. */
int exfat_name_prepare(exfat_fs_t *fs, exfat_ctx_t *ctx,
		       const char *comp, unsigned long len,
		       int for_create, uint16_t *hash)
{
	int n;

	if (len == 0)
		return ST_INVALID;
	n = exfat_utf8_to_utf16(comp, len, ctx->name, EXFAT_NAME_MAX);
	if (n < 0)
		return n == ST_NAMETOOLONG ? ST_NAMETOOLONG : ST_INVALID;
	if (n == 0)
		return ST_INVALID;
	if (for_create) {
		for (int i = 0; i < n; i++)
			if (exfat_char_forbidden(ctx->name[i]))
				return ST_INVALID;
	}
	exfat_fold(fs, ctx->name, (unsigned)n, ctx->up);
	*hash = exfat_name_hash(ctx->up, (unsigned)n);
	if (exfat_utf16_to_utf8(ctx->up, (unsigned)n, ctx->key,
				sizeof(ctx->key)) < 0)
		ctx->key[0] = 0;
	return n;
}
