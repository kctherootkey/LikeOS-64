// LikeOS - ext4 filesystem driver: extended attributes
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* --- the lock instances ------------------------------------------------ */

/* Serializes the function-scope STATIC xattr parse scratch lists in
 * ext4_xattr_get_ino / ext4_xattr_list_ino.  Those lists are static because
 * ext4_xa_list is ~8.6 KB — far too big for the 16 KB kernel stack — which was
 * safe under the old global ext4 mutex but NOT under the fine-grained locking:
 * getxattr/listxattr are read ops holding st->meta only SHARED, so two
 * concurrent ACL/xattr reads parsed into and freed the SAME static list —
 * double-free of the value buffers (SLAB "Double free ... size=32" via
 * ext4_xa_list_free) plus wrong xattr bytes returned to one caller (ACL grant
 * "vanishing" with a clean on-disk fs).  Sleeping lock, NOT a spinlock:
 * ext4_xa_list_free → kfree can TLB-shootdown.  Order: st->meta → s_xa_scratch,
 * never the reverse.  setxattr's static lists need no lock — mutators hold
 * st->meta EXCLUSIVE, which already excludes readers and each other. */
static ext4_rwsem_t s_xa_scratch = EXT4_RWSEM_INIT("ext4_xa");

/* Crash-abandon cleanup: give back every hold the dead task still has --
 * exclusive ones (tracked by owner) and the shared ones recorded per task
 * (see ext4_rwsem_t). */
int ext4_io_release_if_owner(ext4_fs_t *fs, uint64_t task_id)
{
	int released = 0, excl = 0;
	uint64_t flags;

	/* The metadata lock: an exclusive hold also owns the running
	 * transaction, which is discarded first (under the lock's spinlock,
	 * so nobody who takes it next can see it).  Nothing of it was
	 * journaled or checkpointed, so the filesystem stays at its
	 * pre-operation state (atomicity).  Any committed batch (st->ckpt +
	 * log) is durable and stays -- it is checkpointed/replayed normally. */
	spin_lock_irqsave(&fs->st->meta.lock, &flags);
	if (fs->st->meta.writer && fs->st->meta.owner == task_id) {
		fs->st->txn.active = 0;
		fs->st->txn.n = 0;
		if (fs->st->is_root) {
			ext4_io_locked = 0;
			ext4_io_owner = (uint64_t)-1;
			ext4_io_depth = 0;
		}
	}
	spin_unlock_irqrestore(&fs->st->meta.lock, flags);
	released |= ext4_rwsem_release_owner(&fs->st->meta, task_id, &excl);

	for (int i = 0; i < EXT4_ILOCKS; i++)
		released |= ext4_rwsem_release_owner(&fs->st->ilock[i], task_id,
						     NULL);
	for (int i = 0; i < EXT4_BGLOCKS; i++)
		released |= ext4_rwsem_release_owner(&fs->st->bglock[i],
						     task_id, NULL);
	/* The xattr scratch lock is every filesystem's; releasing it from
	 * any of them is the same. */
	released |= ext4_rwsem_release_owner(&s_xa_scratch, task_id, NULL);
	return released;
}

/* ===================================================================
 * Extended attributes (xattr) — storage layer.
 *
 * In-inode ("ibody") attributes — those stored in the inode's
 * slack space [128 + i_extra_isize, s_inode_size).  This region is covered by
 * the inode metadata_csum (already computed correctly), so there is no separate
 * on-disk hash/checksum to get byte-exact here.  The external xattr block (with
 * its own hash + crc32c) is a later increment; get/list already leave a hook for
 * it.  Values stored in a separate inode (e_value_inum != 0) are not produced by
 * us and are treated as absent on read.
 * =================================================================== */

/* xattr set flags (match the userspace XATTR_CREATE / XATTR_REPLACE). */
#define EXT4_XATTR_CREATE 1 /* fail with EEXIST if the attr exists      */
#define EXT4_XATTR_REPLACE 2 /* fail with ENODATA if it does not exist   */

/* Map a namespace prefix to its e_name_index; *suffix and *slen receive the
 * part after the prefix.  Returns 0 for an unknown/unsupported namespace. */
