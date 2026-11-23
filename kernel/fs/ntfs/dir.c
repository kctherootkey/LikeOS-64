/* LikeOS - NTFS filesystem driver
 *
 * Directory lookup, path resolution, readdir.
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
 * S20  Directory lookup and path resolution
 *
 * ntfs_dir_lookup() answers "which MFT reference does `name' have in
 * directory `dir'", through the dentry cache (*_cs interface, keyed by the
 * directory's $I30 stream chain id, owner = this mount's superblock).
 * Names are compared case-sensitively with the B+tree's collation; with
 * `nocase' the cache key is the name folded through $UpCase and the index
 * search ignores case.  Misses after a complete search are remembered as
 * negative entries.  MFT references read from an index entry are
 * untrusted: one outside the MFT, or record 0, is corrupt metadata.
 *
 * ntfs_resolve() walks a path (relative to the mount; absolute paths start
 * at the root, relative ones at the working directory) iteratively:
 * symbolic links met on the way are handed to the VFS (vfs_link_hop) when
 * the path is the operation's own, else followed here, at most
 * NTFS_MAX_LINKS times, without recursion.  A final component "base:stream"
 * names an alternate data stream (streams_interface, ntfs.h).
 * =========================================================================== */

/* dcache key of `name' (`nlen' bytes) in `key' (DC_NAME_MAX + 1 bytes). */
int ntfs_dc_key(ntfs_fs_t *fs, const char *name, uint32_t nlen,
		char *key, uint16_t *u16)
{
	long n;

	if (nlen == 0 || nlen > DC_NAME_MAX)
		return 0;
	if (!fs->opt.nocase) {
		mm_memcpy(key, name, nlen);
		key[nlen] = '\0';
		return 1;
	}
	n = ntfs_utf8_to_utf16(name, nlen, u16, NTFS_MAX_NAME_LEN);
	if (n <= 0)
		return 0;
	for (long i = 0; i < n; i++)
		ntfs_put_le16(&u16[i], ntfs_upc(fs, ntfs_le16(&u16[i])));
	return ntfs_utf16_to_utf8(u16, (uint32_t)n, key, DC_NAME_MAX + 1) > 0;
}

/* Look `name' up in directory `dir'.  ST_OK (*mref, *fattr), ST_NOT_FOUND,
 * ST_NOTDIR, ST_IO. */
