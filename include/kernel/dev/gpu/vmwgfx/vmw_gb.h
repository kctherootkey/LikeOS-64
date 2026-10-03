// LikeOS -- vmwgfx: guest-backed objects (SVGA3D with GB/DX).
//
// Internal to the backend.  The device keeps its object tables (OTables)
// in guest memory the driver hands it; every surface, context, shader and
// screen target is an entry there, and its storage is a MOB -- a
// page-table-described guest memory region.  Commands reach the device
// through command buffers (which can name a DX context) or, on hosts
// without them, the FIFO.
//
// This is the driver's one internal header: it defines the device, the
// buffer, surface and context objects, and pulls in the per-subsystem
// headers (command submission, fences, display, cursor, resources and
// bindings, views, command-buffer resources, blits, id accounting,
// capabilities), so every file of the driver includes just this.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_GB_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_GB_H

#include <kernel/mm/rwsem.h>
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_reg.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_surfacedefs.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>

#include <kernel/dev/gpu/vmwgfx/vmw_compat.h>
#include <kernel/dev/gpu/vmwgfx/vmw_resource.h>
#include <kernel/dev/gpu/vmwgfx/vmw_cmdbuf.h>
#include <kernel/dev/gpu/vmwgfx/vmw_binding.h>
#include <kernel/dev/gpu/vmwgfx/vmw_so.h>
#include <kernel/dev/gpu/vmwgfx/vmw_cmdbuf_res.h>
#include <kernel/dev/gpu/vmwgfx/vmw_fence.h>
#include <kernel/dev/gpu/vmwgfx/vmw_kms.h>
#include <kernel/dev/gpu/vmwgfx/vmw_cursor.h>
#include <kernel/dev/gpu/vmwgfx/vmw_blit.h>
#include <kernel/dev/gpu/vmwgfx/vmw_gmrid.h>
#include <kernel/dev/gpu/vmwgfx/vmw_surface_cache.h>

/* Object counts (OTable sizes).
 *
 * Contexts and DX contexts share one id space (vmw_device.context_ids), so
 * a DX context's id is below VMW_NUM_CONTEXTS and the two tables must be
 * the same size.  Each entry of either table is 8 bytes: 256 of them are
 * one page of OTable, and 256 per-file context pointers are 2 KB of the
 * file's vmw_file. */
#define VMW_NUM_MOBS (64 * 1024)
#define VMW_NUM_SURFACES (32 * 1024)
#define VMW_NUM_CONTEXTS 256
#define VMW_NUM_SHADERS (16 * 1024)
#define VMW_NUM_SCREENTARGETS 64
#define VMW_NUM_DXCONTEXTS 256

/* A MOB: the device's view of a page array. */
struct vmw_mob {
	uint32_t id; /* SVGAMobId, or SVGA3D_INVALID_ID */
	SVGAMobFormat fmt;
	uint64_t base_ppn; /* what DEFINE_GB_MOB64 names */
	uint32_t size; /* bytes covered */
	uint64_t *pt_pages; /* page-table pages (phys), when depth >= 1 */
	uint32_t npt;
	int defined; /* DEFINE sent */
};

struct vmw_device;
/* private to vmw_mob.c. */
struct vmw_otable_batch;

/* ---- command buffers / FIFO: vmw_cmdbuf.h ---- */

/* ---- MOBs and OTables ---- */
/* an id with no pages accounted (-1 when none is free); a buffer
 * object's MOB uses vmw_gmrid_man_get_node/put_node(VMW_PL_MOB, npages)
 * instead.  vmw_mob_bind() takes the id from mob->id and, on failure,
 * leaves nothing behind but the id. */
int vmw_mob_alloc_id(struct vmw_device *v);
void vmw_mob_free_id(struct vmw_device *v, uint32_t id);
/* Build the page tables for `pages' and send DEFINE_GB_MOB64. */
int vmw_mob_bind(struct vmw_device *v, struct vmw_mob *mob, const uint64_t *pages,
		 uint32_t npages, uint32_t size_bytes);
/* DESTROY_GB_MOB and release the page tables. */
void vmw_mob_unbind(struct vmw_device *v, struct vmw_mob *mob);
/* After the device lost its MOBs (power management): send DEFINE_GB_MOB64
 * again from the recorded id, depth, root and size (0 for a MOB never
 * defined; on a submission error the MOB is marked undefined), or mark one
 * undefined without sending anything (its object is going away). */
