/* File-backed block device and host helpers for the filesystem harness
 * (host/test-fs.sh).
 *
 * This unit is compiled against the HOST C library, not the kernel
 * headers (the two disagree about uint64_t and friends), so its interface
 * to the kernel side (test-fs-stubs.c, test-fs-main.c) uses only plain C
 * types.  It provides:
 *
 *   - one block device over an image file: pread/pwrite in units of the
 *     configured sector size (512, 2048 or 4096), optionally read-only;
 *   - copy-on-write overlay mode: writes land in memory and the image file
 *     is never modified (fuzzing, replaying crashers);
 *   - byte patches applied over what is read (the fuzzer's mutations);
 *   - write trap: any write or discard is a test failure with a stack
 *     trace -- proves that a mount, a walk and an unmount never write;
 *   - a log of every write, discard and flush (sequence, lba, count) that
 *     can be dumped for ordering assertions;
 *   - fault injection: fail the Nth read or write with an I/O error, or
 *     "cut the power" after N writes (later writes are dropped silently);
 *   - small file helpers for the runner.
 *
 * Copyright (C) 2026 The LikeOS Project */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void __sanitizer_print_stack_trace(void);

/* ---- overlay: modified sectors kept in memory ---- */
struct ov_ent {
	unsigned long long lba;
	struct ov_ent *next;
	unsigned char data[];
};

#define OV_BUCKETS 65536u

struct hbd {
	int fd;
	int ro; /* image opened read-only: writes fail (ST_IO) */
	int cow; /* writes go to the overlay */
	unsigned sector;
	unsigned long long sectors;
	unsigned long long file_bytes;
	struct ov_ent **ov;
	/* patches: sorted (offset, byte) pairs applied over reads */
	unsigned long long *p_off;
	unsigned char *p_val;
	unsigned long p_n, p_cap;
	/* trap / log / faults */
	int trap;
	int log_on;
	unsigned long long seq;
	unsigned long long n_reads, n_writes, n_discards, n_flushes;
	unsigned long long w_sectors, r_sectors;
	unsigned long long fail_read_at, fail_write_at; /* 0 = off */
	unsigned long long cut_after; /* 0 = off: writes after the Nth are dropped */
	int cut_active;
	unsigned long long dropped;
	int discard_scribble;
	int fail_reads_from_off; /* sticky failure of every read */
	/* log */
	struct hlog {
		unsigned long long seq;
		char op;
		unsigned long long lba, count;
	} *log;
	unsigned long log_n, log_cap;
};

static struct hbd g;

static void hbd_die(const char *msg)
{
	fprintf(stderr, "test-fs: %s\n", msg);
	fflush(stderr);
	exit(2);
}

int hbd_open(const char *path, unsigned sector, int ro, int cow)
{
	struct stat st;

	memset(&g, 0, sizeof(g));
	g.fd = open(path, (ro || cow) ? O_RDONLY : O_RDWR);
	if (g.fd < 0) {
		fprintf(stderr, "test-fs: cannot open image %s: %s\n", path,
			strerror(errno));
		return -1;
	}
	if (fstat(g.fd, &st) != 0)
		return -1;
	if (sector != 512 && sector != 1024 && sector != 2048 &&
	    sector != 4096) {
		fprintf(stderr, "test-fs: bad sector size %u\n", sector);
		return -1;
	}
	g.ro = ro;
	g.cow = cow;
	g.sector = sector;
	g.file_bytes = (unsigned long long)st.st_size;
	g.sectors = g.file_bytes / sector;
	g.discard_scribble = 1;
	if (cow) {
		g.ov = calloc(OV_BUCKETS, sizeof(*g.ov));
		if (!g.ov)
			hbd_die("out of memory (overlay)");
	}
	return 0;
}

void hbd_close(void)
{
	if (g.ov) {
		for (unsigned i = 0; i < OV_BUCKETS; i++) {
			struct ov_ent *e = g.ov[i], *n;
			for (; e; e = n) {
				n = e->next;
				free(e);
			}
		}
		free(g.ov);
	}
	free(g.p_off);
	free(g.p_val);
	free(g.log);
	if (g.fd >= 0)
		close(g.fd);
	memset(&g, 0, sizeof(g));
	g.fd = -1;
}

