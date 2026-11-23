// devd - the LikeOS device manager: shared definitions.
//
// devd keeps the list of disks and volumes, answers clients on
// /var/run/devd/socket and is the only program that mounts, unmounts,
// ejects and formats media for users: USB drives, CD/DVD discs and the
// partitions of built-in disks that the running system does not use.  It
// never mounts anything on its own: the session's volume agent and the
// file manager ask it to.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef DEVD_DEVD_H
#define DEVD_DEVD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "devproto.h"

#define DEVD_RUN_DIR "/var/run/devd"
#define DEVD_SOCKET_PATH DEVP_SOCKET_PATH
#define DEVD_EVENTS_PATH "/dev/devevents"
#define DEVD_MEDIA "/media"

#define DEVD_OUT_MAX (64 * 1024)  /* unsent output per connection */
#define DEVD_MAX_CLIENTS 32
#define DEVD_MAX_PER_UID 8
#define DEVD_MAX_DEVS 128
#define DEVD_MAX_JOBS 4           /* formats at the same time */
#define DEVD_LINES_PER_ROUND 16   /* requests taken from one client per loop */

#define DEVD_NAME_MAX 32          /* device names, NUL included */
#define DEVD_LABEL_MAX 192        /* display label, NUL included */
#define DEVD_UUID_MAX 48
#define DEVD_IDENT_MAX 64         /* vendor, model */
#define DEVD_MNTNAME_MAX 64       /* bytes of a mount directory's name */
#define DEVD_USERDIR_MAX 33       /* /media/<user>: name, NUL included */
#define DEVD_PATH_MAX 256

#define DEVD_POLL_RESCAN_MS 2000  /* without /dev/devevents */
#define DEVD_MOUNT_CHECK_MS 3000  /* the mount table, with events */

/* ---- probing ------------------------------------------------------------- */

#define PROBE_BYTES (68 * 1024)

struct probe_result {
	char fstype[16];   /* protocol id: vfat ext4 ntfs exfat iso9660 swap
			      unknown none */
	int fat_bits;      /* 12, 16, 32 for vfat */
	char label[DEVD_LABEL_MAX]; /* display-safe UTF-8 */
	char uuid[DEVD_UUID_MAX];
};

/* Identify the filesystem in `buf' (`len' bytes from the start of the
 * volume).  `rd' may read more of the volume (the FAT root directory, for
 * the label written by other systems); NULL when only `buf' is available.
 * Always fills `out'; fstype "unknown" when nothing matched. */
typedef int (*probe_read_fn)(void *ctx, uint64_t off, void *dst, size_t n);
void probe_buffer(const uint8_t *buf, size_t len, probe_read_fn rd, void *ctx,
		  struct probe_result *out);
/* Probe the block device node `path'. */
int probe_device(const char *path, struct probe_result *out);

/* UTF-16 (little or big endian) into display-safe UTF-8, through
 * label_display(); unpaired surrogates become '?'. */
void utf16_label(char *dst, size_t cap, const uint8_t *src, size_t units,
		 int big_endian);

/* ---- partition tables ------------------------------------------------------ */

/* The partitions of a disk that belong to the firmware and the installed
 * systems rather than to people: EFI system partitions, reserved and
 * recovery partitions, BIOS boot partitions.  Their start sectors (512-byte
 * units, as /sys/block/<disk>/<part>/start gives them). */
#define PT_RESERVED_MAX 32
struct pt_reserved {
	int n;
	uint64_t start[PT_RESERVED_MAX];
};

/* Read the partition table (GPT, else MBR) through `rd' -- `lbs' is the
 * disk's logical block size.  Fills `out' (n = 0 when there is none). */
void pt_reserved_read(probe_read_fn rd, void *ctx, uint32_t lbs,
		      uint64_t disk_bytes, struct pt_reserved *out);
