/*
 * POSIX advisory record locks -- fcntl(F_GETLK / F_SETLK / F_SETLKW).
 *
 * Advisory, as POSIX specifies: a lock stops nothing by itself, it lets
 * processes that DO ask coordinate.  fontconfig is the first caller here -- it
 * takes a whole-file write lock around a font-cache rebuild so two clients
 * starting at once cannot interleave writes into the same cache file.
 *
 * Ownership is the PROCESS (tgid), not the descriptor and not the thread, which
 * is what POSIX requires: threads of one process share its locks, and a lock is
 * released when the process closes ANY descriptor for the file, or exits.
 *
 * Files are identified by (st_dev, st_ino) rather than by vfs_file pointer, so
 * two independent opens of the same file conflict with each other -- which is
 * the entire point.
 *
 * Ranges are held as inclusive [start, end], with end = FR_EOF standing for
 * "to end of file" (l_len == 0).  Locks are stored as given rather than merged:
 * merging adjacent ranges would save table slots but makes the unlock path much
 * harder to get right, and the callers here lock whole files.
 */
#include <kernel/fs/vfs.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>
#include <kernel/io/console.h>

/* Bounded and statically allocated: the lock path must not need to allocate,
 * and a process that leaks locks must not be able to exhaust kernel memory.
 * Exceeding it is reported as ENOLCK, which is what POSIX defines for "no locks
 * available" and what callers already handle. */
#define FRLOCK_MAX 128

struct frlock {
	int used;
	uint64_t dev;
	uint64_t ino;
	uint32_t pid; /* owning process (tgid) */
	int type; /* F_RDLCK or F_WRLCK */
	int64_t start; /* inclusive */
	int64_t end; /* inclusive; FR_EOF = to EOF */
};

static struct frlock g_locks[FRLOCK_MAX];
static spinlock_t g_lock = SPINLOCK_INIT("frlock");

/* No <stdint.h> limits here: the kernel builds -nostdinc. */
#define FR_EOF ((int64_t)0x7FFFFFFFFFFFFFFFLL)

static int ranges_overlap(int64_t s1, int64_t e1, int64_t s2, int64_t e2)
{
	return s1 <= e2 && s2 <= e1;
}

/* Does an existing record block `type` for `pid` over [start,end]?
 * Callers hold g_lock. */
static struct frlock *find_conflict(uint64_t dev, uint64_t ino, uint32_t pid,
				    int type, int64_t start, int64_t end)
{
	for (int i = 0; i < FRLOCK_MAX; i++) {
		struct frlock *l = &g_locks[i];
		if (!l->used || l->dev != dev || l->ino != ino)
			continue;
		if (l->pid == pid)
			continue; /* our own locks never conflict */
		if (!ranges_overlap(start, end, l->start, l->end))
			continue;
		/* Two read locks coexist; anything involving a write does not. */
		if (type == F_WRLCK || l->type == F_WRLCK)
			return l;
	}
	return NULL;
}

/* Drop this owner's records overlapping [start,end].  Whole records are removed
 * rather than split: every caller here locks and unlocks the same region, and a
 * partial unlock of a larger lock is better refused than done wrongly. */
static void remove_owned(uint64_t dev, uint64_t ino, uint32_t pid, int64_t start,
			 int64_t end)
{
	for (int i = 0; i < FRLOCK_MAX; i++) {
		struct frlock *l = &g_locks[i];
		if (!l->used || l->dev != dev || l->ino != ino || l->pid != pid)
			continue;
		if (ranges_overlap(start, end, l->start, l->end))
			l->used = 0;
	}
}

static int insert_lock(uint64_t dev, uint64_t ino, uint32_t pid, int type,
		       int64_t start, int64_t end)
{
	for (int i = 0; i < FRLOCK_MAX; i++) {
		if (!g_locks[i].used) {
			g_locks[i].used = 1;
			g_locks[i].dev = dev;
			g_locks[i].ino = ino;
			g_locks[i].pid = pid;
			g_locks[i].type = type;
			g_locks[i].start = start;
			g_locks[i].end = end;
			return 0;
		}
	}
	return -ENOLCK;
}

/* Turn an l_whence/l_start/l_len triple into an inclusive absolute range. */
static int resolve_range(vfs_file_t *f, const k_flock_t *fl, int64_t *start,
			 int64_t *end)
{
	int64_t base;

	switch (fl->l_whence) {
	case 0: /* SEEK_SET */
		base = 0;
		break;
	case 2: { /* SEEK_END */
		struct kstat st;
		if (vfs_fstat(f, &st) != ST_OK)
			return -EINVAL;
		base = (int64_t)st.st_size;
		break;
	}
	case 1: /* SEEK_CUR */
		/* Would need the descriptor's current offset, which the VFS does
		 * not expose to this layer.  Refused rather than guessed: a lock
		 * placed over the wrong range is worse than no lock, because the
		 * caller believes it is protected. */
		return -EINVAL;
	default:
		return -EINVAL;
	}

	if (fl->l_len == 0) {
		/* "to end of file", and it stays that way as the file grows. */
		*start = base + fl->l_start;
		*end = FR_EOF;
	} else if (fl->l_len > 0) {
		*start = base + fl->l_start;
		*end = *start + fl->l_len - 1;
	} else {
		/* POSIX: a negative length means the region BEFORE l_start. */
		*start = base + fl->l_start + fl->l_len;
		*end = base + fl->l_start - 1;
	}
	if (*start < 0 || *end < *start)
		return -EINVAL;
	return 0;
}

