/* libinstaller -- disks: what the system has, and what is on them.
 *
 * The disks come from /sys/block/<disk>/ (size in 512-byte units, ro,
 * removable, queue/logical_block_size, queue/physical_block_size,
 * device/{model,vendor,serial}); their partition tables are read from the
 * device node with libgpt; the file systems inside the partitions are
 * recognised by their superblocks, read here directly (ext2/3/4, FAT,
 * exFAT, NTFS, swap and a few more that only get a name).
 *
 * The disk the running system was started from is marked is_boot_medium:
 * its root device comes from /proc/mounts (and, when that names no real
 * node, from the device number of "/").  In simulate mode the "disks" are
 * the image files of il_options.simulate_disks.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "il_internal.h"

#define IL_PROBE_BYTES (68 * 1024)

/* ------------------------------------------------------------------ */
/* Sizes and descriptions                                              */
/* ------------------------------------------------------------------ */

void il_format_size(uint64_t bytes, char *buf, size_t len)
{
	static const char *const units[] = { "KB", "MB", "GB", "TB", "PB" };
	double v = (double)bytes;
	int u = -1;

	if (bytes < 1000) {
		snprintf(buf, len, "%llu B", (unsigned long long)bytes);
		return;
	}
	while (v >= 1000.0 && u < 4) {
		v /= 1000.0;
		u++;
	}
	/* 999.95 would print as "1000.0": go one unit up instead. */
	if (v >= 999.95 && u < 4) {
		v /= 1000.0;
		u++;
	}
	snprintf(buf, len, "%.1f %s", v, units[u]);
}

static const char *il_transport_name(int t)
{
	switch (t) {
	case IL_TRANSPORT_SATA:
		return "SATA";
	case IL_TRANSPORT_NVME:
		return "NVMe";
	case IL_TRANSPORT_USB:
		return "USB";
	case IL_TRANSPORT_FILE:
		return "Image file";
	default:
		return "";
	}
}

void il_disk_describe(const struct il_disk *d, int index, char *buf,
		      size_t len)
{
	char size[32];
	const char *tr = il_transport_name(d->transport);
	const char *model = d->model[0] ? d->model : d->name;

	il_format_size(d->size_bytes, size, sizeof(size));
	/* U+00B7 MIDDLE DOT between the fields. */
	if (tr[0])
		snprintf(buf, len, "Disk %d \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %s",
			 index, model, size, tr);
	else
		snprintf(buf, len, "Disk %d \xc2\xb7 %s \xc2\xb7 %s", index,
			 model, size);
}

void il_part_devname(const char *disk_dev, int partno, char *out, size_t len)
{
	size_t n = strlen(disk_dev);

	/* A disk whose name ends in a digit (nvme0n1, mmcblk0, loop0) puts
	 * a 'p' between the name and the partition number. */
	if (n && isdigit((unsigned char)disk_dev[n - 1]))
		snprintf(out, len, "%sp%d", disk_dev, partno);
	else
		snprintf(out, len, "%s%d", disk_dev, partno);
}

const struct il_disk *il_disks_find(const struct il_disk_list *l,
				    const char *which)
{
	const struct il_disk *found = NULL;
	int n = 0;

	if (!l || !which || !which[0])
		return NULL;
	if (strcmp(which, "auto") == 0) {
		for (int i = 0; i < l->n; i++) {
			if (l->disks[i].is_boot_medium || l->disks[i].read_only)
				continue;
			found = &l->disks[i];
			n++;
		}
		return n == 1 ? found : NULL;
	}
	for (int i = 0; i < l->n; i++) {
		const struct il_disk *d = &l->disks[i];
		const char *base = strrchr(which, '/');

		if (strcmp(d->dev, which) == 0 || strcmp(d->name, which) == 0)
			return d;
		if (base && strcmp(d->name, base + 1) == 0 &&
		    il_starts_with(which, "/dev/"))
			return d;
	}
	return NULL;
}

int il_disk_node(const il_ctx *ctx, const struct il_disk *d, char *out,
		 size_t len)
{
	int n;

	if (ctx && ctx->simulate && sscanf(d->name, "img%d", &n) == 1 &&
	    n >= 0 && n < ctx->nsim_disks) {
		if (il_strlcpy(out, ctx->sim_disks[n], len) >= len)
			return -ENAMETOOLONG;
		return 0;
	}
	if (il_strlcpy(out, d->dev, len) >= len)
		return -ENAMETOOLONG;
	return 0;
}

const struct il_partition *il_disk_part(const struct il_disk *d, int partno)
{
	if (!d)
		return NULL;
	for (int i = 0; i < d->nparts; i++)
		if (d->parts[i].number == partno)
			return &d->parts[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* File system probing                                                 */
/* ------------------------------------------------------------------ */

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* A fixed-size label field: printable bytes, trailing blanks/NULs gone. */
static void il_copy_label(char *dst, size_t dlen, const uint8_t *src,
			  size_t slen)
{
	size_t n = 0;

	for (size_t i = 0; i < slen && n + 1 < dlen; i++) {
		if (src[i] == 0)
			break;
		dst[n++] = (char)src[i];
	}
	while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0))
		n--;
	dst[n] = '\0';
	/* Bytes that are not printable ASCII and not UTF-8 lead/trail bytes
	 * are replaced, so a label is always safe to show. */
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)dst[i] < 0x20 || dst[i] == 0x7f)
			dst[i] = '?';
}

