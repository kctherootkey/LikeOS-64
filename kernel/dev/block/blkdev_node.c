// LikeOS - block device nodes: /dev/sdX, /dev/sdXN, /dev/srN and the
// /dev/disk/by-* links, reading and writing a device through its node,
// and the established block device ioctls.
//
// ---- Nodes --------------------------------------------------------------------------
// Every registered extended block device gets an S_IFBLK node in devfs
// (kernel/fs/devfs.c, struct devfs_node.is_blk) named after it, mode 0660,
// owner root, group disk (6) -- an optical drive group cdrom (24) -- with
// the device's major:minor.  Programs open it like a file:
//
//   read()/write()  at the handle's position, any length and alignment:
//                   whole sectors go straight through a kernel buffer, a
//                   partial first or last sector is read, patched and
//                   written back.  The end of the device is end-of-file
//                   for a read and ENOSPC for a write.  A device that is
//                   read-only (BLKROSET, write-protected medium) cannot
//                   be opened for writing (EACCES) and fails a write
//                   (EPERM) if it became read-only after the open.
//   lseek()         SEEK_SET/CUR/END (the end is the device size), and
//                   SEEK_DATA/SEEK_HOLE as "all data".
//   fsync()         flushes the drive's write cache.
//   O_EXCL          claims the device exclusively for the handle: the
//                   open fails with EBUSY while it is mounted or claimed
//                   by someone else, and a mount fails while it is held.
//   ioctl()         the driver's own requests first (SG_IO, HDIO_*, ...),
//                   then the generic BLK* set below.
//
// ---- /dev/disk ----------------------------------------------------------------------
// Stable names, as symbolic links to the node ("../../sda1"):
//   by-id/ata-<model>_<serial>[-partN], wwn-0x<wwn>[-partN]
//         (usb-<vendor>_<model>_<serial>-0:<lun> for USB storage)
//   by-path/pci-<domain:bus:dev.fn>-ata-<port>.<pmp>[-partN]
//   by-partuuid/<GPT unique GUID | MBR ssssssss-nn>
//   by-partlabel/<GPT partition name>
//   by-uuid/<filesystem UUID>, by-label/<filesystem label>
//   by-diskseq/<diskseq>[-partN]
// Names are escaped the conventional way (a blank or '/' in a label
// becomes \x20, \x2f).  When two devices claim the same name the first
// one keeps it.
//
// ---- Lifetime -----------------------------------------------------------------------
// A node record is referenced by its registration and by every handle
// opened through it (devfs keeps a pointer to the record's devfs_node in
// the handle).  Removing a device unregisters its records; a record whose
// last handle is gone is freed after a grace period, which covers an
// open() that found the node an instant before it was unregistered.
//
// All node and link changes happen with the registry mutex held.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/fs/device.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define BLKNODE_MAGIC 0x4E4B4C42u /* "BLKN" */
#define BLKFILE_MAGIC 0x464B4C42u /* "BLKF" */
#define BLKNODE_DEV 1
#define BLKNODE_LINK 2
/* How long an unregistered record stays allocated (see above). */
#define BLKNODE_GRACE_NS (10ULL * 1000ULL * 1000ULL * 1000ULL)
/* The largest kernel buffer one read()/write()/pread()/pwrite() on a node
 * goes through: a transfer is cut into requests of at most this size, so it
 * is what lets a large sequential read reach the drive as large commands
 * (the queue splits it further at the device's max_sectors). */
#define BLKDEV_BOUNCE_BYTES (1024 * 1024)

#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

struct blk_node {
	struct devfs_node dn; /* registered with devfs; priv -> this */
	uint32_t magic;
	int kind; /* BLKNODE_DEV / BLKNODE_LINK */
	block_device_t *bdev; /* the device it names (no reference) */
	int registered;
	refcount_t ref; /* registration + open handles */
	uint64_t dead_ns; /* when it was unregistered */
	struct blk_node *next; /* g_nodes or g_graveyard */
};

/* One open handle. */
struct blk_file {
	uint32_t magic;
	struct blk_node *node;
	block_device_t *bdev; /* referenced by blk_bdev_open() */
	uint64_t pos;
	int flags; /* open flags */
	int excl; /* holds the exclusive claim */
	uint32_t bsize; /* BLKBSZGET/BLKBSZSET */
	struct blk_mutex io; /* position + the read-modify-write buffer */
	struct blk_file *excl_next; /* g_excl_files, registry mutex */
};

/* Handles holding an exclusive claim (O_EXCL), so a rescan can tell a
 * claim made through a node -- the partitioning tool itself -- from a
 * mount.  Registry mutex. */
static struct blk_file *g_excl_files;

int blkdev_holder_is_node_locked(const void *holder)
{
	for (struct blk_file *f = g_excl_files; f; f = f->excl_next)
		if (f == holder)
			return 1;
	return 0;
}

/* Records currently registered.  Registry mutex. */
static struct blk_node *g_nodes;
/* Unregistered records waiting to be freed.  g_grave_lock. */
static spinlock_t g_grave_lock = SPINLOCK_INIT("blknode_grave");
static struct blk_node *g_graveyard;

/* SG_GET/SET_TIMEOUT and SG_GET/SET_RESERVED_SIZE values, per registry
 * slot (a property of the device in the established interface). */
static int g_sg_timeout[BLOCK_MAX_DEVICES];
static int g_sg_reserved[BLOCK_MAX_DEVICES];

/* ==========================================================================
 * /dev/disk directories
 * ========================================================================== */