int ntfs_dir_lookup(ntfs_fs_t *fs, uint64_t dir, const char *name,
		    uint32_t nlen, uint64_t *mref, uint32_t *fattr)
{
	ntfs_stream_t *ds = NULL;
	ntfs_index_t *ix = NULL;
	uint16_t *u16 = NULL;
	char *key = NULL;
	uint8_t *ent = NULL;
	dc_result_t res;
	ntfs_ikey_t k;
	int have_key, r;
	long n;

	ntfs_assert_meta_any(fs);
	if (nlen == 0 || nlen > VFS_NAME_MAX)
		return nlen ? ST_NAMETOOLONG : ST_NOT_FOUND;
	r = ntfs_dir_stream(fs, dir, &ds);
	if (r != ST_OK)
		return r;
	u16 = (uint16_t *)kalloc(NTFS_MAX_NAME_LEN * 2);
	key = (char *)kalloc(DC_NAME_MAX + 1);
	if (!u16 || !key) {
		r = ST_NOMEM;
		goto out;
	}
	have_key = ntfs_dc_key(fs, name, nlen, key, u16);
	if (have_key) {
		switch (dcache_lookup_cs(&fs->sb, ntfs_chain(ds), key, &res)) {
		case DC_LOOKUP_FOUND:
			*mref = res.ino;
			if (fattr)
				*fattr = res.attr;
			r = ST_OK;
			goto out;
		case DC_LOOKUP_NEGATIVE:
			r = ST_NOT_FOUND;
			goto out;
		default:
			break;
		}
	}
	n = ntfs_utf8_to_utf16(name, nlen, u16, NTFS_MAX_NAME_LEN);
	if (n <= 0) {
		/* Not representable on NTFS: it cannot exist. */
		r = n == ST_NAMETOOLONG ? ST_NAMETOOLONG : ST_NOT_FOUND;
		goto out;
	}
	r = ntfs_index_open(fs, dir, ntfs_i30_name, 4, ds, &ix);
	if (r != ST_OK) {
		if (r == ST_NOT_FOUND)
			r = ST_NOTDIR;
		goto out;
	}
	k.key = u16;
	k.len = (uint32_t)n;
	k.ic = fs->opt.nocase;
	r = ntfs_index_lookup(ix, &k, &ent);
	if (r == ST_OK) {
		const ntfs_index_entry_t *e = (const ntfs_index_entry_t *)ent;
		uint64_t ref = e->data.indexed_file;
		uint64_t no = NTFS_MREF_NO(ref);

		/* Record 0 is $MFT itself: named only by the root. */
		if ((no == 0 && NTFS_MREF_NO(dir) != NTFS_FILE_ROOT) ||
		    no >= fs->mft_records) {
			WARN_RATELIMIT(1, "ntfs: directory %llu: entry points "
					  "to MFT record %llu",
				       (unsigned long long)NTFS_MREF_NO(dir),
				       (unsigned long long)no);
			r = ST_IO;
		} else {
			*mref = ref;
			if (fattr)
				*fattr = ntfs_ie_fn(e)->file_attributes;
			if (have_key)
				dcache_insert_cs(&fs->sb, ntfs_chain(ds), key,
						 ref, 0,
						 ntfs_ie_fn(e)->file_attributes);
		}
		kfree(ent);
	} else if (r == ST_NOT_FOUND && have_key) {
		dcache_insert_negative_cs(&fs->sb, ntfs_chain(ds), key);
	}
out:
	ntfs_index_close(ix);
	ntfs_stream_put(fs, ds);
	ntfs_free(u16);
	ntfs_free(key);
	return r;
}

/* The parent directory of `mref' (from its first non-DOS $FILE_NAME). */
int ntfs_parent_of(ntfs_fs_t *fs, uint64_t mref, uint64_t *parent)
{
	ntfs_actx_t *c;
	uint64_t dos = 0;
	int r;

	if (NTFS_MREF_NO(mref) == NTFS_FILE_ROOT) {
		*parent = mref;
		return ST_OK;
	}
	r = ntfs_actx_open(fs, mref, &c);
	if (r != ST_OK)
		return r;
	*parent = 0;
	while ((r = ntfs_actx_find(c, NTFS_AT_FILE_NAME, NULL, 0, 0)) == ST_OK) {
		const ntfs_file_name_t *fn =
			(const ntfs_file_name_t *)ntfs_attr_value(c->a);

		if (fn->file_name_type == NTFS_FILE_NAME_DOS) {
			if (!dos)
				dos = fn->parent_directory;
			continue;
		}
		*parent = fn->parent_directory;
		break;
	}
	ntfs_actx_close(c);
	if (!*parent)
		*parent = dos;
	if (!*parent)
		return r == ST_OK || r == ST_NOT_FOUND ? ST_IO : r;
	return ST_OK;
}

/* Depth of directory `mref' below the root (bounded walk). */
uint32_t ntfs_dir_depth(ntfs_fs_t *fs, uint64_t mref)
{
	uint32_t depth = 0;

	while (NTFS_MREF_NO(mref) != NTFS_FILE_ROOT && depth < 256) {
		uint64_t p;

		if (ntfs_parent_of(fs, mref, &p) != ST_OK)
			break;
		mref = p;
		depth++;
	}
	return depth;
}

/* Split a final component "base:stream" (streams_interface).  Returns 1
 * and the base length when it is a stream reference. */