/* The same from the disk's device node. */
int pt_reserved_device(const char *path, uint32_t lbs, uint64_t disk_bytes,
		       struct pt_reserved *out);
int pt_reserved_has(const struct pt_reserved *pt, uint64_t start);

/* ---- names --------------------------------------------------------------- */

/* A label from a device made safe to show: valid UTF-8 without control
 * characters ('?' for anything else), trailing blanks and NULs gone. */
void label_display(char *dst, size_t cap, const uint8_t *src, size_t n);
/* A mount directory name from the label, else the UUID, else the device
 * name: no '/', no control characters, not "." or "..", no leading '.',
 * at most DEVD_MNTNAME_MAX bytes on a character boundary. */
void mountname_choose(char *dst, size_t cap, const char *label,
		      const char *uuid, const char *devname);
/* `base' with the collision suffix `n' (n >= 1) appended, cut so that the
 * whole stays within DEVD_MNTNAME_MAX bytes. */
void mountname_suffix(char *dst, size_t cap, const char *base, unsigned n);
/* 1 when `s' may be used as /media/<s>: a plain user name. */
int username_ok(const char *s);
/* 1 when `s' is a kernel block device name devd accepts. */
int devname_ok(const char *s);
/* 1 when the `n' bytes at `s' are valid UTF-8 without control characters. */
int utf8_text_ok(const char *s, size_t n);
/* Copy with truncation; always NUL-terminates when cap > 0. */
size_t strlcpy_d(char *dst, const char *src, size_t cap);

/* ---- filesystem table ---------------------------------------------------- */

struct fs_type {
	const char *id;          /* protocol id */
	const char *name;        /* shown to people */
	const char *kernel_type; /* mount(2) type, NULL: cannot mount */
	int is_default;
	size_t labelmax;         /* bytes, 0: no label */
	const uint32_t *clusters; /* allowed cluster/block sizes, 0-terminated */
	uint32_t defcluster;
	const char *mkfs;        /* absolute path, NULL: cannot format */
	/* Check and normalise a label (decoded bytes) into `out'. 0 or -errno */
	int (*label_ok)(const char *in, size_t n, char *out, size_t cap);
	/* The mount(2) data string for a user's mount. */
	void (*mount_opts)(char *out, size_t cap, uid_t uid, gid_t gid);
	/* File systems with a journal: the option that makes the kernel
	 * refuse (EUCLEAN) a volume whose journal must be replayed, so that
	 * the user is asked first; and the option that has it replayed
	 * once the user agreed (NULL: replayed without one). */
	const char *ask_opt;
	const char *recover_opt;
	int always_ro;           /* mounted read-only, always (ISO 9660) */
	/* Fill argv for mkfs (argv[0] = mkfs path).  Returns argc or -errno. */
	int (*mkfs_argv)(char **argv, int max, char *store, size_t store_cap,
			 const char *dev, const char *label, uint32_t cluster,
			 uint32_t sector, int whole_disk, uid_t uid, gid_t gid);
};

const struct fs_type *fs_table(size_t *n);
const struct fs_type *fs_by_id(const char *id);
/* Can devd mount a volume of this probed type? */
int fs_supported(const struct probe_result *pr);
int fs_cluster_ok(const struct fs_type *t, uint32_t cluster);
/* The whole mount(2) data string for a user's mount of a volume of type
 * `t': its owner options, then the ask option -- or, with `recover' (the
 * user agreed to have the journal replayed), the recover option.  0, or
 * -ENAMETOOLONG when it does not fit in `cap'. */
int fs_mount_data(const struct fs_type *t, int recover, uid_t uid, gid_t gid,
		  char *out, size_t cap);

/* ---- the device table ---------------------------------------------------- */

enum dev_kind { DEV_DISK = 0, DEV_PART = 1 };
enum dev_state { DS_READY = 0, DS_BUSY = 1, DS_FORMATTING = 2 };

