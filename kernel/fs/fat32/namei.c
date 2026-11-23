/* LikeOS - FAT filesystem driver: long and short (8.3) names, name lookup,
 * path resolution, unlink, rmdir, mkdir and rename
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* Length of a name without its trailing dots (the volume never stores
 * them: "name." is "name"). */
static unsigned fat_strip_dots(const uint16_t *u, unsigned n)
{
	while (n && u[n - 1] == '.')
		n--;
	return n;
}

/* The dcache key of folded units: their UTF-8 form.  "" when it does not
 * fit (then nothing is cached for the name). */
static void fat_make_key(const uint16_t *up, unsigned n, char *key,
			 unsigned cap)
{
	if (fat_utf16_to_utf8(up, n, key, cap) < 0)
		key[0] = 0;
}

/* A path component as the volume would store it: UTF-16 in ctx->name
 * (trailing dots dropped), folded copy in ctx->up, the folded UTF-8 dcache
 * key in ctx->key.  `for_create' also refuses names a long name cannot
 * hold.  Returns the length in units or an ST_ error. */
int fat_name_prepare(fat_fs_t *fs, fat_ctx_t *ctx, const char *comp,
			    unsigned long len, int for_create)
{
	int n;

	if (len == 0)
		return ST_INVALID;
	n = fat_ext_to_utf16(fs, comp, len, ctx->name, FAT_NAME_MAX);
	if (n < 0)
		return n == ST_NAMETOOLONG ? ST_NAMETOOLONG : ST_INVALID;
	n = (int)fat_strip_dots(ctx->name, (unsigned)n);
	if (n == 0)
		return ST_INVALID;
	if (for_create) {
		if (ctx->name[n - 1] == ' ')
			return ST_INVALID;
		for (int i = 0; i < n; i++)
			if (fat_lfn_bad_char(ctx->name[i]))
				return ST_INVALID;
	}
	fat_fold(ctx->name, (unsigned)n, ctx->up);
	fat_make_key(ctx->up, (unsigned)n, ctx->key, sizeof(ctx->key));
	return n;
}

/* ---- Short names ------------------------------------------------------------------- */

/* Short name display: the 11 bytes `nm' (with the case bits `ntres') as
 * shown under the mount's shortname= mode, into `out' (13 units at most).
 * Returns the number of units; 0 for a name whose base is empty (corrupt). */
unsigned fat_sfn_display(fat_fs_t *fs, const uint8_t *nm, uint8_t ntres,
				uint16_t *out)
{
	int lb, le;
	unsigned bl = 8, el = 3, o = 0;

	if (fs->opts.shortname & FAT_SN_DISPLAY_LOWER) {
		lb = le = 1;
	} else if (fs->opts.shortname & FAT_SN_DISPLAY_WIN95) {
		lb = le = 0;
	} else {
		lb = (ntres & FAT_CASE_LOWER_BASE) != 0;
		le = (ntres & FAT_CASE_LOWER_EXT) != 0;
	}
	while (bl && nm[bl - 1] == ' ')
		bl--;
	while (el && nm[8 + el - 1] == ' ')
		el--;
	if (bl == 0)
		return 0;
	for (unsigned i = 0; i < bl; i++) {
		uint8_t b = nm[i];
		uint16_t u;

		if (i == 0 && b == FAT_NAME_KANJI_E5)
			b = FAT_NAME_DELETED;
		u = fat_cp_to_uni(fs->cp, b);
		if (!u)
			u = (uint16_t)(0xF000 | b); /* no character: kept apart */
		out[o++] = lb ? fat_lowc(u) : u;
	}
	if (el) {
		out[o++] = '.';
		for (unsigned i = 0; i < el; i++) {
			uint16_t u = fat_cp_to_uni(fs->cp, nm[8 + i]);

			if (!u)
				u = (uint16_t)(0xF000 | nm[8 + i]);
			out[o++] = le ? fat_lowc(u) : u;
		}
	}
	return o;
}

/* Is the short name one of the dot entries ("." or "..")? */
int fat_sfn_is_dot(const uint8_t *nm)
{
	if (nm[0] != '.')
		return 0;
	for (unsigned i = 2; i < 11; i++)
		if (nm[i] != ' ')
			return 0;
	return nm[1] == ' ' || nm[1] == '.';
}

/* What one long-name character contributes to a short name (the
 * short-name rules: dots and spaces are dropped, + , ; = [ ] become "_",
 * characters the code page cannot hold become "_", letters become upper
 * case), and what it says about the name: `valid' (the character is kept as
 * it is), `lower' / `upper' (no letter of the other case met). */
struct fat_sn_info {
	int valid;
	int lower;
	int upper;
};

static unsigned fat_sn_char(fat_fs_t *fs, uint16_t u, uint8_t *out,
			    struct fat_sn_info *info)
{
	uint8_t b, prev;

	if (u == '.' || u == ' ') {
		info->valid = 0;
		return 0;
	}
	if (u == '+' || u == ',' || u == ';' || u == '=' || u == '[' ||
	    u == ']') {
		info->valid = 0;
		*out = '_';
		return 1;
	}
	b = fat_cp_from_uni(fs->cp, u);
	if (!b) {
		info->valid = 0;
		*out = '_';
		return 1;
	}
	prev = b;
	if (b >= 0x7F) {
		/* Not a plain letter: the name keeps long-name entries. */
		info->lower = 0;
		info->upper = 0;
	}
	b = fat_cp_toupper(fs->cp, b);
	if ((b >= 'A' && b <= 'Z')) {
		if (b == prev)
			info->lower = 0;
		else
			info->upper = 0;
	}
	*out = b;
	return 1;
}

