/* LikeOS - NTFS filesystem driver
 *
 * Extended attributes ($EA): reading, the WSL EAs, rebuilding.
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
 * S18a  Extended attributes: the validated $EA walk
 *
 * $EA holds a chain of FILE_FULL_EA_INFORMATION entries; $EA_INFORMATION
 * says how many bytes of it are in use (ea_query_length).  EAs are read
 * only when both attributes exist.  Before any field of an entry is
 * used:
 *   - the query length is not larger than $EA; 0 means "no EAs",
 *     anything below one entry header is corrupt;
 *   - at every step at least an entry header remains;
 *   - the entry size is next_entry_offset, or the rest for the last entry;
 *     it is >= the header, inside the rest, a multiple of 4 when it links
 *     to a next entry;
 *   - name + NUL fits after the header, the name is NUL-terminated, the
 *     value fits after it, and the aligned real size fits the entry.
 * A malformed chain fails as a whole (ST_IO); listxattr never returns a
 * partial list.
 * =========================================================================== */

/* Walk the EA chain in `buf' (`qlen' bytes in use).  The callback returns
 * 0 to continue, 1 to stop (ST_OK), < 0 for an error. */
int ntfs_ea_walk(const uint8_t *buf, uint32_t qlen, ntfs_ea_cb cb,
		 void *arg)
{
	uint32_t o = 0;

	while (o < qlen) {
		const ntfs_ea_entry_t *e;
		uint32_t rem = qlen - o, size, nl, vl, real;
		ntfs_ea_view_t v;
		int r;

		if (rem < NTFS_EA_HDR)
			return ST_IO;
		e = (const ntfs_ea_entry_t *)(buf + o);
		size = e->next_entry_offset ? e->next_entry_offset : rem;
		if (size < NTFS_EA_HDR || size > rem ||
		    (e->next_entry_offset && (size & 3)))
			return ST_IO;
		nl = e->ea_name_length;
		vl = e->ea_value_length;
		if (nl + 1U > size - NTFS_EA_HDR)
			return ST_IO;
		real = NTFS_EA_HDR + nl + 1U + vl;
		if (vl > size - NTFS_EA_HDR - nl - 1U ||
		    ((real + 3U) & ~3U) > ((size + 3U) & ~3U))
			return ST_IO;
		if (buf[o + NTFS_EA_HDR + nl] != 0)
			return ST_IO;
		v.name = (const char *)(buf + o + NTFS_EA_HDR);
		v.name_len = nl;
		v.value = buf + o + NTFS_EA_HDR + nl + 1;
		v.value_len = vl;
		v.flags = e->flags;
		r = cb(&v, arg);
		if (r < 0)
			return r;
		if (r > 0)
			return ST_OK;
		if (!e->next_entry_offset)
			break;
		o += size;
	}
	return ST_OK;
}

struct ntfs_ea_find {
	const char *name;
	uint32_t nlen;
	const ntfs_ea_view_t *hit;
	ntfs_ea_view_t copy;
};

static int ntfs_ea_find_cb(const ntfs_ea_view_t *ea, void *arg)
{
	struct ntfs_ea_find *f = (struct ntfs_ea_find *)arg;

	if (ea->name_len == f->nlen && kmemcmp(ea->name, f->name, f->nlen) == 0) {
		f->copy = *ea;
		f->hit = &f->copy;
		return 1;
	}
	return 0;
}

/* Find EA `name' in a loaded chain. */
int ntfs_ea_find(const uint8_t *buf, uint32_t qlen, const char *name,
		 ntfs_ea_view_t *out)
{
	struct ntfs_ea_find f;
	int r;

	f.name = name;
	f.nlen = (uint32_t)ntfs_strlen(name);
	f.hit = NULL;
	r = ntfs_ea_walk(buf, qlen, ntfs_ea_find_cb, &f);
	if (r != ST_OK)
		return r;
	if (!f.hit)
		return ST_NOT_FOUND;
	*out = f.copy;
	return ST_OK;
}

/* ===========================================================================
 * S18a  Extended attributes: loading $EA, the WSL metadata EAs
 *
 * The validated walk of the EA chain (ntfs_ea_walk(), ntfs_ea_find()) and
 * the rules it checks are in the section above (S18a).
 * =========================================================================== */

