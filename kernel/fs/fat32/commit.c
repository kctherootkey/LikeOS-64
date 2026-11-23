/* LikeOS - FAT filesystem driver: the commit machinery: arming, ordered
 * commit, clean marking
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* A mutation is about to change the volume: may it?  ST_OK when the mount
 * is writable; then the chokepoint arms it right before the first real
 * write -- an operation that ends up changing nothing writes nothing.
 * Metadata lock exclusive. */
int fat_may_write(fat_fs_t *fs)
{
	fat_assert_excl(fs);
	if (fs->ro || fs->ro_latched)
		return ST_ROFS;
	fs->may_write = 1;
	return ST_OK;
}

/* Set or clear the clean-shutdown bit of FAT[1] (FAT16, FAT32) in the cache
 * and write the sector holding it to every FAT copy at once.  The sector's
 * other entries go with it: allocations of the running operation, which may
 * always be stored early (section 7).  Metadata lock exclusive, armed. */
static int fat_fat1_write_clean(fat_fs_t *fs, int clean)
{
	struct fat_fatcur cur = { NULL };
	uint32_t v, bit, nv;
	unsigned in, sct, done = 0;
	int r;

	fat_assert_excl(fs);
	if (fs->fat_bits == 12)
		return ST_OK;
	bit = fs->fat_bits == 16 ? FAT16_CLEAN_BIT : FAT32_CLEAN_BIT;
	r = fat_ent_get(fs, &cur, 1, &v);
	if (r != ST_OK)
		goto out;
	nv = clean ? (v | bit) : (v & ~bit);
	if (nv == v)
		goto out;
	r = fat_ent_put(fs, &cur, 1, nv);
	if (r != ST_OK)
		goto out;
	r = fat_ent_locate(fs, &cur, 1, &in);
	if (r != ST_OK)
		goto out;
	sct = in >> fs->dev_shift;
	/* Links held back must not ride along in this sector. */
	if (fs->nlnk) {
		r = fat_links_swap(fs, &done);
		if (r != ST_OK)
			goto out;
	}
	for (unsigned copy = 0; copy < fs->nfats && r == ST_OK; copy++) {
		if (!fs->mirror && copy != fs->active_fat)
			continue;
		r = fat_dev_write(fs, fat_mc_lba(fs, copy, cur.u->off) + sct, 1,
				  cur.u->data + ((unsigned long)sct << fs->dev_shift),
				  FAT_W_FAT);
	}
	if (r == ST_OK) {
		uint64_t flags;

		spin_lock_irqsave(&fs->mc_lock, &flags);
		if (cur.u->dirty & (1u << sct)) {
			cur.u->dirty &= ~(1u << sct);
			if (!cur.u->dirty)
				fs->mc_dirty--;
		}
		spin_unlock_irqrestore(&fs->mc_lock, flags);
	}
	if (done) {
		/* back (this marks their sectors dirty again) */
		int r2 = fat_links_swap(fs, &done);

		if (r2 != ST_OK) {
			WARN_ON_ONCE(1);
			fat_fs_error(fs, "FAT cache inconsistent", 1);
			if (r == ST_OK)
				r = r2;
		}
	}
out:
	fat_fatcur_done(fs, &cur);
	return r;
}

/* Mark the volume dirty on the device and sync: from the chokepoint, at the
 * first real write after fat_may_write().  Metadata lock exclusive. */
int fat_arm_now(fat_fs_t *fs)
{
	int r;

	fat_assert_excl(fs);
	if (fs->armed)
		return ST_OK;
	__atomic_store_n(&fs->armed, 1, __ATOMIC_RELEASE);
	r = fat_fat1_write_clean(fs, 0);
	if (r == ST_OK)
		r = fat_boot_write_state(fs, 1);
	if (r == ST_OK)
		r = fat_dev_sync(fs);
	if (r != ST_OK) {
		__atomic_store_n(&fs->armed, 0, __ATOMIC_RELEASE);
		kprintf("fat: %s: cannot mark the volume dirty (%d); refusing to write\n",
			fat_name_of(fs), r);
		return r == ST_ROFS ? ST_ROFS : ST_IO;
	}
	return ST_OK;
}

/* Drop every deferred change without storing it (a latched read-only mount
 * cannot store anything).  Metadata lock exclusive. */
void fat_commit_abandon(fat_fs_t *fs)
{
	for (unsigned i = 0; i < fs->ndirw; i++) {
		pagecache_put(fs->dirw[i].pg);
		fat_stream_put(fs->dirw[i].d);
	}
	fs->ndirw = 0;
	fs->npf = 0;
	fs->pf_clusters = 0;
	fs->ndfat = 0;
	fs->nlnk = 0;
	while (fs->dirty_head) {
		fat_stream_t *st = fs->dirty_head;

		fs->dirty_head = st->dnext;
		st->dnext = NULL;
		st->on_dirty_list = 0;
		st->meta_dirty = 0;
		fat_stream_put(st);
	}
}