struct dev {
	char name[DEVD_NAME_MAX];
	char disk[DEVD_NAME_MAX];
	int kind;
	uint32_t major, minor;
	uint64_t size;           /* bytes */
	int removable, usb, system;
	int readonly;            /* write-protected (or optical): read-only */
	int optical;             /* a CD/DVD drive (SCSI type 5) */
	int internal;            /* a built-in disk: neither USB nor optical */
	int reserved;            /* firmware/system partition (EFI, recovery) */
	uint64_t start;          /* partitions: first sector (512-byte units) */
	struct probe_result pr;
	int supported;
	char vendor[DEVD_IDENT_MAX], model[DEVD_IDENT_MAX];
	char mounted[DEVD_PATH_MAX];
	long mountuid;           /* -1: no devd mount */
	int devd_mount;          /* `mounted' is a mount devd made */
	int foreign_mount;       /* mounted by someone else */
	int ejected;
	int state;
	/* disks */
	uint64_t diskseq;
	int nparts;
	int ev_usb;              /* the kernel said USB in an event */
	int partno;              /* partitions */
	int probed;
	uint64_t sig;            /* identity: re-probe when it changes */
};

struct devtab {
	struct dev d[DEVD_MAX_DEVS];
	int n;
};

extern struct devtab g_tab;

struct dev *tab_find(const char *name);
/* The volume rules: a disk with partitions is no volume. */
int dev_is_volume(const struct dev *d);
/* Format a DEV/EVENT line ("DEV", "EVENT ADDED" ...). -1 if it did not fit */
long dev_format_line(struct devp_line *l, const char *prefix,
		     const struct dev *d);

/* Rescan /sys/block and the mount table; send events for what changed.
 * `force' names a disk to probe again ("" or NULL: none); `all' probes
 * every disk again. */
void tab_rescan(const char *force, int all);
/* Note a USB flag from a kernel event for disk `name'. */
void tab_note_usb(const char *name);

/* ---- policy (policy.c: pure decisions, tested on the build machine) ------- */

/* What a request does to a device. */
enum dev_op { OP_MOUNT = 0, OP_UNMOUNT, OP_EJECT, OP_FORMAT };

/* The class of a disk from what the kernel says about it: an optical
 * drive (SCSI type 5), a USB drive, or a built-in disk (neither). */
void dev_classify(struct dev *d, int usb, int optical, int disk_ro,
		  int part_ro);
/* May `op' be done on `d' at all (whoever asks)?  0, or -EPERM and *why.
 *   USB drive:      mount, unmount, eject; format unless read-only
 *   CD/DVD drive:   mount (read-only), unmount, eject; never format
 *   built-in disk:  mount, unmount; never eject or format
 *   system:         nothing (a disk the running system uses, a firmware or
 *                   recovery partition of a built-in disk) */
int dev_policy_check(const struct dev *d, enum dev_op op, const char **why);
/* The mount(2) flags for a user's mount of `d' with filesystem `t':
 * nosuid and nodev always, read-only for read-only media and ISO 9660. */
unsigned long dev_mount_flags(const struct dev *d, const struct fs_type *t);
/* The medium under a mount went (no medium now, or another one): `old' is
 * the device as it was, `cur' as it is now (the same name and number). */
int dev_medium_changed(const struct dev *old, const struct dev *cur);

/* ---- mount table --------------------------------------------------------- */

struct mnt_ent {
	uint32_t major, minor;   /* from mountinfo; 0:0 unknown */
	uint32_t src_major, src_minor; /* from stat() of the source */
	int src_blk;
	char path[DEVD_PATH_MAX];
};

/* Read the mount table.  Returns the count (at most `max') or -1. */
int mnt_read(struct mnt_ent *out, int max);
/* Unescape the \ooo of a mount table field in place. */
void mnt_unescape(char *s);
/* Is `path' a mount devd makes: /media/<user>/<name>?  Sets *uid. */
int mnt_is_devd_path(const char *path, long *uid);

