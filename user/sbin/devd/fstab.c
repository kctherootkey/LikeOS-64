// devd: the filesystem table.
//
// One entry per filesystem devd knows: how it is called, how it is mounted
// for a user, what a label may look like, which cluster sizes it takes and
// how mkfs is run for it.  Adding a filesystem is adding an entry here (and
// the kernel driver or the mkfs tool it needs).  FAT32, ext4, exFAT and NTFS
// are mounted and made (mkfs.fat, mke2fs, mkfs.exfat, mkntfs); ISO 9660 is
// mounted only; swap is an identity only, so that the desktop can name what
// it found.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "devd.h"

/* ---- labels ------------------------------------------------------------------- */

/* FAT: 11 bytes of upper-case ASCII without the characters FAT reserves.
 * Lower case is accepted and upper-cased, as other systems do. */
static int vfat_label_ok(const char *in, size_t n, char *out, size_t cap)
{
	static const char bad[] = "\"*+,./:;<=>?[\\]|";
	size_t len = n;

	while (len > 0 && in[len - 1] == ' ')
		len--;
	if (len > 11 || len + 1 > cap)
		return -EINVAL;
	if (len && in[0] == ' ')
		return -EINVAL;
	for (size_t i = 0; i < len; i++) {
		char c = in[i];

		if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e ||
		    strchr(bad, c))
			return -EINVAL;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		out[i] = c;
	}
	out[len] = '\0';
	return 0;
}

/* ext4: 16 bytes of UTF-8 text, no control characters, no '/'. */
static int ext4_label_ok(const char *in, size_t n, char *out, size_t cap)
{
	if (n > 16 || n + 1 > cap)
		return -EINVAL;
	if (!utf8_text_ok(in, n) || memchr(in, '/', n))
		return -EINVAL;
	memcpy(out, in, n);
	out[n] = '\0';
	return 0;
}

/* UTF-16 code units of the valid UTF-8 text `s' (n bytes): one per
 * character, two for a character beyond U+FFFF (a 4-byte sequence). */
static size_t utf16_units(const char *s, size_t n)
{
	size_t units = 0;

	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if ((c & 0xC0) == 0x80)
			continue; /* continuation byte */
		units += c >= 0xF0 ? 2 : 1;
	}
	return units;
}

/* exFAT and NTFS store a label as UTF-16: at most `maxunits' code units of
 * UTF-8 text without control characters and without the characters a name
 * on these volumes may not contain. */
static int utf16_label_ok(const char *in, size_t n, char *out, size_t cap,
			  size_t maxunits)
{
	static const char bad[] = "\"*/:<>?\\|";

	if (n + 1 > cap)
		return -EINVAL;
	if (!utf8_text_ok(in, n) || utf16_units(in, n) > maxunits)
		return -EINVAL;
	for (size_t i = 0; i < n; i++)
		if (memchr(bad, in[i], sizeof(bad) - 1))
			return -EINVAL;
	memcpy(out, in, n);
	out[n] = '\0';
	return 0;
}

/* exFAT: the volume label entry holds 11 UTF-16 code units. */
static int exfat_label_ok(const char *in, size_t n, char *out, size_t cap)
{
	return utf16_label_ok(in, n, out, cap, 11);
}

/* NTFS: $Volume's name, 128 UTF-16 code units. */
static int ntfs_label_ok(const char *in, size_t n, char *out, size_t cap)
{
	return utf16_label_ok(in, n, out, cap, 128);
}

/* ---- mount options ------------------------------------------------------------ */

static void vfat_mount_opts(char *out, size_t cap, uid_t uid, gid_t gid)
{
	/* FAT has no owners: the volume belongs to the user it is mounted
	 * for, private to them (files 0600, directories 0700). */
	snprintf(out, cap, "uid=%u,gid=%u,fmask=0177,dmask=0077,shortname=mixed",
		 (unsigned)uid, (unsigned)gid);
}

/* exFAT and NTFS: the same owner rule as FAT.  Only the options both
 * kernel drivers take: never "force" -- a volume another system left
 * hibernated is mounted read-only by the driver instead.  An NTFS volume
 * whose log must be replayed is refused until the user agrees (the ask
 * and recover options in the table). */
