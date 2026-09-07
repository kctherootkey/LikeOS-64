/*
 * posix_spawn() and friends.
 *
 * Built on clone(CLONE_VM | CLONE_VFORK): the child runs in this process's
 * address space, on a small stack of its own, and the calling thread sleeps
 * until the child has exec'd or exited.  Nothing is copied.  That is the
 * whole point of the interface on a system where fork is not cheap: a fork of
 * a large threaded program walks every page-table entry it has, marks each
 * page copy-on-write and broadcasts an invalidation -- and every other thread
 * of the program that faults meanwhile waits the whole time -- only for the
 * copy to be thrown away at exec a few microseconds later.
 *
 * Sharing the address space is also what makes the error reporting free.  A
 * failed exec happens in the child; with fork it has to invent a channel back
 * (a close-on-exec pipe, reinvented by every program that cares), here the
 * child writes its errno into the caller's frame and the parent reads it when
 * clone() returns, which is not before the child is gone.
 *
 * The price is the vfork discipline: until exec, the child may touch nothing
 * the parent will look at afterwards, may not allocate, and may not take a
 * lock -- another thread of the parent may hold it.  Everything the child does
 * here is a plain system call on data prepared before the clone.
 *
 * Errors are RETURNED, not signalled through errno -- that is what POSIX
 * specifies for this family, and it differs from almost every other call here.
 */

#include <spawn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>      /* clone(), CLONE_VM, CLONE_VFORK */
#include <sys/mman.h>   /* the child's stack */
#include <sys/wait.h>   /* waitpid: reaping a child whose exec failed */

/* One entry in the file-actions list.  A linked list rather than an array so
 * that adding an action cannot invalidate the ones already added, and so the
 * list has no arbitrary length limit. */
enum spawn_op {
	SPAWN_OPEN,
	SPAWN_CLOSE,
	SPAWN_DUP2,
	SPAWN_CHDIR,
	SPAWN_FCHDIR,
};

struct __spawn_action {
	struct __spawn_action *next;
	enum spawn_op op;
	int fd;
	int newfd;
	int oflag;
	mode_t mode;
	char *path; /* owned; freed by _destroy */
};

/* ------------------------------------------------------------------ */
/* File actions                                                        */
/* ------------------------------------------------------------------ */

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *acts)
{
	if (!acts)
		return EINVAL;
	acts->__head = NULL;
	acts->__tail = NULL;
	return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *acts)
{
	struct __spawn_action *a, *next;

	if (!acts)
		return EINVAL;
	for (a = acts->__head; a; a = next) {
		next = a->next;
		free(a->path);
		free(a);
	}
	acts->__head = NULL;
	acts->__tail = NULL;
	return 0;
}

static int spawn_append(posix_spawn_file_actions_t *acts,
			struct __spawn_action *a)
{
	if (acts->__tail)
		acts->__tail->next = a;
	else
		acts->__head = a;
	acts->__tail = a;
	return 0;
}

static struct __spawn_action *spawn_new(enum spawn_op op)
{
	struct __spawn_action *a = calloc(1, sizeof(*a));

	if (a)
		a->op = op;
	return a;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *acts, int fd,
				     const char *path, int oflag, mode_t mode)
{
	struct __spawn_action *a;

	if (!acts || !path || fd < 0)
		return EINVAL;
	a = spawn_new(SPAWN_OPEN);
	if (!a)
		return ENOMEM;
	/* The path is copied because the action outlives this call and the
	 * caller is entitled to reuse or free its buffer immediately. */
	a->path = strdup(path);
	if (!a->path) {
		free(a);
		return ENOMEM;
	}
	a->fd = fd;
	a->oflag = oflag;
	a->mode = mode;
	return spawn_append(acts, a);
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *acts, int fd)
{
	struct __spawn_action *a;

	if (!acts || fd < 0)
		return EINVAL;
	a = spawn_new(SPAWN_CLOSE);
	if (!a)
		return ENOMEM;
	a->fd = fd;
	return spawn_append(acts, a);
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *acts, int fd,
				     int newfd)
{
	struct __spawn_action *a;

	if (!acts || fd < 0 || newfd < 0)
		return EINVAL;
	a = spawn_new(SPAWN_DUP2);
	if (!a)
		return ENOMEM;
	a->fd = fd;
	a->newfd = newfd;
	return spawn_append(acts, a);
}

