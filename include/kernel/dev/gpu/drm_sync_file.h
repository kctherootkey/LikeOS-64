// LikeOS -- display-manager core: sync_file descriptors.
//
// A sync_file is a fence as a file descriptor, passed between processes
// and between APIs.  drm_fence_export_fd() and drm_fence_from_fd() (drm.h)
// are the usual way in and out; this is the rest.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Google's code:
// GPL-2.0-only
// Portions Copyright (C) 2012 Google, Inc.

#ifndef KERNEL_DEV_GPU_DRM_SYNC_FILE_H
#define KERNEL_DEV_GPU_DRM_SYNC_FILE_H

#include <kernel/dev/gpu/drm.h>

struct drm_sync_file {
	struct drm_fence *fence; /* referenced */
	/* The name given at SYNC_IOC_MERGE; empty: made up from the fence. */
	char user_name[32];
	/* poll() has asked the fence to signal its waiters (once). */
	int poll_enabled;
};

/* A new descriptor of the caller for `fence' (referenced by the call),
 * named `name' (NULL: none), close-on-exec when `cloexec'.  The fd or a
 * negative error. */
int drm_sync_file_install(struct drm_fence *fence, const char *name,
			  int cloexec);
/* The fence behind descriptor `fd', referenced, or NULL. */
struct drm_fence *drm_sync_file_get_fence(int fd);
/* The sync_file's name into `buf'. */
char *drm_sync_file_get_name(struct drm_sync_file *sync_file, char *buf,
			     int len);

#endif
