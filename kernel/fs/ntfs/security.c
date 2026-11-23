/* LikeOS - NTFS filesystem driver
 *
 * Security descriptors, POSIX ACLs, the $Secure writer.
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
 * S19, S29b, S31, S32  Security descriptors and POSIX ACLs: validation,
 * hashing, the default descriptor, ACL entries and modes
 *
 * Reading descriptors ($Secure, own attributes) is S19, the $Secure
 * writer S29b (both below); the xattr side, S32 (xattr.c).
 * =========================================================================== */

int ntfs_sd_check(const uint8_t *sd, uint32_t len)
{
	if (len < NTFS_SD_HDR || sd[0] != 1)
		return ST_IO;
	for (int i = 0; i < 4; i++) {
		uint32_t off = ntfs_le32(sd + 4 + 4 * i);

		if (off && (off < NTFS_SD_HDR || off >= len))
			return ST_IO;
	}
	return ST_OK;
}

/* The hash of a descriptor (the words of its whole length; a tail of
 * fewer than four bytes does not count). */
uint32_t ntfs_sd_hash(const uint8_t *sd, uint32_t len)
{
	uint32_t h = 0;

	for (uint32_t i = 0; i + 4 <= len; i += 4)
		h = ((h << 3) | (h >> 29)) + ntfs_le32(sd + i);
	return h;
}

/* A SID at `off' of the `len'-byte descriptor: revision 1, at most 15
 * sub-authorities, inside the descriptor.  *end: the byte after it. */
static int ntfs_sid_ok(const uint8_t *sd, uint32_t len, uint32_t off,
		       uint32_t *end)
{
	uint32_t n;

	if (off < NTFS_SD_HDR || off > len || len - off < 8)
		return 0;
	n = sd[off + 1];
	if (sd[off] != 1 || n > 15 || len - off < 8 + 4 * n)
		return 0;
	*end = off + 8 + 4 * n;
	return 1;
}

/* An ACL at `off': revision 2 or 4, its size inside the descriptor, and
 * every ACE inside the ACL (size >= 8, a multiple of 4).  *end: the byte
 * after it. */
static int ntfs_acl_ok(const uint8_t *sd, uint32_t len, uint32_t off,
		       uint32_t *end)
{
	uint32_t size, count, p;

	if (off < NTFS_SD_HDR || off > len || len - off < 8)
		return 0;
	if (sd[off] != 2 && sd[off] != 4)
		return 0;
	size = ntfs_le16(sd + off + 2);
	count = ntfs_le16(sd + off + 4);
	if (size < 8 || size > len - off)
		return 0;
	p = 8;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t asz;

		if (size - p < 8)
			return 0;
		asz = ntfs_le16(sd + off + p + 2);
		if (asz < 8 || (asz & 3) || asz > size - p)
			return 0;
		p += asz;
	}
	*end = off + size;
	return 1;
}

/* A descriptor that may be written: self-relative, revision 1, owner and
 * group SIDs (when present) and both ACLs (when present) valid, the
 * length at most NTFS_SD_MAX and no shorter than what it describes.
 * Stricter than the reader's check: nothing malformed is ever stored. */
int ntfs_sd_valid(const uint8_t *sd, uint32_t len)
{
	uint32_t ctl, off, end, need = NTFS_SD_HDR;

	if (!sd || len < NTFS_SD_HDR || len > NTFS_SD_MAX || sd[0] != 1)
		return 0;
	ctl = ntfs_le16(sd + 2);
	if (!(ctl & 0x8000))
		return 0; /* not self-relative */
	for (int i = 0; i < 2; i++) {
		off = ntfs_le32(sd + 4 + 4 * i); /* owner, group */
		if (!off)
			continue;
		if (!ntfs_sid_ok(sd, len, off, &end))
			return 0;
		if (end > need)
			need = end;
	}
	for (int i = 0; i < 2; i++) {
		off = ntfs_le32(sd + 12 + 4 * i); /* SACL, DACL */
		if (!off)
			continue;
		/* An ACL offset only counts with its PRESENT flag. */
		if (!(ctl & (i ? 0x0004 : 0x0010)))
			return 0;
		if (!ntfs_acl_ok(sd, len, off, &end))
			return 0;
		if (end > need)
			need = end;
	}
	return need <= len;
}

/* The default descriptor of new files: owner and group Administrators,
 * a DACL granting Everyone full access (zeroed first). */
