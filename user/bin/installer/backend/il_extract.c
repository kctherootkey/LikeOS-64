/* libinstaller -- the streaming tar.zst extractor.
 *
 * Unpacks the system image (likeos-rootfs.tar.zst) into the mounted target
 * in one pass: the archive is read once, decompressed as a stream (zstd,
 * any number of frames; a plain uncompressed tar is taken as well), hashed
 * on the way (SHA-256 of the compressed bytes, compared with the .sha256
 * file at the end) and written out entry by entry.
 *
 * What it understands of the tar format (headers in il_tar.c):
 *   ustar and GNU headers, the old v7 layout, octal and base-256 numbers,
 *   GNU long names and link names ('L', 'K'), pax extended headers per
 *   entry ('x') and global ('g'; its path, linkpath and size records are
 *   ignored, see il_x_strip_global), time stamps with nanoseconds, extended
 *   attributes (SCHILY.xattr.*, LIBARCHIVE.xattr.*), POSIX ACLs
 *   (SCHILY.acl.access / .default, written as system.posix_acl_* values),
 *   sparse files (GNU formats 0.0, 0.1, 1.0 and the old 'S' header);
 *   regular files, hard links, symbolic links, character and block
 *   devices, FIFOs, directories, contiguous files (as regular files).
 *
 * Safety: every name is taken relative to the target; a leading '/' is
 * removed, a ".." component refuses the archive.  Directories are walked
 * component by component with O_NOFOLLOW|O_DIRECTORY from a descriptor of
 * the target (each component also checked with lstat, for a kernel that
 * would not honour O_NOFOLLOW), so no symbolic link -- one in the archive or
 * one already on the disk -- is ever followed out of the target.  Files are
 * created with O_EXCL|O_NOFOLLOW after whatever was in their place is
 * removed.
 *
 * Metadata: owner (numeric ids) first, then the mode (so set-id bits
 * survive the chown), then extended attributes and ACLs (after the chown,
 * which would drop a capability attribute), then the time stamps.
 * Directories are created writable and get their metadata in a final pass,
 * deepest first, so a read-only directory can still be filled and a
 * directory's time stamps are not disturbed by what is put into it.
 *
 * The ESP is mounted at boot/efi below the target and does not keep owners,
 * modes, attributes or links; anything below a directory on a different
 * file system than the target root is written best effort: what that file
 * system refuses is logged, not fatal.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#ifndef IL_NO_ZSTD
#include <zstd.h>
#endif

#include "il_internal.h"
#include "il_tar.h"

/* Limits that keep a damaged archive from eating the memory. */
#define IL_X_MAX_LONGNAME    (1u << 20)          /* 'L' / 'K' payload */
#define IL_X_MAX_PAX         (16u << 20)         /* one pax header */
#define IL_X_MAX_SPARSE_MAP  (1u << 22)          /* segments of one file */
#define IL_X_PLAIN_BUFSIZE   (256u * 1024u)
#define IL_X_CANCEL_EVERY    (4ULL << 20)        /* bytes between checks */
#define IL_X_WARN_LIMIT      10
#define IL_X_MAX_COMPONENTS  1024

#define IL_X_ZSTD_MAGIC      0xfd2fb528u
#define IL_X_ZSTD_SKIP_MAGIC 0x184d2a50u         /* ..5f: skippable frames */

/* Kinds of warnings, each rate-limited on its own. */
enum {
	W_OWNER = 0,
	W_MODE,
	W_XATTR,
	W_ACL,
	W_TIME,
	W_SKIP,
	W_MISC,
	W_COUNT
};

static const char *const il_x_warn_names[W_COUNT] = {
	"owner", "permission", "extended attribute", "ACL", "time stamp",
	"skipped entry", "other",
};

/* ------------------------------------------------------------------ */
/* The archive reader                                                  */
/* ------------------------------------------------------------------ */

struct il_rd {
	int fd;
	uint64_t raw_size;        /* size of the archive file */
	uint64_t raw_pos;         /* bytes read from it */
	struct il_sha256 sha;     /* over every byte read */
	int zstd;                 /* compressed stream */
	int raw_eof;
	uint8_t *in;              /* raw input */
	size_t insize;
	size_t in_pos;
	size_t in_len;
	uint8_t *out;             /* decompressed output (zstd) */
	size_t outsize;
	const uint8_t *buf;       /* what get() serves: in or out */
	size_t pos;
	size_t len;
	uint64_t tar_off;         /* uncompressed bytes consumed */
#ifndef IL_NO_ZSTD
	ZSTD_DStream *zs;
	size_t zret;              /* last ZSTD_decompressStream result */
#endif
};

/* ------------------------------------------------------------------ */
/* State of one extraction                                             */
/* ------------------------------------------------------------------ */

/* Metadata to put on an object. */
struct il_x_meta {
	uint32_t mode;            /* permission bits incl. set-id, sticky */
	uint64_t uid;
	uint64_t gid;
	struct timespec ts[2];    /* atime, mtime */
	struct il_tar_xattr *xattrs;
	size_t nxattrs;
	uint8_t *acl_acc;
	size_t acl_acc_len;
	uint8_t *acl_def;
	size_t acl_def_len;
};

/* A directory whose metadata waits for the final pass. */
struct il_x_dir {
	char *rel;                /* "" = the target itself */
	int depth;
	size_t seq;
	struct il_x_meta m;       /* owns xattrs and ACL values */
};

/* One archive member, assembled from its header and extensions. */
struct il_x_entry {
	char type;
	char *path;               /* normalised, relative ("" = the root) */
	char *link;               /* symbolic link text / hard link target */
	uint64_t size;            /* data bytes in the archive */
	uint64_t devmajor;
	uint64_t devminor;
	int sparse;
	int sparse_in_data;       /* GNU 1.0: the map heads the data */
	uint64_t realsize;
	uint64_t hdr_off;         /* archive offset of the header */
	struct il_x_meta m;       /* xattrs point into the pax state */
	int acl_acc_owned;
};

struct il_x {
	il_ctx *ctx;
	const struct il_extract_opts *o;
	struct il_extract_stats *st;
	struct il_rd rd;

	char root[IL_PATH_MAX];
	int rootfd;
	dev_t rootdev;

	/* the parent directory used last */
	char *cache_dir;
	int cache_fd;
	dev_t cache_dev;

	struct il_tar_pax global;  /* 'g' records */
	struct il_tar_pax local;   /* 'x' records of the next entry */
	char *longname;
	char *longlink;

	struct il_x_dir *dirs;
	size_t ndirs;
	size_t dircap;

	unsigned warn[W_COUNT];
	int told_abs;
	int told_global_names;
	uint64_t last_progress;
	unsigned interval;
	uint64_t since_cancel_check;
	/* The entry being unpacked, as the progress reports name it: a copy,
	 * since the entry's own name is freed before the report after it. */
	char *cur;

	/* scratch for full paths (heap: frames stay small) */
	char *full;
	char *full2;
	uint8_t *blk;              /* one header block */
};

/* ---- warnings ----------------------------------------------------- */