int ext4_xattr_name_index(const char *full, const char **suffix,
				 unsigned *slen)
{
	static const struct {
		const char *p;
		int idx;
	} pre[] = {
		{ "user.", EXT4_XATTR_INDEX_USER },
		{ "system.posix_acl_access",
		  EXT4_XATTR_INDEX_POSIX_ACL_ACCESS },
		{ "system.posix_acl_default",
		  EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT },
		{ "trusted.", EXT4_XATTR_INDEX_TRUSTED },
		{ "security.", EXT4_XATTR_INDEX_SECURITY },
		{ "system.", EXT4_XATTR_INDEX_SYSTEM },
	};
	for (unsigned k = 0; k < sizeof(pre) / sizeof(pre[0]); k++) {
		const char *p = pre[k].p;
		const char *f = full;
		while (*p && *p == *f) {
			p++;
			f++;
		}
		if (*p)
			continue; /* prefix didn't fully match */
		/* posix_acl_* are whole names with an EMPTY on-disk name (the index is
         * the identity); the others are prefixes that require a suffix. */
		int whole = (pre[k].idx == EXT4_XATTR_INDEX_POSIX_ACL_ACCESS ||
			     pre[k].idx == EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT);
		if (whole && *f)
			continue; /* whole name must match exactly */
		if (!whole && !*f)
			continue; /* prefix name needs a suffix    */
		*suffix = f;
		unsigned n = 0;
		while (f[n])
			n++;
		*slen = n;
		return pre[k].idx;
	}
	return 0;
}

/* The namespace prefix for an index (for listxattr output).  NULL if unknown. */
static const char *ext4_xattr_prefix(int idx)
{
	switch (idx) {
	case EXT4_XATTR_INDEX_USER:
		return "user.";
	case EXT4_XATTR_INDEX_POSIX_ACL_ACCESS:
		return "system.posix_acl_access";
	case EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT:
		return "system.posix_acl_default";
	case EXT4_XATTR_INDEX_TRUSTED:
		return "trusted.";
	case EXT4_XATTR_INDEX_SECURITY:
		return "security.";
	case EXT4_XATTR_INDEX_SYSTEM:
		return "system.";
	default:
		return 0;
	}
}

/* In-memory attribute used while rebuilding a region (parse -> modify -> serialize). */
#define EXT4_XA_MAX_ATTRS 32
#define EXT4_XA_MAX_NAME 255
typedef struct {
	uint8_t index;
	uint8_t name_len;
	char name[EXT4_XA_MAX_NAME];
	uint32_t value_size;
	uint8_t *value; /* kalloc'd (value_size bytes) or NULL    */
} ext4_xa_attr;
typedef struct {
	ext4_xa_attr a[EXT4_XA_MAX_ATTRS];
	unsigned n;
} ext4_xa_list;

static void ext4_xa_list_free(ext4_xa_list *L)
{
	for (unsigned i = 0; i < L->n; i++)
		if (L->a[i].value) {
			kfree(L->a[i].value);
			L->a[i].value = 0;
		}
	L->n = 0;
}

/* Parse the entries of one xattr region into L.  `first` points at the first
 * entry, `end` bounds both entries and (inline) values, `value_base` is what
 * e_value_offs is measured from (the block start for a block; `first` for an
 * ibody region). */
static int ext4_xa_parse(ext4_xa_list *L, const uint8_t *first,
			 const uint8_t *end, const uint8_t *value_base)
{
	const uint8_t *p = first;
	while (p + sizeof(ext4_xattr_entry) <= end) {
		const ext4_xattr_entry *e = (const ext4_xattr_entry *)p;
		uint32_t first4;
		mm_memcpy(&first4, p, 4);
		if (first4 == 0)
			break; /* zero terminator (name_len &
                                                   * name_index both 0).  A POSIX
                                                   * ACL entry has name_len==0 but
                                                   * name_index!=0, so first4!=0. */
		const uint8_t *nm = p + sizeof(ext4_xattr_entry);
		if (nm + e->e_name_len > end)
			break; /* malformed: stop          */
		if (L->n <
		    EXT4_XA_MAX_ATTRS) { /* e_name_len is u8 <= EXT4_XA_MAX_NAME */
			ext4_xa_attr *a = &L->a[L->n];
			a->index = e->e_name_index;
			a->name_len = e->e_name_len;
			mm_memcpy(a->name, nm, e->e_name_len);
			a->value_size = e->e_value_size;
			a->value = 0;
			if (e->e_value_inum == 0 && e->e_value_size > 0) {
				const uint8_t *v = value_base + e->e_value_offs;
				if (v >= first && v + e->e_value_size <= end) {
					a->value = (uint8_t *)kalloc(
						e->e_value_size);
					if (a->value)
						mm_memcpy(a->value, v,
							  e->e_value_size);
				}
			}
			L->n++;
		}
		p = nm +
		    ((e->e_name_len + EXT4_XATTR_ROUND) & ~EXT4_XATTR_ROUND);
	}
	return ST_OK;
}