int vmw_mob_redefine(struct vmw_device *v, struct vmw_mob *mob);
void vmw_mob_forget(struct vmw_mob *mob);
/* (vmw_defer_free_pages(): vmw_cmdbuf.h) */
int vmw_otables_setup(struct vmw_device *v);
void vmw_otables_takedown(struct vmw_device *v);

/* ---- surfaces ---- */
struct vmw_surface {
	/* The device-object base: reference count, bindings that point at
	 * this surface.  `res.id' is `sid'.  Set up by vmw_surface_res_init();
	 * the surface is freed by the last vmw_resource_unreference(). */
	struct vmw_resource res;
	uint32_t sid;
	uint64_t flags; /* SVGA3dSurfaceAllFlags */
	uint32_t format;
	uint32_t mip_levels;
	uint32_t multisample_count;
	uint32_t multisample_pattern;
	uint32_t quality_level;
	uint32_t autogen_filter;
	SVGA3dSize base_size;
	uint32_t array_size;
	uint32_t byte_stride;
	uint32_t backup_size;
	/* Where this surface starts inside `backup'.
	 *
	 * A surface is created against whatever buffer handle the client
	 * names, and the client's allocator puts several of them in one
	 * buffer -- but the dirty tracking belongs to the BUFFER.  So the
	 * window has to be carried here, or consuming this surface's dirty
	 * pages takes its neighbours' as well.  Zero for a surface that was
	 * given a buffer of its own, which is why the arithmetic below is
	 * written out rather than left implicit: it is right by construction
	 * instead of by coincidence. */
	uint64_t backup_offset;
	struct drm_gem_object *backup; /* the MOB-backed buffer */
	int defined; /* DEFINE_GB_SURFACE sent */
	int bound; /* BIND_GB_SURFACE sent */
	int scanout;
	int shareable;
	int coherent; /* CPU writes reach the device without explicit updates */
	/* Dirty boxes of a coherent surface, one per subresource; set up
	 * with the tracking on the backup when the coherent flag is taken
	 * (see vmw_dirty.c). */
	struct vmw_surface_dirty *dirty;
	/* A surface defined the old way (SURFACE_DEFINE, no MOB behind it).
	 * It is destroyed with a different command, which is the only place
	 * the rest of the driver has to care. */
	int legacy;
	/* The DX views of this surface (vmw_so.c), destroyed before the
	 * surface is. */
	struct list_head view_list;
	/* The snooped image of a legacy cursor surface (vmw_cursor.c). */
	struct vmw_cursor_snooper snooper;
};

static inline struct vmw_surface *vmw_res_to_srf(struct vmw_resource *res)
{
	return container_of(res, struct vmw_surface, res);
}

static inline struct vmw_surface *vmw_surface_reference(struct vmw_surface *srf)
{
	(void)vmw_resource_reference(&srf->res);
	return srf;
}

static inline void vmw_surface_unreference(struct vmw_surface **srf)
{
	struct vmw_surface *tmp_srf = *srf;
	struct vmw_resource *res = &tmp_srf->res;

	*srf = NULL;
	vmw_resource_unreference(&res);
}

/* Set up the base of a newly allocated surface whose sid and `legacy' flag
 * are filled in: one reference (the surface object's), an empty view list.
 * From here on the surface is released through vmw_surface_destroy(). */
void vmw_surface_res_init(struct vmw_device *v, struct vmw_surface *s);

/* Serialized size of a surface with these parameters. */
/* Bytes of backing a surface needs.  `flags' carries SVGA3D_SURFACE_CUBEMAP,
 * which decides the layer count when there is no array size. */
uint32_t vmw_surface_size(uint32_t format, const SVGA3dSize *size,
			  uint32_t mip_levels, uint32_t array_size,
			  uint32_t samples, uint64_t flags);
int vmw_surface_alloc_id(struct vmw_device *v);
void vmw_surface_free_id(struct vmw_device *v, uint32_t sid);
int vmw_surface_define(struct vmw_device *v, struct vmw_surface *s);
int vmw_surface_bind(struct vmw_device *v, struct vmw_surface *s);
void vmw_surface_destroy(struct vmw_device *v, struct vmw_surface *s);
/* Coherent-surface dirty regions (vmw_dirty.c). */
int vmw_surface_dirty_alloc(struct vmw_surface *s);
void vmw_surface_dirty_free(struct vmw_surface *s);
void vmw_surface_dirty_range_add(struct vmw_surface *s, size_t start,
				 size_t end);
