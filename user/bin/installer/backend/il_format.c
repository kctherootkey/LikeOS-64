/* libinstaller -- making the file systems.
 *
 *   EFI system partition   mkfs.fat -F 32 [-S <logical sector size>]
 *                            -n LIKEOS-ESP
 *   LikeOS root            mke2fs -F -q -t ext4 -b 4096 -I 256
 *                            -O ^64bit,^metadata_csum_seed,^orphan_file
 *                            -L likeos-root -E root_owner=0:0
 * The LikeOS boot loader reads the root file system itself and does not
 * understand 64-bit block numbers or meta_bg, hence the switched-off
 * features.  e2fsck -fn checks the new file system right away; tune2fs
 * turns off the periodic checks.  The UUIDs of the new file systems are then
 * read from their superblocks (they go into /etc/fstab).
 *
 * In simulate mode the partitions are areas of an image file: each file
 * system is made in a scratch file of the partition's size and copied into
 * the image afterwards (skipping blocks of zeroes, so sparse images stay
 * sparse).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "il_internal.h"

/* simulate mode: <image>.p<N>.fs next to the disk image */
static void il_sim_scratch(const struct il_target *t, int partno, char *out,
			   size_t len)
{
	char suffix[24];

	snprintf(suffix, sizeof(suffix), ".p%d.fs", partno);
	il_strlcpy(out, t->disk_dev, len);
	il_strlcat(out, suffix, len);
}

static int il_sim_prepare(il_ctx *ctx, const char *path, uint64_t size)
{
	int fd;

	unlink(path);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		return il_set_error(ctx, errno, "%s: %s", path, strerror(errno));
	if (ftruncate(fd, (off_t)size) != 0) {
		int e = errno;

		close(fd);
		unlink(path);
		return il_set_error(ctx, e, "%s: %s", path, strerror(e));
	}
	close(fd);
	return 0;
}

int il_format_esp(il_ctx *ctx, struct il_target *t)
{
	const char *argv[10];
	char ss[16];
	int n = 0, rc;

	if (ctx->simulate) {
		il_sim_scratch(t, t->esp_partno, t->esp_dev, sizeof(t->esp_dev));
		if (!t->format_esp)
			return 0;
		rc = il_sim_prepare(ctx, t->esp_dev, t->esp_size);
		if (rc)
			return rc;
	}
	if (!t->format_esp) {
		il_log(ctx, IL_LOG_INFO, "the EFI system partition %s is kept as "
		       "it is", t->esp_dev);
		return 0;
	}
	il_step_progress(ctx, 0.05, "Formatting the EFI system partition");
	argv[n++] = ctx->tool_mkfs_fat;
	argv[n++] = "-F";
	argv[n++] = "32";
	/* The disk's logical sector size, said explicitly: mkfs.fat takes
	 * 512 for anything it cannot ask (an image file, a device that does
	 * not answer BLKSSZGET). */
	if (t->sector_size && t->sector_size != 512) {
		snprintf(ss, sizeof(ss), "%u", (unsigned)t->sector_size);
		argv[n++] = "-S";
		argv[n++] = ss;
	}
	argv[n++] = "-n";
	argv[n++] = IL_ESP_LABEL;
	argv[n++] = t->esp_dev;
	argv[n] = NULL;
	rc = il_run_tool(ctx, "Formatting the EFI system partition", argv);
	if (rc)
		return rc;
	il_log(ctx, IL_LOG_INFO, "%s: FAT32 file system made", t->esp_dev);
	return 0;
}

int il_format_root(il_ctx *ctx, struct il_target *t)
{
	const char *argv[24];
	int n = 0, rc, status = -1;

	if (ctx->simulate) {
		il_sim_scratch(t, t->root_partno, t->root_dev,
			       sizeof(t->root_dev));
		rc = il_sim_prepare(ctx, t->root_dev, t->root_size);
		if (rc)
			return rc;
	}
	if (!t->format_root)
		return il_set_error(ctx, EINVAL, "internal: the root partition "
				    "is always formatted");
	il_step_progress(ctx, 0.2, "Formatting the LikeOS partition");
	argv[n++] = ctx->tool_mke2fs;
	argv[n++] = "-F";
	argv[n++] = "-q";
	argv[n++] = "-t";
	argv[n++] = "ext4";
	argv[n++] = "-b";
	argv[n++] = "4096";
	argv[n++] = "-I";
	argv[n++] = "256";
	argv[n++] = "-O";
	argv[n++] = "^64bit,^metadata_csum_seed,^orphan_file";
	argv[n++] = "-L";
	argv[n++] = IL_ROOT_LABEL;
	argv[n++] = "-E";
	argv[n++] = "root_owner=0:0";
	argv[n++] = t->root_dev;
	argv[n] = NULL;
	rc = il_run_tool(ctx, "Formatting the LikeOS partition", argv);
	if (rc)
		return rc;
	il_log(ctx, IL_LOG_INFO, "%s: ext4 file system made", t->root_dev);

	/* Checked before anything is written into it. */
	il_step_progress(ctx, 0.75, "Checking the new file system");
	n = 0;
	argv[n++] = ctx->tool_e2fsck;
	argv[n++] = "-f";
	argv[n++] = "-n";
	argv[n++] = t->root_dev;
	argv[n] = NULL;
	rc = il_spawn(ctx, argv, &status, NULL, 0);
	if (rc)
		return rc;
	if (status != 0)
		return il_set_error(ctx, EIO, "The new file system on %s fails "
				    "its check (e2fsck exit status %d); the log "
				    "%s has the details", t->root_dev, status,
				    ctx->log_path);

	/* No forced checks after a number of mounts or days: the system has
	 * a journal, and a check at an arbitrary boot surprises people. */
	if (access(ctx->tool_tune2fs, X_OK) == 0) {
		n = 0;
		argv[n++] = ctx->tool_tune2fs;
		argv[n++] = "-c";
		argv[n++] = "0";
		argv[n++] = "-i";
		argv[n++] = "0";
		argv[n++] = t->root_dev;
		argv[n] = NULL;
		if (il_spawn(ctx, argv, &status, NULL, 0) != 0 || status != 0)
			il_log(ctx, IL_LOG_WARN, "tune2fs could not switch off "
			       "the periodic checks (not needed)");
	}
	return 0;
}