enum blk_link_dir {
	BLD_ID = 0,
	BLD_PATH,
	BLD_PARTUUID,
	BLD_PARTLABEL,
	BLD_UUID,
	BLD_LABEL,
	BLD_DISKSEQ,
	BLD_NR
};

static const char *const g_link_dir_names[BLD_NR] = {
	"/dev/disk/by-id",	"/dev/disk/by-path",  "/dev/disk/by-partuuid",
	"/dev/disk/by-partlabel", "/dev/disk/by-uuid", "/dev/disk/by-label",
	"/dev/disk/by-diskseq",
};

static struct devfs_node g_dir_disk;
static struct devfs_node g_link_dirs[BLD_NR];
static int g_dir_disk_ok;
static int g_link_dir_ok[BLD_NR];

static int blkdev_dir_ensure(int which)
{
	if (!g_dir_disk_ok) {
		mm_memset(&g_dir_disk, 0, sizeof(g_dir_disk));
		ksnprintf(g_dir_disk.path, sizeof(g_dir_disk.path), "/dev/disk");
		g_dir_disk.mode = 0755;
		if (device_register_dir(&g_dir_disk) == 0)
			g_dir_disk_ok = 1;
	}
	if (which < 0)
		return g_dir_disk_ok ? 0 : -ENOSPC;
	if (!g_link_dir_ok[which]) {
		struct devfs_node *d = &g_link_dirs[which];

		mm_memset(d, 0, sizeof(*d));
		ksnprintf(d->path, sizeof(d->path), "%s", g_link_dir_names[which]);
		d->mode = 0755;
		if (device_register_dir(d) == 0)
			g_link_dir_ok[which] = 1;
	}
	return g_link_dir_ok[which] ? 0 : -ENOSPC;
}

void blkdev_nodes_init(void)
{
	/* /dev/disk, by-id and by-path exist from boot (tools look there
	 * before any disk is known); the other directories appear with their
	 * first link. */
	(void)blkdev_dir_ensure(-1);
	(void)blkdev_dir_ensure(BLD_ID);
	(void)blkdev_dir_ensure(BLD_PATH);
}

/* ==========================================================================
 * Records
 * ========================================================================== */

static const struct device_ops g_blkdev_node_ops;

static struct blk_node *blknode_alloc(block_device_t *bdev, int kind)
{
	struct blk_node *n = (struct blk_node *)kcalloc(1, sizeof(*n));

	if (!n)
		return NULL;
	n->magic = BLKNODE_MAGIC;
	n->kind = kind;
	n->bdev = bdev;
	refcount_set(&n->ref, 1);
	n->dn.priv = n;
	return n;
}

static void blknode_free(struct blk_node *n)
{
	n->magic = 0;
	kfree(n);
}

/* Register a filled-in record; on failure it is freed.  Registry mutex. */
static int blknode_register_locked(struct blk_node *n)
{
	int rc = n->kind == BLKNODE_LINK ? device_register_link(&n->dn) :
					   device_register(&n->dn);

	if (rc) {
		blknode_free(n);
		return rc;
	}
	n->registered = 1;
	n->next = g_nodes;
	g_nodes = n;
	return 0;
}

/* Unregister a record and hand it to the graveyard.  Registry mutex. */
static void blknode_unregister_locked(struct blk_node *n)
{
	struct blk_node **pp;
	uint64_t fl;

	for (pp = &g_nodes; *pp; pp = &(*pp)->next) {
		if (*pp == n) {
			*pp = n->next;
			break;
		}
	}
	if (n->registered)
		device_unregister(&n->dn);
	n->registered = 0;
	n->dead_ns = hrtimer_now_ns();
	/* the registration's reference */
	(void)refcount_dec_and_test(&n->ref);
	spin_lock_irqsave(&g_grave_lock, &fl);
	n->next = g_graveyard;
	g_graveyard = n;
	spin_unlock_irqrestore(&g_grave_lock, fl);
}

void blkdev_nodes_reap(void)
{
	struct blk_node *keep = NULL, *dead = NULL, *n;
	uint64_t fl, now = hrtimer_now_ns();

	if (!__atomic_load_n(&g_graveyard, __ATOMIC_ACQUIRE))
		return;
	spin_lock_irqsave(&g_grave_lock, &fl);
	n = g_graveyard;
	g_graveyard = NULL;
	while (n) {
		struct blk_node *next = n->next;

		if (refcount_read(&n->ref) == 0 &&
		    now - n->dead_ns >= BLKNODE_GRACE_NS) {
			n->next = dead;
			dead = n;
		} else {
			n->next = keep;
			keep = n;
		}
		n = next;
	}
	/* put the survivors back (others may have been added meanwhile) */
	while (keep) {
		struct blk_node *next = keep->next;
		keep->next = g_graveyard;
		g_graveyard = keep;
		keep = next;
	}
	spin_unlock_irqrestore(&g_grave_lock, fl);
	while (dead) {
		struct blk_node *next = dead->next;
		blknode_free(dead);
		dead = next;
	}
}

/* ==========================================================================
 * Link names
 * ========================================================================== */

