/* Runner of the filesystem harness (host/test-fs.sh).
 *
 * Mounts one image with the driver under test -- through its
 * vfs_fstype_t, exactly as mount(2) does -- and drives it through its
 * vfs_ops_t the way the VFS does: canonical paths, the read-only mount
 * gate, symbolic links followed by the walk (vfs_link_hop), handles set
 * up as vfs_open_common() sets them, syncfs as mnt_flush() does it.
 *
 * Input is a small command language, one command per line (from -s FILE)
 * or separated by ';' (from -c).  Output on stdout is deterministic and
 * meant to be diffed or grepped; the kernel log goes to stderr.  Every
 * command is expected to succeed unless prefixed with =STATUS (expect
 * that status, e.g. "=EXISTS mkdir /a") or '?' (any result).  A mismatch,
 * a write in trap mode, a WARN/BUG from the kernel code, a leak after
 * unmount or a model mismatch in stress mode prints a "FAIL ..." line and
 * makes the exit status nonzero.  See host/fsimg/README for the commands.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/dirent.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif
#ifndef O_DIRECT
#define O_DIRECT 0x4000
#endif

#ifndef TEST_FSTYPE
#error "build with -DTEST_FSTYPE=<driver>_fstype (host/test-fs.sh does)"
#endif
extern const vfs_fstype_t TEST_FSTYPE;

/* stubs */
extern long test_warnings, test_bugs, test_failures, test_io_errors;
extern int test_quiet_klog, test_klog_prefix, test_have_task;
extern long test_kalloc_live;
extern uint64_t test_kalloc_bytes, test_kalloc_peak, test_kalloc_max,
	test_kalloc_limit, test_kalloc_fail_at, test_kalloc_count;
extern uint64_t test_pages_live, test_pages_peak, test_pages_limit,
	test_free_pages_override;
extern uint64_t test_clock_ns, test_epoch_base, test_yields, test_yield_limit;
extern block_device_t test_bdev;
extern int test_bdev_discard;
extern vfs_mount_t *test_mount_ptr;
void test_task_init(uint32_t uid, uint32_t gid);
void test_task_set_umask(uint32_t mask);
void test_task_set_id(uint64_t id);
void test_bdev_setup(const char *name, int ro);

/* ================================================================== */
/* small utilities                                                     */
/* ================================================================== */

static long g_fail;
static int g_verbose;
static int g_lineno;
static const char *g_curcmd = "";

static int g_silent;
#define OUT(...) (g_silent ? 0 : h_printf(__VA_ARGS__))

static void failf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void failf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	__builtin_va_start(ap, fmt);
	h_vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	g_fail++;
	h_printf("FAIL line %d: %s: %s\n", g_lineno, g_curcmd, buf);
	host_flush_out();
}

static const char *st_name(long st)
{
	static char other[32];

	if (st >= 0)
		return "OK";
	switch (st) {
	case ST_ERR: return "ERR";
	case ST_UNSUPPORTED: return "UNSUPPORTED";
	case ST_NO_DEVICE: return "NO_DEVICE";
	case ST_TIMEOUT: return "TIMEOUT";
	case ST_IO: return "IO";
	case ST_NOT_FOUND: return "NOT_FOUND";
	case ST_EOF: return "EOF";
	case ST_INVALID: return "INVALID";
	case ST_NOMEM: return "NOMEM";
	case ST_AGAIN: return "AGAIN";
	case ST_BUSY: return "BUSY";
	case ST_EXISTS: return "EXISTS";
	case ST_NOTEMPTY: return "NOTEMPTY";
	case ST_ROFS: return "ROFS";
	case ST_NOSPC: return "NOSPC";
	case ST_NODATA: return "NODATA";
	case ST_RANGE: return "RANGE";
	case ST_ACCESS: return "ACCESS";
	case ST_PERM: return "PERM";
	case ST_XDEV: return "XDEV";
	case ST_ISDIR: return "ISDIR";
	case ST_NOTDIR: return "NOTDIR";
	case ST_LOOP: return "LOOP";
	case ST_LINK: return "LINK";
	case ST_NAMETOOLONG: return "NAMETOOLONG";
	case ST_NXIO: return "NXIO";
	case ST_UCLEAN: return "UCLEAN";
	}
	h_snprintf(other, sizeof(other), "ST%ld", st);
	return other;
}

static int st_parse(const char *s, long *out)
{
	static const long all[] = {
		ST_OK, ST_ERR, ST_UNSUPPORTED, ST_NO_DEVICE, ST_TIMEOUT, ST_IO,
		ST_NOT_FOUND, ST_EOF, ST_INVALID, ST_NOMEM, ST_AGAIN, ST_BUSY,
		ST_EXISTS, ST_NOTEMPTY, ST_ROFS, ST_NOSPC, ST_NODATA, ST_RANGE,
		ST_ACCESS, ST_PERM, ST_XDEV, ST_ISDIR, ST_NOTDIR, ST_LOOP,
		ST_LINK, ST_NAMETOOLONG, ST_NXIO, ST_UCLEAN
	};
	if (!h_strncmp(s, "ST_", 3))
		s += 3;
	for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (!h_strcmp(s, st_name(all[i]))) {
			*out = all[i];
			return 0;
		}
	return -1;
}

static int streq(const char *a, const char *b)
{
	return h_strcmp(a, b) == 0;
}

static unsigned long long num(const char *s)
{
	char *e;
	unsigned long long v;

	if (!s)
		return 0;
	v = h_strtoull(s, &e, 0);
	if (*e == 'k' || *e == 'K')
		v <<= 10;
	else if (*e == 'm' || *e == 'M')
		v <<= 20;
	else if (*e == 'g' || *e == 'G')
		v <<= 30;
	return v;
}

/* ---- deterministic PRNG (xorshift64*) ---- */
static uint64_t rng_s = 0x9e3779b97f4a7c15ULL;

static void rng_seed(uint64_t s)
{
	rng_s = s * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL;
	if (!rng_s)
		rng_s = 1;
}

static uint64_t rng(void)
{
	rng_s ^= rng_s >> 12;
	rng_s ^= rng_s << 25;
	rng_s ^= rng_s >> 27;
	return rng_s * 2685821657736338717ULL;
}

static uint64_t rng_below(uint64_t n)
{
	return n ? rng() % n : 0;
}

/* Bytes of the "pattern" data source: position-dependent, so any range
 * of a pattern file can be checked without storing it. */
static uint8_t pat_byte(uint64_t seed, uint64_t pos)
{
	uint64_t x = (pos >> 3) * 0x9e3779b97f4a7c15ULL ^ seed * 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 31;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 29;
	return (uint8_t)(x >> ((pos & 7) * 8));
}

/* ---- SHA-256 ---- */
struct sha256 {
	uint32_t h[8];
	uint8_t buf[64];
	uint64_t len;
	unsigned n;
};

static const uint32_t sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha256 *s, const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;

	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
		       (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + sha_k[i] + w[i];
		uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
		uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + mj;
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(struct sha256 *s)
{
	static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372,
					0xa54ff53a, 0x510e527f, 0x9b05688c,
					0x1f83d9ab, 0x5be0cd19 };
	h_memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
	s->n = 0;
}

static void sha_update(struct sha256 *s, const void *data, unsigned long len)
{
	const uint8_t *p = data;

	s->len += len;
	while (len) {
		unsigned take = 64 - s->n;
		if (take > len)
			take = (unsigned)len;
		h_memcpy(s->buf + s->n, p, take);
		s->n += take;
		p += take;
		len -= take;
		if (s->n == 64) {
			sha_block(s, s->buf);
			s->n = 0;
		}
	}
}

static void sha_final(struct sha256 *s, char hex[65])
{
	uint64_t bits = s->len * 8;
	uint8_t pad = 0x80, z = 0, lenb[8];

	sha_update(s, &pad, 1);
	while (s->n != 56)
		sha_update(s, &z, 1);
	for (int i = 0; i < 8; i++)
		lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha_update(s, lenb, 8);
	for (int i = 0; i < 8; i++)
		h_snprintf(hex + i * 8, 9, "%08x", s->h[i]);
	hex[64] = 0;
}

/* ---- canonical paths (as vfs_canon: no "." "..", no doubled slashes) ---- */
static int canon(const char *in, char *out)
{
	unsigned long n = 0;
	const char *p = in;

	if (!in || in[0] != '/')
		return ST_INVALID;
	out[n++] = '/';
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *s = p;
		while (*p && *p != '/')
			p++;
		unsigned long l = (unsigned long)(p - s);
		if (l == 1 && s[0] == '.')
			continue;
		if (l == 2 && s[0] == '.' && s[1] == '.') {
			if (n > 1) {
				n--;
				while (n > 1 && out[n - 1] != '/')
					n--;
				if (n > 1)
					n--;
			}
			continue;
		}
		if (n > 1)
			out[n++] = '/';
		/* (the name and the NUL after it must fit: a path of
		 * VFS_MAX_PATH - 1 bytes is the longest) */
		if (n + l + 1 > VFS_MAX_PATH)
			return ST_NAMETOOLONG;
		h_memcpy(out + n, s, l);
		n += l;
	}
	if (n > 1 && out[n - 1] == '/')
		n--;
	out[n] = 0;
	return ST_OK;
}

/* ================================================================== */
/* the mount                                                           */
/* ================================================================== */

static vfs_mount_t g_m;
/* The mount point and source text: the kernel gives a slot its two
 * VFS_MAX_PATH-byte buffers once and keeps them (mount.c slot_alloc). */
static char g_m_path[VFS_MAX_PATH], g_m_source[VFS_MAX_PATH];
static int g_mounted;
static int g_raw_ro; /* call the driver even where the VFS refuses (ro) */
static int g_linkhop = 1; /* the VFS follows links (vfs_link_hop) */
static int g_allow_warn;
static int g_leakcheck = 1;
static long g_base_kalloc;
static uint64_t g_base_pages;
static long g_chunk = 65536;
static int g_fsck_after;

static int ro_mount(void)
{
	return (g_m.flags & VFS_MS_RDONLY) != 0;
}

static unsigned long parse_mflags(const char *s, unsigned long base)
{
	if (!s)
		return base;
	if (streq(s, "ro"))
		return base | VFS_MS_RDONLY;
	if (streq(s, "rw"))
		return base & ~VFS_MS_RDONLY;
	return base;
}

static int do_mount(unsigned long flags, const char *opts)
{
	int r;

	if (g_mounted) {
		failf("already mounted");
		return ST_BUSY;
	}
	/* The path-buffer allocator keeps freed buffers on a cache of up to
	 * PATHBUF_CACHE_MAX (64).  Fill that cache once, before the first
	 * baseline, so a buffer a driver takes in a later cycle comes from the
	 * cache instead of showing up as a heap block that cycle left live. */
	static int pathbufs_warm;
	if (!pathbufs_warm) {
		static char *warm[64];
		int n = 0;

		while (n < 64 && (warm[n] = pathbuf_get()) != NULL)
			n++;
		while (n > 0)
			pathbuf_put(warm[--n]);
		pathbufs_warm = 1;
	}
	g_base_kalloc = test_kalloc_live;
	g_base_pages = test_pages_live;
	h_memset(&g_m, 0, sizeof(g_m));
	g_m.path = g_m_path;
	g_m.source = g_m_source;
	g_m.state = VFS_MNT_LIVE;
	g_m.id = 2;
	g_m.parent_id = 1;
	g_m.path[0] = '/';
	g_m.path[1] = '\0';
	g_m.path_len = 1;
	h_snprintf(g_m.source, VFS_MAX_PATH, "/dev/%s", test_bdev.name);
	h_snprintf(g_m.fstype, sizeof(g_m.fstype), "%s", TEST_FSTYPE.name);
	g_m.flags = flags & VFS_MS_PERMANENT;
	if (opts)
		h_snprintf(g_m.options, sizeof(g_m.options), "%s", opts);
	g_m.dev = ((uint64_t)8 << 8) | 1;
	g_m.dev_override = 1;
	g_m.type = &TEST_FSTYPE;
	/* As vfs_mount(): a write-protected device is refused rw. */
	if (!(flags & VFS_MS_RDONLY) && blk_bdev_read_only(&test_bdev))
		return ST_ROFS;
	if (blk_bdev_claim(&test_bdev, &g_m))
		return ST_BUSY;
	blk_bdev_get(&test_bdev);
	g_m.bdev = &test_bdev;
	test_mount_ptr = &g_m;
	r = TEST_FSTYPE.mount(&g_m, &test_bdev, g_m.flags, opts);
	if (r != ST_OK) {
		blk_bdev_unclaim(&test_bdev, &g_m);
		blk_bdev_put(&test_bdev);
		test_mount_ptr = NULL;
		/* nothing may be left behind by a failed mount */
		if (g_leakcheck && test_kalloc_live != g_base_kalloc)
			failf("failed mount left %ld heap blocks behind",
			      test_kalloc_live - g_base_kalloc);
		return r;
	}
	if (!g_m.ops) {
		failf("mount succeeded without setting m->ops");
		TEST_FSTYPE.unmount(&g_m);
		return ST_INVALID;
	}
	g_mounted = 1;
	return ST_OK;
}

/* mnt_flush() of kernel/fs/mount.c */
static void mnt_flush(void)
{
	if (g_m.sb) {
		pagecache_flush_sb(g_m.sb);
		icache_flush_sb(g_m.sb);
	}
	if (g_m.ops && g_m.ops->sync)
		g_m.ops->sync(&g_m);
	block_device_t *b = g_m.bdev;
	if (b && b->sync) {
		if (g_m.sb)
			vfs_sb_lock_io(g_m.sb);
		b->sync(b);
		if (g_m.sb)
			vfs_sb_unlock_io(g_m.sb);
	}
}

static int g_open_files;

