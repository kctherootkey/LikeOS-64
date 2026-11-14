/* libinstaller -- writing the partition table.
 *
 * The plan is replayed by libgpt on the table as it is on the disk now
 * (after checking that the node is still the disk that was listed -- its
 * model, serial number, size, sector size and disk GUID -- and that its
 * table is still the one the plan was made for), the
 * result is written (protective MBR, both GPT copies), old file system
 * signatures at the start and end of every new or reformatted partition are
 * wiped, and the kernel is told about the new partitions: BLKRRPART, retried
 * while the disk is busy, and when it stays busy, BLKPG add/delete for each
 * partition that changed.  Then the device nodes of the partitions are
 * awaited (up to ten seconds).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "il_internal.h"

#ifndef BLKPG
#define BLKPG 0x1269
#endif
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 0x80081272
#endif
#ifndef BLKPG_ADD_PARTITION
#define BLKPG_ADD_PARTITION 1
#define BLKPG_DEL_PARTITION 2
#endif

#define IL_RRPART_TRIES      10
#define IL_RRPART_DELAY_MS   200
#define IL_NODE_WAIT_MS      10000
#define IL_WIPE_BYTES        (1024ULL * 1024ULL)

/* The argument structures of BLKPG, spelled out for C libraries whose
 * <sys/mount.h> does not define them (glibc keeps them elsewhere). */
struct il_blkpg_partition {
	long long start;
	long long length;
	int pno;
	char devname[64];
	char volname[64];
};

struct il_blkpg_ioctl_arg {
	int op;
	int flags;
	int datalen;
	void *data;
};

int il_wipe_signatures(il_ctx *ctx, struct gpt_dev *d, uint64_t off,
		       uint64_t len)
{
	uint64_t n = len < IL_WIPE_BYTES ? len : IL_WIPE_BYTES;
	int rc;

	if (!len)
		return 0;
	rc = gpt_dev_zero(d, off, n);
	if (!rc && len > IL_WIPE_BYTES) {
		uint64_t tail = len - IL_WIPE_BYTES;

		if (tail < IL_WIPE_BYTES)
			tail = IL_WIPE_BYTES;
		rc = gpt_dev_zero(d, off + tail, len - tail);
	}
	if (rc)
		return il_set_error(ctx, -rc, "Old data at %llu on %s cannot be "
				    "cleared: %s", (unsigned long long)off,
				    d->path, strerror(-rc));
	return 0;
}

/* The disk at the node is not the one that was listed: the plan is for
 * another disk, nothing may be written. */
static int il_disks_changed(il_ctx *ctx, const struct il_disk *scan,
			    const char *what)
{
	il_log(ctx, IL_LOG_WARN, "%s is not the disk that was listed: %s",
	       scan->dev, what);
	return il_set_error(ctx, ESTALE, "The disks changed since they were "
			    "listed; choose the disk again.");
}

/* A GPT disk is also known by its disk GUID: the table on it now must
 * carry the GUID the scan saw, and a disk that had no GPT then must have
 * none now. */
static int il_check_disk_guid(il_ctx *ctx, const struct il_disk *scan,
			      struct gpt_dev *d)
{
	struct gpt_table t;
	char now[GPT_GUID_STR_LEN] = "", what[128];
	int rc;

	memset(&t, 0, sizeof(t));
	rc = gpt_read(d, &t);
	if (rc == 0) {
		gpt_guid_format(&t.disk_guid, now);
		gpt_table_free(&t);
	} else if (rc != -ENOENT && rc != -EBADMSG) {
		return il_set_error(ctx, -rc, "The partition table of %s cannot "
				    "be read: %s", scan->dev,
				    t.errmsg[0] ? t.errmsg : strerror(-rc));
	}
	if (scan->table == IL_TABLE_GPT ? strcmp(now, scan->disk_guid) == 0
					: !now[0])
		return 0;
	snprintf(what, sizeof(what), "disk GUID %s, listed as %s",
		 now[0] ? now : "(no GPT)",
		 scan->table == IL_TABLE_GPT ? scan->disk_guid : "(no GPT)");
	return il_disks_changed(ctx, scan, what);
}