static int ntfs_stream_split(ntfs_fs_t *fs, const char *comp, uint32_t clen,
			     uint32_t *blen, const char **sname,
			     uint32_t *slen)
{
	uint32_t i;

	if (fs->opt.streams == NTFS_STREAMS_NONE)
		return 0;
	for (i = 0; i < clen && comp[i] != ':'; i++)
		;
	if (i == clen || i == 0 || i + 1 >= clen)
		return 0;
	/* Typed forms ("a::$DATA", "a:s:$DATA") are not accepted. */
	for (uint32_t j = i + 1; j < clen; j++)
		if (comp[j] == ':')
			return 0;
	*blen = i;
	*sname = comp + i + 1;
	*slen = clen - i - 1;
	return 1;
}

/* Resolve `path'.  `follow' follows a final symbolic link. */
int ntfs_resolve(ntfs_fs_t *fs, const char *path, int follow,
		 ntfs_res_t *res)
{
	char *buf = NULL, *tgt = NULL;
	const char *p;
	uint64_t cur, root = NTFS_MAKE_MREF(NTFS_FILE_ROOT, 0);
	uint32_t depth, links = 0;
	int pass0 = 1, trailing, r = ST_OK;
	unsigned long plen = ntfs_strlen(path);

	ntfs_assert_meta_any(fs);
	mm_memset(res, 0, sizeof(*res));
	if (!path || plen >= VFS_MAX_PATH)
		return ST_NAMETOOLONG;
	trailing = plen > 0 && path[plen - 1] == '/';
	if (trailing)
		follow = 1;
	if (path[0] == '/' || !fs->cwd) {
		cur = root;
		depth = 0;
	} else {
		cur = fs->cwd;
		depth = fs->opt.native_symlink == NTFS_SYMLINK_REL ?
				ntfs_dir_depth(fs, cur) :
				0;
	}
	p = path;
	res->mref = cur;
	res->parent = cur;
	res->depth = depth;
	for (;;) {
		const char *cstart, *cend;
		uint32_t clen, blen, slen;
		const char *sname;
		uint64_t child;
		int last, is_link = 0, is_dir = 0, stream = 0;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		cstart = p;
		while (*p && *p != '/')
			p++;
		cend = p;
		clen = (uint32_t)(cend - cstart);
		while (*p == '/')
			p++;
		last = (*p == '\0');
		if (clen > VFS_NAME_MAX) {
			r = ST_NAMETOOLONG;
			goto out;
		}
		if (clen == 1 && cstart[0] == '.')
			continue;
		if (clen == 2 && cstart[0] == '.' && cstart[1] == '.') {
			uint64_t par;

			r = ntfs_parent_of(fs, cur, &par);
			if (r != ST_OK)
				goto out;
			cur = par;
			if (depth)
				depth--;
			res->mref = res->parent = cur;
			res->depth = depth;
			continue;
		}
		if (last) {
			mm_memcpy(res->lname, cstart, clen);
			res->lname[clen] = '\0';
			res->llen = clen;
		}
		r = ST_NOT_FOUND;
		if (last && fs->opt.streams == NTFS_STREAMS_WINDOWS &&
		    ntfs_stream_split(fs, cstart, clen, &blen, &sname, &slen)) {
			stream = 1; /* every "a:b" is a stream */
		} else {
			r = ntfs_dir_lookup(fs, cur, cstart, clen, &child, NULL);
			if (r == ST_NOT_FOUND && last &&
			    ntfs_stream_split(fs, cstart, clen, &blen, &sname,
					      &slen)) {
				stream = 1;
			} else if (r != ST_OK) {
				if (r == ST_NOT_FOUND && last) {
					res->last_missing = 1;
					res->missing_dir = cur;
					res->depth = depth;
				}
				goto out;
			}
		}
		if (stream) {
			long n;

			r = ntfs_dir_lookup(fs, cur, cstart, blen, &child, NULL);
			if (r != ST_OK)
				goto out;
			n = ntfs_utf8_to_utf16(sname, slen, res->sname,
					       NTFS_MAX_NAME_LEN);
			if (n <= 0) {
				r = ST_NOT_FOUND;
				goto out;
			}
			res->is_stream = 1;
			res->snlen = (uint32_t)n;
			res->mref = child;
			res->parent = cur;
			res->depth = depth;
			if (trailing) {
				r = ST_NOTDIR;
				goto out;
			}
			break;
		}
		r = ntfs_inode_quick_link(fs, child, &is_link, &is_dir);
		if (r != ST_OK)
			goto out;
		if (is_link && (!last || follow)) {
			ntfs_inode_t *ino;
			long tl;

			if (pass0 && path[0] == '/') {
				if (!tgt) {
					tgt = pathbuf_get();
					if (!tgt) {
						r = ST_NOMEM;
						goto out;
					}
				}
				ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
				if (!ino) {
					r = ST_NOMEM;
					goto out;
				}
				r = ntfs_inode_load(fs, child, ino);
				tl = r == ST_OK ? ntfs_link_target_ino(
							  fs, ino, depth, tgt,
							  PATHBUF_SIZE) :
						  r;
				kfree(ino);
				if (tl < 0) {
					r = (int)tl;
					goto out;
				}
				if (vfs_link_hop(fs->mnt, path,
						 (unsigned long)(cstart - path),
						 (unsigned long)(cend - path),
						 tgt)) {
					r = ST_LINK;
					goto out;
				}
			}
			/* Follow it here: target + "/" + the rest. */
			if (++links > NTFS_MAX_LINKS) {
				r = ST_LOOP;
				goto out;
			}
			{
				char *nb = (char *)kalloc(2 * VFS_MAX_PATH);
				unsigned long rl = ntfs_strlen(p), o = 0;

				ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
				if (!nb || !ino) {
					ntfs_free(nb);
					ntfs_free(ino);
					r = ST_NOMEM;
					goto out;
				}
				r = ntfs_inode_load(fs, child, ino);
				tl = r == ST_OK ? ntfs_link_target_ino(
							  fs, ino, depth, nb,
							  VFS_MAX_PATH) :
						  r;
				kfree(ino);
				if (tl <= 0) {
					kfree(nb);
					r = tl < 0 ? (int)tl : ST_NOT_FOUND;
					goto out;
				}
				o = (unsigned long)tl;
				if (o + 2 + rl >= 2 * VFS_MAX_PATH) {
					kfree(nb);
					r = ST_NAMETOOLONG;
					goto out;
				}
				if (rl) {
					nb[o++] = '/';
					mm_memcpy(nb + o, p, rl);
					o += rl;
				} else if (trailing) {
					nb[o++] = '/';
				}
				nb[o] = '\0';
				if (nb[0] == '/') {
					cur = root;
					depth = 0;
				}
				ntfs_free(buf);
				buf = nb;
				p = buf;
				pass0 = 0;
				res->mref = res->parent = cur;
				res->depth = depth;
				continue;
			}
		}
		if (!last && !is_dir) {
			r = ST_NOTDIR;
			goto out;
		}
		res->parent = cur;
		res->depth = depth;
		res->mref = child;
		if (!last) {
			cur = child;
			depth++;
		}
	}
	if (trailing && !res->is_stream) {
		int is_link = 0, is_dir = 0;

		r = ntfs_inode_quick_link(fs, res->mref, &is_link, &is_dir);
		if (r == ST_OK && !is_dir)
			r = ST_NOTDIR;
	}
out:
	ntfs_free(buf);
	pathbuf_put(tgt);
	return r;
}

