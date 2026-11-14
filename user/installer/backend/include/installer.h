/* LikeOS installer backend -- public interface.
 *
 * libinstaller does every step of installing LikeOS on a disk: it lists the
 * disks, plans and writes a GPT partition table, formats the partitions,
 * mounts them, unpacks the prepared system image (likeos-rootfs.tar.zst),
 * configures the result (fstab, hostname, accounts, keyboard, time zone,
 * locale) and installs the boot loader with its firmware boot entry.
 *
 * It has no knowledge of any toolkit.  Two front ends drive it:
 *  - likeos-installer, the GTK3 wizard (user/installer/frontend), and
 *  - likeos-install, the text-mode installer (user/installer/cli), which
 *    also runs unattended from an answer file and needs no X server.
 *
 * Threading: a context is used by one thread at a time.  il_install() runs
 * for minutes; a graphical front end calls it on a worker thread and hands
 * the progress callbacks back to its main loop.  il_cancel() and
 * il_can_cancel() are the only functions that may be called from another
 * thread while a call on the context runs (see "Threads" at the end).
 *
 * Errors: functions returning int give 0 on success and a negative errno
 * value on failure; il_last_error() then holds a sentence for a person.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_H
#define LIKEOS_INSTALLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IL_API_VERSION 1

/* ------------------------------------------------------------------ */
/* Limits and well-known values                                        */
/* ------------------------------------------------------------------ */

#define IL_MAX_DISKS          32
#define IL_MAX_PARTS          128
#define IL_MAX_FREE           130
#define IL_MAX_LAYOUTS        4
#define IL_HOSTNAME_MAX       63
#define IL_USERNAME_MAX       32
#define IL_PASSWORD_MAX       255
#define IL_FULLNAME_MAX       127
#define IL_ERROR_MAX          512

/* Default paths (all overridable through il_options). */
#define IL_DEFAULT_SOURCE     "/install/likeos-rootfs.tar.zst"
#define IL_DEFAULT_TARGET     "/mnt/likeos"
#define IL_DEFAULT_LOG        "/tmp/likeos-install.log"
#define IL_TARGET_LOG         "/var/log/likeos-install.log"
#define IL_EFI_LOADER_IN_IMAGE "/usr/lib/likeos/efi/BOOTX64.EFI"

/* The default layout of a whole-disk installation. */
#define IL_ALIGN_BYTES        (1024ULL * 1024ULL)          /* 1 MiB */
#define IL_ESP_SIZE_BYTES     (512ULL * 1024ULL * 1024ULL) /* 512 MiB */
#define IL_ESP_LABEL          "LIKEOS-ESP"
#define IL_ROOT_LABEL         "likeos-root"
#define IL_ESP_PARTNAME       "EFI system partition"
#define IL_ROOT_PARTNAME      "likeos-root"

/* GPT partition type GUIDs (lower case, canonical text form). */
#define IL_GUID_ESP           "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
#define IL_GUID_LIKEOS_ROOT   "4f68bce3-e8cd-4db1-96e7-fbcaf984b709"
#define IL_GUID_GENERIC_DATA  "0fc63daf-8483-4772-8e79-3d69d8477de4"
#define IL_GUID_BASIC_DATA    "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"
#define IL_GUID_MSR           "e3c9e316-0b5c-4db8-817d-f92df00215ae"
#define IL_GUID_SWAP          "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

typedef struct il_ctx il_ctx;

enum il_log_level {
	IL_LOG_DEBUG = 0,
	IL_LOG_INFO  = 1,
	IL_LOG_WARN  = 2,
	IL_LOG_ERROR = 3,
};

/* Called for every log line (after it has gone to the log file, and, in a
 * build for a serial console, to /dev/kmsg).  May be NULL. */
typedef void (*il_log_cb)(int level, const char *line, void *user);

