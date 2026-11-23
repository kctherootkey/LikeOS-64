/* LikeOS - NTFS filesystem driver
 *
 * Extended attribute views: user.*, system.*, streams, the label.
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
 * S18b  Extended attribute names (xattr view)
 *
 *   EA "user.x" / "system.x" / "security.x" / "trusted.x"
 *                        -> exposed under its own name;
 *   EA "$LXUID", "$LXGID", "$LXMOD", "$LXDEV", the file-flags EA
 *                        -> internal, never listed;
 *   any other EA "X"     -> "user.X";
 *   alternate data stream "s" -> "user.s" (an EA of that name wins);
 *   system.posix_acl_access / _default -> EAs of those names, with `acl';
 *   system.ntfs_security, system.ntfs_attrib, system.ntfs_attrib_be,
 *   system.dos_attrib, system.ntfs_crtime, system.ntfs_crtime_be,
 *   system.ntfs_reparse_data -> computed (getxattr only, not listed).
 * =========================================================================== */

int ntfs_ea_reserved(const char *name, uint32_t nl)
{
	static const char *const res[] = { "$LXUID", "$LXGID", "$LXMOD",
					   "$LXDEV", "$LINUXFLAGS" };

	for (unsigned i = 0; i < sizeof(res) / sizeof(res[0]); i++)
		if (ntfs_strlen(res[i]) == nl && kmemcmp(res[i], name, nl) == 0)
			return 1;
	return 0;
}

int ntfs_has_xattr_prefix(const char *name, uint32_t nl)
{
	static const char *const pfx[] = { "user.", "system.", "security.",
					   "trusted." };

	for (unsigned i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++) {
		uint32_t l = (uint32_t)ntfs_strlen(pfx[i]);

		if (nl > l && kmemcmp(pfx[i], name, l) == 0)
			return 1;
	}
	return 0;
}

/* The xattr name of an EA ("" when hidden) into `out' (256 bytes). */
static uint32_t ntfs_ea_xname(const ntfs_fs_t *fs, const ntfs_ea_view_t *ea,
			      char *out)
{
	uint32_t o = 0;

	if (ea->name_len == 0 || ntfs_ea_reserved(ea->name, ea->name_len))
		return 0;
	if (ntfs_has_xattr_prefix(ea->name, ea->name_len)) {
		/* POSIX ACLs are shown only with the `acl' option. */
		if (!fs->opt.acl &&
		    ((ea->name_len == 23 &&
		      kmemcmp(ea->name, "system.posix_acl_access", 23) == 0) ||
		     (ea->name_len == 24 &&
		      kmemcmp(ea->name, "system.posix_acl_default", 24) == 0)))
			return 0;
		mm_memcpy(out, ea->name, ea->name_len);
		return ea->name_len;
	}
	mm_memcpy(out, "user.", 5);
	o = 5;
	mm_memcpy(out + o, ea->name, ea->name_len);
	return o + ea->name_len;
}

int ntfs_xlist_add(struct ntfs_xlist *x, const char *n, uint32_t len)
{
	if (x->list && x->size) {
		if (x->used + len + 1 > x->size)
			return ST_RANGE;
		mm_memcpy(x->list + x->used, n, len);
		x->list[x->used + len] = '\0';
	}
	x->used += len + 1;
	return 0;
}

int ntfs_xlist_ea_cb(const ntfs_ea_view_t *ea, void *arg)
{
	struct ntfs_xlist *x = (struct ntfs_xlist *)arg;
	uint32_t n = ntfs_ea_xname(x->fs, ea, x->name);

	if (!n)
		return 0;
	return ntfs_xlist_add(x, x->name, n);
}

static int ntfs_xattr_copy(void *val, unsigned long size, const void *src,
			   unsigned long len);

/* ---- the volume label ----
 *
 * The label is the $VOLUME_NAME attribute of $Volume (UTF-16, at most 128
 * units).  The VFS has no label ioctl, so it is offered as the xattr
 * "system.ntfs_volume_label" of the root directory: getxattr returns the
 * UTF-8 label (possibly empty), setxattr replaces it (root only), and
 * removexattr empties it.
 *
 * Order: the new in-memory copy and the UTF-16 value
 * are prepared before anything is touched (ENOMEM changes nothing); the
 * record of $Volume is loaded through the editable-inode layer, which
 * fails on any lookup error instead of treating a corrupt record as "no
 * label" (a second $VOLUME_NAME is never added: set_res replaces the one
 * attribute of that type); only after the record was committed is the
 * in-memory label swapped, under fs->sf_lock, and readers copy it under
 * the same lock. */
#define NTFS_LABEL_MAX 128 /* UTF-16 units */
#define NTFS_LABEL_XATTR "system.ntfs_volume_label"

static int ntfs_label_get(ntfs_fs_t *fs, void *val, unsigned long size)
{
	char *snap = (char *)kalloc(NTFS_LABEL_MAX * 3 + 1);
	unsigned long n = 0;
	uint64_t fl;
	int r;

	if (!snap)
		return ST_NOMEM;
	spin_lock_irqsave(&fs->sf_lock, &fl);
	if (fs->label) {
		while (fs->label[n] && n < NTFS_LABEL_MAX * 3) {
			snap[n] = fs->label[n];
			n++;
		}
	}
	spin_unlock_irqrestore(&fs->sf_lock, fl);
	r = ntfs_xattr_copy(val, size, snap, n);
	kfree(snap);
	return r;
}

