// LikeOS -- vmwgfx: reserve / commit command emission.
//
// The idiom most device code is written in: reserve room for a command,
// fill it in place, commit it.  Here the room is a staging buffer owned by
// the device and held under a sleeping lock from the reservation to the
// commit; the commit hands the bytes to the channel that is up, through the
// same entry points everything else in the driver uses, so a command emitted
// this way is ordered with every other command exactly as if it had been
// passed to vmw_cmd_raw() or vmw_cmd_submit_async() directly:
//   - no DX context: vmw_cmd_raw().  It takes FIFO-format and SVGA3D
//     commands alike, gathers them into the command-buffer batch while that
//     channel is up and writes them to the FIFO otherwise;
//   - a DX context: vmw_cmd_submit_async() on that context, which needs
//     command buffers -- the reservation itself fails without them.
// vmw_cmd_commit() leaves gathered commands for the next hand-over (a
// fence, a flush, a full batch); vmw_cmd_commit_flush() hands them over at
// once.  On the FIFO every commit is announced.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2009-2023 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/io/console.h>

/* The largest reservation the FIFO path takes.  The same bound the FIFO arm
 * of vmw_cmd_submit() chunks at; with command buffers the bound is a slot. */
#define VMW_CMD_RESERVE_FIFO_MAX (64 * 1024)
/* The staging buffer starts at a page and doubles from there. */
#define VMW_CMD_RESERVE_MIN_CAP 4096u

void vmw_cmd_reserve_init(struct vmw_device *v)
{
	struct vmw_cmd_reserve_state *r = &v->cmd_reserve;

	mm_rwsem_init(&r->lock, "vmw_cmd_reserve");
	r->buf = NULL;
	r->cap = 0;
	r->reserved = 0;
	r->ctx_id = SVGA3D_INVALID_ID;
	r->open = 0;
}

static uint32_t vmw_cmd_reserve_limit(const struct vmw_device *v)
{
	if (v->cb_ready && v->cb_size)
		return v->cb_size;
	return VMW_CMD_RESERVE_FIFO_MAX;
}

void *vmw_cmd_ctx_reserve(struct vmw_device *v, uint32_t bytes, int ctx_id)
{
	struct vmw_cmd_reserve_state *r = &v->cmd_reserve;
	uint32_t ctx = (uint32_t)ctx_id;

	if (bytes == 0 || bytes > vmw_cmd_reserve_limit(v))
		return NULL;
	/* A DX context is addressed through a command buffer only. */
	if (ctx != SVGA3D_INVALID_ID && !v->cb_ready)
		return NULL;

	mm_write_lock(&r->lock);
	/* The lock is reentrant for its holder, so a second reservation by
	 * the same thread before committing the first gets this far -- and
	 * would hand out the bytes the first is still filling. */
	if (r->open) {
		mm_write_unlock(&r->lock);
		WARN_ON_ONCE(1);
		return NULL;
	}
	if (bytes > r->cap) {
		uint32_t cap = r->cap ? r->cap : VMW_CMD_RESERVE_MIN_CAP;
		uint8_t *nb;

		while (cap < bytes)
			cap *= 2;
		nb = kalloc(cap);
		if (!nb) {
			mm_write_unlock(&r->lock);
			return NULL;
		}
		kfree(r->buf);
		r->buf = nb;
		r->cap = cap;
	}
	r->reserved = bytes;
	r->ctx_id = ctx;
	r->open = 1;
	return r->buf;
}

void *vmw_cmd_ctx_reserve_named(struct vmw_device *v, uint32_t bytes,
				int ctx_id, const char *caller)
{
	void *p = vmw_cmd_ctx_reserve(v, bytes, ctx_id);

	if (!p) {
		static int budget = 8;

		if (budget > 0) {
			budget--;
			kprintf("[drm] vmwgfx: command reserve failed at %s for %u bytes\n",
				caller, bytes);
		}
	}
	return p;
}

static void vmw_cmd_commit_common(struct vmw_device *v, uint32_t bytes,
				  int flush)
{
	struct vmw_cmd_reserve_state *r = &v->cmd_reserve;
	int rc = 0;

	if (WARN_ON_ONCE(!r->open))
		return;
	if (WARN_ON_ONCE(bytes > r->reserved))
		bytes = r->reserved;
	if (bytes) {
		if (r->ctx_id != SVGA3D_INVALID_ID) {
			rc = vmw_cmd_submit_async(v, r->buf, bytes, r->ctx_id);
			if (rc == 0 && flush)
				vmw_cmd_flush(v);
		} else {
			/* On the FIFO a commit is always announced; gathered,
			 * only a flushing commit hands the batch over. */
			rc = vmw_cmd_raw(v, r->buf, bytes,
					 flush || !v->cb_ready);
		}
	}
	r->open = 0;
	r->reserved = 0;
	r->ctx_id = SVGA3D_INVALID_ID;
	mm_write_unlock(&r->lock);
	if (rc) {
		static int budget = 8;

		if (budget > 0) {
			budget--;
			kprintf("[drm] vmwgfx: command commit of %u bytes failed (%d)\n",
				bytes, rc);
		}
	}
}

void vmw_cmd_commit(struct vmw_device *v, uint32_t bytes)
{
	vmw_cmd_commit_common(v, bytes, 0);
}

void vmw_cmd_commit_flush(struct vmw_device *v, uint32_t bytes)
{
	vmw_cmd_commit_common(v, bytes, 1);
}
