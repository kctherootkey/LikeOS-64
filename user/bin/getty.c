/*
 * getty - open a terminal, make it the controlling terminal, prompt for a
 * login name and exec /bin/login.
 *
 * Usage: getty [tty]      (default /dev/console)
 *
 * This is a minimal getty: it establishes a fresh session on the terminal,
 * displays the issue banner and login prompt, reads the user name and hands
 * off to /bin/login, which performs authentication.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
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

int main(int argc, char *argv[])
{
	const char *tty = (argc > 1) ? argv[1] : DEFAULT_TTY;
	char hostname[128];
	int fd;

	/* New session so we can claim the terminal as our controlling tty. */
	setsid();

	fd = open(tty, O_RDWR);
	if (fd < 0) {
		/* Fall back to whatever stdin already is. */
		fd = 0;
	} else {
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

	for (;;) {
		char *name;

		printf("\n%s login: ", hostname);
		fflush(stdout);

		name = read_line(0);
		if (!name) /* EOF - retry */
			continue;
		if (name[0] == '\0') {
			free(name);
			continue;
		}

		{
			char *login_argv[] = { (char *)"login", name, NULL };
			char *login_envp[] = {
				(char *)"PATH=/bin:/usr/local/bin",
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
