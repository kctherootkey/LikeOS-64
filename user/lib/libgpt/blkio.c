/* libgpt -- sector I/O on a block device or an image file.
 *
 * Everything above this file talks in logical sectors.  A block device
 * reports its logical sector size (BLKSSZGET), its physical one
 * (BLKPBSZGET) and its size in bytes (BLKGETSIZE64); an image file is
 * taken to have 512-byte sectors unless the caller forces 4096 (which is
 * how 4Kn disks are tested on the build host), and its size is the file's.
 *
 * Reads and writes loop over EINTR and short transfers; a transfer past the
 * end of the device is refused with ENOSPC before it is issued.  Byte
 * addressed helpers do read-modify-write of the sectors they touch.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "gpt.h"

/* The block ioctls (group 0x12).  Every C library that declares them puts
 * them in <sys/mount.h>; the ones a library leaves out are given here with
 * their fixed numbers. */
#ifndef BLKRRPART
#define BLKRRPART _IO(0x12, 95)
#endif
#ifndef BLKSSZGET
#define BLKSSZGET _IO(0x12, 104)
#endif
#ifndef BLKPBSZGET
#define BLKPBSZGET _IO(0x12, 123)
#endif
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#ifndef BLKGETSIZE
#define BLKGETSIZE _IO(0x12, 96)
#endif
#ifndef BLKFLSBUF
#define BLKFLSBUF _IO(0x12, 97)
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif

static void gpt_sleep_ms(int ms)
{
	struct timespec ts;

	if (ms <= 0)
		return;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static void gpt_copy_path(char *dst, size_t len, const char *src)
{
	size_t n = strlen(src);

	if (n >= len)
		n = len - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

int gpt_dev_open(struct gpt_dev *d, const char *path, int flags,
		 uint32_t sector_size)
{
	struct stat st;
	int oflags, fd, rc;

	if (!d || !path || !path[0])
		return -EINVAL;
	memset(d, 0, sizeof(*d));
	d->fd = -1;
	if (sector_size != 0 && sector_size != 512 && sector_size != 1024 &&
	    sector_size != 2048 && sector_size != 4096)
		return -EINVAL;
	if (stat(path, &st) != 0)
		return -errno;
	if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode))
		return -ENOTBLK;
	oflags = (flags & GPT_OPEN_RDWR) ? O_RDWR : O_RDONLY;
	oflags |= O_CLOEXEC;
	/* O_EXCL on a block device is the "nobody else has it" lock; on a
	 * regular file it would mean "create", which is not wanted. */
	if ((flags & GPT_OPEN_EXCL) && S_ISBLK(st.st_mode))
		oflags |= O_EXCL;
	fd = open(path, oflags);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) != 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	d->fd = fd;
	gpt_copy_path(d->path, sizeof(d->path), path);
	d->writable = (flags & GPT_OPEN_RDWR) ? 1 : 0;
	if (S_ISREG(st.st_mode)) {
		d->is_file = 1;
		d->sector_size = sector_size ? sector_size : 512;
		d->phys_sector_size = d->sector_size;
		d->size_bytes = (uint64_t)st.st_size;
	} else {
		int ssz = 0;
		unsigned int pssz = 0;
		uint64_t bytes = 0;

		if (ioctl(fd, BLKSSZGET, &ssz) != 0 || ssz < 512 ||
		    (ssz & (ssz - 1)) != 0)
			ssz = 512;
		d->sector_size = (uint32_t)ssz;
		if (ioctl(fd, BLKPBSZGET, &pssz) != 0 || pssz < (unsigned)ssz)
			pssz = (unsigned)ssz;
		d->phys_sector_size = pssz;
		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
			unsigned long sectors512 = 0;

			if (ioctl(fd, BLKGETSIZE, &sectors512) != 0) {
				off_t end = lseek(fd, 0, SEEK_END);

				if (end < 0) {
					rc = -errno;
					gpt_dev_close(d);
					return rc;
				}
				bytes = (uint64_t)end;
			} else {
				bytes = (uint64_t)sectors512 * 512ULL;
			}
		}
		d->size_bytes = bytes;
	}
	d->nsectors = d->size_bytes / d->sector_size;
	if (d->nsectors < 4) {
		gpt_dev_close(d);
		return -ENOSPC;
	}
	return 0;
}

void gpt_dev_close(struct gpt_dev *d)
{
	if (!d)
		return;
	if (d->fd >= 0)
		close(d->fd);
	d->fd = -1;
}

static int gpt_check_range(const struct gpt_dev *d, uint64_t lba,
			   uint64_t count)
{
	if (!d || d->fd < 0)
		return -EBADF;
	if (count == 0)
		return 0;
	if (lba >= d->nsectors || count > d->nsectors - lba)
		return -ENOSPC;
	if (count > (uint64_t)SIZE_MAX / d->sector_size)
		return -EOVERFLOW;
	return 0;
}

