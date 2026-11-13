/*
 * disktest -- block device stress and data-integrity tester
 *
 * Drives one or more block devices (SATA disks behind the AHCI driver, NVMe
 * namespaces, USB disks, their partitions) with sustained sequential and
 * random reads and
 * writes from many threads at once, and proves afterwards that every block
 * holds exactly what was last written to it.
 *
 * WHAT IT WRITES.  The target is cut into blocks of --bs bytes.  Every
 * logical sector of every block is self-describing: a 40-byte header with a
 * magic number, the run's seed, the block number, the sector index inside the
 * block, the block's generation (how many times the block has been written)
 * and a CRC-32 over the whole sector, followed by pseudo-random payload
 * derived from (seed, block, generation, sector).  So a read-back does not
 * only say "wrong": it says which block and which generation the data
 * actually belongs to -- a misdirected write (another block's data), a lost
 * write (an older generation), a torn write (sectors of two generations in
 * one block) and plain corruption (bad CRC) are told apart and reported as
 * such.
 *
 * HOW IT LOADS THE DEVICE.  N threads per target (--threads) issue
 * synchronous pread/pwrite calls, so the device sees up to N outstanding
 * requests: with N >= the NCQ depth (/sys/block/<disk>/device/queue_depth,
 * 31 or 32 on SATA) the drive's command queue is saturated.  A multi-queue
 * device (an NVMe namespace: one hardware queue per CPU or group of CPUs,
 * /sys/block/<disk>/mq/<n>) gets 32 threads per hardware queue by default
 * (at most 128), spread round-robin over the CPUs (--affinity), so every
 * submission queue of the controller carries load at once.  Several targets
 * run concurrently, each with its own threads, which loads several ports of
 * one controller (or several controllers) at the same time.  The sequential
 * phases share one block cursor per target, so the device sees an ascending
 * stream at full depth; the random phase gives each thread its own subset of
 * the blocks (block % threads == thread), so threads never race on one block
 * and the expected generation of every block is always known exactly.
 *
 * PHASES (--mode):
 *   full     write every block (generation 1), verify every block, then the
 *            random mixed phase for --time seconds (or --ops operations),
 *            then a final verify of every block.  The default.
 *   write    the sequential write phase only.
 *   verify   a sequential verify of a target written earlier with the same
 *            --seed and --bs: any generation is accepted, the block number,
 *            seed and CRC must match.
 *   random   the random phase (after an initial write of every block, which
 *            it needs to know what to expect), then a final verify.
 *   read     read-only: sequential then random reads of the whole device,
 *            each block read twice and compared.  Never writes, so this mode
 *            is allowed on ANY block device -- also the system disk.
 *   quick    full, on the first 1 GiB of each target with a 30 s random
 *            phase: about a minute, also on a slow virtual disk.
 *   fault    the error-recovery test: write and verify the first 1 GiB, then
 *            one round per fault kind (device error, device error on a
 *            queued command, host error, link down, command timeout --
 *            or only the --fault SPEC given), each round --time seconds of
 *            random load (default 15) with the fault re-armed every
 *            --fault-every seconds (default 3), followed by a verify.
 *            Errors while a fault is armed are expected and counted apart;
 *            the run fails on any data error or any other I/O error.  A
 *            "timeout" round lasts at least one command timeout (30 s).
 *
 * NVMe TARGETS.  For /dev/nvmeXnY (and its partitions) the controller is
 * found through sysfs (the namespace's device link, the subsystem's
 * controller list for a multipath head, the controller directories, the
 * name), and its model, PCI address, queue count and size are shown with
 * the block layer's hardware queue count and depth.  The read-only mode
 * runs on any namespace; the writing modes on a likeos-test* partition of
 * one, as for SATA.
 *
 * FAULT INJECTION (--fault SPEC).  For SATA targets the AHCI driver has a
 * per-port fault injection knob, /sys/class/ata_port/ataN/fault_inject:
 *     off | <timeout|dev_error|host_error|link_down> [count] [every=N]
 *           [err=0xNN] [ncq] [write]
 * disktest writes SPEC there when the random phase starts (and again every
 * --fault-every seconds), turns it off at the end of the phase, prints the
 * port's error-handling statistics before and after, and then verifies every
 * block.  An I/O error while faults are armed is expected (a device error
 * with ABRT is not retried, for one); the block it hit may then hold either
 * the old or the new generation, and both are accepted -- but nothing else
 * is.  So a fault run passes when the driver recovered (I/O works again after
 * the faults stop) and no data was damaged.
 *
 * For NVMe namespaces the NVMe driver's fault object of the namespace is
 * used: /sys/kernel/debug/<ns>/fault_inject/<attr> (or the same under
 * /sys/kernel/debug/nvme/, or the one-file /sys/block/<ns>/fault_inject),
 * for a multipath head the object of one of its paths.  SPEC is then a
 * list of key=value words for it:
 *     kind=<status|timeout|abort|reset> status=0xSSS dont_retry=<Y|N>
 *     interval=N times=N space=N probability=PCT     (or "off")
 * e.g. "kind=status status=0x281 interval=40 times=-1 probability=100".
 * The fault mode's NVMe rounds: a retried media error, the same with Do
 * Not Retry, a retried internal error, a controller reset during I/O, a
 * lost completion that ends in an Abort, and one that runs the whole
 * timeout ladder (CQ poll, Abort, controller reset).  Before and after
 * each round the controller state, the object's counters and the
 * controller's debug summary are printed.
 *
 * SAFETY.  Every mode except `read' destroys the target's contents.  A
 * target is accepted for writing only when
 *   - it is a partition whose GPT name starts with "likeos-test" (that is,
 *     it is reachable as /dev/disk/by-partlabel/likeos-test*), or
 *   - it is named, exactly, by the environment variable DISKTEST_DISK or
 *     TESTLIBC_DISK (several separated by commas or spaces),
 * and it is not mounted (checked against /proc/mounts) and, for a whole disk,
 * none of its partitions is mounted.  `disktest auto' takes every
 * likeos-test* partition there is.
 *
 * REPORTING.  Every --report seconds one line per target: read and write
 * throughput and IOPS of the interval, average, 99th percentile and
 * maximum latency, the requests in flight now and the most seen
 * (/sys/block/<disk>/inflight, sampled ten times a second), errors so far.
 * Each phase ends with its overall IOPS, throughput and latency
 * percentiles; the run ends with a summary per target, with the latency
 * distribution.
 *
 * EXIT STATUS: 0 everything verified; 1 data integrity failure (the one that
 * matters); 2 unexpected I/O errors but no bad data; 3 usage or setup error.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/mount.h> /* BLK* */
#include <scsi/scsi.h> /* SCSI_IOCTL_GET_IDLUN */

/* ======================================================================
 * Limits and defaults
 * ====================================================================== */

#define DT_MAX_TARGETS 16
#define DT_MAX_THREADS 128
#define DT_DEF_THREADS 32 /* one more than the usual NCQ depth of 31 */
#define DT_DEF_BS (64 * 1024)
#define DT_MAX_BS (16 * 1024 * 1024)
#define DT_DEF_TIME 60 /* seconds of random phase */
#define DT_DEF_REPORT 5 /* seconds between report lines */
#define DT_DEF_RWMIX 50 /* percent writes in the random phase */
#define DT_HDR_SIZE 40 /* sector header bytes */
#define DT_MAGIC 0x54444F4Cu /* "LODT" */
#define DT_LAT_BUCKETS 40 /* log2 microsecond buckets */
#define DT_MAX_REPORTED_BAD 32 /* detailed lines per target */

/* Exit codes */
#define DT_EXIT_OK 0
#define DT_EXIT_DATA 1
#define DT_EXIT_IO 2
#define DT_EXIT_SETUP 3

enum dt_mode {
	MODE_FULL,
	MODE_WRITE,
	MODE_VERIFY,
	MODE_RANDOM,
	MODE_READ,
	MODE_QUICK, /* full, on a 1 GiB region with a 30 s random phase */
	MODE_FAULT, /* a 1 GiB region through a series of fault rounds */
};

/* Region size and timings of the quick and fault modes (when --size,
 * --time and --fault-every do not say otherwise): small enough that a run
 * on a virtual disk, whose write rate can sink to a few MB/s for seconds at
 * a time, still ends within minutes. */
#define DT_QUICK_SIZE (1ULL << 30)
#define DT_QUICK_TIME 30
#define DT_FAULT_TIME 15 /* seconds of load per fault round */
#define DT_FAULT_EVERY 3 /* re-arm the fault every few seconds */

/* The rounds of --mode fault without --fault: every kind of error the
 * AHCI fault knob can inject, each followed by a verify of the region. */
static const char *const g_fault_rounds[] = {
	"dev_error every=40",
	"dev_error every=40 ncq",
	"host_error 1",
	"link_down 1",
	"timeout 1",
};

/* The same for NVMe namespaces, in the grammar of the NVMe fault object
 * (kind, status in the driver's form SCT << 8 | SC, dont_retry, interval,
 * times, probability -- probability last, so the object goes live with
 * the rest in place):
 *   - a media error (Unrecovered Read Error) on every 40th command that
 *     the driver retries: no error should reach disktest;
 *   - the same with Do Not Retry: the errors reach disktest (expected);
 *   - an Internal Error, retried;
 *   - a controller reset while I/O is in flight;
 *   - a lost completion on an I/O queue: timeout, then Abort;
 *   - a lost completion: the full timeout ladder (CQ poll, Abort, reset).
 */
static const char *const g_fault_rounds_nvme[] = {
	"kind=status status=0x281 dont_retry=N interval=40 times=-1 probability=100",
	"kind=status status=0x281 dont_retry=Y interval=40 times=-1 probability=100",
	"kind=status status=0x006 dont_retry=N interval=40 times=-1 probability=100",
	"kind=reset times=1 probability=100",
	"kind=abort times=1 probability=100",
	"kind=timeout times=1 probability=100",
};

/* The kind of a phase a worker runs. */
enum dt_phase {
	PH_SEQ_WRITE,
	PH_SEQ_VERIFY,
	PH_RANDOM,
	PH_SEQ_READ,
	PH_RANDOM_READ,
};

static const char *phase_name(enum dt_phase p)
{
	switch (p) {
	case PH_SEQ_WRITE:
		return "sequential write";
	case PH_SEQ_VERIFY:
		return "sequential verify";
	case PH_RANDOM:
		return "random read/write";
	case PH_SEQ_READ:
		return "sequential read";
	case PH_RANDOM_READ:
		return "random read";
	}
	return "?";
}

/* ======================================================================
 * Options
 * ====================================================================== */

static struct {
	enum dt_mode mode;
	int threads;
	uint32_t bs;
	uint64_t offset; /* bytes into each target */
	uint64_t size; /* bytes of each target to use, 0 = all */
	unsigned time_s;
	uint64_t ops; /* random-phase operations per target, 0 = by time */
	unsigned report_s;
	unsigned rwmix; /* percent writes */
	uint32_t seed;
	unsigned loops; /* random+verify rounds */
	unsigned flush_every; /* fdatasync every n writes per thread, 0 off */
	int verify_after_write; /* read back each random write at once */
	const char *fault; /* fault_inject spec */
	unsigned fault_every_s; /* re-arm interval */
	int quiet;
	int stop_on_error;
	int threads_set; /* -j given */
	int affinity; /* spread the threads over the CPUs (multi-queue) */
} opt = {
	.mode = MODE_FULL,
	.threads = DT_DEF_THREADS,
	.bs = DT_DEF_BS,
	.time_s = DT_DEF_TIME,
	.report_s = DT_DEF_REPORT,
	.rwmix = DT_DEF_RWMIX,
	.loops = 1,
	.affinity = 1,
};