unsigned long long hbd_sectors(void)
{
	return g.sectors;
}

unsigned hbd_sector_size(void)
{
	return g.sector;
}

int hbd_is_ro(void)
{
	return g.ro;
}

static struct ov_ent *ov_find(unsigned long long lba)
{
	struct ov_ent *e = g.ov[(lba * 2654435761ull) % OV_BUCKETS];

	for (; e; e = e->next)
		if (e->lba == lba)
			return e;
	return NULL;
}

static struct ov_ent *ov_get(unsigned long long lba)
{
	struct ov_ent *e = ov_find(lba);

	if (e)
		return e;
	e = malloc(sizeof(*e) + g.sector);
	if (!e)
		hbd_die("out of memory (overlay sector)");
	e->lba = lba;
	if (pread(g.fd, e->data, g.sector, (off_t)(lba * g.sector)) !=
	    (ssize_t)g.sector)
		memset(e->data, 0, g.sector);
	unsigned b = (unsigned)((lba * 2654435761ull) % OV_BUCKETS);
	e->next = g.ov[b];
	g.ov[b] = e;
	return e;
}

static void log_add(char op, unsigned long long lba, unsigned long long count)
{
	g.seq++;
	if (!g.log_on)
		return;
	if (g.log_n == g.log_cap) {
		unsigned long nc = g.log_cap ? g.log_cap * 2 : 4096;
		struct hlog *nl = realloc(g.log, nc * sizeof(*nl));
		if (!nl)
			hbd_die("out of memory (write log)");
		g.log = nl;
		g.log_cap = nc;
	}
	g.log[g.log_n].seq = g.seq;
	g.log[g.log_n].op = op;
	g.log[g.log_n].lba = lba;
	g.log[g.log_n].count = count;
	g.log_n++;
}

