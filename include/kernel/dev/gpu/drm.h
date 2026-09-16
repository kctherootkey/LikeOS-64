// LikeOS -- display-manager core (the DRM interface), kernel side.
//
// One device = one GPU with a primary node (/dev/dri/cardN: mode setting,
// master/authentication, everything) and a render node (/dev/dri/renderDN:
// rendering ioctls only, for any process).  Backends register a
// drm_driver; the core owns the descriptor semantics, handle namespaces,
// buffer sharing across processes (PRIME / dma-buf), fences (sync_file),
// mode objects and the event/vblank machinery, and calls the backend for
// what touches hardware.  The core never includes a backend header.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_DRM_H
#define KERNEL_DEV_GPU_DRM_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/drm/drm.h>
#include <kernel/uapi/drm/drm_mode.h>
#include <kernel/dev/device.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/pci.h>

struct drm_device;
struct drm_file;
struct drm_gem_object;
struct drm_fence;
struct task;

/* Values the mode-setting ioctls report that the UAPI headers leave to the
 * implementation. */
#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR 2
#define DRM_MODE_CONNECTED 1
#define DRM_MODE_DISCONNECTED 2
#define DRM_MODE_UNKNOWNCONNECTION 3
#define DRM_MODE_SUBPIXEL_UNKNOWN 1

/* ---- fences ------------------------------------------------------------ */

struct drm_fence {
	int refs;
	struct drm_device *dev;
	uint32_t seqno; /* backend sequence */
	uint32_t flags; /* DRM_VMW_FENCE_FLAG_* or backend bits */
	/* A second numbering for devices with several independent streams:
	 * `context' names the stream (an engine's timeline), `seqno64' the
	 * position in it.  Zero context = the single device-wide sequence
	 * above, which is what drm_fence_signal_upto() advances. */
	uint64_t context;
	uint64_t seqno64;
	/* Set when the work behind the fence failed (a reset threw it
	 * away); the fence is signalled all the same. */
	int error;
	volatile int signaled;
	uint64_t signal_ns;
	struct wait_queue_head wq;
	struct drm_fence *next; /* device's list of live fences */
	char name[16];
};

struct drm_fence *drm_fence_create(struct drm_device *dev, uint32_t seqno,
				   uint32_t flags);
void drm_fence_get(struct drm_fence *f);
void drm_fence_put(struct drm_fence *f);
void drm_fence_signal(struct drm_fence *f);
/* Signal every fence with seqno <= passed (wrap-safe). */
void drm_fence_signal_upto(struct drm_device *dev, uint32_t passed);
/* A fence on stream `context' at position `seqno64' (never zero); it is
 * signalled by drm_fence_signal_upto_ctx() once the stream has passed it,
 * or individually with drm_fence_signal(). */
struct drm_fence *drm_fence_create_ctx(struct drm_device *dev,
				       uint64_t context, uint64_t seqno64);
void drm_fence_signal_upto_ctx(struct drm_device *dev, uint64_t context,
			       uint64_t passed);
/* 0 signalled, -ETIMEDOUT, -EINTR. */
/* Wait for a fence, bounded by `timeout_ns'.
 *
 * `intr' says what a pending signal means here.  A caller that can REPORT the
 * interruption -- an ioctl that returns -EINTR to a program that will retry
 * it -- passes 1 and gets -EINTR the moment a signal is deliverable.
 *
 * A caller that cannot passes 0.  Teardown is the case: the pages are going
 * to be handed back whatever this returns, and the device is still reading
 * them until the fence passes.  Giving up early there does not interrupt
 * anything, it just frees memory out from under the device -- so those waits
 * are not interruptible, only bounded. */
int drm_fence_wait_flags(struct drm_fence *f, uint64_t timeout_ns, int intr);

static inline int drm_fence_wait(struct drm_fence *f, uint64_t timeout_ns)
{
	return drm_fence_wait_flags(f, timeout_ns, 1);
}
/* A sync_file descriptor for the fence (installs into the caller). */
int drm_fence_export_fd(struct drm_fence *f, int cloexec);
/* The fence behind a sync_file descriptor of the caller (a reference). */
struct drm_fence *drm_fence_from_fd(int fd);
/* A fence that is always signalled. */
struct drm_fence *drm_fence_signalled(struct drm_device *dev);
/* Per-file fence handles (what the backend's fence ioctls name). */
int drm_fence_handle_create(struct drm_file *fp, struct drm_fence *f,
			    uint32_t *handle_out);
struct drm_fence *drm_fence_handle_lookup(struct drm_file *fp, uint32_t handle);
int drm_fence_handle_delete(struct drm_file *fp, uint32_t handle);
void drm_fence_handles_release(struct drm_file *fp);

/* ---- objects (buffers, surfaces) ---------------------------------------- */

enum drm_gem_kind { DRM_GEM_BO = 1, DRM_GEM_SURFACE = 2 };