static int do_umount(void)
{
	int r;

	if (!g_mounted)
		return ST_INVALID;
	if (g_open_files) {
		failf("unmount with %d handle(s) still open", g_open_files);
		return ST_BUSY;
	}
	mnt_flush();
	r = TEST_FSTYPE.unmount(&g_m);
	if (r != ST_OK)
		return r;
	blk_bdev_unclaim(&test_bdev, &g_m);
	blk_bdev_put(&test_bdev);
	g_mounted = 0;
	test_mount_ptr = NULL;
	if (g_leakcheck) {
		pc_stats_t ps;
		ic_stats_t is;
		pagecache_get_stats(&ps);
		icache_get_stats(&is);
		if (ps.total_pages)
			failf("unmount left %llu page-cache pages",
			      (unsigned long long)ps.total_pages);
		if (is.total_entries)
			failf("unmount left %llu icache inodes",
			      (unsigned long long)is.total_entries);
		/* What the first unmount leaves may be one-time global state
		 * (the page cache's writeback bounce buffer is allocated on
		 * first use and kept); from then on every cycle must give back
		 * all it took. */
		static int granted;
		if (test_kalloc_live != g_base_kalloc ||
		    test_pages_live != g_base_pages) {
			if (!granted) {
				h_fprintf_err("test-fs: note: %ld heap block(s) and %lld "
					      "page(s) kept after the first unmount "
					      "(one-time allocations); later cycles "
					      "must not add to them\n",
					      test_kalloc_live - g_base_kalloc,
					      (long long)(test_pages_live - g_base_pages));
				granted = 1;
			} else {
				if (test_kalloc_live != g_base_kalloc)
					failf("unmount left %ld heap blocks (%llu bytes live)",
					      test_kalloc_live - g_base_kalloc,
					      (unsigned long long)test_kalloc_bytes);
				if (test_pages_live != g_base_pages)
					failf("unmount left %lld pages",
					      (long long)(test_pages_live - g_base_pages));
			}
		}
	}
	return ST_OK;
}

/* mnt_remount() of kernel/fs/mount.c, minus the mount table */
static int do_remount(unsigned long nf, const char *data)
{
	nf &= VFS_MS_PERMANENT;
	int ro_change = ((nf ^ g_m.flags) & VFS_MS_RDONLY) != 0;
	if (ro_change && !(nf & VFS_MS_RDONLY) && blk_bdev_read_only(&test_bdev))
		return ST_ROFS;
	if (ro_change && (nf & VFS_MS_RDONLY) && g_open_files)
		return ST_BUSY; /* (the VFS counts writers only) */
	if (TEST_FSTYPE.remount) {
		if (ro_change && (nf & VFS_MS_RDONLY))
			mnt_flush();
		int s = TEST_FSTYPE.remount(&g_m, nf, data);
		if (s != ST_OK)
			return s;
	} else if (ro_change) {
		return ST_INVALID;
	}
	g_m.flags = nf;
	if (data && data[0])
		h_snprintf(g_m.options, sizeof(g_m.options), "%s", data);
	return ST_OK;
}

/* ================================================================== */
/* the walk: symbolic links followed by the "VFS"                      */
/* ================================================================== */

struct walk {
	int active;
	char path[VFS_MAX_PATH];
	int follow;
	int hopped;
	int err;
	char next[VFS_MAX_PATH];
};
static struct walk g_w;

/* vfs_hop_join: the link at op[cs, ce) leads to target; the rest of the
 * path follows it.  The task's root is "/" (the mount). */
static int hop_join(const char *op, unsigned long cs, unsigned long ce,
		    const char *target, char *out)
{
	/* The link's directory, the target and the rest of the path joined:
	 * up to three whole paths.  As the kernel's vfs_hop_join, only where
	 * they lead (canon) has to fit in a path. */
	char tmp[VFS_MAX_PATH * 3];
	unsigned long n = 0;

	if (target[0] != '/') {
		/* the link's directory */
		for (unsigned long i = 0; i < cs && n + 1 < sizeof(tmp); i++)
			tmp[n++] = op[i];
		if (n == 0 || tmp[n - 1] != '/')
			tmp[n++] = '/';
	}
	for (unsigned long i = 0; target[i] && n + 1 < sizeof(tmp); i++)
		tmp[n++] = target[i];
	for (unsigned long i = ce; op[i] && n + 1 < sizeof(tmp); i++)
		tmp[n++] = op[i];
	tmp[n] = 0;
	if (n + 1 >= sizeof(tmp))
		return ST_NAMETOOLONG;
	if (tmp[0] != '/') {
		h_memmove(tmp + 1, tmp, n + 1);
		tmp[0] = '/';
	}
	return canon(tmp, out);
}

int vfs_link_hop(vfs_mount_t *m, const char *path, unsigned long comp_start,
		 unsigned long comp_end, const char *target)
{
	const char *op = g_w.path;

	if (!g_linkhop || !g_w.active || !m || !path || path[0] != '/' ||
	    !target || comp_start >= comp_end)
		return 0;
	if (kstrncmp(op, path, comp_end) != 0)
		return 0;
	if (op[comp_end] != '\0' && op[comp_end] != '/')
		return 0;
	if (op[comp_end] == '\0' && !g_w.follow)
		return 0;
	if (g_w.hopped)
		return 1;
	g_w.hopped = 1;
	if (!target[0])
		g_w.err = ST_NOT_FOUND;
	else if (m->flags & VFS_MS_NOSYMFOLLOW)
		g_w.err = ST_LOOP;
	else
		g_w.err = hop_join(op, comp_start, comp_end, target, g_w.next);
	return 1;
}

/* Run `op' on path p (canonical) with the walk active; restart on the
 * path a link leads to, at most 40 times. */
#define WALK(pathvar, follow_, r, expr)                                   \
	do {                                                              \
		int __hops = 0;                                           \
		for (;;) {                                                \
			h_snprintf(g_w.path, sizeof(g_w.path), "%s",      \
				   pathvar);                              \
			g_w.active = 1;                                   \
			g_w.follow = (follow_);                           \
			g_w.hopped = 0;                                   \
			g_w.err = 0;                                      \
			(r) = (expr);                                     \
			g_w.active = 0;                                   \
			if (!g_w.hopped)                                  \
				break;                                    \
			if ((r) != ST_LINK) {                             \
				failf("driver called vfs_link_hop (took "  \
				      "over) but returned %s, not LINK",  \
				      st_name(r));                        \
				break;                                    \
			}                                                 \
			if (g_w.err) {                                    \
				(r) = g_w.err;                            \
				break;                                    \
			}                                                 \
			if (++__hops > 40) {                              \
				(r) = ST_LOOP;                            \
				break;                                    \
			}                                                 \
			h_snprintf(pathvar, VFS_MAX_PATH, "%s", g_w.next); \
		}                                                         \
	} while (0)

/* ================================================================== */
/* operations, as the vfs_* wrappers run them                          */
/* ================================================================== */

/* Not mounted: ST_NO_DEVICE (an expectation mismatch reports it). */
#define NEED_MOUNT()                                     \
	do {                                             \
		if (!g_mounted)                          \
			return ST_NO_DEVICE;             \
	} while (0)

#define RO_GATE()                                        \
	do {                                             \
		if (ro_mount() && !g_raw_ro)             \
			return ST_ROFS;                  \
	} while (0)

static int op_stat(const char *path, struct kstat *st, int follow)
{
	char p[VFS_MAX_PATH];
	int r;

	NEED_MOUNT();
	if (canon(path, p))
		return ST_INVALID;
	h_memset(st, 0, sizeof(*st));
	if (follow || !g_m.ops->lstat)
		WALK(p, follow, r, g_m.ops->stat(&g_m, p, st));
	else
		WALK(p, 0, r, g_m.ops->lstat(&g_m, p, st));
	return r;
}

static int op_open(const char *path, int flags, unsigned mode, vfs_file_t **out)
{
	char p[VFS_MAX_PATH];
	int r;
	int follow = !(flags & O_NOFOLLOW) && !((flags & O_CREAT) && (flags & O_EXCL));
	int acc = flags & 3;

	NEED_MOUNT();
	*out = NULL;
	if (canon(path, p))
		return ST_INVALID;
	if (ro_mount() && !g_raw_ro) {
		/* vfs_open_common(): no writing, creating or truncating */
		struct kstat st;
		int exists = op_stat(p, &st, follow) == ST_OK;
		if (acc == O_WRONLY || acc == O_RDWR || (flags & O_TRUNC) ||
		    ((flags & O_CREAT) && !exists))
			return ST_ROFS;
	}
	if (g_m.ops->open_mode)
		WALK(p, follow, r, g_m.ops->open_mode(&g_m, p, flags, mode, out));
	else
		WALK(p, follow, r, g_m.ops->open(&g_m, p, flags, out));
	if (r == ST_OK && *out) {
		if ((*out)->ops == NULL)
			failf("open returned a handle without ops");
		(*out)->refcount = 1;
		(*out)->flags = flags;
		(*out)->is_root_dir = (p[0] == '/' && p[1] == 0);
		(*out)->dev_injected = 0;
		(*out)->mnt = &g_m;
		g_open_files++;
	} else if (r == ST_OK) {
		failf("open returned OK without a handle");
		r = ST_INVALID;
	}
	return r;
}

static int op_close(vfs_file_t *f)
{
	if (!f)
		return ST_INVALID;
	int old = __sync_fetch_and_sub(&f->refcount, 1);
	if (old > 1)
		return ST_OK;
	if (old <= 0) {
		failf("close of a released handle");
		return ST_INVALID;
	}
	g_open_files--;
	return f->ops->close(f);
}

static long op_pread(vfs_file_t *f, void *buf, long n, long off)
{
	if (f->ops->read_at)
		return f->ops->read_at(f, buf, n, off);
	if (!f->ops->read || !f->ops->seek)
		return ST_INVALID;
	long saved = f->ops->seek(f, 0, SEEK_CUR);
	if (f->ops->seek(f, off, SEEK_SET) < 0)
		return ST_INVALID;
	long r = f->ops->read(f, buf, n);
	f->ops->seek(f, saved, SEEK_SET);
	return r;
}

static long op_pwrite(vfs_file_t *f, const void *buf, long n, long off)
{
	if (f->ops->write_at)
		return f->ops->write_at(f, buf, n, off);
	if (!f->ops->write || !f->ops->seek)
		return ST_INVALID;
	long saved = f->ops->seek(f, 0, SEEK_CUR);
	if (f->ops->seek(f, off, SEEK_SET) < 0)
		return ST_INVALID;
	long r = f->ops->write(f, buf, n);
	f->ops->seek(f, saved, SEEK_SET);
	return r;
}

#define PATHOP(name, follow_, gate, call)                                  \
	static int name                                                    \
	{                                                                  \
		char p[VFS_MAX_PATH];                                      \
		int r;                                                     \
		NEED_MOUNT();                                              \
		if (canon(path, p))                                        \
			return ST_INVALID;                                 \
		if (gate)                                                  \
			RO_GATE();                                         \
		WALK(p, follow_, r, call);                                 \
		return r;                                                  \
	}

PATHOP(op_mkdir(const char *path, unsigned mode), 0, 1,
       g_m.ops->mkdir ? g_m.ops->mkdir(&g_m, p, mode) : ST_UNSUPPORTED)
PATHOP(op_rmdir(const char *path), 0, 1,
       g_m.ops->rmdir ? g_m.ops->rmdir(&g_m, p) : ST_UNSUPPORTED)
PATHOP(op_unlink(const char *path), 0, 1,
       g_m.ops->unlink ? g_m.ops->unlink(&g_m, p) : ST_UNSUPPORTED)
PATHOP(op_readlink(const char *path, char *buf, unsigned long sz), 0, 0,
       g_m.ops->readlink ? g_m.ops->readlink(&g_m, p, buf, sz) : ST_INVALID)
PATHOP(op_chmod(const char *path, unsigned mode), 1, 1,
       g_m.ops->chmod ? g_m.ops->chmod(&g_m, p, mode) : ST_OK)
PATHOP(op_chown(const char *path, int uid, int gid, int nofollow), !nofollow,
       1, g_m.ops->chown ? g_m.ops->chown(&g_m, p, uid, gid, nofollow) : ST_OK)
PATHOP(op_utimens(const char *path, int nofollow, const struct vfs_utimes *t),
       !nofollow, 1,
       g_m.ops->utimens ? g_m.ops->utimens(&g_m, p, nofollow, t) : ST_OK)
PATHOP(op_getxattr(const char *path, int nofollow, const char *name, void *val,
		   unsigned long size),
       !nofollow, 0,
       g_m.ops->getxattr ? g_m.ops->getxattr(&g_m, p, nofollow, name, val, size) :
			   ST_UNSUPPORTED)
PATHOP(op_setxattr(const char *path, int nofollow, const char *name,
		   const void *val, unsigned long size, int flags),
       !nofollow, 1,
       g_m.ops->setxattr ? g_m.ops->setxattr(&g_m, p, nofollow, name, val, size,
					     flags) :
			   ST_UNSUPPORTED)
PATHOP(op_listxattr(const char *path, int nofollow, char *list,
		    unsigned long size),
       !nofollow, 0,
       g_m.ops->listxattr ? g_m.ops->listxattr(&g_m, p, nofollow, list, size) :
			    ST_UNSUPPORTED)
PATHOP(op_removexattr(const char *path, int nofollow, const char *name),
       !nofollow, 1,
       g_m.ops->removexattr ? g_m.ops->removexattr(&g_m, p, nofollow, name) :
			      ST_UNSUPPORTED)
PATHOP(op_mknod(const char *path, unsigned mode, uint64_t dev), 0, 1,
       g_m.ops->mknod ? g_m.ops->mknod(&g_m, p, mode, dev) : ST_UNSUPPORTED)

static int op_symlink(const char *target, const char *path)
{
	char p[VFS_MAX_PATH];
	int r;

	NEED_MOUNT();
	if (canon(path, p))
		return ST_INVALID;
	RO_GATE();
	if (!g_m.ops->symlink)
		return ST_UNSUPPORTED;
	WALK(p, 0, r, g_m.ops->symlink(&g_m, target, p));
	return r;
}

static int op_rename(const char *from, const char *to)
{
	char a[VFS_MAX_PATH], b[VFS_MAX_PATH];
	int r;

	NEED_MOUNT();
	if (canon(from, a) || canon(to, b))
		return ST_INVALID;
	RO_GATE();
	if (!g_m.ops->rename)
		return ST_UNSUPPORTED;
	WALK(a, 0, r, g_m.ops->rename(&g_m, a, b));
	return r;
}

static int op_link(const char *from, const char *to, int follow)
{
	char a[VFS_MAX_PATH], b[VFS_MAX_PATH];
	int r;

	NEED_MOUNT();
	if (canon(from, a) || canon(to, b))
		return ST_INVALID;
	RO_GATE();
	if (!g_m.ops->link)
		return ST_UNSUPPORTED;
	WALK(b, 0, r, g_m.ops->link(&g_m, a, b, follow));
	return r;
}

static int op_statfs(struct vfs_statfs *sf)
{
	NEED_MOUNT();
	h_memset(sf, 0, sizeof(*sf));
	if (!g_m.ops->statfs)
		return ST_UNSUPPORTED;
	int r = g_m.ops->statfs(&g_m, sf);
	sf->f_flags = ro_mount() ? 1 : 0;
	return r;
}

static int op_truncate_path(const char *path, unsigned long size)
{
	vfs_file_t *f;
	int r = op_open(path, O_WRONLY, 0, &f);

	if (r != ST_OK)
		return r;
	r = f->ops->truncate ? f->ops->truncate(f, size) : ST_UNSUPPORTED;
	int c = op_close(f);
	return r != ST_OK ? r : c;
}