int blkdev_link_escape(const char *in, char *out, size_t cap)
{
	static const char hex[] = "0123456789abcdef";
	size_t o = 0;
	const unsigned char *p = (const unsigned char *)in;

	if (!in || !out || cap == 0)
		return -EINVAL;
	while (*p) {
		unsigned char c = *p;
		int len = 0;

		/* a valid UTF-8 multibyte sequence stays as it is */
		if (c >= 0xC2 && c <= 0xDF)
			len = 2;
		else if (c >= 0xE0 && c <= 0xEF)
			len = 3;
		else if (c >= 0xF0 && c <= 0xF4)
			len = 4;
		if (len) {
			int ok = 1;
			for (int k = 1; k < len; k++)
				if ((p[k] & 0xC0) != 0x80)
					ok = 0;
			if (ok) {
				if (o + (size_t)len + 1 > cap)
					return -ENAMETOOLONG;
				for (int k = 0; k < len; k++)
					out[o++] = (char)p[k];
				p += len;
				continue;
			}
		}
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') || c == '#' || c == '+' || c == '-' ||
		    c == '.' || c == ':' || c == '=' || c == '@' || c == '_') {
			if (o + 2 > cap)
				return -ENAMETOOLONG;
			out[o++] = (char)c;
		} else {
			if (o + 5 > cap)
				return -ENAMETOOLONG;
			out[o++] = '\\';
			out[o++] = 'x';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
		p++;
	}
	out[o] = 0;
	return 0;
}

/* Make one link `dir'/`name' -> ../../<device name>.  Registry mutex. */
static void blkdev_link_add_locked(block_device_t *bdev, int dir,
				   const char *name)
{
	struct blk_node *n;
	int rc;

	if (!name || !*name)
		return;
	if (blkdev_dir_ensure(dir))
		return;
	n = blknode_alloc(bdev, BLKNODE_LINK);
	if (!n)
		return;
	if (ksnprintf(n->dn.path, sizeof(n->dn.path), "%s/%s",
		      g_link_dir_names[dir], name) >= (int)sizeof(n->dn.path)) {
		kprintf(BLK_LOG_PREFIX "%s: link name %s too long, skipped\n",
			bdev->name, name);
		blknode_free(n);
		return;
	}
	ksnprintf(n->dn.link, sizeof(n->dn.link), "../../%s", bdev->name);
	n->dn.mode = 0777;
	rc = blknode_register_locked(n);
	if (rc && rc != -EEXIST)
		kprintf(BLK_LOG_PREFIX "%s: link %s/%s failed (%d)\n",
			bdev->name, g_link_dir_names[dir], name, rc);
}

void blkdev_links_remove_locked(block_device_t *bdev)
{
	struct blk_node *n = g_nodes, *next;

	while (n) {
		next = n->next;
		if (n->bdev == bdev && n->kind == BLKNODE_LINK)
			blknode_unregister_locked(n);
		n = next;
	}
}

/* "-partN" for a partition, "" for a whole disk. */
static void blkdev_part_suffix(const block_device_t *bdev, char *out,
			       size_t cap)
{
	if (bdev->part)
		ksnprintf(out, cap, "-part%d", bdev->part->partno);
	else
		out[0] = 0;
}

int blkdev_links_update_locked(block_device_t *bdev)
{
	struct gendisk *d;
	char name[DEVFS_NODE_PATH_MAX];
	char esc[DEVFS_NODE_PATH_MAX];
	char suffix[16];
	const struct blk_fs_identity *id;

	if (!bdev || !blk_bdev_is_extended(bdev))
		return -EINVAL;
	blkdev_links_remove_locked(bdev);
	if (!(bdev->flags & BDEV_F_NODE) || (bdev->flags & BDEV_F_DEAD))
		return 0;
	d = bdev->disk;
	blkdev_part_suffix(bdev, suffix, sizeof(suffix));

	/* by-id */
	if (blk_bdev_disk_id(bdev, name, sizeof(name)) == 0)
		blkdev_link_add_locked(bdev, BLD_ID, name);
	if (d->wwn[0]) {
		const char *w = d->wwn;
		int has0x = w[0] == '0' && (w[1] == 'x' || w[1] == 'X');

		ksnprintf(name, sizeof(name), "wwn-%s%s%s", has0x ? "" : "0x", w,
			  suffix);
		if (blkdev_link_escape(name, esc, sizeof(esc)) == 0)
			blkdev_link_add_locked(bdev, BLD_ID, esc);
	}
	/* by-path */
	if (d->pci && d->host_no >= 0) {
		const pci_device_t *pd = d->pci;

		if (kstrcmp(d->bus, "usb") == 0)
			ksnprintf(name, sizeof(name),
				  "pci-0000:%02x:%02x.%x-usb-0:%d:1.0-scsi-0:0:0:%d%s",
				  pd->bus, pd->device, pd->function,
				  d->host_no + 1, d->lun > 0 ? d->lun : 0, suffix);
		else
			ksnprintf(name, sizeof(name),
				  "pci-0000:%02x:%02x.%x-%s-%d.%d%s", pd->bus,
				  pd->device, pd->function,
				  d->bus[0] ? d->bus : "ata", d->host_no + 1,
				  d->target > 0 ? d->target : 0, suffix);
		blkdev_link_add_locked(bdev, BLD_PATH, name);
	}
	/* by-partuuid, by-partlabel */
	if (bdev->part) {
		struct blk_partition *p = bdev->part;

		if (p->partuuid[0])
			blkdev_link_add_locked(bdev, BLD_PARTUUID, p->partuuid);
		if (p->name[0] &&
		    blkdev_link_escape(p->name, esc, sizeof(esc)) == 0)
			blkdev_link_add_locked(bdev, BLD_PARTLABEL, esc);
	}
	/* by-uuid, by-label: the filesystem on the device.  A whole disk
	 * with a partition table only when what it holds is an ISO image
	 * (a hybrid image carries both). */
	id = bdev->part ? &bdev->part->fs_id : &d->fs_id;
	if (id->valid &&
	    (bdev->part || d->pt_type == BLK_PT_NONE ||
	     kstrcmp(id->type, "iso9660") == 0)) {
		if (id->uuid[0] &&
		    blkdev_link_escape(id->uuid, esc, sizeof(esc)) == 0)
			blkdev_link_add_locked(bdev, BLD_UUID, esc);
		if (id->label[0] &&
		    blkdev_link_escape(id->label, esc, sizeof(esc)) == 0)
			blkdev_link_add_locked(bdev, BLD_LABEL, esc);
	}
	/* by-diskseq */
	ksnprintf(name, sizeof(name), "%llu%s", (unsigned long long)d->diskseq,
		  suffix);
	blkdev_link_add_locked(bdev, BLD_DISKSEQ, name);
	return 0;
}