uint8_t *ntfs_sd_default(uint32_t *len)
{
	/* header 20 + DACL (8 + ACE 20) + owner SID 16 + group SID 16 */
	uint32_t l = 20 + 28 + 16 + 16;
	uint8_t *sd = (uint8_t *)ntfs_zalloc(l);
	uint8_t *acl, *ace, *own, *grp;

	if (!sd)
		return NULL;
	sd[0] = 1; /* revision */
	ntfs_put_le16(sd + 2, 0x8004); /* SELF_RELATIVE | DACL_PRESENT */
	ntfs_put_le32(sd + 4, 48); /* owner */
	ntfs_put_le32(sd + 8, 64); /* group */
	ntfs_put_le32(sd + 12, 0); /* no SACL */
	ntfs_put_le32(sd + 16, 20); /* DACL */
	acl = sd + 20;
	acl[0] = 2; /* revision */
	ntfs_put_le16(acl + 2, 28); /* size */
	ntfs_put_le16(acl + 4, 1); /* one ACE */
	ace = acl + 8;
	ace[0] = 0; /* ACCESS_ALLOWED */
	ace[1] = 3; /* OBJECT_INHERIT | CONTAINER_INHERIT */
	ntfs_put_le16(ace + 2, 20);
	ntfs_put_le32(ace + 4, 0x001F01FF); /* FILE_ALL_ACCESS */
	ace[8] = 1; /* S-1-1-0 Everyone */
	ace[9] = 1;
	ace[15] = 1;
	ntfs_put_le32(ace + 16, 0);
	own = sd + 48; /* S-1-5-32-544 Administrators */
	own[0] = 1;
	own[1] = 2;
	own[7] = 5;
	ntfs_put_le32(own + 8, 32);
	ntfs_put_le32(own + 12, 544);
	grp = sd + 64;
	mm_memcpy(grp, own, 16);
	*len = l;
	return sd;
}

/* POSIX ACL xattr value: a 32-bit version (2), then entries of
 * {16-bit tag, 16-bit permissions, 32-bit id}. */
#define NTFS_PACL_VERSION 2U
#define NTFS_PACL_USER_OBJ 0x01U
#define NTFS_PACL_USER 0x02U
#define NTFS_PACL_GROUP_OBJ 0x04U
#define NTFS_PACL_GROUP 0x08U
#define NTFS_PACL_MASK 0x10U
#define NTFS_PACL_OTHER 0x20U
#define NTFS_PACL_MAX_ENTRIES 1024U

/* Is `v' (`len' bytes) a well-formed ACL?  `allow_empty': a header
 * without entries (a default ACL being cleared).  *equiv: it has only the
 * three base entries (the mode says everything). */
int ntfs_pacl_valid(const uint8_t *v, uint32_t len, int allow_empty,
		    int *equiv)
{
	uint32_t n, uo = 0, go = 0, ot = 0, mk = 0, ext = 0;

	*equiv = 0;
	if (!v || len < 4 || (len - 4) % 8 || ntfs_le32(v) != NTFS_PACL_VERSION)
		return 0;
	n = (len - 4) / 8;
	if (n > NTFS_PACL_MAX_ENTRIES)
		return 0;
	if (!n)
		return allow_empty;
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *x = v + 4 + 8 * i;
		uint16_t tag = ntfs_le16(x), perm = ntfs_le16(x + 2);

		if (perm & ~7U)
			return 0;
		switch (tag) {
		case NTFS_PACL_USER_OBJ:
			uo++;
			break;
		case NTFS_PACL_GROUP_OBJ:
			go++;
			break;
		case NTFS_PACL_OTHER:
			ot++;
			break;
		case NTFS_PACL_MASK:
			mk++;
			break;
		case NTFS_PACL_USER:
		case NTFS_PACL_GROUP:
			ext++;
			break;
		default:
			return 0;
		}
	}
	if (uo != 1 || go != 1 || ot != 1 || mk > 1 || (ext && !mk))
		return 0;
	*equiv = !ext && !mk;
	return 1;
}

/* Entry with tag `tag' (the first one), or NULL. */
static uint8_t *ntfs_pacl_entry(uint8_t *v, uint32_t len, uint16_t tag)
{
	for (uint32_t p = 4; p + 8 <= len; p += 8)
		if (ntfs_le16(v + p) == tag)
			return v + p;
	return NULL;
}

/* The permission bits (0777) an ACL stands for: owner = USER_OBJ, group =
 * MASK if there is one, else GROUP_OBJ, other = OTHER. */
uint32_t ntfs_pacl_mode(uint8_t *v, uint32_t len)
{
	uint8_t *u = ntfs_pacl_entry(v, len, NTFS_PACL_USER_OBJ);
	uint8_t *g = ntfs_pacl_entry(v, len, NTFS_PACL_MASK);
	uint8_t *o = ntfs_pacl_entry(v, len, NTFS_PACL_OTHER);

	if (!g)
		g = ntfs_pacl_entry(v, len, NTFS_PACL_GROUP_OBJ);
	return (u ? (uint32_t)ntfs_le16(u + 2) << 6 : 0) |
	       (g ? (uint32_t)ntfs_le16(g + 2) << 3 : 0) |
	       (o ? (uint32_t)ntfs_le16(o + 2) : 0);
}

/* chmod of an ACL: the three entries the mode stands for take its bits
 * (`and': only remove bits -- the creation rule for inherited ACLs). */
