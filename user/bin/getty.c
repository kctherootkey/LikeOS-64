/*
 * getty - open a terminal, make it the controlling terminal, prompt for a
 * login name and exec /bin/login.
 *
 * Usage: getty [tty]      (default /dev/console)
 *
 * This is a minimal getty: it establishes a fresh session on the terminal,
 * displays the issue banner and login prompt, reads the user name and hands
 * off to /bin/login, which performs authentication.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>

#define DEFAULT_TTY "/dev/console"
#define LOGIN_PATH  "/bin/login"

static char *read_line(int fd)
{
	size_t cap = 64, len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	for (;;) {
		char c;
		ssize_t n = read(fd, &c, 1);
		if (n < 0 && errno == EINTR)
			continue; /* a signal (a window resize) is not an EOF */
		if (n <= 0) {
			if (len == 0) {
				free(buf);
				return NULL;
			}
			break;
		}
		if (c == '\n')
			break;
		if (len + 1 >= cap) {
			char *nb = realloc(buf, cap *= 2);
			if (!nb) {
				free(buf);
				return NULL;
			}
			buf = nb;
		}
		buf[len++] = c;
	}
	buf[len] = '\0';
	return buf;
}

/* The boot goes on after init starts: the root mount is followed by firmware
 * loads and device bring-up (graphics, sound) that keep printing to the
 * console for a few seconds.  A prompt printed in the middle of that ends up
 * buried under it, so the first one waits until the kernel reports the boot
 * settled -- briefly, and never for good: a device that never finishes must
 * not keep anyone from logging in.  A kernel without the file is taken as
 * settled.  What is typed meanwhile stays in the terminal's input. */
#define SETTLE_POLL_US 50000
#define SETTLE_MAX_US (30 * 1000000)

static void wait_boot_settled(void)
{
	for (long waited = 0; waited < SETTLE_MAX_US;
	     waited += SETTLE_POLL_US) {
		char c = '1';
		int fd = open("/proc/boot_settled", O_RDONLY);

		if (fd < 0)
			return;
		if (read(fd, &c, 1) != 1)
			c = '1';
		close(fd);
		if (c != '0')
			return;
		usleep(SETTLE_POLL_US);
	}
}

int main(int argc, char *argv[])
{
	const char *tty = (argc > 1) ? argv[1] : DEFAULT_TTY;
	char hostname[128];
	int fd;

	/* New session so we can claim the terminal as our controlling tty. */
	setsid();

	/* Open the terminal, retrying briefly.
	 *
	 * This used to fall back to "whatever stdin already is" -- but init
	 * points its children's stdio at /dev/null before spawning us, so the
	 * fallback handed the user a login session whose terminal was
	 * /dev/null.  Reads and writes there quietly succeed, so the prompt
	 * and the shell looked normal; anything that actually needs a terminal
	 * did not.  isatty() is false on /dev/null, so tcgetattr() fails and a
	 * terminal multiplexer reports "open terminal failed: not a terminal"
	 * -- for a session that never had one.
	 *
	 * A getty without its terminal cannot do its job, so retry (the device
	 * may simply not be ready yet this early) and, failing that, exit and
	 * let init respawn us rather than run a session that only looks right.
	 */
	for (int try = 0; try < 50; try++) {
		fd = open(tty, O_RDWR);
		if (fd >= 0)
			break;
		usleep(100000); /* 100ms; 5s total */
	}
	if (fd < 0) {
		/* stderr is /dev/null here, so this is for a serial console. */
		fprintf(stderr, "getty: cannot open %s: %s\n", tty,
			strerror(errno));
		return 1;
	}
	{
		ioctl(fd, TIOCSCTTY, 0);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
		/* Make our session the terminal's foreground process group so
		 * input and terminal-generated signals reach us (and, after
		 * exec, login and the user's shell). */
		tcsetpgrp(0, getpid());
	}

	/* Sane line discipline: canonical mode with echo. */
	{
		struct termios t;
		if (tcgetattr(0, &t) == 0) {
			t.c_lflag |= (ICANON | ECHO);
			tcsetattr(0, TCSANOW, &t);
		}
	}

	if (gethostname(hostname, sizeof(hostname)) != 0)
		strcpy(hostname, "likeos");

	wait_boot_settled();

	for (;;) {
		char *name;

		printf("\n%s login: ", hostname);
		fflush(stdout);

		name = read_line(0);
		if (!name) {
			/* The terminal reached end of file or went away.
			 * Retrying here would print prompts into a dead
			 * descriptor forever, burning a CPU; exit instead and
			 * let init decide whether to start a fresh getty. */
			return 0;
		}
		if (name[0] == '\0') {
			free(name);
			continue;
		}

		{
			char *login_argv[] = { (char *)"login", name, NULL };
			char *login_envp[] = {
				(char *)"PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
				(char *)"TERM=linux",
				NULL
			};
			execve(LOGIN_PATH, login_argv, login_envp);
		}
		/* exec failed */
		fprintf(stderr, "getty: cannot exec %s\n", LOGIN_PATH);
		free(name);
		return 1;
	}
}