/* A deterministic 32-bit hash of a long name (FNV-1a over its units): the
 * seed of the hexadecimal short-name tails. */
static uint32_t fat_name_hash(const uint16_t *u, unsigned n)
{
	uint32_t h = 2166136261u;

	for (unsigned i = 0; i < n; i++) {
		h ^= u[i] & 0xFF;
		h *= 16777619u;
		h ^= u[i] >> 8;
		h *= 16777619u;
	}
	return h;
}

/* Make the short name of long name `name' (`n' units, trailing dots
 * stripped) for directory `d' into ctx->sfn, with its case bits in *lcase.
 *
 *   1   the short entry alone holds the name (no long-name entries)
 *   0   long-name entries are needed; ctx->sfn is a unique alias
 *   <0  ST_INVALID (nothing of the name can be a short name),
 *       ST_EXISTS (the name is a short name the directory has),
 *       ST_NOSPC (no unique alias left), or a read error
 *
 * Metadata lock held. */
int fat_sfn_make(fat_fs_t *fs, fat_ctx_t *ctx, fat_stream_t *d,
			const uint16_t *name, unsigned n, uint8_t *lcase,
			int64_t exclude)
{
	struct fat_sn_info bi = { 1, 1, 1 }, ei = { 1, 1, 1 };
	uint8_t base[9], ext[4];
	unsigned baselen = 0, extlen = 0, sz, numtail_baselen = 6,
		 numtail2_baselen = 2;
	int ext_start = -1, is_short = 1, r;
	uint8_t *res = ctx->sfn;

	*lcase = 0;
	/* The extension: after the last dot -- unless only dots and spaces
	 * stand before it (".profile" is all base). */
	{
		int e;

		for (e = (int)n - 1; e >= 0; e--)
			if (name[e] == '.')
				break;
		if (e < 0 || e == (int)n - 1) {
			sz = n;
		} else {
			int k = 0;

			while (k < e && (name[k] == '.' || name[k] == ' '))
				k++;
			if (k == e) {
				sz = n;
			} else {
				sz = (unsigned)e;
				ext_start = e + 1;
			}
		}
	}
	for (unsigned i = 0; i < sz; i++) {
		uint8_t c;
		unsigned chl = fat_sn_char(fs, name[i], &c, &bi);

		if (chl == 0)
			continue;
		if (baselen < 2 && baselen + chl > 2)
			numtail2_baselen = baselen;
		if (baselen < 6 && baselen + chl > 6)
			numtail_baselen = baselen;
		base[baselen++] = c;
		if (baselen >= 8) {
			if (i + 1 < sz)
				is_short = 0;
			break;
		}
	}
	if (baselen == 0)
		return ST_INVALID;
	if (ext_start >= 0) {
		for (unsigned i = (unsigned)ext_start; extlen < 3 && i < n; i++) {
			uint8_t c;
			unsigned chl = fat_sn_char(fs, name[i], &c, &ei);

			if (chl == 0)
				continue;
			ext[extlen++] = c;
			if (extlen >= 3) {
				if (i + 1 != n)
					is_short = 0;
				break;
			}
		}
	}
	/* A leading 0xE5 would read as a deleted entry. */
	if (base[0] == FAT_NAME_DELETED)
		base[0] = FAT_NAME_KANJI_E5;
	mm_memset(res, ' ', 11);
	mm_memcpy(res, base, baselen);
	mm_memcpy(res + 8, ext, extlen);

	if (is_short && bi.valid && ei.valid) {
		mm_memcpy(ctx->sfn_cand[0], res, 11);
		r = fat_sfn_scan(fs, ctx, d, 1, exclude);
		if (r != ST_OK)
			return r;
		if (ctx->sfn_taken[0])
			return ST_EXISTS;
		if (fs->opts.shortname & FAT_SN_CREATE_WIN95)
			return bi.upper && ei.upper;
		/* winnt: one case per part, recorded in the case bits. */
		if ((bi.upper || bi.lower) && (ei.upper || ei.lower)) {
			if (!bi.upper && bi.lower)
				*lcase |= FAT_CASE_LOWER_BASE;
			if (!ei.upper && ei.lower)
				*lcase |= FAT_CASE_LOWER_EXT;
			return 1;
		}
		return 0;
	}

	/* An alias is needed.  nonumtail: the plain form when free; then
	 * "~1" .. "~9" after at most six characters (one scan for all). */
	{
		unsigned nc = 0, bl = baselen;

		if (fs->opts.nonumtail)
			mm_memcpy(ctx->sfn_cand[nc++], res, 11);
		if (bl > 6) {
			bl = numtail_baselen;
			res[7] = ' ';
		}
		res[bl] = '~';
		for (unsigned i = 1; i < 10; i++) {
			res[bl + 1] = (uint8_t)('0' + i);
			mm_memcpy(ctx->sfn_cand[nc++], res, 11);
		}
		r = fat_sfn_scan(fs, ctx, d, nc, exclude);
		if (r != ST_OK)
			return r;
		for (unsigned k = 0; k < nc; k++)
			if (!ctx->sfn_taken[k]) {
				mm_memcpy(res, ctx->sfn_cand[k], 11);
				return 0;
			}
		baselen = bl;
	}
	/* Then two characters, four hexadecimal digits and "~N": the digits
	 * from a hash of the name, stepping down by 11 (an odd step: every
	 * 16-bit value comes round), FAT_SFN_BATCH candidates per scan. */
	{
		static const char hex[] = "0123456789ABCDEF";
		uint32_t h = fat_name_hash(name, n);
		unsigned tail = (h >> 16) & 7;
		uint32_t i = h;

		if (baselen > 2) {
			baselen = numtail2_baselen;
			res[7] = ' ';
		}
		res[baselen + 4] = '~';
		res[baselen + 5] = (uint8_t)('1' + tail);
		for (unsigned round = 0; round < 65536 / FAT_SFN_BATCH + 1;
		     round++) {
			for (unsigned k = 0; k < FAT_SFN_BATCH; k++) {
				uint16_t v = (uint16_t)(i & 0xFFFF);

				res[baselen] = (uint8_t)hex[(v >> 12) & 0xF];
				res[baselen + 1] = (uint8_t)hex[(v >> 8) & 0xF];
				res[baselen + 2] = (uint8_t)hex[(v >> 4) & 0xF];
				res[baselen + 3] = (uint8_t)hex[v & 0xF];
				mm_memcpy(ctx->sfn_cand[k], res, 11);
				i -= 11;
			}
			r = fat_sfn_scan(fs, ctx, d, FAT_SFN_BATCH, exclude);
			if (r != ST_OK)
				return r;
			for (unsigned k = 0; k < FAT_SFN_BATCH; k++)
				if (!ctx->sfn_taken[k]) {
					mm_memcpy(res, ctx->sfn_cand[k], 11);
					return 0;
				}
		}
	}
	return ST_NOSPC;
}