void ntfs_pacl_apply_mode(uint8_t *v, uint32_t len, uint32_t mode,
			  int and)
{
	uint8_t *u = ntfs_pacl_entry(v, len, NTFS_PACL_USER_OBJ);
	uint8_t *g = ntfs_pacl_entry(v, len, NTFS_PACL_MASK);
	uint8_t *o = ntfs_pacl_entry(v, len, NTFS_PACL_OTHER);
	uint8_t *e[3];
	uint32_t bits[3];

	if (!g)
		g = ntfs_pacl_entry(v, len, NTFS_PACL_GROUP_OBJ);
	e[0] = u;
	e[1] = g;
	e[2] = o;
	bits[0] = (mode >> 6) & 7;
	bits[1] = (mode >> 3) & 7;
	bits[2] = mode & 7;
	for (int i = 0; i < 3; i++) {
		if (!e[i])
			continue;
		ntfs_put_le16(e[i] + 2,
			      (uint16_t)(and ? (ntfs_le16(e[i] + 2) & bits[i]) :
					       bits[i]));
	}
}

/* ===========================================================================
 * S19  Security descriptors
 *
 * A file's descriptor is its own $SECURITY_DESCRIPTOR attribute (NTFS 1.x,
 * or files that carry one), else the one $Secure:$SDS holds for the
 * security id of its $STANDARD_INFORMATION, found through the $SII index
 * (collation NTOFS_ULONG).  The $SDS entry header must repeat the id, its
 * own offset and the length; the entry lies inside $SDS and inside one
 * 256 KB block (each block is mirrored at +256 KB); the descriptor is
 * self-relative, revision 1, with every offset inside it.
 * =========================================================================== */

int ntfs_sd_get(ntfs_fs_t *fs, const ntfs_inode_t *ino, uint8_t **out,
		uint32_t *out_len)
{
	static const uint16_t sii[4] = { '$', 'S', 'I', 'I' };
	ntfs_index_t *ix = NULL;
	uint8_t key[4], *ent = NULL, *sd = NULL;
	ntfs_sds_header_t hdr, chk;
	ntfs_ikey_t k;
	uint32_t doff, dlen, sdlen;
	int r;

	*out = NULL;
	*out_len = 0;
	if (ino->has_sd) {
		r = ntfs_attr_readall(fs, ntfs_ino_mref(ino),
				      NTFS_AT_SECURITY_DESCRIPTOR, ntfs_unnamed,
				      0, NTFS_SD_MAX, &sd, &sdlen);
		if (r != ST_OK)
			return r;
		if (ntfs_sd_check(sd, sdlen) != ST_OK) {
			kfree(sd);
			return ST_IO;
		}
		*out = sd;
		*out_len = sdlen;
		return ST_OK;
	}
	if (!ino->security_id || !fs->st_sds)
		return ST_NODATA;
	ntfs_put_le32(key, ino->security_id);
	r = ntfs_index_open(fs, NTFS_MAKE_MREF(NTFS_FILE_SECURE, 0), sii, 4,
			    NULL, &ix);
	if (r != ST_OK)
		return r == ST_NOT_FOUND ? ST_IO : r;
	if (ix->collation != NTFS_COLLATION_NTOFS_ULONG) {
		ntfs_index_close(ix);
		return ST_IO;
	}
	k.key = key;
	k.len = 4;
	k.ic = 0;
	r = ntfs_index_lookup(ix, &k, &ent);
	ntfs_index_close(ix);
	if (r != ST_OK)
		return r == ST_NOT_FOUND ? ST_NODATA : r;
	doff = ((const ntfs_index_entry_t *)ent)->data.vi.data_offset;
	dlen = ((const ntfs_index_entry_t *)ent)->data.vi.data_length;
	if (dlen < NTFS_SDS_HDR) {
		kfree(ent);
		return ST_IO;
	}
	mm_memcpy(&hdr, ent + doff, sizeof(hdr));
	kfree(ent);
	if (hdr.security_id != ino->security_id ||
	    hdr.length < NTFS_SDS_HDR + NTFS_SD_HDR ||
	    hdr.length > NTFS_SDS_HDR + NTFS_SD_MAX || (hdr.offset & 15) ||
	    !ntfs_range_ok(hdr.offset, hdr.length, fs->st_sds->data_size) ||
	    (hdr.offset % NTFS_SDS_BLOCK) + hdr.length > NTFS_SDS_BLOCK) {
		WARN_RATELIMIT(1, "ntfs: $Secure: bad $SII entry for id %u",
			       ino->security_id);
		return ST_IO;
	}
	r = ntfs_stream_pread(fs, fs->st_sds, hdr.offset, sizeof(chk), &chk, 0);
	if (r != ST_OK)
		return ST_IO;
	if (chk.security_id != hdr.security_id || chk.offset != hdr.offset ||
	    chk.length != hdr.length) {
		WARN_RATELIMIT(1, "ntfs: $Secure: $SDS entry of id %u does not "
				  "match its $SII entry",
			       ino->security_id);
		return ST_IO;
	}
	sdlen = hdr.length - NTFS_SDS_HDR;
	sd = (uint8_t *)kalloc(sdlen);
	if (!sd)
		return ST_NOMEM;
	r = ntfs_stream_pread(fs, fs->st_sds, hdr.offset + NTFS_SDS_HDR, sdlen,
			      sd, 0);
	if (r != ST_OK || ntfs_sd_check(sd, sdlen) != ST_OK) {
		kfree(sd);
		return ST_IO;
	}
	*out = sd;
	*out_len = sdlen;
	return ST_OK;
}

