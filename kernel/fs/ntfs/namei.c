/* LikeOS - NTFS filesystem driver
 *
 * Namespace operations: create, link, unlink, rename, attributes.
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
 * S31  Name checks for new names and named streams
 * =========================================================================== */

int ntfs_win_name_ok(const uint16_t *u, uint32_t n)
{
	static const char *const dev[] = { "CON", "PRN", "AUX", "NUL" };
	uint32_t base = n;

	for (uint32_t i = 0; i < n; i++) {
		uint16_t c = ntfs_le16(&u[i]);

		if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' ||
		    c == '<' || c == '>' || c == '?' || c == '\\' || c == '|')
			return 0;
		if (c == '.' && base == n)
			base = i;
	}
	if (n && (ntfs_le16(&u[n - 1]) == ' ' || ntfs_le16(&u[n - 1]) == '.'))
		return 0;
	/* Reserved device names, also with an extension. */
	for (uint32_t d = 0; d < 4; d++) {
		if (base != 3)
			break;
		if ((ntfs_le16(&u[0]) & ~0x20U) == (uint16_t)dev[d][0] &&
		    (ntfs_le16(&u[1]) & ~0x20U) == (uint16_t)dev[d][1] &&
		    (ntfs_le16(&u[2]) & ~0x20U) == (uint16_t)dev[d][2])
			return 0;
	}
	if (base == 4) {
		uint16_t c0 = ntfs_le16(&u[0]) & ~0x20U;
		uint16_t c1 = ntfs_le16(&u[1]) & ~0x20U;
		uint16_t c2 = ntfs_le16(&u[2]) & ~0x20U;
		uint16_t c3 = ntfs_le16(&u[3]);

		if (((c0 == 'C' && c1 == 'O' && c2 == 'M') ||
		     (c0 == 'L' && c1 == 'P' && c2 == 'T')) &&
		    c3 >= '1' && c3 <= '9')
			return 0;
	}
	return 1;
}

int ntfs_stream_name_ok(const uint16_t *name, uint32_t nlen)
{
	if (!nlen || nlen > NTFS_MAX_NAME_LEN)
		return 0;
	for (uint32_t i = 0; i < nlen; i++) {
		uint16_t c = ntfs_le16(&name[i]);

		if (c == 0 || c == ':' || c == '/' || c == '\\')
			return 0;
	}
	return 1;
}

/* ===========================================================================
 * S31  Namespace operations: create, mkdir, symlink, mknod, link, unlink,
 *      rmdir, rename; ownership, mode and times; open files and orphans
 *
 * Names.  A new name is stored in the POSIX namespace (case-sensitive,
 * any character but NUL and '/'), as the feature model does; with
 * `windows_names' names Windows cannot use are refused; with
 * `nocase' (lookups ignore case, so no two names of a directory differ in
 * case only) names valid in the Win32 namespace go into it -- without it a
 * name differing in case from another one must stay POSIX: Win32 names
 * are unique regardless of case.  No DOS
 * 8.3 names are created; a DOS alias of a name that is removed or renamed
 * goes with it.
 *
 * New files: an MFT record (S25) with a 48-byte $STANDARD_INFORMATION,
 * a $FILE_NAME, an own $SECURITY_DESCRIPTOR granting everyone full access
 * (zero-filled before it is set up), an empty resident $DATA (files) or
 * an empty $INDEX_ROOT $I30 (directories), and the WSL
 * EAs $LXUID / $LXGID / $LXMOD when owner or mode differ from what the
 * mount shows anyway.  Symbolic links are WSL LX_SYMLINK reparse
 * points (UTF-8 target stored as given) or, with symlink=native,
 * IO_REPARSE_TAG_SYMLINK points with UTF-16 names; mknod makes WSL
 * special-file reparse points with RECALL_ON_OPEN kept and $LXDEV.
 * Every reparse point is entered in $Extend\$Reparse:$R (a failure
 * there fails the creation, which is rolled back).
 *
 * Order: the record is written in use before the directory entry refers
 * to it (a power cut leaves an unreferenced record: a leak); removal takes
 * the directory entry away first, then the name from the record, and
 * frees the record and its clusters only when no name is left and nobody
 * has the file open (unlinked open files are deleted at their last close;
 * a power cut in between leaves an orphan record for chkdsk).  rename adds
 * the new name before it removes the old one (a power cut leaves both:
 * a second hard link, never a lost file).  The child-before-parent rule
 * holds trivially: every namespace change runs under the metadata lock
 * exclusive.
 * =========================================================================== */

/* ---- open-file table (files with open handles; deletion is deferred to
 * the last close) ---- */

int ntfs_ofile_open(ntfs_fs_t *fs, uint64_t no)
{
	struct ntfs_ofile *o, *n = (struct ntfs_ofile *)ntfs_zalloc(sizeof(*n));
	uint64_t fl;

	spin_lock_irqsave(&fs->hdl_lock, &fl);
	for (o = fs->ofiles[no % 64]; o; o = o->next)
		if (o->no == no)
			break;
	if (o) {
		o->opens++;
	} else if (n) {
		n->no = no;
		n->opens = 1;
		n->next = fs->ofiles[no % 64];
		fs->ofiles[no % 64] = n;
		n = NULL;
	} else {
		spin_unlock_irqrestore(&fs->hdl_lock, fl);
		return ST_NOMEM;
	}
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	ntfs_free(n);
	return ST_OK;
}

/* 1 when this was the last open of a file whose last name is gone. */
int ntfs_ofile_close(ntfs_fs_t *fs, uint64_t no)
{
	struct ntfs_ofile **pp, *o = NULL;
	uint64_t fl;
	int del = 0;

	spin_lock_irqsave(&fs->hdl_lock, &fl);
	for (pp = &fs->ofiles[no % 64]; *pp; pp = &(*pp)->next)
		if ((*pp)->no == no) {
			o = *pp;
			if (--o->opens <= 0) {
				*pp = o->next;
				del = o->unlinked;
			} else {
				o = NULL;
			}
			break;
		}
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	ntfs_free(o);
	return del;
}

/* Is file `no' open?  With `mark', remember that its last name went. */
static int ntfs_ofile_busy(ntfs_fs_t *fs, uint64_t no, int mark)
{
	struct ntfs_ofile *o;
	uint64_t fl;
	int busy = 0;

	spin_lock_irqsave(&fs->hdl_lock, &fl);
	for (o = fs->ofiles[no % 64]; o; o = o->next)
		if (o->no == no) {
			busy = o->opens > 0;
			if (mark)
				o->unlinked = 1;
			break;
		}
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	return busy;
}

/* ---- names ---- */

/* A new name: UTF-8 -> UTF-16 (`u' has NTFS_MAX_NAME_LEN units), the
 * checks of the options; *ntype the namespace. */
int ntfs_new_name(ntfs_fs_t *fs, const char *name, uint32_t nlen,
		  uint16_t *u, uint32_t *ulen, uint8_t *ntype)
{
	long n;

	if (nlen == 0 || (nlen == 1 && name[0] == '.') ||
	    (nlen == 2 && name[0] == '.' && name[1] == '.'))
		return ST_EXISTS;
	if (nlen > VFS_NAME_MAX)
		return ST_NAMETOOLONG;
	n = ntfs_utf8_to_utf16(name, nlen, u, NTFS_MAX_NAME_LEN);
	if (n == ST_NAMETOOLONG)
		return ST_NAMETOOLONG;
	if (n <= 0)
		return ST_INVALID;
	if (fs->opt.streams == NTFS_STREAMS_WINDOWS) {
		for (long i = 0; i < n; i++)
			if (ntfs_le16(&u[i]) == ':')
				return ST_INVALID;
	}
	if (fs->opt.windows_names && !ntfs_win_name_ok(u, (uint32_t)n))
		return ST_INVALID;
	*ulen = (uint32_t)n;
	*ntype = (fs->opt.nocase && ntfs_win_name_ok(u, (uint32_t)n)) ?
			 NTFS_FILE_NAME_WIN32 :
			 NTFS_FILE_NAME_POSIX;
	return ST_OK;
}

/* Forget the dcache entry of `name' in directory `dir'. */
void ntfs_dc_forget(ntfs_fs_t *fs, uint64_t dir, const char *name,
		    uint32_t nlen)
{
	ntfs_stream_t *ds = NULL;
	char *key = (char *)kalloc(DC_NAME_MAX + 1);
	uint16_t *u16 = (uint16_t *)kalloc(NTFS_MAX_NAME_LEN * 2);

	if (key && u16 && ntfs_dir_stream(fs, dir, &ds) == ST_OK) {
		if (ntfs_dc_key(fs, name, nlen, key, u16))
			dcache_invalidate_cs(&fs->sb, ntfs_chain(ds), key);
		else
			dcache_invalidate_dir(&fs->sb, ntfs_chain(ds));
		ntfs_stream_put(fs, ds);
	} else if (ds) {
		ntfs_stream_put(fs, ds);
	}
	ntfs_free(key);
	ntfs_free(u16);
}

/* The same for a name in UTF-16. */
static void ntfs_dc_forget16(ntfs_fs_t *fs, uint64_t dir, const uint16_t *u,
			     uint32_t n)
{
	char *nm = (char *)kalloc(NTFS_MAX_NAME_LEN * 3 + 1);
	long l;

	if (!nm)
		return;
	l = ntfs_utf16_to_utf8(u, n, nm, NTFS_MAX_NAME_LEN * 3 + 1);
	if (l > 0)
		ntfs_dc_forget(fs, dir, nm, (uint32_t)l);
	kfree(nm);
}