void vmw_surface_dirty_pull(struct vmw_surface *s);
uint32_t vmw_surface_dirty_count(const struct vmw_surface *s);
/* Put a surface's dirt back in full, for when the commands emit() produced
 * never reached the device: emit clears each box as it writes it, so a batch
 * that is dropped afterwards takes the record of those writes with it. */
void vmw_surface_dirty_mark_all(struct vmw_surface *s);



/* Pixels the host is told to re-read (UPDATE_GB_SCREENTARGET) and pixels the
 * guest copied itself, and whether the present covered the whole screen.
 * Together with the frame count these say whether the display path is
 * paying per damaged pixel or per screen. */
/* One submission that found no free command-buffer slot and had to wait. */
uint32_t vmw_surface_dirty_emit(struct vmw_device *v, struct vmw_surface *s,
				void *out, uint32_t cap);
/* The largest update command one dirty subresource can become. */
#define VMW_SURF_DIRTY_CMD_MAX                                  \
	(sizeof(SVGA3dCmdHeader) +                              \
	 (sizeof(SVGA3dCmdDXUpdateSubResource) >                \
			  sizeof(SVGA3dCmdUpdateGBImage) ?      \
		  sizeof(SVGA3dCmdDXUpdateSubResource) :        \
		  sizeof(SVGA3dCmdUpdateGBImage)))
/* The drm object for a surface (its gem kind is SURFACE, priv = this). */
struct drm_gem_object *vmw_surface_object_create(struct vmw_device *v,
						 struct vmw_surface *s);

/* ---- contexts / shaders ---- */
struct vmw_cotable {
	struct drm_gem_object *bo; /* MOB-backed */
	uint32_t size; /* bytes */
	/* The objects with an entry in this table (views, DX shaders, stream
	 * output), so they can be destroyed with the table. */
	struct list_head resource_list;
};

struct vmw_context {
	/* The device-object base.  `res.id' is `cid'. */
	struct vmw_resource res;
	uint32_t cid;
	int dx;
	struct drm_gem_object *state_bo; /* BIND_GB_CONTEXT / DX_BIND_CONTEXT */
	struct vmw_cotable cot[SVGA_COTABLE_MAX];
	struct drm_gem_object *shader_bo; /* DX_BIND_ALL_SHADER (unused) */
	int defined;
	struct drm_file *owner;
	/* What this context has bound (vmw_binding.c); NULL until set up. */
	struct vmw_ctx_binding_state *cbs;
	/* The views and shaders its command streams defined
	 * (vmw_cmdbuf_res.c); NULL until set up. */
	struct vmw_cmdbuf_res_manager *man;
	/* The client's DX query MOB, as named by DX_BIND_QUERY; one
	 * reference, or NULL. */
	struct drm_gem_object *dx_query_mob;
};

static inline struct vmw_context *vmw_res_to_ctx(struct vmw_resource *res)
{
	return container_of(res, struct vmw_context, res);
}

int vmw_context_create(struct vmw_device *v, struct drm_file *fp, int dx,
		       struct vmw_context **out);
void vmw_context_destroy(struct vmw_device *v, struct vmw_context *c);
/* Make sure COTable `type' can hold entry `id'. */
int vmw_context_cotable_reserve(struct vmw_device *v, struct vmw_context *c,
				int type, uint32_t id);
/* The context with device id `cid' among this file's, or NULL. */
struct vmw_context *vmw_file_context(struct drm_file *fp, uint32_t cid);
/* Accessors on a context's base. */
struct list_head *vmw_context_binding_list(struct vmw_resource *ctx);
struct vmw_cmdbuf_res_manager *vmw_context_res_man(struct vmw_resource *ctx);
struct vmw_ctx_binding_state *vmw_context_binding_state(struct vmw_resource *ctx);
/* The object list of COTable `cotable_type' of a DX context. */
struct list_head *vmw_context_cotable_list(struct vmw_resource *ctx,
					   SVGACOTableType cotable_type);
/* Remember `mob' (NULL to forget) as the context's DX query MOB.  0 or
 * -errno. */
int vmw_context_bind_dx_query(struct vmw_resource *ctx_res,
			      struct drm_gem_object *mob);