static int ntfs_label_set(ntfs_fs_t *fs, const void *val, unsigned long size)
{
	uint16_t *u = NULL;
	char *copy = NULL, *old;
	long n = 0;
	uint32_t uid, gid;
	ntfs_eino_t *e = NULL;
	uint64_t fl;
	int r, n2;

	ntfs_assert_meta_excl(fs);
	ntfs_creds(&uid, &gid);
	if (uid != 0)
		return ST_PERM; /* the volume's name is the administrator's */
	if (size > NTFS_LABEL_MAX * 4)
		return ST_RANGE;
	u = (uint16_t *)kalloc(NTFS_LABEL_MAX * 2 + 2);
	copy = (char *)kalloc(size + 1);
	if (!u || !copy) {
		r = ST_NOMEM;
		goto out;
	}
	if (size) {
		n = ntfs_utf8_to_utf16((const char *)val, size, u,
				       NTFS_LABEL_MAX);
		if (n < 0) {
			r = n == ST_NAMETOOLONG ? ST_RANGE : ST_INVALID;
			goto out;
		}
		mm_memcpy(copy, val, size);
	}
	copy[size] = '\0';
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		goto out;
	/* The same name again: nothing to write (no arming either). */
	spin_lock_irqsave(&fs->sf_lock, &fl);
	old = fs->label;
	n2 = (old ? kstrlen(old) : 0) == size &&
	     (!size || kmemcmp(old, copy, size) == 0);
	spin_unlock_irqrestore(&fs->sf_lock, fl);
	if (n2)
		goto out;
	/* Arm first: arming rewrites record 3 (the dirty flag), and the copy
	 * of record 3 loaded below must already show that flag, or its write
	 * would store the volume as clean again. */
	r = ntfs_arm(fs);
	if (r != ST_OK)
		goto out;
	r = ntfs_eino_load(fs, NTFS_MAKE_MREF(NTFS_FILE_VOLUME, 0), &e);
	if (r != ST_OK)
		goto out;
	r = ntfs_eino_set_res(e, NTFS_AT_VOLUME_NAME, ntfs_unnamed, 0, 0, u,
			      (uint32_t)n * 2U);
	if (r == ST_OK)
		r = ntfs_eino_commit(e);
	ntfs_eino_free(e);
	if (r != ST_OK)
		goto out;
	spin_lock_irqsave(&fs->sf_lock, &fl);
	old = fs->label;
	fs->label = copy;
	spin_unlock_irqrestore(&fs->sf_lock, fl);
	copy = old;
out:
	ntfs_free(u);
	ntfs_free(copy);
	return r;
}

/* ---- user extended attributes (EAs) ----
 *
 * user.* xattrs are EAs, stored under their full name (an existing EA
 * stored without the prefix is found and replaced too).  The reserved WSL
 * names are never written through here.  Other namespaces --
 * security descriptors, POSIX ACLs, system.* attributes -- are written
 * through the section below.  ctime changes only when the change
 * succeeded. */
#define NTFS_XATTR_CREATE 1
#define NTFS_XATTR_REPLACE 2

/* ---- system.* and security.* xattr writes ----
 *
 *   system.ntfs_security      the file's security descriptor (validated,
 *                             stored through $Secure on NTFS 3.x);
 *   system.ntfs_attrib(_be)   the file attribute flags (32 bits, little /
 *   system.dos_attrib         big endian; dos_attrib: the low 8 bits);
 *   system.ntfs_crtime(_be)   the creation time (NTFS time, 64 bits);
 *   system.posix_acl_access   POSIX ACLs (mount option `acl'), stored as
 *   system.posix_acl_default  EAs of these names; the access ACL also sets
 *                             the mode ($LXMOD);
 *   security.* / trusted.*    plain EAs (trusted.*: root only).
 *
 * The system.* attributes need the file's owner or root (a
 * non-owner with write access must not change what the mode bits mean).
 * Every change is one commit of the file's records; ctime changes only
 * when it succeeded.  The attribute flags a caller may change are
 * READONLY, HIDDEN, SYSTEM, ARCHIVE, TEMPORARY, OFFLINE,
 * NOT_CONTENT_INDEXED, and COMPRESSED / SPARSE through
 * ntfs_set_data_flags(); the rest describe the file's structure and keep
 * their values. */
#define NTFS_FATTR_SETTABLE                                                  \
	(NTFS_FILE_ATTR_READONLY | NTFS_FILE_ATTR_HIDDEN |                   \
	 NTFS_FILE_ATTR_SYSTEM | NTFS_FILE_ATTR_ARCHIVE |                    \
	 NTFS_FILE_ATTR_TEMPORARY | NTFS_FILE_ATTR_OFFLINE |                 \
	 NTFS_FILE_ATTR_NOT_CONTENT_INDEXED)

/* The current value of EA `name' of `mref' (kalloc'd copy, *len), or
 * NULL (*len 0) when it has none. */
static int ntfs_ea_value_copy(ntfs_fs_t *fs, uint64_t mref, const char *name,
			      uint8_t **out, uint32_t *len)
{
	uint8_t *buf = NULL;
	uint32_t q = 0;
	ntfs_ea_view_t v;
	int r;

	*out = NULL;
	*len = 0;
	r = ntfs_ea_load(fs, mref, &buf, &q);
	if (r != ST_OK || !buf)
		return r;
	r = ntfs_ea_find(buf, q, name, &v);
	if (r == ST_OK) {
		*out = (uint8_t *)kalloc(v.value_len ? v.value_len : 1);
		if (*out) {
			mm_memcpy(*out, v.value, v.value_len);
			*len = v.value_len;
		} else {
			r = ST_NOMEM;
		}
	} else if (r == ST_NOT_FOUND) {
		r = ST_OK;
	}
	kfree(buf);
	return r;
}

/* The caller may change system attributes of `ino': its owner or root. */
static int ntfs_owner_ok(ntfs_fs_t *fs, const ntfs_inode_t *ino)
{
	uint32_t uid, gid, owner;

	ntfs_creds(&uid, &gid);
	owner = (ino->has_lxuid && !fs->opt.uid_set) ? ino->lx_uid :
						      fs->opt.uid;
	return uid == 0 || uid == owner;
}

/* With `acl', chmod also rewrites the access ACL's base entries (the
 * ACL and $LXMOD change in the same commit, so they never disagree). */
int ntfs_acl_chmod(ntfs_fs_t *fs, ntfs_eino_t *e, uint64_t mref,
		   uint32_t mode)
{
	uint8_t *acl = NULL;
	uint32_t alen = 0;
	ntfs_ea_op_t op;
	int r, equiv;

	if (!fs->opt.acl)
		return ST_OK;
	r = ntfs_ea_value_copy(fs, mref, "system.posix_acl_access", &acl,
			       &alen);
	if (r != ST_OK || !acl)
		return r;
	if (!ntfs_pacl_valid(acl, alen, 0, &equiv)) {
		kfree(acl);
		return ST_OK; /* not ours to repair: left as it is */
	}
	ntfs_pacl_apply_mode(acl, alen, mode, 0);
	mm_memset(&op, 0, sizeof(op));
	op.name = "system.posix_acl_access";
	op.nlen = 23;
	op.val = acl;
	op.vlen = alen;
	r = ntfs_ea_apply(e, &op, 1);
	kfree(acl);
	return r;
}

