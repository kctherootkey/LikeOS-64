#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* One record as getdents64() actually writes it, which is NOT the public
 * struct dirent64 in <dirent.h> and must not be confused with it.
 *
 * The public one is a fixed-size value type: portable code declares objects of
 * it, and testlibc requires sizeof/offsetof to match struct dirent exactly.
 * What the kernel writes is a packed sequence of VARIABLE-length records, each
 * d_reclen bytes, with the name occupying only as much as it needs.  So the
 * name is a flexible array here: this type is only ever laid over the buffer,
 * never instantiated, and giving it a char[256] would tell the compiler that
 * 256 bytes are readable at d_name -- untrue for the last record in the
 * buffer, where the record ends well before that.
 *
 * The field offsets are identical to the public struct's, so both describe the
 * same bytes; only the tail differs.  d_off is signed because that is what the
 * getdents64 record carries; readdir() casts it on the way out. */
struct getdents64_record {
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

DIR *opendir(const char *name)
{
	if (!name) {
		errno = EINVAL;
		return NULL;
	}
	int fd = openat(AT_FDCWD, name, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	DIR *dirp = (DIR *)malloc(sizeof(DIR));
	if (!dirp) {
		close(fd);
		errno = ENOMEM;
		return NULL;
	}
	dirp->fd = fd;
	dirp->buf_pos = 0;
	dirp->buf_len = 0;
	return dirp;
}

struct dirent *readdir(DIR *dirp)
{
	if (!dirp) {
		errno = EINVAL;
		return NULL;
	}
	while (1) {
		if (dirp->buf_pos >= dirp->buf_len) {
			int n = getdents64(dirp->fd, dirp->buf,
					   sizeof(dirp->buf));
			if (n <= 0) {
				return NULL;
			}
			dirp->buf_len = n;
			dirp->buf_pos = 0;
		}
		struct getdents64_record *d =
			(struct getdents64_record *)(dirp->buf +
						     dirp->buf_pos);
		if (d->d_reclen == 0) {
			return NULL;
		}
		dirp->buf_pos += d->d_reclen;
		dirp->current.d_ino = d->d_ino;
		dirp->current.d_off = (uint64_t)d->d_off;
		dirp->current.d_reclen = d->d_reclen;
		dirp->current.d_type = d->d_type;
		size_t len = strlen(d->d_name);
		if (len >= sizeof(dirp->current.d_name)) {
			len = sizeof(dirp->current.d_name) - 1;
		}
		memcpy(dirp->current.d_name, d->d_name, len);
		dirp->current.d_name[len] = '\0';
		return &dirp->current;
	}
}

int closedir(DIR *dirp)
{
	if (!dirp) {
		errno = EINVAL;
		return -1;
	}
	int fd = dirp->fd;
	free(dirp);
	return close(fd);
}

void rewinddir(DIR *dirp)
{
	if (!dirp)
		return;
	/* Seek the directory fd back to position 0 */
	lseek(dirp->fd, 0, 0 /* SEEK_SET */);
	dirp->buf_pos = 0;
	dirp->buf_len = 0;
}

DIR *fdopendir(int fd)
{
	DIR *dirp;

	if (fd < 0) {
		errno = EBADF;
		return NULL;
	}
	dirp = (DIR *)malloc(sizeof(DIR));
	if (!dirp) {
		errno = ENOMEM;
		return NULL;
	}
	/* The descriptor becomes the DIR's property: closedir() closes it. */
	dirp->fd = fd;
	dirp->buf_pos = 0;
	dirp->buf_len = 0;
	return dirp;
}

int dirfd(DIR *dirp)
{
	if (!dirp) {
		errno = EINVAL;
		return -1;
	}
	return dirp->fd;
}

int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result)
{
	struct dirent *d;

	if (!dirp || !entry || !result)
		return EINVAL;
	errno = 0;
	d = readdir(dirp);
	if (!d) {
		*result = NULL;
		return errno; /* 0 at end of directory, else the error */
	}
	*entry = *d;
	*result = entry;
	return 0;
}

/* telldir() reports where the NEXT readdir() would resume.  The kernel gives
 * us that directly as the d_off of the entry just returned, which is what
 * seekdir() feeds back to lseek(). */
long telldir(DIR *dirp)
{
	if (!dirp) {
		errno = EINVAL;
		return -1;
	}
	if (dirp->buf_pos == 0 && dirp->buf_len == 0)
		return 0; /* nothing read yet */
	return (long)dirp->current.d_off;
}

void seekdir(DIR *dirp, long loc)
{
	if (!dirp)
		return;
	if (lseek(dirp->fd, (off_t)loc, 0 /* SEEK_SET */) < 0)
		return;
	/* Drop the readahead buffer so the next readdir() refills from loc. */
	dirp->buf_pos = 0;
	dirp->buf_len = 0;
}

int alphasort(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

/* versionsort() orders embedded digit runs numerically, so "mod9" sorts
 * before "mod10" instead of after it. */
int versionsort(const struct dirent **a, const struct dirent **b)
{
	const char *p = (*a)->d_name;
	const char *q = (*b)->d_name;

	while (*p && *q) {
		if (*p >= '0' && *p <= '9' && *q >= '0' && *q <= '9') {
			unsigned long lp = 0, lq = 0;
			/* Leading zeros do not change the value, so compare
			 * the numbers rather than the digit strings. */
			while (*p == '0')
				p++;
			while (*q == '0')
				q++;
			while (*p >= '0' && *p <= '9')
				lp = lp * 10 + (unsigned long)(*p++ - '0');
			while (*q >= '0' && *q <= '9')
				lq = lq * 10 + (unsigned long)(*q++ - '0');
			if (lp != lq)
				return lp < lq ? -1 : 1;
			continue;
		}
		if (*p != *q)
			return (int)((unsigned char)*p) -
			       (int)((unsigned char)*q);
		p++;
		q++;
	}
	return (int)((unsigned char)*p) - (int)((unsigned char)*q);
}

int scandir(const char *dirname, struct dirent ***namelist,
	    int (*filter)(const struct dirent *),
	    int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *dirp;
	struct dirent **list = NULL, **grown;
	size_t used = 0, cap = 0;
	struct dirent *d;
	int saved;

	if (!dirname || !namelist) {
		errno = EINVAL;
		return -1;
	}
	dirp = opendir(dirname);
	if (!dirp)
		return -1;

	while ((d = readdir(dirp)) != NULL) {
		struct dirent *copy;

		if (filter && !filter(d))
			continue;
		if (used == cap) {
			cap = cap ? cap * 2 : 16;
			grown = (struct dirent **)realloc(
				list, cap * sizeof(struct dirent *));
			if (!grown)
				goto nomem;
			list = grown;
		}
		/* A full-size copy: callers index d_name freely, and the
		 * entries outlive this DIR. */
		copy = (struct dirent *)malloc(sizeof(struct dirent));
		if (!copy)
			goto nomem;
		*copy = *d;
		list[used++] = copy;
	}
	closedir(dirp);

	if (compar && used > 1)
		qsort(list, used, sizeof(struct dirent *),
		      (int (*)(const void *, const void *))compar);

	*namelist = list;
	return (int)used;

nomem:
	saved = errno;
	for (size_t i = 0; i < used; i++)
		free(list[i]);
	free(list);
	closedir(dirp);
	errno = saved ? saved : ENOMEM;
	return -1;
}
