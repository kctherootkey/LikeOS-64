/* Populate an NTFS image without mounting it (no root, no FUSE): the
 * operations are done through the ntfs-3g library (libntfs-3g.so.89),
 * whose few entry points used here are declared below by hand because
 * the development headers are not installed.
 *
 *   ntfs-populate IMAGE SCRIPT
 *
 * SCRIPT lines (paths absolute inside the volume, '#' comments):
 *   copytree HOSTDIR            the tree under HOSTDIR into the root
 *                               (directories, files, symbolic links as
 *                               NTFS symlink reparse points, hard links)
 *   mkdir /p
 *   file /p SRC                 SRC = pat:SEED:LEN | zero:LEN | host:PATH
 *   ads /p STREAM SRC           a named $DATA stream (alternate data stream)
 *   sparse /p SIZE [OFF:SRC]... a sparse file of SIZE with data at OFFs
 *   compressed /p SRC           an LZNT1-compressed file (written by the
 *                               library's compressor; the parent is
 *                               flagged compressed for the creation and
 *                               plain again afterwards)
 *   compdir /p                  a directory flagged compressed
 *   ea /p NAME VALUE            an extended attribute ($EA)
 *   symlink /p TARGET [dir]     a native symbolic link (reparse point
 *                               IO_REPARSE_TAG_SYMLINK; relative targets
 *                               flagged relative, absolute ones as
 *                               \??\C:\path), a directory one with "dir"
 *   intxlink /p TARGET          an Interix symbolic link (the library's)
 *   junction /p TARGET          a mount-point reparse point (\??\TARGET)
 *   link /existing /new         a hard link
 *   attrib /p HEX               file attribute flags (e.g. 0x2 hidden)
 *   dirty                       set VOLUME_IS_DIRTY in $Volume (the
 *                               library may clear it at unmount: use
 *                               ntfs-tweak.py dirty for a dirty image)
 *
 * Exit status 0 when every line worked.
 *
 * Copyright (C) 2026 The LikeOS Project */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef uint16_t ntfschar;
typedef struct ntfs_volume ntfs_volume;
typedef struct ntfs_inode ntfs_inode;
typedef struct ntfs_attr ntfs_attr;

ntfs_volume *ntfs_mount(const char *name, unsigned long flags);
int ntfs_umount(ntfs_volume *vol, int force);
ntfs_inode *ntfs_pathname_to_inode(ntfs_volume *vol, ntfs_inode *parent,
				   const char *pathname);
ntfs_inode *ntfs_create(ntfs_inode *dir_ni, uint32_t securid,
			const ntfschar *name, uint8_t name_len, mode_t type);
ntfs_inode *ntfs_create_symlink(ntfs_inode *dir_ni, uint32_t securid,
				const ntfschar *name, uint8_t name_len,
				const ntfschar *target, int target_len);
int ntfs_link(ntfs_inode *ni, ntfs_inode *dir_ni, const ntfschar *name,
	      uint8_t name_len);
int ntfs_mbstoucs(const char *ins, ntfschar **outs);
ntfs_attr *ntfs_attr_open(ntfs_inode *ni, uint32_t type, ntfschar *name,
			  uint32_t name_len);
int64_t ntfs_attr_pwrite(ntfs_attr *na, int64_t pos, int64_t count,
			 const void *b);
int ntfs_attr_pclose(ntfs_attr *na);
void ntfs_attr_close(ntfs_attr *na);
int ntfs_attr_add(ntfs_inode *ni, uint32_t type, ntfschar *name,
		  uint8_t name_len, const uint8_t *val, int64_t size);
int ntfs_attr_truncate(ntfs_attr *na, int64_t newsize);
int ntfs_inode_close(ntfs_inode *ni);
int ntfs_set_ntfs_attrib(ntfs_inode *ni, const char *value, size_t size,
			 int flags);
int ntfs_set_ntfs_ea(ntfs_inode *ni, const char *value, size_t size,
		     int flags);
int ntfs_set_ntfs_reparse_data(ntfs_inode *ni, const char *value,
			       size_t size, int flags);
int ntfs_volume_write_flags(ntfs_volume *vol, uint16_t flags);

#define AT_DATA 0x80u
#define FILE_ATTR_COMPRESSED 0x0800u
#define FILE_ATTR_ARCHIVE 0x0020u
#define VOLUME_IS_DIRTY 0x0001u
/* ntfs_volume.state, bit NV_Compression: the library compresses only
 * when it is set, as the FUSE driver's "compression" option does. */