int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *acts,
				      const char *path)
{
	struct __spawn_action *a;

	if (!acts || !path)
		return EINVAL;
	a = spawn_new(SPAWN_CHDIR);
	if (!a)
		return ENOMEM;
	a->path = strdup(path);
	if (!a->path) {
		free(a);
		return ENOMEM;
	}
	return spawn_append(acts, a);
}

int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *acts, int fd)
{
	struct __spawn_action *a;

	if (!acts || fd < 0)
		return EINVAL;
	a = spawn_new(SPAWN_FCHDIR);
	if (!a)
		return ENOMEM;
	a->fd = fd;
	return spawn_append(acts, a);
}

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *acts,
					 const char *path)
{
	return posix_spawn_file_actions_addchdir(acts, path);
}

int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *acts,
					  int fd)
{
	return posix_spawn_file_actions_addfchdir(acts, fd);
}

/* ------------------------------------------------------------------ */
/* Attributes                                                          */
/* ------------------------------------------------------------------ */

int posix_spawnattr_init(posix_spawnattr_t *attr)
{
	if (!attr)
		return EINVAL;
	memset(attr, 0, sizeof(*attr));
	return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
	/* Nothing is allocated, but the call must exist and succeed: callers
	 * pair it with _init unconditionally. */
	return attr ? 0 : EINVAL;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags)
{
	if (!attr || !flags)
		return EINVAL;
	*flags = attr->__flags;
	return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
	if (!attr)
		return EINVAL;
	attr->__flags = flags;
	return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup)
{
	if (!attr || !pgroup)
		return EINVAL;
	*pgroup = attr->__pgrp;
	return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup)
{
	if (!attr)
		return EINVAL;
	attr->__pgrp = pgroup;
	return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr,
				  sigset_t *sigdefault)
{
	if (!attr || !sigdefault)
		return EINVAL;
	*sigdefault = attr->__sd;
	return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr,
				  const sigset_t *sigdefault)
{
	if (!attr || !sigdefault)
		return EINVAL;
	attr->__sd = *sigdefault;
	return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *sigmask)
{
	if (!attr || !sigmask)
		return EINVAL;
	*sigmask = attr->__ss;
	return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sigmask)
{
	if (!attr || !sigmask)
		return EINVAL;
	attr->__ss = *sigmask;
	return 0;
}

int posix_spawnattr_getschedparam(const posix_spawnattr_t *attr,
				  struct sched_param *schedparam)
{
	if (!attr || !schedparam)
		return EINVAL;
	*schedparam = attr->__sp;
	return 0;
}

int posix_spawnattr_setschedparam(posix_spawnattr_t *attr,
				  const struct sched_param *schedparam)
{
	if (!attr || !schedparam)
		return EINVAL;
	attr->__sp = *schedparam;
	return 0;
}

int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *attr, int *policy)
{
	if (!attr || !policy)
		return EINVAL;
	*policy = attr->__policy;
	return 0;
}

int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int policy)
{
	if (!attr)
		return EINVAL;
	attr->__policy = policy;
	return 0;
}

/* ------------------------------------------------------------------ */
/* The spawn itself                                                    */
/* ------------------------------------------------------------------ */

/* What the child needs, living in the parent's frame.  The child reads it
 * through the address space it shares with the parent and writes back one
 * int; the parent reads that once clone() has returned, which is after the
 * child has exec'd or exited. */
struct spawn_args {
	const char *file;
	char *const *argv;
	char *const *envp;
	const posix_spawn_file_actions_t *file_actions;
	const posix_spawnattr_t *attrp;
	const sigset_t *parent_mask; /* what the parent had before blocking all */
	int use_path;
	volatile int err;
};

