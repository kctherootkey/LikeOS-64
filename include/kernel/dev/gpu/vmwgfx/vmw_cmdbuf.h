// LikeOS -- vmwgfx: command submission.
//
// Commands reach the device through command buffers on command-buffer
// context 0 (which can also name a DX context per buffer) or, on hosts
// without them, the FIFO.  vmw_cmdbuf.c owns the channel: the slots, the
// gather buffer, completion and error recovery, and the pages held back
// until the device has stopped reading them.  vmw_cmd_reserve.c puts the
// reserve/commit interface on top of it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2015-2023 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_CMDBUF_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_CMDBUF_H

#include <kernel/ke/sched.h>
#include <kernel/dev/gpu/vmwgfx/vmw_compat.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_reg.h>

struct vmw_device;

/* One command buffer in flight: its header page, its payload, and the
 * bookkeeping needed to reap or repair it after the submitter has moved on. */
struct vmw_cb_slot {
	volatile SVGACBHeader *hdr;
	uint64_t hdr_phys;
	uint8_t *buf;
	uint64_t buf_phys;
	uint32_t bytes;
	uint32_t flags;
	uint32_t dx;
	uint32_t ctx;
	uint64_t submitted_us;
	/* Submission order.  A device error is repaired by handing the
	 * buffers back to the device, and they have to go back in the order
	 * they were given -- a define that follows the command that uses it
	 * is a second error.  The slots themselves say nothing about order,
	 * so each records the ticket it was rung with. */
	uint64_t seq;
	/* The SVGA_CMD_FENCE sequence this buffer carries, or 0.
	 *
	 * A fence submitted through this channel has passed when the BUFFER
	 * carrying it completes -- that is the only evidence there is.  The
	 * device's fence register cannot be used for it: that register
	 * belongs to the FIFO, which the console writes to independently and
	 * out of the same counter, so a console fence completing there says
	 * nothing whatever about a command-buffer context. */
	uint32_t fence_seq;
	int retried;
	/* The device's verdict on the payload this slot carried, kept for a
	 * synchronous submitter: a repaired buffer COMPLETES (without the
	 * command the device refused), and a caller that acts on the answer
	 * -- the COTable resize does -- must not read that as success. */
	int err_rc;
	volatile int state;
};

/* The open reservation of vmw_cmd_ctx_reserve() (vmw_cmd_reserve.c).
 *
 * `lock' is held from the reservation to its commit, so the bytes a caller
 * writes into `buf' are its own until they are handed over: one
 * reservation at a time per device, a sleeping lock, process context only. */
struct vmw_cmd_reserve_state {
	mm_rwsem_t lock;
	uint8_t *buf;		/* staging, grown on demand */
	uint32_t cap;		/* bytes allocated at `buf' */
	uint32_t reserved;	/* bytes the open reservation asked for */
	uint32_t ctx_id;	/* DX context of the open reservation */
	int open;
};

/* ---- the command-buffer channel (vmw_cmdbuf.c) ---- */

/* Allocate the slots and the gather buffer and start command-buffer
 * context 0.  Never fails: a device whose context will not start is driven
 * through the FIFO instead (has_cmdbuf and has_dx are cleared). */
int vmw_cmdbuf_bringup(struct vmw_device *v);
/* Submit `bytes' of SVGA3D commands from a kernel buffer, on DX context
 * `dx_cid' (SVGA3D_INVALID_ID for none).  Returns 0 or -errno; the
 * caller emits its own fence afterwards. */
int vmw_cmd_submit(struct vmw_device *v, const void *cmds, uint32_t bytes,
		   uint32_t dx_cid);
/* One command with a fixed header; body is copied. */
int vmw_cmd_one(struct vmw_device *v, uint32_t id, const void *body,
		uint32_t body_size);
int vmw_cmd_one_sync(struct vmw_device *v, uint32_t id, const void *body,
		     uint32_t body_size);
int vmw_cmd_one_async(struct vmw_device *v, uint32_t id, const void *body,
		      uint32_t body_size);
int vmw_cmd_submit_async(struct vmw_device *v, const void *cmds, uint32_t bytes,
			 uint32_t dx_cid);