/* ==========================================================================
 * Device nodes
 * ========================================================================== */

int blkdev_node_add_locked(block_device_t *bdev)
{
	struct blk_node *n;
	int rc;

	if (!bdev || !blk_bdev_is_extended(bdev) || !bdev->name)
		return -EINVAL;
	if (bdev->flags & BDEV_F_NODE)
		return 0;
	(void)blkdev_dir_ensure(-1);
	n = blknode_alloc(bdev, BLKNODE_DEV);
	if (!n)
		return -ENOMEM;
	ksnprintf(n->dn.path, sizeof(n->dn.path), "/dev/%s", bdev->name);
	n->dn.mode = 0660;
	n->dn.uid = 0;
	n->dn.gid = (bdev->flags & BDEV_F_CDROM) ? DEVFS_GID_CDROM :
						   DEVFS_GID_DISK;
	n->dn.major = bdev->major;
	n->dn.minor = bdev->minor;
	n->dn.ops = &g_blkdev_node_ops;
	n->dn.is_blk = 1;
	rc = blknode_register_locked(n);
	if (rc)
		return rc;
	bdev->flags |= BDEV_F_NODE;
	if (!bdev->part && bdev->disk)
		bdev->disk->node = &n->dn;
	return blkdev_links_update_locked(bdev);
}

void blkdev_node_remove_locked(block_device_t *bdev)
{
	struct blk_node *n, *next;

	if (!bdev)
		return;
	blkdev_links_remove_locked(bdev);
	for (n = g_nodes; n; n = next) {
		next = n->next;
		if (n->bdev == bdev && n->kind == BLKNODE_DEV)
			blknode_unregister_locked(n);
	}
	bdev->flags &= ~BDEV_F_NODE;
	if (!bdev->part && bdev->disk)
		bdev->disk->node = NULL;
}

/* ==========================================================================
 * Open handles
 * ========================================================================== */

static int blk_open_writable(int flags)
{
	int acc = flags & 3;

	return acc == O_WRONLY || acc == O_RDWR;
}

static struct blk_file *blk_file_of(vfs_file_t *file)
{
	struct blk_file *bf = (struct blk_file *)device_file_priv(file);

	if (!bf || bf->magic != BLKFILE_MAGIC)
		return NULL;
	return bf;
}

static int blkdev_open(struct devfs_node *dn, vfs_file_t *file, int flags,
		       struct task *cur)
{
	struct blk_node *n = dn ? (struct blk_node *)dn->priv : NULL;
	block_device_t *bdev;
	struct blk_file *bf;
	int rc;

	(void)cur;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	if (!n || n->magic != BLKNODE_MAGIC || n->kind != BLKNODE_DEV ||
	    !n->registered || !n->bdev ||
	    !(n->bdev->flags & BDEV_F_REGISTERED) ||
	    (n->bdev->flags & BDEV_F_DEAD)) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return -ENXIO;
	}
	bdev = n->bdev;
	refcount_inc(&n->ref);
	blk_bdev_get(bdev); /* across the unlocked part below */
	blk_mutex_unlock(&g_disk_registry_mutex);

	rc = -EACCES;
	if (blk_open_writable(flags) && blk_bdev_read_only(bdev))
		goto fail_ref;
	rc = blk_bdev_open(bdev, flags);
	if (rc)
		goto fail_ref;
	bf = (struct blk_file *)kcalloc(1, sizeof(*bf));
	if (!bf) {
		rc = -ENOMEM;
		goto fail_open;
	}
	bf->magic = BLKFILE_MAGIC;
	bf->node = n;
	bf->bdev = bdev;
	bf->flags = flags;
	blk_mutex_init(&bf->io, "blkdev_file");
	/* The soft block size: the page size where the device divides into
	 * it, the logical sector size otherwise. */
	bf->bsize = (bdev->sector_size <= 4096 &&
		     ((uint64_t)bdev->total_sectors * bdev->sector_size) % 4096 ==
			     0) ?
			    4096 :
			    bdev->sector_size;
	if (flags & O_EXCL) {
		rc = blk_bdev_claim(bdev, bf);
		if (rc) {
			bf->magic = 0;
			kfree(bf);
			goto fail_open;
		}
		bf->excl = 1;
		blk_mutex_lock(&g_disk_registry_mutex);
		bf->excl_next = g_excl_files;
		g_excl_files = bf;
		blk_mutex_unlock(&g_disk_registry_mutex);
	}
	device_file_set_priv(file, bf);
	blk_bdev_put(bdev); /* blk_bdev_open holds its own */
	return 0;

fail_open:
	blk_bdev_close(bdev);