struct drm_gem_object {
	int refs;
	/* While the device may still be reading this object after its last
	 * reference has gone: the queue it waits on.  See drm_gem_reap(). */
	struct drm_gem_object *dead_next;
	struct drm_device *dev;
	enum drm_gem_kind kind;
	uint32_t id; /* device-global, for mmap offsets and flink names */
	uint64_t size; /* bytes */
	uint32_t npages;
	/* The pages are a client's own (an object over user memory): they
	 * were referenced, not allocated, and are released with a reference
	 * drop, never freed.  Recorded here rather than in the driver's
	 * per-object state, which is gone by the time the pages go. */
	int pages_borrowed;
	uint64_t *pages; /* physical page addresses (BOs) */
	/* Last submission that touched the object; waited for before CPU
	 * access and before destruction. */
	struct drm_fence *fence;
	/* Backend state (GMR id, MOB id, surface id, ...). */
	void *priv;
	uint32_t backend_id; /* what the device calls it (GMR/MOB/sid) */
	int scanout; /* created for display */
	uint32_t width, height, pitch, format; /* dumb/scanout buffers */
	struct drm_gem_object *next; /* device list */
	int flink_name; /* 0 = none */
	/* Dirty tracking, for an object whose pages a client writes through
	 * a coherent mapping (see drm_dirty.c).  NULL until a resource asks. */
	struct drm_gem_dirty *dirty;
	/* Region records currently describing mappings of this object --
	 * the initial mmap plus every split and forked copy.  The tracker
	 * compares it against the records it can actually walk to notice
	 * mappings living in other address spaces. */
	int map_records;
};

struct drm_gem_object *drm_gem_alloc(struct drm_device *dev,
				     enum drm_gem_kind kind, uint64_t size);
void drm_gem_get(struct drm_gem_object *o);
void drm_gem_put(struct drm_gem_object *o);
/* Back a BO with pages (zeroed). */
int drm_gem_alloc_pages(struct drm_gem_object *o);
/* ...with ONE physically contiguous run, for a buffer the CPU addresses
 * linearly (the console's framebuffer). */
int drm_gem_alloc_pages_contig(struct drm_gem_object *o);
/* The mmap offset userspace uses for this object on the device node. */
uint64_t drm_gem_mmap_offset(struct drm_gem_object *o);
/* The same, for a mapping of a particular kind (a driver's notion:
 * write-back, write-combining, uncached, through an aperture).  Kind 0 is
 * the plain offset above.  See drm_gem.c for the encoding. */
#define DRM_GEM_MMAP_KIND_MAX 15
uint64_t drm_gem_mmap_offset_kind(struct drm_gem_object *o, unsigned kind);
static inline unsigned drm_gem_mmap_kind_of(uint64_t offset)
{
	return (unsigned)(offset >> 56) & DRM_GEM_MMAP_KIND_MAX;
}
uint32_t drm_gem_handle_of_slot(struct drm_file *fp, uint32_t slot);
struct drm_gem_object *drm_gem_lookup_foreign(struct drm_device *dev,
					      uint32_t handle);
/* ---- dirty tracking (drm_dirty.c) -------------------------------------- */
/* One more dirty-tracking user of the object (refcounted).  Zero on
 * success; on failure the object simply has no tracker. */
int drm_gem_dirty_add(struct drm_gem_object *o);
void drm_gem_dirty_release(struct drm_gem_object *o);
/* The mapping census the sweeps check themselves against: one record that can
 * WRITE the object came (add = 1) or went (add = 0).  Wired into
 * mm_dirty_ops.map_census; the address space calls it, not the driver. */
void drm_gem_dirty_map_census(void *obj, int add);
/* Harvest the processor's record of client writes into the tracker.
 * Called once per object per submission, before the ranges are consumed. */
void drm_gem_dirty_scan(struct drm_gem_object *o);
/* Hand the accumulated dirty page ranges inside ONE WINDOW of the object to
 * the caller, clearing only those: cb(arg, first, last) per run of dirty
 * pages, in pages of the object.
 *
 * A window because one buffer object can back several resources, while the
 * tracking belongs to the buffer.  Whoever consumes must take only its own
 * pages and leave its neighbours' alone. */
void drm_gem_dirty_transfer(struct drm_gem_object *o, uint64_t first_page,
			    uint64_t last_page,
			    void (*cb)(void *arg, uint64_t first,
				       uint64_t last),
			    void *arg);
/* Record one written page directly (for mapping flavours the sweeps do
 * not walk; see drm_dirty.c): fault-time and unmap-time respectively. */
void drm_gem_dirty_fault_page(struct drm_gem_object *o, uint64_t page);
void drm_gem_dirty_mark_page(struct drm_gem_object *o, uint64_t page);
/* Census of region records mapping the object; the get/put wrappers of
 * every mapping flavour call these as records come and go. */
int drm_gem_dirty_wp_new_mapping(struct drm_gem_object *o);
/* The mapping callbacks a device-mmap of a gem object registers. */
struct mm_dirty_ops;
extern const struct mm_dirty_ops drm_gem_dirty_mmap_ops;
/* The same tracking for a mapping made through an exported descriptor
 * (drm_gem.c); records carrying these address the object from byte zero. */
extern const struct mm_dirty_ops drm_gem_dmabuf_dirty_ops;