void fat_dc_forget(fat_fs_t *fs, fat_stream_t *d, const char *key)
{
	if (fat_dcache_usable(key))
		dcache_invalidate_cs(&fs->sb, fat_dkey(d), key);
}

void fat_dc_remember(fat_fs_t *fs, fat_stream_t *d, const char *key,
			    const fat_dentry_info_t *di)
{
	if (fat_dcache_usable(key))
		dcache_insert_cs(&fs->sb, fat_dkey(d), key, di->index, di->size,
				 di->attr);
}

/* Forget every dcache key a set answers to: its long name and its short
 * name (both folded).  `lname' is the set's long name; `kbuf' scratch of
 * FAT_NAME_MAX * 3 + 8 bytes, `ubuf' of FAT_LNAME_UNITS units. */
void fat_dc_forget_set(fat_fs_t *fs, fat_stream_t *d,
			      const fat_dentry_info_t *di,
			      const uint16_t *lname, char *kbuf, uint16_t *ubuf)
{
	unsigned sl;

	if (di->lname_len) {
		fat_fold(lname, di->lname_len, ubuf);
		fat_make_key(ubuf, di->lname_len, kbuf, FAT_NAME_MAX * 3 + 8);
		fat_dc_forget(fs, d, kbuf);
	}
	sl = fat_sfn_display(fs, di->name83, di->ntres, ubuf);
	if (sl) {
		fat_fold(ubuf, sl, ubuf);
		fat_make_key(ubuf, sl, kbuf, FAT_NAME_MAX * 3 + 8);
		fat_dc_forget(fs, d, kbuf);
	}
}

/* Find the folded name ctx->up (`n' units, dcache key ctx->key) in
 * directory `d'.  ST_OK with *di (and ctx->name2 holding the stored long
 * name), ST_NOT_FOUND, or an error.  Metadata lock held. */
static int fat_dir_find(fat_fs_t *fs, fat_ctx_t *ctx, fat_stream_t *d,
			unsigned n, fat_dentry_info_t *di)
{
	fat_diter_t it;
	dc_result_t res;
	uint64_t idx = 0, slots = fat_dir_slots(d);
	int r = ST_NOT_FOUND;

	fat_assert_meta(fs);
	fat_diter_init(&it, fs, d);
	/* The position the last lookup of this name found: one set read
	 * instead of a scan -- verified, never trusted. */
	if (fat_dcache_usable(ctx->key)) {
		switch (dcache_lookup_cs(&fs->sb, fat_dkey(d), ctx->key, &res)) {
		case DC_LOOKUP_NEGATIVE:
			return ST_NOT_FOUND;
		case DC_LOOKUP_FOUND:
			r = fat_set_read(&it, res.ino, di, ctx->name2);
			if (r == ST_OK &&
			    fat_set_matches(fs, ctx, di, ctx->name2, n)) {
				fat_diter_done(&it);
				return ST_OK;
			}
			dcache_invalidate_cs(&fs->sb, fat_dkey(d), ctx->key);
			r = ST_NOT_FOUND;
			break;
		default:
			break;
		}
	}
	{
		int lfn_before = 0; /* the slot in front is a long-name entry */

		while (idx < slots) {
			const uint8_t *e = fat_diter_get(&it, idx);

			if (!e) {
				if (it.err) {
					r = it.err;
					goto out;
				}
				break;
			}
			if (e[0] == FAT_NAME_END)
				break;
			if (!fat_de_in_use(e)) {
				lfn_before = 0;
				idx++;
				continue;
			}
			if (fat_de_is_lfn(e)) {
				lfn_before = 1;
				idx++;
				continue;
			}
			if (!fat_de_is_object(e)) {
				lfn_before = 0;
				idx++;
				continue;
			}
			if (!lfn_before) {
				/* A short entry alone: compare its shown name
				 * straight from the page. */
				unsigned sl = fat_sfn_display(fs, e,
							      e[FAT_DE_NTRES],
							      ctx->sname);

				if (!sl || !fat_name_eq(ctx->sname, sl, ctx->up,
							n)) {
					idx++;
					continue;
				}
			}
			lfn_before = 0;
			{
				int pr = fat_set_read(&it, idx, di, ctx->name2);

				if (pr == ST_OK || pr == ST_IO) {
					if (fat_set_matches(fs, ctx, di,
							    ctx->name2, n)) {
						if (pr == ST_IO) {
							r = fat_corrupt(
								fs, idx,
								"entry %llu of a directory describes an impossible file",
								(unsigned long long)
									idx);
							goto out;
						}
						r = ST_OK;
						goto out;
					}
				} else if (pr != ST_NOT_FOUND) {
					r = pr;
					goto out;
				}
				/* fat_set_read moved the iterator: re-fetch
				 * the next slot normally. */
			}
			idx++;
		}
	}
	r = ST_NOT_FOUND;
out:
	fat_diter_done(&it);
	if (r == ST_OK)
		fat_dc_remember(fs, d, ctx->key, di);
	else if (r == ST_NOT_FOUND && fat_dcache_usable(ctx->key))
		dcache_insert_negative_cs(&fs->sb, fat_dkey(d), ctx->key);
	return r;
}