/* Bytes a list needs when serialized into a region (header + entries + 4-byte
 * terminator + 4-byte-aligned values).  `hdr_size` is 4 for ibody, 32 for a
 * block. */
static unsigned long ext4_xa_serial_size(const ext4_xa_list *L,
					 unsigned hdr_size)
{
	unsigned long ent = 0, val = 0;
	for (unsigned i = 0; i < L->n; i++) {
		ent += EXT4_XATTR_LEN(L->a[i].name_len);
		val += EXT4_XATTR_SIZE(L->a[i].value_size);
	}
	return hdr_size + ent + 4 /*terminator*/ + val;
}

/* Find an attribute in L by (index,name); returns its index in L or -1. */
static int ext4_xa_find(const ext4_xa_list *L, int index, const char *name,
			unsigned nlen)
{
	for (unsigned i = 0; i < L->n; i++) {
		if (L->a[i].index != index || L->a[i].name_len != nlen)
			continue;
		unsigned k = 0;
		while (k < nlen && L->a[i].name[k] == name[k])
			k++;
		if (k == nlen)
			return (int)i;
	}
	return -1;
}

/* Serialize L into an ibody region [region, region+size).  Entries grow forward
 * after the 4-byte ibody header; values grow backward from region+size.  Returns
 * ST_OK, or ST_NOSPC if it does not fit.  e_hash is left 0 (ibody entries are not
 * part of any shareable-block hash; the inode csum covers the region). */
static int ext4_xa_serialize_ibody(uint8_t *region, unsigned long size,
				   const ext4_xa_list *L)
{
	if (L->n == 0) { /* no attrs: clear the region */
		mm_memset(region, 0, size);
		return ST_OK;
	}
	if (ext4_xa_serial_size(L, sizeof(ext4_xattr_ibody_header)) > size)
		return ST_NOSPC;
	mm_memset(region, 0, size);
	ext4_xattr_ibody_header *h = (ext4_xattr_ibody_header *)region;
	h->h_magic = EXT4_XATTR_MAGIC;
	uint8_t *ifirst = region + sizeof(ext4_xattr_ibody_header);
	uint8_t *ep = ifirst;
	unsigned long vpos = size; /* value offsets are from ifirst */
	for (unsigned i = 0; i < L->n; i++) {
		const ext4_xa_attr *a = &L->a[i];
		unsigned vlen = EXT4_XATTR_SIZE(a->value_size);
		vpos -= vlen; /* place value (region-relative) */
		if (a->value_size && a->value)
			mm_memcpy(region + vpos, a->value, a->value_size);
		ext4_xattr_entry *e = (ext4_xattr_entry *)ep;
		e->e_name_len = a->name_len;
		e->e_name_index = a->index;
		e->e_value_offs =
			(uint16_t)(vpos -
				   (unsigned long)(ifirst -
						   region)); /* from ifirst */
		e->e_value_inum = 0;
		e->e_value_size = a->value_size;
		e->e_hash = 0;
		mm_memcpy(e->e_name, a->name, a->name_len);
		ep += EXT4_XATTR_LEN(a->name_len);
	}
	return ST_OK;
}

/* Per-entry xattr hash: fold the name then the (4-byte-padded, inline) value.
 * `block_base` is the start the entry's e_value_offs is measured from. */
static uint32_t ext4_xattr_entry_hash(const ext4_xattr_entry *e,
				      const uint8_t *block_base)
{
	uint32_t hash = 0;
	const uint8_t *name = (const uint8_t *)e->e_name;
	for (unsigned n = 0; n < e->e_name_len; n++)
		hash = (hash << 5) ^ (hash >> (32 - 5)) ^
		       name[n]; /* NAME_HASH_SHIFT=5 */
	if (e->e_value_inum == 0 && e->e_value_size) {
		const uint8_t *v = block_base + e->e_value_offs;
		unsigned words = (e->e_value_size + EXT4_XATTR_ROUND) >>
				 2; /* padded /4 */
		for (unsigned n = 0; n < words; n++) {
			uint32_t w;
			mm_memcpy(&w, v + (unsigned long)n * 4, 4);
			hash = (hash << 16) ^ (hash >> (32 - 16)) ^
			       w; /* VALUE_HASH_SHIFT=16 */
		}
	}
	return hash;
}

/* Whole-block hash: fold every entry's e_hash (0 if any entry hash is 0, which
 * marks the block non-shareable — matches jbd2/ext4 ext4_xattr_rehash). */