/* Does `st' still have dirty pages in its noted range?  After the commit's
 * flush that means pages another write-back pass had taken (they wait for
 * the lock this task holds): the entry that would cover them must wait for
 * them. */
static int fat_stream_dirty_left(fat_fs_t *fs, fat_stream_t *st)
{
	unsigned long chain = fat_chain_of(st);

	if (st->dhi < st->dlo)
		return 0;
	for (unsigned long p = st->dlo; p <= st->dhi; p++) {
		pc_page_t *pg = pagecache_lookup_held(&fs->sb, chain, p);
		int dirty;

		if (!pg)
			continue;
		dirty = (__atomic_load_n(&pg->flags, __ATOMIC_ACQUIRE) &
			 PC_PAGE_DIRTY) != 0;
		pagecache_put(pg);
		if (dirty)
			return 1;
	}
	return 0;
}

/* Steps 2 to 4 of the commit alone: allocations, barrier, the queued
 * entries.  Used when the queue fills up while the commit itself is running
 * (storing many streams' entries): those entries are independent of each
 * other, and the frees (step 5) stay for the end of the commit, after EVERY
 * entry that drops their clusters is on the device. */
int fat_commit_queue(fat_fs_t *fs)
{
	int r, r2;

	fat_assert_excl(fs);
	r = fat_mc_flush(fs);
	if (fs->ndirw && fs->barrier_needed) {
		r2 = fat_dev_sync(fs);
		if (r == ST_OK)
			r = r2;
	}
	if (r != ST_OK)
		return r; /* the entries wait: their allocations are not stored */
	return fat_dirw_flush(fs);
}

int fat_commit(fat_fs_t *fs)
{
	fat_stream_t *retry = NULL;
	int r = ST_OK, r2, alloc_ok;

	fat_assert_excl(fs);
	might_sleep();
	if (fs->in_commit) {
		/* Never nested: a commit inside a commit would free clusters
		 * before the outer one has stored the entries that drop
		 * them. */
		WARN_ON_ONCE(1);
		return ST_BUSY;
	}
	if (!fs->dirty_head && !fs->ndirw && !fs->npf && !fs->ndfat &&
	    !fs->mc_dirty)
		return ST_OK;
	if (fs->ro_latched || fs->ro || (!fs->armed && !fs->may_write)) {
		WARN_ON_ONCE(!fs->ro_latched && !fs->ro);
		fat_commit_abandon(fs);
		return ST_ROFS;
	}
	fs->in_commit = 1;

	/* 1. Data of the changed streams, then their entries into the queue
	 *    -- only for streams whose data is all stored: an entry never
	 *    describes data that is not on the device (a write error keeps
	 *    the old entry, the stream stays dirty and the error is reported
	 *    by fsync / close / sync). */
	while (fs->dirty_head) {
		fat_stream_t *st = fs->dirty_head;
		int keep = 0;

		fs->dirty_head = st->dnext;
		st->dnext = NULL;
		if (st->data_dirty) {
			uint64_t e0 = fs->io_errors;

			pagecache_flush_file(&fs->sb, fat_chain_of(st));
			if (fs->io_errors != e0) {
				st->wb_err = ST_IO;
				keep = 1;
				if (r == ST_OK)
					r = ST_IO;
			} else if (fat_stream_dirty_left(fs, st)) {
				keep = 1; /* next commit */
			} else {
				st->data_dirty = 0;
				st->dlo = 1;
				st->dhi = 0;
			}
		}
		if (keep) {
			st->dnext = retry;
			retry = st; /* keeps its hold and on_dirty_list */
			continue;
		}
		st->on_dirty_list = 0;
		if (st->meta_dirty) {
			st->meta_dirty = 0;
			r2 = fat_stream_store(fs, st);
			if (r2 != ST_OK && r == ST_OK)
				r = r2;
		}
		fat_stream_put(st);
	}
	/* 2. Allocations (barrier, FAT). */
	alloc_ok = fat_mc_flush(fs) == ST_OK;
	if (!alloc_ok && r == ST_OK)
		r = ST_IO;
	/* 3. Barrier. */
	if (alloc_ok && fs->ndirw && fs->barrier_needed) {
		alloc_ok = fat_dev_sync(fs) == ST_OK;
		if (!alloc_ok && r == ST_OK)
			r = ST_IO;
	}
	/* 4. Entries -- not while the allocations they rely on are not stored
	 *    (they stay queued for the next commit). */
	if (alloc_ok) {
		r2 = fat_dirw_flush(fs);
		if (r2 != ST_OK && r == ST_OK)
			r = r2;
	}
	/* 5. Frees, once nothing on the device references them: the end
	 *    marks of truncated chains first, a barrier, then the freed
	 *    entries, a barrier, then the discards. */
	if (fs->npf || fs->ndfat) {
		if (r != ST_OK || fs->ndirw) {
			/* Not every dereferencing write made it: the clusters
			 * stay allocated (lost, never doubly used). */
			fs->npf = 0;
			fs->pf_clusters = 0;
			fs->ndfat = 0;
			goto out;
		}
		if (fs->barrier_needed) {
			r = fat_dev_sync(fs);
			if (r != ST_OK)
				goto out;
		}
		{
			struct fat_fatcur cur = { NULL };

			for (unsigned i = 0; i < fs->ndfat && r == ST_OK; i++)
				r = fat_ent_put(fs, &cur, fs->dfat[i].cluster,
						fs->dfat[i].value);
			fat_fatcur_done(fs, &cur);
		}
		fs->ndfat = 0;
		if (r == ST_OK)
			r = fat_mc_flush_region(fs);
		if (r == ST_OK && fs->barrier_needed && fs->npf)
			r = fat_dev_sync(fs);
		if (r == ST_OK)
			r = fat_apply_frees(fs);
		if (r == ST_OK) {
			fat_free_count_add(fs, (int64_t)fs->pf_clusters);
			r = fat_mc_flush_region(fs);
		}
		if (r == ST_OK && fs->opts.discard && fs->npf) {
			/* Discard only once the freed entries are durable. */
			r = fat_dev_sync(fs);
			for (unsigned i = 0; i < fs->npf && r == ST_OK; i++)
				fat_dev_discard(fs, fat_cl_lba(fs, fs->pf[i].start),
						(uint64_t)fs->pf[i].len
							<< (fs->cs_shift -
							    fs->dev_shift));
		}
		fs->npf = 0;
		fs->pf_clusters = 0;
	}
out:
	/* Streams whose data is not all stored stay dirty. */
	while (retry) {
		fat_stream_t *st = retry;

		retry = st->dnext;
		st->dnext = fs->dirty_head;
		fs->dirty_head = st;
	}
	fs->in_commit = 0;
	fs->st_commits++;
	return r;
}

