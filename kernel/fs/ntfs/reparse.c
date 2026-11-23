/* LikeOS - NTFS filesystem driver
 *
 * Reparse points and link targets.
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

/* ===========================================================================
 * S9, S16, S17  Reparse data: WOF parameters, link tags, link targets,
 * directory entry types
 *
 * Reading the reparse point is S17 (below); the inode side, S16
 * (inode.c).
 * =========================================================================== */

/* The chunk format of a WOF reparse point (`rp', `rlen' bytes, header
 * included), or ~0U when it is not one this driver can read.  Only
 * version 1 of the file provider (2) with provider version 1, a known
 * format and a data length of exactly the WOF structure. */
uint32_t ntfs_wof_parse(const uint8_t *rp, uint32_t rlen)
{
	const ntfs_wof_reparse_t *w;

	if (rlen < NTFS_REPARSE_HDR + sizeof(ntfs_wof_reparse_t) ||
	    ntfs_le32(rp) != NTFS_IO_REPARSE_TAG_WOF ||
	    ntfs_le16(rp + 4) != sizeof(ntfs_wof_reparse_t))
		return ~0U;
	w = (const ntfs_wof_reparse_t *)(rp + NTFS_REPARSE_HDR);
	if (w->version != 1 || w->provider != 2 || w->provider_version != 1 ||
	    w->compression_format > NTFS_WOF_XPRESS16K)
		return ~0U;
	return w->compression_format;
}

/* Is the inode a symbolic link (native symlink, junction, WSL link)? */
int ntfs_tag_is_link(uint32_t tag)
{
	return tag == NTFS_IO_REPARSE_TAG_SYMLINK ||
	       tag == NTFS_IO_REPARSE_TAG_MOUNT_POINT ||
	       tag == NTFS_IO_REPARSE_TAG_LX_SYMLINK;
}

/* UTF-16 path -> UTF-8 with '\' -> '/'.  Returns the length or an ST_. */
long ntfs_utf16_path(const uint16_t *s, uint32_t n, char *out,
		     unsigned long cap)
{
	unsigned long o = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = ntfs_le16(&s[i]);
		uint16_t one;
		long k;

		if (c == 0)
			return ST_INVALID;
		if (c == '\\' || c == '/') {
			if (o + 2 > cap)
				return ST_NAMETOOLONG;
			out[o++] = '/';
			continue;
		}
		/* Surrogate pairs convert together -- only a high surrogate
		 * followed by a low one is a pair. */
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n &&
		    ntfs_le16(&s[i + 1]) >= 0xDC00 &&
		    ntfs_le16(&s[i + 1]) <= 0xDFFF) {
			k = ntfs_utf16_to_utf8(&s[i], 2, out + o, cap - o);
			if (k == 4) {
				o += 4;
				i++;
				continue;
			}
		}
		ntfs_put_le16(&one, (uint16_t)c);
		k = ntfs_utf16_to_utf8(&one, 1, out + o, cap - o);
		if (k < 0)
			return k;
		o += (unsigned long)k;
	}
	if (o + 1 > cap)
		return ST_NAMETOOLONG;
	out[o] = '\0';
	return (long)o;
}

/* Rewrite an absolute native target (UTF-8 in `t', already with '/') per
 * the native_symlink option; result in `out'. */
static long ntfs_abs_target(ntfs_fs_t *fs, const char *t, long tl,
			    uint32_t depth, char *out, unsigned long cap)
{
	const char *p = t;
	long n = tl;
	unsigned long o = 0;

	/* "/??/" (the NT object-manager prefix) and "//?/" go. */
	if (n >= 4 && (ntfs_strneq(p, "/?" "?/", 4) || ntfs_strneq(p, "//?/", 4))) {
		p += 4;
		n -= 4;
	}
	if (fs->opt.native_symlink == NTFS_SYMLINK_REL && n >= 2 &&
	    p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') ||
			    (p[0] >= 'a' && p[0] <= 'z'))) {
		/* "C:/rest" -> "../" x depth + "rest" ("." for the root). */
		p += 2;
		n -= 2;
		while (n > 0 && *p == '/') {
			p++;
			n--;
		}
		for (uint32_t i = 0; i < depth; i++) {
			if (o + 3 >= cap)
				return ST_NAMETOOLONG;
			out[o++] = '.';
			out[o++] = '.';
			out[o++] = '/';
		}
		if (n == 0) {
			if (o == 0) {
				if (cap < 2)
					return ST_NAMETOOLONG;
				out[o++] = '.';
			} else {
				o--; /* drop the trailing '/' */
			}
		}
	}
	if (o + (unsigned long)n + 1 > cap)
		return ST_NAMETOOLONG;
	mm_memcpy(out + o, p, (unsigned long)n);
	o += (unsigned long)n;
	out[o] = '\0';
	return (long)o;
}