static void ext4_xattr_rehash(ext4_xattr_header *h, const uint8_t *block_base)
{
	uint32_t hash = 0;
	const uint8_t *p = block_base + sizeof(ext4_xattr_header);
	for (;;) {
		const ext4_xattr_entry *e = (const ext4_xattr_entry *)p;
		uint32_t first4;
		mm_memcpy(&first4, p, 4);
		if (first4 == 0)
			break; /* terminator */
		if (e->e_hash == 0) {
			hash = 0;
			break;
		}
		hash = (hash << 16) ^ (hash >> (32 - 16)) ^
		       e->e_hash; /* BLOCK_HASH_SHIFT=16 */
		p += EXT4_XATTR_LEN(e->e_name_len);
	}
	h->h_hash = hash;
}

/* crc32c of an xattr block (metadata_csum): seed with the fs csum seed folded
 * with the LE block number, then the block with h_checksum treated as zero. */
static uint32_t ext4_xattr_block_csum(const ext4_fs_t *fs,
				      unsigned long blocknr,
				      const uint8_t *block)
{
	uint64_t dsk = (uint64_t)blocknr; /* little-endian on x86-64 */
	uint32_t dummy = 0;
	unsigned coff = __builtin_offsetof(ext4_xattr_header, h_checksum);
	uint32_t c = ext4_crc32c(fs->csum_seed, &dsk, sizeof(dsk));
	c = ext4_crc32c(c, block, coff); /* header up to h_checksum   */
	c = ext4_crc32c(c, &dummy,
			sizeof(dummy)); /* the zeroed h_checksum     */
	c = ext4_crc32c(c, block + coff + sizeof(dummy),
			fs->block_size - (coff + sizeof(dummy)));
	return c;
}
static void ext4_xattr_block_csum_set(const ext4_fs_t *fs,
				      unsigned long blocknr, uint8_t *block)
{
	ext4_xattr_header *h = (ext4_xattr_header *)block;
	h->h_checksum = fs->has_metadata_csum ?
				ext4_xattr_block_csum(fs, blocknr, block) :
				0;
}
static int ext4_xattr_block_csum_ok(const ext4_fs_t *fs, unsigned long blocknr,
				    const uint8_t *block)
{
	if (!fs->has_metadata_csum)
		return 1;
	const ext4_xattr_header *h = (const ext4_xattr_header *)block;
	return h->h_checksum == ext4_xattr_block_csum(fs, blocknr, block);
}

/* Serialize L into a fresh external xattr block.  Entries grow forward after the
 * 32-byte header; values grow backward from the block end (e_value_offs is from
 * the block start).  Stamps per-entry hashes, the block hash, then the crc32c.
 * Returns ST_OK, or ST_NOSPC if it does not fit one block. */
static int ext4_xa_serialize_block(ext4_fs_t *fs, uint8_t *block,
				   unsigned long blocknr, const ext4_xa_list *L)
{
	if (ext4_xa_serial_size(L, sizeof(ext4_xattr_header)) > fs->block_size)
		return ST_NOSPC;
	mm_memset(block, 0, fs->block_size);
	ext4_xattr_header *h = (ext4_xattr_header *)block;
	h->h_magic = EXT4_XATTR_MAGIC;
	h->h_refcount = 1;
	h->h_blocks = 1;
	uint8_t *ep = block + sizeof(ext4_xattr_header);
	unsigned long vpos = fs->block_size;
	for (unsigned i = 0; i < L->n; i++) {
		const ext4_xa_attr *a = &L->a[i];
		unsigned vlen = EXT4_XATTR_SIZE(a->value_size);
		vpos -= vlen;
		if (a->value_size && a->value)
			mm_memcpy(block + vpos, a->value, a->value_size);
		ext4_xattr_entry *e = (ext4_xattr_entry *)ep;
		e->e_name_len = a->name_len;
		e->e_name_index = a->index;
		e->e_value_offs = (uint16_t)vpos; /* block-relative */
		e->e_value_inum = 0;
		e->e_value_size = a->value_size;
		e->e_hash = 0;
		mm_memcpy(e->e_name, a->name, a->name_len);
		ep += EXT4_XATTR_LEN(a->name_len);
	}
	/* hashes must be computed after the values are in place */
	ep = block + sizeof(ext4_xattr_header);
	for (unsigned i = 0; i < L->n; i++) {
		ext4_xattr_entry *e = (ext4_xattr_entry *)ep;
		e->e_hash = ext4_xattr_entry_hash(e, block);
		ep += EXT4_XATTR_LEN(e->e_name_len);
	}
	ext4_xattr_rehash(h, block);
	ext4_xattr_block_csum_set(fs, blocknr, block);
	return ST_OK;
}

/* Locate the ibody xattr region of inode `ino` within its inode-table block.
 * Returns ST_OK with *blk and *off (the block + inode offset), *rstart (offset of
 * the region within the block) and *rsize (region length).  *rsize==0 if the
 * inode has no usable ibody xattr space. */
