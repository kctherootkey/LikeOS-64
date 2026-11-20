// devd: which filesystem is on a volume -- type, label, UUID.
//
// Read as root from the device node: FAT12/16/32 (with the volume label
// from the root directory, where other systems keep it), ext2/3/4, exFAT,
// NTFS, ISO 9660 and swap.  Every offset is checked against what was read;
// every label goes through label_display() before anyone sees it.
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

/* ---- NTFS, exFAT ---------------------------------------------------------- */

static int probe_ntfs(const uint8_t *b, size_t len, struct probe_result *o)
{
	if (len < 512 || memcmp(b + 3, "NTFS    ", 8) != 0)
		return 0;
	set_type(o, "ntfs");
	snprintf(o->uuid, sizeof(o->uuid), "%016llX",
		 (unsigned long long)le64(b + 0x48));
	return 1;
}

static int probe_exfat(const uint8_t *b, size_t len, struct probe_result *o)
{
	uint32_t serial;

	if (len < 512 || memcmp(b + 3, "EXFAT   ", 8) != 0)
		return 0;
	set_type(o, "exfat");
	serial = le32(b + 0x64);
	snprintf(o->uuid, sizeof(o->uuid), "%04X-%04X",
		 (unsigned)(serial >> 16), (unsigned)(serial & 0xFFFF));
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

static int probe_iso(const uint8_t *b, size_t len, struct probe_result *o)
{
	const uint8_t *d;

	if (len < 0x8000 + 2048 || memcmp(b + 0x8001, "CD001", 5) != 0)
		return 0;
	set_type(o, "iso9660");
	label_display(o->label, sizeof(o->label), b + 0x8028, 32);
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
	if (probe_ntfs(buf, len, out) || probe_exfat(buf, len, out) ||
	    probe_fat(buf, len, rd, ctx, out) || probe_ext(buf, len, out) ||
	    probe_swap(buf, len, out) || probe_iso(buf, len, out))
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
