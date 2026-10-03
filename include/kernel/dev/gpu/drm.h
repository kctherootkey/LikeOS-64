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
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/drm_rect.h>
#include <kernel/dev/gpu/drm_vblank.h>

struct drm_device;
struct drm_file;
struct drm_gem_object;
struct drm_fence;
struct drm_format_info;
struct drm_atomic_state;
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

struct drm_fence_cb;
struct drm_fence_ops;

/* Called once, when the fence the callback was added to signals.
 *
 * Callbacks run in whatever context signalled the fence: the device
 * interrupt, the driver's poll timer, or a task.  They run with the fence
 * lock (the device's `lock') dropped but with local interrupts DISABLED, so
 * a callback must be short, must not sleep, must not wait for a fence, and
 * may take only interrupt-safe locks.  It may take and drop fence
 * references, signal other fences and add callbacks to other fences. */
typedef void (*drm_fence_func_t)(struct drm_fence *f, struct drm_fence_cb *cb);

/* The links of a fence's callback list (a circular list with the fence's
 * own node as the head; a node that points at itself is on no list). */
struct drm_fence_cb_node {
	struct drm_fence_cb_node *next, *prev;
};

/* Embedded in whatever wants to know: initialised by
 * drm_fence_add_callback(), never by the caller. */
struct drm_fence_cb {
	struct drm_fence_cb_node node;
	drm_fence_func_t func;
};

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
	/* A merged fence (SYNC_IOC_MERGE): signalled once both of these
	 * are; each is referenced while the merged one lives. */
	struct drm_fence *deps[2];

	/* Callbacks and fence containers.
	 *
	 * `ops' is NULL for a device's own fences (and for merged ones):
	 * those are signalled by the driver through drm_fence_signal*().  A
	 * container -- an array, a chain link, a private signalled stub --
	 * has ops of its own and is NOT on the device's list: it lives by
	 * its reference count alone (DRM_FENCE_SF_UNLISTED), and `dev' is
	 * the device of a fence it contains, which lends it its lock.
	 *
	 * `cb_list' holds the callbacks to run when the fence signals, under
	 * dev->lock; `cb_running' is the one being run right now (with the
	 * lock dropped), so that drm_fence_remove_callback() can wait for it
	 * to finish instead of letting its caller free it under the runner. */
	const struct drm_fence_ops *ops;
	struct drm_fence_cb_node cb_list;
	struct drm_fence_cb *volatile cb_running;
	uint32_t sflags; /* DRM_FENCE_SF_*, changed atomically */
};

/* drm_fence.sflags */
#define DRM_FENCE_SF_UNLISTED (1u << 0) /* not on dev->fences */
#define DRM_FENCE_SF_ENABLE_SIGNAL (1u << 1) /* enable_signaling done */

/* What a fence that is not one of a device's own does differently.  Every
 * hook is optional. */
struct drm_fence_ops {
	const char *(*get_driver_name)(struct drm_fence *f);
	const char *(*get_timeline_name)(struct drm_fence *f);
	/* Someone wants to know when the fence signals: start watching what
	 * it is made of.  Called once, without any fence lock held.  Returns
	 * false when the fence turned out to be complete already; the core
	 * then signals it. */
	bool (*enable_signaling)(struct drm_fence *f);
	/* Is it complete?  Must not sleep; may be called in any context.
	 * The core signals the fence when this says yes. */
	bool (*signaled)(struct drm_fence *f);
	/* The last reference went: free the fence (default: kfree). */
	void (*release)(struct drm_fence *f);
	/* A waiter would like the fence signalled by `deadline_ns'
	 * (hrtimer_now_ns() time base). */
	void (*set_deadline)(struct drm_fence *f, uint64_t deadline_ns);
};

struct drm_fence *drm_fence_create(struct drm_device *dev, uint32_t seqno,
				   uint32_t flags);
void drm_fence_get(struct drm_fence *f);
void drm_fence_put(struct drm_fence *f);
void drm_fence_signal(struct drm_fence *f);
/* A fence that signals once both have (one of them, referenced, when
 * the other has already); NULL without memory. */
struct drm_fence *drm_fence_merge(struct drm_fence *a, struct drm_fence *b);
/* Say (rate-limited) that the caller has waited `waited_ns' in `how' for
 * a fence that has not signalled, and let the driver say why. */
void drm_fence_report_stuck(struct drm_fence *f, uint64_t waited_ns, const char *how);
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

/* ---- callbacks, status, deadlines, containers (drm_fence.c) ------ */

/* Run `func' when `f' signals.  0 when added; -ENOENT when the fence has
 * signalled already (the callback is NOT called); -EINVAL on bad
 * arguments.  The caller keeps a reference to `f' while the callback is
 * added.  A callback is on at most one fence at a time.  See
 * drm_fence_func_t for the context callbacks run in. */
int drm_fence_add_callback(struct drm_fence *f, struct drm_fence_cb *cb,
			   drm_fence_func_t func);
/* Take a callback off again: true when it was removed before running
 * (it will not run); false when the fence has signalled and the callback
 * has run -- if it was running on another processor, this waits for it
 * to return.  Never call it on a callback from inside that callback. */
bool drm_fence_remove_callback(struct drm_fence *f, struct drm_fence_cb *cb);
/* Ask a container to start watching its parts (once); nothing for a
 * device's own fences, which the driver signals anyway. */
void drm_fence_enable_signaling(struct drm_fence *f);
/* Has the fence signalled?  For a container, also asks whether its parts
 * have (and signals it if so).  Never asks the driver, never sleeps: safe
 * in any context. */