static int ext4_xattr_ibody_region(ext4_fs_t *fs, unsigned long ino,
				   ext4_inode *in, unsigned long *blk,
				   unsigned *off, unsigned *rstart,
				   unsigned *rsize)
{
	int st = ext4_read_inode_loc(fs, ino, in, blk, off);
	if (st != ST_OK)
		return st;
	unsigned extra = in->i_extra_isize;
	unsigned start = *off + EXT4_GOOD_OLD_INODE_SIZE + extra;
	unsigned endb = *off + fs->inode_size;
	if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE || extra < 4 ||
	    start + sizeof(ext4_xattr_ibody_header) >= endb) {
		*rstart = 0;
		*rsize = 0;
	} else {
		*rstart = start;
		*rsize = endb - start;
	}
	return ST_OK;
}

/* getxattr: copy the value of (index,name) into buf (or return its size if
 * buf==NULL / size==0).  Returns the value size, ST_NOT_FOUND, or ST_INVALID
 * (buffer too small => -ERANGE handled by the caller). */
int ext4_xattr_get_ino(ext4_fs_t *fs, unsigned long ino, int index,
			      const char *name, unsigned nlen, void *buf,
			      unsigned long bufsz)
{
	ext4_inode in;
	unsigned long blk;
	unsigned off, rstart, rsize;
	int st = ext4_xattr_ibody_region(fs, ino, &in, &blk, &off, &rstart,
					 &rsize);
	if (st != ST_OK)
		return st;
	int found_size = ST_NODATA;
	if (rsize) {
		uint8_t *bbuf = (uint8_t *)ext4_bget(fs);
		if (!bbuf)
			return ST_NOMEM;
		if (ext4_read_block(fs, blk, bbuf) == ST_OK) {
			uint8_t *region = bbuf + rstart;
			uint32_t mag;
			mm_memcpy(&mag, region, 4);
			if (mag == EXT4_XATTR_MAGIC) {
				uint8_t *ifirst =
					region +
					sizeof(ext4_xattr_ibody_header);
				uint8_t *rend = bbuf + off + fs->inode_size;
				static ext4_xa_list L;
				ext4_rwsem_write_lock(&s_xa_scratch);
				L.n = 0;
				ext4_xa_parse(&L, ifirst, rend, ifirst);
				int i = ext4_xa_find(&L, index, name, nlen);
				if (i >= 0) {
					unsigned vs = L.a[i].value_size;
					if (bufsz == 0 || buf == 0)
						found_size = (int)vs;
					else if (vs > bufsz)
						found_size =
							ST_RANGE; /* ERANGE */
					else {
						if (vs && L.a[i].value)
							mm_memcpy(buf,
								  L.a[i].value,
								  vs);
						found_size = (int)vs;
					}
				}
				ext4_xa_list_free(&L);
				ext4_rwsem_write_unlock(&s_xa_scratch);
			}
		}
		ext4_bput(fs, bbuf);
	}
	/* External xattr block (i_file_acl) — checked only if not found in the ibody. */
	if (found_size == ST_NODATA && in.i_file_acl_lo) {
		if (!ext4_blk_valid(fs, ino, in.i_file_acl_lo, 1)) {
			ext4_bad_block(fs, ino, "extended attribute block",
				       in.i_file_acl_lo, 1);
			return ST_IO;
		}
		uint8_t *xbuf = (uint8_t *)ext4_bget(fs);
		if (!xbuf)
			return ST_NOMEM;
		if (ext4_read_block(fs, in.i_file_acl_lo, xbuf) == ST_OK) {
			ext4_xattr_header *xh = (ext4_xattr_header *)xbuf;
			if (xh->h_magic == EXT4_XATTR_MAGIC) {
				if (!ext4_mbc_verified(fs, in.i_file_acl_lo)) {
					if (!ext4_xattr_block_csum_ok(
						    fs, in.i_file_acl_lo,
						    xbuf)) {
						ext4_bput(fs, xbuf);
						ext4_fs_error(
							fs,
							"xattr block checksum mismatch",
							ino);
						return ST_IO;
					}
					ext4_mbc_mark_verified(fs, 
						in.i_file_acl_lo);
				}
				static ext4_xa_list LB;
				ext4_rwsem_write_lock(&s_xa_scratch);
				LB.n = 0;
				ext4_xa_parse(&LB,
					      xbuf + sizeof(ext4_xattr_header),
					      xbuf + fs->block_size, xbuf);
				int i = ext4_xa_find(&LB, index, name, nlen);
				if (i >= 0) {
					unsigned vs = LB.a[i].value_size;
					if (bufsz == 0 || buf == 0)
						found_size = (int)vs;
					else if (vs > bufsz)
						found_size = ST_RANGE;
					else {
						if (vs && LB.a[i].value)
							mm_memcpy(buf,
								  LB.a[i].value,
								  vs);
						found_size = (int)vs;
					}
				}
				ext4_xa_list_free(&LB);
				ext4_rwsem_write_unlock(&s_xa_scratch);
			}
		}
		ext4_bput(fs, xbuf);
	}
	return found_size;
}

