/*
 * fstab-util - what init, mount and umount share about mount tables.
 * See fstab-util.h.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "fstab-util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- mount tables ------------------------------------------------------ */

static char *fsu_strdup(const char *s)
{
	size_t n = strlen(s ? s : "");
	char *d = malloc(n + 1);

	if (d) {
		memcpy(d, s ? s : "", n);
		d[n] = '\0';
	}
	return d;
}

static void fsu_entry_free(struct fsu_entry *e)
{
	free(e->spec);
	free(e->dir);
	free(e->type);
	free(e->opts);
	memset(e, 0, sizeof(*e));
}

int fsu_table_load(const char *path, struct fsu_table *t)
{
	FILE *f;
	struct mntent ment, *m;
	char *buf;
	int rc = 0, line = 0;

	memset(t, 0, sizeof(*t));
	f = setmntent(path, "r");
	if (!f)
		return -(errno ? errno : ENOENT);
	buf = malloc(4096);
	if (!buf) {
		endmntent(f);
		return -ENOMEM;
	}
	while ((m = getmntent_r(f, &ment, buf, 4096)) != NULL) {
		struct fsu_entry *e;

		line++;
		if (t->n == t->cap) {
			int nc = t->cap ? t->cap * 2 : 16;
			struct fsu_entry *ne =
				realloc(t->e, (size_t)nc * sizeof(*ne));

			if (!ne) {
				rc = -ENOMEM;
				break;
			}
			t->e = ne;
			t->cap = nc;
		}
		e = &t->e[t->n];
		memset(e, 0, sizeof(*e));
		e->spec = fsu_strdup(m->mnt_fsname);
		e->dir = fsu_strdup(m->mnt_dir);
		e->type = fsu_strdup(m->mnt_type);
		e->opts = fsu_strdup(m->mnt_opts);
		e->freq = m->mnt_freq;
		e->passno = m->mnt_passno;
		e->line = line;
		if (!e->spec || !e->dir || !e->type || !e->opts) {
			fsu_entry_free(e);
			rc = -ENOMEM;
			break;
		}
		t->n++;
	}
	free(buf);
	endmntent(f);
	if (rc)
		fsu_table_free(t);
	return rc;
}

void fsu_table_free(struct fsu_table *t)
{
	for (int i = 0; i < t->n; i++)
		fsu_entry_free(&t->e[i]);
	free(t->e);
	memset(t, 0, sizeof(*t));
}

const struct fsu_entry *fsu_table_find_dir(const struct fsu_table *t,
					   const char *dir)
{
	for (int i = t->n - 1; i >= 0; i--)
		if (strcmp(t->e[i].dir, dir) == 0)
			return &t->e[i];
	return NULL;
}

const struct fsu_entry *fsu_table_find_spec(const struct fsu_table *t,
					    const char *spec)
{
	for (int i = t->n - 1; i >= 0; i--)
		if (strcmp(t->e[i].spec, spec) == 0)
			return &t->e[i];
	return NULL;
}

/* ---- options ------------------------------------------------------------ */

/* The generic words: what they set and what they clear. */
static const struct {
	const char *word;
	unsigned long set;
	unsigned long clear;
} g_flag_words[] = {
	{ "ro", MS_RDONLY, 0 },
	{ "rw", 0, MS_RDONLY },
	{ "suid", 0, MS_NOSUID },
	{ "nosuid", MS_NOSUID, 0 },
	{ "dev", 0, MS_NODEV },
	{ "nodev", MS_NODEV, 0 },
	{ "exec", 0, MS_NOEXEC },
	{ "noexec", MS_NOEXEC, 0 },
	{ "sync", MS_SYNCHRONOUS, 0 },
	{ "async", 0, MS_SYNCHRONOUS },
	{ "dirsync", MS_DIRSYNC, 0 },
	{ "mand", MS_MANDLOCK, 0 },
	{ "nomand", 0, MS_MANDLOCK },
	{ "atime", 0, MS_NOATIME },
	{ "noatime", MS_NOATIME, 0 },
	{ "diratime", 0, MS_NODIRATIME },
	{ "nodiratime", MS_NODIRATIME, 0 },
	{ "relatime", MS_RELATIME, MS_STRICTATIME },
	{ "norelatime", 0, MS_RELATIME },
	{ "strictatime", MS_STRICTATIME, MS_RELATIME },
	{ "nostrictatime", 0, MS_STRICTATIME },
	{ "lazytime", MS_LAZYTIME, 0 },
	{ "nolazytime", 0, MS_LAZYTIME },
	{ "symfollow", 0, MS_NOSYMFOLLOW },
	{ "nosymfollow", MS_NOSYMFOLLOW, 0 },
	{ "silent", MS_SILENT, 0 },
	{ "loud", 0, MS_SILENT },
	/* "defaults" is rw,suid,dev,exec,auto,nouser,async */
	{ "defaults", 0, MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC |
				 MS_SYNCHRONOUS },
};