static void il_guid_text(const uint8_t b[16], char out[40])
{
	snprintf(out, 40,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		 "%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
		 b[10], b[11], b[12], b[13], b[14], b[15]);
}

static int il_all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

/* ext2/3/4: the superblock at 1024. */
static int il_probe_ext(const uint8_t *buf, struct il_fsprobe *out)
{
	const uint8_t *sb = buf + 1024;
	uint32_t compat, incompat, ro_compat, log_bs;
	uint64_t blocks, free_blocks, bs;

	if (le16(sb + 0x38) != 0xEF53)
		return 0;
	compat = le32(sb + 0x5C);
	incompat = le32(sb + 0x60);
	ro_compat = le32(sb + 0x64);
	/* extents, 64bit, flex_bg, mmp, ... or huge_file, gdt_csum,
	 * dir_nlink, extra_isize, metadata_csum make it ext4. */
	if ((incompat & (0x0040 | 0x0080 | 0x0200 | 0x0100 | 0x0400 |
			 0x8000 | 0x10000 | 0x20000)) ||
	    (ro_compat & (0x0008 | 0x0010 | 0x0020 | 0x0040 | 0x0400)))
		il_strlcpy(out->fstype, "ext4", sizeof(out->fstype));
	else if (compat & 0x0004)
		il_strlcpy(out->fstype, "ext3", sizeof(out->fstype));
	else
		il_strlcpy(out->fstype, "ext2", sizeof(out->fstype));
	if (!il_all_zero(sb + 0x68, 16))
		il_guid_text(sb + 0x68, out->uuid);
	il_copy_label(out->label, sizeof(out->label), sb + 0x78, 16);
	log_bs = le32(sb + 0x18);
	if (log_bs > 6)
		return 1;
	bs = 1024ULL << log_bs;
	blocks = le32(sb + 0x04);
	free_blocks = le32(sb + 0x0C);
	if (incompat & 0x0080) { /* 64bit: the high halves count */
		blocks |= (uint64_t)le32(sb + 0x150) << 32;
		free_blocks |= (uint64_t)le32(sb + 0x158) << 32;
	}
	out->size_bytes = blocks * bs;
	out->free_bytes = free_blocks * bs;
	return 1;
}

/* FAT12/16/32: the boot sector. */
static int il_probe_fat(const uint8_t *buf, struct il_fsprobe *out,
			const char *path, uint64_t offset)
{
	uint16_t bps = le16(buf + 0x0B);
	uint8_t spc = buf[0x0D];
	uint16_t reserved = le16(buf + 0x0E);
	uint8_t nfats = buf[0x10];
	uint16_t rootents = le16(buf + 0x11);
	uint32_t total = le16(buf + 0x13);
	uint32_t fatsz = le16(buf + 0x16);
	const uint8_t *ext;
	uint32_t volid;
	int is32 = 0;

	if (buf[510] != 0x55 || buf[511] != 0xAA)
		return 0;
	if (memcmp(buf + 0x52, "FAT32   ", 8) == 0) {
		is32 = 1;
	} else if (memcmp(buf + 0x36, "FAT16   ", 8) != 0 &&
		   memcmp(buf + 0x36, "FAT12   ", 8) != 0 &&
		   memcmp(buf + 0x36, "FAT     ", 8) != 0) {
		return 0;
	}
	if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || spc == 0 ||
	    (spc & (spc - 1)) || nfats == 0 || reserved == 0)
		return 0;
	if (!total)
		total = le32(buf + 0x20);
	if (!fatsz)
		fatsz = le32(buf + 0x24);
	ext = is32 ? buf + 0x40 : buf + 0x24;
	il_strlcpy(out->fstype, "vfat", sizeof(out->fstype));
	if (ext[2] == 0x29) { /* extended boot signature */
		volid = le32(ext + 3);
		snprintf(out->uuid, sizeof(out->uuid), "%04X-%04X",
			 (unsigned)(volid >> 16), (unsigned)(volid & 0xFFFF));
		il_copy_label(out->label, sizeof(out->label), ext + 7, 11);
		if (strcmp(out->label, "NO NAME") == 0)
			out->label[0] = '\0';
	}
	out->size_bytes = (uint64_t)total * bps;
	/* FAT32 keeps a free cluster count in its FSInfo sector. */
	if (is32) {
		uint16_t fsinfo = le16(buf + 0x30);
		uint8_t sec[4096];
		int fd;

		if (fsinfo && fsinfo < reserved && path &&
		    (fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
			ssize_t n = pread(fd, sec, bps,
					  (off_t)(offset + (uint64_t)fsinfo * bps));

			close(fd);
			if (n == bps && le32(sec) == 0x41615252 &&
			    le32(sec + 0x1E4) == 0x61417272) {
				uint32_t freec = le32(sec + 0x1E8);
				uint64_t clusters = 0;
				uint64_t data = (uint64_t)reserved +
						(uint64_t)nfats * fatsz;

				if (total > data)
					clusters = (total - data) / spc;
				if (freec != 0xFFFFFFFFu && freec <= clusters)
					out->free_bytes = (uint64_t)freec * spc *
							  bps;
			}
		}
	}
	(void)rootents;
	return 1;
}