fail_ref:
	blk_bdev_put(bdev);
	(void)refcount_dec_and_test(&n->ref);
	return rc;
}

static void blkdev_release(vfs_file_t *file)
{
	struct blk_file *bf = blk_file_of(file);

	if (!bf)
		return;
	device_file_set_priv(file, NULL);
	if (bf->excl) {
		blk_mutex_lock(&g_disk_registry_mutex);
		for (struct blk_file **pp = &g_excl_files; *pp;
		     pp = &(*pp)->excl_next) {
			if (*pp == bf) {
				*pp = bf->excl_next;
				break;
			}
		}
		blk_mutex_unlock(&g_disk_registry_mutex);
		blk_bdev_unclaim(bf->bdev, bf);
	}
	blk_bdev_close(bf->bdev);
	if (bf->node)
		(void)refcount_dec_and_test(&bf->node->ref);
	bf->magic = 0;
	kfree(bf);
	genhd_reap_deferred();
}

/* Device size in bytes. */
static uint64_t blkdev_size(const block_device_t *bdev)
{
	return (uint64_t)bdev->total_sectors * bdev->sector_size;
}

/* Move `bytes' between the user buffer and the device at byte offset
 * `pos', through a kernel bounce buffer.  Holds no lock of the handle: every
 * thread that reads or writes through one descriptor -- a stress tester's
 * 32 workers, a database's I/O threads -- runs its own requests, so the
 * device sees them side by side (that is what fills an NCQ queue).  Returns
 * the bytes moved, or a negative errno when nothing was. */
static long blkdev_xfer(struct blk_file *bf, void *ubuf, long bytes,
			uint64_t pos, int write)
{
	block_device_t *bdev = bf->bdev;
	uint32_t ss = bdev->sector_size ? bdev->sector_size : 512;
	uint64_t size, n, done = 0, bphys;
	uint32_t bcap, bpages;
	uint8_t *bounce;
	long rc = 0;

	if (bytes < 0)
		return -EINVAL;
	if (bytes == 0)
		return 0;
	if (bdev->flags & BDEV_F_DEAD)
		return -ENODEV;
	if (write) {
		if (!blk_open_writable(bf->flags))
			return -EBADF;
		if (blk_bdev_read_only(bdev))
			return -EPERM;
	}
	size = blkdev_size(bdev);
	if (pos >= size)
		return write ? -ENOSPC : 0;
	n = (uint64_t)bytes;
	if (n > size - pos)
		n = size - pos;
	/* As large as the transfer needs (plus a sector for a misaligned
	 * start and one for the tail), at most BLKDEV_BOUNCE_BYTES. */
	{
		uint64_t want = n + 2ULL * ss;

		want = (want + ss - 1) / ss * ss;
		bcap = want > BLKDEV_BOUNCE_BYTES ? BLKDEV_BOUNCE_BYTES :
						    (uint32_t)want;
	}
	/* Whole pages, starting at a page boundary: a 64 KiB transfer is then
	 * sixteen page-aligned pages, which the driver describes with the
	 * fewest scatter/gather entries and no partial first page.  Not
	 * zeroed: every byte that goes out was either read from the device
	 * or copied from the caller first. */
	bpages = (bcap + PAGE_SIZE - 1) / PAGE_SIZE;
	bphys = mm_allocate_contiguous_pages(bpages);
	if (bphys) {
		bounce = (uint8_t *)phys_to_virt(bphys);
	} else {
		/* No contiguous run of that size free: the general allocator
		 * still serves the transfer, only not page-aligned. */
		bounce = (uint8_t *)kalloc(bcap);
		if (!bounce)
			return -ENOMEM;
	}
	while (done < n) {
		uint32_t off = (uint32_t)(pos % ss);
		uint64_t lba = pos / ss;
		uint64_t span = n - done;
		uint32_t nsect;
		int r;

		if (span > bcap - off)
			span = bcap - off;
		nsect = (uint32_t)((off + span + ss - 1) / ss);
		if (!write) {
			r = blk_read_sync(bdev, lba, nsect, bounce);
			if (r) {
				rc = r;
				break;
			}
			if (copy_to_user((uint8_t *)ubuf + done, bounce + off,
					 (size_t)span)) {
				rc = -EFAULT;
				break;
			}
		} else {
			uint32_t tail = (uint32_t)((off + span) % ss);

			/* A partial first or last sector: read it first so
			 * the bytes around the write survive. */
			if (off) {
				r = blk_read_sync(bdev, lba, 1, bounce);
				if (r) {
					rc = r;
					break;
				}
			}
			if (tail && (nsect > 1 || !off)) {
				r = blk_read_sync(bdev, lba + nsect - 1, 1,
						  bounce + (size_t)(nsect - 1) * ss);
				if (r) {
					rc = r;
					break;
				}
			}
			if (copy_from_user(bounce + off, (uint8_t *)ubuf + done,
					   (size_t)span)) {
				rc = -EFAULT;
				break;
			}
			r = blk_write_sync(bdev, lba, nsect, bounce, 0);
			if (r) {
				rc = r;
				break;
			}
		}
		pos += span;
		done += span;
	}
	if (bphys)
		mm_free_contiguous_pages(bphys, bpages);
	else
		kfree(bounce);
	if (done)
		return (long)done;
	return rc;
}

/* read()/write(): at the handle's position, which moves by what was
 * transferred.  The position is read and advanced under bf->io; the
 * transfer itself runs unlocked, as on any production system, where
 * threads sharing a block device descriptor through read()/write() race on
 * its position (pread()/pwrite() are what such programs use). */