/* Store everything and mark the volume clean.  sync(2), unmount, remount
 * read-only.  A write error anywhere (data or metadata) leaves the volume
 * marked dirty and is returned.  Metadata lock exclusive. */
int fat_clean(fat_fs_t *fs)
{
	uint64_t e0 = fs->io_errors;
	int r = ST_OK, r2;

	fat_assert_excl(fs);
	if (!fs->armed && !fs->may_write) {
		/* No mutation was ever allowed: nothing to do -- and nothing
		 * may be written now. */
		if (fs->dirty_head || fs->ndirw || fs->npf || fs->ndfat)
			fat_commit_abandon(fs);
		return ST_OK;
	}
	if (fs->ro_latched) {
		fat_commit_abandon(fs);
		__atomic_store_n(&fs->armed, 0, __ATOMIC_RELEASE);
		fs->may_write = 0;
		return ST_IO;
	}
	/* Every dirty page of the volume first (data of streams not on the
	 * dirty list too); a stream whose pages another pass had taken is
	 * tried again. */
	for (int tries = 0; tries < 4; tries++) {
		pagecache_flush_sb(&fs->sb);
		r = fat_commit(fs);
		if (r != ST_OK || !fs->dirty_head)
			break;
	}
	if (r == ST_OK && fs->dirty_head)
		r = ST_BUSY;
	r2 = fat_mc_flush(fs);
	if (r == ST_OK)
		r = r2;
	if (fs->armed && r == ST_OK) {
		r2 = fat_fsinfo_write(fs);
		if (r == ST_OK)
			r = r2;
	}
	if (fs->armed) {
		r2 = fat_dev_sync(fs);
		if (r == ST_OK)
			r = r2;
	}
	if (r == ST_OK && fs->io_errors != e0)
		r = ST_IO;
	if (!fs->armed) {
		/* The mutations changed nothing on the device. */
		if (r == ST_OK)
			fs->may_write = 0;
		return r;
	}
	if (r == ST_OK && !fs->errors_seen && !fs->was_dirty &&
	    !fs->ro_latched) {
		r = fat_fat1_write_clean(fs, 1);
		if (r == ST_OK)
			r = fat_boot_write_state(fs, 0);
		if (r == ST_OK)
			r = fat_dev_sync(fs);
	}
	if (r == ST_OK || fs->ro_latched) {
		__atomic_store_n(&fs->armed, 0, __ATOMIC_RELEASE);
		fs->may_write = 0;
	}
	return r;
}

/* ===========================================================================
 * 18. Cluster chains of streams
 * =========================================================================== */