static void il_x_warn(struct il_x *x, int kind, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void il_x_warn(struct il_x *x, int kind, const char *fmt, ...)
{
	char msg[512];
	va_list ap;

	x->warn[kind]++;
	if (x->warn[kind] > IL_X_WARN_LIMIT)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	il_log(x->ctx, IL_LOG_WARN, "%s", msg);
	if (x->warn[kind] == IL_X_WARN_LIMIT)
		il_log(x->ctx, IL_LOG_WARN, "further %s warnings are counted, "
		       "not shown", il_x_warn_names[kind]);
}

/* ---- progress ------------------------------------------------------- */

static void il_x_progress(struct il_x *x, int force)
{
	struct il_extract_progress p;
	uint64_t now;

	if (!x->o->progress)
		return;
	now = il_now_ms();
	if (!force && now - x->last_progress < x->interval)
		return;
	x->last_progress = now;
	p.compressed_done = x->rd.raw_pos;
	p.compressed_total = x->rd.raw_size;
	p.bytes_done = x->st->bytes;
	p.files_done = x->st->entries;
	p.path = x->cur;
	x->o->progress(&p, x->o->user);
}

static void il_x_pause(uint64_t us)
{
	struct timespec ts;

	if (!us)
		return;
	ts.tv_sec = (time_t)(us / 1000000u);
	ts.tv_nsec = (long)(us % 1000000u) * 1000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static void il_x_set_cur(struct il_x *x, const char *path)
{
	il_strlcpy(x->cur, path ? path : "", IL_PATH_MAX);
}

static int il_x_check_cancel(struct il_x *x)
{
	if (il_cancelled(x->ctx))
		return il_set_error(x->ctx, ECANCELED, "the installation was "
				    "cancelled while copying files");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Reading the archive                                                 */
/* ------------------------------------------------------------------ */

static ssize_t il_rd_raw(struct il_x *x, uint8_t *dst, size_t len)
{
	struct il_rd *r = &x->rd;

	for (;;) {
		ssize_t n = read(r->fd, dst, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return il_set_error(x->ctx, errno, "%s: read error at "
					    "byte %llu: %s", x->o->archive,
					    (unsigned long long)r->raw_pos,
					    strerror(errno));
		}
		if (n > 0) {
			il_sha256_update(&r->sha, dst, (size_t)n);
			r->raw_pos += (uint64_t)n;
		}
		return n;
	}
}

/* Makes data available in r->buf; 1 = data, 0 = end of stream, < 0 error. */
static int il_rd_fill(struct il_x *x)
{
	struct il_rd *r = &x->rd;

	if (r->pos < r->len)
		return 1;
	if (!r->zstd) {
		ssize_t n;

		if (r->raw_eof)
			return 0;
		n = il_rd_raw(x, r->in, r->insize);
		if (n < 0)
			return (int)n;
		if (n == 0) {
			r->raw_eof = 1;
			return 0;
		}
		r->buf = r->in;
		r->pos = 0;
		r->len = (size_t)n;
		return 1;
	}
#ifndef IL_NO_ZSTD
	for (;;) {
		ZSTD_inBuffer ib;
		ZSTD_outBuffer ob;
		size_t ret;

		if (r->in_pos == r->in_len && !r->raw_eof) {
			ssize_t n = il_rd_raw(x, r->in, r->insize);

			if (n < 0)
				return (int)n;
			if (n == 0)
				r->raw_eof = 1;
			r->in_pos = 0;
			r->in_len = (size_t)n;
		}
		ib.src = r->in;
		ib.size = r->in_len;
		ib.pos = r->in_pos;
		ob.dst = r->out;
		ob.size = r->outsize;
		ob.pos = 0;
		ret = ZSTD_decompressStream(r->zs, &ob, &ib);
		if (ZSTD_isError(ret))
			return il_set_error(x->ctx, EBADMSG, "%s: damaged "
					    "compressed data near byte %llu: %s",
					    x->o->archive,
					    (unsigned long long)r->raw_pos,
					    ZSTD_getErrorName(ret));
		r->in_pos = ib.pos;
		r->zret = ret;
		if (ob.pos) {
			r->buf = r->out;
			r->pos = 0;
			r->len = ob.pos;
			return 1;
		}
		if (r->in_pos == r->in_len && r->raw_eof) {
			if (r->zret != 0)
				return il_set_error(x->ctx, EBADMSG, "%s: the "
						    "compressed stream is "
						    "truncated (%llu bytes)",
						    x->o->archive,
						    (unsigned long long)
						    r->raw_pos);
			return 0;
		}
	}
#else
	return il_set_error(x->ctx, ENOSYS, "internal: compressed stream "
			    "without zstd");
#endif
}

/* Up to max bytes in place; returns the count, 0 at the end, < 0 error. */
static ssize_t il_rd_get(struct il_x *x, size_t max, const uint8_t **p)
{
	struct il_rd *r = &x->rd;
	size_t n;
	int rc = il_rd_fill(x);

	if (rc <= 0)
		return rc;
	n = r->len - r->pos;
	if (n > max)
		n = max;
	*p = r->buf + r->pos;
	r->pos += n;
	r->tar_off += n;
	return (ssize_t)n;
}

/* Reads up to len bytes; returns how many (short only at the end). */
static ssize_t il_rd_read_upto(struct il_x *x, void *dst, size_t len)
{
	size_t got = 0;

	while (got < len) {
		const uint8_t *p;
		ssize_t n = il_rd_get(x, len - got, &p);

		if (n < 0)
			return n;
		if (n == 0)
			break;
		memcpy((uint8_t *)dst + got, p, (size_t)n);
		got += (size_t)n;
	}
	return (ssize_t)got;
}

static int il_rd_truncated(struct il_x *x, const char *what)
{
	return il_set_error(x->ctx, EBADMSG, "%s: the archive ends in the "
			    "middle of %s (archive offset %llu); it is "
			    "truncated or damaged", x->o->archive, what,
			    (unsigned long long)x->rd.tar_off);
}

static int il_rd_read(struct il_x *x, void *dst, size_t len, const char *what)
{
	ssize_t n = il_rd_read_upto(x, dst, len);

	if (n < 0)
		return (int)n;
	if ((size_t)n != len)
		return il_rd_truncated(x, what);
	return 0;
}

static int il_rd_skip(struct il_x *x, uint64_t len, const char *what)
{
	while (len) {
		const uint8_t *p;
		ssize_t n = il_rd_get(x, len > SIZE_MAX ? SIZE_MAX : (size_t)len,
				      &p);

		if (n < 0)
			return (int)n;
		if (n == 0)
			return il_rd_truncated(x, what);
		len -= (uint64_t)n;
	}
	return 0;
}

/* Reads the rest of the file so that the hash covers all of it. */
static int il_rd_drain(struct il_x *x)
{
	struct il_rd *r = &x->rd;

	while (!r->raw_eof) {
		ssize_t n = il_rd_raw(x, r->in, r->insize);

		if (n < 0)
			return (int)n;
		if (n == 0)
			r->raw_eof = 1;
		/* Keep the decompressor's view consistent (nothing more is
		 * decompressed after the end of the archive). */
		r->in_pos = r->in_len = 0;
	}
	return 0;
}

static int il_rd_open(struct il_x *x)
{
	struct il_rd *r = &x->rd;
	struct stat st;
	ssize_t n;
	uint32_t magic = 0;

	memset(r, 0, sizeof(*r));
	r->fd = -1;
	r->fd = open(x->o->archive, O_RDONLY | O_CLOEXEC);
	if (r->fd < 0)
		return il_set_error(x->ctx, errno, "cannot open the system "
				    "image %s: %s", x->o->archive,
				    strerror(errno));
	if (fstat(r->fd, &st) != 0)
		return il_set_error(x->ctx, errno, "%s: %s", x->o->archive,
				    strerror(errno));
	if (!S_ISREG(st.st_mode))
		return il_set_error(x->ctx, EINVAL, "%s is not a regular file",
				    x->o->archive);
	r->raw_size = (uint64_t)st.st_size;
	il_sha256_init(&r->sha);
#ifndef IL_NO_ZSTD
	r->insize = ZSTD_DStreamInSize();
	if (r->insize < IL_X_PLAIN_BUFSIZE)
		r->insize = IL_X_PLAIN_BUFSIZE;
#else
	r->insize = IL_X_PLAIN_BUFSIZE;
#endif
	r->in = malloc(r->insize);
	if (!r->in)
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	/* The first bytes decide the format. */
	{
		size_t got = 0;

		while (got < 4) {
			n = il_rd_raw(x, r->in + got, r->insize - got);
			if (n < 0)
				return (int)n;
			if (n == 0) {
				r->raw_eof = 1;
				break;
			}
			got += (size_t)n;
		}
		r->in_len = got;
	}
	if (r->in_len >= 4)
		magic = (uint32_t)r->in[0] | (uint32_t)r->in[1] << 8 |
			(uint32_t)r->in[2] << 16 | (uint32_t)r->in[3] << 24;
	if (magic == IL_X_ZSTD_MAGIC ||
	    (magic & 0xfffffff0u) == IL_X_ZSTD_SKIP_MAGIC) {
#ifndef IL_NO_ZSTD
		r->zstd = 1;
		r->outsize = ZSTD_DStreamOutSize();
		r->out = malloc(r->outsize);
		r->zs = ZSTD_createDStream();
		if (!r->out || !r->zs)
			return il_set_error(x->ctx, ENOMEM, "out of memory for "
					    "the decompressor");
		{
			size_t z = ZSTD_initDStream(r->zs);

			if (ZSTD_isError(z))
				return il_set_error(x->ctx, EIO, "zstd: %s",
						    ZSTD_getErrorName(z));
		}
		r->in_pos = 0;
		il_log(x->ctx, IL_LOG_DEBUG, "%s: zstd compressed",
		       x->o->archive);
#else
		return il_set_error(x->ctx, ENOSYS, "%s is zstd compressed, "
				    "but this installer was built without "
				    "zstd; it can only unpack an uncompressed "
				    "tar archive", x->o->archive);
#endif
	} else {
		/* Served straight from the input buffer. */
		r->zstd = 0;
		r->buf = r->in;
		r->pos = 0;
		r->len = r->in_len;
		il_log(x->ctx, IL_LOG_DEBUG, "%s: not compressed",
		       x->o->archive);
	}
	return 0;
}

static void il_rd_close(struct il_rd *r)
{
	if (r->fd >= 0)
		close(r->fd);
	r->fd = -1;
#ifndef IL_NO_ZSTD
	if (r->zs)
		ZSTD_freeDStream(r->zs);
	r->zs = NULL;
#endif
	free(r->in);
	free(r->out);
	r->in = r->out = NULL;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* Turns an archive name into a clean relative path: no leading '/', no
 * "." or empty components; ".." is refused.  *out is malloc'd. */
static int il_x_normalize(struct il_x *x, const char *in, char **out)
{
	size_t len = strlen(in);
	char *res = malloc(len + 1);
	size_t o = 0;
	const char *p = in;
	int ncomp = 0;

	*out = NULL;
	if (!res)
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	if (*p == '/') {
		while (*p == '/')
			p++;
		if (!x->told_abs) {
			x->told_abs = 1;
			il_log(x->ctx, IL_LOG_WARN, "removing the leading '/' "
			       "from names in the archive (%s)", in);
		}
	}
	while (*p) {
		const char *e = strchr(p, '/');
		size_t n = e ? (size_t)(e - p) : strlen(p);

		if (n == 0 || (n == 1 && p[0] == '.')) {
			/* skip */
		} else if (n == 2 && p[0] == '.' && p[1] == '.') {
			free(res);
			return il_set_error(x->ctx, EPERM, "%s: the archive "
					    "names a path with '..'; refusing "
					    "to unpack it", in);
		} else {
			if (++ncomp > IL_X_MAX_COMPONENTS) {
				free(res);
				return il_set_error(x->ctx, ENAMETOOLONG,
						    "%s: too many path "
						    "components", in);
			}
			if (o)
				res[o++] = '/';
			memcpy(res + o, p, n);
			o += n;
		}
		p += n;
		while (*p == '/')
			p++;
	}
	res[o] = '\0';
	if (strlen(x->root) + 2 + o >= IL_PATH_MAX) {
		free(res);
		return il_set_error(x->ctx, ENAMETOOLONG, "%.200s...: the name "
				    "is too long", in);
	}
	*out = res;
	return 0;
}

/* <root>/<rel[0..rellen)> into buf (IL_PATH_MAX bytes), cut to fit (the
 * names were checked against the limit when they were normalised). */
static void il_x_join(struct il_x *x, char *buf, const char *rel,
		      size_t rellen)
{
	size_t rl = strlen(x->root);

	if (rl > IL_PATH_MAX - 1)
		rl = IL_PATH_MAX - 1;
	memcpy(buf, x->root, rl);
	if (rellen && rl + 1 < IL_PATH_MAX) {
		buf[rl++] = '/';
		if (rellen > IL_PATH_MAX - 1 - rl)
			rellen = IL_PATH_MAX - 1 - rl;
		memcpy(buf + rl, rel, rellen);
		rl += rellen;
	}
	buf[rl] = '\0';
}

static void il_x_fullpath(struct il_x *x, char *buf, const char *rel)
{
	il_x_join(x, buf, rel, strlen(rel));
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

/* Opens the directory dir (relative) by walking it component by component
 * from `start' (a descriptor of the directory `startrel'), never following
 * a symbolic link.  Missing components are created (0755) when create is
 * set.  Returns a new descriptor or < 0 with the error set. */
static int il_x_walk_from(struct il_x *x, int start, size_t startlen,
			  const char *dir, int create)
{
	char *full = x->full2;
	int cur;
	const char *p;

	cur = openat(start, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (cur < 0)
		return il_set_error(x->ctx, errno, "%s: %s", x->root,
				    strerror(errno));
	p = dir + startlen;
	while (*p == '/')
		p++;
	while (*p) {
		const char *e = strchr(p, '/');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		char comp[256];
		struct stat lst;
		int nfd;

		if (n >= sizeof(comp)) {
			close(cur);
			return il_set_error(x->ctx, ENAMETOOLONG, "%s: a path "
					    "component is too long", dir);
		}
		memcpy(comp, p, n);
		comp[n] = '\0';
		/* <root>/<dir up to and including this component> */
		il_x_join(x, full, dir, (size_t)(p + n - dir));
		if (lstat(full, &lst) == 0) {
			if (S_ISLNK(lst.st_mode)) {
				close(cur);
				return il_set_error(x->ctx, ELOOP, "%.*s is a "
						    "symbolic link; refusing "
						    "to unpack through it",
						    (int)(p + n - dir), dir);
			}
			if (!S_ISDIR(lst.st_mode)) {
				close(cur);
				return il_set_error(x->ctx, ENOTDIR, "%.*s is "
						    "not a directory",
						    (int)(p + n - dir), dir);
			}
		} else if (errno == ENOENT && create) {
			if (il_mkdirat(cur, comp, full, 0755) != 0 &&
			    errno != EEXIST) {
				int e2 = errno;

				close(cur);
				return il_set_error(x->ctx, e2, "cannot create "
						    "the directory %s: %s",
						    full, strerror(e2));
			}
		} else {
			int e2 = errno;

			close(cur);
			return il_set_error(x->ctx, e2, "%s: %s", full,
					    strerror(e2));
		}
		nfd = openat(cur, comp,
			     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (nfd < 0) {
			int e2 = errno;

			close(cur);
			if (e2 == ELOOP)
				return il_set_error(x->ctx, ELOOP, "%s is a "
						    "symbolic link; refusing "
						    "to unpack through it",
						    full);
			return il_set_error(x->ctx, e2, "cannot open the "
					    "directory %s: %s", full,
					    strerror(e2));
		}
		close(cur);
		cur = nfd;
		p += n;
		while (*p == '/')
			p++;
	}
	return cur;
}

static void il_x_cache_drop(struct il_x *x)
{
	if (x->cache_fd >= 0)
		close(x->cache_fd);
	x->cache_fd = -1;
	free(x->cache_dir);
	x->cache_dir = NULL;
}

/* After `rel' was removed or replaced: a cached descriptor of it (or of
 * something below it) would point at an object no longer in the tree. */
static void il_x_cache_invalidate(struct il_x *x, const char *rel)
{
	size_t n = strlen(rel);

	if (!x->cache_dir)
		return;
	if (strncmp(x->cache_dir, rel, n) == 0 &&
	    (x->cache_dir[n] == '\0' || x->cache_dir[n] == '/'))
		il_x_cache_drop(x);
}

/* The directory holding rel's last component.  *base points into rel.
 * The descriptor belongs to the cache (never closed by the caller). */
static int il_x_parent(struct il_x *x, const char *rel, const char **base,
		       int *dfd, dev_t *dev)
{
	const char *slash = strrchr(rel, '/');
	size_t dlen = slash ? (size_t)(slash - rel) : 0;
	char *dir;
	int fd;
	struct stat st;

	*base = slash ? slash + 1 : rel;
	if (x->cache_dir && strlen(x->cache_dir) == dlen &&
	    strncmp(x->cache_dir, rel, dlen) == 0) {
		*dfd = x->cache_fd;
		*dev = x->cache_dev;
		return 0;
	}
	dir = malloc(dlen + 1);
	if (!dir)
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	memcpy(dir, rel, dlen);
	dir[dlen] = '\0';
	/* Descend from the cached directory when the new one is below it
	 * (sorted archives go down the tree that way). */
	if (x->cache_dir && x->cache_dir[0] &&
	    strncmp(dir, x->cache_dir, strlen(x->cache_dir)) == 0 &&
	    dir[strlen(x->cache_dir)] == '/')
		fd = il_x_walk_from(x, x->cache_fd, strlen(x->cache_dir), dir,
				    1);
	else
		fd = il_x_walk_from(x, x->rootfd, 0, dir, 1);
	if (fd < 0) {
		free(dir);
		return fd;
	}
	if (fstat(fd, &st) != 0) {
		int e = errno;

		close(fd);
		free(dir);
		return il_set_error(x->ctx, e, "%s: %s", rel, strerror(e));
	}
	il_x_cache_drop(x);
	x->cache_dir = dir;
	x->cache_fd = fd;
	x->cache_dev = st.st_dev;
	*dfd = fd;
	*dev = st.st_dev;
	return 0;
}

/* Clears the way for a new object at rel.  Returns 1 when a directory is
 * there and keep_dir is set (it stays), 0 when the name is free now. */
static int il_x_clear(struct il_x *x, int dfd, const char *base,
		      const char *full, const char *rel, int keep_dir)
{
	struct stat st;

	if (lstat(full, &st) != 0) {
		if (errno == ENOENT)
			return 0;
		return il_set_error(x->ctx, errno, "%s: %s", full,
				    strerror(errno));
	}
	if (S_ISDIR(st.st_mode)) {
		if (keep_dir)
			return 1;
		if (unlinkat(dfd, base, AT_REMOVEDIR) != 0) {
			if (errno == ENOTEMPTY || errno == EEXIST)
				return il_set_error(x->ctx, errno, "%s: a "
						    "directory that is not "
						    "empty is in the way",
						    full);
			return il_set_error(x->ctx, errno, "cannot remove the "
					    "directory %s: %s", full,
					    strerror(errno));
		}
		il_x_cache_invalidate(x, rel);
		return 0;
	}
	if (unlinkat(dfd, base, 0) != 0 && errno != ENOENT)
		return il_set_error(x->ctx, errno, "cannot replace %s: %s",
				    full, strerror(errno));
	return 0;
}

/* ------------------------------------------------------------------ */
/* Metadata                                                            */
/* ------------------------------------------------------------------ */

static int il_x_owner_failed(struct il_x *x, const char *rel, int err,
			     int best_effort)
{
	x->st->owner_failures++;
	if (best_effort || err == ENOSYS) {
		il_log(x->ctx, IL_LOG_DEBUG, "%s: owner not set: %s", rel,
		       strerror(err));
		return 0;
	}
	if (x->o->owner_errors_fatal)
		return il_set_error(x->ctx, err, "%s: cannot set the owner: %s",
				    rel, strerror(err));
	il_x_warn(x, W_OWNER, "%s: owner not set: %s", rel, strerror(err));
	return 0;
}

static int il_x_mode_failed(struct il_x *x, const char *rel, int err,
			    int best_effort)
{
	if (best_effort) {
		il_x_warn(x, W_MODE, "%s: permissions not set: %s", rel,
			  strerror(err));
		return 0;
	}
	return il_set_error(x->ctx, err, "%s: cannot set the permissions: %s",
			    rel, strerror(err));
}

static void il_x_xattr_failed(struct il_x *x, const char *rel,
			      const char *name, int err, int is_acl)
{
	if (is_acl) {
		x->st->acl_failures++;
		il_x_warn(x, W_ACL, "%s: ACL %s not set: %s", rel, name,
			  strerror(err));
	} else {
		x->st->xattr_failures++;
		il_x_warn(x, W_XATTR, "%s: attribute %s not set: %s", rel,
			  name, strerror(err));
	}
}

static int il_x_ids_ok(struct il_x *x, const char *rel,
		       const struct il_x_meta *m)
{
	if (m->uid > 0xfffffffeULL || m->gid > 0xfffffffeULL) {
		x->st->owner_failures++;
		il_x_warn(x, W_OWNER, "%s: owner %llu:%llu is out of range; "
			  "not set", rel, (unsigned long long)m->uid,
			  (unsigned long long)m->gid);
		return 0;
	}
	return 1;
}

/* The mode to set.  Set-id bits only go on an object that got the owner
 * the archive names: one left with the extractor's owner (an id out of
 * range, a chown that failed) would otherwise be set-id root when the
 * extractor runs as root. */
static mode_t il_x_mode(struct il_x *x, const struct il_x_meta *m, int owned,
			int ids_ok)
{
	mode_t mode = (mode_t)(m->mode & 07777);

	if (x->o->same_owner && !owned && (!ids_ok || geteuid() == 0))
		mode &= (mode_t)~(S_ISUID | S_ISGID);
	return mode;
}

/* Everything through a descriptor (regular files, directories). */
static int il_x_apply_fd(struct il_x *x, int fd, const char *rel,
			 const struct il_x_meta *m, int is_dir,
			 int best_effort, const char *full)
{
	int rc, owned = 0, ids_ok = 1;

	if (x->o->same_owner && (ids_ok = il_x_ids_ok(x, rel, m)) != 0) {
		if (fchown(fd, (uid_t)m->uid, (gid_t)m->gid) != 0) {
			rc = il_x_owner_failed(x, rel, errno, best_effort);
			if (rc)
				return rc;
		} else {
			owned = 1;
		}
	}
	if (fchmod(fd, il_x_mode(x, m, owned, ids_ok)) != 0) {
		rc = il_x_mode_failed(x, rel, errno, best_effort);
		if (rc)
			return rc;
	}
	if (x->o->xattrs) {
		for (size_t i = 0; i < m->nxattrs; i++) {
			if (fsetxattr(fd, m->xattrs[i].name, m->xattrs[i].value,
				      m->xattrs[i].len, 0) != 0)
				il_x_xattr_failed(x, rel, m->xattrs[i].name,
						  errno, 0);
			else
				x->st->xattrs_set++;
		}
	}
	if (x->o->acls) {
		if (m->acl_acc && fsetxattr(fd, IL_ACL_XATTR_ACCESS, m->acl_acc,
					    m->acl_acc_len, 0) != 0)
			il_x_xattr_failed(x, rel, "access", errno, 1);
		if (m->acl_def && is_dir &&
		    fsetxattr(fd, IL_ACL_XATTR_DEFAULT, m->acl_def,
			      m->acl_def_len, 0) != 0)
			il_x_xattr_failed(x, rel, "default", errno, 1);
	}
	if (il_futimens(fd, m->ts) != 0) {
		int e = errno;

		if (e == ENOSYS && full &&
		    utimensat(AT_FDCWD, full, m->ts, 0) == 0)
			return 0;
		x->st->time_failures++;
		il_x_warn(x, W_TIME, "%s: time stamps not set: %s", rel,
			  strerror(e));
	}
	return 0;
}

/* Through the name, never following it (links, devices, FIFOs). */
static int il_x_apply_name(struct il_x *x, int dfd, const char *base,
			   const char *full, const char *rel,
			   const struct il_x_meta *m, int is_symlink,
			   int best_effort)
{
	int rc, owned = 0, ids_ok = 1;

	if (x->o->same_owner && (ids_ok = il_x_ids_ok(x, rel, m)) != 0) {
		if (il_lchownat(dfd, base, full, (uid_t)m->uid, (gid_t)m->gid,
				is_symlink) != 0) {
			rc = il_x_owner_failed(x, rel, errno, best_effort);
			if (rc)
				return rc;
		} else {
			owned = 1;
		}
	}
	if (!is_symlink && il_fchmodat(dfd, base, full,
				       il_x_mode(x, m, owned, ids_ok)) != 0) {
		rc = il_x_mode_failed(x, rel, errno, best_effort);
		if (rc)
			return rc;
	}
	if (x->o->xattrs) {
		for (size_t i = 0; i < m->nxattrs; i++) {
			if (lsetxattr(full, m->xattrs[i].name,
				      m->xattrs[i].value, m->xattrs[i].len,
				      0) != 0)
				il_x_xattr_failed(x, rel, m->xattrs[i].name,
						  errno, 0);
			else
				x->st->xattrs_set++;
		}
	}
	if (x->o->acls && !is_symlink && m->acl_acc &&
	    lsetxattr(full, IL_ACL_XATTR_ACCESS, m->acl_acc, m->acl_acc_len,
		      0) != 0)
		il_x_xattr_failed(x, rel, "access", errno, 1);
	if (il_utimensat_nofollow(dfd, base, full, m->ts) != 0) {
		int e = errno;

		x->st->time_failures++;
		if (e == ENOSYS || best_effort)
			il_log(x->ctx, IL_LOG_DEBUG, "%s: time stamps not set: "
			       "%s", rel, strerror(e));
		else
			il_x_warn(x, W_TIME, "%s: time stamps not set: %s",
				  rel, strerror(e));
	}
	return 0;
}

static void il_x_meta_free(struct il_x_meta *m)
{
	for (size_t i = 0; i < m->nxattrs; i++) {
		free(m->xattrs[i].name);
		free(m->xattrs[i].value);
	}
	free(m->xattrs);
	free(m->acl_acc);
	free(m->acl_def);
	memset(m, 0, sizeof(*m));
}

/* A deep copy of the entry's metadata for the directory pass. */
static int il_x_meta_dup(struct il_x_meta *dst, const struct il_x_meta *src)
{
	*dst = *src;
	dst->xattrs = NULL;
	dst->nxattrs = 0;
	dst->acl_acc = NULL;
	dst->acl_def = NULL;
	if (src->nxattrs) {
		dst->xattrs = calloc(src->nxattrs, sizeof(*dst->xattrs));
		if (!dst->xattrs)
			return -ENOMEM;
		for (size_t i = 0; i < src->nxattrs; i++) {
			dst->xattrs[i].name = strdup(src->xattrs[i].name);
			dst->xattrs[i].value = malloc(src->xattrs[i].len ?
						      src->xattrs[i].len : 1);
			dst->nxattrs = i + 1;
			if (!dst->xattrs[i].name || !dst->xattrs[i].value)
				return -ENOMEM;
			memcpy(dst->xattrs[i].value, src->xattrs[i].value,
			       src->xattrs[i].len);
			dst->xattrs[i].len = src->xattrs[i].len;
		}
	}
	if (src->acl_acc) {
		dst->acl_acc = malloc(src->acl_acc_len);
		if (!dst->acl_acc)
			return -ENOMEM;
		memcpy(dst->acl_acc, src->acl_acc, src->acl_acc_len);
	}
	if (src->acl_def) {
		dst->acl_def = malloc(src->acl_def_len);
		if (!dst->acl_def)
			return -ENOMEM;
		memcpy(dst->acl_def, src->acl_def, src->acl_def_len);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Writing data                                                        */
/* ------------------------------------------------------------------ */

static int il_x_write_all(struct il_x *x, int fd, const uint8_t *p, size_t n,
			  const char *rel)
{
	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ENOSPC)
				return il_set_error(x->ctx, ENOSPC, "the target "
						    "disk is full (writing "
						    "%s)", rel);
			return il_set_error(x->ctx, errno, "%s: write error: "
					    "%s", rel, strerror(errno));
		}
		if (w == 0)
			return il_set_error(x->ctx, EIO, "%s: write error",
					    rel);
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* Copies len bytes of the archive into fd at its current position. */
static int il_x_copy(struct il_x *x, int fd, uint64_t len, const char *rel)
{
	while (len) {
		const uint8_t *p;
		ssize_t n = il_rd_get(x, len > (1u << 30) ? (1u << 30)
							  : (size_t)len, &p);
		int rc;

		if (n < 0)
			return (int)n;
		if (n == 0)
			return il_rd_truncated(x, rel);
		if (fd >= 0) {
			rc = il_x_write_all(x, fd, p, (size_t)n, rel);
			if (rc)
				return rc;
		}
		len -= (uint64_t)n;
		x->st->bytes += (uint64_t)n;
		if (x->o->sim_mib_delay_us)
			il_x_pause(x->o->sim_mib_delay_us * (uint64_t)n >> 20);
		x->since_cancel_check += (uint64_t)n;
		if (x->since_cancel_check >= IL_X_CANCEL_EVERY) {
			x->since_cancel_check = 0;
			rc = il_x_check_cancel(x);
			if (rc)
				return rc;
			il_x_progress(x, 0);
		}
	}
	return 0;
}

static uint64_t il_x_padding(uint64_t size)
{
	return (IL_TAR_BLOCK - size % IL_TAR_BLOCK) % IL_TAR_BLOCK;
}

/* GNU sparse 1.0: the map at the head of the data: decimal numbers, one
 * per line (count, then offset/length pairs), padded to a block.
 * Returns the bytes it took. */
static int il_x_read_sparse_map(struct il_x *x, struct il_x_entry *e,
				uint64_t *used)
{
	uint8_t *blk = x->blk;
	size_t bpos = IL_TAR_BLOCK;     /* exhausted: read the next block */
	uint64_t consumed = 0;
	uint64_t count = 0, numbers = 0, want = 1;
	uint64_t cur = 0, pending_off = 0;
	int digits = 0;
	int rc;

	for (;;) {
		uint8_t c;

		if (bpos == IL_TAR_BLOCK) {
			if (consumed + IL_TAR_BLOCK > e->size)
				goto bad;
			rc = il_rd_read(x, blk, IL_TAR_BLOCK, "a sparse map");
			if (rc)
				return rc;
			consumed += IL_TAR_BLOCK;
			bpos = 0;
		}
		c = blk[bpos++];
		if (c >= '0' && c <= '9') {
			if (cur > (UINT64_MAX - 9) / 10 || ++digits > 20)
				goto bad;
			cur = cur * 10 + (uint64_t)(c - '0');
			continue;
		}
		if (c != '\n' || digits == 0)
			goto bad;
		if (numbers == 0) {
			count = cur;
			if (count > IL_X_MAX_SPARSE_MAP)
				goto bad;
			want = 1 + 2 * count;
		} else if (numbers % 2 == 1) {
			pending_off = cur;
		} else {
			rc = il_tar_pax_add_sparse(&x->local, pending_off, cur);
			if (rc)
				return il_set_error(x->ctx, -rc, "%s: sparse map: "
						    "%s", e->path,
						    strerror(-rc));
		}
		numbers++;
		cur = 0;
		digits = 0;
		if (numbers == want)
			break;
	}
	*used = consumed;
	return 0;
bad:
	return il_set_error(x->ctx, EBADMSG, "%s: damaged sparse map (archive "
			    "offset %llu)", e->path,
			    (unsigned long long)x->rd.tar_off);
}

static int il_x_write_sparse(struct il_x *x, int fd, struct il_x_entry *e)
{
	uint64_t data = e->size, used = 0, sum = 0, end = 0;
	int rc;

	if (e->sparse_in_data) {
		/* The 1.0 map is not in the pax header: it heads the data. */
		x->local.nmap = 0;
		rc = il_x_read_sparse_map(x, e, &used);
		if (rc)
			return rc;
		data -= used;
	}
	for (size_t i = 0; i < x->local.nmap; i++) {
		const struct il_tar_sparse *s = &x->local.map[i];

		if (s->offset < end || s->offset + s->len < s->offset ||
		    s->offset + s->len > e->realsize)
			return il_set_error(x->ctx, EBADMSG, "%s: the sparse "
					    "map is inconsistent (segment %zu)",
					    e->path, i);
		end = s->offset + s->len;
		sum += s->len;
	}
	if (sum != data)
		return il_set_error(x->ctx, EBADMSG, "%s: the sparse map covers "
				    "%llu bytes, the archive holds %llu",
				    e->path, (unsigned long long)sum,
				    (unsigned long long)data);
	for (size_t i = 0; i < x->local.nmap; i++) {
		const struct il_tar_sparse *s = &x->local.map[i];

		if (!s->len)
			continue;
		if (lseek(fd, (off_t)s->offset, SEEK_SET) < 0)
			return il_set_error(x->ctx, errno, "%s: seek: %s",
					    e->path, strerror(errno));
		rc = il_x_copy(x, fd, s->len, e->path);
		if (rc)
			return rc;
	}
	if (ftruncate(fd, (off_t)e->realsize) != 0)
		return il_set_error(x->ctx, errno, "%s: cannot set the size: %s",
				    e->path, strerror(errno));
	return il_rd_skip(x, il_x_padding(e->size), e->path);
}

/* ------------------------------------------------------------------ */
/* Entries                                                             */
/* ------------------------------------------------------------------ */

static int il_x_regular(struct il_x *x, struct il_x_entry *e)
{
	const char *base;
	int dfd, fd, rc;
	dev_t dev;
	int best;

	if (!e->path[0])
		return il_set_error(x->ctx, EINVAL, "the archive has a file "
				    "without a name (offset %llu)",
				    (unsigned long long)e->hdr_off);
	rc = il_x_parent(x, e->path, &base, &dfd, &dev);
	if (rc)
		return rc;
	best = dev != x->rootdev;
	il_x_fullpath(x, x->full, e->path);
	rc = il_x_clear(x, dfd, base, x->full, e->path, 0);
	if (rc < 0)
		return rc;
	fd = openat(dfd, base,
		    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return il_set_error(x->ctx, errno, "cannot create %s: %s",
				    x->full, strerror(errno));
	if (e->sparse)
		rc = il_x_write_sparse(x, fd, e);
	else {
		rc = il_x_copy(x, fd, e->size, e->path);
		if (!rc)
			rc = il_rd_skip(x, il_x_padding(e->size), e->path);
	}
	if (!rc)
		rc = il_x_apply_fd(x, fd, e->path, &e->m, 0, best, x->full);
	if (close(fd) != 0 && !rc)
		rc = il_set_error(x->ctx, errno, "%s: %s", x->full,
				  strerror(errno));
	if (!rc)
		x->st->files++;
	return rc;
}

static int il_x_directory(struct il_x *x, struct il_x_entry *e)
{
	struct il_x_dir *d;
	int rc, depth = 0;

	if (e->path[0]) {
		const char *base;
		int dfd;
		dev_t dev;

		rc = il_x_parent(x, e->path, &base, &dfd, &dev);
		if (rc)
			return rc;
		il_x_fullpath(x, x->full, e->path);
		rc = il_x_clear(x, dfd, base, x->full, e->path, 1);
		if (rc < 0)
			return rc;
		if (rc == 0 && il_mkdirat(dfd, base, x->full, 0700) != 0 &&
		    errno != EEXIST)
			return il_set_error(x->ctx, errno, "cannot create the "
					    "directory %s: %s", x->full,
					    strerror(errno));
		for (const char *p = e->path; *p; p++)
			if (*p == '/')
				depth++;
		depth++;
	}
	if (e->size)
		il_log(x->ctx, IL_LOG_DEBUG, "%s: directory entry with %llu "
		       "data bytes (skipped)", e->path,
		       (unsigned long long)e->size);
	rc = il_rd_skip(x, e->size + il_x_padding(e->size), e->path);
	if (rc)
		return rc;
	if (x->ndirs == x->dircap) {
		size_t cap = x->dircap ? x->dircap * 2 : 256;
		struct il_x_dir *nd = realloc(x->dirs, cap * sizeof(*nd));

		if (!nd)
			return il_set_error(x->ctx, ENOMEM, "out of memory");
		x->dirs = nd;
		x->dircap = cap;
	}
	d = &x->dirs[x->ndirs];
	memset(d, 0, sizeof(*d));
	d->rel = strdup(e->path);
	if (!d->rel)
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	d->depth = depth;
	d->seq = x->ndirs;
	if (il_x_meta_dup(&d->m, &e->m) != 0) {
		il_x_meta_free(&d->m);
		free(d->rel);
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	}
	x->ndirs++;
	x->st->dirs++;
	return 0;
}

static int il_x_symlink(struct il_x *x, struct il_x_entry *e)
{
	const char *base;
	int dfd, rc;
	dev_t dev;
	int best;

	if (!e->path[0] || !e->link || !e->link[0])
		return il_set_error(x->ctx, EINVAL, "a symbolic link without a "
				    "name or target (offset %llu)",
				    (unsigned long long)e->hdr_off);
	rc = il_x_parent(x, e->path, &base, &dfd, &dev);
	if (rc)
		return rc;
	best = dev != x->rootdev;
	il_x_fullpath(x, x->full, e->path);
	rc = il_x_clear(x, dfd, base, x->full, e->path, 0);
	if (rc < 0)
		return rc;
	if (il_symlinkat(e->link, dfd, base, x->full) != 0) {
		if (best) {
			il_x_warn(x, W_MISC, "%s: symbolic link not created on "
				  "this file system: %s", e->path,
				  strerror(errno));
			x->st->skipped++;
			return il_rd_skip(x, e->size + il_x_padding(e->size),
					  e->path);
		}
		return il_set_error(x->ctx, errno, "cannot create the symbolic "
				    "link %s: %s", x->full, strerror(errno));
	}
	rc = il_x_apply_name(x, dfd, base, x->full, e->path, &e->m, 1, best);
	if (rc)
		return rc;
	x->st->symlinks++;
	return il_rd_skip(x, e->size + il_x_padding(e->size), e->path);
}

static int il_x_hardlink(struct il_x *x, struct il_x_entry *e)
{
	char *target = NULL;
	const char *base, *sbase;
	int dfd, sfd, rc;
	dev_t dev;
	char *sdir;
	const char *slash;

	if (!e->path[0] || !e->link)
		return il_set_error(x->ctx, EINVAL, "a hard link without a "
				    "name or target (offset %llu)",
				    (unsigned long long)e->hdr_off);
	rc = il_x_normalize(x, e->link, &target);
	if (rc)
		return rc;
	if (!target[0]) {
		free(target);
		return il_set_error(x->ctx, EINVAL, "%s: hard link to the "
				    "target directory itself", e->path);
	}
	if (strcmp(target, e->path) == 0) {
		free(target);
		x->st->hardlinks++;
		return il_rd_skip(x, e->size + il_x_padding(e->size), e->path);
	}
	rc = il_x_parent(x, e->path, &base, &dfd, &dev);
	if (rc) {
		free(target);
		return rc;
	}
	il_x_fullpath(x, x->full, e->path);
	rc = il_x_clear(x, dfd, base, x->full, e->path, 0);
	if (rc < 0) {
		free(target);
		return rc;
	}
	/* The directory of the link target, walked on its own. */
	slash = strrchr(target, '/');
	sbase = slash ? slash + 1 : target;
	sdir = strndup(target, slash ? (size_t)(slash - target) : 0);
	if (!sdir) {
		free(target);
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	}
	sfd = il_x_walk_from(x, x->rootfd, 0, sdir, 0);
	free(sdir);
	if (sfd < 0) {
		free(target);
		return sfd;
	}
	{
		char *sfull = malloc(IL_PATH_MAX);

		if (!sfull) {
			close(sfd);
			free(target);
			return il_set_error(x->ctx, ENOMEM, "out of memory");
		}
		il_x_fullpath(x, sfull, target);
		if (il_linkat(sfd, sbase, sfull, dfd, base, x->full) != 0)
			rc = il_set_error(x->ctx, errno, "cannot link %s to "
					  "%s: %s", e->path, target,
					  strerror(errno));
		free(sfull);
	}
	close(sfd);
	free(target);
	if (rc)
		return rc;
	x->st->hardlinks++;
	return il_rd_skip(x, e->size + il_x_padding(e->size), e->path);
}

static int il_x_node(struct il_x *x, struct il_x_entry *e)
{
	const char *base;
	int dfd, rc;
	dev_t dev;
	int best;
	mode_t type;

	switch (e->type) {
	case IL_TAR_CHR:
		type = S_IFCHR;
		break;
	case IL_TAR_BLK:
		type = S_IFBLK;
		break;
	default:
		type = S_IFIFO;
		break;
	}
	if (!e->path[0])
		return il_set_error(x->ctx, EINVAL, "a device node without a "
				    "name (offset %llu)",
				    (unsigned long long)e->hdr_off);
	if (e->devmajor > 0xfffffULL || e->devminor > 0xffffffffULL)
		return il_set_error(x->ctx, EINVAL, "%s: device number %llu:%llu "
				    "is out of range", e->path,
				    (unsigned long long)e->devmajor,
				    (unsigned long long)e->devminor);
	rc = il_x_parent(x, e->path, &base, &dfd, &dev);
	if (rc)
		return rc;
	best = dev != x->rootdev;
	il_x_fullpath(x, x->full, e->path);
	rc = il_x_clear(x, dfd, base, x->full, e->path, 0);
	if (rc < 0)
		return rc;
	if (il_mknodat(dfd, base, x->full, type | 0600,
		       type == S_IFIFO ? 0 :
		       makedev((unsigned)e->devmajor, (unsigned)e->devminor)) !=
	    0) {
		if (best) {
			il_x_warn(x, W_MISC, "%s: node not created on this file "
				  "system: %s", e->path, strerror(errno));
			x->st->skipped++;
			return il_rd_skip(x, e->size + il_x_padding(e->size),
					  e->path);
		}
		return il_set_error(x->ctx, errno, "cannot create %s %s: %s",
				    type == S_IFIFO ? "the FIFO" :
				    "the device node", x->full,
				    strerror(errno));
	}
	rc = il_x_apply_name(x, dfd, base, x->full, e->path, &e->m, 0, best);
	if (rc)
		return rc;
	if (type == S_IFIFO)
		x->st->fifos++;
	else
		x->st->devices++;
	return il_rd_skip(x, e->size + il_x_padding(e->size), e->path);
}

/* ------------------------------------------------------------------ */
/* The directory pass                                                  */
/* ------------------------------------------------------------------ */

static int il_x_dir_cmp(const void *a, const void *b)
{
	const struct il_x_dir *x = a, *y = b;

	if (x->depth != y->depth)
		return x->depth > y->depth ? -1 : 1;
	return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

static int il_x_finish_dirs(struct il_x *x)
{
	struct stat st;
	int rc = 0;

	il_x_cache_drop(x);
	if (x->ndirs > 1)
		qsort(x->dirs, x->ndirs, sizeof(*x->dirs), il_x_dir_cmp);
	for (size_t i = 0; i < x->ndirs; i++) {
		struct il_x_dir *d = &x->dirs[i];
		int fd;

		if (!d->rel[0]) {
			fd = openat(x->rootfd, ".",
				    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		} else {
			const char *slash = strrchr(d->rel, '/');
			char *pdir = strndup(d->rel, slash ? (size_t)(slash -
								      d->rel)
							   : 0);
			int pfd;

			if (!pdir)
				return il_set_error(x->ctx, ENOMEM, "out of "
						    "memory");
			pfd = il_x_walk_from(x, x->rootfd, 0, pdir, 0);
			free(pdir);
			if (pfd < 0) {
				/* The directory was replaced later in the
				 * archive; nothing to do for it. */
				il_x_warn(x, W_MISC, "%s: directory gone before "
					  "its permissions were set", d->rel);
				il_clear_error(x->ctx);
				continue;
			}
			fd = openat(pfd, slash ? slash + 1 : d->rel,
				    O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
				    O_CLOEXEC);
			close(pfd);
		}
		if (fd < 0) {
			il_x_warn(x, W_MISC, "%s: directory not found for its "
				  "permissions: %s", d->rel[0] ? d->rel : ".",
				  strerror(errno));
			continue;
		}
		il_x_fullpath(x, x->full, d->rel);
		il_x_set_cur(x, d->rel);
		rc = il_x_apply_fd(x, fd, d->rel[0] ? d->rel : ".", &d->m, 1,
				   fstat(fd, &st) == 0 && st.st_dev != x->rootdev,
				   x->full);
		close(fd);
		if (rc)
			return rc;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Assembling an entry                                                 */
/* ------------------------------------------------------------------ */

/* "dir/GNUSparseFile.123/name" -> "dir/name" (the name GNU sparse formats
 * 0.0 and 0.1 put in the header when no other name record exists). */
static void il_x_strip_sparse_dir(char *s)
{
	char *p = strstr(s, "GNUSparseFile.");

	while (p) {
		if (p == s || p[-1] == '/') {
			char *slash = strchr(p, '/');

			if (slash) {
				memmove(p, slash + 1, strlen(slash + 1) + 1);
				return;
			}
		}
		p = strstr(p + 1, "GNUSparseFile.");
	}
}

/* Merges the global and the entry's own xattrs (the entry's win). */
static int il_x_merge_xattrs(struct il_x *x, struct il_x_entry *e)
{
	size_t n = x->global.nxattrs + x->local.nxattrs;
	size_t k = 0;

	e->m.xattrs = NULL;
	e->m.nxattrs = 0;
	if (!n)
		return 0;
	e->m.xattrs = calloc(n, sizeof(*e->m.xattrs));
	if (!e->m.xattrs)
		return -ENOMEM;
	for (size_t i = 0; i < x->global.nxattrs; i++) {
		size_t j;

		for (j = 0; j < x->local.nxattrs; j++)
			if (strcmp(x->global.xattrs[i].name,
				   x->local.xattrs[j].name) == 0)
				break;
		if (j == x->local.nxattrs)
			e->m.xattrs[k++] = x->global.xattrs[i];
	}
	for (size_t j = 0; j < x->local.nxattrs; j++)
		e->m.xattrs[k++] = x->local.xattrs[j];
	e->m.nxattrs = k;
	return 0;
}

static void il_x_acl(struct il_x *x, struct il_x_entry *e, const char *text,
		     int is_default)
{
	char err[160];
	uint8_t *bin = NULL;
	size_t len = 0;
	int base_only = 0;
	int rc;

	if (!text || !text[0] || !x->o->acls)
		return;
	rc = il_acl_from_text(text, &bin, &len, &base_only, err, sizeof(err));
	if (rc) {
		x->st->acl_failures++;
		il_x_warn(x, W_ACL, "%s: %s ACL not used: %s", e->path,
			  is_default ? "default" : "access",
			  rc == -ENOMEM ? "out of memory" : err);
		return;
	}
	if (!bin)
		return;
	if (!is_default && base_only) {
		/* Owner, group and other only: that is the mode. */
		free(bin);
		return;
	}
	if (is_default) {
		e->m.acl_def = bin;
		e->m.acl_def_len = len;
	} else {
		e->m.acl_acc = bin;
		e->m.acl_acc_len = len;
	}
}

static void il_x_entry_free(struct il_x_entry *e)
{
	free(e->path);
	free(e->link);
	free(e->m.xattrs);       /* the values belong to the pax state */
	free(e->m.acl_acc);
	free(e->m.acl_def);
	memset(e, 0, sizeof(*e));
}

/* Reads the payload of a 'L', 'K', 'x' or 'g' header. */
static int il_x_read_meta_payload(struct il_x *x, uint64_t size, size_t limit,
				  char **out, const char *what)
{
	char *buf;
	int rc;

	*out = NULL;
	if (size > limit)
		return il_set_error(x->ctx, EBADMSG, "%s of %llu bytes at "
				    "archive offset %llu is implausibly large",
				    what, (unsigned long long)size,
				    (unsigned long long)x->rd.tar_off);
	buf = malloc((size_t)size + 1);
	if (!buf)
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	rc = il_rd_read(x, buf, (size_t)size, what);
	if (!rc)
		rc = il_rd_skip(x, il_x_padding(size), what);
	if (rc) {
		free(buf);
		return rc;
	}
	buf[size] = '\0';
	*out = buf;
	return 0;
}

static void il_x_reset_local(struct il_x *x)
{
	il_tar_pax_free(&x->local);
	il_tar_pax_init(&x->local);
	free(x->longname);
	free(x->longlink);
	x->longname = NULL;
	x->longlink = NULL;
}

/* A global ('g') header names no entry: a path, linkpath or size record
 * in it would rename, relink or resize every entry after it (all onto one
 * name), so those three are dropped when the header is read.  The other
 * global records (owners, times, attributes) stay in force. */
static void il_x_strip_global(struct il_x *x)
{
	struct il_tar_pax *g = &x->global;

	if (!(g->have & (IL_PAX_PATH | IL_PAX_LINKPATH | IL_PAX_SIZE)))
		return;
	if (!x->told_global_names) {
		x->told_global_names = 1;
		il_log(x->ctx, IL_LOG_DEBUG, "%s: path, linkpath and size "
		       "records of a global pax header are ignored",
		       x->o->archive);
	}
	g->have &= ~(IL_PAX_PATH | IL_PAX_LINKPATH | IL_PAX_SIZE);
	free(g->path);
	g->path = NULL;
	free(g->linkpath);
	g->linkpath = NULL;
	g->size = 0;
}

/* Header + pax + long names -> entry.  The data is not read here, except
 * for the extension blocks of an old GNU sparse header. */
static int il_x_assemble(struct il_x *x, const struct il_tar_hdr_raw *h,
			 uint64_t size, struct il_x_entry *e)
{
	const struct il_tar_pax *L = &x->local, *G = &x->global;
	int magic = il_tar_magic(h);
	char *name = NULL;
	const char *src;
	uint64_t u;
	int64_t t;
	int rc;

	e->type = h->typeflag;
	e->size = size;

	/* name */
	if (L->have & IL_PAX_PATH)
		src = L->path;
	else if (x->longname)
		src = x->longname;
	else
		src = NULL;
	if (src) {
		name = strdup(src);
	} else if (magic == IL_TAR_MAGIC_USTAR && h->u.prefix[0]) {
		size_t pl = strnlen(h->u.prefix, sizeof(h->u.prefix));
		size_t nl = strnlen(h->name, sizeof(h->name));

		name = malloc(pl + nl + 2);
		if (name) {
			memcpy(name, h->u.prefix, pl);
			name[pl] = '/';
			memcpy(name + pl + 1, h->name, nl);
			name[pl + 1 + nl] = '\0';
		}
	} else {
		name = il_tar_field_dup(h->name, sizeof(h->name));
	}
	if (!name)
		return il_set_error(x->ctx, ENOMEM, "out of memory");

	/* An old-style directory: a regular file whose name ends in '/'. */
	if ((e->type == IL_TAR_REG || e->type == IL_TAR_AREG) && name[0] &&
	    name[strlen(name) - 1] == '/')
		e->type = IL_TAR_DIR;

	/* link name */
	if (L->have & IL_PAX_LINKPATH)
		src = L->linkpath;
	else if (x->longlink)
		src = x->longlink;
	else
		src = NULL;
	e->link = src ? strdup(src)
		      : il_tar_field_dup(h->linkname, sizeof(h->linkname));
	if (!e->link) {
		free(name);
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	}

	/* numbers */
	if (il_tar_unumber(h->mode, sizeof(h->mode), &u) != 0)
		goto badnum;
	e->m.mode = (uint32_t)(u & 07777);
	if (L->have & IL_PAX_UID)
		e->m.uid = L->uid;
	else if (G->have & IL_PAX_UID)
		e->m.uid = G->uid;
	else if (il_tar_unumber(h->uid, sizeof(h->uid), &e->m.uid) != 0)
		goto badnum;
	if (L->have & IL_PAX_GID)
		e->m.gid = L->gid;
	else if (G->have & IL_PAX_GID)
		e->m.gid = G->gid;
	else if (il_tar_unumber(h->gid, sizeof(h->gid), &e->m.gid) != 0)
		goto badnum;
	if (L->have & IL_PAX_MTIME) {
		e->m.ts[1].tv_sec = (time_t)L->mtime_sec;
		e->m.ts[1].tv_nsec = L->mtime_nsec;
	} else if (G->have & IL_PAX_MTIME) {
		e->m.ts[1].tv_sec = (time_t)G->mtime_sec;
		e->m.ts[1].tv_nsec = G->mtime_nsec;
	} else {
		if (il_tar_number(h->mtime, sizeof(h->mtime), &t) != 0)
			goto badnum;
		e->m.ts[1].tv_sec = (time_t)t;
		e->m.ts[1].tv_nsec = 0;
	}
	e->m.ts[0] = e->m.ts[1];
	if (L->have & IL_PAX_ATIME) {
		e->m.ts[0].tv_sec = (time_t)L->atime_sec;
		e->m.ts[0].tv_nsec = L->atime_nsec;
	} else if (G->have & IL_PAX_ATIME) {
		e->m.ts[0].tv_sec = (time_t)G->atime_sec;
		e->m.ts[0].tv_nsec = G->atime_nsec;
	} else if (magic == IL_TAR_MAGIC_GNU && h->u.gnu.atime[0] &&
		   il_tar_number(h->u.gnu.atime, sizeof(h->u.gnu.atime), &t) ==
		   0 && t > 0) {
		e->m.ts[0].tv_sec = (time_t)t;
		e->m.ts[0].tv_nsec = 0;
	}
	if (e->type == IL_TAR_CHR || e->type == IL_TAR_BLK) {
		if (L->have & IL_PAX_DEVMAJOR)
			e->devmajor = L->devmajor;
		else if (il_tar_unumber(h->devmajor, sizeof(h->devmajor),
					&e->devmajor) != 0)
			goto badnum;
		if (L->have & IL_PAX_DEVMINOR)
			e->devminor = L->devminor;
		else if (il_tar_unumber(h->devminor, sizeof(h->devminor),
					&e->devminor) != 0)
			goto badnum;
	}

	/* sparse files */
	if (e->type == IL_TAR_GNU_SPARSE) {
		/* Old GNU: four pairs in the header, more in extension blocks
		 * that follow it (not counted in the size). */
		int ext = h->u.gnu.isextended;

		e->sparse = 1;
		x->local.nmap = 0;
		if (il_tar_sparse_parse_old((const char (*)[24])h->u.gnu.sparse,
					    4, &x->local) != 0 ||
		    il_tar_unumber(h->u.gnu.realsize, sizeof(h->u.gnu.realsize),
				   &e->realsize) != 0)
			goto badnum;
		while (ext) {
			const struct il_tar_sparse_ext *se =
				(const void *)x->blk;

			rc = il_rd_read(x, x->blk, IL_TAR_BLOCK,
					"a sparse header");
			if (rc) {
				free(name);
				return rc;
			}
			if (il_tar_sparse_parse_old(se->sparse, 21,
						    &x->local) != 0)
				goto badnum;
			ext = se->isextended;
		}
		e->type = IL_TAR_REG;
	} else if ((L->have & (IL_PAX_SP_MAJOR | IL_PAX_SP_MAP |
			       IL_PAX_SP_REAL)) &&
		   (e->type == IL_TAR_REG || e->type == IL_TAR_AREG ||
		    e->type == IL_TAR_CONT)) {
		e->sparse = 1;
		if (!(L->have & IL_PAX_SP_REAL)) {
			rc = il_set_error(x->ctx, EBADMSG, "%s: sparse file "
					  "without its real size", name);
			free(name);
			return rc;
		}
		e->realsize = L->sp_realsize;
		if ((L->have & IL_PAX_SP_MAJOR) && L->sp_major == 1)
			e->sparse_in_data = 1;
		else if ((L->have & IL_PAX_SP_MAJOR) && L->sp_major > 1) {
			rc = il_set_error(x->ctx, ENOTSUP, "%s: sparse format "
					  "%d.%d is not known", name,
					  L->sp_major, L->sp_minor);
			free(name);
			return rc;
		}
		if (L->have & IL_PAX_SP_NAME) {
			free(name);
			name = strdup(L->sp_name);
			if (!name)
				return il_set_error(x->ctx, ENOMEM, "out of "
						    "memory");
		} else if (!(L->have & IL_PAX_PATH)) {
			il_x_strip_sparse_dir(name);
		}
	}

	rc = il_x_normalize(x, name, &e->path);
	free(name);
	if (rc)
		return rc;
	if (il_x_merge_xattrs(x, e) != 0)
		return il_set_error(x->ctx, ENOMEM, "out of memory");
	il_x_acl(x, e, (L->have & IL_PAX_ACL_ACC) ? L->acl_access :
		       (G->have & IL_PAX_ACL_ACC) ? G->acl_access : NULL, 0);
	if (e->type == IL_TAR_DIR || e->type == IL_TAR_GNU_DUMPDIR)
		il_x_acl(x, e, (L->have & IL_PAX_ACL_DEF) ? L->acl_default :
			       (G->have & IL_PAX_ACL_DEF) ? G->acl_default
							  : NULL, 1);
	if ((L->have & IL_PAX_FILETYPE) &&
	    strcmp(L->filetype, "socket") == 0)
		e->type = 's';    /* not restorable: skipped below */
	return 0;
badnum:
	free(name);
	return il_set_error(x->ctx, EBADMSG, "damaged numeric field in the "
			    "header at archive offset %llu",
			    (unsigned long long)e->hdr_off);
}

static int il_x_entry(struct il_x *x, struct il_x_entry *e)
{
	int rc;

	il_x_set_cur(x, e->path);
	switch (e->type) {
	case IL_TAR_REG:
	case IL_TAR_AREG:
	case IL_TAR_CONT:
		rc = il_x_regular(x, e);
		break;
	case IL_TAR_DIR:
	case IL_TAR_GNU_DUMPDIR:
		rc = il_x_directory(x, e);
		break;
	case IL_TAR_SYM:
		rc = il_x_symlink(x, e);
		break;
	case IL_TAR_LNK:
		rc = il_x_hardlink(x, e);
		break;
	case IL_TAR_CHR:
	case IL_TAR_BLK:
	case IL_TAR_FIFO:
		rc = il_x_node(x, e);
		break;
	default:
		x->st->skipped++;
		il_x_warn(x, W_SKIP, "%s: archive entry of type '%c' (0x%02x) "
			  "left out", e->path[0] ? e->path : "(no name)",
			  e->type >= 0x20 && e->type < 0x7f ? e->type : '?',
			  (unsigned)(unsigned char)e->type);
		rc = il_rd_skip(x, e->size + il_x_padding(e->size), e->path);
		break;
	}
	if (!rc)
		x->st->entries++;
	return rc;
}

/* ------------------------------------------------------------------ */
/* The main loop                                                       */
/* ------------------------------------------------------------------ */

static int il_x_run(struct il_x *x)
{
	struct il_tar_hdr_raw *h = (void *)x->blk;
	int zeros = 0;
	int rc;
	char err[160];

	for (;;) {
		uint64_t off = x->rd.tar_off;
		ssize_t n;
		uint64_t size;
		struct il_x_entry e;

		rc = il_x_check_cancel(x);
		if (rc)
			return rc;
		n = il_rd_read_upto(x, x->blk, IL_TAR_BLOCK);
		if (n < 0)
			return (int)n;
		if (n == 0) {
			if (zeros)
				return 0;
			return il_set_error(x->ctx, EBADMSG, "%s: the archive "
					    "ends without its end marker "
					    "(offset %llu); it is truncated",
					    x->o->archive,
					    (unsigned long long)off);
		}
		if (n != IL_TAR_BLOCK)
			return il_rd_truncated(x, "a header");
		if (il_tar_is_zero_block(x->blk)) {
			if (++zeros == 2)
				return 0;
			continue;
		}
		if (zeros) {
			il_x_warn(x, W_MISC, "%s: a lone zero block at offset "
				  "%llu", x->o->archive,
				  (unsigned long long)(off - IL_TAR_BLOCK));
			zeros = 0;
		}
		if (!il_tar_checksum_ok(x->blk))
			return il_set_error(x->ctx, EBADMSG, "%s: damaged header "
					    "at archive offset %llu (checksum "
					    "mismatch)", x->o->archive,
					    (unsigned long long)off);
		if (il_tar_unumber(h->size, sizeof(h->size), &size) != 0)
			return il_set_error(x->ctx, EBADMSG, "%s: damaged size "
					    "field at archive offset %llu",
					    x->o->archive,
					    (unsigned long long)off);
		switch (h->typeflag) {
		case IL_TAR_PAX:
		case IL_TAR_SUN_PAX:
		case IL_TAR_PAX_G: {
			char *data;
			struct il_tar_pax *p = h->typeflag == IL_TAR_PAX_G ?
					       &x->global : &x->local;

			rc = il_x_read_meta_payload(x, size, IL_X_MAX_PAX,
						    &data, "a pax header");
			if (rc)
				return rc;
			err[0] = '\0';
			rc = il_tar_pax_parse(p, data, (size_t)size, err,
					      sizeof(err));
			free(data);
			if (rc == -ENOMEM)
				return il_set_error(x->ctx, ENOMEM, "out of "
						    "memory");
			if (rc)
				return il_set_error(x->ctx, EBADMSG, "%s: %s "
						    "(archive offset %llu)",
						    x->o->archive, err,
						    (unsigned long long)off);
			if (p == &x->global)
				il_x_strip_global(x);
			continue;
		}
		case IL_TAR_GNU_LONGNAME:
		case IL_TAR_GNU_LONGLINK: {
			char *data;
			char **dst = h->typeflag == IL_TAR_GNU_LONGNAME ?
				     &x->longname : &x->longlink;

			rc = il_x_read_meta_payload(x, size, IL_X_MAX_LONGNAME,
						    &data, "a long name");
			if (rc)
				return rc;
			free(*dst);
			*dst = data;
			continue;
		}
		default:
			break;
		}
		/* The entry's pax size (files of 8 GiB and more) replaces
		 * the header's. */
		if (x->local.have & IL_PAX_SIZE)
			size = x->local.size;
		if (size > (uint64_t)INT64_MAX)
			return il_set_error(x->ctx, EBADMSG, "%s: implausible "
					    "size %llu at archive offset %llu",
					    x->o->archive,
					    (unsigned long long)size,
					    (unsigned long long)off);

		memset(&e, 0, sizeof(e));
		e.hdr_off = off;
		rc = il_x_assemble(x, h, size, &e);
		if (!rc)
			rc = il_x_entry(x, &e);
		il_x_entry_free(&e);
		il_x_reset_local(x);
		if (rc)
			return rc;
		if (x->o->sim_entry_delay_us)
			il_x_pause(x->o->sim_entry_delay_us);
		il_x_progress(x, 0);
	}
}

static int il_x_hex_equal(const char *a, const char *b)
{
	for (int i = 0; i < 64; i++) {
		int ca = (unsigned char)a[i], cb = (unsigned char)b[i];

		if (ca >= 'A' && ca <= 'F')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'F')
			cb += 'a' - 'A';
		if (ca != cb || !ca)
			return 0;
	}
	return 1;
}

static int il_x_valid_hex64(const char *s)
{
	int i;

	for (i = 0; i < 64; i++) {
		char c = s[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
		      (c >= 'A' && c <= 'F')))
			return 0;
	}
	return s[64] == '\0' || s[64] == ' ' || s[64] == '\t' ||
	       s[64] == '\n' || s[64] == '\r';
}

int il_extract_archive(il_ctx *ctx, const struct il_extract_opts *opts,
		       struct il_extract_stats *st)
{
	struct il_x *x;
	struct il_extract_stats local_st;
	struct stat rst;
	uint8_t digest[32];
	const char *expect = NULL;
	uint64_t t0 = il_now_ms();
	int rc;

	if (!st)
		st = &local_st;
	memset(st, 0, sizeof(*st));
	if (!opts || !opts->archive || !opts->target)
		return il_set_error(ctx, EINVAL, "internal: nothing to unpack");
	if (opts->expected_sha256 && opts->expected_sha256[0]) {
		expect = opts->expected_sha256;
		while (*expect == ' ' || *expect == '\t')
			expect++;
		if (!il_x_valid_hex64(expect))
			return il_set_error(ctx, EINVAL, "the expected SHA-256 "
					    "of %s is not a 64-digit hex value",
					    opts->archive);
	}
	x = calloc(1, sizeof(*x));
	if (!x)
		return il_set_error(ctx, ENOMEM, "out of memory");
	x->ctx = ctx;
	x->o = opts;
	x->st = st;
	x->rootfd = -1;
	x->cache_fd = -1;
	x->rd.fd = -1;
	x->interval = opts->progress_interval_ms ? opts->progress_interval_ms
						 : 100;
	il_tar_pax_init(&x->global);
	il_tar_pax_init(&x->local);
	x->full = malloc(IL_PATH_MAX);
	x->full2 = malloc(IL_PATH_MAX);
	x->cur = calloc(1, IL_PATH_MAX);
	x->blk = malloc(IL_TAR_BLOCK);
	if (!x->full || !x->full2 || !x->cur || !x->blk) {
		rc = il_set_error(ctx, ENOMEM, "out of memory");
		goto out;
	}
	if (il_strlcpy(x->root, opts->target, sizeof(x->root)) >=
	    sizeof(x->root) - 2) {
		rc = il_set_error(ctx, ENAMETOOLONG, "the target path is too "
				  "long");
		goto out;
	}
	{
		size_t n = strlen(x->root);

		while (n > 1 && x->root[n - 1] == '/')
			x->root[--n] = '\0';
	}
	x->rootfd = open(x->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (x->rootfd < 0) {
		rc = il_set_error(ctx, errno, "cannot open the target %s: %s",
				  x->root, strerror(errno));
		goto out;
	}
	if (fstat(x->rootfd, &rst) != 0) {
		rc = il_set_error(ctx, errno, "%s: %s", x->root,
				  strerror(errno));
		goto out;
	}
	x->rootdev = rst.st_dev;
	rc = il_rd_open(x);
	if (rc)
		goto out;
	il_log(ctx, IL_LOG_INFO, "unpacking %s (%llu bytes%s) into %s",
	       opts->archive, (unsigned long long)x->rd.raw_size,
	       x->rd.zstd ? ", zstd" : ", uncompressed", x->root);
	il_x_progress(x, 1);
	rc = il_x_run(x);
	if (rc)
		goto out;
	rc = il_x_check_cancel(x);
	if (rc)
		goto out;
	il_x_set_cur(x, NULL);
	rc = il_x_finish_dirs(x);
	if (rc)
		goto out;
	rc = il_rd_drain(x);
	if (rc)
		goto out;
	il_sha256_final(&x->rd.sha, digest);
	il_sha256_hex(digest, st->sha256);
	st->compressed_bytes = x->rd.raw_pos;
	if (expect && !il_x_hex_equal(expect, st->sha256)) {
		rc = il_set_error(ctx, EBADMSG, "the system image %s is "
				  "damaged: its SHA-256 is %s, expected %.64s",
				  opts->archive, st->sha256, expect);
		goto out;
	}
	il_x_set_cur(x, NULL);
	il_x_progress(x, 1);
	{
		uint64_t ms = il_now_ms() - t0;

		il_log(ctx, IL_LOG_INFO, "unpacked %llu entries in %llu.%01llu s: "
		       "%llu files (%llu bytes), %llu directories, %llu "
		       "symbolic links, %llu hard links, %llu devices, %llu "
		       "FIFOs, %llu attributes; sha256 %s%s",
		       (unsigned long long)st->entries,
		       (unsigned long long)(ms / 1000),
		       (unsigned long long)(ms % 1000 / 100),
		       (unsigned long long)st->files,
		       (unsigned long long)st->bytes,
		       (unsigned long long)st->dirs,
		       (unsigned long long)st->symlinks,
		       (unsigned long long)st->hardlinks,
		       (unsigned long long)st->devices,
		       (unsigned long long)st->fifos,
		       (unsigned long long)st->xattrs_set, st->sha256,
		       expect ? " (verified)" : "");
		if (st->owner_failures || st->xattr_failures ||
		    st->acl_failures || st->time_failures || st->skipped)
			il_log(ctx, IL_LOG_WARN, "while unpacking: %llu owners, "
			       "%llu attributes, %llu ACLs, %llu time stamps "
			       "not set; %llu entries left out",
			       (unsigned long long)st->owner_failures,
			       (unsigned long long)st->xattr_failures,
			       (unsigned long long)st->acl_failures,
			       (unsigned long long)st->time_failures,
			       (unsigned long long)st->skipped);
	}
	rc = 0;
out:
	if (rc)
		st->compressed_bytes = x->rd.raw_pos;
	il_x_cache_drop(x);
	if (x->rootfd >= 0)
		close(x->rootfd);
	il_rd_close(&x->rd);
	for (size_t i = 0; i < x->ndirs; i++) {
		free(x->dirs[i].rel);
		il_x_meta_free(&x->dirs[i].m);
	}
	free(x->dirs);
	il_tar_pax_free(&x->global);
	il_tar_pax_free(&x->local);
	free(x->longname);
	free(x->longlink);
	free(x->full);
	free(x->full2);
	free(x->cur);
	free(x->blk);
	free(x);
	return rc;
}
