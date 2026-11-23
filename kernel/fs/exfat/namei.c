/* LikeOS - exFAT filesystem driver
 *
 * Name lookup, path resolution, create, unlink, rmdir, mkdir, rename.
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
 * 15. Name lookup and path resolution
 * =========================================================================== */

/* The dcache parent key of a directory stream: its first cluster (stable
 * for its life; the dcache is per filesystem through its owner). */
static inline unsigned long exfat_dkey(const exfat_stream_t *d)
{
	return (unsigned long)d->first_cluster;
}

static inline int exfat_dcache_usable(const exfat_stream_t *d,
				      const char *key)
{
	return d->first_cluster && key[0] && exfat_strlen(key) <= DC_NAME_MAX;
}

static void exfat_dc_forget(exfat_fs_t *fs, exfat_stream_t *d, const char *key)
{
	if (exfat_dcache_usable(d, key))
		dcache_invalidate_cs(&fs->sb, exfat_dkey(d), key);
}

static void exfat_dc_remember(exfat_fs_t *fs, exfat_stream_t *d,
			      const char *key, const exfat_dentry_info_t *di)
{
	if (exfat_dcache_usable(d, key))
		dcache_insert_cs(&fs->sb, exfat_dkey(d), key, di->index,
				 di->size, di->attr);
}

/* Find the name in ctx->up (`n' units, NameHash `hash') in directory `d'.
 * ST_OK with *di (and ctx->set holding the raw set, ctx->name2 the stored
 * name), ST_NOT_FOUND, or an error.  Metadata lock held. */
static int exfat_dir_find(exfat_fs_t *fs, exfat_ctx_t *ctx, exfat_stream_t *d,
			  unsigned n, uint16_t hash, exfat_dentry_info_t *di)
{
	exfat_diter_t it;
	dc_result_t res;
	uint64_t idx = 0, slots = exfat_dir_slots(d);
	int r = ST_NOT_FOUND;

	exfat_assert_meta(fs);
	exfat_diter_init(&it, fs, d);
	/* The position the last lookup of this name found: one set read
	 * instead of a scan -- verified, never trusted. */
	if (exfat_dcache_usable(d, ctx->key)) {
		switch (dcache_lookup_cs(&fs->sb, exfat_dkey(d), ctx->key,
					 &res)) {
		case DC_LOOKUP_NEGATIVE:
			return ST_NOT_FOUND;
		case DC_LOOKUP_FOUND:
			r = exfat_set_read(&it, res.ino, ctx->set, di,
					   ctx->name2);
			if (r == ST_OK && di->name_len == n &&
			    exfat_name_eq(fs, ctx->name2, ctx->up, n)) {
				exfat_diter_done(&it);
				return ST_OK;
			}
			dcache_invalidate_cs(&fs->sb, exfat_dkey(d), ctx->key);
			r = ST_NOT_FOUND;
			break;
		default:
			break;
		}
	}
	while (idx < slots) {
		const uint8_t *e = exfat_diter_get(&it, idx);
		uint8_t type;

		if (!e) {
			if (it.err) {
				r = it.err;
				goto out;
			}
			break;
		}
		type = e[EXFAT_DE_TYPE];
		if (type == EXFAT_ET_EOD)
			break;
		if (!(type & EXFAT_ET_INUSE)) {
			idx++;
			continue;
		}
		if (type != EXFAT_ET_FILE) {
			idx += exfat_skip_slots(e);
			continue;
		}
		unsigned sc = e[EXFAT_DE_SECONDARY_COUNT];

		if (sc < EXFAT_SET_MIN_SECONDARY ||
		    sc > EXFAT_SET_MAX_SECONDARY) {
			WARN_RATELIMIT(1, "exfat: %s: bad entry set at slot %llu",
				       exfat_name_of(fs),
				       (unsigned long long)idx);
			idx++;
			continue;
		}
		/* Cheap test on the stream entry first: length and hash. */
		const uint8_t *s2 = exfat_diter_get(&it, idx + 1);

		if (!s2) {
			r = it.err ? it.err : ST_NOT_FOUND;
			goto out;
		}
		if (s2[EXFAT_DE_TYPE] == EXFAT_ET_STREAM &&
		    s2[EXFAT_STREAM_NAME_LEN] == n &&
		    exfat_le16(s2 + EXFAT_STREAM_NAME_HASH) == hash) {
			int pr = exfat_set_read(&it, idx, ctx->set, di,
						ctx->name2);

			if (pr == ST_OK && exfat_name_eq(fs, ctx->name2,
							 ctx->up, n)) {
				r = ST_OK;
				goto out;
			}
			if (pr == ST_UNSUPPORTED &&
			    exfat_name_eq(fs, ctx->name2, ctx->up, n)) {
				/* The name exists, but the file is not one
				 * this driver can use. */
				r = ST_IO;
				goto out;
			}
			/* A damaged set under the name looked for: the name
			 * may well exist.  The errors= policy applies (read-
			 * only by default), so no second set of that name
			 * can be created next to it. */
			if (pr == ST_IO)
				(void)exfat_corrupt(fs, idx,
						    "%s: entry set at slot %llu skipped",
						    exfat_name_of(fs),
						    (unsigned long long)idx);
			else if (pr < 0 && pr != ST_UNSUPPORTED)
				if (pr != ST_NOT_FOUND) {
					r = pr;
					goto out;
				}
		}
		idx += 1 + sc;
	}
	r = ST_NOT_FOUND;
out:
	exfat_diter_done(&it);
	if (r == ST_OK)
		exfat_dc_remember(fs, d, ctx->key, di);
	else if (r == ST_NOT_FOUND && exfat_dcache_usable(d, ctx->key))
		dcache_insert_negative_cs(&fs->sb, exfat_dkey(d), ctx->key);
	return r;
}