/* The table on the disk must still be the one the plan was made from. */
static int il_check_unchanged(il_ctx *ctx, const struct il_disk *scan,
			      const struct gpt_table *t)
{
	int nused = 0;

	for (uint32_t i = 0; i < t->num_entries; i++) {
		const struct gpt_part *g = &t->parts[i];
		const struct il_partition *p;

		if (!g->used)
			continue;
		nused++;
		p = il_disk_part(scan, (int)i + 1);
		if (!p || p->start_bytes != g->first_lba * t->sector_size ||
		    p->size_bytes != (g->last_lba - g->first_lba + 1) *
				     t->sector_size)
			return il_set_error(ctx, ESTALE, "The partitions on %s "
					    "changed after the disk was "
					    "examined (partition %u); look at "
					    "the disk again", scan->dev, i + 1);
	}
	if (nused != scan->nparts)
		return il_set_error(ctx, ESTALE, "The partitions on %s changed "
				    "after the disk was examined (%d now, %d "
				    "then); look at the disk again", scan->dev,
				    nused, scan->nparts);
	return 0;
}

/* ---- telling the kernel ------------------------------------------- */

/* The partitions the kernel has now: number -> start/size in bytes. */
struct il_kpart {
	int partno;
	uint64_t start;
	uint64_t size;
};

static int il_kernel_parts(il_ctx *ctx, const char *disk,
			   struct il_kpart *out, int max)
{
	char rel[128], dirpath[IL_PATH_MAX];
	DIR *dir;
	struct dirent *de;
	int n = 0;

	snprintf(rel, sizeof(rel), "/sys/block/%s", disk);
	il_sys_path(ctx, dirpath, sizeof(dirpath), rel);
	dir = opendir(dirpath);
	if (!dir)
		return 0;
	while ((de = readdir(dir)) != NULL && n < max) {
		char p[IL_PATH_MAX + 320];
		uint64_t partno = 0, start = 0, size = 0;

		if (!il_starts_with(de->d_name, disk) ||
		    strcmp(de->d_name, disk) == 0)
			continue;
		snprintf(p, sizeof(p), "%s/%s/partition", dirpath, de->d_name);
		if (il_read_u64(p, &partno) != 0 || !partno)
			continue;
		snprintf(p, sizeof(p), "%s/%s/start", dirpath, de->d_name);
		il_read_u64(p, &start);
		snprintf(p, sizeof(p), "%s/%s/size", dirpath, de->d_name);
		il_read_u64(p, &size);
		out[n].partno = (int)partno;
		out[n].start = start * 512;
		out[n].size = size * 512;
		n++;
	}
	closedir(dir);
	return n;
}

static int il_blkpg(int fd, int op, int pno, uint64_t start, uint64_t len,
		    const char *devname)
{
	struct il_blkpg_partition p;
	struct il_blkpg_ioctl_arg a;

	memset(&p, 0, sizeof(p));
	p.start = (long long)start;
	p.length = (long long)len;
	p.pno = pno;
	if (devname)
		il_strlcpy(p.devname, devname, sizeof(p.devname));
	memset(&a, 0, sizeof(a));
	a.op = op;
	a.datalen = (int)sizeof(p);
	a.data = &p;
	return ioctl(fd, BLKPG, &a);
}

/* Partition by partition, for a disk the kernel will not re-read as a
 * whole (one of its partitions is in use). */