/* Apply the patches that fall into [off, off+len) to buf. */
static void apply_patches(unsigned long long off, unsigned long long len,
			  unsigned char *buf)
{
	unsigned long lo = 0, hi = g.p_n;

	if (!g.p_n)
		return;
	while (lo < hi) { /* first patch >= off */
		unsigned long mid = (lo + hi) / 2;
		if (g.p_off[mid] < off)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (; lo < g.p_n && g.p_off[lo] < off + len; lo++)
		buf[g.p_off[lo] - off] = g.p_val[lo];
}

static void trap_hit(const char *what, unsigned long long lba,
		     unsigned long long count)
{
	fprintf(stderr,
		"\nWRITE TRAP: %s lba=%llu count=%llu (sector %u) -- a write "
		"in a run that must not write\n",
		what, lba, count, g.sector);
	__sanitizer_print_stack_trace();
	fflush(stderr);
	printf("FAIL write-trap %s lba=%llu count=%llu\n", what, lba, count);
	fflush(stdout);
	_exit(3);
}

/* 0 ok, -1 I/O error, -2 out of range */
int hbd_read(unsigned long long lba, unsigned long long count, void *buf)
{
	unsigned char *p = buf;

	g.n_reads++;
	if (count == 0)
		return 0;
	if (lba >= g.sectors || count > g.sectors - lba)
		return -2;
	if (g.fail_reads_from_off ||
	    (g.fail_read_at && g.n_reads == g.fail_read_at))
		return -1;
	g.r_sectors += count;
	for (unsigned long long i = 0; i < count; i++) {
		struct ov_ent *e = g.ov ? ov_find(lba + i) : NULL;
		unsigned char *dst = p + i * g.sector;
		if (e) {
			memcpy(dst, e->data, g.sector);
			continue;
		}
		/* runs of sectors without overlay: one pread */
		unsigned long long j = i + 1;
		while (j < count && !(g.ov && ov_find(lba + j)))
			j++;
		size_t want = (size_t)((j - i) * g.sector);
		ssize_t r = pread(g.fd, dst, want, (off_t)((lba + i) * g.sector));
		if (r < 0)
			return -1;
		if ((size_t)r < want)
			memset(dst + r, 0, want - (size_t)r);
		i = j - 1;
	}
	apply_patches(lba * g.sector, count * g.sector, p);
	return 0;
}

int hbd_write(unsigned long long lba, unsigned long long count,
	      const void *buf)
{
	const unsigned char *p = buf;

	if (g.trap)
		trap_hit("write", lba, count);
	g.n_writes++;
	if (count == 0)
		return 0;
	if (lba >= g.sectors || count > g.sectors - lba)
		return -2;
	if (g.ro)
		return -1;
	if (g.fail_write_at && g.n_writes == g.fail_write_at)
		return -1;
	if (g.cut_after && g.n_writes > g.cut_after) {
		g.cut_active = 1;
		g.dropped++;
		log_add('X', lba, count);
		return 0; /* the power is gone: the device "accepts" and loses it */
	}
	log_add('W', lba, count);
	g.w_sectors += count;
	if (g.cow) {
		for (unsigned long long i = 0; i < count; i++)
			memcpy(ov_get(lba + i)->data, p + i * g.sector,
			       g.sector);
		return 0;
	}
	size_t want = (size_t)(count * g.sector);
	ssize_t r = pwrite(g.fd, p, want, (off_t)(lba * g.sector));
	return r == (ssize_t)want ? 0 : -1;
}

int hbd_flush(void)
{
	g.n_flushes++;
	log_add('F', 0, 0);
	if (!g.cow && !g.ro && !g.cut_active)
		fdatasync(g.fd);
	return 0;
}

/* Discarded sectors read back as a scribble pattern by default: a driver
 * that discards blocks still in use loses their content visibly. */
int hbd_discard(unsigned long long lba, unsigned long long count)
{
	if (g.trap)
		trap_hit("discard", lba, count);
	g.n_discards++;
	if (count == 0)
		return 0;
	if (lba >= g.sectors || count > g.sectors - lba)
		return -2;
	if (g.ro)
		return -1;
	if (g.cut_after && g.n_writes >= g.cut_after) {
		log_add('X', lba, count);
		return 0;
	}
	log_add('D', lba, count);
	if (!g.discard_scribble)
		return 0;
	unsigned char *s = malloc(g.sector);
	if (!s)
		hbd_die("out of memory (discard)");
	memset(s, 0xdd, g.sector);
	for (unsigned long long i = 0; i < count; i++) {
		if (g.cow)
			memcpy(ov_get(lba + i)->data, s, g.sector);
		else if (pwrite(g.fd, s, g.sector,
				(off_t)((lba + i) * g.sector)) !=
			 (ssize_t)g.sector) {
			free(s);
			return -1;
		}
	}
	free(s);
	return 0;
}

void hbd_set_trap(int on)
{
	g.trap = on;
}

int hbd_get_trap(void)
{
	return g.trap;
}

void hbd_set_log(int on)
{
	g.log_on = on;
}

void hbd_log_clear(void)
{
	g.log_n = 0;
}

void hbd_set_scribble(int on)
{
	g.discard_scribble = on;
}

/* kind: 'r' fail the Nth read from now, 'w' the Nth write from now,
 * 'c' drop every write after the Nth from now, 'R' every read fails
 * (n != 0) or none (n == 0); n == 0 switches the fault off. */
void hbd_fault(char kind, unsigned long long n)
{
	switch (kind) {
	case 'r':
		g.fail_read_at = n ? g.n_reads + n : 0;
		break;
	case 'w':
		g.fail_write_at = n ? g.n_writes + n : 0;
		break;
	case 'c':
		g.cut_after = n ? g.n_writes + n : 0;
		g.cut_active = 0;
		break;
	case 'R':
		g.fail_reads_from_off = n != 0;
		break;
	}
}

int hbd_cut_active(void)
{
	return g.cut_active;
}

void hbd_counts(unsigned long long *reads, unsigned long long *writes,
		unsigned long long *discards, unsigned long long *flushes,
		unsigned long long *wsect, unsigned long long *dropped)
{
	if (reads)
		*reads = g.n_reads;
	if (writes)
		*writes = g.n_writes;
	if (discards)
		*discards = g.n_discards;
	if (flushes)
		*flushes = g.n_flushes;
	if (wsect)
		*wsect = g.w_sectors;
	if (dropped)
		*dropped = g.dropped;
}

/* "W lba count" lines, one per logged request, in order. */
int hbd_log_dump(const char *path)
{
	FILE *f = (path && path[0] && strcmp(path, "-")) ? fopen(path, "w") :
							   stdout;
	if (!f)
		return -1;
	for (unsigned long i = 0; i < g.log_n; i++)
		fprintf(f, "%llu %c %llu %llu\n", g.log[i].seq, g.log[i].op,
			g.log[i].lba, g.log[i].count);
	if (f != stdout)
		fclose(f);
	else
		fflush(stdout);
	return 0;
}

/* Index of the first log entry with op `op' whose range overlaps
 * [lba, lba+count) at or after entry `from', or -1. */
long hbd_log_find(char op, unsigned long long lba, unsigned long long count,
		  long from)
{
	for (unsigned long i = from < 0 ? 0 : (unsigned long)from; i < g.log_n;
	     i++) {
		if (op && g.log[i].op != op)
			continue;
		if (g.log[i].op == 'F') /* a flush covers the whole device */
			return (long)i;
		if (g.log[i].lba < lba + count && lba < g.log[i].lba + g.log[i].count)
			return (long)i;
	}
	return -1;
}

unsigned long hbd_log_len(void)
{
	return g.log_n;
}

/* Patch file: lines "OFFSET HEXBYTES" (offset decimal or 0x hex), '#'
 * comments.  Later patches of the same byte win. */
static int pcmp_idx(const void *a, const void *b)
{
	const unsigned long long *x = a, *y = b;
	return x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : (x[1] < y[1] ? -1 : x[1] > y[1]);
}

int hbd_load_patches(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[8192];
	unsigned long long *tmp; /* (offset, order, value) triples */
	unsigned long n = 0, cap = 0;

	if (!f)
		return -1;
	tmp = NULL;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *end;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || !*p)
			continue;
		unsigned long long off = strtoull(p, &end, 0);
		if (end == p)
			continue;
		p = end;
		while (*p == ' ' || *p == '\t')
			p++;
		for (; isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]); p += 2) {
			char hx[3] = { p[0], p[1], 0 };
			if (n == cap) {
				cap = cap ? cap * 2 : 1024;
				tmp = realloc(tmp, cap * 3 * sizeof(*tmp));
				if (!tmp)
					hbd_die("out of memory (patches)");
			}
			tmp[n * 3] = off++;
			tmp[n * 3 + 1] = n;
			tmp[n * 3 + 2] = strtoul(hx, NULL, 16);
			n++;
		}
	}
	fclose(f);
	if (n) /* an empty patch file: nothing to sort (tmp is NULL) */
		qsort(tmp, n, 3 * sizeof(*tmp), pcmp_idx);
	free(g.p_off);
	free(g.p_val);
	g.p_off = malloc((n ? n : 1) * sizeof(*g.p_off));
	g.p_val = malloc(n ? n : 1);
	g.p_n = 0;
	for (unsigned long i = 0; i < n; i++) {
		if (g.p_n && g.p_off[g.p_n - 1] == tmp[i * 3]) {
			g.p_val[g.p_n - 1] = (unsigned char)tmp[i * 3 + 2];
			continue;
		}
		g.p_off[g.p_n] = tmp[i * 3];
		g.p_val[g.p_n] = (unsigned char)tmp[i * 3 + 2];
		g.p_n++;
	}
	free(tmp);
	return (int)g.p_n;
}

