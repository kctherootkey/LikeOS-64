/* LikeOS - ISO9660 filesystem driver (read-only)
 *
 * ISO9660 levels 1-3 (primary, supplementary and enhanced volume
 * descriptors), multi-extent files, interleaved files, extended attribute
 * records, Rock Ridge (SUSP SP CE ST ER, RRIP PX PN SL NM CL PL RE TF SF,
 * deep directory relocation), Joliet (UCS-2 / UTF-16 names, levels 1-3),
 * zisofs (RRIP ZF entries, compressed-file header and block pointers, with
 * the driver's own inflate), 512 / 1024 / 2048-byte logical blocks on
 * 512-, 2048- and 4096-byte sector devices, hybrid images, multi-session
 * images (sbsector= / session=).
 *
 * Conventions shared with the NTFS and exFAT drivers, the ISO-specific
 * conventions, locking and the mount options: isofs.h.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/isofs.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/kdev_t.h>

/* 1 once mount() brings up a working instance (isofs.h, section 6).  While
 * 0, probe() answers 0 so that "auto" never picks this driver, and mount()
 * refuses with ST_UNSUPPORTED. */
#define ISOFS_DRIVER_READY 1

/* open(2) flag the syscall header does not define (the VFS's value). */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif

/* ===========================================================================
 * Limits (hostile media, isofs.h section 5)
 * ===========================================================================
 *
 * Every number here bounds work or memory that the medium would otherwise
 * decide.  None of them is reached by any image a mastering program
 * produces; each is far above what the format's own rules allow in
 * practice. */

/* Volume descriptors looked at by probe() (the primary one must be among
 * them) and by mount() (the whole set up to the terminator). */
#define ISOFS_PROBE_MAX_VD 32
#define ISOFS_MOUNT_MAX_VD 64

/* Directory records of one multi-extent file.  4096 extents of the largest
 * size a record can describe (4 GB - 2 KB) is 16 TB. */
#define ISOFS_MAX_EXTENTS 4096

/* SUSP: continuation areas followed from one directory record, and the
 * bytes of System Use data examined for it, across all of them. */
#define ISOFS_MAX_CE_HOPS 32
#define ISOFS_MAX_SU_BYTES 65536

/* Longest name handed to the VFS (Rock Ridge NM, Joliet, plain ISO). */
#define ISOFS_NAME_MAX 255

/* Longest Rock Ridge symbolic link target assembled from SL entries: a
 * whole path (VFS_MAX_PATH, the NUL included), as any other filesystem's. */
#define ISOFS_SYMLINK_MAX (VFS_MAX_PATH - 1)

/* Largest directory read: 64 MB holds about a million directory records. */
#define ISOFS_DIR_MAX_BYTES (64UL << 20)

/* Symbolic links followed inside the driver for one path (when the VFS
 * does not take the walk over), and directories one walk may nest. */
#define ISOFS_MAX_LINK_HOPS 40
#define ISOFS_MAX_DEPTH 128

/* Stream-handle table: slots for dynamic streams, and how many streams
 * that nobody uses are kept for quick reopening before the oldest go. */
#define ISOFS_STREAM_SLOTS 16384
#define ISOFS_STREAM_KEEP 2048
/* ...and how many bytes of extent arrays those unused streams may hold in
 * all (a multi-extent file's array is up to ISOFS_MAX_EXTENTS * 48 bytes;
 * 2048 of those would pin 400 MB).  Beyond it the oldest unused stream
 * WITH an array goes first. */
#define ISOFS_STREAM_KEEP_EXT_BYTES (4UL << 20)
#define ISOFS_STREAM_BUCKETS 512

/* zisofs: block sizes 2^15 .. 2^17, the uncompressed size is 32 bits, and
 * the compressed form of one block may exceed the block only by the
 * stored-block overhead of deflate plus the zlib wrapper. */
#define ISOFS_ZF_MIN_LOG 15
#define ISOFS_ZF_MAX_LOG 17
#define ISOFS_ZF_HEADER_MIN 16
#define ISOFS_ZF_MAX_CSIZE(bs) ((bs) + ((bs) >> 4) + 1024)

/* The block size the page cache sees never exceeds this (one page holds
 * at least two logical blocks, so a record never straddles a page). */
#define ISOFS_LBS_MAX 2048
#define ISOFS_LBS_MIN 512

/* Guard device: largest transfer it forwards to the real device at once
 * (in real sectors) when it translates sector sizes. */
#define ISOFS_GUARD_BOUNCE_SECTORS 32

/* ===========================================================================
 * On-disk layout (ECMA-119 / ISO 9660, RRIP, SUSP, Joliet, zisofs)
 * ===========================================================================
 *
 * Numbers are recorded little-endian, big-endian or both ("both-endian":
 * the little-endian copy first).  Only the little-endian copy is read; a
 * strict mount (check=strict) also compares the big-endian copy of the
 * fields that matter and refuses a disagreement. */

/* Volume descriptor types and the offsets of the fields used. */
#define ISO_VD_BOOT 0
#define ISO_VD_PRIMARY 1
#define ISO_VD_SUPPLEMENTARY 2
#define ISO_VD_PARTITION 3
#define ISO_VD_TERMINATOR 255

#define ISO_VD_OFF_TYPE 0
#define ISO_VD_OFF_ID 1 /* "CD001" */
#define ISO_VD_OFF_VERSION 6
#define ISO_VD_OFF_FLAGS 7 /* supplementary: bit 0 = non-ISO2375 escapes */
#define ISO_VD_OFF_SYSID 8 /* 32 bytes */
#define ISO_VD_OFF_VOLID 40 /* 32 bytes */
#define ISO_VD_OFF_SPACE 80 /* both-endian 32: volume space size */
#define ISO_VD_OFF_ESCAPES 88 /* 32 bytes: Joliet "%/@" "%/C" "%/E" */
#define ISO_VD_OFF_SETSIZE 120
#define ISO_VD_OFF_SEQNO 124
#define ISO_VD_OFF_LBS 128 /* both-endian 16: logical block size */
#define ISO_VD_OFF_PTSIZE 132
#define ISO_VD_OFF_ROOT 156 /* 34-byte root directory record */
#define ISO_VD_OFF_CREATED 813 /* 17-byte dec-digit date */
#define ISO_VD_OFF_MODIFIED 830
#define ISO_VD_OFF_FSVER 881 /* file structure version: 1, or 2 (ISO 9660:1999) */

/* Directory record (ECMA-119 9.1). */
#define ISO_DR_LEN 0 /* length of the record */
#define ISO_DR_XAR 1 /* extended attribute record length, blocks */
#define ISO_DR_EXTENT 2 /* both-endian 32: first logical block */
#define ISO_DR_SIZE 10 /* both-endian 32: data length */
#define ISO_DR_DATE 18 /* 7-byte recording date */
#define ISO_DR_FLAGS 25
#define ISO_DR_FUS 26 /* file unit size (interleaving) */
#define ISO_DR_IGS 27 /* interleave gap size */
#define ISO_DR_VSEQ 28 /* both-endian 16: volume sequence number */
#define ISO_DR_NAMELEN 32
#define ISO_DR_NAME 33
#define ISO_DR_MIN 34 /* 33 bytes + a 1-byte identifier */

#define ISO_FLAG_HIDDEN 0x01 /* "existence": hide from the user */
#define ISO_FLAG_DIR 0x02
#define ISO_FLAG_ASSOC 0x04 /* associated file */
#define ISO_FLAG_RECORD 0x08 /* record format in the XAR */
#define ISO_FLAG_PROTECT 0x10 /* permissions in the XAR */
#define ISO_FLAG_MULTI 0x80 /* more extents of this file follow */

/* SUSP / RRIP signatures as 16-bit numbers (first character high). */
#define SUSP_SIG(a, b) ((unsigned)(((unsigned)(a) << 8) | (unsigned)(b)))

/* RRIP TF flags. */
#define RR_TF_CREATE 0x01
#define RR_TF_MODIFY 0x02
#define RR_TF_ACCESS 0x04
#define RR_TF_ATTR 0x08
#define RR_TF_BACKUP 0x10
#define RR_TF_EXPIRE 0x20
#define RR_TF_EFFECT 0x40
#define RR_TF_LONG 0x80

/* RRIP SL component flags and NM flags. */
#define RR_SLC_CONTINUE 0x01
#define RR_SLC_CURRENT 0x02
#define RR_SLC_PARENT 0x04
#define RR_SLC_ROOT 0x08
#define RR_SLC_VOLROOT 0x10
#define RR_SLC_HOST 0x20
#define RR_NM_CONTINUE 0x01
#define RR_NM_CURRENT 0x02
#define RR_NM_PARENT 0x04

/* POSIX file type bits as RRIP PX records them. */
#define RR_S_IFMT 0170000u
#define RR_S_IFSOCK 0140000u
#define RR_S_IFLNK 0120000u
#define RR_S_IFREG 0100000u
#define RR_S_IFBLK 0060000u
#define RR_S_IFDIR 0040000u
#define RR_S_IFCHR 0020000u
#define RR_S_IFIFO 0010000u

/* zisofs compressed-file header. */
static const uint8_t isofs_zf_magic[8] = { 0x37, 0xE4, 0x53, 0x96,
					   0xC9, 0xDB, 0xD6, 0x07 };

/* ===========================================================================
 * Small helpers: byte order, overflow-checked arithmetic, characters
 * =========================================================================== */

