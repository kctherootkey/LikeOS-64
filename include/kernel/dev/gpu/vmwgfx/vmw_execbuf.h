// LikeOS -- vmwgfx: command-stream verification state.
//
// Shared by the submission path (vmw_execbuf.c: the ioctl, the walk over
// the stream, the object pinning and relocation helpers, the per-context
// binding staging, the fence) and the per-command verifier
// (vmw_execbuf_cmds.c: which commands a client may send and what each one's
// ids are checked and rewritten against).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_EXECBUF_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_EXECBUF_H

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>

/* What the verifier does with a stream that breaks one of the rules it has
 * learnt beyond the ones it always enforced -- a count past the device's
 * limit, a view id the context never defined, a command the device model of
 * this context cannot run, a guest pointer past the end of its buffer.
 *
 *   0: name the command and the rule once on the console and let the stream
 *      through exactly as it always went (the bookkeeping that depends on
 *      the broken rule is skipped for that command);
 *   1: refuse the stream with -EINVAL, like every other verifier error.
 *
 * The rules are new and the client is Mesa, whose answer to a refused
 * submission is to drop the batch and carry on with state the device never
 * received; so they start as warnings, and the log says which ones a real
 * session trips before any of them is made binding. */
#ifndef VMW_EXECBUF_STRICT
#define VMW_EXECBUF_STRICT 0
#endif

/* The switch that turns the view and binding bookkeeping on (vmw_so.h is
 * where it is set; this is only the fallback for a tree where it is not). */
#ifndef VMW_TRACK_VIEWS
#define VMW_TRACK_VIEWS 1
#endif

/* A context the stream touches, and the bindings this stream gives it.
 *
 * @ctx:          referenced for the duration of the submission.
 * @staged:       the bindings the stream establishes, committed to the
 *                context's tracker after a successful submission; NULL when
 *                the context's bindings are not tracked.
 * @staged_owned: @staged was allocated for this node (and is freed with
 *                it) rather than borrowed from the device. */
struct vmw_val_ctx {
	struct list_head head;
	struct vmw_context *ctx;
	struct vmw_ctx_binding_state *staged;
	bool staged_owned;
};

/* A command to be turned into a NOP at submission if the object it
 * destroys has already been destroyed by the time the stream goes out
 * (its id is -1 then).  @offset is where the command's header id sits in
 * the stream. */
struct vmw_val_nop {
	uint32_t offset;
	struct vmw_resource *res;
};

/* One submission being verified. */
struct vmw_val {
	struct vmw_device *v;
	struct drm_file *fp;
	struct vmw_context *ctx; /* DX context of the stream, or NULL */
	struct drm_gem_object **refs;
	uint32_t nrefs;
	uint32_t refs_cap;
	/* Set of what refs[] already holds, so the duplicate check below is
	 * not a walk of it.  Open addressing, power-of-two, always twice
	 * refs_cap -- a load factor of one half, which both keeps the probe
	 * short and is what guarantees the loops terminate: the table can
	 * never fill, so an empty slot is always found. */
	struct drm_gem_object **htab;
	uint32_t hmask; /* size - 1 */
	uint32_t cid_seen; /* for legacy per-command cid */
	/* The verifier's bookkeeping. */
	uint8_t *buf_start;		/* the stream being walked */
	struct list_head ctx_list;	/* struct vmw_val_ctx */
	struct vmw_val_ctx dx_node_storage;
	struct vmw_val_ctx *dx_node;	/* the node of `ctx', or NULL */
	bool staged_inuse;		/* v->execbuf_staged is lent out */
	/* The DX context's view/shader manager, NULL when views are not
	 * tracked, and what the stream defines and removes in it. */
	struct vmw_cmdbuf_res_manager *man;
	struct list_head staged_cmd_res;
	struct vmw_val_nop *nops;
	uint32_t nnops;
	uint32_t nops_cap;
	/* The query MOB a DX_BIND_QUERY of this stream names (pinned in
	 * refs[]), remembered on the context once the stream is submitted. */
	struct drm_gem_object *dx_query_mob;
	struct vmw_context *dx_query_ctx;
};

/* ---- object pinning and relocation (vmw_execbuf.c) ---- */

/* The object behind `handle' if it is of `kind', pinned until the
 * submission's fence; NULL otherwise. */
struct drm_gem_object *vmw_val_obj(struct vmw_val *val, uint32_t handle,
				   enum drm_gem_kind kind);
/* Rewrite a surface handle field to the device sid. */
int vmw_val_reloc_sid(struct vmw_val *val, uint32_t *field);
/* Rewrite a buffer handle field to the MOB id. */
int vmw_val_reloc_mob(struct vmw_val *val, uint32_t *field);
/* Rewrite a buffer handle in a guest pointer to the GMR id. */
int vmw_val_reloc_gmr(struct vmw_val *val, uint32_t *gmr_field);
/* The same three, also handing back what the handle named: the surface
 * (NULL for SVGA3D_INVALID_ID) or the buffer object (NULL for the ids that
 * name no buffer). */
int vmw_val_reloc_sid_srf(struct vmw_val *val, uint32_t *field,
			  struct vmw_surface **srf_out);
int vmw_val_reloc_mob_obj(struct vmw_val *val, uint32_t *field,
			  struct drm_gem_object **obj_out);
int vmw_val_reloc_gmr_obj(struct vmw_val *val, uint32_t *gmr_field,
			  struct drm_gem_object **obj_out);
/* A legacy command's context id must be one of the file's. */
int vmw_val_check_cid(struct vmw_val *val, uint32_t cid);
/* Make sure the stream's DX context can hold entry `id' of COTable `type'. */
int vmw_val_cotable(struct vmw_val *val, int type, uint32_t id);

/* ---- per-context bookkeeping (vmw_execbuf.c) ---- */

/* The node of `ctx' in this submission, created on first use (and the
 * context referenced); NULL if it cannot be had. */
struct vmw_val_ctx *vmw_val_ctx_node(struct vmw_val *val,
				     struct vmw_context *ctx);
/* Turn the command whose header id is at `id_loc' into a NOP at
 * submission if `res' is destroyed by then.  0 or -ENOMEM. */
int vmw_val_cond_nop_add(struct vmw_val *val, uint32_t *id_loc,
			 struct vmw_resource *res);

/* ---- the command table (vmw_execbuf_cmds.c) ---- */

/* What the verifier knows about one command id.
 *
 * @func:       checks and relocates the command; NULL = refused.
 * @user_allow: a client may send it.
 * @gb_disable: refused on a device with guest-backed objects.
 * @gb_enable:  refused on a device without them.
 * @cmd_name:   for messages.
 *
 * A command with no handler is refused outright, as it always was.  The
 * three flags are rules of the strict verifier: a command the client is
 * not supposed to send, or one meant for the other device model, is named
 * on the console and -- unless VMW_EXECBUF_STRICT -- still verified and
 * passed by its handler. */
struct vmw_cmd_entry {
	int (*func)(struct vmw_val *val, SVGA3dCmdHeader *header);
	bool user_allow;
	bool gb_disable;
	bool gb_enable;
	const char *cmd_name;
};

/* Verify and relocate the command at `buf' (`avail' bytes left in the
 * stream).  On success *size is the command's length, header included.
 * 0 or -errno. */
int vmw_execbuf_cmd_check(struct vmw_val *val, void *buf, uint32_t avail,
			  uint32_t *size);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_EXECBUF_H */