/* Append each attribute's full name (prefix+name, NUL-terminated) from L to buf
 * (when non-NULL), tracking the running byte total and a range-overflow flag. */
static void ext4_xa_emit_names(const ext4_xa_list *L, char *buf,
			       unsigned long bufsz, unsigned long *total,
			       int *erange)
{
	for (unsigned i = 0; i < L->n; i++) {
		const char *pre = ext4_xattr_prefix(L->a[i].index);
		if (!pre)
			continue;
		unsigned plen = 0;
		while (pre[plen])
			plen++;
		unsigned full = plen + L->a[i].name_len + 1; /* + NUL */
		if (buf && bufsz) {
			if (*total + full > bufsz) {
				*erange = 1;
			} else {
				mm_memcpy(buf + *total, pre, plen);
				mm_memcpy(buf + *total + plen, L->a[i].name,
					  L->a[i].name_len);
				buf[*total + plen + L->a[i].name_len] = '\0';
			}
		}
		*total += full;
	}
}

/* listxattr: write NUL-separated full names from BOTH the ibody and the external
 * block into buf (or return total size if buf==NULL/size==0).  Returns total
 * bytes, or ST_RANGE (=> -ERANGE). */
int ext4_xattr_list_ino(ext4_fs_t *fs, unsigned long ino, char *buf,
			       unsigned long bufsz)
{
	ext4_inode in;
	unsigned long blk;
	unsigned off, rstart, rsize;
	int st = ext4_xattr_ibody_region(fs, ino, &in, &blk, &off, &rstart,
					 &rsize);
	if (st != ST_OK)
		return st;
	unsigned long total = 0;
	int erange = 0;
	if (rsize) {
		uint8_t *bbuf = (uint8_t *)kalloc(fs->block_size);
		if (!bbuf)
			return ST_NOMEM;
		if (ext4_read_block(fs, blk, bbuf) == ST_OK) {
			uint8_t *region = bbuf + rstart;
			uint32_t mag;
			mm_memcpy(&mag, region, 4);
			if (mag == EXT4_XATTR_MAGIC) {
				uint8_t *ifirst =
					region +
					sizeof(ext4_xattr_ibody_header);
				static ext4_xa_list L;
				ext4_rwsem_write_lock(&s_xa_scratch);
				L.n = 0;
				ext4_xa_parse(&L, ifirst,
					      bbuf + off + fs->inode_size,
					      ifirst);
				ext4_xa_emit_names(&L, buf, bufsz, &total,
						   &erange);
				ext4_xa_list_free(&L);
				ext4_rwsem_write_unlock(&s_xa_scratch);
			}
		}
		kfree(bbuf);
	}
	if (in.i_file_acl_lo) {
		if (!ext4_blk_valid(fs, ino, in.i_file_acl_lo, 1)) {
			ext4_bad_block(fs, ino, "extended attribute block",
				       in.i_file_acl_lo, 1);
			return ST_IO;
		}
		uint8_t *xbuf = (uint8_t *)kalloc(fs->block_size);
		if (!xbuf)
			return ST_NOMEM;
		if (ext4_read_block(fs, in.i_file_acl_lo, xbuf) == ST_OK) {
			ext4_xattr_header *xh = (ext4_xattr_header *)xbuf;
			if (xh->h_magic == EXT4_XATTR_MAGIC) {
				if (!ext4_xattr_block_csum_ok(
					    fs, in.i_file_acl_lo, xbuf)) {
					kfree(xbuf);
					ext4_fs_error(
						fs,
						"xattr block checksum mismatch",
						ino);
					return ST_IO;
				}
				static ext4_xa_list LB;
				ext4_rwsem_write_lock(&s_xa_scratch);
				LB.n = 0;
				ext4_xa_parse(&LB,
					      xbuf + sizeof(ext4_xattr_header),
					      xbuf + fs->block_size, xbuf);
				ext4_xa_emit_names(&LB, buf, bufsz, &total,
						   &erange);
				ext4_xa_list_free(&LB);
				ext4_rwsem_write_unlock(&s_xa_scratch);
			}
		}
		kfree(xbuf);
	}
	if (erange)
		return ST_RANGE; /* -ERANGE */
	return (int)total;
}

