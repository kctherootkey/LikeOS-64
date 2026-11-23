// LikeOS -- the mount table: mount(2), umount2(2), /proc/mounts.
//
// Copyright (C) 2026 The LikeOS Project

/*
 * Every filesystem the VFS can reach is an entry here: the root, the boot-time
 * pseudo filesystems (/dev, /proc, /sys), the boot-time tmpfs at /ram and
 * whatever mount(2) adds.  A
 * path belongs to the live mount whose mount point is its longest prefix ending
 * at a component boundary; the VFS hands the driver the rest of the path, so a
 * filesystem mounted at /mnt/x sees "/etc/fstab" for /mnt/x/etc/fstab and
 * never has to know where it is mounted.  The syscall layer canonicalises
 * paths lexically before they arrive, so ".." out of a mount's root is
 * already the parent mount's business by the time a path gets here.
 *
 * Finding the mount is on the path of every operation, so it takes no lock:
 * the table is a fixed array of slots, each with a sequence count that is odd
 * while the slot is being rewritten.  A reader that chose a slot raises the
 * slot's `active' count and then re-checks that the slot is still live and
 * unchanged; an unmount marks the slot dying and then waits for `active' to
 * drain.  Both sides do this in sequential consistency, so either the reader
 * sees the dying state or the unmount sees the reader -- an operation never
 * runs on a filesystem that is being torn down.  Slots are never freed, only
 * reused, so a reader holding a stale pointer reads valid memory.  The mounts
 * made at boot are never unmounted and are not counted at all: the root pays
 * nothing for the table.
 *
 * Changes to the table (mount, unmount, remount) are serialised by a sleeping
 * mutex; the fields /proc/mounts shows change under a spinlock, so the listing
 * is consistent without holding up anybody.
 *
 * Unmounting refuses (EBUSY) while files are open on the mount, while a task's
 * working directory or root is inside it, or while another mount sits below
 * it.  MNT_DETACH takes a busy mount out of the name space at once and
 * finishes the unmount when the last file open on it is closed and the last
 * operation still running on it has finished; a working directory inside it
 * then refers to whatever is underneath, since working directories are kept
 * as path names.  MNT_FORCE is accepted and changes nothing: a filesystem
 * with open files cannot be torn down from under them.
 *
 * The `active' counts are owned.  Each one a task takes is recorded in the
 * task (task_t.mnt_held) until it is put back, and a task that dies inside an
 * operation -- killed from the fault path, which never returns to put
 * anything -- has its counts dropped by its teardown (vfs_mnt_release_task),
 * after its filesystem locks were released.  An unmount waits for the running
 * operations only for so long (VFS_UMOUNT_DRAIN_MS), sleeping, and fails with
 * EBUSY after that rather than holding the mount mutex for ever.
 *
 * Mount points move with their directories: renaming a directory that has
 * mounts below it rewrites their paths in the table under the mount mutex,
 * which the rename then holds (vfs_mnt_rename_begin/end).  A mount point
 * itself cannot be renamed or removed (EBUSY).
 */

#include <kernel/fs/mount.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/fs/namei.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/ke/task_path.h>
#include <kernel/fs/file.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/syscalls.h>
#include <kernel/ke/uaccess.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/stat.h>
#include <kernel/uapi/bug.h>

#define VFS_MOUNT_MAX 64
/* A slot reserved by a mount(2) still setting the filesystem up. */
#define VFS_MNT_SETUP 4
/* A slot whose filesystem is being torn down: nothing may take a hold on it
 * any more (see mnt_teardown). */
#define VFS_MNT_GONE 5
/* How long an unmount waits for the operations running on the filesystem. */
#define VFS_UMOUNT_DRAIN_MS 5000ULL

static vfs_mount_t g_mnt[VFS_MOUNT_MAX];
/* Every slot ever used is below this (lookups scan no further). */
static volatile int g_mnt_hi;
/* Protects slot allocation and the fields /proc/mounts shows. */
static spinlock_t g_mnt_slot_lock = SPINLOCK_INIT("vfs_mnt_slot");
/* Serialises mount, unmount and remount. */
static struct mutex g_mnt_mutex = MUTEX_INITIALIZER("vfs_mount");
/* Mount ids: the root is always 1. */
static int g_mnt_next_id = 2;
static vfs_mount_t *volatile g_mnt_root;
/* Renames in flight that did not take g_mnt_mutex, and whether a mount(2) is
 * resolving its mount point: the two sides of the handshake that keeps a
 * mount point from being renamed between being resolved and being entered
 * (vfs_mnt_rename_begin, mnt_setup_begin). */
static volatile long g_unlocked_renames;
static volatile int g_mount_setup;
/* A lazy unmount came due while g_mnt_mutex was held by the task it came
 * due on: whoever unlocks finishes it (mnt_unlock). */
static volatile int g_mnt_sweep;

static void mnt_unlock(void);

/* ---- small string helpers ------------------------------------------------ */

static unsigned mnt_strlen(const char *s)
{
	unsigned n = 0;

	while (s && s[n])
		n++;
	return n;
}