#define NV_COMPRESSION_BIT 6

static ntfs_volume *vol;
static int errors;

static void err(const char *what, const char *p)
{
	fprintf(stderr, "ntfs-populate: %s %s: %s\n", what, p, strerror(errno));
	errors++;
}

static void set_compression(void)
{
	/* struct ntfs_volume { void *dev; char *vol_name; unsigned long state; ... } */
	unsigned long *state = (unsigned long *)((char *)vol + 2 * sizeof(void *));
	*state |= 1UL << NV_COMPRESSION_BIT;
}

/* Data source into a malloc'd buffer. */
static unsigned char *src_data(const char *spec, size_t *len)
{
	unsigned char *b;

	if (!strncmp(spec, "pat:", 4)) {
		char *e;
		unsigned long long seed = strtoull(spec + 4, &e, 0);
		size_t n = strtoull(e + 1, NULL, 0);
		b = malloc(n ? n : 1);
		uint64_t x = seed * 0x9e3779b97f4a7c15ULL + 1;
		for (size_t i = 0; i < n; i++) {
			if ((i & 7) == 0) {
				x ^= x >> 12;
				x ^= x << 25;
				x ^= x >> 27;
			}
			b[i] = (unsigned char)((x * 2685821657736338717ULL) >> ((i & 7) * 8));
			/* compressible stretches as well, for LZNT1 */
			if ((i / 1024) % 3 == 1)
				b[i] = (unsigned char)("compressible text "[i % 18]);
		}
		*len = n;
		return b;
	}
	if (!strncmp(spec, "zero:", 5)) {
		*len = strtoull(spec + 5, NULL, 0);
		return calloc(1, *len ? *len : 1);
	}
	if (!strncmp(spec, "host:", 5)) {
		FILE *f = fopen(spec + 5, "rb");
		if (!f)
			return NULL;
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		b = malloc(n ? (size_t)n : 1);
		if (n && fread(b, 1, (size_t)n, f) != (size_t)n) {
			fclose(f);
			free(b);
			return NULL;
		}
		fclose(f);
		*len = (size_t)n;
		return b;
	}
	return NULL;
}

/* Split "/a/b/c" into the parent inode (opened) and the last name. */
static ntfs_inode *open_parent(const char *path, const char **name)
{
	char buf[4096];
	char *slash;

	snprintf(buf, sizeof(buf), "%s", path);
	slash = strrchr(buf, '/');
	if (!slash)
		return NULL;
	*name = path + (slash - buf) + 1;
	if (slash == buf)
		return ntfs_pathname_to_inode(vol, NULL, "/");
	*slash = 0;
	return ntfs_pathname_to_inode(vol, NULL, buf);
}

static ntfs_inode *create(const char *path, mode_t type)
{
	const char *name;
	ntfschar *u = NULL;
	ntfs_inode *dir = open_parent(path, &name);
	ntfs_inode *ni;
	int ul;

	if (!dir) {
		err("parent of", path);
		return NULL;
	}
	ul = ntfs_mbstoucs(name, &u);
	if (ul <= 0) {
		err("name", path);
		ntfs_inode_close(dir);
		return NULL;
	}
	ni = ntfs_create(dir, 0, u, (uint8_t)ul, type);
	if (!ni)
		err("create", path);
	free(u);
	ntfs_inode_close(dir);
	return ni;
}

static int write_attr(ntfs_inode *ni, ntfschar *sname, int slen,
		      const unsigned char *b, size_t len, int pclose)
{
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, sname, (uint32_t)slen);
	int64_t done = 0;

	if (!na)
		return -1;
	while ((size_t)done < len) {
		int64_t w = ntfs_attr_pwrite(na, done, (int64_t)(len - done), b + done);
		if (w <= 0) {
			ntfs_attr_close(na);
			return -1;
		}
		done += w;
	}
	if (pclose && ntfs_attr_pclose(na)) {
		ntfs_attr_close(na);
		return -1;
	}
	ntfs_attr_close(na);
	return 0;
}

static void do_file(const char *path, const char *spec)
{
	size_t len;
	unsigned char *b = src_data(spec, &len);
	ntfs_inode *ni;

	if (!b) {
		err("source", spec);
		return;
	}
	ni = create(path, S_IFREG);
	if (ni) {
		if (len && write_attr(ni, NULL, 0, b, len, 0))
			err("write", path);
		ntfs_inode_close(ni);
	}
	free(b);
}

