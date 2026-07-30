/*
 * posix_spawn() and friends.
 *
 * Built on fork()+exec(), which is what the interface is worth on a system
 * that can fork cheaply: not the process creation itself, but the error
 * reporting.  With plain fork+exec, a failed exec happens in the child, where
 * the only way to tell the parent is to invent a channel for it -- so every
 * program that cares reinvents the same close-on-exec pipe, and every program
 * that does not silently gets a child that exits 127.  posix_spawn() reports
 * the error as its return value, so callers get it right without trying.
 *
 * That pipe is the interesting part of this file.  The child writes its errno
 * into it and _exit()s if anything fails before the program is running; the
 * pipe is O_CLOEXEC, so a successful exec closes it and the parent's read()
 * returns 0.  Nothing else distinguishes "exec failed" from "the program ran
 * and exited immediately".
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

/* Everything the child does between fork() and exec().  Returns an errno on
 * failure; the caller reports it through the pipe and exits.
 *
 * Nothing here may allocate or take a lock: after fork() this process is a
 * copy of a possibly-threaded parent, and any lock held by a thread that did
 * not come along is held forever.  All the memory these actions need was
 * allocated before the fork, when the actions were added. */
static int spawn_child_setup(const posix_spawn_file_actions_t *file_actions,
			     const posix_spawnattr_t *attrp)
{
	short flags = attrp ? attrp->__flags : 0;

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

	if (flags & POSIX_SPAWN_SETSIGDEF) {
		struct sigaction sa;
		int sig;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_DFL;
		for (sig = 1; sig < NSIG; sig++) {
			if (!sigismember(&attrp->__sd, sig))
				continue;
			/* A failure here means the signal cannot be caught
			 * (SIGKILL, SIGSTOP), which is not an error: it is
			 * already at its default disposition. */
			(void)sigaction(sig, &sa, NULL);
		}
	}

	/* The mask is installed AFTER the dispositions are reset, so a signal
	 * arriving in the window does not run a handler inherited from the
	 * parent -- the whole point of SETSIGDEF. */
	if (flags & POSIX_SPAWN_SETSIGMASK) {
		if (sigprocmask(SIG_SETMASK, &attrp->__ss, NULL) != 0)
			return errno;
	}

	if (flags & (POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER)) {
		/* Scheduling policies are not implemented on this system
		 * (_POSIX_PRIORITY_SCHEDULING is -1), so rather than ignore the
		 * request, say so.  A caller that asked for a policy and got
		 * the default would otherwise never find out. */
		return ENOTSUP;
	}

	if (file_actions) {
		const struct __spawn_action *a;

		for (a = file_actions->__head; a; a = a->next) {
			switch (a->op) {
			case SPAWN_OPEN: {
				/* Open, then move to the requested descriptor.
				 * open() gives the lowest free one, which is
				 * usually not the one asked for. */
				int fd = open(a->path, a->oflag, a->mode);

				if (fd < 0)
					return errno;
				if (fd != a->fd) {
					if (dup2(fd, a->fd) < 0) {
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
				if (close(a->fd) != 0)
					return errno;
				break;
			case SPAWN_DUP2:
				if (dup2(a->fd, a->newfd) < 0)
					return errno;
				/* dup2 clears FD_CLOEXEC on the new
				 * descriptor, which is what is wanted: the
				 * point of the action is to hand it to the
				 * program being run. */
				break;
			case SPAWN_CHDIR:
				if (chdir(a->path) != 0)
					return errno;
				break;
			case SPAWN_FCHDIR:
				if (fchdir(a->fd) != 0)
					return errno;
				break;
			}
		}
	}

	return 0;
}

static int spawn_common(pid_t *pid, const char *file,
			const posix_spawn_file_actions_t *file_actions,
			const posix_spawnattr_t *attrp, char *const argv[],
			char *const envp[], int use_path)
{
	int err_pipe[2];
	pid_t child;
	int err = 0;
	ssize_t n;

	if (!file || !argv)
		return EINVAL;

	/* O_CLOEXEC is what makes this work: a successful exec closes the
	 * write end, the parent's read() returns 0, and that -- not any
	 * message -- is how success is signalled. */
	if (pipe2(err_pipe, O_CLOEXEC) != 0)
		return errno;

	child = fork();
	if (child < 0) {
		int e = errno;

		close(err_pipe[0]);
		close(err_pipe[1]);
		return e;
	}

	if (child == 0) {
		close(err_pipe[0]);

		err = spawn_child_setup(file_actions, attrp);
		if (err == 0) {
			if (use_path)
				execvp(file, argv);
			else
				execve(file, argv, envp ? envp : environ);
			err = errno;
		}

		/* Report and go.  A short or failed write leaves the parent
		 * seeing a clean exec, which is wrong but not correctable from
		 * here -- and the child must not linger either way. */
		(void)!write(err_pipe[1], &err, sizeof(err));
		_exit(127);
	}

	close(err_pipe[1]);
	do {
		n = read(err_pipe[0], &err, sizeof(err));
	} while (n < 0 && errno == EINTR);
	close(err_pipe[0]);

	if (n == (ssize_t)sizeof(err) && err != 0) {
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