static inline uint32_t isofs_le16(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t isofs_be16(const uint8_t *p)
{
	return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static inline uint32_t isofs_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline uint32_t isofs_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* A both-endian 32-bit number (ECMA-119 7.3.3): the little-endian half. */
static inline uint32_t isofs_733(const uint8_t *p)
{
	return isofs_le32(p);
}

/* Do the two halves of a both-endian 32-bit number agree? */
static inline int isofs_733_ok(const uint8_t *p)
{
	return isofs_le32(p) == isofs_be32(p + 4);
}

/* A both-endian 16-bit number (7.2.3). */
static inline uint32_t isofs_723(const uint8_t *p)
{
	return isofs_le16(p);
}

static inline int isofs_723_ok(const uint8_t *p)
{
	return isofs_le16(p) == isofs_be16(p + 2);
}

/* a + b without wrapping: 0 and *out untouched on overflow. */
static inline int isofs_add_ok(uint64_t a, uint64_t b, uint64_t *out)
{
	uint64_t r;

	if (__builtin_add_overflow(a, b, &r))
		return 0;
	*out = r;
	return 1;
}

static inline int isofs_mul_ok(uint64_t a, uint64_t b, uint64_t *out)
{
	uint64_t r;

	if (__builtin_mul_overflow(a, b, &r))
		return 0;
	*out = r;
	return 1;
}

static inline uint64_t isofs_min64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

static inline uint64_t isofs_div_up(uint64_t a, uint64_t b)
{
	return a / b + (a % b != 0);
}

static inline char isofs_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static unsigned isofs_strlen(const char *s)
{
	unsigned n = 0;

	while (s[n])
		n++;
	return n;
}

static int isofs_memeq(const void *a, const void *b, unsigned long n)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;

	for (unsigned long i = 0; i < n; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

/* Is the name a single "." or ".." (which a directory never holds as an
 * ordinary entry)? */
static int isofs_is_dot_name(const char *n, unsigned len)
{
	return (len == 1 && n[0] == '.') ||
	       (len == 2 && n[0] == '.' && n[1] == '.');
}

/* ===========================================================================
 * Time
 * =========================================================================== */

/* Days from 1970-01-01 to the civil date y-m-d (proleptic Gregorian), for
 * any year: the era arithmetic keeps everything in integers. */
static int64_t isofs_days_from_civil(int64_t y, unsigned m, unsigned d)
{
	int64_t era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

/* Seconds since the epoch of a date with an offset from UTC in units of 15
 * minutes (the ISO 9660 representation of a time zone).  Fields out of
 * range are clamped rather than trusted: the medium decides nothing
 * beyond the value it gets. */
static int64_t isofs_mktime(int64_t year, int mon, int day, int hour, int min,
			    int sec, int gmtoff15)
{
	int64_t days, t;

	if (mon < 1)
		mon = 1;
	if (mon > 12)
		mon = 12;
	if (day < 1)
		day = 1;
	if (day > 31)
		day = 31;
	if (hour < 0 || hour > 23)
		hour = 0;
	if (min < 0 || min > 59)
		min = 0;
	if (sec < 0 || sec > 60)
		sec = 0;
	if (gmtoff15 < -48 || gmtoff15 > 52)
		gmtoff15 = 0;
	days = isofs_days_from_civil(year, (unsigned)mon, (unsigned)day);
	t = days * 86400 + hour * 3600 + min * 60 + sec;
	return t - (int64_t)gmtoff15 * 15 * 60;
}

/* The 7-byte binary date of a directory record (9.1.5) and of a short-form
 * RRIP TF stamp: years since 1900, month, day, hour, minute, second, and
 * the signed offset from UTC.  An all-zero date means "not specified". */
static int64_t isofs_date7(const uint8_t *p)
{
	if (!p[0] && !p[1] && !p[2] && !p[3] && !p[4] && !p[5])
		return 0;
	return isofs_mktime(1900 + (int64_t)p[0], p[1], p[2], p[3], p[4], p[5],
			    (int)(int8_t)p[6]);
}

static int isofs_digits(const uint8_t *p, unsigned n, int *out)
{
	int v = 0;

	for (unsigned i = 0; i < n; i++) {
		if (p[i] < '0' || p[i] > '9')
			return 0;
		v = v * 10 + (p[i] - '0');
	}
	*out = v;
	return 1;
}

/* The 17-byte dec-digit date (8.4.26.1) of the volume descriptors and of a
 * long-form TF stamp: "YYYYMMDDHHMMSScc" and the offset byte.  Digits that
 * are not digits (all zeros, or all '0' with a zero offset: "not
 * specified") give 0. */
static int64_t isofs_date17(const uint8_t *p, uint32_t *nsec)
{
	int y, mo, d, h, mi, s, cs;

	if (nsec)
		*nsec = 0;
	if (!isofs_digits(p, 4, &y) || !isofs_digits(p + 4, 2, &mo) ||
	    !isofs_digits(p + 6, 2, &d) || !isofs_digits(p + 8, 2, &h) ||
	    !isofs_digits(p + 10, 2, &mi) || !isofs_digits(p + 12, 2, &s) ||
	    !isofs_digits(p + 14, 2, &cs))
		return 0;
	if (y == 0 && mo == 0 && d == 0)
		return 0;
	if (nsec)
		*nsec = (uint32_t)cs * 10000000u;
	return isofs_mktime(y, mo, d, h, mi, s, (int)(int8_t)p[16]);
}

/* ===========================================================================
 * The metadata lock: a recursive reader/writer semaphore
 * ===========================================================================
 *
 * isofs.h section 4.  SHARED for path resolution, stat, readdir and the
 * page cache's mapping calls (lock_map); EXCLUSIVE for the stream table's
 * eviction, a remount's option change and unmount.  The exclusive owner
 * may take it again in either mode (a depth count).  A task that already
 * holds it shared may take it shared again even with a writer queued --
 * a nested page-in under a read must not wait behind a writer that waits
 * for that very read.
 *
 * Fairness alternates: queued readers are let in as one cohort when an
 * exclusive hold ends, and new readers defer to queued writers otherwise,
 * so neither side can starve the other.
 *
 * task->fs_rdepth / fs_held are maintained as by every other filesystem
 * lock: the scheduler and the memory-map semaphore consult them.
 *
 * Holders that die: the scheduler never ends another task in place -- a
 * task asleep in here is woken by a fatal signal and leaves through its
 * own unwind, releasing what it holds.  The one way to die INSIDE the
 * driver is a fault in kernel mode on a user address (the crash handler
 * abandons the frame).  Two rules make that harmless:
 *
 *   1. user memory is never touched while this lock is held, in either
 *      mode (readdir assembles its entries in a kernel buffer and copies
 *      them out after unlocking; read() holds no lock at all);
 *   2. shared holds are attributed: every shared acquisition outside an
 *      exclusive hold is recorded per task in `rh' (ISOFS_RWSEM_TRACK
 *      tasks at a time; holds beyond that are counted in `untracked'),
 *      and release_locks_for_task() takes a dead task's exclusive hold
 *      AND its recorded shared holds back -- so even a path that broke
 *      rule 1 could not leave the lock held for ever. */

/* Tasks whose shared holds are recorded at a time, per lock. */
#define ISOFS_RWSEM_TRACK 64

typedef struct isofs_rhold {
	uint64_t task; /* holder's task id                 */
	int count; /* its shared holds; 0: slot free   */
} isofs_rhold_t;

typedef struct isofs_rwsem {
	volatile int readers; /* shared holders                      */
	volatile int writer; /* 1 while held exclusive              */
	volatile uint64_t owner; /* exclusive owner's task id           */
	volatile int wdepth; /* exclusive recursion depth           */
	volatile int w_wait; /* writers waiting                     */
	volatile int r_wait; /* readers waiting (the next cohort)   */
	volatile int reader_turn; /* that cohort goes before writers  */
	int untracked; /* shared holds not in rh[]            */
	isofs_rhold_t rh[ISOFS_RWSEM_TRACK];
	spinlock_t lock;
} isofs_rwsem_t;

static void isofs_rwsem_init(isofs_rwsem_t *s, const char *name)
{
	s->readers = 0;
	s->writer = 0;
	s->owner = (uint64_t)-1;
	s->wdepth = 0;
	s->w_wait = 0;
	s->r_wait = 0;
	s->reader_turn = 0;
	s->untracked = 0;
	for (unsigned i = 0; i < ISOFS_RWSEM_TRACK; i++) {
		s->rh[i].task = 0;
		s->rh[i].count = 0;
	}
	spinlock_init(&s->lock, name);
}

/* Record one more shared hold of task `id' (lock held). */
static void isofs_rhold_add(isofs_rwsem_t *s, task_t *cur)
{
	int free_slot = -1;

	if (!cur) {
		s->untracked++;
		return;
	}
	for (int i = 0; i < ISOFS_RWSEM_TRACK; i++) {
		if (s->rh[i].count && s->rh[i].task == (uint64_t)cur->id) {
			s->rh[i].count++;
			return;
		}
		if (!s->rh[i].count && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {
		s->untracked++;
		return;
	}
	s->rh[free_slot].task = (uint64_t)cur->id;
	s->rh[free_slot].count = 1;
}

/* Forget one shared hold of the current task (lock held). */
static void isofs_rhold_del(isofs_rwsem_t *s, task_t *cur)
{
	if (cur)
		for (int i = 0; i < ISOFS_RWSEM_TRACK; i++)
			if (s->rh[i].count &&
			    s->rh[i].task == (uint64_t)cur->id) {
				s->rh[i].count--;
				return;
			}
	if (!WARN_ON(s->untracked <= 0))
		s->untracked--;
}

/* Sleep on the semaphore's channel; called with its spinlock held, which
 * is released.  The waker claims BLOCKED -> READY, and the scheduler copes
 * with a wake that comes before the sleep. */
static void isofs_rwsem_wait(isofs_rwsem_t *s, uint64_t *flags)
{
	task_t *cur = sched_current();

	if (cur) {
		cur->wait_channel = (void *)s;
		task_set_blocked(cur);
	}
	spin_unlock_irqrestore(&s->lock, *flags);
	sched_schedule();
}

static void isofs_rwsem_down_read(isofs_rwsem_t *s)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	int queued = 0;

	might_sleep();
	for (;;) {
		uint64_t flags;
		int defer;

		spin_lock_irqsave(&s->lock, &flags);
		if (cur && s->writer && s->owner == me) {
			/* Shared under our own exclusive hold. */
			s->wdepth++;
			cur->fs_held++;
			if (queued)
				s->r_wait--;
			spin_unlock_irqrestore(&s->lock, flags);
			return;
		}
		defer = s->w_wait && !s->reader_turn &&
			!(cur && cur->fs_rdepth);
		if (!s->writer && !defer) {
			s->readers++;
			isofs_rhold_add(s, cur);
			if (cur) {
				cur->fs_rdepth++;
				cur->fs_held++;
			}
			if (queued) {
				s->r_wait--;
				if (s->r_wait == 0)
					s->reader_turn = 0;
			}
			spin_unlock_irqrestore(&s->lock, flags);
			return;
		}
		if (!queued) {
			s->r_wait++;
			queued = 1;
		}
		isofs_rwsem_wait(s, &flags);
	}
}

static void isofs_rwsem_up_read(isofs_rwsem_t *s)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	uint64_t flags;
	int wake;

	spin_lock_irqsave(&s->lock, &flags);
	if (cur && s->writer && s->owner == me) {
		WARN_ON(s->wdepth <= 1);
		s->wdepth--;
		if (cur->fs_held > 0)
			cur->fs_held--;
		spin_unlock_irqrestore(&s->lock, flags);
		return;
	}
	WARN_ON(s->readers <= 0);
	if (s->readers > 0) {
		s->readers--;
		isofs_rhold_del(s, cur);
	}
	if (cur && cur->fs_rdepth > 0)
		cur->fs_rdepth--;
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	wake = s->readers == 0;
	spin_unlock_irqrestore(&s->lock, flags);
	if (wake)
		sched_wake_channel((void *)s);
}

/* Exclusive.  With `try', never sleeps: 1 when taken, 0 when not. */
static int isofs_rwsem_down_write_common(isofs_rwsem_t *s, int try)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	int queued = 0;

	if (!try)
		might_sleep();
	for (;;) {
		uint64_t flags;
		int cohort;

		spin_lock_irqsave(&s->lock, &flags);
		if (s->writer && s->owner == me && cur) {
			s->wdepth++;
			if (queued)
				s->w_wait--;
			cur->fs_held++;
			spin_unlock_irqrestore(&s->lock, flags);
			return 1;
		}
		cohort = s->reader_turn && s->r_wait > 0;
		if (!s->writer && s->readers == 0 && !cohort) {
			s->writer = 1;
			s->owner = me;
			s->wdepth = 1;
			if (queued)
				s->w_wait--;
			if (cur)
				cur->fs_held++;
			spin_unlock_irqrestore(&s->lock, flags);
			return 1;
		}
		if (try) {
			spin_unlock_irqrestore(&s->lock, flags);
			return 0;
		}
		if (!queued) {
			s->w_wait++;
			queued = 1;
		}
		isofs_rwsem_wait(s, &flags);
	}
}

static void isofs_rwsem_down_write(isofs_rwsem_t *s)
{
	(void)isofs_rwsem_down_write_common(s, 0);
}

static int isofs_rwsem_try_write(isofs_rwsem_t *s)
{
	return isofs_rwsem_down_write_common(s, 1);
}

static void isofs_rwsem_up_write(isofs_rwsem_t *s)
{
	task_t *cur = sched_current();
	uint64_t flags;

	spin_lock_irqsave(&s->lock, &flags);
	WARN_ON(!s->writer || s->wdepth <= 0);
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	if (s->wdepth > 1) {
		s->wdepth--;
		spin_unlock_irqrestore(&s->lock, flags);
		return;
	}
	s->writer = 0;
	s->owner = (uint64_t)-1;
	s->wdepth = 0;
	if (s->r_wait > 0)
		s->reader_turn = 1;
	spin_unlock_irqrestore(&s->lock, flags);
	sched_wake_channel((void *)s);
}

/* Does the current task hold it exclusive? */
static int isofs_rwsem_held_excl(isofs_rwsem_t *s)
{
	task_t *cur = sched_current();

	return s->writer && cur && s->owner == (uint64_t)cur->id;
}

/* A task that died inside the driver: give back its exclusive hold and
 * every shared hold recorded for it (see the notes at the top). */
static int isofs_rwsem_release_owner(isofs_rwsem_t *s, uint64_t task_id)
{
	uint64_t flags;
	int released = 0;

	spin_lock_irqsave(&s->lock, &flags);
	for (int i = 0; i < ISOFS_RWSEM_TRACK; i++) {
		if (!s->rh[i].count || s->rh[i].task != task_id)
			continue;
		WARN_ON(s->readers < s->rh[i].count);
		s->readers -= s->rh[i].count;
		if (s->readers < 0)
			s->readers = 0;
		s->rh[i].count = 0;
		released = 1;
	}
	if (s->writer && s->owner == task_id) {
		s->writer = 0;
		s->owner = (uint64_t)-1;
		s->wdepth = 0;
		if (s->r_wait > 0)
			s->reader_turn = 1;
		released = 1;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	if (released)
		sched_wake_channel((void *)s);
	return released;
}

/* ===========================================================================
 * Instance structures
 * =========================================================================== */

/* One extent of a file's data.  The data of a record starts `xar' blocks
 * into its extent (the extended attribute record comes first); `lba'
 * already includes that. */
typedef struct isofs_extent {
	uint64_t lba; /* first logical block of the data          */
	uint64_t len; /* bytes of data                             */
	uint64_t foff; /* byte offset in the file of its first byte */
	uint64_t fblk; /* file block index of its first block       */
	uint64_t nblk; /* logical blocks the data occupies          */
	uint32_t fus; /* interleaving: file unit size, blocks (0 = none) */
	uint32_t igs; /* interleaving: gap between units, blocks   */
} isofs_extent_t;

/* Stream flags. */
#define ISOFS_SF_DIR 0x0001 /* a directory                              */
#define ISOFS_SF_ZISOFS 0x0002 /* zisofs-compressed: NOFILL               */
#define ISOFS_SF_UNALIGNED 0x0004 /* an extent ends inside a block: NOFILL */
#define ISOFS_SF_SPARSE 0x0008 /* RRIP SF sparse layout: unreadable       */
#define ISOFS_SF_VOLUME 0x0010 /* the whole volume (reserved handle)      */
#define ISOFS_SF_NOFILL (ISOFS_SF_ZISOFS | ISOFS_SF_UNALIGNED | ISOFS_SF_SPARSE)

/* A stream: one byte sequence of the volume behind one handle (isofs.h,
 * section 1).  Immutable once published, except for refs and the LRU
 * links, which the table's spinlock guards. */
typedef struct isofs_stream {
	unsigned long key; /* identity: the object's st_ino                */
	unsigned long handle; /* 1..ISOFS_HANDLE_MAX                          */
	unsigned long chain; /* ISOFS_BID_ENC(handle, 0)                     */
	uint32_t flags; /* ISOFS_SF_*                                   */
	int refs; /* users; 0: on the LRU, evictable              */
	uint64_t size; /* bytes a read sees (zisofs: uncompressed)     */
	uint64_t raw_size; /* bytes recorded on the medium                 */
	uint64_t nblocks; /* logical blocks covering `size' (block_at)    */
	uint32_t nextents;
	isofs_extent_t *ext; /* file order; `ext1' for a single one          */
	isofs_extent_t ext1;
	uint64_t ext_bytes; /* bytes of `ext' on the heap (0 for `ext1')    */
	/* zisofs */
	uint32_t zf_block_log;
	uint32_t zf_header; /* header bytes (pointer table follows)         */
	uint64_t zf_nblocks; /* compressed blocks                           */
	/* table links */
	struct isofs_stream *hash_next;
	struct isofs_stream *lru_prev, *lru_next;
} isofs_stream_t;

/* A slot of the handle table that is being retired: its handle is not
 * handed out again until the page cache has forgotten the old stream. */
#define ISOFS_SLOT_RETIRING ((isofs_stream_t *)1)

/* Mount options. */
struct isofs_opts {
	uint32_t uid, gid;
	uint32_t fmode, dmode; /* permission bits without Rock Ridge */
	int norock, nojoliet;
	int strict; /* check=strict                          */
	int map_off; /* map=off                               */
	int hide; /* hide: hidden and associated files out  */
	int unhide; /* unhide / showassoc: both shown         */
	int cruft; /* mask file lengths to 24 bits           */
	int nocompress; /* show zisofs files compressed          */
	int override_rr; /* overriderockperms                     */
	uint32_t block; /* block=: logical block size override (0 none) */
	int session; /* session=N (-1: none)                  */
	long sbsector; /* sbsector=N (-1: none)                 */
	int have_uid, have_gid, have_mode, have_dmode;
};

/* The mounted instance. */
typedef struct isofs_fs {
	vfs_superblock_t sb; /* sb.fs_private = this                  */
	/* The page cache's view of the device (isofs.h, ISO9660 specifics:
	 * "The guard block device").  Embedded: the read callback finds the
	 * instance from its address. */
	block_device_t guard;
	block_device_t *bdev; /* the real device                       */
	vfs_mount_t *mnt;
	isofs_rwsem_t meta;
	struct isofs_opts o;

	/* geometry */
	unsigned long lbs; /* logical block size: 512, 1024, 2048    */
	unsigned lbs_shift;
	unsigned long real_ss; /* the device's sector size               */
	unsigned long vss; /* sector size the guard presents         */
	unsigned long view_per_real; /* real_ss / vss (1 when no translation) */
	unsigned long spb; /* guard sectors per logical block        */
	uint64_t view_sectors; /* guard sectors (without the +1 offset)  */
	uint64_t dev_blocks; /* logical blocks the device holds        */
	uint64_t vol_blocks; /* volume space size (from the descriptor) */
	uint64_t session_start; /* 2048-byte sector of the descriptor set */

	/* volume */
	int rock; /* Rock Ridge names and attributes        */
	int joliet; /* 0, or the Joliet level 1..3            */
	int fs_version; /* file structure version (2: 9660:1999)  */
	unsigned su_skip; /* SUSP SP LEN_SKP                        */
	int rr_version; /* 112 when PX carries a serial (1.12)    */
	uint64_t root_ino; /* root directory: data LBA               */
	uint64_t root_size;
	int64_t vol_mtime; /* descriptor modification / creation time */
	char volid[33];

	/* stream table */
	spinlock_t st_lock;
	isofs_stream_t *st_hash[ISOFS_STREAM_BUCKETS];
	isofs_stream_t **slots; /* [ISOFS_STREAM_SLOTS]          */
	unsigned long slot_cursor;
	unsigned long nstreams; /* published dynamic streams     */
	unsigned long nunused; /* of those, refs == 0 (on LRU)  */
	uint64_t unused_ext_bytes; /* their heap extent arrays      */
	isofs_stream_t lru; /* sentinel: next = most recent  */
	isofs_stream_t vol; /* handle ISOFS_HANDLE_VOLUME     */
	isofs_stream_t *root_st; /* held for the mount's lifetime */
	volatile int dying;
	int sf_logged; /* the SF message was printed            */
} isofs_fs_t;

/* An open file or directory. */
typedef struct isofs_file {
	vfs_file_t vfs; /* first; vfs.fs_private == this       */
	isofs_fs_t *fs;
	isofs_stream_t *st;
	struct kstat kst; /* attributes at open time            */
	uint64_t pos;
	uint64_t parent_ino; /* for readdir's ".."                  */
	int is_dir;
	pc_readahead_t ra;
	spinlock_t pos_lock; /* pos / ra between concurrent reads   */
	ic_inode_t *ic; /* inode-cache entry (regular files)    */
	/* Directories: the position the last readdir left `pos' at -- a
	 * record-set start this driver produced.  Any other position (from
	 * lseek / seekdir) is first moved to the record-set start at or
	 * after it (isofs_dir_resync), never parsed where it points. */
	uint64_t dir_cookie;
} isofs_file_t;

static inline isofs_fs_t *isofs_fs_of_sb(vfs_superblock_t *sb)
{
	return sb ? (isofs_fs_t *)sb->fs_private : NULL;
}

static inline isofs_fs_t *isofs_fs_of(vfs_mount_t *m)
{
	isofs_fs_t *fs = m ? (isofs_fs_t *)m->fs_data : NULL;

	return (fs && !fs->dying) ? fs : NULL;
}

static inline isofs_file_t *isofs_file_of(vfs_file_t *f)
{
	isofs_file_t *ff = f ? (isofs_file_t *)f->fs_private : NULL;

	return (ff && ff->fs) ? ff : NULL;
}

static void isofs_meta_rlock(isofs_fs_t *fs)
{
	isofs_rwsem_down_read(&fs->meta);
}

static void isofs_meta_runlock(isofs_fs_t *fs)
{
	isofs_rwsem_up_read(&fs->meta);
}

static void isofs_meta_wlock(isofs_fs_t *fs)
{
	isofs_rwsem_down_write(&fs->meta);
}

static void isofs_meta_wunlock(isofs_fs_t *fs)
{
	isofs_rwsem_up_write(&fs->meta);
}

/* Report corrupt media: one rate-limited warning naming the volume and
 * what was wrong, and the status every caller hands up (ST_IO). */
#define ISOFS_CORRUPT(fs, fmt, ...)                                         \
	({                                                                  \
		WARN_RATELIMIT(1, "iso9660 %s: " fmt,                       \
			       ((fs) && (fs)->bdev && (fs)->bdev->name) ?   \
				       (fs)->bdev->name :                   \
				       "?",                                 \
			       ##__VA_ARGS__);                              \
		ST_IO;                                                      \
	})

/* ===========================================================================
 * The device: guard block device and the driver's own reads
 * ===========================================================================
 *
 * The page cache never sees the real device (isofs.h section 3): it is
 * handed `fs->guard', whose .read forwards to the real device, whose .write
 * refuses everything with a warning -- there is no write path in this
 * driver at all -- and whose .sync forwards (a cache flush writes nothing
 * of ours).
 *
 * Two translations happen in the guard (isofs.h, ISO9660 specifics):
 *
 *  - Guard sector numbers are shifted by one: guard LBA k is view sector
 *    k - 1.  The page cache reads block_to_lba() == 0 as "a hole, zeros",
 *    and an ISO file may legitimately start at logical block 0 (the system
 *    area: hybrid boot images, "[BOOT]" files of some mastering tools).
 *    With the shift no data block ever maps to guard LBA 0.
 *
 *  - A device whose sectors are larger than the logical block (an image
 *    of 2048-byte blocks on a 4096-byte-sector disk, 512-byte blocks on an
 *    optical drive) is presented as one of logical-block-sized sectors
 *    (`vss'); the guard reads the covering real sectors into a bounce
 *    buffer and copies the part asked for. */

static inline isofs_fs_t *isofs_fs_of_guard(block_device_t *g)
{
	return (isofs_fs_t *)((uint8_t *)g - __builtin_offsetof(isofs_fs_t,
								guard));
}

/* Read `count' real sectors at real LBA `lba', in chunks the device takes. */
static int isofs_real_read(isofs_fs_t *fs, uint64_t lba, uint64_t count,
			   void *buf)
{
	block_device_t *b = fs->bdev;
	uint8_t *p = (uint8_t *)buf;

	might_sleep();
	BUG_ON(!b || !b->read);
	while (count) {
		uint64_t n = count > 64 ? 64 : count;
		int r = b->read(b, (unsigned long)lba, (unsigned long)n, p);

		if (r != ST_OK)
			return r == ST_NOMEM ? ST_NOMEM : ST_IO;
		lba += n;
		p += n * fs->real_ss;
		count -= n;
	}
	return ST_OK;
}

static int isofs_guard_read(block_device_t *g, unsigned long lba,
			    unsigned long count, void *buf)
{
	isofs_fs_t *fs = isofs_fs_of_guard(g);
	uint64_t view, end;
	uint8_t *out = (uint8_t *)buf;
	uint8_t *bounce;
	unsigned long f;

	might_sleep();
	if (!count)
		return ST_OK;
	if (!buf)
		return ST_INVALID;
	/* LBA 0 is never a data block (see above); asking for it, or for
	 * anything past the device, is a mapping bug or a lie of the
	 * medium the stream checks should have caught. */
	if (lba == 0 || !isofs_add_ok(lba - 1, count, &end) ||
	    end > fs->view_sectors) {
		WARN_RATELIMIT(1, "iso9660 %s: read outside the device (%lu+%lu)",
			       fs->bdev->name ? fs->bdev->name : "?", lba,
			       count);
		return ST_IO;
	}
	view = lba - 1;
	f = fs->view_per_real;
	if (f == 1)
		return isofs_real_read(fs, view, count, buf);

	bounce = (uint8_t *)kalloc(ISOFS_GUARD_BOUNCE_SECTORS * fs->real_ss);
	if (!bounce)
		return ST_NOMEM;
	while (count) {
		uint64_t rfirst = view / f;
		uint64_t skip = view % f;
		uint64_t nview = ISOFS_GUARD_BOUNCE_SECTORS * f - skip;
		uint64_t rcount;
		int r;

		if (nview > count)
			nview = count;
		rcount = isofs_div_up(skip + nview, f);
		r = isofs_real_read(fs, rfirst, rcount, bounce);
		if (r != ST_OK) {
			kfree(bounce);
			return r;
		}
		mm_memcpy(out, bounce + skip * fs->vss, nview * fs->vss);
		out += nview * fs->vss;
		view += nview;
		count -= nview;
	}
	kfree(bounce);
	return ST_OK;
}

/* The page cache's write path.  Nothing in an ISO9660 mount is ever dirty,
 * so this can only be reached by a bug elsewhere: refuse, loudly. */
static int isofs_guard_write(block_device_t *g, unsigned long lba,
			     unsigned long count, const void *buf)
{
	isofs_fs_t *fs = isofs_fs_of_guard(g);

	(void)buf;
	WARN_RATELIMIT(1, "iso9660 %s: write refused (lba %lu count %lu)",
		       (fs->bdev && fs->bdev->name) ? fs->bdev->name : "?",
		       lba, count);
	return ST_ROFS;
}

static int isofs_guard_sync(block_device_t *g)
{
	isofs_fs_t *fs = isofs_fs_of_guard(g);

	if (fs->bdev && fs->bdev->sync)
		return fs->bdev->sync(fs->bdev);
	return ST_OK;
}

/* (Re)describe the guard for the current geometry: `vss' bytes per
 * presented sector.  Only before the instance is published. */
static void isofs_guard_setup(isofs_fs_t *fs, unsigned long vss)
{
	block_device_t *g = &fs->guard;

	fs->vss = vss;
	fs->view_per_real = fs->real_ss > vss ? fs->real_ss / vss : 1;
	fs->view_sectors = fs->bdev->total_sectors * fs->view_per_real;
	if (fs->real_ss < vss) /* never: vss = min(real, lbs) */
		fs->view_sectors = fs->bdev->total_sectors;
	mm_memset(g, 0, sizeof(*g));
	g->name = fs->bdev->name;
	g->sector_size = (unsigned int)vss;
	/* +1: guard LBA 0 is the never-used slot of the shift. */
	g->total_sectors = (unsigned long)(fs->view_sectors + 1);
	g->read = isofs_guard_read;
	g->write = isofs_guard_write;
	g->sync = isofs_guard_sync;
	g->release_locks_for_task = NULL;
	g->driver_data = fs;
}

/* The driver's direct reads (the volume descriptors, before the page cache
 * is in use): `count' guard sectors from view sector `view'. */
static int isofs_dev_read(isofs_fs_t *fs, uint64_t view, uint64_t count,
			  void *buf)
{
	might_sleep();
	return isofs_guard_read(&fs->guard, (unsigned long)(view + 1),
				(unsigned long)count, buf);
}

/* One 2048-byte volume descriptor sector (`sector' in 2048-byte units from
 * the start of the device) into `buf' (2048 bytes). */
static int isofs_read_vd_sector(isofs_fs_t *fs, uint64_t sector, uint8_t *buf)
{
	uint64_t byte, view;

	if (!isofs_mul_ok(sector, ISOFS_VD_SIZE, &byte))
		return ST_IO;
	view = byte / fs->vss;
	return isofs_dev_read(fs, view, ISOFS_VD_SIZE / fs->vss, buf);
}

/* ===========================================================================
 * Block mapping (vfs_sb_ops_t, isofs.h section 1)
 * =========================================================================== */

/* Logical block of block `b' (0-based) of an extent, interleaving applied:
 * units of `fus' blocks separated by gaps of `igs' blocks. */
static inline uint64_t isofs_ext_block(const isofs_extent_t *e, uint64_t b)
{
	if (!e->fus)
		return e->lba + b;
	return e->lba + (b / e->fus) * ((uint64_t)e->fus + e->igs) + b % e->fus;
}

/* The extent holding file block `n' of an aligned stream (binary search),
 * or NULL. */
static const isofs_extent_t *isofs_ext_by_block(const isofs_stream_t *s,
						uint64_t n)
{
	uint32_t lo = 0, hi = s->nextents;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const isofs_extent_t *e = &s->ext[mid];

		if (n < e->fblk)
			hi = mid;
		else if (n - e->fblk >= e->nblk)
			lo = mid + 1;
		else
			return e;
	}
	return NULL;
}

/* The extent holding file byte `off' (binary search), or NULL. */
static const isofs_extent_t *isofs_ext_by_byte(const isofs_extent_t *ext,
					       uint32_t n, uint64_t off)
{
	uint32_t lo = 0, hi = n;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const isofs_extent_t *e = &ext[mid];

		if (off < e->foff)
			hi = mid;
		else if (off - e->foff >= e->len)
			lo = mid + 1;
		else
			return e;
	}
	return NULL;
}

/* The stream behind a handle, for the mapping ops.  They run under
 * lock_map (the metadata lock shared), and a slot is emptied only under it
 * exclusive, so the stream stays valid for the call. */
static isofs_stream_t *isofs_stream_of_handle(isofs_fs_t *fs, unsigned long h)
{
	isofs_stream_t *s;

	if (h == ISOFS_HANDLE_VOLUME)
		return &fs->vol;
	if (h < ISOFS_HANDLE_FIRST_DYNAMIC ||
	    h - ISOFS_HANDLE_FIRST_DYNAMIC >= ISOFS_STREAM_SLOTS || !fs->slots)
		return NULL;
	s = __atomic_load_n(&fs->slots[h - ISOFS_HANDLE_FIRST_DYNAMIC],
			    __ATOMIC_ACQUIRE);
	if (!s || s == ISOFS_SLOT_RETIRING)
		return NULL;
	return s;
}

/* Logical block of block `n' of stream `s', or (uint64_t)-1. */
static uint64_t isofs_stream_block(const isofs_stream_t *s, uint64_t n)
{
	const isofs_extent_t *e;

	if (n >= s->nblocks || (s->flags & ISOFS_SF_NOFILL))
		return (uint64_t)-1;
	if (s->flags & ISOFS_SF_VOLUME)
		return n;
	e = isofs_ext_by_block(s, n);
	if (!e)
		return (uint64_t)-1;
	return isofs_ext_block(e, n - e->fblk);
}

static unsigned long isofs_sb_block_size(vfs_superblock_t *sb)
{
	return isofs_fs_of_sb(sb)->lbs;
}

static const block_device_t *isofs_sb_bdev(vfs_superblock_t *sb)
{
	return &isofs_fs_of_sb(sb)->guard;
}

static unsigned long isofs_sb_sector_size(vfs_superblock_t *sb)
{
	return isofs_fs_of_sb(sb)->vss;
}

static unsigned long isofs_sb_end_of_chain(vfs_superblock_t *sb)
{
	(void)sb;
	return ISOFS_EOC_MARKER;
}

/* block_at: the positional id of block N while N is below the stream's
 * block count; 0 past it, and 0 for every block of a NOFILL stream (the
 * driver fills those pages itself). */
static unsigned long isofs_sb_block_at(vfs_superblock_t *sb,
				       unsigned long chain,
				       unsigned long index)
{
	isofs_fs_t *fs = isofs_fs_of_sb(sb);
	isofs_stream_t *s;

	if (!fs || !ISOFS_BID_IS(chain) || ISOFS_BID_INDEX(chain) != 0 ||
	    index > ISOFS_BID_INDEX_MASK)
		return 0;
	s = isofs_stream_of_handle(fs, ISOFS_BID_HANDLE(chain));
	if (!s || (s->flags & ISOFS_SF_NOFILL) || index >= s->nblocks)
		return 0;
	return ISOFS_BID_ENC(s->handle, index);
}

static unsigned long isofs_sb_next_block(vfs_superblock_t *sb,
					 unsigned long bid)
{
	unsigned long n;

	if (!ISOFS_BID_IS(bid))
		return ISOFS_BID_EOC;
	n = isofs_sb_block_at(sb, ISOFS_BID_ENC(ISOFS_BID_HANDLE(bid), 0),
			      ISOFS_BID_INDEX(bid) + 1);
	return n ? n : ISOFS_BID_EOC;
}

/* Every block of an ISO9660 stream has a place on the medium. */
static unsigned long isofs_sb_map_block(vfs_superblock_t *sb,
					unsigned long chain,
					unsigned long index)
{
	return isofs_sb_block_at(sb, chain, index);
}

static unsigned long isofs_sb_block_to_lba(vfs_superblock_t *sb,
					   unsigned long bid)
{
	isofs_fs_t *fs = isofs_fs_of_sb(sb);
	isofs_stream_t *s;
	uint64_t lb;

	if (!fs || !ISOFS_BID_IS(bid))
		return 0;
	s = isofs_stream_of_handle(fs, ISOFS_BID_HANDLE(bid));
	if (!s)
		return 0;
	lb = isofs_stream_block(s, ISOFS_BID_INDEX(bid));
	/* Checked against the device when the stream was built; checked
	 * again because a wrong answer here reads someone else's data. */
	if (lb == (uint64_t)-1 || lb >= fs->dev_blocks) {
		WARN_ON_ONCE(lb != (uint64_t)-1);
		return 0;
	}
	return (unsigned long)(1 + lb * fs->spb);
}

/* Nothing is ever dirty on a read-only filesystem. */
static int isofs_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *ino)
{
	(void)sb;
	(void)ino;
	WARN_RATELIMIT(1, "iso9660: write_inode on a read-only filesystem");
	return ST_ROFS;
}

static void isofs_sb_lock_io(vfs_superblock_t *sb)
{
	isofs_meta_wlock(isofs_fs_of_sb(sb));
}

static void isofs_sb_unlock_io(vfs_superblock_t *sb)
{
	isofs_meta_wunlock(isofs_fs_of_sb(sb));
}

static void isofs_sb_lock_map(vfs_superblock_t *sb)
{
	isofs_meta_rlock(isofs_fs_of_sb(sb));
}

static void isofs_sb_unlock_map(vfs_superblock_t *sb)
{
	isofs_meta_runlock(isofs_fs_of_sb(sb));
}

static unsigned long isofs_sb_reserved_meta_block(vfs_superblock_t *sb)
{
	(void)sb;
	return 0;
}

static const vfs_sb_ops_t isofs_sb_ops = {
	.block_size = isofs_sb_block_size,
	.bdev = isofs_sb_bdev,
	.sector_size = isofs_sb_sector_size,
	.block_to_lba = isofs_sb_block_to_lba,
	.end_of_chain_marker = isofs_sb_end_of_chain,
	.next_block = isofs_sb_next_block,
	.map_block = isofs_sb_map_block,
	.block_at = isofs_sb_block_at,
	.write_inode = isofs_sb_write_inode,
	.lock_io = isofs_sb_lock_io,
	.unlock_io = isofs_sb_unlock_io,
	.lock_map = isofs_sb_lock_map,
	.unlock_map = isofs_sb_unlock_map,
	.reserved_meta_block = isofs_sb_reserved_meta_block,
	.alloc_blocks = NULL, /* read-only: nothing is ever placed */
};

/* ===========================================================================
 * Reading through the page cache
 * =========================================================================== */

/* `len' bytes at byte `off' of the volume, through the volume stream
 * (reserved handle ISOFS_HANDLE_VOLUME, block N = logical block N).  Used
 * for the System Use continuation areas, directory "." records, zisofs
 * headers and pointer tables and compressed data, and the bytes of
 * streams that are not block-aligned. */
static int isofs_vol_read(isofs_fs_t *fs, uint64_t off, void *buf,
			  uint64_t len)
{
	uint8_t *out = (uint8_t *)buf;
	uint64_t end;

	might_sleep();
	if (!isofs_add_ok(off, len, &end) || end > fs->vol.size)
		return ISOFS_CORRUPT(fs, "read past the end of the device "
					 "(offset %lu, %lu bytes)",
				     (unsigned long)off, (unsigned long)len);
	while (len) {
		unsigned long pi = (unsigned long)(off / PAGE_SIZE);
		unsigned po = (unsigned)(off % PAGE_SIZE);
		uint64_t chunk = PAGE_SIZE - po;
		pc_page_t *pg;

		if (chunk > len)
			chunk = len;
		pg = pagecache_get_held(fs->vol.chain, pi,
					(unsigned long)fs->vol.size, &fs->sb,
					fs->vol.chain);
		if (!pg || !pg->data) {
			pagecache_put(pg);
			return ST_IO;
		}
		mm_memcpy(out, pg->data + po, chunk);
		pagecache_put(pg);
		out += chunk;
		off += chunk;
		len -= chunk;
	}
	return ST_OK;
}

/* `len' bytes at byte `off' of the RECORDED data of a file described by
 * `ext' / `n' (multi-extent and interleaving applied), through the volume
 * stream.  The range must lie inside the recorded data. */
static int isofs_raw_read(isofs_fs_t *fs, const isofs_extent_t *ext,
			  uint32_t n, uint64_t off, void *buf, uint64_t len)
{
	uint8_t *out = (uint8_t *)buf;

	might_sleep();
	while (len) {
		const isofs_extent_t *e = isofs_ext_by_byte(ext, n, off);
		uint64_t local, chunk, voff;
		int r;

		if (!e)
			return ISOFS_CORRUPT(fs, "read outside a file's data");
		local = off - e->foff;
		chunk = e->len - local;
		if (e->fus) {
			/* Interleaved: one block at a time. */
			uint64_t boff = local % fs->lbs;

			if (chunk > fs->lbs - boff)
				chunk = fs->lbs - boff;
			voff = isofs_ext_block(e, local / fs->lbs) * fs->lbs +
			       boff;
		} else {
			voff = e->lba * fs->lbs + local;
		}
		if (chunk > len)
			chunk = len;
		r = isofs_vol_read(fs, voff, out, chunk);
		if (r != ST_OK)
			return r;
		out += chunk;
		off += chunk;
		len -= chunk;
	}
	return ST_OK;
}

/* ===========================================================================
 * Stream table (isofs.h section 1; locking in section 4)
 * ===========================================================================
 *
 * Streams are found by key (the object's st_ino) in a hash table and by
 * handle in the slot array.  Both, the use counts and the LRU of
 * unused streams are guarded by st_lock (a spinlock: no allocation, no
 * I/O, no freeing under it).  A stream in use (refs > 0) is never taken
 * away; one nobody uses waits on the LRU for a reopen and is evicted --
 * under the metadata lock exclusive, so no mapping call can be looking at
 * it -- when more than ISOFS_STREAM_KEEP of them have piled up.  Eviction
 * forgets the stream's pages and inode-cache entry BEFORE its handle may
 * be handed out again (the slot holds ISOFS_SLOT_RETIRING meanwhile). */

static inline unsigned isofs_st_bucket(unsigned long key)
{
	key ^= key >> 29;
	key *= 0x9E3779B97F4A7C15UL;
	return (unsigned)(key >> 40) & (ISOFS_STREAM_BUCKETS - 1);
}

static void isofs_lru_unlink(isofs_stream_t *s)
{
	s->lru_prev->lru_next = s->lru_next;
	s->lru_next->lru_prev = s->lru_prev;
	s->lru_prev = s->lru_next = NULL;
}

static void isofs_lru_push(isofs_fs_t *fs, isofs_stream_t *s)
{
	s->lru_next = fs->lru.lru_next;
	s->lru_prev = &fs->lru;
	fs->lru.lru_next->lru_prev = s;
	fs->lru.lru_next = s;
}

static isofs_stream_t *isofs_hash_find(isofs_fs_t *fs, unsigned long key)
{
	isofs_stream_t *s = fs->st_hash[isofs_st_bucket(key)];

	while (s && s->key != key)
		s = s->hash_next;
	return s;
}

static void isofs_hash_remove(isofs_fs_t *fs, isofs_stream_t *s)
{
	isofs_stream_t **pp = &fs->st_hash[isofs_st_bucket(s->key)];

	while (*pp && *pp != s)
		pp = &(*pp)->hash_next;
	if (WARN_ON(!*pp))
		return;
	*pp = s->hash_next;
	s->hash_next = NULL;
}

/* Take a reference; caller holds st_lock. */
static void isofs_stream_ref_locked(isofs_fs_t *fs, isofs_stream_t *s)
{
	if (s->refs == 0 && !(s->flags & ISOFS_SF_VOLUME)) {
		isofs_lru_unlink(s);
		BUG_ON(fs->nunused == 0);
		fs->nunused--;
		BUG_ON(fs->unused_ext_bytes < s->ext_bytes);
		fs->unused_ext_bytes -= s->ext_bytes;
	}
	s->refs++;
}

static void isofs_stream_free(isofs_stream_t *s)
{
	if (!s)
		return;
	if (s->ext && s->ext != &s->ext1)
		kfree(s->ext);
	kfree(s);
}

/* A published stream by key, referenced; NULL when there is none. */
static isofs_stream_t *isofs_stream_get(isofs_fs_t *fs, unsigned long key)
{
	isofs_stream_t *s;
	uint64_t flags;

	spin_lock_irqsave(&fs->st_lock, &flags);
	s = isofs_hash_find(fs, key);
	if (s)
		isofs_stream_ref_locked(fs, s);
	spin_unlock_irqrestore(&fs->st_lock, flags);
	return s;
}

static void isofs_stream_put(isofs_fs_t *fs, isofs_stream_t *s)
{
	uint64_t flags;

	if (!s || (s->flags & ISOFS_SF_VOLUME))
		return;
	spin_lock_irqsave(&fs->st_lock, &flags);
	BUG_ON(s->refs <= 0);
	if (--s->refs == 0) {
		isofs_lru_push(fs, s);
		fs->nunused++;
		fs->unused_ext_bytes += s->ext_bytes;
	}
	spin_unlock_irqrestore(&fs->st_lock, flags);
}

/* Are more unused streams kept than `keep' (count) allows, or do they hold
 * more extent-array bytes than ISOFS_STREAM_KEEP_EXT_BYTES?  1: too many,
 * 2: too many bytes (only), 0: neither.  st_lock held or not (a hint). */
static int isofs_trim_needed(isofs_fs_t *fs, unsigned long keep)
{
	if (__atomic_load_n(&fs->nunused, __ATOMIC_RELAXED) > keep)
		return 1;
	if (__atomic_load_n(&fs->unused_ext_bytes, __ATOMIC_RELAXED) >
	    (keep ? ISOFS_STREAM_KEEP_EXT_BYTES : 0))
		return 2;
	return 0;
}

/* Evict unused streams down to `keep' of them and the extent-byte budget.
 * `may_block': the caller holds no filesystem lock and may wait for the
 * metadata lock; otherwise only an uncontended exclusive hold is taken (a
 * nested caller holding it shared would deadlock waiting) and the trim is
 * left for later when that fails. */
static void isofs_stream_trim(isofs_fs_t *fs, unsigned long keep, int may_block)
{
	task_t *cur = sched_current();

	if (!isofs_trim_needed(fs, keep))
		return;
	if (may_block && !(cur && cur->fs_held) &&
	    !isofs_rwsem_held_excl(&fs->meta))
		isofs_meta_wlock(fs);
	else if (!isofs_rwsem_try_write(&fs->meta))
		return;
	for (;;) {
		isofs_stream_t *v;
		unsigned long idx;
		uint64_t flags;

		int why;

		spin_lock_irqsave(&fs->st_lock, &flags);
		why = isofs_trim_needed(fs, keep);
		if (!why || fs->lru.lru_prev == &fs->lru) {
			spin_unlock_irqrestore(&fs->st_lock, flags);
			break;
		}
		v = fs->lru.lru_prev; /* the least recently used */
		if (why == 2)
			/* Over the byte budget only: the oldest one that
			 * holds an array (there is one: the bytes say so). */
			while (v != &fs->lru && !v->ext_bytes)
				v = v->lru_prev;
		if (v == &fs->lru) {
			WARN_ON_ONCE(1);
			spin_unlock_irqrestore(&fs->st_lock, flags);
			break;
		}
		BUG_ON(v->refs != 0);
		isofs_lru_unlink(v);
		isofs_hash_remove(fs, v);
		idx = v->handle - ISOFS_HANDLE_FIRST_DYNAMIC;
		BUG_ON(idx >= ISOFS_STREAM_SLOTS || fs->slots[idx] != v);
		__atomic_store_n(&fs->slots[idx], ISOFS_SLOT_RETIRING,
				 __ATOMIC_RELEASE);
		fs->nunused--;
		BUG_ON(fs->unused_ext_bytes < v->ext_bytes);
		fs->unused_ext_bytes -= v->ext_bytes;
		fs->nstreams--;
		spin_unlock_irqrestore(&fs->st_lock, flags);

		/* No lock of ours but the metadata one: the caches take
		 * their own spinlocks. */
		pagecache_invalidate_file(&fs->sb, v->chain);
		icache_remove(&fs->sb, v->chain);

		spin_lock_irqsave(&fs->st_lock, &flags);
		__atomic_store_n(&fs->slots[idx], NULL, __ATOMIC_RELEASE);
		spin_unlock_irqrestore(&fs->st_lock, flags);
		isofs_stream_free(v);
	}
	isofs_meta_wunlock(fs);
}

/* Publish a stream built by the caller (refs 0, no handle yet) and return
 * it referenced.  When another task published one with the same key
 * meanwhile, that one is returned and `ns' freed.  NULL (and `ns' freed)
 * when every slot is taken by a stream in use. */
static isofs_stream_t *isofs_stream_publish(isofs_fs_t *fs, isofs_stream_t *ns)
{
	isofs_stream_t *s;
	uint64_t flags;
	int attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		spin_lock_irqsave(&fs->st_lock, &flags);
		s = isofs_hash_find(fs, ns->key);
		if (s) {
			isofs_stream_ref_locked(fs, s);
			spin_unlock_irqrestore(&fs->st_lock, flags);
			isofs_stream_free(ns);
			return s;
		}
		for (unsigned long i = 0; i < ISOFS_STREAM_SLOTS; i++) {
			unsigned long idx =
				(fs->slot_cursor + i) % ISOFS_STREAM_SLOTS;

			if (fs->slots[idx])
				continue;
			ns->handle = ISOFS_HANDLE_FIRST_DYNAMIC + idx;
			ns->chain = ISOFS_BID_ENC(ns->handle, 0);
			ns->refs = 1;
			ns->hash_next = fs->st_hash[isofs_st_bucket(ns->key)];
			fs->st_hash[isofs_st_bucket(ns->key)] = ns;
			__atomic_store_n(&fs->slots[idx], ns, __ATOMIC_RELEASE);
			fs->slot_cursor = (idx + 1) % ISOFS_STREAM_SLOTS;
			fs->nstreams++;
			spin_unlock_irqrestore(&fs->st_lock, flags);
			return ns;
		}
		spin_unlock_irqrestore(&fs->st_lock, flags);
		/* Full: make room from the unused ones and try once more. */
		isofs_stream_trim(fs, 0, 1);
	}
	WARN_RATELIMIT(1, "iso9660: stream table full (%u streams in use)",
		       ISOFS_STREAM_SLOTS);
	isofs_stream_free(ns);
	return NULL;
}

/* Fill in the derived fields of a stream from its extents: file offsets,
 * block offsets, block counts, the aligned / NOFILL decision, and check
 * that every block lies on the device.  ST_OK or ST_IO (corrupt). */
static int isofs_stream_layout(isofs_fs_t *fs, isofs_stream_t *s)
{
	uint64_t foff = 0, fblk = 0;

	for (uint32_t i = 0; i < s->nextents; i++) {
		isofs_extent_t *e = &s->ext[i];
		uint64_t last;

		e->foff = foff;
		e->fblk = fblk;
		e->nblk = isofs_div_up(e->len, fs->lbs);
		if (!isofs_add_ok(foff, e->len, &foff) ||
		    !isofs_add_ok(fblk, e->nblk, &fblk))
			return ISOFS_CORRUPT(fs, "file size overflows");
		if (e->len % fs->lbs && i + 1 < s->nextents)
			s->flags |= ISOFS_SF_UNALIGNED;
		if (!e->nblk)
			continue;
		/* The last block of an extent is its highest (interleaving
		 * only moves later blocks further out). */
		last = isofs_ext_block(e, e->nblk - 1);
		if (last < e->lba || last >= fs->dev_blocks)
			return ISOFS_CORRUPT(fs,
					     "file data beyond the device "
					     "(block %lu of %lu)",
					     (unsigned long)last,
					     (unsigned long)fs->dev_blocks);
	}
	s->raw_size = foff;
	if (!(s->flags & (ISOFS_SF_ZISOFS | ISOFS_SF_SPARSE)))
		s->size = foff;
	s->nblocks = isofs_div_up(s->size, fs->lbs);
	if (s->nblocks > ISOFS_BID_INDEX_MASK)
		return ISOFS_CORRUPT(fs, "file too large");
	return ST_OK;
}

/* ===========================================================================
 * System Use Sharing Protocol and Rock Ridge
 * ===========================================================================
 *
 * The System Use area of a directory record (after the identifier and its
 * pad byte, and after the SP "skip" bytes) holds entries of the form
 *
 *     sig[2] len[1] version[1] data[len - 4]
 *
 * with CE pointing to one more area (a block, an offset into it and a
 * length) where the list continues, and ST ending it.  Everything about
 * the walk is bounded: an entry's length must fit in what is left of its
 * area, an area may continue at most ISOFS_MAX_CE_HOPS times, and at most
 * ISOFS_MAX_SU_BYTES are examined -- a CE chain that loops ends at the
 * hop limit.  A malformed entry ends the walk over its area with what was
 * gathered so far (the record stays usable, minus what was lost), marked
 * as damaged. */

#define SU_SEEN_SP 0x00000001u
#define SU_SEEN_CE 0x00000002u
#define SU_SEEN_ER_RRIP 0x00000004u
#define SU_SEEN_RR 0x00000008u
#define SU_SEEN_PX 0x00000010u
#define SU_SEEN_PN 0x00000020u
#define SU_SEEN_SL 0x00000040u
#define SU_SEEN_NM 0x00000080u
#define SU_SEEN_CL 0x00000100u
#define SU_SEEN_PL 0x00000200u
#define SU_SEEN_RE 0x00000400u
#define SU_SEEN_TF 0x00000800u
#define SU_SEEN_SF 0x00001000u
#define SU_SEEN_ZF 0x00002000u
#define SU_SEEN_ST 0x00004000u
#define SU_SEEN_DAMAGED 0x80000000u
/* Entries that make a record a Rock Ridge one. */
#define SU_SEEN_ROCK                                                     \
	(SU_SEEN_RR | SU_SEEN_PX | SU_SEEN_PN | SU_SEEN_SL | SU_SEEN_NM | \
	 SU_SEEN_CL | SU_SEEN_PL | SU_SEEN_RE | SU_SEEN_TF | SU_SEEN_SF | \
	 SU_SEEN_ZF)

/* What a walk over one record's System Use data found.  On the heap
 * (it holds a continuation-area buffer and a symlink target). */
typedef struct isofs_su {
	uint32_t seen;
	int want_sl; /* collect the SL target            */
	/* SP */
	unsigned sp_skip;
	/* PX */
	uint32_t px_mode, px_nlink, px_uid, px_gid;
	uint64_t px_serial;
	int px_long;
	/* PN */
	uint32_t pn_high, pn_low;
	/* TF */
	unsigned tf_have; /* RR_TF_* bits found               */
	int64_t tf_mtime, tf_atime, tf_ctime, tf_crtime;
	uint32_t tf_mtime_ns, tf_atime_ns, tf_ctime_ns, tf_crtime_ns;
	/* CL / PL */
	uint64_t cl, pl;
	/* SF */
	uint64_t sf_size;
	unsigned sf_depth;
	/* ZF */
	uint8_t zf_alg[2];
	unsigned zf_hdiv4, zf_blog;
	uint64_t zf_usize;
	/* NM */
	unsigned nm_len;
	int nm_done, nm_bad, nm_special;
	char nm[ISOFS_NAME_MAX + 1];
	/* SL */
	unsigned sl_len;
	int sl_done, sl_bad, sl_open, sl_sep;
	char sl[ISOFS_SYMLINK_MAX + 1];
	/* bookkeeping */
	unsigned total;
	/* one continuation area */
	uint8_t ce[ISOFS_LBS_MAX];
	/* ---- per directory scan (not reset per record; see
	 * isofs_su_scan_begin) ---- */
	uint64_t scan_bytes; /* System Use bytes walked in this scan    */
	uint64_t scan_hops; /* continuation areas read in this scan    */
	uint64_t scan_records; /* records parsed in this scan          */
	int over_budget; /* the scan exceeded its work budget       */
} isofs_su_t;

/* Work one directory scan (a lookup, a readdir call, a single record) may
 * spend on System Use data: a hostile directory of 64 MB whose every record
 * follows ISOFS_MAX_CE_HOPS continuation areas would otherwise take minutes
 * under the metadata lock.  The budget is a base plus an allowance per
 * record scanned, so it grows with the directory but not with what each
 * record makes the driver read: real directories use far less than two
 * continuation areas and 1 KB of System Use data per record on average
 * (most none at all).  Beyond it the scan fails with EIO and a warning. */
#define ISOFS_SCAN_BASE_HOPS 262144UL
#define ISOFS_SCAN_HOPS_PER_RECORD 2UL
#define ISOFS_SCAN_BASE_BYTES (64UL << 20)
#define ISOFS_SCAN_BYTES_PER_RECORD 1024UL

static void isofs_su_scan_begin(isofs_su_t *su)
{
	su->scan_bytes = 0;
	su->scan_hops = 0;
	su->scan_records = 0;
	su->over_budget = 0;
}

static int isofs_su_over_hops(const isofs_su_t *su)
{
	return su->scan_hops > ISOFS_SCAN_BASE_HOPS +
				       su->scan_records *
					       ISOFS_SCAN_HOPS_PER_RECORD;
}

static int isofs_su_over_bytes(const isofs_su_t *su)
{
	return su->scan_bytes > ISOFS_SCAN_BASE_BYTES +
					su->scan_records *
						ISOFS_SCAN_BYTES_PER_RECORD;
}

static void isofs_su_reset(isofs_su_t *su, int want_sl)
{
	/* Everything up to (not including) the continuation buffer. */
	mm_memset(su, 0, __builtin_offsetof(isofs_su_t, ce));
	su->want_sl = want_sl;
}

/* NM: a name, possibly in several pieces (NM_CONTINUE). */
static void isofs_su_nm(isofs_su_t *su, const uint8_t *e, unsigned len)
{
	unsigned flags, dlen;

	if (len < 5)
		return;
	su->seen |= SU_SEEN_NM;
	if (su->nm_done || su->nm_bad)
		return;
	flags = e[4];
	dlen = len - 5;
	if (flags & (RR_NM_CURRENT | RR_NM_PARENT)) {
		/* "." or "..": only meaningful in those records. */
		su->nm_special = 1;
		su->nm_done = 1;
		return;
	}
	if (su->nm_len + dlen > ISOFS_NAME_MAX) {
		su->nm_bad = 1;
		return;
	}
	for (unsigned i = 0; i < dlen; i++) {
		char c = (char)e[5 + i];

		/* A '/' or a NUL would make one name several, or none. */
		if (c == '/' || c == '\0') {
			su->nm_bad = 1;
			return;
		}
		su->nm[su->nm_len++] = c;
	}
	su->nm[su->nm_len] = '\0';
	if (!(flags & RR_NM_CONTINUE))
		su->nm_done = 1;
}

/* Append to the symlink target being assembled (bounded). */
static void isofs_sl_append(isofs_su_t *su, const char *s, unsigned n)
{
	if (su->sl_bad)
		return;
	if (su->sl_len + n > ISOFS_SYMLINK_MAX) {
		su->sl_bad = 1;
		return;
	}
	mm_memcpy(su->sl + su->sl_len, s, n);
	su->sl_len += n;
	su->sl[su->sl_len] = '\0';
}

/* SL: a symbolic link target as components, possibly spread over several
 * SL entries (entry flag bit 0), a component possibly split over two
 * (component flag CONTINUE). */
static void isofs_su_sl(isofs_su_t *su, const uint8_t *e, unsigned len)
{
	unsigned pos = 5;

	if (len < 5)
		return;
	su->seen |= SU_SEEN_SL;
	if (!su->want_sl || su->sl_done || su->sl_bad)
		return;
	while (pos < len) {
		unsigned cflags, clen;

		if (pos + 2 > len) {
			su->sl_bad = 1;
			return;
		}
		cflags = e[pos];
		clen = e[pos + 1];
		if (pos + 2 + clen > len) {
			su->sl_bad = 1;
			return;
		}
		if (cflags & RR_SLC_ROOT) {
			/* "/" -- the start of an absolute target. */
			isofs_sl_append(su, "/", 1);
			su->sl_sep = 0;
			su->sl_open = 0;
		} else if (cflags & (RR_SLC_CURRENT | RR_SLC_PARENT)) {
			if (su->sl_sep)
				isofs_sl_append(su, "/", 1);
			if (cflags & RR_SLC_PARENT)
				isofs_sl_append(su, "..", 2);
			else
				isofs_sl_append(su, ".", 1);
			su->sl_sep = 1;
			su->sl_open = 0;
		} else if (cflags & (RR_SLC_VOLROOT | RR_SLC_HOST)) {
			/* Obsolete components with no meaning here: the
			 * target cannot be represented. */
			su->sl_bad = 1;
			return;
		} else {
			if (!su->sl_open && su->sl_sep)
				isofs_sl_append(su, "/", 1);
			for (unsigned i = 0; i < clen; i++) {
				char c = (char)e[pos + 2 + i];

				if (c == '/' || c == '\0') {
					su->sl_bad = 1;
					return;
				}
			}
			isofs_sl_append(su, (const char *)e + pos + 2, clen);
			su->sl_sep = 1;
			su->sl_open = (cflags & RR_SLC_CONTINUE) != 0;
		}
		pos += 2 + clen;
	}
	if (!(e[4] & 1))
		su->sl_done = 1;
}

/* TF: time stamps, in flag order, short (7-byte) or long (17-byte) form. */
static void isofs_su_tf(isofs_su_t *su, const uint8_t *e, unsigned len)
{
	static const unsigned order[7] = { RR_TF_CREATE, RR_TF_MODIFY,
					   RR_TF_ACCESS, RR_TF_ATTR,
					   RR_TF_BACKUP, RR_TF_EXPIRE,
					   RR_TF_EFFECT };
	unsigned flags, sz, pos = 5;

	if (len < 5)
		return;
	su->seen |= SU_SEEN_TF;
	flags = e[4];
	sz = (flags & RR_TF_LONG) ? 17 : 7;
	for (unsigned i = 0; i < 7; i++) {
		int64_t t;
		uint32_t ns = 0;

		if (!(flags & order[i]))
			continue;
		if (pos + sz > len)
			return; /* truncated: keep what was complete */
		t = sz == 17 ? isofs_date17(e + pos, &ns) : isofs_date7(e + pos);
		switch (order[i]) {
		case RR_TF_CREATE:
			su->tf_crtime = t;
			su->tf_crtime_ns = ns;
			break;
		case RR_TF_MODIFY:
			su->tf_mtime = t;
			su->tf_mtime_ns = ns;
			break;
		case RR_TF_ACCESS:
			su->tf_atime = t;
			su->tf_atime_ns = ns;
			break;
		case RR_TF_ATTR:
			su->tf_ctime = t;
			su->tf_ctime_ns = ns;
			break;
		default:
			break;
		}
		su->tf_have |= order[i];
		pos += sz;
	}
}

/* ER: is this the Rock Ridge extension? */
static void isofs_su_er(isofs_su_t *su, const uint8_t *e, unsigned len)
{
	static const char *const ids[] = { "RRIP_1991A", "IEEE_P1282",
					   "IEEE_1282" };
	unsigned idlen;

	if (len < 8)
		return;
	idlen = e[4];
	if (8 + idlen + (unsigned)e[5] + (unsigned)e[6] > len)
		return;
	for (unsigned i = 0; i < 3; i++) {
		unsigned n = isofs_strlen(ids[i]);

		if (idlen == n && isofs_memeq(e + 8, ids[i], n))
			su->seen |= SU_SEEN_ER_RRIP;
	}
}

/* One area's entries.  Returns 1 when a CE was found (*ce_* set), 0 when
 * the list ends here (ST, or the area ends). */
static int isofs_su_area(isofs_fs_t *fs, isofs_su_t *su, const uint8_t *p,
			 unsigned left, uint64_t *ce_lba, uint64_t *ce_off,
			 uint64_t *ce_len)
{
	int have_ce = 0;

	(void)fs;
	while (left >= 4) {
		unsigned sig = SUSP_SIG(p[0], p[1]);
		unsigned len = p[2];

		/* A zero-filled tail is padding, not an entry. */
		if (p[0] == 0 && p[1] == 0)
			break;
		if (len < 4 || len > left) {
			su->seen |= SU_SEEN_DAMAGED;
			break;
		}
		if (su->total + len > ISOFS_MAX_SU_BYTES) {
			su->seen |= SU_SEEN_DAMAGED;
			return 0;
		}
		su->total += len;
		su->scan_bytes += len;
		if (isofs_su_over_bytes(su)) {
			su->seen |= SU_SEEN_DAMAGED;
			su->over_budget = 1;
			return 0;
		}
		switch (sig) {
		case SUSP_SIG('S', 'T'):
			su->seen |= SU_SEEN_ST;
			return have_ce;
		case SUSP_SIG('C', 'E'):
			if (len >= 28) {
				*ce_lba = isofs_733(p + 4);
				*ce_off = isofs_733(p + 12);
				*ce_len = isofs_733(p + 20);
				have_ce = 1;
				su->seen |= SU_SEEN_CE;
			}
			break;
		case SUSP_SIG('S', 'P'):
			if (len >= 7 && p[4] == 0xBE && p[5] == 0xEF) {
				su->seen |= SU_SEEN_SP;
				su->sp_skip = p[6];
			}
			break;
		case SUSP_SIG('E', 'R'):
			isofs_su_er(su, p, len);
			break;
		case SUSP_SIG('R', 'R'):
			su->seen |= SU_SEEN_RR;
			break;
		case SUSP_SIG('P', 'X'):
			if (len >= 36) {
				su->seen |= SU_SEEN_PX;
				su->px_mode = isofs_733(p + 4);
				su->px_nlink = isofs_733(p + 12);
				su->px_uid = isofs_733(p + 20);
				su->px_gid = isofs_733(p + 28);
				if (len >= 44) {
					su->px_serial = isofs_733(p + 36);
					su->px_long = 1;
				}
			}
			break;
		case SUSP_SIG('P', 'N'):
			if (len >= 20) {
				su->seen |= SU_SEEN_PN;
				su->pn_high = isofs_733(p + 4);
				su->pn_low = isofs_733(p + 12);
			}
			break;
		case SUSP_SIG('S', 'L'):
			isofs_su_sl(su, p, len);
			break;
		case SUSP_SIG('N', 'M'):
			isofs_su_nm(su, p, len);
			break;
		case SUSP_SIG('C', 'L'):
			if (len >= 12) {
				su->seen |= SU_SEEN_CL;
				su->cl = isofs_733(p + 4);
			}
			break;
		case SUSP_SIG('P', 'L'):
			if (len >= 12) {
				su->seen |= SU_SEEN_PL;
				su->pl = isofs_733(p + 4);
			}
			break;
		case SUSP_SIG('R', 'E'):
			su->seen |= SU_SEEN_RE;
			break;
		case SUSP_SIG('T', 'F'):
			isofs_su_tf(su, p, len);
			break;
		case SUSP_SIG('S', 'F'):
			if (len >= 21) {
				su->seen |= SU_SEEN_SF;
				su->sf_size = ((uint64_t)isofs_733(p + 4) << 32) |
					      isofs_733(p + 12);
				su->sf_depth = p[20];
			}
			break;
		case SUSP_SIG('Z', 'F'):
			if (len >= 16) {
				su->seen |= SU_SEEN_ZF;
				su->zf_alg[0] = p[4];
				su->zf_alg[1] = p[5];
				su->zf_hdiv4 = p[6];
				su->zf_blog = p[7];
				su->zf_usize = isofs_733(p + 8);
			}
			break;
		default:
			/* PD padding, ES, AA/AB (other systems), and anything
			 * not known: skipped. */
			break;
		}
		p += len;
		left -= len;
	}
	return have_ce;
}

/* Walk the System Use data of a record (`area', `len' bytes, the SP skip
 * already applied by the caller) and its continuation areas. */
static void isofs_su_walk(isofs_fs_t *fs, isofs_su_t *su, const uint8_t *area,
			  unsigned len)
{
	uint64_t ce_lba = 0, ce_off = 0, ce_len = 0;
	unsigned hops = 0;
	const uint8_t *p = area;
	unsigned left = len;

	might_sleep();
	for (;;) {
		uint64_t voff;

		if (!isofs_su_area(fs, su, p, left, &ce_lba, &ce_off, &ce_len))
			return;
		if (++hops > ISOFS_MAX_CE_HOPS) {
			su->seen |= SU_SEEN_DAMAGED;
			ISOFS_CORRUPT(fs, "SUSP continuation chain too long");
			return;
		}
		su->scan_hops++;
		if (isofs_su_over_hops(su)) {
			su->seen |= SU_SEEN_DAMAGED;
			su->over_budget = 1;
			return;
		}
		/* A continuation area lies inside one logical block. */
		if (ce_len == 0 || ce_off >= fs->lbs ||
		    ce_len > fs->lbs - ce_off || ce_lba >= fs->dev_blocks) {
			su->seen |= SU_SEEN_DAMAGED;
			ISOFS_CORRUPT(fs, "bad SUSP continuation area "
					  "(block %lu offset %lu length %lu)",
				      (unsigned long)ce_lba,
				      (unsigned long)ce_off,
				      (unsigned long)ce_len);
			return;
		}
		voff = ce_lba * fs->lbs + ce_off;
		if (isofs_vol_read(fs, voff, su->ce, ce_len) != ST_OK) {
			/* Unreadable: what the record says is incomplete. */
			su->seen |= SU_SEEN_DAMAGED;
			return;
		}
		p = su->ce;
		left = (unsigned)ce_len;
	}
}

/* The System Use area of directory record `rec': where it starts and how
 * long it is, the SP skip applied unless `root_dot' (the first record of
 * the root directory, which holds the SP entry itself).  0 when there is
 * none. */
static unsigned isofs_su_area_of(const isofs_fs_t *fs, const uint8_t *rec,
				 int root_dot, const uint8_t **area)
{
	unsigned dr = rec[ISO_DR_LEN], fi = rec[ISO_DR_NAMELEN];
	unsigned off = ISO_DR_NAME + fi + ((fi & 1) ? 0 : 1);

	if (!root_dot)
		off += fs->su_skip;
	if (off >= dr)
		return 0;
	*area = rec + off;
	return dr - off;
}

/* ===========================================================================
 * Names
 * =========================================================================== */

/* Append the UTF-8 form of code point `c' to `out' (cap bytes, NUL kept
 * free), or return 0 when it does not fit. */
static int isofs_utf8_put(char *out, unsigned *n, unsigned cap, uint32_t c)
{
	uint8_t b[4];
	unsigned k;

	if (c < 0x80) {
		b[0] = (uint8_t)c;
		k = 1;
	} else if (c < 0x800) {
		b[0] = (uint8_t)(0xC0 | (c >> 6));
		b[1] = (uint8_t)(0x80 | (c & 0x3F));
		k = 2;
	} else if (c < 0x10000) {
		b[0] = (uint8_t)(0xE0 | (c >> 12));
		b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
		b[2] = (uint8_t)(0x80 | (c & 0x3F));
		k = 3;
	} else {
		b[0] = (uint8_t)(0xF0 | (c >> 18));
		b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
		b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
		b[3] = (uint8_t)(0x80 | (c & 0x3F));
		k = 4;
	}
	if (*n + k > cap)
		return 0;
	for (unsigned i = 0; i < k; i++)
		out[(*n)++] = (char)b[i];
	return 1;
}

/* Drop a ";<digits>" version suffix and then one trailing '.' (an empty
 * extension) from a name of `n' bytes; returns the new length. */
static unsigned isofs_strip_version(char *s, unsigned n, int drop_dot)
{
	for (unsigned i = n; i > 0; i--) {
		char c = s[i - 1];

		if (c == ';') {
			n = i - 1;
			break;
		}
		if (c < '0' || c > '9')
			break;
	}
	if (drop_dot && n > 1 && s[n - 1] == '.')
		n--;
	s[n] = '\0';
	return n;
}

/* A Joliet identifier (UCS-2 / UTF-16 big-endian) as UTF-8, at most
 * ISOFS_NAME_MAX bytes (cut at a character boundary).  Unpaired
 * surrogates become U+FFFD; '/' and NUL become '_'. */
static unsigned isofs_joliet_name(const uint8_t *id, unsigned len, char *out)
{
	unsigned units = len / 2, n = 0;

	for (unsigned i = 0; i < units; i++) {
		uint32_t c = isofs_be16(id + 2 * i);

		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {
			uint32_t lo = isofs_be16(id + 2 * i + 2);

			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				c = 0x10000 + ((c - 0xD800) << 10) +
				    (lo - 0xDC00);
				i++;
			} else {
				c = 0xFFFD;
			}
		} else if (c >= 0xD800 && c <= 0xDFFF) {
			c = 0xFFFD;
		}
		if (c == 0 || c == '/')
			c = '_';
		if (!isofs_utf8_put(out, &n, ISOFS_NAME_MAX, c))
			break;
	}
	out[n] = '\0';
	return isofs_strip_version(out, n, 1);
}

