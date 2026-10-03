// LikeOS -- display-manager core: sync_file descriptors.
//
// A sync_file is a fence as a file descriptor, passed between processes
// and APIs: exporting a fence into one, the fence behind one, merging two,
// the per-fence information listing, deadlines.
//
// poll() sleeps on the fence's own queue, which every signalling wakes;
// the descriptor's reference keeps the fence (and so the queue) alive for
// as long as anyone can poll it.  A container fence is asked to watch its
// parts the first time it is polled, so that it signals by itself.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Google's code:
// GPL-2.0-only
// Portions Copyright (C) 2012 Google, Inc.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_sync_file.h>
#include <kernel/dev/gpu/drm_fence_unwrap.h>
#include <kernel/uapi/drm/sync_file.h>
#include <kernel/uapi/ioctl.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/syscalls.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/dev/gpu/gpu_util.h>

/* SYNC_IOC_MERGE builds its fence by flattening both sides and keeping the
 * latest fence of each timeline (an array when several remain).  0 goes
 * back to joining the two as one merged pair (drm_fence_merge()). */
#define DRM_SYNC_FILE_MERGE_UNWRAP 1

static const struct device_ops sync_file_ops;

/* The sync_file behind descriptor `fd' of the caller, with the file held
 * (fdput() it), or NULL. */
static struct drm_sync_file *sync_file_fdget(int fd, vfs_file_t **filep)
{
	task_t *cur = sched_current();
	vfs_file_t *f = fdget(cur, fd);

	if (!f)
		return NULL;
	if (device_file_ops(f) != &sync_file_ops) {
		fdput(f);
		return NULL;
	}
	*filep = f;
	return device_file_priv(f);
}

struct drm_fence *drm_sync_file_get_fence(int fd)
{
	struct drm_sync_file *sync_file;
	struct drm_fence *fence;
	vfs_file_t *file;

	sync_file = sync_file_fdget(fd, &file);
	if (!sync_file)
		return NULL;

	fence = drm_fence_ref(sync_file->fence);
	fdput(file);

	return fence;
}

char *drm_sync_file_get_name(struct drm_sync_file *sync_file, char *buf, int len)
{
	if (sync_file->user_name[0]) {
		ksnprintf(buf, (size_t)len, "%s", sync_file->user_name);
	} else {
		struct drm_fence *fence = sync_file->fence;
		char timeline[32];

		drm_fence_timeline_name(fence, timeline, sizeof(timeline));
		ksnprintf(buf, (size_t)len, "%s-%s%llu-%llu",
			  drm_fence_driver_name(fence), timeline,
			  (unsigned long long)fence->context,
			  (unsigned long long)(fence->context ? fence->seqno64 :
								fence->seqno));
	}

	return buf;
}

int drm_sync_file_install(struct drm_fence *fence, const char *name,
			  int cloexec)
{
	task_t *cur = sched_current();
	struct drm_sync_file *sync_file = kalloc(sizeof(*sync_file));

	if (!sync_file)
		return -ENOMEM;
	mm_memset(sync_file, 0, sizeof(*sync_file));
	sync_file->fence = drm_fence_ref(fence);
	if (name)
		ksnprintf(sync_file->user_name, sizeof(sync_file->user_name),
			  "%s", name);
	vfs_file_t *file = device_anon_file(&sync_file_ops, sync_file,
					    "sync_file", O_RDWR);
	if (!file) {
		drm_fence_put(fence);
		kfree(sync_file);
		return -ENOMEM;
	}
	file->refcount = 1;
	int fd = fd_install(cur, file);
	if (fd < 0) {
		vfs_close(file);
		return fd;
	}
	if (cloexec)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	return fd;
}

/* ---- the long-standing entry points (drm.h) ----------------------------- */

int drm_fence_export_fd(struct drm_fence *f, int cloexec)
{
	return drm_sync_file_install(f, NULL, cloexec);
}

struct drm_fence *drm_fence_from_fd(int fd)
{
	struct drm_fence *fence = drm_sync_file_get_fence(fd);

	/* The fence goes to code that looks at `signaled' directly (buffer
	 * busy checks, merged fences): a container has to be watching its
	 * parts for that flag ever to be set. */
	if (fence)
		drm_fence_enable_signaling(fence);
	return fence;
}

/* ---- ioctls ---------------------------------------------------------- */

static long sync_file_ioctl_merge(struct drm_sync_file *sync_file, void *argp)
{
	struct sync_merge_data data;
	struct drm_fence *fences[2], *fence;
	int fd;

	if (copy_from_user(&data, argp, sizeof(data)) != 0)
		return -EFAULT;

	if (data.flags || data.pad)
		return -EINVAL;

	fences[0] = sync_file->fence;
	fences[1] = drm_sync_file_get_fence(data.fd2);
	if (!fences[1])
		return -ENOENT;

	data.name[sizeof(data.name) - 1] = '\0';
	/* Every fence either side stands for, the latest of each timeline
	 * once.  (Taking the later of two fences is no answer when they are
	 * on different timelines: an engine's fences number a stream of
	 * their own, and two engines' streams say nothing about each
	 * other.) */
#if DRM_SYNC_FILE_MERGE_UNWRAP
	fence = drm_fence_unwrap_merge(2, fences);
#else
	fence = drm_fence_merge(fences[0], fences[1]);
#endif
	drm_fence_put(fences[1]);
	if (!fence)
		return -ENOMEM;

	fd = drm_sync_file_install(fence, data.name, 1);
	drm_fence_put(fence);
	if (fd < 0)
		return fd;

	data.fence = fd;
	if (copy_to_user(argp, &data, sizeof(data)) != 0) {
		/* nobody would ever learn the descriptor's number */
		sys_close((uint64_t)fd);
		return -EFAULT;
	}
	return 0;
}