static int il_probe_exfat(const uint8_t *buf, struct il_fsprobe *out)
{
	uint32_t serial;
	uint8_t shift;

	if (memcmp(buf + 3, "EXFAT   ", 8) != 0)
		return 0;
	il_strlcpy(out->fstype, "exfat", sizeof(out->fstype));
	serial = le32(buf + 0x64);
	snprintf(out->uuid, sizeof(out->uuid), "%04X-%04X",
		 (unsigned)(serial >> 16), (unsigned)(serial & 0xFFFF));
	shift = buf[0x6C];
	if (shift >= 9 && shift <= 12)
		out->size_bytes = le64(buf + 0x48) << shift;
	return 1;
}

static int il_probe_ntfs(const uint8_t *buf, struct il_fsprobe *out)
{
	uint16_t bps;

	if (memcmp(buf + 3, "NTFS    ", 8) != 0)
		return 0;
	il_strlcpy(out->fstype, "ntfs", sizeof(out->fstype));
	snprintf(out->uuid, sizeof(out->uuid), "%016llX",
		 (unsigned long long)le64(buf + 0x48));
	bps = le16(buf + 0x0B);
	if (bps >= 512 && bps <= 4096)
		out->size_bytes = le64(buf + 0x28) * bps;
	return 1;
}

static int il_probe_swap(const uint8_t *buf, struct il_fsprobe *out)
{
	static const size_t pages[] = { 4096, 8192, 16384, 65536 };

	for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
		size_t at = pages[i] - 10;

		if (at + 10 > IL_PROBE_BYTES)
			break;
		if (memcmp(buf + at, "SWAPSPACE2", 10) == 0 ||
		    memcmp(buf + at, "SWAP-SPACE", 10) == 0) {
			il_strlcpy(out->fstype, "swap", sizeof(out->fstype));
			if (memcmp(buf + at, "SWAPSPACE2", 10) == 0) {
				if (!il_all_zero(buf + 1024 + 12, 16))
					il_guid_text(buf + 1024 + 12, out->uuid);
				il_copy_label(out->label, sizeof(out->label),
					      buf + 1024 + 28, 16);
				out->size_bytes =
					((uint64_t)le32(buf + 1024 + 4) + 1) *
					pages[i];
			}
			return 1;
		}
	}
	return 0;
}

/* Signatures that only get a name. */
static int il_probe_other(const uint8_t *buf, struct il_fsprobe *out)
{
	if (memcmp(buf, "LUKS\xba\xbe", 6) == 0) {
		il_strlcpy(out->fstype, "crypto_LUKS", sizeof(out->fstype));
		return 1;
	}
	if (memcmp(buf + 512, "LABELONE", 8) == 0 &&
	    memcmp(buf + 512 + 24, "LVM2 001", 8) == 0) {
		il_strlcpy(out->fstype, "LVM2_member", sizeof(out->fstype));
		return 1;
	}
	if (memcmp(buf, "XFSB", 4) == 0) {
		il_strlcpy(out->fstype, "xfs", sizeof(out->fstype));
		il_copy_label(out->label, sizeof(out->label), buf + 108, 12);
		if (!il_all_zero(buf + 32, 16))
			il_guid_text(buf + 32, out->uuid);
		return 1;
	}
	if (memcmp(buf + 0x10040, "_BHRfS_M", 8) == 0) {
		il_strlcpy(out->fstype, "btrfs", sizeof(out->fstype));
		if (!il_all_zero(buf + 0x10020, 16))
			il_guid_text(buf + 0x10020, out->uuid);
		il_copy_label(out->label, sizeof(out->label),
			      buf + 0x10000 + 0x12b, 64);
		return 1;
	}
	if (memcmp(buf + 0x8001, "CD001", 5) == 0) {
		il_strlcpy(out->fstype, "iso9660", sizeof(out->fstype));
		il_copy_label(out->label, sizeof(out->label), buf + 0x8028, 32);
		return 1;
	}
	if (memcmp(buf + 0x10, "BitLocker", 9) == 0 ||
	    memcmp(buf + 3, "-FVE-FS-", 8) == 0) {
		il_strlcpy(out->fstype, "BitLocker", sizeof(out->fstype));
		return 1;
	}
	return 0;
}