/* A plain ISO 9660 identifier.  map=normal: the version suffix and an
 * empty extension dropped, lower case; map=off: as recorded.  Bytes that
 * cannot be in a name ('/', NUL) become '_'. */
static unsigned isofs_plain_name(const isofs_fs_t *fs, const uint8_t *id,
				 unsigned len, int is_dir, char *out)
{
	unsigned n = 0;

	if (len > ISOFS_NAME_MAX)
		len = ISOFS_NAME_MAX;
	for (unsigned i = 0; i < len; i++) {
		char c = (char)id[i];

		if (c == '/' || c == '\0')
			c = '_';
		if (!fs->o.map_off)
			c = isofs_lower(c);
		out[n++] = c;
	}
	out[n] = '\0';
	if (fs->o.map_off)
		return n;
	if (is_dir)
		return n; /* directories carry no version */
	return isofs_strip_version(out, n, 1);
}

/* May a lookup that matches nothing exactly fall back to a match without
 * case?  Only for plain ISO 9660 names, whose d-characters have no case
 * (map=normal shows them lower-cased, map=off as recorded), and only with
 * check=relaxed.  Rock Ridge, Joliet and ISO 9660:1999 names are case
 * preserving and may differ in case alone: always exact there. */
static int isofs_ci_names(const isofs_fs_t *fs)
{
	return !fs->rock && !fs->joliet && fs->fs_version != 2 &&
	       !fs->o.strict;
}