/* Directory / link classification of `mref' without loading EAs: the
 * record's directory flag and its reparse tag. */
int ntfs_inode_quick_link(ntfs_fs_t *fs, uint64_t mref, int *is_link,
			  int *is_dir)
{
	ntfs_actx_t *c;
	ntfs_mft_record_t *m;
	int r;

	*is_link = 0;
	*is_dir = 0;
	r = ntfs_actx_open(fs, mref, &c);
	if (r != ST_OK)
		return r == ST_NOT_FOUND ? ST_IO : r;
	m = (ntfs_mft_record_t *)c->base;
	*is_dir = !!(m->flags & NTFS_MFT_REC_IS_DIRECTORY);
	r = ntfs_actx_find(c, NTFS_AT_REPARSE_POINT, NULL, 0, 0);
	if (r == ST_OK) {
		uint32_t tag = 0;

		if (!c->a->non_resident) {
			if (c->a->data.resident.value_length >= 4)
				tag = ntfs_le32(ntfs_attr_value(c->a));
		} else {
			uint8_t *rp;
			uint32_t rl;

			ntfs_actx_close(c);
			c = NULL;
			r = ntfs_reparse_read(fs, mref, &rp, &rl);
			if (r != ST_OK)
				return r;
			tag = ntfs_le32(rp);
			kfree(rp);
		}
		*is_link = ntfs_tag_is_link(tag);
		if (*is_link)
			*is_dir = 0;
		r = ST_OK;
	} else if (r == ST_NOT_FOUND) {
		r = ST_OK;
	}
	ntfs_actx_close(c);
	return r;
}