static long blkdev_rw(struct blk_file *bf, void *ubuf, long bytes, int write)
{
	uint64_t pos;
	long r;

	blk_mutex_lock(&bf->io);
	pos = bf->pos;
	blk_mutex_unlock(&bf->io);
	r = blkdev_xfer(bf, ubuf, bytes, pos, write);
	if (r > 0) {
		blk_mutex_lock(&bf->io);
		bf->pos = pos + (uint64_t)r;
		blk_mutex_unlock(&bf->io);
	}
	return r;
}

static long blkdev_pread(vfs_file_t *file, void *buf, long bytes, long off)
{
	struct blk_file *bf = blk_file_of(file);

	if (!bf)
		return -EBADF;
	if (off < 0)
		return -EINVAL;
	return blkdev_xfer(bf, buf, bytes, (uint64_t)off, 0);
}

static long blkdev_pwrite(vfs_file_t *file, const void *buf, long bytes,
			  long off)
{
	struct blk_file *bf = blk_file_of(file);

	if (!bf)
		return -EBADF;
	if (off < 0)
		return -EINVAL;
	return blkdev_xfer(bf, (void *)buf, bytes, (uint64_t)off, 1);
}

static long blkdev_read(vfs_file_t *file, void *buf, long bytes, int nonblock)
{
	struct blk_file *bf = blk_file_of(file);

	(void)nonblock;
	if (!bf)
		return -EBADF;
	return blkdev_rw(bf, buf, bytes, 0);
}

static long blkdev_write(vfs_file_t *file, const void *buf, long bytes,
			 int nonblock)
{
	struct blk_file *bf = blk_file_of(file);

	(void)nonblock;
	if (!bf)
		return -EBADF;
	return blkdev_rw(bf, (void *)buf, bytes, 1);
}

static long blkdev_seek(vfs_file_t *file, long offset, int whence)
{
	struct blk_file *bf = blk_file_of(file);
	int64_t base, np;
	uint64_t size;

	if (!bf)
		return -EBADF;
	size = blkdev_size(bf->bdev);
	blk_mutex_lock(&bf->io);
	switch (whence) {
	case 0: /* SEEK_SET */
		base = 0;
		break;
	case 1: /* SEEK_CUR */
		base = (int64_t)bf->pos;
		break;
	case 2: /* SEEK_END */
		base = (int64_t)size;
		break;
	case SEEK_DATA:
		/* everything is data */
		if (offset < 0 || (uint64_t)offset >= size) {
			blk_mutex_unlock(&bf->io);
			return -ENXIO;
		}
		bf->pos = (uint64_t)offset;
		blk_mutex_unlock(&bf->io);
		return offset;
	case SEEK_HOLE:
		if (offset < 0 || (uint64_t)offset >= size) {
			blk_mutex_unlock(&bf->io);
			return -ENXIO;
		}
		bf->pos = size;
		blk_mutex_unlock(&bf->io);
		return (long)size;
	default:
		blk_mutex_unlock(&bf->io);
		return -EINVAL;
	}
	np = base + offset;
	if (np < 0) {
		blk_mutex_unlock(&bf->io);
		return -EINVAL;
	}
	bf->pos = (uint64_t)np;
	blk_mutex_unlock(&bf->io);
	return (long)np;
}

static int blkdev_fsync(vfs_file_t *file)
{
	struct blk_file *bf = blk_file_of(file);

	if (!bf)
		return -EBADF;
	return blk_flush_sync(bf->bdev);
}

static int blkdev_fstat(vfs_file_t *file, struct kstat *st)
{
	struct blk_file *bf = blk_file_of(file);

	/* A block device reports no size of its own (BLKGETSIZE64 does);
	 * the preferred I/O size is the page. */
	st->st_size = 0;
	st->st_blocks = 0;
	st->st_blksize = bf ? bf->bsize : 4096;
	return 0;
}

static long blkdev_node_ioctl(vfs_file_t *file, unsigned long req,
			      void *argp, struct task *cur)
{
	struct blk_file *bf = blk_file_of(file);

	if (!bf)
		return -EBADF;
	/* The two requests that belong to the handle. */
	if (req == BLKBSZGET) {
		int v = (int)bf->bsize;
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	}
	if (req == BLKBSZSET) {
		int v;
		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;
		if (v < (int)bf->bdev->sector_size || v > 4096 || (v & (v - 1)))
			return -EINVAL;
		bf->bsize = (uint32_t)v;
		return 0;
	}
	return blkdev_ioctl(bf->bdev, req, argp, cur, bf->flags);
}

static const struct device_ops g_blkdev_node_ops = {
	.open = blkdev_open,
	.release = blkdev_release,
	.read = blkdev_read,
	.write = blkdev_write,
	.seek = blkdev_seek,
	.ioctl = blkdev_node_ioctl,
	.fstat = blkdev_fstat,
	.fsync = blkdev_fsync,
	.pread = blkdev_pread,
	.pwrite = blkdev_pwrite,
};

/* ==========================================================================
 * ioctls
 * ========================================================================== */

static long put_int(void *argp, int v)
{
	return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
}

static long put_uint(void *argp, unsigned int v)
{
	return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
}

static long put_u64(void *argp, uint64_t v)
{
	return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
}

static long put_ushort(void *argp, unsigned short v)
{
	return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
}

