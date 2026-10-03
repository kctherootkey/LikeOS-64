// LikeOS -- display-manager core: leases.
//
// Leasing hands a subset of the mode objects to another file as its own
// master.  Not supported: every lease ioctl is refused with -EINVAL, which
// is what a client that probes for leasing expects from a device without
// it.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

/* CREATE_LEASE */
int drm_mode_create_lease_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp)
{
	(void)dev;
	(void)kb;
	(void)fp;
	return -EINVAL;
}

/* LIST_LESSEES */
int drm_mode_list_lessees_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp)
{
	(void)dev;
	(void)kb;
	(void)fp;
	return -EINVAL;
}

/* GET_LEASE */
int drm_mode_get_lease_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp)
{
	(void)dev;
	(void)kb;
	(void)fp;
	return -EINVAL;
}

/* REVOKE_LEASE */
int drm_mode_revoke_lease_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp)
{
	(void)dev;
	(void)kb;
	(void)fp;
	return -EINVAL;
}