/* Reparse buffer of a symbolic link: tag, data length, reserved, then
 * substitute name and print name (UTF-16) and the relative flag. */
static void set_reparse_link(ntfs_inode *ni, const char *path,
			     const char *target, int junction)
{
	char subs[4200], prin[4200];
	ntfschar *su = NULL, *pu = NULL;
	int rel = target[0] != '/' && !junction;
	size_t k;

	if (junction)
		snprintf(subs, sizeof(subs), "\\??\\%s", target);
	else if (rel)
		snprintf(subs, sizeof(subs), "%s", target);
	else
		snprintf(subs, sizeof(subs), "\\??\\C:%s", target);
	snprintf(prin, sizeof(prin), "%s%s", (!rel && !junction) ? "C:" : "",
		 target);
	for (k = 0; subs[k]; k++)
		if (subs[k] == '/')
			subs[k] = '\\';
	for (k = 0; prin[k]; k++)
		if (prin[k] == '/')
			prin[k] = '\\';
	int sl = ntfs_mbstoucs(subs, &su), pl = ntfs_mbstoucs(prin, &pu);
	if (sl <= 0 || pl <= 0) {
		err("reparse names", path);
		free(su);
		free(pu);
		return;
	}
	size_t hdr = junction ? 8 : 12;
	size_t dl = hdr + (size_t)(sl + pl) * 2 + (junction ? 4 : 0);
	unsigned char *r = calloc(1, 8 + dl);
	uint32_t tag = junction ? 0xA0000003u : 0xA000000Cu;
	memcpy(r, &tag, 4);
	r[4] = (unsigned char)dl;
	r[5] = (unsigned char)(dl >> 8);
	uint16_t so = 0, sn = (uint16_t)(sl * 2);
	uint16_t po = (uint16_t)(sl * 2 + (junction ? 2 : 0)), pn = (uint16_t)(pl * 2);
	memcpy(r + 8, &so, 2);
	memcpy(r + 10, &sn, 2);
	memcpy(r + 12, &po, 2);
	memcpy(r + 14, &pn, 2);
	if (!junction) {
		uint32_t fl = rel ? 1 : 0;
		memcpy(r + 16, &fl, 4);
	}
	memcpy(r + 8 + hdr + so, su, (size_t)sl * 2);
	memcpy(r + 8 + hdr + po, pu, (size_t)pl * 2);
	if (ntfs_set_ntfs_reparse_data(ni, (char *)r, 8 + dl, 0))
		err("reparse", path);
	free(r);
	free(su);
	free(pu);
}

/* A native symbolic link: an empty file (or directory) with a reparse
 * point of tag IO_REPARSE_TAG_SYMLINK. */
static void do_symlink(const char *path, const char *target, int isdir)
{
	ntfs_inode *ni = create(path, isdir ? S_IFDIR : S_IFREG);

	if (!ni)
		return;
	set_reparse_link(ni, path, target, 0);
	ntfs_inode_close(ni);
}

/* The library's own symbolic links (Interix "IntxLNK" system files). */
static void do_intxlink(const char *path, const char *target)
{
	const char *name;
	ntfschar *u = NULL, *t = NULL;
	ntfs_inode *dir = open_parent(path, &name), *ni;
	int ul, tl;

	if (!dir) {
		err("parent of", path);
		return;
	}
	ul = ntfs_mbstoucs(name, &u);
	tl = ntfs_mbstoucs(target, &t);
	ni = (ul > 0 && tl > 0) ? ntfs_create_symlink(dir, 0, u, (uint8_t)ul, t, tl) : NULL;
	if (!ni)
		err("intxlink", path);
	else
		ntfs_inode_close(ni);
	free(u);
	free(t);
	ntfs_inode_close(dir);
}

static void do_link(const char *from, const char *to)
{
	const char *name;
	ntfschar *u = NULL;
	ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, from);
	ntfs_inode *dir = open_parent(to, &name);
	int ul;

	if (!ni || !dir) {
		err("link", to);
		goto out;
	}
	ul = ntfs_mbstoucs(name, &u);
	if (ul <= 0 || ntfs_link(ni, dir, u, (uint8_t)ul))
		err("link", to);
out:
	free(u);
	if (ni)
		ntfs_inode_close(ni);
	if (dir)
		ntfs_inode_close(dir);
}