struct drm_gem_object *vmw_context_get_dx_query_mob(struct vmw_resource *ctx_res);

/* Object handle namespaces of the file: contexts live in fp->priv. */
struct vmw_file {
	struct vmw_context *contexts[VMW_NUM_CONTEXTS];
	uint32_t shaders[256]; /* legacy GB shader ids owned */
	/* The client asked for DRM_VMW_PARAM_MAX_MOB_MEMORY, which is how a
	 * client that knows about guest-backed objects identifies itself;
	 * DRM_VMW_GET_3D_CAP then answers in the device's own format. */
	int gb_aware;
};

/* ---- legacy file hooks (vmw_context.c, vmw_surface.c) ---- */
long vmw_ioctl_gb_surface_create(struct vmw_device *v, struct drm_file *fp,
				 void *kb, int ext);
long vmw_ioctl_gb_surface_ref(struct vmw_device *v, struct drm_file *fp,
			      void *kb, int ext);
long vmw_ioctl_unref_surface(struct vmw_device *v, struct drm_file *fp, void *kb);
void vmw_surface_gem_free(struct vmw_device *v, struct drm_gem_object *o);
long vmw_ioctl_create_context(struct vmw_device *v, struct drm_file *fp,
			      int dx, int32_t *cid_out);
long vmw_ioctl_unref_context(struct vmw_device *v, struct drm_file *fp, uint32_t cid);
void vmw_file_release(struct vmw_device *v, struct drm_file *fp);
long vmw_ioctl_create_shader(struct vmw_device *v, struct drm_file *fp,
			     struct drm_vmw_shader_create_arg *a);
long vmw_ioctl_unref_shader(struct vmw_device *v, struct drm_file *fp, uint32_t shid);

/* ---- execbuf ---- */
int vmw_execbuf(struct vmw_device *v, struct drm_file *fp,
		struct drm_vmw_execbuf_arg *a);

/* per-BO MOB (in drm_gem_object.priv for kind BO) */
struct vmw_bo {
	int gmr_id; /* -1 when not bound */
	struct vmw_mob mob; /* id SVGA3D_INVALID_ID when not a MOB */
};

/* the device */
/* (struct vmw_cb_slot, one command buffer in flight: vmw_cmdbuf.h) */

/* Per-unit screen-target state beyond the first unit; private to
 * vmw_stdu.c. */
struct vmw_stdu_state;

