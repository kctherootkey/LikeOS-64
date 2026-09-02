// LikeOS-64 -- display-manager core (the DRM interface), kernel side.
//
// One device = one GPU with a primary node (/dev/dri/cardN: mode setting,
// master/authentication, everything) and a render node (/dev/dri/renderDN:
// rendering ioctls only, for any process).  Backends register a
// drm_driver; the core owns the descriptor semantics, handle namespaces,
// buffer sharing across processes (PRIME / dma-buf), fences (sync_file),
// mode objects and the event/vblank machinery, and calls the backend for
// what touches hardware.  The core never includes a backend header.
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
/* 0 signalled, -ETIMEDOUT, -EINTR. */
int drm_fence_wait(struct drm_fence *f, uint64_t timeout_ns);
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
	struct drm_device *dev;
	enum drm_gem_kind kind;
	uint32_t id; /* device-global, for mmap offsets and flink names */
	uint64_t size; /* bytes */
	uint32_t npages;
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
uint32_t drm_gem_handle_of_slot(struct drm_file *fp, uint32_t slot);
struct drm_gem_object *drm_gem_lookup_foreign(struct drm_device *dev,
					      uint32_t handle);
/* ---- dirty tracking (drm_dirty.c) -------------------------------------- */
/* One more dirty-tracking user of the object (refcounted).  Zero on
 * success; on failure the object simply has no tracker. */
int drm_gem_dirty_add(struct drm_gem_object *o);
void drm_gem_dirty_release(struct drm_gem_object *o);
/* Harvest the processor's record of client writes into the tracker.
 * Called once per object per submission, before the ranges are consumed. */
void drm_gem_dirty_scan(struct drm_gem_object *o);
/* Hand the accumulated dirty page ranges to the caller, clearing them from
 * the tracker: cb(arg, first, last) per run of dirty pages. */
void drm_gem_dirty_transfer(struct drm_gem_object *o,
			    void (*cb)(void *arg, uint64_t first,
				       uint64_t last),
			    void *arg);
/* Record one written page directly (for mapping flavours the sweeps do
 * not walk; see drm_dirty.c): fault-time and unmap-time respectively. */
void drm_gem_dirty_fault_page(struct drm_gem_object *o, uint64_t page);
void drm_gem_dirty_mark_page(struct drm_gem_object *o, uint64_t page);
/* Census of region records mapping the object; the get/put wrappers of
 * every mapping flavour call these as records come and go. */
void drm_gem_dirty_map_note(struct drm_gem_object *o);
void drm_gem_dirty_map_drop(struct drm_gem_object *o);
int drm_gem_dirty_wp_new_mapping(struct drm_gem_object *o);
/* The mapping callbacks a device-mmap of a gem object registers. */
struct mm_dirty_ops;
extern const struct mm_dirty_ops drm_gem_dirty_mmap_ops;

/* Handles: per-file namespace. */
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

#define DRM_MAX_CONNECTORS 4
#define DRM_MAX_MODES 24
#define DRM_MAX_PROPS 48
#define DRM_MAX_BLOBS 32
#define DRM_MAX_FBS 64
#define DRM_MAX_HANDLES 4096

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
	uint32_t fb_id;
	int x, y;
	uint16_t gamma[3][256];
	/* cursor */
	uint32_t cursor_handle_w, cursor_handle_h;
	int cursor_x, cursor_y;
	struct drm_gem_object *cursor_obj;
	uint32_t primary_plane_id, cursor_plane_id;
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
	/* handle -> object; index 0 unused (handle 0 is "none") */
	struct drm_gem_object **handles;
	uint32_t nhandles;
	uint32_t file_id; /* names this file inside every handle it hands out */
	/* fence handle -> fence, a namespace of its own */
	struct drm_fence **fences;
	uint32_t nfences;
	spinlock_t lock;
	/* event queue */
	struct drm_pending_event *events, *events_tail;
	struct wait_queue_head wq;
	int pending_vblank; /* WAIT_VBLANK events queued */
	uint64_t client_caps; /* bit n = DRM_CLIENT_CAP_n */
	void *priv; /* backend per-file state */
	struct drm_file *next; /* device list */
};

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

	/* file lifetime */
	int (*open)(struct drm_device *dev, struct drm_file *fp);
	void (*postclose)(struct drm_device *dev, struct drm_file *fp);
	/* master acquired / dropped */
	void (*master_set)(struct drm_device *dev, struct drm_file *fp);
	void (*master_drop)(struct drm_device *dev, struct drm_file *fp);

	/* objects */
	int (*gem_init)(struct drm_gem_object *o); /* after pages exist */
	void (*gem_free)(struct drm_gem_object *o);
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

	/* KMS */
	int (*mode_set)(struct drm_device *dev, struct drm_crtc *crtc,
			const struct drm_mode_modeinfo *mode,
			struct drm_framebuffer *fb, int x, int y);
	int (*crtc_disable)(struct drm_device *dev, struct drm_crtc *crtc);
	/* the framebuffer scanned out changed contents in these rects */
	int (*fb_dirty)(struct drm_device *dev, struct drm_crtc *crtc,
			struct drm_framebuffer *fb,
			const struct drm_mode_rect_k *rects, uint32_t n);
	/* flip to fb (immediately; the core sends the event) */
	int (*page_flip)(struct drm_device *dev, struct drm_crtc *crtc,
			 struct drm_framebuffer *fb);
	int (*cursor_set)(struct drm_device *dev, struct drm_crtc *crtc,
			  struct drm_gem_object *o, uint32_t w, uint32_t h,
			  int32_t hot_x, int32_t hot_y);
	int (*cursor_move)(struct drm_device *dev, struct drm_crtc *crtc,
			   int x, int y);
	int (*dpms)(struct drm_device *dev, struct drm_connector *c, int mode);
	/* 1 when the backend delivers vblanks itself (drm_vblank_tick) */
	int hw_vblank;

	/* driver ioctls: nr is the DRM_COMMAND_BASE-relative number */
	long (*ioctl)(struct drm_device *dev, struct drm_file *fp, unsigned nr,
		      unsigned dir, void *kbuf, unsigned size, int *handled);
	/* which driver nrs a render node may use */
	int (*render_allowed)(unsigned nr);

	/* caps the core does not know */
	int (*get_cap)(struct drm_device *dev, uint64_t cap, uint64_t *val);
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
	struct drm_prop props[DRM_MAX_PROPS];
	uint32_t nprops;
	struct drm_blob blobs[DRM_MAX_BLOBS];
	struct drm_framebuffer fbs[DRM_MAX_FBS];
	uint32_t min_width, min_height, max_width, max_height;
	/* well-known property ids */
	uint32_t prop_dpms, prop_edid, prop_crtc_id, prop_type, prop_fb_id,
		prop_active, prop_mode_id, prop_src_x, prop_src_y, prop_src_w,
		prop_src_h, prop_crtc_x, prop_crtc_y, prop_crtc_w, prop_crtc_h,
		prop_in_formats, prop_link_status, prop_non_desktop;
	uint32_t in_formats_blob;

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
int drm_console_active(const struct drm_device *dev);

int drm_dev_register(struct drm_device *dev, const struct drm_driver *drv,
		     const pci_device_t *pci, void *priv);

/* KMS helpers for backends. */
void drm_mode_fill(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		   uint32_t hz, int preferred);
int drm_connector_add(struct drm_device *dev, uint32_t type, uint32_t mm_w,
		      uint32_t mm_h);
int drm_connector_add_mode(struct drm_device *dev, int conn,
			   const struct drm_mode_modeinfo *m);
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