/* ===========================================================================
 * S29b  Security descriptors: the $Secure writer
 *
 * NTFS 3.x keeps every distinct security descriptor once, in $Secure:
 *
 *   $SDS   the descriptors, as entries {hash, id, offset, length} + the
 *          self-relative descriptor, 16-byte aligned, in 256 KB blocks;
 *          every data block is followed by its mirror copy (block 2k at
 *          [2k * 256K, (2k+1) * 256K) is mirrored at +256 KB), and no entry
 *          crosses a block boundary;
 *   $SII   view index id -> $SDS entry header (collation NTOFS_ULONG);
 *   $SDH   view index (hash, id) -> $SDS entry header (collation
 *          NTOFS_SECURITY_HASH), used to find an existing copy of a
 *          descriptor before adding a new one.
 *
 * A file names its descriptor with the security id of its 72-byte
 * $STANDARD_INFORMATION.  The hash of a descriptor is computed over its
 * 32-bit little-endian words: hash = rotate_left(hash, 3) + word.
 *
 * ntfs_secure_id() returns the id of a descriptor: an existing entry with
 * the same hash and the same bytes is reused, otherwise a new entry is
 * added with the next free id (the largest id + 1, at least 0x100):
 *
 *   1. $SDS grows when the entry and its mirror do not fit: clusters are
 *      allocated (bitmap first), zeroed and the entry + mirror written
 *      into them, and only then does the record of $Secure describe them
 *      (data size and initialized size raised); a device flush follows;
 *   2. the $SII entry, then the $SDH entry are inserted (index sessions,
 *      written through); a device flush follows;
 *   3. the caller writes the file record that names the id.
 *
 * A crash after step 1 leaves an unreferenced $SDS entry (harmless; a
 * check reclaims it); after step 2 an unused id.  Nothing ever references
 * an entry that is not on the device.  Any inconsistency met while doing
 * this is reported through ntfs_fs_error() (errors= policy).
 *
 * Volumes without $Secure (NTFS 1.x) and volumes whose $Secure is not
 * usable give ST_UNSUPPORTED; the caller then stores the descriptor in
 * the file's own $SECURITY_DESCRIPTOR attribute, as NTFS 1.x does.
 * =========================================================================== */

#define NTFS_SEC_FIRST_ID 0x100U
/* Candidates with one hash compared before giving up and adding a copy. */
#define NTFS_SEC_MAX_CAND 64
/* Entries of $SII visited to find the next id and the end of $SDS. */
#define NTFS_SEC_SCAN_MAX (1U << 22)

static const uint16_t ntfs_sii_name[4] = { '$', 'S', 'I', 'I' };
static const uint16_t ntfs_sdh_name[4] = { '$', 'S', 'D', 'H' };

/* State of the scan of $SII (fs->sec_*), done once per mount before the
 * first insertion, kept up to date by the insertions. */
struct ntfs_sec_scan {
	ntfs_fs_t *fs;
	uint32_t max_id;
	uint64_t end; /* first byte after the last entry (data copy) */
	uint8_t last[4];
	int have_last;
	uint32_t seen;
	int bad;
};

static int ntfs_sec_scan_cb(ntfs_index_t *ix, const ntfs_index_entry_t *e,
			    void *arg)
{
	struct ntfs_sec_scan *sc = (struct ntfs_sec_scan *)arg;
	ntfs_sds_header_t h;
	uint32_t id;

	(void)ix;
	if (e->key_length != 4 || e->data.vi.data_length < NTFS_SDS_HDR ||
	    (uint32_t)e->data.vi.data_offset + NTFS_SDS_HDR > e->length) {
		sc->bad = 1;
		return 1;
	}
	id = ntfs_le32(ntfs_ie_key(e));
	mm_memcpy(&h, (const uint8_t *)e + e->data.vi.data_offset, sizeof(h));
	if (h.security_id != id || h.length < NTFS_SDS_HDR ||
	    h.offset > (1ULL << 40)) {
		sc->bad = 1;
		return 1;
	}
	if (id > sc->max_id)
		sc->max_id = id;
	if (h.offset + h.length > sc->end)
		sc->end = h.offset + h.length;
	mm_memcpy(sc->last, ntfs_ie_key(e), 4);
	sc->have_last = 1;
	if (++sc->seen > NTFS_SEC_SCAN_MAX) {
		sc->bad = 1;
		return 1;
	}
	return 0;
}