bool drm_fence_is_signaled(struct drm_fence *f);
/* The same, after asking the driver to bring its idea of completed work
 * up to date (drv->fence_poll) -- process context only, no locks held. */
bool drm_fence_poll_signaled(struct drm_fence *f);
/* 0 still pending, 1 signalled without error, the negative error the work
 * failed with otherwise. */
int drm_fence_get_status(struct drm_fence *f);
/* Mark the work behind an unsignalled fence as failed (-Exxx), before it
 * is signalled. */
void drm_fence_set_error(struct drm_fence *f, int error);
/* Signal with an explicit completion time (hrtimer_now_ns() base);
 * -EINVAL when it had signalled already. */
int drm_fence_signal_timestamp(struct drm_fence *f, uint64_t timestamp_ns);
/* When it signalled (only meaningful once it has). */
static inline uint64_t drm_fence_timestamp(const struct drm_fence *f)
{
	return f->signal_ns;
}
/* A hint: the waiter would like the fence signalled by `deadline_ns'.
 * Forwarded to the container's parts, or to drv->fence_set_deadline. */
void drm_fence_set_deadline(struct drm_fence *f, uint64_t deadline_ns);
/* Wait until any one of `fences' signals.  0 with *idx (if given) the
 * index of a signalled one; -ETIMEDOUT, -ERESTARTSYS when `intr' and a
 * signal is pending, -EINVAL, -ENOMEM.  A zero timeout only checks.  The
 * caller holds references to every fence. */
int drm_fence_wait_any_timeout(struct drm_fence **fences, uint32_t count,
			       int intr, uint64_t timeout_ns, uint32_t *idx);
/* `num' consecutive fresh stream numbers for fences that are not a
 * device's own (containers, software timelines); never zero and never
 * one an engine of this kernel uses. */
uint64_t drm_fence_context_alloc(unsigned num);
/* The timeline a fence orders on, for comparing and de-duplicating: its
 * stream when it has one, the device-wide sequence of its device, or a
 * timeline of its own for a merged fence. */
uint64_t drm_fence_timeline(const struct drm_fence *f);
/* Is `a' after `b' on their (common) timeline? */
bool drm_fence_is_later(const struct drm_fence *a, const struct drm_fence *b);
bool drm_fence_is_later_or_same(const struct drm_fence *a, const struct drm_fence *b);
/* Names for reports and the sync_file listing. */
const char *drm_fence_driver_name(struct drm_fence *f);
void drm_fence_timeline_name(struct drm_fence *f, char *buf, unsigned len);
/* A private fence, signalled at `timestamp_ns', for "nothing to wait
 * for" results that should still say when that became true. */
struct drm_fence *drm_fence_signalled_at(struct drm_device *dev,
					 uint64_t timestamp_ns);
/* For container implementations: set up `f' (zeroed by the caller) as a
 * fence of no device list, lent `dev''s lock, with `ops', on stream
 * `context' at `seqno64'. */
void drm_fence_init_unlisted(struct drm_fence *f, struct drm_device *dev,
			     const struct drm_fence_ops *ops, uint64_t context,
			     uint64_t seqno64);
/* Is the fence a container (array or chain link)? */
bool drm_fence_is_container(struct drm_fence *f);
/* drm_fence_get() as an expression: `f' (may be NULL), referenced. */
static inline struct drm_fence *drm_fence_ref(struct drm_fence *f)
{
	if (f)
		drm_fence_get(f);
	return f;
}

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
/* The device runs again after a suspend: finish the objects whose
 * teardown waited at the power gate meanwhile.  Process context. */
void drm_gem_pm_resumed(struct drm_device *dev);

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
/* Room for the properties a driver attaches per object (rotation, zpos,
 * colour, connector properties) besides the global ones, and for the blobs
 * clients create per mode set and per frame (modes, lookup tables, damage
 * rectangles) on top of the kernel's own (EDIDs, format lists). */
#define DRM_MAX_PROPS 192
#define DRM_MAX_BLOBS 256
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
	/* An enum or bitmask property with more entries than enums[] holds
	 * keeps its whole list here instead, allocated by drm_property.c
	 * (enum_list_cap entries of room); NULL while enums[] suffices.
	 * nenums counts the entries either way. */
	struct drm_mode_property_enum *enum_list;
	uint32_t enum_list_cap;
	/* The device the property belongs to (for the
	 * blob and object lookups a value check needs), and the number of
	 * values it was created with -- for an enum or bitmask the most
	 * entries it takes, -1 when it was made without a bound. */
	struct drm_device *dev;
	int num_values;
};

struct drm_blob {
	uint32_t id;
	uint32_t length;
	void *data;
	int in_use;
	/* Reference counted (drm_property.h): the creator holds one, every
	 * state or object that refers to the blob holds one.  `owner' is the
	 * file that created it through CREATEPROPBLOB (NULL: the kernel);
	 * its reference goes when that file destroys the blob or closes. */
	struct drm_device *dev;
	int refs;
	struct drm_file *owner;
};

