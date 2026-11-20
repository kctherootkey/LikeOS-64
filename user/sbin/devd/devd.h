// devd - the LikeOS device manager: shared definitions.
//
// devd keeps the list of disks and volumes, answers clients on
// /var/run/devd/socket and is the only program that mounts, unmounts and
// formats removable media for users.  It never mounts anything on its own:
// the session's volume agent and the file manager ask it to.
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
/* removable=1 usb=1 system=0 */
int dev_policy_ok(const struct dev *d);
/* Format a DEV/EVENT line ("DEV", "EVENT ADDED" ...). -1 if it did not fit */
long dev_format_line(struct devp_line *l, const char *prefix,
		     const struct dev *d);

/* Rescan /sys/block and the mount table; send events for what changed.
 * `force' names a disk to probe again ("" or NULL: none); `all' probes
 * every disk again. */
void tab_rescan(const char *force, int all);
/* Note a USB flag from a kernel event for disk `name'. */
void tab_note_usb(const char *name);

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
int do_mount(struct dev *d, uid_t uid, gid_t gid, char *path, size_t cap,
	     const char **msg);
int do_unmount(struct dev *d, uid_t uid, const char **msg);
int do_eject(const char *disk, uid_t uid, const char **msg);
/* sync and unmount a devd mount by path (and remove its directory) */
int unmount_path(const char *path, const char **msg);
/* /dev/<name> is the block device the table says it is */
int dev_node_ok(const struct dev *d);
/* A removed disk's devd mounts: detach them and remove the directories. */
void detach_removed(const struct dev *d);

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
