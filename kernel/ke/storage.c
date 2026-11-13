// LikeOS storage bootstrap: finding and mounting the root filesystem
//
// See storage.h for the rules.  This file implements them on top of the
// disk layer's registry (kernel/dev/block/blkdev.h):
//
//   * The bootloader's identity of the boot partition and the boot command
//     line are copied out of boot_info very early (storage_boot_info_save),
//     because boot_info lives in low memory that is unmapped later.
//
//   * storage_fs_poll() runs on the kdevpoll thread.  Disks keep appearing
//     after boot -- a USB stick is enumerated by that same thread seconds
//     after the SATA disks were registered at boot -- so the search is a
//     small state machine: every STORAGE_SCAN_MS it collects the current
//     candidates (every partition of every registered disk, plus disks that
//     hold a filesystem without a partition table; never optical drives),
//     applies the active rule, and when the rule names a candidate that has
//     stayed ready for STORAGE_SETTLE_MS, mounts it.
//
//   * Candidates are only ever READ while the rules decide (superblock and
//     directory probes through the device's read callback); the one
//     partition chosen is the only thing handed to ext4_mount/fat32_mount,
//     at offset 0 -- the partition device itself, so the filesystem drivers
//     never see the rest of the disk.
//
//   * Devices registered the old way (block_register() without the disk
//     layer: magic != BLOCK_DEVICE_MAGIC) are still supported.  Their
//     partition table is parsed here (with the disk layer's pure parser) and
//     each partition is presented as a small offset view, so the rules and
//     the mount work on them exactly as on disk-layer partitions.
//
// Diagnostics: everything this file decides is logged with the "[storage] "
// prefix -- the identity the bootloader passed, the candidates, which rule
// chose the root and why, and every PARTUUID involved.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h> /* first: it includes ke/syscall.h */
#include <kernel/ke/storage.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/userinit.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/sysfont.h>
#include <kernel/io/cursor.h>
#include <kernel/dev/input/mouse.h>
/* Display drivers finish their bring-up once there is a filesystem to
 * read firmware from and a scheduler to give them a thread. */
void drm_late_init(void);
void snd_late_init(void);
#include <kernel/fs/pagecache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/ext4.h>
#include <kernel/uapi/bug.h>

/* ==========================================================================
 * Tunables
 * ========================================================================== */

/* How often the candidates are collected and the rule is applied.  The poll
 * itself runs every kdevpoll loop (about a tick); scanning that often would
 * only repeat the same registry walk. */
#define STORAGE_SCAN_MS 20
/* How long the chosen device must stay ready before it is mounted: a USB
 * stick that just finished its first commands, a disk whose link just came
 * up.  The old search waited 20 polls; this is the same in time. */
#define STORAGE_SETTLE_MS 40
/* How long root= may name a device that does not exist before the identity
 * rule takes over (with a loud warning).  A production system would wait
 * forever; here a typo in cmdline.txt must not make the machine unbootable
 * when the bootloader's identity can still find the root. */
#define STORAGE_ROOT_SPEC_WAIT_MS 30000
/* Interval of the "still waiting" report. */
#define STORAGE_REPORT_MS 5000
/* How long the identity rule waits for the loader's disk before it accepts
 * the partition the bootloader read kernel.elf from on ANOTHER disk (an ESP
 * on a disk this kernel has no driver for, the root on one it has). */
#define STORAGE_LOADER_GRACE_MS 10000

#define STORAGE_MAX_CANDS 128 /* partitions + whole disks considered */
#define STORAGE_MAX_VIEWS 32 /* partitions of legacy devices */
#define STORAGE_MAX_FAILED 16 /* devices whose mount failed */
#define STORAGE_PROBE_CACHE 128 /* probed filesystem identities */

/* Read-only probe limits */
#define STO_EXT4_MAX_DIR_BLOCKS 4096 /* root directory blocks scanned */
#define STO_EXT4_MAX_DEPTH 5 /* extent tree depth */
#define STO_FAT_MAX_CLUSTERS 4096 /* root directory clusters scanned */
#define STO_READ_CHUNK_SECTORS 64 /* bounce buffer for byte reads */

/* Filesystem kinds the probe recognises */
#define STO_FS_UNKNOWN 0
#define STO_FS_NONE 1
#define STO_FS_EXT4 2 /* ext2/3/4 superblock */
#define STO_FS_FAT32 3

#define STO_PREFIX "[storage] "

/* ==========================================================================
 * The bootloader's identity and command line
 * ========================================================================== */

static struct {
	int present; /* boot_info carried BOOT_ROOTID_MAGIC */
	int have_identity; /* a usable loader PARTUUID */
	uint32_t sig_type; /* BOOT_PART_SIG_* */
	uint32_t part_number;
	uint64_t part_start, part_size;
	char loader_partuuid[40];
	uint32_t disk_bus, disk_port;
	char device_path[256];
	char kernel_partuuid[40];
	char root_spec[128];
	char cmdline[512];
	uint32_t cmdline_source;
	char rootfstype[16]; /* rootfstype= ("" = auto) */
} g_bootid;

/* The parsed root= specification */
enum sto_spec_kind {
	SPEC_NONE = 0,
	SPEC_PARTUUID, /* PARTUUID=<guid>[/PARTNROFF=<n>] */
	SPEC_PARTLABEL, /* PARTLABEL=<GPT partition name> */
	SPEC_UUID, /* UUID=<filesystem uuid> */
	SPEC_LABEL, /* LABEL=<filesystem label> */
	SPEC_NAME, /* /dev/sda2 or sda2 */
	SPEC_DEVT, /* MAJ:MIN */
	SPEC_INVALID, /* given, but not understood */
};

static struct {
	enum sto_spec_kind kind;
	char value[128];
	int partnroff; /* PARTUUID=.../PARTNROFF=n, 0 none */
	uint32_t major, minor; /* SPEC_DEVT */
} g_spec;

/* ==========================================================================
 * Small string helpers
 * ========================================================================== */

static char sto_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int sto_eq_nocase(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b) {
		if (sto_lower(*a) != sto_lower(*b))
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

static int sto_eq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int sto_prefix(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s != *prefix)
			return 0;
		s++;
		prefix++;
	}
	return 1;
}

/* Bounded copy that always terminates and never reads past `srcmax'. */
static void sto_copy(char *dst, size_t cap, const char *src, size_t srcmax)
{
	size_t i = 0;
	if (!cap)
		return;
	if (src)
		for (; i + 1 < cap && i < srcmax && src[i]; i++)
			dst[i] = src[i];
	dst[i] = 0;
}

static int sto_parse_uint(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	int digits = 0;
	while (*s >= '0' && *s <= '9' && digits < 9) {
		v = v * 10 + (uint32_t)(*s - '0');
		s++;
		digits++;
	}
	if (!digits || *s)
		return 0;
	*out = v;
	return 1;
}

/* The value of word `key=' in the command line (the last one wins). */
static int sto_cmdline_get(const char *key, char *val, size_t cap)
{
	const char *s = g_bootid.cmdline;
	size_t klen = kstrlen(key);
	int found = 0;
	while (*s) {
		while (*s == ' ')
			s++;
		const char *w = s;
		while (*s && *s != ' ')
			s++;
		if ((size_t)(s - w) > klen && kstrncmp(w, key, klen) == 0 &&
		    w[klen] == '=') {
			sto_copy(val, cap, w + klen + 1,
				 (size_t)(s - (w + klen + 1)));
			found = 1;
		}
	}
	return found;
}

/* Is `s' a canonical GUID text ("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx") or
 * an MBR partition id ("xxxxxxxx-nn")? */
static int sto_partuuid_wellformed(const char *s)
{
	size_t n = kstrlen(s);
	if (n != 36 && n != 11)
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = sto_lower(s[i]);
		int dash = (n == 36) ? (i == 8 || i == 13 || i == 18 ||
					i == 23) :
				       (i == 8);
		if (dash) {
			if (c != '-')
				return 0;
		} else if (!((c >= '0' && c <= '9') ||
			     (c >= 'a' && c <= 'f'))) {
			return 0;
		}
	}
	return 1;
}

static const char *sto_bus_name(uint32_t bus)
{
	switch (bus) {
	case BOOT_DISK_BUS_USB:
		return "usb";
	case BOOT_DISK_BUS_SATA:
		return "sata";
	case BOOT_DISK_BUS_NVME:
		return "nvme";
	case BOOT_DISK_BUS_SCSI:
		return "scsi";
	case BOOT_DISK_BUS_ATAPI:
		return "ata";
	case BOOT_DISK_BUS_SD:
		return "sd/mmc";
	case BOOT_DISK_BUS_VIRTUAL:
		return "virtual";
	default:
		return "unknown";
	}
}

/* Does a disk-layer bus name ("ata", "usb", "scsi") match the bus the
 * firmware reached the loader disk by?  Used only to break a tie between
 * two disks carrying the same PARTUUID (a cloned image). */
static int sto_bus_matches(const char *disk_bus, uint32_t boot_bus)
{
	if (!disk_bus || !disk_bus[0])
		return 0;
	switch (boot_bus) {
	case BOOT_DISK_BUS_USB:
		return sto_eq(disk_bus, "usb");
	case BOOT_DISK_BUS_SATA:
	case BOOT_DISK_BUS_ATAPI:
		return sto_eq(disk_bus, "ata");
	case BOOT_DISK_BUS_SCSI:
		return sto_eq(disk_bus, "scsi");
	case BOOT_DISK_BUS_NVME:
		return sto_eq(disk_bus, "nvme");
	default:
		return 0;
	}
}