/* Handles: per-file namespace. */
/* Finish off every object whose device work has since completed.
 *
 * Freeing an object the device is still reading has to wait for it, and
 * waiting is the one thing that must not happen on a thread that is trying to
 * draw: measured at a quarter of the wall clock in a maximized browser, ~30
 * objects a second at ~8ms each, all of it inside whatever thread happened to
 * drop the last reference.  So the wait is not done there any more -- the
 * object is queued and finished here, by whoever comes past next.
 *
 * Process context only: the teardown submits commands (a MOB has to be
 * destroyed before its pages go back) and takes the device lock. */
void drm_gem_reap(struct drm_device *dev);

/* Start the thread that does the above, so that no client's ioctl has to.
 * Called once, from drm_dev_register(). */
void drm_gem_reap_start(struct drm_device *dev);

/* Take a reference only if the object still has one; see the definition. */
int drm_gem_get_unless_zero(struct drm_gem_object *o);

int drm_gem_handle_create(struct drm_file *fp, struct drm_gem_object *o,
			  uint32_t *handle_out);
struct drm_gem_object *drm_gem_lookup(struct drm_file *fp, uint32_t handle);
int drm_gem_handle_delete(struct drm_file *fp, uint32_t handle);
/* Object by mmap offset (a reference). */
struct drm_gem_object *drm_gem_by_offset(struct drm_device *dev,
					 uint64_t offset);
/* PRIME: a dma-buf descriptor for the object; the object behind one. */
int drm_prime_export(struct drm_file *fp, struct drm_gem_object *o,
		     int flags);
struct drm_gem_object *drm_prime_import(int fd);
/* Virtual address of a page-backed object in the direct map (for kernel
 * copies), NULL when not page-backed. */
void *drm_gem_page_virt(struct drm_gem_object *o, uint32_t page);

/* ---- mode objects ------------------------------------------------------ */

/* Sized for a real display controller: several outputs, each with the
 * modes a monitor's EDID lists plus the standard table behind it, and a
 * compositor that keeps a framebuffer per surface it presents. */
#define DRM_MAX_CONNECTORS 8
#define DRM_MAX_CRTCS DRM_MAX_CONNECTORS
#define DRM_MAX_PLANES 32
#define DRM_MAX_MODES 128
#define DRM_MAX_PROPS 96
#define DRM_MAX_BLOBS 128
#define DRM_MAX_FBS 1024
/* Handles per file.  The slot half of a handle is 16 bits wide (see
 * drm_gem.c), so this is the most the encoding can name; it was 4096, and a
 * web process rendering a heavy page -- two handles per texture, one per
 * buffer -- ran into that with most of RAM free.  The table is kept in
 * chunks of one page of pointers so growing it never needs a large
 * contiguous allocation, which the kernel allocator cannot reclaim for. */
#define DRM_MAX_HANDLES 65536
#define DRM_HANDLE_CHUNK 512
#define DRM_HANDLE_CHUNKS (DRM_MAX_HANDLES / DRM_HANDLE_CHUNK)

struct drm_prop {
	uint32_t id;
	uint32_t flags; /* DRM_MODE_PROP_* */
	char name[32];
	uint64_t values[2]; /* range: min,max */
	struct drm_mode_property_enum enums[8];
	uint32_t nenums;
};

struct drm_blob {
	uint32_t id;
	uint32_t length;
	void *data;
	int in_use;
};

struct drm_framebuffer {
	uint32_t id; /* 0 = free slot */
	uint32_t width, height, pitch, format, bpp, depth;
	uint64_t modifier;
	struct drm_gem_object *obj;
	uint32_t offset;
	struct drm_file *owner;
};

struct drm_crtc {
	uint32_t id;
	int index;
	int active;
	struct drm_mode_modeinfo mode;
	uint32_t mode_blob; /* MODE_ID: the mode above as a blob, 0 when off */
	uint32_t fb_id;
	int x, y;
	uint16_t gamma[3][256];
	int gamma_identity; /* the table above is the identity */
	/* cursor */
	uint32_t cursor_handle_w, cursor_handle_h;
	int cursor_x, cursor_y;
	struct drm_gem_object *cursor_obj;
	/* The framebuffer the core wraps a legacy cursor object in, so that
	 * the cursor is a plane like any other (atomic drivers). */
	uint32_t cursor_fb_id;
	uint32_t primary_plane_id, cursor_plane_id;
};

/* A plane: a source rectangle of a framebuffer shown on a rectangle of a
 * CRTC.  Every CRTC has a primary and a cursor plane; a driver may add
 * overlays.  The fields below are the COMMITTED state; a change goes
 * through a drm_atomic_state first. */
struct drm_plane {
	uint32_t id;
	int index;
	uint32_t type; /* DRM_PLANE_TYPE_* */
	uint32_t possible_crtcs; /* bit = crtc index */
	int crtc; /* crtc index, -1 when off */
	uint32_t fb_id;
	uint32_t src_x, src_y, src_w, src_h; /* 16.16 */
	int32_t crtc_x, crtc_y;
	uint32_t crtc_w, crtc_h;
	int32_t hot_x, hot_y; /* cursor hot spot */
	/* what it scans out */
	const uint32_t *formats;
	uint32_t nformats;
	const uint64_t *modifiers;
	uint32_t nmodifiers;
	uint32_t in_formats_blob;
	void *priv; /* the driver's per-plane state */
};