static int op_fsync_path(const char *path)
{
	vfs_file_t *f;
	int r = op_open(path, O_RDONLY, 0, &f);

	if (r != ST_OK)
		return r;
	r = f->ops->fsync ? f->ops->fsync(f) : ST_OK;
	int c = op_close(f);
	return r != ST_OK ? r : c;
}

/* ---- data sources for writes ----
 *   text:STRING      the bytes of STRING
 *   pat:SEED:LEN     LEN pattern bytes (position-dependent, from SEED)
 *   zero:LEN         LEN zero bytes
 *   fill:BYTE:LEN    LEN copies of BYTE
 *   file:HOSTPATH    a host file's content */
struct src {
	int kind; /* 0 text/file buffer, 1 pattern, 2 fill */
	const uint8_t *buf;
	void *owned;
	uint64_t len, seed;
	uint8_t fill;
};

static int src_parse(const char *spec, struct src *s)
{
	h_memset(s, 0, sizeof(*s));
	if (!h_strncmp(spec, "text:", 5)) {
		s->buf = (const uint8_t *)spec + 5;
		s->len = h_strlen(spec + 5);
		return 0;
	}
	if (!h_strncmp(spec, "pat:", 4)) {
		char *e;
		s->kind = 1;
		s->seed = h_strtoull(spec + 4, &e, 0);
		if (*e != ':')
			return -1;
		s->len = num(e + 1);
		return 0;
	}
	if (!h_strncmp(spec, "zero:", 5)) {
		s->kind = 2;
		s->len = num(spec + 5);
		return 0;
	}
	if (!h_strncmp(spec, "fill:", 5)) {
		char *e;
		s->kind = 2;
		s->fill = (uint8_t)h_strtoull(spec + 5, &e, 0);
		if (*e != ':')
			return -1;
		s->len = num(e + 1);
		return 0;
	}
	if (!h_strncmp(spec, "file:", 5)) {
		unsigned long l;
		s->owned = host_read_file(spec + 5, &l);
		if (!s->owned)
			return -1;
		s->buf = s->owned;
		s->len = l;
		return 0;
	}
	return -1;
}

static void src_get(const struct src *s, uint64_t off, uint8_t *out,
		    unsigned long n)
{
	if (s->kind == 0)
		h_memcpy(out, s->buf + off, n);
	else if (s->kind == 1)
		for (unsigned long i = 0; i < n; i++)
			out[i] = pat_byte(s->seed, off + i);
	else
		h_memset(out, s->fill, n);
}

/* Write `s' at `off' in chunks of g_chunk.  Returns ST_OK or an error;
 * a short write is an error (NOSPC from the driver passes through). */
static long write_src(vfs_file_t *f, const struct src *s, long off, int append)
{
	long chunk = g_chunk > 0 ? g_chunk : 65536;
	uint8_t *buf = h_malloc((unsigned long)chunk);
	uint64_t done = 0;
	long r = ST_OK;

	while (done < s->len) {
		long n = (long)(s->len - done < (uint64_t)chunk ? s->len - done : (uint64_t)chunk);
		src_get(s, done, buf, (unsigned long)n);
		long w = append ? f->ops->write(f, buf, n) :
				  op_pwrite(f, buf, n, off + (long)done);
		if (w < 0) {
			r = w;
			break;
		}
		if (w == 0) {
			r = ST_NOSPC;
			break;
		}
		done += (uint64_t)w;
	}
	h_free(buf);
	return r;
}

/* Read a whole file through pread in g_chunk pieces, hashing it. */
static long read_all(vfs_file_t *f, uint64_t size, struct sha256 *sh,
		     uint8_t **keep)
{
	long chunk = g_chunk > 0 ? g_chunk : 65536;
	uint8_t *buf = h_malloc((unsigned long)chunk);
	uint64_t off = 0;

	if (keep)
		*keep = h_malloc(size ? size : 1);
	for (;;) {
		long r = op_pread(f, buf, chunk, (long)off);
		if (r < 0) {
			h_free(buf);
			return r;
		}
		if (r == 0)
			break;
		if (sh)
			sha_update(sh, buf, (unsigned long)r);
		if (keep && *keep) {
			if (off + (uint64_t)r > size) {
				*keep = h_realloc(*keep, off + (uint64_t)r);
			}
			h_memcpy(*keep + off, buf, (unsigned long)r);
		}
		off += (uint64_t)r;
		if (off > size + (64ULL << 20)) {
			h_free(buf);
			return ST_RANGE; /* reads far past st_size */
		}
	}
	h_free(buf);
	return (long)off;
}

/* ================================================================== */
/* directory listing                                                   */
/* ================================================================== */

struct dent {
	char *name;
	uint64_t ino;
	uint8_t type;
};

static int dent_cmp(const void *a, const void *b)
{
	return h_strcmp(((const struct dent *)a)->name,
			((const struct dent *)b)->name);
}

static void dents_free(struct dent *d, long n)
{
	for (long i = 0; i < n; i++)
		h_free(d[i].name);
	h_free(d);
}

/* All entries of a directory but "." and "..", sorted by name. */
static int list_dir(const char *path, struct dent **out, long *nout)
{
	vfs_file_t *f;
	int r = op_open(path, O_RDONLY | O_DIRECTORY, 0, &f);
	long n = 0, cap = 0;
	struct dent *d = NULL;
	uint8_t *buf;
	long bsz = g_chunk >= 512 && g_chunk < 65536 ? g_chunk : 4096;

	*out = NULL;
	*nout = 0;
	if (r != ST_OK)
		return r;
	if (!f->ops->readdir) {
		op_close(f);
		return ST_UNSUPPORTED;
	}
	buf = h_malloc((unsigned long)bsz);
	for (int rounds = 0;; rounds++) {
		long got = f->ops->readdir(f, buf, bsz);
		if (got < 0) {
			r = (int)got;
			break;
		}
		if (got == 0)
			break;
		if (got > bsz) {
			failf("readdir returned %ld bytes for a %ld-byte buffer",
			      got, bsz);
			r = ST_INVALID;
			break;
		}
		if (rounds > 10000000) {
			failf("readdir never ends");
			r = ST_LOOP;
			break;
		}
		long off = 0;
		while (off < got) {
			struct dirent64 *e = (struct dirent64 *)(buf + off);
			if (off + 19 > got || e->d_reclen < 20 ||
			    off + e->d_reclen > got) {
				failf("malformed dirent64 at %ld (reclen %u)", off,
				      off + 19 <= got ? e->d_reclen : 0);
				r = ST_INVALID;
				goto out;
			}
			const char *nm = (const char *)buf + off + 19;
			unsigned long ml = (unsigned long)e->d_reclen - 19;
			unsigned long l = 0;
			while (l < ml && nm[l])
				l++;
			if (l == ml) {
				failf("dirent64 name not terminated");
				r = ST_INVALID;
				goto out;
			}
			off += e->d_reclen;
			if ((l == 1 && nm[0] == '.') ||
			    (l == 2 && nm[0] == '.' && nm[1] == '.'))
				continue;
			if (l == 0) {
				failf("empty name in %s", path);
				continue;
			}
			if (n == cap) {
				cap = cap ? cap * 2 : 64;
				d = h_realloc(d, (unsigned long)cap * sizeof(*d));
			}
			d[n].name = h_malloc(l + 1);
			h_memcpy(d[n].name, nm, l);
			d[n].name[l] = 0;
			d[n].ino = e->d_ino;
			d[n].type = e->d_type;
			n++;
		}
	}
out:
	h_free(buf);
	op_close(f);
	if (r != ST_OK) {
		dents_free(d, n);
		return r;
	}
	if (n > 1)
		h_qsort(d, (unsigned long)n, sizeof(*d), dent_cmp);
	for (long i = 1; i < n; i++)
		if (!h_strcmp(d[i - 1].name, d[i].name))
			failf("duplicate name \"%s\" in %s", d[i].name, path);
	*out = d;
	*nout = n;
	return ST_OK;
}

static char type_char(uint32_t mode)
{
	if (S_ISDIR(mode)) return 'd';
	if (S_ISREG(mode)) return 'f';
	if (S_ISLNK(mode)) return 'l';
	if (S_ISCHR(mode)) return 'c';
	if (S_ISBLK(mode)) return 'b';
	if (S_ISFIFO(mode)) return 'p';
	if (S_ISSOCK(mode)) return 's';
	return '?';
}

static uint8_t dt_of(uint32_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR: return DT_DIR;
	case S_IFREG: return DT_REG;
	case S_IFLNK: return DT_LNK;
	case S_IFCHR: return DT_CHR;
	case S_IFBLK: return DT_BLK;
	case S_IFIFO: return DT_FIFO;
	case S_IFSOCK: return DT_SOCK;
	}
	return DT_UNKNOWN;
}

/* Name with control bytes, spaces, backslash and non-ASCII escaped (\xHH),
 * so every line is one record. */
static void put_name(const char *s)
{
	char out[VFS_MAX_PATH * 4 + 1];
	unsigned long n = 0;

	for (; *s && n + 5 < sizeof(out); s++) {
		unsigned char c = (unsigned char)*s;
		if (c <= ' ' || c == '\\' || c >= 0x7f) {
			h_snprintf(out + n, 5, "\\x%02x", c);
			n += 4;
		} else {
			out[n++] = (char)c;
		}
	}
	out[n] = 0;
	OUT("%s", out);
}

/* manifest/ls field selection */
#define MF_MODE 0x01
#define MF_OWNER 0x02
#define MF_TIME 0x04
#define MF_INO 0x08
#define MF_NLINK 0x10
#define MF_XATTR 0x20
#define MF_NOSHA 0x40
#define MF_BLOCKS 0x80
#define MF_ATIME 0x100

static unsigned parse_mf(int argc, char **argv, int from)
{
	unsigned f = 0;
	for (int i = from; i < argc; i++) {
		const char *a = argv[i];
		if (streq(a, "+mode")) f |= MF_MODE;
		else if (streq(a, "+owner")) f |= MF_OWNER;
		else if (streq(a, "+time")) f |= MF_TIME;
		else if (streq(a, "+atime")) f |= MF_ATIME;
		else if (streq(a, "+ino")) f |= MF_INO;
		else if (streq(a, "+nlink")) f |= MF_NLINK;
		else if (streq(a, "+xattr")) f |= MF_XATTR;
		else if (streq(a, "+blocks")) f |= MF_BLOCKS;
		else if (streq(a, "-sha")) f |= MF_NOSHA;
		else if (streq(a, "+all")) f |= MF_MODE | MF_OWNER | MF_TIME | MF_NLINK | MF_XATTR;
	}
	return f;
}

static void print_xattrs(const char *path)
{
	char list[8192];
	int n = op_listxattr(path, 1, list, sizeof(list));
	if (n == ST_UNSUPPORTED)
		return;
	if (n < 0) {
		OUT(" xattr=%s", st_name(n));
		return;
	}
	/* names sorted for determinism */
	const char *names[256];
	int k = 0;
	for (int i = 0; i < n && k < 256;) {
		names[k++] = list + i;
		i += (int)h_strlen(list + i) + 1;
	}
	for (int a = 0; a < k; a++)
		for (int b = a + 1; b < k; b++)
			if (h_strcmp(names[a], names[b]) > 0) {
				const char *t = names[a];
				names[a] = names[b];
				names[b] = t;
			}
	for (int a = 0; a < k; a++) {
		uint8_t val[4096];
		int vl = op_getxattr(path, 1, names[a], val, sizeof(val));
		OUT(" x:");
		put_name(names[a]);
		if (vl < 0) {
			OUT("=%s", st_name(vl));
			continue;
		}
		struct sha256 sh;
		char hex[65];
		sha_init(&sh);
		sha_update(&sh, val, (unsigned long)vl);
		sha_final(&sh, hex);
		OUT("=%d:%.16s", vl, hex);
	}
}

/* One record: TYPE PATH [size sha256 | -> target] [fields]. */
static long g_walk_files, g_walk_bytes;

static void manifest_one(const char *path, const struct kstat *st, unsigned mf)
{
	char t = type_char(st->st_mode);

	OUT("%c ", t);
	put_name(path);
	if (t == 'f') {
		OUT(" %llu", (unsigned long long)st->st_size);
		if (!(mf & MF_NOSHA)) {
			vfs_file_t *f;
			int r = op_open(path, O_RDONLY, 0, &f);
			if (r != ST_OK) {
				OUT(" open=%s", st_name(r));
				failf("open %s: %s", path, st_name(r));
			} else {
				struct sha256 sh;
				char hex[65];
				sha_init(&sh);
				long n = read_all(f, st->st_size, &sh, NULL);
				op_close(f);
				if (n < 0) {
					OUT(" read=%s", st_name(n));
					failf("read %s: %s", path, st_name(n));
				} else {
					sha_final(&sh, hex);
					OUT(" %s", hex);
					if ((uint64_t)n != st->st_size) {
						OUT(" readlen=%ld", n);
						failf("%s: read %ld bytes, size %llu",
						      path, n,
						      (unsigned long long)st->st_size);
					}
					g_walk_files++;
					g_walk_bytes += n;
				}
			}
		}
	} else if (t == 'l') {
		char tgt[VFS_MAX_PATH * 4];
		int n = op_readlink(path, tgt, sizeof(tgt) - 1);
		if (n < 0) {
			OUT(" readlink=%s", st_name(n));
			failf("readlink %s: %s", path, st_name(n));
		} else {
			tgt[n] = 0;
			OUT(" -> ");
			put_name(tgt);
			if ((uint64_t)n != st->st_size && st->st_size)
				OUT(" lsize=%llu", (unsigned long long)st->st_size);
		}
	} else if (t == 'c' || t == 'b') {
		OUT(" rdev=%llu", (unsigned long long)st->st_rdev);
	}
	if (mf & MF_MODE)
		OUT(" mode=%04o", st->st_mode & 07777);
	if (mf & MF_OWNER)
		OUT(" uid=%u gid=%u", st->st_uid, st->st_gid);
	if (mf & MF_NLINK)
		OUT(" nlink=%u", st->st_nlink);
	if (mf & MF_INO)
		OUT(" ino=%llu", (unsigned long long)st->st_ino);
	if (mf & MF_BLOCKS)
		OUT(" blocks=%llu", (unsigned long long)st->st_blocks);
	if (mf & MF_TIME)
		OUT(" mtime=%llu.%09llu ctime=%llu.%09llu",
		    (unsigned long long)st->st_mtime,
		    (unsigned long long)st->st_mtime_nsec,
		    (unsigned long long)st->st_ctime,
		    (unsigned long long)st->st_ctime_nsec);
	if (mf & MF_ATIME)
		OUT(" atime=%llu.%09llu", (unsigned long long)st->st_atime,
		    (unsigned long long)st->st_atime_nsec);
	if (mf & MF_XATTR)
		print_xattrs(path);
	OUT("\n");
}

