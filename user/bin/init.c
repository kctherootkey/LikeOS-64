/*
 * init - process 1.
 *
 * Minimal system init: spawn a single getty on the console, reap children,
 * respawn getty whenever its login session ends, and handle shutdown/reboot
 * signals.  Filesystems are assumed already mounted (the USB root is mounted
 * by the kernel); userspace mounting is a future addition.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/reboot.h>

#define GETTY_PATH "/sbin/getty"
#define CONSOLE    "/dev/console"

static volatile sig_atomic_t want_poweroff = 0;
static volatile sig_atomic_t want_reboot = 0;

static void on_term(int sig) { (void)sig; want_poweroff = 1; }
static void on_int(int sig)  { (void)sig; want_reboot = 1; }  /* Ctrl-Alt-Del */

static pid_t spawn_getty(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		char *argv[] = { (char *)"getty", (char *)CONSOLE, NULL };
		char *envp[] = { (char *)"PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", NULL };
		execve(GETTY_PATH, argv, envp);
		/* exec failed */
		_exit(127);
	}
	return pid; /* -1 on failure is handled by the caller */
}

static void do_shutdown(void)
{
	sync();
	if (want_reboot)
		reboot(RB_AUTOBOOT);
	else
		reboot(RB_POWER_OFF);
	/* If reboot returns, there is nothing sensible left to do. */
	for (;;)
		pause();
}

int main(void)
{
	pid_t gpid;

	signal(SIGTERM, on_term);
	signal(SIGINT, on_int);

	gpid = spawn_getty();

	for (;;) {
		int status;
		pid_t w;

		if (want_poweroff || want_reboot)
			do_shutdown();

		w = waitpid(-1, &status, 0);
		if (w < 0) {
			if (errno == EINTR)
				continue; /* re-check shutdown flags */
			if (errno == ECHILD) {
				/* No children at all - (re)start getty. */
				gpid = spawn_getty();
				continue;
			}
			continue;
		}

		/* When the getty/login/shell session ends, start a fresh one. */
		if (w == gpid)
			gpid = spawn_getty();
		/* Any other reaped pid is an orphan we adopted; nothing to do. */
	}
}