void exfat_walk_release(struct exfat_walk *w)
{
	if (w->st)
		exfat_stream_put(w->st);
	if (w->dir)
		exfat_stream_put(w->dir);
	w->st = NULL;
	w->dir = NULL;
}

/* Strip the leading slashes the VFS hands paths over with. */
static const char *exfat_rel(const char *path)
{
	if (!path)
		return NULL;
	while (*path == '/')
		path++;
	return path;
}

/* Resolve `path' up to its last component: w->dir is that component's
 * directory (held) and, when it exists, w->di its entry set; ctx->name/up/
 * key describe the last component.  The root itself gives is_root (w->dir
 * NULL, w->st the root, held).  "." and ".." are understood anywhere.
 * Metadata lock held. */
int exfat_walk(exfat_fs_t *fs, exfat_ctx_t *ctx, const char *path,
	       struct exfat_walk *w)
{
	const char *p = exfat_rel(path);
	exfat_stream_t *cur;
	int r;

	mm_memset(w, 0, sizeof(*w));
	if (!p)
		return ST_INVALID;
	exfat_stream_hold(fs->root);
	cur = fs->root;
	for (;;) {
		const char *s;
		unsigned long len = 0;
		int last = 1;

		while (*p == '/')
			p++;
		if (!*p) {
			/* No (more) components: the path names `cur'. */
			if (cur == fs->root) {
				w->is_root = 1;
				w->st = cur;
				return ST_OK;
			}
			/* "a/b/.." style endings: name `cur' through its
			 * parent's entry. */
			w->dir = cur->parent;
			if (!w->dir) {
				exfat_stream_put(cur);
				return ST_NOT_FOUND;
			}
			exfat_stream_hold(w->dir);
			w->st = cur;
			w->found = 1;
			w->di.index = cur->entry_index;
			w->di.secondary_count = cur->secondary_count;
			w->di.attr = cur->attr;
			w->di.first_cluster = cur->first_cluster;
			w->di.size = cur->size;
			w->di.vdl = cur->vdl;
			w->di.sflags = cur->sflags;
			w->last = NULL;
			return ST_OK;
		}
		s = p;
		while (p[len] && p[len] != '/')
			len++;
		p += len;
		for (const char *q = p; *q; q++)
			if (*q != '/') {
				last = 0;
				break;
			}
		if (len > VFS_NAME_MAX) {
			exfat_stream_put(cur);
			return ST_NAMETOOLONG;
		}
		if (len == 1 && s[0] == '.') {
			if (last && *p == 0) {
				/* Trailing "/.": the directory itself. */
				continue;
			}
			continue;
		}
		if (len == 2 && s[0] == '.' && s[1] == '.') {
			if (cur->parent) {
				exfat_stream_t *up = cur->parent;

				exfat_stream_hold(up);
				exfat_stream_put(cur);
				cur = up;
			}
			continue;
		}
		if (!cur->is_dir) {
			exfat_stream_put(cur);
			return ST_NOTDIR;
		}
		r = exfat_name_prepare(fs, ctx, s, len, 0, &w->hash);
		if (r == ST_INVALID) {
			/* Not UTF-8: no name on the volume can be it.  A
			 * create of it is refused when it prepares the name
			 * for writing. */
			r = ST_NOT_FOUND;
			w->nlen = 0;
		} else if (r < 0) {
			exfat_stream_put(cur);
			return r;
		} else {
			w->nlen = r;
			r = exfat_dir_find(fs, ctx, cur, (unsigned)w->nlen,
					   w->hash, &w->di);
		}
		if (last) {
			if (r != ST_OK && r != ST_NOT_FOUND) {
				exfat_stream_put(cur);
				return r;
			}
			w->dir = cur;
			w->found = (r == ST_OK);
			w->last = s;
			w->last_len = len;
			w->trailing_slash = (*p == '/');
			if (w->found && w->trailing_slash &&
			    !(w->di.attr & EXFAT_ATTR_DIR)) {
				exfat_walk_release(w);
				return ST_NOTDIR;
			}
			return ST_OK;
		}
		if (r != ST_OK) {
			exfat_stream_put(cur);
			return r;
		}
		if (!(w->di.attr & EXFAT_ATTR_DIR)) {
			exfat_stream_put(cur);
			return ST_NOTDIR;
		}
		{
			int err;
			exfat_stream_t *next = exfat_stream_get(fs, cur, &w->di,
								&err);

			exfat_stream_put(cur);
			if (!next)
				return err;
			cur = next;
		}
	}
}