/* ---- standard information ---- */

ntfs_standard_information_t *ntfs_eino_si(ntfs_eino_t *e)
{
	long i = ntfs_eino_find(e, NTFS_AT_STANDARD_INFORMATION, NULL, 0, 0);
	ntfs_attr_record_t *a;

	if (i < 0)
		return NULL;
	a = ntfs_ea_hdr(&e->a[i]);
	if (a->non_resident || a->data.resident.value_length < NTFS_SI_SIZE_V12)
		return NULL;
	e->a[i].dirty = 1;
	return (ntfs_standard_information_t *)(e->a[i].b +
					       a->data.resident.value_offset);
}

/* ---- reparse points in $Extend\$Reparse:$R ---- */

static const uint16_t ntfs_r_name[2] = { '$', 'R' };

static int ntfs_reparse_index(ntfs_fs_t *fs, uint64_t mref, uint32_t tag,
			      int add)
{
	uint64_t rmref;
	ntfs_ixs_t *s;
	uint8_t key[12];
	int r;

	r = ntfs_dir_lookup(fs, NTFS_MAKE_MREF(NTFS_FILE_EXTEND, 0), "$Reparse",
			    8, &rmref, NULL);
	if (r == ST_NOT_FOUND || r == ST_NOTDIR)
		return ST_OK; /* a volume without the index */
	if (r != ST_OK)
		return r;
	r = ntfs_ixs_open(fs, rmref, ntfs_r_name, 2, &s);
	if (r != ST_OK)
		return r;
	ntfs_put_le32(key, tag);
	ntfs_put_le32(key + 4, (uint32_t)mref);
	ntfs_put_le32(key + 8, (uint32_t)(mref >> 32));
	if (add) {
		uint8_t *e = (uint8_t *)ntfs_zalloc(32);
		ntfs_index_entry_t *ie = (ntfs_index_entry_t *)e;

		if (!e) {
			ntfs_ixs_abort(s);
			return ST_NOMEM;
		}
		ie->data.vi.data_offset = NTFS_IE_HDR + 12;
		ie->data.vi.data_length = 0;
		ie->length = 32;
		ie->key_length = 12;
		mm_memcpy(e + NTFS_IE_HDR, key, 12);
		r = ntfs_ixs_insert(s, e);
		if (r == ST_EXISTS)
			r = ST_OK;
	} else {
		ntfs_ikey_t k;

		k.key = key;
		k.len = 12;
		k.ic = 0;
		r = ntfs_ixs_delete_commit(s, &k, 0);
		return r == ST_NOT_FOUND ? ST_OK : r;
	}
	if (r != ST_OK) {
		ntfs_ixs_abort(s);
		return r;
	}
	return ntfs_ixs_commit(s);
}

/* ---- $FILE_NAME duplicates in the parent indexes ---- */

/* Bring every $FILE_NAME entry of file `mref' in its directories up to
 * date with the file's times, sizes and attributes (the duplicated
 * information other systems list directories from). */
int ntfs_update_dups(ntfs_fs_t *fs, uint64_t mref)
{
	ntfs_eino_t *e;
	ntfs_standard_information_t si;
	uint64_t alloc = 0, data = 0;
	int is_dir, r;
	long i;

	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		return r;
	i = ntfs_eino_find(e, NTFS_AT_STANDARD_INFORMATION, NULL, 0, 0);
	if (i < 0 || ntfs_ea_hdr(&e->a[i])->non_resident) {
		ntfs_eino_free(e);
		return ST_IO;
	}
	mm_memcpy(&si, e->a[i].b + ntfs_ea_hdr(&e->a[i])->data.resident.value_offset,
		  NTFS_SI_SIZE_V12);
	is_dir = !!(e->rflags & NTFS_MFT_REC_IS_DIRECTORY);
	i = ntfs_eino_find(e, NTFS_AT_DATA, ntfs_unnamed, 0, 0);
	if (i >= 0) {
		ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);

		if (!a->non_resident) {
			data = a->data.resident.value_length;
			alloc = (data + 7) & ~7ULL;
		} else {
			alloc = (uint64_t)a->data.non_resident.allocated_size;
			data = (uint64_t)a->data.non_resident.data_size;
			if (a->flags & (NTFS_ATTR_IS_SPARSE |
					NTFS_ATTR_COMPRESSION_MASK))
				alloc = (uint64_t)
					a->data.non_resident.compressed_size;
		}
	}
	r = ST_OK;
	for (i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0, 0); i >= 0;
	     i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0, (uint32_t)i + 1)) {
		ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);
		uint32_t fl = a->data.resident.value_length;
		uint8_t *fn = (uint8_t *)kalloc(fl), *ie;
		ntfs_file_name_t *f;
		ntfs_ixs_t *s;
		ntfs_ikey_t k;
		int rr;

		if (!fn) {
			r = ST_NOMEM;
			break;
		}
		mm_memcpy(fn, ntfs_attr_value(a), fl);
		f = (ntfs_file_name_t *)fn;
		if (NTFS_MREF_NO(f->parent_directory) == e->no) {
			kfree(fn); /* the root is its own parent */
			continue;
		}
		f->creation_time = si.creation_time;
		f->last_data_change_time = si.last_data_change_time;
		f->last_mft_change_time = si.last_mft_change_time;
		f->last_access_time = si.last_access_time;
		f->allocated_size = is_dir ? 0 : (int64_t)alloc;
		f->data_size = is_dir ? 0 : (int64_t)data;
		f->file_attributes =
			(si.file_attributes & ~NTFS_FILE_ATTR_DIRECTORY) |
			(is_dir ? NTFS_FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT : 0);
		ie = ntfs_ie_build(NTFS_MAKE_MREF(e->no, e->seq), fn, fl);
		if (!ie) {
			kfree(fn);
			r = ST_NOMEM;
			break;
		}
		rr = ntfs_ixs_open(fs, f->parent_directory, ntfs_i30_name, 4,
				   &s);
		if (rr == ST_OK) {
			k.key = ntfs_fn_name(f);
			k.len = f->file_name_length;
			k.ic = 0;
			rr = ntfs_ixs_update(s, &k, ie);
			if (rr == ST_OK)
				rr = ntfs_ixs_commit(s);
			else
				ntfs_ixs_abort(s);
		}
		if (rr == ST_NOT_FOUND)
			rr = ST_OK; /* a name without an entry: nothing to do */
		if (rr != ST_OK && r == ST_OK)
			r = rr;
		kfree(ie);
		kfree(fn);
	}
	ntfs_eino_free(e);
	return r;
}

/* ---- deleting a file ---- */

static void ntfs_streams_kill(ntfs_fs_t *fs, uint64_t no);

/* Free file `mref' (no name left, nobody has it open): the record is
 * written as not in use first, then the clusters of every non-resident
 * attribute, the extension records and the bits are given back. */
static int ntfs_file_delete_do(ntfs_fs_t *fs, uint64_t mref)
{
	ntfs_eino_t *e;
	ntfs_runlist_t all;
	uint32_t tag = 0;
	int r;

	ntfs_streams_kill(fs, NTFS_MREF_NO(mref));
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		return r;
	mm_memset(&all, 0, sizeof(all));
	{
		uint8_t *rpv = NULL;
		uint32_t rpl = 0;

		if (ntfs_eino_value(e, NTFS_AT_REPARSE_POINT, ntfs_unnamed, 0,
				    NTFS_REPARSE_MAX, &rpv, &rpl) == ST_OK &&
		    rpl >= 4)
			tag = ntfs_le32(rpv);
		ntfs_free(rpv);
	}
	/* Collect the clusters of every non-resident attribute. */
	for (uint32_t i = 0; r == ST_OK && i < e->n; i++) {
		ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);
		ntfs_runlist_t rl;

		if (!a->non_resident)
			continue;
		mm_memset(&rl, 0, sizeof(rl));
		/* Each extent decodes on its own (its VCNs start where the
		 * extent says). */
		{
			uint8_t *copy = (uint8_t *)kalloc(a->length);

			if (!copy) {
				r = ST_NOMEM;
				break;
			}
			mm_memcpy(copy, a, a->length);
			((ntfs_attr_record_t *)copy)->data.non_resident.lowest_vcn = 0;
			((ntfs_attr_record_t *)copy)->data.non_resident.highest_vcn =
				a->data.non_resident.highest_vcn -
				a->data.non_resident.lowest_vcn;
			r = ntfs_mp_decode(fs, (ntfs_attr_record_t *)copy,
					   a->length, &rl);
			kfree(copy);
		}
		for (uint32_t k = 0; r == ST_OK && k < rl.count; k++)
			if (rl.runs[k].lcn != NTFS_LCN_HOLE)
				r = ntfs_runs_add(&all.runs, &all.count,
						  &all.cap, rl.runs[k].lcn,
						  rl.runs[k].len);
		ntfs_rl_free(&rl);
	}
	if (e->al_nonres)
		for (uint32_t k = 0; r == ST_OK && k < e->al_rl.count; k++)
			if (e->al_rl.runs[k].lcn != NTFS_LCN_HOLE)
				r = ntfs_runs_add(&all.runs, &all.count,
						  &all.cap, e->al_rl.runs[k].lcn,
						  e->al_rl.runs[k].len);
	if (r == ST_OK && tag)
		r = ntfs_reparse_index(fs, mref, tag, 0);
	if (r == ST_OK)
		r = ntfs_write_begin(fs);
	if (r == ST_OK)
		r = ntfs_mft_free(fs, e->no); /* not in use: dereferenced */
	if (r == ST_OK) {
		for (uint32_t k = 0; k < all.count; k++)
			ntfs_clusters_free(fs, all.runs[k].lcn,
					   (uint64_t)all.runs[k].len);
		for (uint32_t k = 0; k < e->nx; k++)
			ntfs_mft_free(fs, e->x[k].no);
	}
	ntfs_free(all.runs);
	ntfs_eino_free(e);
	return r;
}