/* Set by SIGINT/SIGTERM: finish the current phase early, still verify. */
static volatile sig_atomic_t g_interrupted;

/* ======================================================================
 * Small helpers
 * ====================================================================== */

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void *xmalloc_aligned(size_t align, size_t size)
{
	void *p = NULL;

	if (posix_memalign(&p, align, size) != 0 || !p) {
		fprintf(stderr, "disktest: out of memory (%zu bytes)\n", size);
		exit(DT_EXIT_SETUP);
	}
	return p;
}

static void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n, size);

	if (!p) {
		fprintf(stderr, "disktest: out of memory (%zu x %zu bytes)\n",
			n, size);
		exit(DT_EXIT_SETUP);
	}
	return p;
}

/* Read a small sysfs/procfs text file into buf (NUL-terminated, trailing
 * newline removed).  0 or -1. */
static int read_text(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (cap == 0)
		return -1;
	buf[0] = 0;
	if (fd < 0)
		return -1;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return 0;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;
	size_t len = strlen(text);

	if (fd < 0)
		return -1;
	n = write(fd, text, len);
	close(fd);
	return n == (ssize_t)len ? 0 : -1;
}

/* Human-readable size. */
static const char *fmt_bytes(uint64_t b, char *buf, size_t cap)
{
	static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double v = (double)b;
	int u = 0;

	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		snprintf(buf, cap, "%llu B", (unsigned long long)b);
	else
		snprintf(buf, cap, "%.2f %s", v, unit[u]);
	return buf;
}

/* Parse a size with an optional k/m/g/t suffix (powers of 1024). */
static int parse_size(const char *s, uint64_t *out)
{
	char *end;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || end == s)
		return -1;
	switch (*end) {
	case 0:
		break;
	case 'k':
	case 'K':
		v <<= 10;
		end++;
		break;
	case 'm':
	case 'M':
		v <<= 20;
		end++;
		break;
	case 'g':
	case 'G':
		v <<= 30;
		end++;
		break;
	case 't':
	case 'T':
		v <<= 40;
		end++;
		break;
	default:
		return -1;
	}
	if (*end == 'i' || *end == 'b' || *end == 'B')
		end++;
	if (*end)
		return -1;
	*out = v;
	return 0;
}

/* ======================================================================
 * CRC-32 (IEEE 802.3, reflected, as zlib computes it) and the generator
 * ====================================================================== */