static void copytree(const char *host, const char *vpath)
{
	DIR *d = opendir(host);
	struct dirent *e;
	char hp[4096], vp[4096];
	static struct {
		dev_t dev;
		ino_t ino;
		char path[4096];
	} seen[256];
	static int nseen;

	if (!d) {
		err("opendir", host);
		return;
	}
	/* sorted for a deterministic MFT layout */
	char **names = NULL;
	int n = 0;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		names = realloc(names, sizeof(*names) * (n + 1));
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (strcmp(names[i], names[j]) > 0) {
				char *t = names[i];
				names[i] = names[j];
				names[j] = t;
			}
	for (int i = 0; i < n; i++) {
		struct stat st;
		snprintf(hp, sizeof(hp), "%s/%s", host, names[i]);
		snprintf(vp, sizeof(vp), "%s/%s", strcmp(vpath, "/") ? vpath : "",
			 names[i]);
		if (lstat(hp, &st)) {
			err("lstat", hp);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			ntfs_inode *ni = create(vp, S_IFDIR);
			if (ni)
				ntfs_inode_close(ni);
			copytree(hp, vp);
		} else if (S_ISLNK(st.st_mode)) {
			char t[4096];
			ssize_t l = readlink(hp, t, sizeof(t) - 1);
			if (l < 0) {
				err("readlink", hp);
				continue;
			}
			t[l] = 0;
			struct stat ts;
			do_symlink(vp, t, stat(hp, &ts) == 0 && S_ISDIR(ts.st_mode));
		} else if (S_ISREG(st.st_mode)) {
			int k;
			for (k = 0; k < nseen; k++)
				if (seen[k].dev == st.st_dev && seen[k].ino == st.st_ino)
					break;
			if (st.st_nlink > 1 && k < nseen) {
				do_link(seen[k].path, vp);
				continue;
			}
			if (st.st_nlink > 1 && nseen < 256) {
				seen[nseen].dev = st.st_dev;
				seen[nseen].ino = st.st_ino;
				snprintf(seen[nseen].path, sizeof(seen[0].path), "%s", vp);
				nseen++;
			}
			char spec[4200];
			snprintf(spec, sizeof(spec), "host:%s", hp);
			do_file(vp, spec);
		}
		free(names[i]);
	}
	free(names);
}

static void set_attrib(ntfs_inode *ni, uint32_t a, const char *p)
{
	uint32_t le = a; /* little-endian host */
	if (ntfs_set_ntfs_attrib(ni, (const char *)&le, sizeof(le), 0))
		err("attrib", p);
}