/* Depth-first walk with an explicit stack (deep trees must not recurse
 * here either). */
struct wstack {
	char *path;
};

static int walk_tree(const char *root, unsigned mf, int print, int recurse)
{
	struct wstack *stk = NULL;
	long sp = 0, cap = 0;
	int bad = 0;
	char rp[VFS_MAX_PATH];

	if (canon(root, rp))
		return ST_INVALID;
	{
		struct kstat st;
		int r = op_stat(rp, &st, 0);
		if (r != ST_OK)
			return r;
		if (print)
			manifest_one(rp, &st, mf);
		if (!S_ISDIR(st.st_mode))
			return ST_OK;
	}
	cap = 64;
	stk = h_malloc((unsigned long)cap * sizeof(*stk));
	stk[sp++].path = h_strdup(rp);
	while (sp) {
		char *dir = stk[--sp].path;
		struct dent *d;
		long n;
		int r = list_dir(dir, &d, &n);
		if (r != ST_OK) {
			OUT("! ");
			put_name(dir);
			OUT(" readdir=%s\n", st_name(r));
			failf("readdir %s: %s", dir, st_name(r));
			bad++;
			h_free(dir);
			continue;
		}
		/* children in order; subdirectories pushed in reverse so they
		 * pop in name order after this directory's entries */
		long firstdir = sp;
		for (long i = 0; i < n; i++) {
			char cp[VFS_MAX_PATH];
			int l = h_snprintf(cp, sizeof(cp), "%s%s%s", dir,
					   dir[1] ? "/" : "", d[i].name);
			if (l >= (int)sizeof(cp)) {
				OUT("! ");
				put_name(dir);
				OUT(" name too long for the harness\n");
				continue;
			}
			struct kstat st;
			r = op_stat(cp, &st, 0);
			if (r != ST_OK) {
				OUT("! ");
				put_name(cp);
				OUT(" lstat=%s\n", st_name(r));
				failf("lstat %s (listed by readdir): %s", cp,
				      st_name(r));
				bad++;
				continue;
			}
			if (d[i].type != DT_UNKNOWN && d[i].type != dt_of(st.st_mode))
				failf("%s: readdir type %u, stat type %c", cp,
				      d[i].type, type_char(st.st_mode));
			if (print)
				manifest_one(cp, &st, mf);
			else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
				g_silent = 1;
				manifest_one(cp, &st, mf);
				g_silent = 0;
			}
			if (S_ISDIR(st.st_mode) && recurse) {
				if (sp == cap) {
					cap *= 2;
					stk = h_realloc(stk, (unsigned long)cap *
								     sizeof(*stk));
				}
				stk[sp++].path = h_strdup(cp);
			}
		}
		/* reverse the pushed run */
		for (long a = firstdir, b = sp - 1; a < b; a++, b--) {
			struct wstack t = stk[a];
			stk[a] = stk[b];
			stk[b] = t;
		}
		dents_free(d, n);
		h_free(dir);
	}
	h_free(stk);
	return bad ? ST_IO : ST_OK;
}

/* ================================================================== */
/* stress: random operations mirrored into an in-memory model          */
/* ================================================================== */

struct mdata {
	uint8_t *buf;
	uint64_t size, cap;
	int nlink;
	/* xattrs */
	int nx;
	char *xn[16];
	uint8_t *xv[16];
	unsigned xl[16];
};

struct mnode {
	char *name;
	char type; /* 'f' 'd' 'l' */
	struct mdata *data; /* files (shared by hard links) */
	char *target; /* symlinks */
	struct mnode *parent, *child, *next;
};

static struct mnode *m_root;
static int f_symlink = 1, f_link = 1, f_xattr = 1, f_rename_replace = 1,
	   f_dir_rename = 1, f_holes = 1, f_truncate = 1, f_xdir_rename = 1;
static int g_names_ci; /* names compared case-insensitively */
static int g_names_dos; /* restricted charset */
static int g_names_utf8;
static uint64_t g_stress_budget = 8ULL << 20;
static uint64_t g_stress_bytes;
static uint64_t g_max_file = 1ULL << 20;
static unsigned long g_bsize = 4096;

static void m_path(const struct mnode *n, char *out)
{
	const struct mnode *chain[128];
	int k = 0;
	unsigned long l = 0;

	for (; n && n->parent && k < 128; n = n->parent)
		chain[k++] = n;
	out[l++] = '/';
	out[l] = 0;
	for (int i = k - 1; i >= 0; i--) {
		int w = h_snprintf(out + l, VFS_MAX_PATH - l, "%s%s",
				   l > 1 ? "/" : "", chain[i]->name);
		if (w < 0 || l + (unsigned long)w >= VFS_MAX_PATH) {
			failf("model path longer than %d bytes", VFS_MAX_PATH);
			out[VFS_MAX_PATH - 1] = 0;
			return;
		}
		l += (unsigned long)w;
	}
	out[l] = 0;
}

static int name_eq(const char *a, const char *b)
{
	if (!g_names_ci)
		return !h_strcmp(a, b);
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y)
			return 0;
	}
	return *a == *b;
}

static struct mnode *m_find(struct mnode *dir, const char *name)
{
	for (struct mnode *c = dir->child; c; c = c->next)
		if (name_eq(c->name, name))
			return c;
	return NULL;
}

static void m_attach(struct mnode *dir, struct mnode *n)
{
	n->parent = dir;
	n->next = dir->child;
	dir->child = n;
}

static void m_detach(struct mnode *n)
{
	struct mnode **pp = &n->parent->child;
	while (*pp != n)
		pp = &(*pp)->next;
	*pp = n->next;
	n->next = NULL;
	n->parent = NULL;
}

static void m_free_data(struct mdata *d)
{
	if (--d->nlink > 0)
		return;
	g_stress_bytes -= d->size;
	for (int i = 0; i < d->nx; i++) {
		h_free(d->xn[i]);
		h_free(d->xv[i]);
	}
	h_free(d->buf);
	h_free(d);
}

static void m_free(struct mnode *n)
{
	while (n->child) {
		struct mnode *c = n->child;
		m_detach(c);
		m_free(c);
	}
	if (n->data)
		m_free_data(n->data);
	h_free(n->target);
	h_free(n->name);
	h_free(n);
}

static void m_resize(struct mdata *d, uint64_t size)
{
	if (size > d->cap) {
		uint64_t nc = d->cap ? d->cap : 4096;
		while (nc < size)
			nc *= 2;
		d->buf = h_realloc(d->buf, nc);
		d->cap = nc;
	}
	if (size > d->size)
		h_memset(d->buf + d->size, 0, size - d->size);
	g_stress_bytes += size - d->size;
	d->size = size;
}

static void rand_name(char *out, struct mnode *dir)
{
	static const char posix[] = "abcdefghijklmnopqrstuvwxyz0123456789_-.";
	static const char dos[] = "abcdefghijklmnopqrstuvwxyz0123456789_-";
	static const char *utf8[] = { "\xc3\xa4", "\xc3\xb6", "\xc3\x9f",
				      "\xe6\x97\xa5", "\xd0\x96", "\xe2\x82\xac" };
	for (int tries = 0;; tries++) {
		unsigned long len;
		uint64_t r = rng_below(100);
		if (r < 70)
			len = 1 + rng_below(12);
		else if (r < 95)
			len = 13 + rng_below(40);
		else
			len = 100 + rng_below(g_names_dos ? 100 : 140);
		unsigned long n = 0;
		while (n < len && n + 4 < 250) {
			if (g_names_utf8 && rng_below(10) == 0) {
				const char *u = utf8[rng_below(6)];
				unsigned long ul = h_strlen(u);
				h_memcpy(out + n, u, ul);
				n += ul;
				continue;
			}
			out[n++] = g_names_dos ? dos[rng_below(sizeof(dos) - 1)] :
						 posix[rng_below(sizeof(posix) - 1)];
		}
		out[n] = 0;
		/* A leading dot is replaced (an ASCII byte, so the UTF-8
		 * stays whole); a trailing one just below. */
		if (out[0] == '.')
			out[0] = 'n';
		if (out[n - 1] == '.')
			out[n - 1] = 'z';
		if (!m_find(dir, out) || tries > 50)
			return;
	}
}

/* every node, for random choice */
static struct mnode **g_all;
static long g_nall, g_capall;

static void m_collect(void)
{
	struct mnode *stack[4096];
	int sp = 0;

	g_nall = 0;
	stack[sp++] = m_root;
	while (sp) {
		struct mnode *n = stack[--sp];
		if (g_nall == g_capall) {
			g_capall = g_capall ? g_capall * 2 : 256;
			g_all = h_realloc(g_all, (unsigned long)g_capall * sizeof(*g_all));
		}
		g_all[g_nall++] = n;
		for (struct mnode *c = n->child; c && sp < 4096; c = c->next)
			stack[sp++] = c;
	}
}

static struct mnode *m_pick(char type)
{
	long cand = 0;
	for (long i = 0; i < g_nall; i++)
		if (!type || g_all[i]->type == type)
			cand++;
	if (!cand)
		return NULL;
	long k = (long)rng_below((uint64_t)cand);
	for (long i = 0; i < g_nall; i++)
		if (!type || g_all[i]->type == type)
			if (k-- == 0)
				return g_all[i];
	return NULL;
}

static int m_depth(const struct mnode *n)
{
	int d = 0;
	for (; n->parent; n = n->parent)
		d++;
	return d;
}

/* Longest path below n (n's own name excluded), in bytes. */
static unsigned long m_sublen(const struct mnode *n)
{
	unsigned long best = 0;
	for (const struct mnode *c = n->child; c; c = c->next) {
		unsigned long l = 1 + h_strlen(c->name) + m_sublen(c);
		if (l > best)
			best = l;
	}
	return best;
}

static int m_under(const struct mnode *n, const struct mnode *anc)
{
	for (; n; n = n->parent)
		if (n == anc)
			return 1;
	return 0;
}

static uint64_t rand_size(void)
{
	uint64_t r = rng_below(100), bs = g_bsize;
	uint64_t s;
	if (r < 10) s = 0;
	else if (r < 30) s = 1 + rng_below(512);
	else if (r < 45) s = bs - 1 + rng_below(3);
	else if (r < 60) s = bs * (1 + rng_below(8)) + rng_below(3) - 1;
	else if (r < 85) s = rng_below(64 * 1024);
	else if (r < 97) s = rng_below(256 * 1024);
	else s = rng_below(g_max_file);
	if (s > g_max_file)
		s = g_max_file;
	return s;
}

static int s_write(struct mnode *n, uint64_t off, uint64_t len)
{
	char p[VFS_MAX_PATH];
	vfs_file_t *f;
	uint8_t *buf;
	uint64_t seed = rng();
	int r;

	m_path(n, p);
	r = op_open(p, O_WRONLY, 0, &f);
	if (r != ST_OK)
		return r;
	buf = h_malloc(len ? len : 1);
	for (uint64_t i = 0; i < len; i++)
		buf[i] = pat_byte(seed, i);
	long done = 0;
	while ((uint64_t)done < len) {
		long chunk = (long)(rng_below(3) == 0 ? 1 + rng_below(len - done) : len - done);
		long w = op_pwrite(f, buf + done, chunk, (long)(off + done));
		if (w <= 0) {
			r = w < 0 ? (int)w : ST_NOSPC;
			break;
		}
		done += w;
	}
	if (done > 0) {
		if (off + (uint64_t)done > n->data->size)
			m_resize(n->data, off + (uint64_t)done);
		h_memcpy(n->data->buf + off, buf, (unsigned long)done);
	}
	h_free(buf);
	int c = op_close(f);
	if (r == ST_OK)
		r = c;
	return r;
}

static int s_verify_file(struct mnode *n, const char *p)
{
	vfs_file_t *f;
	uint8_t *got = NULL;
	int r = op_open(p, O_RDONLY, 0, &f);

	if (r != ST_OK) {
		failf("stress verify: open %s: %s", p, st_name(r));
		return r;
	}
	long len = read_all(f, n->data->size, NULL, &got);
	op_close(f);
	if (len < 0) {
		failf("stress verify: read %s: %s", p, st_name(len));
		h_free(got);
		return (int)len;
	}
	if ((uint64_t)len != n->data->size) {
		failf("stress verify: %s is %ld bytes, model %llu", p, len,
		      (unsigned long long)n->data->size);
	} else if (len && h_memcmp(got, n->data->buf, (unsigned long)len)) {
		long i = 0;
		while (got[i] == n->data->buf[i])
			i++;
		failf("stress verify: %s differs at byte %ld (0x%02x, model 0x%02x)",
		      p, i, got[i], n->data->buf[i]);
	}
	h_free(got);
	return ST_OK;
}

static void s_verify(void)
{
	struct mnode *stack[4096];
	int sp = 0;
	char p[VFS_MAX_PATH];

	stack[sp++] = m_root;
	while (sp) {
		struct mnode *d = stack[--sp];
		struct dent *de;
		long nd;
		m_path(d, p);
		int r = list_dir(p, &de, &nd);
		if (r != ST_OK) {
			failf("stress verify: readdir %s: %s", p, st_name(r));
			continue;
		}
		long nm = 0;
		for (struct mnode *c = d->child; c; c = c->next)
			nm++;
		if (nm != nd)
			failf("stress verify: %s has %ld entries, model %ld", p,
			      nd, nm);
		for (long i = 0; i < nd; i++)
			if (!m_find(d, de[i].name))
				failf("stress verify: %s/%s exists, not in model",
				      p, de[i].name);
		dents_free(de, nd);
		for (struct mnode *c = d->child; c; c = c->next) {
			char cp[VFS_MAX_PATH];
			struct kstat st;
			m_path(c, cp);
			r = op_stat(cp, &st, 0);
			if (r != ST_OK) {
				failf("stress verify: lstat %s: %s", cp, st_name(r));
				continue;
			}
			if (type_char(st.st_mode) != c->type) {
				failf("stress verify: %s is '%c', model '%c'", cp,
				      type_char(st.st_mode), c->type);
				continue;
			}
			if (c->type == 'f') {
				if (st.st_size != c->data->size)
					failf("stress verify: %s st_size %llu, model %llu",
					      cp, (unsigned long long)st.st_size,
					      (unsigned long long)c->data->size);
				s_verify_file(c, cp);
				if (f_link && st.st_nlink != (uint32_t)c->data->nlink)
					failf("stress verify: %s nlink %u, model %d",
					      cp, st.st_nlink, c->data->nlink);
				for (int x = 0; f_xattr && x < c->data->nx; x++) {
					uint8_t v[512];
					int l = op_getxattr(cp, 1, c->data->xn[x], v,
							    sizeof(v));
					if (l != (int)c->data->xl[x] ||
					    h_memcmp(v, c->data->xv[x], c->data->xl[x]))
						failf("stress verify: %s xattr %s "
						      "(%d bytes, model %u)",
						      cp, c->data->xn[x], l,
						      c->data->xl[x]);
				}
			} else if (c->type == 'l') {
				char t[VFS_MAX_PATH + 1];
				int l = op_readlink(cp, t, VFS_MAX_PATH);
				if (l < 0 || (unsigned long)l != h_strlen(c->target) ||
				    h_memcmp(t, c->target, (unsigned long)l))
					failf("stress verify: readlink %s", cp);
			} else if (sp < 4096) {
				stack[sp++] = c;
			}
		}
	}
}