/* setxattr/removexattr (value==NULL && size==0 with EXT4_XATTR_REPLACE-less means
 * set empty; the dedicated remove path passes remove=1).  Writes the rebuilt
 * ibody region back with a recomputed inode csum, journalled.  Returns ST_OK or
 * an ST_ error (ST_EXISTS, ST_NOT_FOUND, ST_NOSPC, ...). */
static int ext4_xattr_set_ino_locked(ext4_fs_t *fs, unsigned long ino,
				     int index, const char *name, unsigned nlen,
				     const void *value, unsigned long size,
				     int flags, int remove);

/* The parse lists below are static (too big for the stack) and shared by
 * every mounted ext4 filesystem, while the metadata lock that serialises
 * mutators is each filesystem's own: s_xa_scratch, which the readers take
 * for their lists, covers these as well. */
int ext4_xattr_set_ino(ext4_fs_t *fs, unsigned long ino, int index,
			      const char *name, unsigned nlen,
			      const void *value, unsigned long size, int flags,
			      int remove)
{
	ext4_rwsem_write_lock(&s_xa_scratch);
	int r = ext4_xattr_set_ino_locked(fs, ino, index, name, nlen, value,
					  size, flags, remove);
	ext4_rwsem_write_unlock(&s_xa_scratch);
	return r;
}