struct il_options {
	const char *source;      /* the system image; NULL = IL_DEFAULT_SOURCE */
	const char *target;      /* mount point of the new root; NULL = default */
	const char *log_path;    /* NULL = IL_DEFAULT_LOG */
	const char *sysroot;     /* prefix for /sys, /proc, /dev lookups ("" or NULL
	                          * on a running system; a directory in host tests) */
	int dry_run;             /* plan and validate, but write nothing */
	int simulate;            /* operate on the image files named in
	                          * simulate_disks instead of block devices */
	const char *const *simulate_disks; /* NULL-terminated list of image paths */
	int verbose;             /* also log IL_LOG_DEBUG lines */
	il_log_cb log_cb;
	void *log_user;
};

il_ctx *il_ctx_new(const struct il_options *opt);
void il_ctx_free(il_ctx *ctx);
const char *il_last_error(const il_ctx *ctx);
const char *il_version(void);

/* printf-style log into the context's log (file + callback + serial). */
void il_log(il_ctx *ctx, int level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* ------------------------------------------------------------------ */
/* Disks                                                               */
/* ------------------------------------------------------------------ */

enum il_table_kind {
	IL_TABLE_NONE = 0,     /* blank disk (or unknown signature) */
	IL_TABLE_GPT  = 1,
	IL_TABLE_MBR  = 2,     /* legacy MBR (not protective) */
};

enum il_transport {
	IL_TRANSPORT_UNKNOWN = 0,
	IL_TRANSPORT_SATA,
	IL_TRANSPORT_NVME,
	IL_TRANSPORT_USB,
	IL_TRANSPORT_FILE,     /* simulate mode */
};

struct il_partition {
	int number;               /* 1-based partition number */
	char dev[64];             /* /dev/sda1, /dev/nvme0n1p2 */
	uint64_t start_bytes;
	uint64_t size_bytes;
	char type_guid[37];
	char part_guid[37];       /* PARTUUID */
	char name[72];            /* GPT name, UTF-8 */
	char type_desc[48];       /* "EFI system", "LikeOS root", "Basic data" */
	char fstype[16];          /* ext4, vfat, ntfs, swap, "" when unknown */
	char fs_label[64];
	char fs_uuid[40];
	uint64_t fs_free_bytes;   /* 0 when unknown */
	int is_esp;
	int is_likeos_root;
	int mbr_type;             /* MBR partition type byte, legacy tables */
};

struct il_free_region {
	uint64_t start_bytes;     /* already aligned to IL_ALIGN_BYTES */
	uint64_t size_bytes;
};

struct il_disk {
	char name[32];            /* sda, nvme0n1 */
	char dev[64];             /* /dev/sda */
	char model[64];
	char serial[48];
	int transport;            /* enum il_transport */
	uint64_t size_bytes;
	uint32_t sector_size;     /* logical */
	uint32_t phys_sector_size;
	int removable;
	int read_only;
	int is_boot_medium;       /* holds the running system: never offered */
	int table;                /* enum il_table_kind */
	char disk_guid[37];       /* GPT disk GUID, "" otherwise */
	int nparts;
	struct il_partition parts[IL_MAX_PARTS];
	int nfree;
	struct il_free_region free[IL_MAX_FREE];
};

struct il_disk_list {
	int n;
	struct il_disk disks[IL_MAX_DISKS];
};

/* Fills *out with every disk the system has (the boot medium included, with
 * is_boot_medium set).  Returns 0 or -errno. */
int il_disks_scan(il_ctx *ctx, struct il_disk_list *out);

/* A one-line description: "Disk 0 · Samsung SSD 970 · 476.9 GB · NVMe". */
void il_disk_describe(const struct il_disk *d, int index, char *buf, size_t len);

/* "476.9 GB", "512.0 MB" (decimal units, one decimal). */
void il_format_size(uint64_t bytes, char *buf, size_t len);

/* ------------------------------------------------------------------ */
/* The system image                                                    */
/* ------------------------------------------------------------------ */

struct il_source_info {
	int present;
	char path[256];
	char version[64];          /* from <source>.info: version= */
	char build_date[32];       /* build= */
	uint64_t compressed_bytes; /* size of the archive */
	uint64_t uncompressed_bytes; /* bytes= */
	uint64_t file_count;       /* files= */
	char sha256[65];           /* from <source>.sha256, "" when absent */
	uint64_t required_bytes;   /* minimum root partition size */
};

int il_source_probe(il_ctx *ctx, struct il_source_info *out);

/* Reads the whole archive and compares its SHA-256 (slow; optional). */
int il_source_verify(il_ctx *ctx, void (*progress)(double fraction, void *user),
		     void *user);

/* ------------------------------------------------------------------ */
/* Partitioning plan                                                   */
/* ------------------------------------------------------------------ */
/* Nothing is written while a plan is edited.  Each edit is checked
 * against the projected layout; il_plan_view() returns that layout for a
 * front end to draw.  il_install() applies the plan. */

typedef struct il_plan il_plan;

enum il_op_kind {
	IL_OP_NEW_TABLE = 1,   /* fresh GPT (erases everything) */
	IL_OP_DELETE,          /* delete partition */
	IL_OP_CREATE,          /* create partition */
	IL_OP_EXTEND,          /* grow partition into following free space */
	IL_OP_FORMAT,          /* format partition (ext4 or FAT32) */
	IL_OP_SET_TYPE,        /* change partition type GUID */
};

struct il_op {
	int kind;              /* enum il_op_kind */
	int partno;
	uint64_t start_bytes;
	uint64_t size_bytes;
	char type_guid[37];
	char fstype[16];       /* for IL_OP_FORMAT: "ext4" or "vfat" */
	char name[72];
};

il_plan *il_plan_new(il_ctx *ctx, const struct il_disk *disk);
void il_plan_free(il_plan *plan);

/* Replace everything: new GPT, ESP 512 MiB, root on the rest. */
int il_plan_use_whole_disk(il_plan *plan);
/* Converts a legacy-MBR or blank disk to an empty GPT (erases it). */
int il_plan_new_table(il_plan *plan);
int il_plan_delete(il_plan *plan, int partno);
/* Creates a partition of size_bytes (0 = all) in free region free_index.
 * When the disk has no ESP yet the first partition created on it also
 * gets an ESP in front of it (the way a whole-disk install does). */
int il_plan_create(il_plan *plan, int free_index, uint64_t size_bytes,
		   int *partno_out);
int il_plan_extend(il_plan *plan, int partno, uint64_t new_size_bytes);
int il_plan_format(il_plan *plan, int partno);
/* Chooses the partition LikeOS is installed on (it will be formatted). */
int il_plan_select_root(il_plan *plan, int partno);
int il_plan_root(const il_plan *plan);   /* selected partno or -1 */
int il_plan_esp(const il_plan *plan);    /* ESP partno or -1 */

/* Projected layout after all operations. */
int il_plan_view(const il_plan *plan, struct il_disk *out);
int il_plan_nops(const il_plan *plan);
const struct il_op *il_plan_op(const il_plan *plan, int i);
/* "Delete partition 3 (63.8 GB)" etc. */
void il_op_describe(const struct il_op *op, char *buf, size_t len);
/* 0 when installable, else -errno with a reason in *why. */
int il_plan_validate(const il_plan *plan, const struct il_source_info *src,
		     char *why, size_t len);
/* The disk the plan is for (the scan result it was created from). */
const struct il_disk *il_plan_disk(const il_plan *plan);

/* ------------------------------------------------------------------ */
/* What the user chose                                                 */
/* ------------------------------------------------------------------ */

struct il_user {
	char fullname[IL_FULLNAME_MAX + 1];
	char username[IL_USERNAME_MAX + 1];
	char password[IL_PASSWORD_MAX + 1];
};

struct il_config {
	char locale[32];                   /* en_US.UTF-8 */
	char time_format[32];              /* en_US.UTF-8, en_GB.UTF-8 (LC_TIME) */
	char timezone[64];                 /* Olson name, Europe/Berlin */
	int nlayouts;                      /* 1..IL_MAX_LAYOUTS */
	char layouts[IL_MAX_LAYOUTS][32];  /* XKB layouts: de, us */
	char variants[IL_MAX_LAYOUTS][32]; /* XKB variants: "" or nodeadkeys */
	char xkb_options[128];             /* grp:alt_shift_toggle */
	char hostname[IL_HOSTNAME_MAX + 1];
	char root_password[IL_PASSWORD_MAX + 1];
	struct il_user user;
	int user_extra_groups;             /* add audio,render,users (default 1) */
	/* Added in API 1 (answer files): a ready crypt(3) hash ("$y$...") used
	 * instead of the plain password when the plain one is empty. */
	char root_password_hash[256];
	char user_password_hash[256];
};

void il_config_defaults(struct il_config *cfg);

/* Validators: 0 when valid, else -EINVAL and a sentence in *why. */
int il_validate_hostname(const char *name, char *why, size_t len);
int il_validate_username(const char *name, char *why, size_t len);
int il_validate_password(const char *pw, char *why, size_t len);
/* 0 (very weak) .. 4 (strong). */
int il_password_strength(const char *pw);
/* A login name from a full name ("Ada Lovelace" -> "ada"). */
void il_suggest_username(const char *fullname, char *buf, size_t len);
/* "likeos-3fa2" */
void il_suggest_hostname(char *buf, size_t len);

/* Time zones the system knows (the C library's built-in table). */
int il_timezone_count(void);
const char *il_timezone_name(int i);       /* Europe/Berlin */
const char *il_timezone_label(int i);      /* "(UTC+01:00) Berlin" */
/* Install languages / formats on offer. */
int il_locale_count(void);
const char *il_locale_name(int i);         /* en_US.UTF-8 */
const char *il_locale_label(int i);        /* English (United States) */

/* ------------------------------------------------------------------ */
/* Installing                                                          */
/* ------------------------------------------------------------------ */

enum il_step {
	IL_STEP_PREPARE = 0,     /* checks, unmount leftovers */
	IL_STEP_PARTITION,       /* write the partition table */
	IL_STEP_FORMAT,          /* mkfs.fat, mke2fs */
	IL_STEP_MOUNT,           /* mount root and ESP */
	IL_STEP_EXTRACT,         /* unpack likeos-rootfs.tar.zst */
	IL_STEP_CONFIGURE,       /* fstab, hostname, accounts, ... */
	IL_STEP_BOOTLOADER,      /* ESP files, firmware boot entry */
	IL_STEP_FINALIZE,        /* sync, unmount */
	IL_STEP_COUNT
};

struct il_progress {
	int step;                /* enum il_step */
	double step_fraction;    /* 0..1 within the step (the unpacking: its
	                          * share of the work, bytes and files each at
	                          * their measured cost); never goes back */
	double overall;          /* 0..1 weighted over all steps; never goes
	                          * back */
	const char *detail;      /* "Copying /usr/lib/libgtk-3.so.0" (valid during
	                          * the callback only: a front end that shows it
	                          * later keeps a copy) */
	uint64_t bytes_done;     /* extract step: uncompressed bytes written */
	uint64_t bytes_total;
	uint64_t files_done;     /* entries unpacked (files= of the .info) */
	uint64_t files_total;
	int eta_seconds;         /* -1 when unknown; already smoothed, so a
	                          * front end shows it as it comes */
	/* Added in API 1: the one percentage every front end shows, so that
	 * no two places round differently: floor(overall * 100), held at 99
	 * until the installation has finished; 100 only then. */
	int percent;
};

typedef void (*il_progress_cb)(const struct il_progress *p, void *user);

const char *il_step_title(int step);       /* "Copying LikeOS files" */
double il_step_weight(int step);           /* share of the whole, sums to 1 */

/* Runs every step.  Returns 0, -ECANCELED after il_cancel(), or -errno. */
int il_install(il_ctx *ctx, il_plan *plan, const struct il_config *cfg,
	       il_progress_cb cb, void *user);

/* Requests cancellation; honoured until the partition table is written
 * (il_install then returns -ECANCELED); afterwards it is ignored and the
 * installation runs to its end, since a half-written disk is worse. */
void il_cancel(il_ctx *ctx);
/* 1 while il_cancel() would still stop the installation. */
int il_can_cancel(const il_ctx *ctx);

/* After a finished installation. */
int il_reboot(il_ctx *ctx);
int il_poweroff(il_ctx *ctx);

/* ------------------------------------------------------------------ */
/* Answer files (likeos-install.conf(5))                               */
/* ------------------------------------------------------------------ */

struct il_answers {
	char disk[64];           /* /dev/sda or "auto" (the only eligible disk) */
	int whole_disk;          /* 1: erase the disk (default) */
	int root_partno;         /* when whole_disk is 0 */
	int reboot;              /* reboot when done */
	struct il_config cfg;
};

int il_answers_load(il_ctx *ctx, const char *path, struct il_answers *out);
int il_answers_save(il_ctx *ctx, const char *path, const struct il_answers *a,
		    int include_passwords);

/* ------------------------------------------------------------------ */
/* Additions (compatible extensions of API 1)                          */
/* ------------------------------------------------------------------ */

/* Index of a time zone / locale by name, -1 when unknown. */
int il_timezone_find(const char *name);
int il_locale_find(const char *name);
/* The standard (winter) offset of a zone east of UTC, in minutes. */
int il_timezone_offset_minutes(int i);

/* The disk of a list by device ("/dev/sda"), name ("sda") or "auto" (the
 * only disk that is not the boot medium); NULL when none / ambiguous. */
const struct il_disk *il_disks_find(const struct il_disk_list *l,
				    const char *which);
/* The partition of a disk by number, NULL when there is none. */
const struct il_partition *il_disk_part(const struct il_disk *d, int partno);

/* Checks a whole configuration with the validators above; the first
 * problem goes to *why. */
int il_config_validate(const struct il_config *cfg, char *why, size_t len);

/* Builds a plan from answers (whole disk, or a root partition of the
 * disk).  *out must be freed with il_plan_free. */
int il_answers_plan(il_ctx *ctx, const struct il_answers *a,
		    const struct il_disk_list *disks, il_plan **out);

/* A multi-line, human-readable summary of a plan (one operation per line). */
void il_plan_summary(const il_plan *plan, char *buf, size_t len);

/* The path of the context's log file. */
const char *il_log_path(const il_ctx *ctx);

/* ------------------------------------------------------------------ */
/* Additions for the graphical front end (compatible, API 1)           */
/* ------------------------------------------------------------------ */

/* Passwords: il_validate_password() accepts a password shorter than this,
 * but returns 0 with a warning sentence in *why ("This password is short;
 * 8 characters or more are safer"); *why is empty for a password that
 * needs no word.  A front end shows a non-empty *why with a return of 0 as
 * a warning, not as an error. */
#define IL_PASSWORD_SHORT     8

/* Whether partition partno could be chosen for LikeOS: the choice is made
 * on a copy of the plan and the result validated against src (as
 * il_plan_validate() does), so the answer covers everything -- the
 * partition's kind and size, the EFI system partition, the disk.  0 when
 * it can, else -errno and the reason in *why.  The plan is not changed,
 * nothing is logged and the context's error is left alone. */
int il_plan_can_select_root(const il_plan *plan, int partno,
			    const struct il_source_info *src, char *why,
			    size_t len);
/* Takes back il_plan_select_root(): what the choice added to the plan goes
 * again (the type change and the format of an existing partition), a
 * partition the plan created is a plain data partition again, and
 * il_plan_root() is -1.  Changes made with the other calls stay. */
int il_plan_clear_root(il_plan *plan);

/* Threads.  While any call on a context runs on one thread (il_install()
 * on a front end's worker thread, a disk scan, a check of the image),
 * another thread may call only il_cancel() and il_can_cancel() on that
 * context; both are safe to call at any time (they use atomic operations).
 * Everything else -- il_log(), the plan calls of a plan made with that
 * context, il_last_error() -- waits until the call has returned.  Calls
 * that take no context (il_format_size, the validators, the time zone and
 * locale tables, il_op_describe, il_step_title) may be used from any
 * thread at any time. */

#ifdef __cplusplus
}
#endif

#endif /* LIKEOS_INSTALLER_H */