static void mnt_strcpy(char *dst, const char *src, unsigned cap)
{
	unsigned i = 0;

	if (!cap)
		return;
	for (; src && src[i] && i + 1 < cap; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static int mnt_streq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	return kstrcmp(a, b) == 0;
}

/* Does `path' lie at or below the mount point `mp' (length `len')? */
static int mnt_under(const char *path, const char *mp, unsigned len)
{
	if (len == 1)
		return path[0] == '/';
	for (unsigned k = 0; k < len; k++)
		if (path[k] != mp[k])
			return 0;
	return path[len] == '\0' || path[len] == '/';
}

/* ---- slot protocol ------------------------------------------------------- */

static void slot_write_begin(vfs_mount_t *m)
{
	__atomic_add_fetch(&m->seq, 1, __ATOMIC_SEQ_CST);
}

static void slot_write_end(vfs_mount_t *m)
{
	__atomic_add_fetch(&m->seq, 1, __ATOMIC_SEQ_CST);
}

/* Reserve a free slot, left odd (being written) in state SETUP.  `active'
 * and `seq' are never cleared: a reader that raced with the slot's previous
 * life may still drop a count it took.  A slot used for the first time gets
 * its path and source buffers here, taken before the lock; NULL when the
 * table is full or they cannot be had. */
static vfs_mount_t *slot_alloc(void)
{
	vfs_mount_t *m = NULL;
	char *np = pathbuf_get(), *ns = pathbuf_get();
	uint64_t fl;

	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	for (int i = 0; i < VFS_MOUNT_MAX; i++) {
		if (g_mnt[i].state != VFS_MNT_FREE)
			continue;
		if ((!g_mnt[i].path && !np) || (!g_mnt[i].source && !ns))
			break; /* no memory for a new slot's buffers */
		m = &g_mnt[i];
		if (!m->path) {
			m->path = np;
			np = NULL;
		}
		if (!m->source) {
			m->source = ns;
			ns = NULL;
		}
		slot_write_begin(m);
		__atomic_store_n(&m->state, VFS_MNT_SETUP, __ATOMIC_SEQ_CST);
		m->open_files = 0;
		m->pinned = 0;
		m->full_paths = 0;
		m->inject = 0;
		m->id = 0;
		m->parent_id = 0;
		m->path[0] = '\0';
		m->path_len = 0;
		m->source[0] = '\0';
		m->fstype[0] = '\0';
		m->flags = 0;
		m->options[0] = '\0';
		m->show_flags = NULL;
		m->dev = 0;
		m->dev_override = 0;
		m->ops = NULL;
		m->type = NULL;
		m->fs_data = NULL;
		m->sb = NULL;
		m->bdev = NULL;
		if (i + 1 > g_mnt_hi)
			__atomic_store_n(&g_mnt_hi, i + 1, __ATOMIC_RELEASE);
		break;
	}
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
	/* Not needed: the slot had its own already. */
	pathbuf_put(ns);
	pathbuf_put(np);
	return m;
}

/* Make a SETUP slot reachable. */
static void slot_publish(vfs_mount_t *m)
{
	uint64_t fl;

	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	__atomic_store_n(&m->state, VFS_MNT_LIVE, __ATOMIC_SEQ_CST);
	slot_write_end(m);
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
}

/* Give a slot back: from SETUP (a mount that failed, slot still odd) or from
 * a finished unmount (even). */
static void slot_free(vfs_mount_t *m)
{
	uint64_t fl;

	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	if (!(m->seq & 1))
		slot_write_begin(m);
	__atomic_store_n(&m->state, VFS_MNT_FREE, __ATOMIC_SEQ_CST);
	slot_write_end(m);
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
}

static void slot_set_state(vfs_mount_t *m, int state)
{
	uint64_t fl;

	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	__atomic_store_n(&m->state, state, __ATOMIC_SEQ_CST);
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
}

/* ---- holds ---------------------------------------------------------------- */

static void mnt_finish_detached(vfs_mount_t *m, int may_wait);

/* Record a hold the current task took on `m' (see task_t.mnt_held).  With
 * every slot in use the hold goes unrecorded: should the task die before
 * putting it, the unmount's bounded wait is what copes. */
static void mnt_hold_note(vfs_mount_t *m)
{
	task_t *t = sched_current();

	if (!t)
		return;
	for (int i = 0; i < TASK_MNT_HOLDS; i++)
		if (!t->mnt_held[i]) {
			t->mnt_held[i] = m;
			return;
		}
}

static void mnt_hold_forget(vfs_mount_t *m)
{
	task_t *t = sched_current();

	if (!t)
		return;
	for (int i = TASK_MNT_HOLDS; i-- > 0;)
		if (t->mnt_held[i] == m) {
			t->mnt_held[i] = NULL;
			return;
		}
}

/* One hold on `m' ends.  The last one of a lazily unmounted filesystem
 * with no file open on it any more finishes the unmount (`may_wait': the
 * caller may wait for the mount mutex to do it). */
static void mnt_active_drop_ex(vfs_mount_t *m, int may_wait)
{
	long left = __atomic_sub_fetch(&m->active, 1, __ATOMIC_SEQ_CST);

	WARN_ON(left < 0);
	if (left == 0 &&
	    __atomic_load_n(&m->state, __ATOMIC_SEQ_CST) == VFS_MNT_DETACHED &&
	    __atomic_load_n(&m->open_files, __ATOMIC_SEQ_CST) == 0)
		mnt_finish_detached(m, may_wait);
}

static void mnt_active_drop(vfs_mount_t *m)
{
	mnt_active_drop_ex(m, 1);
}

void vfs_mnt_release_task(struct task *t)
{
	if (!t)
		return;
	for (int i = 0; i < TASK_MNT_HOLDS; i++) {
		vfs_mount_t *m = t->mnt_held[i];

		if (!m)
			continue;
		t->mnt_held[i] = NULL;
		/* From a dead task's teardown, which may be running in the
		 * scheduling tail of a task that is itself waiting for the
		 * mount mutex: never wait for it here. */
		mnt_active_drop_ex(m, 0);
	}
}

/* Wait, sleeping, until nothing holds `m' any more -- at most `ms'
 * milliseconds.  0, -EBUSY when the time ran out, -EINTR when a signal
 * arrived. */
static int mnt_drain(vfs_mount_t *m, uint64_t ms)
{
	uint64_t start = hrtimer_now_ns();

	while (__atomic_load_n(&m->active, __ATOMIC_SEQ_CST) != 0) {
		uint64_t now = hrtimer_now_ns();

		if (now - start >= ms * 1000000ULL)
			return -EBUSY;
		if (hrtimer_sleep_until(now + 1000000ULL, NULL) == -EINTR)
			return -EINTR;
	}
	return 0;
}

/* ---- lookup -------------------------------------------------------------- */

static const char *mnt_rel(const vfs_mount_t *m, const char *path)
{
	if (m->full_paths || m->path_len == 1 || path[0] != '/')
		return path;
	const char *r = path + m->path_len;
	return *r ? r : "/";
}

vfs_mount_t *vfs_mnt_get(const char *path, const char **rel)
{
	if (!path)
		return NULL;
	int relative = (path[0] != '/');

	for (;;) {
		vfs_mount_t *best = NULL;
		uint32_t best_seq = 0;
		unsigned best_len = 0;
		int best_pinned = 0, retry = 0;
		int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);

		for (int i = 0; i < hi; i++) {
			vfs_mount_t *m = &g_mnt[i];
			uint32_t seq = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);

			if (seq & 1)
				continue;
			if (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) !=
			    VFS_MNT_LIVE)
				continue;
			unsigned len = m->path_len;
			if (best && len <= best_len)
				continue;
			if (len == 0 || len >= VFS_MAX_PATH)
				continue;
			int match = relative ? (len == 1) :
					       mnt_under(path, m->path, len);
			int pinned = m->pinned;
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if (__atomic_load_n(&m->seq, __ATOMIC_RELAXED) != seq) {
				retry = 1;
				break;
			}
			if (!match)
				continue;
			best = m;
			best_seq = seq;
			best_len = len;
			best_pinned = pinned;
		}
		if (retry) {
			/* A slot changed under the walk -- a mount or unmount
			 * committing, which is brief.  Walk again. */
			__asm__ volatile("pause");
			continue;
		}
		if (!best)
			return NULL;
		if (!best_pinned) {
			__atomic_add_fetch(&best->active, 1, __ATOMIC_SEQ_CST);
			if (__atomic_load_n(&best->state, __ATOMIC_SEQ_CST) !=
				    VFS_MNT_LIVE ||
			    __atomic_load_n(&best->seq, __ATOMIC_SEQ_CST) !=
				    best_seq) {
				/* (A lazy unmount that saw this count
				 * leaves the teardown to whoever drops the
				 * last one.) */
				mnt_active_drop(best);
				continue;
			}
			mnt_hold_note(best);
		}
		if (rel)
			*rel = mnt_rel(best, path);
		return best;
	}
}

void vfs_mnt_put(vfs_mount_t *m)
{
	if (m && !m->pinned) {
		mnt_hold_forget(m);
		mnt_active_drop(m);
	}
}

vfs_mount_t *vfs_mnt_root(void)
{
	return g_mnt_root;
}

int vfs_root_ready(void)
{
	return g_mnt_root != NULL;
}

/* Is `state' one vfs_mnt_for_each_instance() visits? */
static int mnt_state_visited(int state, int dying)
{
	return state == VFS_MNT_LIVE || state == VFS_MNT_DETACHED ||
	       (dying && state == VFS_MNT_DYING);
}

void vfs_mnt_for_each_instance(void (*fn)(vfs_mount_t *m, void *arg),
			       void *arg, int dying)
{
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);

	for (int i = 0; i < hi; i++) {
		vfs_mount_t *m = &g_mnt[i];
		uint32_t seq = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);

		if ((seq & 1) ||
		    !mnt_state_visited(__atomic_load_n(&m->state,
						       __ATOMIC_ACQUIRE),
				       dying))
			continue;
		if (!m->pinned) {
			/* The hold keeps the instance from being torn down
			 * under fn: a teardown marks the slot GONE first and
			 * then waits for every hold (mnt_teardown), and this
			 * re-check after raising the count sees the mark or
			 * is seen by that wait. */
			__atomic_add_fetch(&m->active, 1, __ATOMIC_SEQ_CST);
			if (!mnt_state_visited(__atomic_load_n(&m->state,
							       __ATOMIC_SEQ_CST),
					       dying) ||
			    __atomic_load_n(&m->seq, __ATOMIC_SEQ_CST) != seq) {
				mnt_active_drop(m);
				continue;
			}
			mnt_hold_note(m);
		}
		fn(m, arg);
		vfs_mnt_put(m);
	}
}

void vfs_mnt_for_each(void (*fn)(vfs_mount_t *m, void *arg), void *arg)
{
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);

	for (int i = 0; i < hi; i++) {
		vfs_mount_t *m = &g_mnt[i];
		uint32_t seq = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);

		if ((seq & 1) ||
		    __atomic_load_n(&m->state, __ATOMIC_ACQUIRE) != VFS_MNT_LIVE)
			continue;
		if (!m->pinned) {
			__atomic_add_fetch(&m->active, 1, __ATOMIC_SEQ_CST);
			if (__atomic_load_n(&m->state, __ATOMIC_SEQ_CST) !=
				    VFS_MNT_LIVE ||
			    __atomic_load_n(&m->seq, __ATOMIC_SEQ_CST) != seq) {
				mnt_active_drop(m);
				continue;
			}
			mnt_hold_note(m);
		}
		fn(m, arg);
		vfs_mnt_put(m);
	}
}

/* `path' with its links followed into `buf', or just made canonical when
 * they cannot be. */
static void mnt_resolved(const char *path, char *buf)
{
	if (vfs_resolve(path, 1, buf) != ST_OK) {
		const char *c = vfs_canon(path, buf);

		if (c != buf)
			mnt_strcpy(buf, c, VFS_MAX_PATH);
	}
}

