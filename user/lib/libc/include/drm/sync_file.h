/* <drm/sync_file.h> -- fences as descriptors: poll() readable when signalled;
 * SYNC_IOC_MERGE joins two into one that signals when both have.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _UAPI_SYNC_FILE_H
#define _UAPI_SYNC_FILE_H
#include <stdint.h>
#include <sys/ioctl.h>
struct sync_merge_data {
	char name[32];
	int32_t fd2;
	int32_t fence;
	uint32_t flags;
	uint32_t pad;
};
struct sync_fence_info {
	char obj_name[32];
	char driver_name[32];
	int32_t status;
	uint32_t flags;
	uint64_t timestamp_ns;
};
struct sync_file_info {
	char name[32];
	int32_t status;
	uint32_t flags;
	uint32_t num_fences;
	uint32_t pad;
	uint64_t sync_fence_info;
};
#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO _IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)
/* Tell the fence's producer when its result is wanted, as an absolute
 * CLOCK_MONOTONIC time in nanoseconds; pad must be zero. */
struct sync_set_deadline {
	uint64_t deadline_ns;
	uint64_t pad;
};
#define SYNC_IOC_SET_DEADLINE _IOW(SYNC_IOC_MAGIC, 5, struct sync_set_deadline)
#endif
