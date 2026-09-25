/*
 * Copyright (C) 2026 The LikeOS Project
 *
 * System V semaphore sets.
 *
 * One table of sets under one lock.  A set's identifier carries a
 * generation, so an identifier that named a removed set is refused rather
 * than reaching whatever set took the slot.  Waiters sleep on the set's
 * wait queue and every change to the set wakes them all: each looks at the
 * values again, and a set removed under a waiter answers EIDRM.  The undo
 * records hang off the set, one per process that made an operation with
 * SEM_UNDO; a process's records are applied when it ends, which is what
 * lets a lock held through a semaphore outlive a crash of its holder.
 */
#include <kernel/mm/sysv_sem.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/syscalls.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/dev/device.h> /* poll_notify_wq */

struct sem_undo {
	struct sem_undo *next;
	int tgid;
	int16_t adjust[SEM_MAX_PER_SET];
};

struct sem_entry {
	int val;
	int pid; /* the last process to operate on it */
};

struct sem_set {
	int used;
	int key;
	uint32_t seq; /* the generation of this slot */
	uint32_t uid, gid, cuid, cgid, mode;
	int nsems;
	struct sem_entry *sems;
	struct sem_undo *undos;
	int64_t otime, ctime;
	int nwaiters; /* tasks inside semop() on this set */
	struct wait_queue_head wq;
};

static struct sem_set g_sets[SEM_MAX_SETS];
static spinlock_t g_sem_lock;
static int g_sem_ready;
static uint32_t g_sem_seq = 1;

static void sem_init(void)
{
	if (g_sem_ready)
		return;
	spinlock_init(&g_sem_lock, "sysv_sem");
	for (int i = 0; i < SEM_MAX_SETS; i++)
		wq_head_init(&g_sets[i].wq, "sysv_sem_set");
	g_sem_ready = 1;
}

static int64_t now_seconds(void)
{
	return (int64_t)(hrtimer_now_ns() / 1000000000ULL);
}

/* the identifier: the slot in the low bits, the generation above */
static int set_id(const struct sem_set *s)
{
	return (int)((s->seq << 8) | (uint32_t)(s - g_sets));
}

/* With the lock held: the set an identifier names, or NULL. */
static struct sem_set *set_by_id(int id)
{
	if (id < 0)
		return NULL;
	unsigned slot = (unsigned)id & 0xff;
	if (slot >= SEM_MAX_SETS)
		return NULL;
	struct sem_set *s = &g_sets[slot];
	if (!s->used || s->seq != ((uint32_t)id >> 8))
		return NULL;
	return s;
}

/* The System V permission test: the requested rwx triple, collapsed from
 * the three the caller names, against the triple that applies to it as
 * owner, group member or other.  Root passes. */
static int perm_ok(const struct sem_set *s, task_t *cur, unsigned flag)
{
	if (!cur)
		return 0;
	if (cred_is_root(&cur->cred))
		return 1;
	unsigned requested = ((flag >> 6) | (flag >> 3) | flag) & 07;
	unsigned granted = s->mode & 0777;
	if (cur->cred.euid == s->uid)
		granted >>= 6;
	else if (cred_in_group(&cur->cred, s->gid))
		granted >>= 3;
	return (requested & ~granted & 07) == 0;
}

static int owner_ok(const struct sem_set *s, task_t *cur)
{
	return cur && (cred_is_root(&cur->cred) || cur->cred.euid == s->uid ||
		       cur->cred.euid == s->cuid);
}

static void set_free_locked(struct sem_set *s)
{
	while (s->undos) {
		struct sem_undo *u = s->undos;
		s->undos = u->next;
		kfree(u);
	}
	kfree(s->sems);
	s->sems = NULL;
	s->used = 0;
}

