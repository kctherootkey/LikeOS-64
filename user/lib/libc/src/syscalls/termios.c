/*
 * Copyright (C) 2026 The LikeOS Project
 */

#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

int tcgetattr(int fd, struct termios *termios_p)
{
	if (!termios_p) {
		return -1;
	}
	return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
	unsigned long req = TCSETS;
	if (!termios_p) {
		return -1;
	}
	if (optional_actions == TCSADRAIN) {
		req = TCSETSW;
	} else if (optional_actions == TCSAFLUSH) {
		req = TCSETSF;
	}
	return ioctl(fd, req, (void *)termios_p);
}

void cfmakeraw(struct termios *termios_p)
{
	if (!termios_p) {
		return;
	}
	termios_p->c_iflag = 0;
	termios_p->c_oflag = 0;
	termios_p->c_cflag = 0;
	termios_p->c_lflag = 0;
}

int tcflush(int fd, int q)
{
	(void)fd;
	(void)q;
	return 0;
}
int tcdrain(int fd)
{
	(void)fd;
	return 0;
}
int tcflow(int fd, int a)
{
	(void)fd;
	(void)a;
	return 0;
}
int tcsendbreak(int fd, int d)
{
	(void)fd;
	(void)d;
	return 0;
}
pid_t tcgetsid(int fd)
{
	(void)fd;
	return getpgrp();
}

/* Baud-rate accessors. We don't expose a configurable baud setting
 * (the kernel TTY layer is line-discipline only), so report a
 * conservative B38400 (15) and accept any input silently. */
speed_t cfgetispeed(const struct termios *t)
{
	(void)t;
	return 15;
}
speed_t cfgetospeed(const struct termios *t)
{
	(void)t;
	return 15;
}
int cfsetispeed(struct termios *t, speed_t s)
{
	(void)t;
	(void)s;
	return 0;
}
int cfsetospeed(struct termios *t, speed_t s)
{
	(void)t;
	(void)s;
	return 0;
}

/* getpass() - read a password from the terminal without echoing it.
 *
 * The terminal is /dev/tty when the process has one, else stdin for reading
 * and stderr for the prompt.  Echo and signal characters are off while the
 * line is read and the previous settings are restored afterwards (a newline
 * is then written, since the user's Enter was not echoed).  Bytes beyond 128
 * are read and dropped so the rest of the line does not leak into the next
 * read.  End of input before a newline returns what was read, possibly "".
 *
 * Copyright (C) 2026 The LikeOS Project
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#define GETPASS_MAX 128

char *getpass(const char *prompt)
{
	static char buf[GETPASS_MAX + 1];
	struct termios saved, quiet;
	int in, out, opened, restore = 0;
	size_t n = 0;
	ssize_t r;
	char c;

	in = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
	opened = in >= 0;
	if (opened) {
		out = in;
	} else {
		in = STDIN_FILENO;
		out = STDERR_FILENO;
	}

	if (tcgetattr(in, &saved) == 0) {
		quiet = saved;
		quiet.c_lflag &= ~(ECHO | ISIG);
		if (tcsetattr(in, TCSAFLUSH, &quiet) == 0)
			restore = 1;
	}

	if (prompt && *prompt)
		(void)write(out, prompt, strlen(prompt));

	for (;;) {
		r = read(in, &c, 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0 || c == '\n')
			break;
		if (n < GETPASS_MAX)
			buf[n++] = c;
	}
	buf[n] = '\0';

	if (restore) {
		(void)tcsetattr(in, TCSAFLUSH, &saved);
		(void)write(out, "\n", 1);
	}
	if (opened)
		close(in);
	return buf;
}
