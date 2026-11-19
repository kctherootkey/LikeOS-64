/* libinstaller -- mounting the new system.
 *
 * The root file system goes to the target directory (/mnt/likeos), the EFI
 * system partition to <target>/boot/efi.  Every mount this context makes is
 * recorded and undone in reverse order -- at the end of the installation
 * and after a failure alike.  Leftovers of an earlier attempt below the
 * target are found in /proc/mounts and unmounted first.
 *
 * In simulate mode nothing is mounted: the target is a plain directory and
 * the EFI files go to <target>/boot/efi as a directory.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "il_internal.h"

#define IL_UMOUNT_TRIES 10

/* /proc/mounts writes blanks and the like as \ooo. */
static void il_unescape(char *s)
{
	char *w = s;

	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' &&
		    s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			*w++ = (char)((s[1] - '0') << 6 | (s[2] - '0') << 3 |
				      (s[3] - '0'));
			s += 4;
		} else {
			*w++ = *s++;
		}
	}
	*w = '\0';
}

struct il_mnt {
	char dev[256];
	char dir[IL_PATH_MAX];
	char type[32];
};

/* The system's mounts; *n gets the count.  NULL when /proc/mounts cannot
 * be read. */
static struct il_mnt *il_read_mounts(il_ctx *ctx, int *n)
{
	char path[IL_PATH_MAX];
	char *text = NULL, *save = NULL;
	struct il_mnt *m = NULL;
	int cap = 0;