void fat_walk_release(struct fat_walk *w)
{
	if (w->st)
		fat_stream_put(w->st);
	if (w->dir)
		fat_stream_put(w->dir);
	w->st = NULL;
	w->dir = NULL;
}

/* Strip the leading slashes the VFS hands paths over with. */
static const char *fat_rel(const char *path)
{
	if (!path)
		return NULL;
	while (*path == '/')
		path++;
	return path;
}

/* Describe an existing stream as the entry it has (a walk ending in "..",
 * the object itself named by its parent). */
static void fat_di_from_stream(const fat_stream_t *st, fat_dentry_info_t *di)
{
	mm_memset(di, 0, sizeof(*di));
	di->index = st->entry_index;
	di->lfn_count = st->lfn_count;
	mm_memcpy(di->name83, st->name83, 11);
	di->attr = st->attr;
	di->ntres = st->ntres;
	di->ctime_cs = st->ctime_cs;
	di->ctime = st->ctime;
	di->cdate = st->cdate;
	di->adate = st->adate;
	di->mtime = st->mtime;
	di->mdate = st->mdate;
	di->first_cluster = st->first_cluster;
	di->size = st->is_dir ? 0 : (uint32_t)st->size;
	di->chksum = fat_lfn_checksum(st->name83);
}

/* Resolve `path' up to its last component: w->dir is that component's
 * directory (held) and, when it exists, w->di its entry; ctx->name/up/key
 * describe the last component.  The root itself gives is_root (w->dir NULL,
 * w->st the root, held).  "." and ".." are understood anywhere.  Metadata
 * lock held. */
int fat_walk(fat_fs_t *fs, fat_ctx_t *ctx, const char *path,
		    struct fat_walk *w)
{
	const char *p = fat_rel(path);
	fat_stream_t *cur;
	int r;

	mm_memset(w, 0, sizeof(*w));
	if (!p)
		return ST_INVALID;
	fat_stream_hold(fs->root);
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
				fat_stream_put(cur);
				return ST_NOT_FOUND;
			}
			fat_stream_hold(w->dir);
			w->st = cur;
			w->found = 1;
			fat_di_from_stream(cur, &w->di);
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
			fat_stream_put(cur);
			return ST_NAMETOOLONG;
		}
		if (len == 1 && s[0] == '.')
			continue;
		if (len == 2 && s[0] == '.' && s[1] == '.') {
			if (cur->parent) {
				fat_stream_t *up = cur->parent;

				fat_stream_hold(up);
				fat_stream_put(cur);
				cur = up;
			}
			continue;
		}
		if (!cur->is_dir) {
			fat_stream_put(cur);
			return ST_NOTDIR;
		}
		r = fat_name_prepare(fs, ctx, s, len, 0);
		if (r == ST_INVALID) {
			/* Not a name the volume can hold: nothing on it can
			 * be it.  A create of it is refused when it prepares
			 * the name for writing. */
			r = ST_NOT_FOUND;
			w->nlen = 0;
		} else if (r < 0) {
			fat_stream_put(cur);
			return r;
		} else {
			w->nlen = r;
			r = fat_dir_find(fs, ctx, cur, (unsigned)w->nlen, &w->di);
		}
		if (last) {
			if (r != ST_OK && r != ST_NOT_FOUND) {
				fat_stream_put(cur);
				return r;
			}
			w->dir = cur;
			w->found = (r == ST_OK);
			w->last = s;
			w->last_len = len;
			w->trailing_slash = (*p == '/');
			if (w->found && w->trailing_slash &&
			    !(w->di.attr & FAT_ATTR_DIR)) {
				fat_walk_release(w);
				return ST_NOTDIR;
			}
			return ST_OK;
		}
		if (r != ST_OK) {
			fat_stream_put(cur);
			return r;
		}
		if (!(w->di.attr & FAT_ATTR_DIR)) {
			fat_stream_put(cur);
			return ST_NOTDIR;
		}
		{
			int err;
			fat_stream_t *next = fat_stream_get(fs, cur, &w->di,
							    &err);

			fat_stream_put(cur);
			if (!next)
				return err;
			cur = next;
		}
	}
}

/* fat_walk(), and the object's stream in w->st (held) when it exists.
 * ST_NOT_FOUND when it does not. */
int fat_walk_obj(fat_fs_t *fs, fat_ctx_t *ctx, const char *path,
			struct fat_walk *w)
{
	int r = fat_walk(fs, ctx, path, w);
	int err;

