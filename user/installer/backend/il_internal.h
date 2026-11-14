/* libinstaller -- what the modules share with each other.
 *
 * Nothing here is part of the public interface (include/installer.h); the
 * front ends never see these names.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef IL_INTERNAL_H
#define IL_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "installer.h"
#include "il_compat.h"
#include "gpt.h"

#define IL_VERSION_STRING  "1.0.0"
#define IL_LOG_PREFIX      "likeos-install: "
#define IL_PATH_MAX        4096
#define IL_MAX_MOUNTS      8
#define IL_MAX_OPS         64
#define IL_MAX_SIM_DISKS   IL_MAX_DISKS

/* Where the tools live on LikeOS.  Each can be replaced through the
 * environment variable named next to it (host tests use the host's). */
#define IL_TOOL_MKFS_FAT   "/sbin/mkfs.fat"           /* IL_MKFS_FAT */
#define IL_TOOL_MKE2FS     "/sbin/mke2fs"             /* IL_MKE2FS */
#define IL_TOOL_E2FSCK     "/sbin/e2fsck"             /* IL_E2FSCK */
#define IL_TOOL_TUNE2FS    "/sbin/tune2fs"            /* IL_TUNE2FS */
#define IL_TOOL_EFIBOOT    "/usr/sbin/likeos-efiboot" /* IL_EFIBOOT */

/* Every minimum a plan is checked against. */
#define IL_ESP_MIN_BYTES        (100ULL * 1024ULL * 1024ULL)
#define IL_ROOT_DEFAULT_MIN     (8ULL * 1024ULL * 1024ULL * 1024ULL)
#define IL_ROOT_SLACK_BYTES     (1024ULL * 1024ULL * 1024ULL)

/* ------------------------------------------------------------------ */
/* The context                                                         */
/* ------------------------------------------------------------------ */

struct il_mount_rec {
	char target[IL_PATH_MAX];
	char source[128];
	char fstype[16];
	int simulated;            /* simulate mode: nothing was mounted */
};

struct il_run;                    /* il_install.c: one installation */

struct il_ctx {
	/* options, as owned copies */
	char source[IL_PATH_MAX];
	char target[IL_PATH_MAX];
	char log_path[IL_PATH_MAX];
	char sysroot[IL_PATH_MAX];
	int dry_run;
	int simulate;
	int verbose;
	int nsim_disks;
	char *sim_disks[IL_MAX_SIM_DISKS];
	il_log_cb log_cb;
	void *log_user;

	/* tools (paths) */
	char tool_mkfs_fat[256];
	char tool_mke2fs[256];
	char tool_e2fsck[256];
	char tool_tune2fs[256];
	char tool_efiboot[256];

	/* log */
	int log_fd;               /* -1 when the file could not be opened */
	int kmsg_fd;              /* -1 when not used or not available */
	int log_open_failed;      /* reported once */
	int in_log;               /* a log callback must not log again */

	/* errors */
	char err[IL_ERROR_MAX];
	int last_errno;

	/* cancellation (il_cancel may run on another thread) */
	volatile int cancel_requested;
	volatile int cancel_allowed;
	volatile int installing;

	/* mounts made by this context (undone in reverse order) */
	int nmounts;
	struct il_mount_rec mounts[IL_MAX_MOUNTS];

	/* the installation running now */
	struct il_run *run;
};

/* Sets the context's error sentence and logs it; returns -err (err is a
 * positive errno value; 0 is taken as EIO). */