/* ==========================================================================
 * Boot identity: saved from boot_info, parsed, reported
 * ========================================================================== */

static void sto_parse_root_spec(void)
{
	const char *s = g_bootid.root_spec;

	g_spec.kind = SPEC_NONE;
	g_spec.value[0] = 0;
	g_spec.partnroff = 0;
	if (!s[0])
		return;

	/* The /dev/disk/by-* link forms name the same identities. */
	if (sto_prefix(s, "/dev/disk/by-partuuid/")) {
		g_spec.kind = SPEC_PARTUUID;
		s += 22;
	} else if (sto_prefix(s, "/dev/disk/by-partlabel/")) {
		g_spec.kind = SPEC_PARTLABEL;
		s += 23;
	} else if (sto_prefix(s, "/dev/disk/by-uuid/")) {
		g_spec.kind = SPEC_UUID;
		s += 18;
	} else if (sto_prefix(s, "/dev/disk/by-label/")) {
		g_spec.kind = SPEC_LABEL;
		s += 19;
	} else if (sto_prefix(s, "PARTUUID=")) {
		g_spec.kind = SPEC_PARTUUID;
		s += 9;
	} else if (sto_prefix(s, "PARTLABEL=")) {
		g_spec.kind = SPEC_PARTLABEL;
		s += 10;
	} else if (sto_prefix(s, "UUID=")) {
		g_spec.kind = SPEC_UUID;
		s += 5;
	} else if (sto_prefix(s, "LABEL=")) {
		g_spec.kind = SPEC_LABEL;
		s += 6;
	} else if (sto_prefix(s, "/dev/")) {
		g_spec.kind = SPEC_NAME;
		s += 5;
	} else {
		/* MAJ:MIN in decimal, or a bare device name */
		const char *colon = s;
		while (*colon && *colon != ':')
			colon++;
		if (*colon == ':') {
			char maj[12];
			sto_copy(maj, sizeof(maj), s, (size_t)(colon - s));
			if (sto_parse_uint(maj, &g_spec.major) &&
			    sto_parse_uint(colon + 1, &g_spec.minor) &&
			    kdev_valid(g_spec.major, g_spec.minor)) {
				g_spec.kind = SPEC_DEVT;
				sto_copy(g_spec.value, sizeof(g_spec.value), s,
					 sizeof(g_spec.value));
				return;
			}
			g_spec.kind = SPEC_INVALID;
			return;
		}
		g_spec.kind = SPEC_NAME;
	}
	sto_copy(g_spec.value, sizeof(g_spec.value), s, sizeof(g_spec.value));
	if (!g_spec.value[0]) {
		g_spec.kind = SPEC_INVALID;
		return;
	}
	if (g_spec.kind == SPEC_PARTUUID) {
		/* PARTUUID=<id>/PARTNROFF=<n>: the partition n numbers after
		 * (or before) the one with that id, on the same disk. */
		char *slash = g_spec.value;
		while (*slash && *slash != '/')
			slash++;
		if (*slash == '/') {
			*slash = 0;
			const char *off = slash + 1;
			int neg = 0;
			uint32_t v;
			if (!sto_prefix(off, "PARTNROFF=")) {
				g_spec.kind = SPEC_INVALID;
				return;
			}
			off += 10;
			if (*off == '-') {
				neg = 1;
				off++;
			} else if (*off == '+') {
				off++;
			}
			if (!sto_parse_uint(off, &v) || v > DISK_MAX_PARTS) {
				g_spec.kind = SPEC_INVALID;
				return;
			}
			g_spec.partnroff = neg ? -(int)v : (int)v;
		}
		if (!sto_partuuid_wellformed(g_spec.value))
			g_spec.kind = SPEC_INVALID;
	}
	if (g_spec.kind == SPEC_NAME) {
		/* a device name: letters and digits only */
		for (const char *p = g_spec.value; *p; p++) {
			char c = *p;
			if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			      (c >= 'A' && c <= 'Z'))) {
				g_spec.kind = SPEC_INVALID;
				return;
			}
		}
	}
}

void storage_boot_info_save(const boot_info_t *bi)
{
	BUILD_BUG_ON(sizeof(g_bootid.loader_partuuid) !=
		     sizeof(bi->loader_partuuid));
	BUILD_BUG_ON(sizeof(g_bootid.root_spec) != sizeof(bi->root_spec));
	BUILD_BUG_ON(sizeof(g_bootid.cmdline) != sizeof(bi->cmdline));

	mm_memset(&g_bootid, 0, sizeof(g_bootid));
	g_spec.kind = SPEC_NONE;
	if (!bi || bi->rootid_magic != BOOT_ROOTID_MAGIC)
		return;

	g_bootid.present = 1;
	g_bootid.sig_type = bi->loader_part_sig_type;
	g_bootid.part_number = bi->loader_part_number;
	g_bootid.part_start = bi->loader_part_start;
	g_bootid.part_size = bi->loader_part_size;
	/* Every string is copied bounded and terminated: boot_info is data
	 * from another program and is not trusted to be well formed. */
	sto_copy(g_bootid.loader_partuuid, sizeof(g_bootid.loader_partuuid),
		 bi->loader_partuuid, sizeof(bi->loader_partuuid));
	g_bootid.disk_bus = bi->loader_disk_bus;
	g_bootid.disk_port = bi->loader_disk_port;
	sto_copy(g_bootid.device_path, sizeof(g_bootid.device_path),
		 bi->loader_device_path, sizeof(bi->loader_device_path));
	sto_copy(g_bootid.kernel_partuuid, sizeof(g_bootid.kernel_partuuid),
		 bi->kernel_partuuid, sizeof(bi->kernel_partuuid));
	sto_copy(g_bootid.root_spec, sizeof(g_bootid.root_spec),
		 bi->root_spec, sizeof(bi->root_spec));
	sto_copy(g_bootid.cmdline, sizeof(g_bootid.cmdline), bi->cmdline,
		 sizeof(bi->cmdline));
	g_bootid.cmdline_source = bi->cmdline_source;

	/* A loader identity is usable when it is a GPT GUID or an MBR id in
	 * the text form the disk layer uses for by-partuuid. */
	g_bootid.have_identity =
		(g_bootid.sig_type == BOOT_PART_SIG_GPT ||
		 g_bootid.sig_type == BOOT_PART_SIG_MBR) &&
		sto_partuuid_wellformed(g_bootid.loader_partuuid);
	if (g_bootid.kernel_partuuid[0] &&
	    !sto_partuuid_wellformed(g_bootid.kernel_partuuid))
		g_bootid.kernel_partuuid[0] = 0;

	/* root= from the bootloader; when the bootloader is new enough to pass
	 * a command line but did not extract root= (it always does), the
	 * command line is consulted directly. */
	if (!g_bootid.root_spec[0])
		sto_cmdline_get("root", g_bootid.root_spec,
				sizeof(g_bootid.root_spec));
	sto_cmdline_get("rootfstype", g_bootid.rootfstype,
			sizeof(g_bootid.rootfstype));
	sto_parse_root_spec();
}

static void sto_report_identity(void)
{
	if (!g_bootid.present) {
		kprintf(STO_PREFIX
			"WARNING: the bootloader passed no root identity -- this is an old "
			"bootloader (EFI/BOOT/BOOTX64.EFI on the ESP); falling back to the "
			"legacy /LIKEOS.SIG search. Rebuild and rewrite the image to boot by "
			"partition identity.\n");
		return;
	}
	if (g_bootid.have_identity) {
		kprintf(STO_PREFIX
			"boot identity: loader partition %u (%s) PARTUUID=%s, disk bus %s",
			g_bootid.part_number,
			g_bootid.sig_type == BOOT_PART_SIG_GPT ? "GPT" : "MBR",
			g_bootid.loader_partuuid,
			sto_bus_name(g_bootid.disk_bus));
		if (g_bootid.disk_port != ~0u)
			kprintf(" port %u", g_bootid.disk_port);
		kprintf(", start LBA %llu, %llu blocks\n",
			(unsigned long long)g_bootid.part_start,
			(unsigned long long)g_bootid.part_size);
	} else {
		kprintf(STO_PREFIX
			"boot identity: the loader was not started from a disk partition "
			"(sig type %u, \"%s\") -- no identity to find the root by\n",
			g_bootid.sig_type, g_bootid.loader_partuuid);
	}
	if (g_bootid.device_path[0])
		kprintf(STO_PREFIX "loader device path: %s\n",
			g_bootid.device_path);
	if (g_bootid.kernel_partuuid[0])
		kprintf(STO_PREFIX
			"bootloader read kernel.elf from PARTUUID=%s\n",
			g_bootid.kernel_partuuid);
	if (g_bootid.cmdline[0])
		kprintf(STO_PREFIX "command line (%s): %s\n",
			g_bootid.cmdline_source == BOOT_CMDLINE_LOAD_OPTIONS ?
				"boot entry load options" :
			g_bootid.cmdline_source == BOOT_CMDLINE_ESP_FILE ?
				"ESP \\EFI\\LIKEOS\\cmdline.txt" :
				"bootloader",
			g_bootid.cmdline);
	if (g_bootid.root_spec[0])
		kprintf(STO_PREFIX "root=%s%s\n", g_bootid.root_spec,
			g_spec.kind == SPEC_INVALID ?
				" -- NOT UNDERSTOOD, ignored (use PARTUUID=, "
				"PARTLABEL=, UUID=, LABEL=, /dev/sdXN or MAJ:MIN)" :
				"");
	if (g_bootid.rootfstype[0])
		kprintf(STO_PREFIX "rootfstype=%s\n", g_bootid.rootfstype);
}