/* ACL inheritance for a new file in directory `dir': the
 * directory's default ACL, if any, becomes the new file's access ACL with
 * the base entries limited by the creation mode, and a new directory
 * inherits the default ACL itself.  *mode is updated to what the access
 * ACL stands for.  Runs on the new record's editable inode before its
 * first commit, so the file never exists without them. */
int ntfs_acl_inherit(ntfs_fs_t *fs, ntfs_eino_t *e, uint64_t dir,
		     int is_dir, uint32_t *mode, int *changed)
{
	uint8_t *def = NULL;
	uint32_t dlen = 0;
	ntfs_ea_op_t ops[2];
	uint32_t nops = 0;
	int r, equiv;

	*changed = 0;
	if (!fs->opt.acl)
		return ST_OK;
	r = ntfs_ea_value_copy(fs, dir, "system.posix_acl_default", &def,
			       &dlen);
	if (r != ST_OK || !def)
		return r;
	if (!ntfs_pacl_valid(def, dlen, 0, &equiv)) {
		kfree(def);
		return ST_OK;
	}
	mm_memset(ops, 0, sizeof(ops));
	if (is_dir) {
		ops[nops].name = "system.posix_acl_default";
		ops[nops].nlen = 24;
		ops[nops].val = def;
		ops[nops].vlen = dlen;
		nops++;
	}
	{
		uint8_t *acc = (uint8_t *)kalloc(dlen);

		if (!acc) {
			kfree(def);
			return ST_NOMEM;
		}
		mm_memcpy(acc, def, dlen);
		ntfs_pacl_apply_mode(acc, dlen, *mode, 1);
		*mode = (*mode & ~0777U) | ntfs_pacl_mode(acc, dlen);
		if (!equiv) {
			ops[nops].name = "system.posix_acl_access";
			ops[nops].nlen = 23;
			ops[nops].val = acc;
			ops[nops].vlen = dlen;
			nops++;
		}
		r = nops ? ntfs_ea_apply(e, ops, nops) : ST_OK;
		kfree(acc);
	}
	kfree(def);
	*changed = 1;
	return r;
}

/* Set / clear the compressed and sparse flags of the unnamed $DATA of
 * `e'.  Only an empty stream changes: a resident empty value becomes an
 * empty non-resident attribute with the flags (compression unit 16
 * clusters when compressed); non-empty data
 * keeps its layout (EOPNOTSUPP), compression needs clusters of at most
 * 4 KB.  The attribute record is rebuilt whole by the
 * editable-inode layer, so no header field is moved in place (the
 * overlap and ordering bugs of in-place header edits cannot happen).
 * `st' (when the stream is in memory) is updated after the commit by the
 * caller. */
int ntfs_set_data_flags(ntfs_fs_t *fs, ntfs_eino_t *e, int comp,
			int sparse)
{
	long i = ntfs_eino_find(e, NTFS_AT_DATA, ntfs_unnamed, 0, 0);
	ntfs_attr_record_t *a;
	ntfs_runlist_t empty;
	uint16_t fl = 0;

	if (i < 0)
		return ST_UNSUPPORTED; /* a directory or special file */
	a = ntfs_ea_hdr(&e->a[i]);
	if (comp && fs->cluster_size > NTFS_COMPRESS_MAX_CLUSTER)
		return ST_UNSUPPORTED;
	if (a->non_resident) {
		uint16_t cur = a->flags & (NTFS_ATTR_COMPRESSION_MASK |
					   NTFS_ATTR_IS_SPARSE);

		fl = (comp ? NTFS_ATTR_IS_COMPRESSED : 0) |
		     (sparse ? NTFS_ATTR_IS_SPARSE : 0);
		if (cur == fl)
			return ST_OK; /* nothing to change */
		if (a->data.non_resident.data_size ||
		    a->data.non_resident.allocated_size ||
		    a->data.non_resident.lowest_vcn)
			return ST_UNSUPPORTED;
	} else {
		if (!comp && !sparse)
			return ST_OK; /* resident data has no such flags */
		if (a->data.resident.value_length)
			return ST_UNSUPPORTED;
		fl = (comp ? NTFS_ATTR_IS_COMPRESSED : 0) |
		     (sparse ? NTFS_ATTR_IS_SPARSE : 0);
	}
	if (!fl) {
		/* Back to a plain empty attribute: resident, as new files
		 * start. */
		return ntfs_eino_set_res(e, NTFS_AT_DATA, ntfs_unnamed, 0, 0,
					 NULL, 0);
	}
	mm_memset(&empty, 0, sizeof(empty));
	return ntfs_eino_set_nonres(e, NTFS_AT_DATA, ntfs_unnamed, 0, fl,
				    comp ? NTFS_COMPRESSION_UNIT : 0, &empty, 0,
				    0, 0, 0);
}

/* The in-memory stream of an empty file whose flags changed: drop it
 * from the cache, so that the next open reads the new attribute.  An open
 * stream is updated in place (it is empty: no page, no run to keep). */
static void ntfs_data_flags_changed(ntfs_fs_t *fs, uint64_t mref, int comp,
				    int sparse)
{
	ntfs_stream_t *st = NULL;
	uint64_t fl;

	spin_lock_irqsave(&fs->hdl_lock, &fl);
	st = ntfs_stream_find_locked(fs, NTFS_MREF_NO(mref), NTFS_MREF_SEQ(mref),
				     NTFS_AT_DATA, ntfs_unnamed, 0, 0);
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	if (!st)
		return;
	if (st->data_size == 0 && !st->dl_n) {
		pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
		ntfs_free(st->res_data);
		st->res_data = NULL;
		st->res_len = 0;
		ntfs_rl_free(&st->rl);
		mm_memset(&st->rl, 0, sizeof(st->rl));
		st->flags &= ~(NTFS_ST_RESIDENT | NTFS_ST_COMPRESSED |
			       NTFS_ST_SPARSE | NTFS_ST_WASRES);
		st->data_size = st->init_size = st->alloc_size = 0;
		st->comp_size = 0;
		st->comp_unit = 0;
		st->cu_bytes = 0;
		if (comp) {
			st->flags |= NTFS_ST_COMPRESSED;
			st->comp_unit = NTFS_COMPRESSION_UNIT;
			st->cu_bytes = fs->cluster_size << NTFS_COMPRESSION_UNIT;
		}
		if (sparse)
			st->flags |= NTFS_ST_SPARSE;
		if (!comp && !sparse)
			st->flags |= NTFS_ST_RESIDENT;
	}
	ntfs_stream_put(fs, st);
}