/* Words that only steer the mount programs and never reach the kernel. */
static const char *const g_user_words[] = {
	"auto", "user", "nouser", "users", "owner", "group", "_netdev",
	"nobootwait", NULL,
};

void fsu_opts_init(struct fsu_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->mkdir_mode = 0755;
}

static int fsu_data_add(struct fsu_opts *o, const char *word, size_t n)
{
	size_t len = strlen(o->data);

	if (len + (len ? 1 : 0) + n + 1 > sizeof(o->data))
		return -E2BIG;
	if (len)
		o->data[len++] = ',';
	memcpy(o->data + len, word, n);
	o->data[len + n] = '\0';
	return 0;
}

int fsu_opts_parse(struct fsu_opts *o, const char *opts)
{
	const char *p = opts;
	int rc = 0;

	while (p && *p) {
		size_t n = strcspn(p, ",");
		char word[256];
		const char *eq;
		size_t i;
		int done = 0;

		if (n == 0) {
			p++;
			continue;
		}
		if (n >= sizeof(word)) {
			/* An option this long is a filesystem's own. */
			rc = fsu_data_add(o, p, n);
			if (rc)
				return rc;
			p += n;
			continue;
		}
		memcpy(word, p, n);
		word[n] = '\0';
		p += n;
		eq = strchr(word, '=');

		for (i = 0; !eq && i < sizeof(g_flag_words) /
				     sizeof(g_flag_words[0]); i++) {
			if (strcmp(word, g_flag_words[i].word) != 0)
				continue;
			o->flags = (o->flags & ~g_flag_words[i].clear) |
				   g_flag_words[i].set;
			o->given |= g_flag_words[i].set | g_flag_words[i].clear;
			if (!strcmp(word, "ro") || !strcmp(word, "rw") ||
			    !strcmp(word, "defaults"))
				o->ro_given = 1;
			if (!strcmp(word, "rw"))
				o->rw_given = 1;
			else if (!strcmp(word, "ro"))
				o->rw_given = 0;
			done = 1;
		}
		if (done)
			continue;
		for (i = 0; !eq && g_user_words[i]; i++)
			if (strcmp(word, g_user_words[i]) == 0)
				done = 1;
		if (done)
			continue;
		if (!strcmp(word, "noauto")) {
			o->noauto = 1;
		} else if (!strcmp(word, "nofail")) {
			o->nofail = 1;
		} else if (!strcmp(word, "remount")) {
			o->remount = 1;
		} else if (!strcmp(word, "bind") || !strcmp(word, "rbind") ||
			   !strcmp(word, "move")) {
			o->bind = 1;
		} else if (!strcmp(word, "loop") ||
			   !strncmp(word, "loop=", 5)) {
			o->loop = 1;
		} else if (!strcasecmp(word, "x-mount.mkdir")) {
			o->mkdir = 1;
		} else if (!strncasecmp(word, "x-mount.mkdir=", 14)) {
			char *end;
			unsigned long m = strtoul(word + 14, &end, 8);

			o->mkdir = 1;
			if (*end == '\0' && end != word + 14 && m <= 07777)
				o->mkdir_mode = (mode_t)m;
		} else if (!strncasecmp(word, "x-", 2) ||
			   !strncmp(word, "comment=", 8)) {
			/* Notes for programs reading the table; not options. */
		} else {
			rc = fsu_data_add(o, word, n);
			if (rc)
				return rc;
		}
	}
	return 0;
}