/* ==========================================================================
 * Byte-granular reads through a device's read callback
 * ========================================================================== */

static unsigned int sto_ss(const block_device_t *b)
{
	unsigned int ss = b->sector_size;
	if (ss < 512 || ss > 65536 || (ss & (ss - 1)))
		ss = 512;
	return ss;
}

/* Read `len' bytes at byte offset `off' of `b' into `dst'.  0 or -errno.
 * Goes through the device's legacy read callback (for a disk-layer device
 * that is the request queue), so it works for every kind of device. */
static int sto_read_bytes(const block_device_t *b, uint64_t off, uint32_t len,
			  void *dst)
{
	unsigned int ss;
	uint8_t *bounce;
	uint8_t *out = (uint8_t *)dst;

	if (!b || !b->read || !len)
		return -EINVAL;
	ss = sto_ss(b);
	if (b->total_sectors &&
	    (off + len + ss - 1) / ss > (uint64_t)b->total_sectors)
		return -EINVAL;
	bounce = (uint8_t *)kalloc((size_t)ss * STO_READ_CHUNK_SECTORS);
	if (!bounce)
		return -ENOMEM;
	while (len) {
		uint64_t lba = off / ss;
		uint32_t skip = (uint32_t)(off % ss);
		uint32_t want = skip + len;
		uint32_t nsec = (want + ss - 1) / ss;
		if (nsec > STO_READ_CHUNK_SECTORS)
			nsec = STO_READ_CHUNK_SECTORS;
		int st = b->read((block_device_t *)b, (unsigned long)lba,
				 nsec, bounce);
		if (st != ST_OK) {
			kfree(bounce);
			return -EIO;
		}
		uint32_t got = nsec * ss - skip;
		if (got > len)
			got = len;
		mm_memcpy(out, bounce + skip, got);
		out += got;
		off += got;
		len -= got;
	}
	kfree(bounce);
	return 0;
}

static uint16_t sto_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t sto_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void sto_hex(char **p, const uint8_t *b, int n)
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < n; i++) {
		*(*p)++ = hx[b[i] >> 4];
		*(*p)++ = hx[b[i] & 15];
	}
}

/* ==========================================================================
 * Read-only ext4 probe: superblock identity and a root-directory lookup
 * ==========================================================================
 * A deliberately small reader, independent of the ext4 driver: the driver's
 * mount replays the journal and may upgrade metadata -- it WRITES -- and the
 * rules must be able to look at any partition (the legacy /LIKEOS.SIG
 * search in particular) without changing it. */

#define STO_EXT4_MAGIC 0xEF53
#define STO_EXT4_EXT_MAGIC 0xF30A
#define STO_EXT4_EXTENTS_FL 0x00080000u
#define STO_EXT4_INLINE_FL 0x10000000u
#define STO_EXT4_INCOMPAT_64BIT 0x0080u

struct sto_ext4 {
	const block_device_t *b;
	uint32_t block_size;
	uint32_t inodes_per_group;
	uint32_t inode_size;
	uint32_t desc_size;
	uint64_t gdt_block;
	uint64_t blocks_count;
	uint8_t uuid[16];
	char label[17];
	uint8_t *blk; /* one filesystem block */
	uint8_t *node; /* one extent-tree node */
};

/* Superblock: 0 and *e filled (buffers not allocated), -ENOENT not ext4,
 * -EIO / -EINVAL. */
static int sto_ext4_super(const block_device_t *b, struct sto_ext4 *e)
{
	uint8_t sb[1024];
	int rc;

	mm_memset(e, 0, sizeof(*e));
	rc = sto_read_bytes(b, 1024, sizeof(sb), sb);
	if (rc)
		return rc;
	if (sto_le16(sb + 56) != STO_EXT4_MAGIC)
		return -ENOENT;
	uint32_t log = sto_le32(sb + 24);
	if (log > 6) /* 1 KiB .. 64 KiB */
		return -EINVAL;
	e->b = b;
	e->block_size = 1024u << log;
	e->inodes_per_group = sto_le32(sb + 40);
	e->inode_size = (sto_le32(sb + 76) == 0) ? 128 : sto_le16(sb + 88);
	if (e->inode_size < 128 || e->inode_size > e->block_size)
		e->inode_size = 128;
	e->desc_size = 32;
	if (sto_le32(sb + 96) & STO_EXT4_INCOMPAT_64BIT) {
		uint16_t ds = sto_le16(sb + 254);
		e->desc_size = (ds >= 32 && ds <= 1024) ? ds : 32;
	}
	e->gdt_block = (uint64_t)sto_le32(sb + 20) + 1;
	e->blocks_count = (uint64_t)sto_le32(sb + 4) |
			  ((sto_le32(sb + 96) & STO_EXT4_INCOMPAT_64BIT) ?
				   ((uint64_t)sto_le32(sb + 336) << 32) :
				   0);
	mm_memcpy(e->uuid, sb + 104, 16);
	mm_memcpy(e->label, sb + 120, 16);
	e->label[16] = 0;
	if (!e->inodes_per_group)
		return -EINVAL;
	return 0;
}

static int sto_ext4_read_block(struct sto_ext4 *e, uint64_t blk, uint8_t *buf)
{
	if (e->blocks_count && blk >= e->blocks_count)
		return -EINVAL;
	return sto_read_bytes(e->b, blk * e->block_size, e->block_size, buf);
}

/* Read inode `ino' (its first min(inode_size, 256) bytes) into out[256]. */
static int sto_ext4_inode(struct sto_ext4 *e, uint32_t ino, uint8_t *out)
{
	uint8_t gd[64];
	uint32_t group = (ino - 1) / e->inodes_per_group;
	uint32_t index = (ino - 1) % e->inodes_per_group;
	uint64_t gd_off = e->gdt_block * e->block_size +
			  (uint64_t)group * e->desc_size;
	uint32_t gd_len = e->desc_size >= 64 ? 64 : 32;
	int rc = sto_read_bytes(e->b, gd_off, gd_len, gd);
	if (rc)
		return rc;
	uint64_t itbl = sto_le32(gd + 8);
	if (e->desc_size >= 64)
		itbl |= (uint64_t)sto_le32(gd + 40) << 32;
	uint32_t n = e->inode_size > 256 ? 256 : e->inode_size;
	mm_memset(out, 0, 256);
	return sto_read_bytes(e->b,
			      itbl * e->block_size +
				      (uint64_t)index * e->inode_size,
			      n, out);
}

/* Logical block -> physical block of an inode (0 = hole / not mapped). */
static uint64_t sto_ext4_bmap(struct sto_ext4 *e, const uint8_t *inode,
			      uint64_t lblk)
{
	uint32_t flags = sto_le32(inode + 32);
	if (flags & STO_EXT4_EXTENTS_FL) {
		const uint8_t *node = inode + 40;
		uint32_t node_cap = 60;
		for (int depth_walk = 0; depth_walk <= STO_EXT4_MAX_DEPTH;
		     depth_walk++) {
			if (sto_le16(node) != STO_EXT4_EXT_MAGIC)
				return 0;
			uint16_t entries = sto_le16(node + 2);
			uint16_t depth = sto_le16(node + 6);
			if (12u + (uint32_t)entries * 12u > node_cap)
				return 0;
			if (depth == 0) {
				for (uint16_t i = 0; i < entries; i++) {
					const uint8_t *x = node + 12 + i * 12;
					uint32_t first = sto_le32(x);
					uint16_t len = sto_le16(x + 4);
					if (len > 32768) /* uninitialised */
						continue;
					if (lblk >= first && lblk < first + len)
						return (((uint64_t)sto_le16(x + 6)
							 << 32) |
							sto_le32(x + 8)) +
						       (lblk - first);
				}
				return 0;
			}
			uint64_t child = 0;
			int found = 0;
			for (uint16_t i = 0; i < entries; i++) {
				const uint8_t *x = node + 12 + i * 12;
				if (sto_le32(x) > lblk)
					break;
				child = ((uint64_t)sto_le16(x + 8) << 32) |
					sto_le32(x + 4);
				found = 1;
			}
			if (!found || sto_ext4_read_block(e, child, e->node))
				return 0;
			node = e->node;
			node_cap = e->block_size;
		}
		return 0;
	}
	/* classic block map: direct blocks and one level of indirection
	 * (a root directory never needs more) */
	if (lblk < 12)
		return sto_le32(inode + 40 + lblk * 4);
	lblk -= 12;
	if (lblk < e->block_size / 4) {
		uint32_t ind = sto_le32(inode + 40 + 12 * 4);
		if (!ind || sto_ext4_read_block(e, ind, e->node))
			return 0;
		return sto_le32(e->node + lblk * 4);
	}
	return 0;
}

/* Scan dirents in buf[0, len) for `name'.  1 found, 0 not. */
static int sto_ext4_scan_dirents(const uint8_t *buf, uint32_t len,
				 const char *name)
{
	size_t nlen = kstrlen(name);
	uint32_t off = 0;
	while (off + 8 <= len) {
		uint32_t ino = sto_le32(buf + off);
		uint16_t rec = sto_le16(buf + off + 4);
		uint8_t nl = buf[off + 6];
		if (rec < 8 || (rec & 3) || off + rec > len)
			return 0; /* damaged: stop, never loop */
		if (ino && nl == nlen && 8u + nl <= rec &&
		    kmemcmp(buf + off + 8, name, nlen) == 0)
			return 1;
		off += rec;
	}
	return 0;
}