struct drm_connector {
	uint32_t id;
	uint32_t encoder_id;
	uint32_t type; /* DRM_MODE_CONNECTOR_* */
	uint32_t type_id;
	int connected;
	uint32_t mm_width, mm_height;
	uint32_t crtc_id; /* current */
	int dpms;
	struct drm_mode_modeinfo modes[DRM_MAX_MODES];
	uint32_t nmodes;
	uint32_t edid_blob_id;
	/* From the EDID, when there is one: the sink's limits (used to
	 * filter the standard table) and what it is. */
	uint32_t range_max_clock_khz, range_min_vrefresh, range_max_vrefresh;
	uint32_t range_min_hfreq_khz, range_max_hfreq_khz;
	int is_hdmi;
	char sink_name[14];
	void *priv; /* the driver's per-connector state */
};

struct drm_encoder {
	uint32_t id;
	uint32_t type;
	uint32_t crtc_id;
	uint32_t possible_crtcs;
};

/* ---- files -------------------------------------------------------------- */

struct drm_pending_event {
	struct drm_pending_event *next;
	uint32_t length;
	uint8_t data[64]; /* drm_event_vblank / drm_event_crtc_sequence */
};

struct drm_file {
	struct drm_device *dev;
	int is_render;
	int is_master;
	int authenticated;
	uint32_t magic;
	uint32_t uid;
	/* handle -> object, chunk k holding slots [k * DRM_HANDLE_CHUNK, +CHUNK);
	 * index 0 unused (handle 0 is "none").  nhandles is the number of
	 * slots installed, a multiple of the chunk; handle_hint is the lowest
	 * slot that may be free (everything below it is taken), so a create
	 * does not rescan a full prefix under the lock every time. */
	struct drm_gem_object **handles[DRM_HANDLE_CHUNKS];
	uint32_t nhandles;
	uint32_t handle_hint;
	int handles_full_reported;
	uint32_t file_id; /* names this file inside every handle it hands out */
	/* fence handle -> fence, a namespace of its own */
	struct drm_fence **fences;
	uint32_t nfences;
	/* synchronisation objects (drm_syncobj.c), another namespace */
	struct drm_syncobj **syncobjs;
	uint32_t nsyncobjs;
	spinlock_t lock;
	/* event queue */
	struct drm_pending_event *events, *events_tail;
	struct wait_queue_head wq;
	int pending_vblank; /* WAIT_VBLANK events queued */
	uint64_t client_caps; /* bit n = DRM_CLIENT_CAP_n */
	void *priv; /* backend per-file state */
	struct drm_file *next; /* device list */
};

/* The slot's cell; the caller holds fp->lock and has checked
 * slot < fp->nhandles. */
static inline struct drm_gem_object **drm_handle_slot(struct drm_file *fp,
						      uint32_t slot)
{
	return &fp->handles[slot / DRM_HANDLE_CHUNK][slot % DRM_HANDLE_CHUNK];
}

/* ---- atomic state (drm_atomic.c) ---------------------------------------- */
/*
 * A mode-setting request is a STATE: the value every plane, CRTC and
 * connector would have once it is applied, built from the committed
 * state and the properties the caller changes, checked as a whole by the
 * core and the driver, and then applied by the driver in one go.  The
 * legacy calls (SETCRTC, PAGE_FLIP, SETPLANE, CURSOR, DPMS, SETGAMMA) and
 * the kernel console's own mode set are translated into such states for
 * a driver that has the atomic entry points; a driver without them keeps
 * the older per-call entry points below.
 */
struct drm_plane_state {
	struct drm_plane *plane;
	int crtc; /* crtc index, -1 off */
	uint32_t fb_id;
	struct drm_framebuffer *fb; /* resolved fb_id, NULL when 0 */
	uint32_t src_x, src_y, src_w, src_h; /* 16.16 */
	int32_t crtc_x, crtc_y;
	uint32_t crtc_w, crtc_h;
	int32_t hot_x, hot_y;
	int changed; /* named in this request */
	int fb_changed; /* a different framebuffer (or none) */
	int crtc_changed; /* moved between crtcs or switched on/off */
	int geometry_changed;
};

struct drm_crtc_state {
	struct drm_crtc *crtc;
	int active;
	int mode_valid;
	struct drm_mode_modeinfo mode; /* what the client asked for */
	/* What the transcoder actually runs: the driver's check fills it in
	 * when it differs (a panel with one native timing that the pipe
	 * scales the client's mode onto). */
	struct drm_mode_modeinfo adjusted_mode;
	int use_scaler;
	uint16_t gamma[3][256];
	int gamma_identity;
	int changed; /* named in this request, or a plane of it was */
	int mode_changed; /* a full mode set: the pipe comes down and up */
	int active_changed;
	int gamma_changed;
	int connectors_changed;
	uint32_t connector_mask; /* connectors on this crtc in this state */
	int event; /* a flip event was asked for */
};