/* exfat_walk(), and the object's stream in w->st (held) when it exists.
 * ST_NOT_FOUND when it does not. */
int exfat_walk_obj(exfat_fs_t *fs, exfat_ctx_t *ctx, const char *path,
		   struct exfat_walk *w)
{
	int r = exfat_walk(fs, ctx, path, w);
	int err;

	if (r != ST_OK)
		return r;
	if (w->is_root || w->st)
		return ST_OK;
	if (!w->found) {
		exfat_walk_release(w);
		return ST_NOT_FOUND;
	}
	w->st = exfat_stream_get(fs, w->dir, &w->di, &err);
	if (!w->st) {
		exfat_walk_release(w);
		return err;
	}
	return ST_OK;
}

exfat_ctx_t *exfat_ctx_new(void)
{
	return (exfat_ctx_t *)kalloc(sizeof(exfat_ctx_t));
}

void exfat_ctx_free(exfat_ctx_t *ctx)
{
	if (ctx)
		kfree(ctx);
}

/* Create the file of the walk's last component.  Metadata lock exclusive.
 * The new stream (held) in *out. */
int exfat_create_locked(exfat_fs_t *fs, exfat_ctx_t *ctx,
			struct exfat_walk *w, unsigned int mode,
			exfat_stream_t **out)
{
	uint8_t *set = ctx->set;
	exfat_dentry_info_t di;
	uint64_t idx;
	unsigned count;
	uint16_t hash;
	int r, n;
	uint16_t attr = EXFAT_ATTR_ARCHIVE;

	exfat_assert_excl(fs);
	if (!w->last || w->trailing_slash)
		return w->is_root ? ST_EXISTS : ST_INVALID;
	r = exfat_writable(fs);
	if (r != ST_OK)
		return r;
	n = exfat_name_prepare(fs, ctx, w->last, w->last_len, 1, &hash);
	if (n < 0)
		return n;
	r = exfat_may_write(fs);
	if (r != ST_OK)
		return r;
	if (!(mode & 0222))
		attr |= EXFAT_ATTR_RO;
	count = exfat_set_build(fs, set, ctx->name, (unsigned)n, hash, attr,
				EXFAT_SF_ALLOC_POSSIBLE, 0, 0, 0);
	r = exfat_dir_alloc_slots(fs, w->dir, count, &idx);
	if (r != ST_OK)
		return r;
	r = exfat_dir_write(fs, w->dir, idx, count, set);
	if (r != ST_OK)
		return r;
	r = exfat_set_parse(fs, set, &di, ctx->name2);
	if (WARN_ON(r != ST_OK))
		return ST_IO;
	di.index = (uint32_t)idx;
	exfat_dc_forget(fs, w->dir, ctx->key);
	exfat_dc_remember(fs, w->dir, ctx->key, &di);
	exfat_touch_mtime(fs, w->dir);
	exfat_stream_mark_dirty(fs, w->dir);
	r = exfat_commit(fs);
	if (r != ST_OK)
		return r;
	{
		int err;

		*out = exfat_stream_get(fs, w->dir, &di, &err);
		if (!*out)
			return err;
	}
	return ST_OK;
}