/* ---- readdir ----
 *
 * Cookies (d_off) are entry ordinals: "." is 0, ".." 1, then the index
 * entries in collation order.  The handle keeps the ordinal of the next
 * entry and, as the resume key, the name of the last entry consumed; every
 * call descends the B+tree once per leaf to the first entry after that
 * key, so entries are neither repeated nor skipped while the directory is
 * read.  A seek to another cookie starts over and skips that many listed
 * entries. */

struct ntfs_rd {
	ntfs_file_t *nf;
	uint8_t *ubuf; /* the caller's buffer */
	long cap;
	long used;
	int full;
	char *name; /* conversion buffer */
};

/* Emit one dirent64 (or count it while skipping); 1 when the buffer is
 * full. */
static int ntfs_rd_emit(struct ntfs_rd *rd, uint64_t ino, uint8_t type,
			const char *name, long nlen)
{
	unsigned reclen = (unsigned)((19 + nlen + 1 + 7) & ~7L);
	struct dirent64 *d;

	if (rd->nf->rd_ord < rd->nf->rd_skip) {
		rd->nf->rd_ord++;
		return 0;
	}
	if (rd->used + (long)reclen > rd->cap) {
		rd->full = 1;
		return 1;
	}
	/* A kernel buffer: copied out after the lock is gone. */
	d = (struct dirent64 *)(rd->ubuf + rd->used);
	d->d_ino = ino;
	d->d_off = (int64_t)(rd->nf->rd_ord + 1);
	d->d_reclen = (uint16_t)reclen;
	d->d_type = type;
	mm_memcpy(d->d_name, name, (unsigned long)nlen);
	d->d_name[nlen] = '\0';
	rd->used += reclen;
	rd->nf->rd_ord++;
	return 0;
}

static int ntfs_rd_cb(ntfs_index_t *ix, const ntfs_index_entry_t *e,
		      void *arg)
{
	struct ntfs_rd *rd = (struct ntfs_rd *)arg;
	ntfs_file_t *nf = rd->nf;
	ntfs_fs_t *fs = nf->fs;
	const ntfs_file_name_t *fn = ntfs_ie_fn(e);
	long nlen;

	/* Entries come in strictly increasing collation order; one that
	 * does not would make the walk go back and never end: the index is
	 * corrupt. */
	if (nf->rd_key_valid &&
	    ntfs_collate_names(fs, ntfs_fn_name(fn), fn->file_name_length,
			       nf->rd_key, nf->rd_key_len, 0) <= 0) {
		WARN_RATELIMIT(1, "ntfs: directory %llu: index entries out of "
				  "order",
			       (unsigned long long)ix->mft_no);
		return ST_IO;
	}
	if (ntfs_entry_listed(fs, ix->mft_no, e)) {
		/* Names that cannot be a path component (a NUL or '/'
		 * stored on the volume) are not listed. */
		nlen = ntfs_utf16_to_utf8(ntfs_fn_name(fn),
					  fn->file_name_length, rd->name,
					  NTFS_MAX_NAME_LEN * 3 + 1);
		if (nlen > VFS_NAME_MAX) {
			/* 255 UTF-16 units can need up to 765 bytes of UTF-8,
			 * more than a path component may have: such a name
			 * could not be looked up, and cutting it could name
			 * another file.  Not listed. */
			WARN_RATELIMIT(1, "ntfs: directory %llu holds a name "
					  "longer than %d bytes in UTF-8; not "
					  "listed",
				       (unsigned long long)ix->mft_no,
				       VFS_NAME_MAX);
		} else if (nlen > 0) {
			if (ntfs_rd_emit(rd,
					 NTFS_MREF_NO(e->data.indexed_file),
					 ntfs_fn_dtype(fn), rd->name, nlen))
				return 1; /* full: resume at this entry */
		} else {
			WARN_RATELIMIT(1, "ntfs: directory %llu holds a name "
					  "that is not a valid path component",
				       (unsigned long long)ix->mft_no);
		}
	}
	/* The entry is consumed: it becomes the resume key (the
	 * position advances only after an entry was taken). */
	mm_memcpy(nf->rd_key, ntfs_fn_name(fn), fn->file_name_length * 2U);
	nf->rd_key_len = fn->file_name_length;
	nf->rd_key_valid = 1;
	return 0;
}