struct drm_connector_state {
	struct drm_connector *conn;
	int crtc; /* crtc index, -1 */
	int dpms;
	int changed;
};

struct drm_atomic_state {
	struct drm_device *dev;
	struct drm_file *fp; /* NULL: the kernel console */
	uint32_t flags; /* DRM_MODE_ATOMIC_* */
	uint64_t user_data; /* for the flip events */
	int allow_modeset;
	struct drm_plane_state planes[DRM_MAX_PLANES];
	struct drm_crtc_state crtcs[DRM_MAX_CRTCS];
	struct drm_connector_state conns[DRM_MAX_CONNECTORS];
};

struct drm_atomic_state *drm_atomic_state_alloc(struct drm_device *dev,
						struct drm_file *fp,
						uint32_t flags);
void drm_atomic_state_free(struct drm_atomic_state *st);
/* Property writes into a state; -EINVAL for a property the object does
 * not have, -ENOENT for an object id that does not exist. */
int drm_atomic_plane_set(struct drm_atomic_state *st, struct drm_plane *p,
			 uint32_t prop, uint64_t val);
int drm_atomic_crtc_set(struct drm_atomic_state *st, struct drm_crtc *c,
			uint32_t prop, uint64_t val);
int drm_atomic_conn_set(struct drm_atomic_state *st, struct drm_connector *c,
			uint32_t prop, uint64_t val);
/* The core's consistency checks, then the driver's. */
int drm_atomic_check(struct drm_atomic_state *st);
/* Apply: the driver programs the hardware, then the objects take the
 * state's values and the flip events are queued.  Check first. */
int drm_atomic_commit(struct drm_atomic_state *st);
/* The primary and cursor plane of a crtc. */
struct drm_plane *drm_crtc_primary(struct drm_device *dev, int crtc);
struct drm_plane *drm_crtc_cursor(struct drm_device *dev, int crtc);
/* The plane's state inside `st'. */
static inline struct drm_plane_state *drm_atomic_plane_state(struct drm_atomic_state *st,
							     const struct drm_plane *p)
{
	return &st->planes[p->index];
}
/* Iteration helpers for drivers: the crtcs and planes the request names. */
static inline int drm_crtc_state_needs_modeset(const struct drm_crtc_state *cs)
{
	return cs->mode_changed || cs->active_changed || cs->connectors_changed;
}

/* ---- the driver ---------------------------------------------------------- */

struct drm_mode_rect_k {
	int32_t x1, y1, x2, y2;
};

struct drm_driver {
	const char *name; /* "vmwgfx" */
	const char *desc;
	const char *date;
	int major, minor, patch;
	uint32_t cursor_w, cursor_h; /* 0 = no hw cursor */

	/* How this driver's object handles are numbered.
	 *
	 * 0: a handle is the object's slot in the file's own table -- small,
	 * dense, reused lowest-first, meaningful only to that file.  That is
	 * what every graphics library assumes: Mesa's i915 backend sizes a
	 * per-submission array by the largest handle VALUE in the batch, so
	 * with the other numbering a file that had been opened 25th carried
	 * a 6.5 MB allocation and clear on every frame, growing with each
	 * open of the device until malloc failed.
	 *
	 * 1: the file's id rides in the handle's upper half, so a handle
	 * names one object device-wide and another process can resolve it
	 * (drm_gem_lookup_foreign).  Only vmwgfx wants this: its surface ids
	 * are passed between processes by value, X server to client, and
	 * its graphics library never indexes by them.  Everything else
	 * leaves it 0. */
	int global_handles;

	/* file lifetime */
	int (*open)(struct drm_device *dev, struct drm_file *fp);
	void (*postclose)(struct drm_device *dev, struct drm_file *fp);
	/* master acquired / dropped */
	void (*master_set)(struct drm_device *dev, struct drm_file *fp);
	void (*master_drop)(struct drm_device *dev, struct drm_file *fp);

	/* objects */
	int (*gem_init)(struct drm_gem_object *o); /* after pages exist */
	void (*gem_free)(struct drm_gem_object *o);
	/* Bring the driver's idea of which fences have passed up to date.
	 *
	 * A fence is marked signalled by whoever notices, and on a device with
	 * a fence interrupt that is the interrupt.  Where there is none --
	 * SVGA_CAP_IRQMASK is absent on some hosts -- nothing notices unless
	 * somebody asks, so a waiter that only tests the flag is waiting for
	 * an unrelated thread to ask on its behalf.  This is how it asks for
	 * itself. */
	void (*fence_poll)(struct drm_device *dev);
	/* Release the object's backing pages.
	 *
	 * Optional, and the reason it exists is that a driver may have given
	 * those pages to its device -- the frame numbers written into a page
	 * table the hardware walks -- in which case the core must not simply
	 * hand them back to the allocator when the last reference goes.  A
	 * driver that implements this takes over releasing them, and may hold
	 * them until its device is provably finished with them.  Without it
	 * the core frees them itself, which is right for a driver whose
	 * objects the hardware never sees.
	 *
	 * Called after gem_free(), so a driver has already had its chance to
	 * tell the device to let go; this is where it waits for that to have
	 * taken effect.  The pages array itself stays the core's to free. */
	void (*gem_release_pages)(struct drm_gem_object *o);
	/* the pages of an object for mmap, or -1 if not mappable */
	uint64_t (*gem_page_phys)(struct drm_gem_object *o, uint64_t index);
	uint64_t gem_mmap_pte_extra;
	/* Optional: the PTE cache bits for a mapping of this object of this
	 * kind (drm_gem_mmap_offset_kind), overriding the constant above.
	 * PAGE_WRITE_THROUGH alone is write-combining; with
	 * PAGE_CACHE_DISABLE, uncached; 0 write-back. */
	uint64_t (*gem_mmap_pte)(struct drm_gem_object *o, unsigned kind);
	/* Optional: called once the root filesystem is mounted, for the
	 * parts of bring-up that need a file (firmware) or a thread. */
	int (*late_init)(struct drm_device *dev);