/* Load the in-use part of $EA of `mref'.  ST_OK with *buf NULL when the
 * file has no EAs. */
int ntfs_ea_load(ntfs_fs_t *fs, uint64_t mref, uint8_t **buf,
		 uint32_t *qlen)
{
	uint8_t *info = NULL, *ea = NULL;
	uint32_t ilen = 0, elen = 0, q;
	int r;

	*buf = NULL;
	*qlen = 0;
	r = ntfs_attr_readall(fs, mref, NTFS_AT_EA_INFORMATION, ntfs_unnamed,
			      0, 64, &info, &ilen);
	if (r == ST_NOT_FOUND)
		return ST_OK;
	if (r != ST_OK)
		return r;
	if (ilen < sizeof(ntfs_ea_information_t)) {
		kfree(info);
		return ST_IO;
	}
	q = ((const ntfs_ea_information_t *)info)->ea_query_length;
	kfree(info);
	if (q == 0)
		return ST_OK; /* nothing to list */
	if (q < NTFS_EA_HDR)
		return ST_IO;
	r = ntfs_attr_readall(fs, mref, NTFS_AT_EA, ntfs_unnamed, 0,
			      NTFS_EA_MAX, &ea, &elen);
	if (r == ST_NOT_FOUND)
		return ST_IO; /* one of the pair alone is corrupt */
	if (r != ST_OK)
		return r;
	if (q > elen) {
		kfree(ea);
		return ST_IO;
	}
	*buf = ea;
	*qlen = q;
	return ST_OK;
}

/* The WSL metadata EAs and the file-flags EA of an inode. */
int ntfs_inode_load_eas(ntfs_fs_t *fs, uint64_t mref, ntfs_inode_t *ino)
{
	uint8_t *buf;
	uint32_t q;
	ntfs_ea_view_t v;
	int r;

	r = ntfs_ea_load(fs, mref, &buf, &q);
	if (r != ST_OK || !buf)
		return r;
	if (ntfs_ea_find(buf, q, "$LXUID", &v) == ST_OK) {
		if (v.value_len != 4) {
			r = ST_IO;
			goto out;
		}
		ino->has_lxuid = 1;
		ino->lx_uid = ntfs_le32(v.value);
	}
	if (ntfs_ea_find(buf, q, "$LXGID", &v) == ST_OK) {
		if (v.value_len != 4) {
			r = ST_IO;
			goto out;
		}
		ino->has_lxgid = 1;
		ino->lx_gid = ntfs_le32(v.value);
	}
	if (ntfs_ea_find(buf, q, "$LXMOD", &v) == ST_OK) {
		if (v.value_len == 4) {
			ino->has_lxmod = 1;
			ino->lx_mode = ntfs_le32(v.value);
		} else {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: $LXMOD of "
					  "%u bytes ignored",
				       (unsigned long long)ino->mft_no,
				       v.value_len);
		}
	}
	if (ntfs_ea_find(buf, q, "$LXDEV", &v) == ST_OK) {
		if (v.value_len == 8) {
			ino->has_lxdev = 1;
			ino->lx_major = ntfs_le32(v.value);
			ino->lx_minor = ntfs_le32(v.value + 4);
		}
	}
	if (ntfs_ea_find(buf, q, "$LINUXFLAGS", &v) == ST_OK &&
	    v.value_len == 4) {
		ino->has_lflags = 1;
		ino->lflags = ntfs_le32(v.value);
	}
	r = ST_OK;
out:
	kfree(buf);
	return r;
}

/* ===========================================================================
 * S28  Extended attributes: rebuilding $EA / $EA_INFORMATION
 *
 * The EA pair is always rewritten as a whole: the complete new
 * chain is built in memory from the old one (validated by S18a's walk)
 * minus the entries replaced or removed plus the new ones, its size
 * checked against the 64 KB limit of $EA before anything changes,
 * and both attributes are set in the same editable inode, so they reach
 * the device in ONE record commit together (they can never be out of
 * step; one of the pair without the other is corruption, EUCLEAN =
 * ST_IO).  An empty chain removes both attributes.  Removing an
 * absent entry is ST_NODATA (the caller decides whether that is an
 * error); an empty value is a valid value.  Entries are FILE_FULL_EA
 * records: next-entry offset (4-byte aligned size), flags, name length,
 * value length, the name, a NUL, the value.  $EA_INFORMATION carries the
 * packed size (5 + name + value per entry), the count of entries flagged
 * NEED_EA and the size of the chain.
 *
 * It writes the WSL owner / mode / device EAs ($LXUID, $LXGID, $LXMOD,
 * $LXDEV) of create, chmod, chown and mknod; setxattr / removexattr and
 * the ACLs are built on it too.
 * =========================================================================== */