static void owned_mount_opts(char *out, size_t cap, uid_t uid, gid_t gid)
{
	snprintf(out, cap, "uid=%u,gid=%u,fmask=0177,dmask=0077",
		 (unsigned)uid, (unsigned)gid);
}

/* ISO 9660: read-only anyway; the files belong to the user it is mounted
 * for where the disc names no owners of its own. */
static void iso_mount_opts(char *out, size_t cap, uid_t uid, gid_t gid)
{
	snprintf(out, cap, "uid=%u,gid=%u", (unsigned)uid, (unsigned)gid);
}

static void ext4_mount_opts(char *out, size_t cap, uid_t uid, gid_t gid)
{
	(void)uid;
	(void)gid;
	if (cap)
		out[0] = '\0';
}

/* ---- mkfs ------------------------------------------------------------------------ */

/* Copy `s' into the argument store; NULL when it is full. */
static char *store_put(char *store, size_t cap, size_t *used, const char *s)
{
	size_t n = strlen(s) + 1;
	char *p;

	if (*used + n > cap)
		return NULL;
	p = store + *used;
	memcpy(p, s, n);
	*used += n;
	return p;
}

/* `x' is evaluated once: a second store_put() would copy the string again
 * and could find the store full, leaving a NULL inside argv. */
#define ARG(x)                                         \
	do {                                           \
		char *arg_ = (x);                      \
		if (argc + 1 >= max || !arg_)          \
			return -E2BIG;                 \
		argv[argc++] = arg_;                   \
	} while (0)

static int vfat_mkfs_argv(char **argv, int max, char *store, size_t store_cap,
			  const char *dev, const char *label, uint32_t cluster,
			  uint32_t sector, int whole_disk, uid_t uid, gid_t gid)
{
	size_t used = 0;
	int argc = 0;
	char num[16];

	(void)uid;
	(void)gid;
	ARG(store_put(store, store_cap, &used, "/sbin/mkfs.fat"));
	ARG(store_put(store, store_cap, &used, "-F"));
	ARG(store_put(store, store_cap, &used, "32"));
	if (label && *label) {
		ARG(store_put(store, store_cap, &used, "-n"));
		ARG(store_put(store, store_cap, &used, label));
	}
	if (cluster) {
		uint32_t spc;

		if (!sector || cluster % sector)
			return -EINVAL;
		spc = cluster / sector;
		if (spc < 1 || spc > 128 || (spc & (spc - 1)))
			return -EINVAL;
		snprintf(num, sizeof(num), "%u", (unsigned)spc);
		ARG(store_put(store, store_cap, &used, "-s"));
		ARG(store_put(store, store_cap, &used, num));
	}
	if (whole_disk)
		ARG(store_put(store, store_cap, &used, "-I"));
	ARG(store_put(store, store_cap, &used, dev));
	argv[argc] = NULL;
	return argc;
}

static int ext4_mkfs_argv(char **argv, int max, char *store, size_t store_cap,
			  const char *dev, const char *label, uint32_t cluster,
			  uint32_t sector, int whole_disk, uid_t uid, gid_t gid)
{
	size_t used = 0;
	int argc = 0;
	char num[48];

	(void)whole_disk;
	ARG(store_put(store, store_cap, &used, "/sbin/mke2fs"));
	ARG(store_put(store, store_cap, &used, "-F"));
	ARG(store_put(store, store_cap, &used, "-q"));
	ARG(store_put(store, store_cap, &used, "-t"));
	ARG(store_put(store, store_cap, &used, "ext4"));
	if (label && *label) {
		ARG(store_put(store, store_cap, &used, "-L"));
		ARG(store_put(store, store_cap, &used, label));
	}
	if (cluster) {
		if (sector && cluster < sector)
			return -EINVAL;
		snprintf(num, sizeof(num), "%u", (unsigned)cluster);
		ARG(store_put(store, store_cap, &used, "-b"));
		ARG(store_put(store, store_cap, &used, num));
	}
	/* the user who formats it owns the new root directory */
	snprintf(num, sizeof(num), "root_owner=%u:%u", (unsigned)uid,
		 (unsigned)gid);
	ARG(store_put(store, store_cap, &used, "-E"));
	ARG(store_put(store, store_cap, &used, num));
	ARG(store_put(store, store_cap, &used, dev));
	argv[argc] = NULL;
	return argc;
}

