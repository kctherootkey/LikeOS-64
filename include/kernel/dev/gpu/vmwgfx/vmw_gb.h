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

#include <kernel/mm/rwsem.h>
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
};

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

/* What the command-buffer channel cost since the last call: buffers handed
 * to the device, their payload, and how long the device took to finish one
 * (submission to the moment it was found completed).  Reset by the call. */
void vmw_cmdbuf_stats(uint64_t *submits, uint64_t *kbytes, uint64_t *avg_us,
		      uint64_t *max_us);
/* Texels the emitted boxes covered, and texels those surfaces hold, since
 * the last call.  Says whether the updates are tight or whole-surface. */
void vmw_surface_dirty_emit_stats(uint64_t *covered, uint64_t *total,
				  uint64_t *bytes);

/* Per-frame accounting for the scan-out path.
 *
 * The question these answer is why a window at 1920x1200 is unusable while
 * the same page at 1024x768 is not, when everything measurable on the host
 * scales with area and nothing in it is superlinear.  What a host harness
 * cannot see is how much WORK per frame the client actually asks for, so the
 * counts are taken where the work arrives and reported against presents.
 *
 * vmw_stdu_present() is the frame marker; everything else accumulates. */
void vmw_execbuf_note_frame(void);
/* One screen-target bind; the display server should need very few. */
void vmw_execbuf_note_bind(void);

/* ---- where the time goes ------------------------------------------------
 *
 * The counts above say how much work arrives per frame; they cannot say
 * whether the frame rate is set by this kernel, by the client, or by the
 * host -- and those three want completely different fixes.  A second's
 * worth of a frame is 1000ms, so a stage that accumulates 700ms in a second
 * IS the frame rate and everything else is noise.  That is the number these
 * produce.
 *
 * Timestamp-counter ticks rather than microseconds: a submission takes a
 * few microseconds, so a microsecond clock read around it rounds most
 * samples to zero or one and the sum says nothing.  The conversion happens
 * once, in the report.
 *
 * VMW_T_EXECBUF is the whole ioctl and CONTAINS scan/emit/submit/fence;
 * VMW_T_PRESENT is the whole present and contains blit.  They are reported
 * as they are rather than as remainders so a stage that is missing from the
 * breakdown shows up as a gap rather than as a negative number. */
enum vmw_time_stage {
	VMW_T_EXECBUF,	/* the whole execbuf ioctl */
	VMW_T_SCAN,	/* coherent-surface page-table scan + box pull */
	VMW_T_EMIT,	/* turning boxes into update commands */
	VMW_T_SUBMIT,	/* handing the batch to the device */
	VMW_T_FENCE,	/* emitting the fence that ends the batch */
	VMW_T_PRESENT,	/* the whole scan-out present */
	VMW_T_BLIT,	/* the guest-pixel copy inside a present */
	VMW_T_SLOT,	/* waiting for a free command-buffer slot */
	VMW_T_LOCK,	/* waiting for execbuf_lock */
	VMW_T_VALIDATE, /* copying and checking the client's stream */
	VMW_T_MAX
};
void vmw_execbuf_note_time(enum vmw_time_stage stage, uint64_t tsc_ticks);

/* Pixels the host is told to re-read (UPDATE_GB_SCREENTARGET) and pixels the
 * guest copied itself, and whether the present covered the whole screen.
 * Together with the frame count these say whether the display path is
 * paying per damaged pixel or per screen. */
void vmw_execbuf_note_update(uint64_t pixels, int full);
void vmw_execbuf_note_blit(uint64_t pixels);
/* One submission that found no free command-buffer slot and had to wait. */
void vmw_execbuf_note_slot_wait(void);
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
	 * The reference serialises the whole of execbuf for the same reason
	 * (its cmdbuf_mutex).  Held across validation and hand-over only --
	 * execution is asynchronous, so this does not serialise the device. */
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
	/* screen target scan-out (vmw_stdu.c) */
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
