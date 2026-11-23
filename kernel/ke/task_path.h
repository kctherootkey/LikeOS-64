// LikeOS -- the paths a task keeps: working directory, chroot root and the
// executable it runs.
//
// Each is an immutable, reference-counted string (struct task_path) that the
// task points to; a NULL pointer means the default ("/" for the working
// directory, none for the root and the executable).  Changing one makes a new
// string and swaps the pointer; fork and clone share the parent's strings by
// reference -- every task still has a working directory of its own: a
// chdir() changes the calling task's only.  A path is up to VFS_MAX_PATH
// bytes, and keeping three of them inside every task_t would make each one
// 12 KB larger.
//
// Who may read what:
//   - a task reads its OWN paths with task_cwd()/task_root()/task_exe(), no
//     lock, no reference: only the task itself replaces them;
//   - anybody else (procfs, the mount busy check, getprocinfo, ptrace) takes
//     a counted reference with task_path_ref() and drops it with
//     task_path_put(); that reference keeps the string alive whatever the
//     task does meanwhile (chdir, exec, exit).
// The pointers change under one spinlock (task_path_set, the teardown); no
// allocation is made under it.  task_path_new() allocates (kalloc) and may
// fail: its callers (chdir, chroot, exec) fail with ENOMEM before changing
// anything.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_TASK_PATH_H
#define KERNEL_KE_TASK_PATH_H

#include <kernel/uapi/types.h>

struct task;

struct task_path {
	long refs;
	unsigned long len; /* strlen(s) */
	char s[]; /* NUL-terminated, never changed once made */
};

enum task_path_kind {
	TASK_PATH_CWD = 0,
	TASK_PATH_ROOT = 1,
	TASK_PATH_EXE = 2,
};

/* A new string holding `s' (one reference), or NULL: no memory. */
struct task_path *task_path_new(const char *s);
/* Drop a reference; NULL is fine. */
void task_path_put(struct task_path *p);

/* The calling task's own paths (t == sched_current(), or a task nobody else
 * can see yet): the working directory ("/" when unset), the chroot root (""
 * when none) and the executable ("" when none). */
const char *task_cwd(const struct task *t);
const char *task_root(const struct task *t);
const char *task_exe(const struct task *t);

/* Any task's path `kind', as a counted reference (task_path_put it), or NULL
 * when it has none.  The task_t must stay valid for the call (the caller's
 * own hold on it, or the task-list lock). */
struct task_path *task_path_ref(struct task *t, enum task_path_kind kind);

/* Replace the path `kind' of `t' -- the calling task, or one whose creator
 * is still setting it up -- with `np' (whose reference passes to the task;
 * NULL: back to the default).  The old one is dropped. */
void task_path_set(struct task *t, enum task_path_kind kind,
		   struct task_path *np);

/* fork/clone: right after the task_t copy, forget the parent's pointers
 * (nothing is held yet, so a creation that fails has nothing to give back);
 * then, once the child can no longer fail, take references on the parent's
 * strings.  Neither allocates. */
void task_paths_clear(struct task *child);
void task_paths_fork(struct task *child, const struct task *parent);

/* The task_t is going away: drop all three. */
void task_paths_release(struct task *t);

#endif /* KERNEL_KE_TASK_PATH_H */