/* system.* writes on file `mref' (metadata lock exclusive).  `remove':
 * removexattr.  ST_UNSUPPORTED: not one of these names. */
static int ntfs_sysxattr_set(ntfs_fs_t *fs, uint64_t mref, const char *name,
			     const uint8_t *val, unsigned long size, int flags,
			     int remove)
{
	ntfs_inode_t *ino = NULL;
	ntfs_eino_t *e = NULL;
	ntfs_standard_information_t *si;
	int r, is_attrib = 0, be = 0, is_crtime = 0, is_sec = 0, acl = 0;
	int data_flags = 0, comp = 0, sparse = 0, touch_dups = 0;

	if (ntfs_streq(name, "system.ntfs_attrib"))
		is_attrib = 1;
	else if (ntfs_streq(name, "system.ntfs_attrib_be"))
		is_attrib = be = 1;
	else if (ntfs_streq(name, "system.dos_attrib"))
		is_attrib = 2;
	else if (ntfs_streq(name, "system.ntfs_crtime"))
		is_crtime = 1;
	else if (ntfs_streq(name, "system.ntfs_crtime_be"))
		is_crtime = be = 1;
	else if (ntfs_streq(name, "system.ntfs_security"))
		is_sec = 1;
	else if (ntfs_streq(name, "system.posix_acl_access"))
		acl = 1;
	else if (ntfs_streq(name, "system.posix_acl_default"))
		acl = 2;
	else
		return ST_UNSUPPORTED;
	if (acl && !fs->opt.acl)
		return ST_UNSUPPORTED;
	if (remove && !acl)
		return ST_PERM; /* computed attributes always exist */
	if ((flags & NTFS_XATTR_CREATE) && !acl)
		return ST_EXISTS;
	if (NTFS_MREF_NO(mref) < NTFS_FILE_FIRST_USER &&
	    NTFS_MREF_NO(mref) != NTFS_FILE_ROOT)
		return ST_PERM;
	ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	if (!ino)
		return ST_NOMEM;
	r = ntfs_inode_load(fs, mref, ino);
	if (r != ST_OK)
		goto out;
	if (!ntfs_owner_ok(fs, ino)) {
		r = ST_PERM;
		goto out;
	}
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		goto out;
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		goto out;
	si = ntfs_eino_si(e);
	if (!si) {
		r = ntfs_fs_error(fs, NTFS_MREF_NO(mref),
				  "no $STANDARD_INFORMATION");
		goto out;
	}
	if (is_attrib) {
		uint32_t want, cur = si->file_attributes, nw;

		if (is_attrib == 2) {
			if (size != 1 && size != 4) {
				r = ST_INVALID;
				goto out;
			}
			want = (cur & ~0xFFU) | val[0];
		} else {
			if (size != 4) {
				r = ST_INVALID;
				goto out;
			}
			want = be ? ((uint32_t)val[0] << 24 |
				     (uint32_t)val[1] << 16 |
				     (uint32_t)val[2] << 8 | val[3]) :
				    ntfs_le32(val);
		}
		nw = (cur & ~NTFS_FATTR_SETTABLE) | (want & NTFS_FATTR_SETTABLE);
		if (ino->is_dir)
			nw &= ~NTFS_FILE_ATTR_READONLY;
		comp = (want & NTFS_FILE_ATTR_COMPRESSED) != 0;
		sparse = (want & NTFS_FILE_ATTR_SPARSE_FILE) != 0;
		if (comp != !!(cur & NTFS_FILE_ATTR_COMPRESSED) ||
		    (!ino->is_dir &&
		     sparse != !!(cur & NTFS_FILE_ATTR_SPARSE_FILE))) {
			if (ino->is_dir) {
				/* A directory's flag is what new files in it
				 * get (S31). */
				if (comp && fs->cluster_size >
						    NTFS_COMPRESS_MAX_CLUSTER) {
					r = ST_UNSUPPORTED;
					goto out;
				}
			} else {
				if (ino->has_reparse ||
				    (ino->fattr & NTFS_FILE_ATTR_ENCRYPTED)) {
					r = ST_UNSUPPORTED;
					goto out;
				}
				r = ntfs_set_data_flags(fs, e, comp, sparse);
				if (r != ST_OK)
					goto out;
				data_flags = 1;
			}
			nw = (nw & ~(NTFS_FILE_ATTR_COMPRESSED |
				     NTFS_FILE_ATTR_SPARSE_FILE)) |
			     (comp ? NTFS_FILE_ATTR_COMPRESSED : 0) |
			     (!ino->is_dir && sparse ?
				      NTFS_FILE_ATTR_SPARSE_FILE :
				      0);
		}
		if (nw == cur && !data_flags) {
			r = ST_OK; /* nothing changes: no write */
			goto out;
		}
		si = ntfs_eino_si(e);
		if (!si) {
			r = ST_IO;
			goto out;
		}
		si->file_attributes = nw;
		touch_dups = 1;
	} else if (is_crtime) {
		uint64_t t = 0;

		if (size != 8) {
			r = ST_INVALID;
			goto out;
		}
		for (int i = 0; i < 8; i++)
			t |= (uint64_t)val[be ? 7 - i : i] << (8 * i);
		if ((int64_t)t < 0) {
			r = ST_INVALID;
			goto out;
		}
		si->creation_time = (int64_t)t;
		touch_dups = 1;
	} else if (is_sec) {
		if (size > NTFS_SD_MAX ||
		    !ntfs_sd_valid(val, (uint32_t)size)) {
			r = ST_INVALID;
			goto out;
		}
		r = ntfs_eino_set_sd(e, val, (uint32_t)size);
		if (r != ST_OK)
			goto out;
	} else {
		/* POSIX ACLs. */
		ntfs_ea_op_t op;
		int equiv = 0;
		uint32_t cur = ntfs_inode_mode(fs, ino), nm = cur;

		if (acl == 2 && !ino->is_dir) {
			r = remove ? ST_NODATA : ST_PERM;
			goto out;
		}
		if (!remove && !ntfs_pacl_valid(val, (uint32_t)size, acl == 2,
						&equiv)) {
			r = ST_INVALID;
			goto out;
		}
		mm_memset(&op, 0, sizeof(op));
		op.name = name;
		op.nlen = (uint32_t)ntfs_strlen(name);
		op.val = remove ? NULL : val;
		op.vlen = remove ? 0 : (uint32_t)size;
		op.must_exist = remove;
		if (acl == 1 && !remove) {
			uint8_t *tmp = (uint8_t *)kalloc(size);

			if (!tmp) {
				r = ST_NOMEM;
				goto out;
			}
			mm_memcpy(tmp, val, size);
			nm = (cur & ~0777U) | ntfs_pacl_mode(tmp, (uint32_t)size);
			kfree(tmp);
			if (equiv) {
				/* The mode says it all: no ACL stored. */
				op.val = NULL;
				op.vlen = 0;
				op.must_exist = 0;
			}
		}
		if (acl == 2 && !remove && size == 4) {
			op.val = NULL; /* an empty default ACL: none */
			op.vlen = 0;
		}
		if ((flags & NTFS_XATTR_CREATE) && !remove) {
			uint8_t *old = NULL;
			uint32_t ol = 0;

			r = ntfs_ea_value_copy(fs, mref, name, &old, &ol);
			ntfs_free(old);
			if (r == ST_OK && old)
				r = ST_EXISTS;
			if (r != ST_OK)
				goto out;
		}
		r = ntfs_ea_apply(e, &op, 1);
		si = ntfs_eino_si(e);
		if (r == ST_OK && !si)
			r = ST_IO;
		if (r == ST_OK && nm != cur &&
		    (cur & S_IFMT) != S_IFLNK) {
			if (!ino->is_dir) {
				if (nm & 0222)
					si->file_attributes &=
						~NTFS_FILE_ATTR_READONLY;
				else
					si->file_attributes |=
						NTFS_FILE_ATTR_READONLY;
			}
			r = ntfs_wsl_set(e, 0, 0, 0, 0, 1, nm, 0, 0, 0);
		}
		if (r != ST_OK)
			goto out;
	}
	si = ntfs_eino_si(e); /* the editable inode may have moved it */
	if (si)
		si->last_mft_change_time = ntfs_now();
	r = ntfs_eino_commit(e);
	if (r == ST_OK) {
		ntfs_eino_free(e);
		e = NULL;
		if (data_flags)
			ntfs_data_flags_changed(fs, mref, comp, sparse);
		if (touch_dups)
			ntfs_update_dups(fs, mref);
	}
out:
	ntfs_eino_free(e);
	ntfs_free(ino);
	return r;
}