static int ntfs_sec_scan(ntfs_fs_t *fs)
{
	struct ntfs_sec_scan sc;
	ntfs_index_t *ix = NULL;
	ntfs_ikey_t k;
	int r;

	if (fs->sec_valid)
		return ST_OK;
	mm_memset(&sc, 0, sizeof(sc));
	sc.fs = fs;
	r = ntfs_index_open(fs, NTFS_MAKE_MREF(NTFS_FILE_SECURE, 0),
			    ntfs_sii_name, 4, NULL, &ix);
	if (r != ST_OK)
		return r == ST_NOT_FOUND ? ST_UNSUPPORTED : r;
	if (ix->collation != NTFS_COLLATION_NTOFS_ULONG) {
		ntfs_index_close(ix);
		return ST_UNSUPPORTED;
	}
	for (;;) {
		uint32_t before = sc.seen;

		k.key = sc.last;
		k.len = 4;
		k.ic = 0;
		r = ntfs_index_walk(ix, sc.have_last ? &k : NULL,
				    ntfs_sec_scan_cb, &sc);
		if (r == ST_EOF) {
			r = ST_OK;
			break;
		}
		if (r != ST_OK || sc.bad)
			break;
		if (sc.seen == before)
			break; /* no progress: done */
	}
	ntfs_index_close(ix);
	if (r != ST_OK)
		return r;
	if (sc.bad)
		return ntfs_fs_error(fs, NTFS_FILE_SECURE, "corrupt $SII");
	fs->sec_next_id = sc.max_id + 1 > NTFS_SEC_FIRST_ID ? sc.max_id + 1 :
							       NTFS_SEC_FIRST_ID;
	fs->sec_sds_end = sc.end;
	fs->sec_valid = 1;
	return ST_OK;
}

/* Candidates of one hash, from $SDH. */
struct ntfs_sec_find {
	uint32_t hash;
	uint32_t n;
	uint32_t id[NTFS_SEC_MAX_CAND];
	uint64_t off[NTFS_SEC_MAX_CAND];
	uint32_t len[NTFS_SEC_MAX_CAND];
	uint8_t last[8];
	int done;
	int bad;
};

static int ntfs_sec_find_cb(ntfs_index_t *ix, const ntfs_index_entry_t *e,
			    void *arg)
{
	struct ntfs_sec_find *f = (struct ntfs_sec_find *)arg;
	ntfs_sds_header_t h;
	const uint8_t *key = ntfs_ie_key(e);

	(void)ix;
	if (e->key_length != 8 || e->data.vi.data_length < NTFS_SDS_HDR ||
	    (uint32_t)e->data.vi.data_offset + NTFS_SDS_HDR > e->length) {
		f->bad = 1;
		return 1;
	}
	if (ntfs_le32(key) != f->hash) {
		f->done = 1; /* past the hash */
		return 1;
	}
	mm_memcpy(f->last, key, 8);
	mm_memcpy(&h, (const uint8_t *)e + e->data.vi.data_offset, sizeof(h));
	if (f->n == NTFS_SEC_MAX_CAND) {
		f->done = 1;
		return 1;
	}
	f->id[f->n] = ntfs_le32(key + 4);
	f->off[f->n] = h.offset;
	f->len[f->n] = h.length;
	f->n++;
	return 0;
}

/* An existing entry holding exactly `sd': *id.  ST_NOT_FOUND when none. */
static int ntfs_sec_lookup(ntfs_fs_t *fs, const uint8_t *sd, uint32_t len,
			   uint32_t hash, uint32_t *id)
{
	struct ntfs_sec_find *f;
	ntfs_index_t *ix = NULL;
	ntfs_ikey_t k;
	uint8_t *buf = NULL;
	int r;

	f = (struct ntfs_sec_find *)ntfs_zalloc(sizeof(*f));
	if (!f)
		return ST_NOMEM;
	f->hash = hash;
	ntfs_put_le32(f->last, hash);
	ntfs_put_le32(f->last + 4, 0);
	r = ntfs_index_open(fs, NTFS_MAKE_MREF(NTFS_FILE_SECURE, 0),
			    ntfs_sdh_name, 4, NULL, &ix);
	if (r != ST_OK) {
		kfree(f);
		return r == ST_NOT_FOUND ? ST_UNSUPPORTED : r;
	}
	if (ix->collation != NTFS_COLLATION_NTOFS_SECURITY_HASH) {
		ntfs_index_close(ix);
		kfree(f);
		return ST_UNSUPPORTED;
	}
	while (!f->done && !f->bad) {
		uint32_t before = f->n;

		k.key = f->last;
		k.len = 8;
		k.ic = 0;
		r = ntfs_index_walk(ix, &k, ntfs_sec_find_cb, f);
		if (r == ST_EOF) {
			r = ST_OK;
			break;
		}
		if (r != ST_OK)
			break;
		if (f->n == before)
			break;
	}
	ntfs_index_close(ix);
	if (r == ST_OK && f->bad)
		r = ntfs_fs_error(fs, NTFS_FILE_SECURE, "corrupt $SDH");
	if (r != ST_OK) {
		kfree(f);
		return r;
	}
	r = ST_NOT_FOUND;
	buf = (uint8_t *)kalloc(len);
	if (!buf) {
		kfree(f);
		return ST_NOMEM;
	}
	for (uint32_t i = 0; i < f->n; i++) {
		ntfs_sds_header_t h;

		if (f->len[i] != NTFS_SDS_HDR + len ||
		    !ntfs_range_ok(f->off[i], f->len[i],
				   fs->st_sds->data_size))
			continue;
		if (ntfs_stream_pread(fs, fs->st_sds, f->off[i], sizeof(h), &h,
				      0) != ST_OK ||
		    h.security_id != f->id[i] || h.hash != hash ||
		    h.offset != f->off[i] || h.length != f->len[i])
			continue;
		if (ntfs_stream_pread(fs, fs->st_sds, f->off[i] + NTFS_SDS_HDR,
				      len, buf, 0) != ST_OK)
			continue;
		if (kmemcmp(buf, sd, len) == 0) {
			*id = f->id[i];
			r = ST_OK;
			break;
		}
	}
	kfree(buf);
	kfree(f);
	return r;
}