/* Emit a fence for everything queued so far and return its sequence, or 0.
 * The number is allocated and submitted under one claim, so the order the
 * numbers are handed out is the order the device is told about them. */
uint32_t vmw_cmd_fence_emit(struct vmw_device *v);
/* Device-format bytes (SVGA_CMD_*) down whichever channel the driver owns:
 * the command-buffer one while it is up, the FIFO otherwise.  `ring' = 0
 * queues without announcing, for a run finished by a ring = 1 call or by
 * vmw_cmd_flush(). */
int vmw_cmd_raw(struct vmw_device *v, const void *cmds, uint32_t bytes, int ring);
void vmw_cmdbuf_poll(struct vmw_device *v);
/* Thread context, no locks held (the DRM reaper's tick): hand back the
 * pages held for the device once it has worked through everything. */
void vmw_cmdbuf_idle_reclaim(struct vmw_device *v);
void vmw_cmd_flush(struct vmw_device *v);
void vmw_cmd_drain(struct vmw_device *v);
/* Power management (vmw_pm.c).  suspend: drain, stop command-buffer
 * context 0 and hand the console driver's commands back to the FIFO
 * (vmsvga2_set_cmd_channel(NULL), vmsvga2_set_cmdbuf_owner(0)); the slots
 * stay allocated.  resume: every slot free and clean, context 0 started,
 * the channel registered again -- also on a channel that never went down
 * whose device lost its state; a context that will not start leaves the
 * device on the FIFO (has_cmdbuf/has_dx cleared) and returns the error.
 * Both: process context, nothing else submitting. */
int vmw_cmdbuf_suspend(struct vmw_device *v);
int vmw_cmdbuf_resume(struct vmw_device *v);

/* ---- pages the device may still be reading (vmw_cmdbuf.c) ---- */

/* Hand a buffer object's backing pages back, once the device is done walking
 * the MOB page table that names them.  See the definition in vmw_cmdbuf.c. */
void vmw_defer_free_pages(struct vmw_device *v, const uint64_t *pages,
			  uint32_t n);
/* One page-table page the device was told to stop using. */
void vmw_defer_free_page(struct vmw_device *v, uint64_t phys);

/* ---- reserve / commit (vmw_cmd_reserve.c) ----
 *
 * The way most device code emits a command:
 *
 *	struct { SVGA3dCmdHeader h; SVGA3dCmdFoo body; } *cmd;
 *
 *	cmd = VMW_CMD_RESERVE(v, sizeof(*cmd));
 *	if (!cmd)
 *		return -ENOMEM;
 *	cmd->h.id = SVGA_3D_CMD_FOO;
 *	...
 *	vmw_cmd_commit(v, sizeof(*cmd));
 *
 * The reservation is a staging buffer, not device memory; commit hands the
 * bytes to the channel that is up: gathered into the command-buffer batch
 * (and handed to the device at once by vmw_cmd_commit_flush()), or written
 * to the FIFO and announced.  A reservation naming a DX context needs
 * command buffers and fails (NULL) without them.  Committing fewer bytes
 * than reserved is allowed; committing 0 cancels.  Every reservation must be
 * committed, on every path, because it holds the reservation lock. */

void vmw_cmd_reserve_init(struct vmw_device *v);
void *vmw_cmd_ctx_reserve(struct vmw_device *v, uint32_t bytes, int ctx_id);
void vmw_cmd_commit(struct vmw_device *v, uint32_t bytes);
void vmw_cmd_commit_flush(struct vmw_device *v, uint32_t bytes);
/* vmw_cmd_ctx_reserve() that names the caller when it fails (rate-limited). */
void *vmw_cmd_ctx_reserve_named(struct vmw_device *v, uint32_t bytes,
				int ctx_id, const char *caller);

#define VMW_CMD_CTX_RESERVE(__v, __bytes, __ctx_id) \
	vmw_cmd_ctx_reserve_named((__v), (__bytes), (__ctx_id), __func__)

#define VMW_CMD_RESERVE(__v, __bytes) \
	VMW_CMD_CTX_RESERVE(__v, __bytes, SVGA3D_INVALID_ID)

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_CMDBUF_H */