static uint32_t crc_table[256];

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static uint32_t crc32_buf(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;

	crc = ~crc;
	while (len--)
		crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

/* splitmix64: seeds the per-sector payload generator and the per-thread
 * random streams.  Good avalanche, so neighbouring blocks get unrelated
 * payloads (a block shifted by one sector does not look "almost right"). */
static uint64_t splitmix64(uint64_t *state)
{
	uint64_t z = (*state += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* xorshift64*: the fast payload/random generator. */
static inline uint64_t xs64(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 0x2545F4914F6CDD1Dull;
}

/* ======================================================================
 * The on-disk pattern
 * ====================================================================== */

/* Header at the start of every logical sector.  Little-endian, packed by
 * construction (all naturally aligned). */
struct dt_hdr {
	uint32_t magic; /* DT_MAGIC */
	uint32_t seed; /* the run's --seed */
	uint64_t block; /* block number within the target region */
	uint32_t gen; /* the block's write generation, >= 1 */
	uint32_t sector; /* sector index inside the block */
	uint32_t bs; /* block size of the run */
	uint32_t ssz; /* logical sector size of the run */
	uint32_t pad; /* 0 */
	uint32_t crc; /* CRC-32 of the sector with this field 0 */
};

_Static_assert(sizeof(struct dt_hdr) == DT_HDR_SIZE, "dt_hdr layout");

static void fill_sector(uint8_t *sec, uint32_t ssz, uint32_t seed,
			uint64_t block, uint32_t gen, uint32_t idx, uint32_t bs)
{
	struct dt_hdr h;
	uint64_t st = ((uint64_t)seed << 32) ^ (block * 0x9E3779B97F4A7C15ull) ^
		      ((uint64_t)gen << 17) ^ idx;
	uint64_t s = splitmix64(&st) | 1;
	uint32_t i;

	memset(&h, 0, sizeof(h));
	h.magic = DT_MAGIC;
	h.seed = seed;
	h.block = block;
	h.gen = gen;
	h.sector = idx;
	h.bs = bs;
	h.ssz = ssz;
	memcpy(sec, &h, sizeof(h));
	for (i = DT_HDR_SIZE; i + 8 <= ssz; i += 8) {
		uint64_t v = xs64(&s);

		memcpy(sec + i, &v, 8);
	}
	for (; i < ssz; i++)
		sec[i] = (uint8_t)xs64(&s);
	h.crc = crc32_buf(0, sec, ssz);
	memcpy(sec + offsetof(struct dt_hdr, crc), &h.crc, sizeof(h.crc));
}

static void fill_block(uint8_t *buf, uint32_t bs, uint32_t ssz, uint32_t seed,
		       uint64_t block, uint32_t gen)
{
	for (uint32_t i = 0; i < bs / ssz; i++)
		fill_sector(buf + (size_t)i * ssz, ssz, seed, block, gen, i,
			    bs);
}

/* What one sector turned out to be. */
enum dt_sec_state {
	SEC_OK, /* the expected block, a valid generation, CRC good */
	SEC_ZERO, /* all zeroes (never written / discarded) */
	SEC_BAD_MAGIC, /* not our pattern at all */
	SEC_BAD_CRC, /* our header, corrupted content */
	SEC_WRONG_SEED, /* written by another run */
	SEC_WRONG_BLOCK, /* misdirected: another block's data */
	SEC_WRONG_SECTOR, /* shifted inside the block */
	SEC_WRONG_GEN, /* lost or phantom write */
};

static const char *sec_state_name(enum dt_sec_state s)
{
	switch (s) {
	case SEC_OK:
		return "ok";
	case SEC_ZERO:
		return "all zeroes";
	case SEC_BAD_MAGIC:
		return "foreign data (no pattern header)";
	case SEC_BAD_CRC:
		return "corrupted (CRC mismatch)";
	case SEC_WRONG_SEED:
		return "data of another run (seed differs)";
	case SEC_WRONG_BLOCK:
		return "misdirected (another block's data)";
	case SEC_WRONG_SECTOR:
		return "sector shifted inside its block";
	case SEC_WRONG_GEN:
		return "wrong generation (lost or stale write)";
	}
	return "?";
}

static int all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

/* Check one sector.  gen_a and gen_b are the acceptable generations (equal
 * when only one is); gen_a == 0 accepts any generation (verify mode).  The
 * header found is returned through *found. */
static enum dt_sec_state check_sector(const uint8_t *sec, uint32_t ssz,
				      uint32_t seed, uint64_t block,
				      uint32_t idx, uint32_t gen_a,
				      uint32_t gen_b, struct dt_hdr *found)
{
	struct dt_hdr h;
	uint8_t tmp[DT_HDR_SIZE];
	uint32_t crc;

	memcpy(&h, sec, sizeof(h));
	*found = h;
	if (h.magic != DT_MAGIC)
		return all_zero(sec, ssz) ? SEC_ZERO : SEC_BAD_MAGIC;
	/* CRC over the sector with the crc field taken as zero. */
	memcpy(tmp, sec, DT_HDR_SIZE);
	memset(tmp + offsetof(struct dt_hdr, crc), 0, sizeof(uint32_t));
	crc = crc32_buf(0, tmp, DT_HDR_SIZE);
	crc = crc32_buf(crc, sec + DT_HDR_SIZE, ssz - DT_HDR_SIZE);
	if (crc != h.crc)
		return SEC_BAD_CRC;
	if (h.seed != seed)
		return SEC_WRONG_SEED;
	if (h.block != block)
		return SEC_WRONG_BLOCK;
	if (h.sector != idx)
		return SEC_WRONG_SECTOR;
	if (gen_a != 0 && h.gen != gen_a && h.gen != gen_b)
		return SEC_WRONG_GEN;
	return SEC_OK;
}

/* ======================================================================
 * Statistics
 * ====================================================================== */

struct dt_stats {
	uint64_t rd_ops, wr_ops, flush_ops;
	uint64_t rd_bytes, wr_bytes;
	uint64_t lat_sum_ns, lat_max_ns, lat_n;
	uint64_t lat_hist[DT_LAT_BUCKETS];
	uint64_t io_errors; /* outside fault windows */
	uint64_t fault_errors; /* while fault injection was armed */
	uint64_t bad_blocks; /* data integrity failures */
	uint64_t bad_by_state[8];
	uint64_t short_io;
};

static void stats_add(struct dt_stats *dst, const struct dt_stats *src)
{
	dst->rd_ops += src->rd_ops;
	dst->wr_ops += src->wr_ops;
	dst->flush_ops += src->flush_ops;
	dst->rd_bytes += src->rd_bytes;
	dst->wr_bytes += src->wr_bytes;
	dst->lat_sum_ns += src->lat_sum_ns;
	dst->lat_n += src->lat_n;
	if (src->lat_max_ns > dst->lat_max_ns)
		dst->lat_max_ns = src->lat_max_ns;
	for (int i = 0; i < DT_LAT_BUCKETS; i++)
		dst->lat_hist[i] += src->lat_hist[i];
	dst->io_errors += src->io_errors;
	dst->fault_errors += src->fault_errors;
	dst->bad_blocks += src->bad_blocks;
	for (int i = 0; i < 8; i++)
		dst->bad_by_state[i] += src->bad_by_state[i];
	dst->short_io += src->short_io;
}

static int lat_bucket(uint64_t ns)
{
	uint64_t us = ns / 1000;
	int b = 0;

	while (us > 1 && b < DT_LAT_BUCKETS - 1) {
		us >>= 1;
		b++;
	}
	return b;
}

/* The latency below which `pct' percent of the operations completed (upper
 * bucket bound, microseconds). */
static uint64_t lat_percentile_us(const struct dt_stats *s, double pct)
{
	uint64_t total = 0, acc = 0;

	for (int i = 0; i < DT_LAT_BUCKETS; i++)
		total += s->lat_hist[i];
	if (!total)
		return 0;
	for (int i = 0; i < DT_LAT_BUCKETS; i++) {
		acc += s->lat_hist[i];
		if ((double)acc * 100.0 >= pct * (double)total)
			return 1ull << (i + 1);
	}
	return 1ull << DT_LAT_BUCKETS;
}

/* The same for the operations between two snapshots (`prev' taken first). */
static uint64_t lat_percentile_delta_us(const struct dt_stats *now,
					const struct dt_stats *prev, double pct)
{
	struct dt_stats d;

	memset(&d, 0, sizeof(d));
	for (int i = 0; i < DT_LAT_BUCKETS; i++)
		d.lat_hist[i] = now->lat_hist[i] - prev->lat_hist[i];
	return lat_percentile_us(&d, pct);
}

/* ======================================================================
 * Targets
 * ====================================================================== */

struct dt_target;

struct dt_worker {
	struct dt_target *t;
	int tid;
	pthread_t thread;
	uint8_t *buf; /* bs bytes, aligned */
	uint8_t *buf2; /* second buffer (read compare, verify-after-write) */
	uint64_t rnd;
	/* Stats: written only by the worker; the reporter reads them racily
	 * through __atomic loads, the final summary after join. */
	struct dt_stats st;
	int failed; /* stop_on_error tripped */
	int cpu; /* pinned to this CPU, -1 = anywhere */
};

struct dt_target {
	char path[PATH_MAX]; /* as given */
	char real[PATH_MAX]; /* resolved node */
	char name[32]; /* "sdb1" */
	char disk[32]; /* "sdb" */
	char ata_port[16]; /* "ata3" or "" */
	int fd;
	/* NVMe namespaces (is_nvme): the controller serving the namespace,
	 * found through sysfs, and the namespace's fault injection object. */
	int is_nvme;
	char nvme_ctrl[32]; /* "nvme0" */
	char nvme_model[64];
	char nvme_addr[32]; /* PCI address of the controller */
	unsigned nvme_queue_count; /* I/O queues + admin (controller attr) */
	unsigned nvme_sqsize; /* 0-based queue size (controller attr) */
	unsigned nr_hw_queues; /* /sys/block/<disk>/mq/<n> directories */
	unsigned nr_requests; /* /sys/block/<disk>/queue/nr_requests */
	char fi_path[PATH_MAX]; /* fault_inject directory or file, "" none */
	int fi_onefile; /* fi_path is the one-file form */
	int is_partition;
	uint32_t lsz; /* logical sector size */
	uint32_t psz; /* physical sector size */
	uint64_t dev_bytes; /* whole device size */
	uint64_t base; /* byte offset of the region */
	uint64_t nblocks; /* blocks of opt.bs in the region */
	unsigned queue_depth; /* /sys/block/<disk>/device/queue_depth */

	/* Expected state of every block: gen[] is what the last successful
	 * write put there; alt[] is a second acceptable generation when a
	 * write failed with an error (old or new data may be on the media). */
	uint32_t *gen;
	uint32_t *alt;

	/* Shared sequential cursor */
	uint64_t next_block;

	/* Phase control */
	enum dt_phase phase;
	uint64_t phase_deadline_ns; /* 0 = none */
	uint64_t ops_per_thread; /* random phase, 0 = by time */
	volatile int faults_armed;
	int nworkers;
	struct dt_worker *w;

	/* Reporting */
	unsigned inflight_max;
	struct dt_stats last; /* at the previous report line */
	uint64_t last_ns;
	unsigned bad_reported;
	pthread_mutex_t report_lock;
	struct dt_stats total; /* summed over all phases */
	unsigned inflight_now; /* last sample */
	struct dt_stats phase_base; /* at the start of the phase */
	const char *fault; /* fault spec of the current random phase, or NULL */
};

static struct dt_target g_t[DT_MAX_TARGETS];
static int g_nt;

/* The kernel names a partition "<disk><n>", or "<disk>p<n>" when the disk
 * name ends in a digit.  Find the disk via sysfs first (the class link of a
 * partition points into its disk's directory), then by the name rule. */
static void derive_disk_name(struct dt_target *t)
{
	char p[PATH_MAX], link[PATH_MAX], val[64];
	ssize_t n;

	snprintf(p, sizeof(p), "/sys/class/block/%s/partition", t->name);
	t->is_partition = (read_text(p, val, sizeof(val)) == 0);
	if (!t->is_partition) {
		snprintf(t->disk, sizeof(t->disk), "%s", t->name);
		return;
	}
	snprintf(p, sizeof(p), "/sys/class/block/%s", t->name);
	n = readlink(p, link, sizeof(link) - 1);
	if (n > 0) {
		char *slash, *prev;

		link[n] = 0;
		slash = strrchr(link, '/');
		if (slash) {
			*slash = 0;
			prev = strrchr(link, '/');
			snprintf(t->disk, sizeof(t->disk), "%s",
				 prev ? prev + 1 : link);
			if (t->disk[0])
				return;
		}
	}
	/* By the naming rule. */
	snprintf(t->disk, sizeof(t->disk), "%s", t->name);
	n = (ssize_t)strlen(t->disk);
	while (n > 0 && t->disk[n - 1] >= '0' && t->disk[n - 1] <= '9')
		t->disk[--n] = 0;
	if (n > 1 && t->disk[n - 1] == 'p' && t->disk[n - 2] >= '0' &&
	    t->disk[n - 2] <= '9')
		t->disk[n - 1] = 0;
}

/* The name of the whole disk a block device number belongs to, from
 * sysfs: /sys/dev/block/MAJ:MIN links to ../../block/<disk> or
 * ../../block/<disk>/<part>.  Works for every disk kind (sd disks with 16
 * minors each, NVMe namespaces with dynamic minors on the extended
 * major).  0 when found. */
static int disk_name_of_devt(dev_t dev, char *out, size_t cap)
{
	char p[64], link[PATH_MAX], *b, *slash;
	ssize_t n;

	snprintf(p, sizeof(p), "/sys/dev/block/%u:%u", major(dev), minor(dev));
	n = readlink(p, link, sizeof(link) - 1);
	if (n <= 0)
		return -1;
	link[n] = 0;
	b = strstr(link, "block/");
	if (!b)
		return -1;
	b += 6;
	slash = strchr(b, '/');
	if (slash)
		*slash = 0;
	snprintf(out, cap, "%s", b);
	return 0;
}

/* Is the device (or, for a whole disk, any of its partitions) mounted?  The
 * mount table names devices by their node path; compare by device number so
 * /dev/root-style aliases count too. */
static int target_mounted(const struct dt_target *t, char *what, size_t cap)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[512];
	struct stat tst;
	int hit = 0;

	if (stat(t->real, &tst) != 0)
		return 0;
	if (!f)
		return 0;
	while (!hit && fgets(line, sizeof(line), f)) {
		char dev[256];
		struct stat st;

		if (sscanf(line, "%255s", dev) != 1 || dev[0] != '/')
			continue;
		if (stat(dev, &st) != 0 || !S_ISBLK(st.st_mode))
			continue;
		if (st.st_rdev == tst.st_rdev) {
			hit = 1;
		} else if (!t->is_partition) {
			/* A partition of this disk?  Its sysfs entry lives in
			 * the disk's directory. */
			char dn[64];

			if (disk_name_of_devt(st.st_rdev, dn, sizeof(dn)) == 0 &&
			    strcmp(dn, t->disk) == 0)
				hit = 1;
		}
		if (hit)
			snprintf(what, cap, "%s", dev);
	}
	fclose(f);
	return hit;
}

/* Is `real' one of the devices named in DISKTEST_DISK / TESTLIBC_DISK? */
static int named_by_env(const char *real)
{
	static const char *vars[] = { "DISKTEST_DISK", "TESTLIBC_DISK" };

	for (unsigned v = 0; v < 2; v++) {
		const char *e = getenv(vars[v]);
		char list[1024], *tok, *save = NULL;

		if (!e || !*e)
			continue;
		snprintf(list, sizeof(list), "%s", e);
		for (tok = strtok_r(list, ", ", &save); tok;
		     tok = strtok_r(NULL, ", ", &save)) {
			char r[PATH_MAX];

			if (realpath(tok, r) && strcmp(r, real) == 0)
				return 1;
		}
	}
	return 0;
}

/* Is `real' a partition labelled likeos-test*?  Walks the by-partlabel
 * links. */
static int labelled_test(const char *real)
{
	DIR *d = opendir("/dev/disk/by-partlabel");
	struct dirent *de;
	int hit = 0;

	if (!d)
		return 0;
	while (!hit && (de = readdir(d)) != NULL) {
		char p[PATH_MAX], r[PATH_MAX];

		if (strncmp(de->d_name, "likeos-test", 11) != 0)
			continue;
		snprintf(p, sizeof(p), "/dev/disk/by-partlabel/%s",
			 de->d_name);
		if (realpath(p, r) && strcmp(r, real) == 0)
			hit = 1;
	}
	closedir(d);
	return hit;
}

/* `disktest auto': every likeos-test* partition. */
static int add_auto_targets(char paths[][PATH_MAX], int max)
{
	DIR *d = opendir("/dev/disk/by-partlabel");
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while (n < max && (de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "likeos-test", 11) != 0)
			continue;
		snprintf(paths[n], PATH_MAX, "/dev/disk/by-partlabel/%s",
			 de->d_name);
		n++;
	}
	closedir(d);
	return n;
}

/* ======================================================================
 * NVMe namespaces
 * ====================================================================== */

/* "nvme<digits>" and nothing else: a controller's name. */
static int is_ctrl_name(const char *s)
{
	if (strncmp(s, "nvme", 4) != 0 || !s[4])
		return 0;
	for (s += 4; *s; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}

static int ctrl_is_live(const char *ctrl)
{
	char p[PATH_MAX], v[32];

	snprintf(p, sizeof(p), "/sys/class/nvme/%s/state", ctrl);
	return read_text(p, v, sizeof(v)) == 0 && strncmp(v, "live", 4) == 0;
}

/* The controller that serves namespace disk `disk', from sysfs.  The
 * namespace's device link names the controller -- or, for a multipath
 * head, the subsystem, whose directory lists its controllers (the first
 * live one is taken).  Failing that, the controller whose directory holds
 * the namespace, and last the naming rule nvme<ctrl>n<ns>.  0 when found. */
static int nvme_find_ctrl(const char *disk, char *out, size_t cap)
{
	char p[PATH_MAX], link[PATH_MAX], first[32] = "";
	struct dirent *de;
	unsigned a, b;
	ssize_t n;
	DIR *d;

	snprintf(p, sizeof(p), "/sys/block/%s/device", disk);
	n = readlink(p, link, sizeof(link) - 1);
	if (n > 0) {
		const char *base;

		link[n] = 0;
		base = strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
		if (is_ctrl_name(base)) {
			snprintf(out, cap, "%s", base);
			return 0;
		}
		if (strncmp(base, "nvme-subsys", 11) == 0) {
			snprintf(p, sizeof(p), "/sys/class/nvme-subsystem/%s", base);
			d = opendir(p);
			if (d) {
				while ((de = readdir(d)) != NULL) {
					if (!is_ctrl_name(de->d_name))
						continue;
					if (!first[0])
						snprintf(first, sizeof(first), "%s",
							 de->d_name);
					if (ctrl_is_live(de->d_name)) {
						snprintf(first, sizeof(first), "%s",
							 de->d_name);
						break;
					}
				}
				closedir(d);
				if (first[0]) {
					snprintf(out, cap, "%s", first);
					return 0;
				}
			}
		}
	}
	d = opendir("/sys/class/nvme");
	if (d) {
		while ((de = readdir(d)) != NULL) {
			struct stat st;

			if (!is_ctrl_name(de->d_name))
				continue;
			snprintf(p, sizeof(p), "/sys/class/nvme/%s/%s", de->d_name,
				 disk);
			if (stat(p, &st) == 0) {
				snprintf(out, cap, "%s", de->d_name);
				closedir(d);
				return 0;
			}
		}
		closedir(d);
	}
	if (sscanf(disk, "nvme%un%u", &a, &b) == 2) {
		snprintf(p, sizeof(p), "/sys/class/nvme/nvme%u", a);
		if (access(p, F_OK) == 0) {
			snprintf(out, cap, "nvme%u", a);
			return 0;
		}
	}
	return -1;
}

/* The fault injection object of the namespace: the per-attribute directory
 * (/sys/kernel/debug/<name>/fault_inject/, or under debug/nvme/), else a
 * one-file attribute (/sys/block/<name>/fault_inject).  <name> is the
 * namespace disk, or for a multipath head one of its paths (listed in
 * /sys/block/<head>/multipath/) -- a fault on one path is what makes the
 * head fail over.  0 when found. */
static int nvme_find_fault(struct dt_target *t)
{
	char names[9][64], p[PATH_MAX];
	int nn = 0;
	DIR *d;

	snprintf(names[nn++], sizeof(names[0]), "%s", t->disk);
	snprintf(p, sizeof(p), "/sys/block/%s/multipath", t->disk);
	d = opendir(p);
	if (d) {
		struct dirent *de;

		while (nn < 9 && (de = readdir(d)) != NULL)
			if (strncmp(de->d_name, "nvme", 4) == 0)
				snprintf(names[nn++], sizeof(names[0]), "%s",
					 de->d_name);
		closedir(d);
	}
	for (int i = 0; i < nn; i++) {
		static const char *const dirs[] = {
			"/sys/kernel/debug/%s/fault_inject",
			"/sys/kernel/debug/nvme/%s/fault_inject",
		};

		for (unsigned k = 0; k < sizeof(dirs) / sizeof(dirs[0]); k++) {
			char q[PATH_MAX];

			snprintf(p, sizeof(p), dirs[k], names[i]);
			snprintf(q, sizeof(q), "%s/probability", p);
			if (access(q, F_OK) == 0) {
				snprintf(t->fi_path, sizeof(t->fi_path), "%s", p);
				t->fi_onefile = 0;
				return 0;
			}
		}
		snprintf(p, sizeof(p), "/sys/block/%s/fault_inject", names[i]);
		if (access(p, F_OK) == 0) {
			snprintf(t->fi_path, sizeof(t->fi_path), "%s", p);
			t->fi_onefile = 1;
			return 0;
		}
	}
	return -1;
}

/* Is the target on an NVMe namespace?  Then fill in its controller, the
 * controller's and the queue's properties and the fault object. */
static void nvme_target_init(struct dt_target *t)
{
	char p[PATH_MAX], v[128];
	DIR *d;

	if (strncmp(t->disk, "nvme", 4) != 0)
		return;
	if (nvme_find_ctrl(t->disk, t->nvme_ctrl, sizeof(t->nvme_ctrl)) != 0)
		return;
	t->is_nvme = 1;

	snprintf(p, sizeof(p), "/sys/class/nvme/%s/model", t->nvme_ctrl);
	if (read_text(p, v, sizeof(v)) == 0)
		snprintf(t->nvme_model, sizeof(t->nvme_model), "%s", v);
	snprintf(p, sizeof(p), "/sys/class/nvme/%s/address", t->nvme_ctrl);
	if (read_text(p, v, sizeof(v)) == 0)
		snprintf(t->nvme_addr, sizeof(t->nvme_addr), "%s", v);
	snprintf(p, sizeof(p), "/sys/class/nvme/%s/queue_count", t->nvme_ctrl);
	if (read_text(p, v, sizeof(v)) == 0)
		t->nvme_queue_count = (unsigned)strtoul(v, NULL, 10);
	snprintf(p, sizeof(p), "/sys/class/nvme/%s/sqsize", t->nvme_ctrl);
	if (read_text(p, v, sizeof(v)) == 0)
		t->nvme_sqsize = (unsigned)strtoul(v, NULL, 10);

	/* The block layer's view: one mq/<n> directory per hardware queue. */
	snprintf(p, sizeof(p), "/sys/block/%s/mq", t->disk);
	d = opendir(p);
	if (d) {
		struct dirent *de;

		while ((de = readdir(d)) != NULL)
			if (de->d_name[0] >= '0' && de->d_name[0] <= '9')
				t->nr_hw_queues++;
		closedir(d);
	}
	nvme_find_fault(t);
}

/* Write one attribute of a per-attribute fault directory. */
static int nvme_fi_write_attr(const struct dt_target *t, const char *key,
			      const char *val)
{
	char p[PATH_MAX];

	snprintf(p, sizeof(p), "%s/%s", t->fi_path, key);
	return write_text(p, val);
}

/*
 * Apply an NVMe fault spec: "off" (probability 0), "clear" (the defaults
 * back) or whitespace/comma separated key=value words with the keys of the
 * fault object -- kind (status|timeout|abort|reset), probability, interval,
 * times, space, status, dont_retry, verbose.  The one-file attribute takes
 * the spec as it is; the per-attribute directory gets each word written to
 * its file, in the order given (so a spec names probability last: the
 * object goes live with every other value already in place).
 */
static int nvme_fi_apply(const struct dt_target *t, const char *spec)
{
	char buf[512], *tok, *save = NULL;

	if (t->fi_onefile)
		return write_text(t->fi_path, spec);
	snprintf(buf, sizeof(buf), "%s", spec);
	for (tok = strtok_r(buf, " \t,", &save); tok;
	     tok = strtok_r(NULL, " \t,", &save)) {
		char *eq = strchr(tok, '=');

		if (!strcmp(tok, "off")) {
			if (nvme_fi_write_attr(t, "probability", "0"))
				return -1;
			continue;
		}
		if (!strcmp(tok, "clear")) {
			/* probability first: quiet before anything else moves */
			static const char *const def[][2] = {
				{ "probability", "0" }, { "interval", "1" },
				{ "times", "1" },       { "space", "0" },
				{ "kind", "status" },   { "dont_retry", "Y" },
			};

			for (unsigned i = 0; i < sizeof(def) / sizeof(def[0]); i++)
				if (nvme_fi_write_attr(t, def[i][0], def[i][1]))
					return -1;
			continue;
		}
		if (!eq || eq == tok) {
			errno = EINVAL;
			return -1;
		}
		*eq = 0;
		if (nvme_fi_write_attr(t, tok, eq + 1))
			return -1;
	}
	return 0;
}

/* The controller state and the fault object's settings and counters. */
static void print_nvme_fault_state(const struct dt_target *t, const char *label)
{
	char p[PATH_MAX], v[2048];

	snprintf(p, sizeof(p), "/sys/class/nvme/%s/state", t->nvme_ctrl);
	if (read_text(p, v, sizeof(v)) == 0)
		msg("  %s %s state %s: %s\n", t->name, t->nvme_ctrl, label, v);
	if (!t->fi_path[0])
		return;
	if (t->fi_onefile) {
		if (read_text(t->fi_path, v, sizeof(v)) == 0)
			msg("  %s %s %s: %s\n", t->name, t->fi_path, label, v);
	} else {
		static const char *const keys[] = {
			"kind", "probability", "interval", "times", "status",
			"dont_retry", "count", "injected",
		};
		char line[512];
		size_t used = 0;

		line[0] = 0;
		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			char a[64];

			snprintf(p, sizeof(p), "%s/%s", t->fi_path, keys[i]);
			if (read_text(p, a, sizeof(a)) != 0)
				continue;
			used += (size_t)snprintf(line + used, sizeof(line) - used,
						 "%s%s=%s", used ? " " : "", keys[i], a);
			if (used >= sizeof(line))
				break;
		}
		msg("  %s %s %s: %s\n", t->name, t->fi_path, label, line);
	}
	/* The controller's debug summary (error and reset counters), when the
	 * debug tree is there. */
	snprintf(p, sizeof(p), "/sys/kernel/debug/nvme/%s/info", t->nvme_ctrl);
	if (read_text(p, v, sizeof(v)) == 0) {
		msg("  %s %s %s:\n", t->name, p, label);
		for (char *ln = strtok(v, "\n"); ln; ln = strtok(NULL, "\n"))
			msg("      %s\n", ln);
	}
}

static int target_open(struct dt_target *t, const char *path, int writable)
{
	struct stat st;
	char p[PATH_MAX], val[64];
	unsigned long long size64 = 0;
	int ssz = 0;
	unsigned int pbs = 0;
	struct scsi_idlun idlun;

	memset(t, 0, sizeof(*t));
	t->fd = -1;
	pthread_mutex_init(&t->report_lock, NULL);
	snprintf(t->path, sizeof(t->path), "%s", path);
	if (!realpath(path, t->real)) {
		fprintf(stderr, "disktest: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (stat(t->real, &st) != 0 || !S_ISBLK(st.st_mode)) {
		fprintf(stderr, "disktest: %s is not a block device\n",
			t->real);
		return -1;
	}
	snprintf(t->name, sizeof(t->name), "%s",
		 strrchr(t->real, '/') ? strrchr(t->real, '/') + 1 : t->real);
	derive_disk_name(t);

	if (writable) {
		char what[256];

		if (!labelled_test(t->real) && !named_by_env(t->real)) {
			fprintf(stderr,
				"disktest: refusing to write %s: it is neither a partition named likeos-test*\n"
				"          nor named in DISKTEST_DISK/TESTLIBC_DISK (all its data would be lost)\n",
				t->real);
			return -1;
		}
		if (target_mounted(t, what, sizeof(what))) {
			fprintf(stderr,
				"disktest: refusing to write %s: %s is mounted\n",
				t->real, what);
			return -1;
		}
	}

	t->fd = open(t->real, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (t->fd < 0) {
		fprintf(stderr, "disktest: open %s: %s\n", t->real,
			strerror(errno));
		return -1;
	}
	if (ioctl(t->fd, BLKGETSIZE64, &size64) != 0 || size64 == 0) {
		fprintf(stderr, "disktest: BLKGETSIZE64 on %s: %s\n", t->real,
			strerror(errno));
		return -1;
	}
	if (ioctl(t->fd, BLKSSZGET, &ssz) != 0 || ssz < 512 ||
	    (ssz & (ssz - 1))) {
		fprintf(stderr, "disktest: BLKSSZGET on %s: %s (%d)\n",
			t->real, strerror(errno), ssz);
		return -1;
	}
	if (ioctl(t->fd, BLKPBSZGET, &pbs) != 0 || pbs < (unsigned)ssz)
		pbs = (unsigned)ssz;
	t->dev_bytes = size64;
	t->lsz = (uint32_t)ssz;
	t->psz = pbs;

	if (opt.bs % t->lsz) {
		fprintf(stderr,
			"disktest: %s: block size %u is not a multiple of the %u-byte logical sector\n",
			t->real, opt.bs, t->lsz);
		return -1;
	}
	if (opt.offset % opt.bs || opt.offset >= size64) {
		fprintf(stderr,
			"disktest: %s: --offset must be a multiple of --bs and inside the device\n",
			t->real);
		return -1;
	}
	t->base = opt.offset;
	{
		uint64_t avail = size64 - opt.offset;

		if (opt.size && opt.size < avail)
			avail = opt.size;
		t->nblocks = avail / opt.bs;
	}

	snprintf(p, sizeof(p), "/sys/block/%s/device/queue_depth", t->disk);
	if (read_text(p, val, sizeof(val)) == 0)
		t->queue_depth = (unsigned)strtoul(val, NULL, 10);
	snprintf(p, sizeof(p), "/sys/block/%s/queue/nr_requests", t->disk);
	if (read_text(p, val, sizeof(val)) == 0)
		t->nr_requests = (unsigned)strtoul(val, NULL, 10);

	/* NVMe: the controller behind the namespace, its queues and the
	 * fault object.  The command queue depth is the block layer's
	 * per-hardware-queue request count. */
	nvme_target_init(t);
	if (t->is_nvme && !t->queue_depth)
		t->queue_depth = t->nr_requests ? t->nr_requests :
				 t->nvme_sqsize ? t->nvme_sqsize + 1 : 0;

	/* Threads: as given, else enough to keep every hardware queue of a
	 * multi-queue device busy -- 32 per queue (one more than a SATA NCQ
	 * depth), within 32..DT_MAX_THREADS. */
	t->nworkers = opt.threads;
	if (!opt.threads_set && t->nr_hw_queues > 1) {
		unsigned n = 32u * t->nr_hw_queues;

		t->nworkers = n > DT_MAX_THREADS ? DT_MAX_THREADS : (int)n;
		/* A small region: no more threads than blocks. */
		if ((uint64_t)t->nworkers > t->nblocks && t->nblocks >= (uint64_t)opt.threads)
			t->nworkers = (int)t->nblocks;
	}
	if (t->nblocks < (uint64_t)t->nworkers) {
		fprintf(stderr,
			"disktest: %s: only %llu blocks of %u bytes -- fewer than threads\n",
			t->real, (unsigned long long)t->nblocks, opt.bs);
		return -1;
	}

	/* The ATA port, for fault injection: SCSI_IOCTL_GET_IDLUN reports
	 * host_no = port number - 1 on a SATA disk.  An NVMe namespace has
	 * no ATA port: the ioctl answers with the controller instance as
	 * host (nvme0 -> host 0), which would name ata1, a SATA port that
	 * is not this disk's.  Its fault injection is the namespace's own
	 * fault_inject object. */
	memset(&idlun, 0, sizeof(idlun));
	if (strncmp(t->disk, "nvme", 4) != 0 &&
	    ioctl(t->fd, SCSI_IOCTL_GET_IDLUN, &idlun) == 0) {
		int host = (int)((idlun.dev_id >> 24) & 0xFF);

		snprintf(t->ata_port, sizeof(t->ata_port), "ata%d", host + 1);
		snprintf(p, sizeof(p), "/sys/class/ata_port/%s/fault_inject",
			 t->ata_port);
		if (access(p, F_OK) != 0)
			t->ata_port[0] = 0; /* not an AHCI port (USB...) */
	}

	t->gen = xcalloc(t->nblocks, sizeof(uint32_t));
	t->alt = xcalloc(t->nblocks, sizeof(uint32_t));
	return 0;
}

static void target_describe(const struct dt_target *t)
{
	char a[32], b[32];

	msg("  %-10s %s  (%s of %s, disk %s%s%s)\n", t->name, t->real,
	    fmt_bytes(t->nblocks * opt.bs, a, sizeof(a)),
	    fmt_bytes(t->dev_bytes, b, sizeof(b)), t->disk,
	    t->ata_port[0] ? ", port " : "", t->ata_port);
	if (t->is_nvme) {
		msg("             NVMe controller %s%s%s%s%s%s, %u I/O queue(s) (sqsize %u), %u hardware queue(s) of depth %u\n",
		    t->nvme_ctrl, t->nvme_model[0] ? " \"" : "", t->nvme_model,
		    t->nvme_model[0] ? "\"" : "",
		    t->nvme_addr[0] ? " at " : "", t->nvme_addr,
		    t->nvme_queue_count ? t->nvme_queue_count - 1 : 0,
		    t->nvme_sqsize, t->nr_hw_queues, t->nr_requests);
		if (t->fi_path[0])
			msg("             fault injection: %s%s\n", t->fi_path,
			    t->fi_onefile ? " (one-file)" : "");
	}
	msg("             logical %u, physical %u, %llu blocks of %u, queue depth %u, %d threads%s\n",
	    t->lsz, t->psz, (unsigned long long)t->nblocks, opt.bs,
	    t->queue_depth, t->nworkers,
	    t->nworkers > 1 && opt.affinity && t->nr_hw_queues > 1 ?
		    " spread over the CPUs" : "");
}

/* ======================================================================
 * I/O
 * ====================================================================== */

/* Full-length positional I/O: retries short transfers, returns 0 or the
 * errno of the failing call. */
static int do_io(struct dt_worker *w, int write, void *buf, size_t len,
		 uint64_t off)
{
	struct dt_target *t = w->t;
	size_t done = 0;
	uint64_t t0 = now_ns(), dt;

	while (done < len) {
		ssize_t n;

		if (write)
			n = pwrite(t->fd, (uint8_t *)buf + done, len - done,
				   (off_t)(off + done));
		else
			n = pread(t->fd, (uint8_t *)buf + done, len - done,
				  (off_t)(off + done));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno ? errno : EIO;
		}
		if (n == 0)
			return EIO; /* end of device inside the region */
		if ((size_t)n < len - done)
			__atomic_add_fetch(&w->st.short_io, 1,
					   __ATOMIC_RELAXED);
		done += (size_t)n;
	}
	dt = now_ns() - t0;
	if (write) {
		__atomic_add_fetch(&w->st.wr_ops, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&w->st.wr_bytes, len, __ATOMIC_RELAXED);
	} else {
		__atomic_add_fetch(&w->st.rd_ops, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&w->st.rd_bytes, len, __ATOMIC_RELAXED);
	}
	__atomic_add_fetch(&w->st.lat_sum_ns, dt, __ATOMIC_RELAXED);
	__atomic_add_fetch(&w->st.lat_n, 1, __ATOMIC_RELAXED);
	if (dt > w->st.lat_max_ns)
		__atomic_store_n(&w->st.lat_max_ns, dt, __ATOMIC_RELAXED);
	__atomic_add_fetch(&w->st.lat_hist[lat_bucket(dt)], 1,
			   __ATOMIC_RELAXED);
	return 0;
}

static void note_io_error(struct dt_worker *w, const char *what,
			  uint64_t block, int err)
{
	struct dt_target *t = w->t;

	if (t->faults_armed) {
		__atomic_add_fetch(&w->st.fault_errors, 1, __ATOMIC_RELAXED);
		if (!opt.quiet)
			msg("  %s: %s of block %llu failed during fault injection: %s (expected)\n",
			    t->name, what, (unsigned long long)block,
			    strerror(err));
		return;
	}
	__atomic_add_fetch(&w->st.io_errors, 1, __ATOMIC_RELAXED);
	msg("  %s: %s of block %llu (byte %llu) FAILED: %s\n", t->name, what,
	    (unsigned long long)block,
	    (unsigned long long)(t->base + block * opt.bs), strerror(err));
	if (opt.stop_on_error)
		w->failed = 1;
}

/* Verify the block in w->buf against the expected generation(s).  Returns
 * 1 when good. */
static int verify_block(struct dt_worker *w, const uint8_t *buf,
			uint64_t block, uint32_t gen_a, uint32_t gen_b)
{
	struct dt_target *t = w->t;
	uint32_t nsec = opt.bs / t->lsz;
	enum dt_sec_state first = SEC_OK;
	struct dt_hdr fh;
	uint32_t first_idx = 0, bad = 0, gen_seen = 0;
	int mixed_gen = 0;

	memset(&fh, 0, sizeof(fh));
	for (uint32_t i = 0; i < nsec; i++) {
		struct dt_hdr h;
		enum dt_sec_state s = check_sector(buf + (size_t)i * t->lsz,
						   t->lsz, opt.seed, block, i,
						   gen_a, gen_b, &h);

		if (s == SEC_OK) {
			/* All sectors of a block must carry the same
			 * generation: a block written as one request is
			 * either old or new as a whole. */
			if (i == 0 || gen_seen == 0)
				gen_seen = h.gen;
			else if (h.gen != gen_seen)
				mixed_gen = 1;
			continue;
		}
		if (bad++ == 0) {
			first = s;
			first_idx = i;
			fh = h;
		}
	}
	if (!bad && !mixed_gen)
		return 1;

	__atomic_add_fetch(&w->st.bad_blocks, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&w->st.bad_by_state[bad ? first : SEC_WRONG_GEN],
			   1, __ATOMIC_RELAXED);
	pthread_mutex_lock(&t->report_lock);
	if (t->bad_reported < DT_MAX_REPORTED_BAD) {
		t->bad_reported++;
		if (!bad)
			msg("  %s: DATA ERROR block %llu: torn write, sectors of different generations (expected gen %u%s)\n",
			    t->name, (unsigned long long)block, gen_a,
			    gen_b != gen_a ? " or the previous one" : "");
		else {
			char exp[48];

			if (gen_a == 0)
				snprintf(exp, sizeof(exp), "any gen");
			else if (gen_b != gen_a)
				snprintf(exp, sizeof(exp), "gen %u or %u",
					 gen_a, gen_b);
			else
				snprintf(exp, sizeof(exp), "gen %u", gen_a);
			msg("  %s: DATA ERROR block %llu (byte %llu): %u of %u sectors bad, first at sector %u: %s"
			    " [found block %llu gen %u sector %u seed 0x%08x; expected block %llu %s seed 0x%08x]\n",
			    t->name, (unsigned long long)block,
			    (unsigned long long)(t->base + block * opt.bs),
			    bad, nsec, first_idx, sec_state_name(first),
			    (unsigned long long)fh.block, fh.gen, fh.sector,
			    fh.seed, (unsigned long long)block, exp, opt.seed);
		}
		if (t->bad_reported == DT_MAX_REPORTED_BAD)
			msg("  %s: (further data errors are counted, not listed)\n",
			    t->name);
	}
	pthread_mutex_unlock(&t->report_lock);
	if (opt.stop_on_error)
		w->failed = 1;
	return 0;
}

static int phase_over(const struct dt_worker *w)
{
	const struct dt_target *t = w->t;

	if (g_interrupted || w->failed)
		return 1;
	if (t->phase_deadline_ns && now_ns() >= t->phase_deadline_ns)
		return 1;
	return 0;
}

/* One write of a block with generation `gen', updating the expectation. */
static void write_block(struct dt_worker *w, uint64_t block, uint32_t gen)
{
	struct dt_target *t = w->t;
	int err;

	fill_block(w->buf, opt.bs, t->lsz, opt.seed, block, gen);
	err = do_io(w, 1, w->buf, opt.bs, t->base + block * opt.bs);
	if (err) {
		/* Old or new data may be on the media now; accept both from
		 * here on (until the next successful write). */
		note_io_error(w, "write", block, err);
		t->alt[block] = gen;
		return;
	}
	t->gen[block] = gen;
	t->alt[block] = gen;
}

static int read_verify_block(struct dt_worker *w, uint64_t block, int any_gen)
{
	struct dt_target *t = w->t;
	int err = do_io(w, 0, w->buf, opt.bs, t->base + block * opt.bs);

	if (err) {
		note_io_error(w, "read", block, err);
		return 0;
	}
	if (any_gen)
		return verify_block(w, w->buf, block, 0, 0);
	return verify_block(w, w->buf, block, t->gen[block], t->alt[block]);
}

static void *worker_main(void *arg)
{
	struct dt_worker *w = arg;
	struct dt_target *t = w->t;
	uint64_t ops = 0, writes = 0;

	/* Multi-queue devices map each CPU to a hardware queue: threads
	 * spread over the CPUs put load on every queue, not just on the ones
	 * of the CPUs the scheduler happens to pick. */
	if (w->cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(w->cpu, &set);
		pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
	}

	switch (t->phase) {
	case PH_SEQ_WRITE:
		for (;;) {
			uint64_t b = __atomic_fetch_add(&t->next_block, 1,
							__ATOMIC_RELAXED);

			if (b >= t->nblocks || g_interrupted || w->failed)
				break;
			write_block(w, b, t->gen[b] + 1);
			if (opt.flush_every && ++writes % opt.flush_every == 0) {
				fdatasync(t->fd);
				__atomic_add_fetch(&w->st.flush_ops, 1,
						   __ATOMIC_RELAXED);
			}
		}
		break;

	case PH_SEQ_VERIFY:
		for (;;) {
			uint64_t b = __atomic_fetch_add(&t->next_block, 1,
							__ATOMIC_RELAXED);

			if (b >= t->nblocks || g_interrupted || w->failed)
				break;
			read_verify_block(w, b, opt.mode == MODE_VERIFY);
		}
		break;

	case PH_SEQ_READ:
		/* Read-only mode: every block twice, the two reads must
		 * agree (a flaky link or a DMA into the wrong page shows up
		 * as a difference). */
		for (;;) {
			uint64_t b = __atomic_fetch_add(&t->next_block, 1,
							__ATOMIC_RELAXED);
			uint64_t off = t->base + b * opt.bs;
			int e1, e2;

			if (b >= t->nblocks || g_interrupted || w->failed)
				break;
			e1 = do_io(w, 0, w->buf, opt.bs, off);
			e2 = e1 ? 0 : do_io(w, 0, w->buf2, opt.bs, off);
			if (e1 || e2) {
				note_io_error(w, "read", b, e1 ? e1 : e2);
				continue;
			}
			if (memcmp(w->buf, w->buf2, opt.bs) != 0) {
				__atomic_add_fetch(&w->st.bad_blocks, 1,
						   __ATOMIC_RELAXED);
				msg("  %s: DATA ERROR block %llu: two reads returned different data\n",
				    t->name, (unsigned long long)b);
				if (opt.stop_on_error)
					w->failed = 1;
			}
		}
		break;

	case PH_RANDOM_READ:
		while (!phase_over(w) &&
		       (!t->ops_per_thread || ops < t->ops_per_thread)) {
			uint64_t b = xs64(&w->rnd) % t->nblocks;
			uint64_t off = t->base + b * opt.bs;
			int e1, e2;

			ops++;
			e1 = do_io(w, 0, w->buf, opt.bs, off);
			e2 = e1 ? 0 : do_io(w, 0, w->buf2, opt.bs, off);
			if (e1 || e2) {
				note_io_error(w, "read", b, e1 ? e1 : e2);
				continue;
			}
			if (memcmp(w->buf, w->buf2, opt.bs) != 0) {
				__atomic_add_fetch(&w->st.bad_blocks, 1,
						   __ATOMIC_RELAXED);
				msg("  %s: DATA ERROR block %llu: two reads returned different data\n",
				    t->name, (unsigned long long)b);
				if (opt.stop_on_error)
					w->failed = 1;
			}
		}
		break;

	case PH_RANDOM: {
		/* This thread's blocks: b % nworkers == tid. */
		uint64_t mine = (t->nblocks - (uint64_t)w->tid +
				 (uint64_t)t->nworkers - 1) /
				(uint64_t)t->nworkers;

		while (mine && !phase_over(w) &&
		       (!t->ops_per_thread || ops < t->ops_per_thread)) {
			uint64_t b = (xs64(&w->rnd) % mine) *
					     (uint64_t)t->nworkers +
				     (uint64_t)w->tid;
			int is_write = (xs64(&w->rnd) % 100) < opt.rwmix;

			ops++;
			if (b >= t->nblocks)
				continue;
			if (is_write) {
				write_block(w, b, t->gen[b] + 1);
				if (opt.verify_after_write)
					read_verify_block(w, b, 0);
				if (opt.flush_every &&
				    ++writes % opt.flush_every == 0) {
					fdatasync(t->fd);
					__atomic_add_fetch(&w->st.flush_ops, 1,
							   __ATOMIC_RELAXED);
				}
			} else {
				read_verify_block(w, b, 0);
			}
		}
		break;
	}
	}
	return NULL;
}

/* ======================================================================
 * Reporting
 * ====================================================================== */

static void target_snapshot(const struct dt_target *t, struct dt_stats *out)
{
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < t->nworkers; i++) {
		const struct dt_stats *s = &t->w[i].st;
		struct dt_stats c;

		/* Racy but monotonic per field: good enough for a rate. */
		memset(&c, 0, sizeof(c));
		c.rd_ops = __atomic_load_n(&s->rd_ops, __ATOMIC_RELAXED);
		c.wr_ops = __atomic_load_n(&s->wr_ops, __ATOMIC_RELAXED);
		c.rd_bytes = __atomic_load_n(&s->rd_bytes, __ATOMIC_RELAXED);
		c.wr_bytes = __atomic_load_n(&s->wr_bytes, __ATOMIC_RELAXED);
		c.lat_sum_ns =
			__atomic_load_n(&s->lat_sum_ns, __ATOMIC_RELAXED);
		c.lat_n = __atomic_load_n(&s->lat_n, __ATOMIC_RELAXED);
		c.lat_max_ns =
			__atomic_load_n(&s->lat_max_ns, __ATOMIC_RELAXED);
		c.io_errors = __atomic_load_n(&s->io_errors, __ATOMIC_RELAXED);
		c.fault_errors =
			__atomic_load_n(&s->fault_errors, __ATOMIC_RELAXED);
		c.bad_blocks =
			__atomic_load_n(&s->bad_blocks, __ATOMIC_RELAXED);
		for (int b = 0; b < DT_LAT_BUCKETS; b++)
			c.lat_hist[b] = __atomic_load_n(&s->lat_hist[b],
							__ATOMIC_RELAXED);
		stats_add(out, &c);
	}
}

/* /sys/block/<disk>/inflight: "<reads> <writes>". */
static unsigned sample_inflight(const struct dt_target *t)
{
	char p[PATH_MAX], val[64];
	unsigned r = 0, wr = 0;

	snprintf(p, sizeof(p), "/sys/block/%s/inflight", t->disk);
	if (read_text(p, val, sizeof(val)) != 0)
		return 0;
	if (sscanf(val, "%u %u", &r, &wr) < 1)
		return 0;
	return r + wr;
}

static void report_line(struct dt_target *t, uint64_t now)
{
	struct dt_stats s;
	double secs = (double)(now - t->last_ns) / 1e9;
	uint64_t drb, dwb, dro, dwo, dln, dls;

	if (secs <= 0)
		secs = 1e-9;
	target_snapshot(t, &s);
	drb = s.rd_bytes - t->last.rd_bytes;
	dwb = s.wr_bytes - t->last.wr_bytes;
	dro = s.rd_ops - t->last.rd_ops;
	dwo = s.wr_ops - t->last.wr_ops;
	dln = s.lat_n - t->last.lat_n;
	dls = s.lat_sum_ns - t->last.lat_sum_ns;
	msg("  [%-8s] %-17s rd %7.1f MB/s %6.0f IOPS | wr %7.1f MB/s %6.0f IOPS | lat avg %6.2f ms p99 %6.2f ms max %7.2f ms | inflight %u max %u | err %llu/%llu bad %llu\n",
	    t->name, phase_name(t->phase), (double)drb / secs / 1e6,
	    (double)dro / secs, (double)dwb / secs / 1e6, (double)dwo / secs,
	    dln ? (double)dls / (double)dln / 1e6 : 0.0,
	    (double)lat_percentile_delta_us(&s, &t->last, 99.0) / 1e3,
	    (double)s.lat_max_ns / 1e6, t->inflight_now, t->inflight_max,
	    (unsigned long long)s.io_errors,
	    (unsigned long long)s.fault_errors,
	    (unsigned long long)s.bad_blocks);
	t->last = s;
	t->last_ns = now;
}

/* ======================================================================
 * Fault injection
 * ====================================================================== */

static void print_port_file(const struct dt_target *t, const char *file,
			    const char *label)
{
	char p[PATH_MAX], buf[2048];

	snprintf(p, sizeof(p), "/sys/class/ata_port/%s/%s", t->ata_port,
		 file);
	if (read_text(p, buf, sizeof(buf)) != 0)
		return;
	msg("  %s %s/%s %s:\n", t->name, t->ata_port, file, label);
	for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
		msg("      %s\n", line);
}

/* on: 1 arm at the start of the phase, 2 re-arm (the same spec again,
 * which renews its count), 0 disarm. */
static int fault_arm(struct dt_target *t, int on)
{
	char p[PATH_MAX];

	if (on && !t->fault)
		return 0;
	if (t->is_nvme) {
		int r;

		if (!t->fi_path[0])
			return -1;
		/* The first arm starts from the defaults, so nothing of an
		 * earlier round's spec lingers; a re-arm keeps the counters. */
		if (on == 1)
			r = nvme_fi_apply(t, "clear") || nvme_fi_apply(t, t->fault);
		else if (on)
			r = nvme_fi_apply(t, t->fault);
		else
			r = nvme_fi_apply(t, "off");
		if (r) {
			msg("  %s: applying \"%s\" to %s failed: %s\n", t->name,
			    on ? t->fault : "off", t->fi_path, strerror(errno));
			return -1;
		}
		t->faults_armed = on != 0;
		return 0;
	}
	if (!t->ata_port[0])
		return -1;
	snprintf(p, sizeof(p), "/sys/class/ata_port/%s/fault_inject",
		 t->ata_port);
	if (write_text(p, on ? t->fault : "off") != 0) {
		msg("  %s: writing \"%s\" to %s failed: %s\n", t->name,
		    on ? t->fault : "off", p, strerror(errno));
		return -1;
	}
	t->faults_armed = on != 0;
	return 0;
}

/* ======================================================================
 * Phases
 * ====================================================================== */

static void run_phase(enum dt_phase ph, uint64_t duration_ns,
		      uint64_t ops_per_target)
{
	uint64_t start = now_ns(), next_report, next_fault = 0;
	int fault_phase = 0;

	if (ph == PH_RANDOM)
		for (int i = 0; i < g_nt; i++)
			if (g_t[i].fault)
				fault_phase = 1;

	msg("\n== %s ==\n", phase_name(ph));
	for (int i = 0; i < g_nt; i++) {
		struct dt_target *t = &g_t[i];

		t->phase = ph;
		t->next_block = 0;
		t->phase_deadline_ns = duration_ns ? start + duration_ns : 0;
		t->ops_per_thread =
			ops_per_target ?
				(ops_per_target + (uint64_t)t->nworkers - 1) /
					(uint64_t)t->nworkers :
				0;
		t->inflight_max = 0;
		t->inflight_now = 0;
		target_snapshot(t, &t->last);
		t->phase_base = t->last;
		t->last_ns = start;
		if (fault_phase && t->fault) {
			if (t->is_nvme) {
				print_nvme_fault_state(t, "before");
			} else {
				print_port_file(t, "stats", "before");
				print_port_file(t, "eh", "before");
			}
			fault_arm(t, 1);
		}
	}
	if (fault_phase && opt.fault_every_s)
		next_fault = start + (uint64_t)opt.fault_every_s * 1000000000ull;

	for (int i = 0; i < g_nt; i++)
		for (int k = 0; k < g_t[i].nworkers; k++) {
			struct dt_worker *w = &g_t[i].w[k];

			if (pthread_create(&w->thread, NULL, worker_main, w)) {
				fprintf(stderr,
					"disktest: cannot start a thread\n");
				exit(DT_EXIT_SETUP);
			}
		}

	/* The main thread samples inflight often, reports every
	 * --report seconds, re-arms faults, and notices the end. */
	next_report = start + (uint64_t)opt.report_s * 1000000000ull;
	for (;;) {
		uint64_t now;
		int busy = 0;

		usleep(100000);
		now = now_ns();
		for (int i = 0; i < g_nt; i++) {
			struct dt_target *t = &g_t[i];
			unsigned inf = sample_inflight(t);

			t->inflight_now = inf;
			if (inf > t->inflight_max)
				t->inflight_max = inf;
			if (t->phase == PH_SEQ_WRITE ||
			    t->phase == PH_SEQ_VERIFY ||
			    t->phase == PH_SEQ_READ) {
				if (__atomic_load_n(&t->next_block,
						    __ATOMIC_RELAXED) <
				    t->nblocks + (uint64_t)t->nworkers)
					busy = 1;
			} else if (t->phase_deadline_ns ?
					   now < t->phase_deadline_ns :
					   1) {
				busy = 1;
			}
		}
		if (opt.report_s && now >= next_report) {
			for (int i = 0; i < g_nt; i++)
				report_line(&g_t[i], now);
			next_report = now + (uint64_t)opt.report_s *
						    1000000000ull;
		}
		if (next_fault && now >= next_fault) {
			for (int i = 0; i < g_nt; i++)
				fault_arm(&g_t[i], 2);
			next_fault = now + (uint64_t)opt.fault_every_s *
						   1000000000ull;
		}
		if (g_interrupted)
			break;
		/* Op-count-limited random phases end when the threads do;
		 * the join below waits for that. */
		if (!busy || ops_per_target)
			break;
	}

	for (int i = 0; i < g_nt; i++)
		for (int k = 0; k < g_t[i].nworkers; k++)
			pthread_join(g_t[i].w[k].thread, NULL);

	{
		uint64_t now = now_ns();

		for (int i = 0; i < g_nt; i++) {
			struct dt_target *t = &g_t[i];
			double secs = (double)(now - start) / 1e9;

			if (fault_phase && t->fault) {
				fault_arm(t, 0);
				if (t->is_nvme) {
					print_nvme_fault_state(t, "after");
				} else {
					print_port_file(t, "fault_inject", "after");
					print_port_file(t, "stats", "after");
					print_port_file(t, "eh", "after");
				}
			}
			/* The rate of the stretch since the last report
			 * line (unless that line was just printed), then
			 * the phase's duration and its overall rate. */
			if (now - t->last_ns > 200000000ull)
				report_line(t, now);
			{
				struct dt_stats e;
				uint64_t ops, bytes, ln, ls;

				target_snapshot(t, &e);
				ops = (e.rd_ops + e.wr_ops) -
				      (t->phase_base.rd_ops + t->phase_base.wr_ops);
				bytes = (e.rd_bytes + e.wr_bytes) -
					(t->phase_base.rd_bytes +
					 t->phase_base.wr_bytes);
				ln = e.lat_n - t->phase_base.lat_n;
				ls = e.lat_sum_ns - t->phase_base.lat_sum_ns;
				msg("  [%-8s] %s done in %.1f s: %.0f IOPS, %.1f MB/s, lat avg %.2f ms p50 %.2f p99 %.2f p99.9 %.2f ms, inflight max %u\n",
				    t->name, phase_name(ph), secs,
				    secs > 0 ? (double)ops / secs : 0.0,
				    secs > 0 ? (double)bytes / secs / 1e6 : 0.0,
				    ln ? (double)ls / (double)ln / 1e6 : 0.0,
				    (double)lat_percentile_delta_us(&e, &t->phase_base, 50.0) / 1e3,
				    (double)lat_percentile_delta_us(&e, &t->phase_base, 99.0) / 1e3,
				    (double)lat_percentile_delta_us(&e, &t->phase_base, 99.9) / 1e3,
				    t->inflight_max);
			}
		}
	}
	/* Make sure the next phase reads the media, not a cache: write back
	 * and drop what the kernel holds for the device. */
	for (int i = 0; i < g_nt; i++) {
		if (ph == PH_SEQ_WRITE || ph == PH_RANDOM) {
			if (fdatasync(g_t[i].fd) != 0)
				msg("  %s: fdatasync failed: %s\n", g_t[i].name,
				    strerror(errno));
		}
		if (ioctl(g_t[i].fd, BLKFLSBUF, 0) != 0 && errno != EACCES &&
		    errno != EPERM)
			msg("  %s: BLKFLSBUF failed: %s\n", g_t[i].name,
			    strerror(errno));
	}
}

/* ======================================================================
 * Main
 * ====================================================================== */

static void on_signal(int sig)
{
	(void)sig;
	g_interrupted = 1;
}

static void usage(void)
{
	printf("usage: disktest [options] TARGET...   (TARGET: /dev/sdXN, /dev/nvmeXnYpZ, or `auto')\n"
	       "\n"
	       "Stress a block device with concurrent sequential and random I/O and\n"
	       "verify every block afterwards.  Writing modes DESTROY the target: it must be\n"
	       "a partition named likeos-test* or be named in DISKTEST_DISK/TESTLIBC_DISK.\n"
	       "\n"
	       "  -m, --mode MODE        full (default), write, verify, random, read,\n"
	       "                         quick (1 GiB, ~1 min), fault (error-recovery rounds)\n"
	       "  -j, --threads N        threads per target (default %d, multi-queue: 32 per\n"
	       "                         hardware queue up to 128; >= queue depth saturates)\n"
	       "  -b, --bs SIZE          block size (default 64k, up to 16m, multiple of the sector)\n"
	       "  -o, --offset SIZE      start this far into each target (multiple of --bs)\n"
	       "  -s, --size SIZE        use only this much of each target\n"
	       "  -t, --time SEC         random phase duration (default %d; 0 = until -n)\n"
	       "  -n, --ops N            random phase: operations per target\n"
	       "  -l, --loops N          random + verify rounds (default 1; 0 = until Ctrl-C)\n"
	       "  -w, --rwmix PCT        percent writes in the random phase (default %d)\n"
	       "  -S, --seed N           pattern seed (default: from the clock)\n"
	       "  -F, --flush-every N    fdatasync after every N writes per thread\n"
	       "  -V, --verify-after-write  read back every random write at once\n"
	       "  -f, --fault SPEC       arm /sys/class/ata_port/ataN/fault_inject during the\n"
	       "                         random phase, e.g. \"timeout 3\", \"dev_error every=50 ncq\";\n"
	       "                         NVMe: the namespace's fault object, e.g.\n"
	       "                         \"kind=status status=0x281 interval=40 times=-1 probability=100\"\n"
	       "                         or \"kind=reset times=1 probability=100\"\n"
	       "  -e, --fault-every SEC  re-arm the fault every SEC seconds\n"
	       "  -r, --report SEC       report interval (default %d; 0 = only at phase ends)\n"
	       "  -x, --stop-on-error    stop a target at its first error\n"
	       "  -A, --affinity on|off  spread the threads of a multi-queue target over the\n"
	       "                         CPUs (default on)\n"
	       "  -q, --quiet            do not list expected fault-injection errors\n"
	       "  -h, --help\n"
	       "\n"
	       "exit status: 0 all verified, 1 data integrity failure, 2 I/O errors, 3 setup\n",
	       DT_DEF_THREADS, DT_DEF_TIME, DT_DEF_RWMIX, DT_DEF_REPORT);
}

int main(int argc, char **argv)
{
	static const struct option lo[] = {
		{ "mode", required_argument, NULL, 'm' },
		{ "threads", required_argument, NULL, 'j' },
		{ "bs", required_argument, NULL, 'b' },
		{ "offset", required_argument, NULL, 'o' },
		{ "size", required_argument, NULL, 's' },
		{ "time", required_argument, NULL, 't' },
		{ "ops", required_argument, NULL, 'n' },
		{ "loops", required_argument, NULL, 'l' },
		{ "rwmix", required_argument, NULL, 'w' },
		{ "seed", required_argument, NULL, 'S' },
		{ "flush-every", required_argument, NULL, 'F' },
		{ "verify-after-write", no_argument, NULL, 'V' },
		{ "fault", required_argument, NULL, 'f' },
		{ "fault-every", required_argument, NULL, 'e' },
		{ "report", required_argument, NULL, 'r' },
		{ "stop-on-error", no_argument, NULL, 'x' },
		{ "affinity", required_argument, NULL, 'A' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	static char paths[DT_MAX_TARGETS][PATH_MAX];
	int npaths = 0, c, seed_set = 0, writable;
	int time_set = 0, every_set = 0;
	uint64_t v;
	struct dt_stats all;
	uint64_t t_start;
	unsigned loop;
	int rc;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

	if (ncpu > 64)
		ncpu = 64; /* the width of cpu_set_t here */

	while ((c = getopt_long(argc, argv, "m:j:b:o:s:t:n:l:w:S:F:Vf:e:r:xA:qh",
				lo, NULL)) != -1) {
		switch (c) {
		case 'm':
			if (!strcmp(optarg, "full"))
				opt.mode = MODE_FULL;
			else if (!strcmp(optarg, "write"))
				opt.mode = MODE_WRITE;
			else if (!strcmp(optarg, "verify"))
				opt.mode = MODE_VERIFY;
			else if (!strcmp(optarg, "random"))
				opt.mode = MODE_RANDOM;
			else if (!strcmp(optarg, "read"))
				opt.mode = MODE_READ;
			else if (!strcmp(optarg, "quick"))
				opt.mode = MODE_QUICK;
			else if (!strcmp(optarg, "fault"))
				opt.mode = MODE_FAULT;
			else {
				fprintf(stderr, "disktest: unknown mode %s\n",
					optarg);
				return DT_EXIT_SETUP;
			}
			break;
		case 'j':
			opt.threads_set = 1;
			opt.threads = atoi(optarg);
			if (opt.threads < 1 || opt.threads > DT_MAX_THREADS) {
				fprintf(stderr,
					"disktest: --threads must be 1..%d\n",
					DT_MAX_THREADS);
				return DT_EXIT_SETUP;
			}
			break;
		case 'b':
			if (parse_size(optarg, &v) || v < 512 ||
			    v > DT_MAX_BS || (v % 512)) {
				fprintf(stderr,
					"disktest: --bs must be a multiple of 512 up to 16m\n");
				return DT_EXIT_SETUP;
			}
			opt.bs = (uint32_t)v;
			break;
		case 'o':
			if (parse_size(optarg, &opt.offset))
				goto bad_arg;
			break;
		case 's':
			if (parse_size(optarg, &opt.size))
				goto bad_arg;
			break;
		case 't':
			opt.time_s = (unsigned)strtoul(optarg, NULL, 0);
			time_set = 1;
			break;
		case 'n':
			if (parse_size(optarg, &opt.ops))
				goto bad_arg;
			break;
		case 'l':
			opt.loops = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'w':
			opt.rwmix = (unsigned)strtoul(optarg, NULL, 0);
			if (opt.rwmix > 100)
				goto bad_arg;
			break;
		case 'S':
			opt.seed = (uint32_t)strtoul(optarg, NULL, 0);
			seed_set = 1;
			break;
		case 'F':
			opt.flush_every = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'V':
			opt.verify_after_write = 1;
			break;
		case 'f':
			opt.fault = optarg;
			break;
		case 'e':
			opt.fault_every_s = (unsigned)strtoul(optarg, NULL, 0);
			every_set = 1;
			break;
		case 'r':
			opt.report_s = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'x':
			opt.stop_on_error = 1;
			break;
		case 'A':
			if (!strcmp(optarg, "on"))
				opt.affinity = 1;
			else if (!strcmp(optarg, "off"))
				opt.affinity = 0;
			else
				goto bad_arg;
			break;
		case 'q':
			opt.quiet = 1;
			break;
		case 'h':
			usage();
			return 0;
		default:
		bad_arg:
			usage();
			return DT_EXIT_SETUP;
		}
	}
	if (optind >= argc) {
		usage();
		return DT_EXIT_SETUP;
	}
	if (opt.mode == MODE_VERIFY && !seed_set) {
		fprintf(stderr,
			"disktest: --mode verify needs the --seed of the run that wrote the target\n");
		return DT_EXIT_SETUP;
	}
	if (!seed_set)
		opt.seed = (uint32_t)(now_ns() ^ ((uint64_t)getpid() << 16));
	/* The quick and fault modes: a 1 GiB region and short phases unless
	 * the options say otherwise. */
	if (opt.mode == MODE_QUICK || opt.mode == MODE_FAULT) {
		if (!opt.size)
			opt.size = DT_QUICK_SIZE;
		if (!time_set)
			opt.time_s = opt.mode == MODE_QUICK ? DT_QUICK_TIME :
							      DT_FAULT_TIME;
		if (opt.mode == MODE_FAULT && !every_set)
			opt.fault_every_s = DT_FAULT_EVERY;
	}
	if (opt.fault && opt.mode != MODE_FULL && opt.mode != MODE_RANDOM &&
	    opt.mode != MODE_QUICK && opt.mode != MODE_FAULT) {
		fprintf(stderr,
			"disktest: --fault applies to the random phase (modes full, random, quick and fault)\n");
		return DT_EXIT_SETUP;
	}
	if (opt.time_s == 0 && opt.ops == 0 &&
	    (opt.mode == MODE_FULL || opt.mode == MODE_RANDOM ||
	     opt.mode == MODE_READ || opt.mode == MODE_QUICK ||
	     opt.mode == MODE_FAULT)) {
		fprintf(stderr,
			"disktest: --time 0 needs --ops to end the random phase\n");
		return DT_EXIT_SETUP;
	}

	for (; optind < argc; optind++) {
		if (!strcmp(argv[optind], "auto")) {
			int n = add_auto_targets(paths + npaths,
						 DT_MAX_TARGETS - npaths);

			if (n == 0)
				fprintf(stderr,
					"disktest: auto: no partition named likeos-test* (/dev/disk/by-partlabel)\n");
			npaths += n;
		} else if (npaths < DT_MAX_TARGETS) {
			snprintf(paths[npaths++], PATH_MAX, "%s",
				 argv[optind]);
		}
	}
	if (npaths == 0)
		return DT_EXIT_SETUP;

	crc_init();
	writable = (opt.mode != MODE_READ && opt.mode != MODE_VERIFY);
	for (int i = 0; i < npaths; i++) {
		struct dt_target *t = &g_t[g_nt];
		int dup = 0;

		if (target_open(t, paths[i], writable) != 0)
			return DT_EXIT_SETUP;
		for (int k = 0; k < g_nt; k++)
			if (!strcmp(g_t[k].real, t->real))
				dup = 1;
		if (dup) {
			close(t->fd);
			free(t->gen);
			free(t->alt);
			continue;
		}
		if ((opt.fault || opt.mode == MODE_FAULT) && !t->ata_port[0] &&
		    !(t->is_nvme && t->fi_path[0])) {
			fprintf(stderr,
				t->is_nvme ?
					"disktest: %s: no fault_inject object for the namespace (/sys/kernel/debug/<ns>/fault_inject)\n" :
					"disktest: %s has no AHCI port with a fault_inject knob\n",
				t->real);
			return DT_EXIT_SETUP;
		}
		t->fault = opt.fault;
		t->w = xcalloc((size_t)t->nworkers, sizeof(struct dt_worker));
		for (int k = 0; k < t->nworkers; k++) {
			uint64_t st = ((uint64_t)opt.seed << 20) ^
				      ((uint64_t)g_nt << 8) ^ (uint64_t)k;

			t->w[k].t = t;
			t->w[k].tid = k;
			t->w[k].cpu = -1;
			if (opt.affinity && t->nr_hw_queues > 1 && ncpu > 1)
				t->w[k].cpu = k % ncpu;
			t->w[k].buf = xmalloc_aligned(4096, opt.bs);
			t->w[k].buf2 = xmalloc_aligned(4096, opt.bs);
			t->w[k].rnd = splitmix64(&st) | 1;
		}
		g_nt++;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	msg("disktest: seed 0x%08x, mode %s, %d target(s)\n", opt.seed,
	    opt.mode == MODE_FULL     ? "full" :
	    opt.mode == MODE_WRITE    ? "write" :
	    opt.mode == MODE_VERIFY   ? "verify" :
	    opt.mode == MODE_RANDOM   ? "random" :
	    opt.mode == MODE_QUICK    ? "quick" :
	    opt.mode == MODE_FAULT    ? "fault" :
					"read",
	    g_nt);
	for (int i = 0; i < g_nt; i++)
		target_describe(&g_t[i]);
	if (opt.mode == MODE_FAULT) {
		if (opt.fault)
			msg("  fault round: \"%s\", %u s of load, re-armed every %u s\n",
			    opt.fault, opt.time_s, opt.fault_every_s);
		else
			msg("  %zu fault rounds (SATA) / %zu (NVMe) of %u s each (re-armed every %u s), a verify after each\n",
			    sizeof(g_fault_rounds) / sizeof(g_fault_rounds[0]),
			    sizeof(g_fault_rounds_nvme) /
				    sizeof(g_fault_rounds_nvme[0]),
			    opt.time_s, opt.fault_every_s);
	} else if (opt.fault) {
		msg("  fault injection during the random phase: \"%s\"%s\n",
		    opt.fault, opt.fault_every_s ? " (re-armed periodically)" :
						   "");
	}

	t_start = now_ns();
	switch (opt.mode) {
	case MODE_READ:
		run_phase(PH_SEQ_READ, 0, 0);
		if (!g_interrupted)
			run_phase(PH_RANDOM_READ,
				  (uint64_t)opt.time_s * 1000000000ull,
				  opt.ops);
		break;
	case MODE_WRITE:
		run_phase(PH_SEQ_WRITE, 0, 0);
		msg("  written with seed 0x%08x: `disktest -m verify -S 0x%08x -b %u ...' checks it later\n",
		    opt.seed, opt.seed, opt.bs);
		break;
	case MODE_VERIFY:
		run_phase(PH_SEQ_VERIFY, 0, 0);
		break;
	case MODE_FAULT: {
		/* One round per fault kind: random load with the fault
		 * armed, then a verify of the whole region -- also after
		 * Ctrl-C, the data has to be checked whatever stopped it. */
		/* The rounds are per target kind: SATA targets go through
		 * the AHCI knob's list, NVMe namespaces through theirs; a
		 * target whose list is shorter idles (no fault) in the extra
		 * rounds.  --fault SPEC is one round for every target. */
		const size_t n_ahci = sizeof(g_fault_rounds) /
				      sizeof(g_fault_rounds[0]);
		const size_t n_nvme = sizeof(g_fault_rounds_nvme) /
				      sizeof(g_fault_rounds_nvme[0]);
		const char *given = opt.fault;
		size_t nrounds = 1;

		if (!given) {
			nrounds = 0;
			for (int i = 0; i < g_nt; i++) {
				size_t n = g_t[i].is_nvme ? n_nvme : n_ahci;

				if (n > nrounds)
					nrounds = n;
			}
		}
		run_phase(PH_SEQ_WRITE, 0, 0);
		if (!g_interrupted)
			run_phase(PH_SEQ_VERIFY, 0, 0);
		for (size_t r = 0; r < nrounds && !g_interrupted; r++) {
			volatile sig_atomic_t was;

			msg("\n-- fault round %zu of %zu --\n", r + 1, nrounds);
			for (int i = 0; i < g_nt; i++) {
				struct dt_target *t = &g_t[i];

				if (given)
					t->fault = given;
				else if (t->is_nvme)
					t->fault = r < n_nvme ?
							   g_fault_rounds_nvme[r] :
							   NULL;
				else
					t->fault = r < n_ahci ? g_fault_rounds[r] :
								NULL;
				msg("  %s: %s%s%s\n", t->name,
				    t->fault ? "\"" : "",
				    t->fault ? t->fault : "no fault this round",
				    t->fault ? "\"" : "");
			}
			run_phase(PH_RANDOM,
				  (uint64_t)opt.time_s * 1000000000ull, 0);
			was = g_interrupted;
			g_interrupted = 0;
			run_phase(PH_SEQ_VERIFY, 0, 0);
			g_interrupted = was;
		}
		for (int i = 0; i < g_nt; i++)
			g_t[i].fault = given;
		break;
	}
	case MODE_FULL:
	case MODE_RANDOM:
	case MODE_QUICK:
		run_phase(PH_SEQ_WRITE, 0, 0);
		if ((opt.mode == MODE_FULL || opt.mode == MODE_QUICK) &&
		    !g_interrupted)
			run_phase(PH_SEQ_VERIFY, 0, 0);
		for (loop = 0; !g_interrupted && (opt.loops == 0 ||
						  loop < opt.loops);
		     loop++) {
			if (opt.loops != 1)
				msg("\n-- round %u --\n", loop + 1);
			run_phase(PH_RANDOM,
				  (uint64_t)opt.time_s * 1000000000ull,
				  opt.ops);
			/* The final verify runs even after Ctrl-C: the data
			 * has to be checked whatever stopped the load. */
			{
				volatile sig_atomic_t was = g_interrupted;

				g_interrupted = 0;
				run_phase(PH_SEQ_VERIFY, 0, 0);
				g_interrupted = was;
			}
		}
		break;
	}

	/* Summary */
	memset(&all, 0, sizeof(all));
	msg("\n== summary (%.1f s) ==\n", (double)(now_ns() - t_start) / 1e9);
	for (int i = 0; i < g_nt; i++) {
		struct dt_target *t = &g_t[i];
		struct dt_stats s;
		char a[32], b[32];

		memset(&s, 0, sizeof(s));
		for (int k = 0; k < t->nworkers; k++)
			stats_add(&s, &t->w[k].st);
		stats_add(&all, &s);
		msg("  %-10s read %s in %llu ops, wrote %s in %llu ops, %llu flushes\n",
		    t->name, fmt_bytes(s.rd_bytes, a, sizeof(a)),
		    (unsigned long long)s.rd_ops,
		    fmt_bytes(s.wr_bytes, b, sizeof(b)),
		    (unsigned long long)s.wr_ops,
		    (unsigned long long)s.flush_ops);
		msg("             latency avg %.2f ms, p50 <= %llu us, p99 <= %llu us, p99.9 <= %llu us, max %.2f ms\n",
		    s.lat_n ? (double)s.lat_sum_ns / (double)s.lat_n / 1e6 : 0.0,
		    (unsigned long long)lat_percentile_us(&s, 50.0),
		    (unsigned long long)lat_percentile_us(&s, 99.0),
		    (unsigned long long)lat_percentile_us(&s, 99.9),
		    (double)s.lat_max_ns / 1e6);
		msg("             I/O errors %llu, during fault injection %llu, short transfers %llu, queue depth %u%s\n",
		    (unsigned long long)s.io_errors,
		    (unsigned long long)s.fault_errors,
		    (unsigned long long)s.short_io, t->queue_depth,
		    t->nr_hw_queues > 1 ? " per hardware queue" : "");
		if (t->is_nvme)
			msg("             NVMe %s: %u hardware queue(s), %d threads%s\n",
			    t->nvme_ctrl, t->nr_hw_queues, t->nworkers,
			    t->w[0].cpu >= 0 ? " spread over the CPUs" : "");
		if (s.bad_blocks) {
			msg("             DATA ERRORS: %llu blocks\n",
			    (unsigned long long)s.bad_blocks);
			for (int k = 1; k < 8; k++)
				if (s.bad_by_state[k])
					msg("               %-40s %llu\n",
					    sec_state_name(
						    (enum dt_sec_state)k),
					    (unsigned long long)
						    s.bad_by_state[k]);
		} else {
			msg("             data verified: no errors\n");
		}
	}

	if (all.bad_blocks)
		rc = DT_EXIT_DATA;
	else if (all.io_errors)
		rc = DT_EXIT_IO;
	else
		rc = DT_EXIT_OK;
	msg("\ndisktest: %s%s (exit %d)\n",
	    rc == DT_EXIT_OK   ? "PASS" :
	    rc == DT_EXIT_DATA ? "FAIL: data integrity" :
				 "FAIL: I/O errors",
	    g_interrupted ? " (interrupted)" : "", rc);

	for (int i = 0; i < g_nt; i++)
		close(g_t[i].fd);
	return rc;
}