/* Everything the child does before exec().  Returns an errno on failure.
 *
 * This runs in the PARENT'S address space with the parent thread parked (see
 * the file comment): every call below is a plain system call wrapper, the
 * only memory written is the caller's `err', and execvp()'s PATH search works
 * in a buffer on this stack.  Reading environ is fine -- it is the parent's,
 * and it is what the child is meant to read. */
static int spawn_child_setup(const struct spawn_args *a)
{
	const posix_spawnattr_t *attrp = a->attrp;
	short flags = attrp ? attrp->__flags : 0;
	int sig;

	/* Signals first.  Every handler the parent installed is code written
	 * to run in the parent; run in a child that shares the parent's
	 * memory it could corrupt what the parent expects to find, so each
	 * caught signal goes back to SIG_DFL here (exec would do the same a
	 * moment later, so the program being started sees no difference).
	 * SIG_IGN survives exec and stays, unless SETSIGDEF names the signal.
	 * None of this is observable in between: the parent blocked every
	 * signal before the clone, the mask is inherited, and it is only
	 * replaced at the very end.  The child's dispositions are its own
	 * copy -- the parent's are untouched. */
	for (sig = 1; sig < NSIG; sig++) {
		struct sigaction sa;

		if (sig == SIGKILL || sig == SIGSTOP)
			continue;
		if ((flags & POSIX_SPAWN_SETSIGDEF) &&
		    sigismember(&attrp->__sd, sig) > 0) {
			memset(&sa, 0, sizeof(sa));
			sa.sa_handler = SIG_DFL;
			(void)sigaction(sig, &sa, NULL);
			continue;
		}
		if (sigaction(sig, NULL, &sa) != 0)
			continue;
		if (sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN) {
			memset(&sa, 0, sizeof(sa));
			sa.sa_handler = SIG_DFL;
			(void)sigaction(sig, &sa, NULL);
		}
	}

	if (flags & POSIX_SPAWN_SETSID) {
		if (setsid() < 0)
			return errno;
	}

	if (flags & POSIX_SPAWN_SETPGROUP) {
		if (setpgid(0, attrp->__pgrp) != 0)
			return errno;
	}

	if (flags & POSIX_SPAWN_RESETIDS) {
		/* Drop back to the real IDs.  Group first: after setuid() the
		 * process may no longer be privileged enough to change groups,
		 * and the wrong order leaves it with the old group. */
		if (setgid(getgid()) != 0)
			return errno;
		if (setuid(getuid()) != 0)
			return errno;
	}

	if (flags & (POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER)) {
		/* Scheduling policies are not implemented on this system
		 * (_POSIX_PRIORITY_SCHEDULING is -1), so rather than ignore the
		 * request, say so.  A caller that asked for a policy and got
		 * the default would otherwise never find out. */
		return ENOTSUP;
	}

	if (a->file_actions) {
		const struct __spawn_action *act;

		for (act = a->file_actions->__head; act; act = act->next) {
			switch (act->op) {
			case SPAWN_OPEN: {
				/* Open, then move to the requested descriptor.
				 * open() gives the lowest free one, which is
				 * usually not the one asked for. */
				int fd = open(act->path, act->oflag, act->mode);

				if (fd < 0)
					return errno;
				if (fd != act->fd) {
					if (dup2(fd, act->fd) < 0) {
						int e = errno;

						close(fd);
						return e;
					}
					close(fd);
				}
				break;
			}
			case SPAWN_CLOSE:
				/* Closing an already-closed descriptor is a
				 * failure per POSIX, so EBADF propagates. */
				if (close(act->fd) != 0)
					return errno;
				break;
			case SPAWN_DUP2:
				if (dup2(act->fd, act->newfd) < 0)
					return errno;
				/* dup2 clears FD_CLOEXEC on the new
				 * descriptor, which is what is wanted: the
				 * point of the action is to hand it to the
				 * program being run. */
				break;
			case SPAWN_CHDIR:
				if (chdir(act->path) != 0)
					return errno;
				break;
			case SPAWN_FCHDIR:
				if (fchdir(act->fd) != 0)
					return errno;
				break;
			}
		}
	}

	/* Last, so that nothing above ran with a signal deliverable: the mask
	 * the caller asked for, or else the one the parent had. */
	if (sigprocmask(SIG_SETMASK,
			(flags & POSIX_SPAWN_SETSIGMASK) ? &attrp->__ss :
							   a->parent_mask,
			NULL) != 0)
		return errno;

	return 0;
}