/* The target of the link whose reparse data is `rp' (`rlen' bytes). */
long ntfs_link_target(ntfs_fs_t *fs, const uint8_t *rp, uint32_t rlen,
		      uint32_t depth, char *out, unsigned long cap)
{
	uint32_t tag = ntfs_le32(rp);
	uint32_t dlen = ntfs_le16(rp + 4);
	const uint8_t *d = rp + NTFS_REPARSE_HDR;
	uint32_t hdr, so, sl, flags = 0;
	char *tmp;
	long tl;

	if (rlen < NTFS_REPARSE_HDR || dlen > rlen - NTFS_REPARSE_HDR)
		return ST_IO;
	if (tag == NTFS_IO_REPARSE_TAG_LX_SYMLINK) {
		/* WSL: a 4-byte version (2), then the UTF-8 target. */
		if (dlen < 5 || ntfs_le32(d) != 2)
			return ST_IO;
		tl = (long)(dlen - 4);
		if ((unsigned long)tl + 1 > cap)
			return ST_NAMETOOLONG;
		for (long i = 0; i < tl; i++)
			if (d[4 + i] == 0)
				return ST_IO;
		mm_memcpy(out, d + 4, (unsigned long)tl);
		out[tl] = '\0';
		return tl;
	}
	if (tag == NTFS_IO_REPARSE_TAG_SYMLINK)
		hdr = 12;
	else if (tag == NTFS_IO_REPARSE_TAG_MOUNT_POINT)
		hdr = 8;
	else
		return ST_INVALID;
	if (dlen < hdr)
		return ST_IO;
	so = ntfs_le16(d);
	sl = ntfs_le16(d + 2);
	if (tag == NTFS_IO_REPARSE_TAG_SYMLINK)
		flags = ntfs_le32(d + 8);
	/* The name range inside the buffer, an even length. */
	if ((so | sl) & 1 || sl == 0 || so > dlen - hdr ||
	    sl > dlen - hdr - so)
		return ST_IO;
	tmp = (char *)kalloc(cap);
	if (!tmp)
		return ST_NOMEM;
	tl = ntfs_utf16_path((const uint16_t *)(d + hdr + so), sl / 2, tmp, cap);
	if (tl < 0) {
		kfree(tmp);
		return tl;
	}
	if (tag == NTFS_IO_REPARSE_TAG_SYMLINK &&
	    (flags & NTFS_SYMLINK_FLAG_RELATIVE)) {
		mm_memcpy(out, tmp, (unsigned long)tl + 1);
	} else {
		tl = ntfs_abs_target(fs, tmp, tl, depth, out, cap);
	}
	kfree(tmp);
	return tl;
}

/* d_type of an $I30 entry, from its $FILE_NAME key alone (the
 * reparse bit before the directory bit; unknown when undecidable). */
uint8_t ntfs_fn_dtype(const ntfs_file_name_t *fn)
{
	uint32_t fa = fn->file_attributes;

	if (fa & NTFS_FILE_ATTR_REPARSE_POINT) {
		switch (fn->type.reparse_point_tag) {
		case NTFS_IO_REPARSE_TAG_SYMLINK:
		case NTFS_IO_REPARSE_TAG_MOUNT_POINT:
		case NTFS_IO_REPARSE_TAG_LX_SYMLINK:
			return DT_LNK;
		case NTFS_IO_REPARSE_TAG_LX_FIFO:
			return DT_FIFO;
		case NTFS_IO_REPARSE_TAG_LX_CHR:
			return DT_CHR;
		case NTFS_IO_REPARSE_TAG_LX_BLK:
			return DT_BLK;
		case NTFS_IO_REPARSE_TAG_AF_UNIX:
			return DT_SOCK;
		case 0:
			return DT_UNKNOWN;
		default:
			break;
		}
	}
	if (fa & NTFS_FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT)
		return DT_DIR;
	return DT_REG;
}

/* ===========================================================================
 * S17  Reparse points and link targets
 *
 * Symbolic links (IO_REPARSE_TAG_SYMLINK), junctions (MOUNT_POINT) and WSL
 * links (LX_SYMLINK) are links; WSL FIFOs, sockets and devices are special
 * files; every other tag (WOF, deduplication, cloud placeholders, ...)
 * leaves the file or directory as it is.
 *
 * Native targets: the substitute name, bounds-checked inside
 * the reparse buffer, converted to UTF-8 with '\' -> '/'.  A relative
 * symlink is returned so.  An absolute one ("\??\C:\dir", a junction):
 * native_symlink=raw (default) returns it with the "\??\" prefix removed
 * ("C:/dir"); native_symlink=rel rewrites a drive-letter path into one
 * relative to the link's directory ("../../dir"), assuming the target is
 * on this volume.  Volume-GUID and UNC targets are returned raw.  WSL link
 * targets are stored as UTF-8 and returned as they are.
 * =========================================================================== */

int ntfs_reparse_read(ntfs_fs_t *fs, uint64_t mref, uint8_t **buf,
		      uint32_t *len)
{
	uint8_t *rp = NULL;
	uint32_t rlen = 0;
	int r;

	*buf = NULL;
	*len = 0;
	r = ntfs_attr_readall(fs, mref, NTFS_AT_REPARSE_POINT, ntfs_unnamed, 0,
			      NTFS_REPARSE_MAX + NTFS_REPARSE_HDR, &rp, &rlen);
	if (r != ST_OK)
		return r;
	if (rlen < NTFS_REPARSE_HDR ||
	    (uint32_t)ntfs_le16(rp + 4) > rlen - NTFS_REPARSE_HDR) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt reparse point",
			       (unsigned long long)NTFS_MREF_NO(mref));
		kfree(rp);
		return ST_IO;
	}
	*buf = rp;
	*len = rlen;
	return ST_OK;
}

int ntfs_link_target_ino(ntfs_fs_t *fs, const ntfs_inode_t *ino,
			 uint32_t depth, char *out, unsigned long cap)
{
	uint8_t *rp;
	uint32_t rlen;
	long tl;
	int r;

	if (!ntfs_inode_is_link(ino))
		return ST_INVALID;
	r = ntfs_reparse_read(fs, ntfs_ino_mref(ino), &rp, &rlen);
	if (r != ST_OK)
		return r;
	tl = ntfs_link_target(fs, rp, rlen, depth, out, cap);
	kfree(rp);
	if (tl > 0x7FFFFFFF)
		return ST_NAMETOOLONG;
	return (int)tl;
}