	if (r != ST_OK)
		return r;
	if (w->is_root || w->st)
		return ST_OK;
	if (!w->found) {
		fat_walk_release(w);
		return ST_NOT_FOUND;
	}
	w->st = fat_stream_get(fs, w->dir, &w->di, &err);
	if (!w->st) {
		fat_walk_release(w);
		return err;
	}
	return ST_OK;
}

fat_ctx_t *fat_ctx_new(void)
{
	return (fat_ctx_t *)kalloc(sizeof(fat_ctx_t));
}

void fat_ctx_free(fat_ctx_t *ctx)
{
	if (ctx)
		kfree(ctx);
}

/* ===========================================================================
 * 16. Directory modification
 *
 * Entries are changed in the cached directory pages, under the metadata
 * lock exclusive, and the changed bytes are queued (the pages held, so they
 * cannot be reclaimed with the change in them).  The commit writes the
 * queued sectors -- after the FAT changes they depend on are on the device
 * (section 17).  Directory pages are never marked dirty in the page cache:
 * its background writeback must not decide this order.
 * =========================================================================== */

/* Queue bytes [lo, hi) of page `pidx' of directory `d' (held page `pg'). */
int fat_dirw_add(fat_fs_t *fs, fat_stream_t *d, unsigned long pidx,
			pc_page_t **pg, unsigned lo, unsigned hi)
{
	for (unsigned i = 0; i < fs->ndirw; i++) {
		struct fat_dirw *q = &fs->dirw[i];

		if (q->d == d && q->pidx == pidx) {
			if (lo < q->lo)
				q->lo = lo;
			if (hi > q->hi)
				q->hi = hi;
			return ST_OK;
		}
	}
	if (fs->ndirw >= FAT_DIRW_MAX)
		return ST_BUSY;
	fs->dirw[fs->ndirw].pg = *pg;
	fs->dirw[fs->ndirw].d = d;
	fs->dirw[fs->ndirw].pidx = pidx;
	fs->dirw[fs->ndirw].lo = lo;
	fs->dirw[fs->ndirw].hi = hi;
	fs->ndirw++;
	fat_stream_hold(d);
	*pg = NULL; /* the queue owns the hold now */
	return ST_OK;
}

/* ---- removing names ----------------------------------------------------------- */

/* Read the set of `st' in `dir' into *odi (its long name into ctx->name2,
 * its raw entries into ctx->set2) and check it is still the stream's.
 * Metadata lock held. */
static int fat_read_own_set(fat_fs_t *fs, fat_ctx_t *ctx, fat_stream_t *dir,
			    fat_stream_t *st, fat_dentry_info_t *odi)
{
	fat_diter_t it;
	int r;

	fat_diter_init(&it, fs, dir);
	r = fat_set_read(&it, st->entry_index, odi, ctx->name2);
	if (r == ST_OK || r == ST_IO) {
		int same = 1;

		for (unsigned i = 0; i < 11; i++)
			if (odi->name83[i] != st->name83[i])
				same = 0;
		if (!same)
			r = ST_NOT_FOUND;
		else
			r = fat_diter_copy(&it,
					   st->entry_index - odi->lfn_count,
					   odi->lfn_count + 1u, ctx->set2);
	}
	fat_diter_done(&it);
	if (r == ST_NOT_FOUND)
		return fat_corrupt(fs, st->entry_index,
				   "entry of a file vanished (slot %u)",
				   st->entry_index);
	return r == ST_EOF ? ST_IO : r;
}

/* Remove the entry set of `st' (found through `w') and release or orphan
 * the stream.  Stream lock and metadata lock exclusive, writes allowed. */
static int fat_remove_locked(fat_fs_t *fs, fat_ctx_t *ctx, struct fat_walk *w,
			     fat_stream_t *st)
{
	fat_stream_t *dir = w->dir;
	fat_dentry_info_t odi;
	int r;

	fat_assert_excl(fs);
	r = fat_read_own_set(fs, ctx, dir, st, &odi);
	if (r != ST_OK)
		return r;
	/* The long-name entries go with (and before) the short entry. */
	r = fat_dir_clear(fs, dir, st->entry_index - odi.lfn_count,
			  odi.lfn_count + 1u, ctx->set3);
	if (r != ST_OK)
		return r;
	fat_dc_forget_set(fs, dir, &odi, ctx->name2, ctx->tmpk, ctx->tmpu);
	fat_dc_forget(fs, dir, ctx->key);
	if (st->is_dir) {
		dcache_invalidate_dir(&fs->sb, fat_dkey(st));
		if (dir->subdirs > 0)
			dir->subdirs--;
	}
	fat_touch_mtime(fs, dir);
	fat_stream_mark_dirty(fs, dir);
	/* The name is gone; the clusters go now unless a handle still uses
	 * them (then at its last close) -- or never, when the chain is
	 * corrupt (lost clusters: a corrupt chain may lead into other
	 * files). */
	fat_stream_orphan(fs, st);
	if (__atomic_load_n(&st->opens, __ATOMIC_SEQ_CST) == 0) {
		pagecache_invalidate_file(&fs->sb, fat_chain_of(st));
		st->data_dirty = 0;
		r = st->chain_bad ? ST_OK :
				    fat_free_extents(fs, st->ext, st->next);
		st->nclusters = 0;
		st->next = 0;
		st->first_cluster = 0;
		if (r != ST_OK)
			return r;
	}
	return ST_OK;
}