/* Save the device as it reads now (overlay and patches applied). */
int hbd_save(const char *path)
{
	FILE *f = fopen(path, "w");
	unsigned char *buf = malloc(1u << 20);
	unsigned long long per = (1u << 20) / g.sector;

	if (!f || !buf) {
		if (f)
			fclose(f);
		free(buf);
		return -1;
	}
	for (unsigned long long l = 0; l < g.sectors; l += per) {
		unsigned long long n = g.sectors - l < per ? g.sectors - l : per;
		unsigned long long sr = g.n_reads;
		int fr = g.fail_reads_from_off;
		unsigned long long fa = g.fail_read_at;
		g.fail_reads_from_off = 0;
		g.fail_read_at = 0;
		hbd_read(l, n, buf);
		g.n_reads = sr;
		g.fail_reads_from_off = fr;
		g.fail_read_at = fa;
		fwrite(buf, 1, (size_t)(n * g.sector), f);
	}
	free(buf);
	return fclose(f);
}

/* ---- host helpers for the runner ---- */

/* Whole file into a malloc'd buffer; NULL on error. */
void *host_read_file(const char *path, unsigned long *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b = NULL;
	unsigned long n = 0, cap = 0;
	size_t r;

	if (!f)
		return NULL;
	for (;;) {
		if (n == cap) {
			cap = cap ? cap * 2 : 65536;
			unsigned char *nb = realloc(b, cap);
			if (!nb) {
				free(b);
				fclose(f);
				return NULL;
			}
			b = nb;
		}
		r = fread(b + n, 1, cap - n, f);
		if (r == 0)
			break;
		n += r;
	}
	fclose(f);
	*len = n;
	return b ? b : malloc(1);
}