/* ---- the xattr view of named streams ----
 *
 * getxattr("user.X") falls back to the named stream X when the file has no
 * EA of that name; the write side matches it: setxattr replaces the
 * stream's contents, removexattr removes the stream.  A name that is
 * neither an EA nor a stream becomes a new EA (creating streams is the
 * "file:stream" path's job).  Writing the contents needs the file lock,
 * which ranks above the metadata lock: ntfs_setxattr_mref() answers
 * NTFS_XATTR_IS_STREAM and the caller rewrites the stream through
 * ntfs_xattr_stream_write() with the locks taken in order. */
#define NTFS_XATTR_IS_STREAM 1

/* Does `mref' have a stream named `sname' (UTF-8)?  ST_NOT_FOUND when it
 * does not; with `remove' the stream is removed here; otherwise
 * NTFS_XATTR_IS_STREAM (or ST_EXISTS for XATTR_CREATE). */
static int ntfs_xattr_stream_probe(ntfs_fs_t *fs, uint64_t mref,
				   const char *sname, int remove, int flags)
{
	uint16_t *u = (uint16_t *)kalloc(NTFS_MAX_NAME_LEN * 2);
	ntfs_stream_t *st = NULL;
	long n;
	int r;

	if (!u)
		return ST_NOMEM;
	n = ntfs_utf8_to_utf16(sname, ntfs_strlen(sname), u,
			       NTFS_MAX_NAME_LEN);
	if (n <= 0) {
		kfree(u);
		return ST_NOT_FOUND;
	}
	r = ntfs_stream_get(fs, mref, NTFS_AT_DATA, u, (uint32_t)n, 0, &st);
	if (r == ST_OK) {
		ntfs_stream_put(fs, st);
		if (remove)
			r = ntfs_stream_remove(fs, mref, u, (uint32_t)n, 0);
		else if (flags & NTFS_XATTR_CREATE)
			r = ST_EXISTS;
		else
			r = NTFS_XATTR_IS_STREAM;
	}
	kfree(u);
	return r;
}

/* Replace the contents of stream `sname' of `mref' with `val' (`size'
 * bytes, kernel memory).  Takes the file lock and the metadata lock. */
static int ntfs_xattr_stream_write(ntfs_fs_t *fs, uint64_t mref,
				   const char *sname, const void *val,
				   unsigned long size)
{
	uint16_t *u = (uint16_t *)kalloc(NTFS_MAX_NAME_LEN * 2);
	ntfs_stream_t *st = NULL;
	uint64_t no = NTFS_MREF_NO(mref);
	long n, w;
	int r;

	if (!u)
		return ST_NOMEM;
	n = ntfs_utf8_to_utf16(sname, ntfs_strlen(sname), u,
			       NTFS_MAX_NAME_LEN);
	if (n <= 0) {
		kfree(u);
		return ST_NODATA;
	}
	ntfs_wlock_file(fs, no);
	r = ntfs_stream_get(fs, mref, NTFS_AT_DATA, u, (uint32_t)n, 0, &st);
	if (r == ST_NOT_FOUND)
		r = ST_NODATA; /* removed meanwhile */
	if (r == ST_OK)
		r = ntfs_write_begin(fs);
	if (r == ST_OK)
		r = ntfs_stream_truncate(fs, st, 0);
	if (r == ST_OK && size) {
		w = ntfs_stream_write(fs, st, 0, (const uint8_t *)val, size);
		if (w < 0)
			r = (int)w;
		else if ((unsigned long)w != size)
			r = ST_NOSPC;
	}
	if (r == ST_OK)
		r = ntfs_stream_commit(fs, st);
	if (st)
		ntfs_stream_put(fs, st);
	ntfs_wunlock_file(fs, no);
	kfree(u);
	return r;
}