static int il_blkpg_sync(il_ctx *ctx, const struct il_target *tg,
			 const struct gpt_table *t)
{
	struct il_kpart kp[IL_MAX_PARTS];
	int nk, fd, failures = 0;

	nk = il_kernel_parts(ctx, tg->disk_name, kp, IL_MAX_PARTS);
	fd = open(tg->disk_dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return il_set_error(ctx, errno, "%s: %s", tg->disk_dev,
				    strerror(errno));
	/* Gone or changed: delete. */
	for (int i = 0; i < nk; i++) {
		const struct gpt_part *g = gpt_part_get(t, kp[i].partno);

		if (g && g->first_lba * t->sector_size == kp[i].start &&
		    (g->last_lba - g->first_lba + 1) * t->sector_size ==
		    kp[i].size)
			continue;
		if (il_blkpg(fd, BLKPG_DEL_PARTITION, kp[i].partno, 0, 0,
			     NULL) != 0) {
			il_log(ctx, IL_LOG_WARN, "BLKPG: partition %d of %s "
			       "cannot be removed: %s", kp[i].partno,
			       tg->disk_dev, strerror(errno));
			failures++;
		} else {
			il_log(ctx, IL_LOG_DEBUG, "BLKPG: removed partition %d",
			       kp[i].partno);
		}
	}
	/* New or changed: add. */
	for (uint32_t i = 0; i < t->num_entries; i++) {
		const struct gpt_part *g = &t->parts[i];
		uint64_t start, size;
		char node[64];
		int have = 0;

		if (!g->used)
			continue;
		start = g->first_lba * t->sector_size;
		size = (g->last_lba - g->first_lba + 1) * t->sector_size;
		for (int k = 0; k < nk; k++)
			if (kp[k].partno == (int)i + 1 && kp[k].start == start &&
			    kp[k].size == size)
				have = 1;
		if (have)
			continue;
		il_part_devname(tg->disk_name, (int)i + 1, node, sizeof(node));
		if (il_blkpg(fd, BLKPG_ADD_PARTITION, (int)i + 1, start, size,
			     node) != 0) {
			il_log(ctx, IL_LOG_WARN, "BLKPG: partition %u of %s "
			       "cannot be added: %s", i + 1, tg->disk_dev,
			       strerror(errno));
			failures++;
		} else {
			il_log(ctx, IL_LOG_DEBUG, "BLKPG: added partition %u",
			       i + 1);
		}
	}
	close(fd);
	if (failures)
		return il_set_error(ctx, EBUSY, "The kernel could not be told "
				    "about the new partitions of %s (%d "
				    "failed); restart and try again",
				    tg->disk_dev, failures);
	return 0;
}

static int il_reread(il_ctx *ctx, const struct il_target *tg,
		     const struct gpt_table *t)
{
	struct gpt_dev d;
	int rc;

	memset(&d, 0, sizeof(d));
	rc = gpt_dev_open(&d, tg->disk_dev, GPT_OPEN_RDONLY, 0);
	if (rc)
		return il_set_error(ctx, -rc, "%s: %s", tg->disk_dev,
				    strerror(-rc));
	rc = gpt_dev_reread(&d, IL_RRPART_TRIES, IL_RRPART_DELAY_MS);
	gpt_dev_close(&d);
	if (rc == 0) {
		il_log(ctx, IL_LOG_INFO, "the kernel re-read the partition "
		       "table of %s", tg->disk_dev);
		return 0;
	}
	il_log(ctx, IL_LOG_WARN, "re-reading the partition table of %s "
	       "failed (%s); updating partition by partition", tg->disk_dev,
	       strerror(-rc));
	return il_blkpg_sync(ctx, tg, t);
}

/* Waits until the node exists, is a block device and has the size. */
static int il_wait_node(il_ctx *ctx, const char *node, uint64_t size)
{
	uint64_t t0 = il_now_ms();
	char path[IL_PATH_MAX];
	int last_err = ENOENT;

	il_sys_path(ctx, path, sizeof(path), node);
	for (;;) {
		struct stat st;

		if (stat(path, &st) == 0) {
			if (!S_ISBLK(st.st_mode)) {
				last_err = ENOTBLK;
			} else {
				int fd = open(path, O_RDONLY | O_CLOEXEC);
				uint64_t have = 0;

				if (fd >= 0) {
					if (ioctl(fd, BLKGETSIZE64, &have) != 0)
						have = 0;
					close(fd);
				}
				if (have == size)
					return 0;
				last_err = EAGAIN;
			}
		}
		if (il_now_ms() - t0 >= IL_NODE_WAIT_MS)
			break;
		il_sleep_ms(100);
	}
	return il_set_error(ctx, last_err == EAGAIN ? EIO : last_err,
			    "The partition %s did not appear %s", node,
			    last_err == EAGAIN ? "with its new size" :
			    "within ten seconds");
}

/* ---- applying ------------------------------------------------------ */

static void il_target_part(struct il_target *tg, const struct gpt_table *t,
			   int partno, int is_root)
{
	const struct gpt_part *g = gpt_part_get(t, partno);
	char guid[GPT_GUID_STR_LEN];

	if (!g)
		return;
	gpt_guid_format(&g->uuid, guid);
	if (is_root) {
		tg->root_offset = g->first_lba * t->sector_size;
		tg->root_size = (g->last_lba - g->first_lba + 1) * t->sector_size;
		il_strlcpy(tg->root_partuuid, guid, sizeof(tg->root_partuuid));
	} else {
		tg->esp_offset = g->first_lba * t->sector_size;
		tg->esp_size = (g->last_lba - g->first_lba + 1) * t->sector_size;
		il_strlcpy(tg->esp_partuuid, guid, sizeof(tg->esp_partuuid));
	}
}

int il_partition_apply(il_ctx *ctx, il_plan *plan, struct il_target *tg)
{
	const struct il_disk *disk = &plan->disk;
	struct gpt_dev d;
	struct gpt_table t;
	char why[256];
	int rc, erases = il_plan_erases_disk(plan);
	int flags = GPT_OPEN_RDWR | (ctx->simulate ? 0 : GPT_OPEN_EXCL);

	if (il_disk_node(ctx, disk, tg->disk_dev, sizeof(tg->disk_dev)))
		return il_set_error(ctx, ENAMETOOLONG, "%s: path too long",
				    disk->dev);
	il_strlcpy(tg->disk_name, disk->name, sizeof(tg->disk_name));
	tg->root_partno = plan->root_partno;
	tg->esp_partno = il_plan_esp(plan);
	{
		const struct il_op *rf = il_plan_format_op(plan, tg->root_partno);
		const struct il_op *ef = il_plan_format_op(plan, tg->esp_partno);

		tg->format_root = rf != NULL;
		tg->format_esp = ef != NULL;
	}
	if (tg->root_partno <= 0 || tg->esp_partno <= 0)
		return il_set_error(ctx, EINVAL, "internal: the plan has no "
				    "root or no EFI system partition");
	/* The plan's copy of the disk is from the last scan; the disk
	 * the running system lives on is looked up again right before it
	 * would be written (device names can move between scans). */
	if (plan->disk.is_boot_medium ||
	    (!ctx->simulate && il_disk_holds_running_root(ctx, disk->name)))
		return il_set_error(ctx, EBUSY, "%s holds the running LikeOS "
				    "(the installation medium) and cannot be "
				    "changed", disk->dev);
	/* Device names can move between scans: the node must still be the
	 * disk that was listed (model, serial number, then size, sector
	 * size and disk GUID once it is open). */
	if (!il_disk_same_ident(ctx, disk, why, sizeof(why)))
		return il_disks_changed(ctx, disk, why);

	memset(&d, 0, sizeof(d));
	rc = gpt_dev_open(&d, tg->disk_dev, flags, 0);
	if (rc == -EBUSY)
		return il_set_error(ctx, EBUSY, "%s is in use (a partition of it "
				    "is mounted or open); it cannot be "
				    "partitioned", disk->dev);
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be opened for writing: "
				    "%s", disk->dev, strerror(-rc));
	if (d.sector_size != (disk->sector_size ? disk->sector_size : 512) ||
	    d.size_bytes != disk->size_bytes) {
		snprintf(why, sizeof(why), "%llu bytes in %u-byte sectors, "
			 "listed as %llu bytes in %u-byte sectors",
			 (unsigned long long)d.size_bytes, d.sector_size,
			 (unsigned long long)disk->size_bytes,
			 disk->sector_size ? disk->sector_size : 512);
		gpt_dev_close(&d);
		return il_disks_changed(ctx, disk, why);
	}
	rc = il_check_disk_guid(ctx, disk, &d);
	if (rc) {
		gpt_dev_close(&d);
		return rc;
	}
	tg->sector_size = d.sector_size;

	memset(&t, 0, sizeof(t));
	if (erases) {
		rc = gpt_table_new(&t, d.sector_size, d.nsectors);
		if (rc) {
			gpt_dev_close(&d);
			return il_set_error(ctx, -rc, "A partition table does not "
					    "fit on %s", disk->dev);
		}
	} else {
		rc = gpt_read(&d, &t);
		if (rc) {
			gpt_dev_close(&d);
			return il_set_error(ctx, -rc, "The partition table of %s "
					    "cannot be read: %s", disk->dev,
					    t.errmsg[0] ? t.errmsg :
					    strerror(-rc));
		}
		rc = il_check_unchanged(ctx, disk, &t);
		if (rc) {
			gpt_table_free(&t);
			gpt_dev_close(&d);
			return rc;
		}
	}
	rc = il_plan_replay(plan, &t, 1, why, sizeof(why));
	if (rc) {
		gpt_table_free(&t);
		gpt_dev_close(&d);
		return il_set_error(ctx, -rc, "%s", why);
	}

	/* Last moment a cancel is honoured; from here on the disk changes. */
	if (il_cancelled(ctx)) {
		gpt_table_free(&t);
		gpt_dev_close(&d);
		return -ECANCELED;
	}
	__atomic_store_n(&ctx->cancel_allowed, 0, __ATOMIC_SEQ_CST);
	il_step_progress(ctx, 0.2, "Writing the partition table");

	if (erases) {
		rc = gpt_wipe(&d);
		if (rc) {
			gpt_table_free(&t);
			gpt_dev_close(&d);
			return il_set_error(ctx, -rc, "%s cannot be erased: %s",
					    disk->dev, strerror(-rc));
		}
		il_log(ctx, IL_LOG_INFO, "%s: old partition tables erased",
		       disk->dev);
	}
	rc = gpt_write(&d, &t);
	if (rc) {
		il_set_error(ctx, -rc, "The partition table cannot be written "
			     "to %s: %s", disk->dev,
			     t.errmsg[0] ? t.errmsg : strerror(-rc));
		gpt_table_free(&t);
		gpt_dev_close(&d);
		return rc;
	}
	il_log(ctx, IL_LOG_INFO, "%s: partition table written", disk->dev);
	il_step_progress(ctx, 0.5, "Clearing old file system signatures");

	/* Old signatures in the new partitions must not survive: a stale
	 * superblock in a partition about to be formatted could still be
	 * found by a probe that looks further than mkfs writes. */
	for (uint32_t i = 0; i < t.num_entries; i++) {
		const struct gpt_part *g = &t.parts[i];
		int partno = (int)i + 1;

		if (!g->used)
			continue;
		if (!erases && !il_plan_creates(plan, partno) &&
		    !il_plan_format_op(plan, partno))
			continue;
		rc = il_wipe_signatures(ctx, &d, g->first_lba * d.sector_size,
					(g->last_lba - g->first_lba + 1) *
					d.sector_size);
		if (rc) {
			gpt_table_free(&t);
			gpt_dev_close(&d);
			return rc;
		}
	}
	rc = gpt_dev_sync(&d);
	gpt_dev_close(&d);
	if (rc) {
		gpt_table_free(&t);
		return il_set_error(ctx, -rc, "%s: the changes cannot be flushed: "
				    "%s", disk->dev, strerror(-rc));
	}
	il_target_part(tg, &t, tg->root_partno, 1);
	il_target_part(tg, &t, tg->esp_partno, 0);
	il_log(ctx, IL_LOG_INFO, "root partition %d PARTUUID=%s, EFI system "
	       "partition %d PARTUUID=%s", tg->root_partno, tg->root_partuuid,
	       tg->esp_partno, tg->esp_partuuid);

	if (ctx->simulate) {
		/* The partitions are areas of the image; the format step
		 * works on scratch files and copies them in. */
		gpt_table_free(&t);
		il_step_progress(ctx, 1.0, "Partition table written");
		return 0;
	}
	il_step_progress(ctx, 0.7, "Waiting for the new partitions");
	rc = il_reread(ctx, tg, &t);
	if (rc) {
		gpt_table_free(&t);
		return rc;
	}
	il_part_devname(disk->dev, tg->root_partno, tg->root_dev,
			sizeof(tg->root_dev));
	il_part_devname(disk->dev, tg->esp_partno, tg->esp_dev,
			sizeof(tg->esp_dev));
	rc = il_wait_node(ctx, tg->root_dev, tg->root_size);
	if (!rc)
		rc = il_wait_node(ctx, tg->esp_dev, tg->esp_size);
	gpt_table_free(&t);
	if (rc)
		return rc;
	il_step_progress(ctx, 1.0, "Partitions ready");
	return 0;
}