int il_set_error(il_ctx *ctx, int err, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void il_clear_error(il_ctx *ctx);
void il_logv(il_ctx *ctx, int level, const char *fmt, va_list ap);
/* A block of text (tool output): one log line per text line. */
void il_log_text(il_ctx *ctx, int level, const char *prefix,
		 const char *text, size_t len);
/* Opens the log file (truncating it) and /dev/kmsg; called by il_ctx_new. */
void il_log_open(il_ctx *ctx);
void il_log_close(il_ctx *ctx);
/* fsync of the log file (before it is copied, before a reboot). */
void il_log_flush(il_ctx *ctx);
/* 1 once il_cancel() was called and cancelling is still allowed. */
int il_cancelled(const il_ctx *ctx);

/* <sysroot><path>, for /sys, /proc and /dev lookups. */
void il_sys_path(const il_ctx *ctx, char *buf, size_t len, const char *path);

/* ------------------------------------------------------------------ */
/* Utilities (il_util.c)                                               */
/* ------------------------------------------------------------------ */

size_t il_strlcpy(char *dst, const char *src, size_t len);
size_t il_strlcat(char *dst, const char *src, size_t len);
/* Removes blanks and line ends at both ends, in place. */
char *il_trim(char *s);
int il_starts_with(const char *s, const char *prefix);
int il_ends_with(const char *s, const char *suffix);
/* a + "/" + b without doubled slashes; -ENAMETOOLONG when it does not fit. */
int il_path_join(char *buf, size_t len, const char *a, const char *b);
/* <root>/<path> where path may start with '/'. */
int il_path_in(char *buf, size_t len, const char *root, const char *path);

/* Creates every missing directory of path with mode. */
int il_mkdir_p(const char *path, mode_t mode);
/* Reads a small file into buf (always terminated); returns the length or
 * -errno. */
ssize_t il_read_file(const char *path, char *buf, size_t len);
/* Reads a whole file into a malloc'd, terminated buffer. */
int il_read_file_alloc(const char *path, char **out, size_t *len_out);
/* Reads a sysfs-style one-line value; trims it. */
int il_read_line(const char *path, char *buf, size_t len);
int il_read_u64(const char *path, uint64_t *out);
/* Writes data to path.tmp, fsyncs, sets mode and owner (uid/gid -1 =
 * leave), renames over path, fsyncs the directory. */
int il_write_file_atomic(const char *path, const void *data, size_t len,
			 mode_t mode, uid_t uid, gid_t gid);
/* Copies a regular file (mode as given; -1 = the source's). */
int il_copy_file(const char *src, const char *dst, int mode);
/* Copies a tree preserving symbolic links, modes and times; every object
 * created is owned by uid:gid (-1 = keep the source's). */
int il_copy_tree(il_ctx *ctx, const char *src, const char *dst, uid_t uid,
		 gid_t gid);
/* chown -R without following symbolic links. */
int il_chown_tree(il_ctx *ctx, const char *path, uid_t uid, gid_t gid);
/* rm -rf; refuses "/" and "". */
int il_remove_tree(il_ctx *ctx, const char *path);
int il_file_exists(const char *path);
int il_is_dir(const char *path);
uint64_t il_file_size(const char *path);

/* getrandom(), with /dev/urandom behind it. */
int il_random_bytes(void *buf, size_t n);
void il_hex(const void *data, size_t n, char *out);
uint64_t il_now_ms(void);
void il_sleep_ms(unsigned ms);
/* Days since 1970-01-01 (for /etc/shadow). */
long il_days_since_epoch(void);

/* Runs argv[0] (an absolute path) with argv, stdin from /dev/null, its
 * stdout and stderr captured line by line into the log.  *status gets the
 * exit code (128 + signal for a killed child).  Returns 0 when the program
 * ran (whatever its exit code), -errno when it could not be started.
 * out/outlen, when given, also receive the output (terminated, truncated
 * to fit). */
int il_spawn(il_ctx *ctx, const char *const argv[], int *status, char *out,
	     size_t outlen);
/* il_spawn that fails (with an error sentence) on a non-zero exit. */
int il_run_tool(il_ctx *ctx, const char *what, const char *const argv[]);

/* ------------------------------------------------------------------ */
/* SHA-256 (il_sha256.c)                                               */
/* ------------------------------------------------------------------ */

struct il_sha256 {
	uint32_t h[8];
	uint64_t len;             /* bytes hashed */
	uint8_t buf[64];
	size_t nbuf;
};

void il_sha256_init(struct il_sha256 *s);
void il_sha256_update(struct il_sha256 *s, const void *data, size_t len);
void il_sha256_final(struct il_sha256 *s, uint8_t digest[32]);
void il_sha256_hex(const uint8_t digest[32], char out[65]);
/* Hashes a whole file; progress (may be NULL) gets the fraction read;
 * stops with -ECANCELED when cancel is set. */
int il_sha256_file(const char *path, char out[65],
		   void (*progress)(double fraction, void *user), void *user,
		   const volatile int *cancel);

/* ------------------------------------------------------------------ */
/* File system probing (il_disk.c)                                     */
/* ------------------------------------------------------------------ */

struct il_fsprobe {
	char fstype[16];          /* ext4, ext3, ext2, vfat, exfat, ntfs, swap */
	char label[64];
	char uuid[40];            /* ext: canonical GUID text; FAT: XXXX-XXXX;
	                           * NTFS: 16 hex digits */
	uint64_t size_bytes;      /* the file system's own idea, 0 unknown */
	uint64_t free_bytes;      /* 0 unknown */
};

/* Reads the superblocks at `offset' of a device or image file. */
int il_fs_probe(const char *path, uint64_t offset, struct il_fsprobe *out);
/* The node of partition partno of a disk: sda -> sda1, nvme0n1 ->
 * nvme0n1p1, mmcblk0 -> mmcblk0p1. */
void il_part_devname(const char *disk_dev, int partno, char *out,
		     size_t len);
/* Fills a disk from its device (or image) with libgpt: table, partitions,
 * free regions, file systems.  The caller has filled name, dev, sizes. */
int il_disk_read_table(il_ctx *ctx, struct il_disk *d, const char *path);
/* The node (or, simulating, the image file) a disk is read and written
 * through: il_disk.dev holds at most 63 characters, an image path may be
 * longer, so a simulated disk is named img<N> there. */
int il_disk_node(const il_ctx *ctx, const struct il_disk *d, char *out,
		 size_t len);
/* The free list of a disk from its partitions (aligned regions). */
void il_disk_compute_free(struct il_disk *d);
/* 1 when the running system's root is on the disk (by name, sda) right
 * now: looked up afresh, not taken from an earlier scan. */
int il_disk_holds_running_root(il_ctx *ctx, const char *disk_name);
/* 1 when the disk still reports the model and (when the scan knew one)
 * the serial number the scan saw, read afresh; else 0 with the
 * difference in *what. */
int il_disk_same_ident(il_ctx *ctx, const struct il_disk *d, char *what,
		       size_t len);

/* ------------------------------------------------------------------ */
/* Plan (il_plan.c)                                                    */
/* ------------------------------------------------------------------ */

struct il_plan {
	il_ctx *ctx;
	struct il_disk disk;      /* as scanned */
	int nops;
	struct il_op ops[IL_MAX_OPS];
	int root_partno;          /* -1 none */
	/* Set on a scratch copy that only answers a question
	 * (il_plan_can_select_root): its errors go to qerr instead of the
	 * context, and it logs nothing. */
	int quiet;
	char qerr[IL_ERROR_MAX];
};

/* Builds an in-memory GPT that describes the disk as scanned (the base the
 * operations are replayed on for the projected view). */
int il_plan_base_table(const struct il_disk *d, struct gpt_table *t);
/* Replays the plan's table operations on t.  With real_guids set, created
 * partitions get random unique GUIDs (installing); otherwise zero ones
 * (the view). */
int il_plan_replay(const il_plan *plan, struct gpt_table *t, int real_guids,
		   char *why, size_t len);
/* 1 when the plan starts with a new table (the disk is erased). */
int il_plan_erases_disk(const il_plan *plan);
/* The format op for partno, or NULL. */
const struct il_op *il_plan_format_op(const il_plan *plan, int partno);
/* 1 when the partition is created by the plan. */
int il_plan_creates(const il_plan *plan, int partno);
uint64_t il_required_root_bytes(const struct il_source_info *src);

/* ------------------------------------------------------------------ */
/* What the steps hand each other                                      */
/* ------------------------------------------------------------------ */

struct il_target {
	char disk_dev[IL_PATH_MAX]; /* /dev/sda or the image file */
	char disk_name[32];
	int root_partno;
	int esp_partno;
	char root_dev[IL_PATH_MAX]; /* /dev/sda2 (simulate: a scratch image) */
	char esp_dev[IL_PATH_MAX];
	uint64_t root_offset;      /* simulate: offset inside the disk image */
	uint64_t esp_offset;
	uint64_t root_size;
	uint64_t esp_size;
	char root_partuuid[37];
	char esp_partuuid[37];
	char root_fsuuid[40];
	char esp_fsuuid[40];
	int format_root;
	int format_esp;
	uint32_t sector_size;      /* logical sector size of the disk */
	char root_dir[IL_PATH_MAX]; /* where the root is mounted */
	char esp_dir[IL_PATH_MAX];  /* <root_dir>/boot/efi */
};

/* il_partition.c */
int il_partition_apply(il_ctx *ctx, il_plan *plan, struct il_target *t);
/* Zeroes the first and last MiB of [off, off + len) of a device. */
int il_wipe_signatures(il_ctx *ctx, struct gpt_dev *d, uint64_t off,
		       uint64_t len);

/* il_format.c */
int il_format_esp(il_ctx *ctx, struct il_target *t);
int il_format_root(il_ctx *ctx, struct il_target *t);
/* simulate mode: copies the scratch images into the disk image */
int il_format_commit_sim(il_ctx *ctx, struct il_target *t);
/* Fills the file system UUIDs of t (also for partitions kept as they are) */
int il_format_probe(il_ctx *ctx, struct il_target *t);

/* il_mount.c */
int il_mount_target(il_ctx *ctx, struct il_target *t);
/* Unmounts everything this context mounted, in reverse order. */
int il_umount_all(il_ctx *ctx);
/* Unmounts what the system has mounted at or below dir (leftovers of an
 * earlier attempt), deepest first. */
int il_umount_below(il_ctx *ctx, const char *dir);
/* 1 when a device or something under dir is mounted (from /proc/mounts). */
int il_is_mounted(il_ctx *ctx, const char *dev_or_dir);

/* il_extract.c */
struct il_extract_progress {
	uint64_t compressed_done;   /* archive bytes consumed */
	uint64_t compressed_total;  /* archive size */
	uint64_t bytes_done;        /* uncompressed payload bytes written */
	uint64_t files_done;        /* entries extracted */
	const char *path;           /* the entry being extracted (callback only) */
};

struct il_extract_opts {
	const char *archive;        /* .tar.zst (or a plain .tar) */
	const char *target;         /* the directory to extract into */
	const char *expected_sha256;/* hex of the compressed stream, NULL/"" skip */
	int same_owner;             /* chown to the archive's numeric ids */
	int owner_errors_fatal;     /* 0: EPERM from chown is counted, not fatal */
	int xattrs;                 /* apply SCHILY.xattr / LIBARCHIVE.xattr */
	int acls;                   /* apply SCHILY.acl.* */
	void (*progress)(const struct il_extract_progress *p, void *user);
	void *user;
	unsigned progress_interval_ms; /* 0 = 100 */
	/* Simulate mode only (tests): a pause after every entry and per MiB
	 * written, in microseconds, standing for a slow disk. */
	unsigned sim_entry_delay_us;
	unsigned sim_mib_delay_us;
};

struct il_extract_stats {
	uint64_t entries;
	uint64_t files;
	uint64_t dirs;
	uint64_t symlinks;
	uint64_t hardlinks;
	uint64_t devices;
	uint64_t fifos;
	uint64_t bytes;             /* payload bytes written */
	uint64_t compressed_bytes;
	uint64_t xattrs_set;
	uint64_t xattr_failures;
	uint64_t acl_failures;
	uint64_t owner_failures;
	uint64_t time_failures;
	uint64_t skipped;           /* unsupported entries left out */
	char sha256[65];            /* of the compressed stream */
};

/* Streams the archive into opts->target.  Honours il_cancelled(ctx)
 * (returns -ECANCELED).  Returns 0 or -errno with the context error set. */
int il_extract_archive(il_ctx *ctx, const struct il_extract_opts *opts,
		       struct il_extract_stats *st);

/* il_config.c */
int il_configure_target(il_ctx *ctx, const struct il_config *cfg,
			const struct il_target *t);
/* The text of /etc/likeos/keyboard.conf for cfg. */
int il_keyboard_conf_text(const struct il_config *cfg, char *buf, size_t len);
/* il_accounts.c */
int il_accounts_apply(il_ctx *ctx, const char *root,
		      const struct il_config *cfg);
/* $y$ hash of a password (yescrypt), into out. */
int il_hash_password(il_ctx *ctx, const char *pw, char *out, size_t len);
/* il_boot.c */
int il_boot_install(il_ctx *ctx, const struct il_target *t);

/* il_install.c: progress inside the running step (0..1), with a detail
 * line; a no-op outside il_install. */
void il_step_progress(il_ctx *ctx, double fraction, const char *detail);

/* il_progress.c: how far an installation is and how long it still takes.
 *
 * Unpacking costs time per byte and time per file (create, write, owner,
 * mode, times, attributes); with many small files the second dominates.
 * The tracker fits  seconds = a * MiB + b * files  online over the recent
 * past (least squares, exponentially forgotten, held towards a default
 * cost of IL_TRACK_FILE_KIB per file where the data cannot tell the two
 * apart), and derives from it the share of the unpacking that is done and
 * the time it still needs.  The steps after it are estimated from their
 * weights against the unpacking's time; the step running is extrapolated
 * from its own time.  The time left is smoothed and kept within reach of
 * the model; the shown fractions never go back.  Every time is passed in
 * (milliseconds), so tests can play any sequence. */
#define IL_TRACK_FILE_KIB     256      /* default cost of a file, in bytes */
#define IL_TRACK_WINDOW_MS    1000     /* one sample of the fit */
#define IL_TRACK_WARMUP_MS    5000     /* unpacking time before a time left */

struct il_track {
	int step;
	double frac;                 /* the running step, never back */
	uint64_t frac_ms;            /* when the unpacking's share moved */
	double overall;              /* never back */
	int finished;
	uint64_t step_t0[IL_STEP_COUNT];
	uint64_t step_ms[IL_STEP_COUNT]; /* duration of a finished step */
	/* unpacking */
	uint64_t bytes_total, files_total, comp_total;
	uint64_t bytes_done, files_done, comp_done;
	double fit_xx, fit_xy, fit_yy, fit_xt, fit_yt; /* normal equations */
	double cost_mib, cost_file;  /* a and b, seconds */
	int samples;
	uint64_t win_ms, win_bytes, win_files;
	/* time left */
	double eta;                  /* smoothed seconds, < 0 unknown */
	uint64_t eta_ms;
};

void il_track_init(struct il_track *t, uint64_t now_ms);
/* The size of the image (.info): bytes=, files=, and the archive size. */
void il_track_totals(struct il_track *t, uint64_t bytes, uint64_t files,
		     uint64_t compressed);
void il_track_step(struct il_track *t, int step, uint64_t now_ms);
/* Progress inside a step other than the unpacking. */
void il_track_fraction(struct il_track *t, double fraction);
/* Progress of the unpacking. */
void il_track_extract(struct il_track *t, uint64_t now_ms, uint64_t bytes,
		      uint64_t files, uint64_t compressed);
/* The installation is complete: 100 %. */
void il_track_finish(struct il_track *t, uint64_t now_ms);
/* Fills step, step_fraction, overall, percent, eta_seconds and the
 * counters of *p (detail is left alone). */
void il_track_report(struct il_track *t, uint64_t now_ms,
		     struct il_progress *p);
/* The model's raw estimate of the seconds left, -1 when unknown. */
double il_track_remaining(const struct il_track *t, uint64_t now_ms);

#endif /* IL_INTERNAL_H */