struct ntfs_ea_build {
	uint8_t *out;
	uint32_t len;
	uint32_t cap;
	uint32_t packed;
	uint32_t need;
	const ntfs_ea_op_t *ops;
	uint32_t nops;
	uint32_t *hit; /* ops whose name was found */
	int err;
};

static int ntfs_ea_emit(struct ntfs_ea_build *b, const char *name, uint32_t nl,
			const uint8_t *val, uint32_t vl, uint8_t flags)
{
	uint32_t size = (NTFS_EA_HDR + nl + 1U + vl + 3U) & ~3U;
	uint8_t *p;

	if (nl > 255 || vl > 0xFFFF || size > NTFS_EA_MAX ||
	    b->len + size > NTFS_EA_MAX)
		return ST_NOSPC; /* checked before anything changes */
	if (b->len + size > b->cap) {
		uint32_t nc = b->cap ? b->cap * 2 : 256;
		uint8_t *n;

		while (nc < b->len + size)
			nc *= 2;
		n = (uint8_t *)ntfs_zalloc(nc);
		if (!n)
			return ST_NOMEM;
		if (b->len)
			mm_memcpy(n, b->out, b->len);
		ntfs_free(b->out);
		b->out = n;
		b->cap = nc;
	}
	p = b->out + b->len;
	mm_memset(p, 0, size);
	ntfs_put_le32(p, size);
	p[4] = flags;
	p[5] = (uint8_t)nl;
	ntfs_put_le16(p + 6, (uint16_t)vl);
	mm_memcpy(p + NTFS_EA_HDR, name, nl);
	if (vl)
		mm_memcpy(p + NTFS_EA_HDR + nl + 1, val, vl);
	b->len += size;
	b->packed += 5U + nl + vl;
	if (flags & NTFS_EA_NEED_EA)
		b->need++;
	return ST_OK;
}

static int ntfs_ea_keep_cb(const ntfs_ea_view_t *ea, void *arg)
{
	struct ntfs_ea_build *b = (struct ntfs_ea_build *)arg;

	for (uint32_t i = 0; i < b->nops; i++)
		if (b->ops[i].nlen == ea->name_len &&
		    kmemcmp(b->ops[i].name, ea->name, ea->name_len) == 0) {
			b->hit[i] = 1;
			return 0; /* replaced or removed */
		}
	b->err = ntfs_ea_emit(b, ea->name, ea->name_len, ea->value,
			      ea->value_len, ea->flags);
	return b->err ? b->err : 0;
}

/* Apply `ops' to the EA pair of the set `e'.  The commit is the
 * caller's. */