/* ---- mounting ------------------------------------------------------------ */

int media_setup(void);                 /* /media, stale directories */
/* `recover': the journal is replayed (the user agreed); without it a volume
 * whose journal must be replayed is refused with -EUCLEAN. */
int do_mount(struct dev *d, uid_t uid, gid_t gid, int recover, char *path,
	     size_t cap, const char **msg);
int do_unmount(struct dev *d, uid_t uid, const char **msg);
int do_eject(const char *disk, uid_t uid, const char **msg);
/* sync and unmount a devd mount by path (and remove its directory) */
int unmount_path(const char *path, const char **msg);
/* /dev/<name> is the block device the table says it is */
int dev_node_ok(const struct dev *d);
/* A removed disk's devd mounts: detach them and remove the directories. */
void detach_removed(const struct dev *d);
/* The CD/DVD drive's tray opens (CDROMEJECT).  0 or -errno. */
int eject_tray(const char *disk);

/* ---- formatting ---------------------------------------------------------- */

struct client;
/* FORMAT for client `c' on volume `d' (authorized, policy checked):
 * tok[0] = "FORMAT", tok[1] = the name, then key=value tokens.  0 when the
 * job runs (the reply comes when it ends), else -errno and *msg. */
int format_start(struct client *c, struct dev *d, char *const *tok, int ntok,
		 const char **msg);
int format_busy(const char *name);     /* formatting this volume? */
int format_disk_busy(const char *disk);
struct pollfd;
int format_pollfds(struct pollfd *pfd, int max);
void format_handle(struct pollfd *pfd, int n);
void format_reap(void);
void format_kill_all(void);
void format_client_gone(int client_id);

/* ---- authorization ------------------------------------------------------- */

/* Pure decision on the texts of /sys/kernel/console_user,
 * /sys/kernel/console_session and /proc/<pid>/sessionid. */
int auth_console_decide(uint32_t uid, const char *console_user,
			const char *console_session, const char *peer_session);
/* May this peer mount, unmount, eject and format? */
int auth_may_modify(uint32_t uid, int32_t pid);

/* ---- clients ------------------------------------------------------------- */

struct client {
	int fd;
	int id;
	uint32_t uid, gid;
	int32_t pid;
	int subscribed;
	int dead;
	int waiting;             /* a FORMAT is pending: take no more requests */
	int eof;                 /* the peer sends no more */
	char in[DEVP_LINE_MAX];
	size_t inlen;
	char *out;
	size_t outlen;
	struct client *next;
};

extern struct client *g_clients;

int clients_listen(const char *path);
void clients_accept(int lfd);
void client_read(struct client *c);
void client_flush(struct client *c);
void client_process(struct client *c);
void clients_reap(void);
int client_has_line(const struct client *c);
struct client *client_by_id(int id);
void client_send(struct client *c, const char *buf, size_t len);
void client_send_line(struct client *c, struct devp_line *l);
void client_err(struct client *c, const char *verb, const char *name, int err,
		const char *msg);
/* An EVENT line to every subscriber. */
void clients_broadcast(struct devp_line *l);
void clients_close_all(void);

/* ---- events -------------------------------------------------------------- */

int events_open(void);
/* Read what is queued; returns 1 when the table must be rescanned. */
int events_read(int fd, int *all, char *force, size_t force_cap);

/* ---- logging and helpers ------------------------------------------------- */

void log_msg(int level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
#define LOG_ERR 0
#define LOG_WARN 1
#define LOG_INFO 2
#define LOG_DEBUG 3
extern int g_log_level;
#define log_err(...) log_msg(LOG_ERR, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_WARN, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)

/* Read a small file into buf (NUL-terminated).  Returns the length or -1. */
long read_file(const char *path, char *buf, size_t cap);
/* The same with trailing white space removed. */
long read_attr(const char *path, char *buf, size_t cap);
uint64_t now_ms(void);

#endif