/* ASCII case-blind comparison (the d-characters are ASCII). */
static int isofs_name_eq_ci(const char *a, unsigned alen, const char *b,
			    unsigned blen)
{
	if (alen != blen)
		return 0;
	for (unsigned i = 0; i < alen; i++)
		if (isofs_lower(a[i]) != isofs_lower(b[i]))
			return 0;
	return 1;
}

/* ===========================================================================
 * Directory records and nodes
 * =========================================================================== */

/* Node flags. */
#define ISOFS_NF_DIR 0x0001
#define ISOFS_NF_LINK 0x0002
#define ISOFS_NF_RR 0x0004 /* the record carries Rock Ridge entries */
#define ISOFS_NF_ZF 0x0008 /* zisofs-compressed                     */
#define ISOFS_NF_RELOCATED 0x0010 /* RE: hidden at this place          */
#define ISOFS_NF_HIDDEN 0x0020 /* ISO "existence" bit                 */
/* (ISOFS_NF_DAMAGED below: System Use data unreadable, cut short, or
 * beyond its limits -- what the record says is incomplete) */
#define ISOFS_NF_ASSOC 0x0040 /* associated file                     */
#define ISOFS_NF_SPARSE 0x0080 /* RRIP SF                             */
#define ISOFS_NF_CL 0x0100 /* placeholder of a relocated directory */
#define ISOFS_NF_DOT 0x0200 /* the "." record                      */
#define ISOFS_NF_DOTDOT 0x0400 /* the ".." record                     */
#define ISOFS_NF_DAMAGED 0x0800 /* System Use data partly unreadable  */
#define ISOFS_NF_PL 0x1000 /* ".." carries a parent link (PL)     */

/* An object as resolved from its directory record(s): everything stat,
 * open and readdir need.  On the heap (it carries a name and, for a
 * multi-extent file, an extent array). */
typedef struct isofs_node {
	uint64_t ino;
	uint64_t rec_off; /* volume byte offset of the (first) record      */
	uint64_t rec_doff; /* its offset in its directory                 */
	uint64_t next_doff; /* directory offset after the record set       */
	uint32_t flags;
	uint32_t mode, nlink, uid, gid;
	uint64_t rdev;
	uint64_t size; /* what a read sees                               */
	uint64_t raw_size; /* recorded bytes                             */
	int64_t atime, mtime, ctime;
	uint32_t atime_ns, mtime_ns, ctime_ns;
	uint64_t cl; /* CL target (data LBA of the relocated directory) */
	uint64_t pl; /* PL target ("..": the real parent)               */
	uint64_t dotdot; /* ".." record: data LBA of the parent         */
	/* zisofs */
	uint32_t zf_blog, zf_header;
	uint64_t zf_usize;
	/* data */
	uint32_t nextents, ext_cap;
	isofs_extent_t *ext; /* &ext1, or a heap array                   */
	isofs_extent_t ext1;
	/* the first record's identifier, for matching continuation records */
	unsigned id_len;
	uint8_t id[256];
	/* name (when asked for) */
	unsigned name_len;
	char name[ISOFS_NAME_MAX + 1];
} isofs_node_t;

static void isofs_node_clear(isofs_node_t *n)
{
	if (n->ext && n->ext != &n->ext1)
		kfree(n->ext);
	mm_memset(n, 0, sizeof(*n));
	n->ext = &n->ext1;
}

/* Append an extent to a node (multi-extent files), growing the array. */
static int isofs_node_add_extent(isofs_fs_t *fs, isofs_node_t *n,
				 const isofs_extent_t *e)
{
	if (n->nextents >= ISOFS_MAX_EXTENTS)
		return ISOFS_CORRUPT(fs, "more than %u extents in one file",
				     ISOFS_MAX_EXTENTS);
	if (n->nextents == 0) {
		n->ext1 = *e;
		n->ext = &n->ext1;
		n->nextents = 1;
		return ST_OK;
	}
	if (n->ext == &n->ext1 || n->nextents >= n->ext_cap) {
		uint32_t cap = n->ext_cap ? n->ext_cap * 2 : 8;
		isofs_extent_t *ne;

		if (cap > ISOFS_MAX_EXTENTS)
			cap = ISOFS_MAX_EXTENTS;
		ne = (isofs_extent_t *)kalloc(cap * sizeof(*ne));
		if (!ne)
			return ST_NOMEM;
		mm_memcpy(ne, n->ext, n->nextents * sizeof(*ne));
		if (n->ext != &n->ext1)
			kfree(n->ext);
		n->ext = ne;
		n->ext_cap = cap;
	}
	n->ext[n->nextents++] = *e;
	return ST_OK;
}

/* Is the directory record at `rec' (inside a block of the directory with
 * `room' bytes from it to the end of the block or the directory, whichever
 * comes first) well formed? */
static int isofs_record_ok(const isofs_fs_t *fs, const uint8_t *rec,
			   uint64_t room)
{
	unsigned dr = rec[ISO_DR_LEN], fi;

	if (dr < ISO_DR_MIN || dr > room)
		return 0;
	fi = rec[ISO_DR_NAMELEN];
	if (fi == 0 || ISO_DR_NAME + fi > dr)
		return 0;
	if (fs->o.strict &&
	    (!isofs_733_ok(rec + ISO_DR_EXTENT) || !isofs_733_ok(rec + ISO_DR_SIZE)))
		return 0;
	return 1;
}

/* The data length of a record, cruft applied. */
static uint64_t isofs_record_size(const isofs_fs_t *fs, const uint8_t *rec)
{
	uint64_t sz = isofs_733(rec + ISO_DR_SIZE);

	if (fs->o.cruft)
		sz &= 0x00FFFFFFu;
	return sz;
}

/* The extent a record describes (XAR skipped, interleaving noted). */
static int isofs_record_extent(isofs_fs_t *fs, const uint8_t *rec,
			       isofs_extent_t *e)
{
	uint64_t lba = isofs_733(rec + ISO_DR_EXTENT);

	mm_memset(e, 0, sizeof(*e));
	if (!isofs_add_ok(lba, rec[ISO_DR_XAR], &e->lba))
		return ST_IO;
	e->len = isofs_record_size(fs, rec);
	e->fus = rec[ISO_DR_FUS];
	e->igs = e->fus ? rec[ISO_DR_IGS] : 0;
	return ST_OK;
}

/* Decide the attributes of a node from its first record and what the
 * System Use walk found. */
static void isofs_node_attrs(isofs_fs_t *fs, isofs_node_t *n,
			     const uint8_t *rec, const isofs_su_t *su)
{
	int is_dir = (rec[ISO_DR_FLAGS] & ISO_FLAG_DIR) != 0;
	int64_t rdate = isofs_date7(rec + ISO_DR_DATE);
	int rr = su && (su->seen & SU_SEEN_ROCK);

	if (rr)
		n->flags |= ISOFS_NF_RR;
	if (su && (su->seen & SU_SEEN_DAMAGED))
		n->flags |= ISOFS_NF_DAMAGED;
	/* An NM whose last piece announced more that never came: the name
	 * is cut short. */
	if (rr && (su->seen & SU_SEEN_NM) && !su->nm_done && !su->nm_bad &&
	    !su->nm_special)
		n->flags |= ISOFS_NF_DAMAGED;
	if (rr && (su->seen & SU_SEEN_CL) && !(n->flags & (ISOFS_NF_DOT |
							   ISOFS_NF_DOTDOT))) {
		n->flags |= ISOFS_NF_CL;
		n->cl = su->cl;
		is_dir = 1;
	}
	if (rr && (su->seen & SU_SEEN_PL) && (n->flags & ISOFS_NF_DOTDOT)) {
		n->flags |= ISOFS_NF_PL;
		n->pl = su->pl;
	}
	if (rr && (su->seen & SU_SEEN_RE))
		n->flags |= ISOFS_NF_RELOCATED;
	if (rec[ISO_DR_FLAGS] & ISO_FLAG_HIDDEN)
		n->flags |= ISOFS_NF_HIDDEN;
	if (rec[ISO_DR_FLAGS] & ISO_FLAG_ASSOC)
		n->flags |= ISOFS_NF_ASSOC;

	if (rr && (su->seen & SU_SEEN_PX) && !fs->o.override_rr) {
		uint32_t m = su->px_mode;
		uint32_t type = m & RR_S_IFMT;

		/* The directory flag decides what the driver walks into;
		 * a PX type that contradicts it is overridden. */
		if (is_dir && type != RR_S_IFDIR)
			m = (m & 07777) | RR_S_IFDIR;
		else if (!is_dir && (type == RR_S_IFDIR || type == 0))
			m = (m & 07777) | RR_S_IFREG;
		else if (!is_dir && type != RR_S_IFREG && type != RR_S_IFLNK &&
			 type != RR_S_IFCHR && type != RR_S_IFBLK &&
			 type != RR_S_IFIFO && type != RR_S_IFSOCK)
			m = (m & 07777) | RR_S_IFREG;
		n->mode = m;
		n->nlink = su->px_nlink ? su->px_nlink : 1;
		n->uid = su->px_uid;
		n->gid = su->px_gid;
	} else {
		n->mode = is_dir ? (S_IFDIR | (fs->o.dmode & 07777)) :
				   (S_IFREG | (fs->o.fmode & 07777));
		/* Unknown: 1 tells tree walkers not to count on the
		 * "subdirectories + 2" rule. */
		n->nlink = 1;
		n->uid = fs->o.uid;
		n->gid = fs->o.gid;
		if (rr && (su->seen & SU_SEEN_PX) && fs->o.override_rr &&
		    !is_dir && (su->px_mode & RR_S_IFMT) == RR_S_IFLNK)
			n->mode = S_IFLNK | 0777;
	}
	if (is_dir)
		n->flags |= ISOFS_NF_DIR;
	if ((n->mode & RR_S_IFMT) == RR_S_IFLNK)
		n->flags |= ISOFS_NF_LINK;
	if (rr && (su->seen & SU_SEEN_PN) &&
	    ((n->mode & RR_S_IFMT) == RR_S_IFCHR ||
	     (n->mode & RR_S_IFMT) == RR_S_IFBLK)) {
		uint32_t hi = su->pn_high, lo = su->pn_low;

		/* A high word of 0: the low word is a device number in the
		 * 32-bit encoding (major in bits 8..19, minor in bits 0..7
		 * and 20..31 -- which includes the old 16-bit one).  Else
		 * the high word is the major and the low the minor. */
		if (hi == 0)
			n->rdev = kdev_encode((lo >> 8) & 0xFFFu,
					      (lo & 0xFFu) |
						      ((lo >> 12) & 0xFFF00u));
		else
			n->rdev = kdev_encode(hi, lo);
	}

	n->mtime = n->atime = n->ctime = rdate;
	if (rr && (su->seen & SU_SEEN_TF)) {
		if (su->tf_have & RR_TF_MODIFY) {
			n->mtime = su->tf_mtime;
			n->mtime_ns = su->tf_mtime_ns;
		} else if (su->tf_have & RR_TF_CREATE) {
			n->mtime = su->tf_crtime;
			n->mtime_ns = su->tf_crtime_ns;
		}
		if (su->tf_have & RR_TF_ACCESS) {
			n->atime = su->tf_atime;
			n->atime_ns = su->tf_atime_ns;
		} else {
			n->atime = n->mtime;
			n->atime_ns = n->mtime_ns;
		}
		if (su->tf_have & RR_TF_ATTR) {
			n->ctime = su->tf_ctime;
			n->ctime_ns = su->tf_ctime_ns;
		} else {
			n->ctime = n->mtime;
			n->ctime_ns = n->mtime_ns;
		}
	}
	if (rr && (su->seen & SU_SEEN_ZF) && !is_dir && !fs->o.nocompress &&
	    (n->mode & RR_S_IFMT) == RR_S_IFREG && su->zf_alg[0] == 'p' &&
	    su->zf_alg[1] == 'z') {
		n->flags |= ISOFS_NF_ZF;
		n->zf_blog = su->zf_blog;
		n->zf_header = su->zf_hdiv4 * 4;
		n->zf_usize = su->zf_usize;
	}
	if (rr && (su->seen & SU_SEEN_SF) && !is_dir &&
	    (n->mode & RR_S_IFMT) == RR_S_IFREG)
		n->flags |= ISOFS_NF_SPARSE;
}

/* The name of a node from its first record, by the volume's rules: Rock
 * Ridge NM when the volume has Rock Ridge and the record a usable NM, else
 * Joliet on a Joliet tree, else the plain identifier.  A name that would
 * be "." or ".." (a hostile NM or identifier) is replaced. */
static void isofs_node_name(isofs_fs_t *fs, isofs_node_t *n, const uint8_t *rec,
			    const isofs_su_t *su)
{
	unsigned fi = rec[ISO_DR_NAMELEN];
	const uint8_t *id = rec + ISO_DR_NAME;

	if (n->flags & ISOFS_NF_DOT) {
		n->name[0] = '.';
		n->name[1] = '\0';
		n->name_len = 1;
		return;
	}
	if (n->flags & ISOFS_NF_DOTDOT) {
		n->name[0] = '.';
		n->name[1] = '.';
		n->name[2] = '\0';
		n->name_len = 2;
		return;
	}
	if (fs->rock && su && (su->seen & SU_SEEN_NM) && !su->nm_bad &&
	    !su->nm_special && su->nm_len > 0 &&
	    !isofs_is_dot_name(su->nm, su->nm_len)) {
		mm_memcpy(n->name, su->nm, su->nm_len + 1);
		n->name_len = su->nm_len;
		return;
	}
	if (fs->joliet)
		n->name_len = isofs_joliet_name(id, fi, n->name);
	else
		n->name_len = isofs_plain_name(fs, id, fi,
					       (rec[ISO_DR_FLAGS] &
						ISO_FLAG_DIR) != 0,
					       n->name);
	if (n->name_len == 0 || isofs_is_dot_name(n->name, n->name_len)) {
		n->name[0] = '_';
		n->name[1] = '\0';
		n->name_len = 1;
	}
}

/* Parse one directory record (the first of a record set) at volume byte
 * offset `rec_off' into `n'.  `root_dot': the root directory's "." record
 * (System Use from its very start, where SP is).  `su' is scratch. */
static int isofs_parse_record(isofs_fs_t *fs, isofs_su_t *su, const uint8_t *rec,
			      uint64_t rec_off, int root_dot, int want_name,
			      int want_link, isofs_node_t *n)
{
	unsigned fi = rec[ISO_DR_NAMELEN];
	const uint8_t *area = NULL;
	unsigned alen;
	isofs_extent_t e;
	int r;

	isofs_node_clear(n);
	n->rec_off = rec_off;
	if (fi == 1 && rec[ISO_DR_NAME] == 0)
		n->flags |= ISOFS_NF_DOT;
	else if (fi == 1 && rec[ISO_DR_NAME] == 1)
		n->flags |= ISOFS_NF_DOTDOT;
	n->id_len = fi;
	mm_memcpy(n->id, rec + ISO_DR_NAME, fi);

	isofs_su_reset(su, want_link);
	su->scan_records++;
	if (fs->rock) {
		alen = isofs_su_area_of(fs, rec, root_dot, &area);
		if (alen)
			isofs_su_walk(fs, su, area, alen);
	}
	r = isofs_record_extent(fs, rec, &e);
	if (r != ST_OK)
		return ISOFS_CORRUPT(fs, "directory record extent overflows");
	isofs_node_attrs(fs, n, rec, fs->rock ? su : NULL);
	if (want_name)
		isofs_node_name(fs, n, rec, fs->rock ? su : NULL);
	if (n->flags & ISOFS_NF_DOTDOT)
		n->dotdot = e.lba;
	r = isofs_node_add_extent(fs, n, &e);
	if (r != ST_OK)
		return r;
	n->raw_size = e.len;
	n->size = e.len;
	if (n->flags & ISOFS_NF_ZF)
		n->size = n->zf_usize;
	if (n->flags & ISOFS_NF_SPARSE)
		n->size = su->sf_size;
	/* st_ino: a directory is its data's first block (a relocated
	 * directory the CL target's), anything else its record. */
	if (n->flags & ISOFS_NF_CL)
		n->ino = n->cl;
	else if (n->flags & ISOFS_NF_DIR)
		n->ino = e.lba;
	else
		n->ino = ISOFS_INO_FILE(rec_off);
	return ST_OK;
}

/* ===========================================================================
 * Directory iteration
 * =========================================================================== */

typedef struct isofs_dirit {
	isofs_fs_t *fs;
	isofs_stream_t *ds; /* the directory's stream (referenced by caller) */
	const isofs_extent_t *ext; /* the directory's one extent           */
	uint64_t size;
	uint64_t off; /* next record                                     */
	pc_page_t *pg; /* held page under `off'                           */
	unsigned long pg_idx;
	int damaged; /* a record was skipped as corrupt                */
	int ioerr; /* a page could not be read                       */
} isofs_dirit_t;

static void isofs_dirit_init(isofs_dirit_t *it, isofs_fs_t *fs,
			     isofs_stream_t *ds, uint64_t off)
{
	mm_memset(it, 0, sizeof(*it));
	it->fs = fs;
	it->ds = ds;
	it->ext = &ds->ext[0];
	it->size = ds->size;
	it->off = off;
}

static void isofs_dirit_done(isofs_dirit_t *it)
{
	if (it->pg)
		pagecache_put(it->pg);
	it->pg = NULL;
}

/* The next directory record: ST_OK with *rec pointing into a held page
 * (valid until the next call), ST_EOF at the end, ST_IO when the
 * directory cannot be read.  A record that is not well formed is skipped
 * with the rest of its block (it->damaged set). */
static int isofs_dirit_next(isofs_dirit_t *it, const uint8_t **rec,
			    uint64_t *rec_doff)
{
	isofs_fs_t *fs = it->fs;

	might_sleep();
	while (it->off < it->size) {
		uint64_t blk_end = (it->off / fs->lbs + 1) * fs->lbs;
		unsigned long pi = (unsigned long)(it->off / PAGE_SIZE);
		const uint8_t *r;

		if (blk_end > it->size)
			blk_end = it->size;
		if (!it->pg || it->pg_idx != pi) {
			if (it->pg)
				pagecache_put(it->pg);
			it->pg = pagecache_get_held(it->ds->chain, pi,
						    (unsigned long)it->size,
						    &fs->sb, it->ds->chain);
			if (!it->pg || !it->pg->data) {
				if (it->pg)
					pagecache_put(it->pg);
				it->pg = NULL;
				it->ioerr = 1;
				return ST_IO;
			}
			it->pg_idx = pi;
		}
		r = it->pg->data + (it->off % PAGE_SIZE);
		if (r[0] == 0) {
			/* Padding: the rest of the block is unused. */
			it->off = blk_end;
			continue;
		}
		if (!isofs_record_ok(fs, r, blk_end - it->off)) {
			ISOFS_CORRUPT(fs, "bad directory record in directory "
					  "%lu at offset %lu",
				      (unsigned long)it->ds->key,
				      (unsigned long)it->off);
			it->damaged = 1;
			it->off = blk_end;
			continue;
		}
		*rec = r;
		*rec_doff = it->off;
		it->off += r[ISO_DR_LEN];
		return ST_OK;
	}
	return ST_EOF;
}

/* Read a whole record set: the record just returned by the iterator
 * (`rec' at directory offset `doff') and, for a multi-extent file, the
 * continuation records after it.  On return n->next_doff is the offset
 * after the set and the iterator stands there. */
static int isofs_read_set(isofs_dirit_t *it, isofs_su_t *su, const uint8_t *rec,
			  uint64_t doff, int root_dot, int want_name,
			  int want_link, isofs_node_t *n)
{
	isofs_fs_t *fs = it->fs;
	int multi = (rec[ISO_DR_FLAGS] & ISO_FLAG_MULTI) != 0;
	int r;

	/* The record's real place on the volume, through the directory's
	 * extent mapping (a hostile directory may be interleaved): what
	 * st_ino of a file is made of, so it must be unique. */
	r = isofs_parse_record(fs, su, rec,
			       isofs_ext_block(it->ext, doff / fs->lbs) *
					       fs->lbs +
				       doff % fs->lbs,
			       root_dot,
			       want_name, want_link, n);
	if (r != ST_OK)
		return r;
	n->rec_doff = doff;
	if (multi && (n->flags & (ISOFS_NF_DIR | ISOFS_NF_DOT |
				  ISOFS_NF_DOTDOT))) {
		/* Directories are never split; ignore the bit. */
		multi = 0;
	}
	while (multi) {
		const uint8_t *c;
		uint64_t cdoff;
		isofs_extent_t e;

		r = isofs_dirit_next(it, &c, &cdoff);
		if (r == ST_EOF)
			return ISOFS_CORRUPT(fs, "multi-extent file ends with "
						 "its directory");
		if (r != ST_OK)
			return r;
		if (c[ISO_DR_FLAGS] & ISO_FLAG_DIR)
			return ISOFS_CORRUPT(fs, "directory inside a "
						 "multi-extent file");
		if (c[ISO_DR_NAMELEN] != n->id_len ||
		    !isofs_memeq(c + ISO_DR_NAME, n->id, n->id_len)) {
			if (fs->o.strict)
				return ISOFS_CORRUPT(fs, "multi-extent records "
							 "with different names");
		}
		multi = (c[ISO_DR_FLAGS] & ISO_FLAG_MULTI) != 0;
		r = isofs_record_extent(fs, c, &e);
		if (r != ST_OK)
			return ISOFS_CORRUPT(fs, "extent overflows");
		r = isofs_node_add_extent(fs, n, &e);
		if (r != ST_OK)
			return r;
		if (!isofs_add_ok(n->raw_size, e.len, &n->raw_size))
			return ISOFS_CORRUPT(fs, "file size overflows");
		if (!(n->flags & (ISOFS_NF_ZF | ISOFS_NF_SPARSE)))
			n->size = n->raw_size;
	}
	n->next_doff = it->off;
	return ST_OK;
}

/* Should readdir and lookup skip this entry? */
static int isofs_node_invisible(const isofs_fs_t *fs, const isofs_node_t *n)
{
	if (n->flags & ISOFS_NF_RELOCATED)
		return 1;
	if ((n->flags & ISOFS_NF_ASSOC) && !fs->o.unhide)
		return 1;
	if ((n->flags & ISOFS_NF_HIDDEN) && fs->o.hide)
		return 1;
	return 0;
}

/* ===========================================================================
 * Path operations: work area, directory streams, lookup, walk
 * =========================================================================== */

/* Room for a path being walked: the VFS's longest path with the longest
 * symlink target spliced in front of its rest. */
#define ISOFS_WALK_BUF (VFS_MAX_PATH + ISOFS_SYMLINK_MAX + 8)

/* Everything one path operation needs, in one heap allocation (no large
 * buffers on the kernel stack). */