static int fat_unlink_rmdir(vfs_mount_t *m, const char *path, int dir)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	fat_stream_t *st;
	struct fat_walk w;
	int r, bad = 0;

	if (!fs || !path)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	if (!fat_fence(fs, ctx, path, &st, !dir)) {
		/* Not there (or keeps changing): report what a plain lookup
		 * says. */
		fat_meta_rlock(fs);
		r = fat_walk_obj(fs, ctx, path, &w);
		if (r == ST_OK) {
			fat_walk_release(&w);
			r = ST_BUSY;
		}
		fat_meta_runlock(fs);
		fat_ctx_free(ctx);
		return r;
	}
	r = fat_walk(fs, ctx, path, &w);
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
	} else if (st->chain_bad && st->is_dir) {
		r = fat_corrupt(fs, st->entry_index,
				"directory with a corrupt cluster chain");
	} else if ((r = fat_writable(fs)) == ST_OK) {
		if (dir) {
			int e = fat_dir_empty(fs, st);

			r = e < 0 ? e : (e ? ST_OK : ST_NOTEMPTY);
		}
		if (r == ST_OK)
			r = fat_may_write(fs);
		if (r == ST_OK) {
			bad = st->chain_bad;
			r = fat_remove_locked(fs, ctx, &w, st);
		}
		if (r == ST_OK)
			r = fat_commit(fs);
		if (r == ST_OK && bad) {
			/* The name is gone (and stored); the clusters are kept
			 * as lost ones.  Now the error policy. */
			WARN_RATELIMIT(1, "fat: %s: removed a file whose cluster chain is corrupt; its clusters are kept",
				       fat_name_of(fs));
			fat_fs_error(fs, "unlinked a file with a corrupt cluster chain",
				     st->entry_index);
		}
	}
	fat_walk_release(&w);
out:
	fat_unfence(fs, st);
	fat_ctx_free(ctx);
	return r;
}

int fat_unlink_op(vfs_mount_t *m, const char *path)
{
	return fat_unlink_rmdir(m, path, 0);
}

int fat_rmdir_op(vfs_mount_t *m, const char *path)
{
	return fat_unlink_rmdir(m, path, 1);
}

/* ---- mkdir ------------------------------------------------------------------- */

/* The "." / ".." entry of a new directory. */
static void fat_dot_entry(fat_fs_t *fs, uint8_t *e, int dotdot, uint32_t cl,
			  const uint8_t *proto)
{
	mm_memcpy(e, proto, FAT_DENTRY_SIZE);
	mm_memset(e, ' ', 11);
	e[0] = '.';
	if (dotdot)
		e[1] = '.';
	e[FAT_DE_ATTR] = FAT_ATTR_DIR;
	e[FAT_DE_NTRES] = 0;
	fat_de_set_cluster(fs, e, cl);
	fat_put32(e + FAT_DE_SIZE, 0);
}

/* What ".." of a directory inside `parent' names: the parent's first
 * cluster, 0 for the root (FAT32 included). */
static inline uint32_t fat_dotdot_cluster(const fat_stream_t *parent)
{
	return parent->is_root ? 0 : parent->first_cluster;
}

int fat_mkdir_op(vfs_mount_t *m, const char *path, unsigned int mode)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct fat_walk w;
	struct fat_run *runs = NULL;
	unsigned nruns = 0;
	fat_dentry_info_t di;
	uint8_t proto[FAT_DENTRY_SIZE];
	uint8_t *sec = NULL;
	int r, n;

	(void)mode;
	if (!fs || !path)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	fat_meta_wlock(fs);
	r = fat_walk(fs, ctx, path, &w);
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
	r = fat_writable(fs);
	if (r != ST_OK)
		goto out;
	n = fat_name_prepare(fs, ctx, w.last, w.last_len, 1);
	if (n < 0) {
		r = n;
		goto out;
	}
	r = fat_may_write(fs);
	if (r != ST_OK)
		goto out;
	/* The new directory's cluster: zeroed, "." and ".." in its first
	 * sector, all on the device before anything names it. */
	r = fat_alloc(fs, 0, 1, &runs, &nruns);
	if (r != ST_OK)
		goto out;
	fat_proto_new(fs, proto, FAT_ATTR_DIR, runs[0].start, 0);
	sec = (uint8_t *)kcalloc(1, fs->dev_ss);
	if (!sec) {
		r = ST_NOMEM;
	} else {
		r = fat_dev_zero(fs, fat_cl_lba(fs, runs[0].start),
				 1ULL << (fs->cs_shift - fs->dev_shift));
		fat_dot_entry(fs, sec, 0, runs[0].start, proto);
		fat_dot_entry(fs, sec + FAT_DENTRY_SIZE, 1,
			      fat_dotdot_cluster(w.dir), proto);
		if (r == ST_OK)
			r = fat_dev_write(fs, fat_cl_lba(fs, runs[0].start), 1,
					  sec, FAT_W_DATA);
	}
	if (r == ST_OK)
		r = fat_place_set(fs, ctx, &w, (unsigned)n, proto, &di);
	if (r != ST_OK) {
		/* Nothing references the cluster: straight back. */
		fat_unmark_run(fs, runs[0].start, 1);
		fat_free_count_add(fs, 1);
		goto out;
	}
	if (w.dir->subdirs >= 0)
		w.dir->subdirs++;
	fat_touch_mtime(fs, w.dir);
	fat_stream_mark_dirty(fs, w.dir);
	r = fat_commit(fs);
out:
	fat_walk_release(&w);
out_unlock:
	fat_meta_wunlock(fs);
	if (sec)
		kfree(sec);
	if (runs)
		kfree(runs);
	fat_ctx_free(ctx);
	return r;
}

/* ---- rename ------------------------------------------------------------------ */