struct vmw_device {
	struct drm_device drm;
	struct vmsvga2_hw_geometry hw;
	int has_3d, has_screen_object, has_gmr, has_gb, has_dx, has_cmdbuf;
	int has_sm41, has_sm5, has_gl43, has_screentarget;
	int has_msg;			/* the host message channel answers */
	uint32_t cap2;
	uint64_t max_mob_memory, max_mob_size;
	/* What the scan-out can be, as opposed to what the framebuffer
	 * aperture would allow; see vmw_scanout_limits(). */
	uint64_t scanout_max_mem;
	uint32_t scanout_max_width, scanout_max_height;
	int scanout_in_guest_memory; /* screen object or screen target */
	uint32_t devcaps[SVGA3D_DEVCAP_MAX + 1];
	int screen_defined;
	uint32_t screen_w, screen_h;
	uint32_t scan_gmr;
	hrtimer_t fence_poll;
	int fence_poll_running;
	uint32_t next_seq;
	/* GB */
	struct {
		struct drm_gem_object *bo; /* MOB-backed table storage */
		uint32_t size;
	} otable[SVGA_OTABLE_DX_MAX];
	int otables_ready;
	int cb_ready;    /* the command-buffer channel is up and started */
	/* Buffers the device refused or that were given up on: every path
	 * out of cb_slot_handle_error() that ends with work thrown away.
	 * vmw_mode_set() notes the count and vmw_display_verify() compares:
	 * "did the console's screen come up" is answered by "nothing was
	 * refused between the mode set and now". */
	uint32_t cb_errors, cb_errors_seen;
	/* The screen-target path was tried for the console and the device
	 * would not carry it; the display runs on the framebuffer aperture
	 * from here on.  See vmw_display_fallback(). */
	int st_refused;
	uint8_t *mob_ids; /* bitmap */
	uint8_t *surface_ids;
	uint8_t *context_ids;
	uint8_t *shader_ids;
	spinlock_t id_lock;
	/* command buffer */
	struct vmw_cb_slot cb_slot[16];
	int cb_nslots;
	/* Submitters that found every slot in flight sleep here; the reaper
	 * wakes them as slots come free.  See cb_slot_claim(). */
	struct wait_queue_head cb_wq;
	/* One submission at a time.
	 *
	 * A submission is not just a copy of the client's commands: it
	 * REWRITES them (handles become device ids), it can GROW a context's
	 * object tables underneath the device, and it harvests and emits the
	 * coherent-surface updates.  None of that is atomic, and two threads
	 * of one process submit against the same context constantly.
	 *
	 * The table growth is what makes it unsafe rather than merely
	 * racy: vmw_context_cotable_reserve() drains the queue, asks the
	 * device to write the current table back, copies it into a bigger
	 * buffer and points the context at the copy.  A DEFINE submitted by
	 * another thread between the readback and the switch executes against
	 * the OLD table and is then thrown away with it -- so its view id
	 * survives with a stale entry, and every draw that uses it samples
	 * whatever that entry happens to name.  The device reports nothing:
	 * the id is valid, only its contents are wrong.
	 *
	 * Held across validation and hand-over only -- execution is
	 * asynchronous, so this does not serialise the device. */
	mm_rwsem_t execbuf_lock;
	/* Commands accumulate here and go to the device as one buffer. */
	uint8_t *pend;
	uint32_t pend_len;
	uint32_t pend_dx; /* the DX context the pending commands belong to */
	uint32_t pend_fence_seq; /* fence sequence gathered with them, or 0 */
	volatile unsigned char pend_busy;
	/* Page-table pages the device was told to stop using, freed once it
	 * has worked through everything it was given.
	 *
	 * `defer_lock' covers the array and the count, and nothing else.  Every
	 * processor that destroys a buffer object reaches here -- a browser
	 * with one process per core destroys them constantly -- and unlocked
	 * this had both failure modes at once: two drains reading the same
	 * count freed the same pages TWICE, and two appends writing the same
	 * slot lost one page for ever.  A page freed twice comes back allocated
	 * to two owners, which is how it turned into corrupted kernel memory
	 * far away from here. */
	spinlock_t defer_lock;
	/* Grown on demand, not a fixed 64.
	 *
	 * A full-screen image is 1920x1200x4 -- 2250 pages -- and they are
	 * released together.  With room for 64 the rest had to go back
	 * IMMEDIATELY, while the device could still be reading them, and the
	 * page-table pages freed after them each found the area full and made
	 * the device idle one page at a time.  So the same fixed size caused
	 * both a use-after-free and a stall, and both scaled with the image. */
	uint64_t *defer_free;
	uint32_t defer_n;
	uint32_t defer_cap;
	uint64_t cb_last_progress_us; /* last time ANY buffer finished */
	uint32_t cb_size; /* payload bytes per slot */
	/* The command-buffer channel is claimed with this flag, not with a
	 * spinlock: there is one header and one buffer, so one submitter at a
	 * time, and the claim is held across the WAIT for the device, which
	 * runs to two seconds.  A spinlock cannot span that -- see the
	 * acquire in vmw_mob.c for what went wrong when it did. */
	volatile unsigned char cb_busy;
	/* Tickets handed out by cb_slot_ring(), and the claim that lets one
	 * thread at a time repair the channel after the device has rejected
	 * a buffer.  Error recovery preempts the context, rewrites the
	 * failing buffer and re-rings every buffer that came back -- none of
	 * which survives two threads doing it at once. */
	uint64_t cb_next_seq;
	volatile unsigned char cb_recover_busy;
	/* screen target scan-out: a mirror of screen-target unit 0, written
	 * only by vmw_stdu.c */
	int st_defined;
	uint32_t st_w, st_h;
	uint32_t st_bound_sid;		/* what the target currently shows */
	/* WHICH surface that id belonged to.
	 *
	 * The id alone is not identity: ids are reused, so a destroyed
	 * surface's number can come back attached to a different one, and
	 * a target left bound to the old meaning shows the wrong thing.
	 * Rebinding on every full present avoided that by never trusting the
	 * id -- at the cost of a BIND_GB_SCREENTARGET per page flip, which
	 * the display server issues once a frame, and which makes the host
	 * re-establish the scan-out every time.  Keeping the object as well
	 * settles identity properly and lets the bind be skipped when
	 * nothing has actually changed. */
	const void *st_bound_obj;
	struct vmw_surface *st_surface;	/* the driver's display surface */
	struct drm_gem_object *st_bo;	/* its MOB backing */
	void *overlay_priv;		/* the video overlay streams (vmw_overlay.c) */
	/* All screen-target units, unit 0 included (vmw_stdu.c); NULL until
	 * the first target is set up. */
	struct vmw_stdu_state *stdu;
	/* The context binding records: every vmw_resource.binding_head and
	 * the trackers that link into them (vmw_binding.c).  A sleeping lock,
	 * taken inside execbuf_lock when both are held, and never held across
	 * drm_gem_put() -- dropping the last reference to a surface destroys
	 * it, and that scrubs bindings. */
	mm_rwsem_t binding_lock;
	/* The open VMW_CMD_RESERVE reservation (vmw_cmd_reserve.c). */
	struct vmw_cmd_reserve_state cmd_reserve;
	/* Fence event and action lists (vmw_fence.c); NULL until set up. */
	struct vmw_fence_manager *fman;
	/* MOB cursors and their recycling (vmw_cursor.c); NULL until set up. */
	struct vmw_cursor_state *cursor;
	/* Id and page accounting per id space (vmw_gmrid.c); NULL where not
	 * set up, which means unaccounted. */
	struct vmw_gmrid_man *gmrid_man[VMW_PL_MAX];
	/* What the device can scan out of guest memory:
	 * SVGA_REG_MAX_PRIMARY_MEM on a guest-backed device (0 where it does
	 * not say), the VRAM size on an older device. */
	uint64_t max_primary_mem;
	/* the object tables -- one buffer holding all of them and each
	 * table's page tables (vmw_mob.c); NULL until vmw_otables_setup()
	 * and after vmw_otables_takedown(). */
	struct vmw_otable_batch *otable_batch;
	/* the binding state a submission stages its context's new
	 * bindings in (vmw_execbuf.c).  Tens of kilobytes, so it is allocated
	 * once, on first use, and reset after every submission rather than
	 * allocated per call; owned by whoever holds execbuf_lock. */
	struct vmw_ctx_binding_state *execbuf_staged;
	/* the surface memory the device grants a client that does not
	 * use guest-backed objects (DRM_VMW_PARAM_MAX_SURF_MEMORY):
	 * SVGA_REG_MEMORY_SIZE less VRAM with second-generation regions,
	 * otherwise a fixed 512 MB. */
	uint64_t memory_size;
	/* The display units, one per head (vmw_kms.c; struct
	 * vmw_display_unit in vmw_kms.h).  Unit 0's screen object is also
	 * published in screen_defined / screen_w / screen_h / scan_gmr. */
	struct vmw_display_unit du[VMW_MAX_HEADS];
};