/* A file that cannot be deleted (completely) stays behind for a check:
 * an orphan record, clusters marked in use. */
int ntfs_file_delete(ntfs_fs_t *fs, uint64_t mref)
{
	int r = ntfs_file_delete_do(fs, mref);

	if (r != ST_OK)
		ntfs_leaked(fs);
	return r;
}

/* Stream `st' is dead: its pages (dirty ones too: nothing may write them
 * back into clusters about to be freed) and reservations are dropped, no
 * commit writes it again, and lookups no longer find it. */
static void ntfs_stream_kill(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	ntfs_assert_meta_excl(fs);
	st->flags |= NTFS_ST_DEAD;
	st->flags &= ~(NTFS_ST_META | NTFS_ST_DIRTY);
	pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
	ntfs_unreserve_from(fs, st, 0);
	if (st->flags & NTFS_ST_INDEX)
		ntfs_ic_drop_stream(fs, st->handle);
	ntfs_stream_unpin(fs, st);
}

/* Every stream of file `no' is dead. */
static void ntfs_streams_kill(ntfs_fs_t *fs, uint64_t no)
{
	for (uint32_t h = NTFS_HANDLE_FIRST_DYNAMIC; h < NTFS_SLOTS; h++) {
		ntfs_stream_t *st = ntfs_slot(fs, h);

		if (!st || st->mft_no != no || (st->flags & NTFS_ST_DEAD))
			continue;
		ntfs_stream_kill(fs, st);
	}
}

/* ---- named streams: creation and removal ----
 *
 * A named stream is a $DATA attribute with a name.  It is created empty
 * and resident; writes make it grow (and non-resident) like any file's
 * data, truncate resizes it.  Stream names follow the naming rules of the
 * format: 1..255 UTF-16 units, no NUL, '/', '\' or ':'; two streams of one
 * file never differ only in case (names are compared through $UpCase, as
 * other systems look them up case-insensitively).
 *
 * Removal refuses a stream that is open (EBUSY).  The stream's
 * cached pages and reservations are dropped BEFORE the record no longer
 * describes it (so writeback can never store pages into clusters that are
 * being freed); the clusters are freed after the base record (and its
 * extension records) were written without the attribute (S27's deferred
 * frees).  The base file's ctime records both changes. */

/* Index of the $DATA attribute named (name, nlen) of `e', matching case
 * through $UpCase when `ic'; -1 when there is none.  *exact receives the
 * stored spelling (nlen units). */
static long ntfs_eino_find_stream(ntfs_eino_t *e, const uint16_t *name,
				  uint32_t nlen, int ic, uint16_t *exact)
{
	for (uint32_t i = 0; i < e->n; i++) {
		const ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);
		const uint16_t *an;

		if (a->type != NTFS_AT_DATA || a->name_length != nlen || !nlen)
			continue;
		an = ntfs_attr_name(a);
		if (ic ? !ntfs_upcase_eq(e->fs, an, name, nlen) :
			 kmemcmp(an, name, nlen * 2U) != 0)
			continue;
		if (exact)
			mm_memcpy(exact, an, nlen * 2U);
		return (long)i;
	}
	return -1;
}

/* May the streams of file `mref' be created or removed? */
static int ntfs_stream_owner_ok(ntfs_fs_t *fs, uint64_t mref)
{
	return !ntfs_protected(fs, mref);
}

/* Create the empty named stream (name, nlen) of `mref'.  ST_EXISTS when a
 * stream of that name (in any case) is there. */
int ntfs_stream_create(ntfs_fs_t *fs, uint64_t mref,
		       const uint16_t *name, uint32_t nlen)
{
	ntfs_eino_t *e = NULL;
	ntfs_standard_information_t *si;
	int r;

	ntfs_assert_meta_excl(fs);
	if (!ntfs_stream_name_ok(name, nlen))
		return ST_INVALID;
	if (!ntfs_stream_owner_ok(fs, mref))
		return ST_PERM;
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		return r;
	if (ntfs_eino_find_stream(e, name, nlen, 1, NULL) >= 0) {
		ntfs_eino_free(e);
		return ST_EXISTS;
	}
	r = ntfs_eino_set_res(e, NTFS_AT_DATA, name, nlen, 0, "", 0);
	if (r == ST_OK) {
		si = ntfs_eino_si(e);
		if (si)
			si->last_mft_change_time = ntfs_now();
		r = ntfs_eino_commit(e);
	}
	ntfs_eino_free(e);
	return r;
}

/* Remove the named stream (name, nlen) of `mref' (case-insensitively when
 * `ic').  ST_BUSY while it is open, ST_NOT_FOUND when there is none. */
int ntfs_stream_remove(ntfs_fs_t *fs, uint64_t mref,
		       const uint16_t *name, uint32_t nlen, int ic)
{
	ntfs_eino_t *e = NULL;
	ntfs_stream_t *st;
	ntfs_standard_information_t *si;
	uint16_t *exact = NULL;
	uint64_t fl;
	int r, busy = 0;

	ntfs_assert_meta_excl(fs);
	if (!nlen || nlen > NTFS_MAX_NAME_LEN)
		return ST_NOT_FOUND;
	if (!ntfs_stream_owner_ok(fs, mref))
		return ST_PERM;
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	exact = (uint16_t *)kalloc(nlen * 2U);
	if (!exact)
		return ST_NOMEM;
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		goto out;
	if (ntfs_eino_find_stream(e, name, nlen, ic, exact) < 0) {
		r = ST_NOT_FOUND;
		goto out;
	}
	/* Open?  Our own lookup holds one reference. */
	spin_lock_irqsave(&fs->hdl_lock, &fl);
	st = ntfs_stream_find_locked(fs, NTFS_MREF_NO(mref), e->seq,
				     NTFS_AT_DATA, exact, nlen, 0);
	if (st && st->refs > 1)
		busy = 1;
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	if (busy) {
		ntfs_stream_put(fs, st);
		r = ST_BUSY;
		goto out;
	}
	if (st) {
		ntfs_stream_kill(fs, st);
		ntfs_stream_put(fs, st);
	}
	r = ntfs_eino_drop(e, NTFS_AT_DATA, exact, nlen, 1);
	if (r == ST_OK) {
		si = ntfs_eino_si(e);
		if (si)
			si->last_mft_change_time = ntfs_now();
		r = ntfs_eino_commit(e);
	}
out:
	ntfs_eino_free(e);
	kfree(exact);
	return r;
}

/* ---- creation ---- */

/* The current task's owner and group for new files. */
void ntfs_creds(uint32_t *uid, uint32_t *gid)
{
	task_t *cur = sched_current();

	*uid = cur ? (uint32_t)cur->cred.fsuid : 0;
	*gid = cur ? (uint32_t)cur->cred.fsgid : 0;
}

/* Reparse data of a symbolic link. */
static uint8_t *ntfs_symlink_data(ntfs_fs_t *fs, const char *t, uint32_t *len,
				  uint32_t *tag)
{
	unsigned long tl = ntfs_strlen(t);
	uint8_t *d;

	if (!tl || tl > 4095)
		return NULL;
	if (fs->opt.symlink_native) {
		/* Substitute and print name: the target with '\' separators;
		 * relative targets flagged relative. */
		uint16_t *u = (uint16_t *)kalloc(2 * (tl + 1));
		long n;
		uint32_t dl;

		if (!u)
			return NULL;
		n = ntfs_utf8_to_utf16(t, tl, u, (uint32_t)tl);
		if (n <= 0) {
			kfree(u);
			return NULL;
		}
		for (long i = 0; i < n; i++)
			if (ntfs_le16(&u[i]) == '/')
				ntfs_put_le16(&u[i], '\\');
		dl = 12 + 4U * (uint32_t)n;
		if (NTFS_REPARSE_HDR + dl > NTFS_REPARSE_MAX) {
			kfree(u);
			return NULL;
		}
		d = (uint8_t *)ntfs_zalloc(NTFS_REPARSE_HDR + dl);
		if (!d) {
			kfree(u);
			return NULL;
		}
		ntfs_put_le32(d, NTFS_IO_REPARSE_TAG_SYMLINK);
		ntfs_put_le16(d + 4, (uint16_t)dl);
		ntfs_put_le16(d + 8, 0); /* substitute name offset */
		ntfs_put_le16(d + 10, (uint16_t)(2 * n));
		ntfs_put_le16(d + 12, (uint16_t)(2 * n)); /* print name */
		ntfs_put_le16(d + 14, (uint16_t)(2 * n));
		ntfs_put_le32(d + 16, t[0] == '/' ? 0 : NTFS_SYMLINK_FLAG_RELATIVE);
		mm_memcpy(d + 20, u, 2 * (unsigned long)n);
		mm_memcpy(d + 20 + 2 * n, u, 2 * (unsigned long)n);
		kfree(u);
		*len = NTFS_REPARSE_HDR + dl;
		*tag = NTFS_IO_REPARSE_TAG_SYMLINK;
		return d;
	}
	/* WSL: version 2, the target as given. */
	if (NTFS_REPARSE_HDR + 4 + tl > NTFS_REPARSE_MAX)
		return NULL;
	d = (uint8_t *)ntfs_zalloc(NTFS_REPARSE_HDR + 4 + tl);
	if (!d)
		return NULL;
	ntfs_put_le32(d, NTFS_IO_REPARSE_TAG_LX_SYMLINK);
	ntfs_put_le16(d + 4, (uint16_t)(4 + tl));
	ntfs_put_le32(d + 8, 2);
	mm_memcpy(d + 12, t, tl);
	*len = NTFS_REPARSE_HDR + 4 + (uint32_t)tl;
	*tag = NTFS_IO_REPARSE_TAG_LX_SYMLINK;
	return d;
}