/* A result the model cannot follow: stop. */
static int s_bad(const char *what, const char *p, int r)
{
	failf("stress: %s %s: %s", what, p, st_name(r));
	return -1;
}

static int stress_one(void)
{
	char p[VFS_MAX_PATH], q[VFS_MAX_PATH], name[256];
	uint64_t r = rng_below(1000);
	int st;

	m_collect();
	if (r < 160) { /* create a file with content */
		struct mnode *d = m_pick('d');
		if (!d || m_depth(d) > 20)
			return 0;
		rand_name(name, d);
		if (m_find(d, name))
			return 0;
		m_path(d, p);
		h_snprintf(q, sizeof(q), "%s%s%s", p, p[1] ? "/" : "", name);
		if (h_strlen(q) > 200)
			return 0;
		uint64_t size = rand_size();
		if (g_stress_bytes + size > g_stress_budget)
			size = 0;
		vfs_file_t *f;
		st = op_open(q, O_WRONLY | O_CREAT | O_EXCL, 0644, &f);
		if (st != ST_OK)
			return s_bad("create", q, st);
		op_close(f);
		struct mnode *n = h_calloc(1, sizeof(*n));
		n->name = h_strdup(name);
		n->type = 'f';
		n->data = h_calloc(1, sizeof(*n->data));
		n->data->nlink = 1;
		m_attach(d, n);
		if (size && (st = s_write(n, 0, size)) != ST_OK)
			return s_bad("write", q, st);
		return 0;
	}
	if (r < 230) { /* mkdir */
		struct mnode *d = m_pick('d');
		if (!d || m_depth(d) > 12)
			return 0;
		rand_name(name, d);
		if (m_find(d, name))
			return 0;
		m_path(d, p);
		h_snprintf(q, sizeof(q), "%s%s%s", p, p[1] ? "/" : "", name);
		if (h_strlen(q) > 200)
			return 0;
		st = op_mkdir(q, 0755);
		if (st != ST_OK)
			return s_bad("mkdir", q, st);
		struct mnode *n = h_calloc(1, sizeof(*n));
		n->name = h_strdup(name);
		n->type = 'd';
		m_attach(d, n);
		return 0;
	}
	if (r < 400) { /* overwrite / extend a range */
		struct mnode *n = m_pick('f');
		if (!n)
			return 0;
		uint64_t size = n->data->size;
		uint64_t off = rng_below(size + 1);
		if (f_holes && rng_below(5) == 0)
			off = size + rng_below(4 * g_bsize + 1);
		uint64_t len = 1 + rng_below(rng_below(3) ? 2 * g_bsize : 128 * 1024);
		if (off + len > g_max_file || g_stress_bytes + off + len - size > g_stress_budget)
			return 0;
		m_path(n, p);
		if ((st = s_write(n, off, len)) != ST_OK)
			return s_bad("pwrite", p, st);
		return 0;
	}
	if (r < 470) { /* append through O_APPEND */
		struct mnode *n = m_pick('f');
		if (!n)
			return 0;
		uint64_t len = 1 + rng_below(3 * g_bsize);
		if (n->data->size + len > g_max_file || g_stress_bytes + len > g_stress_budget)
			return 0;
		m_path(n, p);
		vfs_file_t *f;
		st = op_open(p, O_WRONLY | O_APPEND, 0, &f);
		if (st != ST_OK)
			return s_bad("open append", p, st);
		uint64_t seed = rng();
		uint8_t *buf = h_malloc(len);
		for (uint64_t i = 0; i < len; i++)
			buf[i] = pat_byte(seed, i);
		long w = f->ops->write(f, buf, (long)len);
		op_close(f);
		if (w != (long)len) {
			h_free(buf);
			return s_bad("append", p, w < 0 ? (int)w : ST_NOSPC);
		}
		uint64_t old = n->data->size;
		m_resize(n->data, old + len);
		h_memcpy(n->data->buf + old, buf, len);
		h_free(buf);
		return 0;
	}
	if (r < 540 && f_truncate) { /* truncate */
		struct mnode *n = m_pick('f');
		if (!n)
			return 0;
		uint64_t ns = rng_below(2) ? rng_below(n->data->size + 1) :
					     n->data->size + rng_below(3 * g_bsize);
		if (ns > g_max_file || g_stress_bytes + (ns > n->data->size ? ns - n->data->size : 0) > g_stress_budget)
			return 0;
		m_path(n, p);
		st = op_truncate_path(p, ns);
		if (st == ST_UNSUPPORTED) {
			f_truncate = 0;
			OUT("stress: truncate unsupported, disabled\n");
			return 0;
		}
		if (st != ST_OK)
			return s_bad("truncate", p, st);
		if (ns < n->data->size) {
			g_stress_bytes -= n->data->size - ns;
			n->data->size = ns;
		} else {
			m_resize(n->data, ns);
		}
		return 0;
	}
	if (r < 640) { /* unlink */
		struct mnode *n = m_pick(rng_below(4) ? 'f' : 'l');
		if (!n)
			return 0;
		m_path(n, p);
		st = op_unlink(p);
		if (st != ST_OK)
			return s_bad("unlink", p, st);
		m_detach(n);
		m_free(n);
		return 0;
	}
	if (r < 680) { /* rmdir of an empty directory (or expect NOTEMPTY) */
		struct mnode *n = m_pick('d');
		if (!n || n == m_root)
			return 0;
		m_path(n, p);
		st = op_rmdir(p);
		if (n->child) {
			if (st != ST_NOTEMPTY)
				return s_bad("rmdir (non-empty, expected NOTEMPTY)", p, st);
			return 0;
		}
		if (st != ST_OK)
			return s_bad("rmdir", p, st);
		m_detach(n);
		m_free(n);
		return 0;
	}
	if (r < 800) { /* rename */
		struct mnode *n = m_pick(0);
		if (!n || n == m_root)
			return 0;
		if (n->type == 'd' && !f_dir_rename)
			return 0;
		struct mnode *d = f_xdir_rename ? m_pick('d') : n->parent;
		if (!d || m_under(d, n) || m_depth(d) > 15)
			return 0;
		struct mnode *victim = NULL;
		if (f_rename_replace && n->type == 'f' && rng_below(4) == 0) {
			for (struct mnode *c = d->child; c; c = c->next)
				if (c->type == 'f' && c != n) {
					victim = c;
					break;
				}
		}
		if (victim)
			h_snprintf(name, sizeof(name), "%s", victim->name);
		else
			rand_name(name, d);
		if (!victim && m_find(d, name))
			return 0;
		m_path(n, p);
		m_path(d, q);
		unsigned long ql = h_strlen(q);
		h_snprintf(q + ql, sizeof(q) - ql, "%s%s", ql > 1 ? "/" : "", name);
		if (h_strlen(q) > 200 || h_strlen(q) + m_sublen(n) > 240)
			return 0;
		st = op_rename(p, q);
		if (victim && (st == ST_EXISTS || st == ST_UNSUPPORTED)) {
			f_rename_replace = 0;
			OUT("stress: rename over an existing file refused (%s), disabled\n",
			    st_name(st));
			return 0;
		}
		if (st == ST_UNSUPPORTED && !victim) {
			if (d != n->parent) {
				f_xdir_rename = 0;
				OUT("stress: rename into another directory unsupported, disabled\n");
			} else if (n->type == 'd') {
				f_dir_rename = 0;
				OUT("stress: directory rename unsupported, disabled\n");
			} else {
				return s_bad("rename", p, st);
			}
			return 0;
		}
		if (st != ST_OK)
			return s_bad("rename", p, st);
		/* Two names of one file: rename succeeds and does nothing
		 * (POSIX), both names stay. */
		if (victim && victim->data == n->data)
			return 0;
		if (victim) {
			m_detach(victim);
			m_free(victim);
		}
		m_detach(n);
		h_free(n->name);
		n->name = h_strdup(name);
		m_attach(d, n);
		return 0;
	}
	if (r < 840 && f_symlink) { /* symlink */
		struct mnode *d = m_pick('d');
		if (!d || m_depth(d) > 15)
			return 0;
		rand_name(name, d);
		if (m_find(d, name))
			return 0;
		m_path(d, p);
		h_snprintf(q, sizeof(q), "%s%s%s", p, p[1] ? "/" : "", name);
		char tgt[200];
		unsigned long tl = 1 + rng_below(rng_below(5) ? 30 : 150);
		for (unsigned long i = 0; i < tl; i++)
			tgt[i] = "abcdefgh/._xyz0123"[rng_below(18)];
		tgt[tl] = 0;
		st = op_symlink(tgt, q);
		if (st == ST_UNSUPPORTED) {
			f_symlink = 0;
			OUT("stress: symlinks unsupported, disabled\n");
			return 0;
		}
		if (st != ST_OK)
			return s_bad("symlink", q, st);
		struct mnode *n = h_calloc(1, sizeof(*n));
		n->name = h_strdup(name);
		n->type = 'l';
		n->target = h_strdup(tgt);
		m_attach(d, n);
		return 0;
	}
	if (r < 870 && f_link) { /* hard link */
		struct mnode *s = m_pick('f');
		struct mnode *d = m_pick('d');
		if (!s || !d || m_depth(d) > 15)
			return 0;
		rand_name(name, d);
		if (m_find(d, name))
			return 0;
		m_path(s, p);
		m_path(d, q);
		unsigned long ql = h_strlen(q);
		h_snprintf(q + ql, sizeof(q) - ql, "%s%s", ql > 1 ? "/" : "", name);
		st = op_link(p, q, 0);
		if (st == ST_UNSUPPORTED || st == ST_PERM) {
			f_link = 0;
			OUT("stress: hard links unsupported, disabled\n");
			return 0;
		}
		if (st != ST_OK)
			return s_bad("link", q, st);
		struct mnode *n = h_calloc(1, sizeof(*n));
		n->name = h_strdup(name);
		n->type = 'f';
		n->data = s->data;
		n->data->nlink++;
		m_attach(d, n);
		return 0;
	}
	if (r < 920 && f_xattr) { /* xattr set / remove */
		struct mnode *n = m_pick('f');
		if (!n)
			return 0;
		struct mdata *dd = n->data;
		m_path(n, p);
		if (dd->nx && rng_below(3) == 0) {
			int k = (int)rng_below((uint64_t)dd->nx);
			st = op_removexattr(p, 1, dd->xn[k]);
			if (st != ST_OK)
				return s_bad("removexattr", p, st);
			h_free(dd->xn[k]);
			h_free(dd->xv[k]);
			dd->nx--;
			dd->xn[k] = dd->xn[dd->nx];
			dd->xv[k] = dd->xv[dd->nx];
			dd->xl[k] = dd->xl[dd->nx];
			return 0;
		}
		char xn[64];
		int k = -1;
		if (dd->nx && rng_below(2)) {
			k = (int)rng_below((uint64_t)dd->nx);
			h_snprintf(xn, sizeof(xn), "%s", dd->xn[k]);
		} else {
			if (dd->nx >= 8)
				return 0;
			h_snprintf(xn, sizeof(xn), "user.t%llu",
				   (unsigned long long)rng_below(100000));
			for (int i = 0; i < dd->nx; i++)
				if (!h_strcmp(dd->xn[i], xn))
					return 0;
		}
		unsigned vl = (unsigned)rng_below(200);
		uint8_t v[200];
		uint64_t seed = rng();
		for (unsigned i = 0; i < vl; i++)
			v[i] = pat_byte(seed, i);
		st = op_setxattr(p, 1, xn, v, vl, 0);
		if (st == ST_UNSUPPORTED) {
			f_xattr = 0;
			OUT("stress: xattrs unsupported, disabled\n");
			return 0;
		}
		if (st == ST_NOSPC)
			return 0; /* attribute space is small on some filesystems */
		if (st != ST_OK)
			return s_bad("setxattr", p, st);
		if (k < 0) {
			k = dd->nx++;
			dd->xn[k] = h_strdup(xn);
		} else {
			h_free(dd->xv[k]);
		}
		dd->xv[k] = h_malloc(vl ? vl : 1);
		h_memcpy(dd->xv[k], v, vl);
		dd->xl[k] = vl;
		return 0;
	}
	if (r < 960) { /* fsync a file */
		struct mnode *n = m_pick('f');
		if (!n)
			return 0;
		m_path(n, p);
		st = op_fsync_path(p);
		if (st != ST_OK)
			return s_bad("fsync", p, st);
		return 0;
	}
	if (r < 980) { /* background writeback */
		test_clock_ns += 5ULL * 1000000000ULL;
		for (int i = 0; i < 8; i++)
			pagecache_timer_tick(1000ULL * (uint64_t)i);
		pagecache_writeback_if_due();
		return 0;
	}
	if (r < 990) { /* memory pressure */
		pagecache_shrink(64, 1);
		return 0;
	}
	mnt_flush(); /* sync */
	return 0;
}

static void m_reset(void)
{
	if (m_root)
		m_free(m_root);
	m_root = h_calloc(1, sizeof(*m_root));
	m_root->name = h_strdup("");
	m_root->type = 'd';
}

/* Load what is in a directory of the mounted fs into the model (so stress
 * may run on a populated image). */
static struct {
	uint64_t ino;
	struct mdata *d;
} g_ldino[1024];
static int g_nldino;