/* Grow $SDS so that it is allocated up to at least `need' bytes: new
 * clusters are allocated and appended to the
 * runlist kept in `nrl' (the stream's own runlist is untouched until the
 * caller commits).  *newruns / *nnew: what was allocated (for freeing on
 * failure). */
static int ntfs_sds_grow(ntfs_fs_t *fs, uint64_t need, ntfs_runlist_t *nrl,
			 ntfs_run_t **newruns, uint32_t *nnew)
{
	ntfs_stream_t *st = fs->st_sds;
	uint64_t have = (uint64_t)nrl->end_vcn << fs->cluster_bits;
	uint64_t cl;
	int64_t goal = -1;
	int r;

	*newruns = NULL;
	*nnew = 0;
	if (need <= have)
		return ST_OK;
	/* Whole 256 KB blocks: the next data block and its mirror. */
	need = (need + NTFS_SDS_BLOCK - 1) & ~(uint64_t)(NTFS_SDS_BLOCK - 1);
	cl = (need - have + fs->cluster_size - 1) >> fs->cluster_bits;
	if (nrl->count && nrl->runs[nrl->count - 1].lcn >= 0)
		goal = nrl->runs[nrl->count - 1].lcn +
		       nrl->runs[nrl->count - 1].len;
	r = ntfs_clusters_alloc(fs, goal, cl, NTFS_AL_META, newruns, nnew);
	if (r != ST_OK)
		return r;
	/* The new clusters lie beyond the initialized size: the caller
	 * writes zeros up to the entries it adds, the rest reads as zeros
	 * until a later entry initializes it. */
	for (uint32_t i = 0; r == ST_OK && i < *nnew; i++)
		r = ntfs_rl_append(nrl, nrl->end_vcn, (*newruns)[i].lcn,
				   (*newruns)[i].len);
	(void)st;
	return r;
}