/* Create `name' (UTF-16) in directory `dir'.  *out: the new file's
 * reference. */
int ntfs_create_in(ntfs_fs_t *fs, uint64_t dir, const uint16_t *u,
		   uint32_t ulen, uint8_t ntype, const ntfs_mkargs_t *mk,
		   uint64_t *out)
{
	ntfs_eino_t *e = NULL;
	uint64_t no = 0, mref;
	uint16_t seq = 0;
	uint8_t *fn = NULL, *sd = NULL, *rp = NULL;
	uint32_t fl = 0, sdl = 0, rpl = 0, tag = 0, uid, gid, fattr;
	uint32_t type = mk->mode & S_IFMT;
	int64_t now = ntfs_now();
	int r, reparse_added = 0, comp_inherit = 0;
	ntfs_standard_information_t si;

	ntfs_creds(&uid, &gid);
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	if (ntfs_protected(fs, dir))
		return ST_PERM; /* nothing is created among system files */
	if (mk->kind == NTFS_MK_LINK) {
		rp = ntfs_symlink_data(fs, mk->target, &rpl, &tag);
		if (!rp)
			return ST_NAMETOOLONG;
	} else if (mk->kind == NTFS_MK_NODE) {
		switch (type) {
		case S_IFIFO:
			tag = NTFS_IO_REPARSE_TAG_LX_FIFO;
			break;
		case S_IFCHR:
			tag = NTFS_IO_REPARSE_TAG_LX_CHR;
			break;
		case S_IFBLK:
			tag = NTFS_IO_REPARSE_TAG_LX_BLK;
			break;
		case S_IFSOCK:
			tag = NTFS_IO_REPARSE_TAG_AF_UNIX;
			break;
		default:
			return ST_UNSUPPORTED;
		}
		rpl = NTFS_REPARSE_HDR;
		rp = (uint8_t *)ntfs_zalloc(rpl);
		if (!rp)
			return ST_NOMEM;
		ntfs_put_le32(rp, tag);
	}
	r = ntfs_mft_alloc(fs, &no, &seq);
	if (r != ST_OK)
		goto out;
	mref = NTFS_MAKE_MREF(no, seq);
	e = ntfs_eino_new(fs, no, seq,
			  mk->kind == NTFS_MK_DIR ? NTFS_MFT_REC_IS_DIRECTORY : 0);
	if (!e) {
		r = ST_NOMEM;
		goto fail_rec;
	}
	e->link_count = 1;
	/* Directories carry "has a $I30 index" in their attributes, as
	 * other implementations write it (standard information and name). */
	fattr = mk->kind == NTFS_MK_DIR ?
			NTFS_FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT :
			NTFS_FILE_ATTR_ARCHIVE;
	if (rp)
		fattr |= NTFS_FILE_ATTR_REPARSE_POINT;
	if (mk->kind == NTFS_MK_NODE)
		fattr |= NTFS_FILE_ATTR_RECALL_ON_OPEN;
	if (fs->opt.hide_dot && ulen && ntfs_le16(&u[0]) == '.')
		fattr |= NTFS_FILE_ATTR_HIDDEN;
	/* A directory flagged compressed passes the flag on: new files get
	 * an (empty) compressed $DATA, new directories the flag. */
	if ((mk->kind == NTFS_MK_FILE || mk->kind == NTFS_MK_DIR) &&
	    fs->cluster_size <= NTFS_COMPRESS_MAX_CLUSTER) {
		ntfs_inode_t *pino = (ntfs_inode_t *)kalloc(sizeof(*pino));

		if (!pino) {
			r = ST_NOMEM;
			goto fail_rec;
		}
		if (ntfs_inode_load(fs, dir, pino) == ST_OK &&
		    (pino->fattr & NTFS_FILE_ATTR_COMPRESSED))
			comp_inherit = 1;
		kfree(pino);
		if (comp_inherit)
			fattr |= NTFS_FILE_ATTR_COMPRESSED;
	}
	mm_memset(&si, 0, sizeof(si));
	si.creation_time = si.last_data_change_time = now;
	si.last_mft_change_time = si.last_access_time = now;
	si.file_attributes = fattr;
	r = ntfs_eino_set_res(e, NTFS_AT_STANDARD_INFORMATION, NULL, 0, 0, &si,
			      NTFS_SI_SIZE_V12);
	if (r != ST_OK)
		goto fail_rec;
	fn = ntfs_fn_build(NTFS_MAKE_MREF(NTFS_MREF_NO(dir), NTFS_MREF_SEQ(dir)),
			   u, ulen, ntype, now, now, now, now, 0, 0,
			   (fattr & ~NTFS_FILE_ATTR_RECALL_ON_OPEN) |
				   (mk->kind == NTFS_MK_DIR ?
					    NTFS_FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT :
					    0),
			   rp ? tag : 0, &fl);
	if (!fn) {
		r = ST_NOMEM;
		goto fail_rec;
	}
	r = ntfs_eino_set_res(e, NTFS_AT_FILE_NAME, NULL, 0, 0, fn, fl);
	if (r != ST_OK)
		goto fail_rec;
	/* The default descriptor: on NTFS 3.x one shared $Secure entry
	 * (added once, then found by its id), else the file's own
	 * $SECURITY_DESCRIPTOR attribute. */
	if (fs->sec_default_id) {
		r = ntfs_eino_set_secid(e, fs->sec_default_id);
	} else {
		uint32_t sid = 0;

		sd = ntfs_sd_default(&sdl);
		if (!sd) {
			r = ST_NOMEM;
			goto fail_rec;
		}
		r = ntfs_secure_id(fs, sd, sdl, &sid);
		if (r == ST_OK) {
			fs->sec_default_id = sid;
			r = ntfs_eino_set_secid(e, sid);
		} else if (r == ST_UNSUPPORTED) {
			r = ntfs_eino_set_res(e, NTFS_AT_SECURITY_DESCRIPTOR,
					      NULL, 0, 0, sd, sdl);
		}
	}
	if (r != ST_OK)
		goto fail_rec;
	if (mk->kind == NTFS_MK_DIR) {
		uint8_t root[NTFS_INDEX_ROOT_HDR + NTFS_INDEX_HDR + NTFS_IE_HDR];
		ntfs_index_root_t *ir = (ntfs_index_root_t *)root;
		ntfs_index_entry_t *end;

		mm_memset(root, 0, sizeof(root));
		ir->type = NTFS_AT_FILE_NAME;
		ir->collation_rule = NTFS_COLLATION_FILE_NAME;
		ir->index_block_size = fs->ib_size;
		ir->clusters_per_index_block = fs->ir_cpib;
		ir->index.entries_offset = NTFS_INDEX_HDR;
		ir->index.index_length = NTFS_INDEX_HDR + NTFS_IE_HDR;
		ir->index.allocated_size = NTFS_INDEX_HDR + NTFS_IE_HDR;
		end = (ntfs_index_entry_t *)(root + NTFS_INDEX_ROOT_HDR +
					     NTFS_INDEX_HDR);
		end->length = NTFS_IE_HDR;
		end->flags = NTFS_INDEX_ENTRY_END;
		r = ntfs_eino_set_res(e, NTFS_AT_INDEX_ROOT, ntfs_i30_name, 4,
				      0, root, sizeof(root));
	} else {
		r = ntfs_eino_set_res(e, NTFS_AT_DATA, NULL, 0, 0, NULL, 0);
		if (r == ST_OK && comp_inherit && mk->kind == NTFS_MK_FILE)
			r = ntfs_set_data_flags(fs, e, 1, 0);
	}
	if (r != ST_OK)
		goto fail_rec;
	if (rp) {
		/* Long targets make the reparse data non-resident. */
		r = ntfs_eino_set_value(e, NTFS_AT_REPARSE_POINT, ntfs_unnamed,
					0, rp, rpl, fs->rec_size / 4);
		if (r != ST_OK)
			goto fail_rec;
	}
	/* WSL owner / mode when the mount would show something else; POSIX
	 * ACLs inherited from the directory's default ACL (`acl')
	 * before the record is first written. */
	{
		uint32_t perm = mk->mode & 07777;
		uint32_t dflt = (mk->kind == NTFS_MK_DIR ? 0777 & ~fs->opt.dmask :
							   0777 & ~fs->opt.fmask);
		int acl_changed = 0, need_mode;

		if (mk->kind != NTFS_MK_LINK) {
			r = ntfs_acl_inherit(fs, e, dir, mk->kind == NTFS_MK_DIR,
					     &perm, &acl_changed);
			if (r != ST_OK)
				goto fail_rec;
			perm &= 07777;
		}
		need_mode = mk->kind != NTFS_MK_LINK &&
			    (perm != dflt || mk->kind == NTFS_MK_NODE ||
			     acl_changed);
		int need_own = (uid != fs->opt.uid && !fs->opt.uid_set) ||
			       (gid != fs->opt.gid && !fs->opt.gid_set);

		if (need_mode || need_own || mk->kind == NTFS_MK_NODE) {
			uint32_t lxmode = (mk->kind == NTFS_MK_DIR ? S_IFDIR :
					   mk->kind == NTFS_MK_NODE ? type :
								      S_IFREG) |
					  perm;

			r = ntfs_wsl_set(e, need_own, uid, need_own, gid,
					 need_mode || need_own, lxmode,
					 type == S_IFCHR || type == S_IFBLK,
					 (uint32_t)kdev_major(mk->dev),
					 (uint32_t)kdev_minor(mk->dev));
			if (r != ST_OK)
				goto fail_rec;
		}
	}
	/* The record, in use (allocate before reference). */
	r = ntfs_eino_commit(e);
	if (r != ST_OK)
		goto fail_rec;
	if (tag) {
		r = ntfs_reparse_index(fs, mref, tag, 1);
		if (r != ST_OK)
			goto fail_written;
		reparse_added = 1;
	}
	/* The name (reference). */
	r = ntfs_dir_add_entry(fs, dir, mref, fn, fl, 1);
	if (r != ST_OK)
		goto fail_written;
	*out = mref;
	goto out;
fail_written:
	/* The reparse index entry goes too; the record was written:
	 * free it like a deleted file. */
	ntfs_eino_free(e);
	e = NULL;
	if (reparse_added && ntfs_reparse_index(fs, mref, tag, 0) != ST_OK) {
		/* its $Reparse entry stays: so does the record (an orphan) */
		ntfs_leaked(fs);
		goto out;
	}
	ntfs_file_delete(fs, mref);
	goto out;
fail_rec:
	/* Only the record this call allocated (never record 0). */
	if (no >= NTFS_FIRST_FREE_REC)
		ntfs_mft_free(fs, no);
out:
	ntfs_eino_free(e);
	ntfs_free(fn);
	ntfs_free(sd);
	ntfs_free(rp);
	return r;
}