	*n = 0;
	il_sys_path(ctx, path, sizeof(path), "/proc/mounts");
	if (il_read_file_alloc(path, &text, NULL) != 0)
		return NULL;
	for (char *line = strtok_r(text, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *f[3], *tok, *s2 = NULL;
		int k = 0;

		for (tok = strtok_r(line, " \t", &s2); tok && k < 3;
		     tok = strtok_r(NULL, " \t", &s2))
			f[k++] = tok;
		if (k < 3)
			continue;
		if (*n >= cap) {
			struct il_mnt *nm;

			cap = cap ? cap * 2 : 16;
			nm = realloc(m, (size_t)cap * sizeof(*m));
			if (!nm)
				break;
			m = nm;
		}
		il_unescape(f[0]);
		il_unescape(f[1]);
		il_strlcpy(m[*n].dev, f[0], sizeof(m[*n].dev));
		il_strlcpy(m[*n].dir, f[1], sizeof(m[*n].dir));
		il_strlcpy(m[*n].type, f[2], sizeof(m[*n].type));
		(*n)++;
	}
	free(text);
	return m;
}

/* The node a /dev path names (links in /dev/disk/by-* resolved). */
static void il_canon_dev(il_ctx *ctx, const char *dev, char *out, size_t len)
{
	char path[IL_PATH_MAX], real[IL_PATH_MAX];
	size_t sl = strlen(ctx->sysroot);

	il_strlcpy(out, dev, len);
	if (!il_starts_with(dev, "/dev/"))
		return;
	il_sys_path(ctx, path, sizeof(path), dev);
	if (!realpath(path, real))
		return;
	if (sl && strncmp(real, ctx->sysroot, sl) == 0)
		il_strlcpy(out, real + sl, len);
	else
		il_strlcpy(out, real, len);
}

int il_is_mounted(il_ctx *ctx, const char *dev_or_dir)
{
	struct il_mnt *m;
	char want[IL_PATH_MAX];
	int n, found = 0;

	if (!dev_or_dir || !dev_or_dir[0])
		return 0;
	m = il_read_mounts(ctx, &n);
	if (!m)
		return 0;
	il_canon_dev(ctx, dev_or_dir, want, sizeof(want));
	for (int i = 0; i < n && !found; i++) {
		char have[IL_PATH_MAX];

		if (strcmp(m[i].dir, dev_or_dir) == 0) {
			found = 1;
			break;
		}
		il_canon_dev(ctx, m[i].dev, have, sizeof(have));
		if (strcmp(have, want) == 0 || strcmp(m[i].dev, dev_or_dir) == 0)
			found = 1;
	}
	free(m);
	return found;
}

static int il_umount_one(il_ctx *ctx, const char *dir)
{
	for (int i = 0; i < IL_UMOUNT_TRIES; i++) {
		if (il_sys_umount2(dir, 0) == 0) {
			il_log(ctx, IL_LOG_INFO, "unmounted %s", dir);
			return 0;
		}
		if (errno == EINVAL || errno == ENOENT) {
			/* Not (or no longer) a mount point. */
			il_log(ctx, IL_LOG_DEBUG, "%s: not mounted", dir);
			return 0;
		}
		if (errno == ENOSYS)
			return il_set_error(ctx, ENOSYS, "%s cannot be unmounted: "
					    "this kernel has no umount2", dir);
		if (errno != EBUSY)
			return il_set_error(ctx, errno, "%s cannot be unmounted: "
					    "%s", dir, strerror(errno));
		/* Busy: what is still being written out holds it. */
		sync();
		il_sleep_ms(200);
	}
	il_log(ctx, IL_LOG_WARN, "%s stays busy; detaching it", dir);
	if (il_sys_umount2(dir, MNT_DETACH) == 0)
		return 0;
	return il_set_error(ctx, errno, "%s cannot be unmounted: %s", dir,
			    strerror(errno));
}

static int il_len_desc(const void *a, const void *b)
{
	const struct il_mnt *x = a, *y = b;
	size_t lx = strlen(x->dir), ly = strlen(y->dir);

	return lx < ly ? 1 : lx > ly ? -1 : 0;
}

int il_umount_below(il_ctx *ctx, const char *dir)
{
	struct il_mnt *m;
	size_t dl = strlen(dir);
	int n, w = 0, rc = 0;

	if (ctx->simulate || !dir[0] || strcmp(dir, "/") == 0)
		return 0;
	m = il_read_mounts(ctx, &n);
	if (!m)
		return 0;
	for (int i = 0; i < n; i++)
		if (strncmp(m[i].dir, dir, dl) == 0 &&
		    (m[i].dir[dl] == '\0' || m[i].dir[dl] == '/'))
			m[w++] = m[i];
	qsort(m, (size_t)w, sizeof(*m), il_len_desc);
	for (int i = 0; i < w; i++) {
		il_log(ctx, IL_LOG_INFO, "%s is still mounted from an earlier "
		       "attempt (%s)", m[i].dir, m[i].dev);
		rc = il_umount_one(ctx, m[i].dir);
		if (rc)
			break;
	}
	free(m);
	return rc;
}

static int il_record(il_ctx *ctx, const char *target, const char *source,
		     const char *fstype, int simulated)
{
	struct il_mount_rec *r;

	if (ctx->nmounts >= IL_MAX_MOUNTS)
		return il_set_error(ctx, ENOSPC, "internal: too many mounts");
	r = &ctx->mounts[ctx->nmounts++];
	memset(r, 0, sizeof(*r));
	il_strlcpy(r->target, target, sizeof(r->target));
	il_strlcpy(r->source, source, sizeof(r->source));
	il_strlcpy(r->fstype, fstype, sizeof(r->fstype));
	r->simulated = simulated;
	return 0;
}

static int il_do_mount(il_ctx *ctx, const char *src, const char *dir,
		       const char *fstype, const char *opts)
{
	if (il_sys_mount(src, dir, fstype, 0, opts) == 0)
		return 0;
	/* Options the kernel does not know: without them. */
	if (errno == EINVAL && opts && opts[0] &&
	    il_sys_mount(src, dir, fstype, 0, NULL) == 0) {
		il_log(ctx, IL_LOG_INFO, "%s mounted without the options %s",
		       dir, opts);
		return 0;
	}
	if (errno == ENOSYS)
		return il_set_error(ctx, ENOSYS, "This LikeOS kernel cannot "
				    "mount file systems; the installation needs "
				    "a newer one");
	return il_set_error(ctx, errno, "%s cannot be mounted on %s: %s", src,
			    dir, strerror(errno));
}

int il_mount_target(il_ctx *ctx, struct il_target *t)
{
	int rc;

	il_strlcpy(t->root_dir, ctx->target, sizeof(t->root_dir));
	if (il_path_join(t->esp_dir, sizeof(t->esp_dir), t->root_dir,
			 "boot/efi"))
		return il_set_error(ctx, ENAMETOOLONG, "%s: path too long",
				    t->root_dir);
	rc = il_mkdir_p(t->root_dir, 0755);
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be created: %s",
				    t->root_dir, strerror(-rc));
	if (ctx->simulate) {
		rc = il_mkdir_p(t->esp_dir, 0700);
		if (rc)
			return il_set_error(ctx, -rc, "%s cannot be created: %s",
					    t->esp_dir, strerror(-rc));
		il_record(ctx, t->root_dir, t->root_dev, "ext4", 1);
		il_record(ctx, t->esp_dir, t->esp_dev, "vfat", 1);
		il_log(ctx, IL_LOG_INFO, "simulation: %s stands for the new root, "
		       "%s for the EFI system partition", t->root_dir,
		       t->esp_dir);
		return 0;
	}
	rc = il_umount_below(ctx, t->root_dir);
	if (rc)
		return rc;
	rc = il_do_mount(ctx, t->root_dev, t->root_dir, "ext4", NULL);
	if (rc)
		return rc;
	il_record(ctx, t->root_dir, t->root_dev, "ext4", 0);
	il_log(ctx, IL_LOG_INFO, "mounted %s on %s (ext4)", t->root_dev,
	       t->root_dir);
	il_step_progress(ctx, 0.5, "Mounting the EFI system partition");
	rc = il_mkdir_p(t->esp_dir, 0700);
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be created: %s",
				    t->esp_dir, strerror(-rc));
	rc = il_do_mount(ctx, t->esp_dev, t->esp_dir, "vfat",
			 "umask=0077,shortname=mixed");
	if (rc)
		return rc;
	il_record(ctx, t->esp_dir, t->esp_dev, "vfat", 0);
	il_log(ctx, IL_LOG_INFO, "mounted %s on %s (vfat)", t->esp_dev,
	       t->esp_dir);
	return 0;
}

int il_umount_all(il_ctx *ctx)
{
	int rc = 0;

	sync();
	while (ctx->nmounts > 0) {
		struct il_mount_rec *r = &ctx->mounts[ctx->nmounts - 1];
		int e = 0;

		if (!r->simulated) {
			char dir[IL_PATH_MAX];
			int fd;

			il_strlcpy(dir, r->target, sizeof(dir));
			fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (fd >= 0) {
				il_syncfs(fd);
				close(fd);
			}
			e = il_umount_one(ctx, dir);
		}
		if (e && !rc)
			rc = e;
		ctx->nmounts--;
	}
	return rc;
}