/* An EA stored under its full xattr name (security.*, trusted.*). */
static int ntfs_ea_xattr_set(ntfs_fs_t *fs, uint64_t mref, const char *name,
			     uint32_t nl, const void *val, unsigned long size,
			     int flags, int remove)
{
	uint8_t *old = NULL;
	uint32_t ol = 0;
	ntfs_eino_t *e = NULL;
	ntfs_ea_op_t op;
	int r;

	if (nl > 255 || (!remove && size > 0xFFFF))
		return ST_RANGE;
	if (NTFS_MREF_NO(mref) < NTFS_FILE_FIRST_USER &&
	    NTFS_MREF_NO(mref) != NTFS_FILE_ROOT)
		return ST_PERM;
	r = ntfs_ea_value_copy(fs, mref, name, &old, &ol);
	if (r != ST_OK)
		return r;
	ntfs_free(old);
	if ((remove || (flags & NTFS_XATTR_REPLACE)) && !old)
		return ST_NODATA;
	if ((flags & NTFS_XATTR_CREATE) && old)
		return ST_EXISTS;
	mm_memset(&op, 0, sizeof(op));
	op.name = name;
	op.nlen = nl;
	op.val = remove ? NULL : (const uint8_t *)(val ? val : "");
	op.vlen = remove ? 0 : (uint32_t)size;
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		return r;
	r = ntfs_ea_apply(e, &op, 1);
	if (r == ST_OK) {
		ntfs_standard_information_t *si = ntfs_eino_si(e);

		if (si)
			si->last_mft_change_time = ntfs_now();
		r = ntfs_eino_commit(e);
	}
	ntfs_eino_free(e);
	return r;
}

static int ntfs_setxattr_mref(ntfs_fs_t *fs, uint64_t mref, const char *name,
			      const void *val, unsigned long size, int flags,
			      int remove)
{
	uint32_t nl = (uint32_t)ntfs_strlen(name);
	uint8_t *buf = NULL;
	uint32_t q = 0;
	ntfs_ea_view_t v;
	int full = 0, bare = 0, r;
	ntfs_eino_t *e = NULL;
	ntfs_ea_op_t ops[2];
	uint32_t nops = 0;

	if (ntfs_streq(name, NTFS_LABEL_XATTR)) {
		if (NTFS_MREF_NO(mref) != NTFS_FILE_ROOT)
			return ST_UNSUPPORTED;
		if (flags & NTFS_XATTR_CREATE)
			return ST_EXISTS; /* the label always exists */
		return ntfs_label_set(fs, remove ? NULL : val,
				      remove ? 0 : size);
	}
	if (ntfs_protected(fs, mref))
		return ST_PERM; /* the volume's own metadata */
	r = ntfs_sysxattr_set(fs, mref, name, (const uint8_t *)val, size, flags,
			      remove);
	if (r != ST_UNSUPPORTED)
		return r;
	if (ntfs_strneq(name, "security.", 9) && nl > 9) {
		/* security.* and trusted.* are plain EAs of their full
		 * names (trusted.*: root only). */
		return ntfs_ea_xattr_set(fs, mref, name, nl, val, size, flags,
					 remove);
	}
	if (ntfs_strneq(name, "trusted.", 8) && nl > 8) {
		uint32_t uid, gid;

		ntfs_creds(&uid, &gid);
		if (uid != 0)
			return ST_PERM;
		return ntfs_ea_xattr_set(fs, mref, name, nl, val, size, flags,
					 remove);
	}
	if (!ntfs_strneq(name, "user.", 5) || nl <= 5)
		return ST_UNSUPPORTED;
	if (nl > 255 || (!remove && size > 0xFFFF))
		return ST_RANGE;
	if (ntfs_ea_reserved(name + 5, nl - 5))
		return ST_PERM;
	if (NTFS_MREF_NO(mref) < NTFS_FILE_FIRST_USER &&
	    NTFS_MREF_NO(mref) != NTFS_FILE_ROOT)
		return ST_PERM;
	r = ntfs_ea_load(fs, mref, &buf, &q);
	if (r != ST_OK)
		return r;
	if (buf) {
		full = ntfs_ea_find(buf, q, name, &v) == ST_OK;
		bare = ntfs_ea_find(buf, q, name + 5, &v) == ST_OK;
	}
	ntfs_free(buf);
	if (!full && !bare) {
		/* Not an EA: is it the xattr view of a named stream
		 * ("user.<stream>", the read side's fallback)?  Then the
		 * stream is replaced or removed. */
		r = ntfs_xattr_stream_probe(fs, mref, name + 5, remove, flags);
		if (r != ST_NOT_FOUND)
			return r;
		if (remove || (flags & NTFS_XATTR_REPLACE))
			return ST_NODATA;
	}
	if ((flags & NTFS_XATTR_CREATE) && (full || bare))
		return ST_EXISTS;
	mm_memset(ops, 0, sizeof(ops));
	if (bare) {
		ops[nops].name = name + 5;
		ops[nops].nlen = nl - 5;
		nops++; /* replaced by the full name, or removed */
	}
	ops[nops].name = name;
	ops[nops].nlen = nl;
	ops[nops].val = remove ? NULL : (const uint8_t *)(val ? val : "");
	ops[nops].vlen = remove ? 0 : (uint32_t)size;
	nops++;
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		return r;
	r = ntfs_ea_apply(e, ops, nops);
	if (r == ST_OK) {
		ntfs_standard_information_t *si = ntfs_eino_si(e);

		if (si)
			si->last_mft_change_time = ntfs_now();
		r = ntfs_eino_commit(e);
	}
	ntfs_eino_free(e);
	return r;
}

static int ntfs_xattr_set_path(vfs_mount_t *m, const char *path, int nofollow,
			       const char *name, const void *val,
			       unsigned long size, int flags, int remove)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *res;
	int r;

	if (!fs || !path || !name)
		return ST_INVALID;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	if (!res)
		return ST_NOMEM;
	ntfs_meta_wlock(fs);
	r = ntfs_resolve(fs, path, !nofollow, res);
	if (r == ST_OK && res->is_stream)
		r = ST_UNSUPPORTED;
	if (r == ST_OK)
		r = ntfs_setxattr_mref(fs, res->mref, name, val, size, flags,
				       remove);
	ntfs_meta_wunlock(fs);
	if (r == NTFS_XATTR_IS_STREAM)
		r = ntfs_xattr_stream_write(fs, res->mref, name + 5, val, size);
	kfree(res);
	return r;
}

int ntfs_setxattr(vfs_mount_t *m, const char *path, int nofollow,
		  const char *name, const void *val, unsigned long size,
		  int flags)
{
	return ntfs_xattr_set_path(m, path, nofollow, name, val, size, flags,
				   0);
}

int ntfs_removexattr(vfs_mount_t *m, const char *path, int nofollow,
		     const char *name)
{
	return ntfs_xattr_set_path(m, path, nofollow, name, NULL, 0, 0, 1);
}