/* ---- removing names ----------------------------------------------------------- */

/* Remove the entry set of `st' (found through `w') and release or orphan
 * the stream.  Stream lock and metadata lock exclusive, armed. */
static int exfat_remove_locked(exfat_fs_t *fs, exfat_ctx_t *ctx,
			       struct exfat_walk *w, exfat_stream_t *st)
{
	exfat_stream_t *dir = w->dir;
	exfat_extent_t *vext = NULL;
	uint32_t nvext = 0;
	int r;

	exfat_assert_excl(fs);
	/* The set as it is on the device now (vendor allocations to free),
	 * read before it is cleared. */
	{
		exfat_diter_t it;
		exfat_dentry_info_t di;

		exfat_diter_init(&it, fs, dir);
		r = exfat_set_read(&it, st->entry_index, ctx->set2, &di,
				   ctx->name2);
		exfat_diter_done(&it);
		if (r != ST_OK && r != ST_UNSUPPORTED)
			return r == ST_NOT_FOUND ?
				       exfat_corrupt(fs, st->entry_index,
						     "entry set vanished") :
				       r;
		if (di.secondary_count != st->secondary_count)
			return exfat_corrupt(fs, st->entry_index,
					     "entry set changed size");
	}
	/* ALL OR NOTHING up to here: whatever can fail for want of memory
	 * (the vendor allocations' clusters, room for every deferred free)
	 * comes before the set is cleared. */
	r = exfat_secondary_allocs(fs, ctx->set2, &vext, &nvext);
	if (r == ST_OK)
		r = exfat_free_reserve(fs, (uint64_t)nvext + st->next, 0);
	if (r == ST_OK)
		r = exfat_dir_clear(fs, dir, st->entry_index,
				    st->secondary_count + 1u, ctx->set);
	if (r != ST_OK) {
		if (vext)
			kfree(vext);
		return r;
	}
	exfat_dc_forget(fs, dir, ctx->key);
	if (st->is_dir) {
		if (st->first_cluster)
			dcache_invalidate_dir(&fs->sb, st->first_cluster);
		if (dir->subdirs > 0)
			dir->subdirs--;
	}
	exfat_touch_mtime(fs, dir);
	exfat_stream_mark_dirty(fs, dir);
	/* Vendor allocations go with the set. */
	r = exfat_free_extents(fs, vext, nvext);
	if (vext)
		kfree(vext);
	if (r != ST_OK) {
		/* Only a failed commit of a full free list gets here: the
		 * rest stays allocated (lost). */
		fs->needs_check = 1;
		return r;
	}
	/* The name is gone; the clusters go now unless a handle still uses
	 * them (then at its last close). */
	exfat_stream_orphan(fs, st);
	if (__atomic_load_n(&st->opens, __ATOMIC_SEQ_CST) != 0 &&
	    !st->orphan_held) {
		st->orphan_held = 1;
		fs->orphans_held++;
	}
	if (__atomic_load_n(&st->opens, __ATOMIC_SEQ_CST) == 0) {
		pagecache_invalidate_file(&fs->sb, exfat_chain_of(st));
		st->data_dirty = 0;
		r = exfat_free_extents(fs, st->ext, st->next);
		st->nclusters = 0;
		st->next = 0;
		st->first_cluster = 0;
		if (r != ST_OK) {
			fs->needs_check = 1;
			return r;
		}
	}
	return ST_OK;
}