unsigned long vfs_path_mount_flags(const char *path)
{
	char *cbuf = pathbuf_get();
	vfs_mount_t *m;
	unsigned long fl;

	/* The mount of what the path names, its links followed: a link on
	 * one filesystem to a program on another runs with the other's
	 * noexec and nosuid.  Without memory to find out, the strictest
	 * flags: nothing runs with privileges it may not have. */
	if (!cbuf)
		return VFS_MS_NOSUID | VFS_MS_NOEXEC | VFS_MS_NODEV;
	mnt_resolved(path, cbuf);
	m = vfs_mnt_get(cbuf, NULL);
	pathbuf_put(cbuf);
	if (!m)
		return 0;
	fl = m->flags;
	vfs_mnt_put(m);
	return fl;
}

/* The flags of the mount an open file was opened through: what the file
 * itself lives on, whatever its name points at now.  A handle that knows no
 * mount reports nothing. */
unsigned long vfs_file_mount_flags(vfs_file_t *f)
{
	return (f && f->mnt) ? f->mnt->flags : 0;
}

/* ---- open-file accounting ------------------------------------------------ */

static void mnt_teardown_detached(vfs_mount_t *m);

void vfs_mnt_file_opened(vfs_mount_t *m)
{
	if (m && !m->pinned)
		__atomic_add_fetch(&m->open_files, 1, __ATOMIC_SEQ_CST);
}

void vfs_mnt_file_closed(vfs_mount_t *m)
{
	if (!m || m->pinned)
		return;
	long left = __atomic_sub_fetch(&m->open_files, 1, __ATOMIC_SEQ_CST);
	WARN_ON(left < 0);
	if (left != 0 ||
	    __atomic_load_n(&m->state, __ATOMIC_SEQ_CST) != VFS_MNT_DETACHED ||
	    __atomic_load_n(&m->active, __ATOMIC_SEQ_CST) != 0)
		return;
	mnt_finish_detached(m, 1);
}

/* A lazily unmounted filesystem may have just lost its last file or its last
 * running operation: tear it down if it has neither.  Both counts only fall
 * once the slot is DETACHED (nothing new can reach it), so whoever drops the
 * last of either sees the other at zero and gets here. */
static void mnt_finish_detached(vfs_mount_t *m, int may_wait)
{
	/* The caller may hold the mutex already: a dead task's teardown runs
	 * in the scheduling tail of whatever task resumes, and that one may
	 * be in the middle of a mount or unmount.  The mutex is not
	 * recursive; leave the teardown to its unlock (mnt_unlock).  The
	 * same when the caller must not wait and somebody else holds it --
	 * whoever that is unlocks it. */
	if (mutex_held(&g_mnt_mutex)) {
		__atomic_store_n(&g_mnt_sweep, 1, __ATOMIC_SEQ_CST);
		return;
	}
	if (!may_wait) {
		if (!mutex_trylock(&g_mnt_mutex)) {
			__atomic_store_n(&g_mnt_sweep, 1, __ATOMIC_SEQ_CST);
			/* Unlocked meanwhile, before it saw the flag?  Then
			 * nobody would sweep: try once more to do it here. */
			if (!mutex_trylock(&g_mnt_mutex))
				return;
		}
	} else {
		mutex_lock(&g_mnt_mutex);
	}
	if (m->state == VFS_MNT_DETACHED &&
	    __atomic_load_n(&m->open_files, __ATOMIC_SEQ_CST) == 0 &&
	    __atomic_load_n(&m->active, __ATOMIC_SEQ_CST) == 0)
		mnt_teardown_detached(m);
	mnt_unlock();
}

/* Release g_mnt_mutex -- first finishing any lazy unmount that came due
 * while it was held by the task that came due on (mnt_finish_detached). */
static void mnt_unlock(void)
{
	for (;;) {
		if (__atomic_exchange_n(&g_mnt_sweep, 0, __ATOMIC_SEQ_CST)) {
			int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);

			for (int i = 0; i < hi; i++) {
				vfs_mount_t *m = &g_mnt[i];

				if (m->state == VFS_MNT_DETACHED &&
				    __atomic_load_n(&m->open_files,
						    __ATOMIC_SEQ_CST) == 0 &&
				    __atomic_load_n(&m->active,
						    __ATOMIC_SEQ_CST) == 0)
					mnt_teardown_detached(m);
			}
		}
		mutex_unlock(&g_mnt_mutex);
		/* Raised after the look above by a task that could not take
		 * the mutex (mnt_finish_detached)?  It counts on whoever
		 * has the mutex next: take it back, unless somebody else
		 * already did. */
		if (!__atomic_load_n(&g_mnt_sweep, __ATOMIC_SEQ_CST) ||
		    !mutex_trylock(&g_mnt_mutex))
			return;
	}
}

/* ---- boot-time registration ---------------------------------------------- */

static vfs_mount_t *mnt_register_boot(const char *path, const vfs_ops_t *ops,
				      const char *fstype, const char *source,
				      void *fs_data, unsigned long flags,
				      const char *show, int full_paths,
				      int inject, int id)
{
	if (!path || !ops)
		return NULL;
	unsigned len = mnt_strlen(path);
	if (len == 0 || len >= VFS_MAX_PATH)
		return NULL;
	vfs_mount_t *m = slot_alloc();
	if (!m) {
		kprintf("vfs: mount table full, %s not mounted\n", path);
		return NULL;
	}
	mnt_strcpy(m->path, path, VFS_MAX_PATH);
	m->path_len = len;
	mnt_strcpy(m->source, source, VFS_MAX_PATH);
	mnt_strcpy(m->fstype, fstype, sizeof(m->fstype));
	m->ops = ops;
	m->fs_data = fs_data;
	m->flags = flags & VFS_MS_PERMANENT;
	m->show_flags = show;
	m->pinned = 1;
	m->full_paths = full_paths;
	m->inject = inject;
	m->id = id;
	m->parent_id = id == 1 ? 0 : 1;
	return m;
}

int vfs_register_root(const vfs_ops_t *ops, void *fs_data,
		      struct vfs_superblock *sb, const vfs_fstype_t *type,
		      const char *fstype)
{
	if (!ops)
		return ST_INVALID;
	if (g_mnt_root)
		return ST_BUSY;
	vfs_mount_t *m = mnt_register_boot("/", ops, fstype, "/dev/root",
					   fs_data, 0, NULL, 0, 0, 1);
	if (!m)
		return ST_NOMEM;
	m->sb = sb;
	m->type = type;
	slot_publish(m);
	__atomic_store_n(&g_mnt_root, m, __ATOMIC_RELEASE);
	return ST_OK;
}

void vfs_root_set_source(const char *source, uint64_t dev)
{
	vfs_mount_t *m = g_mnt_root;
	uint64_t fl;

	if (!m)
		return;
	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	mnt_strcpy(m->source, source, VFS_MAX_PATH);
	m->dev = dev;
	/* stat() on the root reports its device, the number mountinfo shows
	 * and every other disk mount reports for its own files -- not the
	 * driver's 0.  Before the device is known (and for a root without
	 * one) the driver's value stands. */
	m->dev_override = dev != 0;
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
}

static int mnt_take_id(void)
{
	uint64_t fl;
	int id;

	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	id = g_mnt_next_id++;
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
	return id;
}

int vfs_register_devfs(const vfs_ops_t *ops)
{
	vfs_mount_t *m = mnt_register_boot("/dev", ops, "devfs", "devfs", NULL,
					   VFS_MS_NOSUID, NULL, 1, 1,
					   mnt_take_id());
	if (!m)
		return ST_INVALID;
	m->dev = kdev_encode(0, 5);
	slot_publish(m);
	return ST_OK;
}

int vfs_register_mount(const char *prefix, const vfs_ops_t *ops,
		       const char *fstype, void *fs_data, unsigned long flags,
		       const char *show, int full_paths)
{
	int id = mnt_take_id();
	vfs_mount_t *m = mnt_register_boot(prefix, ops, fstype, fstype, fs_data,
					   flags, show, full_paths, 1, id);
	if (!m)
		return ST_INVALID;
	m->dev = kdev_encode(0, 16 + (uint32_t)id);
	slot_publish(m);
	return ST_OK;
}

/* A boot-time instance of a mountable filesystem type (tmpfs at /ram): set
 * up by the type's own mount, as mount(2) would, and then kept like the
 * other boot mounts -- pinned (never unmounted), its name listed in the root
 * directory, paths handed over relative to the mount point. */