/* Is `anc' the directory `d' or one of its ancestors? */
static int fat_is_ancestor(const fat_stream_t *anc, const fat_stream_t *d)
{
	for (unsigned depth = 0; d && depth < 4096; depth++, d = d->parent)
		if (d == anc)
			return 1;
	return 0;
}

/* Point ".." of directory `d' at `parent' (a directory moved to another
 * parent).  Metadata lock exclusive, writes allowed. */
static int fat_update_dotdot(fat_fs_t *fs, fat_stream_t *d,
			     fat_stream_t *parent, uint8_t *scratch)
{
	fat_diter_t it;
	int r;

	fat_diter_init(&it, fs, d);
	r = fat_diter_copy(&it, 1, 1, scratch);
	fat_diter_done(&it);
	if (r != ST_OK)
		return r == ST_EOF ? ST_IO : r;
	if (scratch[0] != '.' || scratch[1] != '.' || scratch[2] != ' ' ||
	    !(scratch[FAT_DE_ATTR] & FAT_ATTR_DIR))
		return fat_corrupt(fs, d->first_cluster,
				   "directory at cluster %u has no \"..\" entry",
				   d->first_cluster);
	if (fat_de_cluster(fs, scratch) == fat_dotdot_cluster(parent))
		return ST_OK;
	fat_de_set_cluster(fs, scratch, fat_dotdot_cluster(parent));
	return fat_dir_write(fs, d, 1, 1, scratch);
}

/* Is the name of the set (`odi', long name `lname') exactly ctx->name (`n'
 * units)? */
static int fat_set_named_exactly(fat_fs_t *fs, fat_ctx_t *ctx,
				 const fat_dentry_info_t *odi,
				 const uint16_t *lname, unsigned n)
{
	unsigned l = fat_set_name(fs, odi, lname, ctx->tmpu);

	if (l != n)
		return 0;
	for (unsigned i = 0; i < n; i++)
		if (ctx->tmpu[i] != ctx->name[i])
			return 0;
	return 1;
}

