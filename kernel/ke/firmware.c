// LikeOS -- firmware blobs, read from the root filesystem.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/firmware.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

int firmware_request(const char *name, void **data, size_t *len)
{
	char path[160];
	struct kstat st;
	vfs_file_t *f = NULL;
	uint8_t *buf;
	long got, total = 0;
	int rc;

	if (!name || !data || !len)
		return -EINVAL;
	*data = NULL;
	*len = 0;
	/* No walking out of the firmware directory through the name. */
	for (const char *q = name; *q; q++)
		if (q[0] == '.' && q[1] == '.')
			return -EINVAL;
	ksnprintf(path, sizeof(path), FIRMWARE_DIR "/%s", name);

	rc = vfs_stat(path, &st);
	if (rc != ST_OK)
		return -ENOENT;
	if (st.st_size == 0)
		return -EIO;
	if (st.st_size > FIRMWARE_MAX_BYTES)
		return -EFBIG;
	rc = vfs_open(path, 0, &f);
	if (rc != ST_OK || !f)
		return -ENOENT;
	buf = kalloc((size_t)st.st_size);
	if (!buf) {
		vfs_close(f);
		return -ENOMEM;
	}
	while (total < (long)st.st_size) {
		got = vfs_read(f, buf + total, (long)st.st_size - total);
		if (got <= 0)
			break;
		total += got;
	}
	vfs_close(f);
	if (total != (long)st.st_size) {
		kprintf("firmware: %s: short read (%ld of %lu bytes)\n", name,
			total, (unsigned long)st.st_size);
		kfree(buf);
		return -EIO;
	}
	*data = buf;
	*len = (size_t)total;
	return 0;
}

void firmware_release(void *data)
{
	if (data)
		kfree(data);
}