int64_t sys_semget(uint64_t key_u, uint64_t nsems_u, uint64_t semflg_u)
{
	int key = (int)(int32_t)key_u;
	int nsems = (int)(int32_t)nsems_u;
	unsigned semflg = (unsigned)semflg_u;
	task_t *cur = sched_current();
	uint64_t fl;

	sem_init();
	if (nsems < 0 || nsems > SEM_MAX_PER_SET)
		return -EINVAL;
	spin_lock_irqsave(&g_sem_lock, &fl);
	if (key != IPC_PRIVATE) {
		for (int i = 0; i < SEM_MAX_SETS; i++) {
			struct sem_set *s = &g_sets[i];
			if (!s->used || s->key != key)
				continue;
			if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
				spin_unlock_irqrestore(&g_sem_lock, fl);
				return -EEXIST;
			}
			if (nsems > s->nsems) {
				spin_unlock_irqrestore(&g_sem_lock, fl);
				return -EINVAL;
			}
			if (!perm_ok(s, cur, semflg & 0777)) {
				spin_unlock_irqrestore(&g_sem_lock, fl);
				return -EACCES;
			}
			int id = set_id(s);
			spin_unlock_irqrestore(&g_sem_lock, fl);
			return id;
		}
		if (!(semflg & IPC_CREAT)) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			return -ENOENT;
		}
	}
	/* a new set: a size is required of it */
	if (nsems == 0) {
		spin_unlock_irqrestore(&g_sem_lock, fl);
		return -EINVAL;
	}
	struct sem_set *s = NULL;
	for (int i = 0; i < SEM_MAX_SETS; i++)
		if (!g_sets[i].used) {
			s = &g_sets[i];
			break;
		}
	if (!s) {
		spin_unlock_irqrestore(&g_sem_lock, fl);
		return -ENOSPC;
	}
	/* claim the slot before dropping the lock for the allocation */
	s->used = 1;
	s->nsems = 0;
	s->sems = NULL;
	s->undos = NULL;
	s->nwaiters = 0;
	spin_unlock_irqrestore(&g_sem_lock, fl);
	struct sem_entry *sems = kalloc((size_t)nsems * sizeof(*sems));
	spin_lock_irqsave(&g_sem_lock, &fl);
	if (!sems) {
		s->used = 0;
		spin_unlock_irqrestore(&g_sem_lock, fl);
		return -ENOMEM;
	}
	mm_memset(sems, 0, (size_t)nsems * sizeof(*sems));
	s->key = key;
	s->seq = g_sem_seq++;
	if (g_sem_seq == 0 || g_sem_seq >= (1u << 23))
		g_sem_seq = 1;
	s->uid = s->cuid = cur ? cur->cred.euid : 0;
	s->gid = s->cgid = cur ? cur->cred.egid : 0;
	s->mode = semflg & 0777;
	s->nsems = nsems;
	s->sems = sems;
	s->otime = 0;
	s->ctime = now_seconds();
	int id = set_id(s);
	spin_unlock_irqrestore(&g_sem_lock, fl);
	return id;
}

/* With the lock held: whether every operation can be applied now.  0 when
 * it can, 1 when one of them would have to wait, a negative errno for a
 * bad operation. */
static int ops_check(const struct sem_set *s, const struct k_sembuf *ops, unsigned n)
{
	/* applied on a copy of the values, since an array may touch one
	 * semaphore several times */
	int vals[SEM_MAX_PER_SET];
	for (int i = 0; i < s->nsems; i++)
		vals[i] = s->sems[i].val;
	for (unsigned i = 0; i < n; i++) {
		if (ops[i].sem_num >= s->nsems)
			return -EFBIG;
		int *v = &vals[ops[i].sem_num];
		if (ops[i].sem_op == 0) {
			if (*v != 0)
				return 1;
		} else if (ops[i].sem_op < 0) {
			if (*v + ops[i].sem_op < 0)
				return 1;
			*v += ops[i].sem_op;
		} else {
			if (*v + ops[i].sem_op > SEM_VALUE_MAX)
				return -ERANGE;
			*v += ops[i].sem_op;
		}
	}
	return 0;
}

/* With the lock held: the undo record of this process on this set, made
 * if there is none. */
static struct sem_undo *undo_for(struct sem_set *s, int tgid, struct sem_undo *spare)
{
	for (struct sem_undo *u = s->undos; u; u = u->next)
		if (u->tgid == tgid)
			return u;
	if (!spare)
		return NULL;
	mm_memset(spare, 0, sizeof(*spare));
	spare->tgid = tgid;
	spare->next = s->undos;
	s->undos = spare;
	return spare;
}

