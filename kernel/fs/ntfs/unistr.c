/* LikeOS - NTFS filesystem driver
 *
 * UTF-16 <-> UTF-8, $UpCase and name collation.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#define NTFS_INTERNAL 1 /* the driver-internal section of ntfs.h */
#include <kernel/fs/ntfs/ntfs.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/uapi/bug.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/fs/ntfs/ntfs_int.h>

/* ---- UTF-16 <-> UTF-8 ------------------------------------------------------
 *
 * NTFS names are UTF-16LE sequences that need not be well formed: a name
 * may hold an unpaired surrogate.  Such a unit is encoded as its own
 * three-byte sequence (the generalisation of UTF-8 that keeps every 16-bit
 * value), and the reverse conversion turns that sequence back into the
 * same unit, so every on-disk name has exactly one UTF-8 spelling and can
 * be looked up again by it.  A paired surrogate becomes one four-byte
 * sequence.  NUL and '/' cannot appear in a path component: names holding
 * them are reported as unrepresentable (ST_INVALID). */

/* UTF-8 bytes needed for `n' units of `s'; ST_INVALID for a NUL or '/'. */
NTFS_MAYBE_UNUSED static long ntfs_utf16_utf8_len(const uint16_t *s, uint32_t n)
{
	long len = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint16_t c = ntfs_le16(&s[i]);

		if (c == 0 || c == '/')
			return ST_INVALID;
		if (c < 0x80)
			len += 1;
		else if (c < 0x800)
			len += 2;
		else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n &&
			 ntfs_le16(&s[i + 1]) >= 0xDC00 &&
			 ntfs_le16(&s[i + 1]) <= 0xDFFF) {
			len += 4;
			i++;
		} else
			len += 3;
	}
	return len;
}

/* Convert `n' little-endian UTF-16 units to UTF-8 in `out' (`cap' bytes,
 * room for the NUL included).  Returns the length (without the NUL), or
 * ST_INVALID (NUL / '/') or ST_NAMETOOLONG (does not fit). */
long ntfs_utf16_to_utf8(const uint16_t *s, uint32_t n, char *out,
			unsigned long cap)
{
	unsigned long o = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = ntfs_le16(&s[i]);
		uint8_t b[4];
		unsigned k;

		if (c == 0 || c == '/')
			return ST_INVALID;
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
			uint32_t d = ntfs_le16(&s[i + 1]);

			if (d >= 0xDC00 && d <= 0xDFFF) {
				c = 0x10000 + ((c - 0xD800) << 10) +
				    (d - 0xDC00);
				i++;
			}
		}
		if (c < 0x80) {
			b[0] = (uint8_t)c;
			k = 1;
		} else if (c < 0x800) {
			b[0] = (uint8_t)(0xC0 | (c >> 6));
			b[1] = (uint8_t)(0x80 | (c & 0x3F));
			k = 2;
		} else if (c < 0x10000) {
			b[0] = (uint8_t)(0xE0 | (c >> 12));
			b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
			b[2] = (uint8_t)(0x80 | (c & 0x3F));
			k = 3;
		} else {
			b[0] = (uint8_t)(0xF0 | (c >> 18));
			b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
			b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
			b[3] = (uint8_t)(0x80 | (c & 0x3F));
			k = 4;
		}
		if (o + k + 1 > cap)
			return ST_NAMETOOLONG;
		for (unsigned j = 0; j < k; j++)
			out[o++] = (char)b[j];
	}
	if (o + 1 > cap)
		return ST_NAMETOOLONG;
	out[o] = '\0';
	return (long)o;
}

/* Convert `len' bytes of UTF-8 to little-endian UTF-16 units in `out' (at
 * most `cap' units).  Returns the number of units, ST_INVALID for a byte
 * sequence that is not (generalised) UTF-8, ST_NAMETOOLONG when it does
 * not fit. */