int host_write_file(const char *path, const void *data, unsigned long len)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return -1;
	if (len && fwrite(data, 1, len, f) != len) {
		fclose(f);
		return -1;
	}
	return fclose(f);
}

void host_flush_out(void)
{
	fflush(stdout);
	fflush(stderr);
}

void host_print_stack(void)
{
	__sanitizer_print_stack_trace();
}

/* ---- the C library, for the kernel-side units ---- */
void *h_malloc(unsigned long n)
{
	return malloc(n);
}

void *h_calloc(unsigned long n, unsigned long m)
{
	return calloc(n, m);
}

void *h_realloc(void *p, unsigned long n)
{
	return realloc(p, n);
}

void *h_aligned_alloc(unsigned long align, unsigned long n)
{
	return aligned_alloc(align, n);
}

void h_free(void *p)
{
	free(p);
}

void *h_memset(void *p, int c, unsigned long n)
{
	return memset(p, c, n);
}

void *h_memcpy(void *d, const void *s, unsigned long n)
{
	return memcpy(d, s, n);
}

void *h_memmove(void *d, const void *s, unsigned long n)
{
	return memmove(d, s, n);
}

int h_memcmp(const void *a, const void *b, unsigned long n)
{
	return memcmp(a, b, n);
}

unsigned long h_strlen(const char *s)
{
	return strlen(s);
}

int h_strcmp(const char *a, const char *b)
{
	return strcmp(a, b);
}

int h_strncmp(const char *a, const char *b, unsigned long n)
{
	return strncmp(a, b, n);
}

char *h_strdup(const char *s)
{
	return strdup(s);
}

unsigned long long h_strtoull(const char *s, char **end, int base)
{
	return strtoull(s, end, base);
}

long long h_strtoll(const char *s, char **end, int base)
{
	return strtoll(s, end, base);
}

int h_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}

int h_fprintf_err(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vfprintf(stderr, fmt, ap);
	va_end(ap);
	return n;
}

int h_snprintf(char *b, unsigned long n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(b, n, fmt, ap);
	va_end(ap);
	return r;
}

int h_vsnprintf(char *b, unsigned long n, const char *fmt, va_list ap)
{
	return vsnprintf(b, n, fmt, ap);
}

void h_abort(void)
{
	fflush(stdout);
	fflush(stderr);
	abort();
}

void h_exit(int code)
{
	fflush(stdout);
	fflush(stderr);
	exit(code);
}

char *h_getenv(const char *name)
{
	return getenv(name);
}

void h_qsort(void *base, unsigned long n, unsigned long size,
	     int (*cmp)(const void *, const void *))
{
	qsort(base, n, size, cmp);
}

void *h_open_script(const char *path)
{
	if (!strcmp(path, "-"))
		return stdin;
	return fopen(path, "r");
}

void h_close_script(void *fh)
{
	if (fh && fh != stdin)
		fclose(fh);
}

char *h_read_line(void *fh, char *buf, int cap)
{
	if (!fgets(buf, cap, fh))
		return NULL;
	size_t n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	return buf;
}
