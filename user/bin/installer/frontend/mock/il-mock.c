/* LikeOS installer -- a complete stand-in for libinstaller.
 *
 * Implements every function of installer.h without touching a single disk,
 * so the graphical installer can be run and tested on any desktop:
 *
 *  - four made-up disks: a blank 64 GB NVMe drive, a 256 GB SATA drive with
 *    a GPT and four partitions (EFI system, reserved, a large NTFS data
 *    partition, an ext4 one) and free space at its end, the USB stick the
 *    installer was "booted" from (marked is_boot_medium), and a 32 GB drive
 *    with a legacy MBR table;
 *  - a partitioning plan that is edited in memory with the same rules the
 *    real one follows: 1 MiB alignment, an EFI system partition created in
 *    front of the first new partition of a disk without one, deleting a
 *    partition the plan created simply forgets it, every view is a replay
 *    of the queued operations on the scanned disk;
 *  - an installation that runs for about twenty seconds and reports the
 *    same steps, details and counters the real one does.
 *
 * Environment:
 *   IL_MOCK_FAIL=<step>   fail during that step: prepare, partition, format,
 *                         mount, extract, configure, bootloader, finalize
 *   IL_MOCK_SPEED=<f>     run the installation f times as fast (0.1 .. 50)
 *   IL_MOCK_NO_DISKS=1    report no disk at all but the boot medium
 *   IL_MOCK_NO_SOURCE=1   pretend the system image is missing
 *
 * It is part of every build of likeos-installer and chosen with --mock (a
 * build without libinstaller has nothing else).  So that it can sit next
 * to the real library in one program, every function is compiled under an
 * "ilmock_" name (il-mock-names.h) and reached through the function table
 * il_backend_mock at the end of this file (see backend-ops.h).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "il-mock-names.h"
#include "installer.h"
#include "backend-ops.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MOCK_VERSION      "LikeOS 2026.11 (mock backend)"
#define MOCK_MAX_OPS      96
#define MIB               (1024ULL * 1024ULL)
#define GIB               (1024ULL * MIB)
#define GPT_TAIL_BYTES    (33ULL * 512ULL)   /* backup entries + header */

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

struct il_ctx {
	char *source;
	char *target;
	char *log_path;
	char *sysroot;
	int dry_run;
	int verbose;
	il_log_cb log_cb;
	void *log_user;
	FILE *log;
	char last_error[IL_ERROR_MAX];
	atomic_int cancel;
	atomic_int can_cancel;
	atomic_int installing;
	double speed;
	char fail_step[32];
	unsigned int seed;
};

static char *dup_or(const char *s, const char *fallback)
{
	const char *v = (s && *s) ? s : fallback;

	return v ? strdup(v) : NULL;
}