static int m_load(struct mnode *d, const char *p)
{
	struct dent *de;
	long n;
	int r = list_dir(p, &de, &n);
	if (r != ST_OK)
		return r;
	for (long i = 0; i < n; i++) {
		char cp[VFS_MAX_PATH];
		struct kstat st;
		h_snprintf(cp, sizeof(cp), "%s%s%s", p, p[1] ? "/" : "", de[i].name);
		if (op_stat(cp, &st, 0) != ST_OK)
			continue;
		struct mnode *c = h_calloc(1, sizeof(*c));
		c->name = h_strdup(de[i].name);
		c->type = type_char(st.st_mode);
		if (c->type == 'f' && st.st_nlink > 1) {
			/* a hard link to a file already loaded shares its data */
			int k;
			for (k = 0; k < g_nldino; k++)
				if (g_ldino[k].ino == st.st_ino)
					break;
			if (k < g_nldino) {
				c->data = g_ldino[k].d;
				c->data->nlink++;
				m_attach(d, c);
				continue;
			}
		}
		if (c->type == 'f') {
			vfs_file_t *f;
			c->data = h_calloc(1, sizeof(*c->data));
			c->data->nlink = 1;
			if (st.st_nlink > 1 && g_nldino < 1024) {
				g_ldino[g_nldino].ino = st.st_ino;
				g_ldino[g_nldino].d = c->data;
				g_nldino++;
			}
			if (op_open(cp, O_RDONLY, 0, &f) == ST_OK) {
				uint8_t *b = NULL;
				long l = read_all(f, st.st_size, NULL, &b);
				op_close(f);
				if (l >= 0) {
					c->data->buf = b;
					c->data->size = c->data->cap = (uint64_t)l;
					g_stress_bytes += (uint64_t)l;
				} else {
					h_free(b);
				}
			}
		} else if (c->type == 'l') {
			char t[VFS_MAX_PATH + 1];
			int l = op_readlink(cp, t, VFS_MAX_PATH);
			t[l > 0 ? l : 0] = 0;
			c->target = h_strdup(t);
		} else if (c->type != 'd') {
			h_free(c->name);
			h_free(c);
			continue;
		}
		m_attach(d, c);
		if (c->type == 'd')
			m_load(c, cp);
	}
	dents_free(de, n);
	return ST_OK;
}

static int do_stress(uint64_t seed, long ops, long remount_every,
		     unsigned long mflags, const char *opts)
{
	struct vfs_statfs sf;
	long fails0 = g_fail;

	rng_seed(seed);
	m_reset();
	g_nldino = 0;
	m_load(m_root, "/");
	if (op_statfs(&sf) == ST_OK && sf.f_bsize) {
		g_bsize = sf.f_bsize;
		uint64_t free = (uint64_t)sf.f_bavail * sf.f_bsize;
		if (g_stress_budget > free / 2)
			g_stress_budget = free / 2;
	}
	OUT("stress: seed %llu ops %ld block %lu budget %llu\n",
	    (unsigned long long)seed, ops, g_bsize,
	    (unsigned long long)g_stress_budget);
	for (long i = 0; i < ops; i++) {
		test_yields = 0;
		if (stress_one() < 0 || g_fail != fails0) {
			OUT("stress: stopped at op %ld\n", i);
			return ST_ERR;
		}
		if (remount_every && (i + 1) % remount_every == 0) {
			int r = do_umount();
			if (r == ST_OK)
				r = do_mount(mflags, opts);
			if (r != ST_OK) {
				failf("stress remount at op %ld: %s", i, st_name(r));
				return r;
			}
			s_verify();
			if (g_fail != fails0) {
				OUT("stress: model mismatch after remount at op %ld\n", i);
				return ST_ERR;
			}
		}
	}
	s_verify();
	int r = do_umount();
	if (r == ST_OK)
		r = do_mount(mflags, opts);
	if (r != ST_OK) {
		failf("stress final remount: %s", st_name(r));
		return r;
	}
	s_verify();
	m_collect();
	long nf = 0, nd = 0;
	for (long i = 0; i < g_nall; i++)
		nf += g_all[i]->type == 'f', nd += g_all[i]->type == 'd';
	OUT("stress: done, %ld files %ld dirs %llu bytes in the model\n", nf, nd,
	    (unsigned long long)g_stress_bytes);
	return g_fail != fails0 ? ST_ERR : ST_OK;
}

/* ================================================================== */
/* commands                                                            */
/* ================================================================== */

#define MAXARG 16
static vfs_file_t *g_h[16]; /* handles of the fopen family */
static unsigned long g_last_mflags;
static char g_last_opts[VFS_MOUNT_OPTS_MAX];
static unsigned long long g_mark_writes;

static int tokenize(char *s, char **argv)
{
	int argc = 0;
	char *w = s;

	while (*s) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s || *s == '#')
			break;
		if (argc == MAXARG)
			break;
		argv[argc++] = w;
		int q = 0;
		while (*s && (q || (*s != ' ' && *s != '\t'))) {
			if (*s == '"') {
				q = !q;
				s++;
				continue;
			}
			if (*s == '\\' && s[1]) {
				s++;
				if (*s == 'n') *w++ = '\n';
				else if (*s == 't') *w++ = '\t';
				else if (*s == 'x' && s[1] && s[2]) {
					char hx[3] = { s[1], s[2], 0 };
					*w++ = (char)h_strtoull(hx, NULL, 16);
					s += 2;
				} else *w++ = *s;
				s++;
				continue;
			}
			*w++ = *s++;
		}
		if (*s)
			s++;
		*w++ = 0;
	}
	return argc;
}

static int handle_no(const char *s)
{
	unsigned long long n = num(s);
	return n < 16 ? (int)n : -1;
}

static void print_stat(const char *path, const struct kstat *st)
{
	OUT("stat ");
	put_name(path);
	OUT(" type=%c mode=%04o nlink=%u uid=%u gid=%u size=%llu blocks=%llu "
	    "mtime=%llu.%09llu\n",
	    type_char(st->st_mode), st->st_mode & 07777, st->st_nlink,
	    st->st_uid, st->st_gid, (unsigned long long)st->st_size,
	    (unsigned long long)st->st_blocks, (unsigned long long)st->st_mtime,
	    (unsigned long long)st->st_mtime_nsec);
}

static void hexdump_line(const uint8_t *b, long n)
{
	for (long i = 0; i < n; i++)
		OUT("%02x", b[i]);
	OUT("\n");
}

/* Returns the status of the command. */
static long run_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	const char *a1 = argc > 1 ? argv[1] : NULL;
	const char *a2 = argc > 2 ? argv[2] : NULL;
	const char *a3 = argc > 3 ? argv[3] : NULL;