void fsu_flags_text(unsigned long flags, char *buf, size_t len)
{
	static const struct {
		unsigned long bit;
		const char *word;
	} w[] = {
		{ MS_NOSUID, "nosuid" },	 { MS_NODEV, "nodev" },
		{ MS_NOEXEC, "noexec" },	 { MS_SYNCHRONOUS, "sync" },
		{ MS_DIRSYNC, "dirsync" },	 { MS_MANDLOCK, "mand" },
		{ MS_NOSYMFOLLOW, "nosymfollow" }, { MS_NOATIME, "noatime" },
		{ MS_NODIRATIME, "nodiratime" }, { MS_RELATIME, "relatime" },
		{ MS_STRICTATIME, "strictatime" }, { MS_LAZYTIME, "lazytime" },
	};
	size_t n;

	if (!len)
		return;
	snprintf(buf, len, "%s", (flags & MS_RDONLY) ? "ro" : "rw");
	for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
		if (!(flags & w[i].bit))
			continue;
		n = strlen(buf);
		snprintf(buf + n, len - n, ",%s", w[i].word);
	}
}

unsigned long fsu_flags_of(const char *opts)
{
	struct fsu_opts o;

	fsu_opts_init(&o);
	fsu_opts_parse(&o, opts);
	return o.flags & FSU_FLAG_MASK;
}

int fsu_has_opt(const char *opts, const char *word)
{
	size_t wl = strlen(word);
	const char *p = opts;

	while (p && *p) {
		size_t n = strcspn(p, ",");

		if (n == wl && strncmp(p, word, wl) == 0)
			return 1;
		/* "key" also matches "key=value" */
		if (n > wl && p[wl] == '=' && strncmp(p, word, wl) == 0)
			return 1;
		p += n;
		if (*p == ',')
			p++;
	}
	return 0;
}

/* ---- sources ------------------------------------------------------------ */

static const struct {
	const char *key;
	int tag;
	const char *dir;
} g_tags[] = {
	{ "UUID=", FSU_TAG_UUID, "/dev/disk/by-uuid" },
	{ "LABEL=", FSU_TAG_LABEL, "/dev/disk/by-label" },
	{ "PARTUUID=", FSU_TAG_PARTUUID, "/dev/disk/by-partuuid" },
	{ "PARTLABEL=", FSU_TAG_PARTLABEL, "/dev/disk/by-partlabel" },
};

#define FSU_NTAGS ((int)(sizeof(g_tags) / sizeof(g_tags[0])))

int fsu_spec_tag(const char *spec, char *val, size_t len)
{
	if (!spec)
		return FSU_TAG_NONE;
	for (int i = 0; i < FSU_NTAGS; i++) {
		size_t kl = strlen(g_tags[i].key);
		const char *v;
		size_t vl;

		if (strncasecmp(spec, g_tags[i].key, kl) != 0)
			continue;
		v = spec + kl;
		vl = strlen(v);
		/* UUID="..." and UUID='...' as some tables write them */
		if (vl >= 2 && (v[0] == '"' || v[0] == '\'') &&
		    v[vl - 1] == v[0]) {
			v++;
			vl -= 2;
		}
		if (val && len) {
			if (vl >= len)
				vl = len - 1;
			memcpy(val, v, vl);
			val[vl] = '\0';
		}
		return g_tags[i].tag;
	}
	return FSU_TAG_NONE;
}

int fsu_spec_is_device(const char *spec)
{
	if (fsu_spec_tag(spec, NULL, 0) != FSU_TAG_NONE)
		return 1;
	return spec && strncmp(spec, "/dev/", 5) == 0;
}