/* Is there an entry `name' in the root directory?  1 yes, 0 no, -errno. */
static int sto_ext4_root_has(const block_device_t *b, const char *name)
{
	struct sto_ext4 e;
	uint8_t inode[256];
	int rc = sto_ext4_super(b, &e);
	if (rc)
		return rc;
	rc = sto_ext4_inode(&e, 2, inode);
	if (rc)
		return rc;
	if ((sto_le16(inode) & 0xF000) != 0x4000) /* not a directory */
		return -EINVAL;
	uint64_t size = sto_le32(inode + 4) | ((uint64_t)sto_le32(inode + 108)
					       << 32);
	if (sto_le32(inode + 32) & STO_EXT4_INLINE_FL) {
		/* inline directory: 4-byte parent, then dirents in i_block */
		return sto_ext4_scan_dirents(inode + 44, 56, name);
	}
	e.blk = (uint8_t *)kalloc(e.block_size);
	e.node = (uint8_t *)kalloc(e.block_size);
	if (!e.blk || !e.node) {
		kfree(e.blk);
		kfree(e.node);
		return -ENOMEM;
	}
	uint64_t nblocks = (size + e.block_size - 1) / e.block_size;
	if (nblocks > STO_EXT4_MAX_DIR_BLOCKS)
		nblocks = STO_EXT4_MAX_DIR_BLOCKS;
	int found = 0;
	for (uint64_t l = 0; l < nblocks && !found; l++) {
		uint64_t p = sto_ext4_bmap(&e, inode, l);
		if (!p || sto_ext4_read_block(&e, p, e.blk))
			continue;
		found = sto_ext4_scan_dirents(e.blk, e.block_size, name);
	}
	kfree(e.blk);
	kfree(e.node);
	return found;
}

/* ==========================================================================
 * Read-only FAT32 probe
 * ========================================================================== */

struct sto_fat {
	const block_device_t *b;
	uint32_t bps; /* bytes per sector (BPB) */
	uint32_t spc; /* sectors per cluster */
	uint32_t reserved;
	uint32_t num_fats;
	uint32_t fat_size;
	uint32_t root_cluster;
	uint64_t data_start; /* sector */
	uint64_t total_sectors;
	uint32_t vol_id;
	int has_vol_id;
	char label[12];
};

/* FAT32 boot sector at offset 0.  0, -ENOENT not FAT32, -EIO. */
static int sto_fat_super(const block_device_t *b, struct sto_fat *f)
{
	uint8_t bs[512];
	int rc;

	mm_memset(f, 0, sizeof(*f));
	rc = sto_read_bytes(b, 0, sizeof(bs), bs);
	if (rc)
		return rc;
	if (!((bs[0] == 0xEB && bs[2] == 0x90) || bs[0] == 0xE9))
		return -ENOENT;
	f->b = b;
	f->bps = sto_le16(bs + 11);
	f->spc = bs[13];
	f->reserved = sto_le16(bs + 14);
	f->num_fats = bs[16];
	f->fat_size = sto_le32(bs + 36);
	f->root_cluster = sto_le32(bs + 44);
	f->total_sectors = sto_le16(bs + 19) ? sto_le16(bs + 19) :
					       sto_le32(bs + 32);
	if (!(f->bps == 512 || f->bps == 1024 || f->bps == 2048 ||
	      f->bps == 4096) ||
	    !f->spc || (f->spc & (f->spc - 1)) || !f->reserved ||
	    !f->num_fats || f->num_fats > 2 || sto_le16(bs + 17) != 0 ||
	    sto_le16(bs + 22) != 0 || !f->fat_size || f->root_cluster < 2 ||
	    kmemcmp(bs + 82, "FAT", 3) != 0)
		return -ENOENT;
	f->data_start = f->reserved + (uint64_t)f->num_fats * f->fat_size;
	if (bs[66] == 0x29) {
		f->vol_id = sto_le32(bs + 67);
		f->has_vol_id = 1;
		sto_copy(f->label, sizeof(f->label), (const char *)bs + 71,
			 11);
	}
	/* trailing blanks are padding; "NO NAME" is the "no label" value */
	for (int i = 10; i >= 0 && (f->label[i] == ' ' || !f->label[i]); i--)
		f->label[i] = 0;
	if (sto_eq(f->label, "NO NAME"))
		f->label[0] = 0;
	return 0;
}

static uint32_t sto_fat_next(struct sto_fat *f, uint32_t cl)
{
	uint8_t ent[4];
	uint64_t off = (uint64_t)f->reserved * f->bps + (uint64_t)cl * 4;
	if (sto_read_bytes(f->b, off, 4, ent))
		return 0x0FFFFFFF;
	return sto_le32(ent) & 0x0FFFFFFF;
}

/* Scan the root directory: is the 8.3 entry `name83' (11 bytes, blank
 * padded) present?  The volume-label entry, when there is one, replaces
 * the boot sector's label (it is what tools report).  1/0/-errno. */
static int sto_fat_root_scan(struct sto_fat *f, const char *name83)
{
	uint32_t cl = f->root_cluster;
	uint8_t *buf = (uint8_t *)kalloc(f->bps);
	int found = 0, end = 0;

	if (!buf)
		return -ENOMEM;
	for (uint32_t n = 0; n < STO_FAT_MAX_CLUSTERS && !found && !end &&
			     cl >= 2 && cl < 0x0FFFFFF8;
	     n++) {
		uint64_t first = f->data_start + (uint64_t)(cl - 2) * f->spc;
		for (uint32_t s = 0; s < f->spc && !found && !end; s++) {
			if (sto_read_bytes(f->b, (first + s) * f->bps, f->bps,
					   buf)) {
				end = 1;
				break;
			}
			for (uint32_t o = 0; o + 32 <= f->bps; o += 32) {
				const uint8_t *d = buf + o;
				if (d[0] == 0) {
					end = 1;
					break;
				}
				if (d[0] == 0xE5 || d[11] == 0x0F)
					continue;
				if (d[11] & 0x08) { /* volume label */
					sto_copy(f->label, sizeof(f->label),
						 (const char *)d, 11);
					for (int i = 10;
					     i >= 0 && (f->label[i] == ' ' ||
							!f->label[i]);
					     i--)
						f->label[i] = 0;
					continue;
				}
				if (name83 && kmemcmp(d, name83, 11) == 0) {
					found = 1;
					break;
				}
			}
		}
		if (!found && !end)
			cl = sto_fat_next(f, cl);
	}
	kfree(buf);
	return found;
}

/* ==========================================================================
 * Partitions of legacy devices: offset views
 * ========================================================================== */

struct sto_view {
	block_device_t bdev; /* legacy device: magic 0 */
	const block_device_t *parent;
	int parent_slot;
	int partno;
	uint64_t start, count;
	char name[DISK_NAME_LEN + 8];
	char partuuid[BLK_GUID_STR_LEN];
	char partlabel[72];
	struct blk_guid type;
	uint64_t attrs;
};

static struct sto_view g_views[STORAGE_MAX_VIEWS];
static int g_nviews;
static uint64_t g_legacy_parsed; /* registry slots whose table was read */

static int sto_view_read(block_device_t *dev, unsigned long lba,
			 unsigned long count, void *buf)
{
	struct sto_view *v = (struct sto_view *)dev->driver_data;
	if (!v || !v->parent || !v->parent->read)
		return ST_NO_DEVICE;
	if (lba >= v->count || count > v->count - lba)
		return ST_INVALID;
	return v->parent->read((block_device_t *)v->parent,
			       (unsigned long)(v->start + lba), count, buf);
}

static int sto_view_write(block_device_t *dev, unsigned long lba,
			  unsigned long count, const void *buf)
{
	struct sto_view *v = (struct sto_view *)dev->driver_data;
	if (!v || !v->parent || !v->parent->write)
		return ST_NO_DEVICE;
	if (lba >= v->count || count > v->count - lba)
		return ST_INVALID;
	return v->parent->write((block_device_t *)v->parent,
				(unsigned long)(v->start + lba), count, buf);
}

static int sto_view_sync(block_device_t *dev)
{
	struct sto_view *v = (struct sto_view *)dev->driver_data;
	if (!v || !v->parent)
		return ST_NO_DEVICE;
	if (!v->parent->sync)
		return ST_OK;
	return v->parent->sync((block_device_t *)v->parent);
}

static int sto_view_release(block_device_t *dev, uint64_t task_id)
{
	struct sto_view *v = (struct sto_view *)dev->driver_data;
	if (!v || !v->parent || !v->parent->release_locks_for_task)
		return 0;
	return v->parent->release_locks_for_task((block_device_t *)v->parent,
						 task_id);
}

static int sto_pt_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
	const block_device_t *b = (const block_device_t *)ctx;
	return b->read((block_device_t *)b, (unsigned long)lba, count, buf) ==
			       ST_OK ?
		       0 :
		       -EIO;
}

/* Read the partition table of legacy registry slot `slot' once and create
 * a view per partition. */
