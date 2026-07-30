/*
 * POSIX shared memory objects.
 *
 * These are thin wrappers over /dev/shm: the kernel exposes the shared memory
 * namespace as a directory, so an object handle is an ordinary descriptor and
 * ftruncate/fstat/mmap/close/dup all work on it without anything special.
 * That also means `ls /dev/shm` shows what exists, which is how the same
 * namespace behaves elsewhere.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define SHM_DIR "/dev/shm/"

/* Build "/dev/shm/<name>", rejecting anything that is not a single component.
 * POSIX says the name should start with a slash and contain no others; a name
 * with an embedded slash would otherwise escape the namespace. */
static int shm_build_path(const char *name, char *out, size_t cap)
{
	size_t i, n;

	if (!name) {
		errno = EINVAL;
		return -1;
	}
	while (*name == '/') /* a single leading slash is conventional */
		name++;
	if (*name == '\0') {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; name[i]; i++) {
		if (name[i] == '/') {
			errno = EINVAL;
			return -1;
		}
	}
	n = sizeof(SHM_DIR) - 1;
	if (n + i + 1 > cap) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(out, SHM_DIR, n);
	memcpy(out + n, name, i + 1);
	return 0;
}

int shm_open(const char *name, int oflag, mode_t mode)
{
	char path[128];

	(void)mode; /* the object is created 0600; chmod is not supported */
	if (shm_build_path(name, path, sizeof(path)) != 0)
		return -1;
	return open(path, oflag);
}

int shm_unlink(const char *name)
{
	char path[128];

	if (shm_build_path(name, path, sizeof(path)) != 0)
		return -1;
	return unlink(path);
}
