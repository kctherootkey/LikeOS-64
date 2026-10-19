/* <drm/sync_file.h> -- fences as descriptors: poll() readable when signalled;
 * SYNC_IOC_MERGE joins two into one that signals when both have,
 * SYNC_IOC_FILE_INFO lists the fences a descriptor stands for, and
 * SYNC_IOC_SET_DEADLINE hints when the caller would like it signalled.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _UAPI_SYNC_FILE_H
#define _UAPI_SYNC_FILE_H
#include <kernel/uapi/types.h>
#include <kernel/uapi/ioctl.h>
/* SYNC_IOC_MERGE: a new descriptor (always close-on-exec) for a fence
 * that signals when this one and `fd2' have.  `flags' and `pad' must be
 * zero. */
struct sync_merge_data {
	char name[32];
	int32_t fd2;
	int32_t fence;
	uint32_t flags;
	uint32_t pad;
};
/* One fence of a sync_file: status 1 signalled, 0 pending, negative the
 * error it completed with; timestamp_ns when it signalled. */
struct sync_fence_info {
	char obj_name[32];
	char driver_name[32];
	int32_t status;
	uint32_t flags;
	uint64_t timestamp_ns;
};
/* SYNC_IOC_FILE_INFO: num_fences 0 asks only how many there are; else
 * `sync_fence_info' points at room for num_fences entries.  `flags' and
 * `pad' must be zero. */
struct sync_file_info {
	char name[32];
	int32_t status;
	uint32_t flags;
	uint32_t num_fences;
	uint32_t pad;
	uint64_t sync_fence_info;
};
/* SYNC_IOC_SET_DEADLINE: the time (CLOCK_MONOTONIC, ns) by which the
 * caller would like the fence signalled -- a hint.  `pad' must be zero. */
struct sync_set_deadline {
	uint64_t deadline_ns;
	uint64_t pad;
};
#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO _IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)
#define SYNC_IOC_SET_DEADLINE _IOW(SYNC_IOC_MAGIC, 5, struct sync_set_deadline)
#endif