int fsu_link_escape(const char *in, char *out, size_t len)
{
	static const char hex[] = "0123456789abcdef";
	const unsigned char *p = (const unsigned char *)in;
	size_t o = 0;

	if (!len)
		return -ENAMETOOLONG;
	while (*p) {
		unsigned char c = *p;
		int n = 0;

		/* A valid UTF-8 multibyte sequence stays as it is. */
		if (c >= 0xC2 && c <= 0xDF)
			n = 2;
		else if (c >= 0xE0 && c <= 0xEF)
			n = 3;
		else if (c >= 0xF0 && c <= 0xF4)
			n = 4;
		if (n) {
			int ok = 1;

			for (int k = 1; k < n; k++)
				if ((p[k] & 0xC0) != 0x80)
					ok = 0;
			if (ok) {
				if (o + (size_t)n + 1 > len)
					return -ENAMETOOLONG;
				memcpy(out + o, p, (size_t)n);
				o += (size_t)n;
				p += n;
				continue;
			}
		}
		if (isalnum(c) || strchr("#+-.:=@_", c)) {
			if (o + 2 > len)
				return -ENAMETOOLONG;
			out[o++] = (char)c;
		} else {
			if (o + 5 > len)
				return -ENAMETOOLONG;
			out[o++] = '\\';
			out[o++] = 'x';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
		p++;
	}
	out[o] = '\0';
	return 0;
}

/* The device a link in `dir' named `name' points to; a UUID is compared
 * without regard to case, since tables write FAT volume ids both ways. */
static int fsu_lookup_link(const char *dir, const char *name, int anycase,
			   char *out, size_t len)
{
	char path[FSU_PATH_MAX], real[PATH_MAX];
	DIR *d;
	struct dirent *de;
	int found = 0;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, name) >=
	    sizeof(path))
		return -ENAMETOOLONG;
	if (access(path, F_OK) == 0) {
		found = 1;
	} else if (anycase && (d = opendir(dir)) != NULL) {
		while ((de = readdir(d)) != NULL) {
			if (strcasecmp(de->d_name, name) == 0) {
				snprintf(path, sizeof(path), "%s/%s", dir,
					 de->d_name);
				found = 1;
				break;
			}
		}
		closedir(d);
	}
	if (!found)
		return -ENOENT;
	if (!realpath(path, real))
		return -ENOENT; /* a link to a node that went away */
	if (strlen(real) >= len)
		return -ENAMETOOLONG;
	strcpy(out, real);
	return 0;
}

int fsu_resolve(const char *spec, char *out, size_t len)
{
	char val[256], esc[1024];
	int tag;

	if (!spec || !spec[0])
		return -EINVAL;
	tag = fsu_spec_tag(spec, val, sizeof(val));
	if (tag != FSU_TAG_NONE) {
		if (!val[0])
			return -EINVAL;
		for (int i = 0; i < FSU_NTAGS; i++) {
			int anycase;

			if (g_tags[i].tag != tag)
				continue;
			if (fsu_link_escape(val, esc, sizeof(esc)) != 0)
				return -ENAMETOOLONG;
			anycase = tag == FSU_TAG_UUID || tag == FSU_TAG_PARTUUID;
			return fsu_lookup_link(g_tags[i].dir, esc, anycase, out,
					       len);
		}
		return -EINVAL;
	}
	if (spec[0] == '/') {
		char real[PATH_MAX];

		if (!realpath(spec, real))
			return errno == ENOTDIR ? -ENOENT : -(errno ? errno : ENOENT);
		if (strlen(real) >= len)
			return -ENAMETOOLONG;
		strcpy(out, real);
		return 0;
	}
	/* A label such as "tmpfs" or "none". */
	if (strlen(spec) >= len)
		return -ENAMETOOLONG;
	strcpy(out, spec);
	return 0;
}

long long fsu_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return (long long)time(NULL) * 1000;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int fsu_wait_resolve(const char *spec, char *out, size_t len,
		     long long deadline_ms)
{
	for (;;) {
		int rc = fsu_resolve(spec, out, len);

		if (rc != -ENOENT || fsu_now_ms() >= deadline_ms)
			return rc;
		usleep(100 * 1000);
	}
}

/* ---- paths ----------------------------------------------------------------- */