static int exfat_unlink_rmdir(vfs_mount_t *m, const char *path, int dir)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	exfat_stream_t *st;
	struct exfat_walk w;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	r = exfat_fence(fs, ctx, path, &st);
	if (r < 0) {
		exfat_ctx_free(ctx);
		return r;
	}
	if (!r) {
		/* Not there (or keeps changing): report what a plain
		 * lookup says. */
		exfat_meta_rlock(fs);
		r = exfat_walk_obj(fs, ctx, path, &w);
		if (r == ST_OK) {
			exfat_walk_release(&w);
			r = ST_BUSY;
		}
		exfat_meta_runlock(fs);
		exfat_ctx_free(ctx);
		return r;
	}
	r = exfat_walk(fs, ctx, path, &w);
	if (r != ST_OK)
		goto out;
	if (w.is_root) {
		r = ST_BUSY;
	} else if (!w.last) {
		r = ST_INVALID;
	} else if (dir && !st->is_dir) {
		r = ST_NOTDIR;
	} else if (!dir && st->is_dir) {
		r = ST_ISDIR;
	} else if ((r = exfat_writable(fs)) == ST_OK) {
		if (dir) {
			int e = exfat_dir_empty(fs, st);

			r = e < 0 ? e : (e ? ST_OK : ST_NOTEMPTY);
		}
		if (r == ST_OK)
			r = exfat_may_write(fs);
		if (r == ST_OK)
			r = exfat_remove_locked(fs, ctx, &w, st);
		if (r == ST_OK)
			r = exfat_commit(fs);
	}
	exfat_walk_release(&w);
out:
	exfat_unfence(fs, st);
	exfat_ctx_free(ctx);
	return r;
}

int exfat_unlink_op(vfs_mount_t *m, const char *path)
{
	return exfat_unlink_rmdir(m, path, 0);
}

int exfat_rmdir_op(vfs_mount_t *m, const char *path)
{
	return exfat_unlink_rmdir(m, path, 1);
}

/* ---- mkdir ------------------------------------------------------------------- */

int exfat_mkdir_op(vfs_mount_t *m, const char *path, unsigned int mode)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	struct exfat_walk w;
	struct exfat_run *runs = NULL;
	unsigned nruns = 0, count;
	exfat_dentry_info_t di;
	uint64_t idx;
	uint16_t hash;
	int r, n;

	(void)mode;
	if (!fs || !path)
		return ST_INVALID;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	exfat_meta_wlock(fs);
	r = exfat_walk(fs, ctx, path, &w);
	if (r != ST_OK)
		goto out_unlock;
	if (w.is_root || w.found) {
		r = ST_EXISTS;
		goto out;
	}
	if (!w.last) {
		r = ST_INVALID;
		goto out;
	}
	r = exfat_writable(fs);
	if (r != ST_OK)
		goto out;
	n = exfat_name_prepare(fs, ctx, w.last, w.last_len, 1, &hash);
	if (n < 0) {
		r = n;
		goto out;
	}
	r = exfat_may_write(fs);
	if (r != ST_OK)
		goto out;
	/* The new directory's cluster, zeroed before anything names it. */
	r = exfat_alloc(fs, 0, 1, &runs, &nruns);
	if (r != ST_OK)
		goto out;
	r = exfat_dev_zero(fs,
			   fs->heap_lba + ((uint64_t)(runs[0].start -
						      EXFAT_FIRST_CLUSTER)
					   << (fs->cs_shift - fs->dev_shift)),
			   1ULL << (fs->cs_shift - fs->dev_shift));
	if (r == ST_OK) {
		count = exfat_set_build(fs, ctx->set, ctx->name, (unsigned)n,
					hash,
					EXFAT_ATTR_DIR,
					EXFAT_SF_ALLOC_POSSIBLE |
						EXFAT_SF_NO_FAT_CHAIN,
					runs[0].start, fs->cs, fs->cs);
		r = exfat_dir_alloc_slots(fs, w.dir, count, &idx);
		if (r == ST_OK)
			r = exfat_dir_write(fs, w.dir, idx, count, ctx->set);
	}
	if (r != ST_OK) {
		/* Nothing references the cluster: straight back. */
		exfat_bm_change(fs, runs[0].start, 1, 0);
		exfat_free_count_add(fs, 1);
		goto out;
	}
	if (exfat_set_parse(fs, ctx->set, &di, ctx->name2) == ST_OK) {
		di.index = (uint32_t)idx;
		exfat_dc_forget(fs, w.dir, ctx->key);
		exfat_dc_remember(fs, w.dir, ctx->key, &di);
	}
	if (w.dir->subdirs >= 0)
		w.dir->subdirs++;
	exfat_touch_mtime(fs, w.dir);
	exfat_stream_mark_dirty(fs, w.dir);
	r = exfat_commit(fs);
