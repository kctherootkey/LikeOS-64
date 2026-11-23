// LikeOS -- the paths a task keeps (see task_path.h).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/task_path.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

/* Every task's three pointers change under this one lock (rarely: chdir,
 * chroot, exec, fork, exit), and a reader takes its reference under it. */
static spinlock_t g_task_path_lock = SPINLOCK_INIT("task_path");

struct task_path *task_path_new(const char *s)
{
	unsigned long len = 0;
	struct task_path *p;

	while (s[len])
		len++;
	if (len >= VFS_MAX_PATH)
		return NULL; /* (no caller makes one: paths come from buffers) */
	p = (struct task_path *)kalloc(sizeof(*p) + len + 1);
	if (!p)
		return NULL;
	p->refs = 1;
	p->len = len;
	mm_memcpy(p->s, s, len + 1);
	return p;
}

void task_path_put(struct task_path *p)
{
	if (!p)
		return;
	long left = __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);

	WARN_ON(left < 0);
	if (left == 0)
		kfree(p);
}

static struct task_path **task_path_slot(struct task *t,
					 enum task_path_kind kind)
{
	switch (kind) {
	case TASK_PATH_CWD:
		return &t->fs_cwd;
	case TASK_PATH_ROOT:
		return &t->fs_root;
	default:
		return &t->fs_exe;
	}
}

/* (Own paths: only the task itself replaces them, so the pointer read here
 * stays valid for as long as the task uses the string.) */
const char *task_cwd(const struct task *t)
{
	const struct task_path *p =
		t ? __atomic_load_n(&t->fs_cwd, __ATOMIC_ACQUIRE) : NULL;

	return p ? p->s : "/";
}

const char *task_root(const struct task *t)
{
	const struct task_path *p =
		t ? __atomic_load_n(&t->fs_root, __ATOMIC_ACQUIRE) : NULL;

	return p ? p->s : "";
}

const char *task_exe(const struct task *t)
{
	const struct task_path *p =
		t ? __atomic_load_n(&t->fs_exe, __ATOMIC_ACQUIRE) : NULL;

	return p ? p->s : "";
}

struct task_path *task_path_ref(struct task *t, enum task_path_kind kind)
{
	struct task_path *p;
	uint64_t fl;

	if (!t)
		return NULL;
	spin_lock_irqsave(&g_task_path_lock, &fl);
	p = *task_path_slot(t, kind);
	if (p)
		__atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
	spin_unlock_irqrestore(&g_task_path_lock, fl);
	return p;
}

void task_path_set(struct task *t, enum task_path_kind kind,
		   struct task_path *np)
{
	struct task_path **slot = task_path_slot(t, kind), *old;
	uint64_t fl;

	spin_lock_irqsave(&g_task_path_lock, &fl);
	old = *slot;
	__atomic_store_n(slot, np, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&g_task_path_lock, fl);
	task_path_put(old);
}

void task_paths_clear(struct task *child)
{
	child->fs_cwd = NULL;
	child->fs_root = NULL;
	child->fs_exe = NULL;
}

void task_paths_fork(struct task *child, const struct task *parent)
{
	/* The parent is the calling task: its pointers hold still.  The child
	 * may already be visible to readers (elf_exec), hence the lock. */
	struct task_path *c = parent ? parent->fs_cwd : NULL;
	struct task_path *r = parent ? parent->fs_root : NULL;
	struct task_path *e = parent ? parent->fs_exe : NULL;
	struct task_path *oc, *or_, *oe;
	uint64_t fl;

	if (c)
		__atomic_add_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
	if (r)
		__atomic_add_fetch(&r->refs, 1, __ATOMIC_ACQ_REL);
	if (e)
		__atomic_add_fetch(&e->refs, 1, __ATOMIC_ACQ_REL);
	spin_lock_irqsave(&g_task_path_lock, &fl);
	oc = child->fs_cwd;
	or_ = child->fs_root;
	oe = child->fs_exe;
	__atomic_store_n(&child->fs_cwd, c, __ATOMIC_RELEASE);
	__atomic_store_n(&child->fs_root, r, __ATOMIC_RELEASE);
	__atomic_store_n(&child->fs_exe, e, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&g_task_path_lock, fl);
	task_path_put(oc);
	task_path_put(or_);
	task_path_put(oe);
}

void task_paths_release(struct task *t)
{
	struct task_path *c, *r, *e;
	uint64_t fl;

	if (!t)
		return;
	spin_lock_irqsave(&g_task_path_lock, &fl);
	c = t->fs_cwd;
	r = t->fs_root;
	e = t->fs_exe;
	t->fs_cwd = NULL;
	t->fs_root = NULL;
	t->fs_exe = NULL;
	spin_unlock_irqrestore(&g_task_path_lock, fl);
	task_path_put(c);
	task_path_put(r);
	task_path_put(e);
}