int fsu_canon_dir(const char *in, char *out, size_t len)
{
	char work[PATH_MAX], real[PATH_MAX];
	char *parts[256];
	int np = 0;
	size_t o;

	if (!in || !in[0])
		return -EINVAL;
	if (in[0] == '/') {
		if (strlen(in) >= sizeof(work))
			return -ENAMETOOLONG;
		strcpy(work, in);
	} else {
		char cwd[PATH_MAX];

		if (!getcwd(cwd, sizeof(cwd)))
			return -errno;
		if ((size_t)snprintf(work, sizeof(work), "%s/%s", cwd, in) >=
		    sizeof(work))
			return -ENAMETOOLONG;
	}
	/* The links, when it exists. */
	if (realpath(work, real)) {
		if (strlen(real) >= len)
			return -ENAMETOOLONG;
		strcpy(out, real);
		return 0;
	}
	/* Lexically otherwise. */
	for (char *s = strtok(work, "/"); s; s = strtok(NULL, "/")) {
		if (!strcmp(s, "."))
			continue;
		if (!strcmp(s, "..")) {
			if (np > 0)
				np--;
			continue;
		}
		if (np == (int)(sizeof(parts) / sizeof(parts[0])))
			return -ENAMETOOLONG;
		parts[np++] = s;
	}
	if (len < 2)
		return -ENAMETOOLONG;
	o = 0;
	out[0] = '\0';
	for (int i = 0; i < np; i++) {
		size_t n = strlen(parts[i]);

		if (o + 1 + n + 1 > len)
			return -ENAMETOOLONG;
		out[o++] = '/';
		memcpy(out + o, parts[i], n);
		o += n;
		out[o] = '\0';
	}
	if (o == 0) {
		out[0] = '/';
		out[1] = '\0';
	}
	return 0;
}

int fsu_mkdir_p(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	size_t n = strlen(path);
	struct stat st;

	if (n == 0 || n >= sizeof(buf))
		return -ENAMETOOLONG;
	memcpy(buf, path, n + 1);
	for (char *p = buf + 1; ; p++) {
		if (*p != '/' && *p != '\0')
			continue;
		{
			char c = *p;

			*p = '\0';
			if (mkdir(buf, mode) != 0 && errno != EEXIST)
				return -errno;
			if (stat(buf, &st) != 0)
				return -errno;
			if (!S_ISDIR(st.st_mode))
				return -ENOTDIR;
			*p = c;
			if (c == '\0')
				break;
		}
	}
	return 0;
}

/* ---- filesystem checkers ------------------------------------------------- */

int fsu_type_is_fat(const char *type)
{
	return type && (!strcmp(type, "vfat") || !strcmp(type, "fat32") ||
			!strcmp(type, "msdos") || !strcmp(type, "fat"));
}

int fsu_type_is_kernel(const char *type)
{
	static const char *const k[] = { "proc", "sysfs", "devfs", "devtmpfs",
					 "devpts", NULL };

	for (int i = 0; type && k[i]; i++)
		if (!strcmp(type, k[i]))
			return 1;
	return 0;
}

static int fsu_executable(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	       access(path, X_OK) == 0;
}

int fsu_fsck_program(const char *type, char *prog, size_t len,
		     const char **arg)
{
	char cand[FSU_PATH_MAX];

	if (!type || !type[0] || !strcmp(type, "auto") ||
	    !strcmp(type, "tmpfs") || !strcmp(type, "swap") ||
	    fsu_type_is_kernel(type))
		return -ENOENT;
	if (strchr(type, '/') || strlen(type) > 32)
		return -EINVAL;
	if (fsu_type_is_fat(type)) {
		static const char *const fat[] = { "/sbin/fsck.vfat",
						   "/sbin/fsck.fat",
						   "/sbin/dosfsck", NULL };

		for (int i = 0; fat[i]; i++) {
			if (fsu_executable(fat[i])) {
				if (strlen(fat[i]) >= len)
					return -ENAMETOOLONG;
				strcpy(prog, fat[i]);
				*arg = "-a";
				return 0;
			}
		}
		return -ENOENT;
	}
	snprintf(cand, sizeof(cand), "/sbin/fsck.%s", type);
	if (!fsu_executable(cand) &&
	    (!strcmp(type, "ext4") || !strcmp(type, "ext3") ||
	     !strcmp(type, "ext2")))
		snprintf(cand, sizeof(cand), "/sbin/e2fsck");
	if (!fsu_executable(cand))
		return -ENOENT;
	if (strlen(cand) >= len)
		return -ENAMETOOLONG;
	strcpy(prog, cand);
	*arg = "-p";
	return 0;
}