/* exFAT: mkfs.exfat -q [-L label] [-c bytes] -P none <dev>.  -P none: no
 * partition table, also on a whole disk (it stays a single volume, as the
 * FAT32 format leaves it).  A full format is devd's zero pass first; the
 * tool itself only clears the head of the volume. */
static int exfat_mkfs_argv(char **argv, int max, char *store, size_t store_cap,
			   const char *dev, const char *label, uint32_t cluster,
			   uint32_t sector, int whole_disk, uid_t uid, gid_t gid)
{
	size_t used = 0;
	int argc = 0;
	char num[16];

	(void)whole_disk;
	(void)uid;
	(void)gid;
	ARG(store_put(store, store_cap, &used, "/sbin/mkfs.exfat"));
	ARG(store_put(store, store_cap, &used, "-q"));
	if (label && *label) {
		ARG(store_put(store, store_cap, &used, "-L"));
		ARG(store_put(store, store_cap, &used, label));
	}
	if (cluster) {
		if (cluster < 4096 || cluster > 32u * 1024 * 1024 ||
		    (cluster & (cluster - 1)) || (sector && cluster < sector))
			return -EINVAL;
		snprintf(num, sizeof(num), "%u", (unsigned)cluster);
		ARG(store_put(store, store_cap, &used, "-c"));
		ARG(store_put(store, store_cap, &used, num));
	}
	ARG(store_put(store, store_cap, &used, "-P"));
	ARG(store_put(store, store_cap, &used, "none"));
	ARG(store_put(store, store_cap, &used, dev));
	argv[argc] = NULL;
	return argc;
}

/* NTFS: mkntfs -q -f [-L label] [-c bytes] <dev>.  Always -f (fast: no zero
 * pass of its own): a full format is devd's zero pass first, as for every
 * file system.  No -F: mkntfs needs it only for a target that is not a
 * block device, and it would also let it past its "mounted" check. */
static int ntfs_mkfs_argv(char **argv, int max, char *store, size_t store_cap,
			  const char *dev, const char *label, uint32_t cluster,
			  uint32_t sector, int whole_disk, uid_t uid, gid_t gid)
{
	size_t used = 0;
	int argc = 0;
	char num[16];

	(void)whole_disk;
	(void)uid;
	(void)gid;
	ARG(store_put(store, store_cap, &used, "/sbin/mkntfs"));
	ARG(store_put(store, store_cap, &used, "-q"));
	ARG(store_put(store, store_cap, &used, "-f"));
	if (label && *label) {
		ARG(store_put(store, store_cap, &used, "-L"));
		ARG(store_put(store, store_cap, &used, label));
	}
	if (cluster) {
		/* a power of two from 512 bytes to 2 MiB, at least one and at
		 * most 4096 sectors */
		if (cluster < 512 || cluster > 2u * 1024 * 1024 ||
		    (cluster & (cluster - 1)) ||
		    (sector && (cluster < sector ||
				(uint64_t)cluster > 4096ull * sector)))
			return -EINVAL;
		snprintf(num, sizeof(num), "%u", (unsigned)cluster);
		ARG(store_put(store, store_cap, &used, "-c"));
		ARG(store_put(store, store_cap, &used, num));
	}
	ARG(store_put(store, store_cap, &used, dev));
	argv[argc] = NULL;
	return argc;
}

#undef ARG

/* ---- the table ------------------------------------------------------------------ */

static const uint32_t vfat_clusters[] = { 512,  1024,  2048,  4096, 8192,
					  16384, 32768, 65536, 0 };
static const uint32_t ext4_clusters[] = { 1024, 2048, 4096, 0 };
/* exFAT: 4 KiB to 32 MiB; the default follows the volume size */
static const uint32_t exfat_clusters[] = {
	4096,	  8192,	    16384,   32768,   65536,	131072,	 262144,
	524288,	  1048576,  2097152, 4194304, 8388608, 16777216, 33554432,
	0
};
/* NTFS: 512 bytes to 2 MiB; 4096 by default */
static const uint32_t ntfs_clusters[] = { 512,	  1024,	   2048,   4096,
					  8192,	  16384,   32768,  65536,
					  131072, 262144,  524288, 1048576,
					  2097152, 0 };