int vfs_mount_boot(const char *prefix, const vfs_fstype_t *type,
		   unsigned long flags, const char *data)
{
	if (!prefix || prefix[0] != '/' || !prefix[1] || !type ||
	    !type->mount || !type->unmount)
		return ST_INVALID;
	unsigned len = mnt_strlen(prefix);
	if (len >= VFS_MAX_PATH)
		return ST_NAMETOOLONG;
	int id = mnt_take_id();
	vfs_mount_t *m = slot_alloc();
	if (!m) {
		kprintf("vfs: mount table full, %s not mounted\n", prefix);
		return ST_NOMEM;
	}
	mnt_strcpy(m->path, prefix, VFS_MAX_PATH);
	m->path_len = len;
	mnt_strcpy(m->source, type->name, VFS_MAX_PATH);
	mnt_strcpy(m->fstype, type->name, sizeof(m->fstype));
	m->flags = flags & VFS_MS_PERMANENT;
	if (data)
		mnt_strcpy(m->options, data, sizeof(m->options));
	m->pinned = 1;
	m->inject = 1;
	m->id = id;
	m->parent_id = 1;
	m->dev = kdev_encode(0, 16 + (uint32_t)id);
	m->type = type;

	int s = type->mount(m, NULL, m->flags, data);
	if (s == ST_OK && !m->ops) {
		WARN_ON(1);
		type->unmount(m);
		s = ST_INVALID;
	}
	if (s != ST_OK) {
		slot_free(m);
		return s;
	}
	slot_publish(m);
	return ST_OK;
}

/* Names of the boot-time mount points directly under /, for the root
 * listing (the root filesystem need not have directories for them). */
int vfs_mount_name(int index, const char **name_out)
{
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);
	int n = 0;

	for (int i = 0; i < hi; i++) {
		vfs_mount_t *m = &g_mnt[i];

		/* Boot mounts are pinned: their slots never change once
		 * published, so no sequence check is needed here. */
		if (!m->pinned || !m->inject ||
		    __atomic_load_n(&m->state, __ATOMIC_ACQUIRE) != VFS_MNT_LIVE)
			continue;
		if (n++ == index) {
			*name_out = m->path + 1;
			return 1;
		}
	}
	return 0;
}

/* ---- filesystem types ---------------------------------------------------- */

#define VFS_FSTYPES_MAX 16
static const vfs_fstype_t *g_fstypes[VFS_FSTYPES_MAX];
static int g_nfstypes;

int vfs_fstype_register(const vfs_fstype_t *type)
{
	uint64_t fl;
	int r = ST_NOMEM;

	if (!type || !type->name || !type->mount || !type->unmount)
		return ST_INVALID;
	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	if (g_nfstypes < VFS_FSTYPES_MAX) {
		g_fstypes[g_nfstypes++] = type;
		r = ST_OK;
	}
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
	return r;
}

static const vfs_fstype_t *mnt_find_type(const char *name)
{
	for (int i = 0; i < g_nfstypes; i++) {
		const vfs_fstype_t *t = g_fstypes[i];

		if (mnt_streq(t->name, name))
			return t;
		for (const char *const *a = t->aliases; a && *a; a++)
			if (mnt_streq(*a, name))
				return t;
	}
	return NULL;
}

/* The kernel's own filesystems, mounted at boot and nowhere else. */
static int mnt_is_kernel_fs(const char *name)
{
	static const char *const k[] = { "proc", "sysfs", "devfs", "devtmpfs",
					 "devpts", NULL };

	for (int i = 0; k[i]; i++)
		if (mnt_streq(name, k[i]))
			return 1;
	return 0;
}

/* ---- options ------------------------------------------------------------- */

int vfs_next_option(const char **cursor, char *tmp, unsigned cap,
		    const char **key, const char **val)
{
	const char *p = *cursor;

	if (!p || cap < 2)
		return 0;
	while (*p == ',')
		p++;
	if (!*p) {
		*cursor = p;
		return 0;
	}
	unsigned n = 0;
	while (*p && *p != ',') {
		if (n + 1 < cap)
			tmp[n++] = *p;
		p++;
	}
	tmp[n] = '\0';
	*cursor = p;
	*key = tmp;
	*val = NULL;
	for (unsigned i = 0; i < n; i++)
		if (tmp[i] == '=') {
			tmp[i] = '\0';
			*val = tmp + i + 1;
			break;
		}
	return 1;
}

/* ---- the device behind a source ------------------------------------------ */

/* The block device `source' names ("/dev/sda2", "/dev/disk/by-uuid/...":
 * links are followed), with a hold taken on it.  0 or -errno. */
static int mnt_source_bdev(const char *source, block_device_t **out)
{
	struct kstat st;
	int r;

	*out = NULL;
	if (!source || !source[0])
		return -ENOENT;
	if (source[0] != '/')
		return -ENOTBLK;
	r = vfs_stat(source, &st);
	if (r != ST_OK)
		return vfs_status_to_errno(r);
	if (!S_ISBLK(st.st_mode))
		return -ENOTBLK;
	block_device_t *b =
		blk_lookup_devt(kdev_major(st.st_rdev), kdev_minor(st.st_rdev));
	if (!b)
		return -ENXIO;
	*out = b;
	return 0;
}

/* ---- the mount point ----------------------------------------------------- */

/* Resolve symbolic links in an absolute canonical path, so the mount is
 * entered under the name every later lookup will use.  `out' is `cap' bytes.
 * 0 or -errno. */
static int mnt_realpath(const char *in, char *out, unsigned cap)
{
	char *work = NULL, *next = NULL, *link = NULL;
	int hops = 0, ret = 0;

	if (!in || in[0] != '/' || cap < 2)
		return -EINVAL;
	work = pathbuf_get();
	next = pathbuf_get();
	link = pathbuf_get();
	if (!work || !next || !link) {
		ret = -ENOMEM;
		goto out_free;
	}
	mnt_strcpy(work, in, PATHBUF_SIZE);
restart:
	out[0] = '/';
	out[1] = '\0';
	unsigned olen = 1;
	const char *p = work;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *e = p;
		while (*e && *e != '/')
			e++;
		unsigned clen = (unsigned)(e - p);
		unsigned before = olen; /* length of the containing directory */
		/* The path is canonical: no "." or ".." components. */
		unsigned nlen = olen + (olen > 1 ? 1 : 0) + clen;
		if (nlen + 1 >= cap) {
			ret = -ENAMETOOLONG;
			goto out_free;
		}
		if (olen > 1)
			out[olen++] = '/';
		for (unsigned i = 0; i < clen; i++)
			out[olen++] = p[i];
		out[olen] = '\0';
		struct kstat st;
		int r = vfs_lstat(out, &st);
		if (r != ST_OK) {
			ret = vfs_status_to_errno(r);
			goto out_free;
		}
		if (S_ISLNK(st.st_mode)) {
			if (++hops > 8) {
				ret = -ELOOP;
				goto out_free;
			}
			int n = vfs_readlink(out, link, PATHBUF_SIZE - 1);
			if (n <= 0) {
				ret = -EINVAL;
				goto out_free;
			}
			link[n] = '\0';
			/* Where the link leads, then the rest of the path: from
			 * the directory holding the link (out[0, before)) for
			 * a relative target. */
			size_t nl = 1;
			next[0] = '/';
			next[1] = '\0';
			int nr = 0;
			if (link[0] != '/')
				nr = normalize_path_append(next, PATHBUF_SIZE, &nl,
							   out, before);
			if (nr == 0)
				nr = normalize_path_append(next, PATHBUF_SIZE, &nl,
							   link, (size_t)n);
			if (nr == 0)
				nr = normalize_path_append(next, PATHBUF_SIZE, &nl,
							   e, (size_t)-1);
			if (nr != 0) {
				ret = nr;
				goto out_free;
			}
			/* `e' points into `work': only now may it change. */
			char *t = work;
			work = next;
			next = t;
			goto restart;
		}
		p = e;
	}
out_free:
	pathbuf_put(link);
	pathbuf_put(next);
	pathbuf_put(work);
	return ret;
}

/* ---- tasks inside a mount ------------------------------------------------- */

static int mnt_tasks_inside(const vfs_mount_t *m)
{
	uint32_t next = 0;

	for (;;) {
		uint64_t fl;
		int inside = 0, found = 0;

		struct task_path *cp = NULL, *rp = NULL;

		spin_lock_irqsave(&g_task_list_lock, &fl);
		task_t *t = sched_find_next_task_by_id_locked(next);
		if (t) {
			found = 1;
			next = (uint32_t)t->id + 1;
			/* References: the task may chdir or exit the moment
			 * the list lock is dropped. */
			cp = task_path_ref(t, TASK_PATH_CWD);
			rp = task_path_ref(t, TASK_PATH_ROOT);
		}
		spin_unlock_irqrestore(&g_task_list_lock, fl);
		/* (A NULL working directory is "/", which no mount point
		 * other than the root's contains.) */
		if (cp && mnt_under(cp->s, m->path, m->path_len))
			inside = 1;
		if (rp && mnt_under(rp->s, m->path, m->path_len))
			inside = 1;
		task_path_put(cp);
		task_path_put(rp);
		if (!found)
			return 0;
		if (inside)
			return 1;
		if (next == 0)
			return 0;
	}
}