/* Add `sd' (`len' bytes, valid) as a new entry; *id_out its id. */
static int ntfs_sec_insert(ntfs_fs_t *fs, const uint8_t *sd, uint32_t len,
			   uint32_t hash, uint32_t *id_out)
{
	ntfs_stream_t *st = fs->st_sds;
	uint32_t elen = NTFS_SDS_HDR + len, id = fs->sec_next_id;
	uint32_t alen = (elen + 15) & ~15U;
	uint64_t off, nsize, old_init = st->init_size, old_data = st->data_size;
	ntfs_runlist_t nrl;
	ntfs_run_t *newruns = NULL;
	uint32_t nnew = 0;
	ntfs_eino_t *e = NULL;
	ntfs_ixs_t *s = NULL;
	uint8_t *ent = NULL, *zero = NULL;
	ntfs_sds_header_t h;
	int r;

	if (id == 0 || id >= 0xFFFFFFF0U)
		return ST_NOSPC;
	/* Place the entry: 16-byte aligned after the last one, inside a
	 * data (even) block, wholly inside it. */
	off = (fs->sec_sds_end + 15) & ~15ULL;
	if ((off / NTFS_SDS_BLOCK) & 1)
		off = (off / NTFS_SDS_BLOCK + 1) * NTFS_SDS_BLOCK;
	if (off % NTFS_SDS_BLOCK + alen > NTFS_SDS_BLOCK)
		off = (off / NTFS_SDS_BLOCK + 2) * NTFS_SDS_BLOCK;
	nsize = off + NTFS_SDS_BLOCK + elen; /* through the mirror copy */
	if (nsize < old_data)
		nsize = old_data;
	r = ntfs_rl_copy(&nrl, &st->rl);
	if (r != ST_OK)
		return r;
	r = ntfs_sds_grow(fs, nsize, &nrl, &newruns, &nnew);
	if (r != ST_OK)
		goto fail;
	ent = (uint8_t *)ntfs_zalloc(alen);
	zero = (uint8_t *)ntfs_zalloc(PAGE_SIZE);
	if (!ent || !zero) {
		r = ST_NOMEM;
		goto fail;
	}
	h.hash = hash;
	h.security_id = id;
	h.offset = off;
	h.length = elen;
	mm_memcpy(ent, &h, sizeof(h));
	mm_memcpy(ent + NTFS_SDS_HDR, sd, len);
	/* The stream's runlist covers the new clusters while they are
	 * written (wt_write maps through it); the record of $Secure does
	 * not describe them yet. */
	{
		ntfs_runlist_t keep = st->rl;
		uint64_t keep_alloc = st->alloc_size;

		st->rl = nrl;
		st->alloc_size = (uint64_t)nrl.end_vcn << fs->cluster_bits;
		/* Bytes between the old initialized size and the entries
		 * read as zeros on the device from now on. */
		for (uint64_t p = old_init; r == ST_OK && p < nsize;) {
			uint64_t n = ntfs_min64(PAGE_SIZE - p % PAGE_SIZE,
						nsize - p);

			r = ntfs_wt_write(fs, st, p, zero, n);
			p += n;
		}
		if (r == ST_OK)
			r = ntfs_wt_write(fs, st, off, ent, elen);
		if (r == ST_OK)
			r = ntfs_wt_write(fs, st, off + NTFS_SDS_BLOCK, ent,
					  elen);
		st->rl = keep;
		st->alloc_size = keep_alloc;
	}
	if (r != ST_OK)
		goto fail;
	r = ntfs_barrier(fs);
	if (r != ST_OK)
		goto fail;
	/* The record of $Secure describes the clusters and the sizes. */
	r = ntfs_eino_load(fs, NTFS_MAKE_MREF(NTFS_FILE_SECURE, 0), &e);
	if (r == ST_OK)
		r = ntfs_eino_set_nonres(e, NTFS_AT_DATA, st->name, st->name_len,
					 0, 0, &nrl,
					 (uint64_t)nrl.end_vcn
						 << fs->cluster_bits,
					 nsize, nsize, 0);
	if (r == ST_OK)
		r = ntfs_eino_commit(e);
	ntfs_eino_free(e);
	e = NULL;
	if (r != ST_OK)
		goto fail;
	/* Published: the stream follows the record. */
	ntfs_free(newruns);
	newruns = NULL;
	nnew = 0;
	ntfs_rl_free(&st->rl);
	st->rl = nrl;
	nrl.runs = NULL;
	st->alloc_size = (uint64_t)st->rl.end_vcn << fs->cluster_bits;
	st->data_size = st->init_size = nsize;
	fs->sec_sds_end = off + elen;
	fs->sec_next_id = id + 1;
	r = ntfs_barrier(fs);
	if (r != ST_OK)
		goto out;
	/* $SII: id -> header. */
	r = ntfs_ixs_open(fs, NTFS_MAKE_MREF(NTFS_FILE_SECURE, 0),
			  ntfs_sii_name, 4, &s);
	if (r == ST_OK) {
		uint8_t *ie = (uint8_t *)ntfs_zalloc(40);
		ntfs_index_entry_t *x = (ntfs_index_entry_t *)ie;

		if (!ie) {
			ntfs_ixs_abort(s);
			r = ST_NOMEM;
		} else {
			x->data.vi.data_offset = NTFS_IE_HDR + 4;
			x->data.vi.data_length = NTFS_SDS_HDR;
			x->length = 40;
			x->key_length = 4;
			ntfs_put_le32(ie + NTFS_IE_HDR, id);
			mm_memcpy(ie + NTFS_IE_HDR + 4, &h, NTFS_SDS_HDR);
			r = ntfs_ixs_insert(s, ie);
			if (r == ST_OK)
				r = ntfs_ixs_commit(s);
			else
				ntfs_ixs_abort(s);
		}
	}
	if (r != ST_OK)
		goto out;
	/* $SDH: (hash, id) -> header, padded like other writers do. */
	r = ntfs_ixs_open(fs, NTFS_MAKE_MREF(NTFS_FILE_SECURE, 0),
			  ntfs_sdh_name, 4, &s);
	if (r == ST_OK) {
		uint8_t *ie = (uint8_t *)ntfs_zalloc(48);
		ntfs_index_entry_t *x = (ntfs_index_entry_t *)ie;

		if (!ie) {
			ntfs_ixs_abort(s);
			r = ST_NOMEM;
		} else {
			x->data.vi.data_offset = NTFS_IE_HDR + 8;
			x->data.vi.data_length = NTFS_SDS_HDR;
			x->length = 48;
			x->key_length = 8;
			ntfs_put_le32(ie + NTFS_IE_HDR, hash);
			ntfs_put_le32(ie + NTFS_IE_HDR + 4, id);
			mm_memcpy(ie + NTFS_IE_HDR + 8, &h, NTFS_SDS_HDR);
			ie[44] = 'I';
			ie[46] = 'I';
			r = ntfs_ixs_insert(s, ie);
			if (r == ST_OK)
				r = ntfs_ixs_commit(s);
			else
				ntfs_ixs_abort(s);
		}
	}
	if (r != ST_OK) {
		/* $SII names the entry, $SDH does not: the id stays valid
		 * (it is only not found again by hash); recorded for a
		 * check, the volume stays dirty. */
		ntfs_fs_error(fs, NTFS_FILE_SECURE, "$SDH entry missing");
		goto out;
	}
	r = ntfs_barrier(fs);
	if (r == ST_OK)
		*id_out = id;
	goto out;
fail:
	/* Nothing references the new clusters: back to free. */
	for (uint32_t i = 0; i < nnew; i++)
		ntfs_clusters_free(fs, newruns[i].lcn, (uint64_t)newruns[i].len);
out:
	ntfs_free(newruns);
	ntfs_rl_free(&nrl);
	ntfs_free(ent);
	ntfs_free(zero);
	return r;
}