/* Append the allocated runs to a stream: extents and FAT links.  ALL OR
 * NOTHING.  The extent array is grown before anything changes, so the only
 * step that can fail is a FAT write (a FAT unit that cannot be read or
 * allocated); then the one FAT entry of the stream that was changed -- the
 * old last cluster's -- gets its old value back (and its deferred end mark,
 * if it had one), and the stream in memory is untouched.  Every other FAT
 * entry written belongs to the runs, which the caller gives back.  Should
 * even that restore fail, *pinned says so: the caller must then keep the
 * runs allocated (lost clusters, not clusters a chain on the device may lead
 * into while they are free).  Metadata lock exclusive; the runs are marked
 * used in the FAT. */
int fat_stream_append(fat_fs_t *fs, fat_stream_t *st,
			     const struct fat_run *runs, unsigned nruns,
			     int *pinned)
{
	struct fat_fatcur cur = { NULL };
	uint32_t last = fat_st_last(st), old_last_val = 0, def_val = 0;
	uint64_t total = 0;
	int had_def = 0, touched_last = 0, noted = 0;
	int r = ST_OK;

	fat_assert_excl(fs);
	*pinned = 0;
	if (nruns == 0)
		return ST_OK;
	for (unsigned i = 0; i < nruns; i++)
		total += runs[i].len;
	if ((uint64_t)st->nclusters + total > fs->cluster_count)
		return ST_NOSPC;

	/* 1. Room for the held-back link and in the extent array first:
	 *    appending cannot fail later. */
	if (st->nclusters) {
		r = fat_link_reserve(fs);
		if (r != ST_OK)
			return r;
		last = fat_st_last(st);
	}
	if ((uint64_t)st->next + nruns > st->ext_cap) {
		uint64_t want = (uint64_t)st->next + nruns;
		uint64_t nc = st->ext_cap ? (uint64_t)st->ext_cap * 2 : 4;
		fat_extent_t *n;

		if (nc < want)
			nc = want;
		if (nc > FAT_MAX_EXTENTS)
			nc = FAT_MAX_EXTENTS;
		if (nc < want)
			return ST_NOMEM;
		n = (fat_extent_t *)krealloc(st->ext, (size_t)nc * sizeof(*n));
		if (!n)
			return ST_NOMEM;
		st->ext = n;
		st->ext_cap = (uint32_t)nc;
	}
	if (st->nclusters) {
		r = fat_fat_read(fs, &cur, last, &old_last_val);
		if (r != ST_OK) {
			fat_fatcur_done(fs, &cur);
			return r;
		}
		had_def = fat_fat_deferred(fs, last, &def_val);
	}

	/* 2. The FAT: link the old end to the first new cluster, the runs
	 *    internally and to each other, and end the chain. */
	if (st->nclusters) {
		/* Recorded first: a flush while the runs are written (a cache
		 * full of dirty units) already holds the link back. */
		touched_last = 1;
		noted = fat_link_note(fs, last, old_last_val);
		r = fat_fat_write(fs, &cur, last, runs[0].start);
	}
	for (unsigned i = 0; i < nruns && r == ST_OK; i++) {
		for (uint32_t k = 0; k + 1 < runs[i].len && r == ST_OK; k++)
			r = fat_fat_write(fs, &cur, runs[i].start + k,
					  runs[i].start + k + 1);
		if (r != ST_OK)
			break;
		r = fat_fat_write(fs, &cur, runs[i].start + runs[i].len - 1,
				  i + 1 < nruns ? runs[i + 1].start :
						  fs->eoc_val);
	}
	if (r != ST_OK) {
		/* Undo the one link the stream's chain can lead through. */
		if (touched_last) {
			int r2 = fat_fat_write(fs, &cur, last, old_last_val);

			if (r2 == ST_OK && noted)
				fat_link_forget(fs, last);

			if (r2 == ST_OK && had_def)
				r2 = fat_fat_defer(fs, last, def_val);
			if (r2 != ST_OK) {
				*pinned = 1;
				WARN_RATELIMIT(1, "fat: %s: cannot undo a FAT link (%d): %u clusters stay allocated",
					       fat_name_of(fs), r2,
					       (unsigned)total);
			}
		}
		fat_fatcur_done(fs, &cur);
		return r;
	}
	fat_fatcur_done(fs, &cur);

	/* 3. Memory: cannot fail (room reserved above). */
	for (unsigned i = 0; i < nruns; i++) {
		r = fat_ext_append(&st->ext, &st->next, &st->ext_cap,
				   st->nclusters, runs[i].start, runs[i].len);
		BUG_ON(r != ST_OK); /* capacity was reserved */
		st->nclusters += runs[i].len;
	}
	if (st->first_cluster == 0 || st->nclusters == total)
		st->first_cluster = runs[0].start;
	return ST_OK;
}