/* Another mount below `m'? */
static int mnt_has_children(const vfs_mount_t *m)
{
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);

	for (int i = 0; i < hi; i++) {
		vfs_mount_t *c = &g_mnt[i];
		int st = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);

		/* A lazily unmounted one is out of the name space already:
		 * it waits for its own files, not for the mount above it. */
		if (c == m || st == VFS_MNT_FREE || st == VFS_MNT_DETACHED ||
		    st == VFS_MNT_GONE)
			continue;
		if (c->path_len > m->path_len &&
		    mnt_under(c->path, m->path, m->path_len))
			return 1;
	}
	return 0;
}

/* The live mount exactly at `path', or NULL.  Called under g_mnt_mutex,
 * which every change of a slot's state but publish/teardown takes. */
static vfs_mount_t *mnt_find_exact(const char *path)
{
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);

	for (int i = 0; i < hi; i++) {
		vfs_mount_t *m = &g_mnt[i];

		if (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == VFS_MNT_LIVE &&
		    mnt_streq(m->path, path))
			return m;
	}
	return NULL;
}

/* ---- renames ---------------------------------------------------------------- */

/* Is a mount in the name space (or being set up, or being unmounted) exactly
 * at `path'?  The root does not count: it is nobody's mount point. */
int vfs_mnt_is_point(const char *path)
{
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);
	unsigned len = mnt_strlen(path);

	if (len <= 1)
		return 0;
	while (len > 1 && path[len - 1] == '/')
		len--;
	for (int i = 0; i < hi; i++) {
		vfs_mount_t *m = &g_mnt[i];

		for (;;) {
			uint32_t seq = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
			int st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
			int hit;

			if (st != VFS_MNT_LIVE && st != VFS_MNT_DYING &&
			    st != VFS_MNT_SETUP)
				break;
			hit = m->path_len == len &&
			      mnt_under(path, m->path, len);
			/* A slot being set up stays odd until it is
			 * published; its path was written before the
			 * filesystem was, and is what it is.  Any other
			 * odd or changed slot is being rewritten, briefly:
			 * look again. */
			if (st == VFS_MNT_SETUP) {
				if (hit)
					return 1;
				break;
			}
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if ((seq & 1) ||
			    __atomic_load_n(&m->seq, __ATOMIC_RELAXED) != seq) {
				__asm__ volatile("pause");
				continue;
			}
			if (hit)
				return 1;
			break;
		}
	}
	return 0;
}

/* Is a mount (live, being set up or unmounted) strictly below `dir'? */
static int mnt_any_below(const char *dir, unsigned dlen)
{
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);

	for (int i = 0; i < hi; i++) {
		vfs_mount_t *m = &g_mnt[i];
		uint32_t seq = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
		int st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
		int hit;

		if (st != VFS_MNT_LIVE && st != VFS_MNT_DYING &&
		    st != VFS_MNT_SETUP)
			continue;
		hit = m->path_len > dlen && mnt_under(m->path, dir, dlen);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if ((seq & 1) || __atomic_load_n(&m->seq, __ATOMIC_RELAXED) != seq ||
		    hit)
			return 1;
	}
	return 0;
}

static unsigned mnt_trimmed_len(const char *p)
{
	unsigned n = mnt_strlen(p);

	while (n > 1 && p[n - 1] == '/')
		n--;
	return n;
}

int vfs_mnt_rename_begin(const char *oldpath, const char *newpath, int *locked)
{
	unsigned olen = mnt_trimmed_len(oldpath);
	unsigned nlen = mnt_trimmed_len(newpath);

	*locked = 0;
	if (olen <= 1)
		return ST_OK;
	/* Counted first, then the table looked at; a mount(2) sets its flag
	 * first, then looks at the count (mnt_setup_begin).  Either this
	 * rename sees the mount coming and takes the mutex, or the mount
	 * waits for this rename to finish before resolving its mount
	 * point. */
	__atomic_add_fetch(&g_unlocked_renames, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&g_mount_setup, __ATOMIC_SEQ_CST) &&
	    !mnt_any_below(oldpath, olen))
		return ST_OK;
	__atomic_sub_fetch(&g_unlocked_renames, 1, __ATOMIC_SEQ_CST);

	mutex_lock(&g_mnt_mutex);
	*locked = 1;
	/* Every path that moves must still fit once it has. */
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);
	for (int i = 0; i < hi; i++) {
		vfs_mount_t *m = &g_mnt[i];

		if (m->state != VFS_MNT_LIVE && m->state != VFS_MNT_DYING)
			continue;
		if (m->path_len > olen && mnt_under(m->path, oldpath, olen) &&
		    nlen + (m->path_len - olen) >= VFS_MAX_PATH) {
			mnt_unlock();
			*locked = 0;
			return ST_NAMETOOLONG;
		}
	}
	return ST_OK;
}

void vfs_mnt_rename_end(int locked, const char *oldpath, const char *newpath,
			int renamed)
{
	if (!locked) {
		__atomic_sub_fetch(&g_unlocked_renames, 1, __ATOMIC_SEQ_CST);
		return;
	}
	if (renamed) {
		unsigned olen = mnt_trimmed_len(oldpath);
		unsigned nlen = mnt_trimmed_len(newpath);
		int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);
		int moved = 0;

		for (int i = 0; i < hi; i++) {
			vfs_mount_t *m = &g_mnt[i];
			unsigned tail;
			uint64_t fl;

			if (m->state != VFS_MNT_LIVE &&
			    m->state != VFS_MNT_DYING)
				continue;
			if (m->path_len <= olen ||
			    !mnt_under(m->path, oldpath, olen))
				continue;
			/* The new directory's name, then the part of the
			 * mount point below the old one (checked to fit in
			 * vfs_mnt_rename_begin).  Rewritten in place, inside
			 * the slot's write section: a reader that looks
			 * meanwhile sees the sequence change and looks
			 * again.  The tail moves first -- from its end when
			 * it moves up, from its start when it moves down. */
			tail = m->path_len - olen;
			if (nlen + tail >= VFS_MAX_PATH)
				continue;
			spin_lock_irqsave(&g_mnt_slot_lock, &fl);
			slot_write_begin(m);
			if (nlen > olen) {
				for (unsigned j = tail; j-- > 0;)
					m->path[nlen + j] = m->path[olen + j];
			} else {
				for (unsigned j = 0; j < tail; j++)
					m->path[nlen + j] = m->path[olen + j];
			}
			for (unsigned j = 0; j < nlen; j++)
				m->path[j] = newpath[j];
			m->path[nlen + tail] = '\0';
			m->path_len = nlen + tail;
			slot_write_end(m);
			spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
			moved = 1;
		}
		if (moved)
			vfs_meta_changed();
	}
	mnt_unlock();
}

/* ---- teardown ------------------------------------------------------------- */

void vfs_sb_release_caches(struct vfs_superblock *sb)
{
	if (!sb)
		return;
	pagecache_invalidate_sb(sb);
	icache_invalidate_sb(sb);
	dcache_invalidate_owner(sb);
}

/* Store everything of one filesystem: its pages, inodes, metadata, and the
 * device's cache. */
static void mnt_flush(vfs_mount_t *m)
{
	if (m->sb) {
		pagecache_flush_sb(m->sb);
		icache_flush_sb(m->sb);
	}
	if (m->ops && m->ops->sync)
		m->ops->sync(m);
	block_device_t *b = m->bdev;
	if (!b && m->sb)
		b = (block_device_t *)vfs_sb_bdev(m->sb);
	if (b && b->sync) {
		if (m->sb)
			vfs_sb_lock_io(m->sb);
		b->sync(b);
		if (m->sb)
			vfs_sb_unlock_io(m->sb);
	}
}

/* Unmount a mount nobody uses any more (state DYING or DETACHED, nothing in
 * flight, nothing open).  Under g_mnt_mutex.  0 or -errno; on failure the
 * filesystem is still there, in the state it came in.
 *
 * The slot is marked GONE first, and then the holds are waited out once
 * more: a sync or a dead task's lock release (vfs_mnt_for_each_instance)
 * may hold a DETACHED or DYING instance, and none can be taken from the
 * mark on. */
