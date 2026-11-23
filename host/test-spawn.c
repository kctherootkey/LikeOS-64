/* Host test for posix_spawn / posix_spawnp (user/lib/libc/src/stdlib/spawn.c);
 * built and run by host/test-spawn.sh.
 *
 * spawn.c is compiled against the LikeOS headers and its symbols renamed to
 * lk_ ones, so it runs beside the host C library.  Its signal calls are
 * routed to the hk_ stand-ins below, because the LikeOS sigset_t and struct
 * sigaction are not the host's; everything else it calls (clone, execve,
 * getenv, dup2, waitpid, ...) has the same interface on the host and is
 * used as is.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The LikeOS types, as far as this test needs them. */
typedef unsigned long lk_sigset_t;
typedef struct {
	void *head, *tail;
} lk_actions_t;

int lk_posix_spawn(pid_t *pid, const char *path, const lk_actions_t *acts,
		   const void *attrp, char *const argv[], char *const envp[]);
int lk_posix_spawnp(pid_t *pid, const char *file, const lk_actions_t *acts,
		    const void *attrp, char *const argv[], char *const envp[]);
int lk_posix_spawn_file_actions_init(lk_actions_t *acts);
int lk_posix_spawn_file_actions_destroy(lk_actions_t *acts);
int lk_posix_spawn_file_actions_adddup2(lk_actions_t *acts, int fd, int newfd);
int lk_posix_spawn_file_actions_addclose(lk_actions_t *acts, int fd);

/* ---- Signal stand-ins (LikeOS sigset_t: one word, bit n-1 = signal n) */

int hk_sigfillset(lk_sigset_t *s)
{
	*s = ~0UL;
	return 0;
}

int hk_sigismember(const lk_sigset_t *s, int sig)
{
	return (int)((*s >> (sig - 1)) & 1);
}

int hk_sigprocmask(int how, const lk_sigset_t *set, lk_sigset_t *old)
{
	sigset_t h, ho;

	sigemptyset(&h);
	if (set)
		for (int sig = 1; sig < 64; sig++)
			if ((*set >> (sig - 1)) & 1)
				sigaddset(&h, sig);
	if (sigprocmask(how, set ? &h : NULL, &ho) != 0)
		return -1;
	if (old) {
		*old = 0;
		for (int sig = 1; sig < 64; sig++)
			if (sigismember(&ho, sig) == 1)
				*old |= 1UL << (sig - 1);
	}
	return 0;
}

/* Dispositions are not what this test is about: report "none to change". */
int hk_sigaction(int sig, const void *act, void *old)
{
	(void)sig;
	(void)act;
	(void)old;
	errno = EINVAL;
	return -1;
}

/* ---- The test ---------------------------------------------------- */

static int g_fail;

static void check(const char *what, int ok)
{
	printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
	if (!ok)
		g_fail++;
}

/* Spawn with stdout on a pipe; returns the spawn result and puts what the
 * child printed in `out' and its exit status in *status. */
static int run(int use_path, const char *file, char *const argv[],
	       char *const envp[], char *out, size_t outsz, int *status)
{
	int p[2];
	lk_actions_t acts;
	pid_t pid = -1;
	size_t n = 0;
	int r;

	out[0] = '\0';
	*status = -1;
	if (pipe(p) != 0)
		return -1;
	lk_posix_spawn_file_actions_init(&acts);
	lk_posix_spawn_file_actions_adddup2(&acts, p[1], 1);
	lk_posix_spawn_file_actions_addclose(&acts, p[0]);
	r = use_path ? lk_posix_spawnp(&pid, file, &acts, NULL, argv, envp) :
		       lk_posix_spawn(&pid, file, &acts, NULL, argv, envp);
	lk_posix_spawn_file_actions_destroy(&acts);
	close(p[1]);
	if (r == 0) {
		ssize_t k;

		while (n < outsz - 1 &&
		       (k = read(p[0], out + n, outsz - 1 - n)) > 0)
			n += (size_t)k;
		out[n] = '\0';
		while (waitpid(pid, status, 0) < 0 && errno == EINTR)
			;
	}
	close(p[0]);
	return r;
}

int main(void)
{
	char out[256];
	int st;
	char *argv_sh[] = { "sh", "-c", "printf '%s' \"$LK_SPAWN_VAR\"", NULL };
	char *argv_abs[] = { "/bin/sh", "-c", "printf '%s' \"$LK_SPAWN_VAR\"",
			     NULL };
	char *env_hello[] = { "LK_SPAWN_VAR=hello", "PATH=/nonexistent", NULL };
	char *env_empty[] = { NULL };
	int r;

	setenv("PATH", "/nonexistent:/usr/bin/:/bin", 1);
	setenv("LK_SPAWN_VAR", "parent", 1);

	printf("posix_spawnp honours envp\n");
	r = run(1, "sh", argv_sh, env_hello, out, sizeof(out), &st);
	check("spawnp sh succeeds", r == 0);
	check("the child sees the envp value", strcmp(out, "hello") == 0);
	check("and exits 0", WIFEXITED(st) && WEXITSTATUS(st) == 0);

	r = run(1, "sh", argv_sh, env_empty, out, sizeof(out), &st);
	check("spawnp with an empty envp succeeds", r == 0);
	check("the child does not see the caller's environment",
	      strcmp(out, "") == 0);

	r = run(1, "sh", argv_sh, NULL, out, sizeof(out), &st);
	check("spawnp with a null envp passes the caller's environment",
	      r == 0 && strcmp(out, "parent") == 0);

	printf("PATH: the caller's, searched as execvp searches it\n");
	r = run(1, "/bin/sh", argv_abs, env_hello, out, sizeof(out), &st);
	check("a name with a slash is not searched, envp still applies",
	      r == 0 && strcmp(out, "hello") == 0);
	r = run(1, "lk-no-such-program", argv_sh, env_hello, out, sizeof(out),
		&st);
	check("a name found nowhere: ENOENT", r == ENOENT);
	check("and no child left over", waitpid(-1, NULL, WNOHANG) < 0 &&
						errno == ECHILD);
	r = run(1, "", argv_sh, env_hello, out, sizeof(out), &st);
	check("an empty name: ENOENT", r == ENOENT);
	unsetenv("PATH");
	r = run(1, "sh", argv_sh, env_hello, out, sizeof(out), &st);
	check("no PATH: the default /bin:/usr/local/bin finds sh",
	      r == 0 && strcmp(out, "hello") == 0);
	setenv("PATH", "/nonexistent", 1);
	r = run(1, "sh", argv_sh, env_hello, out, sizeof(out), &st);
	check("the caller's PATH, not the one in envp, is searched",
	      r == ENOENT);
	setenv("PATH", "/nonexistent:/usr/bin/:/bin", 1);

	printf("posix_spawn (no search) honours envp\n");
	r = run(0, "/bin/sh", argv_abs, env_hello, out, sizeof(out), &st);
	check("spawn /bin/sh with envp", r == 0 && strcmp(out, "hello") == 0);
	r = run(0, "/bin/sh", argv_abs, NULL, out, sizeof(out), &st);
	check("spawn with a null envp passes the caller's environment",
	      r == 0 && strcmp(out, "parent") == 0);
	r = run(0, "sh", argv_sh, env_hello, out, sizeof(out), &st);
	check("spawn does not search PATH", r == ENOENT);

	printf(g_fail ? "FAILED: %d\n" : "all passed\n", g_fail);
	return g_fail ? 1 : 0;
}