struct drm_framebuffer {
	uint32_t id; /* 0 = free slot */
	uint32_t width, height, pitch, format, bpp, depth;
	uint64_t modifier;
	struct drm_gem_object *obj;
	uint32_t offset;
	struct drm_file *owner;
	/* The format's description (drm_fourcc.h); NULL where the framebuffer
	 * was made without one. */
	const struct drm_format_info *format_info;
	/* Per plane of the format: pitch, offset and object.  Plane 0 is the
	 * same as pitch / offset / obj above, which stay the single-plane
	 * view everything else reads; objs[0] is `obj' (no second reference),
	 * objs[1..3] hold a reference each.  Unused planes are 0 / NULL. */
	uint32_t pitches[4];
	uint32_t offsets[4];
	struct drm_gem_object *objs[4];
	/* Framebuffers carry no properties: always NULL.  (A pointer rather
	 * than an embedded list: the device keeps DRM_MAX_FBS of these.) */
	struct drm_object_properties *properties;
	/* The owning file let go of it (CLOSEFB) while something still
	 * showed it.  Owner NULL from then on; drm_framebuffer.c frees it
	 * once no plane or crtc shows it any more. */
	int closed;
};

struct drm_crtc {
	uint32_t id;
	int index;
	int active;
	/* A mode is set on the crtc (MODE_ID names one).  A crtc can be enabled
	 * and not active: DPMS off keeps its mode, its planes and its
	 * connectors and only stops the pipe, so that switching it back on
	 * shows what it showed.  active implies enabled. */
	int enabled;
	/* Entries of the legacy gamma table (GETCRTC's gamma_size, SETGAMMA,
	 * GETGAMMA), at most 256; 0 reads as 256.  Independent of the
	 * GAMMA_LUT_SIZE an atomic client sees.  drm_mode_crtc_set_gamma_size()
	 * sets it. */
	uint32_t gamma_size;
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
	/* The legacy cursor's hot spot (CURSOR2), for a driver without atomic
	 * entry points: what a resume sets the cursor with again. */
	int32_t cursor_hot_x, cursor_hot_y;
	/* The framebuffer the core wraps a legacy cursor object in, so that
	 * the cursor is a plane like any other (atomic drivers). */
	uint32_t cursor_fb_id;
	uint32_t primary_plane_id, cursor_plane_id;
	/* the properties attached to it (drm_mode_object.h) */
	struct drm_object_properties properties;

	/* The committed colour management (DEGAMMA_LUT, CTM,
	 * GAMMA_LUT as blobs, each referenced while the crtc shows it, NULL
	 * for none) and VRR_ENABLED -- what the next request starts from. */
	struct drm_blob *degamma_lut, *ctm, *gamma_lut;
	int vrr_enabled;
	/* The stream the crtc's OUT_FENCE_PTR fences are numbered on (0
	 * until the first one) and the last number handed out. */
	uint64_t fence_context;
	uint64_t fence_seqno;
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
	/* the properties attached to it (drm_mode_object.h) */
	struct drm_object_properties properties;

	/* The committed values of the optional plane properties
	 * (meaning as in struct drm_plane_state).  drm_plane_add sets the
	 * defaults: rotation DRM_MODE_ROTATE_0, alpha 0xffff (opaque),
	 * pixel_blend_mode 0 (pre-multiplied), the rest 0; whoever attaches
	 * a property with another initial value sets the field too. */
	uint32_t rotation;
	uint32_t zpos, normalized_zpos;
	uint16_t alpha;
	uint16_t pixel_blend_mode;
	uint32_t color_encoding;
	uint32_t color_range;
	/* The plane's own instance of each optional property, NULL where
	 * the plane does not have it.  Set by whoever creates and attaches
	 * the property (drm_blend.c, drm_color_mgmt.c); the atomic core
	 * routes a write of that property into the state field above. */
	struct drm_prop *rotation_property;
	struct drm_prop *zpos_property;
	struct drm_prop *alpha_property;
	struct drm_prop *blend_mode_property;
	struct drm_prop *color_encoding_property;
	struct drm_prop *color_range_property;
	/* HOTSPOT_X / HOTSPOT_Y of a cursor plane (DRM_FEATURE_CURSOR_HOTSPOT),
	 * NULL elsewhere; a write goes to the state's hot_x / hot_y. */
	struct drm_prop *hotspot_x_property;
	struct drm_prop *hotspot_y_property;
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
	/* What the sink can do, from its EDID (drm_display_info.h); zero
	 * without one. */
	struct drm_display_info display_info;
	/* The sink's EDID-Like Data for the audio driver (drm_eld.h), all
	 * zero when there is none. */
	uint8_t eld[128];
	/* the properties attached to it (drm_mode_object.h) */
	struct drm_object_properties properties;

	/* The committed values of the optional connector properties
	 * (meaning as in struct drm_connector_state); hdr_output_metadata is
	 * referenced while committed, NULL none. */
	uint32_t max_requested_bpc;
	uint32_t colorspace;
	uint32_t broadcast_rgb;
	uint32_t content_type;
	uint32_t scaling_mode;
	struct drm_blob *hdr_output_metadata;
	/* The connector's own instance of each optional property, NULL where
	 * it does not have it; set by whoever creates and attaches it.  (The
	 * device-wide ones are dev->prop_content_type and
	 * dev->prop_hdr_output_metadata.) */
	struct drm_prop *max_bpc_property;
	struct drm_prop *colorspace_property;
	struct drm_prop *broadcast_rgb_property;
	struct drm_prop *scaling_mode_property;