static int mnt_teardown(vfs_mount_t *m)
{
	int before = m->state;

	slot_set_state(m, VFS_MNT_GONE);
	int d = mnt_drain(m, VFS_UMOUNT_DRAIN_MS);
	if (d != 0) {
		/* Still held: back as it was.  A lazy unmount is finished by
		 * the drop of that hold (mnt_active_drop) -- or, should it
		 * have dropped while the slot said GONE, by the sweep at our
		 * unlock. */
		slot_set_state(m, before);
		if (before == VFS_MNT_DETACHED)
			__atomic_store_n(&g_mnt_sweep, 1, __ATOMIC_SEQ_CST);
		return d;
	}
	mnt_flush(m);
	int r = m->type ? m->type->unmount(m) : ST_BUSY;
	if (r != ST_OK) {
		slot_set_state(m, before);
		return vfs_status_to_errno(r);
	}
	if (m->bdev) {
		blk_bdev_unclaim(m->bdev, m);
		blk_bdev_put(m->bdev);
		m->bdev = NULL;
	}
	slot_free(m);
	return 0;
}

static void mnt_teardown_detached(vfs_mount_t *m)
{
	int r = mnt_teardown(m);

	vfs_meta_changed();

	/* (Still held: finished when the hold is dropped.) */
	if (r != 0 && r != -EBUSY && r != -EINTR)
		kprintf("vfs: lazy unmount of %s could not finish (%d); the "
			"filesystem stays detached\n",
			m->path, r);
}

/* ---- mount(2) -------------------------------------------------------------- */

/* A mount(2) is about to resolve its mount point: from here until
 * mnt_setup_end() every rename takes g_mnt_mutex (which the mount holds),
 * and the renames that started without it are waited out -- so the mount
 * point cannot be renamed between being resolved and being entered.  Under
 * g_mnt_mutex. */
static void mnt_setup_begin(void)
{
	__atomic_store_n(&g_mount_setup, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&g_unlocked_renames, __ATOMIC_SEQ_CST) != 0) {
		if (hrtimer_sleep_until(hrtimer_now_ns() + 1000000ULL, NULL) ==
		    -EINTR)
			sched_yield_in_kernel();
	}
}

static void mnt_setup_end(void)
{
	__atomic_store_n(&g_mount_setup, 0, __ATOMIC_SEQ_CST);
}

/* Under g_mnt_mutex. */
static int mnt_remount(const char *target, unsigned long flags,
		       const char *data)
{
	int r = 0;

	vfs_mount_t *m = mnt_find_exact(target);
	if (!m) {
		r = -EINVAL;
		goto out;
	}
	unsigned long nf = flags & VFS_MS_PERMANENT;
	int ro_change = ((nf ^ m->flags) & VFS_MS_RDONLY) != 0;
	/* A write-protected device stays mounted read-only. */
	if (ro_change && !(nf & VFS_MS_RDONLY) && m->bdev &&
	    blk_bdev_read_only(m->bdev)) {
		r = -EROFS;
		goto out;
	}
	/* Going read-only while a file is open for writing would leave its
	 * writes going on: refused, whatever the filesystem (one that has no
	 * remount of its own, like tmpfs, would not notice). */
	if (ro_change && (nf & VFS_MS_RDONLY) &&
	    __atomic_load_n(&m->open_writers, __ATOMIC_SEQ_CST) > 0) {
		r = -EBUSY;
		goto out;
	}
	if (m->type && m->type->remount) {
		/* Going read-only stores what is pending first. */
		if (ro_change && (nf & VFS_MS_RDONLY))
			mnt_flush(m);
		int s = m->type->remount(m, nf, data);
		if (s != ST_OK) {
			r = vfs_status_to_errno(s);
			goto out;
		}
	} else if (ro_change) {
		/* Nothing to make the filesystem itself read-only with. */
		r = -EINVAL;
		goto out;
	}
	uint64_t fl;
	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	m->flags = nf;
	if (data && data[0])
		mnt_strcpy(m->options, data, sizeof(m->options));
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
	vfs_meta_changed();
out:
	return r;
}

static int vfs_mount_in(const char *source, const char *target,
			const char *fstype, unsigned long flags,
			const char *data, char *tpath);

int vfs_mount(const char *source, const char *target, const char *fstype,
	      unsigned long flags, const char *data)
{
	char *tpath = pathbuf_get(); /* the mount point, links resolved */
	int r;

	if (!tpath)
		return -ENOMEM;
	r = vfs_mount_in(source, target, fstype, flags, data, tpath);
	pathbuf_put(tpath);
	return r;
}

static int vfs_mount_in(const char *source, const char *target,
			const char *fstype, unsigned long flags,
			const char *data, char *tpath)
{
	const vfs_fstype_t *type = NULL;
	int is_auto = 0;
	int r;

	if (!target || target[0] != '/')
		return -EINVAL;
	if (!(flags & VFS_MS_REMOUNT)) {
		if (flags & (VFS_MS_BIND | VFS_MS_MOVE))
			return -EINVAL;
		if (!fstype || !fstype[0])
			return -EINVAL;
		if (mnt_is_kernel_fs(fstype))
			return -EBUSY; /* mounted by the kernel, once */
		is_auto = mnt_streq(fstype, "auto");
		if (!is_auto) {
			type = mnt_find_type(fstype);
			if (!type)
				return -ENODEV;
		}
	}

	mutex_lock(&g_mnt_mutex);
	mnt_setup_begin();
	block_device_t *bdev = NULL;
	vfs_mount_t *m = NULL;
	int claimed = 0;

	r = mnt_realpath(target, tpath, PATHBUF_SIZE);
	if (r != 0)
		goto fail;
	if (flags & VFS_MS_REMOUNT) {
		r = mnt_remount(tpath, flags, data);
		goto fail; /* nothing to undo: the same exit */
	}

	/* The mount point: an existing directory. */
	{
		struct kstat tst;
		int sr = vfs_stat(tpath, &tst);

		if (sr != ST_OK) {
			r = vfs_status_to_errno(sr);
			goto fail;
		}
		if (!S_ISDIR(tst.st_mode)) {
			r = -ENOTDIR;
			goto fail;
		}
	}

	if (mnt_find_exact(tpath)) {
		r = -EBUSY;
		goto fail;
	}
	/* Not inside the kernel's own pseudo filesystems (/dev, /proc,
	 * /sys): their drivers take absolute paths, and opens below /dev go
	 * to devfs by name in the syscall layer, so a filesystem mounted
	 * there would be reached by some calls and bypassed by others. */
	{
		vfs_mount_t *under = vfs_mnt_get(tpath, NULL);
		int kernel_fs = under && under->pinned && under->full_paths;

		vfs_mnt_put(under);
		if (kernel_fs) {
			r = -EINVAL;
			goto fail;
		}
	}
	if (is_auto || type->needs_device) {
		r = mnt_source_bdev(source, &bdev);
		if (r != 0)
			goto fail;
	}
	if (is_auto) {
		for (int i = 0; i < g_nfstypes && !type; i++)
			if (g_fstypes[i]->needs_device && g_fstypes[i]->probe &&
			    g_fstypes[i]->probe(bdev))
				type = g_fstypes[i];
		if (!type) {
			r = -EINVAL; /* nothing recognised on the device */
			goto fail;
		}
		fstype = type->name;
	}
	if (bdev && !(flags & VFS_MS_RDONLY) && blk_bdev_read_only(bdev)) {
		r = -EROFS;
		goto fail;
	}

	m = slot_alloc();
	if (!m) {
		r = -ENOMEM;
		goto fail;
	}
	if (bdev) {
		int c = blk_bdev_claim(bdev, m);
		if (c) {
			r = c;
			goto fail;
		}
		claimed = 1;
	}

	mnt_strcpy(m->path, tpath, VFS_MAX_PATH);
	m->path_len = mnt_strlen(m->path);
	mnt_strcpy(m->source, source && source[0] ? source : "none",
		   VFS_MAX_PATH);
	/* The aliases of a FAT driver are one type to everyone else. */
	if (mnt_streq(type->name, "vfat"))
		mnt_strcpy(m->fstype, "vfat", sizeof(m->fstype));
	else
		mnt_strcpy(m->fstype, fstype, sizeof(m->fstype));
	m->flags = flags & VFS_MS_PERMANENT;
	if (data)
		mnt_strcpy(m->options, data, sizeof(m->options));
	m->id = mnt_take_id();
	{
		vfs_mount_t *parent = vfs_mnt_get(tpath, NULL);
		m->parent_id = parent ? parent->id : 1;
		vfs_mnt_put(parent);
	}
	m->bdev = bdev;
	m->dev = bdev ? kdev_encode(bdev->major, bdev->minor) :
			kdev_encode(0, 128 + (uint32_t)m->id);
	m->dev_override = 1;
	m->type = type;

	int s = type->mount(m, bdev, m->flags, data);
	if (s != ST_OK) {
		r = vfs_status_to_errno(s);
		if (s == ST_NOT_FOUND || s == ST_UNSUPPORTED)
			r = -EINVAL; /* wrong or unsupported filesystem */
		goto fail;
	}
	if (!m->ops) {
		WARN_ON(1);
		m->type->unmount(m);
		r = -EINVAL;
		goto fail;
	}
	slot_publish(m);
	vfs_meta_changed();
	mnt_setup_end();
	mnt_unlock();
	kprintf("vfs: mounted %s (%s) on %s%s\n", m->source, m->fstype, m->path,
		(m->flags & VFS_MS_RDONLY) ? " read-only" : "");
	return 0;

fail:
	if (m)
		slot_free(m);
	if (claimed)
		blk_bdev_unclaim(bdev, m);
	if (bdev)
		blk_bdev_put(bdev);
	mnt_setup_end();
	mnt_unlock();
	return r;
}