typedef struct isofs_work {
	isofs_fs_t *fs;
	vfs_mount_t *m;
	isofs_su_t su;
	isofs_node_t node; /* the result                   */
	isofs_node_t tmp; /* records being scanned        */
	isofs_node_t dot; /* "." records                  */
	unsigned link_len;
	int link_bad;
	char link[ISOFS_SYMLINK_MAX + 1];
	char key[ISOFS_NAME_MAX + 1];
	char path[ISOFS_WALK_BUF];
	char spare[ISOFS_WALK_BUF];
	uint64_t stack[ISOFS_MAX_DEPTH];
	uint8_t dotrec[256];
} isofs_work_t;

static isofs_work_t *isofs_work_alloc(isofs_fs_t *fs, vfs_mount_t *m)
{
	isofs_work_t *w = (isofs_work_t *)kalloc(sizeof(*w));

	if (!w)
		return NULL;
	/* Only the parts that are read before written need clearing. */
	w->fs = fs;
	w->m = m;
	w->link_len = 0;
	w->link_bad = 0;
	w->link[0] = '\0';
	isofs_su_scan_begin(&w->su);
	mm_memset(&w->node, 0, sizeof(w->node));
	mm_memset(&w->tmp, 0, sizeof(w->tmp));
	mm_memset(&w->dot, 0, sizeof(w->dot));
	w->node.ext = &w->node.ext1;
	w->tmp.ext = &w->tmp.ext1;
	w->dot.ext = &w->dot.ext1;
	return w;
}

static void isofs_work_free(isofs_work_t *w)
{
	if (!w)
		return;
	isofs_node_clear(&w->node);
	isofs_node_clear(&w->tmp);
	isofs_node_clear(&w->dot);
	kfree(w);
}

/* The "." record of the directory whose data starts at logical block
 * `ino', parsed into `n' (attributes of the directory itself). */
static int isofs_read_dot(isofs_fs_t *fs, isofs_work_t *w, uint64_t ino,
			  isofs_node_t *n)
{
	const uint8_t *rec = w->dotrec;
	uint64_t room = isofs_min64(sizeof(w->dotrec), fs->lbs);
	int r;

	if (ino >= fs->dev_blocks)
		return ISOFS_CORRUPT(fs, "directory at block %lu is beyond "
					 "the device",
				     (unsigned long)ino);
	/* The system area (the first 32 KB) holds no directory. */
	if (ino * fs->lbs < ISOFS_VD_START_BYTE)
		return ISOFS_CORRUPT(fs, "directory at block %lu inside the "
					 "system area",
				     (unsigned long)ino);
	r = isofs_vol_read(fs, ino * fs->lbs, w->dotrec, room);
	if (r != ST_OK)
		return r;
	if (!isofs_record_ok(fs, rec, room) || rec[ISO_DR_NAMELEN] != 1 ||
	    rec[ISO_DR_NAME] != 0 || !(rec[ISO_DR_FLAGS] & ISO_FLAG_DIR))
		return ISOFS_CORRUPT(fs, "block %lu does not start a directory",
				     (unsigned long)ino);
	r = isofs_parse_record(fs, &w->su, rec, ino * fs->lbs,
			       ino == fs->root_ino, 0, 0, n);
	if (r != ST_OK)
		return r;
	if (n->ext[0].lba != ino)
		return ISOFS_CORRUPT(fs, "directory at block %lu names itself "
					 "at block %lu",
				     (unsigned long)ino,
				     (unsigned long)n->ext[0].lba);
	if (n->raw_size < ISO_DR_MIN || n->raw_size > ISOFS_DIR_MAX_BYTES)
		return ISOFS_CORRUPT(fs, "directory at block %lu has size %lu",
				     (unsigned long)ino,
				     (unsigned long)n->raw_size);
	n->ino = ino;
	n->flags |= ISOFS_NF_DIR;
	n->flags &= ~(ISOFS_NF_LINK | ISOFS_NF_ZF | ISOFS_NF_SPARSE |
		      ISOFS_NF_CL);
	n->size = n->raw_size;
	if ((n->mode & RR_S_IFMT) != RR_S_IFDIR)
		n->mode = (n->mode & 07777) | RR_S_IFDIR;
	return ST_OK;
}

/* Build an unpublished stream from a node's extents. */
static int isofs_stream_build(isofs_fs_t *fs, const isofs_node_t *n,
			      isofs_stream_t **out)
{
	isofs_stream_t *s = (isofs_stream_t *)kalloc(sizeof(*s));
	int r;

	*out = NULL;
	if (!s)
		return ST_NOMEM;
	mm_memset(s, 0, sizeof(*s));
	s->key = n->ino;
	s->nextents = n->nextents;
	if (n->nextents <= 1) {
		s->ext1 = n->ext[0];
		s->ext = &s->ext1;
		s->nextents = 1;
	} else {
		s->ext = (isofs_extent_t *)kalloc(n->nextents * sizeof(*s->ext));
		if (!s->ext) {
			kfree(s);
			return ST_NOMEM;
		}
		mm_memcpy(s->ext, n->ext, n->nextents * sizeof(*s->ext));
		s->ext_bytes = n->nextents * sizeof(*s->ext);
	}
	if (n->flags & ISOFS_NF_DIR)
		s->flags |= ISOFS_SF_DIR;
	if (n->flags & ISOFS_NF_ZF)
		s->flags |= ISOFS_SF_ZISOFS;
	if (n->flags & ISOFS_NF_SPARSE)
		s->flags |= ISOFS_SF_SPARSE;
	s->size = n->size;
	r = isofs_stream_layout(fs, s);
	if (r != ST_OK) {
		isofs_stream_free(s);
		return r;
	}
	*out = s;
	return ST_OK;
}

static int isofs_zf_validate(isofs_fs_t *fs, isofs_stream_t *s,
			     const isofs_node_t *n);

/* The stream of the object `n' describes (n->ino is its key), referenced:
 * the published one, or a new one. */
static int isofs_stream_of_node(isofs_fs_t *fs, const isofs_node_t *n,
				isofs_stream_t **out)
{
	isofs_stream_t *s;
	int r;

	*out = isofs_stream_get(fs, n->ino);
	if (*out)
		return ST_OK;
	r = isofs_stream_build(fs, n, &s);
	if (r != ST_OK)
		return r;
	if (s->flags & ISOFS_SF_ZISOFS) {
		r = isofs_zf_validate(fs, s, n);
		if (r != ST_OK) {
			isofs_stream_free(s);
			return r;
		}
	}
	*out = isofs_stream_publish(fs, s);
	return *out ? ST_OK : ST_NOMEM;
}

/* The stream of the directory with data at block `ino', referenced. */
static int isofs_dir_stream(isofs_fs_t *fs, isofs_work_t *w, uint64_t ino,
			    isofs_stream_t **out)
{
	int r;

	*out = isofs_stream_get(fs, ino);
	if (*out)
		return ST_OK;
	r = isofs_read_dot(fs, w, ino, &w->dot);
	if (r != ST_OK)
		return r;
	return isofs_stream_of_node(fs, &w->dot, out);
}

/* ---- the dentry cache -------------------------------------------------
 *
 * name -> object, per directory, positive and negative, through the
 * case-sensitive interface (dcache.h).  Keys: the directory's st_ino (its
 * data block -- stable, unlike a stream handle that eviction recycles),
 * tagged, and the name exactly as asked for (a case-blind match of a plain
 * ISO name is remembered under the spelling that was asked).  Values:
 * st_ino, the DIRECTORY offset of the object's (first) record (in the
 * `size' slot, which the cache does not interpret) and the kind.  Only
 * what was read completely is remembered: no entry for an object whose
 * System Use data was damaged, no negative entry from a scan that skipped
 * or could not read anything, and readdir only adds names nobody has
 * recorded yet (never overwriting what a lookup decided).
 * Nothing on the volume ever changes, so entries never go stale while the
 * instance lives; unmount drops them all (vfs_sb_release_caches). */

#define ISOFS_DC_DIR 0x10 /* the cache's directory convention  */
/* The parent key: the directory's st_ino with a tag bit, never 0 (the
 * cache's "no parent"). */
#define ISOFS_DC_PARENT(ino) ((unsigned long)(ino) | (1UL << 62))
#define ISOFS_DC_LINK 0x20

typedef struct isofs_dent {
	uint64_t ino;
	uint64_t doff; /* directory offset of the (first) record */
	unsigned kind; /* ISOFS_DC_DIR, ISOFS_DC_LINK or 0      */
	uint32_t nflags; /* ISOFS_NF_* of the scanned node (0 from the cache) */
} isofs_dent_t;

/* The dentry cache key of a name: the name exactly (NUL-terminated). */
static void isofs_make_key(const char *name, unsigned len, char *key)
{
	mm_memcpy(key, name, len);
	key[len] = '\0';
}

static unsigned isofs_node_kind(const isofs_node_t *n)
{
	if (n->flags & ISOFS_NF_DIR)
		return ISOFS_DC_DIR;
	if (n->flags & ISOFS_NF_LINK)
		return ISOFS_DC_LINK;
	return 0;
}

/* Is the CL target of a relocation placeholder in directory `dir_ino' a
 * directory, and not that directory itself? */
static int isofs_check_cl(isofs_fs_t *fs, isofs_work_t *w, uint64_t dir_ino,
			  uint64_t target)
{
	isofs_stream_t *ts;
	int r;

	if (target == dir_ino || target == 0 || target >= fs->dev_blocks)
		return ISOFS_CORRUPT(fs, "bad Rock Ridge child link to block "
					 "%lu in directory %lu",
				     (unsigned long)target,
				     (unsigned long)dir_ino);
	r = isofs_dir_stream(fs, w, target, &ts);
	if (r != ST_OK)
		return r;
	isofs_stream_put(fs, ts);
	return ST_OK;
}

/* Find `name' (`len' bytes) in the directory with data at block `dir_ino'.
 * ST_OK and *d, ST_NOT_FOUND, or an error.
 *
 * Names match exactly.  Only on a volume with plain ISO 9660 names (no
 * Rock Ridge, no Joliet, no ISO 9660:1999 tree -- the one format whose
 * d-characters have no case) and with check=relaxed, a name that matches
 * nothing exactly matches the first entry equal to it without case.  The
 * dentry cache is keyed by the exact name asked for.
 *
 * A scan that met anything it could not read completely -- a record
 * skipped as corrupt, an unreadable page, System Use data that was damaged
 * or cut short -- proves nothing about absence: then "not found" is EIO
 * and nothing negative is remembered.  An entry whose own System Use data
 * was damaged is answered but not cached. */
static int isofs_lookup(isofs_fs_t *fs, isofs_work_t *w, uint64_t dir_ino,
			const char *name, unsigned len, isofs_dent_t *d)
{
	isofs_stream_t *ds;
	isofs_dirit_t it;
	dc_result_t res;
	isofs_dent_t ci = { 0, 0, 0, 0 };
	int r, found = 0, have_ci = 0, damaged = 0;

	if (len == 0)
		return ST_NOT_FOUND;
	if (len > ISOFS_NAME_MAX)
		return ST_NAMETOOLONG;
	isofs_make_key(name, len, w->key);
	switch (dcache_lookup_cs(&fs->sb, ISOFS_DC_PARENT(dir_ino), w->key,
				 &res)) {
	case DC_LOOKUP_FOUND:
		d->ino = res.ino;
		d->doff = res.size;
		d->kind = res.attr & (ISOFS_DC_DIR | ISOFS_DC_LINK);
		d->nflags = 0;
		return ST_OK;
	case DC_LOOKUP_NEGATIVE:
		return ST_NOT_FOUND;
	default:
		break;
	}

	r = isofs_dir_stream(fs, w, dir_ino, &ds);
	if (r != ST_OK)
		return r;
	isofs_su_scan_begin(&w->su);
	isofs_dirit_init(&it, fs, ds, 0);
	for (;;) {
		const uint8_t *rec;
		uint64_t doff;
		unsigned fi;

		r = isofs_dirit_next(&it, &rec, &doff);
		if (r != ST_OK)
			break;
		fi = rec[ISO_DR_NAMELEN];
		if (fi == 1 && rec[ISO_DR_NAME] <= 1)
			continue; /* "." and ".." */
		r = isofs_read_set(&it, &w->su, rec, doff, 0, 1, 0, &w->tmp);
		if (w->su.over_budget)
			break;
		if (r != ST_OK) {
			it.damaged = 1;
			if (r == ST_NOMEM)
				break;
			r = ST_OK;
			continue;
		}
		if (w->tmp.flags & ISOFS_NF_DAMAGED)
			damaged = 1;
		if (isofs_node_invisible(fs, &w->tmp))
			continue;
		if (w->tmp.name_len == len &&
		    isofs_memeq(w->tmp.name, name, len)) {
			found = 1;
			d->ino = w->tmp.ino;
			d->doff = w->tmp.rec_doff;
			d->kind = isofs_node_kind(&w->tmp);
			d->nflags = w->tmp.flags;
			break;
		}
		if (!have_ci && isofs_ci_names(fs) &&
		    isofs_name_eq_ci(w->tmp.name, w->tmp.name_len, name, len)) {
			have_ci = 1;
			ci.ino = w->tmp.ino;
			ci.doff = w->tmp.rec_doff;
			ci.kind = isofs_node_kind(&w->tmp);
			ci.nflags = w->tmp.flags;
		}
	}
	isofs_dirit_done(&it);
	if (w->su.over_budget) {
		isofs_stream_put(fs, ds);
		return ISOFS_CORRUPT(fs, "directory %lu: System Use data beyond "
					 "the work budget of a scan",
				     (unsigned long)dir_ino);
	}
	/* The case-blind answer only from a scan that saw every entry: an
	 * exact match could hide in what was not read. */
	if (!found && have_ci && r != ST_NOMEM && !it.ioerr && !it.damaged &&
	    !damaged) {
		*d = ci;
		found = 1;
	}
	if (found && (d->nflags & ISOFS_NF_CL)) {
		r = isofs_check_cl(fs, w, dir_ino, d->ino);
		if (r != ST_OK) {
			isofs_stream_put(fs, ds);
			return r;
		}
	}
	isofs_stream_put(fs, ds);
	if (found) {
		if (!(d->nflags & ISOFS_NF_DAMAGED))
			dcache_insert_cs(&fs->sb, ISOFS_DC_PARENT(dir_ino),
					 w->key, d->ino, d->doff, d->kind);
		return ST_OK;
	}
	if (r == ST_NOMEM)
		return ST_NOMEM;
	if (it.ioerr)
		return ST_IO;
	if (it.damaged || damaged)
		return ISOFS_CORRUPT(fs, "directory %lu could not be read "
					 "completely: \"%s\" not decidable",
				     (unsigned long)dir_ino, w->key);
	/* "Not here" is remembered only when every record was read. */
	dcache_insert_negative_cs(&fs->sb, ISOFS_DC_PARENT(dir_ino), w->key);
	return ST_NOT_FOUND;
}

/* The full node of a looked-up entry of directory `dir_ino'.  With
 * `want_link', a symbolic link's target lands in w->link. */
static int isofs_load(isofs_fs_t *fs, isofs_work_t *w, uint64_t dir_ino,
		      const isofs_dent_t *d, int want_link, isofs_node_t *out)
{
	isofs_stream_t *ds;
	isofs_dirit_t it;
	const uint8_t *rec;
	uint64_t doff;
	int r;

	w->link_len = 0;
	w->link_bad = 0;
	w->link[0] = '\0';
	isofs_su_scan_begin(&w->su);
	if (d->kind & ISOFS_DC_DIR)
		return isofs_read_dot(fs, w, d->ino, out);
	/* A link's size is its target's length: always assembled. */
	if (d->kind & ISOFS_DC_LINK)
		want_link = 1;

	r = isofs_dir_stream(fs, w, dir_ino, &ds);
	if (r != ST_OK)
		return r;
	if (d->doff >= ds->size) {
		isofs_stream_put(fs, ds);
		WARN_ON_ONCE(1); /* our own cache entry: cannot happen */
		return ST_IO;
	}
	isofs_dirit_init(&it, fs, ds, d->doff);
	r = isofs_dirit_next(&it, &rec, &doff);
	if (r == ST_OK && doff != d->doff)
		r = ST_IO;
	if (r == ST_OK)
		r = isofs_read_set(&it, &w->su, rec, doff, 0, 0, want_link,
				   out);
	isofs_dirit_done(&it);
	isofs_stream_put(fs, ds);
	if (r == ST_EOF)
		r = ST_IO;
	if (r != ST_OK)
		return r;
	if (out->ino != d->ino) {
		WARN_ON_ONCE(1);
		return ST_IO;
	}
	if (want_link && (out->flags & ISOFS_NF_LINK)) {
		/* A target is only usable when its SL entries were read to
		 * the last one: damaged or unterminated is EIO, never a
		 * shorter link. */
		if (w->su.sl_bad || !w->su.sl_done ||
		    (w->su.seen & SU_SEEN_DAMAGED) ||
		    (out->flags & ISOFS_NF_DAMAGED)) {
			w->link_bad = 1;
		} else {
			mm_memcpy(w->link, w->su.sl, w->su.sl_len + 1);
			w->link_len = w->su.sl_len;
			out->size = w->link_len;
		}
	}
	return ST_OK;
}

/* Resolve `path' (relative to the volume's root; the VFS hands every path
 * over that way) into w->node.  `follow': a symbolic link as the last
 * component is followed (a trailing slash follows it too and requires a
 * directory).  *parent_out: st_ino of the directory the object was found
 * in (the object itself for the root).
 *
 * Iterative: ".." pops an explicit stack of the directories walked
 * through; a symbolic link is handed to the VFS (vfs_link_hop) while the
 * path is still the one it gave, and is otherwise spliced into the path
 * here, at most ISOFS_MAX_LINK_HOPS times. */
static int isofs_walk(isofs_fs_t *fs, isofs_work_t *w, const char *path,
		      int follow, uint64_t *parent_out)
{
	unsigned len, depth = 0, hops = 0;
	int must_dir, orig = 1, have_final = 0, r;
	uint64_t cur = fs->root_ino;
	isofs_dent_t fin = { 0, 0, 0, 0 };
	char *p;

	if (!path)
		return ST_INVALID;
	len = isofs_strlen(path);
	if (len >= VFS_MAX_PATH + 1 || len >= sizeof(w->path))
		return ST_NAMETOOLONG;
	mm_memcpy(w->path, path, len + 1);
	must_dir = len > 1 && path[len - 1] == '/';
	if (must_dir)
		follow = 1;
	w->stack[0] = fs->root_ino;
	p = w->path;

	for (;;) {
		char *cs, *ce;
		unsigned clen;
		int is_last;
		isofs_dent_t d;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		cs = p;
		while (*p && *p != '/')
			p++;
		ce = p;
		clen = (unsigned)(ce - cs);
		while (*p == '/')
			p++;
		is_last = !*p;

		if (clen == 1 && cs[0] == '.') {
			have_final = 0;
			continue;
		}
		if (clen == 2 && cs[0] == '.' && cs[1] == '.') {
			if (depth > 0)
				depth--;
			cur = w->stack[depth];
			have_final = 0;
			continue;
		}
		r = isofs_lookup(fs, w, cur, cs, clen, &d);
		if (r != ST_OK)
			return r;

		if ((d.kind & ISOFS_DC_LINK) && (!is_last || follow)) {
			unsigned tl, rest;

			r = isofs_load(fs, w, cur, &d, 1, &w->node);
			if (r != ST_OK)
				return r;
			if (w->link_bad)
				return ISOFS_CORRUPT(fs, "unreadable symbolic "
							 "link target");
			tl = w->link_len;
			/* The VFS follows a link wherever it leads (another
			 * mount, the process's root) -- while the path is
			 * still exactly the one it handed over. */
			if (orig && path[0] == '/' &&
			    vfs_link_hop(w->m, path,
					 (unsigned long)(cs - w->path),
					 (unsigned long)(ce - w->path), w->link))
				return ST_LINK;
			if (tl == 0)
				return ST_NOT_FOUND;
			if (++hops > ISOFS_MAX_LINK_HOPS)
				return ST_LOOP;
			rest = isofs_strlen(p);
			if (tl + 1 + rest + 1 > sizeof(w->spare))
				return ST_NAMETOOLONG;
			mm_memcpy(w->spare, w->link, tl);
			if (rest) {
				w->spare[tl] = '/';
				mm_memcpy(w->spare + tl + 1, p, rest + 1);
			} else {
				w->spare[tl] = '\0';
			}
			mm_memcpy(w->path, w->spare, isofs_strlen(w->spare) + 1);
			p = w->path;
			orig = 0;
			have_final = 0;
			if (w->link[0] == '/') {
				depth = 0;
				cur = fs->root_ino;
			}
			continue;
		}

		if (!is_last) {
			if (!(d.kind & ISOFS_DC_DIR))
				return ST_NOTDIR;
			if (depth + 1 >= ISOFS_MAX_DEPTH)
				return ST_NAMETOOLONG;
			for (unsigned i = 0; i <= depth; i++)
				if (w->stack[i] == d.ino) {
					ISOFS_CORRUPT(fs, "directory loop at "
							  "block %lu",
						      (unsigned long)d.ino);
					return ST_LOOP;
				}
			w->stack[++depth] = d.ino;
			cur = d.ino;
			have_final = 0;
			continue;
		}
		fin = d;
		have_final = 1;
	}

	if (have_final) {
		r = isofs_load(fs, w, cur, &fin, 0, &w->node);
		if (parent_out)
			*parent_out = cur;
	} else {
		r = isofs_read_dot(fs, w, cur, &w->node);
		if (parent_out)
			*parent_out = depth > 0 ? w->stack[depth - 1] :
						  fs->root_ino;
	}
	if (r != ST_OK)
		return r;
	if (must_dir && !(w->node.flags & ISOFS_NF_DIR))
		return ST_NOTDIR;
	return ST_OK;
}

/* stat() from a node. */
static void isofs_fill_kstat(isofs_fs_t *fs, const isofs_node_t *n,
			     struct kstat *st)
{
	mm_memset(st, 0, sizeof(*st));
	st->st_dev = 0; /* the VFS reports the mount's */
	st->st_ino = n->ino;
	st->st_mode = n->mode;
	st->st_nlink = n->nlink ? n->nlink : 1;
	st->st_uid = n->uid;
	st->st_gid = n->gid;
	st->st_rdev = n->rdev;
	st->st_size = n->size;
	st->st_blksize = fs->lbs;
	st->st_blocks = isofs_div_up(n->raw_size, 512);
	st->st_atime = (uint64_t)n->atime;
	st->st_atime_nsec = n->atime_ns;
	st->st_mtime = (uint64_t)n->mtime;
	st->st_mtime_nsec = n->mtime_ns;
	st->st_ctime = (uint64_t)n->ctime;
	st->st_ctime_nsec = n->ctime_ns;
}

/* ===========================================================================
 * Inflate (DEFLATE, RFC 1951; zlib wrapper, RFC 1950)
 * ===========================================================================
 *
 * The decoder zisofs needs: one compressed block in, one block of known
 * maximum size out, everything in memory.  Written for bounded behaviour
 * on hostile input:
 *
 *  - every output write is checked against the output capacity and every
 *    back reference against the bytes produced so far (there is no preset
 *    dictionary);
 *  - input is consumed through a bit buffer that never reads past the
 *    input; running out of input is an error, not a read of whatever
 *    follows;
 *  - Huffman tables are checked when built (no over-subscribed code, code
 *    lengths in range, an end-of-block code present), and a code the
 *    table does not assign is an error;
 *  - every loop iteration consumes input or produces output, so the work
 *    is bounded by the sizes of the two buffers.
 *
 * Decoding: canonical Huffman codes, with a direct table for codes of up
 * to ISOFS_HUFF_FAST bits (indexed by the next bits of the stream as they
 * arrive, least significant first) and, for longer codes, a walk over the
 * code lengths in canonical order. */

#define ISOFS_HUFF_FAST 10
#define ISOFS_HUFF_MAXBITS 15
#define ISOFS_INF_LITLEN 288
#define ISOFS_INF_DIST 32
#define ISOFS_INF_CODELEN 19

typedef struct isofs_huff {
	uint16_t fast[1u << ISOFS_HUFF_FAST]; /* (len << 9) | sym, 0 = slow */
	uint16_t count[ISOFS_HUFF_MAXBITS + 1]; /* codes of each length      */
	uint16_t first[ISOFS_HUFF_MAXBITS + 1]; /* first code of each length */
	uint16_t index[ISOFS_HUFF_MAXBITS + 1]; /* its symbol's slot in sym  */
	uint16_t sym[ISOFS_INF_LITLEN]; /* symbols by (length, value)       */
	unsigned nsym;
} isofs_huff_t;

typedef struct isofs_inflate {
	const uint8_t *in;
	uint64_t in_len, in_pos;
	uint64_t bits; /* bit buffer, next bit lowest   */
	unsigned nbits;
	uint8_t *out;
	uint64_t out_len, out_cap;
	isofs_huff_t lit, dist, clen;
	uint8_t lens[ISOFS_INF_LITLEN + ISOFS_INF_DIST];
} isofs_inflate_t;

/* RFC 1951 3.2.5: base values and extra bits of the length and distance
 * codes. */
static const uint16_t isofs_len_base[29] = { 3,	 4,   5,   6,	7,   8,
					     9,	 10,  11,  13,	15,  17,
					     19, 23,  27,  31,	35,  43,
					     51, 59,  67,  83,	99,  115,
					     131, 163, 195, 227, 258 };