long ntfs_utf8_to_utf16(const char *s, unsigned long len, uint16_t *out,
			uint32_t cap)
{
	uint32_t o = 0;
	unsigned long i = 0;
	int prev_was3 = 0;

	while (i < len) {
		uint8_t c = (uint8_t)s[i];
		uint32_t cp;
		unsigned need;

		if (c == 0)
			return ST_INVALID;
		if (c < 0x80) {
			cp = c;
			need = 0;
		} else if ((c & 0xE0) == 0xC0) {
			cp = c & 0x1F;
			need = 1;
		} else if ((c & 0xF0) == 0xE0) {
			cp = c & 0x0F;
			need = 2;
		} else if ((c & 0xF8) == 0xF0) {
			cp = c & 0x07;
			need = 3;
		} else
			return ST_INVALID;
		if (need > len - i - 1)
			return ST_INVALID;
		for (unsigned k = 1; k <= need; k++) {
			uint8_t d = (uint8_t)s[i + k];

			if ((d & 0xC0) != 0x80)
				return ST_INVALID;
			cp = (cp << 6) | (d & 0x3F);
		}
		/* Overlong forms are refused: every name has one spelling. */
		if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
		    (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
			return ST_INVALID;
		i += need + 1;
		/* One spelling per name: a surrogate pair is the four-byte
		 * sequence, never two three-byte ones (a lone surrogate keeps
		 * its own three-byte form, see above). */
		if (cp >= 0xDC00 && cp <= 0xDFFF && o > 0 && need == 2) {
			uint16_t prev = ntfs_le16(&out[o - 1]);

			if (prev >= 0xD800 && prev <= 0xDBFF && prev_was3)
				return ST_INVALID;
		}
		prev_was3 = need == 2;
		if (cp >= 0x10000) {
			if (o + 2 > cap)
				return ST_NAMETOOLONG;
			cp -= 0x10000;
			ntfs_put_le16(&out[o++], (uint16_t)(0xD800 + (cp >> 10)));
			ntfs_put_le16(&out[o++],
				      (uint16_t)(0xDC00 + (cp & 0x3FF)));
		} else {
			if (o + 1 > cap)
				return ST_NAMETOOLONG;
			ntfs_put_le16(&out[o++], (uint16_t)cp);
		}
	}
	return (long)o;
}

/* ASCII string -> UTF-16LE units (for fixed system names). */
uint32_t ntfs_ascii_to_utf16(const char *s, uint16_t *out, uint32_t cap)
{
	uint32_t n = 0;

	while (s[n] && n < cap) {
		ntfs_put_le16(&out[n], (uint16_t)(uint8_t)s[n]);
		n++;
	}
	return n;
}

/* ===========================================================================
 * S14  $UpCase and name collation
 *
 * The volume's $UpCase table (loaded at mount, up to 64 K entries, kept in
 * CPU order) maps every UTF-16 unit to its upper-case form; units beyond
 * the table map to themselves.  $I30 indexes are ordered by
 * COLLATION_FILE_NAME: the names compared unit by unit through the table
 * (shorter first on a common prefix); names that are equal that way are
 * ordered by their raw units.  That is a total order, so a case-sensitive
 * lookup is an ordinary B+tree search, and the case-insensitive one
 * (`nocase') finds the first entry that is equal ignoring case.  POSIX
 * names with characters that are invalid for other namespaces collate like
 * any other: collation never fails on a name.
 * =========================================================================== */

int ntfs_upcase_eq(ntfs_fs_t *fs, const uint16_t *a, const uint16_t *b,
		   uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (ntfs_upc(fs, ntfs_le16(&a[i])) !=
		    ntfs_upc(fs, ntfs_le16(&b[i])))
			return 0;
	return 1;
}

/* <0, 0, >0: a against b.  `ic_only': stop after the case-insensitive
 * comparison. */
int ntfs_collate_names(ntfs_fs_t *fs, const uint16_t *a, uint32_t alen,
		       const uint16_t *b, uint32_t blen, int ic_only)
{
	uint32_t n = alen < blen ? alen : blen;

	for (uint32_t i = 0; i < n; i++) {
		uint16_t ca = ntfs_upc(fs, ntfs_le16(&a[i]));
		uint16_t cb = ntfs_upc(fs, ntfs_le16(&b[i]));

		if (ca != cb)
			return ca < cb ? -1 : 1;
	}
	if (alen != blen)
		return alen < blen ? -1 : 1;
	if (ic_only)
		return 0;
	for (uint32_t i = 0; i < n; i++) {
		uint16_t ca = ntfs_le16(&a[i]);
		uint16_t cb = ntfs_le16(&b[i]);

		if (ca != cb)
			return ca < cb ? -1 : 1;
	}
	return 0;
}