/* fcntl(fd, F_GETLK|F_SETLK|F_SETLKW, &flock), on an already-copied struct.
 *
 * The caller copies from and back to user memory, for two reasons: the copy
 * helpers are private to the syscall layer, and user memory must never be
 * touched while this file's spinlock is held -- a user page can be
 * demand-faulted, and faulting with interrupts disabled cannot sleep for the
 * page-in, which would wedge the CPU still holding the lock.
 *
 * On F_GETLK the struct is updated in place and the caller copies it back. */
int frlock_fcntl(vfs_file_t *f, int cmd, k_flock_t *flp, task_t *cur)
{
	k_flock_t fl;
	struct kstat st;
	uint64_t flags;
	int64_t start, end;
	uint32_t pid;
	int r;

	if (!f || !cur || !flp)
		return -EBADF;
	fl = *flp;
	if (vfs_fstat(f, &st) != ST_OK)
		return -EBADF;

	r = resolve_range(f, &fl, &start, &end);
	if (r != 0)
		return r;

	pid = (uint32_t)cur->tgid;

	if (cmd == F_GETLK) {
		struct frlock *c;
		int type = (fl.l_type == F_UNLCK) ? F_WRLCK : fl.l_type;
		spin_lock_irqsave(&g_lock, &flags);
		c = find_conflict(st.st_dev, st.st_ino, pid, type, start, end);
		if (c) {
			fl.l_type = (short)c->type;
			fl.l_whence = 0; /* answered in absolute terms */
			fl.l_start = c->start;
			fl.l_len = (c->end == FR_EOF) ? 0 :
						       c->end - c->start + 1;
			fl.l_pid = (int32_t)c->pid;
		} else {
			/* F_UNLCK in the reply is how "nothing blocks you" is
			 * spelled. */
			fl.l_type = F_UNLCK;
		}
		spin_unlock_irqrestore(&g_lock, flags);
		*flp = fl;
		return 0;
	}

	if (cmd != F_SETLK && cmd != F_SETLKW)
		return -EINVAL;

	if (fl.l_type == F_UNLCK) {
		spin_lock_irqsave(&g_lock, &flags);
		remove_owned(st.st_dev, st.st_ino, pid, start, end);
		spin_unlock_irqrestore(&g_lock, flags);
		return 0;
	}
	if (fl.l_type != F_RDLCK && fl.l_type != F_WRLCK)
		return -EINVAL;

	for (;;) {
		int busy;

		spin_lock_irqsave(&g_lock, &flags);
		busy = find_conflict(st.st_dev, st.st_ino, pid, fl.l_type, start,
				     end) != NULL;
		if (!busy) {
			/* Replace any of our own overlapping records, so
			 * upgrading a read lock to a write lock (or re-taking
			 * the same region) does not consume a second slot. */
			remove_owned(st.st_dev, st.st_ino, pid, start, end);
			r = insert_lock(st.st_dev, st.st_ino, pid, fl.l_type,
					start, end);
			spin_unlock_irqrestore(&g_lock, flags);
			return r;
		}
		spin_unlock_irqrestore(&g_lock, flags);

		if (cmd == F_SETLK)
			return -EAGAIN; /* POSIX: do not block */

		/* F_SETLKW waits.  A signal must break the wait, or a process
		 * blocked on a lock whose holder has wedged could not be killed
		 * -- and the holder's locks are dropped when it exits or closes
		 * the file, so the wait does end on its own in the normal case. */
		if (signal_pending(cur))
			return -EINTR;
		sched_yield_in_kernel();
	}
}

/* Release everything a process holds.  Called from the task-exit path: a lock
 * that outlived its owner would block every later process for good, and the
 * F_SETLKW loop above would spin on it forever. */
void frlock_release_for_task(uint32_t pid)
{
	uint64_t flags;

	spin_lock_irqsave(&g_lock, &flags);
	for (int i = 0; i < FRLOCK_MAX; i++)
		if (g_locks[i].used && g_locks[i].pid == pid)
			g_locks[i].used = 0;
	spin_unlock_irqrestore(&g_lock, flags);
}

/* Release a process's locks on one file.  POSIX: closing ANY descriptor for a
 * file drops that process's locks on it, even if other descriptors remain. */
void frlock_release_for_file(vfs_file_t *f, uint32_t pid)
{
	struct kstat st;
	uint64_t flags;

	if (!f || vfs_fstat(f, &st) != ST_OK)
		return;
	spin_lock_irqsave(&g_lock, &flags);
	for (int i = 0; i < FRLOCK_MAX; i++) {
		struct frlock *l = &g_locks[i];
		if (l->used && l->pid == pid && l->dev == st.st_dev &&
		    l->ino == st.st_ino)
			l->used = 0;
	}
	spin_unlock_irqrestore(&g_lock, flags);
}