#define NARGS(n)                                                  \
	do {                                                      \
		if (argc < (n) + 1) {                             \
			failf("needs %d argument(s)", (n));       \
			return ST_INVALID;                        \
		}                                                 \
	} while (0)

	if (streq(c, "mount")) {
		unsigned long fl = parse_mflags(a1, 0);
		const char *opts = (a1 && (streq(a1, "ro") || streq(a1, "rw"))) ? a2 : a1;
		g_last_mflags = fl;
		h_snprintf(g_last_opts, sizeof(g_last_opts), "%s", opts ? opts : "");
		return do_mount(fl, opts);
	}
	if (streq(c, "umount") || streq(c, "unmount"))
		return do_umount();
	if (streq(c, "remount")) {
		NARGS(1);
		unsigned long fl = parse_mflags(a1, g_m.flags);
		return do_remount(fl, a2);
	}
	if (streq(c, "cycle")) { /* unmount and mount again as before */
		long r = do_umount();
		if (r != ST_OK)
			return r;
		return do_mount(g_last_mflags, g_last_opts[0] ? g_last_opts : NULL);
	}
	if (streq(c, "sync") || streq(c, "syncfs")) {
		if (!g_mounted)
			return ST_INVALID;
		mnt_flush();
		return ST_OK;
	}
	if (streq(c, "options")) {
		char buf[VFS_MOUNT_OPTS_MAX];
		if (!TEST_FSTYPE.show_options)
			return ST_UNSUPPORTED;
		long n = TEST_FSTYPE.show_options(&g_m, buf, sizeof(buf));
		if (n < 0 || n >= (long)sizeof(buf))
			return ST_INVALID;
		buf[n] = 0;
		OUT("options %s\n", buf);
		return ST_OK;
	}
	if (streq(c, "probe")) {
		if (!TEST_FSTYPE.probe)
			return ST_UNSUPPORTED;
		OUT("probe %d\n", TEST_FSTYPE.probe(&test_bdev));
		return ST_OK;
	}
	if (streq(c, "echo")) {
		for (int i = 1; i < argc; i++)
			OUT("%s%s", argv[i], i + 1 < argc ? " " : "\n");
		if (argc == 1)
			OUT("\n");
		return ST_OK;
	}
	if (streq(c, "stat") || streq(c, "lstat")) {
		NARGS(1);
		struct kstat st;
		long r = op_stat(a1, &st, c[0] == 's');
		if (r == ST_OK)
			print_stat(a1, &st);
		return r;
	}
	if (streq(c, "ls") || streq(c, "tree") || streq(c, "manifest") ||
	    streq(c, "readall")) {
		const char *root = a1 && a1[0] == '/' ? a1 : "/";
		unsigned mf = parse_mf(argc, argv, 1);
		if (streq(c, "ls"))
			return walk_tree(root, mf | MF_NOSHA | MF_MODE | MF_NLINK |
						       MF_OWNER,
					 1, 0);
		if (streq(c, "tree"))
			return walk_tree(root, mf | MF_NOSHA | MF_MODE | MF_NLINK |
						       MF_OWNER,
					 1, 1);
		if (streq(c, "readall")) {
			g_walk_files = g_walk_bytes = 0;
			long r = walk_tree(root, mf, 0, 1);
			OUT("readall %ld files %ld bytes\n", g_walk_files,
			    g_walk_bytes);
			return r;
		}
		return walk_tree(root, mf, 1, 1);
	}
	if (streq(c, "cat") || streq(c, "sha") || streq(c, "hexdump")) {
		NARGS(1);
		struct kstat st;
		vfs_file_t *f;
		long r = op_stat(a1, &st, 1);
		if (r != ST_OK)
			return r;
		r = op_open(a1, O_RDONLY, 0, &f);
		if (r != ST_OK)
			return r;
		struct sha256 sh;
		uint8_t *b = NULL;
		sha_init(&sh);
		r = read_all(f, st.st_size, &sh, &b);
		op_close(f);
		if (r < 0) {
			h_free(b);
			return r;
		}
		if (c[0] == 'c') {
			for (long i = 0; i < r; i++)
				OUT("%c", b[i]);
			if (r && b[r - 1] != '\n')
				OUT("\n");
		} else if (c[0] == 'h') {
			for (long i = 0; i < r; i += 32)
				hexdump_line(b + i, r - i < 32 ? r - i : 32);
		} else {
			char hex[65];
			sha_final(&sh, hex);
			OUT("sha %s %ld ", hex, r);
			put_name(a1);
			OUT("\n");
		}
		h_free(b);
		return ST_OK;
	}
	if (streq(c, "pread")) { /* pread PATH OFF LEN */
		NARGS(3);
		vfs_file_t *f;
		long r = op_open(a1, O_RDONLY, 0, &f);
		if (r != ST_OK)
			return r;
		long len = (long)num(a3);
		uint8_t *b = h_malloc((unsigned long)(len ? len : 1));
		r = op_pread(f, b, len, (long)num(a2));
		op_close(f);
		if (r >= 0) {
			struct sha256 sh;
			char hex[65];
			sha_init(&sh);
			sha_update(&sh, b, (unsigned long)r);
			sha_final(&sh, hex);
			OUT("pread %ld %s\n", r, hex);
			r = ST_OK;
		}
		h_free(b);
		return r;
	}
	if (streq(c, "cachepage")) { /* cachepage PATH IDX: a mapping's page */
		NARGS(2);
		vfs_file_t *f;
		long r = op_open(a1, O_RDONLY, 0, &f);
		if (r != ST_OK)
			return r;
		unsigned long idx = num(a2);
		struct pc_page *pg =
			f->ops->cache_page ? f->ops->cache_page(f, idx) : NULL;
		if (!pg) {
			op_close(f);
			OUT("cachepage %lu none\n", idx);
			return ST_OK;
		}
		/* It must hold what a read of the same page returns. */
		uint8_t *b = h_malloc(PAGE_SIZE);
		long got = op_pread(f, b, PAGE_SIZE, (long)(idx * PAGE_SIZE));
		int same = got == PAGE_SIZE && h_memcmp(b, pg->data, PAGE_SIZE) == 0;
		h_free(b);
		pagecache_put(pg);
		op_close(f);
		OUT("cachepage %lu %s\n", idx, same ? "same" : "DIFFERS");
		return same ? ST_OK : ST_IO;
	}
	if (streq(c, "zeros")) { /* zeros PATH OFF LEN: that range reads as 0 */
		NARGS(3);
		vfs_file_t *f;
		long len = (long)num(a3);
		long r = op_open(a1, O_RDONLY, 0, &f);
		if (r != ST_OK)
			return r;
		uint8_t *b = h_malloc((unsigned long)(len ? len : 1));
		r = op_pread(f, b, len, (long)num(a2));
		op_close(f);
		if (r >= 0) {
			if (r != len)
				failf("%s: read %ld of %ld bytes", a1, r, len);
			for (long i = 0; i < r; i++)
				if (b[i]) {
					failf("%s: byte %llu is 0x%02x, not 0", a1,
					      num(a2) + (unsigned long long)i, b[i]);
					break;
				}
			r = ST_OK;
		}
		h_free(b);
		return r;
	}
	if (streq(c, "check")) { /* check PATH SPEC: content equals the source */
		NARGS(2);
		struct src s;
		if (src_parse(a2, &s)) {
			failf("bad data source %s", a2);
			return ST_INVALID;
		}
		vfs_file_t *f;
		uint8_t *b = NULL;
		long r = op_open(a1, O_RDONLY, 0, &f);
		if (r == ST_OK) {
			r = read_all(f, s.len, NULL, &b);
			op_close(f);
		}
		if (r >= 0) {
			if ((uint64_t)r != s.len) {
				failf("%s: %ld bytes, expected %llu", a1, r,
				      (unsigned long long)s.len);
			} else {
				uint8_t tmp[4096];
				for (uint64_t o = 0; o < s.len; o += sizeof(tmp)) {
					unsigned long n = s.len - o < sizeof(tmp) ? (unsigned long)(s.len - o) : sizeof(tmp);
					src_get(&s, o, tmp, n);
					if (h_memcmp(tmp, b + o, n)) {
						failf("%s: content differs near %llu", a1,
						      (unsigned long long)o);
						break;
					}
				}
			}
			r = ST_OK;
		}
		h_free(b);
		h_free(s.owned);
		return r;
	}
	if (streq(c, "write") || streq(c, "append") || streq(c, "pwrite") ||
	    streq(c, "create")) {
		/* write PATH SPEC | append PATH SPEC | pwrite PATH OFF SPEC |
		 * create PATH [MODE] (O_EXCL) */
		NARGS(1);
		vfs_file_t *f;
		struct src s;
		long off = 0;
		const char *spec = a2;
		int flags = O_WRONLY | O_CREAT;
		if (streq(c, "write"))
			flags |= O_TRUNC;
		else if (streq(c, "append"))
			flags |= O_APPEND;
		else if (streq(c, "create")) {
			flags |= O_EXCL;
			long r = op_open(a1, flags, a2 ? (unsigned)num(a2) : 0644, &f);
			if (r == ST_OK)
				r = op_close(f);
			return r;
		} else {
			NARGS(3);
			off = (long)num(a2);
			spec = a3;
		}
		NARGS(2);
		if (src_parse(spec, &s)) {
			failf("bad data source %s", spec);
			return ST_INVALID;
		}
		long r = op_open(a1, flags, 0644, &f);
		if (r == ST_OK) {
			r = write_src(f, &s, off, streq(c, "append"));
			long cr = op_close(f);
			if (r == ST_OK)
				r = cr;
		}
		h_free(s.owned);
		return r;
	}
	if (streq(c, "mkdir")) {
		NARGS(1);
		return op_mkdir(a1, a2 ? (unsigned)num(a2) : 0755);
	}
	if (streq(c, "mkdirp")) { /* every missing component */
		NARGS(1);
		char p[VFS_MAX_PATH];
		if (canon(a1, p))
			return ST_INVALID;
		for (char *s = p + 1;; s++) {
			if (*s == '/' || !*s) {
				char ch = *s;
				*s = 0;
				long r = op_mkdir(p, 0755);
				if (r != ST_OK && r != ST_EXISTS)
					return r;
				*s = ch;
				if (!ch)
					break;
			}
		}
		return ST_OK;
	}
	if (streq(c, "rmdir")) {
		NARGS(1);
		return op_rmdir(a1);
	}
	if (streq(c, "unlink") || streq(c, "rm")) {
		NARGS(1);
		return op_unlink(a1);
	}
	if (streq(c, "rename") || streq(c, "mv")) {
		NARGS(2);
		return op_rename(a1, a2);
	}
	if (streq(c, "truncate")) {
		NARGS(2);
		return op_truncate_path(a1, (unsigned long)num(a2));
	}
	if (streq(c, "symlink")) { /* symlink TARGET PATH */
		NARGS(2);
		return op_symlink(a1, a2);
	}
	if (streq(c, "readlink")) {
		NARGS(1);
		char b[VFS_MAX_PATH * 4];
		int r = op_readlink(a1, b, sizeof(b) - 1);
		if (r < 0)
			return r;
		b[r] = 0;
		OUT("readlink ");
		put_name(a1);
		OUT(" -> ");
		put_name(b);
		OUT("\n");
		return ST_OK;
	}
	if (streq(c, "link")) {
		NARGS(2);
		return op_link(a1, a2, a3 && streq(a3, "follow"));
	}
	if (streq(c, "mknod")) { /* mknod PATH fifo|sock|chr|blk [DEV] */
		NARGS(2);
		unsigned mode = 0644;
		if (streq(a2, "fifo")) mode |= S_IFIFO;
		else if (streq(a2, "sock")) mode |= S_IFSOCK;
		else if (streq(a2, "chr")) mode |= S_IFCHR;
		else if (streq(a2, "blk")) mode |= S_IFBLK;
		return op_mknod(a1, mode, a3 ? num(a3) : 0);
	}
	if (streq(c, "chmod")) {
		NARGS(2);
		return op_chmod(a1, (unsigned)h_strtoull(a2, NULL, 8));
	}
	if (streq(c, "chown") || streq(c, "lchown")) {
		NARGS(3);
		return op_chown(a1, (int)h_strtoll(a2, NULL, 0),
				(int)h_strtoll(a3, NULL, 0), c[0] == 'l');
	}
	if (streq(c, "utimens") || streq(c, "lutimens")) { /* PATH ATIME MTIME */
		NARGS(3);
		struct vfs_utimes t;
		t.atime_sec = (int64_t)h_strtoll(a2, NULL, 0);
		t.atime_nsec = 0;
		t.mtime_sec = (int64_t)h_strtoll(a3, NULL, 0);
		t.mtime_nsec = argc > 4 ? (long)num(argv[4]) : 0;
		if (streq(a2, "now"))
			t.atime_nsec = VFS_UTIME_NOW;
		if (streq(a2, "omit"))
			t.atime_nsec = VFS_UTIME_OMIT;
		if (streq(a3, "now"))
			t.mtime_nsec = VFS_UTIME_NOW;
		if (streq(a3, "omit"))
			t.mtime_nsec = VFS_UTIME_OMIT;
		return op_utimens(a1, c[0] == 'l', &t);
	}
	if (streq(c, "setxattr")) { /* PATH NAME VALUE [create|replace] */
		NARGS(3);
		int fl = 0;
		if (argc > 4 && streq(argv[4], "create"))
			fl = 1;
		if (argc > 4 && streq(argv[4], "replace"))
			fl = 2;
		struct src s;
		if (src_parse(a3, &s)) { /* a bare value is text */
			s.kind = 0;
			s.buf = (const uint8_t *)a3;
			s.len = h_strlen(a3);
			s.owned = NULL;
		}
		uint8_t *v = h_malloc(s.len ? s.len : 1);
		src_get(&s, 0, v, (unsigned long)s.len);
		long r = op_setxattr(a1, 1, a2, v, (unsigned long)s.len, fl);
		h_free(v);
		h_free(s.owned);
		return r;
	}
	if (streq(c, "getxattr")) {
		NARGS(2);
		uint8_t v[65536];
		int r = op_getxattr(a1, 1, a2, NULL, 0);
		if (r < 0)
			return r;
		int r2 = op_getxattr(a1, 1, a2, v, sizeof(v));
		if (r2 < 0)
			return r2;
		if (r2 != r)
			failf("getxattr size query %d, read %d", r, r2);
		OUT("xattr ");
		put_name(a2);
		OUT(" %d ", r2);
		hexdump_line(v, r2 > 64 ? 64 : r2);
		return ST_OK;
	}
	if (streq(c, "listxattr")) {
		NARGS(1);
		char l[65536];
		int r = op_listxattr(a1, 1, l, sizeof(l));
		if (r < 0)
			return r;
		OUT("listxattr");
		for (int i = 0; i < r;) {
			OUT(" ");
			put_name(l + i);
			i += (int)h_strlen(l + i) + 1;
		}
		OUT("\n");
		return ST_OK;
	}
	if (streq(c, "rmxattr") || streq(c, "removexattr")) {
		NARGS(2);
		return op_removexattr(a1, 1, a2);
	}
	if (streq(c, "statfs")) {
		struct vfs_statfs sf;
		long r = op_statfs(&sf);
		if (r != ST_OK)
			return r;
		OUT("statfs type=0x%lx bsize=%lu frsize=%lu blocks=%lu bfree=%lu "
		    "bavail=%lu files=%lu ffree=%lu namelen=%lu\n",
		    sf.f_type, sf.f_bsize, sf.f_frsize, sf.f_blocks, sf.f_bfree,
		    sf.f_bavail, sf.f_files, sf.f_ffree, sf.f_namelen);
		return ST_OK;
	}
	if (streq(c, "fsync")) {
		NARGS(1);
		return op_fsync_path(a1);
	}
	/* ---- handles: fopen H PATH FLAGS [MODE] ... ---- */
	if (streq(c, "freaddir")) { /* freaddir H: one readdir call on handle H */
		NARGS(1);
		int h = handle_no(a1);
		if (h < 0 || !g_h[h] || !g_h[h]->ops->readdir)
			return ST_INVALID;
		uint8_t *b = h_malloc(4096);
		h_memset(b, 0xAA, 4096);
		long r = g_h[h]->ops->readdir(g_h[h], b, 4096);
		OUT("freaddir %ld\n", r);
		for (long off = 0; r > 0 && off < r;) {
			struct dirent64 *e = (struct dirent64 *)(b + off);
			OUT("  ent ino=%lu off=%ld type=%u reclen=%u name=",
			    (unsigned long)e->d_ino, (long)e->d_off, e->d_type,
			    e->d_reclen);
			for (const char *q = (const char *)b + off + 19; *q; q++)
				OUT("%c", (*q >= 32 && *q < 127) ? *q : '?');
			OUT("\n");
			if (e->d_reclen == 0)
				break;
			off += e->d_reclen;
		}
		h_free(b);
		return r < 0 ? r : ST_OK;
	}
	if (streq(c, "fopen")) {
		NARGS(3);
		int h = handle_no(a1);
		int fl = 0;
		if (h < 0 || g_h[h]) {
			failf("bad or busy handle %s", a1);
			return ST_INVALID;
		}
		for (const char *s = a3; *s; s++) {
			switch (*s) {
			case 'r': break;
			case 'w': fl = (fl & ~3) | O_WRONLY; break;
			case '+': fl = (fl & ~3) | O_RDWR; break;
			case 'c': fl |= O_CREAT; break;
			case 'x': fl |= O_EXCL; break;
			case 't': fl |= O_TRUNC; break;
			case 'a': fl |= O_APPEND; break;
			case 'd': fl |= O_DIRECTORY; break;
			case 'D': fl |= O_DIRECT; break;
			case 'n': fl |= O_NOFOLLOW; break;
			}
		}
		return op_open(a2, fl, argc > 4 ? (unsigned)h_strtoull(argv[4], NULL, 8) : 0644,
			       &g_h[h]);
	}
	if (streq(c, "fclose")) {
		NARGS(1);
		int h = handle_no(a1);
		if (h < 0 || !g_h[h])
			return ST_INVALID;
		long r = op_close(g_h[h]);
		g_h[h] = NULL;
		return r;
	}
	if (streq(c, "fwrite") || streq(c, "fpwrite")) { /* fwrite H SPEC | fpwrite H OFF SPEC */
		NARGS(2);
		int h = handle_no(a1);
		if (h < 0 || !g_h[h])
			return ST_INVALID;
		struct src s;
		const char *spec = c[1] == 'p' ? a3 : a2;
		if (!spec || src_parse(spec, &s))
			return ST_INVALID;
		long r = write_src(g_h[h], &s, c[1] == 'p' ? (long)num(a2) : 0,
				   c[1] != 'p');
		h_free(s.owned);
		return r;
	}
	if (streq(c, "fread") || streq(c, "fpread")) { /* fread H LEN | fpread H OFF LEN */
		NARGS(2);
		int h = handle_no(a1);
		if (h < 0 || !g_h[h])
			return ST_INVALID;
		long len = (long)num(c[1] == 'p' ? a3 : a2);
		uint8_t *b = h_malloc((unsigned long)(len ? len : 1));
		long r = c[1] == 'p' ? op_pread(g_h[h], b, len, (long)num(a2)) :
				       g_h[h]->ops->read(g_h[h], b, len);
		if (r >= 0) {
			struct sha256 sh;
			char hex[65];
			sha_init(&sh);
			sha_update(&sh, b, (unsigned long)r);
			sha_final(&sh, hex);
			OUT("fread %ld %s\n", r, hex);
			r = ST_OK;
		}
		h_free(b);
		return r;
	}
	if (streq(c, "fseek")) {
		NARGS(3);
		int h = handle_no(a1);
		if (h < 0 || !g_h[h] || !g_h[h]->ops->seek)
			return ST_INVALID;
		long r = g_h[h]->ops->seek(g_h[h], (long)h_strtoll(a2, NULL, 0),
					   (int)num(a3));
		if (r >= 0)
			OUT("fseek %ld\n", r);
		return r < 0 ? r : ST_OK;
	}
	if (streq(c, "ftruncate")) {
		NARGS(2);
		int h = handle_no(a1);
		if (h < 0 || !g_h[h] || !g_h[h]->ops->truncate)
			return ST_INVALID;
		return g_h[h]->ops->truncate(g_h[h], (unsigned long)num(a2));
	}
	if (streq(c, "ffsync")) {
		NARGS(1);
		int h = handle_no(a1);
		if (h < 0 || !g_h[h])
			return ST_INVALID;
		return g_h[h]->ops->fsync ? g_h[h]->ops->fsync(g_h[h]) : ST_OK;
	}
	if (streq(c, "fstat")) {
		NARGS(1);
		int h = handle_no(a1);
		struct kstat st;
		if (h < 0 || !g_h[h] || !g_h[h]->ops->fstat)
			return ST_INVALID;
		h_memset(&st, 0, sizeof(st));
		long r = g_h[h]->ops->fstat(g_h[h], &st);
		if (r == ST_OK)
			print_stat(a1, &st);
		return r;
	}
	/* ---- device and environment control ---- */
	if (streq(c, "trap")) {
		NARGS(1);
		hbd_set_trap(streq(a1, "on"));
		return ST_OK;
	}
	if (streq(c, "log")) { /* log on|off|clear|dump [FILE] */
		NARGS(1);
		if (streq(a1, "on")) hbd_set_log(1);
		else if (streq(a1, "off")) hbd_set_log(0);
		else if (streq(a1, "clear")) hbd_log_clear();
		else if (streq(a1, "dump")) return hbd_log_dump(a2) ? ST_IO : ST_OK;
		else return ST_INVALID;
		return ST_OK;
	}
	if (streq(c, "logorder")) {
		/* logorder A_LBA A_COUNT B_LBA B_COUNT: in the write log, the
		 * first write touching range A comes before the first write
		 * touching range B (and A is written at all when B is). */
		NARGS(4);
		long ia = hbd_log_find('W', num(a1), num(a2), 0);
		long ib = hbd_log_find('W', num(a3), num(argv[4]), 0);
		if (ib < 0)
			return ST_OK;
		if (ia < 0 || ia > ib) {
			failf("range %s+%s is %s, range %s+%s first written at entry %ld",
			      a1, a2, ia < 0 ? "never written" : "written later", a3,
			      argv[4], ib);
			return ST_ERR;
		}
		return ST_OK;
	}
	if (streq(c, "logcount")) { /* logcount W|D|F|X LBA COUNT */
		NARGS(3);
		long n = 0;
		for (long i = hbd_log_find(a1[0], num(a2), num(a3), 0); i >= 0;
		     i = hbd_log_find(a1[0], num(a2), num(a3), i + 1))
			n++;
		OUT("logcount %c %s %s %ld\n", a1[0], a2, a3, n);
		return ST_OK;
	}
	if (streq(c, "fault")) { /* fault read|write|cut|readall N */
		NARGS(2);
		char k = streq(a1, "read") ? 'r' : streq(a1, "write") ? 'w' :
			 streq(a1, "cut") ? 'c' : streq(a1, "readall") ? 'R' : 0;
		if (!k)
			return ST_INVALID;
		hbd_fault(k, num(a2));
		return ST_OK;
	}
	if (streq(c, "kfault")) { /* fail the Nth kalloc from now */
		NARGS(1);
		test_kalloc_fail_at = num(a1) ? test_kalloc_count + num(a1) : 0;
		return ST_OK;
	}
	if (streq(c, "kcount")) { /* kallocs so far (to size kfault sweeps) */
		OUT("kcount %llu\n", (unsigned long long)test_kalloc_count);
		return ST_OK;
	}
	if (streq(c, "mark")) {
		hbd_counts(NULL, &g_mark_writes, NULL, NULL, NULL, NULL);
		unsigned long long d;
		hbd_counts(NULL, NULL, &d, NULL, NULL, NULL);
		g_mark_writes += d;
		return ST_OK;
	}
	if (streq(c, "nowrites")) { /* no write or discard since mark */
		unsigned long long w, d;
		hbd_counts(NULL, &w, &d, NULL, NULL, NULL);
		if (w + d != g_mark_writes) {
			failf("%llu write/discard request(s) since mark",
			      w + d - g_mark_writes);
			return ST_ERR;
		}
		return ST_OK;
	}
	if (streq(c, "counts")) {
		unsigned long long r, w, d, f, ws, dr;
		hbd_counts(&r, &w, &d, &f, &ws, &dr);
		OUT("counts reads=%llu writes=%llu discards=%llu flushes=%llu "
		    "wsectors=%llu dropped=%llu\n", r, w, d, f, ws, dr);
		return ST_OK;
	}
	if (streq(c, "save")) { /* save the device as it reads now */
		NARGS(1);
		return hbd_save(a1) ? ST_IO : ST_OK;
	}
	if (streq(c, "tick")) { /* tick MS: time passes, timer ticks fire */
		NARGS(1);
		uint64_t t0 = timer_ticks();
		test_clock_ns += num(a1) * 1000000ULL;
		uint64_t t1 = timer_ticks();
		uint64_t irq = test_irq_flag;
		test_irq_flag = 0; /* the tick runs in the timer interrupt */
		for (uint64_t t = t0 + 1; t <= t1 && t - t0 < 100000; t++)
			pagecache_timer_tick(t);
		test_irq_flag = irq;
		return ST_OK;
	}
	if (streq(c, "wb")) { /* the flush thread's pass */
		pagecache_writeback_if_due();
		return ST_OK;
	}
	if (streq(c, "flushall")) {
		pagecache_flush_all();
		return ST_OK;
	}
	if (streq(c, "shrink")) { /* shrink N [dirty] */
		NARGS(1);
		unsigned long n = pagecache_shrink((unsigned long)num(a1),
						   a2 && streq(a2, "dirty"));
		if (g_verbose)
			OUT("shrink %lu\n", n);
		return ST_OK;
	}
	if (streq(c, "dropcaches")) { /* drop every clean page, then names */
		if (g_m.sb) {
			pagecache_flush_sb(g_m.sb);
			icache_flush_sb(g_m.sb);
		}
		pagecache_shrink(1UL << 30, 0);
		dcache_invalidate_owner(g_m.sb);
		return ST_OK;
	}
	if (streq(c, "freepages")) { /* pretend memory is short */
		NARGS(1);
		test_free_pages_override = num(a1);
		return ST_OK;
	}
	if (streq(c, "reclaim")) { /* reclaim N: every cache's shrinker */
		extern unsigned long test_vm_shrink(unsigned long nr);
		NARGS(1);
		OUT("reclaim freed=%lu\n", test_vm_shrink(num(a1)));
		return ST_OK;
	}
	if (streq(c, "cachestats")) {
		pc_stats_t ps;
		ic_stats_t is;
		pagecache_get_stats(&ps);
		icache_get_stats(&is);
		OUT("cachestats pages=%llu inodes=%llu heap=%ld pages_live=%llu\n",
		    (unsigned long long)ps.total_pages,
		    (unsigned long long)is.total_entries, test_kalloc_live,
		    (unsigned long long)test_pages_live);
		return ST_OK;
	}
	if (streq(c, "uid")) { /* uid UID GID */
		NARGS(2);
		test_task_init((uint32_t)num(a1), (uint32_t)num(a2));
		return ST_OK;
	}
	if (streq(c, "umask")) {
		NARGS(1);
		test_task_set_umask((uint32_t)h_strtoull(a1, NULL, 8));
		return ST_OK;
	}
	if (streq(c, "task")) { /* switch the task id (lock ownership) */
		NARGS(1);
		test_task_set_id(num(a1));
		return ST_OK;
	}
	if (streq(c, "release")) { /* release_locks_for_task ID */
		NARGS(1);
		if (!g_m.ops->release_locks_for_task)
			return ST_UNSUPPORTED;
		return g_m.ops->release_locks_for_task(&g_m, num(a1));
	}
	if (streq(c, "rawro")) {
		NARGS(1);
		g_raw_ro = streq(a1, "on");
		return ST_OK;
	}
	if (streq(c, "linkhop")) {
		NARGS(1);
		g_linkhop = streq(a1, "on");
		return ST_OK;
	}
	if (streq(c, "chunk")) {
		NARGS(1);
		g_chunk = (long)num(a1);
		return ST_OK;
	}
	if (streq(c, "warnings")) { /* warnings ok|fail */
		NARGS(1);
		g_allow_warn = streq(a1, "ok");
		return ST_OK;
	}
	if (streq(c, "leakcheck")) {
		NARGS(1);
		g_leakcheck = streq(a1, "on");
		return ST_OK;
	}
	if (streq(c, "stress")) { /* stress SEED OPS [REMOUNT_EVERY] */
		NARGS(2);
		return do_stress(num(a1), (long)num(a2), a3 ? (long)num(a3) : 0,
				 g_last_mflags, g_last_opts[0] ? g_last_opts : NULL);
	}
	if (streq(c, "stressopt")) { /* stressopt KEY VALUE */
		NARGS(2);
		if (streq(a1, "budget")) g_stress_budget = num(a2);
		else if (streq(a1, "maxfile")) g_max_file = num(a2);
		else if (streq(a1, "names")) {
			g_names_dos = streq(a2, "dos");
			g_names_ci = streq(a2, "dos") || streq(a2, "ci");
		} else if (streq(a1, "utf8")) g_names_utf8 = (int)num(a2);
		else if (streq(a1, "symlink")) f_symlink = (int)num(a2);
		else if (streq(a1, "link")) f_link = (int)num(a2);
		else if (streq(a1, "xattr")) f_xattr = (int)num(a2);
		else if (streq(a1, "replace")) f_rename_replace = (int)num(a2);
		else if (streq(a1, "dirrename")) f_dir_rename = (int)num(a2);
		else if (streq(a1, "xdirrename")) f_xdir_rename = (int)num(a2);
		else if (streq(a1, "holes")) f_holes = (int)num(a2);
		else if (streq(a1, "truncate")) f_truncate = (int)num(a2);
		else return ST_INVALID;
		return ST_OK;
	}
	failf("unknown command");
	return ST_INVALID;
}