static int fat_rename_locked(fat_fs_t *fs, fat_ctx_t *ctx, const char *oldpath,
			     const char *newpath)
{
	struct fat_walk wo, wn;
	fat_stream_t *src = NULL, *victim = NULL;
	fat_dentry_info_t odi;
	unsigned old_count, new_count, n;
	uint64_t old_start, new_idx;
	uint8_t lcase;
	int r, nn, need_lfn, same_dir, moved_dotdot = 0;

	fat_assert_excl(fs);
	mm_memset(&wn, 0, sizeof(wn));
	r = fat_walk(fs, ctx, oldpath, &wo);
	if (r != ST_OK)
		return r;
	if (wo.is_root) {
		fat_walk_release(&wo);
		return ST_BUSY;
	}
	if (!wo.found) {
		fat_walk_release(&wo);
		return ST_NOT_FOUND;
	}
	if (!wo.last) {
		fat_walk_release(&wo);
		return ST_INVALID;
	}
	r = fat_writable(fs);
	if (r == ST_OK)
		r = fat_may_write(fs);
	if (r != ST_OK) {
		fat_walk_release(&wo);
		return r;
	}
	/* A file keeps its clusters as they are: a corrupt chain does not
	 * stop its rename.  A directory's own entries ("..") must be
	 * readable. */
	src = fat_stream_get_ex(fs, wo.dir, &wo.di, 1, &r);
	if (!src) {
		fat_walk_release(&wo);
		return r;
	}
	if (src->is_dir && src->chain_bad) {
		r = fat_corrupt(fs, src->entry_index,
				"directory with a corrupt cluster chain");
		goto out;
	}
	/* Settle every pending entry update first: the source's set is about
	 * to move, and the copy taken below must be current. */
	r = fat_commit(fs);
	if (r != ST_OK)
		goto out;

	r = fat_walk(fs, ctx, newpath, &wn);
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
	nn = fat_name_prepare(fs, ctx, wn.last, wn.last_len, 1);
	if (nn < 0) {
		r = nn;
		goto out;
	}
	n = (unsigned)nn;
	/* A directory cannot go below itself. */
	if (src->is_dir && fat_is_ancestor(src, wn.dir)) {
		r = ST_INVALID;
		goto out;
	}
	same_dir = (wn.dir == wo.dir);
	if (wn.found && same_dir && wn.di.index == src->entry_index) {
		/* The same file under another spelling (case, or its short
		 * name): nothing to do when the stored name is the new one
		 * exactly; else it is renamed in place below. */
		r = fat_read_own_set(fs, ctx, wo.dir, src, &odi);
		if (r != ST_OK)
			goto out;
		if (fat_set_named_exactly(fs, ctx, &odi, ctx->name2, n)) {
			r = ST_OK;
			goto out;
		}
	} else if (wn.found) {
		victim = fat_stream_get(fs, wn.dir, &wn.di, &r);
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
			int e = fat_dir_empty(fs, victim);

			if (e <= 0) {
				r = e < 0 ? e : ST_NOTEMPTY;
				goto out;
			}
		}
		/* The victim's name goes first (its clusters are freed once
		 * the new set is stored). */
		r = fat_remove_locked(fs, ctx, &wn, victim);
		if (r != ST_OK)
			goto out;
	}
	/* The source set, fresh (ctx->name2 / ctx->set2), and the new one
	 * built from its short entry (times, attributes, cluster, size). */
	r = fat_read_own_set(fs, ctx, wo.dir, src, &odi);
	if (r != ST_OK)
		goto out;
	old_count = odi.lfn_count + 1u;
	old_start = src->entry_index - odi.lfn_count;
	r = fat_sfn_make(fs, ctx, wn.dir, ctx->name, n, &lcase,
			 same_dir ? (int64_t)src->entry_index : -1);
	if (r < 0)
		goto out;
	need_lfn = (r == 0);
	new_count = fat_set_build(ctx->set, ctx->name, n, need_lfn, ctx->sfn,
				  lcase,
				  ctx->set2 + (old_count - 1) * FAT_DENTRY_SIZE);

	if (same_dir && new_count <= old_count &&
	    fat_slot_fits(fs, wo.dir, old_start, old_count)) {
		/* In place: the new set at the end of the old one's slots
		 * (the short entry keeps its slot), the spare ones in front
		 * marked free -- one sector write. */
		uint8_t *buf = ctx->set3;
		unsigned spare = old_count - new_count;

		mm_memcpy(buf, ctx->set2, old_count * FAT_DENTRY_SIZE);
		for (unsigned i = 0; i < spare; i++)
			buf[i * FAT_DENTRY_SIZE] = FAT_NAME_DELETED;
		mm_memcpy(buf + spare * FAT_DENTRY_SIZE, ctx->set,
			  new_count * FAT_DENTRY_SIZE);
		r = fat_dir_write(fs, wo.dir, old_start, old_count, buf);
		if (r != ST_OK)
			goto out;
		new_idx = src->entry_index;
	} else {
		uint64_t start;

		/* Moved: the old set is cleared and STORED first, then the
		 * new one written -- a stop in between loses the file's
		 * clusters (fsck reclaims them) but never leaves two sets
		 * naming the same clusters.  A directory that changes parent
		 * gets its new ".." in between, while nothing names it: a
		 * stop then leaves lost clusters, never a ".." that points
		 * at the wrong parent. */
		r = fat_dir_clear(fs, wo.dir, old_start, old_count, ctx->set3);
		if (r == ST_OK)
			r = fat_commit(fs);
		if (r != ST_OK)
			goto out;
		if (src->is_dir && !same_dir) {
			r = fat_update_dotdot(fs, src, wn.dir, ctx->set3);
			if (r == ST_OK)
				r = fat_commit(fs);
			moved_dotdot = (r == ST_OK);
		}
		if (r == ST_OK)
			r = fat_dir_alloc_slots(fs, wn.dir, new_count, &start);
		if (r == ST_OK)
			r = fat_dir_write(fs, wn.dir, start, new_count, ctx->set);
		if (r != ST_OK) {
			/* The file has no name now: put the old set (and the
			 * old "..") back where it was. */
			if (moved_dotdot)
				(void)fat_update_dotdot(fs, src, wo.dir, ctx->set3);
			if (fat_dir_write(fs, wo.dir, old_start, old_count,
					  ctx->set2) == ST_OK)
				fat_commit(fs);
			goto out;
		}
		new_idx = start + new_count - 1;
	}
	/* Every name either set answered to is new in the dcache. */
	fat_dc_forget_set(fs, wo.dir, &odi, ctx->name2, ctx->tmpk, ctx->tmpu);
	{
		fat_dentry_info_t ndi;

		fat_di_from_sfn(fs, ctx->set + (new_count - 1) * FAT_DENTRY_SIZE,
				(uint32_t)new_idx, &ndi);
		ndi.lfn_count = (uint8_t)(new_count - 1);
		ndi.lname_len = need_lfn ? (uint16_t)n : 0;
		fat_dc_forget_set(fs, wn.dir, &ndi, ctx->name, ctx->tmpk,
				  ctx->tmpu);
	}
	fat_dc_forget(fs, wn.dir, ctx->key);
	if (src->is_dir && !same_dir) {
		if (wo.dir->subdirs > 0)
			wo.dir->subdirs--;
		if (wn.dir->subdirs >= 0)
			wn.dir->subdirs++;
	}
	mm_memcpy(src->name83, ctx->sfn, 11);
	src->ntres = ctx->set[(new_count - 1) * FAT_DENTRY_SIZE + FAT_DE_NTRES];
	fat_stream_rekey(fs, src, wn.dir, (uint32_t)new_idx,
			 (uint8_t)(new_count - 1));
	fat_touch_mtime(fs, wo.dir);
	fat_stream_mark_dirty(fs, wo.dir);
	if (!same_dir) {
		fat_touch_mtime(fs, wn.dir);
		fat_stream_mark_dirty(fs, wn.dir);
	}
	r = fat_commit(fs);
out:
	if (victim)
		fat_stream_put(victim);
	if (src)
		fat_stream_put(src);
	fat_walk_release(&wn);
	fat_walk_release(&wo);
	return r;
}

int fat_rename_op(vfs_mount_t *m, const char *oldpath,
			 const char *newpath)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	fat_stream_t *dst;
	int r;

	if (!fs || !oldpath || !newpath)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	/* An existing destination has its data freed: fence it. */
	if (fat_fence(fs, ctx, newpath, &dst, 0)) {
		r = fat_rename_locked(fs, ctx, oldpath, newpath);
		fat_unfence(fs, dst);
	} else {
		fat_meta_wlock(fs);
		r = fat_rename_locked(fs, ctx, oldpath, newpath);
		fat_meta_wunlock(fs);
	}
	fat_ctx_free(ctx);
	return r;
}

/* ---- chdir, attributes, owners, times ------------------------------------ */

int fat_chdir_op(vfs_mount_t *m, const char *path)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct fat_walk w;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	fat_meta_rlock(fs);
	r = fat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		if (!w.st->is_dir)
			r = ST_NOTDIR;
		fat_walk_release(&w);
	}
	fat_meta_runlock(fs);
	fat_ctx_free(ctx);
	return r;
}