static int ext4_xattr_set_ino_locked(ext4_fs_t *fs, unsigned long ino,
				     int index, const char *name, unsigned nlen,
				     const void *value, unsigned long size,
				     int flags, int remove)
{
	if (nlen > EXT4_XA_MAX_NAME)
		return ST_INVALID; /* nlen==0 = POSIX-ACL name */
	ext4_inode in;
	unsigned long blk;
	unsigned off, rstart, rsize;
	int st = ext4_xattr_ibody_region(fs, ino, &in, &blk, &off, &rstart,
					 &rsize);
	if (st != ST_OK)
		return st;

	uint8_t *bbuf =
		(uint8_t *)kalloc(fs->block_size); /* inode-table block    */
	uint8_t *xbuf =
		(uint8_t *)kalloc(fs->block_size); /* external xattr block */
	if (!bbuf || !xbuf) {
		if (bbuf)
			kfree(bbuf);
		if (xbuf)
			kfree(xbuf);
		return ST_NOMEM;
	}
	if (ext4_read_block(fs, blk, bbuf) != ST_OK) {
		kfree(bbuf);
		kfree(xbuf);
		return ST_IO;
	}

	ext4_inode *din = (ext4_inode *)(bbuf + off);
	unsigned long xblk =
		din->i_file_acl_lo; /* current external block (0=none) */
	int allocated_now = 0;
	int rc = ST_OK;

	static ext4_xa_list LI;
	LI.n = 0; /* in-inode (ibody) attributes */
	static ext4_xa_list LB;
	LB.n = 0; /* external-block attributes   */

	if (rsize) {
		uint8_t *region = bbuf + rstart;
		uint32_t mag;
		mm_memcpy(&mag, region, 4);
		if (mag == EXT4_XATTR_MAGIC)
			ext4_xa_parse(&LI,
				      region + sizeof(ext4_xattr_ibody_header),
				      bbuf + off + fs->inode_size,
				      region + sizeof(ext4_xattr_ibody_header));
	}
	if (xblk) {
		/* The block is rewritten or freed below: it has to be one the
		 * inode may use. */
		if (!ext4_blk_valid(fs, ino, xblk, 1)) {
			ext4_bad_block(fs, ino, "extended attribute block",
				       xblk, 1);
			rc = ST_IO;
			goto out;
		}
		if (ext4_read_block(fs, xblk, xbuf) != ST_OK) {
			rc = ST_IO;
			goto out;
		}
		ext4_xattr_header *xh = (ext4_xattr_header *)xbuf;
		if (xh->h_magic == EXT4_XATTR_MAGIC) {
			if (!ext4_xattr_block_csum_ok(fs, xblk, xbuf)) {
				ext4_fs_error(fs,
					      "xattr block checksum mismatch",
					      ino);
				rc = ST_IO;
				goto out;
			}
			ext4_xa_parse(&LB, xbuf + sizeof(ext4_xattr_header),
				      xbuf + fs->block_size, xbuf);
		}
	}

	int ii = ext4_xa_find(&LI, index, name, nlen);
	int bi = ext4_xa_find(&LB, index, name, nlen);
	int existed = (ii >= 0) || (bi >= 0);

	if (remove) {
		if (!existed) {
			rc = ST_NODATA;
			goto out;
		}
	} else {
		if (existed && (flags & EXT4_XATTR_CREATE)) {
			rc = ST_EXISTS;
			goto out;
		}
		if (!existed && (flags & EXT4_XATTR_REPLACE)) {
			rc = ST_NODATA;
			goto out;
		}
	}

	/* Remove the target from whichever store currently holds it. */
	if (ii >= 0) {
		if (LI.a[ii].value)
			kfree(LI.a[ii].value);
		for (unsigned i = ii; i + 1 < LI.n; i++)
			LI.a[i] = LI.a[i + 1];
		LI.n--;
	}
	if (bi >= 0) {
		if (LB.a[bi].value)
			kfree(LB.a[bi].value);
		for (unsigned i = bi; i + 1 < LB.n; i++)
			LB.a[i] = LB.a[i + 1];
		LB.n--;
	}

	if (!remove) {
		uint8_t *nv = 0;
		if (size) {
			nv = (uint8_t *)kalloc(size);
			if (!nv) {
				rc = ST_NOMEM;
				goto out;
			}
			mm_memcpy(nv, value, size);
		}
		/* Prefer the ibody if the attribute fits there next to the others;
         * otherwise spill it to the external block. */
		int placed = 0;
		if (rsize && LI.n < EXT4_XA_MAX_ATTRS) {
			ext4_xa_attr *a = &LI.a[LI.n];
			a->index = (uint8_t)index;
			a->name_len = (uint8_t)nlen;
			mm_memcpy(a->name, name, nlen);
			a->value = nv;
			a->value_size = (uint32_t)size;
			LI.n++;
			if (ext4_xa_serial_size(
				    &LI, sizeof(ext4_xattr_ibody_header)) <=
			    rsize)
				placed = 1;
			else
				LI.n--; /* doesn't fit the inode slack -> use the block */
		}
		if (!placed) {
			if (LB.n >= EXT4_XA_MAX_ATTRS) {
				if (nv)
					kfree(nv);
				rc = ST_NOSPC;
				goto out;
			}
			ext4_xa_attr *a = &LB.a[LB.n++];
			a->index = (uint8_t)index;
			a->name_len = (uint8_t)nlen;
			mm_memcpy(a->name, name, nlen);
			a->value = nv;
			a->value_size = (uint32_t)size;
		}
	}

	/* Rewrite the ibody region. */
	if (rsize) {
		if (ext4_xa_serialize_ibody(bbuf + rstart, rsize, &LI) !=
		    ST_OK) {
			rc = ST_NOSPC;
			goto out;
		}
	} else if (LI.n) {
		rc = ST_NOSPC;
		goto out; /* (defensive: no ibody to hold them) */
	}

	/* Allocate / rewrite / free the external block as needed. */
	if (LB.n == 0) {
		if (xblk) { /* block emptied -> free it */
			ext4_free_block(fs, xblk);
			din->i_file_acl_lo = 0;
			unsigned long sub = fs->block_size / 512;
			din->i_blocks_lo = (din->i_blocks_lo >= sub) ?
						   din->i_blocks_lo - sub :
						   0;
			xblk = 0;
		}
	} else {
		if (ext4_xa_serial_size(&LB, sizeof(ext4_xattr_header)) >
		    fs->block_size) {
			rc = ST_NOSPC;
			goto out; /* too big for one block (nothing allocated) */
		}
		if (!xblk) {
			xblk = ext4_alloc_one_block(fs);
			if (!xblk) {
				rc = ST_NOSPC;
				goto out;
			}
			allocated_now = 1;
			din->i_file_acl_lo = (uint32_t)xblk;
			din->i_blocks_lo += fs->block_size / 512;
		}
		if (ext4_xa_serialize_block(fs, xbuf, xblk, &LB) != ST_OK ||
		    ext4_write_block(fs, xblk, xbuf) != ST_OK) {
			if (allocated_now)
				ext4_free_block(fs,
						xblk); /* undo the allocation */
			rc = ST_IO;
			goto out;
		}
	}

	ext4_inode_csum_set(fs, ino, bbuf + off); /* re-stamp the inode csum */
	if (ext4_write_block(fs, blk, bbuf) != ST_OK) {
		if (allocated_now)
			ext4_free_block(fs, xblk);
		rc = ST_IO;
		goto out;
	}
	ext4_inode_cache_drop(fs, ino);
out:
	ext4_xa_list_free(&LI);
	ext4_xa_list_free(&LB);
	kfree(bbuf);
	kfree(xbuf);
	return rc;
}
