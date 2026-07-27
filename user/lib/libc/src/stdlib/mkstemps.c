/* mkstemps.c - create temporary file for LikeOS libc */

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

static unsigned long _temp_seed = 0;

static unsigned long _temp_rand(void)
{
	if (_temp_seed == 0) {
		struct timespec ts;
		clock_gettime(0, &ts);
		_temp_seed = (unsigned long)ts.tv_nsec ^
			     (unsigned long)ts.tv_sec ^ (unsigned long)getpid();
	}
	_temp_seed =
		_temp_seed * 6364136223846793005ULL + 1442695040888963407ULL;
	return _temp_seed;
}

int mkstemps(char *templ, int suffixlen)
{
	static const char letters[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	int len = (int)strlen(templ);
	int xstart = len - suffixlen - 6;

	if (xstart < 0) {
		errno = EINVAL;
		return -1;
	}

	/* Verify XXXXXX */
	for (int i = xstart; i < xstart + 6; i++) {
		if (templ[i] != 'X') {
			errno = EINVAL;
			return -1;
		}
	}

	for (int attempts = 0; attempts < 100; attempts++) {
		unsigned long r = _temp_rand();
		for (int i = 0; i < 6; i++) {
			templ[xstart + i] = letters[r % 62];
			r /= 62;
		}

		int fd = open(templ, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0)
			return fd;
		if (errno != EEXIST)
			return -1;
	}

	errno = EEXIST;
	return -1;
}

int mkstemp(char *templ)
{
	return mkstemps(templ, 0);
}

/* mktemp: replace the trailing XXXXXX with a name that does not exist.
 * Inherently racy (POSIX marks it obsolescent) - prefer mkstemp(). */
char *mktemp(char *templ)
{
	size_t len = strlen(templ);
	if (len < 6 || strcmp(templ + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		if (len)
			templ[0] = '\0';
		return templ;
	}
	for (int attempt = 0; attempt < 100; attempt++) {
		unsigned long r = _temp_rand();
		for (int i = 0; i < 6; i++) {
			templ[len - 6 + i] =
				"abcdefghijklmnopqrstuvwxyz0123456789"[r % 36];
			r /= 36;
		}
		if (access(templ, F_OK) != 0)
			return templ; /* name is free */
	}
	templ[0] = '\0';
	errno = EEXIST;
	return templ;
}

/* mkdtemp: like mkstemp but creates a directory. */
char *mkdtemp(char *templ)
{
	size_t len = strlen(templ);
	if (len < 6 || strcmp(templ + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return 0;
	}
	for (int attempt = 0; attempt < 100; attempt++) {
		unsigned long r = _temp_rand();
		for (int i = 0; i < 6; i++) {
			templ[len - 6 + i] =
				"abcdefghijklmnopqrstuvwxyz0123456789"[r % 36];
			r /= 36;
		}
		if (mkdir(templ, 0700) == 0)
			return templ;
		if (errno != EEXIST)
			return 0;
	}
	errno = EEXIST;
	return 0;
}