/* Longest script line: room for two whole paths (symlink, rename, link)
 * and their quoting. */
#define TF_LINE_MAX (4 * VFS_MAX_PATH + 256)

/* One script line: [=STATUS|?] command args... */
static void run_line(char *line)
{
	char *argv[MAXARG + 1];
	char copy[TF_LINE_MAX];
	long want = ST_OK;
	int any = 0;
	int argc;

	h_snprintf(copy, sizeof(copy), "%s", line);
	argc = tokenize(line, argv);
	if (argc == 0)
		return;
	if (argv[0][0] == '?' && argv[0][1]) { /* "?cmd" */
		any = 1;
		argv[0]++;
	} else if (argv[0][0] == '=' || argv[0][0] == '?') {
		if (argv[0][0] == '?')
			any = 1;
		else if (st_parse(argv[0] + 1, &want)) {
			g_curcmd = copy;
			failf("unknown status %s", argv[0] + 1);
			return;
		}
		for (int i = 1; i <= argc; i++)
			argv[i - 1] = argv[i];
		argc--;
		if (argc == 0)
			return;
	}
	g_curcmd = copy;
	test_yields = 0;
	long w0 = test_warnings;
	if (g_verbose)
		OUT("> %s\n", copy);
	long r = run_cmd(argc, argv);
	if (r > 0)
		r = ST_OK;
	if (!any && r != want)
		failf("got %s, expected %s", st_name(r), st_name(want));
	else if (any && g_verbose)
		OUT("= %s\n", st_name(r));
	if (test_warnings != w0 && !g_allow_warn)
		failf("the kernel code printed %ld WARNING/BUG line(s)",
		      test_warnings - w0);
	if (test_failures) {
		g_fail += test_failures;
		test_failures = 0;
	}
	if (test_spins_held)
		failf("%d spinlock(s) still held after the command",
		      test_spins_held);
	host_flush_out();
}

static void run_script_text(const char *text)
{
	char line[TF_LINE_MAX];
	unsigned long n = 0;

	for (const char *s = text;; s++) {
		if (*s == ';' || *s == '\n' || !*s) {
			line[n] = 0;
			g_lineno++;
			run_line(line);
			n = 0;
			if (!*s)
				break;
			continue;
		}
		if (n + 1 < sizeof(line))
			line[n++] = *s;
	}
}

static void usage(void)
{
	h_fprintf_err(
		"usage: test-fs [options] IMAGE\n"
		"  -c 'CMD; CMD'      commands (repeatable, run in order)\n"
		"  -s FILE            script file ('-' = stdin)\n"
		"  --sector N         sector size 512/2048/4096 (default 512)\n"
		"  --ro-dev           the device is write-protected\n"
		"  --cow              writes stay in memory (image untouched)\n"
		"  --patch FILE       byte patches applied over reads\n"
		"  --trap             any write is a failure (from the start)\n"
		"  --log              log writes/discards/flushes\n"
		"  --allow-warn       kernel WARNINGs do not fail the run\n"
		"  --quiet            no kernel log on stderr\n"
		"  --uid U --gid G    credentials of the task (default 0 0)\n"
		"  --epoch SECS       wall clock at start\n"
		"  --pages N          page limit (default 262144)\n"
		"  --kalloc-max BYTES largest single kalloc (default 64M)\n"
		"  --no-discard       the device has no discard\n"
		"  --no-leakcheck     no leak checks after unmount\n"
		"  --chunk N          read/write chunk size (default 65536)\n"
		"  --no-linkhop       drivers follow links themselves\n"
		"  --save FILE        save the device (overlay applied) at exit\n"
		"  -v                 echo commands\n");
	h_exit(2);
}

int main(int argc, char **argv)
{
	const char *image = NULL, *patch = NULL, *save = NULL;
	unsigned sector = 512;
	int ro_dev = 0, cow = 0, trap = 0, logw = 0;
	uint32_t uid = 0, gid = 0;
	const char *scripts[64];
	int kinds[64], nscripts = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
#define ARGV() (i + 1 < argc ? argv[++i] : (usage(), (char *)0))
		if (streq(a, "-c")) { kinds[nscripts] = 0; scripts[nscripts++] = ARGV(); }
		else if (streq(a, "-s")) { kinds[nscripts] = 1; scripts[nscripts++] = ARGV(); }
		else if (streq(a, "--sector")) sector = (unsigned)num(ARGV());
		else if (streq(a, "--ro-dev")) ro_dev = 1;
		else if (streq(a, "--cow")) cow = 1;
		else if (streq(a, "--patch")) patch = ARGV();
		else if (streq(a, "--trap")) trap = 1;
		else if (streq(a, "--log")) logw = 1;
		else if (streq(a, "--allow-warn")) g_allow_warn = 1;
		else if (streq(a, "--quiet")) test_quiet_klog = 1;
		else if (streq(a, "--uid")) uid = (uint32_t)num(ARGV());
		else if (streq(a, "--gid")) gid = (uint32_t)num(ARGV());
		else if (streq(a, "--epoch")) test_epoch_base = num(ARGV());
		else if (streq(a, "--pages")) test_pages_limit = num(ARGV());
		else if (streq(a, "--kalloc-max")) test_kalloc_max = num(ARGV());
		else if (streq(a, "--no-discard")) test_bdev_discard = 0;
		else if (streq(a, "--no-leakcheck")) g_leakcheck = 0;
		else if (streq(a, "--chunk")) g_chunk = (long)num(ARGV());
		else if (streq(a, "--no-linkhop")) g_linkhop = 0;
		else if (streq(a, "--save")) save = ARGV();
		else if (streq(a, "-v")) g_verbose = 1;
		else if (a[0] == '-' && a[1]) usage();
		else image = a;
		if (nscripts >= 63)
			usage();
	}
	if (!image)
		usage();
	if (hbd_open(image, sector, ro_dev, cow))
		return 2;
	if (patch && hbd_load_patches(patch) < 0) {
		h_fprintf_err("test-fs: cannot read patches %s\n", patch);
		return 2;
	}
	hbd_set_trap(trap);
	hbd_set_log(logw);
	test_task_init(uid, gid);
	test_bdev_setup("sdz1", ro_dev);
	pagecache_init();
	icache_init();
	dcache_init();

	for (int i = 0; i < nscripts; i++) {
		if (kinds[i] == 0) {
			run_script_text(scripts[i]);
			continue;
		}
		void *fh = h_open_script(scripts[i]);
		char line[TF_LINE_MAX];
		if (!fh) {
			h_fprintf_err("test-fs: cannot open script %s\n", scripts[i]);
			return 2;
		}
		while (h_read_line(fh, line, sizeof(line))) {
			g_lineno++;
			run_line(line);
		}
		h_close_script(fh);
	}
	g_curcmd = "(end)";
	for (int h = 0; h < 16; h++)
		if (g_h[h]) {
			op_close(g_h[h]);
			g_h[h] = NULL;
		}
	if (g_mounted) {
		long w0 = test_warnings;
		int r = do_umount();
		if (r != ST_OK)
			failf("final unmount: %s", st_name(r));
		if (test_warnings != w0 && !g_allow_warn)
			failf("WARNING during the final unmount");
	}
	if (test_failures)
		g_fail += test_failures;
	if (save && hbd_save(save))
		h_fprintf_err("test-fs: cannot save %s\n", save);
	(void)g_fsck_after;
	{
		unsigned long long r, w, d, f, ws, dr;
		hbd_counts(&r, &w, &d, &f, &ws, &dr);
		h_fprintf_err("test-fs: %s reads=%llu writes=%llu discards=%llu "
			      "flushes=%llu warnings=%ld io_errors=%ld heap_peak=%llu "
			      "pages_peak=%llu failures=%ld\n",
			      g_fail ? "FAILED" : "ok", r, w, d, f, test_warnings,
			      test_io_errors, (unsigned long long)test_kalloc_peak,
			      (unsigned long long)test_pages_peak, g_fail);
	}
	hbd_close();
	m_reset();
	m_free(m_root);
	h_free(g_all);
	host_flush_out();
	return g_fail ? 1 : 0;
}