/* Resolve `path' for creating: the directory and the new name.  ST_OK
 * with *dir / u / *ulen when the name does not exist; ST_EXISTS (and
 * res->mref) when it does. */
static int ntfs_resolve_new(ntfs_fs_t *fs, const char *path, int follow,
			    ntfs_res_t *res, uint64_t *dir, uint16_t *u,
			    uint32_t *ulen, uint8_t *ntype)
{
	int r = ntfs_resolve(fs, path, follow, res);

	if (r == ST_OK)
		return res->is_stream ? ST_UNSUPPORTED : ST_EXISTS;
	if (r != ST_NOT_FOUND || !res->last_missing)
		return r;
	*dir = res->missing_dir;
	return ntfs_new_name(fs, res->lname, res->llen, u, ulen, ntype);
}

/* ---- removal of a name ---- */

/* The $FILE_NAME of file `e' in directory `dir' named like `u' (exactly,
 * or ignoring case with `nocase'): its index in the set, or -1. */
static long ntfs_eino_fn(ntfs_fs_t *fs, ntfs_eino_t *e, uint64_t dir,
			 const uint16_t *u, uint32_t ulen, int ic)
{
	long hit = -1;

	for (long i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0, 0); i >= 0;
	     i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0, (uint32_t)i + 1)) {
		const ntfs_file_name_t *fn = (const ntfs_file_name_t *)
			ntfs_attr_value(ntfs_ea_hdr(&e->a[i]));

		if (NTFS_MREF_NO(fn->parent_directory) != NTFS_MREF_NO(dir) ||
		    fn->file_name_length != ulen)
			continue;
		if (kmemcmp(ntfs_fn_name(fn), u, 2U * ulen) == 0)
			return i;
		if (ic && hit < 0 && ntfs_upcase_eq(fs, ntfs_fn_name(fn), u, ulen))
			hit = i;
	}
	return hit;
}

/* Remove name `u' of file `mref' from directory `dir': the index entry
 * (and that of a DOS alias), then the $FILE_NAME(s) from the record; the
 * file is deleted when no name is left and it is not open.  `want_dir':
 * 1 rmdir (must be an empty directory), 0 unlink (must not be one), -1
 * either (rename over). */
static int ntfs_remove_name(ntfs_fs_t *fs, uint64_t dir, uint64_t mref,
			    const uint16_t *u, uint32_t ulen, int want_dir)
{
	ntfs_eino_t *e = NULL;
	uint16_t *dos = NULL, *exact = NULL;
	uint32_t dos_len = 0;
	long fi;
	int r, is_dir, links, gone = 0;
	const ntfs_file_name_t *fn;

	if (NTFS_MREF_NO(mref) < NTFS_FILE_FIRST_USER ||
	    NTFS_MREF_NO(dir) == NTFS_FILE_EXTEND ||
	    ntfs_protected(fs, dir) || ntfs_protected(fs, mref))
		return ST_PERM; /* system files stay */
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		return r;
	is_dir = !!(e->rflags & NTFS_MFT_REC_IS_DIRECTORY);
	{
		/* A directory symbolic link / junction is a link here. */
		uint8_t *rpv = NULL;
		uint32_t rpl = 0;

		if (is_dir &&
		    ntfs_eino_value(e, NTFS_AT_REPARSE_POINT, ntfs_unnamed, 0,
				    NTFS_REPARSE_MAX, &rpv, &rpl) == ST_OK &&
		    rpl >= 4 && ntfs_tag_is_link(ntfs_le32(rpv)))
			is_dir = 0;
		ntfs_free(rpv);
	}
	if (want_dir == 1 && !is_dir) {
		r = ST_NOTDIR;
		goto out;
	}
	if (want_dir == 0 && is_dir) {
		r = ST_ISDIR;
		goto out;
	}
	if (is_dir && want_dir == 1) {
		ntfs_ixs_t *s;
		int empty = 0;

		r = ntfs_ixs_open(fs, mref, ntfs_i30_name, 4, &s);
		if (r != ST_OK)
			goto out;
		r = ntfs_ixs_empty(s, &empty);
		ntfs_ixs_free(s);
		if (r != ST_OK)
			goto out;
		if (!empty) {
			r = ST_NOTEMPTY;
			goto out;
		}
		if (NTFS_MREF_NO(mref) == NTFS_MREF_NO(fs->cwd))
			fs->cwd = NTFS_MAKE_MREF(NTFS_FILE_ROOT, 0);
	}
	fi = ntfs_eino_fn(fs, e, dir, u, ulen, fs->opt.nocase);
	if (fi < 0) {
		r = ntfs_fs_error(fs, NTFS_MREF_NO(mref),
				  "directory entry without a $FILE_NAME");
		goto out;
	}
	fn = (const ntfs_file_name_t *)ntfs_attr_value(ntfs_ea_hdr(&e->a[fi]));
	exact = (uint16_t *)kalloc(2U * NTFS_MAX_NAME_LEN);
	if (!exact) {
		r = ST_NOMEM;
		goto out;
	}
	mm_memcpy(exact, ntfs_fn_name(fn), 2U * fn->file_name_length);
	ulen = fn->file_name_length;
	/* Named through its DOS alias: the Win32 name it belongs to goes
	 * with it (the pair is one name). */
	if (fn->file_name_type == NTFS_FILE_NAME_DOS) {
		for (long i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0, 0);
		     i >= 0; i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0,
						(uint32_t)i + 1)) {
			const ntfs_file_name_t *w = (const ntfs_file_name_t *)
				ntfs_attr_value(ntfs_ea_hdr(&e->a[i]));

			if (w->file_name_type == NTFS_FILE_NAME_WIN32 &&
			    NTFS_MREF_NO(w->parent_directory) ==
				    NTFS_MREF_NO(dir)) {
				dos = (uint16_t *)kalloc(2U * ulen);
				if (!dos) {
					r = ST_NOMEM;
					goto out;
				}
				mm_memcpy(dos, exact, 2U * ulen);
				dos_len = ulen;
				mm_memcpy(exact, ntfs_fn_name(w),
					  2U * w->file_name_length);
				ulen = w->file_name_length;
				break;
			}
		}
	}
	/* A DOS alias of this (Win32) name. */
	if (fn->file_name_type == NTFS_FILE_NAME_WIN32) {
		for (long i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0, 0);
		     i >= 0; i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0,
						(uint32_t)i + 1)) {
			const ntfs_file_name_t *d = (const ntfs_file_name_t *)
				ntfs_attr_value(ntfs_ea_hdr(&e->a[i]));

			if (d->file_name_type == NTFS_FILE_NAME_DOS &&
			    NTFS_MREF_NO(d->parent_directory) ==
				    NTFS_MREF_NO(dir)) {
				dos = (uint16_t *)kalloc(2U * d->file_name_length);
				if (!dos) {
					r = ST_NOMEM;
					goto out;
				}
				mm_memcpy(dos, ntfs_fn_name(d),
					  2U * d->file_name_length);
				dos_len = d->file_name_length;
				break;
			}
		}
	}
	/* 1. The directory entries (dereference). */
	r = ntfs_dir_del_entry(fs, dir, mref, exact, ulen, 1);
	if (r == ST_NOT_FOUND)
		r = ntfs_fs_error(fs, NTFS_MREF_NO(dir), "name not in its index");
	if (r != ST_OK)
		goto out;
	gone = 1;
	if (dos) {
		r = ntfs_dir_del_entry(fs, dir, mref, dos, dos_len, 0);
		if (r == ST_NOT_FOUND)
			r = ST_OK;
		if (r != ST_OK)
			goto out;
	}
	ntfs_dc_forget16(fs, dir, exact, ulen);
	if (dos)
		ntfs_dc_forget16(fs, dir, dos, dos_len);
	/* 2. The names in the record. */
	ntfs_eino_remove(e, (uint32_t)ntfs_eino_fn(fs, e, dir, exact, ulen, 0));
	if (dos) {
		long di = ntfs_eino_fn(fs, e, dir, dos, dos_len, 0);

		if (di >= 0)
			ntfs_eino_remove(e, (uint32_t)di);
	}
	links = 0;
	for (long i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0, 0); i >= 0;
	     i = ntfs_eino_find(e, NTFS_AT_FILE_NAME, NULL, 0, (uint32_t)i + 1))
		links++;
	e->link_count = (uint16_t)links;
	/* The last name: an open file is marked here, in the same step as
	 * the check (a close racing with this unlink either comes first and
	 * the file goes now, or it sees the mark and deletes the file). */
	if (links == 0 && !ntfs_ofile_busy(fs, NTFS_MREF_NO(mref), 1)) {
		ntfs_eino_free(e);
		e = NULL;
		r = ntfs_file_delete(fs, mref);
		goto out;
	}
	{
		ntfs_standard_information_t *si = ntfs_eino_si(e);

		if (si)
			si->last_mft_change_time = ntfs_now();
	}
	r = ntfs_eino_commit(e);
	if (r == ST_OK && links > 0)
		ntfs_update_dups(fs, mref);