/* The security id of descriptor `sd' (`len' bytes): an existing one or a
 * new entry.  ST_UNSUPPORTED when the volume keeps no $Secure. */
int ntfs_secure_id(ntfs_fs_t *fs, const uint8_t *sd, uint32_t len,
		   uint32_t *id)
{
	uint32_t hash;
	int r;

	ntfs_assert_meta_excl(fs);
	if (fs->major_ver < 3 || !fs->st_sds || fs->sec_broken)
		return ST_UNSUPPORTED;
	if (!ntfs_sd_valid(sd, len))
		return ST_INVALID;
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	r = ntfs_sec_scan(fs);
	if (r == ST_UNSUPPORTED)
		fs->sec_broken = 1;
	if (r != ST_OK)
		return r;
	hash = ntfs_sd_hash(sd, len);
	r = ntfs_sec_lookup(fs, sd, len, hash, id);
	if (r == ST_UNSUPPORTED)
		fs->sec_broken = 1;
	if (r != ST_NOT_FOUND)
		return r;
	return ntfs_sec_insert(fs, sd, len, hash, id);
}

/* Make `e' name its descriptor by security id `id' (a 72-byte
 * $STANDARD_INFORMATION) and drop a descriptor attribute of its own. */
int ntfs_eino_set_secid(ntfs_eino_t *e, uint32_t id)
{
	ntfs_standard_information_t si;
	long i = ntfs_eino_find(e, NTFS_AT_STANDARD_INFORMATION, NULL, 0, 0);
	const ntfs_attr_record_t *a;
	uint32_t vl;
	int r;

	if (i < 0)
		return ST_IO;
	a = ntfs_ea_hdr(&e->a[i]);
	if (a->non_resident)
		return ST_IO;
	vl = a->data.resident.value_length;
	if (vl < NTFS_SI_SIZE_V12)
		return ST_IO;
	mm_memset(&si, 0, sizeof(si));
	mm_memcpy(&si, (const uint8_t *)a + a->data.resident.value_offset,
		  vl < sizeof(si) ? vl : sizeof(si));
	si.security_id = id;
	r = ntfs_eino_set_res(e, NTFS_AT_STANDARD_INFORMATION, NULL, 0, 0, &si,
			      NTFS_SI_SIZE_V3);
	if (r == ST_OK)
		r = ntfs_eino_drop(e, NTFS_AT_SECURITY_DESCRIPTOR, NULL, 0, 1);
	return r;
}

/* Give `e' the descriptor `sd': through $Secure where the volume has it,
 * else as its own $SECURITY_DESCRIPTOR attribute (with security id 0 in
 * a 72-byte $STANDARD_INFORMATION, which would otherwise win). */
int ntfs_eino_set_sd(ntfs_eino_t *e, const uint8_t *sd, uint32_t len)
{
	uint32_t id = 0;
	int r = ntfs_secure_id(e->fs, sd, len, &id);

	if (r == ST_OK)
		return ntfs_eino_set_secid(e, id);
	if (r != ST_UNSUPPORTED)
		return r;
	if (!ntfs_sd_valid(sd, len))
		return ST_INVALID;
	r = ntfs_eino_set_value(e, NTFS_AT_SECURITY_DESCRIPTOR, NULL, 0, sd,
				len, e->fs->rec_size / 2);
	if (r != ST_OK)
		return r;
	{
		long i = ntfs_eino_find(e, NTFS_AT_STANDARD_INFORMATION, NULL,
					0, 0);
		ntfs_attr_record_t *a;

		if (i < 0)
			return ST_IO;
		a = ntfs_ea_hdr(&e->a[i]);
		if (!a->non_resident &&
		    a->data.resident.value_length >= NTFS_SI_SIZE_V3) {
			ntfs_standard_information_t *si =
				(ntfs_standard_information_t
					 *)((uint8_t *)a +
					    a->data.resident.value_offset);

			si->security_id = 0;
			e->a[i].dirty = 1;
		}
	}
	return ST_OK;
}