/* BLKDISCARD / BLKSECDISCARD / BLKZEROOUT: {start, length} in bytes. */
static long blkdev_range_op(block_device_t *bdev, unsigned long cmd,
			    void *argp, int open_flags)
{
	uint64_t r[2];
	uint32_t ss = bdev->sector_size;
	uint64_t size = blkdev_size(bdev);

	if (!blk_open_writable(open_flags))
		return -EBADF;
	if (blk_bdev_read_only(bdev))
		return -EPERM;
	if (copy_from_user(r, argp, sizeof(r)))
		return -EFAULT;
	if ((r[0] % ss) || (r[1] % ss))
		return -EINVAL;
	if (r[0] + r[1] < r[0] || r[0] + r[1] > size)
		return -EINVAL;
	if (!r[1])
		return 0;
	/* BLKZEROOUT writes zeroes: the blocks stay allocated (no TRIM in
	 * its place, even on a drive that reads trimmed blocks as zero). */
	if (cmd == BLKZEROOUT)
		return blk_zeroout_sync(bdev, r[0] / ss, r[1] / ss,
					BLK_REQ_NOUNMAP);
	return blk_discard_sync(bdev, r[0] / ss, r[1] / ss,
				cmd == BLKSECDISCARD);
}

static long blkdev_blkpg(block_device_t *bdev, void *argp)
{
	struct blkpg_ioctl_arg a;
	struct blkpg_partition p;
	struct gendisk *disk = bdev->disk;
	uint32_t ss = bdev->sector_size;

	if (!capable())
		return -EACCES;
	if (bdev->part)
		return -EINVAL; /* only on the whole disk */
	if (copy_from_user(&a, argp, sizeof(a)))
		return -EFAULT;
	if (a.datalen < (int)sizeof(p) || !a.data)
		return -EINVAL;
	if (copy_from_user(&p, a.data, sizeof(p)))
		return -EFAULT;
	if (p.pno <= 0)
		return -EINVAL;
	switch (a.op) {
	case BLKPG_ADD_PARTITION:
		if (p.start < 0 || p.length <= 0 || (p.start % ss) ||
		    (p.length % ss))
			return -EINVAL;
		return blk_partition_add(disk, p.pno, (uint64_t)p.start / ss,
					 (uint64_t)p.length / ss);
	case BLKPG_DEL_PARTITION:
		return blk_partition_del(disk, p.pno);
	case BLKPG_RESIZE_PARTITION:
		if (p.length <= 0 || (p.length % ss))
			return -EINVAL;
		/* the start may not move */
		blk_mutex_lock(&g_disk_registry_mutex);
		if (p.pno > DISK_MAX_PARTS || !disk->part[p.pno]) {
			blk_mutex_unlock(&g_disk_registry_mutex);
			return -ENXIO;
		}
		if ((uint64_t)p.start != disk->part[p.pno]->start_lba * ss) {
			blk_mutex_unlock(&g_disk_registry_mutex);
			return -EINVAL;
		}
		blk_mutex_unlock(&g_disk_registry_mutex);
		return blk_partition_resize(disk, p.pno,
					    (uint64_t)p.length / ss);
	default:
		return -EINVAL;
	}
}

static long blkdev_getgeo(block_device_t *bdev, void *argp)
{
	struct gendisk *disk = bdev->disk;
	struct hd_geometry g;
	uint64_t s512 = (uint64_t)disk->capacity * disk->bdev.sector_size / 512;
	uint64_t cyl;

	mm_memset(&g, 0, sizeof(g));
	if (disk->heads && disk->sectors_per_track) {
		g.heads = (unsigned char)disk->heads;
		g.sectors = (unsigned char)disk->sectors_per_track;
		cyl = disk->cylinders ? disk->cylinders :
					s512 / (disk->heads *
						disk->sectors_per_track);
	} else {
		g.heads = 255;
		g.sectors = 63;
		cyl = s512 / (255 * 63);
	}
	g.cylinders = cyl > 65535 ? 65535 : (unsigned short)cyl;
	g.start = (unsigned long)(bdev->start_lba * bdev->sector_size / 512);
	return copy_to_user(argp, &g, sizeof(g)) ? -EFAULT : 0;
}

long blkdev_ioctl(block_device_t *bdev, unsigned long cmd, void *argp,
		  struct task *cur, int open_flags)
{
	struct gendisk *disk;
	struct blk_queue *q;
	int slot;

	if (!bdev || !blk_bdev_is_extended(bdev) || !bdev->disk)
		return -ENODEV;
	blk_might_sleep();
	disk = bdev->disk;
	q = disk->queue;
	if ((bdev->flags & BDEV_F_DEAD) || (disk->flags & GENHD_FL_DEAD))
		return -ENODEV;

	/* The driver's own requests first (SG_IO, HDIO_*, eject ...). */
	if (disk->ops && (disk->ops->ioctl_mode || disk->ops->ioctl)) {
		long r = disk->ops->ioctl_mode ?
				 disk->ops->ioctl_mode(bdev, cmd, argp, cur,
						       open_flags) :
				 disk->ops->ioctl(bdev, cmd, argp, cur);
		if (r != -ENOTTY)
			return r;
	}
	slot = bdev->registry_index >= 0 &&
			       bdev->registry_index < BLOCK_MAX_DEVICES ?
		       bdev->registry_index :
		       0;