static const uint32_t no_clusters[] = { 0 };

static const struct fs_type g_fs[] = {
	{ .id = "vfat", .name = "FAT32", .kernel_type = "vfat",
	  .is_default = 1, .labelmax = 11, .clusters = vfat_clusters,
	  .defcluster = 4096, .mkfs = "/sbin/mkfs.fat",
	  .label_ok = vfat_label_ok, .mount_opts = vfat_mount_opts,
	  .mkfs_argv = vfat_mkfs_argv },
	{ .id = "ext4", .name = "ext4", .kernel_type = "ext4",
	  .labelmax = 16, .clusters = ext4_clusters, .defcluster = 4096,
	  .mkfs = "/sbin/mke2fs", .label_ok = ext4_label_ok,
	  .mount_opts = ext4_mount_opts, .mkfs_argv = ext4_mkfs_argv,
	  .ask_opt = "recovery=ask" },
	{ .id = "exfat", .name = "exFAT", .kernel_type = "exfat",
	  .labelmax = 11, .clusters = exfat_clusters, .defcluster = 0,
	  .mkfs = "/sbin/mkfs.exfat", .label_ok = exfat_label_ok,
	  .mount_opts = owned_mount_opts, .mkfs_argv = exfat_mkfs_argv },
	{ .id = "ntfs", .name = "NTFS", .kernel_type = "ntfs",
	  .labelmax = 128, .clusters = ntfs_clusters, .defcluster = 4096,
	  .mkfs = "/sbin/mkntfs", .label_ok = ntfs_label_ok,
	  .mount_opts = owned_mount_opts, .mkfs_argv = ntfs_mkfs_argv,
	  .ask_opt = "recovery=ask", .recover_opt = "recover" },
	{ .id = "iso9660", .name = "ISO 9660", .kernel_type = "iso9660",
	  .clusters = no_clusters, .mount_opts = iso_mount_opts,
	  .always_ro = 1 },
	{ .id = "swap", .name = "Swap", .clusters = no_clusters },
};

const struct fs_type *fs_table(size_t *n)
{
	*n = sizeof(g_fs) / sizeof(g_fs[0]);
	return g_fs;
}

const struct fs_type *fs_by_id(const char *id)
{
	for (size_t i = 0; i < sizeof(g_fs) / sizeof(g_fs[0]); i++)
		if (strcmp(g_fs[i].id, id) == 0)
			return &g_fs[i];
	return NULL;
}

int fs_supported(const struct probe_result *pr)
{
	const struct fs_type *t = fs_by_id(pr->fstype);

	if (!t || !t->kernel_type || !t->mount_opts)
		return 0;
	/* the kernel's FAT driver mounts FAT12, FAT16 and FAT32 volumes */
	if (strcmp(t->id, "vfat") == 0 && pr->fat_bits != 12 &&
	    pr->fat_bits != 16 && pr->fat_bits != 32)
		return 0;
	return 1;
}

int fs_cluster_ok(const struct fs_type *t, uint32_t cluster)
{
	if (cluster == 0)
		return 1;
	for (const uint32_t *c = t->clusters; *c; c++)
		if (*c == cluster)
			return 1;
	return 0;
}

int fs_mount_data(const struct fs_type *t, int recover, uid_t uid, gid_t gid,
		  char *out, size_t cap)
{
	const char *extra = recover ? t->recover_opt : t->ask_opt;
	size_t n;
	int w;

	if (!cap)
		return -ENAMETOOLONG;
	out[0] = '\0';
	if (t->mount_opts)
		t->mount_opts(out, cap, uid, gid);
	n = strlen(out);
	if (n && n + 1 >= cap)
		return -ENAMETOOLONG; /* filled up: possibly cut */
	if (!extra || !*extra)
		return 0;
	w = snprintf(out + n, cap - n, "%s%s", n ? "," : "", extra);
	if (w < 0 || (size_t)w >= cap - n)
		return -ENAMETOOLONG;
	return 0;
}