out:
	/* The name left its directory on the device: the removal stands.
	 * What could not follow (the name in the record, the record or its
	 * clusters freed) is left behind for a check, and a lack of memory
	 * is not reported as a failed removal (callers would act as if the
	 * name were still there). */
	if (gone && r != ST_OK) {
		ntfs_leaked(fs);
		if (r == ST_NOMEM)
			r = ST_OK;
	}
	ntfs_eino_free(e);
	ntfs_free(dos);
	ntfs_free(exact);
	return r;
}

/* Add name `u' in directory `dir' to file `mref' (link, rename). */
static int ntfs_add_name(ntfs_fs_t *fs, uint64_t dir, uint64_t mref,
			 const uint16_t *u, uint32_t ulen, uint8_t ntype)
{
	ntfs_eino_t *e = NULL;
	ntfs_standard_information_t *si;
	uint8_t *fn = NULL;
	uint32_t fl = 0, extra = 0;
	int64_t now = ntfs_now();
	uint64_t alloc = 0, data = 0;
	int r, is_dir;
	long i;

	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		return r;
	if (e->link_count >= 0xFFFE) {
		r = ST_NOSPC;
		goto out;
	}
	is_dir = !!(e->rflags & NTFS_MFT_REC_IS_DIRECTORY);
	si = ntfs_eino_si(e);
	if (!si) {
		r = ST_IO;
		goto out;
	}
	si->last_mft_change_time = now; /* only the change time */
	i = ntfs_eino_find(e, NTFS_AT_DATA, ntfs_unnamed, 0, 0);
	if (i >= 0) {
		ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);

		data = a->non_resident ? (uint64_t)a->data.non_resident.data_size :
					 a->data.resident.value_length;
		alloc = a->non_resident ?
				(uint64_t)a->data.non_resident.allocated_size :
				(data + 7) & ~7ULL;
	}
	/* The packed EA size, or the reparse tag. */
	i = ntfs_eino_find(e, NTFS_AT_REPARSE_POINT, NULL, 0, 0);
	if (i >= 0) {
		ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);

		if (!a->non_resident && a->data.resident.value_length >= 4)
			extra = ntfs_le32(ntfs_attr_value(a));
	} else {
		i = ntfs_eino_find(e, NTFS_AT_EA_INFORMATION, ntfs_unnamed, 0, 0);
		if (i >= 0) {
			ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);

			if (!a->non_resident &&
			    a->data.resident.value_length >= 8)
				extra = ntfs_le16(ntfs_attr_value(a));
		}
	}
	fn = ntfs_fn_build(dir, u, ulen, ntype, si->creation_time,
			   si->last_data_change_time, now,
			   si->last_access_time, is_dir ? 0 : alloc,
			   is_dir ? 0 : data,
			   (si->file_attributes & ~NTFS_FILE_ATTR_DIRECTORY) |
				   (is_dir ? NTFS_FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT :
					     0),
			   extra, &fl);
	if (!fn) {
		r = ST_NOMEM;
		goto out;
	}
	{
		uint8_t *b = ntfs_ab_res(NTFS_AT_FILE_NAME, NULL, 0, 0, fn, fl, 1);

		if (!b) {
			r = ST_NOMEM;
			goto out;
		}
		r = ntfs_eino_put(e, b, ~0ULL, 0);
		if (r != ST_OK)
			goto out;
	}
	e->link_count++;
	r = ntfs_eino_commit(e);
	if (r != ST_OK)
		goto out;
	ntfs_eino_free(e);
	e = NULL;
	r = ntfs_dir_add_entry(fs, dir, mref, fn, fl, 1);
	if (r != ST_OK) {
		/* Roll the name back out of the record. */
		ntfs_eino_t *e2;

		if (ntfs_eino_load(fs, mref, &e2) == ST_OK) {
			long fi = ntfs_eino_fn(fs, e2, dir, u, ulen, 0);

			if (fi >= 0) {
				ntfs_eino_remove(e2, (uint32_t)fi);
				if (e2->link_count)
					e2->link_count--;
				if (ntfs_eino_commit(e2) != ST_OK)
					ntfs_fs_error(fs, NTFS_MREF_NO(mref),
						      "name rollback failed");
			}
			ntfs_eino_free(e2);
		} else {
			ntfs_leaked(fs); /* the name stays in the record */
		}
	}
out:
	ntfs_eino_free(e);
	ntfs_free(fn);
	return r;
}

/* Is `dir' `anc' or below it? */
static int ntfs_is_below(ntfs_fs_t *fs, uint64_t dir, uint64_t anc, int *below)
{
	*below = 0;
	for (int depth = 0; depth < 4096; depth++) {
		uint64_t p;
		int r;

		if (NTFS_MREF_NO(dir) == NTFS_MREF_NO(anc)) {
			*below = 1;
			return ST_OK;
		}
		if (NTFS_MREF_NO(dir) == NTFS_FILE_ROOT)
			return ST_OK;
		r = ntfs_parent_of(fs, dir, &p);
		if (r != ST_OK)
			return r;
		dir = p;
	}
	return ST_LOOP;
}

/* ---- attribute changes: mode, owner, times ---- */

/* chmod / chown / utimens of file `mref' (metadata lock exclusive).
 * `what': 1 mode, 2 owner, 4 times.  Nothing is written when nothing
 * changes. */
