#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

int posix_openpt(int flags)
{
	return open("/dev/ptmx", flags);
}

int grantpt(int fd)
{
	(void)fd;
	return 0;
}

int unlockpt(int fd)
{
	(void)fd;
	return 0;
}

/* ptsname_r() is the reentrant form and the one new code should use;
 * ptsname() is the same lookup into a shared static buffer. */
int ptsname_r(int fd, char *buf, size_t buflen)
{
	char tmp[32];
	int pty = -1;
	int n;

	if (!buf) {
		errno = EINVAL;
		return EINVAL;
	}
	if (ioctl(fd, TIOCGPTN, &pty) != 0) {
		/* ioctl already set errno (ENOTTY for a non-ptmx fd). */
		return errno ? errno : EINVAL;
	}
	n = snprintf(tmp, sizeof(tmp), "/dev/pts/%d", pty);
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		errno = ERANGE;
		return ERANGE;
	}
	if ((size_t)n + 1 > buflen) {
		errno = ERANGE;
		return ERANGE;
	}
	memcpy(buf, tmp, (size_t)n + 1);
	return 0;
}

char *ptsname(int fd)
{
	static char buf[32];

	if (ptsname_r(fd, buf, sizeof(buf)) != 0)
		return NULL;
	return buf;
}