static const uint8_t isofs_len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
					     1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
					     4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t isofs_dist_base[30] = {
	1,    2,    3,	  4,	5,    7,     9,	    13,	   17,	  25,
	33,   49,   65,	  97,	129,  193,   257,   385,   513,	  769,
	1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t isofs_dist_extra[30] = { 0, 0, 0,	0,  1,	1,  2,	2,
					      3, 3, 4,	4,  5,	5,  6,	6,
					      7, 7, 8,	8,  9,	9,  10, 10,
					      11, 11, 12, 12, 13, 13 };
/* The order code length code lengths are stored in (3.2.7). */
static const uint8_t isofs_clen_order[ISOFS_INF_CODELEN] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* Top the bit buffer up from the input (never past its end). */
static inline void isofs_inf_fill(isofs_inflate_t *z)
{
	while (z->nbits <= 56 && z->in_pos < z->in_len) {
		z->bits |= (uint64_t)z->in[z->in_pos++] << z->nbits;
		z->nbits += 8;
	}
}

/* `n' (<= 32) bits, least significant first.  -1 when the input ends. */
static inline int64_t isofs_inf_bits(isofs_inflate_t *z, unsigned n)
{
	uint64_t v;

	if (z->nbits < n) {
		isofs_inf_fill(z);
		if (z->nbits < n)
			return -1;
	}
	v = z->bits & ((n == 64) ? ~0ULL : ((1ULL << n) - 1));
	z->bits >>= n;
	z->nbits -= n;
	return (int64_t)v;
}

/* Bit-reverse the low `n' bits of `v' (codes are stored most significant
 * bit first, the stream is read least significant first). */
static inline unsigned isofs_rev(unsigned v, unsigned n)
{
	unsigned r = 0;

	for (unsigned i = 0; i < n; i++) {
		r = (r << 1) | (v & 1);
		v >>= 1;
	}
	return r;
}

/* Build a decoding table from `n' code lengths.  ST_OK, or ST_IO for a set
 * of lengths that is not a prefix code (over-subscribed).  An incomplete
 * code is allowed: the codes it lacks are errors when met. */
static int isofs_huff_build(isofs_huff_t *h, const uint8_t *lens, unsigned n)
{
	uint16_t offs[ISOFS_HUFF_MAXBITS + 2];
	int left = 1;
	unsigned code = 0, idx = 0;

	mm_memset(h->count, 0, sizeof(h->count));
	mm_memset(h->fast, 0, sizeof(h->fast));
	for (unsigned i = 0; i < n; i++) {
		if (lens[i] > ISOFS_HUFF_MAXBITS)
			return ST_IO;
		h->count[lens[i]]++;
	}
	h->count[0] = 0;
	for (unsigned l = 1; l <= ISOFS_HUFF_MAXBITS; l++) {
		left <<= 1;
		left -= h->count[l];
		if (left < 0)
			return ST_IO;
	}
	for (unsigned l = 1; l <= ISOFS_HUFF_MAXBITS; l++) {
		h->first[l] = (uint16_t)code;
		h->index[l] = (uint16_t)idx;
		offs[l] = (uint16_t)idx;
		code = (code + h->count[l]) << 1;
		idx += h->count[l];
	}
	h->nsym = idx;
	for (unsigned s = 0; s < n; s++)
		if (lens[s])
			h->sym[offs[lens[s]]++] = (uint16_t)s;
	/* The direct table: every code of up to ISOFS_HUFF_FAST bits fills
	 * all slots whose low bits are its (reversed) code. */
	for (unsigned l = 1; l <= ISOFS_HUFF_FAST; l++) {
		for (unsigned k = 0; k < h->count[l]; k++) {
			unsigned c = h->first[l] + k;
			unsigned r = isofs_rev(c, l);
			uint16_t e = (uint16_t)((l << 9) |
						h->sym[h->index[l] + k]);

			for (unsigned j = r; j < (1u << ISOFS_HUFF_FAST);
			     j += 1u << l)
				h->fast[j] = e;
		}
	}
	return ST_OK;
}

/* The next symbol, or -1 (input ended, or a code with no symbol). */
static int isofs_huff_decode(isofs_inflate_t *z, const isofs_huff_t *h)
{
	unsigned code = 0;
	uint16_t e;

	isofs_inf_fill(z);
	e = h->fast[z->bits & ((1u << ISOFS_HUFF_FAST) - 1)];
	if (e && (unsigned)(e >> 9) <= z->nbits) {
		z->bits >>= e >> 9;
		z->nbits -= e >> 9;
		return e & 0x1FF;
	}
	for (unsigned l = 1; l <= ISOFS_HUFF_MAXBITS; l++) {
		if (l > z->nbits)
			return -1;
		code = (code << 1) | (unsigned)((z->bits >> (l - 1)) & 1);
		if (code - h->first[l] < h->count[l]) {
			z->bits >>= l;
			z->nbits -= l;
			return h->sym[h->index[l] + (code - h->first[l])];
		}
	}
	return -1;
}

/* The fixed codes of block type 1 (3.2.6). */
static int isofs_inf_fixed(isofs_inflate_t *z)
{
	unsigned i;

	for (i = 0; i < 144; i++)
		z->lens[i] = 8;
	for (; i < 256; i++)
		z->lens[i] = 9;
	for (; i < 280; i++)
		z->lens[i] = 7;
	for (; i < 288; i++)
		z->lens[i] = 8;
	if (isofs_huff_build(&z->lit, z->lens, 288) != ST_OK)
		return ST_IO;
	for (i = 0; i < 30; i++)
		z->lens[i] = 5;
	return isofs_huff_build(&z->dist, z->lens, 30);
}

/* The code descriptions of a dynamic block (3.2.7). */
static int isofs_inf_dynamic(isofs_inflate_t *z)
{
	int64_t hlit, hdist, hclen;
	unsigned n = 0, total;

	hlit = isofs_inf_bits(z, 5);
	hdist = isofs_inf_bits(z, 5);
	hclen = isofs_inf_bits(z, 4);
	if (hlit < 0 || hdist < 0 || hclen < 0)
		return ST_IO;
	hlit += 257;
	hdist += 1;
	hclen += 4;
	if (hlit > 286 || hdist > 30)
		return ST_IO;
	mm_memset(z->lens, 0, sizeof(z->lens));
	for (unsigned i = 0; i < (unsigned)hclen; i++) {
		int64_t v = isofs_inf_bits(z, 3);

		if (v < 0)
			return ST_IO;
		z->lens[isofs_clen_order[i]] = (uint8_t)v;
	}
	if (isofs_huff_build(&z->clen, z->lens, ISOFS_INF_CODELEN) != ST_OK)
		return ST_IO;

	total = (unsigned)(hlit + hdist);
	mm_memset(z->lens, 0, sizeof(z->lens));
	while (n < total) {
		int sym = isofs_huff_decode(z, &z->clen);
		unsigned rep;
		uint8_t val;
		int64_t x;

		if (sym < 0)
			return ST_IO;
		if (sym < 16) {
			z->lens[n++] = (uint8_t)sym;
			continue;
		}
		if (sym == 16) {
			if (n == 0)
				return ST_IO; /* nothing to repeat */
			val = z->lens[n - 1];
			x = isofs_inf_bits(z, 2);
			rep = 3;
		} else if (sym == 17) {
			val = 0;
			x = isofs_inf_bits(z, 3);
			rep = 3;
		} else {
			val = 0;
			x = isofs_inf_bits(z, 7);
			rep = 11;
		}
		if (x < 0)
			return ST_IO;
		rep += (unsigned)x;
		if (n + rep > total)
			return ST_IO;
		while (rep--)
			z->lens[n++] = val;
	}
	if (z->lens[256] == 0)
		return ST_IO; /* no end-of-block code */
	if (isofs_huff_build(&z->lit, z->lens, (unsigned)hlit) != ST_OK)
		return ST_IO;
	return isofs_huff_build(&z->dist, z->lens + hlit, (unsigned)hdist);
}

/* The data of a Huffman-coded block. */
static int isofs_inf_codes(isofs_inflate_t *z)
{
	for (;;) {
		int sym = isofs_huff_decode(z, &z->lit);
		int64_t x;
		unsigned len, dist;

		if (sym < 0)
			return ST_IO;
		if (sym < 256) {
			if (z->out_len >= z->out_cap)
				return ST_IO;
			z->out[z->out_len++] = (uint8_t)sym;
			continue;
		}
		if (sym == 256)
			return ST_OK;
		sym -= 257;
		if (sym >= 29)
			return ST_IO;
		x = isofs_inf_bits(z, isofs_len_extra[sym]);
		if (x < 0)
			return ST_IO;
		len = isofs_len_base[sym] + (unsigned)x;
		sym = isofs_huff_decode(z, &z->dist);
		if (sym < 0 || sym >= 30)
			return ST_IO;
		x = isofs_inf_bits(z, isofs_dist_extra[sym]);
		if (x < 0)
			return ST_IO;
		dist = isofs_dist_base[sym] + (unsigned)x;
		if (dist > z->out_len || len > z->out_cap - z->out_len)
			return ST_IO;
		/* Byte by byte: source and destination may overlap. */
		for (unsigned i = 0; i < len; i++) {
			z->out[z->out_len] = z->out[z->out_len - dist];
			z->out_len++;
		}
	}
}

/* A stored block (3.2.4): byte-aligned LEN, NLEN, then LEN bytes. */
static int isofs_inf_stored(isofs_inflate_t *z)
{
	int64_t len, nlen;

	/* Drop the bits up to the byte boundary. */
	z->bits >>= z->nbits & 7;
	z->nbits -= z->nbits & 7;
	len = isofs_inf_bits(z, 16);
	nlen = isofs_inf_bits(z, 16);
	if (len < 0 || nlen < 0 || (uint64_t)len != (~(uint64_t)nlen & 0xFFFF))
		return ST_IO;
	if ((uint64_t)len > z->out_cap - z->out_len)
		return ST_IO;
	/* First what the bit buffer holds (whole bytes now), then straight
	 * from the input. */
	while (len && z->nbits >= 8) {
		z->out[z->out_len++] = (uint8_t)(z->bits & 0xFF);
		z->bits >>= 8;
		z->nbits -= 8;
		len--;
	}
	if ((uint64_t)len > z->in_len - z->in_pos)
		return ST_IO;
	mm_memcpy(z->out + z->out_len, z->in + z->in_pos, (unsigned long)len);
	z->out_len += (uint64_t)len;
	z->in_pos += (uint64_t)len;
	return ST_OK;
}

/* Raw DEFLATE: decode z->in into z->out until the final block. */
static int isofs_inflate_raw(isofs_inflate_t *z)
{
	for (;;) {
		int64_t final = isofs_inf_bits(z, 1);
		int64_t type = isofs_inf_bits(z, 2);
		int r;

		if (final < 0 || type < 0)
			return ST_IO;
		switch (type) {
		case 0:
			r = isofs_inf_stored(z);
			break;
		case 1:
			r = isofs_inf_fixed(z);
			if (r == ST_OK)
				r = isofs_inf_codes(z);
			break;
		case 2:
			r = isofs_inf_dynamic(z);
			if (r == ST_OK)
				r = isofs_inf_codes(z);
			break;
		default:
			return ST_IO;
		}
		if (r != ST_OK)
			return r;
		if (final)
			return ST_OK;
	}
}

static uint32_t isofs_adler32(const uint8_t *p, uint64_t n)
{
	uint32_t a = 1, b = 0;

	while (n) {
		/* 5552: the most bytes before the sums can overflow. */
		unsigned k = n > 5552 ? 5552 : (unsigned)n;

		n -= k;
		while (k--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return (b << 16) | a;
}

/* Decode the zlib stream in[0..in_len) into out (at most out_cap bytes).
 * *out_len: bytes produced.  ST_OK only for a complete, well-formed stream
 * whose checksum matches. */
static int isofs_zlib_inflate(const uint8_t *in, uint64_t in_len, uint8_t *out,
			      uint64_t out_cap, uint64_t *out_len)
{
	isofs_inflate_t *z;
	int64_t sum;
	int r;

	might_sleep();
	*out_len = 0;
	if (in_len < 6)
		return ST_IO;
	/* CMF: method 8 (deflate), window <= 32 KB; FCHECK; no dictionary. */
	if ((in[0] & 0x0F) != 8 || (in[0] >> 4) > 7 ||
	    ((unsigned)in[0] * 256 + in[1]) % 31 != 0 || (in[1] & 0x20))
		return ST_IO;
	z = (isofs_inflate_t *)kalloc(sizeof(*z));
	if (!z)
		return ST_NOMEM;
	z->in = in + 2;
	z->in_len = in_len - 2;
	z->in_pos = 0;
	z->bits = 0;
	z->nbits = 0;
	z->out = out;
	z->out_len = 0;
	z->out_cap = out_cap;
	r = isofs_inflate_raw(z);
	if (r == ST_OK) {
		/* Adler-32, big-endian, after the byte boundary. */
		z->bits >>= z->nbits & 7;
		z->nbits -= z->nbits & 7;
		sum = 0;
		for (unsigned i = 0; i < 4 && r == ST_OK; i++) {
			int64_t b = isofs_inf_bits(z, 8);

			if (b < 0)
				r = ST_IO;
			else
				sum = (sum << 8) | b;
		}
		if (r == ST_OK &&
		    (uint32_t)sum != isofs_adler32(out, z->out_len))
			r = ST_IO;
	}
	*out_len = z->out_len;
	kfree(z);
	return r;
}

/* ===========================================================================
 * zisofs
 * ===========================================================================
 *
 * A zisofs file (RRIP "ZF" entry, algorithm "pz") is recorded as
 *
 *     header  magic[8] usize[4 LE] header_size/4[1] block_log[1] 0[2]
 *     table   (nblocks + 1) block pointers [4 LE each], byte offsets in
 *             the recorded file
 *     data    block i is the zlib stream from ptr[i] to ptr[i + 1]; an
 *             empty one (ptr[i] == ptr[i + 1]) is a block of zeros
 *
 * with nblocks = ceil(usize / 2^block_log).  The stream is NOFILL: a read
 * that misses the cache decodes the whole block holding its page and
 * installs all of the block's pages (isofs.h section 2). */

/* Check the header against the ZF entry and the recorded size, and fill in
 * the stream's zisofs fields. */
static int isofs_zf_validate(isofs_fs_t *fs, isofs_stream_t *s,
			     const isofs_node_t *n)
{
	uint8_t h[ISOFS_ZF_HEADER_MIN];
	uint64_t bs, nblk, table_end;
	int r;

	if (n->zf_blog < ISOFS_ZF_MIN_LOG || n->zf_blog > ISOFS_ZF_MAX_LOG)
		return ISOFS_CORRUPT(fs, "zisofs block size 2^%u",
				     n->zf_blog);
	if (n->zf_header < ISOFS_ZF_HEADER_MIN)
		return ISOFS_CORRUPT(fs, "zisofs header of %u bytes",
				     n->zf_header);
	bs = 1ULL << n->zf_blog;
	nblk = isofs_div_up(n->zf_usize, bs);
	table_end = n->zf_header + (nblk + 1) * 4;
	if (table_end > s->raw_size)
		return ISOFS_CORRUPT(fs, "zisofs file shorter than its "
					 "block table");
	r = isofs_raw_read(fs, s->ext, s->nextents, 0, h, sizeof(h));
	if (r != ST_OK)
		return r;
	if (!isofs_memeq(h, isofs_zf_magic, 8) ||
	    isofs_le32(h + 8) != (uint32_t)n->zf_usize ||
	    (unsigned)h[12] * 4 != n->zf_header || h[13] != n->zf_blog)
		return ISOFS_CORRUPT(fs, "zisofs header does not match its "
					 "ZF entry");
	s->zf_block_log = n->zf_blog;
	s->zf_header = n->zf_header;
	s->zf_nblocks = nblk;
	s->size = n->zf_usize;
	s->nblocks = isofs_div_up(s->size, fs->lbs);
	return ST_OK;
}

/* Decode zisofs block `zb' of `s' and install its pages.  Returns the page
 * `want' (which must lie in the block), held; NULL with *err set. */
static pc_page_t *isofs_zf_fill(isofs_fs_t *fs, isofs_stream_t *s,
				unsigned long want, int *err)
{
	uint64_t bs = 1ULL << s->zf_block_log;
	uint64_t zb = ((uint64_t)want * PAGE_SIZE) >> s->zf_block_log;
	uint64_t start = zb * bs, blen, csize, got = 0;
	uint8_t ptrs[8];
	uint32_t p0, p1;
	uint8_t *out = NULL, *in = NULL;
	pc_page_t *res = NULL;
	unsigned long first_page, npages;
	int r;

	*err = ST_IO;
	if (zb >= s->zf_nblocks || start >= s->size) {
		WARN_ON_ONCE(1);
		return NULL;
	}
	blen = isofs_min64(bs, s->size - start);
	r = isofs_raw_read(fs, s->ext, s->nextents, s->zf_header + zb * 4, ptrs,
			   8);
	if (r != ST_OK) {
		*err = r;
		return NULL;
	}
	p0 = isofs_le32(ptrs);
	p1 = isofs_le32(ptrs + 4);
	if (p0 < s->zf_header + (s->zf_nblocks + 1) * 4 || p1 < p0 ||
	    p1 > s->raw_size) {
		*err = ISOFS_CORRUPT(fs, "zisofs block %lu pointers %u..%u "
					 "outside the file",
				     (unsigned long)zb, p0, p1);
		return NULL;
	}
	csize = p1 - p0;
	if (csize > ISOFS_ZF_MAX_CSIZE(bs)) {
		*err = ISOFS_CORRUPT(fs, "zisofs block %lu of %lu bytes",
				     (unsigned long)zb, (unsigned long)csize);
		return NULL;
	}
	out = (uint8_t *)kalloc(bs);
	if (!out) {
		*err = ST_NOMEM;
		return NULL;
	}
	if (csize == 0) {
		mm_memset(out, 0, blen);
		got = blen;
	} else {
		in = (uint8_t *)kalloc(csize);
		if (!in) {
			kfree(out);
			*err = ST_NOMEM;
			return NULL;
		}
		r = isofs_raw_read(fs, s->ext, s->nextents, p0, in, csize);
		if (r == ST_OK)
			r = isofs_zlib_inflate(in, csize, out, blen, &got);
		kfree(in);
		if (r == ST_OK && got != blen)
			r = ST_IO;
		if (r != ST_OK) {
			kfree(out);
			*err = r == ST_NOMEM ? ST_NOMEM :
					       ISOFS_CORRUPT(fs, "zisofs block "
								 "%lu does not "
								 "decompress",
							     (unsigned long)zb);
			return NULL;
		}
	}
	first_page = (unsigned long)(start / PAGE_SIZE);
	npages = (unsigned long)isofs_div_up(blen, PAGE_SIZE);
	for (unsigned long k = 0; k < npages; k++) {
		unsigned long pi = first_page + k;
		uint64_t plen = isofs_min64(PAGE_SIZE, blen - k * PAGE_SIZE);
		pc_page_t *pg = pagecache_install(&fs->sb, s->chain, pi,
						  out + k * PAGE_SIZE,
						  (unsigned long)plen,
						  pi == want);

		if (pi == want)
			res = pg;
	}
	kfree(out);
	if (!res)
		*err = ST_NOMEM;
	return res;
}

/* A page of a stream that is not block-aligned: its bytes read one by one
 * from the extents and installed. */
static pc_page_t *isofs_unaligned_fill(isofs_fs_t *fs, isofs_stream_t *s,
				       unsigned long pi, int *err)
{
	uint64_t start = (uint64_t)pi * PAGE_SIZE;
	uint64_t len;
	uint8_t *buf;
	pc_page_t *pg;
	int r;

	if (start >= s->size) {
		*err = ST_IO;
		return NULL;
	}
	len = isofs_min64(PAGE_SIZE, s->size - start);
	buf = (uint8_t *)kalloc(PAGE_SIZE);
	if (!buf) {
		*err = ST_NOMEM;
		return NULL;
	}
	r = isofs_raw_read(fs, s->ext, s->nextents, start, buf, len);
	if (r != ST_OK) {
		kfree(buf);
		*err = r;
		return NULL;
	}
	pg = pagecache_install(&fs->sb, s->chain, pi, buf, (unsigned long)len,
			       1);
	kfree(buf);
	if (!pg)
		*err = ST_NOMEM;
	return pg;
}

/* Page `pi' of stream `s', held: from the cache, from the device through
 * the cache, or decoded by the driver (NOFILL).  NULL with *err set. */
static pc_page_t *isofs_page(isofs_fs_t *fs, isofs_stream_t *s,
			     unsigned long pi, int *err)
{
	pc_page_t *pg;

	might_sleep();
	*err = ST_IO;
	if (!(s->flags & ISOFS_SF_NOFILL)) {
		pg = pagecache_get_held(s->chain, pi, (unsigned long)s->size,
					&fs->sb, s->chain);
		if (pg && !pg->data) {
			pagecache_put(pg);
			pg = NULL;
		}
		return pg;
	}
	pg = pagecache_lookup_held(&fs->sb, s->chain, pi);
	if (pg)
		return pg;
	if (s->flags & ISOFS_SF_SPARSE) {
		/* Not corruption: a layout no mastering program writes and
		 * this driver does not decode.  Said once per mount. */
		if (!__atomic_exchange_n(&fs->sf_logged, 1, __ATOMIC_RELAXED))
			kprintf("iso9660: %s: Rock Ridge sparse files (SF) "
				"cannot be read\n",
				fs->bdev->name ? fs->bdev->name : "?");
		*err = ST_IO;
		return NULL;
	}
	if (s->flags & ISOFS_SF_ZISOFS)
		return isofs_zf_fill(fs, s, pi, err);
	return isofs_unaligned_fill(fs, s, pi, err);
}

/* Read `bytes' at `pos' of an open regular file into `buf' (a user
 * buffer, or a kernel one for page-ins).  No filesystem lock is held: the
 * stream is the handle's and immutable, and the page cache takes the
 * mapping lock around its own mapping calls. */
static long isofs_read_stream(isofs_file_t *ff, void *buf, long bytes,
			      uint64_t pos, pc_readahead_t *ra)
{
	isofs_fs_t *fs = ff->fs;
	isofs_stream_t *s = ff->st;
	uint64_t copied = 0, remaining;
	int err = ST_IO;

	if (ff->is_dir)
		return ST_ISDIR;
	if (!s || !buf || bytes < 0)
		return ST_INVALID;
	if (bytes == 0 || pos >= s->size)
		return 0;
	remaining = isofs_min64((uint64_t)bytes, s->size - pos);
	while (remaining) {
		unsigned long pi = (unsigned long)(pos / PAGE_SIZE);
		unsigned po = (unsigned)(pos % PAGE_SIZE);
		uint64_t chunk = isofs_min64(PAGE_SIZE - po, remaining);
		pc_page_t *pg = isofs_page(fs, s, pi, &err);

		if (!pg)
			return copied ? (long)copied : err;
		smap_disable();
		mm_memcpy((uint8_t *)buf + copied, pg->data + po, chunk);
		smap_enable();
		pagecache_put(pg);
		if (ra && !(s->flags & ISOFS_SF_NOFILL))
			pagecache_readahead(ra, s->chain, pi,
					    (unsigned long)s->size, &fs->sb,
					    s->chain);
		pos += chunk;
		copied += chunk;
		remaining -= chunk;
	}
	return (long)copied;
}

/* ===========================================================================
 * VFS operations
 * ===========================================================================
 *
 * Locking (isofs.h section 4): path operations hold the metadata lock
 * SHARED across the walk and the stat (the options a stat reports change
 * only under it exclusive); reads hold no filesystem lock at all -- the
 * handle references its stream, which never changes, and the page cache
 * takes lock_map around its own mapping calls.  At the end of an operation
 * that may have made streams, unused ones beyond the keep limit are evicted
 * (with no lock of ours held, so the exclusive acquisition cannot
 * deadlock). */

static const vfs_ops_t isofs_vfs_ops;

/* Resolve `path' under the shared lock into a work area the caller frees
 * with isofs_op_end().  *wout is set even on failure (NULL only when out
 * of memory). */
static int isofs_resolve(isofs_fs_t *fs, vfs_mount_t *m, const char *path,
			 int follow, isofs_work_t **wout, uint64_t *parent)
{
	isofs_work_t *w = isofs_work_alloc(fs, m);
	int r;

	*wout = w;
	if (!w)
		return ST_NOMEM;
	isofs_meta_rlock(fs);
	r = isofs_walk(fs, w, path, follow, parent);
	isofs_meta_runlock(fs);
	return r;
}

static void isofs_op_end(isofs_fs_t *fs, isofs_work_t *w)
{
	isofs_work_free(w);
	isofs_stream_trim(fs, ISOFS_STREAM_KEEP, 1);
}

static uint8_t isofs_dtype(uint32_t mode)
{
	switch (mode & RR_S_IFMT) {
	case RR_S_IFDIR:
		return DT_DIR;
	case RR_S_IFREG:
		return DT_REG;
	case RR_S_IFLNK:
		return DT_LNK;
	case RR_S_IFCHR:
		return DT_CHR;
	case RR_S_IFBLK:
		return DT_BLK;
	case RR_S_IFIFO:
		return DT_FIFO;
	case RR_S_IFSOCK:
		return DT_SOCK;
	default:
		return DT_UNKNOWN;
	}
}

/* ---- open / close ------------------------------------------------------ */

static int isofs_open_common(vfs_mount_t *m, const char *path, int flags,
			     vfs_file_t **out)
{
	isofs_fs_t *fs = isofs_fs_of(m);
	isofs_work_t *w = NULL;
	isofs_file_t *ff = NULL;
	isofs_stream_t *st = NULL;
	uint64_t parent = 0;
	int acc, r;

	if (!fs || !path || !out)
		return ST_INVALID;
	*out = NULL;
	acc = flags & 3;
	r = isofs_resolve(fs, m, path, !(flags & O_NOFOLLOW), &w, &parent);
	if (r == ST_NOT_FOUND && (flags & O_CREAT))
		r = ST_ROFS; /* nothing can be created here */
	if (r != ST_OK)
		goto out;
	if ((flags & O_CREAT) && (flags & O_EXCL)) {
		r = ST_EXISTS;
		goto out;
	}
	if (acc == 1 || acc == 2 || (flags & O_TRUNC)) {
		r = (w->node.flags & ISOFS_NF_DIR) && acc ? ST_ISDIR : ST_ROFS;
		goto out;
	}
	if (w->node.flags & ISOFS_NF_LINK) {
		/* Only reached with O_NOFOLLOW. */
		r = ST_LOOP;
		goto out;
	}
	if ((flags & O_DIRECTORY) && !(w->node.flags & ISOFS_NF_DIR)) {
		r = ST_NOTDIR;
		goto out;
	}

	ff = (isofs_file_t *)kalloc(sizeof(*ff));
	if (!ff) {
		r = ST_NOMEM;
		goto out;
	}
	mm_memset(ff, 0, sizeof(*ff));
	spinlock_init(&ff->pos_lock, "isofs_pos");

	if ((w->node.mode & RR_S_IFMT) == RR_S_IFREG ||
	    (w->node.flags & ISOFS_NF_DIR)) {
		isofs_meta_rlock(fs);
		r = isofs_stream_of_node(fs, &w->node, &st);
		isofs_meta_runlock(fs);
		if (r != ST_OK)
			goto out;
	}
	ff->fs = fs;
	ff->st = st;
	ff->is_dir = (w->node.flags & ISOFS_NF_DIR) != 0;
	ff->parent_ino = parent;
	ff->pos = 0;
	ff->ra.last_page_index = (unsigned long)-1;
	ff->ra.sequential_count = 0;
	ff->ra.ra_pages = 0;
	isofs_fill_kstat(fs, &w->node, &ff->kst);
	if (st && !ff->is_dir)
		/* The inode cache's entry for the open file, keyed by the
		 * stream's chain id like its pages; dropped at close, and
		 * removed when the stream is evicted. */
		ff->ic = icache_get(&fs->sb, st->chain, (unsigned long)st->size,
				    ff->kst.st_mode, parent, 0, 0, 0, 0);
	ff->vfs.ops = &isofs_vfs_ops;
	ff->vfs.fs_private = ff;
	*out = &ff->vfs;
	ff = NULL;
	st = NULL;
	r = ST_OK;
out:
	if (st)
		isofs_stream_put(fs, st);
	if (ff)
		kfree(ff);
	isofs_op_end(fs, w);
	return r;
}

static int isofs_open_op(vfs_mount_t *m, const char *path, int flags,
			 vfs_file_t **out)
{
	return isofs_open_common(m, path, flags, out);
}

static int isofs_open_mode_op(vfs_mount_t *m, const char *path, int flags,
			      unsigned int mode, vfs_file_t **out)
{
	(void)mode;
	return isofs_open_common(m, path, flags, out);
}

static int isofs_close_op(vfs_file_t *f)
{
	isofs_file_t *ff = isofs_file_of(f);
	isofs_fs_t *fs;

	if (!ff)
		return ST_INVALID;
	fs = ff->fs;
	if (ff->ic)
		icache_unref(ff->ic);
	if (ff->st)
		isofs_stream_put(fs, ff->st);
	kfree(ff);
	isofs_stream_trim(fs, ISOFS_STREAM_KEEP, 1);
	return ST_OK;
}

/* ---- reading ------------------------------------------------------------ */

static long isofs_read_op(vfs_file_t *f, void *buf, long bytes)
{
	isofs_file_t *ff = isofs_file_of(f);
	pc_readahead_t ra;
	uint64_t pos, flags;
	long r;

	if (!ff)
		return ST_INVALID;
	if (ff->is_dir)
		return ST_ISDIR;
	spin_lock_irqsave(&ff->pos_lock, &flags);
	pos = ff->pos;
	ra = ff->ra;
	spin_unlock_irqrestore(&ff->pos_lock, flags);
	r = isofs_read_stream(ff, buf, bytes, pos, &ra);
	if (r > 0) {
		spin_lock_irqsave(&ff->pos_lock, &flags);
		ff->pos = pos + (uint64_t)r;
		ff->ra = ra;
		spin_unlock_irqrestore(&ff->pos_lock, flags);
	}
	return r;
}

/* Positional read (pread, page-ins): its own readahead state, the
 * handle's position untouched. */
static long isofs_read_at_op(vfs_file_t *f, void *buf, long bytes, long off)
{
	isofs_file_t *ff = isofs_file_of(f);
	pc_readahead_t ra;

	if (!ff || off < 0)
		return ST_INVALID;
	if (ff->is_dir)
		return ST_ISDIR;
	ra.last_page_index = (unsigned long)-1;
	ra.sequential_count = 0;
	ra.ra_pages = 0;
	return isofs_read_stream(ff, buf, bytes, (uint64_t)off, &ra);
}

static long isofs_seek_op(vfs_file_t *f, long offset, int whence)
{
	isofs_file_t *ff = isofs_file_of(f);
	uint64_t flags;
	long base, np;

	if (!ff)
		return ST_INVALID;
	spin_lock_irqsave(&ff->pos_lock, &flags);
	switch (whence) {
	case 0: /* SEEK_SET */
		base = 0;
		break;
	case 1: /* SEEK_CUR */
		base = (long)ff->pos;
		break;
	case 2: /* SEEK_END */
		base = (long)ff->kst.st_size;
		break;
	default:
		spin_unlock_irqrestore(&ff->pos_lock, flags);
		return ST_INVALID;
	}
	if (__builtin_add_overflow(base, offset, &np) || np < 0) {
		spin_unlock_irqrestore(&ff->pos_lock, flags);
		return ST_INVALID;
	}
	ff->pos = (uint64_t)np;
	spin_unlock_irqrestore(&ff->pos_lock, flags);
	return np;
}

/* ---- readdir ------------------------------------------------------------ */

/* Most bytes of directory entries one readdir call assembles (in a kernel
 * buffer, under the metadata lock; copied to the caller afterwards). */
#define ISOFS_READDIR_BUF 32768

/* One dirent64 into the kernel buffer `kb' (cap bytes).  0 when it does
 * not fit. */
static int isofs_put_dirent(uint8_t *kb, long cap, long *used, uint64_t ino,
			    uint64_t next, uint8_t type, const char *name,
			    unsigned nlen)
{
	unsigned reclen = (19 + nlen + 1 + 7) & ~7u;
	struct dirent64 *d;

	if (*used + (long)reclen > cap)
		return 0;
	d = (struct dirent64 *)(kb + *used);
	mm_memset(d, 0, reclen);
	d->d_ino = ino;
	d->d_off = (int64_t)next;
	d->d_reclen = (uint16_t)reclen;
	d->d_type = type;
	mm_memcpy(d->d_name, name, nlen);
	d->d_name[nlen] = '\0';
	*used += reclen;
	return 1;
}

/* The first record-set start at or after directory offset `pos' (the
 * directory's size when there is none), walking from the start exactly as
 * readdir does: a multi-extent file's continuation records belong to its
 * first record.  For positions that did not come from readdir itself. */
static int isofs_dir_resync(isofs_fs_t *fs, isofs_stream_t *ds, uint64_t pos,
			    uint64_t *out)
{
	isofs_dirit_t it;
	int r;

	isofs_dirit_init(&it, fs, ds, 0);
	for (;;) {
		const uint8_t *rec;
		uint64_t doff;
		unsigned fi;
		uint8_t id[256];

		r = isofs_dirit_next(&it, &rec, &doff);
		if (r == ST_EOF) {
			*out = ds->size;
			r = ST_OK;
			break;
		}
		if (r != ST_OK)
			break;
		if (doff >= pos) {
			*out = doff;
			break;
		}
		fi = rec[ISO_DR_NAMELEN];
		if (!(rec[ISO_DR_FLAGS] & ISO_FLAG_MULTI) ||
		    (rec[ISO_DR_FLAGS] & ISO_FLAG_DIR) ||
		    (fi == 1 && rec[ISO_DR_NAME] <= 1))
			continue;
		/* Consume the continuation records as isofs_read_set does
		 * (it stops after a directory record, after a name that
		 * differs under check=strict, or at the last extent). */
		mm_memcpy(id, rec + ISO_DR_NAME, fi);
		for (;;) {
			const uint8_t *c;
			uint64_t cdoff;

			r = isofs_dirit_next(&it, &c, &cdoff);
			if (r != ST_OK)
				break;
			if (c[ISO_DR_FLAGS] & ISO_FLAG_DIR)
				break;
			if (fs->o.strict && (c[ISO_DR_NAMELEN] != fi ||
					     !isofs_memeq(c + ISO_DR_NAME, id, fi)))
				break;
			if (!(c[ISO_DR_FLAGS] & ISO_FLAG_MULTI))
				break;
		}
		if (r == ST_EOF)
			continue; /* the next call ends the walk */
		if (r != ST_OK)
			break;
	}
	isofs_dirit_done(&it);
	return r;
}

static long isofs_readdir_op(vfs_file_t *f, void *buf, long bytes)
{
	isofs_file_t *ff = isofs_file_of(f);
	isofs_fs_t *fs;
	isofs_work_t *w;
	isofs_dirit_t it;
	uint8_t *kb;
	uint64_t pos, cookie, flags;
	long used = 0, cap, result;
	int r = ST_OK;

	if (!ff)
		return ST_INVALID;
	if (!ff->is_dir || !ff->st)
		return ST_NOTDIR;
	if (!buf || bytes <= 0)
		return ST_INVALID;
	fs = ff->fs;
	cap = bytes < ISOFS_READDIR_BUF ? bytes : ISOFS_READDIR_BUF;
	w = isofs_work_alloc(fs, fs->mnt);
	kb = (uint8_t *)kalloc((unsigned long)cap);
	if (!w || !kb) {
		isofs_work_free(w);
		kfree(kb);
		return ST_NOMEM;
	}
	spin_lock_irqsave(&ff->pos_lock, &flags);
	pos = ff->pos;
	cookie = ff->dir_cookie;
	spin_unlock_irqrestore(&ff->pos_lock, flags);

	isofs_meta_rlock(fs);
	/* Only a position this driver handed out is parsed as it is. */
	if (pos != 0 && pos != cookie && pos < ff->st->size) {
		r = isofs_dir_resync(fs, ff->st, pos, &pos);
		if (r != ST_OK) {
			/* Position and cookie stay as they were. */
			isofs_meta_runlock(fs);
			kfree(kb);
			isofs_op_end(fs, w);
			return r;
		}
	}
	if (pos > ff->st->size)
		pos = ff->st->size;
	isofs_su_scan_begin(&w->su);
	isofs_dirit_init(&it, fs, ff->st, pos);
	while (r == ST_OK) {
		const uint8_t *rec;
		uint64_t doff, ino;
		const char *name;
		unsigned nlen, fi;
		uint8_t type;

		r = isofs_dirit_next(&it, &rec, &doff);
		if (r != ST_OK)
			break;
		fi = rec[ISO_DR_NAMELEN];
		if (fi == 1 && rec[ISO_DR_NAME] == 0) {
			name = ".";
			nlen = 1;
			ino = ff->st->key;
			type = DT_DIR;
		} else if (fi == 1 && rec[ISO_DR_NAME] == 1) {
			name = "..";
			nlen = 2;
			ino = ff->parent_ino;
			type = DT_DIR;
		} else {
			r = isofs_read_set(&it, &w->su, rec, doff, 0, 1, 0,
					   &w->tmp);
			if (w->su.over_budget) {
				r = ISOFS_CORRUPT(fs, "directory %lu: System Use "
							  "data beyond the work "
							  "budget of a scan",
						      (unsigned long)ff->st->key);
				break;
			}
			if (r == ST_NOMEM)
				break;
			if (r != ST_OK || isofs_node_invisible(fs, &w->tmp) ||
			    w->tmp.ino == ff->st->key) {
				/* Skipped: unreadable, hidden, or a record
				 * naming this very directory (a loop). */
				pos = it.off;
				r = ST_OK;
				continue;
			}
			name = w->tmp.name;
			nlen = w->tmp.name_len;
			ino = w->tmp.ino;
			type = (w->tmp.flags & ISOFS_NF_DIR) ?
				       DT_DIR :
				       isofs_dtype(w->tmp.mode);
		}
		if (!isofs_put_dirent(kb, cap, &used, ino, it.off, type, name,
				      nlen))
			break; /* full: this entry comes first next time */
		pos = it.off;
		if (name == w->tmp.name && !(w->tmp.flags & ISOFS_NF_DAMAGED)) {
			/* A listing is usually followed by a stat of every
			 * name in it: remember them, so that each one is not
			 * another scan of the directory -- but never over
			 * what a lookup already decided (with duplicate names
			 * the lookup's first match stands). */
			dc_result_t res;

			isofs_make_key(name, nlen, w->key);
			if (dcache_lookup_cs(&fs->sb,
					     ISOFS_DC_PARENT(ff->st->key),
					     w->key, &res) == DC_LOOKUP_MISS)
				dcache_insert_cs(&fs->sb,
						 ISOFS_DC_PARENT(ff->st->key),
						 w->key, ino, w->tmp.rec_doff,
						 isofs_node_kind(&w->tmp));
		}
	}
	isofs_dirit_done(&it);
	isofs_meta_runlock(fs);

	/* User memory only now, with no lock of ours held. */
	if (used > 0) {
		smap_disable();
		mm_memcpy(buf, kb, (unsigned long)used);
		smap_enable();
	}
	kfree(kb);

	if (r == ST_EOF)
		pos = it.size;
	spin_lock_irqsave(&ff->pos_lock, &flags);
	ff->pos = pos;
	ff->dir_cookie = pos;
	spin_unlock_irqrestore(&ff->pos_lock, flags);
	if (used > 0)
		result = used;
	else if (r == ST_OK)
		result = ST_INVALID; /* the buffer cannot hold one entry */
	else if (r == ST_EOF)
		result = 0;
	else
		result = r;
	isofs_op_end(fs, w);
	return result;
}

/* ---- stat and friends --------------------------------------------------- */

static int isofs_stat_common(vfs_mount_t *m, const char *path,
			     struct kstat *st, int follow)
{
	isofs_fs_t *fs = isofs_fs_of(m);
	isofs_work_t *w;
	int r;

	if (!fs || !path || !st)
		return ST_INVALID;
	r = isofs_resolve(fs, m, path, follow, &w, NULL);
	if (r == ST_OK) {
		isofs_meta_rlock(fs);
		isofs_fill_kstat(fs, &w->node, st);
		isofs_meta_runlock(fs);
	}
	isofs_op_end(fs, w);
	return r;
}

static int isofs_stat_op(vfs_mount_t *m, const char *path, struct kstat *st)
{
	return isofs_stat_common(m, path, st, 1);
}

static int isofs_lstat_op(vfs_mount_t *m, const char *path, struct kstat *st)
{
	return isofs_stat_common(m, path, st, 0);
}

static int isofs_fstat_op(vfs_file_t *f, struct kstat *st)
{
	isofs_file_t *ff = isofs_file_of(f);

	if (!ff || !st)
		return ST_INVALID;
	*st = ff->kst;
	return ST_OK;
}

static int isofs_readlink_op(vfs_mount_t *m, const char *path, char *buf,
			     unsigned long bufsz)
{
	isofs_fs_t *fs = isofs_fs_of(m);
	isofs_work_t *w;
	unsigned long n;
	int r;

	if (!fs || !path || !buf || bufsz == 0)
		return ST_INVALID;
	r = isofs_resolve(fs, m, path, 0, &w, NULL);
	if (r == ST_OK && !(w->node.flags & ISOFS_NF_LINK))
		r = ST_INVALID;
	if (r == ST_OK && w->link_bad)
		r = ISOFS_CORRUPT(fs, "unreadable symbolic link target");
	if (r == ST_OK) {
		n = w->link_len;
		if (n > bufsz)
			n = bufsz; /* readlink truncates, no NUL */
		mm_memcpy(buf, w->link, n);
		r = (int)n;
	}
	isofs_op_end(fs, w);
	return r;
}

static int isofs_chdir_op(vfs_mount_t *m, const char *path)
{
	isofs_fs_t *fs = isofs_fs_of(m);
	isofs_work_t *w;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	r = isofs_resolve(fs, m, path, 1, &w, NULL);
	if (r == ST_OK && !(w->node.flags & ISOFS_NF_DIR))
		r = ST_NOTDIR;
	isofs_op_end(fs, w);
	return r;
}

static int isofs_statfs_op(vfs_mount_t *m, struct vfs_statfs *out)
{
	isofs_fs_t *fs = isofs_fs_of(m);

	if (!fs || !out)
		return ST_INVALID;
	mm_memset(out, 0, sizeof(*out));
	out->f_type = ISOFS_SUPER_MAGIC;
	out->f_bsize = fs->lbs;
	out->f_frsize = fs->lbs;
	out->f_blocks = fs->vol_blocks;
	out->f_bfree = 0;
	out->f_bavail = 0;
	out->f_files = 0;
	out->f_ffree = 0;
	out->f_fsid = 0;
	out->f_namelen = ISOFS_NAME_MAX;
	return ST_OK;
}

/* ---- extended attributes: there are none --------------------------------
 *
 * The object must exist (ENOENT otherwise); every attribute is absent
 * (ENODATA) and every list empty.  Setting or removing one is EROFS. */

static int isofs_getxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			     const char *name, void *val, unsigned long size)
{
	isofs_fs_t *fs = isofs_fs_of(m);
	isofs_work_t *w;
	int r;

	(void)name;
	(void)val;
	(void)size;
	if (!fs || !path)
		return ST_INVALID;
	r = isofs_resolve(fs, m, path, !nofollow, &w, NULL);
	isofs_op_end(fs, w);
	return r == ST_OK ? ST_NODATA : r;
}

static int isofs_listxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			      char *list, unsigned long size)
{
	isofs_fs_t *fs = isofs_fs_of(m);
	isofs_work_t *w;
	int r;

	(void)list;
	(void)size;
	if (!fs || !path)
		return ST_INVALID;
	r = isofs_resolve(fs, m, path, !nofollow, &w, NULL);
	isofs_op_end(fs, w);
	return r == ST_OK ? 0 : r;
}

static int isofs_fgetxattr_op(vfs_file_t *f, const char *name, void *val,
			      unsigned long size)
{
	(void)name;
	(void)val;
	(void)size;
	return isofs_file_of(f) ? ST_NODATA : ST_INVALID;
}

static int isofs_flistxattr_op(vfs_file_t *f, char *list, unsigned long size)
{
	(void)list;
	(void)size;
	return isofs_file_of(f) ? 0 : ST_INVALID;
}

static int isofs_getxattr_ino_op(vfs_mount_t *m, unsigned long ino,
				 const char *name, void *val,
				 unsigned long size)
{
	(void)ino;
	(void)name;
	(void)val;
	(void)size;
	return isofs_fs_of(m) ? ST_NODATA : ST_INVALID;
}

/* ---- everything that would change the volume: EROFS -------------------- */

static long isofs_write_op(vfs_file_t *f, const void *buf, long bytes)
{
	(void)f;
	(void)buf;
	(void)bytes;
	return ST_ROFS;
}

static long isofs_write_at_op(vfs_file_t *f, const void *buf, long bytes,
			      long off)
{
	(void)f;
	(void)buf;
	(void)bytes;
	(void)off;
	return ST_ROFS;
}

static int isofs_truncate_op(vfs_file_t *f, unsigned long size)
{
	(void)f;
	(void)size;
	return ST_ROFS;
}

static int isofs_unlink_op(vfs_mount_t *m, const char *path)
{
	(void)m;
	(void)path;
	return ST_ROFS;
}

static int isofs_rename_op(vfs_mount_t *m, const char *o, const char *n)
{
	(void)m;
	(void)o;
	(void)n;
	return ST_ROFS;
}

static int isofs_mkdir_op(vfs_mount_t *m, const char *path, unsigned int mode)
{
	(void)m;
	(void)path;
	(void)mode;
	return ST_ROFS;
}

static int isofs_rmdir_op(vfs_mount_t *m, const char *path)
{
	(void)m;
	(void)path;
	return ST_ROFS;
}

static int isofs_symlink_op(vfs_mount_t *m, const char *target,
			    const char *linkpath)
{
	(void)m;
	(void)target;
	(void)linkpath;
	return ST_ROFS;
}

static int isofs_link_op(vfs_mount_t *m, const char *o, const char *n,
			 int follow)
{
	(void)m;
	(void)o;
	(void)n;
	(void)follow;
	return ST_ROFS;
}

static int isofs_chmod_op(vfs_mount_t *m, const char *path, unsigned int mode)
{
	(void)m;
	(void)path;
	(void)mode;
	return ST_ROFS;
}

static int isofs_chown_op(vfs_mount_t *m, const char *path, int uid, int gid,
			  int nofollow)
{
	(void)m;
	(void)path;
	(void)uid;
	(void)gid;
	(void)nofollow;
	return ST_ROFS;
}

static int isofs_fchmod_op(vfs_file_t *f, unsigned int mode)
{
	(void)f;
	(void)mode;
	return ST_ROFS;
}

static int isofs_fchown_op(vfs_file_t *f, int uid, int gid)
{
	(void)f;
	(void)uid;
	(void)gid;
	return ST_ROFS;
}

static int isofs_utimens_op(vfs_mount_t *m, const char *path, int nofollow,
			    const struct vfs_utimes *t)
{
	(void)m;
	(void)path;
	(void)nofollow;
	(void)t;
	return ST_ROFS;
}

static int isofs_futimens_op(vfs_file_t *f, const struct vfs_utimes *t)
{
	(void)f;
	(void)t;
	return ST_ROFS;
}

static int isofs_mknod_op(vfs_mount_t *m, const char *path, unsigned int mode,
			  uint64_t dev)
{
	(void)m;
	(void)path;
	(void)mode;
	(void)dev;
	return ST_ROFS;
}

static int isofs_setxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			     const char *name, const void *val,
			     unsigned long size, int flags)
{
	(void)m;
	(void)path;
	(void)nofollow;
	(void)name;
	(void)val;
	(void)size;
	(void)flags;
	return ST_ROFS;
}

