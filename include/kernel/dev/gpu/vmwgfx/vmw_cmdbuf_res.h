// LikeOS -- vmwgfx: objects a command stream creates and destroys.
//
// Views, DX shaders and stream-output objects are not created by an ioctl:
// the client defines them with commands inside its own command stream, in
// its own DX context, under ids it chose.  The driver still has to know
// them -- a view pins its surface, a shader its code -- so each DX context
// carries a manager that maps (type, client id) to the object.
//
// What a stream defines or removes is STAGED on a list while the stream is
// verified: committed when the stream is handed to the device, reverted when
// it is refused, so a rejected submission leaves the manager exactly as it
// was.  Callers serialise on vmw_device.execbuf_lock.
//
// Commit, revert and destroy can drop the last reference to an object, and
// releasing a view takes vmw_device.binding_lock: call them without
// binding_lock held.  Commit before dropping the submission's references to
// the buffers and surfaces it named -- a surface destroyed first would miss
// the views its stream just defined.
//
// struct vmw_cmdbuf_res_manager is private to vmw_cmdbuf_res.c.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2014-2022 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_CMDBUF_RES_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_CMDBUF_RES_H

#include <kernel/dev/gpu/vmwgfx/vmw_resource.h>

struct vmw_cmdbuf_res_manager;

/* A new, empty manager, or ERR_PTR(-ENOMEM). */
struct vmw_cmdbuf_res_manager *vmw_cmdbuf_res_man_create(struct vmw_device *v);
/* Drop every committed entry (and its reference) and the manager. */
void vmw_cmdbuf_res_man_destroy(struct vmw_cmdbuf_res_manager *man);
/* The object under (res_type, user_key), staged or committed, or
 * ERR_PTR(-EINVAL).  Not referenced: valid while the caller holds
 * execbuf_lock. */
struct vmw_resource *vmw_cmdbuf_res_lookup(struct vmw_cmdbuf_res_manager *man,
					   enum vmw_cmdbuf_res_type res_type,
					   u32 user_key);
/* Undo every staged add and remove on `list'. */
void vmw_cmdbuf_res_revert(struct list_head *list);
/* Tell every resource on `list' that its staged add or remove reached the
 * device (commit_notify).  Caller holds binding_lock, the same hold as the
 * submission: a view the stream destroyed is dead before anything else can
 * look at it -- a surface destroyed in between would otherwise destroy it
 * on the device a second time. */
void vmw_cmdbuf_res_commit_notify(struct list_head *list);
/* Then make every staged add and remove on `list' permanent, without
 * binding_lock (a removed resource may be released). */
void vmw_cmdbuf_res_commit(struct list_head *list);
/* Stage `res' under (res_type, user_key); the manager takes a reference. */
int vmw_cmdbuf_res_add(struct vmw_cmdbuf_res_manager *man,
		       enum vmw_cmdbuf_res_type res_type, u32 user_key,
		       struct vmw_resource *res, struct list_head *list);
/* Stage the removal of (res_type, user_key): -EINVAL if there is none.
 * *res_p is the committed object now staged for removal (not referenced),
 * or NULL when a merely staged add was dropped. */
int vmw_cmdbuf_res_remove(struct vmw_cmdbuf_res_manager *man,
			  enum vmw_cmdbuf_res_type res_type, u32 user_key,
			  struct list_head *list, struct vmw_resource **res_p);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_CMDBUF_RES_H */
