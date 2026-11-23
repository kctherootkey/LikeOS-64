/* LikeOS - FAT filesystem driver: extended attributes
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* ---- the attribute byte as an extended attribute --------------------------- */

#define FAT_XATTR_ATTRIB "system.fat_attrib"
#define FAT_XATTR_CREATE 1

/* getxattr of a stream: "system.fat_attrib" as a 32-bit little-endian
 * value.  Metadata lock held. */
static int fat_xattr_get(fat_fs_t *fs, fat_stream_t *st, const char *name,
			 void *val, unsigned long size)
{
	uint8_t v[4];

	(void)fs;
	if (!name || !fat_streq(name, FAT_XATTR_ATTRIB))
		return ST_NODATA;
	v[0] = st->is_root ? FAT_ATTR_DIR : st->attr;
	v[1] = v[2] = v[3] = 0;
	if (!val || size == 0)
		return 4;
	if (size < 4)
		return ST_RANGE;
	mm_memcpy(val, v, 4);
	return 4;
}

/* setxattr of a stream: read-only, hidden, system and archive may change;
 * the directory bit must stay what it is, the volume bit 0.  `val' is a
 * kernel copy.  Metadata lock exclusive. */
static int fat_xattr_set(fat_fs_t *fs, fat_stream_t *st, const char *name,
			 const uint8_t *val, unsigned long size, int flags)
{
	uint32_t v = 0;

	if (!name || !fat_streq(name, FAT_XATTR_ATTRIB))
		return ST_UNSUPPORTED;
	if (flags & FAT_XATTR_CREATE)
		return ST_EXISTS; /* it always exists */
	if (!val || size < 1 || size > 4)
		return ST_INVALID;
	for (unsigned long i = 0; i < size; i++)
		v |= (uint32_t)val[i] << (8 * i);
	if (st->is_root)
		return v == FAT_ATTR_DIR ? ST_OK : ST_PERM;
	if (v & ~(uint32_t)(FAT_ATTR_CHANGEABLE | FAT_ATTR_DIR))
		return ST_INVALID;
	if (!!(v & FAT_ATTR_DIR) != !!st->is_dir)
		return ST_INVALID;
	return fat_set_attr(fs, st,
			    (uint8_t)((st->attr & ~FAT_ATTR_CHANGEABLE) |
				      (v & FAT_ATTR_CHANGEABLE)));
}

/* Copy a value from the caller (user memory) before any lock is taken. */
static uint8_t *fat_xattr_copyin(const void *val, unsigned long size, int *err)
{
	uint8_t *k;

	*err = ST_OK;
	if (size > 4) {
		*err = ST_INVALID;
		return NULL;
	}
	k = (uint8_t *)kalloc(4);
	if (!k) {
		*err = ST_NOMEM;
		return NULL;
	}
	if (val && size) {
		smap_disable();
		mm_memcpy(k, val, size);
		smap_enable();
	}
	return k;
}

int fat_getxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			   const char *name, void *val, unsigned long size)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct fat_walk w;
	uint8_t v[4];
	int r;

	(void)nofollow;
	if (!fs || !path)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	fat_meta_rlock(fs);
	r = fat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		r = fat_xattr_get(fs, w.st, name, v, sizeof(v));
		fat_walk_release(&w);
	}
	fat_meta_runlock(fs);
	fat_ctx_free(ctx);
	if (r < 0)
		return r;
	if (!val || size == 0)
		return r;
	if (size < (unsigned long)r)
		return ST_RANGE;
	/* No lock held while user memory is written. */
	smap_disable();
	mm_memcpy(val, v, (size_t)r);
	smap_enable();
	return r;
}

int fat_setxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			   const char *name, const void *val, unsigned long size,
			   int flags)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct fat_walk w;
	uint8_t *k;
	int r;

	(void)nofollow;
	if (!fs || !path)
		return ST_INVALID;
	if (!name || !fat_streq(name, FAT_XATTR_ATTRIB))
		return ST_UNSUPPORTED;
	k = fat_xattr_copyin(val, size, &r);
	if (!k)
		return r;
	ctx = fat_ctx_new();
	if (!ctx) {
		kfree(k);
		return ST_NOMEM;
	}
	fat_meta_wlock(fs);
	r = fat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		r = fat_xattr_set(fs, w.st, name, k, size, flags);
		fat_walk_release(&w);
	}
	fat_meta_wunlock(fs);
	fat_ctx_free(ctx);
	kfree(k);
	return r;
}

int fat_listxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			    char *list, unsigned long size)
{
	(void)m;
	(void)path;
	(void)nofollow;
	(void)list;
	(void)size;
	return 0; /* the attribute byte is not listed (fat32.h) */
}

int fat_removexattr_op(vfs_mount_t *m, const char *path, int nofollow,
			      const char *name)
{
	(void)m;
	(void)path;
	(void)nofollow;
	if (name && fat_streq(name, FAT_XATTR_ATTRIB))
		return ST_PERM; /* every entry has an attribute byte */
	return ST_NODATA;
}

int fat_fgetxattr_op(vfs_file_t *f, const char *name, void *val,
			    unsigned long size)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	uint8_t v[4];
	int r;

	if (!h)
		return ST_INVALID;
	fat_meta_rlock(h->fs);
	r = fat_xattr_get(h->fs, h->st, name, v, sizeof(v));
	fat_meta_runlock(h->fs);
	if (r < 0 || !val || size == 0)
		return r;
	if (size < (unsigned long)r)
		return ST_RANGE;
	smap_disable();
	mm_memcpy(val, v, (size_t)r);
	smap_enable();
	return r;
}

int fat_fsetxattr_op(vfs_file_t *f, const char *name, const void *val,
			    unsigned long size, int flags)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	uint8_t *k;
	int r;

	if (!h)
		return ST_INVALID;
	if (!name || !fat_streq(name, FAT_XATTR_ATTRIB))
		return ST_UNSUPPORTED;
	k = fat_xattr_copyin(val, size, &r);
	if (!k)
		return r;
	fat_meta_wlock(h->fs);
	r = fat_xattr_set(h->fs, h->st, name, k, size, flags);
	fat_meta_wunlock(h->fs);
	kfree(k);
	return r;
}

int fat_flistxattr_op(vfs_file_t *f, char *list, unsigned long size)
{
	(void)f;
	(void)list;
	(void)size;
	return 0;
}

int fat_fremovexattr_op(vfs_file_t *f, const char *name)
{
	(void)f;
	if (name && fat_streq(name, FAT_XATTR_ATTRIB))
		return ST_PERM;
	return ST_NODATA;
}