static int sync_fill_fence_info(struct drm_fence *fence,
				struct sync_fence_info *info)
{
	drm_fence_timeline_name(fence, info->obj_name, sizeof(info->obj_name));
	ksnprintf(info->driver_name, sizeof(info->driver_name), "%s",
		  drm_fence_driver_name(fence));

	info->status = drm_fence_get_status(fence);
	info->timestamp_ns = drm_fence_is_signaled(fence) ?
				     drm_fence_timestamp(fence) : 0;

	return info->status;
}

static long sync_file_ioctl_fence_info(struct drm_sync_file *sync_file,
				       void *argp)
{
	struct sync_fence_info *fence_info = NULL;
	struct drm_fence_unwrap iter;
	struct sync_file_info info;
	unsigned int num_fences, room;
	struct drm_fence *fence;
	long ret;
	uint32_t size;

	if (copy_from_user(&info, argp, sizeof(info)) != 0)
		return -EFAULT;

	if (info.flags || info.pad)
		return -EINVAL;

	num_fences = 0;
	drm_fence_unwrap_for_each(fence, &iter, sync_file->fence)
		++num_fences;

	/* num_fences = 0 asks only how many there are: no entries are
	 * filled, the count is returned. */
	if (!info.num_fences) {
		info.status = drm_fence_get_status(sync_file->fence);
		goto no_fences;
	} else {
		info.status = 1;
	}

	if (info.num_fences < num_fences)
		return -EINVAL;

	size = num_fences * sizeof(*fence_info);
	fence_info = kcalloc(num_fences ? num_fences : 1, sizeof(*fence_info));
	if (!fence_info)
		return -ENOMEM;

	/* A second walk may find fewer (signalled links are cut out of a
	 * chain as it is walked), never more. */
	room = num_fences;
	num_fences = 0;
	drm_fence_unwrap_for_each(fence, &iter, sync_file->fence) {
		int status;

		if (num_fences >= room) {
			drm_fence_unwrap_end(&iter);
			break;
		}
		status = sync_fill_fence_info(fence, &fence_info[num_fences++]);
		info.status = info.status <= 0 ? info.status : status;
	}
	size = num_fences * sizeof(*fence_info);

	if (size && copy_to_user((void *)(uintptr_t)info.sync_fence_info,
				 fence_info, size) != 0) {
		ret = -EFAULT;
		goto out;
	}

no_fences:
	drm_sync_file_get_name(sync_file, info.name, sizeof(info.name));
	info.num_fences = num_fences;

	if (copy_to_user(argp, &info, sizeof(info)) != 0)
		ret = -EFAULT;
	else
		ret = 0;

out:
	kfree(fence_info);

	return ret;
}

static long sync_file_ioctl_set_deadline(struct drm_sync_file *sync_file,
					 void *argp)
{
	struct sync_set_deadline ts;

	if (copy_from_user(&ts, argp, sizeof(ts)) != 0)
		return -EFAULT;

	if (ts.pad)
		return -EINVAL;

	drm_fence_set_deadline(sync_file->fence, ts.deadline_ns);

	return 0;
}

static long sync_file_ioctl(vfs_file_t *f, unsigned long req, void *argp,
			    struct task *cur)
{
	struct drm_sync_file *sync_file = device_file_priv(f);
	(void)cur;

	if (_IOC_TYPE(req) != SYNC_IOC_MAGIC)
		return -ENOTTY;
	switch (_IOC_NR(req)) {
	case _IOC_NR(SYNC_IOC_MERGE):
		return sync_file_ioctl_merge(sync_file, argp);
	case _IOC_NR(SYNC_IOC_FILE_INFO):
		return sync_file_ioctl_fence_info(sync_file, argp);
	case _IOC_NR(SYNC_IOC_SET_DEADLINE):
		return sync_file_ioctl_set_deadline(sync_file, argp);
	default:
		return -ENOTTY;
	}
}

static short sync_file_poll(vfs_file_t *f, short events, struct poll_table *pt)
{
	struct drm_sync_file *sync_file = device_file_priv(f);

	poll_wait(pt, f, &sync_file->fence->wq);

	if (!sync_file->poll_enabled) {
		sync_file->poll_enabled = 1;
		drm_fence_enable_signaling(sync_file->fence);
	}

	return (events & POLLIN) && drm_fence_is_signaled(sync_file->fence) ?
		       POLLIN : 0;
}

static void sync_file_release(vfs_file_t *f)
{
	struct drm_sync_file *sync_file = device_file_priv(f);

	if (sync_file) {
		drm_fence_put(sync_file->fence);
		kfree(sync_file);
	}
}

static const struct device_ops sync_file_ops = {
	.ioctl = sync_file_ioctl,
	.poll = sync_file_poll,
	.release = sync_file_release,
};