static long ntfs_readdir_impl(ntfs_file_t *nf, void *buf, long bytes)
{
	ntfs_fs_t *fs = nf->fs;
	ntfs_index_t *ix = NULL;
	struct ntfs_rd rd;
	int r = ST_OK;

	rd.nf = nf;
	rd.ubuf = (uint8_t *)buf;
	rd.cap = bytes;
	rd.used = 0;
	rd.full = 0;
	rd.name = (char *)kalloc(NTFS_MAX_NAME_LEN * 3 + 1);
	if (!rd.name)
		return ST_NOMEM;
	/* "." and ".." first (ordinals 0 and 1). */
	while (nf->rd_ord < 2) {
		uint64_t ino = nf->rd_ord == 0 ? NTFS_MREF_NO(nf->mref) :
						 NTFS_MREF_NO(nf->rd_parent);

		if (ntfs_rd_emit(&rd, ino, DT_DIR, "..",
				 nf->rd_ord == 0 ? 1 : 2))
			goto done;
	}
	if (nf->rd_eof)
		goto done;
	r = ntfs_index_open(fs, nf->mref, ntfs_i30_name, 4, nf->st, &ix);
	if (r != ST_OK)
		goto done;
	while (!rd.full) {
		ntfs_ikey_t k;
		int w;

		k.key = nf->rd_key;
		k.len = nf->rd_key_len;
		k.ic = 0;
		w = ntfs_index_walk(ix, nf->rd_key_valid ? &k : NULL,
				    ntfs_rd_cb, &rd);
		if (w == ST_EOF) {
			nf->rd_eof = 1;
			break;
		}
		if (w < 0) {
			/* The error is returned, the position
			 * is not marked as the end. */
			r = w;
			break;
		}
	}
done:
	ntfs_index_close(ix);
	kfree(rd.name);
	if (rd.used)
		return rd.used;
	if (r != ST_OK)
		return r;
	if (rd.full)
		return ST_INVALID; /* the buffer cannot hold one entry */
	return 0;
}

long ntfs_readdir(vfs_file_t *f, void *buf, long bytes)
{
	ntfs_file_t *nf = ntfs_nf(f);
	long r;

	if (!nf || !buf || bytes <= 0)
		return ST_INVALID;
	if (!nf->is_dir)
		return ST_NOTDIR;
	{
		long cap = bytes > 32768 ? 32768 : bytes;
		uint8_t *kbuf = (uint8_t *)kalloc((unsigned long)cap);

		if (!kbuf)
			return ST_NOMEM;
		ntfs_meta_rlock(nf->fs);
		r = ntfs_readdir_impl(nf, kbuf, cap);
		ntfs_meta_runlock(nf->fs);
		/* User memory only now, with no lock held. */
		if (r > 0 && copy_to_user(buf, kbuf, (size_t)r))
			r = ST_INVALID;
		kfree(kbuf);
	}
	return r;
}