static void sto_legacy_parse(int slot, const block_device_t *b)
{
	struct blk_pt_result *res;
	struct blk_pt_reader rd;

	if (g_legacy_parsed & (1ULL << slot))
		return;
	g_legacy_parsed |= 1ULL << slot;
	res = (struct blk_pt_result *)kalloc(sizeof(*res));
	if (!res) {
		kprintf(STO_PREFIX "%s: no memory for its partition table\n",
			b->name ? b->name : "?");
		return;
	}
	mm_memset(res, 0, sizeof(*res));
	rd.read = sto_pt_read;
	rd.ctx = (void *)b;
	rd.sector_size = sto_ss(b);
	rd.nr_sectors = b->total_sectors;
	int rc = blk_parse_partitions(&rd, res);
	if (rc || res->type == BLK_PT_NONE) {
		kfree(res);
		return; /* a whole-device filesystem (or nothing) */
	}
	kprintf(STO_PREFIX "%s (legacy device): %s partition table, %d entries%s%s\n",
		b->name ? b->name : "?", res->type == BLK_PT_GPT ? "GPT" : "MBR",
		res->nr, res->warn[0] ? " -- " : "", res->warn);
	for (int i = 0; i < res->nr && g_nviews < STORAGE_MAX_VIEWS; i++) {
		const struct blk_pt_entry *pe = &res->e[i];
		if (pe->flags & PART_FL_EXTENDED || !pe->nr_sectors)
			continue;
		if (pe->start_lba >= b->total_sectors ||
		    pe->nr_sectors > b->total_sectors - pe->start_lba)
			continue;
		struct sto_view *v = &g_views[g_nviews++];
		mm_memset(v, 0, sizeof(*v));
		v->parent = b;
		v->parent_slot = slot;
		v->partno = pe->partno;
		v->start = pe->start_lba;
		v->count = pe->nr_sectors;
		v->type = pe->type_guid;
		v->attrs = pe->gpt_attrs;
		sto_copy(v->partlabel, sizeof(v->partlabel), pe->name,
			 sizeof(pe->name));
		if (res->type == BLK_PT_GPT)
			blk_guid_to_str(&pe->part_guid, v->partuuid);
		else
			ksnprintf(v->partuuid, sizeof(v->partuuid),
				  "%08x-%02x", res->mbr_signature,
				  (unsigned)pe->partno);
		if (disk_part_name(b->name ? b->name : "legacy", pe->partno,
				   v->name, sizeof(v->name)))
			ksnprintf(v->name, sizeof(v->name), "legacy%dp%d",
				  slot, pe->partno);
		v->bdev.name = v->name;
		v->bdev.sector_size = b->sector_size;
		v->bdev.total_sectors = (unsigned long)v->count;
		v->bdev.read = sto_view_read;
		v->bdev.write = sto_view_write;
		v->bdev.sync = sto_view_sync;
		v->bdev.release_locks_for_task = sto_view_release;
		v->bdev.driver_data = v;
		v->bdev.registry_index = -1;
	}
	kfree(res);
}

/* ==========================================================================
 * Candidates
 * ========================================================================== */

struct sto_cand {
	block_device_t *bdev; /* what would be mounted */
	struct gendisk *disk; /* disk-layer disk, NULL for a legacy device */
	const void *disk_key; /* identifies the disk (gendisk or legacy dev) */
	int refd; /* we hold a blk_bdev_get() reference */
	int partno; /* 0 = whole device */
	int ready;
	uint64_t diskseq;
	uint64_t start_lba;
	uint64_t attrs;
	struct blk_guid type;
	const char *bus; /* "ata", "usb" ... or "" */
	char name[DISK_NAME_LEN + 8];
	char partuuid[BLK_GUID_STR_LEN];
	char partlabel[72];
};

static struct sto_cand g_cands[STORAGE_MAX_CANDS];
static int g_ncand;

static int sto_disk_ready(const struct gendisk *d)
{
	if (!d || d->magic != GENDISK_MAGIC)
		return 0;
	if (d->flags & (GENHD_FL_DEAD | GENHD_FL_SUSPENDED))
		return 0;
	if (d->bdev.flags & BDEV_F_DEAD)
		return 0;
	return d->bdev.ready != 0;
}

static void sto_add_disk_part(struct gendisk *d, struct blk_partition *p)
{
	struct sto_cand *c;
	if (g_ncand >= STORAGE_MAX_CANDS)
		return;
	if (p && ((p->flags & PART_FL_EXTENDED) || (p->bdev.flags & BDEV_F_DEAD)))
		return;
	c = &g_cands[g_ncand];
	mm_memset(c, 0, sizeof(*c));
	c->disk = d;
	c->disk_key = d;
	c->bus = d->bus;
	c->diskseq = d->diskseq;
	c->ready = sto_disk_ready(d);
	if (p) {
		c->bdev = &p->bdev;
		c->partno = p->partno;
		c->start_lba = p->start_lba;
		c->attrs = p->gpt_attrs;
		c->type = p->type_guid;
		sto_copy(c->partuuid, sizeof(c->partuuid), p->partuuid,
			 sizeof(p->partuuid));
		sto_copy(c->partlabel, sizeof(c->partlabel), p->name,
			 sizeof(p->name));
		sto_copy(c->name, sizeof(c->name),
			 p->bdev.name ? p->bdev.name : p->dev_name,
			 sizeof(c->name));
		if (p->bdev.flags & BDEV_F_DEAD)
			c->ready = 0;
	} else {
		c->bdev = &d->bdev;
		c->type = BLK_GUID_ZERO;
		sto_copy(c->name, sizeof(c->name), d->disk_name,
			 sizeof(d->disk_name));
	}
	/* This blk_bdev_get() keeps the device (and its disk) alive while the rules
	 * look at it after the registry mutex is dropped. */
	blk_bdev_get(c->bdev);
	c->refd = 1;
	g_ncand++;
}

/* disk_for_each() callback, registry mutex held: no lookups, no I/O. */
static int sto_collect_disk(struct gendisk *d, void *arg)
{
	(void)arg;
	if (!d || d->magic != GENDISK_MAGIC)
		return 0;
	if (d->kind == BLK_DISK_SR || (d->bdev.flags & BDEV_F_CDROM))
		return 0; /* optical drives are never root candidates */
	if (d->flags & (GENHD_FL_DEAD | GENHD_FL_HIDDEN))
		return 0;
	if (!(d->flags & GENHD_FL_UP) && !(d->bdev.flags & BDEV_F_REGISTERED))
		return 0; /* disk_add() not finished: partitions incomplete */
	int any = 0;
	/* every slot: nr_parts is a high-water mark, not relied upon */
	for (int i = 1; i <= DISK_MAX_PARTS; i++) {
		if (d->part[i]) {
			sto_add_disk_part(d, d->part[i]);
			any = 1;
		}
	}
	/* A disk without a partition table may hold a filesystem itself. */
	if (!any && d->pt_type == BLK_PT_NONE)
		sto_add_disk_part(d, NULL);
	return 0;
}

static void sto_collect_legacy(void)
{
	int n = block_count();
	if (n > BLOCK_MAX_DEVICES)
		n = BLOCK_MAX_DEVICES;
	for (int slot = 0; slot < n; slot++) {
		const block_device_t *b = block_get(slot);
		if (!b || blk_bdev_is_extended(b) || !b->read)
			continue;
		sto_legacy_parse(slot, b);
		int any = 0;
		for (int i = 0; i < g_nviews && g_ncand < STORAGE_MAX_CANDS;
		     i++) {
			struct sto_view *v = &g_views[i];
			if (v->parent_slot != slot || v->parent != b)
				continue;
			struct sto_cand *c = &g_cands[g_ncand++];
			mm_memset(c, 0, sizeof(*c));
			c->bdev = &v->bdev;
			c->disk_key = b;
			c->bus = "";
			c->ready = 1; /* registered only once usable */
			c->partno = v->partno;
			c->start_lba = v->start;
			c->attrs = v->attrs;
			c->type = v->type;
			sto_copy(c->name, sizeof(c->name), v->name,
				 sizeof(v->name));
			sto_copy(c->partuuid, sizeof(c->partuuid), v->partuuid,
				 sizeof(v->partuuid));
			sto_copy(c->partlabel, sizeof(c->partlabel),
				 v->partlabel, sizeof(v->partlabel));
			any = 1;
		}
		if (!any && g_ncand < STORAGE_MAX_CANDS) {
			struct sto_cand *c = &g_cands[g_ncand++];
			mm_memset(c, 0, sizeof(*c));
			c->bdev = (block_device_t *)b;
			c->disk_key = b;
			c->bus = "";
			c->ready = 1;
			c->type = BLK_GUID_ZERO;
			sto_copy(c->name, sizeof(c->name),
				 b->name ? b->name : "legacy", DISK_NAME_LEN);
		}
	}
}

static void sto_collect(void)
{
	g_ncand = 0;
	disk_for_each(sto_collect_disk, NULL);
	sto_collect_legacy();
}

static void sto_release(void)
{
	for (int i = 0; i < g_ncand; i++) {
		if (g_cands[i].refd) {
			blk_bdev_put(g_cands[i].bdev);
			g_cands[i].refd = 0;
		}
	}
	g_ncand = 0;
}

/* ==========================================================================
 * Filesystem identity of candidates (cached)
 * ========================================================================== */

struct sto_probe {
	const block_device_t *bdev; /* key (compared, never dereferenced) */
	uint64_t diskseq; /* key: a media change invalidates */
	int fs; /* STO_FS_* */
	int sig; /* /LIKEOS.SIG: -1 not looked, 0 no, 1 yes */
	char uuid[40];
	char label[64];
};

static struct sto_probe g_probe[STORAGE_PROBE_CACHE];
static int g_probe_next;

static struct sto_probe *sto_probe_slot(const struct sto_cand *c)
{
	for (int i = 0; i < STORAGE_PROBE_CACHE; i++)
		if (g_probe[i].bdev == c->bdev &&
		    g_probe[i].diskseq == c->diskseq)
			return &g_probe[i];
	struct sto_probe *p = &g_probe[g_probe_next];
	g_probe_next = (g_probe_next + 1) % STORAGE_PROBE_CACHE;
	mm_memset(p, 0, sizeof(*p));
	p->bdev = c->bdev;
	p->diskseq = c->diskseq;
	p->fs = STO_FS_UNKNOWN;
	p->sig = -1;
	return p;
}