/* Which shader model the device runs, from the has_* flags.  Each level
 * implies the ones below it. */
enum vmw_sm_type {
	VMW_SM_LEGACY = 0,
	VMW_SM_4,
	VMW_SM_4_1,
	VMW_SM_5,
	VMW_SM_5_1X,
	VMW_SM_MAX
};

static inline enum vmw_sm_type vmw_sm_type(const struct vmw_device *v)
{
	if (v->has_gl43)
		return VMW_SM_5_1X;
	if (v->has_sm5)
		return VMW_SM_5;
	if (v->has_sm41)
		return VMW_SM_4_1;
	if (v->has_dx)
		return VMW_SM_4;
	return VMW_SM_LEGACY;
}

static inline bool has_sm4_context(const struct vmw_device *v)
{
	return vmw_sm_type(v) >= VMW_SM_4;
}

static inline bool has_sm4_1_context(const struct vmw_device *v)
{
	return vmw_sm_type(v) >= VMW_SM_4_1;
}

static inline bool has_sm5_context(const struct vmw_device *v)
{
	return vmw_sm_type(v) >= VMW_SM_5;
}

static inline bool has_gl43_context(const struct vmw_device *v)
{
	return vmw_sm_type(v) >= VMW_SM_5_1X;
}

static inline u32 vmw_max_num_uavs(const struct vmw_device *v)
{
	return has_gl43_context(v) ? SVGA3D_DX11_1_MAX_UAVIEWS :
				     SVGA3D_MAX_UAVIEWS;
}