static int isofs_removexattr_op(vfs_mount_t *m, const char *path, int nofollow,
				const char *name)
{
	(void)m;
	(void)path;
	(void)nofollow;
	(void)name;
	return ST_ROFS;
}

static int isofs_fsetxattr_op(vfs_file_t *f, const char *name, const void *val,
			      unsigned long size, int flags)
{
	(void)f;
	(void)name;
	(void)val;
	(void)size;
	(void)flags;
	return ST_ROFS;
}

static int isofs_fremovexattr_op(vfs_file_t *f, const char *name)
{
	(void)f;
	(void)name;
	return ST_ROFS;
}

/* ---- the rest ------------------------------------------------------------ */

/* Nothing is ever pending. */
static int isofs_fsync_op(vfs_file_t *f)
{
	return isofs_file_of(f) ? ST_OK : ST_INVALID;
}

static int isofs_sync_op(vfs_mount_t *m)
{
	return isofs_fs_of(m) ? ST_OK : ST_INVALID;
}

/* A task killed inside the driver: its exclusive hold of the metadata
 * lock, and any lock it held in the device driver below. */
static int isofs_release_locks_op(vfs_mount_t *m, uint64_t task_id)
{
	isofs_fs_t *fs = m ? (isofs_fs_t *)m->fs_data : NULL;
	int released;

	if (!fs)
		return 0;
	released = isofs_rwsem_release_owner(&fs->meta, task_id);
	if (fs->bdev && fs->bdev->release_locks_for_task)
		released |= fs->bdev->release_locks_for_task(fs->bdev, task_id);
	return released;
}

static const vfs_ops_t isofs_vfs_ops = {
	.open = isofs_open_op,
	.open_mode = isofs_open_mode_op,
	.stat = isofs_stat_op,
	.lstat = isofs_lstat_op,
	.fstat = isofs_fstat_op,
	.read = isofs_read_op,
	.read_at = isofs_read_at_op,
	.write = isofs_write_op,
	.write_at = isofs_write_at_op,
	.seek = isofs_seek_op,
	.readdir = isofs_readdir_op,
	.truncate = isofs_truncate_op,
	.unlink = isofs_unlink_op,
	.rename = isofs_rename_op,
	.mkdir = isofs_mkdir_op,
	.rmdir = isofs_rmdir_op,
	.chdir = isofs_chdir_op,
	.close = isofs_close_op,
	.release_locks_for_task = isofs_release_locks_op,
	.fsync = isofs_fsync_op,
	.symlink = isofs_symlink_op,
	.readlink = isofs_readlink_op,
	.link = isofs_link_op,
	.chmod = isofs_chmod_op,
	.chown = isofs_chown_op,
	.fchmod = isofs_fchmod_op,
	.fchown = isofs_fchown_op,
	.utimens = isofs_utimens_op,
	.futimens = isofs_futimens_op,
	.statfs = isofs_statfs_op,
	.sync = isofs_sync_op,
	.getxattr = isofs_getxattr_op,
	.setxattr = isofs_setxattr_op,
	.listxattr = isofs_listxattr_op,
	.removexattr = isofs_removexattr_op,
	.fgetxattr = isofs_fgetxattr_op,
	.fsetxattr = isofs_fsetxattr_op,
	.flistxattr = isofs_flistxattr_op,
	.fremovexattr = isofs_fremovexattr_op,
	.getxattr_ino = isofs_getxattr_ino_op,
	.mknod = isofs_mknod_op,
	/* permission: NULL -- the generic mode check on what stat reports.
	 * inode_flags, setid_clean: NULL -- nothing to report or strip. */
};

/* ===========================================================================
 * Mount options
 * =========================================================================== */

static void isofs_opts_default(struct isofs_opts *o)
{
	mm_memset(o, 0, sizeof(*o));
	o->uid = 0;
	o->gid = 0;
	o->fmode = 0444;
	o->dmode = 0555;
	o->session = -1;
	o->sbsector = -1;
}

/* A number in `base' (10 or 8) up to `max'; 0 when `v' is not one. */
static int isofs_opt_number(const char *v, unsigned base, uint64_t max,
			    uint64_t *out)
{
	uint64_t n = 0;

	if (!v || !*v)
		return 0;
	for (; *v; v++) {
		unsigned d;

		if (*v < '0' || *v > '9')
			return 0;
		d = (unsigned)(*v - '0');
		if (d >= base)
			return 0;
		n = n * base + d;
		if (n > max)
			return 0;
	}
	*out = n;
	return 1;
}

static int isofs_opt_is(const char *a, const char *b)
{
	return a && kstrcmp(a, b) == 0;
}

/* Parse `data' over the values in `o'.  ST_OK, or ST_INVALID for a value
 * that is not one (`o' unchanged then).  The generic flags (ro, rw, nosuid,
 * ...) and unknown words are left to the VFS and ignored here. */