	/* Probing and the connector's EDID-derived state (drm_connector.c,
	 * drm_probe_helper.c).  drm_connector_add sets them. */
	struct drm_device *dev; /* the device it belongs to */
	/* Bumped whenever the status or the EDID changes; a probe compares
	 * it to tell whether to say so. */
	uint64_t epoch_counter;
	/* What the source behind the connector can send.  The probe refuses
	 * the modes needing what is not allowed; the defaults are what the
	 * drivers have always been offered (interlace and double scan yes,
	 * stereo and 4:2:0-only no). */
	bool interlace_allowed;
	bool doublescan_allowed;
	bool stereo_allowed;
	bool ycbcr_420_allowed;
	/* The EDID, PATH and TILE blobs (each referenced while installed;
	 * edid_blob_id is the EDID's id, 0 none). */
	struct drm_blob *edid_blob_ptr;
	struct drm_blob *path_blob_ptr;
	struct drm_blob *tile_blob_ptr;
	/* A tile of a monitor shown over several connectors (from the
	 * EDID's DisplayID block): the group (tile_group_id, 0 none, names
	 * it), the grid and this tile's place and size in it. */
	bool has_tile;
	bool tile_is_single_monitor;
	uint8_t num_h_tile, num_v_tile;
	uint8_t tile_h_loc, tile_v_loc;
	uint16_t tile_h_size, tile_v_size;
	uint32_t tile_group_id;
	/* The sink's audio/video latencies from its HDMI block (ms),
	 * progressive and interlaced. */
	bool latency_present[2];
	int video_latency[2];
	int audio_latency[2];
	/* "link-status": DRM_MODE_LINK_STATUS_GOOD / _BAD, set by the driver
	 * when the link failed and a client should set the mode again. */
	uint64_t link_status;
	/* The bpc the driver settled on under "max bpc". */
	uint32_t max_bpc;
	/* "vrr_capable", immutable, NULL when the connector does not have it. */
	struct drm_prop *vrr_capable_property;
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
	/* Bytes of the blobs this file created and still owns (CREATEPROPBLOB),
	 * bounded per file (drm_property.c); under the blob lock. */
	uint64_t blob_bytes;
	void *priv; /* backend per-file state */
	struct vfs_file *vfs; /* the open file this is the state of */
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
	/* The request this state belongs to. */
	struct drm_atomic_state *state;
	/* DRM_MODE_ROTATE_* | DRM_MODE_REFLECT_* */
	uint32_t rotation;
	/* stacking position as asked, and as normalised to 0..n-1 over the
	 * planes of the crtc */
	uint32_t zpos, normalized_zpos;
	/* plane opacity, 0 transparent .. 0xffff opaque */
	uint16_t alpha;
	/* DRM_MODE_BLEND_* (drm_blend.h) */
	uint16_t pixel_blend_mode;
	/* YCbCr to RGB conversion: enum drm_color_encoding / drm_color_range
	 * values (drm_color_mgmt.h) */
	uint32_t color_encoding;
	uint32_t color_range;
	/* FB_DAMAGE_CLIPS: the blob id (0 none), and its rectangles as the
	 * atomic core resolved them (pointing into the blob, valid for the
	 * request); ignore_damage_clips: the driver redraws everything */
	uint32_t fb_damage_clips;
	struct drm_mode_rect *damage_clips;
	uint32_t num_damage_clips;
	int ignore_damage_clips;
	/* After the plane check: the source (16.16) and destination (crtc
	 * pixels) clipped to the crtc, and whether anything of the plane is
	 * visible at all. */
	struct drm_rect src, dst;
	int visible;
	/* IN_FENCE_FD: what the update waits for before it is shown
	 * (referenced; NULL none). */
	struct drm_fence *in_fence;
	/* The FB_DAMAGE_CLIPS blob itself, referenced until the state
	 * is freed (damage_clips points into it; clearing fb_damage_clips /
	 * damage_clips does not drop it). */
	struct drm_blob *fb_damage_clips_blob;
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
	/* The request this state belongs to. */
	struct drm_atomic_state *state;
	/* planes on this crtc in this state (bit = plane index) */
	uint32_t plane_mask;
	/* Colour management as blobs, each referenced by the state until it
	 * is freed (NULL = none, bypass): DEGAMMA_LUT and GAMMA_LUT of
	 * struct drm_color_lut, CTM of struct drm_color_ctm;
	 * color_mgmt_changed when one of them was replaced in the request.
	 * (The legacy 256-entry table above stays what drivers program; the
	 * core keeps it in step with GAMMA_LUT.) */
	struct drm_blob *degamma_lut, *ctm, *gamma_lut;
	int color_mgmt_changed;
	/* VRR_ENABLED: variable refresh asked for */
	int vrr_enabled;
	/* the flip may happen outside vblank (DRM_MODE_PAGE_FLIP_ASYNC) */
	int async_flip;
	/* OUT_FENCE_PTR: the client's s32 the request's fence descriptor
	 * goes to (a user address, 0 = none) */
	uint64_t out_fence_ptr;
	/* The MODE_ID blob the request named, referenced until the
	 * state is freed (NULL: the mode is the committed one or none); and
	 * the out-fence made for out_fence_ptr with its descriptor (-1),
	 * between the ATOMIC ioctl's preparation and its end. */
	struct drm_blob *mode_blob;
	struct drm_fence *out_fence;
	int out_fence_fd;
};