	switch (cmd) {
	case BLKGETSIZE:
		return put_u64(argp, blkdev_size(bdev) / 512);
	case BLKGETSIZE64:
		return put_u64(argp, blkdev_size(bdev));
	case BLKSSZGET:
		return put_int(argp, (int)bdev->sector_size);
	case BLKPBSZGET:
		return put_uint(argp, bdev->physical_sector_size ?
					      bdev->physical_sector_size :
					      bdev->sector_size);
	case BLKIOMIN:
		return put_uint(argp, bdev->io_min ? bdev->io_min :
						     bdev->physical_sector_size);
	case BLKIOOPT:
		return put_uint(argp, bdev->io_opt);
	case BLKALIGNOFF:
		return put_int(argp, (int)bdev->alignment_offset);
	case BLKROGET:
		return put_int(argp, blk_bdev_read_only(bdev));
	case BLKROSET: {
		int v;
		if (!capable())
			return -EACCES;
		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;
		blk_mutex_lock(&g_disk_registry_mutex);
		bdev->ro = v ? 1 : 0;
		if (!bdev->part)
			disk_set_ro(disk, v);
		blk_mutex_unlock(&g_disk_registry_mutex);
		kprintf(BLK_LOG_PREFIX "%s: set %s\n", bdev->name,
			v ? "read-only" : "read-write");
		return 0;
	}
	case BLKFLSBUF:
		if (!capable())
			return -EACCES;
		return blk_flush_sync(bdev);
	case BLKRRPART:
		if (!capable())
			return -EACCES;
		if (bdev->part)
			return -EINVAL;
		return blk_partition_rescan(disk);
	case BLKPG:
		return blkdev_blkpg(bdev, argp);
	case BLKDISCARD:
	case BLKSECDISCARD:
	case BLKZEROOUT:
		return blkdev_range_op(bdev, cmd, argp, open_flags);
	case BLKGETDISKSEQ:
		return put_u64(argp, disk->diskseq);
	case BLKDISCARDZEROES:
		return put_uint(argp, 0);
	case BLKROTATIONAL:
		return put_ushort(argp, bdev->rotational ? 1 : 0);
	case BLKSECTGET: {
		uint64_t v = (uint64_t)bdev->max_sectors * bdev->sector_size / 512;
		return put_ushort(argp, v > 65535 ? 65535 : (unsigned short)v);
	}
	case BLKSECTSET:
		return -EINVAL; /* obsolete */
	case BLKRAGET:
	case BLKFRAGET:
		return put_u64(argp, q ? (uint64_t)q->read_ahead_kb * 2 : 0);
	case BLKRASET:
	case BLKFRASET:
		/* the value itself is the argument, in 512-byte sectors */
		if (!capable())
			return -EACCES;
		if (q)
			q->read_ahead_kb = (uint32_t)((unsigned long)argp / 2);
		return 0;
	case HDIO_GETGEO:
		return blkdev_getgeo(bdev, argp);

	/* The informational SCSI generic requests, for a driver that does not
	 * answer them itself. */
	case SG_GET_VERSION_NUM:
		return put_int(argp, SG_VERSION_NUM);
	case SG_EMULATED_HOST:
		return put_int(argp, 0);
	case SG_GET_SCSI_ID: {
		struct sg_scsi_id id;
		mm_memset(&id, 0, sizeof(id));
		id.host_no = disk->host_no;
		id.channel = disk->channel;
		id.scsi_id = disk->target;
		id.lun = disk->lun;
		id.scsi_type = disk->scsi_type;
		id.h_cmd_per_lun = (short)(q ? q->depth : 1);
		id.d_queue_depth = (short)(q ? q->depth : 1);
		return copy_to_user(argp, &id, sizeof(id)) ? -EFAULT : 0;
	}
	case SG_GET_RESERVED_SIZE:
		return put_int(argp, g_sg_reserved[slot] ? g_sg_reserved[slot] :
							   SG_DEF_RESERVED_SIZE);
	case SG_SET_RESERVED_SIZE: {
		int v;
		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;
		if (v < 0)
			return -EINVAL;
		g_sg_reserved[slot] = v > 1024 * 1024 ? 1024 * 1024 : v;
		return 0;
	}
	case SG_GET_TIMEOUT:
		/* the value is the result, in USER_HZ (100) ticks */
		return g_sg_timeout[slot] ? g_sg_timeout[slot] :
					    SG_DEFAULT_TIMEOUT_MS / 10;
	case SG_SET_TIMEOUT: {
		int v;
		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;
		if (v < 0)
			return -EINVAL;
		g_sg_timeout[slot] = v;
		return 0;
	}
	case SG_GET_SG_TABLESIZE:
		return put_int(argp, q ? (int)q->limits.max_segments : 1);
	case SG_GET_COMMAND_Q:
		return put_int(argp, q && q->depth > 1);
	case SG_GET_LOW_DMA:
	case SG_GET_NUM_WAITING:
		return put_int(argp, 0);
	case SG_GET_PACK_ID:
		return put_int(argp, -1);
	case SG_GET_ACCESS_COUNT:
		return put_int(argp, bdev->open_count);
	case SCSI_IOCTL_GET_IDLUN: {
		struct scsi_idlun l;
		l.dev_id = (unsigned)(disk->target & 0xFF) |
			   ((unsigned)(disk->lun & 0xFF) << 8) |
			   ((unsigned)(disk->channel & 0xFF) << 16) |
			   ((unsigned)(disk->host_no & 0xFF) << 24);
		l.host_unique_id = (unsigned)(disk->host_no + 1);
		return copy_to_user(argp, &l, sizeof(l)) ? -EFAULT : 0;
	}
	case SCSI_IOCTL_GET_BUS_NUMBER:
		return put_int(argp, disk->host_no);
	default:
		return -ENOTTY;
	}
}