/* Probe (once) what filesystem `c' holds, its UUID and label; with
 * `want_sig' also whether its root directory holds LIKEOS.SIG. */
static struct sto_probe *sto_probe(const struct sto_cand *c, int want_sig)
{
	struct sto_probe *p = sto_probe_slot(c);
	if (!c->ready)
		return p;
	if (p->fs == STO_FS_UNKNOWN) {
		struct sto_ext4 e;
		struct sto_fat f;
		if (sto_ext4_super(c->bdev, &e) == 0) {
			char *q = p->uuid;
			p->fs = STO_FS_EXT4;
			sto_hex(&q, e.uuid, 4);
			*q++ = '-';
			sto_hex(&q, e.uuid + 4, 2);
			*q++ = '-';
			sto_hex(&q, e.uuid + 6, 2);
			*q++ = '-';
			sto_hex(&q, e.uuid + 8, 2);
			*q++ = '-';
			sto_hex(&q, e.uuid + 10, 6);
			*q = 0;
			sto_copy(p->label, sizeof(p->label), e.label,
				 sizeof(e.label));
		} else if (sto_fat_super(c->bdev, &f) == 0) {
			p->fs = STO_FS_FAT32;
			if (f.has_vol_id)
				ksnprintf(p->uuid, sizeof(p->uuid), "%04X-%04X",
					  f.vol_id >> 16, f.vol_id & 0xFFFF);
			sto_fat_root_scan(&f, NULL); /* volume label entry */
			sto_copy(p->label, sizeof(p->label), f.label,
				 sizeof(f.label));
		} else {
			p->fs = STO_FS_NONE;
		}
		/* The disk layer's own identity probe is what /dev/disk/by-uuid
		 * and by-label are made of; where it answers, its strings are
		 * the ones root=UUID= / LABEL= must match. */
		if (blk_bdev_is_extended(c->bdev)) {
			struct blk_fs_identity id;
			mm_memset(&id, 0, sizeof(id));
			if (blk_probe_fs_identity(c->bdev, &id) == 0 &&
			    id.valid) {
				if (id.uuid[0])
					sto_copy(p->uuid, sizeof(p->uuid),
						 id.uuid, sizeof(id.uuid));
				if (id.label[0])
					sto_copy(p->label, sizeof(p->label),
						 id.label, sizeof(id.label));
			}
		}
	}
	if (want_sig && p->sig < 0) {
		int rc = 0;
		if (p->fs == STO_FS_EXT4) {
			rc = sto_ext4_root_has(c->bdev, "LIKEOS.SIG");
		} else if (p->fs == STO_FS_FAT32) {
			struct sto_fat f;
			rc = sto_fat_super(c->bdev, &f);
			if (rc == 0)
				rc = sto_fat_root_scan(&f, "LIKEOS  SIG");
		}
		p->sig = rc > 0 ? 1 : 0;
	}
	return p;
}

static const char *sto_fs_name(int fs)
{
	switch (fs) {
	case STO_FS_EXT4:
		return "ext4";
	case STO_FS_FAT32:
		return "vfat";
	case STO_FS_NONE:
		return "none";
	default:
		return "?";
	}
}

/* ==========================================================================
 * Failed devices (a mount that did not succeed is not retried)
 * ========================================================================== */

static struct {
	const block_device_t *bdev;
	uint64_t diskseq;
} g_failed[STORAGE_MAX_FAILED];
static int g_nfailed;

static int sto_failed(const struct sto_cand *c)
{
	for (int i = 0; i < g_nfailed; i++)
		if (g_failed[i].bdev == c->bdev &&
		    g_failed[i].diskseq == c->diskseq)
			return 1;
	return 0;
}

static void sto_mark_failed(const struct sto_cand *c)
{
	if (g_nfailed < STORAGE_MAX_FAILED) {
		g_failed[g_nfailed].bdev = c->bdev;
		g_failed[g_nfailed].diskseq = c->diskseq;
		g_nfailed++;
	} else {
		/* forget the oldest */
		for (int i = 1; i < STORAGE_MAX_FAILED; i++)
			g_failed[i - 1] = g_failed[i];
		g_failed[STORAGE_MAX_FAILED - 1].bdev = c->bdev;
		g_failed[STORAGE_MAX_FAILED - 1].diskseq = c->diskseq;
	}
}

/* ==========================================================================
 * The mounted root
 * ========================================================================== */

static struct {
	int mounted;
	const block_device_t *bdev;
	int claimed;
	int rule;
	int fs;
	char name[DISK_NAME_LEN + 8];
	char partuuid[BLK_GUID_STR_LEN];
} g_root;

static ext4_fs_t s_ext4_root;
static fat32_fs_t s_fat_root;

const char *storage_root_device_name(void)
{
	return g_root.mounted ? g_root.name : NULL;
}

const char *storage_root_fs_type(void)
{
	if (!g_root.mounted)
		return NULL;
	return g_root.fs == STO_FS_FAT32 ? "vfat" : "ext4";
}

const block_device_t *storage_root_bdev(void)
{
	return g_root.mounted ? g_root.bdev : NULL;
}

int storage_root_rule(void)
{
	return g_root.mounted ? g_root.rule : STORAGE_RULE_NONE;
}

const char *storage_boot_cmdline(void)
{
	return g_bootid.cmdline;
}

static const char *sto_rule_name(int rule)
{
	switch (rule) {
	case STORAGE_RULE_ROOT_SPEC:
		return "root= on the boot command line";
	case STORAGE_RULE_IDENTITY:
		return "loader disk + x86-64 root partition type";
	case STORAGE_RULE_LEGACY:
		return "legacy /LIKEOS.SIG search (no boot identity)";
	default:
		return "none";
	}
}

/* Everything that follows a root mount -- unchanged from the original
 * bootstrap: the caches, the console font, the mouse cursor, and the late
 * bring-up of display and sound drivers (firmware files live on the root). */
static void sto_after_mount(void)
{
	pagecache_init();
	dcache_init();
	icache_init();

	vfs_file_t *sf = 0;
	if (vfs_open("/LIKEOS.SIG", 0, &sf) == ST_OK) {
		vfs_close(sf);
		kprintf(STO_PREFIX "/LIKEOS.SIG present on the root\n");
	} else if (g_root.rule != STORAGE_RULE_LEGACY) {
		kprintf(STO_PREFIX
			"note: no /LIKEOS.SIG on the root (not needed: the root was "
			"chosen by identity)\n");
	}

	if (sysfont_load("/res/Uni2-Terminus16.psf") == 0)
		console_apply_sysfont();
	if (cursor_load("/res/left_ptr") == 0)
		mouse_apply_cursor();
	/* firmware loads and device bring-up: the first login prompt waits
	 * for them */
	boot_work_begin();
	drm_late_init();
	snd_late_init();
	boot_work_end();
	userinit_redisplay_prompt();
}

/* Mount candidate `c' as the root.  1 mounted, 0 failed (marked). */
static int sto_mount(storage_fs_state_t *state, struct sto_cand *c, int rule)
{
	struct sto_probe *p = sto_probe(c, 0);
	int want = p->fs;
	int rc_ext4 = ST_NOT_FOUND, rc_fat = ST_NOT_FOUND;

	if (g_bootid.rootfstype[0]) {
		if (sto_eq(g_bootid.rootfstype, "ext4") ||
		    sto_eq(g_bootid.rootfstype, "ext3") ||
		    sto_eq(g_bootid.rootfstype, "ext2"))
			want = STO_FS_EXT4;
		else if (sto_eq(g_bootid.rootfstype, "vfat") ||
			 sto_eq(g_bootid.rootfstype, "fat32") ||
			 sto_eq(g_bootid.rootfstype, "msdos"))
			want = STO_FS_FAT32;
		else if (!sto_eq(g_bootid.rootfstype, "auto"))
			kprintf(STO_PREFIX
				"rootfstype=%s is not supported (ext4, vfat, auto); "
				"detecting\n",
				g_bootid.rootfstype);
	}

	kprintf(STO_PREFIX "mounting root %s (PARTUUID=%s, filesystem %s) -- %s\n",
		c->name, c->partuuid[0] ? c->partuuid : "-",
		sto_fs_name(p->fs), sto_rule_name(rule));

	/* ext4 first (the default root), then FAT32 as the original bootstrap
	 * did -- but only on this one device, at its offset 0. */
	if (want != STO_FS_FAT32) {
		rc_ext4 = ext4_mount(c->bdev, &s_ext4_root);
		if (rc_ext4 == ST_OK) {
			ext4_vfs_register_root(&s_ext4_root);
			g_root.fs = STO_FS_EXT4;
			kprintf("EXT4: mount succeeded on %s\n", c->name);
		}
	}
	if (rc_ext4 != ST_OK && want != STO_FS_EXT4) {
		rc_fat = fat32_mount(c->bdev, &s_fat_root);
		if (rc_fat == ST_OK) {
			fat32_vfs_register_root(&s_fat_root);
			g_root.fs = STO_FS_FAT32;
			kprintf("FAT32: mount succeeded on %s\n", c->name);
		}
	}
	if (rc_ext4 != ST_OK && rc_fat != ST_OK) {
		kprintf(STO_PREFIX
			"ERROR: cannot mount %s as root (ext4: %d, FAT32: %d); "
			"it will not be tried again\n",
			c->name, rc_ext4, rc_fat);
		sto_mark_failed(c);
		userinit_redisplay_prompt();
		return 0;
	}

	g_root.mounted = 1;
	g_root.bdev = c->bdev;
	g_root.rule = rule;
	sto_copy(g_root.name, sizeof(g_root.name), c->name, sizeof(c->name));
	sto_copy(g_root.partuuid, sizeof(g_root.partuuid), c->partuuid,
		 sizeof(c->partuuid));
	/* The root keeps a reference of its own for as long as the system
	 * runs, and claims the device: BLKRRPART on its disk is refused while
	 * the root partition is mounted. */
	if (c->refd) {
		blk_bdev_get(c->bdev);
		int crc = blk_bdev_claim(c->bdev, &g_root);
		g_root.claimed = (crc == 0);
		if (crc)
			kprintf(STO_PREFIX
				"warning: could not claim %s for the root mount (%d)\n",
				c->name, crc);
	}
	state->signature_found = 1;
	kprintf(STO_PREFIX "root filesystem %s mounted from /dev/%s (PARTUUID=%s) -- %s\n",
		storage_root_fs_type(), g_root.name,
		g_root.partuuid[0] ? g_root.partuuid : "-",
		sto_rule_name(rule));
	if (g_bootid.kernel_partuuid[0] && g_root.partuuid[0] &&
	    !sto_eq_nocase(g_bootid.kernel_partuuid, g_root.partuuid))
		kprintf(STO_PREFIX
			"WARNING: the bootloader read the kernel from PARTUUID=%s, but the "
			"root is PARTUUID=%s -- kernel and root are from different "
			"partitions\n",
			g_bootid.kernel_partuuid, g_root.partuuid);
	sto_after_mount();
	return 1;
}

