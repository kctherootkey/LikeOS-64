// LikeOS-64 -- vmwgfx: guest-backed objects (SVGA3D with GB/DX).
//
// Internal to the backend.  The device keeps its object tables (OTables)
// in guest memory the driver hands it; every surface, context, shader and
// screen target is an entry there, and its storage is a MOB -- a
// page-table-described guest memory region.  Commands reach the device
// through command buffers (which can name a DX context) or, on hosts
// without them, the FIFO.
#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_GB_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_GB_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_reg.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_surfacedefs.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>

/* Object counts (OTable sizes). */
#define VMW_NUM_MOBS (64 * 1024)
#define VMW_NUM_SURFACES (32 * 1024)
#define VMW_NUM_CONTEXTS 64
#define VMW_NUM_SHADERS (16 * 1024)
#define VMW_NUM_SCREENTARGETS 64
#define VMW_NUM_DXCONTEXTS 64

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

/* ---- command buffers / FIFO ---- */
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
void vmw_cmd_flush(struct vmw_device *v);
void vmw_cmd_drain(struct vmw_device *v);


/* ---- MOBs and OTables ---- */
int vmw_mob_alloc_id(struct vmw_device *v);
void vmw_mob_free_id(struct vmw_device *v, uint32_t id);
/* Build the page tables for `pages' and send DEFINE_GB_MOB64. */
int vmw_mob_bind(struct vmw_device *v, struct vmw_mob *mob, const uint64_t *pages,
		 uint32_t npages, uint32_t size_bytes);
/* DESTROY_GB_MOB and release the page tables. */
void vmw_mob_unbind(struct vmw_device *v, struct vmw_mob *mob);
/* Hand a buffer object's backing pages back, once the device is done walking
 * the MOB page table that names them.  See the definition in vmw_mob.c. */
void vmw_defer_free_pages(struct vmw_device *v, const uint64_t *pages,
			  uint32_t n);
int vmw_otables_setup(struct vmw_device *v);
void vmw_otables_takedown(struct vmw_device *v);

/* ---- surfaces ---- */
struct vmw_surface {
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
};

/* Serialized size of a surface with these parameters. */
uint32_t vmw_surface_size(uint32_t format, const SVGA3dSize *size,
			  uint32_t mip_levels, uint32_t array_size,
			  uint32_t samples);
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
};

struct vmw_context {
	uint32_t cid;
	int dx;
	struct drm_gem_object *state_bo; /* BIND_GB_CONTEXT / DX_BIND_CONTEXT */
	struct vmw_cotable cot[SVGA_COTABLE_MAX];
	struct drm_gem_object *shader_bo; /* DX_BIND_ALL_SHADER (unused) */
	int defined;
	struct drm_file *owner;
};

int vmw_context_create(struct vmw_device *v, struct drm_file *fp, int dx,
		       struct vmw_context **out);
void vmw_context_destroy(struct vmw_device *v, struct vmw_context *c);
/* Make sure COTable `type' can hold entry `id'. */
int vmw_context_cotable_reserve(struct vmw_device *v, struct vmw_context *c,
				int type, uint32_t id);

/* Object handle namespaces of the file: contexts live in fp->priv. */
struct vmw_file {
	struct vmw_context *contexts[64];
	uint32_t shaders[256]; /* legacy GB shader ids owned */
};

/* ---- execbuf ---- */
int vmw_execbuf(struct vmw_device *v, struct drm_file *fp,
		struct drm_vmw_execbuf_arg *a);

/* per-BO MOB (in drm_gem_object.priv for kind BO) */
struct vmw_bo {
	int gmr_id; /* -1 when not bound */
	struct vmw_mob mob; /* id SVGA3D_INVALID_ID when not a MOB */
};

/* the device */
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
	uint8_t *mob_ids; /* bitmap */
	uint8_t *surface_ids;
	uint8_t *context_ids;
	uint8_t *shader_ids;
	spinlock_t id_lock;
	/* command buffer */
	struct vmw_cb_slot cb_slot[16];
	int cb_nslots;
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
	uint64_t defer_free[64];
	uint32_t defer_n;
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
	/* screen target scan-out (vmw_stdu.c) */
	int st_defined;
	uint32_t st_w, st_h;
	uint32_t st_bound_sid;		/* what the target currently shows */
	struct vmw_surface *st_surface;	/* the driver's display surface */
	struct drm_gem_object *st_bo;	/* its MOB backing */
};

/* ---- screen-target scan-out ---- */
struct drm_framebuffer;
/* Is this the device's scan-out path?  (Guest-backed objects up, and the
 * device advertises screen targets.) */
int vmw_stdu_available(struct vmw_device *v);
int vmw_stdu_set_mode(struct vmw_device *v, uint32_t w, uint32_t h,
		      struct drm_framebuffer *fb);
/* Bring a screen target of this size up if there is not one already. */
int vmw_stdu_ensure(struct vmw_device *v, uint32_t w, uint32_t h);
/* Formats a screen target can scan out. */
int vmw_format_is_screen_target(uint32_t format);
int vmw_stdu_present(struct vmw_device *v, struct drm_framebuffer *fb, int x1,
		     int y1, int x2, int y2, int full);
void vmw_stdu_teardown(struct vmw_device *v);

/* ---- legacy (non-guest-backed) surfaces ---- */
long vmw_ioctl_create_surface(struct vmw_device *v, struct drm_file *fp, void *kb);
long vmw_ioctl_ref_surface(struct vmw_device *v, struct drm_file *fp, void *kb);

/* ---- the host message channel (vmw_msg.c) ---- */
struct drm_vmw_msg_arg;
int vmw_msg_probe(void);
int vmw_host_log(const char *line);
long vmw_ioctl_msg(struct vmw_device *v, struct drm_vmw_msg_arg *a);

int vmw_gb_init(struct vmw_device *v);
struct drm_fence *vmw_fence_emit(struct vmw_device *v, uint32_t flags);
void vmw_fence_check(struct vmw_device *v);

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

#endif