int64_t sys_semop(uint64_t semid_u, uint64_t sops_u, uint64_t nsops_u)
{
	int semid = (int)(int32_t)semid_u;
	unsigned nsops = (unsigned)nsops_u;
	struct k_sembuf ops[SEM_MAX_OPS];
	task_t *cur = sched_current();
	uint64_t fl;

	sem_init();
	if (nsops == 0)
		return -EINVAL;
	if (nsops > SEM_MAX_OPS)
		return -E2BIG;
	if (copy_from_user(ops, (const void *)(uintptr_t)sops_u, nsops * sizeof(ops[0])) != 0)
		return -EFAULT;
	int wants_undo = 0, writes = 0;
	for (unsigned i = 0; i < nsops; i++) {
		if (ops[i].sem_flg & SEM_UNDO)
			wants_undo = 1;
		if (ops[i].sem_op != 0)
			writes = 1;
	}
	/* an undo record may be needed; taken ahead of the lock */
	struct sem_undo *spare = wants_undo ? kalloc(sizeof(*spare)) : NULL;
	if (wants_undo && !spare)
		return -ENOMEM;
	int rc;
	for (;;) {
		spin_lock_irqsave(&g_sem_lock, &fl);
		struct sem_set *s = set_by_id(semid);
		if (!s) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			rc = -EINVAL;
			break;
		}
		if (!perm_ok(s, cur, writes ? 0222 : 0444)) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			rc = -EACCES;
			break;
		}
		int c = ops_check(s, ops, nsops);
		if (c < 0) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			rc = c;
			break;
		}
		if (c == 0) {
			struct sem_undo *u = NULL;
			if (wants_undo) {
				u = undo_for(s, cur ? cur->tgid : 0, spare);
				if (u == spare)
					spare = NULL;
			}
			for (unsigned i = 0; i < nsops; i++) {
				struct sem_entry *e = &s->sems[ops[i].sem_num];
				e->val += ops[i].sem_op;
				e->pid = cur ? cur->tgid : 0;
				if ((ops[i].sem_flg & SEM_UNDO) && u)
					u->adjust[ops[i].sem_num] -= ops[i].sem_op;
			}
			s->otime = now_seconds();
			int wake = s->nwaiters > 0 && writes;
			struct wait_queue_head *wq = &s->wq;
			spin_unlock_irqrestore(&g_sem_lock, fl);
			if (wake)
				poll_notify_wq(wq);
			rc = 0;
			break;
		}
		/* something has to wait */
		int nowait = 0;
		for (unsigned i = 0; i < nsops; i++)
			if (ops[i].sem_flg & IPC_NOWAIT)
				nowait = 1;
		if (nowait) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			rc = -EAGAIN;
			break;
		}
		if (cur && signal_pending(cur)) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			rc = -EINTR;
			break;
		}
		/* Asleep on the set until it changes.  Blocked is set while the
		 * lock is still held, so a change made after this check is a
		 * change made after the wait began, and its wake reaches us. */
		struct wait_queue_entry we;
		wq_entry_init(&we, cur);
		wq_add(&s->wq, &we);
		s->nwaiters++;
		uint32_t seq = s->seq;
		unsigned slot = (unsigned)(s - g_sets);
		cur->wait_channel = s;
		cur->state = TASK_BLOCKED;
		spin_unlock_irqrestore(&g_sem_lock, fl);
		sched_schedule();
		cur->wait_channel = NULL;
		spin_lock_irqsave(&g_sem_lock, &fl);
		wq_remove(&g_sets[slot].wq, &we);
		/* the set may have been removed while we slept */
		if (!g_sets[slot].used || g_sets[slot].seq != seq) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			rc = -EIDRM;
			break;
		}
		g_sets[slot].nwaiters--;
		spin_unlock_irqrestore(&g_sem_lock, fl);
	}
	if (spare)
		kfree(spare);
	return rc;
}

