// LikeOS -- vmwgfx: id and memory accounting for guest memory regions and
// MOBs.
//
// The device offers a fixed number of ids for guest memory regions (GMRs)
// and for MOBs, and a budget of pages behind them: SVGA_REG_GMR_MAX_IDS /
// GMRS_MAX_PAGES for regions, the guest-backed memory pool for MOBs.
// Asking for more than the device has is answered by the device with a
// command error -- which halts the command-buffer context, not just the
// request.  So every buffer object's id and pages are accounted here first,
// and a buffer that would not fit is refused with -ENOSPC before any
// command is sent.
//
// The page budget is a soft limit, the way the device treats it: when it is
// exceeded it is doubled (up to half of RAM) with a warning on the console
// and in the host's log, and only an object that would not fit even then is
// refused.
//
// Ids:
//   VMW_PL_MOB  -- allocated here, lowest free id first (id 0 is never
//                  handed out), below VMW_NUM_MOBS, the MOB OTable's size.
//   VMW_PL_GMR  -- the hardware layer owns the region id table
//                  (vmsvga2_gmr_alloc/free), and there must be only one
//                  allocator: get_node takes the id from it after the page
//                  budget, and put_node gives it back with
//                  vmsvga2_gmr_free(), which also unbinds the region.  So a
//                  region's owner calls get_node, vmsvga2_gmr_bind() and, at
//                  the end, put_node -- never vmsvga2_gmr_free() itself.
//
// struct vmw_gmrid_man is private to vmw_gmrid.c; vmw_device.gmrid_man[]
// holds one per type.  The managers are created by vmw_gb_init() and must
// outlive every object accounted against them.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2007-2010 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_GMRID_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_GMRID_H

#include <kernel/uapi/types.h>

struct vmw_device;
struct vmw_gmrid_man;

/* The id spaces accounted. */
enum vmw_gmrid_type {
	VMW_PL_GMR = 0,
	VMW_PL_MOB = 1,
	VMW_PL_MAX
};

/* Create the manager of `type' from the device's limits.  0 or -errno
 * (-ENODEV: the device has no objects of that type).  Without a manager:
 * VMW_PL_GMR ids still come from the hardware layer, unaccounted;
 * VMW_PL_MOB get_node fails with -ENODEV, since there are no MOBs. */
int vmw_gmrid_man_init(struct vmw_device *v, int type);
/* Tear the manager down.  Objects still accounted are reported, not
 * freed. */
void vmw_gmrid_man_fini(struct vmw_device *v, int type);
/* Take an id and `num_pages' of budget.  0 with *id_out set, -ENOSPC when
 * no id is free or the pages exceed even the grown budget, -ENODEV as
 * above.  *id_out is SVGA3D_INVALID_ID on failure.  Process context: a
 * region id comes from the hardware layer, and the host-log warning may be
 * sent. */
int vmw_gmrid_man_get_node(struct vmw_device *v, int type, uint32_t num_pages,
			   uint32_t *id_out);
/* Give back what vmw_gmrid_man_get_node() handed out: the same id and the
 * same `num_pages'.  For VMW_PL_GMR this unbinds and frees the region. */
void vmw_gmrid_man_put_node(struct vmw_device *v, int type, uint32_t id,
			    uint32_t num_pages);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_GMRID_H */