/* ==========================================================================
 * The rules
 * ========================================================================== */

/* Result of applying a rule to the current candidates */
#define STO_PICK_WAIT (-1) /* nothing yet: keep waiting */
#define STO_PICK_GIVEUP (-2) /* this rule cannot succeed: next rule */

/* The candidate index of the loader partition (identity rule), STO_PICK_WAIT
 * when it is not registered (yet).  Several matches are a cloned image (the
 * same PARTUUIDs on a USB stick and an internal disk): the disk on the bus
 * the firmware booted from wins, then the one at the same start LBA, then
 * the first -- with a warning, because the choice is a guess. */
static int sto_find_loader(void)
{
	int match[8], n = 0;
	for (int i = 0; i < g_ncand && n < 8; i++)
		if (g_cands[i].partno &&
		    sto_eq_nocase(g_cands[i].partuuid, g_bootid.loader_partuuid))
			match[n++] = i;
	if (n == 0)
		return STO_PICK_WAIT;
	if (n == 1)
		return match[0];

	static int warned;
	int best = -1, best_score = -1;
	for (int k = 0; k < n; k++) {
		const struct sto_cand *c = &g_cands[match[k]];
		int score = 0;
		if (sto_bus_matches(c->bus, g_bootid.disk_bus))
			score += 2;
		if (c->start_lba == g_bootid.part_start)
			score += 1;
		if (score > best_score) {
			best = match[k];
			best_score = score;
		}
	}
	if (!warned) {
		warned = 1;
		kprintf(STO_PREFIX
			"WARNING: %d partitions carry the loader PARTUUID %s (cloned disk "
			"image?):",
			n, g_bootid.loader_partuuid);
		for (int k = 0; k < n; k++)
			kprintf(" %s(%s)", g_cands[match[k]].name,
				g_cands[match[k]].bus && g_cands[match[k]].bus[0] ?
					g_cands[match[k]].bus :
					"?");
		kprintf("; using %s (boot bus %s)\n", g_cands[best].name,
			sto_bus_name(g_bootid.disk_bus));
	}
	return best;
}

/* The candidate holding the partition the bootloader read kernel.elf from,
 * -1 when there is none (or no such PARTUUID was passed). */
static int sto_find_kernel_part(void)
{
	if (!g_bootid.kernel_partuuid[0])
		return -1;
	for (int i = 0; i < g_ncand; i++)
		if (g_cands[i].partno && g_cands[i].ready &&
		    !sto_failed(&g_cands[i]) &&
		    sto_eq_nocase(g_cands[i].partuuid, g_bootid.kernel_partuuid))
			return i;
	return -1;
}

/* Rule (b): the loader disk's x86-64 root partition.  In order:
 *   1. on the loader's disk, the partition of the x86-64 root type (with
 *      several, the one kernel.elf was read from);
 *   2. the partition kernel.elf was read from, wherever it is (the
 *      bootloader chose it by the same ladder) -- also when the loader's
 *      own disk has not appeared after STORAGE_LOADER_GRACE_MS;
 *   3. on the loader's disk, the first ext4 (then FAT32) partition that is
 *      not the ESP: an image from before the root type was set. */
static int sto_rule_identity(const storage_fs_state_t *state)
{
	int li = sto_find_loader();
	if (li < 0) {
		int ki = sto_find_kernel_part();
		if (ki >= 0 && timer_ticks() - state->start_tick >=
				       timer_ms_to_ticks(STORAGE_LOADER_GRACE_MS)) {
			static int warned_grace;
			if (!warned_grace) {
				warned_grace = 1;
				kprintf(STO_PREFIX
					"WARNING: the loader's disk (PARTUUID=%s) is not "
					"visible to the kernel; using %s, the partition the "
					"bootloader read kernel.elf from\n",
					g_bootid.loader_partuuid, g_cands[ki].name);
			}
			return ki;
		}
		return li;
	}
	const struct sto_cand *loader = &g_cands[li];
	const void *disk = loader->disk_key;
	int roots[16], nroots = 0;

	for (int i = 0; i < g_ncand && nroots < 16; i++) {
		const struct sto_cand *c = &g_cands[i];
		if (c->disk_key != disk || !c->partno || sto_failed(c))
			continue;
		if (!blk_guid_equal(&c->type, &BLK_GUID_ROOT_X86_64))
			continue;
		if (c->attrs & BLK_GPT_ATTR_NO_AUTO) {
			static int noauto_warned;
			if (!noauto_warned) {
				noauto_warned = 1;
				kprintf(STO_PREFIX
					"%s: x86-64 root type, but its GPT 'no-auto' attribute "
					"(bit 63) is set -- skipped\n",
					c->name);
			}
			continue;
		}
		roots[nroots++] = i;
	}

	static int reported;
	if (nroots > 0) {
		int pick = roots[0];
		/* With several root partitions, the one the bootloader read the
		 * kernel from is this installation's. */
		for (int k = 0; k < nroots && g_bootid.kernel_partuuid[0]; k++)
			if (sto_eq_nocase(g_cands[roots[k]].partuuid,
					  g_bootid.kernel_partuuid))
				pick = roots[k];
		if (!reported) {
			reported = 1;
			kprintf(STO_PREFIX
				"loader partition %s found; root partition %s (type x86-64 "
				"root%s)",
				loader->name, g_cands[pick].name,
				nroots > 1 ? ", several on this disk" : "");
			if (nroots > 1) {
				kprintf(":");
				for (int k = 0; k < nroots; k++)
					kprintf(" %s", g_cands[roots[k]].name);
			}
			kprintf("\n");
			if (g_cands[pick].attrs & BLK_GPT_ATTR_READ_ONLY)
				kprintf(STO_PREFIX
					"note: %s has the GPT read-only attribute; the root is "
					"mounted read-write regardless\n",
					g_cands[pick].name);
		}
		return pick;
	}

	/* No partition of the root type on the loader disk.  The bootloader
	 * found kernel.elf somewhere by the same ladder: that partition is this
	 * installation's root. */
	{
		int ki = sto_find_kernel_part();
		if (ki >= 0) {
			static int warned_kpart;
			if (!warned_kpart) {
				warned_kpart = 1;
				kprintf(STO_PREFIX
					"no x86-64 root type partition on the loader disk; "
					"using %s, the partition the bootloader read "
					"kernel.elf from\n",
					g_cands[ki].name);
			}
			return ki;
		}
	}

	/* An image made before the root type was set (type 8300 "generic
	 * data").  Still the loader's own disk, so still this installation:
	 * take its first ext4 partition (then its first FAT32 one) other than
	 * the loader partition and the ESP, and say how to fix the type. */
	int fallback = -1, fallback_fat = -1, waiting = 0;
	for (int i = 0; i < g_ncand; i++) {
		const struct sto_cand *c = &g_cands[i];
		if (c->disk_key != disk || !c->partno || i == li ||
		    sto_failed(c))
			continue;
		if (blk_guid_equal(&c->type, &BLK_GUID_EFI_SYSTEM) ||
		    blk_guid_equal(&c->type, &BLK_GUID_SWAP) ||
		    (c->attrs & BLK_GPT_ATTR_NO_AUTO))
			continue;
		if (!c->ready) {
			waiting = 1;
			continue;
		}
		struct sto_probe *p = sto_probe(c, 0);
		if (p->fs == STO_FS_EXT4 && fallback < 0)
			fallback = i;
		else if (p->fs == STO_FS_FAT32 && fallback_fat < 0)
			fallback_fat = i;
	}
	if (fallback < 0)
		fallback = fallback_fat;
	if (fallback >= 0) {
		static int warned_type;
		if (!warned_type) {
			const char *tn = blk_guid_type_name(&g_cands[fallback].type);
			char ts[BLK_GUID_STR_LEN];
			blk_guid_to_str(&g_cands[fallback].type, ts);
			warned_type = 1;
			kprintf(STO_PREFIX
				"WARNING: no partition of the x86-64 root type on the loader "
				"disk; using %s (%s, type %s) -- an older image. Set the type "
				"with: sgdisk -t %d:4f68bce3-e8cd-4db1-96e7-fbcaf984b709 "
				"<disk>\n",
				g_cands[fallback].name,
				sto_fs_name(sto_probe(&g_cands[fallback], 0)->fs),
				tn ? tn : ts, g_cands[fallback].partno);
		}
		return fallback;
	}
	if (waiting)
		return STO_PICK_WAIT;
	{
		static int warned_none;
		if (!warned_none) {
			warned_none = 1;
			kprintf(STO_PREFIX
				"ERROR: the loader disk (partition %s) has no root partition: "
				"no x86-64 root type, no ext4 or FAT32 partition besides the "
				"ESP\n",
				loader->name);
		}
	}
	return STO_PICK_WAIT;
}