/* The child, from its first instruction on its own stack to exec or _exit.
 * Never returns: clone() would exit the process with its value, but the
 * parent must find `err' set either way. */
static int spawn_child(void *arg)
{
	struct spawn_args *a = arg;
	int err = spawn_child_setup(a);

	if (err == 0) {
		if (a->use_path)
			execvp(a->file, a->argv);
		else
			execve(a->file, a->argv, a->envp ? a->envp : environ);
		err = errno;
	}
	a->err = err;
	_exit(127);
}

/* The child's stack.  It runs in the parent's address space and cannot use
 * the parent's stack -- the parent is parked in the middle of it -- so it
 * gets a mapping of its own, released once it is done.  The deepest path is
 * execvp()'s PATH search with its on-stack buffers, well under this. */
#define SPAWN_STACK_SIZE (64 * 1024)

static int spawn_common(pid_t *pid, const char *file,
			const posix_spawn_file_actions_t *file_actions,
			const posix_spawnattr_t *attrp, char *const argv[],
			char *const envp[], int use_path)
{
	struct spawn_args args;
	sigset_t all, saved;
	void *stack;
	pid_t child;
	int err;

	if (!file || !argv)
		return EINVAL;

	stack = mmap(NULL, SPAWN_STACK_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED)
		return errno;

	args.file = file;
	args.argv = argv;
	args.envp = envp;
	args.file_actions = file_actions;
	args.attrp = attrp;
	args.parent_mask = &saved;
	args.use_path = use_path;
	args.err = 0;

	/* Block everything: the child inherits the mask, and between its
	 * first instruction and its own sigprocmask() it must not run one of
	 * this process's handlers (see spawn_child_setup).  Restored below,
	 * whatever happened. */
	sigfillset(&all);
	sigprocmask(SIG_BLOCK, &all, &saved);

	/* This thread sleeps in here until the child has exec'd or exited;
	 * the other threads of the process keep running. */
	child = clone(spawn_child, (char *)stack + SPAWN_STACK_SIZE,
		      CLONE_VM | CLONE_VFORK | SIGCHLD, &args, (pid_t *)NULL,
		      (void *)NULL, (pid_t *)NULL);
	err = (child < 0) ? errno : (int)args.err;

	sigprocmask(SIG_SETMASK, &saved, NULL);
	munmap(stack, SPAWN_STACK_SIZE);

	if (child < 0)
		return err;

	if (err != 0) {
		/* The child never became the program, so it is this call's
		 * business to reap it -- reporting failure AND leaving a zombie
		 * would be the worst of both. */
		int status;

		while (waitpid(child, &status, 0) < 0 && errno == EINTR)
			;
		return err;
	}

	if (pid)
		*pid = child;
	return 0;
}

int posix_spawn(pid_t *pid, const char *path,
		const posix_spawn_file_actions_t *file_actions,
		const posix_spawnattr_t *attrp, char *const argv[],
		char *const envp[])
{
	return spawn_common(pid, path, file_actions, attrp, argv, envp, 0);
}

int posix_spawnp(pid_t *pid, const char *file,
		 const posix_spawn_file_actions_t *file_actions,
		 const posix_spawnattr_t *attrp, char *const argv[],
		 char *const envp[])
{
	return spawn_common(pid, file, file_actions, attrp, argv, envp, 1);
}
