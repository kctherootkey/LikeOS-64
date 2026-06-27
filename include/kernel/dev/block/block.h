// LikeOS-64 - Block device abstraction
#ifndef LIKEOS_BLOCK_H
#define LIKEOS_BLOCK_H

#include <kernel/uapi/status.h>
#include <kernel/uapi/types.h>

#define BLOCK_MAX_DEVICES 8

typedef struct block_device block_device_t;

typedef int (*block_read_fn)(block_device_t *dev, unsigned long lba,
			     unsigned long count, void *buf);
typedef int (*block_write_fn)(block_device_t *dev, unsigned long lba,
			      unsigned long count, const void *buf);
typedef int (*block_sync_fn)(block_device_t *dev);
/* Force-release any per-device sleeping locks held by the given task id.
 * Used by the FS-layer release_locks_for_task hook to recover from a task
 * that was killed mid-I/O while holding the device's own sleeping mutex
 * (e.g. usb_msd's per-device io_locked, which a fat32_io_lock holder
 * acquires nested when reading/writing sectors). */
typedef int (*block_release_locks_fn)(block_device_t *dev, uint64_t task_id);

struct block_device {
	const char *name;
	unsigned int sector_size; // bytes per sector
	unsigned long total_sectors;
	block_read_fn read;
	block_write_fn write;
	block_sync_fn sync; // Optional: flush write cache to media
	block_release_locks_fn release_locks_for_task; // Optional
	void *driver_data; // pointer to underlying msd/scsi device
};

int block_register(block_device_t *dev);
const block_device_t *block_get(int index);
int block_count(void);
int block_sync(block_device_t *dev);

#endif // LIKEOS_BLOCK_H