static inline bool vmw_shadertype_is_valid(enum vmw_sm_type shader_model,
					   u32 shader_type)
{
	SVGA3dShaderType max_allowed = SVGA3D_SHADERTYPE_PREDX_MAX;

	if (shader_model >= VMW_SM_5)
		max_allowed = SVGA3D_SHADERTYPE_MAX;
	else if (shader_model >= VMW_SM_4)
		max_allowed = SVGA3D_SHADERTYPE_DX10_MAX;
	return shader_type >= SVGA3D_SHADERTYPE_MIN && shader_type < max_allowed;
}

/* Does the device signal fences at all?  (QEMU's does not.) */
static inline bool vmw_has_fences(const struct vmw_device *v)
{
	if ((v->hw.caps & (SVGA_CAP_COMMAND_BUFFERS |
			   SVGA_CAP_CMD_BUFFERS_2)) != 0)
		return true;
	return vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_FENCE) != 0;
}

/* ---- video overlay streams (vmw_overlay.c) ---- */
int vmw_overlay_init(struct vmw_device *v);
void vmw_overlay_close(struct vmw_device *v);
int vmw_overlay_num_streams(struct vmw_device *v);
int vmw_overlay_num_free_streams(struct vmw_device *v);
void vmw_overlay_file_release(struct vmw_device *v, struct drm_file *fp);
long vmw_ioctl_control_stream(struct vmw_device *v, struct drm_file *fp,
			      struct drm_vmw_control_stream_arg *arg);
long vmw_ioctl_claim_stream(struct vmw_device *v, struct drm_file *fp,
			    struct drm_vmw_stream_arg *arg);
long vmw_ioctl_unref_stream(struct vmw_device *v, struct drm_file *fp,
			    struct drm_vmw_stream_arg *arg);

/* ---- screen-target scan-out: vmw_kms.h ---- */

/* ---- legacy (non-guest-backed) surfaces ---- */
long vmw_ioctl_create_surface(struct vmw_device *v, struct drm_file *fp, void *kb);
long vmw_ioctl_ref_surface(struct vmw_device *v, struct drm_file *fp, void *kb);

/* ---- the host message channel (vmw_msg.c) ---- */
struct drm_vmw_msg_arg;
int vmw_msg_probe(void);
int vmw_host_log(const char *line);
long vmw_ioctl_msg(struct vmw_device *v, struct drm_vmw_msg_arg *a);

/* Host messaging and overlay streams. */
/* Probe the channel into v->has_msg and log the driver version to the host. */
void vmw_msg_init(struct vmw_device *v);
/* A line for the hypervisor's log (cut at a page); -ENODEV without a host. */
__attribute__((format(printf, 1, 2)))
int vmw_host_printf(const char *fmt, ...);
/* A guestinfo.* variable: *length in = buffer size, out = bytes stored. */
int vmw_host_get_guestinfo(const char *guest_info_param, char *buffer,
			   size_t *length);
/* Stop every running overlay stream around a scan-out change, and start the
 * paused ones again with their last register set afterwards. */
int vmw_overlay_pause_all(struct vmw_device *v);
int vmw_overlay_resume_all(struct vmw_device *v);

int vmw_gb_init(struct vmw_device *v);
/* undo the guest-backed half of vmw_gb_init() (object tables, MOB
 * accounting) once no buffer object's MOB is left. */
void vmw_gb_takedown(struct vmw_device *v);
/* (vmw_fence_emit(), vmw_fence_check(): vmw_fence.h) */

/* ---- the driver (vmw_drv.c) and its ioctls (vmw_ioctl.c) ---- */
/* drm_driver.gem_init: a buffer object's MOB and region. */
int vmw_gem_init(struct drm_gem_object *o);
long vmw_ioctl(struct drm_device *dev, struct drm_file *fp, unsigned nr,
	       unsigned dir, void *kb, unsigned size, int *handled);
int vmw_render_allowed(unsigned nr);

/* Sanitised fixed-format helpers. */
static inline int vmw_id_alloc(uint8_t *bm, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (!(bm[i / 8] & (1u << (i % 8)))) {
			bm[i / 8] |= (uint8_t)(1u << (i % 8));
			return (int)i;
		}
	return -1;
}

static inline void vmw_id_free(uint8_t *bm, uint32_t i)
{
	bm[i / 8] &= (uint8_t)~(1u << (i % 8));
}

/* Needs struct vmw_device. */
#include <kernel/dev/gpu/vmwgfx/vmw_devcaps.h>

#endif