	/* KMS */
	int (*mode_set)(struct drm_device *dev, struct drm_crtc *crtc,
			const struct drm_mode_modeinfo *mode,
			struct drm_framebuffer *fb, int x, int y);
	int (*crtc_disable)(struct drm_device *dev, struct drm_crtc *crtc);
	/* Optional: the connector's sink, asked on a probe (GETCONNECTOR
	 * from a client, a hotplug).  detect returns DRM_MODE_CONNECTED /
	 * DISCONNECTED / UNKNOWNCONNECTION; get_modes refills the mode list
	 * (through drm_connector_set_edid() or drm_connector_add_mode())
	 * and returns how many there are. */
	int (*detect)(struct drm_device *dev, struct drm_connector *c);
	int (*get_modes)(struct drm_device *dev, struct drm_connector *c);
	/* the framebuffer scanned out changed contents in these rects */
	int (*fb_dirty)(struct drm_device *dev, struct drm_crtc *crtc,
			struct drm_framebuffer *fb,
			const struct drm_mode_rect_k *rects, uint32_t n);
	/* flip to fb (immediately; the core sends the event) */
	int (*page_flip)(struct drm_device *dev, struct drm_crtc *crtc,
			 struct drm_framebuffer *fb);
	/* Scanout formats and layout modifiers.  NULL lists mean the core's
	 * defaults: XR24/AR24/RG16 and the linear layout.  They fill the
	 * IN_FORMATS property and bound what ADDFB2 accepts. */
	const uint32_t *fb_formats;
	uint32_t nfb_formats;
	const uint64_t *fb_modifiers;
	uint32_t nfb_modifiers;
	/* Optional: check a framebuffer against the object it lives in and
	 * settle its layout.  *modifier holds what the caller asked for
	 * (DRM_FORMAT_MOD_INVALID when it named none, as the old ADDFB does);
	 * the driver replaces it with the layout the object actually has and
	 * rejects pitches or sizes that layout cannot scan out.  Without it
	 * only the linear layout is accepted. */
	int (*fb_check)(struct drm_device *dev, struct drm_gem_object *o,
			const struct drm_mode_fb_cmd2 *r, uint64_t *modifier);
	int (*cursor_set)(struct drm_device *dev, struct drm_crtc *crtc,
			  struct drm_gem_object *o, uint32_t w, uint32_t h,
			  int32_t hot_x, int32_t hot_y);
	int (*cursor_move)(struct drm_device *dev, struct drm_crtc *crtc,
			   int x, int y);
	int (*dpms)(struct drm_device *dev, struct drm_connector *c, int mode);
	/* Atomic mode setting.  A driver with these two never sees mode_set,
	 * crtc_disable, page_flip, cursor_set/move or dpms: every change
	 * arrives as a checked drm_atomic_state.  atomic_check refuses a
	 * state the hardware cannot show (and may fill in adjusted_mode /
	 * use_scaler); atomic_commit applies one the check accepted, in
	 * whatever order the hardware wants, and returns 0 -- after which
	 * the core records the state as current. */
	int (*atomic_check)(struct drm_device *dev, struct drm_atomic_state *st);
	int (*atomic_commit)(struct drm_device *dev, struct drm_atomic_state *st);
	/* Entries in the CRTC's gamma table (0: no table).  With atomic ops
	 * the core advertises GAMMA_LUT / GAMMA_LUT_SIZE and hands the table
	 * over in the crtc state; SETGAMMA arrives the same way. */
	uint32_t gamma_size;
	/* Power.  suspend takes the hardware down -- outputs, engines,
	 * interrupts -- without touching the core's idea of what is shown;
	 * resume brings the hardware back to where a mode set can be made,
	 * after which the core replays the committed state through
	 * atomic_commit.  Atomic drivers only. */
	int (*suspend)(struct drm_device *dev);
	int (*resume)(struct drm_device *dev);
	/* 1 when the backend delivers vblanks itself (drm_vblank_tick) */
	int hw_vblank;

	/* driver ioctls: nr is the DRM_COMMAND_BASE-relative number */
	long (*ioctl)(struct drm_device *dev, struct drm_file *fp, unsigned nr,
		      unsigned dir, void *kbuf, unsigned size, int *handled);
	/* which driver nrs a render node may use */
	int (*render_allowed)(unsigned nr);

