// devd: the filesystem table.
//
// One entry per filesystem devd knows: how it is called, how it is mounted
// for a user, what a label may look like, which cluster sizes it takes and
// how mkfs is run for it.  Adding a filesystem is adding an entry here (and
// the kernel driver or the mkfs tool it needs).  The entries without a mount
// type and without mkfs are identities only, so that the desktop can name
// what it found ("uses a file system LikeOS can't open (NTFS)").
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

/* ---- mount options ------------------------------------------------------------ */

static void vfat_mount_opts(char *out, size_t cap, uid_t uid, gid_t gid)
{
	/* FAT has no owners: the volume belongs to the user it is mounted
	 * for, private to them (files 0600, directories 0700). */
	snprintf(out, cap, "uid=%u,gid=%u,fmask=0177,dmask=0077,shortname=mixed",
		 (unsigned)uid, (unsigned)gid);
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

#define ARG(x)                                         \
	do {                                           \
		if (argc + 1 >= max || !(x))           \
			return -E2BIG;                 \
		argv[argc++] = (x);                    \
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

#undef ARG

/* ---- the table ------------------------------------------------------------------ */

static const uint32_t vfat_clusters[] = { 512,  1024,  2048,  4096, 8192,
					  16384, 32768, 65536, 0 };
static const uint32_t ext4_clusters[] = { 1024, 2048, 4096, 0 };
static const uint32_t no_clusters[] = { 0 };

static const struct fs_type g_fs[] = {
	{ "vfat", "FAT32", "vfat", 1, 11, vfat_clusters, 4096,
	  "/sbin/mkfs.fat", vfat_label_ok, vfat_mount_opts, vfat_mkfs_argv },
	{ "ext4", "ext4", "ext4", 0, 16, ext4_clusters, 4096, "/sbin/mke2fs",
	  ext4_label_ok, ext4_mount_opts, ext4_mkfs_argv },
	{ "exfat", "exFAT", NULL, 0, 0, no_clusters, 0, NULL, NULL, NULL,
	  NULL },
	{ "ntfs", "NTFS", NULL, 0, 0, no_clusters, 0, NULL, NULL, NULL, NULL },
	{ "iso9660", "ISO 9660", NULL, 0, 0, no_clusters, 0, NULL, NULL, NULL,
	  NULL },
	{ "swap", "Swap", NULL, 0, 0, no_clusters, 0, NULL, NULL, NULL, NULL },
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

	if (!t || !t->kernel_type)
		return 0;
	/* the kernel's FAT driver reads FAT32 only */
	if (strcmp(t->id, "vfat") == 0 && pr->fat_bits != 32)
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