/* Full-length pread, looping over short reads and EINTR. */
static int gpt_pread_all(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = pread(fd, p, len, (off_t)off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO; /* end of file inside the device size */
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static int gpt_pwrite_all(int fd, const void *buf, size_t len, uint64_t off)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = pwrite(fd, p, len, (off_t)off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

int gpt_dev_read(struct gpt_dev *d, uint64_t lba, uint64_t count, void *buf)
{
	int rc = gpt_check_range(d, lba, count);

	if (rc || count == 0)
		return rc;
	return gpt_pread_all(d->fd, buf, (size_t)(count * d->sector_size),
			     lba * d->sector_size);
}

int gpt_dev_write(struct gpt_dev *d, uint64_t lba, uint64_t count,
		  const void *buf)
{
	int rc = gpt_check_range(d, lba, count);

	if (rc || count == 0)
		return rc;
	if (!d->writable)
		return -EBADF;
	return gpt_pwrite_all(d->fd, buf, (size_t)(count * d->sector_size),
			      lba * d->sector_size);
}

int gpt_dev_pread(struct gpt_dev *d, uint64_t off, size_t len, void *buf)
{
	uint32_t ss;
	uint64_t first, last;
	uint8_t *tmp;
	int rc;

	if (!d || d->fd < 0)
		return -EBADF;
	if (len == 0)
		return 0;
	ss = d->sector_size;
	if (off >= d->size_bytes || len > d->size_bytes - off)
		return -ENOSPC;
	if (off % ss == 0 && len % ss == 0)
		return gpt_dev_read(d, off / ss, len / ss, buf);
	first = off / ss;
	last = (off + len - 1) / ss;
	tmp = malloc((size_t)((last - first + 1) * ss));
	if (!tmp)
		return -ENOMEM;
	rc = gpt_dev_read(d, first, last - first + 1, tmp);
	if (!rc)
		memcpy(buf, tmp + (off - first * ss), len);
	free(tmp);
	return rc;
}

int gpt_dev_pwrite(struct gpt_dev *d, uint64_t off, size_t len,
		   const void *buf)
{
	uint32_t ss;
	uint64_t first, last, nsec;
	uint8_t *tmp;
	int rc;

	if (!d || d->fd < 0)
		return -EBADF;
	if (!d->writable)
		return -EBADF;
	if (len == 0)
		return 0;
	ss = d->sector_size;
	if (off >= d->size_bytes || len > d->size_bytes - off)
		return -ENOSPC;
	if (off % ss == 0 && len % ss == 0)
		return gpt_dev_write(d, off / ss, len / ss, buf);
	first = off / ss;
	last = (off + len - 1) / ss;
	nsec = last - first + 1;
	tmp = malloc((size_t)(nsec * ss));
	if (!tmp)
		return -ENOMEM;
	/* Only the partial first and last sectors need their old contents. */
	rc = gpt_dev_read(d, first, 1, tmp);
	if (!rc && nsec > 1)
		rc = gpt_dev_read(d, last, 1, tmp + (nsec - 1) * ss);
	if (!rc) {
		memcpy(tmp + (off - first * ss), buf, len);
		rc = gpt_dev_write(d, first, nsec, tmp);
	}
	free(tmp);
	return rc;
}

int gpt_dev_zero(struct gpt_dev *d, uint64_t off, uint64_t len)
{
	const size_t chunk = 1024 * 1024;
	uint8_t *zero;
	int rc = 0;

	if (!d || d->fd < 0)
		return -EBADF;
	if (!d->writable)
		return -EBADF;
	if (len == 0)
		return 0;
	if (off >= d->size_bytes || len > d->size_bytes - off)
		return -ENOSPC;
	zero = calloc(1, chunk);
	if (!zero)
		return -ENOMEM;
	while (len > 0) {
		size_t n = len < chunk ? (size_t)len : chunk;

		/* Keep the bulk of the writes sector aligned. */
		if (off % d->sector_size) {
			size_t head = d->sector_size - (size_t)(off % d->sector_size);

			if (n > head)
				n = head;
		}
		rc = gpt_dev_pwrite(d, off, n, zero);
		if (rc)
			break;
		off += n;
		len -= n;
	}
	free(zero);
	return rc;
}

int gpt_dev_sync(struct gpt_dev *d)
{
	if (!d || d->fd < 0)
		return -EBADF;
	if (fsync(d->fd) != 0)
		return -errno;
	if (!d->is_file) {
		/* Drop the cached sectors so a re-read sees the new table;
		 * not every kernel offers it, which is fine. */
		(void)ioctl(d->fd, BLKFLSBUF, 0);
	}
	return 0;
}

int gpt_dev_reread(struct gpt_dev *d, int tries, int delay_ms)
{
	int err = 0;

	if (!d || d->fd < 0)
		return -EBADF;
	if (d->is_file)
		return 0;
	if (tries < 1)
		tries = 1;
	for (int i = 0; i < tries; i++) {
		if (ioctl(d->fd, BLKRRPART, 0) == 0)
			return 0;
		err = errno;
		if (err != EBUSY && err != EAGAIN && err != EINTR)
			break;
		if (i + 1 < tries)
			gpt_sleep_ms(delay_ms);
	}
	return -(err ? err : EIO);
}