/* The counts the GETNCNT / GETZCNT commands report: how many waiters
 * are blocked because this semaphore is too small, or not zero.  Found
 * by asking the sleepers what they wait for would need each sleeper's
 * operations kept; the count of tasks in semop() on the set is what is
 * known, attributed to the semaphore asked about when it cannot satisfy
 * a wait of that kind. */
static int waiter_count(const struct sem_set *s, int semnum, int zero)
{
	if (s->nwaiters == 0)
		return 0;
	int v = s->sems[semnum].val;
	if (zero)
		return v != 0 ? s->nwaiters : 0;
	return v == 0 ? s->nwaiters : 0;
}

int64_t sys_semctl(uint64_t semid_u, uint64_t semnum_u, uint64_t cmd_u, uint64_t arg)
{
	int semid = (int)(int32_t)semid_u;
	int semnum = (int)(int32_t)semnum_u;
	int cmd = (int)(int32_t)cmd_u;
	task_t *cur = sched_current();
	uint64_t fl;

	sem_init();
	spin_lock_irqsave(&g_sem_lock, &fl);
	struct sem_set *s = set_by_id(semid);
	if (!s) {
		spin_unlock_irqrestore(&g_sem_lock, fl);
		return -EINVAL;
	}
	switch (cmd) {
	case IPC_STAT: {
		if (!perm_ok(s, cur, 0444)) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			return -EACCES;
		}
		struct k_semid_ds ds;
		mm_memset(&ds, 0, sizeof(ds));
		ds.sem_perm.key = s->key;
		ds.sem_perm.uid = s->uid;
		ds.sem_perm.gid = s->gid;
		ds.sem_perm.cuid = s->cuid;
		ds.sem_perm.cgid = s->cgid;
		ds.sem_perm.mode = s->mode;
		ds.sem_perm.seq = s->seq;
		ds.sem_otime = s->otime;
		ds.sem_ctime = s->ctime;
		ds.sem_nsems = (uint64_t)s->nsems;
		spin_unlock_irqrestore(&g_sem_lock, fl);
		if (copy_to_user((void *)(uintptr_t)arg, &ds, sizeof(ds)) != 0)
			return -EFAULT;
		return 0;
	}
	case IPC_SET: {
		if (!owner_ok(s, cur)) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			return -EPERM;
		}
		spin_unlock_irqrestore(&g_sem_lock, fl);
		struct k_semid_ds ds;
		if (copy_from_user(&ds, (const void *)(uintptr_t)arg, sizeof(ds)) != 0)
			return -EFAULT;
		spin_lock_irqsave(&g_sem_lock, &fl);
		if (set_by_id(semid) != s) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			return -EIDRM;
		}
		s->uid = ds.sem_perm.uid;
		s->gid = ds.sem_perm.gid;
		s->mode = ds.sem_perm.mode & 0777;
		s->ctime = now_seconds();
		spin_unlock_irqrestore(&g_sem_lock, fl);
		return 0;
	}
	case IPC_RMID: {
		if (!owner_ok(s, cur)) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			return -EPERM;
		}
		struct wait_queue_head *wq = &s->wq;
		int wake = s->nwaiters > 0;
		set_free_locked(s);
		spin_unlock_irqrestore(&g_sem_lock, fl);
		if (wake)
			poll_notify_wq(wq); /* they find the set gone: EIDRM */
		return 0;
	}
	case SEM_GETALL:
	case SEM_SETALL: {
		unsigned need = cmd == SEM_GETALL ? 0444 : 0222;
		if (!perm_ok(s, cur, need)) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			return -EACCES;
		}
		uint16_t vals[SEM_MAX_PER_SET];
		int n = s->nsems;
		if (cmd == SEM_GETALL) {
			for (int i = 0; i < n; i++)
				vals[i] = (uint16_t)s->sems[i].val;
			spin_unlock_irqrestore(&g_sem_lock, fl);
			if (copy_to_user((void *)(uintptr_t)arg, vals, (size_t)n * sizeof(vals[0])) != 0)
				return -EFAULT;
			return 0;
		}
		spin_unlock_irqrestore(&g_sem_lock, fl);
		if (copy_from_user(vals, (const void *)(uintptr_t)arg, (size_t)n * sizeof(vals[0])) != 0)
			return -EFAULT;
		for (int i = 0; i < n; i++)
			if (vals[i] > SEM_VALUE_MAX)
				return -ERANGE;
		spin_lock_irqsave(&g_sem_lock, &fl);
		if (set_by_id(semid) != s || s->nsems != n) {
			spin_unlock_irqrestore(&g_sem_lock, fl);
			return -EIDRM;
		}
		for (int i = 0; i < n; i++) {
			s->sems[i].val = vals[i];
			s->sems[i].pid = cur ? cur->tgid : 0;
		}
		/* what was set outright is not undone later */
		for (struct sem_undo *u = s->undos; u; u = u->next)
			for (int i = 0; i < n; i++)
				u->adjust[i] = 0;
		s->ctime = now_seconds();
		int wake = s->nwaiters > 0;
		struct wait_queue_head *wq = &s->wq;
		spin_unlock_irqrestore(&g_sem_lock, fl);
		if (wake)
			poll_notify_wq(wq);
		return 0;
	}
	default:
		break;
	}
	/* the rest name one semaphore */
	if (semnum < 0 || semnum >= s->nsems) {
		spin_unlock_irqrestore(&g_sem_lock, fl);
		return -EINVAL;
	}
	int64_t rc;
	switch (cmd) {
	case SEM_GETVAL:
	case SEM_GETPID:
	case SEM_GETNCNT:
	case SEM_GETZCNT:
		if (!perm_ok(s, cur, 0444)) {
			rc = -EACCES;
			break;
		}
		if (cmd == SEM_GETVAL)
			rc = s->sems[semnum].val;
		else if (cmd == SEM_GETPID)
			rc = s->sems[semnum].pid;
		else
			rc = waiter_count(s, semnum, cmd == SEM_GETZCNT);
		break;
	case SEM_SETVAL: {
		int val = (int)(int32_t)(arg & 0xffffffffu);
		if (!perm_ok(s, cur, 0222)) {
			rc = -EACCES;
			break;
		}
		if (val < 0 || val > SEM_VALUE_MAX) {
			rc = -ERANGE;
			break;
		}
		s->sems[semnum].val = val;
		s->sems[semnum].pid = cur ? cur->tgid : 0;
		for (struct sem_undo *u = s->undos; u; u = u->next)
			u->adjust[semnum] = 0;
		s->ctime = now_seconds();
		int wake = s->nwaiters > 0;
		struct wait_queue_head *wq = &s->wq;
		spin_unlock_irqrestore(&g_sem_lock, fl);
		if (wake)
			poll_notify_wq(wq);
		return 0;
	}
	default:
		rc = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&g_sem_lock, fl);
	return rc;
}