/* Copies a scratch file into the image at off, leaving out zero blocks. */
static int il_sim_copy(il_ctx *ctx, const char *src, const char *image,
		       uint64_t off, uint64_t size)
{
	const size_t chunk = 1024 * 1024;
	uint8_t *buf;
	int in, out, rc = 0;
	uint64_t done = 0;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return il_set_error(ctx, errno, "%s: %s", src, strerror(errno));
	out = open(image, O_WRONLY | O_CLOEXEC);
	if (out < 0) {
		rc = il_set_error(ctx, errno, "%s: %s", image, strerror(errno));
		close(in);
		return rc;
	}
	buf = malloc(chunk);
	if (!buf) {
		close(in);
		close(out);
		return il_set_error(ctx, ENOMEM, "out of memory");
	}
	while (done < size) {
		size_t want;
		ssize_t n;
		int zero = 1;

#ifdef SEEK_DATA
		/* Holes of the scratch file need no reading at all. */
		{
			off_t data = lseek(in, (off_t)done, SEEK_DATA);

			if (data < 0 && errno == ENXIO)
				break;          /* only a hole is left */
			if (data > (off_t)done) {
				done = (uint64_t)data & ~(uint64_t)(chunk - 1);
				if (done >= size)
					break;
			}
		}
#endif
		want = (size - done) < chunk ? (size_t)(size - done) : chunk;
		n = pread(in, buf, want, (off_t)done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = il_set_error(ctx, errno, "%s: %s", src,
					  strerror(errno));
			break;
		}
		if (n == 0)
			break;
		for (ssize_t i = 0; i < n; i++)
			if (buf[i]) {
				zero = 0;
				break;
			}
		if (!zero) {
			ssize_t w = 0;

			while (w < n) {
				ssize_t k = pwrite(out, buf + w, (size_t)(n - w),
						   (off_t)(off + done + (uint64_t)w));

				if (k < 0 && errno == EINTR)
					continue;
				if (k <= 0) {
					rc = il_set_error(ctx, k < 0 ? errno : EIO,
							  "%s: %s", image,
							  strerror(k < 0 ? errno
									 : EIO));
					break;
				}
				w += k;
			}
			if (rc)
				break;
		}
		done += (uint64_t)n;
	}
	free(buf);
	close(in);
	if (!rc && fsync(out) != 0)
		rc = il_set_error(ctx, errno, "%s: %s", image, strerror(errno));
	close(out);
	return rc;
}

int il_format_commit_sim(il_ctx *ctx, struct il_target *t)
{
	int rc = 0;

	if (!ctx->simulate)
		return 0;
	if (t->format_esp) {
		rc = il_sim_copy(ctx, t->esp_dev, t->disk_dev, t->esp_offset,
				 t->esp_size);
		if (rc)
			return rc;
	}
	if (t->format_root)
		rc = il_sim_copy(ctx, t->root_dev, t->disk_dev, t->root_offset,
				 t->root_size);
	if (!rc)
		il_log(ctx, IL_LOG_INFO, "file systems copied into %s",
		       t->disk_dev);
	return rc;
}

int il_format_probe(il_ctx *ctx, struct il_target *t)
{
	struct il_fsprobe fp;
	const char *src_root = t->root_dev, *src_esp = t->esp_dev;
	uint64_t off_root = 0, off_esp = 0;
	int rc;

	if (ctx->simulate) {
		/* Read them back from the image: proves the copy. */
		src_root = src_esp = t->disk_dev;
		off_root = t->root_offset;
		off_esp = t->esp_offset;
	}
	rc = il_fs_probe(src_root, off_root, &fp);
	if (rc || strcmp(fp.fstype, "ext4") != 0 || !fp.uuid[0])
		return il_set_error(ctx, rc ? -rc : EIO, "The new LikeOS file "
				    "system on %s cannot be found (%s)",
				    t->root_dev, rc ? strerror(-rc) : fp.fstype);
	il_strlcpy(t->root_fsuuid, fp.uuid, sizeof(t->root_fsuuid));
	rc = il_fs_probe(src_esp, off_esp, &fp);
	if (rc || strcmp(fp.fstype, "vfat") != 0 || !fp.uuid[0])
		return il_set_error(ctx, rc ? -rc : EIO, "The FAT file system of "
				    "the EFI system partition %s cannot be found "
				    "(%s)", t->esp_dev,
				    rc ? strerror(-rc) : fp.fstype);
	il_strlcpy(t->esp_fsuuid, fp.uuid, sizeof(t->esp_fsuuid));
	il_log(ctx, IL_LOG_INFO, "root file system UUID=%s, EFI system "
	       "partition UUID=%s", t->root_fsuuid, t->esp_fsuuid);
	return 0;
}