struct drm_connector_state {
	struct drm_connector *conn;
	int crtc; /* crtc index, -1 */
	int dpms;
	int changed;
	/* The request set DPMS (the legacy property); dpms is what it set. */
	int dpms_changed;
	/* The request this state belongs to. */
	struct drm_atomic_state *state;
	/* "max bpc" as asked, and what the driver settled on */
	uint32_t max_requested_bpc, max_bpc;
	/* "Colorspace": DRM_MODE_COLORIMETRY_* */
	uint32_t colorspace;
	/* "Broadcast RGB": automatic / full / limited range */
	uint32_t broadcast_rgb;
	/* "content type": DRM_MODE_CONTENT_TYPE_* */
	uint32_t content_type;
	/* HDR_OUTPUT_METADATA: blob of struct hdr_output_metadata, referenced
	 * by the state until it is freed, NULL none */
	struct drm_blob *hdr_output_metadata;
	/* "scaling mode": DRM_MODE_SCALE_* */
	uint32_t scaling_mode;
	/* "link-status" (DRM_MODE_LINK_STATUS_*): what the request leaves
	 * it at, and what it was when the state was made.  A request only
	 * ever sets a BAD link GOOD; one that does is a full mode set of the
	 * connector's crtc. */
	uint64_t link_status;
	uint64_t old_link_status;
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
/* The same by property (the ATOMIC ioctl's routing).  The value is not
 * checked against the property here: drm_atomic_set_property() does that,
 * and a kernel caller knows what it writes. */
struct drm_prop;
int drm_atomic_plane_set_property(struct drm_atomic_state *st,
				  struct drm_plane *p,
				  struct drm_prop *prop, uint64_t val);
int drm_atomic_crtc_set_property(struct drm_atomic_state *st,
				 struct drm_crtc *c,
				 struct drm_prop *prop, uint64_t val);
int drm_atomic_conn_set_property(struct drm_atomic_state *st,
				 struct drm_connector *c,
				 struct drm_prop *prop, uint64_t val);
/* A client's write of `prop' on object `obj' of type `type'
 * (DRM_MODE_OBJECT_CRTC / PLANE / CONNECTOR), the property attached to it:
 * the value is checked against the property (-EINVAL), what it names kept
 * alive for the write, and the write routed to the object's state. */
int drm_atomic_set_property(struct drm_atomic_state *st, uint32_t type,
			    void *obj, struct drm_prop *prop, uint64_t val);
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

/* drm_driver.features: behaviour of the core a driver opts into, so that a
 * change in what the core does reaches only the drivers that asked for it.
 *
 * ATOMIC_PLANE_CHECK: the atomic check clips each plane against its crtc
 * (drm_atomic_helper_check_plane_state) instead of requiring the plane to
 * lie inside its framebuffer and on the crtc.
 * PROBE_HELPER: a connector's mode list is built by the probe helper from
 * the EDID and the standard table, validated (mode_valid), pruned, de-
 * duplicated and sorted, rather than taken as the driver filled it.
 * DAMAGE_CLIPS: planes advertise FB_DAMAGE_CLIPS.
 * BLEND: planes advertise rotation, zpos, alpha and pixel blend mode (the
 * core's minimal set, see drm_plane_add; a driver that can do more makes
 * its own with drm_blend.h and leaves the bit off).
 * COLOR_MGMT: crtcs advertise DEGAMMA_LUT (when degamma_size below is not
 * 0) and CTM (when color_has_ctm is set) besides GAMMA_LUT.
 * ATOMIC_FENCES: IN_FENCE_FD on planes and OUT_FENCE_PTR on crtcs.
 * VRR: crtcs advertise VRR_ENABLED.
 * CURSOR_HOTSPOT: see its definition.
 *
 * Every one of these that adds properties creates them in drm_kms_init,
 * after the core's own, so a driver without the bit sees the same
 * property ids and listings as before. */
#define DRM_FEATURE_ATOMIC_PLANE_CHECK (1u << 0)
#define DRM_FEATURE_PROBE_HELPER (1u << 1)
#define DRM_FEATURE_DAMAGE_CLIPS (1u << 2)
#define DRM_FEATURE_BLEND (1u << 3)
#define DRM_FEATURE_COLOR_MGMT (1u << 4)
#define DRM_FEATURE_ATOMIC_FENCES (1u << 5)
#define DRM_FEATURE_VRR (1u << 6)
/* CURSOR_HOTSPOT: cursor planes advertise HOTSPOT_X / HOTSPOT_Y (the
 * pointer's hot spot inside the image, for a display that draws the cursor
 * itself -- a virtual machine's host), and an atomic client that has not
 * set DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT does not see the cursor planes
 * at all, since it would leave the hot spot unset. */
#define DRM_FEATURE_CURSOR_HOTSPOT (1u << 7)

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
	/* Optional, thread context, no locks held: called on every tick of
	 * the core's reaper thread (a few milliseconds apart) so that memory
	 * the driver holds back until the device has caught up is returned
	 * when it has, even with no client submitting any more. */
	void (*idle_reclaim)(struct drm_device *dev);
	/* Optional: say what the work behind a fence that has been waited
	 * for for seconds is doing (which engine, how far it got).  Called
	 * from thread context, rate-limited by the core. */
	void (*fence_stuck)(struct drm_device *dev, struct drm_fence *f);
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
	/* Optional: the fence the object's users stand behind, referenced,
	 * NULL when it is idle -- every user's (write: what a writer must
	 * wait for) or the writers' only (what a reader must wait for).
	 * Without it the core takes the object's last fence. */
	struct drm_fence *(*gem_busy_fence)(struct drm_gem_object *o, int write);
	/* Optional: a fence from outside (DMA_BUF_IOCTL_IMPORT_SYNC_FILE)
	 * that the object's next implicitly synchronised users wait for --
	 * every one of them (write), or the writers only. */
	int (*gem_attach_fence)(struct drm_gem_object *o, struct drm_fence *f, int write);
	/* the pages of an object for mmap, or -1 if not mappable */
	uint64_t (*gem_page_phys)(struct drm_gem_object *o, uint64_t index);
	uint64_t gem_mmap_pte_extra;
	/* Optional: the PTE cache bits for a mapping of this object of this
	 * kind (drm_gem_mmap_offset_kind), overriding the constant above.
	 * PAGE_WRITE_THROUGH alone is write-combining; with
	 * PAGE_CACHE_DISABLE, uncached; 0 write-back. */
	uint64_t (*gem_mmap_pte)(struct drm_gem_object *o, unsigned kind);
	/* Optional: a mapping kind the driver serves itself (a graphics
	 * aperture rather than the object's pages).  `first_page' is where
	 * the mapping starts in the object.  Returns 1 with `m' filled in
	 * (the driver took its own reference on the object for the
	 * mapping), 0 to let the core map the pages, or a negative errno. */
	int (*gem_mmap_kind)(struct drm_gem_object *o, unsigned kind,
			     uint64_t first_page, struct device_mmap *m);
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
	/* Optional: may `o' be shown as a w x h ARGB8888 cursor (the legacy
	 * CURSOR / CURSOR2 call on an atomic driver)?  0 or -Exxx.  Without it
	 * the core requires a plain buffer (DRM_GEM_BO) to hold w * h * 4
	 * bytes and lets any other kind of object through for the driver's
	 * own check -- a surface's size is that of its backing store, not of
	 * the image. */
	int (*cursor_obj_check)(struct drm_device *dev, struct drm_gem_object *o,
				uint32_t w, uint32_t h);
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
	 * the core advertises GAMMA_LUT / GAMMA_LUT_SIZE (this many entries,
	 * each crtc's own value changeable with drm_crtc_enable_color_mgmt)
	 * and hands the table over in the crtc state; SETGAMMA arrives the
	 * same way.  The legacy table SETGAMMA / GETGAMMA / GETCRTC speak of is
	 * the crtc's gamma_size (256), not this. */
	uint32_t gamma_size;
	/* Power.  suspend takes the hardware down -- outputs, engines,
	 * interrupts -- without touching the core's idea of what is shown;
	 * resume brings the hardware back to where a mode set can be made,
	 * after which the core shows the committed state again: through
	 * atomic_commit for an atomic driver, else through mode_set,
	 * cursor_set / cursor_move and dpms.  Every ioctl waits while the
	 * device is down (drm_pm_gate_enter). */
	int (*suspend)(struct drm_device *dev);
	int (*resume)(struct drm_device *dev);
	/* 1 when the backend delivers vblanks itself (drm_vblank_tick) */
	int hw_vblank;
	/* Optional, with hw_vblank: the vblank hardware itself.
	 * get_vblank_counter reads the crtc's hardware frame counter, which
	 * wraps at max_vblank_count (0: there is no counter, the core counts
	 * interrupts).  get_scanout_position says where the scanout is
	 * (lines, pixels; negative inside vertical blanking) with the
	 * time the reading was taken between *stime_ns and *etime_ns, for
	 * vblank timestamps that do not depend on interrupt latency; false
	 * when it cannot.  enable_vblank / disable_vblank switch the crtc's
	 * vblank interrupt while somebody needs it.
	 *
	 * All four are called with the vblank lock held and interrupts off
	 * (get_scanout_position also from the vblank interrupt itself): they
	 * must not sleep, and must not call back into the vblank core or
	 * take a lock that is held while calling into it. */
	uint32_t (*get_vblank_counter)(struct drm_device *dev, int crtc);
	uint32_t max_vblank_count;
	bool (*get_scanout_position)(struct drm_device *dev, int crtc,
				     bool in_vblank_irq, int *vpos, int *hpos,
				     uint64_t *stime_ns, uint64_t *etime_ns,
				     const struct drm_display_mode *mode);
	int (*enable_vblank)(struct drm_device *dev, int crtc);
	void (*disable_vblank)(struct drm_device *dev, int crtc);
	/* Optional: can the hardware behind this connector show the mode?
	 * MODE_OK or the reason it cannot; asked while the connector's mode
	 * list is built and when a client sets a mode. */
	enum drm_mode_status (*mode_valid)(struct drm_device *dev,
					   struct drm_connector *c,
					   const struct drm_display_mode *mode);
	/* DRM_FEATURE_*: core behaviour the driver opts into. */
	uint32_t features;
	/* With DRM_FEATURE_COLOR_MGMT: entries in the crtc's degamma table
	 * (0: none -- the crtc then advertises CTM and GAMMA_LUT only). */
	uint32_t degamma_size;
	/* With DRM_FEATURE_COLOR_MGMT: the crtcs have a colour transformation
	 * matrix, and the core attaches CTM to them.  0: no CTM listed. */
	int color_has_ctm;
	/* Optional, with hw_vblank: the core's watchdog found the crtc on
	 * and no vblank for several periods while somebody waits; the
	 * backend says why (and may repair it).  Called from the timer
	 * (interrupt context), each time the timer starts standing in. */
	void (*vblank_report)(struct drm_device *dev, int crtc);

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

	/* Optional: the description of a format in one of the driver's
	 * own layouts (`modifier') where it differs from the generic table,
	 * e.g. a compressed layout with an extra plane; NULL to use the
	 * table.  Asked by drm_get_format_info() on every ADDFB2. */
	const struct drm_format_info *(*get_format_info)(uint32_t format,
							 uint64_t modifier);

	/* Optional: a waiter would like fence `f' (one of this
	 * device's own) signalled by `deadline_ns' (hrtimer_now_ns() base) --
	 * a hint, e.g. to raise the clocks.  May be called in any context,
	 * with no fence lock held; must not sleep. */
	void (*fence_set_deadline)(struct drm_device *dev, struct drm_fence *f,
				   uint64_t deadline_ns);
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

	/* vblank: one counter per crtc, driven by hrtimer or the backend
	 * (drm_vblank.h) */
	struct drm_vblank_crtc vbl[DRM_MAX_CONNECTORS];
	struct wait_queue_head vbl_wq;
	uint32_t refresh_hz;

	/* the devfs nodes */
	struct devfs_node node_card, node_render;
	char devname_card[32], devname_render[32];
	/* all registered devices, for drm_late_init() */
	struct drm_device *next_dev;
	int late_init_done;
	int suspended; /* drm_suspend() done, drm_resume() not yet */
	/* The power gate (drm_drv.c): pm_state is DRM_PM_RUNNING, _SUSPENDING,
	 * _SUSPENDED or _RESUMING, pm_active the gated calls in flight; both
	 * under pm_lock.  Callers sleep on &pm_state (the wait channel) for the
	 * device to run again, a suspend on it for the calls in flight to
	 * leave. */
	spinlock_t pm_lock;
	int pm_state;
	int pm_active;

	/* Ids of the device-wide optional properties, 0 where they
	 * were not created (see the DRM_FEATURE_* bits).  drm_kms_init
	 * creates the crtc and plane ones; the connector ones
	 * (content type, HDR_OUTPUT_METADATA) belong to the connector code. */
	uint32_t prop_degamma_lut, prop_degamma_lut_size, prop_ctm,
		prop_vrr_enabled, prop_fb_damage_clips, prop_in_fence_fd,
		prop_out_fence_ptr, prop_content_type,
		prop_hdr_output_metadata;
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
/* The power gate around everything that reaches the hardware from a
 * client's side.  enter: 0 once the device runs (waiting, interruptibly,
 * while it is suspended or being suspended or resumed), or -ERESTARTSYS when
 * a signal came first; every 0 is paired with drm_pm_gate_exit().
 * tryenter: the same without waiting -- false while the device is not
 * running (for threads that must not block, such as the object reaper:
 * the work is simply left for later).  A suspend waits for the callers
 * inside to leave, and gives up with -EBUSY if they do not within a few
 * seconds.  Process context only. */
#define DRM_PM_RUNNING 0
#define DRM_PM_SUSPENDING 1
#define DRM_PM_SUSPENDED 2
#define DRM_PM_RESUMING 3
int drm_pm_gate_enter(struct drm_device *dev);
bool drm_pm_gate_tryenter(struct drm_device *dev);
void drm_pm_gate_exit(struct drm_device *dev);
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
/* The object's fence (a reference), NULL when none: a binary object's
 * payload, or the newest link of a timeline's fence chain. */
struct drm_fence *drm_syncobj_fence_get(struct drm_syncobj *so);
/* Replace the fence (NULL = reset); a timeline is dropped whole. */
void drm_syncobj_replace_fence(struct drm_syncobj *so, struct drm_fence *f);
/* Attach a fence at a timeline point (point 0: replace the fence, the
 * binary form).  0, or -ENOMEM; any number of points may be pending. */
int drm_syncobj_add_point(struct drm_syncobj *so, struct drm_fence *f,
			  uint64_t point);
/* The fence that stands for `point' (a reference): the fence itself for 0,
 * else the link of the oldest submitted point still covering it.  -ENOENT
 * when nothing has been submitted for it yet (or a point of a binary
 * object is asked for); a signalled fence when the timeline has already
 * passed it; -ENOMEM. */
int drm_syncobj_find_fence(struct drm_syncobj *so, uint64_t point,
			   struct drm_fence **out);
/* The same by handle, with the ioctl's error codes: -ENOENT for an unknown
 * handle, -EINVAL when nothing stands for the point.  `flags' may hold
 * DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT: wait (sleeping, interruptibly,
 * no locks held) up to five seconds for the point to be submitted, then
 * -ETIME or -ERESTARTSYS. */
int drm_syncobj_find_fence_handle(struct drm_file *fp, uint32_t handle,
				  uint64_t point, uint64_t flags,
				  struct drm_fence **fence);
/* Make `chain' (drm_fence_chain_alloc()) the link for `point', holding
 * `fence' (a plain fence or an array, referenced by the call). */
struct drm_fence_chain;
void drm_syncobj_add_point_chain(struct drm_syncobj *so,
				 struct drm_fence_chain *chain,
				 struct drm_fence *fence, uint64_t point);
/* A new object on `dev' (DRM_SYNCOBJ_CREATE_SIGNALED: holding a signalled
 * fence; `fence' non-NULL: holding that), one reference to the caller. */
int drm_syncobj_create(struct drm_device *dev, struct drm_syncobj **out,
		       uint32_t flags, struct drm_fence *fence);
/* A handle of `fp' for the object (it takes a reference of its own). */
int drm_syncobj_get_handle(struct drm_file *fp, struct drm_syncobj *so,
			   uint32_t *handle);
/* A close-on-exec descriptor of the caller for the object. */
int drm_syncobj_get_fd(struct drm_syncobj *so, int *p_fd);

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
/* Is the source rectangle (16.16) inside the framebuffer?  0, or -ENOSPC.
 * (drm_plane.c) */
int drm_framebuffer_check_src_coords(uint32_t src_x, uint32_t src_y,
				     uint32_t src_w, uint32_t src_h,
				     const struct drm_framebuffer *fb);
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

/* ---- connector names and optional properties (drm_connector.c) ----
 *
 * The optional connector properties a driver attaches to the connectors
 * that have what they control.  None is attached unless a driver asks, so
 * a driver that attaches none lists what it always listed. */

/* "Colorspace" values. */
enum drm_colorspace {
	DRM_MODE_COLORIMETRY_DEFAULT = 0, /* the driver chooses */
	DRM_MODE_COLORIMETRY_NO_DATA = 0,
	DRM_MODE_COLORIMETRY_SMPTE_170M_YCC = 1,
	DRM_MODE_COLORIMETRY_BT709_YCC = 2,
	DRM_MODE_COLORIMETRY_XVYCC_601 = 3,
	DRM_MODE_COLORIMETRY_XVYCC_709 = 4,
	DRM_MODE_COLORIMETRY_SYCC_601 = 5,
	DRM_MODE_COLORIMETRY_OPYCC_601 = 6,
	DRM_MODE_COLORIMETRY_OPRGB = 7,
	DRM_MODE_COLORIMETRY_BT2020_CYCC = 8,
	DRM_MODE_COLORIMETRY_BT2020_RGB = 9,
	DRM_MODE_COLORIMETRY_BT2020_YCC = 10,
	DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65 = 11,
	DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER = 12,
	DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED = 13,
	DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT = 14,
	DRM_MODE_COLORIMETRY_BT601_YCC = 15,
	DRM_MODE_COLORIMETRY_COUNT
};

/* "Broadcast RGB" values: the RGB quantisation range sent to the sink. */
enum drm_hdmi_broadcast_rgb {
	DRM_HDMI_BROADCAST_RGB_AUTO, /* as the mode and the sink say */
	DRM_HDMI_BROADCAST_RGB_FULL,
	DRM_HDMI_BROADCAST_RGB_LIMITED,
};

/* "panel orientation": how the panel is mounted (display_info). */
enum drm_panel_orientation {
	DRM_MODE_PANEL_ORIENTATION_UNKNOWN = -1,
	DRM_MODE_PANEL_ORIENTATION_NORMAL = 0,
	DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP,
	DRM_MODE_PANEL_ORIENTATION_LEFT_UP,
	DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

/* Names, for logs and enum properties ("(null)" / NULL when unknown). */
const char *drm_get_connector_type_name(unsigned int type);
const char *drm_get_connector_status_name(int status);
const char *drm_get_subpixel_order_name(int order);
const char *drm_get_dpms_name(int val);
const char *drm_get_colorspace_name(enum drm_colorspace colorspace);
const char *drm_hdmi_connector_get_broadcast_rgb_name(enum drm_hdmi_broadcast_rgb broadcast_rgb);

/* The device-wide connector properties that exist before any driver
 * attaches them (PATH, TILE, HDR_OUTPUT_METADATA); see drm_connector.c
 * for when they are made.  0 or -ENOMEM. */
int drm_connector_create_standard_properties(struct drm_device *dev);
/* Each returns 0 or a negative errno; c->dev must be set (it is, for a
 * connector from drm_connector_add). */
int drm_mode_create_content_type_property(struct drm_device *dev);
int drm_connector_attach_content_type_property(struct drm_connector *c);
int drm_connector_attach_scaling_mode_property(struct drm_connector *c,
					       uint32_t scaling_mode_mask);
int drm_connector_attach_vrr_capable_property(struct drm_connector *c);
void drm_connector_set_vrr_capable_property(struct drm_connector *c, bool capable);
/* "Colorspace" with the given DRM_MODE_COLORIMETRY_* bits (0: all that
 * HDMI / DisplayPort can signal), then attached. */
int drm_mode_create_hdmi_colorspace_property(struct drm_connector *c,
					     uint32_t supported_colorspaces);
int drm_mode_create_dp_colorspace_property(struct drm_connector *c,
					   uint32_t supported_colorspaces);
int drm_connector_attach_colorspace_property(struct drm_connector *c);
int drm_connector_attach_max_bpc_property(struct drm_connector *c, int min, int max);
int drm_connector_attach_hdr_output_metadata_property(struct drm_connector *c);
bool drm_connector_atomic_hdr_metadata_equal(const struct drm_connector_state *old_state,
					     const struct drm_connector_state *new_state);
int drm_connector_attach_broadcast_rgb_property(struct drm_connector *c);
/* The panel's mounting: recorded in display_info and shown as the
 * immutable "panel orientation" property (once; UNKNOWN attaches nothing). */
int drm_connector_set_panel_orientation(struct drm_connector *c,
					enum drm_panel_orientation panel_orientation);
/* PATH (a connector behind a DisplayPort branch) and TILE blobs: attach
 * first, then set (TILE follows the EDID by itself once attached). */
int drm_connector_attach_path_property(struct drm_connector *c);
int drm_connector_attach_tile_property(struct drm_connector *c);
int drm_connector_set_path_property(struct drm_connector *c, const char *path);
int drm_connector_set_tile_property(struct drm_connector *c);
/* The link failed (BAD) or works again (GOOD). */
void drm_connector_set_link_status_property(struct drm_connector *c,
					    uint64_t link_status);

#endif