int il_fs_probe(const char *path, uint64_t offset, struct il_fsprobe *out)
{
	uint8_t *buf;
	int fd, rc = -ENOENT;
	size_t got = 0;

	memset(out, 0, sizeof(*out));
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	buf = calloc(1, IL_PROBE_BYTES);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	while (got < IL_PROBE_BYTES) {
		ssize_t n = pread(fd, buf + got, IL_PROBE_BYTES - got,
				  (off_t)(offset + got));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	if (got >= 4096) {
		/* The order matters: FAT and NTFS boot sectors look alike to a
		 * careless check, and an ext superblock leaves sector 0 to a
		 * boot loader. */
		if (il_probe_ntfs(buf, out) || il_probe_exfat(buf, out) ||
		    il_probe_fat(buf, out, path, offset) ||
		    il_probe_ext(buf, out) || il_probe_swap(buf, out) ||
		    (got == IL_PROBE_BYTES && il_probe_other(buf, out)))
			rc = 0;
		else
			rc = -ENOENT;
	}
	free(buf);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Partition tables                                                    */
/* ------------------------------------------------------------------ */

static int il_part_cmp(const void *a, const void *b)
{
	const struct il_partition *x = a, *y = b;

	if (x->start_bytes != y->start_bytes)
		return x->start_bytes < y->start_bytes ? -1 : 1;
	return x->number - y->number;
}

static void il_part_fill_fs(struct il_partition *p, const char *path,
			    uint64_t offset)
{
	struct il_fsprobe fp;

	if (il_fs_probe(path, offset, &fp) != 0)
		return;
	il_strlcpy(p->fstype, fp.fstype, sizeof(p->fstype));
	il_strlcpy(p->fs_label, fp.label, sizeof(p->fs_label));
	il_strlcpy(p->fs_uuid, fp.uuid, sizeof(p->fs_uuid));
	p->fs_free_bytes = fp.free_bytes;
}

void il_disk_compute_free(struct il_disk *d)
{
	uint64_t align = IL_ALIGN_BYTES;
	uint64_t ss = d->sector_size ? d->sector_size : 512;
	uint64_t lo, hi, cur;
	struct il_partition sorted[IL_MAX_PARTS];
	int n = d->nparts;

	d->nfree = 0;
	if (d->size_bytes < 2 * align)
		return;
	/* The area a GPT leaves usable (34 / 6 sectors at either end); a
	 * legacy table uses the same, which is close enough for the free
	 * list a person looks at. */
	lo = 34 * ss;
	if (ss == 4096)
		lo = 6 * ss;
	hi = d->size_bytes - (lo - ss); /* exclusive end */
	memcpy(sorted, d->parts, (size_t)n * sizeof(sorted[0]));
	qsort(sorted, (size_t)n, sizeof(sorted[0]), il_part_cmp);
	cur = lo;
	for (int i = 0; i <= n && d->nfree < IL_MAX_FREE; i++) {
		uint64_t end = (i < n) ? sorted[i].start_bytes : hi;
		uint64_t a = (cur + align - 1) / align * align;

		if (end > hi)
			end = hi;
		if (end > a && end - a >= align) {
			d->free[d->nfree].start_bytes = a;
			d->free[d->nfree].size_bytes = (end - a) / ss * ss;
			d->nfree++;
		}
		if (i < n) {
			uint64_t pend = sorted[i].start_bytes +
					sorted[i].size_bytes;

			if (pend > cur)
				cur = pend;
		}
	}
}

/* The partitions of a GPT, from libgpt. */
static void il_disk_from_gpt(struct il_disk *d, const struct gpt_table *t,
			     const char *path)
{
	struct gpt_free fr[IL_MAX_FREE];
	int nf;

	d->table = IL_TABLE_GPT;
	gpt_guid_format(&t->disk_guid, d->disk_guid);
	d->nparts = 0;
	for (uint32_t i = 0; i < t->num_entries && d->nparts < IL_MAX_PARTS;
	     i++) {
		const struct gpt_part *g = &t->parts[i];
		struct il_partition *p;
		char guid[GPT_GUID_STR_LEN];

		if (!g->used)
			continue;
		p = &d->parts[d->nparts++];
		memset(p, 0, sizeof(*p));
		p->number = (int)i + 1;
		il_part_devname(d->dev, p->number, p->dev, sizeof(p->dev));
		p->start_bytes = g->first_lba * t->sector_size;
		p->size_bytes = (g->last_lba - g->first_lba + 1) * t->sector_size;
		gpt_guid_format(&g->type, guid);
		il_strlcpy(p->type_guid, guid, sizeof(p->type_guid));
		gpt_guid_format(&g->uuid, guid);
		il_strlcpy(p->part_guid, guid, sizeof(p->part_guid));
		il_strlcpy(p->name, g->name, sizeof(p->name));
		il_strlcpy(p->type_desc, gpt_type_name(&g->type),
			   sizeof(p->type_desc));
		p->is_esp = strcmp(p->type_guid, IL_GUID_ESP) == 0;
		p->is_likeos_root = strcmp(p->type_guid, IL_GUID_LIKEOS_ROOT) == 0;
		if (path)
			il_part_fill_fs(p, path, p->start_bytes);
	}
	qsort(d->parts, (size_t)d->nparts, sizeof(d->parts[0]), il_part_cmp);
	nf = gpt_free_regions(t, fr, IL_MAX_FREE, 1);
	d->nfree = 0;
	for (int i = 0; i < nf; i++) {
		d->free[d->nfree].start_bytes = fr[i].first_lba * t->sector_size;
		d->free[d->nfree].size_bytes =
			(fr[i].last_lba - fr[i].first_lba + 1) * t->sector_size;
		d->nfree++;
	}
}

static void il_disk_from_mbr(struct il_disk *d, const struct mbr_table *m,
			     const char *path)
{
	d->table = IL_TABLE_MBR;
	d->nparts = 0;
	for (int i = 0; i < m->nparts && d->nparts < IL_MAX_PARTS; i++) {
		const struct mbr_part *mp = &m->parts[i];
		struct il_partition *p;

		if (mp->is_extended || mp->type == MBR_TYPE_EMPTY)
			continue;
		p = &d->parts[d->nparts++];
		memset(p, 0, sizeof(*p));
		p->number = mp->number;
		il_part_devname(d->dev, p->number, p->dev, sizeof(p->dev));
		p->start_bytes = mp->first_lba * d->sector_size;
		p->size_bytes = mp->nsectors * d->sector_size;
		p->mbr_type = mp->type;
		il_strlcpy(p->type_desc, mbr_type_name(mp->type),
			   sizeof(p->type_desc));
		p->is_esp = mp->type == MBR_TYPE_ESP;
		snprintf(p->part_guid, sizeof(p->part_guid), "%08x-%02x",
			 (unsigned)m->disk_signature, (unsigned)p->number);
		if (path)
			il_part_fill_fs(p, path, p->start_bytes);
	}
	qsort(d->parts, (size_t)d->nparts, sizeof(d->parts[0]), il_part_cmp);
	il_disk_compute_free(d);
	/* Space inside an extended partition is not free for a new primary
	 * partition: take it out of the list. */
	for (int i = 0; i < m->nparts; i++) {
		const struct mbr_part *mp = &m->parts[i];
		uint64_t es, ee;
		int w = 0;

		if (!mp->is_extended)
			continue;
		es = mp->first_lba * d->sector_size;
		ee = es + mp->nsectors * d->sector_size;
		for (int j = 0; j < d->nfree; j++) {
			uint64_t fs = d->free[j].start_bytes;
			uint64_t fe = fs + d->free[j].size_bytes;

			if (fs >= es && fe <= ee)
				continue;
			d->free[w++] = d->free[j];
		}
		d->nfree = w;
	}
}

int il_disk_read_table(il_ctx *ctx, struct il_disk *d, const char *path)
{
	struct gpt_dev dev;
	struct gpt_table t;
	struct mbr_table m;
	int rc;

	memset(&dev, 0, sizeof(dev));
	rc = gpt_dev_open(&dev, path, GPT_OPEN_RDONLY, 0);
	if (rc) {
		il_log(ctx, IL_LOG_WARN, "%s: cannot read the disk: %s", path,
		       strerror(-rc));
		return rc;
	}
	if (!d->sector_size || dev.is_file)
		d->sector_size = dev.sector_size;
	if (!d->phys_sector_size)
		d->phys_sector_size = dev.phys_sector_size;
	if (dev.size_bytes)
		d->size_bytes = dev.size_bytes;
	memset(&t, 0, sizeof(t));
	rc = gpt_read(&dev, &t);
	if (rc == 0) {
		if (t.status.using_backup)
			il_log(ctx, IL_LOG_WARN, "%s: the primary GPT is damaged "
			       "(%s); the backup copy is used", path,
			       t.status.primary_problem);
		if (t.status.backup_not_at_end)
			il_log(ctx, IL_LOG_INFO, "%s: the backup GPT is not at "
			       "the end of the disk", path);
		il_disk_from_gpt(d, &t, path);
		gpt_table_free(&t);
		gpt_dev_close(&dev);
		return 0;
	}
	if (rc == -EBADMSG)
		il_log(ctx, IL_LOG_WARN, "%s: both copies of the GPT are "
		       "damaged (%s / %s); the disk counts as blank", path,
		       t.status.primary_problem, t.status.backup_problem);
	memset(&m, 0, sizeof(m));
	if (mbr_read(&dev, &m) == 0 && m.kind == MBR_KIND_LEGACY) {
		if (m.chain_broken)
			il_log(ctx, IL_LOG_WARN, "%s: the chain of logical "
			       "partitions is damaged", path);
		il_disk_from_mbr(d, &m, path);
	} else {
		d->table = IL_TABLE_NONE;
		d->nparts = 0;
		il_disk_compute_free(d);
	}
	gpt_dev_close(&dev);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Scanning                                                            */
/* ------------------------------------------------------------------ */

static int il_skip_disk(const char *name)
{
	/* RAM disks, loop devices, optical drives, compressed swap, floppies,
	 * network block devices, device-mapper and software RAID nodes are
	 * never installation targets. */
	static const char *const skip[] = {
		"ram", "loop", "sr", "zram", "fd", "nbd", "dm-", "md", "scd",
		NULL,
	};

	if (name[0] == '.')
		return 1;
	for (int i = 0; skip[i]; i++)
		if (il_starts_with(name, skip[i]))
			return 1;
	return 0;
}

/* A sysfs attribute of a disk: <sysroot>/sys/block/<disk>/<attr>. */
static int il_disk_attr(const il_ctx *ctx, const char *disk, const char *attr,
			char *buf, size_t len)
{
	char rel[640], path[IL_PATH_MAX];

	if (snprintf(rel, sizeof(rel), "/sys/block/%s/%s", disk, attr) >=
	    (int)sizeof(rel))
		return -ENAMETOOLONG;
	il_sys_path(ctx, path, sizeof(path), rel);
	return il_read_line(path, buf, len);
}

static uint64_t il_disk_attr_u64(const il_ctx *ctx, const char *disk,
				 const char *attr, uint64_t def)
{
	char buf[64];
	char *end;
	uint64_t v;

	if (il_disk_attr(ctx, disk, attr, buf, sizeof(buf)) != 0 || !buf[0])
		return def;
	v = strtoull(buf, &end, 10);
	return end == buf ? def : v;
}

/* USB disks are named usb-* in /dev/disk/by-id. */
static int il_disk_is_usb(const il_ctx *ctx, const char *name)
{
	char dirpath[IL_PATH_MAX];
	DIR *dir;
	struct dirent *de;
	int usb = 0;

	il_sys_path(ctx, dirpath, sizeof(dirpath), "/dev/disk/by-id");
	dir = opendir(dirpath);
	if (!dir)
		return 0;
	while (!usb && (de = readdir(dir)) != NULL) {
		char link[IL_PATH_MAX], target[256];
		const char *base;
		ssize_t n;

		if (!il_starts_with(de->d_name, "usb-") ||
		    strstr(de->d_name, "-part"))
			continue;
		if (il_path_join(link, sizeof(link), dirpath, de->d_name))
			continue;
		n = readlink(link, target, sizeof(target) - 1);
		if (n <= 0)
			continue;
		target[n] = '\0';
		base = strrchr(target, '/');
		base = base ? base + 1 : target;
		if (strcmp(base, name) == 0)
			usb = 1;
	}
	closedir(dir);
	return usb;
}

static int il_detect_transport(const il_ctx *ctx, const char *name,
			       const char *vendor)
{
	if (il_starts_with(name, "nvme"))
		return IL_TRANSPORT_NVME;
	if (il_disk_is_usb(ctx, name))
		return IL_TRANSPORT_USB;
	if (il_starts_with(vendor, "ATA"))
		return IL_TRANSPORT_SATA;
	if (il_starts_with(vendor, "NVMe"))
		return IL_TRANSPORT_NVME;
	if (il_starts_with(name, "sd"))
		return IL_TRANSPORT_SATA;
	return IL_TRANSPORT_UNKNOWN;
}

/* When the device node cannot be read (no permission): the partitions the
 * kernel knows, from sysfs, so the disk is at least shown as it is. */
static void il_disk_parts_from_sysfs(const il_ctx *ctx, struct il_disk *d)
{
	char rel[256], dirpath[IL_PATH_MAX];
	DIR *dir;
	struct dirent *de;

	snprintf(rel, sizeof(rel), "/sys/block/%s", d->name);
	il_sys_path(ctx, dirpath, sizeof(dirpath), rel);
	dir = opendir(dirpath);
	if (!dir)
		return;
	while ((de = readdir(dir)) != NULL && d->nparts < IL_MAX_PARTS) {
		char attr[300];
		uint64_t start, size, partno;
		struct il_partition *p;

		if (!il_starts_with(de->d_name, d->name) ||
		    strcmp(de->d_name, d->name) == 0)
			continue;
		snprintf(attr, sizeof(attr), "%s/start", de->d_name);
		start = il_disk_attr_u64(ctx, d->name, attr, UINT64_MAX);
		snprintf(attr, sizeof(attr), "%s/size", de->d_name);
		size = il_disk_attr_u64(ctx, d->name, attr, 0);
		snprintf(attr, sizeof(attr), "%s/partition", de->d_name);
		partno = il_disk_attr_u64(ctx, d->name, attr, 0);
		if (start == UINT64_MAX || !partno)
			continue;
		p = &d->parts[d->nparts++];
		memset(p, 0, sizeof(*p));
		p->number = (int)partno;
		il_strlcpy(p->dev, "/dev/", sizeof(p->dev));
		il_strlcat(p->dev, de->d_name, sizeof(p->dev));
		p->start_bytes = start * 512;
		p->size_bytes = size * 512;
		il_strlcpy(p->type_desc, "Unknown", sizeof(p->type_desc));
	}
	closedir(dir);
	qsort(d->parts, (size_t)d->nparts, sizeof(d->parts[0]), il_part_cmp);
	il_disk_compute_free(d);
}

/* The device the running system's root lives on: "sda2", or "" when
 * /proc/mounts names none.  Also gives the device number of "/". */
static void il_root_device(const il_ctx *ctx, char *out, size_t len,
			   dev_t *root_dev)
{
	char path[IL_PATH_MAX];
	char *text = NULL;
	struct stat st;

	out[0] = '\0';
	*root_dev = 0;
	il_sys_path(ctx, path, sizeof(path), "/proc/mounts");
	if (il_read_file_alloc(path, &text, NULL) == 0) {
		char *save = NULL;

		for (char *line = strtok_r(text, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			char dev[256], mnt[256];

			if (sscanf(line, "%255s %255s", dev, mnt) != 2)
				continue;
			if (strcmp(mnt, "/") != 0)
				continue;
			if (il_starts_with(dev, "/dev/") &&
			    strcmp(dev, "/dev/root") != 0) {
				char real[IL_PATH_MAX];
				char devpath[IL_PATH_MAX];
				const char *base;

				/* /dev/disk/by-uuid/... -> its node. */
				il_sys_path(ctx, devpath, sizeof(devpath), dev);
				if (realpath(devpath, real))
					base = strrchr(real, '/');
				else
					base = strrchr(dev, '/');
				il_strlcpy(out, base ? base + 1 : dev, len);
			}
		}
		free(text);
	}
	il_sys_path(ctx, path, sizeof(path), "/");
	if (stat(path, &st) == 0)
		*root_dev = st.st_dev;
}

/* 1 when the disk holds the root: the root device is the disk itself or
 * one of its partitions (by name, or by device number). */
static int il_disk_holds_root(const il_ctx *ctx, const char *disk,
			      const char *rootname, dev_t root_dev)
{
	char rel[256], dirpath[IL_PATH_MAX], buf[64];
	DIR *dir;
	struct dirent *de;
	int found = 0;
	unsigned ma, mi;

	if (rootname[0]) {
		if (strcmp(rootname, disk) == 0)
			return 1;
		snprintf(rel, sizeof(rel), "/sys/block/%s/%s", disk, rootname);
		il_sys_path(ctx, dirpath, sizeof(dirpath), rel);
		if (il_is_dir(dirpath))
			return 1;
	}
	if (!root_dev || ctx->sysroot[0])
		return 0;
	if (il_disk_attr(ctx, disk, "dev", buf, sizeof(buf)) == 0 &&
	    sscanf(buf, "%u:%u", &ma, &mi) == 2 &&
	    makedev(ma, mi) == root_dev)
		return 1;
	snprintf(rel, sizeof(rel), "/sys/block/%s", disk);
	il_sys_path(ctx, dirpath, sizeof(dirpath), rel);
	dir = opendir(dirpath);
	if (!dir)
		return 0;
	while (!found && (de = readdir(dir)) != NULL) {
		char attr[300];

		if (!il_starts_with(de->d_name, disk) ||
		    strcmp(de->d_name, disk) == 0)
			continue;
		snprintf(attr, sizeof(attr), "%s/dev", de->d_name);
		if (il_disk_attr(ctx, disk, attr, buf, sizeof(buf)) == 0 &&
		    sscanf(buf, "%u:%u", &ma, &mi) == 2 &&
		    makedev(ma, mi) == root_dev)
			found = 1;
	}
	closedir(dir);
	return found;
}

int il_disk_holds_running_root(il_ctx *ctx, const char *disk_name)
{
	char rootname[64];
	dev_t root_dev;

	il_root_device(ctx, rootname, sizeof(rootname), &root_dev);
	return il_disk_holds_root(ctx, disk_name, rootname, root_dev);
}

/* What a disk says it is: the model (the vendor when there is no model)
 * and the serial number from sysfs.  The scan and the check before
 * writing both read them here, so both derive the same text. */
static void il_sysfs_ident(const il_ctx *ctx, const char *name, char *model,
			   size_t mlen, char *serial, size_t slen, char *vendor,
			   size_t vlen)
{
	model[0] = serial[0] = vendor[0] = '\0';
	il_disk_attr(ctx, name, "device/model", model, mlen);
	il_disk_attr(ctx, name, "device/serial", serial, slen);
	il_disk_attr(ctx, name, "device/vendor", vendor, vlen);
	if (!model[0] && vendor[0])
		il_strlcpy(model, vendor, mlen);
}

/* The same for an image file (simulate mode): its name, no serial. */
static void il_sim_ident(const char *path, char *model, size_t mlen,
			 char *serial, size_t slen)
{
	const char *base = strrchr(path, '/');

	snprintf(model, mlen, "%s", base ? base + 1 : path);
	il_strlcpy(serial, "", slen);
}

int il_disk_same_ident(il_ctx *ctx, const struct il_disk *d, char *what,
		       size_t len)
{
	char model[sizeof(d->model)], serial[sizeof(d->serial)], vendor[32];

	if (ctx->simulate) {
		char node[IL_PATH_MAX];

		if (il_disk_node(ctx, d, node, sizeof(node)) != 0) {
			snprintf(what, len, "its image path is too long");
			return 0;
		}
		il_sim_ident(node, model, sizeof(model), serial,
			     sizeof(serial));
	} else {
		il_sysfs_ident(ctx, d->name, model, sizeof(model), serial,
			       sizeof(serial), vendor, sizeof(vendor));
	}
	if (d->model[0] && strcmp(model, d->model) != 0) {
		snprintf(what, len, "model \"%.63s\", listed as \"%.63s\"",
			 model, d->model);
		return 0;
	}
	if (d->serial[0] && strcmp(serial, d->serial) != 0) {
		snprintf(what, len, "serial number \"%.47s\", listed as "
			 "\"%.47s\"", serial, d->serial);
		return 0;
	}
	return 1;
}

static int il_disk_name_cmp(const void *a, const void *b)
{
	const struct il_disk *x = a, *y = b;

	return strcmp(x->name, y->name);
}

static int il_scan_simulated(il_ctx *ctx, struct il_disk_list *out)
{
	for (int i = 0; i < ctx->nsim_disks && out->n < IL_MAX_DISKS; i++) {
		struct il_disk *d = &out->disks[out->n];
		const char *path = ctx->sim_disks[i];
		struct stat st;

		memset(d, 0, sizeof(*d));
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			il_log(ctx, IL_LOG_WARN, "%s: not an image file; left "
			       "out", path);
			continue;
		}
		snprintf(d->name, sizeof(d->name), "img%d", i);
		/* A path too long for il_disk.dev is reached through the
		 * name (il_disk_node). */
		if (strlen(path) < sizeof(d->dev))
			il_strlcpy(d->dev, path, sizeof(d->dev));
		else
			il_strlcpy(d->dev, d->name, sizeof(d->dev));
		il_sim_ident(path, d->model, sizeof(d->model), d->serial,
			     sizeof(d->serial));
		d->transport = IL_TRANSPORT_FILE;
		d->size_bytes = (uint64_t)st.st_size;
		d->sector_size = 512;
		d->phys_sector_size = 512;
		d->read_only = access(path, W_OK) != 0;
		if (il_disk_read_table(ctx, d, path) != 0)
			continue;
		out->n++;
	}
	return 0;
}

int il_disks_scan(il_ctx *ctx, struct il_disk_list *out)
{
	char dirpath[IL_PATH_MAX], rootname[64];
	DIR *dir;
	struct dirent *de;
	dev_t root_dev;

	if (!ctx || !out)
		return -EINVAL;
	il_clear_error(ctx);
	memset(out, 0, sizeof(*out));
	if (ctx->simulate) {
		il_scan_simulated(ctx, out);
		il_log(ctx, IL_LOG_INFO, "%d image disk(s)", out->n);
		return 0;
	}
	il_root_device(ctx, rootname, sizeof(rootname), &root_dev);
	il_log(ctx, IL_LOG_DEBUG, "the running system's root: %s",
	       rootname[0] ? rootname : "(no node named)");
	il_sys_path(ctx, dirpath, sizeof(dirpath), "/sys/block");
	dir = opendir(dirpath);
	if (!dir)
		return il_set_error(ctx, errno, "cannot list the disks (%s): %s",
				    dirpath, strerror(errno));
	while ((de = readdir(dir)) != NULL && out->n < IL_MAX_DISKS) {
		struct il_disk *d = &out->disks[out->n];
		char vendor[32], node[IL_PATH_MAX], rel[96];
		uint64_t sectors;

		if (il_skip_disk(de->d_name) || strlen(de->d_name) >= sizeof(d->name))
			continue;
		memset(d, 0, sizeof(*d));
		il_strlcpy(d->name, de->d_name, sizeof(d->name));
		il_strlcpy(d->dev, "/dev/", sizeof(d->dev));
		il_strlcat(d->dev, d->name, sizeof(d->dev));
		sectors = il_disk_attr_u64(ctx, d->name, "size", 0);
		if (!sectors) {
			il_log(ctx, IL_LOG_DEBUG, "%s: no medium (size 0); left "
			       "out", d->name);
			continue;
		}
		d->size_bytes = sectors * 512;
		d->sector_size = (uint32_t)il_disk_attr_u64(
			ctx, d->name, "queue/logical_block_size", 512);
		d->phys_sector_size = (uint32_t)il_disk_attr_u64(
			ctx, d->name, "queue/physical_block_size",
			d->sector_size);
		d->removable = il_disk_attr_u64(ctx, d->name, "removable", 0) != 0;
		d->read_only = il_disk_attr_u64(ctx, d->name, "ro", 0) != 0;
		il_sysfs_ident(ctx, d->name, d->model, sizeof(d->model),
			       d->serial, sizeof(d->serial), vendor,
			       sizeof(vendor));
		d->transport = il_detect_transport(ctx, d->name, vendor);
		d->is_boot_medium = il_disk_holds_root(ctx, d->name, rootname,
						       root_dev);
		snprintf(rel, sizeof(rel), "/dev/%s", d->name);
		il_sys_path(ctx, node, sizeof(node), rel);
		if (il_disk_read_table(ctx, d, node) != 0)
			il_disk_parts_from_sysfs(ctx, d);
		/* Partition nodes are named after the disk node, not the
		 * sysroot copy of it. */
		for (int i = 0; i < d->nparts; i++)
			il_part_devname(d->dev, d->parts[i].number,
					d->parts[i].dev, sizeof(d->parts[i].dev));
		{
			char size[32];

			il_format_size(d->size_bytes, size, sizeof(size));
			il_log(ctx, IL_LOG_INFO, "disk %s: %s, %s, %u-byte "
			       "sectors, %s, %d partition(s)%s%s%s", d->dev,
			       d->model[0] ? d->model : "(no model)", size,
			       d->sector_size,
			       d->table == IL_TABLE_GPT ? "GPT" :
			       d->table == IL_TABLE_MBR ? "MBR" : "no table",
			       d->nparts, d->removable ? ", removable" : "",
			       d->read_only ? ", read-only" : "",
			       d->is_boot_medium ? ", holds the running system"
						 : "");
		}
		out->n++;
	}
	closedir(dir);
	qsort(out->disks, (size_t)out->n, sizeof(out->disks[0]),
	      il_disk_name_cmp);
	return 0;
}
