// devd: which filesystem is on a volume -- type, label, UUID.
//
// Read as root from the device node: FAT12/16/32 (with the volume label
// from the root directory, where other systems keep it), ext2/3/4, exFAT
// (the label from its root directory), NTFS (the label from the $Volume
// record of the MFT), ISO 9660 (the Joliet label when there is one) and
// swap.  Every offset is checked against what was read, every count is
// capped, nothing is followed in a loop without a bound; every label goes
// through label_display() before anyone sees it.  The partition tables of
// built-in disks are read for the partitions that belong to the firmware
// and to installed systems (EFI system, reserved, recovery).
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "devd.h"

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

static void guid_text(const uint8_t b[16], char *out, size_t cap)
{
	snprintf(out, cap,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		 "%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
		 b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void set_type(struct probe_result *o, const char *t)
{
	strlcpy_d(o->fstype, t, sizeof(o->fstype));
}

/* ---- UTF-16 labels ------------------------------------------------------ */

#define UTF16_LABEL_UNITS 256

void utf16_label(char *dst, size_t cap, const uint8_t *src, size_t units,
		 int big_endian)
{
	uint8_t tmp[UTF16_LABEL_UNITS * 3 + 4];
	size_t o = 0;

	if (units > UTF16_LABEL_UNITS)
		units = UTF16_LABEL_UNITS;
	for (size_t i = 0; i < units; i++) {
		uint32_t c = big_endian ? (uint32_t)((src[2 * i] << 8) |
						     src[2 * i + 1]) :
					  le16(src + 2 * i);

		if (c == 0)
			break;
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {
			uint32_t lo = big_endian ?
					      (uint32_t)((src[2 * i + 2] << 8) |
							 src[2 * i + 3]) :
					      le16(src + 2 * i + 2);

			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				c = 0x10000 + ((c - 0xD800) << 10) +
				    (lo - 0xDC00);
				i++;
			}
		}
		if (c >= 0xD800 && c <= 0xDFFF)
			c = '?'; /* an unpaired surrogate */
		if (c < 0x80) {
			tmp[o++] = (uint8_t)c;
		} else if (c < 0x800) {
			tmp[o++] = (uint8_t)(0xC0 | (c >> 6));
			tmp[o++] = (uint8_t)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			tmp[o++] = (uint8_t)(0xE0 | (c >> 12));
			tmp[o++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
			tmp[o++] = (uint8_t)(0x80 | (c & 0x3F));
		} else {
			tmp[o++] = (uint8_t)(0xF0 | (c >> 18));
			tmp[o++] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
			tmp[o++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
			tmp[o++] = (uint8_t)(0x80 | (c & 0x3F));
		}
	}
	label_display(dst, cap, tmp, o);
}

/* ---- NTFS ------------------------------------------------------------------- */

#define NTFS_RECORD_MAX 65536
#define NTFS_AT_VOLUME_NAME 0x60u
#define NTFS_AT_END 0xFFFFFFFFu
#define NTFS_VOLUME_RECORD 3

/* The update sequence of an MFT record: the last two bytes of every
 * stride carry the sequence number, the real bytes are in the array.  0
 * when the record is whole. */
static int ntfs_fixup(uint8_t *r, size_t len)
{
	uint16_t usa_ofs = le16(r + 4), usa_count = le16(r + 6);
	size_t stride;
	uint16_t usn;

	if (usa_count < 2 || usa_ofs < 0x28 || (usa_ofs & 1) ||
	    (size_t)usa_ofs + 2 * (size_t)usa_count > len)
		return -1;
	stride = len / (usa_count - 1);
	if (stride < 256 || stride * (usa_count - 1) != len)
		return -1;
	usn = le16(r + usa_ofs);
	for (size_t i = 1; i < usa_count; i++) {
		uint8_t *end = r + i * stride - 2;

		if (le16(end) != usn)
			return -1; /* torn or not a record */
		memcpy(end, r + usa_ofs + 2 * i, 2);
	}
	return 0;
}

/* The $VOLUME_NAME of a fixed-up $Volume record. */
static int ntfs_record_label(const uint8_t *r, size_t len,
			     struct probe_result *o)
{
	size_t off = le16(r + 0x14);
	size_t limit = le32(r + 0x18);

	if (memcmp(r, "FILE", 4) != 0 || !(le16(r + 0x16) & 1))
		return 0;
	/* NTFS 3.1 records say which record they are */
	if (le16(r + 4) >= 0x30 && le32(r + 0x2C) != NTFS_VOLUME_RECORD)
		return 0;
	if (limit > len)
		limit = len;
	for (int n = 0; n < 512 && off + 16 <= limit; n++) {
		uint32_t type = le32(r + off), alen;

		if (type == NTFS_AT_END)
			break;
		alen = le32(r + off + 4);
		if (alen < 16 || (alen & 7) || alen > limit - off)
			break;
		if (type == NTFS_AT_VOLUME_NAME && r[off + 8] == 0 &&
		    alen >= 0x18) {
			uint32_t vlen = le32(r + off + 0x10);
			uint16_t voff = le16(r + off + 0x14);

			if (voff > alen || vlen > alen - voff || (vlen & 1))
				return 0;
			utf16_label(o->label, sizeof(o->label), r + off + voff,
				    vlen / 2, 0);
			return 1;
		}
		off += alen;
	}
	return 0;
}

/* Record 3 of the MFT, else of its mirror (which keeps the first four). */
static void ntfs_label(const uint8_t *b, probe_read_fn rd, void *ctx,
		       struct probe_result *o)
{
	uint16_t bps = le16(b + 0x0B);
	uint8_t spc = b[0x0D];
	int8_t cpr = (int8_t)b[0x40];
	uint64_t cluster, recsize;
	uint8_t *rec;

	if (!rd || bps < 256 || bps > 4096 || (bps & (bps - 1)))
		return;
	if (spc == 0)
		return;
	if (spc <= 0x80) {
		if (spc & (spc - 1))
			return;
		cluster = (uint64_t)bps * spc;
	} else {
		unsigned shift = 256u - spc;

		if (shift > 21)
			return; /* clusters of up to 2 MiB */
		cluster = 1ULL << shift;
	}
	if (cpr > 0) {
		recsize = cluster * (uint64_t)cpr;
		if (recsize & (recsize - 1))
			return;
	} else {
		if (cpr < -16 || cpr > -8)
			return; /* 256 bytes .. 64 KiB */
		recsize = 1ULL << (-cpr);
	}
	if (recsize < 256 || recsize > NTFS_RECORD_MAX)
		return;
	rec = malloc((size_t)recsize);
	if (!rec)
		return;
	for (int copy = 0; copy < 2; copy++) {
		uint64_t lcn = le64(b + (copy ? 0x38 : 0x30));
		uint64_t off;

		if (lcn == 0 || lcn > (1ULL << 50) / cluster)
			continue;
		off = lcn * cluster + NTFS_VOLUME_RECORD * recsize;
		if (rd(ctx, off, rec, (size_t)recsize) != 0)
			continue;
		if (ntfs_fixup(rec, (size_t)recsize) == 0 &&
		    ntfs_record_label(rec, (size_t)recsize, o))
			break;
	}
	free(rec);
}

static int probe_ntfs(const uint8_t *b, size_t len, probe_read_fn rd,
		      void *ctx, struct probe_result *o)
{
	if (len < 512 || memcmp(b + 3, "NTFS    ", 8) != 0)
		return 0;
	set_type(o, "ntfs");
	snprintf(o->uuid, sizeof(o->uuid), "%016llX",
		 (unsigned long long)le64(b + 0x48));
	ntfs_label(b, rd, ctx, o);
	return 1;
}

/* ---- exFAT ------------------------------------------------------------------ */

#define EXFAT_DIR_READ_MAX (64 * 1024)
#define EXFAT_DIR_CLUSTERS 4
#define EXFAT_ENTRY_LABEL 0x83
#define EXFAT_LABEL_UNITS 11

/* The volume label entry among `n' bytes of directory entries: 1 found,
 * 0 not here, -1 the directory ended. */
static int exfat_dir_label(const uint8_t *dir, size_t n,
			   struct probe_result *o)
{
	for (size_t off = 0; off + 32 <= n; off += 32) {
		const uint8_t *e = dir + off;

		if (e[0] == 0x00)
			return -1;
		if (e[0] == EXFAT_ENTRY_LABEL) {
			size_t units = e[1];

			if (units > EXFAT_LABEL_UNITS)
				return -1;
			utf16_label(o->label, sizeof(o->label), e + 2, units,
				    0);
			return 1;
		}
	}
	return 0;
}

static void exfat_label(const uint8_t *b, probe_read_fn rd, void *ctx,
			struct probe_result *o)
{
	uint8_t bpss = b[0x6C], spcs = b[0x6D];
	uint32_t fat_off = le32(b + 0x50), fat_len = le32(b + 0x54);
	uint32_t heap = le32(b + 0x58), count = le32(b + 0x5C);
	uint32_t cl = le32(b + 0x60);
	uint64_t csize;
	size_t n;
	uint8_t *dir;

	if (!rd || bpss < 9 || bpss > 12 || spcs > 25 - bpss)
		return;
	csize = 1ULL << (bpss + spcs);
	if (count == 0 || count > 0xFFFFFFF5u ||
	    (uint64_t)fat_len << bpss < ((uint64_t)count + 2) * 4)
		return;
	n = csize < EXFAT_DIR_READ_MAX ? (size_t)csize : EXFAT_DIR_READ_MAX;
	dir = malloc(n);
	if (!dir)
		return;
	for (int i = 0; i < EXFAT_DIR_CLUSTERS; i++) {
		uint64_t off;
		uint8_t next[4];
		int r;

		if (cl < 2 || cl - 2 >= count)
			break;
		off = (((uint64_t)heap << bpss) + (uint64_t)(cl - 2) * csize);
		if (rd(ctx, off, dir, n) != 0)
			break;
		r = exfat_dir_label(dir, n, o);
		if (r != 0 || n < csize)
			break; /* found, ended, or more than was read */
		/* the root directory is always a FAT chain */
		if (rd(ctx, ((uint64_t)fat_off << bpss) + (uint64_t)cl * 4,
		       next, 4) != 0)
			break;
		cl = le32(next);
	}
	free(dir);
}

static int probe_exfat(const uint8_t *b, size_t len, probe_read_fn rd,
		       void *ctx, struct probe_result *o)
{
	uint32_t serial;

	if (len < 512 || memcmp(b + 3, "EXFAT   ", 8) != 0)
		return 0;
	set_type(o, "exfat");
	serial = le32(b + 0x64);
	snprintf(o->uuid, sizeof(o->uuid), "%04X-%04X",
		 (unsigned)(serial >> 16), (unsigned)(serial & 0xFFFF));
	exfat_label(b, rd, ctx, o);
	return 1;
}

/* ---- FAT ---------------------------------------------------------------------- */

#define FAT_DIR_READ_MAX (64 * 1024)

/* The volume label entry of a FAT root directory (`n' bytes at `dir'). */
static int fat_dir_label(const uint8_t *dir, size_t n, uint8_t out[11])
{
	for (size_t off = 0; off + 32 <= n; off += 32) {
		const uint8_t *e = dir + off;
		uint8_t attr = e[11];

		if (e[0] == 0x00)
			break; /* end of directory */
		if (e[0] == 0xE5)
			continue; /* deleted */
		if ((attr & 0x3F) == 0x0F)
			continue; /* long name part */
		if ((attr & 0x08) && !(attr & 0x10)) {
			memcpy(out, e, 11);
			if (out[0] == 0x05)
				out[0] = 0xE5;
			return 1;
		}
	}
	return 0;
}

static int probe_fat(const uint8_t *b, size_t len, probe_read_fn rd, void *ctx,
		     struct probe_result *o)
{
	uint16_t bps, reserved, rootents;
	uint8_t spc, nfats;
	uint32_t total, fatsz, rootsecs, data_secs, clusters;
	const uint8_t *ext;
	int is32sig;
	uint8_t lab[11];
	int have_lab = 0;

	memset(lab, 0, sizeof(lab));

	if (len < 512 || b[510] != 0x55 || b[511] != 0xAA)
		return 0;
	is32sig = memcmp(b + 0x52, "FAT32   ", 8) == 0;
	if (!is32sig && memcmp(b + 0x36, "FAT16   ", 8) != 0 &&
	    memcmp(b + 0x36, "FAT12   ", 8) != 0 &&
	    memcmp(b + 0x36, "FAT     ", 8) != 0)
		return 0;
	bps = le16(b + 0x0B);
	spc = b[0x0D];
	reserved = le16(b + 0x0E);
	nfats = b[0x10];
	rootents = le16(b + 0x11);
	total = le16(b + 0x13);
	fatsz = le16(b + 0x16);
	if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || spc == 0 ||
	    (spc & (spc - 1)) || nfats == 0 || nfats > 4 || reserved == 0)
		return 0;
	if (!total)
		total = le32(b + 0x20);
	if (!fatsz)
		fatsz = le32(b + 0x24);
	if (!fatsz)
		return 0;
	rootsecs = (uint32_t)(((uint64_t)rootents * 32 + bps - 1) / bps);
	{
		uint64_t meta = (uint64_t)reserved + (uint64_t)nfats * fatsz +
				rootsecs;
		if (meta >= total)
			return 0;
		data_secs = (uint32_t)(total - meta);
	}
	clusters = data_secs / spc;
	set_type(o, "vfat");
	if (clusters < 4085)
		o->fat_bits = 12;
	else if (clusters < 65525)
		o->fat_bits = 16;
	else
		o->fat_bits = 32;
	ext = o->fat_bits == 32 ? b + 0x40 : b + 0x24;
	if (ext[2] == 0x29) { /* extended boot signature */
		uint32_t volid = le32(ext + 3);

		snprintf(o->uuid, sizeof(o->uuid), "%04X-%04X",
			 (unsigned)(volid >> 16), (unsigned)(volid & 0xFFFF));
		memcpy(lab, ext + 7, 11);
		have_lab = 1;
	}
	/* The label other systems set lives in the root directory. */
	if (rd) {
		uint64_t off, n;
		uint64_t first_data = (uint64_t)reserved +
				      (uint64_t)nfats * fatsz + rootsecs;

		if (o->fat_bits == 32) {
			uint32_t rc = le32(b + 0x2C);

			off = (first_data + (uint64_t)(rc - 2) * spc) * bps;
			n = (uint64_t)spc * bps;
			if (rc < 2 || (uint64_t)(rc - 2) >= clusters)
				n = 0;
		} else {
			off = ((uint64_t)reserved + (uint64_t)nfats * fatsz) *
			      bps;
			n = (uint64_t)rootents * 32;
		}
		if (n > FAT_DIR_READ_MAX)
			n = FAT_DIR_READ_MAX;
		if (n >= 32) {
			uint8_t *dir = malloc((size_t)n);

			if (dir) {
				uint8_t dl[11];

				if (rd(ctx, off, dir, (size_t)n) == 0 &&
				    fat_dir_label(dir, (size_t)n, dl)) {
					memcpy(lab, dl, 11);
					have_lab = 1;
				}
				free(dir);
			}
		}
	}
	if (have_lab) {
		label_display(o->label, sizeof(o->label), lab, 11);
		if (strcmp(o->label, "NO NAME") == 0)
			o->label[0] = '\0';
	}
	return 1;
}

/* ---- ext2/3/4 -------------------------------------------------------------------- */

static int probe_ext(const uint8_t *b, size_t len, struct probe_result *o)
{
	const uint8_t *sb = b + 1024;

	if (len < 2048 || le16(sb + 0x38) != 0xEF53)
		return 0;
	/* ext2 and ext3 are mounted by the same driver as ext4: one entry */
	set_type(o, "ext4");
	if (!all_zero(sb + 0x68, 16))
		guid_text(sb + 0x68, o->uuid, sizeof(o->uuid));
	label_display(o->label, sizeof(o->label), sb + 0x78, 16);
	return 1;
}

/* ---- swap, ISO 9660 -------------------------------------------------------------- */

static int probe_swap(const uint8_t *b, size_t len, struct probe_result *o)
{
	static const size_t pages[] = { 4096, 8192, 16384, 65536 };

	for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
		size_t at = pages[i] - 10;

		if (at + 10 > len)
			break;
		if (memcmp(b + at, "SWAPSPACE2", 10) == 0 ||
		    memcmp(b + at, "SWAP-SPACE", 10) == 0) {
			set_type(o, "swap");
			if (memcmp(b + at, "SWAPSPACE2", 10) == 0 &&
			    len >= 1024 + 44) {
				if (!all_zero(b + 1024 + 12, 16))
					guid_text(b + 1024 + 12, o->uuid,
						  sizeof(o->uuid));
				label_display(o->label, sizeof(o->label),
					      b + 1024 + 28, 16);
			}
			return 1;
		}
	}
	return 0;
}

#define ISO_VD_FIRST 0x8000
#define ISO_VD_SIZE 2048
#define ISO_VD_MAX 32

/* A supplementary descriptor with a Joliet escape sequence: its volume
 * identifier, in UCS-2 big endian, is the label other systems show. */
static void iso_joliet_label(const uint8_t *b, size_t len, probe_read_fn rd,
			     void *ctx, struct probe_result *o)
{
	uint8_t *tmp = NULL;

	for (int i = 0; i < ISO_VD_MAX; i++) {
		uint64_t off = ISO_VD_FIRST + (uint64_t)i * ISO_VD_SIZE;
		const uint8_t *vd;

		if (off + ISO_VD_SIZE <= len) {
			vd = b + off;
		} else {
			if (!rd)
				break;
			if (!tmp && !(tmp = malloc(ISO_VD_SIZE)))
				break;
			if (rd(ctx, off, tmp, ISO_VD_SIZE) != 0)
				break;
			vd = tmp;
		}
		if (memcmp(vd + 1, "CD001", 5) != 0 || vd[0] == 255)
			break; /* the end of the set */
		if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' &&
		    (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
			char lab[DEVD_LABEL_MAX];

			utf16_label(lab, sizeof(lab), vd + 40, 16, 1);
			if (lab[0])
				strlcpy_d(o->label, lab, sizeof(o->label));
			break;
		}
	}
	free(tmp);
}

static int probe_iso(const uint8_t *b, size_t len, probe_read_fn rd,
		     void *ctx, struct probe_result *o)
{
	const uint8_t *d;

	if (len < 0x8000 + 2048 || memcmp(b + 0x8001, "CD001", 5) != 0)
		return 0;
	set_type(o, "iso9660");
	label_display(o->label, sizeof(o->label), b + 0x8028, 32);
	iso_joliet_label(b, len, rd, ctx, o);
	/* the creation time, the conventional ISO 9660 "UUID" */
	d = b + 0x8000 + 813;
	{
		int ok = 1;
		for (int i = 0; i < 16; i++)
			if (d[i] < '0' || d[i] > '9')
				ok = 0;
		if (ok)
			snprintf(o->uuid, sizeof(o->uuid),
				 "%.4s-%.2s-%.2s-%.2s-%.2s-%.2s-%.2s",
				 (const char *)d, (const char *)d + 4,
				 (const char *)d + 6, (const char *)d + 8,
				 (const char *)d + 10, (const char *)d + 12,
				 (const char *)d + 14);
	}
	return 1;
}

void probe_buffer(const uint8_t *buf, size_t len, probe_read_fn rd, void *ctx,
		  struct probe_result *out)
{
	memset(out, 0, sizeof(*out));
	/* NTFS and exFAT first: their boot sectors look like FAT's to a
	 * careless check; ext leaves sector 0 to a boot loader. */
	if (probe_ntfs(buf, len, rd, ctx, out) ||
	    probe_exfat(buf, len, rd, ctx, out) ||
	    probe_fat(buf, len, rd, ctx, out) || probe_ext(buf, len, out) ||
	    probe_swap(buf, len, out) || probe_iso(buf, len, rd, ctx, out))
		return;
	memset(out, 0, sizeof(*out));
	set_type(out, "unknown");
}

struct fd_ctx {
	int fd;
};

static int fd_read(void *ctx, uint64_t off, void *dst, size_t n)
{
	struct fd_ctx *c = ctx;
	size_t got = 0;

	if (off > (1ULL << 50))
		return -1;
	while (got < n) {
		ssize_t r = pread(c->fd, (uint8_t *)dst + got, n - got,
				  (off_t)(off + got));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

int probe_device(const char *path, struct probe_result *out)
{
	uint8_t *buf;
	size_t got = 0;
	struct fd_ctx c;

	memset(out, 0, sizeof(*out));
	set_type(out, "unknown");
	c.fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (c.fd < 0)
		return -errno;
	buf = calloc(1, PROBE_BYTES);
	if (!buf) {
		close(c.fd);
		return -ENOMEM;
	}
	while (got < PROBE_BYTES) {
		ssize_t n = pread(c.fd, buf + got, PROBE_BYTES - got,
				  (off_t)got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	if (got >= 512)
		probe_buffer(buf, got, fd_read, &c, out);
	free(buf);
	close(c.fd);
	return got >= 512 ? 0 : -EIO;
}

/* ---- partition tables ---------------------------------------------------------- */

#define GPT_ENTRIES_MAX 1024
#define GPT_ENTRY_SIZE_MAX 1024
#define MBR_TYPE_GPT 0xEE

/* Partition type GUIDs as text; compared in the on-disk byte order (the
 * first three groups little endian). */
static const char *const g_reserved_guids[] = {
	"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", /* EFI system partition */
	"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", /* reserved (MSR) */
	"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", /* recovery environment */
	"21686148-6449-6E6F-744E-656564454649", /* BIOS boot */
	"5808C8AA-7E8F-42E0-85D2-E1E90434CFB3", /* LDM metadata */
	"AF9B60A0-1431-4F62-BC68-3311714A69AD", /* LDM data */
};

/* MBR types: EFI system, the hidden recovery partition. */
static const uint8_t g_reserved_mbr[] = { 0xEF, 0x27 };

static int hexdig(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void guid_bytes(const char *t, uint8_t out[16])
{
	uint8_t be[16];
	int n = 0;

	for (const char *p = t; *p && n < 16; p++) {
		if (*p == '-')
			continue;
		be[n++] = (uint8_t)(hexdig(p[0]) << 4 | hexdig(p[1]));
		p++;
	}
	out[0] = be[3];
	out[1] = be[2];
	out[2] = be[1];
	out[3] = be[0];
	out[4] = be[5];
	out[5] = be[4];
	out[6] = be[7];
	out[7] = be[6];
	memcpy(out + 8, be + 8, 8);
}

static void pt_add(struct pt_reserved *pt, uint64_t lba, uint32_t lbs)
{
	uint64_t f = lbs / 512;

	if (pt->n >= PT_RESERVED_MAX || lba == 0 || lba > UINT64_MAX / f)
		return;
	pt->start[pt->n++] = lba * f;
}

static void pt_gpt(probe_read_fn rd, void *ctx, uint32_t lbs,
		   uint64_t disk_bytes, struct pt_reserved *pt)
{
	uint8_t *h = malloc(lbs), *ents;
	uint8_t guids[sizeof(g_reserved_guids) / sizeof(g_reserved_guids[0])]
		     [16];
	uint64_t elba, total;
	uint32_t count, esize;

	if (!h)
		return;
	if (rd(ctx, lbs, h, lbs) != 0 || memcmp(h, "EFI PART", 8) != 0) {
		free(h);
		return;
	}
	elba = le64(h + 0x48);
	count = le32(h + 0x50);
	esize = le32(h + 0x54);
	free(h);
	if (esize < 128 || esize > GPT_ENTRY_SIZE_MAX || (esize & 7) ||
	    count == 0 || count > GPT_ENTRIES_MAX || elba < 2 ||
	    elba > UINT64_MAX / lbs)
		return;
	total = (uint64_t)count * esize;
	if (disk_bytes && (elba * lbs > disk_bytes ||
			   total > disk_bytes - elba * lbs))
		return;
	ents = malloc((size_t)total);
	if (!ents)
		return;
	if (rd(ctx, elba * lbs, ents, (size_t)total) != 0) {
		free(ents);
		return;
	}
	for (size_t g = 0; g < sizeof(guids) / sizeof(guids[0]); g++)
		guid_bytes(g_reserved_guids[g], guids[g]);
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = ents + (size_t)i * esize;

		for (size_t g = 0; g < sizeof(guids) / sizeof(guids[0]); g++)
			if (memcmp(e, guids[g], 16) == 0) {
				pt_add(pt, le64(e + 32), lbs);
				break;
			}
	}
	free(ents);
}

void pt_reserved_read(probe_read_fn rd, void *ctx, uint32_t lbs,
		      uint64_t disk_bytes, struct pt_reserved *out)
{
	uint8_t *mbr;
	int gpt = 0;

	memset(out, 0, sizeof(*out));
	if (lbs < 512 || lbs > 4096 || (lbs & (lbs - 1)))
		lbs = 512;
	mbr = malloc(lbs);
	if (!mbr)
		return;
	if (rd(ctx, 0, mbr, lbs) != 0 || mbr[510] != 0x55 ||
	    mbr[511] != 0xAA) {
		free(mbr);
		return;
	}
	for (int i = 0; i < 4; i++) {
		const uint8_t *e = mbr + 446 + 16 * i;

		if (e[4] == MBR_TYPE_GPT)
			gpt = 1;
	}
	if (!gpt) {
		for (int i = 0; i < 4; i++) {
			const uint8_t *e = mbr + 446 + 16 * i;

			for (size_t k = 0; k < sizeof(g_reserved_mbr); k++)
				if (e[4] == g_reserved_mbr[k])
					pt_add(out, le32(e + 8), lbs);
		}
	}
	free(mbr);
	if (gpt)
		pt_gpt(rd, ctx, lbs, disk_bytes, out);
}

int pt_reserved_device(const char *path, uint32_t lbs, uint64_t disk_bytes,
		       struct pt_reserved *out)
{
	struct fd_ctx c;

	memset(out, 0, sizeof(*out));
	c.fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (c.fd < 0)
		return -errno;
	pt_reserved_read(fd_read, &c, lbs, disk_bytes, out);
	close(c.fd);
	return 0;
}

int pt_reserved_has(const struct pt_reserved *pt, uint64_t start)
{
	for (int i = 0; i < pt->n; i++)
		if (pt->start[i] == start)
			return 1;
	return 0;
}