/* Does candidate `c' match the root= specification? */
static int sto_spec_matches(const struct sto_cand *c)
{
	switch (g_spec.kind) {
	case SPEC_PARTUUID:
		return c->partno && sto_eq_nocase(c->partuuid, g_spec.value);
	case SPEC_PARTLABEL:
		return c->partno && c->partlabel[0] &&
		       sto_eq(c->partlabel, g_spec.value);
	case SPEC_UUID:
		if (!c->ready)
			return 0;
		return sto_eq_nocase(sto_probe(c, 0)->uuid, g_spec.value);
	case SPEC_LABEL: {
		if (!c->ready)
			return 0;
		const char *l = sto_probe(c, 0)->label;
		return l[0] && sto_eq(l, g_spec.value);
	}
	case SPEC_NAME:
		return sto_eq(c->name, g_spec.value);
	case SPEC_DEVT:
		return blk_bdev_is_extended(c->bdev) &&
		       c->bdev->major == g_spec.major &&
		       c->bdev->minor == g_spec.minor;
	default:
		return 0;
	}
}

/* Rule (a): root= */
static int sto_rule_spec(void)
{
	int match[8], n = 0;
	for (int i = 0; i < g_ncand && n < 8; i++)
		if (!sto_failed(&g_cands[i]) && sto_spec_matches(&g_cands[i]))
			match[n++] = i;
	if (n == 0)
		return STO_PICK_WAIT;
	int pick = match[0];
	if (n > 1) {
		/* Duplicates (a cloned filesystem UUID or label): prefer the
		 * loader's disk when the identity is known. */
		int li = g_bootid.have_identity ? sto_find_loader() : -1;
		for (int k = 0; k < n && li >= 0; k++)
			if (g_cands[match[k]].disk_key == g_cands[li].disk_key) {
				pick = match[k];
				break;
			}
		static int warned;
		if (!warned) {
			warned = 1;
			kprintf(STO_PREFIX "WARNING: root=%s matches %d devices:",
				g_bootid.root_spec, n);
			for (int k = 0; k < n; k++)
				kprintf(" %s", g_cands[match[k]].name);
			kprintf("; using %s\n", g_cands[pick].name);
		}
	}
	if (g_spec.kind == SPEC_PARTUUID && g_spec.partnroff) {
		int want = g_cands[pick].partno + g_spec.partnroff;
		const void *disk = g_cands[pick].disk_key;
		for (int i = 0; i < g_ncand; i++)
			if (g_cands[i].disk_key == disk &&
			    g_cands[i].partno == want && !sto_failed(&g_cands[i]))
				return i;
		static int warned_off;
		if (!warned_off) {
			warned_off = 1;
			kprintf(STO_PREFIX
				"root=%s: %s has no partition %d (PARTNROFF)\n",
				g_bootid.root_spec, g_cands[pick].name, want);
		}
		return STO_PICK_WAIT;
	}
	return pick;
}

/* Rule (c): the legacy /LIKEOS.SIG search -- read-only probes; nothing is
 * mounted until the signature was seen. */
static int sto_rule_legacy(void)
{
	for (int i = 0; i < g_ncand; i++) {
		const struct sto_cand *c = &g_cands[i];
		if (!c->ready || sto_failed(c))
			continue;
		struct sto_probe *p = sto_probe(c, 1);
		if (p->sig == 1) {
			static const block_device_t *reported;
			if (reported != c->bdev) {
				reported = c->bdev;
				kprintf(STO_PREFIX "%s: /LIKEOS.SIG found (%s)\n",
					c->name, sto_fs_name(p->fs));
			}
			return i;
		}
	}
	return STO_PICK_WAIT;
}

/* "Still waiting" report: what is wanted and what is there. */
static void sto_report_waiting(storage_fs_state_t *state)
{
	uint64_t secs = timer_ticks_to_s(timer_ticks() - state->start_tick);
	switch (state->rule) {
	case STORAGE_RULE_ROOT_SPEC:
		kprintf(STO_PREFIX "waiting for root=%s (%llus)",
			g_bootid.root_spec, (unsigned long long)secs);
		break;
	case STORAGE_RULE_IDENTITY:
		kprintf(STO_PREFIX
			"waiting for the loader disk (PARTUUID=%s, %s) and its root "
			"partition (%llus)",
			g_bootid.loader_partuuid, sto_bus_name(g_bootid.disk_bus),
			(unsigned long long)secs);
		break;
	default:
		kprintf(STO_PREFIX
			"waiting for a device with /LIKEOS.SIG (%llus)",
			(unsigned long long)secs);
		break;
	}
	kprintf("; candidates:");
	if (!g_ncand)
		kprintf(" none");
	for (int i = 0; i < g_ncand; i++) {
		const struct sto_cand *c = &g_cands[i];
		kprintf(" %s%s", c->name, c->ready ? "" : "(not ready)");
		if (c->partuuid[0])
			kprintf("[%s]", c->partuuid);
	}
	kprintf("\n");
}

/* ==========================================================================
 * The poll
 * ========================================================================== */

void storage_fs_init(storage_fs_state_t *state)
{
	/* g_legacy_parsed has one bit per registry slot */
	BUILD_BUG_ON(BLOCK_MAX_DEVICES > 64);
	if (!state)
		return;
	mm_memset(state, 0, sizeof(*state));
	state->rule = STORAGE_RULE_NONE;
}

void storage_fs_set_ready(storage_fs_state_t *state)
{
	if (!state)
		return;
	state->os_ready = 1;
}

/* The rule to start with, from what the bootloader passed. */
static int sto_initial_rule(void)
{
	if (g_spec.kind != SPEC_NONE && g_spec.kind != SPEC_INVALID)
		return STORAGE_RULE_ROOT_SPEC;
	if (g_bootid.have_identity)
		return STORAGE_RULE_IDENTITY;
	return STORAGE_RULE_LEGACY;
}

/* root= could not be satisfied: fall back to the identity (or legacy) rule */
static void sto_leave_spec_rule(storage_fs_state_t *state, const char *why)
{
	int next = g_bootid.have_identity ? STORAGE_RULE_IDENTITY :
					    STORAGE_RULE_LEGACY;
	kprintf(STO_PREFIX "WARNING: root=%s %s -- falling back to the %s\n",
		g_bootid.root_spec, why, sto_rule_name(next));
	if (next == STORAGE_RULE_LEGACY)
		kprintf(STO_PREFIX
			"WARNING: no boot identity either; using the legacy /LIKEOS.SIG "
			"search\n");
	state->rule = next;
	state->pending = NULL;
}

void storage_fs_poll(storage_fs_state_t *state)
{
	BUG_ON(state == NULL);
	if (!state || state->signature_found || !state->os_ready)
		return;
	might_sleep();

	uint64_t now = timer_ticks();
	if (state->rule == STORAGE_RULE_NONE) {
		state->start_tick = now;
		state->last_report_tick = now;
		state->rule = sto_initial_rule();
		sto_report_identity();
		kprintf(STO_PREFIX "looking for the root filesystem: %s\n",
			sto_rule_name(state->rule));
	}
	if (state->scans &&
	    now - state->last_scan_tick < timer_ms_to_ticks(STORAGE_SCAN_MS))
		return;
	state->last_scan_tick = now;
	state->scans++;

	sto_collect();

	int pick;
	switch (state->rule) {
	case STORAGE_RULE_ROOT_SPEC:
		pick = sto_rule_spec();
		if (pick == STO_PICK_WAIT &&
		    now - state->start_tick >
			    timer_ms_to_ticks(STORAGE_ROOT_SPEC_WAIT_MS)) {
			sto_leave_spec_rule(state,
					    "names no device after 30 seconds");
			sto_release();
			return;
		}
		break;
	case STORAGE_RULE_IDENTITY:
		pick = sto_rule_identity(state);
		break;
	default:
		pick = sto_rule_legacy();
		break;
	}

	if (pick < 0) {
		state->pending = NULL;
		if (now - state->last_report_tick >=
		    timer_ms_to_ticks(STORAGE_REPORT_MS)) {
			state->last_report_tick = now;
			sto_report_waiting(state);
		}
		sto_release();
		return;
	}

	/* A choice: it must stay chosen and ready STORAGE_SETTLE_MS. */
	struct sto_cand *c = &g_cands[pick];
	if (!c->ready || state->pending != c->bdev) {
		state->pending = c->ready ? c->bdev : NULL;
		state->pending_ready_polls = 0;
		state->pending_since_tick = now;
		sto_release();
		return;
	}
	state->pending_ready_polls++;
	if (now - state->pending_since_tick <
	    timer_ms_to_ticks(STORAGE_SETTLE_MS)) {
		sto_release();
		return;
	}

	int rule = state->rule;
	if (!sto_mount(state, c, rule)) {
		state->pending = NULL;
		if (rule == STORAGE_RULE_ROOT_SPEC)
			sto_leave_spec_rule(state, "could not be mounted");
	}
	sto_release();
}