static int ntfs_setattr(ntfs_fs_t *fs, uint64_t mref, int what, uint32_t mode,
			int uid, int gid, const struct vfs_utimes *t)
{
	ntfs_inode_t *ino = NULL;
	ntfs_eino_t *e = NULL;
	ntfs_standard_information_t *si;
	int64_t now = ntfs_now();
	int r, change = 0;

	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	if (!ino)
		return ST_NOMEM;
	r = ntfs_inode_load(fs, mref, ino);
	if (r != ST_OK)
		goto out;
	r = ntfs_eino_load(fs, mref, &e);
	if (r != ST_OK)
		goto out;
	si = ntfs_eino_si(e);
	if (!si) {
		r = ST_IO;
		goto out;
	}
	if (what & 1) {
		uint32_t cur = ntfs_inode_mode(fs, ino);
		uint32_t nm = (cur & S_IFMT) | (mode & 07777);
		uint32_t fa = si->file_attributes;

		if ((cur & S_IFMT) == S_IFLNK) {
			r = ST_OK; /* links have no mode of their own */
			goto out;
		}
		if (!ino->is_dir) {
			if (mode & 0222)
				fa &= ~NTFS_FILE_ATTR_READONLY;
			else
				fa |= NTFS_FILE_ATTR_READONLY;
		}
		/* The same mode as shown already is no change (no write). */
		if (nm != cur || fa != si->file_attributes) {
			si->file_attributes = fa;
			r = ntfs_wsl_set(e, 0, 0, 0, 0, 1, nm, 0, 0, 0);
			if (r == ST_OK)
				r = ntfs_acl_chmod(fs, e, mref, nm);
			if (r != ST_OK)
				goto out;
			change = 1;
		}
	}
	if (what & 2) {
		uint32_t cu = (ino->has_lxuid && !fs->opt.uid_set) ? ino->lx_uid :
								      fs->opt.uid;
		uint32_t cg = (ino->has_lxgid && !fs->opt.gid_set) ? ino->lx_gid :
								      fs->opt.gid;
		int su = uid >= 0 && (uint32_t)uid != cu;
		int sg = gid >= 0 && (uint32_t)gid != cg;

		if (su || sg) {
			r = ntfs_wsl_set(e, su, (uint32_t)uid, sg,
					 (uint32_t)gid, 0, 0, 0, 0, 0);
			if (r != ST_OK)
				goto out;
			change = 1;
		}
	}
	if (what & 4) {
		if (t->atime_nsec != VFS_UTIME_OMIT) {
			si->last_access_time =
				t->atime_nsec == VFS_UTIME_NOW ?
					now :
					ntfs_unix_to_time(t->atime_sec,
							  t->atime_nsec);
			change = 1;
		}
		if (t->mtime_nsec != VFS_UTIME_OMIT) {
			si->last_data_change_time =
				t->mtime_nsec == VFS_UTIME_NOW ?
					now :
					ntfs_unix_to_time(t->mtime_sec,
							  t->mtime_nsec);
			change = 1;
		}
	}
	if (!change) {
		r = ST_OK; /* no write, no arming (nothing changed) */
		goto out;
	}
	si->last_mft_change_time = now;
	r = ntfs_eino_commit(e);
	if (r == ST_OK) {
		ntfs_eino_free(e);
		e = NULL;
		ntfs_update_dups(fs, mref);
	}
out:
	ntfs_eino_free(e);
	ntfs_free(ino);
	return r;
}

/* ---- namespace ops ---- */

static int ntfs_mk_common(vfs_mount_t *m, const char *path,
			  const ntfs_mkargs_t *mk, int follow)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *res;
	uint16_t *u;
	uint32_t ulen = 0;
	uint8_t ntype = 0;
	uint64_t dir = 0, mref = 0;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	u = (uint16_t *)kalloc(2U * NTFS_MAX_NAME_LEN);
	if (!res || !u) {
		ntfs_free(res);
		ntfs_free(u);
		return ST_NOMEM;
	}
	ntfs_meta_wlock(fs);
	r = ntfs_resolve_new(fs, path, follow, res, &dir, u, &ulen, &ntype);
	if (r == ST_OK) {
		r = ntfs_write_begin(fs);
		if (r == ST_OK)
			r = ntfs_create_in(fs, dir, u, ulen, ntype, mk, &mref);
		if (r == ST_OK)
			ntfs_dc_forget(fs, dir, res->lname, res->llen);
	}
	ntfs_meta_wunlock(fs);
	kfree(res);
	kfree(u);
	return r;
}

int ntfs_mkdir(vfs_mount_t *m, const char *path, unsigned int mode)
{
	ntfs_mkargs_t mk;

	mk.kind = NTFS_MK_DIR;
	mk.mode = S_IFDIR | (mode & 07777);
	mk.target = NULL;
	mk.dev = 0;
	return ntfs_mk_common(m, path, &mk, 0);
}

int ntfs_symlink(vfs_mount_t *m, const char *target,
		 const char *linkpath)
{
	ntfs_mkargs_t mk;

	if (!target || !target[0])
		return ST_NOT_FOUND;
	mk.kind = NTFS_MK_LINK;
	mk.mode = S_IFLNK | 0777;
	mk.target = target;
	mk.dev = 0;
	return ntfs_mk_common(m, linkpath, &mk, 0);
}

int ntfs_mknod(vfs_mount_t *m, const char *path, unsigned int mode,
	       uint64_t dev)
{
	ntfs_mkargs_t mk;
	uint32_t t = mode & S_IFMT;

	if (t != S_IFIFO && t != S_IFSOCK && t != S_IFCHR && t != S_IFBLK)
		return ST_UNSUPPORTED;
	mk.kind = NTFS_MK_NODE;
	mk.mode = mode;
	mk.target = NULL;
	mk.dev = dev;
	return ntfs_mk_common(m, path, &mk, 0);
}

/* unlink / rmdir */
static int ntfs_rm_common(vfs_mount_t *m, const char *path, int want_dir)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *res;
	uint16_t *u;
	long n;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	u = (uint16_t *)kalloc(2U * NTFS_MAX_NAME_LEN);
	if (!res || !u) {
		ntfs_free(res);
		ntfs_free(u);
		return ST_NOMEM;
	}
	ntfs_meta_wlock(fs);
	r = ntfs_resolve(fs, path, 0, res);
	if (r == ST_OK && res->is_stream) {
		/* "file:stream": remove the named stream. */
		r = want_dir ? ST_NOTDIR :
			       ntfs_stream_remove(fs, res->mref, res->sname,
						  res->snlen,
						  fs->opt.streams ==
							  NTFS_STREAMS_WINDOWS);
		ntfs_meta_wunlock(fs);
		kfree(res);
		kfree(u);
		return r;
	}
	if (r == ST_OK && (!res->llen || NTFS_MREF_NO(res->mref) ==
						 NTFS_FILE_ROOT))
		r = want_dir ? ST_BUSY : ST_ISDIR;
	if (r == ST_OK && (res->llen == 1 && res->lname[0] == '.'))
		r = ST_INVALID;
	if (r == ST_OK && (res->llen == 2 && res->lname[0] == '.' &&
			   res->lname[1] == '.'))
		r = ST_NOTEMPTY;
	if (r == ST_OK) {
		n = ntfs_utf8_to_utf16(res->lname, res->llen, u,
				       NTFS_MAX_NAME_LEN);
		if (n <= 0)
			r = ST_NOT_FOUND;
		else
			r = ntfs_remove_name(fs, res->parent, res->mref, u,
					     (uint32_t)n, want_dir);
	}
	ntfs_meta_wunlock(fs);
	kfree(res);
	kfree(u);
	return r;
}

int ntfs_unlink(vfs_mount_t *m, const char *path)
{
	return ntfs_rm_common(m, path, 0);
}

int ntfs_rmdir(vfs_mount_t *m, const char *path)
{
	return ntfs_rm_common(m, path, 1);
}

int ntfs_link(vfs_mount_t *m, const char *oldpath, const char *newpath,
	      int follow)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *res;
	uint16_t *u;
	uint32_t ulen = 0;
	uint8_t ntype = 0;
	uint64_t dir = 0, src;
	int r, is_link = 0, is_dir = 0;

	if (!fs || !oldpath || !newpath)
		return ST_INVALID;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	u = (uint16_t *)kalloc(2U * NTFS_MAX_NAME_LEN);
	if (!res || !u) {
		ntfs_free(res);
		ntfs_free(u);
		return ST_NOMEM;
	}
	ntfs_meta_wlock(fs);
	r = ntfs_resolve(fs, oldpath, follow, res);
	if (r == ST_OK && res->is_stream)
		r = ST_PERM;
	src = res->mref;
	if (r == ST_OK)
		r = ntfs_inode_quick_link(fs, src, &is_link, &is_dir);
	/* No second name for any directory record -- a junction or a
	 * directory symbolic link included (lstat shows those as links, but
	 * the record is a directory, which has exactly one name). */
	if (r == ST_OK && !is_dir) {
		ntfs_actx_t *c;

		r = ntfs_actx_open(fs, src, &c);
		if (r == ST_OK) {
			is_dir = !!(((const ntfs_mft_record_t *)c->base)->flags &
				    NTFS_MFT_REC_IS_DIRECTORY);
			ntfs_actx_close(c);
		}
	}
	if (r == ST_OK && is_dir)
		r = ST_PERM;
	if (r == ST_OK && (NTFS_MREF_NO(src) < NTFS_FILE_FIRST_USER ||
			   ntfs_protected(fs, src)))
		r = ST_PERM;
	if (r == ST_OK)
		r = ntfs_resolve_new(fs, newpath, 0, res, &dir, u, &ulen,
				     &ntype);
	if (r == ST_OK && ntfs_protected(fs, dir))
		r = ST_PERM; /* no name goes into a system directory */
	if (r == ST_OK)
		r = ntfs_add_name(fs, dir, src, u, ulen, ntype);
	if (r == ST_OK) {
		ntfs_dc_forget(fs, dir, res->lname, res->llen);
		ntfs_update_dups(fs, src);
	}
	ntfs_meta_wunlock(fs);
	kfree(res);
	kfree(u);
	return r;
}