static void set_error(il_ctx *ctx, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void set_error(il_ctx *ctx, const char *fmt, ...)
{
	va_list ap;

	if (!ctx)
		return;
	va_start(ap, fmt);
	vsnprintf(ctx->last_error, sizeof(ctx->last_error), fmt, ap);
	va_end(ap);
	il_log(ctx, IL_LOG_ERROR, "%s", ctx->last_error);
}

/* "0.25" -> 0.25 whatever the locale's decimal separator is. */
static double parse_decimal(const char *p)
{
	double v = 0.0, scale = 0.1;

	while (*p >= '0' && *p <= '9')
		v = v * 10.0 + (*p++ - '0');
	if (*p == '.' || *p == ',') {
		for (p++; *p >= '0' && *p <= '9'; p++, scale /= 10.0)
			v += (*p - '0') * scale;
	}
	return v;
}

il_ctx *il_ctx_new(const struct il_options *opt)
{
	il_ctx *ctx = calloc(1, sizeof(*ctx));
	const char *env;

	if (!ctx)
		return NULL;
	ctx->source = dup_or(opt ? opt->source : NULL, IL_DEFAULT_SOURCE);
	ctx->target = dup_or(opt ? opt->target : NULL, IL_DEFAULT_TARGET);
	ctx->log_path = dup_or(opt ? opt->log_path : NULL, IL_DEFAULT_LOG);
	ctx->sysroot = dup_or(opt ? opt->sysroot : NULL, "");
	ctx->dry_run = opt ? opt->dry_run : 0;
	ctx->verbose = opt ? opt->verbose : 0;
	ctx->log_cb = opt ? opt->log_cb : NULL;
	ctx->log_user = opt ? opt->log_user : NULL;
	atomic_store(&ctx->cancel, 0);
	atomic_store(&ctx->can_cancel, 1);
	ctx->seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();

	ctx->speed = 1.0;
	env = getenv("IL_MOCK_SPEED");
	if (env && *env) {
		double s = parse_decimal(env);

		if (s >= 0.1 && s <= 50.0)
			ctx->speed = s;
	}
	env = getenv("IL_MOCK_FAIL");
	if (env && *env) {
		size_t i;

		for (i = 0; env[i] && i + 1 < sizeof(ctx->fail_step); i++)
			ctx->fail_step[i] = (char)tolower((unsigned char)env[i]);
		ctx->fail_step[i] = '\0';
	}

	if (ctx->log_path && *ctx->log_path) {
		ctx->log = fopen(ctx->log_path, "a");
		if (ctx->log)
			setvbuf(ctx->log, NULL, _IOLBF, 0);
	}
	il_log(ctx, IL_LOG_INFO, "installer backend %s started (mock)",
	       il_version());
	return ctx;
}

void il_ctx_free(il_ctx *ctx)
{
	if (!ctx)
		return;
	il_log(ctx, IL_LOG_DEBUG, "installer backend shut down");
	if (ctx->log)
		fclose(ctx->log);
	free(ctx->source);
	free(ctx->target);
	free(ctx->log_path);
	free(ctx->sysroot);
	free(ctx);
}

const char *il_last_error(const il_ctx *ctx)
{
	return ctx ? ctx->last_error : "";
}

const char *il_version(void)
{
	return MOCK_VERSION;
}

void il_log(il_ctx *ctx, int level, const char *fmt, ...)
{
	static const char *const names[] = { "debug", "info", "warning", "error" };
	char msg[1024];
	char line[1100];
	char stamp[32];
	struct timespec ts;
	struct tm tm;
	va_list ap;

	if (!ctx)
		return;
	if (level == IL_LOG_DEBUG && !ctx->verbose)
		return;
	if (level < IL_LOG_DEBUG || level > IL_LOG_ERROR)
		level = IL_LOG_INFO;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
	snprintf(line, sizeof(line), "%s.%03ld [installer] %s: %s", stamp,
		 ts.tv_nsec / 1000000L, names[level], msg);

	if (ctx->log)
		fprintf(ctx->log, "%s\n", line);
	if (ctx->log_cb)
		ctx->log_cb(level, line, ctx->log_user);
}

/* ------------------------------------------------------------------ */
/* Sizes and names                                                     */
/* ------------------------------------------------------------------ */

void il_format_size(uint64_t bytes, char *buf, size_t len)
{
	static const char *const units[] = { "KB", "MB", "GB", "TB", "PB" };
	uint64_t div = 1000;
	uint64_t tenths;
	int u = 0;

	if (!buf || !len)
		return;
	if (bytes < 1000) {
		snprintf(buf, len, "%llu bytes", (unsigned long long)bytes);
		return;
	}
	/* integer arithmetic: the same text in every locale */
	while (u < 4 && bytes / div >= 1000) {
		div *= 1000;
		u++;
	}
	tenths = (bytes * 10 + div / 2) / div;
	if (bytes > UINT64_MAX / 10)
		tenths = (bytes / div) * 10 + (bytes % div) * 10 / div;
	snprintf(buf, len, "%llu.%llu %s", (unsigned long long)(tenths / 10),
		 (unsigned long long)(tenths % 10), units[u]);
}

static const char *transport_name(int t)
{
	switch (t) {
	case IL_TRANSPORT_SATA: return "SATA";
	case IL_TRANSPORT_NVME: return "NVMe";
	case IL_TRANSPORT_USB:  return "USB";
	case IL_TRANSPORT_FILE: return "Image file";
	default:                return "Disk";
	}
}

void il_disk_describe(const struct il_disk *d, int index, char *buf, size_t len)
{
	char size[32];

	if (!buf || !len)
		return;
	if (!d) {
		buf[0] = '\0';
		return;
	}
	il_format_size(d->size_bytes, size, sizeof(size));
	snprintf(buf, len, "Disk %d \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %s", index,
		 d->model[0] ? d->model : d->name, size,
		 transport_name(d->transport));
}

/* ------------------------------------------------------------------ */
/* Partition types                                                     */
/* ------------------------------------------------------------------ */

static const struct {
	const char *guid;
	const char *desc;
} g_types[] = {
	{ IL_GUID_ESP,          "EFI system" },
	{ IL_GUID_LIKEOS_ROOT,  "LikeOS root" },
	{ IL_GUID_GENERIC_DATA, "Generic data" },
	{ IL_GUID_BASIC_DATA,   "Basic data" },
	{ IL_GUID_MSR,          "Reserved" },
	{ IL_GUID_SWAP,         "Swap" },
};

static const char *type_desc(const char *guid)
{
	for (size_t i = 0; i < sizeof(g_types) / sizeof(g_types[0]); i++)
		if (strcmp(g_types[i].guid, guid) == 0)
			return g_types[i].desc;
	return "Unknown";
}

static const char *mbr_type_desc(int t)
{
	switch (t) {
	case 0x07: return "NTFS / exFAT";
	case 0x0b:
	case 0x0c: return "FAT32";
	case 0x0e: return "FAT16";
	case 0x82: return "Swap";
	case 0x83: return "Data";
	case 0xef: return "EFI system";
	default:   return "Primary";
	}
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	if (!cap)
		return;
	snprintf(dst, cap, "%s", src ? src : "");
}

/* A random-looking but repeatable GUID, so screenshots are stable. */
static void make_guid(unsigned int *seed, char out[37])
{
	unsigned int a = rand_r(seed), b = rand_r(seed), c = rand_r(seed);
	unsigned int d = rand_r(seed), e = rand_r(seed);

	snprintf(out, 37, "%08x-%04x-4%03x-%04x-%04x%08x",
		 a, b & 0xffff, c & 0x0fff, (d & 0x3fff) | 0x8000,
		 e & 0xffff, a ^ (b << 7) ^ c);
}

static void set_part_type(struct il_partition *p, const char *guid)
{
	copy_str(p->type_guid, sizeof(p->type_guid), guid);
	copy_str(p->type_desc, sizeof(p->type_desc), type_desc(guid));
	p->is_esp = strcmp(guid, IL_GUID_ESP) == 0;
	p->is_likeos_root = strcmp(guid, IL_GUID_LIKEOS_ROOT) == 0;
}

/* ------------------------------------------------------------------ */
/* Free space                                                          */
/* ------------------------------------------------------------------ */

static uint64_t align_up(uint64_t v)
{
	return (v + IL_ALIGN_BYTES - 1) / IL_ALIGN_BYTES * IL_ALIGN_BYTES;
}

static uint64_t align_down(uint64_t v)
{
	return v / IL_ALIGN_BYTES * IL_ALIGN_BYTES;
}

static int part_cmp(const void *a, const void *b)
{
	const struct il_partition *pa = a, *pb = b;

	if (pa->start_bytes != pb->start_bytes)
		return pa->start_bytes < pb->start_bytes ? -1 : 1;
	return pa->number - pb->number;
}

/* The usable area of a disk: everything between the first MiB and the
 * GPT's backup copy at the end (or the end itself for MBR). */
static void usable_area(const struct il_disk *d, uint64_t *first, uint64_t *end)
{
	*first = IL_ALIGN_BYTES;
	if (d->table == IL_TABLE_MBR)
		*end = align_down(d->size_bytes);
	else
		*end = align_down(d->size_bytes - GPT_TAIL_BYTES);
}

static void compute_free(struct il_disk *d)
{
	struct il_partition sorted[IL_MAX_PARTS];
	uint64_t first, end, cur;

	memcpy(sorted, d->parts, sizeof(d->parts[0]) * (size_t)d->nparts);
	qsort(sorted, (size_t)d->nparts, sizeof(sorted[0]), part_cmp);
	memcpy(d->parts, sorted, sizeof(d->parts[0]) * (size_t)d->nparts);

	usable_area(d, &first, &end);
	d->nfree = 0;
	cur = first;
	for (int i = 0; i <= d->nparts; i++) {
		uint64_t next = i < d->nparts ? d->parts[i].start_bytes : end;
		uint64_t s = align_up(cur);
		uint64_t e = align_down(next);

		if (e > s && e - s >= IL_ALIGN_BYTES && d->nfree < IL_MAX_FREE) {
			d->free[d->nfree].start_bytes = s;
			d->free[d->nfree].size_bytes = e - s;
			d->nfree++;
		}
		if (i < d->nparts) {
			uint64_t pe = d->parts[i].start_bytes + d->parts[i].size_bytes;

			if (pe > cur)
				cur = pe;
		}
	}
}

static void part_dev_name(const struct il_disk *d, int number, char *out,
			  size_t len)
{
	size_t n = strlen(d->dev);

	if (n && isdigit((unsigned char)d->dev[n - 1]))
		snprintf(out, len, "%.56sp%d", d->dev, number);
	else
		snprintf(out, len, "%.56s%d", d->dev, number);
}

static struct il_partition *add_part(struct il_disk *d, unsigned int *seed,
				     int number, uint64_t start, uint64_t size,
				     const char *type, const char *name,
				     const char *fstype, const char *label,
				     uint64_t used)
{
	struct il_partition *p;

	if (d->nparts >= IL_MAX_PARTS)
		return NULL;
	p = &d->parts[d->nparts++];
	memset(p, 0, sizeof(*p));
	p->number = number;
	part_dev_name(d, number, p->dev, sizeof(p->dev));
	p->start_bytes = start;
	p->size_bytes = size;
	if (type)
		set_part_type(p, type);
	make_guid(seed, p->part_guid);
	copy_str(p->name, sizeof(p->name), name);
	copy_str(p->fstype, sizeof(p->fstype), fstype);
	copy_str(p->fs_label, sizeof(p->fs_label), label);
	if (fstype && *fstype) {
		if (strcmp(fstype, "vfat") == 0)
			snprintf(p->fs_uuid, sizeof(p->fs_uuid), "%04X-%04X",
				 rand_r(seed) & 0xffff, rand_r(seed) & 0xffff);
		else
			make_guid(seed, p->fs_uuid);
		p->fs_free_bytes = used < size ? size - used : 0;
	}
	return p;
}

/* ------------------------------------------------------------------ */
/* Disks                                                               */
/* ------------------------------------------------------------------ */

static void disk_init(struct il_disk *d, const char *name, const char *model,
		      const char *serial, int transport, uint64_t size,
		      uint32_t lss, uint32_t pss, int table)
{
	memset(d, 0, sizeof(*d));
	copy_str(d->name, sizeof(d->name), name);
	snprintf(d->dev, sizeof(d->dev), "/dev/%s", name);
	copy_str(d->model, sizeof(d->model), model);
	copy_str(d->serial, sizeof(d->serial), serial);
	d->transport = transport;
	d->size_bytes = size;
	d->sector_size = lss;
	d->phys_sector_size = pss;
	d->table = table;
}

int il_disks_scan(il_ctx *ctx, struct il_disk_list *out)
{
	unsigned int seed = 0x1ce05;
	struct il_disk *d;
	const char *no_disks = getenv("IL_MOCK_NO_DISKS");

	if (!out) {
		set_error(ctx, "No place to store the list of disks.");
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	il_log(ctx, IL_LOG_INFO, "scanning disks");

	/* A short pause, like a real scan of a few devices. */
	struct timespec pause = { 0, 450L * 1000000L };

	nanosleep(&pause, NULL);

	if (!(no_disks && *no_disks == '1')) {
		/* 0: a blank NVMe drive. */
		d = &out->disks[out->n++];
		disk_init(d, "nvme0n1", "LikeOS Virtual NVMe Disk",
			  "LVN-0001-7F3A", IL_TRANSPORT_NVME,
			  64000000000ULL, 512, 4096, IL_TABLE_NONE);
		compute_free(d);

		/* 1: a SATA drive with a GPT and four partitions. */
		d = &out->disks[out->n++];
		disk_init(d, "sda", "Generic SATA SSD 256G", "GS256-20260311",
			  IL_TRANSPORT_SATA, 256060514304ULL, 512, 4096,
			  IL_TABLE_GPT);
		make_guid(&seed, d->disk_guid);
		add_part(d, &seed, 1, 1 * MIB, 260 * MIB, IL_GUID_ESP,
			 "EFI system partition", "vfat", "SYSTEM", 38 * MIB);
		add_part(d, &seed, 2, 261 * MIB, 16 * MIB, IL_GUID_MSR,
			 "Reserved", "", "", 0);
		add_part(d, &seed, 3, 277 * MIB, 180000 * MIB,
			 IL_GUID_BASIC_DATA, "Data", "ntfs", "Data",
			 61200 * MIB);
		add_part(d, &seed, 4, 180277 * MIB, 48000 * MIB,
			 IL_GUID_GENERIC_DATA, "home", "ext4", "home",
			 9100 * MIB);
		compute_free(d);

		/* 2: a legacy MBR drive. */
		d = &out->disks[out->n++];
		disk_init(d, "sdb", "Generic Hard Disk 32G", "GHD32-118844",
			  IL_TRANSPORT_SATA, 32017047552ULL, 512, 512,
			  IL_TABLE_MBR);
		{
			struct il_partition *p;

			p = add_part(d, &seed, 1, 1 * MIB, 20480 * MIB, NULL,
				     "", "ntfs", "Archive", 15300 * MIB);
			p->mbr_type = 0x07;
			copy_str(p->type_desc, sizeof(p->type_desc),
				 mbr_type_desc(0x07));
			p = add_part(d, &seed, 2, 20481 * MIB, 8192 * MIB, NULL,
				     "", "vfat", "PHOTOS", 2100 * MIB);
			p->mbr_type = 0x0c;
			copy_str(p->type_desc, sizeof(p->type_desc),
				 mbr_type_desc(0x0c));
		}
		compute_free(d);
	}

	/* 3: the USB stick LikeOS was started from. */
	d = &out->disks[out->n++];
	disk_init(d, "sdc", "USB Flash Drive", "UFD-9921-0042",
		  IL_TRANSPORT_USB, 15376318464ULL, 512, 512, IL_TABLE_GPT);
	d->removable = 1;
	d->is_boot_medium = 1;
	make_guid(&seed, d->disk_guid);
	add_part(d, &seed, 1, 1 * MIB, 64 * MIB, IL_GUID_ESP,
		 "EFI system partition", "vfat", "LIKEOS-ESP", 12 * MIB);
	add_part(d, &seed, 2, 65 * MIB, 14600 * MIB, IL_GUID_LIKEOS_ROOT,
		 "likeos-live", "ext4", "likeos-live", 9800 * MIB);
	compute_free(d);

	for (int i = 0; i < out->n; i++) {
		char desc[160];

		il_disk_describe(&out->disks[i], i, desc, sizeof(desc));
		il_log(ctx, IL_LOG_INFO, "found %s%s", desc,
		       out->disks[i].is_boot_medium ? " (boot medium)" : "");
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* The system image                                                    */
/* ------------------------------------------------------------------ */

int il_source_probe(il_ctx *ctx, struct il_source_info *out)
{
	const char *missing = getenv("IL_MOCK_NO_SOURCE");

	if (!out) {
		set_error(ctx, "No place to store the image information.");
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	copy_str(out->path, sizeof(out->path),
		 ctx && ctx->source ? ctx->source : IL_DEFAULT_SOURCE);
	if (missing && *missing == '1') {
		set_error(ctx, "The system image %s was not found on the "
			  "installation medium.", out->path);
		return -ENOENT;
	}
	out->present = 1;
	copy_str(out->version, sizeof(out->version), "2026.11");
	copy_str(out->build_date, sizeof(out->build_date), "2026-11-13");
	out->compressed_bytes = 1874329600ULL;
	out->uncompressed_bytes = 6318451712ULL;
	out->file_count = 148213;
	copy_str(out->sha256, sizeof(out->sha256),
		 "5f1c0b2e9d7a4c3b8e6f0a1d2c3b4a5968778695a4b3c2d1e0f9e8d7c6b5a493");
	out->required_bytes = out->uncompressed_bytes +
		out->uncompressed_bytes / 4 + GIB;
	il_log(ctx, IL_LOG_INFO, "system image %s: version %s, %llu files",
	       out->path, out->version, (unsigned long long)out->file_count);
	return 0;
}

static void mock_sleep(const il_ctx *ctx, long ms)
{
	double s = ctx ? ctx->speed : 1.0;
	long scaled = (long)((double)ms / s);
	struct timespec ts;

	if (scaled < 1)
		scaled = 1;
	ts.tv_sec = scaled / 1000;
	ts.tv_nsec = (scaled % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

int il_source_verify(il_ctx *ctx, void (*progress)(double fraction, void *user),
		     void *user)
{
	il_log(ctx, IL_LOG_INFO, "verifying the system image");
	for (int i = 0; i <= 40; i++) {
		if (progress)
			progress(i / 40.0, user);
		mock_sleep(ctx, 50);
	}
	il_log(ctx, IL_LOG_INFO, "system image checksum matches");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Partitioning plan                                                   */
/* ------------------------------------------------------------------ */

struct il_plan {
	il_ctx *ctx;
	struct il_disk orig;      /* the scan result */
	struct il_disk view;      /* orig with every operation replayed */
	int nops;                 /* operations the user asked for */
	struct il_op ops[MOCK_MAX_OPS];
	int nall;                 /* nops + the ones the root choice implies */
	struct il_op all[MOCK_MAX_OPS + 4];
	int root;
	unsigned int seed;
};

static struct il_partition *view_part(struct il_disk *d, int partno)
{
	for (int i = 0; i < d->nparts; i++)
		if (d->parts[i].number == partno)
			return &d->parts[i];
	return NULL;
}

static void view_remove(struct il_disk *d, int partno)
{
	for (int i = 0; i < d->nparts; i++) {
		if (d->parts[i].number == partno) {
			memmove(&d->parts[i], &d->parts[i + 1],
				sizeof(d->parts[0]) * (size_t)(d->nparts - i - 1));
			d->nparts--;
			return;
		}
	}
}

static void apply_op(struct il_plan *pl, struct il_disk *d,
		     const struct il_op *op, unsigned int *seed)
{
	struct il_partition *p;

	switch (op->kind) {
	case IL_OP_NEW_TABLE:
		d->nparts = 0;
		d->table = IL_TABLE_GPT;
		make_guid(seed, d->disk_guid);
		break;
	case IL_OP_DELETE:
		view_remove(d, op->partno);
		break;
	case IL_OP_CREATE:
		p = add_part(d, seed, op->partno, op->start_bytes,
			     op->size_bytes, op->type_guid, op->name, "", "", 0);
		(void)p;
		break;
	case IL_OP_EXTEND:
		p = view_part(d, op->partno);
		if (p) {
			if (p->fs_free_bytes)
				p->fs_free_bytes += op->size_bytes - p->size_bytes;
			p->size_bytes = op->size_bytes;
		}
		break;
	case IL_OP_FORMAT:
		p = view_part(d, op->partno);
		if (p) {
			int vfat = strcmp(op->fstype, "vfat") == 0;
			uint64_t overhead = vfat ? p->size_bytes / 200 :
				p->size_bytes / 60 + 64 * MIB;

			copy_str(p->fstype, sizeof(p->fstype), op->fstype);
			copy_str(p->fs_label, sizeof(p->fs_label),
				 vfat ? IL_ESP_LABEL :
				 (p->number == pl->root ? IL_ROOT_LABEL : ""));
			if (vfat)
				snprintf(p->fs_uuid, sizeof(p->fs_uuid),
					 "%04X-%04X", rand_r(seed) & 0xffff,
					 rand_r(seed) & 0xffff);
			else
				make_guid(seed, p->fs_uuid);
			p->fs_free_bytes = overhead < p->size_bytes ?
				p->size_bytes - overhead : 0;
		}
		break;
	case IL_OP_SET_TYPE:
		p = view_part(d, op->partno);
		if (p) {
			set_part_type(p, op->type_guid);
			if (op->name[0])
				copy_str(p->name, sizeof(p->name), op->name);
		}
		break;
	default:
		break;
	}
	compute_free(d);
}

static int has_op(const struct il_plan *pl, int kind, int partno)
{
	for (int i = 0; i < pl->nops; i++)
		if (pl->ops[i].kind == kind && pl->ops[i].partno == partno)
			return 1;
	return 0;
}

/* Rebuilds the full operation list and the projected view. */
static void plan_rebuild(struct il_plan *pl)
{
	unsigned int seed = pl->seed;
	struct il_partition *rp;

	pl->view = pl->orig;
	pl->nall = 0;
	for (int i = 0; i < pl->nops; i++) {
		pl->all[pl->nall++] = pl->ops[i];
		apply_op(pl, &pl->view, &pl->ops[i], &seed);
	}

	/* The root partition is always formatted and gets the LikeOS root
	 * type; say so in the list unless the user queued it already. */
	if (pl->root > 0 && (rp = view_part(&pl->view, pl->root)) != NULL) {
		if (!rp->is_likeos_root && pl->view.table == IL_TABLE_GPT) {
			struct il_op op = { 0 };

			op.kind = IL_OP_SET_TYPE;
			op.partno = pl->root;
			copy_str(op.type_guid, sizeof(op.type_guid),
				 IL_GUID_LIKEOS_ROOT);
			copy_str(op.name, sizeof(op.name), IL_ROOT_PARTNAME);
			pl->all[pl->nall++] = op;
			apply_op(pl, &pl->view, &op, &seed);
		}
		if (!has_op(pl, IL_OP_FORMAT, pl->root)) {
			struct il_op op = { 0 };

			op.kind = IL_OP_FORMAT;
			op.partno = pl->root;
			copy_str(op.fstype, sizeof(op.fstype), "ext4");
			pl->all[pl->nall++] = op;
			apply_op(pl, &pl->view, &op, &seed);
		}
		rp = view_part(&pl->view, pl->root);
		if (rp)
			copy_str(rp->fs_label, sizeof(rp->fs_label),
				 IL_ROOT_LABEL);
	}
}

static int plan_push(struct il_plan *pl, const struct il_op *op)
{
	if (pl->nops >= MOCK_MAX_OPS) {
		set_error(pl->ctx, "Too many changes are queued; undo some "
			  "of them first.");
		return -E2BIG;
	}
	pl->ops[pl->nops++] = *op;
	return 0;
}

/* Drops every queued operation for a partition (used when it is deleted
 * again or replaced). */
static void plan_forget(struct il_plan *pl, int partno, int keep_create)
{
	int j = 0;

	for (int i = 0; i < pl->nops; i++) {
		const struct il_op *op = &pl->ops[i];

		if (op->partno == partno && op->kind != IL_OP_NEW_TABLE &&
		    !(keep_create && op->kind == IL_OP_CREATE))
			continue;
		pl->ops[j++] = *op;
	}
	pl->nops = j;
}

static int created_by_plan(const struct il_plan *pl, int partno)
{
	return has_op(pl, IL_OP_CREATE, partno);
}

static int lowest_free_number(const struct il_disk *d)
{
	int max = d->table == IL_TABLE_MBR ? 4 : IL_MAX_PARTS;

	for (int n = 1; n <= max; n++) {
		int used = 0;

		for (int i = 0; i < d->nparts; i++)
			if (d->parts[i].number == n)
				used = 1;
		if (!used)
			return n;
	}
	return -1;
}

il_plan *il_plan_new(il_ctx *ctx, const struct il_disk *disk)
{
	il_plan *pl;

	if (!disk) {
		set_error(ctx, "No disk was given.");
		return NULL;
	}
	pl = calloc(1, sizeof(*pl));
	if (!pl) {
		set_error(ctx, "Out of memory.");
		return NULL;
	}
	pl->ctx = ctx;
	pl->orig = *disk;
	compute_free(&pl->orig);
	pl->root = -1;
	pl->seed = 0x5eed ^ (unsigned int)disk->size_bytes;
	plan_rebuild(pl);
	return pl;
}

void il_plan_free(il_plan *plan)
{
	free(plan);
}

static int plan_check_disk(il_plan *pl)
{
	if (pl->orig.is_boot_medium) {
		set_error(pl->ctx, "%s holds the running LikeOS and cannot be "
			  "changed.", pl->orig.dev);
		return -EBUSY;
	}
	if (pl->orig.read_only) {
		set_error(pl->ctx, "%s is write-protected.", pl->orig.dev);
		return -EROFS;
	}
	return 0;
}

int il_plan_new_table(il_plan *pl)
{
	struct il_op op = { 0 };
	int rc;

	if (!pl)
		return -EINVAL;
	if ((rc = plan_check_disk(pl)) != 0)
		return rc;
	pl->nops = 0;
	pl->root = -1;
	op.kind = IL_OP_NEW_TABLE;
	plan_push(pl, &op);
	plan_rebuild(pl);
	il_log(pl->ctx, IL_LOG_INFO, "plan %s: new GPT", pl->orig.dev);
	return 0;
}

static int plan_create_at(il_plan *pl, uint64_t start, uint64_t size,
			  const char *type, const char *name,
			  const char *fstype, int *partno_out)
{
	struct il_op op = { 0 };
	int n = lowest_free_number(&pl->view);
	int rc;

	if (n < 0) {
		set_error(pl->ctx, "The partition table of %s is full.",
			  pl->orig.dev);
		return -ENOSPC;
	}
	op.kind = IL_OP_CREATE;
	op.partno = n;
	op.start_bytes = start;
	op.size_bytes = size;
	copy_str(op.type_guid, sizeof(op.type_guid), type);
	copy_str(op.name, sizeof(op.name), name);
	if ((rc = plan_push(pl, &op)) != 0)
		return rc;
	plan_rebuild(pl);
	if (fstype) {
		struct il_op f = { 0 };

		f.kind = IL_OP_FORMAT;
		f.partno = n;
		copy_str(f.fstype, sizeof(f.fstype), fstype);
		if ((rc = plan_push(pl, &f)) != 0)
			return rc;
		plan_rebuild(pl);
	}
	if (partno_out)
		*partno_out = n;
	return 0;
}

int il_plan_use_whole_disk(il_plan *pl)
{
	uint64_t first, end;
	int rc, root;

	if (!pl)
		return -EINVAL;
	if ((rc = il_plan_new_table(pl)) != 0)
		return rc;
	usable_area(&pl->view, &first, &end);
	if (end - first < IL_ESP_SIZE_BYTES + 2 * GIB) {
		set_error(pl->ctx, "%s is too small for LikeOS.", pl->orig.dev);
		return -ENOSPC;
	}
	rc = plan_create_at(pl, first, IL_ESP_SIZE_BYTES, IL_GUID_ESP,
			    IL_ESP_PARTNAME, "vfat", NULL);
	if (rc)
		return rc;
	rc = plan_create_at(pl, first + IL_ESP_SIZE_BYTES,
			    end - first - IL_ESP_SIZE_BYTES,
			    IL_GUID_LIKEOS_ROOT, IL_ROOT_PARTNAME, "ext4", &root);
	if (rc)
		return rc;
	pl->root = root;
	plan_rebuild(pl);
	il_log(pl->ctx, IL_LOG_INFO, "plan %s: whole disk, root partition %d",
	       pl->orig.dev, root);
	return 0;
}

int il_plan_delete(il_plan *pl, int partno)
{
	struct il_op op = { 0 };
	struct il_partition *p;
	int rc;

	if (!pl)
		return -EINVAL;
	if ((rc = plan_check_disk(pl)) != 0)
		return rc;
	p = view_part(&pl->view, partno);
	if (!p) {
		set_error(pl->ctx, "Partition %d does not exist.", partno);
		return -ENOENT;
	}
	if (pl->root == partno)
		pl->root = -1;
	if (created_by_plan(pl, partno)) {
		plan_forget(pl, partno, 0);
		plan_rebuild(pl);
		il_log(pl->ctx, IL_LOG_INFO, "plan %s: dropped new partition %d",
		       pl->orig.dev, partno);
		return 0;
	}
	plan_forget(pl, partno, 0);
	op.kind = IL_OP_DELETE;
	op.partno = partno;
	op.start_bytes = p->start_bytes;
	op.size_bytes = p->size_bytes;
	if ((rc = plan_push(pl, &op)) != 0)
		return rc;
	plan_rebuild(pl);
	il_log(pl->ctx, IL_LOG_INFO, "plan %s: delete partition %d",
	       pl->orig.dev, partno);
	return 0;
}

int il_plan_create(il_plan *pl, int free_index, uint64_t size_bytes,
		   int *partno_out)
{
	struct il_free_region r;
	int rc, need_esp = 1;

	if (!pl)
		return -EINVAL;
	if ((rc = plan_check_disk(pl)) != 0)
		return rc;
	if (pl->view.table == IL_TABLE_MBR) {
		set_error(pl->ctx, "%s uses a legacy MBR partition table. "
			  "LikeOS needs a GPT: convert the disk first (this "
			  "erases it).", pl->orig.dev);
		return -EOPNOTSUPP;
	}
	if (free_index < 0 || free_index >= pl->view.nfree) {
		set_error(pl->ctx, "There is no such free space on %s.",
			  pl->orig.dev);
		return -EINVAL;
	}
	r = pl->view.free[free_index];
	if (pl->view.table == IL_TABLE_NONE) {
		struct il_op op = { 0 };

		op.kind = IL_OP_NEW_TABLE;
		if ((rc = plan_push(pl, &op)) != 0)
			return rc;
		plan_rebuild(pl);
		if (pl->view.nfree < 1)
			return -ENOSPC;
		r = pl->view.free[0];
	}
	for (int i = 0; i < pl->view.nparts; i++)
		if (pl->view.parts[i].is_esp)
			need_esp = 0;

	size_bytes = align_down(size_bytes);
	if (need_esp) {
		int esp;

		if (r.size_bytes < IL_ESP_SIZE_BYTES + 64 * MIB) {
			set_error(pl->ctx, "There is not enough room for an EFI "
				  "system partition and a second partition "
				  "here.");
			return -ENOSPC;
		}
		rc = plan_create_at(pl, r.start_bytes, IL_ESP_SIZE_BYTES,
				    IL_GUID_ESP, IL_ESP_PARTNAME, "vfat", &esp);
		if (rc)
			return rc;
		il_log(pl->ctx, IL_LOG_INFO, "plan %s: EFI system partition %d "
		       "added in front", pl->orig.dev, esp);
		r.start_bytes += IL_ESP_SIZE_BYTES;
		r.size_bytes -= IL_ESP_SIZE_BYTES;
	}
	if (size_bytes == 0 || size_bytes > r.size_bytes) {
		if (size_bytes > r.size_bytes + IL_ALIGN_BYTES && !need_esp) {
			char have[32];

			il_format_size(r.size_bytes, have, sizeof(have));
			set_error(pl->ctx, "Only %s of free space is available "
				  "there.", have);
			return -ENOSPC;
		}
		size_bytes = r.size_bytes;
	}
	if (size_bytes < IL_ALIGN_BYTES) {
		set_error(pl->ctx, "A partition must be at least 1 MB.");
		return -EINVAL;
	}
	/* A plain data partition, as libinstaller makes it; choosing it
	 * for LikeOS gives it the LikeOS type (il_plan_select_root). */
	rc = plan_create_at(pl, r.start_bytes, size_bytes, IL_GUID_GENERIC_DATA,
			    "", NULL, partno_out);
	if (rc == 0)
		il_log(pl->ctx, IL_LOG_INFO, "plan %s: create partition (%llu "
		       "MiB)", pl->orig.dev,
		       (unsigned long long)(size_bytes / MIB));
	return rc;
}

int il_plan_extend(il_plan *pl, int partno, uint64_t new_size_bytes)
{
	struct il_partition *p;
	uint64_t end, room = 0;
	struct il_op op = { 0 };
	int rc;

	if (!pl)
		return -EINVAL;
	if ((rc = plan_check_disk(pl)) != 0)
		return rc;
	p = view_part(&pl->view, partno);
	if (!p) {
		set_error(pl->ctx, "Partition %d does not exist.", partno);
		return -ENOENT;
	}
	end = align_up(p->start_bytes + p->size_bytes);
	for (int i = 0; i < pl->view.nfree; i++)
		if (pl->view.free[i].start_bytes == end)
			room = pl->view.free[i].size_bytes;
	if (!room) {
		set_error(pl->ctx, "There is no free space right after "
			  "partition %d.", partno);
		return -ENOSPC;
	}
	new_size_bytes = align_down(new_size_bytes);
	if (new_size_bytes <= p->size_bytes) {
		set_error(pl->ctx, "The new size must be larger than the "
			  "current one.");
		return -EINVAL;
	}
	if (new_size_bytes > (end - p->start_bytes) + room)
		new_size_bytes = (end - p->start_bytes) + room;
	op.kind = IL_OP_EXTEND;
	op.partno = partno;
	op.start_bytes = p->start_bytes;
	op.size_bytes = new_size_bytes;
	if ((rc = plan_push(pl, &op)) != 0)
		return rc;
	plan_rebuild(pl);
	il_log(pl->ctx, IL_LOG_INFO, "plan %s: extend partition %d to %llu MiB",
	       pl->orig.dev, partno,
	       (unsigned long long)(new_size_bytes / MIB));
	return 0;
}

int il_plan_format(il_plan *pl, int partno)
{
	struct il_partition *p;
	struct il_op op = { 0 };
	int rc;

	if (!pl)
		return -EINVAL;
	if ((rc = plan_check_disk(pl)) != 0)
		return rc;
	p = view_part(&pl->view, partno);
	if (!p) {
		set_error(pl->ctx, "Partition %d does not exist.", partno);
		return -ENOENT;
	}
	if (has_op(pl, IL_OP_FORMAT, partno))
		return 0;
	if (p->is_esp && p->size_bytes < 32 * MIB) {
		set_error(pl->ctx, "Partition %d is too small for FAT32.",
			  partno);
		return -ENOSPC;
	}
	op.kind = IL_OP_FORMAT;
	op.partno = partno;
	copy_str(op.fstype, sizeof(op.fstype), p->is_esp ? "vfat" : "ext4");
	if ((rc = plan_push(pl, &op)) != 0)
		return rc;
	plan_rebuild(pl);
	il_log(pl->ctx, IL_LOG_INFO, "plan %s: format partition %d as %s",
	       pl->orig.dev, partno, op.fstype);
	return 0;
}

/* The CREATE of a partition the plan makes gets another type and name
 * (the way libinstaller marks a new partition as the root one). */
static void retype_created(struct il_plan *pl, int partno, const char *guid,
			   const char *name)
{
	for (int i = 0; i < pl->nops; i++) {
		if (pl->ops[i].kind == IL_OP_CREATE && pl->ops[i].partno == partno) {
			copy_str(pl->ops[i].type_guid, sizeof(pl->ops[i].type_guid),
				 guid);
			copy_str(pl->ops[i].name, sizeof(pl->ops[i].name), name);
		}
	}
}

int il_plan_select_root(il_plan *pl, int partno)
{
	struct il_partition *p;
	int rc;

	if (!pl)
		return -EINVAL;
	if ((rc = plan_check_disk(pl)) != 0)
		return rc;
	p = view_part(&pl->view, partno);
	if (!p) {
		set_error(pl->ctx, "Partition %d does not exist.", partno);
		return -ENOENT;
	}
	if (p->is_esp) {
		set_error(pl->ctx, "LikeOS cannot be installed on the EFI "
			  "system partition.");
		return -EINVAL;
	}
	if (p->size_bytes < 2 * GIB) {
		set_error(pl->ctx, "Partition %d is too small for LikeOS.",
			  partno);
		return -ENOSPC;
	}
	if (pl->root > 0 && pl->root != partno)
		retype_created(pl, pl->root, IL_GUID_GENERIC_DATA, "");
	if (created_by_plan(pl, partno))
		retype_created(pl, partno, IL_GUID_LIKEOS_ROOT, IL_ROOT_PARTNAME);
	pl->root = partno;
	plan_rebuild(pl);
	il_log(pl->ctx, IL_LOG_INFO, "plan %s: root on partition %d",
	       pl->orig.dev, partno);
	return 0;
}

int il_plan_root(const il_plan *pl)
{
	return pl ? pl->root : -1;
}

int il_plan_esp(const il_plan *pl)
{
	if (!pl)
		return -1;
	for (int i = 0; i < pl->view.nparts; i++)
		if (pl->view.parts[i].is_esp)
			return pl->view.parts[i].number;
	return -1;
}

int il_plan_view(const il_plan *pl, struct il_disk *out)
{
	if (!pl || !out)
		return -EINVAL;
	*out = pl->view;
	return 0;
}

int il_plan_nops(const il_plan *pl)
{
	return pl ? pl->nall : 0;
}

const struct il_op *il_plan_op(const il_plan *pl, int i)
{
	if (!pl || i < 0 || i >= pl->nall)
		return NULL;
	return &pl->all[i];
}

void il_op_describe(const struct il_op *op, char *buf, size_t len)
{
	char size[32];

	if (!buf || !len)
		return;
	if (!op) {
		buf[0] = '\0';
		return;
	}
	il_format_size(op->size_bytes, size, sizeof(size));
	switch (op->kind) {
	case IL_OP_NEW_TABLE:
		snprintf(buf, len, "Create a new GPT partition table "
			 "(erases the whole disk)");
		break;
	case IL_OP_DELETE:
		snprintf(buf, len, "Delete partition %d (%s)", op->partno, size);
		break;
	case IL_OP_CREATE:
		snprintf(buf, len, "Create partition %d (%s, %s)", op->partno,
			 size, type_desc(op->type_guid));
		break;
	case IL_OP_EXTEND:
		snprintf(buf, len, "Extend partition %d to %s", op->partno, size);
		break;
	case IL_OP_FORMAT:
		snprintf(buf, len, "Format partition %d as %s", op->partno,
			 strcmp(op->fstype, "vfat") == 0 ? "FAT32" : op->fstype);
		break;
	case IL_OP_SET_TYPE:
		snprintf(buf, len, "Mark partition %d as %s", op->partno,
			 type_desc(op->type_guid));
		break;
	default:
		snprintf(buf, len, "Unknown operation %d", op->kind);
		break;
	}
}

int il_plan_validate(const il_plan *pl, const struct il_source_info *src,
		     char *why, size_t len)
{
	const struct il_partition *root = NULL, *esp = NULL;
	char need[32], have[32];

	if (why && len)
		why[0] = '\0';
	if (!pl)
		return -EINVAL;
	if (pl->orig.is_boot_medium) {
		snprintf(why, len, "This disk holds the running LikeOS. Choose "
			 "another disk.");
		return -EBUSY;
	}
	if (pl->orig.read_only) {
		snprintf(why, len, "This disk is write-protected.");
		return -EROFS;
	}
	if (pl->view.table != IL_TABLE_GPT) {
		snprintf(why, len, "LikeOS needs a GPT partition table on this "
			 "disk. Use the entire disk, or convert it to GPT.");
		return -EINVAL;
	}
	for (int i = 0; i < pl->view.nparts; i++) {
		if (pl->view.parts[i].number == pl->root)
			root = &pl->view.parts[i];
		if (pl->view.parts[i].is_esp && !esp)
			esp = &pl->view.parts[i];
	}
	if (!root) {
		snprintf(why, len, "Choose the partition or the free space "
			 "LikeOS is installed on.");
		return -EINVAL;
	}
	if (!esp) {
		snprintf(why, len, "The disk has no EFI system partition. "
			 "Create a partition in free space and one is added "
			 "for you.");
		return -EINVAL;
	}
	if (esp->size_bytes < 32 * MIB) {
		snprintf(why, len, "The EFI system partition is too small "
			 "(at least 32 MB are needed).");
		return -ENOSPC;
	}
	if (src && src->required_bytes && root->size_bytes < src->required_bytes) {
		il_format_size(src->required_bytes, need, sizeof(need));
		il_format_size(root->size_bytes, have, sizeof(have));
		snprintf(why, len, "The selected partition is too small: LikeOS "
			 "needs at least %s, the partition has %s.", need, have);
		return -ENOSPC;
	}
	return 0;
}

const struct il_disk *il_plan_disk(const il_plan *pl)
{
	return pl ? &pl->orig : NULL;
}

/* ------------------------------------------------------------------ */
/* Choices and validators                                              */
/* ------------------------------------------------------------------ */

void il_config_defaults(struct il_config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	copy_str(cfg->locale, sizeof(cfg->locale), "en_US.UTF-8");
	copy_str(cfg->time_format, sizeof(cfg->time_format), "en_US.UTF-8");
	copy_str(cfg->timezone, sizeof(cfg->timezone), "UTC");
	cfg->nlayouts = 1;
	copy_str(cfg->layouts[0], sizeof(cfg->layouts[0]), "us");
	il_suggest_hostname(cfg->hostname, sizeof(cfg->hostname));
	cfg->user_extra_groups = 1;
}

/* Names the system already uses for its own accounts and groups. */
static const char *const g_reserved_names[] = {
	"root", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail",
	"news", "uucp", "proxy", "www-data", "backup", "list", "irc", "nobody",
	"nogroup", "sshd", "audiod", "messagebus", "render", "video", "input",
	"audio", "users", "tty", "disk", "wheel", "adm", "kmem", "dialout",
	"utmp", "shadow", "staff", "operator", "admin", "halt", "shutdown",
	NULL,
};

int il_validate_hostname(const char *name, char *why, size_t len)
{
	size_t n;
	int digits = 1;

	if (why && len)
		why[0] = '\0';
	if (!name || !*name) {
		snprintf(why, len, "Enter a name for this computer.");
		return -EINVAL;
	}
	n = strlen(name);
	if (n > IL_HOSTNAME_MAX) {
		snprintf(why, len, "The name can have at most %d characters.",
			 IL_HOSTNAME_MAX);
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];

		if (c == ' ') {
			snprintf(why, len, "The name cannot contain spaces.");
			return -EINVAL;
		}
		if (isupper(c)) {
			snprintf(why, len, "Use lower-case letters only.");
			return -EINVAL;
		}
		if (!(islower(c) || isdigit(c) || c == '-')) {
			snprintf(why, len, "Only the letters a-z, the digits 0-9 "
				 "and the hyphen (-) are allowed.");
			return -EINVAL;
		}
		if (!isdigit(c))
			digits = 0;
	}
	if (name[0] == '-' || name[n - 1] == '-') {
		snprintf(why, len, "The name cannot start or end with a hyphen.");
		return -EINVAL;
	}
	if (digits) {
		snprintf(why, len, "The name cannot consist of digits only.");
		return -EINVAL;
	}
	return 0;
}

int il_validate_username(const char *name, char *why, size_t len)
{
	size_t n;

	if (why && len)
		why[0] = '\0';
	if (!name || !*name) {
		snprintf(why, len, "Enter a user name.");
		return -EINVAL;
	}
	n = strlen(name);
	if (n > IL_USERNAME_MAX) {
		snprintf(why, len, "A user name can have at most %d characters.",
			 IL_USERNAME_MAX);
		return -EINVAL;
	}
	if (!(islower((unsigned char)name[0]) || name[0] == '_')) {
		snprintf(why, len, "A user name starts with a lower-case letter "
			 "or an underscore.");
		return -EINVAL;
	}
	for (size_t i = 1; i < n; i++) {
		unsigned char c = (unsigned char)name[i];

		if (!(islower(c) || isdigit(c) || c == '_' || c == '-')) {
			snprintf(why, len, "Only lower-case letters, digits, '_' "
				 "and '-' are allowed in a user name.");
			return -EINVAL;
		}
	}
	for (int i = 0; g_reserved_names[i]; i++) {
		if (strcmp(g_reserved_names[i], name) == 0) {
			snprintf(why, len, "\"%s\" is used by the system. Choose "
				 "another name.", name);
			return -EINVAL;
		}
	}
	return 0;
}

int il_validate_password(const char *pw, char *why, size_t len)
{
	size_t n;

	if (why && len)
		why[0] = '\0';
	if (!pw || !*pw) {
		snprintf(why, len, "Enter a password.");
		return -EINVAL;
	}
	n = strlen(pw);
	if (n > IL_PASSWORD_MAX) {
		snprintf(why, len, "A password can have at most %d characters.",
			 IL_PASSWORD_MAX);
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i++) {
		if (pw[i] == '\n' || pw[i] == '\r') {
			snprintf(why, len, "A password cannot contain a line "
				 "break.");
			return -EINVAL;
		}
	}
	/* Valid, but worth a word: a short password is accepted with a
	 * warning in *why (the wording of libinstaller). */
	if (n < IL_PASSWORD_SHORT)
		snprintf(why, len, "This password is short; %d characters or "
			 "more are safer", IL_PASSWORD_SHORT);
	return 0;
}

int il_password_strength(const char *pw)
{
	int lower = 0, upper = 0, digit = 0, other = 0, classes, score;
	size_t n;

	if (!pw || !*pw)
		return 0;
	n = strlen(pw);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)pw[i];

		if (islower(c))
			lower = 1;
		else if (isupper(c))
			upper = 1;
		else if (isdigit(c))
			digit = 1;
		else
			other = 1;
	}
	classes = lower + upper + digit + other;
	if (n < 6)
		score = 0;
	else if (n < 8)
		score = 1;
	else if (n < 12)
		score = 2;
	else if (n < 16)
		score = 3;
	else
		score = 4;
	if (classes >= 3 && score < 4 && n >= 8)
		score++;
	if (classes == 1 && score > 1)
		score--;
	if (score > 4)
		score = 4;
	return score;
}

void il_suggest_username(const char *fullname, char *buf, size_t len)
{
	size_t o = 0;
	const unsigned char *p = (const unsigned char *)fullname;

	if (!buf || !len)
		return;
	buf[0] = '\0';
	if (!p)
		return;
	while (*p == ' ' || *p == '\t')
		p++;
	/* The first word, folded to the allowed characters. */
	for (; *p && *p != ' ' && *p != '\t' && o + 1 < len &&
	     o < IL_USERNAME_MAX; p++) {
		unsigned char c = *p;

		if (c >= 0x80) {
			/* Common Latin letters with marks, in UTF-8. */
			if (c == 0xc3 && p[1]) {
				static const char map[] =
					"aaaaaaaceeeeiiiidnooooo/ouuuuyty";
				unsigned char t = p[1];

				p++;
				if (t >= 0xa0 && t <= 0xbf) {
					char m = map[t - 0xa0];

					if (m == 'a' && t == 0xa4 && o + 2 < len) {
						buf[o++] = 'a';
						buf[o++] = 'e';
						continue;
					}
					if (m == 'o' && t == 0xb6 && o + 2 < len) {
						buf[o++] = 'o';
						buf[o++] = 'e';
						continue;
					}
					if (m == 'u' && t == 0xbc && o + 2 < len) {
						buf[o++] = 'u';
						buf[o++] = 'e';
						continue;
					}
					if (t == 0x9f && o + 2 < len) {
						buf[o++] = 's';
						buf[o++] = 's';
						continue;
					}
					if (m != '/')
						buf[o++] = m;
				} else if (t >= 0x80 && t <= 0x9f) {
					static const char up[] =
						"aaaaaaaceeeeiiiidnooooo-ouuuuyts";

					if (t == 0x9f && o + 2 < len) {
						buf[o++] = 's';
						buf[o++] = 's';
					} else if (up[t - 0x80] != '-') {
						buf[o++] = up[t - 0x80];
					}
				}
			}
			continue;
		}
		c = (unsigned char)tolower(c);
		if (islower(c) || isdigit(c) || c == '_' || c == '-')
			buf[o++] = (char)c;
	}
	buf[o] = '\0';
	if (o && isdigit((unsigned char)buf[0])) {
		memmove(buf + 1, buf, o + 1 < len ? o + 1 : len - 1);
		buf[0] = '_';
		buf[len - 1] = '\0';
	}
	if (!buf[0])
		return;
	/* A system name gets a digit, so the suggestion is always valid. */
	for (int i = 0; g_reserved_names[i]; i++) {
		if (strcmp(g_reserved_names[i], buf) == 0) {
			size_t l = strlen(buf);

			if (l + 2 < len) {
				buf[l] = '1';
				buf[l + 1] = '\0';
			}
			break;
		}
	}
}

void il_suggest_hostname(char *buf, size_t len)
{
	unsigned int v = 0;
	int fd;

	if (!buf || !len)
		return;
	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
			v = 0;
		close(fd);
	}
	if (!v)
		v = (unsigned int)time(NULL) * 2654435761u;
	snprintf(buf, len, "likeos-%04x", v & 0xffff);
}

/* The C library's zones, with the label a person reads. */
static const struct {
	const char *name;
	const char *label;
} g_zones[] = {
	{ "America/Anchorage",   "(UTC-09:00) Anchorage" },
	{ "America/Los_Angeles", "(UTC-08:00) Los Angeles" },
	{ "America/Vancouver",   "(UTC-08:00) Vancouver" },
	{ "America/Denver",      "(UTC-07:00) Denver" },
	{ "America/Phoenix",     "(UTC-07:00) Phoenix" },
	{ "America/Chicago",     "(UTC-06:00) Chicago" },
	{ "America/Mexico_City", "(UTC-06:00) Mexico City" },
	{ "America/New_York",    "(UTC-05:00) New York" },
	{ "America/Toronto",     "(UTC-05:00) Toronto" },
	{ "America/Bogota",      "(UTC-05:00) Bogota" },
	{ "America/Sao_Paulo",   "(UTC-03:00) Sao Paulo" },
	{ "America/Argentina/Buenos_Aires", "(UTC-03:00) Buenos Aires" },
	{ "UTC",                 "(UTC) Coordinated Universal Time" },
	{ "Europe/London",       "(UTC+00:00) London" },
	{ "Europe/Dublin",       "(UTC+00:00) Dublin" },
	{ "Europe/Lisbon",       "(UTC+00:00) Lisbon" },
	{ "Africa/Lagos",        "(UTC+01:00) Lagos" },
	{ "Europe/Amsterdam",    "(UTC+01:00) Amsterdam" },
	{ "Europe/Berlin",       "(UTC+01:00) Berlin" },
	{ "Europe/Bratislava",   "(UTC+01:00) Bratislava" },
	{ "Europe/Brussels",     "(UTC+01:00) Brussels" },
	{ "Europe/Budapest",     "(UTC+01:00) Budapest" },
	{ "Europe/Copenhagen",   "(UTC+01:00) Copenhagen" },
	{ "Europe/Ljubljana",    "(UTC+01:00) Ljubljana" },
	{ "Europe/Luxembourg",   "(UTC+01:00) Luxembourg" },
	{ "Europe/Madrid",       "(UTC+01:00) Madrid" },
	{ "Europe/Oslo",         "(UTC+01:00) Oslo" },
	{ "Europe/Paris",        "(UTC+01:00) Paris" },
	{ "Europe/Prague",       "(UTC+01:00) Prague" },
	{ "Europe/Rome",         "(UTC+01:00) Rome" },
	{ "Europe/Stockholm",    "(UTC+01:00) Stockholm" },
	{ "Europe/Vienna",       "(UTC+01:00) Vienna" },
	{ "Europe/Warsaw",       "(UTC+01:00) Warsaw" },
	{ "Europe/Zagreb",       "(UTC+01:00) Zagreb" },
	{ "Europe/Zurich",       "(UTC+01:00) Zurich" },
	{ "Africa/Cairo",        "(UTC+02:00) Cairo" },
	{ "Africa/Johannesburg", "(UTC+02:00) Johannesburg" },
	{ "Asia/Jerusalem",      "(UTC+02:00) Jerusalem" },
	{ "Europe/Athens",       "(UTC+02:00) Athens" },
	{ "Europe/Bucharest",    "(UTC+02:00) Bucharest" },
	{ "Europe/Helsinki",     "(UTC+02:00) Helsinki" },
	{ "Europe/Kyiv",         "(UTC+02:00) Kyiv" },
	{ "Europe/Riga",         "(UTC+02:00) Riga" },
	{ "Europe/Sofia",        "(UTC+02:00) Sofia" },
	{ "Europe/Tallinn",      "(UTC+02:00) Tallinn" },
	{ "Europe/Vilnius",      "(UTC+02:00) Vilnius" },
	{ "Europe/Istanbul",     "(UTC+03:00) Istanbul" },
	{ "Europe/Moscow",       "(UTC+03:00) Moscow" },
	{ "Asia/Dubai",          "(UTC+04:00) Dubai" },
	{ "Asia/Kolkata",        "(UTC+05:30) Kolkata" },
	{ "Asia/Singapore",      "(UTC+08:00) Singapore" },
	{ "Asia/Shanghai",       "(UTC+08:00) Shanghai" },
	{ "Asia/Hong_Kong",      "(UTC+08:00) Hong Kong" },
	{ "Australia/Perth",     "(UTC+08:00) Perth" },
	{ "Asia/Seoul",          "(UTC+09:00) Seoul" },
	{ "Asia/Tokyo",          "(UTC+09:00) Tokyo" },
	{ "Australia/Brisbane",  "(UTC+10:00) Brisbane" },
	{ "Australia/Melbourne", "(UTC+10:00) Melbourne" },
	{ "Australia/Sydney",    "(UTC+10:00) Sydney" },
	{ "Pacific/Auckland",    "(UTC+12:00) Auckland" },
};

#define NZONES ((int)(sizeof(g_zones) / sizeof(g_zones[0])))

int il_timezone_count(void)
{
	return NZONES;
}

const char *il_timezone_name(int i)
{
	return i >= 0 && i < NZONES ? g_zones[i].name : NULL;
}

const char *il_timezone_label(int i)
{
	return i >= 0 && i < NZONES ? g_zones[i].label : NULL;
}

static const struct {
	const char *name;
	const char *label;
} g_locales[] = {
	{ "en_US.UTF-8", "English (United States)" },
	{ "en_GB.UTF-8", "English (United Kingdom)" },
};

#define NLOCALES ((int)(sizeof(g_locales) / sizeof(g_locales[0])))

int il_locale_count(void)
{
	return NLOCALES;
}

const char *il_locale_name(int i)
{
	return i >= 0 && i < NLOCALES ? g_locales[i].name : NULL;
}

const char *il_locale_label(int i)
{
	return i >= 0 && i < NLOCALES ? g_locales[i].label : NULL;
}

/* ------------------------------------------------------------------ */
/* Installing                                                          */
/* ------------------------------------------------------------------ */

static const struct {
	const char *key;
	const char *title;
	double weight;
	long ms;           /* simulated duration */
} g_steps[IL_STEP_COUNT] = {
	{ "prepare",    "Getting ready",                 0.01, 2600 },
	{ "partition",  "Partitioning the disk",         0.02, 1100 },
	{ "format",     "Creating file systems",         0.05, 2000 },
	{ "mount",      "Mounting the new system",       0.01,  500 },
	{ "extract",    "Copying LikeOS files",          0.74, 9800 },
	{ "configure",  "Applying your settings",        0.06, 2200 },
	{ "bootloader", "Installing the boot loader",    0.03, 1100 },
	{ "finalize",   "Finishing up",                  0.08, 1600 },
};

const char *il_step_title(int step)
{
	if (step < 0 || step >= IL_STEP_COUNT)
		return "";
	return g_steps[step].title;
}

double il_step_weight(int step)
{
	if (step < 0 || step >= IL_STEP_COUNT)
		return 0.0;
	return g_steps[step].weight;
}

/* What the copy step shows: a walk through a plausible file tree. */
static const char *const g_tree_dirs[] = {
	"/usr/bin", "/usr/lib", "/usr/lib/gtk-3.0/3.0.0/immodules",
	"/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders", "/usr/lib/gstreamer-1.0",
	"/usr/lib/dri", "/usr/lib/xorg/modules/drivers", "/usr/libexec",
	"/usr/share/fonts/truetype", "/usr/share/icons/LikeOS/48x48/apps",
	"/usr/share/icons/LikeOS/24x24/actions", "/usr/share/locale/en_GB",
	"/usr/share/X11/xkb/symbols", "/usr/share/zoneinfo/Europe",
	"/usr/share/themes/LikeOS/gtk-3.0", "/usr/share/man/man1",
	"/usr/share/likeos/keyboard", "/usr/share/luakit/lib", "/etc/likeos",
	"/usr/share/mime/packages", "/usr/lib/webkit2gtk-4.1/injected-bundle",
};

static const char *const g_tree_files[] = {
	"libgtk-3.so.0.2400.52", "libglib-2.0.so.0.8400.0", "libcairo.so.2",
	"libpango-1.0.so.0", "libharfbuzz.so.0", "libwebkit2gtk-4.1.so.0",
	"libjavascriptcoregtk-4.1.so.0", "libgstreamer-1.0.so.0",
	"libX11.so.6.4.0", "libxcb.so.1.1.0", "libfreetype.so.6",
	"libfontconfig.so.1", "libpixman-1.so.0", "libzstd.so.1",
	"libssl.so.3", "libcrypto.so.3", "libcurl.so.4", "libpng16.so.16",
	"libjpeg.so.8", "libsqlite3.so.0", "libxml2.so.2", "libepoxy.so.0",
	"bash", "luakit", "Xorg", "xinit", "ctwm", "mke2fs", "e2fsck",
	"mkfs.fat", "sshd", "ssh", "audiod", "likeos-settings", "taskbar",
	"Selawik-Regular.ttf", "NotoSans-Regular.ttf", "likeos-logo.png",
	"gtk.css", "layouts.tsv", "de", "us", "fr", "Berlin", "Paris",
	"bash.1", "ls.1", "tar.1", "freedesktop.org.xml", "keyboard.conf",
	"libgstplayback.so", "iris_dri.so", "vmwgfx_dri.so", "modesetting_drv.so",
};

static void report(const il_ctx *ctx, il_progress_cb cb, void *user,
		   int step, double frac, const char *detail,
		   uint64_t bytes_done, uint64_t bytes_total,
		   uint64_t files_done, uint64_t files_total,
		   const struct timespec *t0)
{
	struct il_progress p;
	struct timespec now;
	double before = 0.0, elapsed;

	(void)ctx;
	if (!cb)
		return;
	for (int i = 0; i < step; i++)
		before += g_steps[i].weight;
	memset(&p, 0, sizeof(p));
	p.step = step;
	p.step_fraction = frac < 0 ? 0 : frac > 1 ? 1 : frac;
	p.overall = before + g_steps[step].weight * p.step_fraction;
	if (p.overall > 1.0)
		p.overall = 1.0;
	p.detail = detail;
	p.bytes_done = bytes_done;
	p.bytes_total = bytes_total;
	p.files_done = files_done;
	p.files_total = files_total;
	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed = (double)(now.tv_sec - t0->tv_sec) +
		(double)(now.tv_nsec - t0->tv_nsec) / 1e9;
	if (p.overall > 0.03 && elapsed > 1.0)
		p.eta_seconds = (int)(elapsed / p.overall * (1.0 - p.overall) + 0.5);
	else
		p.eta_seconds = -1;
	/* as libinstaller does: the floor, 100 only once everything is done */
	p.percent = (int)(p.overall * 100.0);
	if (p.percent > 99 && !(step == IL_STEP_COUNT - 1 && frac >= 1.0))
		p.percent = 99;
	cb(&p, user);
}

static int should_fail(const il_ctx *ctx, int step)
{
	return ctx->fail_step[0] && strcmp(ctx->fail_step, g_steps[step].key) == 0;
}

static int fail_step(il_ctx *ctx, int step, const char *dev, const char *root)
{
	switch (step) {
	case IL_STEP_PREPARE:
		set_error(ctx, "%s is in use by another program and cannot be "
			  "changed.", dev);
		return -EBUSY;
	case IL_STEP_PARTITION:
		set_error(ctx, "The new partition table could not be written to "
			  "%s: Input/output error.", dev);
		return -EIO;
	case IL_STEP_FORMAT:
		set_error(ctx, "mke2fs failed on %s (exit status 1): Could not "
			  "allocate the inode tables.", root);
		return -EIO;
	case IL_STEP_MOUNT:
		set_error(ctx, "%s could not be mounted on %s: Invalid argument.",
			  root, ctx->target);
		return -EINVAL;
	case IL_STEP_EXTRACT:
		set_error(ctx, "Could not write %s/usr/lib/libwebkit2gtk-4.1.so.0: "
			  "No space left on device.", ctx->target);
		return -ENOSPC;
	case IL_STEP_CONFIGURE:
		set_error(ctx, "Could not write %s/etc/shadow: Read-only file "
			  "system.", ctx->target);
		return -EROFS;
	case IL_STEP_BOOTLOADER:
		set_error(ctx, "Could not copy the boot loader to the EFI system "
			  "partition: Input/output error.");
		return -EIO;
	default:
		set_error(ctx, "Writing the cached data to %s failed: "
			  "Input/output error.", dev);
		return -EIO;
	}
}

static int install_run(il_ctx *ctx, il_plan *plan, const struct il_config *cfg,
		       il_progress_cb cb, void *user)
{
	struct il_source_info src;
	struct il_disk view;
	struct timespec t0;
	char why[256], root_dev[64] = "", esp_dev[64] = "", detail[512];
	const char *dev;
	int rc, root, esp;

	if (!ctx || !plan || !cfg)
		return -EINVAL;
	atomic_store(&ctx->cancel, 0);
	atomic_store(&ctx->can_cancel, 1);
	ctx->last_error[0] = '\0';
	clock_gettime(CLOCK_MONOTONIC, &t0);

	il_source_probe(ctx, &src);
	rc = il_plan_validate(plan, &src, why, sizeof(why));
	if (rc) {
		set_error(ctx, "%s", why);
		atomic_store(&ctx->can_cancel, 0);
		return rc;
	}
	il_plan_view(plan, &view);
	dev = plan->orig.dev;
	root = il_plan_root(plan);
	esp = il_plan_esp(plan);
	for (int i = 0; i < view.nparts; i++) {
		if (view.parts[i].number == root)
			copy_str(root_dev, sizeof(root_dev), view.parts[i].dev);
		if (view.parts[i].number == esp)
			copy_str(esp_dev, sizeof(esp_dev), view.parts[i].dev);
	}
	il_log(ctx, IL_LOG_INFO, "installing on %s: root %s, EFI %s, host %s",
	       dev, root_dev, esp_dev, cfg->hostname);

	for (int step = 0; step < IL_STEP_COUNT; step++) {
		long total = g_steps[step].ms;
		int ticks = (int)(total / 50);
		uint64_t bytes = 0, files = 0;

		if (ticks < 1)
			ticks = 1;
		if (step == IL_STEP_PARTITION)
			atomic_store(&ctx->can_cancel, 0);
		il_log(ctx, IL_LOG_INFO, "step %d: %s", step + 1,
		       g_steps[step].title);

		for (int t = 0; t <= ticks; t++) {
			double f = (double)t / ticks;

			if (atomic_load(&ctx->can_cancel) &&
			    atomic_load(&ctx->cancel)) {
				atomic_store(&ctx->can_cancel, 0);
				set_error(ctx, "The installation was cancelled. "
					  "Nothing was written to %s.", dev);
				return -ECANCELED;
			}
			if (should_fail(ctx, step) && f >= 0.4) {
				atomic_store(&ctx->can_cancel, 0);
				return fail_step(ctx, step, dev,
						 root_dev[0] ? root_dev : dev);
			}

			switch (step) {
			case IL_STEP_PREPARE:
				if (f < 0.35)
					snprintf(detail, sizeof(detail),
						 "Checking the system image");
				else if (f < 0.7)
					snprintf(detail, sizeof(detail),
						 "Checking %s", dev);
				else
					snprintf(detail, sizeof(detail),
						 "Releasing %s", dev);
				break;
			case IL_STEP_PARTITION:
				if (f < 0.6)
					snprintf(detail, sizeof(detail),
						 "Writing the GPT partition table "
						 "to %s", dev);
				else
					snprintf(detail, sizeof(detail),
						 "Waiting for %s", root_dev);
				break;
			case IL_STEP_FORMAT:
				if (f < 0.3 && has_op(plan, IL_OP_FORMAT, esp))
					snprintf(detail, sizeof(detail),
						 "Creating a FAT32 file system on "
						 "%s (%s)", esp_dev, IL_ESP_LABEL);
				else if (f < 0.8)
					snprintf(detail, sizeof(detail),
						 "Creating an ext4 file system on "
						 "%s (%s)", root_dev, IL_ROOT_LABEL);
				else
					snprintf(detail, sizeof(detail),
						 "Checking the new file system "
						 "on %s", root_dev);
				break;
			case IL_STEP_MOUNT:
				snprintf(detail, sizeof(detail), "Mounting %s on %s",
					 f < 0.5 ? root_dev : esp_dev,
					 f < 0.5 ? ctx->target : "/boot/efi");
				break;
			case IL_STEP_EXTRACT: {
				size_t nd = sizeof(g_tree_dirs) / sizeof(g_tree_dirs[0]);
				size_t nf = sizeof(g_tree_files) / sizeof(g_tree_files[0]);

				bytes = (uint64_t)(f * (double)src.uncompressed_bytes);
				files = (uint64_t)(f * (double)src.file_count);
				snprintf(detail, sizeof(detail), "Copying %s/%s",
					 g_tree_dirs[(size_t)t * 7 % nd],
					 g_tree_files[(size_t)t * 13 % nf]);
				break;
			}
			case IL_STEP_CONFIGURE:
				if (f < 0.15)
					snprintf(detail, sizeof(detail),
						 "Writing /etc/fstab");
				else if (f < 0.3)
					snprintf(detail, sizeof(detail),
						 "Naming this computer \"%s\"",
						 cfg->hostname);
				else if (f < 0.5)
					snprintf(detail, sizeof(detail),
						 "Setting the password of root");
				else if (f < 0.7)
					snprintf(detail, sizeof(detail),
						 "Creating the account \"%s\"",
						 cfg->user.username);
				else if (f < 0.85)
					snprintf(detail, sizeof(detail),
						 "Setting the keyboard layout (%s)",
						 cfg->layouts[0]);
				else
					snprintf(detail, sizeof(detail),
						 "Setting the time zone (%s)",
						 cfg->timezone);
				break;
			case IL_STEP_BOOTLOADER:
				if (f < 0.6)
					snprintf(detail, sizeof(detail),
						 "Copying the boot loader to %s",
						 esp_dev);
				else
					snprintf(detail, sizeof(detail),
						 "Adding the firmware boot entry "
						 "\"LikeOS\"");
				break;
			default:
				if (f < 0.6)
					snprintf(detail, sizeof(detail),
						 "Writing cached data to the disk");
				else if (f < 0.8)
					snprintf(detail, sizeof(detail),
						 "Unmounting %s/boot/efi",
						 ctx->target);
				else
					snprintf(detail, sizeof(detail),
						 "Unmounting %s", ctx->target);
				break;
			}
			report(ctx, cb, user, step, f, detail, bytes,
			       step == IL_STEP_EXTRACT ? src.uncompressed_bytes : 0,
			       files, step == IL_STEP_EXTRACT ? src.file_count : 0,
			       &t0);
			if (t < ticks)
				mock_sleep(ctx, 50);
		}
	}
	atomic_store(&ctx->can_cancel, 0);
	il_log(ctx, IL_LOG_INFO, "LikeOS was installed on %s", dev);
	return 0;
}

int il_install(il_ctx *ctx, il_plan *plan, const struct il_config *cfg,
	       il_progress_cb cb, void *user)
{
	int rc;

	if (!ctx)
		return -EINVAL;
	atomic_store(&ctx->installing, 1);
	rc = install_run(ctx, plan, cfg, cb, user);
	/* as libinstaller: cancelling is possible again once it is over */
	atomic_store(&ctx->can_cancel, 1);
	atomic_store(&ctx->cancel, 0);
	atomic_store(&ctx->installing, 0);
	return rc;
}

void il_cancel(il_ctx *ctx)
{
	if (!ctx)
		return;
	atomic_store(&ctx->cancel, 1);
	il_log(ctx, IL_LOG_INFO, "cancel requested");
}

int il_can_cancel(const il_ctx *ctx)
{
	il_ctx *c = (il_ctx *)ctx;

	if (!c)
		return 0;
	if (!atomic_load(&c->installing))
		return 1;
	return atomic_load(&c->can_cancel);
}

int il_reboot(il_ctx *ctx)
{
	il_log(ctx, IL_LOG_INFO, "restart requested (mock: nothing happens)");
	return 0;
}

int il_poweroff(il_ctx *ctx)
{
	il_log(ctx, IL_LOG_INFO, "power off requested (mock: nothing happens)");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Answer files (the format of likeos-install.conf(5))                 */
/* ------------------------------------------------------------------ */

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
			 e[-1] == '\r'))
		*--e = '\0';
	return s;
}

/* A value: plain, or in double quotes with \" and \\ (in place). */
static int unquote(char *v)
{
	char *r, *w;

	if (*v != '"')
		return 0;
	for (r = v + 1, w = v; *r && *r != '"'; r++) {
		if (*r == '\\' && (r[1] == '"' || r[1] == '\\'))
			r++;
		*w++ = *r;
	}
	if (*r != '"')
		return -EINVAL;
	*w = '\0';
	return 0;
}

static int parse_bool(const char *v, int *out)
{
	if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
	    !strcasecmp(v, "on") || !strcmp(v, "1")) {
		*out = 1;
		return 0;
	}
	if (!strcasecmp(v, "no") || !strcasecmp(v, "false") ||
	    !strcasecmp(v, "off") || !strcmp(v, "0")) {
		*out = 0;
		return 0;
	}
	return -EINVAL;
}

static int split_list(const char *v, char out[][32], int max)
{
	int n = 0;
	const char *p = v;

	if (!*v)
		return 0;
	while (n < max) {
		const char *c = strchr(p, ',');
		size_t l = c ? (size_t)(c - p) : strlen(p);

		if (l >= 32)
			l = 31;
		memcpy(out[n], p, l);
		out[n][l] = '\0';
		n++;
		if (!c)
			break;
		p = c + 1;
	}
	return n;
}

int il_answers_load(il_ctx *ctx, const char *path, struct il_answers *out)
{
	FILE *f;
	char line[1024];
	int lineno = 0, rc = 0;

	if (!path || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	il_config_defaults(&out->cfg);
	copy_str(out->disk, sizeof(out->disk), "auto");
	out->whole_disk = 1;
	out->root_partno = -1;
	out->reboot = 1;

	f = fopen(path, "r");
	if (!f) {
		int e = errno;

		set_error(ctx, "The answer file %s cannot be read: %s.", path,
			  strerror(e));
		return -e;
	}
	while (!rc && fgets(line, sizeof(line), f)) {
		char *s = trim(line), *eq, *k, *v;
		int b = 0;

		lineno++;
		if (!*s || *s == '#')
			continue;
		eq = strchr(s, '=');
		if (!eq) {
			set_error(ctx, "%s, line %d: expected key=value.", path,
				  lineno);
			rc = -EINVAL;
			break;
		}
		*eq = '\0';
		k = trim(s);
		v = trim(eq + 1);
		if (unquote(v) != 0) {
			set_error(ctx, "%s, line %d: the quote is not closed.",
				  path, lineno);
			rc = -EINVAL;
			break;
		}
		if (!strcmp(k, "disk"))
			copy_str(out->disk, sizeof(out->disk), v);
		else if (!strcmp(k, "whole_disk") || !strcmp(k, "reboot") ||
			 !strcmp(k, "user_extra_groups")) {
			if (parse_bool(v, &b) != 0) {
				set_error(ctx, "%s, line %d: %s wants yes or no.",
					  path, lineno, k);
				rc = -EINVAL;
			} else if (!strcmp(k, "whole_disk")) {
				out->whole_disk = b;
			} else if (!strcmp(k, "reboot")) {
				out->reboot = b;
			} else {
				out->cfg.user_extra_groups = b;
			}
		} else if (!strcmp(k, "root_partition"))
			out->root_partno = atoi(v);
		else if (!strcmp(k, "locale"))
			copy_str(out->cfg.locale, sizeof(out->cfg.locale), v);
		else if (!strcmp(k, "time_format"))
			copy_str(out->cfg.time_format,
				 sizeof(out->cfg.time_format), v);
		else if (!strcmp(k, "timezone"))
			copy_str(out->cfg.timezone, sizeof(out->cfg.timezone), v);
		else if (!strcmp(k, "keyboard"))
			out->cfg.nlayouts = split_list(v, out->cfg.layouts,
						       IL_MAX_LAYOUTS);
		else if (!strcmp(k, "keyboard_variant"))
			split_list(v, out->cfg.variants, IL_MAX_LAYOUTS);
		else if (!strcmp(k, "keyboard_options"))
			copy_str(out->cfg.xkb_options,
				 sizeof(out->cfg.xkb_options), v);
		else if (!strcmp(k, "hostname"))
			copy_str(out->cfg.hostname, sizeof(out->cfg.hostname), v);
		else if (!strcmp(k, "root_password"))
			copy_str(out->cfg.root_password,
				 sizeof(out->cfg.root_password), v);
		else if (!strcmp(k, "root_password_hash"))
			copy_str(out->cfg.root_password_hash,
				 sizeof(out->cfg.root_password_hash), v);
		else if (!strcmp(k, "user_fullname"))
			copy_str(out->cfg.user.fullname,
				 sizeof(out->cfg.user.fullname), v);
		else if (!strcmp(k, "user_name"))
			copy_str(out->cfg.user.username,
				 sizeof(out->cfg.user.username), v);
		else if (!strcmp(k, "user_password"))
			copy_str(out->cfg.user.password,
				 sizeof(out->cfg.user.password), v);
		else if (!strcmp(k, "user_password_hash"))
			copy_str(out->cfg.user_password_hash,
				 sizeof(out->cfg.user_password_hash), v);
		else
			il_log(ctx, IL_LOG_WARN, "%s, line %d: unknown key %s",
			       path, lineno, k);
	}
	fclose(f);
	if (!rc && out->cfg.nlayouts < 1) {
		out->cfg.nlayouts = 1;
		copy_str(out->cfg.layouts[0], sizeof(out->cfg.layouts[0]), "us");
	}
	return rc;
}

static void join_list(const char in[][32], int n, char *buf, size_t len)
{
	size_t o = 0;

	buf[0] = '\0';
	for (int i = 0; i < n && o + 1 < len; i++) {
		int w = snprintf(buf + o, len - o, "%s%s", i ? "," : "", in[i]);

		if (w < 0)
			break;
		o += (size_t)w;
	}
}

/* key=value, the value quoted when it needs it. */
static void put_kv(FILE *f, const char *key, const char *v)
{
	if (!v[0] || strpbrk(v, " \t#\"\\") || v[0] == '"') {
		fprintf(f, "%s=\"", key);
		for (const char *p = v; *p; p++) {
			if (*p == '"' || *p == '\\')
				fputc('\\', f);
			fputc(*p, f);
		}
		fprintf(f, "\"\n");
	} else {
		fprintf(f, "%s=%s\n", key, v);
	}
}

int il_answers_save(il_ctx *ctx, const char *path, const struct il_answers *a,
		    int include_passwords)
{
	char layouts[160], variants[160], num[16];
	FILE *f;
	int fd;

	if (!path || !a)
		return -EINVAL;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || !(f = fdopen(fd, "w"))) {
		int e = errno;

		if (fd >= 0)
			close(fd);
		set_error(ctx, "%s cannot be written: %s.", path, strerror(e));
		return -e;
	}
	join_list((const char (*)[32])a->cfg.layouts, a->cfg.nlayouts, layouts,
		  sizeof(layouts));
	join_list((const char (*)[32])a->cfg.variants, a->cfg.nlayouts,
		  variants, sizeof(variants));
	fprintf(f, "# likeos-install.conf -- written by the LikeOS installer "
		"(mock)\n");
	put_kv(f, "disk", a->disk);
	put_kv(f, "whole_disk", a->whole_disk ? "yes" : "no");
	if (!a->whole_disk) {
		snprintf(num, sizeof(num), "%d", a->root_partno);
		put_kv(f, "root_partition", num);
	}
	put_kv(f, "reboot", a->reboot ? "yes" : "no");
	put_kv(f, "locale", a->cfg.locale);
	put_kv(f, "time_format", a->cfg.time_format);
	put_kv(f, "timezone", a->cfg.timezone);
	put_kv(f, "keyboard", layouts);
	put_kv(f, "keyboard_variant", variants);
	put_kv(f, "keyboard_options", a->cfg.xkb_options);
	put_kv(f, "hostname", a->cfg.hostname);
	if (include_passwords)
		put_kv(f, "root_password", a->cfg.root_password);
	put_kv(f, "user_fullname", a->cfg.user.fullname);
	put_kv(f, "user_name", a->cfg.user.username);
	if (include_passwords)
		put_kv(f, "user_password", a->cfg.user.password);
	put_kv(f, "user_extra_groups", a->cfg.user_extra_groups ? "yes" : "no");
	if (fclose(f) != 0) {
		int e = errno;

		set_error(ctx, "%s cannot be written: %s.", path, strerror(e));
		return -e;
	}
	il_log(ctx, IL_LOG_INFO, "answers saved to %s", path);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Additions of API 1 (see installer.h)                                */
/* ------------------------------------------------------------------ */

int il_timezone_find(const char *name)
{
	for (int i = 0; name && i < NZONES; i++)
		if (strcmp(g_zones[i].name, name) == 0)
			return i;
	return -1;
}

int il_locale_find(const char *name)
{
	for (int i = 0; name && i < NLOCALES; i++)
		if (strcmp(g_locales[i].name, name) == 0)
			return i;
	return -1;
}

/* From the label: "(UTC+05:30) Kolkata" -> 330. */
int il_timezone_offset_minutes(int i)
{
	const char *l = il_timezone_label(i);
	int h = 0, m = 0, sign = 1;

	if (!l || strncmp(l, "(UTC", 4) != 0 || (l[4] != '+' && l[4] != '-'))
		return 0;
	if (l[4] == '-')
		sign = -1;
	if (sscanf(l + 5, "%d:%d", &h, &m) < 1)
		return 0;
	return sign * (h * 60 + m);
}

const struct il_disk *il_disks_find(const struct il_disk_list *l,
				    const char *which)
{
	const struct il_disk *found = NULL;
	int n = 0;

	if (!l || !which || !*which)
		return NULL;
	if (strcmp(which, "auto") == 0) {
		for (int i = 0; i < l->n; i++) {
			if (l->disks[i].is_boot_medium || l->disks[i].read_only)
				continue;
			found = &l->disks[i];
			n++;
		}
		return n == 1 ? found : NULL;
	}
	for (int i = 0; i < l->n; i++) {
		const char *base = strrchr(which, '/');

		if (!strcmp(l->disks[i].dev, which) ||
		    !strcmp(l->disks[i].name, which) ||
		    (base && !strcmp(l->disks[i].name, base + 1)))
			return &l->disks[i];
	}
	return NULL;
}

const struct il_partition *il_disk_part(const struct il_disk *d, int partno)
{
	for (int i = 0; d && i < d->nparts; i++)
		if (d->parts[i].number == partno)
			return &d->parts[i];
	return NULL;
}

int il_config_validate(const struct il_config *cfg, char *why, size_t len)
{
	char sub[256];

	if (why && len)
		why[0] = '\0';
	if (!cfg)
		return -EINVAL;
	if (il_locale_find(cfg->locale) < 0) {
		snprintf(why, len, "The language %s is not offered.", cfg->locale);
		return -EINVAL;
	}
	if (il_timezone_find(cfg->timezone) < 0) {
		snprintf(why, len, "The time zone %s is not known.", cfg->timezone);
		return -EINVAL;
	}
	if (cfg->nlayouts < 1 || cfg->nlayouts > IL_MAX_LAYOUTS) {
		snprintf(why, len, "Choose one to %d keyboard layouts.",
			 IL_MAX_LAYOUTS);
		return -EINVAL;
	}
	if (il_validate_hostname(cfg->hostname, sub, sizeof(sub)) != 0) {
		snprintf(why, len, "Computer name: %s", sub);
		return -EINVAL;
	}
	if (!cfg->root_password_hash[0] &&
	    il_validate_password(cfg->root_password, sub, sizeof(sub)) != 0) {
		snprintf(why, len, "Root password: %s", sub);
		return -EINVAL;
	}
	if (cfg->user.username[0] || cfg->user.fullname[0]) {
		if (il_validate_username(cfg->user.username, sub,
					 sizeof(sub)) != 0) {
			snprintf(why, len, "User name: %s", sub);
			return -EINVAL;
		}
		if (!cfg->user_password_hash[0] &&
		    il_validate_password(cfg->user.password, sub,
					 sizeof(sub)) != 0) {
			snprintf(why, len, "Password of %s: %s",
				 cfg->user.username, sub);
			return -EINVAL;
		}
		if (strpbrk(cfg->user.fullname, ":\n\r")) {
			snprintf(why, len, "The full name cannot contain ':' or "
				 "line ends.");
			return -EINVAL;
		}
	}
	return 0;
}

int il_answers_plan(il_ctx *ctx, const struct il_answers *a,
		    const struct il_disk_list *disks, il_plan **out)
{
	const struct il_disk *d;
	il_plan *plan;
	int rc;

	if (!a || !disks || !out)
		return -EINVAL;
	*out = NULL;
	d = il_disks_find(disks, a->disk[0] ? a->disk : "auto");
	if (!d) {
		set_error(ctx, "There is no disk %s.", a->disk[0] ? a->disk :
			  "auto");
		return -ENODEV;
	}
	plan = il_plan_new(ctx, d);
	if (!plan)
		return -ENOMEM;
	rc = a->whole_disk ? il_plan_use_whole_disk(plan) :
		il_plan_select_root(plan, a->root_partno);
	if (rc) {
		il_plan_free(plan);
		return rc;
	}
	*out = plan;
	return 0;
}

void il_plan_summary(const il_plan *pl, char *buf, size_t len)
{
	char line[256], desc[192], size[32];
	size_t o;

	if (!buf || !len)
		return;
	buf[0] = '\0';
	if (!pl)
		return;
	il_format_size(pl->orig.size_bytes, size, sizeof(size));
	snprintf(buf, len, "Disk %s (%s, %s)\n", pl->orig.dev,
		 pl->orig.model[0] ? pl->orig.model : "disk", size);
	if (pl->nall == 0)
		snprintf(buf + strlen(buf), len - strlen(buf),
			 "  No changes to the partitions\n");
	for (int i = 0; i < pl->nall; i++) {
		il_op_describe(&pl->all[i], desc, sizeof(desc));
		snprintf(line, sizeof(line), "  %d. %s\n", i + 1, desc);
		o = strlen(buf);
		snprintf(buf + o, len - o, "%s", line);
	}
	if (pl->root > 0) {
		o = strlen(buf);
		snprintf(buf + o, len - o, "LikeOS is installed on partition %d\n",
			 pl->root);
	}
}

const char *il_log_path(const il_ctx *ctx)
{
	return ctx && ctx->log_path ? ctx->log_path : IL_DEFAULT_LOG;
}

int il_plan_clear_root(il_plan *pl)
{
	if (!pl)
		return -EINVAL;
	if (pl->root > 0) {
		retype_created(pl, pl->root, IL_GUID_GENERIC_DATA, "");
		il_log(pl->ctx, IL_LOG_INFO, "plan %s: partition %d is no longer "
		       "chosen for LikeOS", pl->orig.dev, pl->root);
	}
	pl->root = -1;
	plan_rebuild(pl);
	return 0;
}

int il_plan_can_select_root(const il_plan *pl, int partno,
			    const struct il_source_info *src, char *why,
			    size_t len)
{
	struct il_plan *copy;
	il_ctx *quiet;
	int rc;

	if (why && len)
		why[0] = '\0';
	if (!pl) {
		snprintf(why, len, "No disk has been chosen.");
		return -EINVAL;
	}
	/* On a copy with a silent context of its own: the error lands
	 * there, nothing is logged. */
	copy = malloc(sizeof(*copy));
	quiet = calloc(1, sizeof(*quiet));
	if (!copy || !quiet) {
		free(copy);
		free(quiet);
		snprintf(why, len, "Out of memory.");
		return -ENOMEM;
	}
	*copy = *pl;
	copy->ctx = quiet;
	rc = il_plan_select_root(copy, partno);
	if (rc)
		snprintf(why, len, "%s", quiet->last_error);
	else
		rc = il_plan_validate(copy, src, why, len);
	free(copy);
	free(quiet);
	return rc;
}

/* ------------------------------------------------------------------ */
/* The function table (backend-ops.h)                                  */
/* ------------------------------------------------------------------ */

const IlBackend il_backend_mock = {
	.name = "stand-in backend (--mock)",
	.is_mock = 1,
	IL_BACKEND_FUNCTIONS,
};