	/* caps the core does not know */
	int (*get_cap)(struct drm_device *dev, uint64_t cap, uint64_t *val);

	/* The console's screen, checked and rescued (drm_console.c).
	 *
	 * display_verify: the console has set its mode and painted its whole
	 * screen; wait until the device has executed all of it and say
	 * whether the device accepted every command.  A display path that
	 * quietly dropped one is a black screen that nothing else reports.
	 * display_fallback: that path cannot be trusted; put the display back
	 * the way the console found it before this driver initialised.  The
	 * console has already stopped drawing into the driver's buffer and
	 * restored the flush hook it had before. */
	int (*display_verify)(struct drm_device *dev);
	void (*display_fallback)(struct drm_device *dev);
};

struct drm_device {
	const struct drm_driver *drv;
	void *priv; /* backend device state */
	const pci_device_t *pci;
	int index; /* 0 -> card0 / renderD128 */
	char unique[32]; /* "pci:0000:00:0f.0" */

	spinlock_t lock;
	struct drm_file *files;
	struct drm_file *master;
	uint32_t next_magic;

	/* objects */
	struct drm_gem_object *objects;
	/* Objects whose last reference has gone while the device was still
	 * working through the submission that named them.  See drm_gem_reap. */
	struct drm_gem_object *dead;
	int dead_n;
	/* One reaper at a time.  Finishing an object frees the objects it
	 * holds -- a surface drops its backup buffer -- and that lands back in
	 * drm_gem_put(), which reaps.  Without this the queue is walked once
	 * per object ON TOP of the walk that is already running: 256 queued
	 * objects became 256 nested frame sets and a kernel stack overflow
	 * (double fault, opening faz.net). */
	int reaping;
	uint32_t next_obj_id;
	uint32_t next_flink;

	/* fences */
	struct drm_fence *fences;
	uint32_t fence_seq; /* last issued */
	uint32_t fence_passed; /* last known signalled */

	/* mode objects (ids allocated from next_mode_id) */
	uint32_t next_mode_id;
	struct drm_crtc crtc[DRM_MAX_CONNECTORS];
	uint32_t ncrtc;
	struct drm_connector conn[DRM_MAX_CONNECTORS];
	uint32_t nconn;
	struct drm_encoder enc[DRM_MAX_CONNECTORS];
	uint32_t nenc;
	struct drm_plane planes[DRM_MAX_PLANES];
	uint32_t nplanes;
	struct drm_prop props[DRM_MAX_PROPS];
	uint32_t nprops;
	struct drm_blob blobs[DRM_MAX_BLOBS];
	struct drm_framebuffer fbs[DRM_MAX_FBS];
	uint32_t min_width, min_height, max_width, max_height;
	/* well-known property ids */
	uint32_t prop_dpms, prop_edid, prop_crtc_id, prop_type, prop_fb_id,
		prop_active, prop_mode_id, prop_src_x, prop_src_y, prop_src_w,
		prop_src_h, prop_crtc_x, prop_crtc_y, prop_crtc_w, prop_crtc_h,
		prop_in_formats, prop_link_status, prop_non_desktop,
		prop_gamma_lut, prop_gamma_lut_size;
	uint32_t in_formats_blob; /* the primary planes' */
	uint32_t in_formats_cursor_blob;
	/* Bumped on every connector status change (drm_connector_hotplug);
	 * what /sys reports so a client can tell whether to look again. */
	uint32_t hotplug_epoch;

	/* vblank: one counter per crtc, driven by hrtimer or the backend */
	struct {
		uint64_t count;
		uint64_t last_ns;
		uint64_t period_ns;
		hrtimer_t timer;
		int running;
		/* How many waiters need the counter to keep advancing.  The
		 * timer runs while the CRTC is active OR this is non-zero, so
		 * that turning a CRTC off cannot strand someone mid-wait. */
		int refs;
	} vbl[DRM_MAX_CONNECTORS];
	struct wait_queue_head vbl_wq;
	uint32_t refresh_hz;

	/* the devfs nodes */
	struct devfs_node node_card, node_render;
	char devname_card[32], devname_render[32];
	/* all registered devices, for drm_late_init() */
	struct drm_device *next_dev;
	int late_init_done;
	int suspended; /* drm_suspend() done, drm_resume() not yet */
};

/* Registration: creates the nodes and the sysfs entries. */
/* The kernel console as a client of this device: a buffer object of its
 * own, a mode set that shows it, and its dirty rectangles pushed through
 * the driver.  Called by a driver once its modes exist; a device that
 * cannot set a mode or report a dirty rectangle is refused, and the console
 * keeps the framebuffer it already had.  See drm_console.c. */
int drm_console_takeover(struct drm_device *dev);
void drm_console_suspend(struct drm_device *dev);
/* Start the thread that pushes the console's dirty rectangles.  Called by
 * the startup code once the scheduler exists; until then pushes are inline. */
void drm_console_start_worker(void);
void drm_console_resume(struct drm_device *dev);
/* Resume the pushes only: the screen was already set up again. */
void drm_console_resume_pushes(struct drm_device *dev);
int drm_console_active(const struct drm_device *dev);