void sysv_sem_exit(struct task *task)
{
	uint64_t fl;
	struct wait_queue_head *wake[SEM_MAX_SETS];
	int nwake = 0;

	if (!g_sem_ready || !task)
		return;
	int tgid = task->tgid;
	spin_lock_irqsave(&g_sem_lock, &fl);
	for (int i = 0; i < SEM_MAX_SETS; i++) {
		struct sem_set *s = &g_sets[i];
		if (!s->used)
			continue;
		struct sem_undo **pp = &s->undos;
		while (*pp) {
			struct sem_undo *u = *pp;
			if (u->tgid != tgid) {
				pp = &u->next;
				continue;
			}
			int changed = 0;
			for (int k = 0; k < s->nsems; k++) {
				if (!u->adjust[k])
					continue;
				int v = s->sems[k].val + u->adjust[k];
				if (v < 0)
					v = 0;
				if (v > SEM_VALUE_MAX)
					v = SEM_VALUE_MAX;
				s->sems[k].val = v;
				s->sems[k].pid = tgid;
				changed = 1;
			}
			*pp = u->next;
			kfree(u);
			if (changed && s->nwaiters > 0 && nwake < SEM_MAX_SETS)
				wake[nwake++] = &s->wq;
		}
	}
	spin_unlock_irqrestore(&g_sem_lock, fl);
	for (int i = 0; i < nwake; i++)
		poll_notify_wq(wake[i]);
}