/* ---- umount2(2) ------------------------------------------------------------ */

static int vfs_umount_in(const char *target, int flags, char *tpath);

int vfs_umount(const char *target, int flags)
{
	char *tpath = pathbuf_get(); /* the mount point, links resolved */
	int r;

	if (!tpath)
		return -ENOMEM;
	r = vfs_umount_in(target, flags, tpath);
	pathbuf_put(tpath);
	return r;
}

static int vfs_umount_in(const char *target, int flags, char *tpath)
{
	int r;

	if (!target || target[0] != '/')
		return -EINVAL;
	if (flags & ~(VFS_MNT_FORCE | VFS_MNT_DETACH | VFS_MNT_EXPIRE |
		      VFS_UMOUNT_NOFOLLOW))
		return -EINVAL;
	if ((flags & VFS_MNT_EXPIRE) && (flags & (VFS_MNT_FORCE | VFS_MNT_DETACH)))
		return -EINVAL;

	mutex_lock(&g_mnt_mutex);
	/* Resolved under the mutex, which a rename moving a mount point
	 * holds too: the name cannot change between being resolved and
	 * being looked up.  A mount point whose filesystem cannot answer
	 * for its links any more is still unmounted under the name it was
	 * given. */
	if ((flags & VFS_UMOUNT_NOFOLLOW) ||
	    mnt_realpath(target, tpath, PATHBUF_SIZE) != 0)
		mnt_strcpy(tpath, target, PATHBUF_SIZE);
	vfs_mount_t *m = mnt_find_exact(tpath);
	if (!m) {
		r = -EINVAL; /* not a mount point */
		goto out;
	}
	if (m->pinned || !m->type) {
		r = -EBUSY; /* the root and the kernel's own filesystems */
		goto out;
	}
	if (flags & VFS_MNT_EXPIRE) {
		/* Never used since mounted is not something tracked here. */
		r = -EAGAIN;
		goto out;
	}
	if (mnt_has_children(m)) {
		r = -EBUSY;
		goto out;
	}
	int detach = (flags & VFS_MNT_DETACH) != 0;
	if (!detach &&
	    (__atomic_load_n(&m->open_files, __ATOMIC_SEQ_CST) > 0 ||
	     mnt_tasks_inside(m))) {
		r = -EBUSY;
		goto out;
	}

	if (detach) {
		/* Out of the name space now; torn down once the files open
		 * on it are closed and the operations running on it have
		 * finished, by whichever of those ends last
		 * (mnt_finish_detached).  Either may already have: then it
		 * found the slot still LIVE and left it, so look here. */
		slot_set_state(m, VFS_MNT_DETACHED);
		vfs_meta_changed();
		if (__atomic_load_n(&m->open_files, __ATOMIC_SEQ_CST) == 0 &&
		    __atomic_load_n(&m->active, __ATOMIC_SEQ_CST) == 0)
			mnt_teardown_detached(m);
		r = 0;
		goto out;
	}

	/* No new operation can start on it; wait for the running ones, for
	 * a while.  One that does not finish (an open of a FIFO waiting for
	 * its other end, say) makes the mount busy. */
	slot_set_state(m, VFS_MNT_DYING);
	r = mnt_drain(m, VFS_UMOUNT_DRAIN_MS);
	if (r != 0) {
		slot_set_state(m, VFS_MNT_LIVE);
		goto out;
	}

	/* An open that was in flight may have completed meanwhile. */
	if (__atomic_load_n(&m->open_files, __ATOMIC_SEQ_CST) > 0) {
		slot_set_state(m, VFS_MNT_LIVE);
		r = -EBUSY;
		goto out;
	}
	/* The slot is DYING: its path stays as it is until the teardown;
	 * `tpath' names the same mount point (mnt_find_exact) and is ours. */
	r = mnt_teardown(m);
	vfs_meta_changed();
	if (r != 0)
		slot_set_state(m, VFS_MNT_LIVE);
	else
		kprintf("vfs: unmounted %s\n", tpath);
out:
	mnt_unlock();
	return r;
}

/* ---- sync ------------------------------------------------------------------ */

static void mnt_sync_one(vfs_mount_t *m, void *arg)
{
	(void)arg;
	if (m->ops && m->ops->sync)
		m->ops->sync(m);
	/* The root's device is synced with the page cache (pagecache_sync);
	 * every other filesystem's here. */
	if (m != g_mnt_root && m->bdev && m->bdev->sync) {
		if (m->sb)
			vfs_sb_lock_io(m->sb);
		m->bdev->sync(m->bdev);
		if (m->sb)
			vfs_sb_unlock_io(m->sb);
	}
}

int vfs_sync(void)
{
	/* The lazily unmounted ones too: out of the name space, but their
	 * open files still write to them.  (One being unmounted stores
	 * everything itself.) */
	vfs_mnt_for_each_instance(mnt_sync_one, NULL, 0);
	return ST_OK;
}

int vfs_syncfs(vfs_file_t *f)
{
	vfs_mount_t *m;
	int held = 0;

	if (!f)
		return ST_INVALID;
	m = f->mnt;
	if (!m) {
		/* A handle made outside vfs_open: its path names the mount. */
		if (!f->at_path)
			return ST_OK;
		m = vfs_mnt_get(f->at_path, NULL);
		if (!m)
			return ST_OK;
		held = 1;
	}
	mnt_flush(m);
	if (held)
		vfs_mnt_put(m);
	return ST_OK;
}

/* ---- /proc/mounts ---------------------------------------------------------- */

static long mnt_put(char *buf, long cap, long pos, const char *s, int esc)
{
	for (; s && *s; s++) {
		char c = *s;

		if (esc && (c == ' ' || c == '\t' || c == '\n' || c == '\\')) {
			char o[4];
			o[0] = '\\';
			o[1] = (char)('0' + ((c >> 6) & 7));
			o[2] = (char)('0' + ((c >> 3) & 7));
			o[3] = (char)('0' + (c & 7));
			for (int i = 0; i < 4; i++, pos++)
				if (pos < cap)
					buf[pos] = o[i];
			continue;
		}
		if (pos < cap)
			buf[pos] = c;
		pos++;
	}
	return pos;
}

static long mnt_put_num(char *buf, long cap, long pos, unsigned long v)
{
	char t[24];
	int n = 0;

	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n--) {
		if (pos < cap)
			buf[pos] = t[n];
		pos++;
	}
	return pos;
}

/* The generic option words for a mount's flags into `out'. */
static void mnt_flag_words(unsigned long fl, char *out, unsigned cap)
{
	static const struct {
		unsigned long bit;
		const char *word;
	} w[] = {
		{ VFS_MS_NOSUID, "nosuid" },	   { VFS_MS_NODEV, "nodev" },
		{ VFS_MS_NOEXEC, "noexec" },	   { VFS_MS_SYNCHRONOUS, "sync" },
		{ VFS_MS_DIRSYNC, "dirsync" },	   { VFS_MS_MANDLOCK, "mand" },
		{ VFS_MS_NOSYMFOLLOW, "nosymfollow" },
		{ VFS_MS_NOATIME, "noatime" },	   { VFS_MS_NODIRATIME, "nodiratime" },
		{ VFS_MS_RELATIME, "relatime" },   { VFS_MS_STRICTATIME, "strictatime" },
		{ VFS_MS_LAZYTIME, "lazytime" },
	};
	unsigned n = 0;
	const char *rw = (fl & VFS_MS_RDONLY) ? "ro" : "rw";

	for (; *rw && n + 1 < cap; rw++)
		out[n++] = *rw;
	for (unsigned i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
		if (!(fl & w[i].bit))
			continue;
		if (n + 1 < cap)
			out[n++] = ',';
		for (const char *c = w[i].word; *c && n + 1 < cap; c++)
			out[n++] = *c;
	}
	out[n] = '\0';
}