int drm_dev_register(struct drm_device *dev, const struct drm_driver *drv,
		     const pci_device_t *pci, void *priv);
/* The device down and up again (a system sleep, or the power_state
 * attribute in /sys for exercising the path): 0, or -ENODEV for a
 * driver without the entry points. */
int drm_suspend(struct drm_device *dev);
int drm_resume(struct drm_device *dev);
/* Every registered device, in registration order. */
int drm_suspend_all(void);
int drm_resume_all(void);
/* Once storage is up: every registered driver's late_init, once. */
void drm_late_init(void);

/* ---- synchronisation objects (drm_syncobj.c) --------------------------- */
/* A container for a fence (binary) or for a timeline of them, shared
 * between processes as a descriptor; what a renderer passes between its
 * submissions and what it waits on.  The kernel-side users are drivers
 * attaching the fences of their submissions. */
struct drm_syncobj;
int drm_syncobj_is_ioctl(unsigned nr);
long drm_syncobj_ioctl(struct drm_device *dev, struct drm_file *fp,
		       unsigned nr, void *kb, unsigned size, int *handled);
void drm_syncobj_file_release(struct drm_file *fp);
/* By handle (a reference), NULL when unknown. */
struct drm_syncobj *drm_syncobj_lookup(struct drm_file *fp, uint32_t handle);
void drm_syncobj_get(struct drm_syncobj *so);
void drm_syncobj_put(struct drm_syncobj *so);
/* The binary payload (a reference), NULL when none. */
struct drm_fence *drm_syncobj_fence_get(struct drm_syncobj *so);
/* Replace the binary payload (NULL = reset). */
void drm_syncobj_replace_fence(struct drm_syncobj *so, struct drm_fence *f);
/* Attach a fence at a timeline point (point 0 = the binary payload). */
int drm_syncobj_add_point(struct drm_syncobj *so, struct drm_fence *f,
			  uint64_t point);
/* The fence that stands for `point' (a reference): the binary payload for
 * 0, else the earliest submitted point at or beyond it.  -ENOENT when
 * nothing has been submitted for it yet; an always-signalled fence when
 * the timeline has already passed it. */
int drm_syncobj_find_fence(struct drm_syncobj *so, uint64_t point,
			   struct drm_fence **out);

/* KMS helpers for backends. */
void drm_mode_fill(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		   uint32_t hz, int preferred);
int drm_connector_add(struct drm_device *dev, uint32_t type, uint32_t mm_w,
		      uint32_t mm_h);
/* An overlay plane on the crtcs of `possible_crtcs' (the primary and
 * cursor planes of every crtc are made by drm_connector_add).  NULL
 * formats: the driver's scanout list. */
int drm_plane_add(struct drm_device *dev, uint32_t type, uint32_t possible_crtcs,
		  const uint32_t *formats, uint32_t nformats,
		  const uint64_t *modifiers, uint32_t nmodifiers);
/* The connector's status changed (a hotplug): re-probe it, bump the
 * epoch clients read from /sys, note it in the log. */
void drm_connector_hotplug(struct drm_device *dev, int conn);
int drm_connector_add_mode(struct drm_device *dev, int conn,
			   const struct drm_mode_modeinfo *m);
/* Forget the connector's modes (before get_modes refills them). */
void drm_connector_clear_modes(struct drm_device *dev, int conn);
/* Install an EDID: the blob property, the physical size, and the mode
 * list with the preferred mode first.  A NULL/short EDID removes the
 * blob.  Returns the number of modes, or a negative errno. */
int drm_connector_set_edid(struct drm_device *dev, int conn,
			   const uint8_t *edid, unsigned len);
/* Add the standard table's modes that fit the connector's EDID range (or,
 * with no EDID, the caller's limits) and are not already listed. */
int drm_connector_add_std_modes(struct drm_device *dev, int conn,
				uint32_t max_clock_khz, uint32_t max_w,
				uint32_t max_h);
struct i2c_adapter;
/* Read a display's EDID over an I2C adapter (DDC at address 0x50, one
 * block per read, extensions at their segment).  `buf' holds up to
 * `max_blocks' * 128 bytes; returns the number of blocks read, 0 when
 * the display did not answer, negative on a bad base block. */
int drm_edid_read(struct i2c_adapter *adapter, uint8_t *buf, int max_blocks);
struct drm_framebuffer *drm_fb_lookup(struct drm_device *dev, uint32_t id);
/* Deliver a vblank from hardware (when drv->hw_vblank). */
void drm_vblank_tick(struct drm_device *dev, int crtc);
/* The current scanout framebuffer object of a crtc (no reference). */
struct drm_gem_object *drm_crtc_scanout(struct drm_device *dev, int crtc);

/* Copy helpers with the user pointer already validated by the caller. */
int drm_copy_from_user(void *dst, const void *user, size_t n);
int drm_copy_to_user(void *user, const void *src, size_t n);

/* File accessors for backends. */
static inline int drm_file_is_master(struct drm_file *fp)
{
	return fp->is_master;
}

#endif