static int isofs_parse_options(const char *data, struct isofs_opts *o)
{
	struct isofs_opts n = *o;
	const char *cur = data, *key, *val;
	char tmp[VFS_MOUNT_OPTS_MAX];
	uint64_t v;

	while (data && vfs_next_option(&cur, tmp, sizeof(tmp), &key, &val)) {
		if (isofs_opt_is(key, "uid")) {
			if (!isofs_opt_number(val, 10, 0xFFFFFFFFu, &v))
				return ST_INVALID;
			n.uid = (uint32_t)v;
			n.have_uid = 1;
		} else if (isofs_opt_is(key, "gid")) {
			if (!isofs_opt_number(val, 10, 0xFFFFFFFFu, &v))
				return ST_INVALID;
			n.gid = (uint32_t)v;
			n.have_gid = 1;
		} else if (isofs_opt_is(key, "mode")) {
			if (!isofs_opt_number(val, 8, 07777, &v))
				return ST_INVALID;
			n.fmode = (uint32_t)v;
			n.have_mode = 1;
		} else if (isofs_opt_is(key, "dmode")) {
			if (!isofs_opt_number(val, 8, 07777, &v))
				return ST_INVALID;
			n.dmode = (uint32_t)v;
			n.have_dmode = 1;
		} else if (isofs_opt_is(key, "norock")) {
			n.norock = 1;
		} else if (isofs_opt_is(key, "nojoliet")) {
			n.nojoliet = 1;
		} else if (isofs_opt_is(key, "check")) {
			if (isofs_opt_is(val, "r") || isofs_opt_is(val, "relaxed"))
				n.strict = 0;
			else if (isofs_opt_is(val, "s") ||
				 isofs_opt_is(val, "strict"))
				n.strict = 1;
			else
				return ST_INVALID;
		} else if (isofs_opt_is(key, "map")) {
			if (isofs_opt_is(val, "o") || isofs_opt_is(val, "off"))
				n.map_off = 1;
			else if (isofs_opt_is(val, "n") ||
				 isofs_opt_is(val, "normal") ||
				 isofs_opt_is(val, "a") ||
				 isofs_opt_is(val, "acorn"))
				n.map_off = 0;
			else
				return ST_INVALID;
		} else if (isofs_opt_is(key, "block")) {
			if (!isofs_opt_number(val, 10, 2048, &v) ||
			    (v != 512 && v != 1024 && v != 2048))
				return ST_INVALID;
			n.block = (uint32_t)v;
		} else if (isofs_opt_is(key, "session")) {
			if (!isofs_opt_number(val, 10, 99, &v))
				return ST_INVALID;
			n.session = (int)v;
		} else if (isofs_opt_is(key, "sbsector")) {
			if (!isofs_opt_number(val, 10, 0xFFFFFFFFu, &v))
				return ST_INVALID;
			n.sbsector = (long)v;
		} else if (isofs_opt_is(key, "hide")) {
			n.hide = 1;
			n.unhide = 0;
		} else if (isofs_opt_is(key, "unhide") ||
			   isofs_opt_is(key, "showassoc")) {
			n.unhide = 1;
			n.hide = 0;
		} else if (isofs_opt_is(key, "cruft")) {
			n.cruft = 1;
		} else if (isofs_opt_is(key, "nocompress")) {
			n.nocompress = 1;
		} else if (isofs_opt_is(key, "overriderockperms")) {
			n.override_rr = 1;
		} else if (isofs_opt_is(key, "iocharset")) {
			/* Names are always UTF-8. */
			if (!val || (!isofs_opt_is(val, "utf8") &&
				     !isofs_opt_is(val, "utf-8")))
				return ST_INVALID;
		}
		/* utf8, ro, rw, nosuid, nodev, ...: nothing to do here. */
	}
	*o = n;
	return ST_OK;
}

/* "uid=..,gid=..,..." for /proc/mounts: the options that differ from the
 * defaults. */
static long isofs_fst_show_options(vfs_mount_t *m, char *buf, long cap)
{
	isofs_fs_t *fs = m ? (isofs_fs_t *)m->fs_data : NULL;
	const struct isofs_opts *o;
	long n = 0;

	if (!fs || !buf || cap < 1)
		return 0;
	o = &fs->o;
	buf[0] = '\0';
#define ISOFS_SHOW(...)                                                       \
	do {                                                                  \
		int k_ = ksnprintf(buf + n, (size_t)(cap - n), __VA_ARGS__); \
		if (k_ > 0)                                                   \
			n = (n + k_ >= cap) ? cap - 1 : n + k_;               \
	} while (0)
#define ISOFS_SEP() (n ? "," : "")
	if (o->have_uid || o->uid)
		ISOFS_SHOW("%suid=%u", ISOFS_SEP(), (unsigned)o->uid);
	if (o->have_gid || o->gid)
		ISOFS_SHOW("%sgid=%u", ISOFS_SEP(), (unsigned)o->gid);
	if (o->fmode != 0444)
		ISOFS_SHOW("%smode=%04o", ISOFS_SEP(), (unsigned)o->fmode);
	if (o->dmode != 0555)
		ISOFS_SHOW("%sdmode=%04o", ISOFS_SEP(), (unsigned)o->dmode);
	if (o->norock)
		ISOFS_SHOW("%snorock", ISOFS_SEP());
	if (o->nojoliet)
		ISOFS_SHOW("%snojoliet", ISOFS_SEP());
	if (o->strict)
		ISOFS_SHOW("%scheck=strict", ISOFS_SEP());
	if (o->map_off)
		ISOFS_SHOW("%smap=off", ISOFS_SEP());
	if (o->block)
		ISOFS_SHOW("%sblock=%u", ISOFS_SEP(), (unsigned)o->block);
	if (o->session >= 0)
		ISOFS_SHOW("%ssession=%d", ISOFS_SEP(), o->session);
	if (o->sbsector >= 0)
		ISOFS_SHOW("%ssbsector=%lu", ISOFS_SEP(),
			   (unsigned long)o->sbsector);
	if (o->hide)
		ISOFS_SHOW("%shide", ISOFS_SEP());
	if (o->unhide)
		ISOFS_SHOW("%sunhide", ISOFS_SEP());
	if (o->cruft)
		ISOFS_SHOW("%scruft", ISOFS_SEP());
	if (o->nocompress)
		ISOFS_SHOW("%snocompress", ISOFS_SEP());
	if (o->override_rr)
		ISOFS_SHOW("%soverriderockperms", ISOFS_SEP());
#undef ISOFS_SEP
#undef ISOFS_SHOW
	return n;
}

/* ===========================================================================
 * Volume descriptors and probing
 * =========================================================================== */

/* Is `vd' a volume descriptor at all (standard identifier "CD001")? */
static int isofs_vd_valid(const uint8_t *vd)
{
	return vd[1] == 'C' && vd[2] == 'D' && vd[3] == '0' && vd[4] == '0' &&
	       vd[5] == '1';
}

/* Is `vd' the primary volume descriptor? */
static int isofs_vd_primary(const uint8_t *vd)
{
	return vd[0] == ISO_VD_PRIMARY && isofs_vd_valid(vd) && vd[6] == 1;
}

/* The Joliet level of a supplementary descriptor (escape sequence %/@,
 * %/C or %/E), or 0. */
static int isofs_vd_joliet(const uint8_t *vd)
{
	const uint8_t *e = vd + ISO_VD_OFF_ESCAPES;

	if (vd[0] != ISO_VD_SUPPLEMENTARY || !isofs_vd_valid(vd) || vd[6] != 1)
		return 0;
	if (e[0] != '%' || e[1] != '/')
		return 0;
	switch (e[2]) {
	case '@':
		return 1;
	case 'C':
		return 2;
	case 'E':
		return 3;
	default:
		return 0;
	}
}

/* Walk the volume descriptor set from byte 32768 for the primary volume
 * descriptor: at most ISOFS_PROBE_MAX_VD descriptors, one bounded read each,
 * stopping at the set terminator or anything that is not a descriptor. */
static int isofs_probe_vds(struct block_device *bdev)
{
	const block_device_t *b = bdev;
	unsigned long ss, nsec;
	uint8_t *buf;
	int found = 0;

	might_sleep();
	if (!b || !b->read)
		return 0;
	ss = b->sector_size ? b->sector_size : 512;
	if (ss < 512 || ss > 4096 || (ss & (ss - 1)))
		return 0;
	/* Device sectors covering one descriptor (a 4096-byte sector holds
	 * two; descriptors never straddle one since 32768 is aligned). */
	nsec = ss >= ISOFS_VD_SIZE ? 1 : ISOFS_VD_SIZE / ss;
	buf = (uint8_t *)kalloc(nsec * ss);
	if (!buf)
		return 0;
	for (unsigned i = 0; i < ISOFS_PROBE_MAX_VD; i++) {
		unsigned long byte = ISOFS_VD_START_BYTE + i * ISOFS_VD_SIZE;
		unsigned long lba = byte / ss;
		const uint8_t *vd = buf + byte % ss;

		if (lba + nsec > b->total_sectors)
			break;
		if (b->read((block_device_t *)b, lba, nsec, buf) != ST_OK)
			break;
		if (!isofs_vd_valid(vd))
			break;
		if (isofs_vd_primary(vd)) {
			found = 1;
			break;
		}
		if (vd[0] == ISO_VD_TERMINATOR)
			break;
	}
	kfree(buf);
	return found;
}

/* What mount() gathers from the descriptor set. */
typedef struct isofs_vdinfo {
	uint8_t buf[ISOFS_VD_SIZE];
	int have_pvd, have_svd, have_evd;
	int joliet_level;
	uint8_t pvd_root[ISO_DR_MIN], svd_root[ISO_DR_MIN], evd_root[ISO_DR_MIN];
	uint32_t lbs, svd_lbs, evd_lbs;
	uint64_t space;
	int64_t mtime;
	char volid[33];
} isofs_vdinfo_t;

/* Read the descriptor set at 2048-byte sector `start' + 16. */
static int isofs_read_vds(isofs_fs_t *fs, uint64_t start, isofs_vdinfo_t *vi)
{
	uint8_t *vd = vi->buf;

	for (unsigned i = 0; i < ISOFS_MOUNT_MAX_VD; i++) {
		int r = isofs_read_vd_sector(fs, start + 16 + i, vd);
		int jl;

		if (r != ST_OK)
			return vi->have_pvd ? ST_OK : r;
		if (!isofs_vd_valid(vd))
			break; /* no terminator: the set ends anyway */
		if (vd[0] == ISO_VD_TERMINATOR)
			break;
		if (isofs_vd_primary(vd) && !vi->have_pvd) {
			vi->have_pvd = 1;
			mm_memcpy(vi->pvd_root, vd + ISO_VD_OFF_ROOT, ISO_DR_MIN);
			vi->lbs = isofs_723(vd + ISO_VD_OFF_LBS);
			vi->space = isofs_733(vd + ISO_VD_OFF_SPACE);
			if (fs->o.strict &&
			    (!isofs_723_ok(vd + ISO_VD_OFF_LBS) ||
			     !isofs_733_ok(vd + ISO_VD_OFF_SPACE)))
				return ISOFS_CORRUPT(fs, "primary volume "
							 "descriptor numbers "
							 "disagree");
			vi->mtime = isofs_date17(vd + ISO_VD_OFF_MODIFIED, NULL);
			if (!vi->mtime)
				vi->mtime = isofs_date17(vd + ISO_VD_OFF_CREATED,
							 NULL);
			for (unsigned k = 0; k < 32; k++)
				vi->volid[k] = (char)vd[ISO_VD_OFF_VOLID + k];
			vi->volid[32] = '\0';
			for (int k = 31; k >= 0 && (vi->volid[k] == ' ' ||
						     vi->volid[k] == '\0');
			     k--)
				vi->volid[k] = '\0';
			continue;
		}
		jl = isofs_vd_joliet(vd);
		if (jl && !vi->have_svd) {
			vi->have_svd = 1;
			vi->joliet_level = jl;
			vi->svd_lbs = isofs_723(vd + ISO_VD_OFF_LBS);
			mm_memcpy(vi->svd_root, vd + ISO_VD_OFF_ROOT, ISO_DR_MIN);
			continue;
		}
		if (vd[0] == ISO_VD_SUPPLEMENTARY && vd[6] == 2 &&
		    !vi->have_evd) {
			/* Enhanced volume descriptor (ISO 9660:1999). */
			vi->have_evd = 1;
			vi->evd_lbs = isofs_723(vd + ISO_VD_OFF_LBS);
			mm_memcpy(vi->evd_root, vd + ISO_VD_OFF_ROOT, ISO_DR_MIN);
		}
		/* Boot records, partition descriptors: nothing to do. */
	}
	return vi->have_pvd ? ST_OK : ST_NOT_FOUND;
}

/* session=N: the descriptor set of the N-th session.  Without a table of
 * contents to ask, the next session is searched for after the end of the
 * previous one (its volume space size): the first primary descriptor in
 * the following ISOFS_SESSION_WINDOW sectors, read in chunks. */
#define ISOFS_SESSION_WINDOW 16384
#define ISOFS_SESSION_CHUNK 32

static int isofs_find_session(isofs_fs_t *fs, int session, uint64_t *start)
{
	uint8_t *chunk = (uint8_t *)kalloc(ISOFS_SESSION_CHUNK * ISOFS_VD_SIZE);
	uint8_t *vd = (uint8_t *)kalloc(ISOFS_VD_SIZE);
	uint64_t cur = 0, dev2k = fs->view_sectors * fs->vss / ISOFS_VD_SIZE;
	int r = ST_OK;

	if (!chunk || !vd) {
		kfree(chunk);
		kfree(vd);
		return ST_NOMEM;
	}
	for (int s = 0; s < session && r == ST_OK; s++) {
		uint64_t end, from, found = 0;
		uint32_t lbs, space;

		r = isofs_read_vd_sector(fs, cur + 16, vd);
		if (r != ST_OK)
			break;
		if (!isofs_vd_primary(vd)) {
			r = ST_NOT_FOUND;
			break;
		}
		lbs = isofs_723(vd + ISO_VD_OFF_LBS);
		space = isofs_733(vd + ISO_VD_OFF_SPACE);
		if (lbs != 512 && lbs != 1024 && lbs != 2048) {
			r = ST_NOT_FOUND;
			break;
		}
		/* The space size counts from the start of the medium. */
		end = (uint64_t)space * lbs / ISOFS_VD_SIZE;
		if (end <= cur)
			end = cur + 1;
		r = ST_NOT_FOUND;
		for (from = end; from < end + ISOFS_SESSION_WINDOW &&
				 from + 16 < dev2k;
		     from += ISOFS_SESSION_CHUNK) {
			uint64_t n = ISOFS_SESSION_CHUNK;
			uint64_t byte, view;

			if (from + 16 + n > dev2k)
				n = dev2k - from - 16;
			byte = (from + 16) * ISOFS_VD_SIZE;
			view = byte / fs->vss;
			if (isofs_dev_read(fs, view, n * ISOFS_VD_SIZE / fs->vss,
					   chunk) != ST_OK)
				break;
			for (uint64_t k = 0; k < n; k++)
				if (isofs_vd_primary(chunk +
						     k * ISOFS_VD_SIZE)) {
					found = from + k;
					r = ST_OK;
					break;
				}
			if (r == ST_OK)
				break;
		}
		if (r == ST_OK)
			cur = found;
	}
	kfree(chunk);
	kfree(vd);
	if (r == ST_OK)
		*start = cur;
	return r;
}

/* Check a root directory record taken from a volume descriptor and return
 * its data block. */
static int isofs_root_of(isofs_fs_t *fs, const uint8_t *rec, uint64_t *ino)
{
	isofs_extent_t e;

	if (rec[ISO_DR_LEN] < ISO_DR_MIN || !(rec[ISO_DR_FLAGS] & ISO_FLAG_DIR) ||
	    isofs_record_extent(fs, rec, &e) != ST_OK || e.lba == 0 ||
	    e.lba >= fs->dev_blocks || e.len < ISO_DR_MIN)
		return ISOFS_CORRUPT(fs, "bad root directory record");
	*ino = e.lba;
	return ST_OK;
}

/* Does the root directory carry SUSP (SP) and Rock Ridge?  Sets fs->rock,
 * fs->su_skip and fs->rr_version. */
static int isofs_detect_rock(isofs_fs_t *fs, isofs_work_t *w, uint64_t root)
{
	const uint8_t *rec = w->dotrec, *area = NULL;
	uint64_t room = isofs_min64(sizeof(w->dotrec), fs->lbs);
	unsigned alen;
	int r;

	r = isofs_vol_read(fs, root * fs->lbs, w->dotrec, room);
	if (r != ST_OK)
		return r;
	if (!isofs_record_ok(fs, rec, room) || rec[ISO_DR_NAMELEN] != 1 ||
	    rec[ISO_DR_NAME] != 0)
		return ISOFS_CORRUPT(fs, "root directory has no \".\" record");
	alen = isofs_su_area_of(fs, rec, 1, &area);
	if (!alen)
		return ST_OK;
	/* SP must be the first entry of the root's "." record. */
	if (alen < 7 || area[0] != 'S' || area[1] != 'P' || area[2] < 7 ||
	    area[4] != 0xBE || area[5] != 0xEF)
		return ST_OK;
	isofs_su_reset(&w->su, 0);
	isofs_su_walk(fs, &w->su, area, alen);
	if (!(w->su.seen & SU_SEEN_SP))
		return ST_OK;
	fs->su_skip = w->su.sp_skip;
	if ((w->su.seen & SU_SEEN_ER_RRIP) || (w->su.seen & SU_SEEN_ROCK)) {
		fs->rock = 1;
		fs->rr_version = w->su.px_long ? 112 : 110;
	}
	return ST_OK;
}

/* ===========================================================================
 * Mount, unmount, remount
 * =========================================================================== */

/* Free everything of an instance that is not (or no longer) published. */
static void isofs_teardown(isofs_fs_t *fs, int used_cache)
{
	if (fs->root_st) {
		isofs_stream_put(fs, fs->root_st);
		fs->root_st = NULL;
	}
	if (used_cache)
		vfs_sb_release_caches(&fs->sb);
	if (fs->slots) {
		for (unsigned long i = 0; i < ISOFS_STREAM_SLOTS; i++) {
			isofs_stream_t *s = fs->slots[i];

			if (!s || s == ISOFS_SLOT_RETIRING)
				continue;
			WARN_ON(s->refs != 0);
			isofs_stream_free(s);
			fs->slots[i] = NULL;
		}
		kfree(fs->slots);
		fs->slots = NULL;
	}
	kfree(fs);
}

static int isofs_fst_mount(vfs_mount_t *m, struct block_device *bdev,
			   unsigned long flags, const char *data)
{
	isofs_fs_t *fs = NULL;
	isofs_vdinfo_t *vi = NULL;
	isofs_work_t *w = NULL;
	struct isofs_opts o;
	const uint8_t *root_rec;
	uint64_t root = 0, start = 0;
	unsigned long ss, lbs;
	int r, used_cache = 0;

	(void)flags; /* always read-only */
	if (!m || !bdev)
		return ST_INVALID;
	if (!ISOFS_DRIVER_READY)
		return ST_UNSUPPORTED;
	isofs_opts_default(&o);
	if (isofs_parse_options(data, &o) != ST_OK)
		return ST_INVALID;
	if (!bdev->read)
		return ST_INVALID;
	ss = bdev->sector_size ? bdev->sector_size : 512;
	if (ss < 512 || ss > 4096 || (ss & (ss - 1)) || 4096 % ss)
		return ST_UNSUPPORTED;
	if (o.sbsector < 0 && o.session <= 0 && !isofs_probe_vds(bdev))
		return ST_NOT_FOUND;

	fs = (isofs_fs_t *)kalloc(sizeof(*fs));
	vi = (isofs_vdinfo_t *)kalloc(sizeof(*vi));
	if (!fs || !vi) {
		kfree(fs);
		kfree(vi);
		return ST_NOMEM;
	}
	mm_memset(fs, 0, sizeof(*fs));
	mm_memset(vi, 0, sizeof(*vi));
	fs->o = o;
	fs->bdev = bdev;
	fs->mnt = m;
	fs->real_ss = ss;
	isofs_rwsem_init(&fs->meta, "isofs_meta");
	spinlock_init(&fs->st_lock, "isofs_streams");
	fs->lru.lru_next = fs->lru.lru_prev = &fs->lru;
	/* Descriptor sectors are 2048 bytes whatever the logical block. */
	isofs_guard_setup(fs, ss < ISOFS_VD_SIZE ? ss : ISOFS_VD_SIZE);

	if (o.sbsector >= 0) {
		start = (uint64_t)o.sbsector;
	} else if (o.session > 0) {
		r = isofs_find_session(fs, o.session, &start);
		if (r != ST_OK) {
			kprintf("iso9660: %s: session %d not found\n",
				bdev->name ? bdev->name : "?", o.session);
			goto fail;
		}
	}
	fs->session_start = start;
	r = isofs_read_vds(fs, start, vi);
	if (r != ST_OK) {
		if (r == ST_NOT_FOUND)
			r = ST_NOT_FOUND;
		goto fail;
	}

	lbs = o.block ? o.block : vi->lbs;
	if (lbs != 512 && lbs != 1024 && lbs != 2048) {
		r = ISOFS_CORRUPT(fs, "logical block size %lu", lbs);
		r = ST_UNSUPPORTED;
		goto fail;
	}
	fs->lbs = lbs;
	fs->lbs_shift = lbs == 512 ? 9 : lbs == 1024 ? 10 : 11;
	isofs_guard_setup(fs, ss < lbs ? ss : lbs);
	fs->spb = lbs / fs->vss;
	fs->dev_blocks = fs->view_sectors * fs->vss / lbs;
	fs->vol_blocks = vi->space;
	fs->vol_mtime = vi->mtime;
	mm_memcpy(fs->volid, vi->volid, sizeof(fs->volid));
	if (fs->dev_blocks < 17) {
		r = ST_NOT_FOUND;
		goto fail;
	}
	if (fs->vol_blocks > fs->dev_blocks)
		kprintf("iso9660: %s: volume of %lu blocks on a device of "
			"%lu: truncated image\n",
			bdev->name ? bdev->name : "?",
			(unsigned long)fs->vol_blocks,
			(unsigned long)fs->dev_blocks);

	fs->slots = (isofs_stream_t **)kalloc(ISOFS_STREAM_SLOTS *
					      sizeof(*fs->slots));
	w = isofs_work_alloc(fs, m);
	if (!fs->slots || !w) {
		r = ST_NOMEM;
		goto fail;
	}
	mm_memset(fs->slots, 0, ISOFS_STREAM_SLOTS * sizeof(*fs->slots));
	fs->slot_cursor = 0;

	/* The page cache's superblock and the volume stream. */
	fs->sb.ops = &isofs_sb_ops;
	fs->sb.fs_private = fs;
	fs->vol.key = 0;
	fs->vol.handle = ISOFS_HANDLE_VOLUME;
	fs->vol.chain = ISOFS_BID_ENC(ISOFS_HANDLE_VOLUME, 0);
	fs->vol.flags = ISOFS_SF_VOLUME;
	fs->vol.refs = 1;
	fs->vol.nblocks = fs->dev_blocks;
	fs->vol.size = fs->dev_blocks * lbs;
	fs->vol.raw_size = fs->vol.size;
	fs->vol.ext = &fs->vol.ext1;
	fs->vol.ext1.lba = 0;
	fs->vol.ext1.len = fs->vol.size;
	fs->vol.ext1.nblk = fs->dev_blocks;
	fs->vol.nextents = 1;
	used_cache = 1;

	/* Which tree: Rock Ridge on the primary one, else Joliet, else the
	 * enhanced (ISO 9660:1999) one, else the primary with plain names. */
	r = isofs_root_of(fs, vi->pvd_root, &root);
	if (r != ST_OK)
		goto fail;
	fs->root_ino = root;
	if (!o.norock) {
		r = isofs_detect_rock(fs, w, root);
		if (r != ST_OK)
			goto fail;
	}
	root_rec = vi->pvd_root;
	if (!fs->rock && vi->have_svd && !o.nojoliet &&
	    isofs_root_of(fs, vi->svd_root, &root) == ST_OK) {
		fs->joliet = vi->joliet_level;
		root_rec = vi->svd_root;
		fs->su_skip = 0;
	} else if (!fs->rock && vi->have_evd &&
		   isofs_root_of(fs, vi->evd_root, &root) == ST_OK) {
		fs->fs_version = 2;
		root_rec = vi->evd_root;
		fs->su_skip = 0;
	} else {
		root = fs->root_ino;
	}
	(void)root_rec;
	fs->root_ino = root;
	r = isofs_dir_stream(fs, w, root, &fs->root_st);
	if (r != ST_OK) {
		kprintf("iso9660: %s: root directory unreadable (%d)\n",
			bdev->name ? bdev->name : "?", r);
		goto fail;
	}
	fs->root_size = fs->root_st->size;

	kprintf("iso9660: %s: volume \"%s\", %lu blocks of %lu bytes, %s%s\n",
		bdev->name ? bdev->name : "?", fs->volid,
		(unsigned long)fs->vol_blocks, lbs,
		fs->rock ? "Rock Ridge" :
		fs->joliet ? "Joliet" :
		fs->fs_version == 2 ? "ISO 9660:1999" : "plain ISO 9660",
		fs->joliet == 1 ? " level 1" :
		fs->joliet == 2 ? " level 2" :
		fs->joliet == 3 ? " level 3" : "");
	isofs_work_free(w);
	kfree(vi);
	m->ops = &isofs_vfs_ops;
	m->fs_data = fs;
	m->sb = &fs->sb;
	m->flags |= VFS_MS_RDONLY; /* whatever was asked */
	return ST_OK;

fail:
	isofs_work_free(w);
	kfree(vi);
	isofs_teardown(fs, used_cache);
	return r;
}

/* The end of an unmount: nothing is open, nothing was ever dirty.  Drop
 * every cached piece of the instance and free it. */
static int isofs_fst_unmount(vfs_mount_t *m)
{
	isofs_fs_t *fs = m ? (isofs_fs_t *)m->fs_data : NULL;

	if (!fs)
		return ST_INVALID;
	fs->dying = 1;
	isofs_teardown(fs, 1);
	m->fs_data = NULL;
	m->sb = NULL;
	return ST_OK;
}

/* Only a read-only remount; the options that only change what stat
 * reports (uid=, gid=, mode=, dmode=, overriderockperms) may change, the
 * ones that change names or which tree is used may not. */
static int isofs_fst_remount(vfs_mount_t *m, unsigned long flags,
			     const char *data)
{
	isofs_fs_t *fs = m ? (isofs_fs_t *)m->fs_data : NULL;
	struct isofs_opts o;

	if (!fs)
		return ST_INVALID;
	if (!(flags & VFS_MS_RDONLY))
		return ST_ROFS;
	o = fs->o;
	if (data && isofs_parse_options(data, &o) != ST_OK)
		return ST_INVALID;
	if (o.norock != fs->o.norock || o.nojoliet != fs->o.nojoliet ||
	    o.strict != fs->o.strict || o.map_off != fs->o.map_off ||
	    o.hide != fs->o.hide || o.unhide != fs->o.unhide ||
	    o.cruft != fs->o.cruft || o.nocompress != fs->o.nocompress ||
	    o.block != fs->o.block || o.session != fs->o.session ||
	    o.sbsector != fs->o.sbsector)
		return ST_INVALID;
	isofs_meta_wlock(fs);
	fs->o = o;
	isofs_meta_wunlock(fs);
	m->flags |= VFS_MS_RDONLY;
	return ST_OK;
}

static int isofs_fst_probe(struct block_device *bdev)
{
	return ISOFS_DRIVER_READY && isofs_probe_vds(bdev);
}

static const char *const isofs_fst_aliases[] = { "isofs", NULL };

const vfs_fstype_t isofs_fstype = {
	.name = "iso9660",
	.aliases = isofs_fst_aliases,
	.needs_device = 1,
	.mount = isofs_fst_mount,
	.unmount = isofs_fst_unmount,
	.remount = isofs_fst_remount,
	.show_options = isofs_fst_show_options,
	.probe = isofs_fst_probe,
};