out:
	exfat_walk_release(&w);
out_unlock:
	exfat_meta_wunlock(fs);
	if (runs)
		kfree(runs);
	exfat_ctx_free(ctx);
	return r;
}

/* ---- rename ------------------------------------------------------------------ */

/* Is `anc' the directory `d' or one of its ancestors? */
static int exfat_is_ancestor(const exfat_stream_t *anc, const exfat_stream_t *d)
{
	for (unsigned depth = 0; d && depth < 4096; depth++, d = d->parent)
		if (d == anc)
			return 1;
	return 0;
}

/* Read the renamed stream's set into ctx->set2 (name in ctx->name2). */
static int exfat_rename_read_src(exfat_fs_t *fs, exfat_ctx_t *ctx,
				 exfat_stream_t *dir, exfat_stream_t *src,
				 exfat_dentry_info_t *odi)
{
	exfat_diter_t it;
	int r;

	exfat_diter_init(&it, fs, dir);
	r = exfat_set_read(&it, src->entry_index, ctx->set2, odi, ctx->name2);
	exfat_diter_done(&it);
	if (r == ST_NOT_FOUND)
		return exfat_corrupt(fs, src->entry_index,
				     "renamed entry set vanished");
	if (r == ST_OK && odi->secondary_count != src->secondary_count)
		return exfat_corrupt(fs, src->entry_index,
				     "renamed entry set changed size");
	return r;
}

