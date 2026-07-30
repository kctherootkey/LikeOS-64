/*
 * spawn.h - posix_spawn(): create a process without fork().
 *
 * The interface exists for systems that cannot fork cheaply (or at all), but
 * its real appeal on a system that *can* is that it collapses the whole
 * fork/redirect/exec dance into one call whose failures are reported to the
 * caller.  With fork+exec, a failed exec happens in the child, where the only
 * way to tell the parent is to invent a channel for it; every program that
 * does this reinvents the same pipe.  posix_spawn() reports the error as its
 * return value, so callers get it right by default.
 *
 * Here it is implemented on fork+exec (see src/stdlib/spawn.c) with exactly
 * that error pipe, which is what the interface is worth on this system.
 */
#ifndef _SPAWN_H
#define _SPAWN_H

#include <sys/types.h>
#include <signal.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attribute flags, POSIX values. */
#define POSIX_SPAWN_RESETIDS      0x01
#define POSIX_SPAWN_SETPGROUP     0x02
#define POSIX_SPAWN_SETSIGDEF     0x04
#define POSIX_SPAWN_SETSIGMASK    0x08
#define POSIX_SPAWN_SETSCHEDPARAM 0x10
#define POSIX_SPAWN_SETSCHEDULER  0x20
/* Widely-used extensions beyond POSIX; accepted here so code written against
 * them compiles and behaves, rather than silently skipping the request. */
#define POSIX_SPAWN_USEVFORK      0x40
#define POSIX_SPAWN_SETSID        0x80

/* Both objects are declared with their layout visible because that is what
 * POSIX requires (callers allocate them), but the fields are private: use the
 * posix_spawnattr_ and posix_spawn_file_actions_ calls, never the members. */
typedef struct {
	short __flags;
	pid_t __pgrp;
	sigset_t __sd; /* signals to reset to SIG_DFL */
	sigset_t __ss; /* signal mask to install     */
	struct sched_param __sp;
	int __policy;
} posix_spawnattr_t;

/* File actions are a linked list rather than an array so that adding one
 * cannot invalidate a caller's earlier additions, and so the list has no
 * arbitrary length limit. */
struct __spawn_action;

typedef struct {
	struct __spawn_action *__head;
	struct __spawn_action *__tail;
} posix_spawn_file_actions_t;

int posix_spawn(pid_t *pid, const char *path,
		const posix_spawn_file_actions_t *file_actions,
		const posix_spawnattr_t *attrp, char *const argv[],
		char *const envp[]);

/* Same, but resolves a bare name through PATH the way execvp() does. */
int posix_spawnp(pid_t *pid, const char *file,
		 const posix_spawn_file_actions_t *file_actions,
		 const posix_spawnattr_t *attrp, char *const argv[],
		 char *const envp[]);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr,
				  sigset_t *sigdefault);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr,
				  const sigset_t *sigdefault);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *sigmask);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sigmask);
int posix_spawnattr_getschedparam(const posix_spawnattr_t *attr,
				  struct sched_param *schedparam);
int posix_spawnattr_setschedparam(posix_spawnattr_t *attr,
				  const struct sched_param *schedparam);
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *attr, int *policy);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int policy);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *acts);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *acts);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *acts, int fd,
				     const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *acts, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *acts, int fd,
				     int newfd);
/* POSIX 2024 additions, already in wide use as the _np spellings. */
int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *acts,
				      const char *path);
int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *acts, int fd);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *acts,
					 const char *path);
int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *acts,
					  int fd);

#ifdef __cplusplus
}
#endif

#endif /* _SPAWN_H */