int ntfs_rename(vfs_mount_t *m, const char *oldpath,
		const char *newpath)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *a = NULL, *b = NULL;
	uint16_t *ua = NULL, *ub = NULL;
	uint32_t ulb = 0;
	uint8_t ntype = 0;
	uint64_t src, sdir, ddir = 0, dst = 0;
	int r, exists = 0, s_link, s_dir = 0, d_link, d_dir = 0;
	long ula;

	if (!fs || !oldpath || !newpath)
		return ST_INVALID;
	a = (ntfs_res_t *)kalloc(sizeof(*a));
	b = (ntfs_res_t *)kalloc(sizeof(*b));
	ua = (uint16_t *)kalloc(2U * NTFS_MAX_NAME_LEN);
	ub = (uint16_t *)kalloc(2U * NTFS_MAX_NAME_LEN);
	if (!a || !b || !ua || !ub) {
		r = ST_NOMEM;
		goto free_out;
	}
	ntfs_meta_wlock(fs);
	r = ntfs_resolve(fs, oldpath, 0, a);
	if (r == ST_OK && (a->is_stream || !a->llen))
		r = a->is_stream ? ST_UNSUPPORTED : ST_BUSY;
	if (r != ST_OK)
		goto out;
	src = a->mref;
	sdir = a->parent;
	if (NTFS_MREF_NO(src) < NTFS_FILE_FIRST_USER ||
	    NTFS_MREF_NO(sdir) == NTFS_FILE_EXTEND ||
	    ntfs_protected(fs, src)) {
		r = ST_PERM;
		goto out;
	}
	ula = ntfs_utf8_to_utf16(a->lname, a->llen, ua, NTFS_MAX_NAME_LEN);
	if (ula <= 0) {
		r = ST_NOT_FOUND;
		goto out;
	}
	r = ntfs_inode_quick_link(fs, src, &s_link, &s_dir);
	if (r != ST_OK)
		goto out;
	if (s_link)
		s_dir = 0;
	r = ntfs_resolve_new(fs, newpath, 0, b, &ddir, ub, &ulb, &ntype);
	if (r == ST_EXISTS) {
		if (b->is_stream) {
			r = ST_UNSUPPORTED;
			goto out;
		}
		exists = 1;
		dst = b->mref;
		ddir = b->parent;
		r = ntfs_new_name(fs, b->lname, b->llen, ub, &ulb, &ntype);
		if (r == ST_EXISTS)
			r = ST_BUSY; /* "." or ".." */
	}
	if (r == ST_OK && ntfs_protected(fs, ddir))
		r = ST_PERM; /* nothing is moved into a system directory */
	if (r != ST_OK)
		goto out;
	if (exists && NTFS_MREF_NO(dst) == NTFS_MREF_NO(src)) {
		/* The same file: the same name is nothing to do; another
		 * name of it (or, with nocase, the same name spelled
		 * differently) is renamed like a new one. */
		if (NTFS_MREF_NO(sdir) == NTFS_MREF_NO(ddir) &&
		    (uint32_t)ula == ulb && kmemcmp(ua, ub, 2U * ulb) == 0) {
			r = ST_OK;
			goto out;
		}
		if (!(fs->opt.nocase && NTFS_MREF_NO(sdir) == NTFS_MREF_NO(ddir) &&
		      (uint32_t)ula == ulb && ntfs_upcase_eq(fs, ua, ub, ulb))) {
			r = ST_OK; /* POSIX: two links to one file */
			goto out;
		}
		exists = 0;
	}
	if (exists) {
		if (NTFS_MREF_NO(dst) < NTFS_FILE_FIRST_USER) {
			r = ST_PERM;
			goto out;
		}
		r = ntfs_inode_quick_link(fs, dst, &d_link, &d_dir);
		if (r != ST_OK)
			goto out;
		if (d_link)
			d_dir = 0;
		if (s_dir && !d_dir) {
			r = ST_NOTDIR;
			goto out;
		}
		if (!s_dir && d_dir) {
			r = ST_ISDIR;
			goto out;
		}
	}
	if (s_dir) {
		int below = 0;

		r = ntfs_is_below(fs, ddir, src, &below);
		if (r == ST_OK && below)
			r = ST_INVALID; /* into itself */
		if (r != ST_OK)
			goto out;
	}
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		goto out;
	if (exists) {
		/* The target goes first (a directory must be empty). */
		uint16_t *ex = (uint16_t *)kalloc(2U * NTFS_MAX_NAME_LEN);
		long el;

		if (!ex) {
			r = ST_NOMEM;
			goto out;
		}
		el = ntfs_utf8_to_utf16(b->lname, b->llen, ex,
					NTFS_MAX_NAME_LEN);
		r = el > 0 ? ntfs_remove_name(fs, ddir, dst, ex, (uint32_t)el,
					      s_dir ? 1 : 0) :
			     ST_NOT_FOUND;
		kfree(ex);
		if (r != ST_OK)
			goto out;
	}
	/* The new name before the old one goes. */
	r = ntfs_add_name(fs, ddir, src, ub, ulb, ntype);
	if (r != ST_OK)
		goto out;
	ntfs_dc_forget(fs, ddir, b->lname, b->llen);
	r = ntfs_remove_name(fs, sdir, src, ua, (uint32_t)ula, -1);
	if (r == ST_OK)
		ntfs_dc_forget(fs, sdir, a->lname, a->llen);
out:
	ntfs_meta_wunlock(fs);
free_out:
	ntfs_free(a);
	ntfs_free(b);
	ntfs_free(ua);
	ntfs_free(ub);
	return r;
}

/* chmod / chown / utimens by path and by handle */
static int ntfs_attr_path(vfs_mount_t *m, const char *path, int follow,
			  int what, uint32_t mode, int uid, int gid,
			  const struct vfs_utimes *t)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *res;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	if (!res)
		return ST_NOMEM;
	ntfs_meta_wlock(fs);
	r = ntfs_resolve(fs, path, follow, res);
	if (r == ST_OK && ntfs_protected(fs, res->mref))
		r = ST_PERM;
	if (r == ST_OK)
		r = ntfs_setattr(fs, res->mref, what, mode, uid, gid, t);
	ntfs_meta_wunlock(fs);
	kfree(res);
	return r;
}

static int ntfs_attr_file(vfs_file_t *f, int what, uint32_t mode, int uid,
			  int gid, const struct vfs_utimes *t)
{
	ntfs_file_t *nf = ntfs_nf(f);
	int r;

	if (!nf)
		return ST_INVALID;
	ntfs_meta_wlock(nf->fs);
	if (ntfs_protected(nf->fs, nf->mref)) {
		ntfs_meta_wunlock(nf->fs);
		return ST_PERM;
	}
	r = ntfs_setattr(nf->fs, nf->mref, what, mode, uid, gid, t);
	ntfs_meta_wunlock(nf->fs);
	return r;
}

int ntfs_chmod(vfs_mount_t *m, const char *path, unsigned int mode)
{
	return ntfs_attr_path(m, path, 1, 1, mode, -1, -1, NULL);
}

int ntfs_chown(vfs_mount_t *m, const char *path, int uid, int gid,
	       int nofollow)
{
	return ntfs_attr_path(m, path, !nofollow, 2, 0, uid, gid, NULL);
}

int ntfs_fchmod(vfs_file_t *f, unsigned int mode)
{
	return ntfs_attr_file(f, 1, mode, -1, -1, NULL);
}

int ntfs_fchown(vfs_file_t *f, int uid, int gid)
{
	return ntfs_attr_file(f, 2, 0, uid, gid, NULL);
}

int ntfs_utimens(vfs_mount_t *m, const char *path, int nofollow,
		 const struct vfs_utimes *t)
{
	if (!t)
		return ST_INVALID;
	return ntfs_attr_path(m, path, !nofollow, 4, 0, -1, -1, t);
}

int ntfs_futimens(vfs_file_t *f, const struct vfs_utimes *t)
{
	if (!t)
		return ST_INVALID;
	return ntfs_attr_file(f, 4, 0, -1, -1, t);
}

/* ---- readlink / chdir ---- */

int ntfs_readlink(vfs_mount_t *m, const char *path, char *buf,
		  unsigned long bufsz)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *res = NULL;
	ntfs_inode_t *ino = NULL;
	char *tgt = NULL;
	int r;

	if (!fs || !path || !buf || !bufsz)
		return ST_INVALID;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	tgt = pathbuf_get();
	if (!res || !ino || !tgt) {
		r = ST_NOMEM;
		goto out;
	}
	ntfs_meta_rlock(fs);
	r = ntfs_resolve(fs, path, 0, res);
	if (r == ST_OK && res->is_stream)
		r = ST_INVALID;
	if (r == ST_OK)
		r = ntfs_inode_load(fs, res->mref, ino);
	if (r == ST_OK) {
		if (!ntfs_inode_is_link(ino))
			r = ST_INVALID;
		else
			r = ntfs_link_target_ino(fs, ino, res->depth, tgt,
						 PATHBUF_SIZE);
	}
	ntfs_meta_runlock(fs);
	if (r >= 0) {
		unsigned long n = (unsigned long)r;

		if (n > bufsz)
			n = bufsz; /* readlink truncates, no NUL */
		mm_memcpy(buf, tgt, n);
		r = (int)n;
	}
out:
	ntfs_free(res);
	ntfs_free(ino);
	pathbuf_put(tgt);
	return r;
}

int ntfs_chdir(vfs_mount_t *m, const char *path)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *res;
	int r, is_link = 0, is_dir = 0;

	if (!fs || !path)
		return ST_INVALID;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	if (!res)
		return ST_NOMEM;
	ntfs_meta_rlock(fs);
	r = ntfs_resolve(fs, path, 1, res);
	if (r == ST_OK && res->is_stream)
		r = ST_NOTDIR;
	if (r == ST_OK)
		r = ntfs_inode_quick_link(fs, res->mref, &is_link, &is_dir);
	if (r == ST_OK && !is_dir)
		r = ST_NOTDIR;
	if (r == ST_OK)
		fs->cwd = res->mref;
	ntfs_meta_runlock(fs);
	kfree(res);
	return r;
}