int ntfs_fsetxattr(vfs_file_t *f, const char *name, const void *val,
		   unsigned long size, int flags)
{
	ntfs_file_t *nf = ntfs_nf(f);
	int r;

	if (!nf || !name)
		return ST_INVALID;
	if (nf->is_ads)
		return ST_UNSUPPORTED;
	ntfs_meta_wlock(nf->fs);
	r = ntfs_setxattr_mref(nf->fs, nf->mref, name, val, size, flags, 0);
	ntfs_meta_wunlock(nf->fs);
	if (r == NTFS_XATTR_IS_STREAM)
		r = ntfs_xattr_stream_write(nf->fs, nf->mref, name + 5, val,
					    size);
	return r;
}

int ntfs_fremovexattr(vfs_file_t *f, const char *name)
{
	ntfs_file_t *nf = ntfs_nf(f);
	int r;

	if (!nf || !name)
		return ST_INVALID;
	if (nf->is_ads)
		return ST_UNSUPPORTED;
	ntfs_meta_wlock(nf->fs);
	r = ntfs_setxattr_mref(nf->fs, nf->mref, name, NULL, 0, 0, 1);
	ntfs_meta_wunlock(nf->fs);
	return r;
}

/* ---- extended attributes ---- */

static int ntfs_xattr_copy(void *val, unsigned long size, const void *src,
			   unsigned long len)
{
	if (!val || !size)
		return (int)len;
	if (size < len)
		return ST_RANGE;
	mm_memcpy(val, src, len);
	return (int)len;
}

/* Read an alternate data stream as an xattr value. */
static int ntfs_xattr_ads(ntfs_fs_t *fs, uint64_t mref, const char *sname,
			  void *val, unsigned long size)
{
	uint16_t *u16 = (uint16_t *)kalloc(NTFS_MAX_NAME_LEN * 2);
	ntfs_stream_t *st = NULL;
	long n;
	int r;

	if (!u16)
		return ST_NOMEM;
	n = ntfs_utf8_to_utf16(sname, ntfs_strlen(sname), u16,
			       NTFS_MAX_NAME_LEN);
	if (n <= 0) {
		kfree(u16);
		return ST_NODATA;
	}
	r = ntfs_stream_get(fs, mref, NTFS_AT_DATA, u16, (uint32_t)n, 0, &st);
	kfree(u16);
	if (r == ST_NOT_FOUND)
		return ST_NODATA;
	if (r != ST_OK)
		return r;
	if (st->flags & (NTFS_ST_ENCRYPTED | NTFS_ST_WOF)) {
		ntfs_stream_put(fs, st);
		return ST_UNSUPPORTED;
	}
	if (st->data_size > 0x7FFFFFFFULL) {
		ntfs_stream_put(fs, st);
		return ST_RANGE;
	}
	if (!val || !size) {
		r = (int)st->data_size;
	} else if (size < st->data_size) {
		r = ST_RANGE;
	} else {
		/* The metadata lock (held shared by the caller) fences the
		 * stream's clusters: every path that frees or moves them holds
		 * it exclusive.  No file lock here: its order is before the
		 * metadata lock. */
		r = ntfs_stream_pread(fs, st, 0, st->data_size, val, 0);
		if (r == ST_OK)
			r = (int)st->data_size;
	}
	ntfs_stream_put(fs, st);
	return r;
}

/* getxattr on the file `mref' (metadata lock held shared). */
static int ntfs_getxattr_mref(ntfs_fs_t *fs, uint64_t mref, const char *name,
			      void *val, unsigned long size)
{
	ntfs_inode_t *ino;
	uint8_t *buf = NULL;
	uint32_t q = 0;
	ntfs_ea_view_t v;
	unsigned long nl = ntfs_strlen(name);
	int r;

	if (!name || !nl)
		return ST_INVALID;
	ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	if (!ino)
		return ST_NOMEM;
	r = ntfs_inode_load(fs, mref, ino);
	if (r != ST_OK)
		goto out;
	/* Computed system.* attributes. */
	if (ntfs_streq(name, NTFS_LABEL_XATTR)) {
		r = NTFS_MREF_NO(mref) == NTFS_FILE_ROOT ?
			    ntfs_label_get(fs, val, size) :
			    ST_NODATA;
		goto out;
	}
	if (ntfs_streq(name, "system.ntfs_attrib") ||
	    ntfs_streq(name, "system.ntfs_attrib_be")) {
		uint8_t b[4];

		if (name[18] == '_') {
			for (int i = 0; i < 4; i++)
				b[i] = (uint8_t)(ino->fattr >> (24 - 8 * i));
		} else {
			ntfs_put_le32(b, ino->fattr);
		}
		r = ntfs_xattr_copy(val, size, b, 4);
		goto out;
	}
	if (ntfs_streq(name, "system.dos_attrib")) {
		uint8_t b = (uint8_t)(ino->fattr & 0xFF);

		r = ntfs_xattr_copy(val, size, &b, 1);
		goto out;
	}
	if (ntfs_streq(name, "system.ntfs_crtime") ||
	    ntfs_streq(name, "system.ntfs_crtime_be")) {
		uint8_t b[8];
		uint64_t t = (uint64_t)ino->crtime;

		for (int i = 0; i < 8; i++)
			b[name[18] == '_' ? 7 - i : i] =
				(uint8_t)(t >> (8 * i));
		r = ntfs_xattr_copy(val, size, b, 8);
		goto out;
	}
	if (ntfs_streq(name, "system.ntfs_security")) {
		uint8_t *sd;
		uint32_t sl;

		r = ntfs_sd_get(fs, ino, &sd, &sl);
		if (r == ST_OK) {
			r = ntfs_xattr_copy(val, size, sd, sl);
			kfree(sd);
		}
		goto out;
	}
	if (ntfs_streq(name, "system.ntfs_reparse_data")) {
		uint8_t *rp;
		uint32_t rl;

		if (!ino->has_reparse) {
			r = ST_NODATA;
			goto out;
		}
		r = ntfs_reparse_read(fs, mref, &rp, &rl);
		if (r == ST_OK) {
			r = ntfs_xattr_copy(val, size, rp, rl);
			kfree(rp);
		}
		goto out;
	}
	if (ntfs_streq(name, "system.posix_acl_access") ||
	    ntfs_streq(name, "system.posix_acl_default")) {
		if (!fs->opt.acl) {
			r = ST_UNSUPPORTED;
			goto out;
		}
	} else if (!ntfs_strneq(name, "user.", 5) &&
		   !ntfs_strneq(name, "security.", 9) &&
		   !ntfs_strneq(name, "trusted.", 8)) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	/* EAs: the full name first, then "user.X" -> EA "X". */
	if (ino->has_ea_info && ino->has_ea) {
		r = ntfs_ea_load(fs, mref, &buf, &q);
		if (r != ST_OK)
			goto out;
	}
	if (buf) {
		r = ntfs_ea_find(buf, q, name, &v);
		if (r == ST_NOT_FOUND && ntfs_strneq(name, "user.", 5) &&
		    !ntfs_has_xattr_prefix(name + 5, (uint32_t)(nl - 5)) &&
		    !ntfs_ea_reserved(name + 5, (uint32_t)(nl - 5)))
			r = ntfs_ea_find(buf, q, name + 5, &v);
		if (r == ST_OK) {
			r = ntfs_xattr_copy(val, size, v.value, v.value_len);
			goto out;
		}
		if (r != ST_NOT_FOUND)
			goto out;
	}
	/* Alternate data streams: "user.<stream>". */
	if (ntfs_strneq(name, "user.", 5) && nl > 5)
		r = ntfs_xattr_ads(fs, mref, name + 5, val, size);
	else
		r = ST_NODATA;
out:
	ntfs_free(buf);
	kfree(ino);
	return r;
}