int ntfs_ea_apply(ntfs_eino_t *e, const ntfs_ea_op_t *ops,
		  uint32_t nops)
{
	ntfs_fs_t *fs = e->fs;
	struct ntfs_ea_build b;
	uint8_t *info = NULL, *ea = NULL;
	uint32_t ilen = 0, elen = 0, q = 0;
	int ri, rea, r = ST_OK;
	uint8_t newinfo[8];

	mm_memset(&b, 0, sizeof(b));
	b.ops = ops;
	b.nops = nops;
	b.hit = (uint32_t *)ntfs_zalloc((nops + 1) * sizeof(uint32_t));
	if (!b.hit)
		return ST_NOMEM;
	ri = ntfs_eino_value(e, NTFS_AT_EA_INFORMATION, ntfs_unnamed, 0, 64,
			     &info, &ilen);
	rea = ntfs_eino_value(e, NTFS_AT_EA, ntfs_unnamed, 0, NTFS_EA_MAX, &ea,
			      &elen);
	if ((ri != ST_OK && ri != ST_NOT_FOUND) ||
	    (rea != ST_OK && rea != ST_NOT_FOUND)) {
		r = ri != ST_OK && ri != ST_NOT_FOUND ? ri : rea;
		goto out;
	}
	if ((ri == ST_OK) != (rea == ST_OK)) {
		r = ST_IO; /* one of the pair alone */
		goto out;
	}
	if (ri == ST_OK) {
		if (ilen < sizeof(ntfs_ea_information_t)) {
			r = ST_IO;
			goto out;
		}
		q = ((const ntfs_ea_information_t *)info)->ea_query_length;
		if (q > elen || (q && q < NTFS_EA_HDR)) {
			r = ST_IO;
			goto out;
		}
		r = ntfs_ea_walk(ea, q, ntfs_ea_keep_cb, &b);
		if (r == ST_OK)
			r = b.err;
		if (r != ST_OK)
			goto out;
	}
	for (uint32_t i = 0; i < nops; i++) {
		if (!ops[i].val) {
			if (!b.hit[i] && ops[i].must_exist) {
				r = ST_NODATA;
				goto out;
			}
			continue;
		}
		if (!b.hit[i] && ops[i].must_exist) {
			r = ST_NODATA;
			goto out;
		}
		r = ntfs_ea_emit(&b, ops[i].name, ops[i].nlen, ops[i].val,
				 ops[i].vlen, ops[i].flags);
		if (r != ST_OK)
			goto out;
	}
	if (b.packed > 0xFFFF) {
		r = ST_NOSPC;
		goto out;
	}
	if (!b.len) {
		/* The last entry removes the pair. */
		r = ntfs_eino_drop(e, NTFS_AT_EA, ntfs_unnamed, 0, 1);
		if (r == ST_OK)
			r = ntfs_eino_drop(e, NTFS_AT_EA_INFORMATION,
					   ntfs_unnamed, 0, 1);
		goto out;
	}
	ntfs_put_le16(newinfo, (uint16_t)b.packed);
	ntfs_put_le16(newinfo + 2, (uint16_t)b.need);
	ntfs_put_le32(newinfo + 4, b.len);
	r = ntfs_eino_set_value(e, NTFS_AT_EA, ntfs_unnamed, 0, b.out, b.len,
				fs->rec_size / 4);
	if (r == ST_OK)
		r = ntfs_eino_set_res(e, NTFS_AT_EA_INFORMATION, ntfs_unnamed, 0,
				      0, newinfo, 8);
out:
	ntfs_free(info);
	ntfs_free(ea);
	ntfs_free(b.out);
	ntfs_free(b.hit);
	return r;
}

/* The WSL metadata EAs: each `has_*' adds / replaces that entry. */
int ntfs_wsl_set(ntfs_eino_t *e, int has_uid, uint32_t uid,
		 int has_gid, uint32_t gid, int has_mode, uint32_t mode,
		 int has_dev, uint32_t major, uint32_t minor)
{
	ntfs_ea_op_t ops[4];
	uint8_t vu[4], vg[4], vm[4], vd[8];
	uint32_t n = 0;

	mm_memset(ops, 0, sizeof(ops));
	if (has_uid) {
		ntfs_put_le32(vu, uid);
		ops[n].name = "$LXUID";
		ops[n].nlen = 6;
		ops[n].val = vu;
		ops[n].vlen = 4;
		n++;
	}
	if (has_gid) {
		ntfs_put_le32(vg, gid);
		ops[n].name = "$LXGID";
		ops[n].nlen = 6;
		ops[n].val = vg;
		ops[n].vlen = 4;
		n++;
	}
	if (has_mode) {
		ntfs_put_le32(vm, mode);
		ops[n].name = "$LXMOD";
		ops[n].nlen = 6;
		ops[n].val = vm;
		ops[n].vlen = 4;
		n++;
	}
	if (has_dev) {
		ntfs_put_le32(vd, major);
		ntfs_put_le32(vd + 4, minor);
		ops[n].name = "$LXDEV";
		ops[n].nlen = 6;
		ops[n].val = vd;
		ops[n].vlen = 8;
		n++;
	}
	if (!n)
		return ST_OK;
	return ntfs_ea_apply(e, ops, n);
}