/* A consistent copy of one live slot, taken under the slot lock. */
struct mnt_snap {
	int id, parent_id;
	unsigned long flags;
	uint64_t dev;
	const char *show_flags;
	const vfs_fstype_t *type;
	vfs_mount_t *m;
	char path[VFS_MAX_PATH];
	char source[VFS_MAX_PATH];
	char fstype[VFS_FSTYPE_MAX];
	char options[VFS_MOUNT_OPTS_MAX];
};

static int mnt_snap_by_rank(int after_id, struct mnt_snap *s)
{
	uint64_t fl;
	int best = -1;
	int hi = __atomic_load_n(&g_mnt_hi, __ATOMIC_ACQUIRE);

	spin_lock_irqsave(&g_mnt_slot_lock, &fl);
	for (int i = 0; i < hi; i++) {
		vfs_mount_t *m = &g_mnt[i];

		if (m->state != VFS_MNT_LIVE || m->id <= after_id)
			continue;
		if (best < 0 || m->id < g_mnt[best].id)
			best = i;
	}
	if (best >= 0) {
		vfs_mount_t *m = &g_mnt[best];
		s->id = m->id;
		s->parent_id = m->parent_id;
		s->flags = m->flags;
		s->dev = m->dev;
		s->show_flags = m->show_flags;
		s->type = m->type;
		s->m = m;
		mnt_strcpy(s->path, m->path, sizeof(s->path));
		mnt_strcpy(s->source, m->source, sizeof(s->source));
		mnt_strcpy(s->fstype, m->fstype, sizeof(s->fstype));
		mnt_strcpy(s->options, m->options, sizeof(s->options));
	}
	spin_unlock_irqrestore(&g_mnt_slot_lock, fl);
	return best >= 0;
}

long vfs_mounts_show(char *buf, long cap, long pos, int info)
{
	struct mnt_snap *s = kalloc(sizeof(*s));
	int after = 0;

	if (!s)
		return pos;
	/* Mount ids grow with each mount and the root has 1: walking them in
	 * order lists the root first and every mount after the one it is
	 * mounted on. */
	while (mnt_snap_by_rank(after, s)) {
		char words[96], fsopts[VFS_MOUNT_OPTS_MAX];

		after = s->id;
		mnt_flag_words(s->flags, words, sizeof(words));
		fsopts[0] = '\0';
		/* The filesystem's own options: from the driver when it can
		 * say them (it holds the mount meanwhile), else as given. */
		if (s->type && s->type->show_options) {
			vfs_mount_t *h = vfs_mnt_get(s->path, NULL);
			if (h == s->m) {
				long n = s->type->show_options(h, fsopts,
							       sizeof(fsopts));
				if (n < 0 || n >= (long)sizeof(fsopts))
					n = 0;
				fsopts[n] = '\0';
			}
			vfs_mnt_put(h);
		} else {
			mnt_strcpy(fsopts, s->options, sizeof(fsopts));
		}
		const char *flagtext = s->show_flags ? s->show_flags : words;

		if (!info) {
			pos = mnt_put(buf, cap, pos, s->source, 1);
			pos = mnt_put(buf, cap, pos, " ", 0);
			pos = mnt_put(buf, cap, pos, s->path, 1);
			pos = mnt_put(buf, cap, pos, " ", 0);
			pos = mnt_put(buf, cap, pos, s->fstype, 1);
			pos = mnt_put(buf, cap, pos, " ", 0);
			pos = mnt_put(buf, cap, pos, flagtext, 0);
			if (fsopts[0]) {
				pos = mnt_put(buf, cap, pos, ",", 0);
				pos = mnt_put(buf, cap, pos, fsopts, 1);
			}
			pos = mnt_put(buf, cap, pos, " 0 0\n", 0);
		} else {
			pos = mnt_put_num(buf, cap, pos, (unsigned long)s->id);
			pos = mnt_put(buf, cap, pos, " ", 0);
			pos = mnt_put_num(buf, cap, pos,
					  (unsigned long)s->parent_id);
			pos = mnt_put(buf, cap, pos, " ", 0);
			pos = mnt_put_num(buf, cap, pos, kdev_major(s->dev));
			pos = mnt_put(buf, cap, pos, ":", 0);
			pos = mnt_put_num(buf, cap, pos, kdev_minor(s->dev));
			pos = mnt_put(buf, cap, pos, " / ", 0);
			pos = mnt_put(buf, cap, pos, s->path, 1);
			pos = mnt_put(buf, cap, pos, " ", 0);
			pos = mnt_put(buf, cap, pos, flagtext, 0);
			pos = mnt_put(buf, cap, pos, " shared:", 0);
			pos = mnt_put_num(buf, cap, pos, (unsigned long)s->id);
			pos = mnt_put(buf, cap, pos, " - ", 0);
			pos = mnt_put(buf, cap, pos, s->fstype, 1);
			pos = mnt_put(buf, cap, pos, " ", 0);
			pos = mnt_put(buf, cap, pos, s->source, 1);
			pos = mnt_put(buf, cap, pos,
				      (s->flags & VFS_MS_RDONLY) ? " ro" : " rw",
				      0);
			if (fsopts[0]) {
				pos = mnt_put(buf, cap, pos, ",", 0);
				pos = mnt_put(buf, cap, pos, fsopts, 1);
			}
			pos = mnt_put(buf, cap, pos, "\n", 0);
		}
	}
	kfree(s);
	return pos;
}

int vfs_path_dev(const char *path, uint64_t *dev)
{
	char *cbuf = pathbuf_get();
	vfs_mount_t *m;

	if (!cbuf)
		return ST_NOMEM;
	mnt_resolved(path, cbuf);
	m = vfs_mnt_get(cbuf, NULL);
	pathbuf_put(cbuf);
	if (!m)
		return ST_NOT_FOUND;
	*dev = m->dev;
	vfs_mnt_put(m);
	return ST_OK;
}

/* ---- syscalls -------------------------------------------------------------- */

/* A user string of at most cap - 1 bytes into buf; NULL user pointer gives
 * an empty string when `optional'.  0 or -errno. */
static int mnt_copy_str(uint64_t uptr, char *buf, size_t cap, int optional)
{
	size_t len = 0;
	int r;

	buf[0] = '\0';
	if (!uptr)
		return optional ? 0 : -EFAULT;
	r = user_strnlen((const char *)uptr, cap, &len);
	if (r != 0)
		return r == -ENAMETOOLONG ? -EINVAL : r;
	if (len >= cap)
		return -EINVAL;
	r = copy_from_user(buf, (const void *)uptr, len);
	if (r != 0)
		return -EFAULT;
	buf[len] = '\0';
	return 0;
}

/* mount(source, target, fstype, flags, data) */
int64_t sys_mount(uint64_t u_source, uint64_t u_target, uint64_t u_fstype,
		  uint64_t flags, uint64_t u_data)
{
	if (!capable())
		return -EPERM;

	char *buf = kalloc(3 * VFS_MAX_PATH + 1024);
	if (!buf)
		return -ENOMEM;
	char *source = buf;
	char *target = buf + VFS_MAX_PATH;
	char *fstype = buf + 2 * VFS_MAX_PATH;
	char *data = buf + 3 * VFS_MAX_PATH;
	int r;

	/* The old magic number in the high half of the flags means nothing
	 * any more; drop it. */
	if ((flags & 0xffff0000UL) == 0xC0ED0000UL)
		flags &= 0xffffUL;

	r = copy_user_path((const char *)u_target, target, VFS_MAX_PATH);
	if (r == 0)
		r = canon_task_path(target, VFS_MAX_PATH);
	if (r == 0)
		r = mnt_copy_str(u_source, source, VFS_MAX_PATH, 1);
	/* A device path is resolved like any path; a label ("tmpfs", "none")
	 * is kept as written. */
	if (r == 0 && source[0] == '/')
		r = canon_task_path(source, VFS_MAX_PATH);
	if (r == 0)
		r = mnt_copy_str(u_fstype, fstype, VFS_MAX_PATH, 1);
	if (r == 0)
		r = mnt_copy_str(u_data, data, 1024, 1);
	if (r == 0)
		r = vfs_mount(source, target, fstype, (unsigned long)flags,
			      data[0] ? data : NULL);
	kfree(buf);
	return r;
}

/* umount2(target, flags) */
int64_t sys_umount2(uint64_t u_target, uint64_t flags)
{
	char *target;
	int64_t r;

	if (!capable())
		return -EPERM;
	r = user_path_get_canon(u_target, &target);
	if (r != 0)
		return r;
	r = vfs_umount(target, (int)flags);
	pathbuf_put(target);
	return r;
}

/* syncfs(fd) */
int64_t sys_syncfs(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	vfs_file_t *file = fdget(cur, (int)fd);

	if (!file)
		return -EBADF;
	if (!fd_is_special(file))
		vfs_syncfs(file);
	fdput(file);
	return 0;
}