static int exfat_rename_locked(exfat_fs_t *fs, exfat_ctx_t *ctx,
			       const char *oldpath, const char *newpath)
{
	struct exfat_walk wo, wn;
	exfat_stream_t *src = NULL, *victim = NULL;
	exfat_dentry_info_t odi;
	unsigned old_count, new_count, nlen;
	uint16_t hash;
	uint64_t new_idx = 0;
	int r, n, have_idx = 0;

	exfat_assert_excl(fs);
	mm_memset(&wn, 0, sizeof(wn));
	r = exfat_walk(fs, ctx, oldpath, &wo);
	if (r != ST_OK)
		return r;
	if (wo.is_root) {
		exfat_walk_release(&wo);
		return ST_BUSY;
	}
	if (!wo.found) {
		exfat_walk_release(&wo);
		return ST_NOT_FOUND;
	}
	if (!wo.last) {
		exfat_walk_release(&wo);
		return ST_INVALID;
	}
	r = exfat_writable(fs);
	if (r == ST_OK)
		r = exfat_may_write(fs);
	if (r != ST_OK) {
		exfat_walk_release(&wo);
		return r;
	}
	src = exfat_stream_get(fs, wo.dir, &wo.di, &r);
	if (!src) {
		exfat_walk_release(&wo);
		return r;
	}
	mm_memcpy(ctx->key2, ctx->key, sizeof(ctx->key2));
	/* Settle every pending entry-set update first: the source's set is
	 * about to move, and the copy taken below must be current. */
	r = exfat_commit(fs);
	if (r != ST_OK)
		goto out;

	r = exfat_walk(fs, ctx, newpath, &wn);
	if (r != ST_OK)
		goto out;
	if (wn.is_root) {
		r = ST_BUSY;
		goto out;
	}
	if (!wn.last) {
		r = ST_INVALID;
		goto out;
	}
	if (wn.trailing_slash && !src->is_dir) {
		r = ST_NOTDIR;
		goto out;
	}
	n = exfat_name_prepare(fs, ctx, wn.last, wn.last_len, 1, &hash);
	if (n < 0) {
		r = n;
		goto out;
	}
	nlen = (unsigned)n;
	/* A directory cannot go below itself. */
	if (src->is_dir && exfat_is_ancestor(src, wn.dir)) {
		r = ST_INVALID;
		goto out;
	}
	if (wn.found && wn.dir == wo.dir && wn.di.index == src->entry_index) {
		{
			/* The same file: a case change, or nothing. */
			int same;

			r = exfat_rename_read_src(fs, ctx, wo.dir, src, &odi);
			if (r != ST_OK)
				goto out;
			old_count = odi.secondary_count + 1u;
			same = odi.name_len == nlen;
			for (unsigned i = 0; same && i < nlen; i++)
				if (ctx->name2[i] != ctx->name[i])
					same = 0;
			if (same) {
				r = ST_OK;
				goto out;
			}
			new_count = exfat_set_rename(ctx->set2, ctx->name, nlen,
						     hash, ctx->set);
			if (!new_count || new_count != old_count) {
				r = new_count ? ST_INVALID : ST_NAMETOOLONG;
				goto out;
			}
			if (exfat_slot_fits(fs, wo.dir, src->entry_index,
					    old_count)) {
				/* In place: one sector write. */
				r = exfat_dir_write(fs, wo.dir,
						    src->entry_index, new_count,
						    ctx->set);
				if (r == ST_OK) {
					exfat_dc_forget(fs, wo.dir, ctx->key2);
					exfat_dc_forget(fs, wn.dir, ctx->key);
					r = exfat_commit(fs);
				}
				goto out;
			}
			/* A set lying across sectors (written elsewhere) is
			 * not rewritten in place: it moves, like any rename
			 * below. */
		}
	} else if (wn.found) {
		victim = exfat_stream_get(fs, wn.dir, &wn.di, &r);
		if (!victim)
			goto out;
		if (src->is_dir && !victim->is_dir) {
			r = ST_NOTDIR;
			goto out;
		}
		if (!src->is_dir && victim->is_dir) {
			r = ST_ISDIR;
			goto out;
		}
		if (victim->is_dir) {
			int e = exfat_dir_empty(fs, victim);

			if (e <= 0) {
				r = e < 0 ? e : ST_NOTEMPTY;
				goto out;
			}
		}
		/* A rename that cannot complete must leave the victim alone:
		 * when the set moves, its new place is found (the directory
		 * grown if need be) before anything is removed.  The slots
		 * found are free and stay free -- the removals below only
		 * clear slots that are in use now. */
		r = exfat_rename_read_src(fs, ctx, wo.dir, src, &odi);
		if (r != ST_OK)
			goto out;
		old_count = odi.secondary_count + 1u;
		new_count = exfat_set_rename(ctx->set2, ctx->name, nlen, hash,
					     ctx->set);
		if (!new_count) {
			r = ST_NAMETOOLONG;
			goto out;
		}
		if (!(wn.dir == wo.dir && new_count <= old_count &&
		      exfat_slot_fits(fs, wo.dir, src->entry_index,
				      old_count))) {
			r = exfat_dir_alloc_slots(fs, wn.dir, new_count,
						  &new_idx);
			if (r == ST_NOSPC &&
			    new_count <= victim->secondary_count + 1u &&
			    exfat_slot_fits(fs, wn.dir, victim->entry_index,
					    new_count)) {
				/* A full directory on a full volume: the
				 * victim's own slots, free once it is
				 * removed, take the new set. */
				new_idx = victim->entry_index;
				r = ST_OK;
			}
			if (r != ST_OK)
				goto out;
			have_idx = 1;
		}
		/* The victim's name goes first, and is STORED before the new
		 * set is written (its clusters are freed by that commit too):
		 * the device must never see the new set while the victim's
		 * set, of the same name, is still there -- not even with the
		 * two writes reordered by the device.  This uses the scratch
		 * buffers, so it comes before the new set is built. */
		r = exfat_remove_locked(fs, ctx, &wn, victim);
		if (r == ST_OK)
			r = exfat_commit(fs);
		if (r != ST_OK)
			goto out;
	}
	/* The source set, fresh, and the new one built from it. */
	r = exfat_rename_read_src(fs, ctx, wo.dir, src, &odi);
	if (r != ST_OK)
		goto out;
	old_count = odi.secondary_count + 1u;
	new_count = exfat_set_rename(ctx->set2, ctx->name, nlen, hash, ctx->set);
	if (!new_count) {
		r = ST_NAMETOOLONG;
		goto out;
	}

	if (wn.dir == wo.dir && new_count <= old_count &&
	    exfat_slot_fits(fs, wo.dir, src->entry_index, old_count)) {
		/* In place: the new set over the old one, the spare entries
		 * marked unused -- one sector write. */
		uint8_t *buf = ctx->set2;

		for (unsigned i = new_count; i < old_count; i++)
			buf[i * EXFAT_DENTRY_SIZE] &= (uint8_t)~EXFAT_ET_INUSE;
		mm_memcpy(buf, ctx->set, new_count * EXFAT_DENTRY_SIZE);
		r = exfat_dir_write(fs, wo.dir, src->entry_index, old_count,
				    buf);
		if (r != ST_OK)
			goto out;
		new_idx = src->entry_index;
	} else {
		/* Moved: the old set is cleared and STORED first, then the
		 * new one written -- a stop in between loses the file's
		 * clusters (fsck reclaims them) but never leaves two sets
		 * naming the same clusters. */
		r = exfat_dir_clear(fs, wo.dir, src->entry_index, old_count,
				    ctx->set2);
		if (r != ST_OK)
			goto out;
		r = exfat_commit(fs);
		if (r == ST_OK && !have_idx)
			r = exfat_dir_alloc_slots(fs, wn.dir, new_count, &new_idx);
		if (r == ST_OK)
			r = exfat_dir_write(fs, wn.dir, new_idx, new_count,
					    ctx->set);
		if (r != ST_OK) {
			/* The file has no name now (the commit that was to
			 * store the cleared set failed, or the new set has
			 * no place): put the old set back where it was --
			 * its clusters were never freed. */
			int r2 = exfat_set_rename(ctx->set, ctx->name2,
						  odi.name_len, odi.name_hash,
						  ctx->set2) ?
					 exfat_dir_write(fs, wo.dir,
							 src->entry_index,
							 old_count, ctx->set2) :
					 ST_IO;

			if (r2 == ST_OK)
				exfat_commit(fs);
			goto out;
		}
	}
	exfat_dc_forget(fs, wo.dir, ctx->key2);
	exfat_dc_forget(fs, wn.dir, ctx->key);
	if (src->is_dir && wo.dir != wn.dir) {
		if (wo.dir->subdirs > 0)
			wo.dir->subdirs--;
		if (wn.dir->subdirs >= 0)
			wn.dir->subdirs++;
	}
	exfat_stream_rekey(fs, src, wn.dir, (uint32_t)new_idx,
			   (uint8_t)(new_count - 1));
	exfat_touch_mtime(fs, wo.dir);
	exfat_stream_mark_dirty(fs, wo.dir);
	if (wn.dir != wo.dir) {
		exfat_touch_mtime(fs, wn.dir);
		exfat_stream_mark_dirty(fs, wn.dir);
	}
	r = exfat_commit(fs);
out:
	if (victim)
		exfat_stream_put(victim);
	if (src)
		exfat_stream_put(src);
	exfat_walk_release(&wn);
	exfat_walk_release(&wo);
	return r;
}

int exfat_rename_op(vfs_mount_t *m, const char *oldpath,
		    const char *newpath)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	exfat_stream_t *dst;
	int r;

	if (!fs || !oldpath || !newpath)
		return ST_INVALID;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	/* An existing destination has its data freed: fence it. */
	r = exfat_fence(fs, ctx, newpath, &dst);
	if (r < 0) {
		/* Never without the fence: the walk was not made. */
	} else if (r) {
		r = exfat_rename_locked(fs, ctx, oldpath, newpath);
		exfat_unfence(fs, dst);
	} else {
		exfat_meta_wlock(fs);
		r = exfat_rename_locked(fs, ctx, oldpath, newpath);
		exfat_meta_wunlock(fs);
	}
	exfat_ctx_free(ctx);
	return r;
}

int exfat_chdir_op(vfs_mount_t *m, const char *path)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	struct exfat_walk w;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	exfat_meta_rlock(fs);
	r = exfat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		if (!w.st->is_dir)
			r = ST_NOTDIR;
		exfat_walk_release(&w);
	}
	exfat_meta_runlock(fs);
	exfat_ctx_free(ctx);
	return r;
}