/* listxattr of `mref'. */
static int ntfs_listxattr_mref(ntfs_fs_t *fs, uint64_t mref, char *list,
			       unsigned long size)
{
	struct ntfs_xlist *x;
	ntfs_actx_t *c = NULL;
	uint8_t *buf = NULL;
	uint32_t q = 0;
	int r;

	x = (struct ntfs_xlist *)ntfs_zalloc(sizeof(*x));
	if (!x)
		return ST_NOMEM;
	x->fs = fs;
	x->list = list;
	x->size = list ? size : 0;
	r = ntfs_ea_load(fs, mref, &buf, &q);
	if (r != ST_OK)
		goto out;
	if (buf) {
		r = ntfs_ea_walk(buf, q, ntfs_xlist_ea_cb, x);
		if (r != ST_OK)
			goto out; /* never a partial list */
	}
	/* Named $DATA streams. */
	r = ntfs_actx_open(fs, mref, &c);
	if (r != ST_OK)
		goto out;
	for (;;) {
		ntfs_ea_view_t v;
		long n;

		r = ntfs_actx_find(c, NTFS_AT_DATA, NULL, 0, 0);
		if (r == ST_NOT_FOUND) {
			r = ST_OK;
			break;
		}
		if (r != ST_OK)
			break;
		if (!c->a->name_length || (c->a->non_resident &&
					   c->a->data.non_resident.lowest_vcn))
			continue;
		mm_memcpy(x->name, "user.", 5);
		n = ntfs_utf16_to_utf8(ntfs_attr_name(c->a),
				       c->a->name_length, x->name + 5,
				       sizeof(x->name) - 5);
		if (n <= 0)
			continue; /* unrepresentable: not listed */
		/* An EA of the same xattr name is listed already. */
		if (buf && (ntfs_ea_find(buf, q, x->name, &v) == ST_OK ||
			    (!ntfs_has_xattr_prefix(x->name + 5, (uint32_t)n) &&
			     ntfs_ea_find(buf, q, x->name + 5, &v) == ST_OK)))
			continue;
		r = ntfs_xlist_add(x, x->name, (uint32_t)(n + 5));
		if (r)
			break;
	}
out:
	ntfs_actx_close(c);
	ntfs_free(buf);
	if (r == ST_OK)
		r = (int)x->used;
	kfree(x);
	return r;
}

static int ntfs_xattr_path(ntfs_fs_t *fs, const char *path, int nofollow,
			   uint64_t *mref)
{
	ntfs_res_t *res = (ntfs_res_t *)kalloc(sizeof(*res));
	int r;

	if (!res)
		return ST_NOMEM;
	r = ntfs_resolve(fs, path, !nofollow, res);
	if (r == ST_OK)
		*mref = res->mref;
	kfree(res);
	return r;
}

int ntfs_getxattr(vfs_mount_t *m, const char *path, int nofollow,
		  const char *name, void *val, unsigned long size)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	uint64_t mref;
	int r;

	if (!fs || !path || !name)
		return ST_INVALID;
	ntfs_meta_rlock(fs);
	r = ntfs_xattr_path(fs, path, nofollow, &mref);
	if (r == ST_OK)
		r = ntfs_getxattr_mref(fs, mref, name, val, size);
	ntfs_meta_runlock(fs);
	return r;
}

int ntfs_listxattr(vfs_mount_t *m, const char *path, int nofollow,
		   char *list, unsigned long size)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	uint64_t mref;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	ntfs_meta_rlock(fs);
	r = ntfs_xattr_path(fs, path, nofollow, &mref);
	if (r == ST_OK)
		r = ntfs_listxattr_mref(fs, mref, list, size);
	ntfs_meta_runlock(fs);
	return r;
}

int ntfs_fgetxattr(vfs_file_t *f, const char *name, void *val,
		   unsigned long size)
{
	ntfs_file_t *nf = ntfs_nf(f);
	int r;

	if (!nf || !name)
		return ST_INVALID;
	ntfs_meta_rlock(nf->fs);
	r = ntfs_getxattr_mref(nf->fs, nf->mref, name, val, size);
	ntfs_meta_runlock(nf->fs);
	return r;
}

int ntfs_flistxattr(vfs_file_t *f, char *list, unsigned long size)
{
	ntfs_file_t *nf = ntfs_nf(f);
	int r;

	if (!nf)
		return ST_INVALID;
	ntfs_meta_rlock(nf->fs);
	r = ntfs_listxattr_mref(nf->fs, nf->mref, list, size);
	ntfs_meta_runlock(nf->fs);
	return r;
}

int ntfs_getxattr_ino(vfs_mount_t *m, unsigned long ino,
		      const char *name, void *val, unsigned long size)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	int r;

	if (!fs || !name || ino >= fs->mft_records)
		return ST_INVALID;
	ntfs_meta_rlock(fs);
	r = ntfs_getxattr_mref(fs, NTFS_MAKE_MREF(ino, 0), name, val, size);
	ntfs_meta_runlock(fs);
	return r;
}