int main(int argc, char **argv)
{
	FILE *f;
	char line[8192];

	if (argc != 3) {
		fprintf(stderr, "usage: ntfs-populate IMAGE SCRIPT\n");
		return 2;
	}
	vol = ntfs_mount(argv[1], 0);
	if (!vol) {
		perror("ntfs-populate: ntfs_mount");
		return 1;
	}
	set_compression();
	f = fopen(argv[2], "r");
	if (!f) {
		perror(argv[2]);
		ntfs_umount(vol, 0);
		return 2;
	}
	while (fgets(line, sizeof(line), f)) {
		char *a[8] = { 0 };
		int n = 0;
		char *s = line, *save = NULL;
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '#' || !line[0])
			continue;
		for (char *t = strtok_r(s, " \t", &save); t && n < 8;
		     t = strtok_r(NULL, " \t", &save))
			a[n++] = t;
		if (!n)
			continue;
		if (!strcmp(a[0], "copytree") && n == 2) {
			copytree(a[1], "/");
		} else if (!strcmp(a[0], "mkdir") && n == 2) {
			ntfs_inode *ni = create(a[1], S_IFDIR);
			if (ni)
				ntfs_inode_close(ni);
		} else if (!strcmp(a[0], "file") && n == 3) {
			do_file(a[1], a[2]);
		} else if (!strcmp(a[0], "ads") && n == 4) {
			ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, a[1]);
			ntfschar *u = NULL;
			int ul = ntfs_mbstoucs(a[2], &u);
			size_t len;
			unsigned char *b = src_data(a[3], &len);
			if (!ni || ul <= 0 || !b ||
			    ntfs_attr_add(ni, AT_DATA, u, (uint8_t)ul, NULL, 0) ||
			    (len && write_attr(ni, u, ul, b, len, 0)))
				err("ads", a[1]);
			free(u);
			free(b);
			if (ni)
				ntfs_inode_close(ni);
		} else if (!strcmp(a[0], "sparse") && n >= 3) {
			ntfs_inode *ni = create(a[1], S_IFREG);
			if (!ni)
				continue;
			ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, NULL, 0);
			if (!na || ntfs_attr_truncate(na, (int64_t)strtoull(a[2], NULL, 0)))
				err("sparse truncate", a[1]);
			for (int k = 3; na && k < n; k++) {
				char *colon = strchr(a[k], ':');
				size_t len;
				unsigned char *b;
				if (!colon)
					continue;
				*colon = 0;
				b = src_data(colon + 1, &len);
				if (!b || ntfs_attr_pwrite(na, (int64_t)strtoull(a[k], NULL, 0),
							   (int64_t)len, b) != (int64_t)len)
					err("sparse write", a[1]);
				free(b);
			}
			if (na)
				ntfs_attr_close(na);
			ntfs_inode_close(ni);
		} else if (!strcmp(a[0], "compressed") && n == 3) {
			size_t len;
			unsigned char *b = src_data(a[2], &len);
			/* the library compresses files created in a compressed
			 * directory: flag the parent for the creation */
			const char *nm;
			ntfs_inode *dir = open_parent(a[1], &nm);
			if (dir) {
				set_attrib(dir, FILE_ATTR_COMPRESSED | 0x10, a[1]);
				ntfs_inode_close(dir);
			}
			ntfs_inode *ni = create(a[1], S_IFREG);
			dir = open_parent(a[1], &nm);
			if (dir) {
				set_attrib(dir, 0x10, a[1]);
				ntfs_inode_close(dir);
			}
			if (!ni || !b) {
				free(b);
				continue;
			}
			if (len && write_attr(ni, NULL, 0, b, len, 1))
				err("compressed write", a[1]);
			ntfs_inode_close(ni);
			free(b);
		} else if (!strcmp(a[0], "compdir") && n == 2) {
			ntfs_inode *ni = create(a[1], S_IFDIR);
			if (ni) {
				set_attrib(ni, FILE_ATTR_COMPRESSED | 0x10, a[1]);
				ntfs_inode_close(ni);
			}
		} else if (!strcmp(a[0], "ea") && n == 4) {
			/* one FILE_FULL_EA_INFORMATION entry */
			ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, a[1]);
			size_t nl = strlen(a[2]), vl = strlen(a[3]);
			size_t sz = (8 + nl + 1 + vl + 3) & ~3UL;
			unsigned char *ea = calloc(1, sz);
			ea[0] = (unsigned char)sz; /* next entry offset: the end */
			ea[1] = (unsigned char)(sz >> 8);
			ea[4] = 0;
			ea[5] = (unsigned char)nl;
			ea[6] = (unsigned char)vl;
			ea[7] = (unsigned char)(vl >> 8);
			memcpy(ea + 8, a[2], nl);
			memcpy(ea + 9 + nl, a[3], vl);
			if (!ni || ntfs_set_ntfs_ea(ni, (char *)ea, sz, 0))
				err("ea", a[1]);
			free(ea);
			if (ni)
				ntfs_inode_close(ni);
		} else if (!strcmp(a[0], "symlink") && (n == 3 || n == 4)) {
			do_symlink(a[1], a[2], n == 4 && !strcmp(a[3], "dir"));
		} else if (!strcmp(a[0], "intxlink") && n == 3) {
			do_intxlink(a[1], a[2]);
		} else if (!strcmp(a[0], "junction") && n == 3) {
			ntfs_inode *ni = create(a[1], S_IFDIR);
			if (ni) {
				set_reparse_link(ni, a[1], a[2], 1);
				ntfs_inode_close(ni);
			}
		} else if (!strcmp(a[0], "link") && n == 3) {
			do_link(a[1], a[2]);
		} else if (!strcmp(a[0], "attrib") && n == 3) {
			ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, a[1]);
			if (!ni) {
				err("attrib", a[1]);
				continue;
			}
			set_attrib(ni, (uint32_t)strtoul(a[2], NULL, 0), a[1]);
			ntfs_inode_close(ni);
		} else if (!strcmp(a[0], "dirty") && n == 1) {
			if (ntfs_volume_write_flags(vol, VOLUME_IS_DIRTY))
				err("dirty", "");
		} else {
			fprintf(stderr, "ntfs-populate: bad line: %s\n", a[0]);
			errors++;
		}
	}
	fclose(f);
	if (ntfs_umount(vol, 0)) {
		perror("ntfs-populate: ntfs_umount");
		errors++;
	}
	return errors ? 1 : 0;
}
