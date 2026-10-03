/*
 * Tests for the display-manager core's atomic mode setting
 * (kernel/dev/gpu/drm/drm_atomic.c) and plane handling (drm_plane.c), run
 * over the real property, mode-object, blending, colour and plane-check
 * code: a crtc that DPMS switches off keeps its mode, its planes and its
 * connectors (and takes cursor updates meanwhile), the checks that tie a
 * mode to connectors and planes, the legacy calls translated into states
 * (mode set, switching off, taking a connector from another crtc, DPMS,
 * the cursor and its object check), the cursor hot spot properties with
 * the cursor planes they hide from atomic clients that do not set them,
 * blobs as large as a multi-segment gamma table with a bound per file, the
 * legacy gamma table's own size, link-status (a client may only set a bad
 * link GOOD, which is a full mode set; SETCRTC sets it GOOD; a failed
 * commit gives it back), and the vblank reference an OUT_FENCE_PTR fence
 * holds until it signals.  See test-drm-atomic.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* The host C library's declarations, spelled out: its headers disagree
 * with the kernel's fixed-width types, which come first. */
extern int printf(const char *, ...);
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);
extern void *malloc(unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void free(void *);
extern void abort(void);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
extern int strncmp(const char *, const char *, unsigned long);

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- what the files under test need from the kernel ---------------------- */

uint64_t test_irq_flag = 0x200;
int test_locks_held;
static int warnings;

void test_fatal(const char *what, const char *name)
{
	printf("FATAL: %s (%s)\n", what, name ? name : "?");
	abort();
}

int kprintf(const char *fmt, ...)
{
	char buf[512];
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	if (!strncmp(buf, "WARNING", 7))
		warnings++;
	return n;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	__builtin_va_end(ap);
	return n;
}

void *kalloc(size_t size)
{
	return calloc(1, size ? size : 1);
}

void *kcalloc(size_t count, size_t size)
{
	return calloc(count ? count : 1, size ? size : 1);
}

void kfree(void *ptr)
{
	free(ptr);
}

void mm_memset(void *dest, int val, size_t len)
{
	memset(dest, val, len);
}

void mm_memcpy(void *dest, const void *src, size_t len)
{
	memcpy(dest, src, len);
}

int copy_from_user(void *dst, const void *src, size_t len)
{
	memcpy(dst, src, len);
	return 0;
}

int copy_to_user(void *dst, const void *src, size_t len)
{
	memcpy(dst, src, len);
	return 0;
}

bool validate_user_ptr(uint64_t ptr, size_t len)
{
	(void)len;
	return ptr != 0;
}

long sys_close(uint64_t fd)
{
	(void)fd;
	return 0;
}

/* Fences: only out-fences, counted as they signal. */
static int fences_signaled;

uint64_t drm_fence_context_alloc(unsigned num)
{
	(void)num;
	return 1;
}
struct drm_fence *drm_fence_from_fd(int fd)
{
	(void)fd;
	return NULL;
}
void drm_fence_get(struct drm_fence *f)
{
	(void)f;
}
void drm_fence_put(struct drm_fence *f)
{
	(void)f;
}
void drm_fence_set_error(struct drm_fence *f, int error)
{
	(void)f;
	(void)error;
}
void drm_fence_signal(struct drm_fence *f)
{
	(void)f;
	fences_signaled++;
}
int drm_fence_wait_flags(struct drm_fence *f, uint64_t timeout_ns, int intr)
{
	(void)f;
	(void)timeout_ns;
	(void)intr;
	return 0;
}
void drm_fence_init_unlisted(struct drm_fence *f, struct drm_device *dev,
			     const struct drm_fence_ops *ops, uint64_t context,
			     uint64_t seqno64)
{
	(void)f;
	(void)dev;
	(void)ops;
	(void)context;
	(void)seqno64;
}
/* The descriptor an out-fence gets (an error: none). */
static int sync_fd = -EINVAL;

int drm_sync_file_install(struct drm_fence *f, int *fd_out, int cloexec)
{
	(void)f;
	(void)fd_out;
	(void)cloexec;
	return sync_fd;
}
/* Timers: the last one started, fired by hand. */
static hrtimer_t *timer_started;
static int timer_starts;

void hrtimer_init(hrtimer_t *t, hrtimer_fn_t fn, void *arg)
{
	memset(t, 0, sizeof(*t));
	t->fn = fn;
	t->arg = arg;
}
uint64_t hrtimer_now_ns(void)
{
	return 0;
}
void hrtimer_start(hrtimer_t *t, uint64_t when)
{
	t->expires_ns = when;
	timer_started = t;
	timer_starts++;
}

/* The vblank references: get brings a counter that stood still while the
 * interrupt was off up to date (vblank_catch_up). */
static int vblank_gets, vblank_puts;
static uint64_t vblank_catch_up;

int drm_crtc_vblank_get(struct drm_device *d, int crtc)
{
	vblank_gets++;
	d->vbl[crtc].count += vblank_catch_up;
	vblank_catch_up = 0;
	return 0;
}
void drm_crtc_vblank_put(struct drm_device *d, int crtc)
{
	(void)d;
	(void)crtc;
	vblank_puts++;
}
uint64_t drm_crtc_accurate_vblank_count(struct drm_device *d, int crtc)
{
	return d->vbl[crtc].count;
}

/* Modes: equal when every field is. */
int drm_mode_convert_umode(struct drm_device *dev, struct drm_display_mode *out,
			   const struct drm_mode_modeinfo *in)
{
	(void)dev;
	(void)out;
	(void)in;
	return 0;
}
bool drm_mode_equal(const struct drm_mode_modeinfo *a, const struct drm_mode_modeinfo *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* Objects. */
struct drm_crtc *drm_crtc_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->ncrtc; i++)
		if (dev->crtc[i].id == id)
			return &dev->crtc[i];
	return NULL;
}

struct drm_connector *drm_conn_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nconn; i++)
		if (dev->conn[i].id == id)
			return &dev->conn[i];
	return NULL;
}

struct drm_encoder *drm_enc_find(struct drm_device *dev, uint32_t id)
{
	(void)dev;
	(void)id;
	return NULL;
}

struct drm_framebuffer *drm_fb_lookup(struct drm_device *dev, uint32_t id)
{
	if (!id)
		return NULL;
	for (int i = 0; i < DRM_MAX_FBS; i++)
		if (dev->fbs[i].id == id)
			return &dev->fbs[i];
	return NULL;
}

static uint32_t fb_new(struct drm_device *dev, struct drm_gem_object *o, uint32_t w,
		       uint32_t h, uint32_t format)
{
	for (int i = 0; i < DRM_MAX_FBS; i++) {
		struct drm_framebuffer *fb = &dev->fbs[i];

		if (fb->id)
			continue;
		memset(fb, 0, sizeof(*fb));
		fb->id = drm_mode_id_alloc(dev);
		fb->width = w;
		fb->height = h;
		fb->pitch = w * 4;
		fb->format = format;
		fb->modifier = DRM_FORMAT_MOD_LINEAR;
		fb->obj = o;
		return fb->id;
	}
	return 0;
}

int drm_kms_fb_add_internal(struct drm_device *dev, struct drm_gem_object *o,
			    uint32_t w, uint32_t h, uint32_t pitch,
			    uint32_t format, uint32_t *id_out)
{
	(void)pitch;
	*id_out = fb_new(dev, o, w, h, format);
	return *id_out ? 0 : -ENOSPC;
}

void drm_kms_fb_remove_internal(struct drm_device *dev, uint32_t id)
{
	struct drm_framebuffer *fb = drm_fb_lookup(dev, id);

	if (fb)
		memset(fb, 0, sizeof(*fb));
}

static int flip_events, vbl_syncs;

int drm_kms_queue_flip_event(struct drm_device *dev, struct drm_file *fp,
			     int crtc, uint64_t user_data)
{
	(void)dev;
	(void)fp;
	(void)crtc;
	(void)user_data;
	flip_events++;
	return 0;
}

void drm_kms_vbl_sync(struct drm_device *dev, int crtc)
{
	(void)dev;
	(void)crtc;
	vbl_syncs++;
}

int drm_crtc_set(struct drm_device *dev, struct drm_file *fp,
		 struct drm_crtc *crtc, const struct drm_mode_modeinfo *mode,
		 uint32_t fb_id, int x, int y)
{
	return drm_atomic_legacy_crtc_set(dev, fp, crtc, mode, fb_id, x, y, NULL, 0);
}

void drm_crtc_attach_core_properties(struct drm_device *dev, struct drm_crtc *crtc)
{
	(void)dev;
	crtc->properties.core_attached = true;
}

void drm_plane_attach_core_properties(struct drm_device *dev, struct drm_plane *plane)
{
	(void)dev;
	plane->properties.core_attached = true;
}

void drm_connector_attach_core_properties(struct drm_device *dev,
					  struct drm_connector *conn)
{
	(void)dev;
	conn->properties.core_attached = true;
}

/* ---- the device ----------------------------------------------------------- */

/* What the driver was asked to commit last, and how often. */
static int commits;
static struct {
	int active[2], enabled[2], active_changed[2], modeset[2];
	int prim_fb[2], cur_fb[2];
	int32_t cur_hot_x;
} last;

/* What the next commit returns, and the link status of connector 0 the
 * driver saw while committing. */
static int commit_rc;
static uint64_t commit_saw_link;

static int test_commit(struct drm_device *d, struct drm_atomic_state *s)
{
	commits++;
	commit_saw_link = d->conn[0].link_status;
	if (commit_rc)
		return commit_rc;
	for (uint32_t i = 0; i < d->ncrtc; i++) {
		struct drm_crtc_state *cs = &s->crtcs[i];

		last.active[i] = cs->active;
		last.enabled[i] = cs->mode_valid;
		last.active_changed[i] = cs->active_changed;
		last.modeset[i] = drm_crtc_state_needs_modeset(cs);
		last.prim_fb[i] = s->planes[d->crtc[i].primary_plane_id ?
						    drm_crtc_primary(d, (int)i)->index : 0].fb_id;
		last.cur_fb[i] = s->planes[drm_crtc_cursor(d, (int)i)->index].fb_id;
	}
	last.cur_hot_x = s->planes[drm_crtc_cursor(d, 0)->index].hot_x;
	return 0;
}

static int hook_calls, hook_rc;

static int test_cursor_obj_check(struct drm_device *d, struct drm_gem_object *o,
				 uint32_t w, uint32_t h)
{
	(void)d;
	(void)o;
	(void)w;
	(void)h;
	hook_calls++;
	return hook_rc;
}

static struct drm_driver drv = {
	.name = "test",
	.atomic_commit = test_commit,
	.cursor_w = 64,
	.cursor_h = 64,
	.features = DRM_FEATURE_CURSOR_HOTSPOT,
};

static struct drm_device dev;
static struct drm_gem_object bo_big, bo_small, surface;
static uint32_t fb_a, fb_b;
static const struct drm_mode_modeinfo mode_a = {
	.clock = 65000, .hdisplay = 1024, .hsync_start = 1048, .hsync_end = 1184,
	.htotal = 1344, .vdisplay = 768, .vsync_start = 771, .vsync_end = 777,
	.vtotal = 806, .vrefresh = 60,
};

static uint32_t id_of(struct drm_prop *p)
{
	return p ? p->id : 0;
}

static const struct drm_prop_enum_list dpms_list[] = {
	{ 0, "On" }, { 1, "Standby" }, { 2, "Suspend" }, { 3, "Off" },
};

static const struct drm_prop_enum_list link_status_list[] = {
	{ DRM_MODE_LINK_STATUS_GOOD, "Good" }, { DRM_MODE_LINK_STATUS_BAD, "Bad" },
};

/* The core's properties as drm_kms_init makes them, two crtcs each with
 * its connector, primary and cursor plane, and two framebuffers. */
static void dev_init(void)
{
	memset(&dev, 0, sizeof(dev));
	dev.drv = &drv;
	dev.next_mode_id = 32;
	dev.max_width = dev.max_height = 8192;
	dev.prop_dpms = id_of(drm_property_create_enum(&dev, 0, "DPMS", dpms_list, 4));
	dev.prop_crtc_id = id_of(drm_property_create_object(&dev, DRM_MODE_PROP_ATOMIC,
							    "CRTC_ID", DRM_MODE_OBJECT_CRTC));
	dev.prop_fb_id = id_of(drm_property_create_object(&dev, DRM_MODE_PROP_ATOMIC,
							  "FB_ID", DRM_MODE_OBJECT_FB));
	dev.prop_active = id_of(drm_property_create_bool(&dev, DRM_MODE_PROP_ATOMIC, "ACTIVE"));
	dev.prop_mode_id = id_of(drm_property_create(&dev, DRM_MODE_PROP_BLOB |
						     DRM_MODE_PROP_ATOMIC, "MODE_ID", 0));
	dev.prop_link_status = id_of(drm_property_create_enum(&dev, 0, "link-status",
							      link_status_list, 2));
	dev.prop_out_fence_ptr = id_of(drm_property_create_range(&dev, DRM_MODE_PROP_ATOMIC,
								 "OUT_FENCE_PTR", 0,
								 0xffffffffffffffffULL));
	dev.ncrtc = dev.nconn = dev.nenc = 2;
	for (int i = 0; i < 2; i++) {
		dev.crtc[i].id = drm_mode_id_alloc(&dev);
		dev.crtc[i].index = i;
		dev.conn[i].id = drm_mode_id_alloc(&dev);
		dev.conn[i].dev = &dev;
		dev.enc[i].id = drm_mode_id_alloc(&dev);
		int p = drm_plane_add(&dev, DRM_PLANE_TYPE_PRIMARY, 1u << i, NULL, 0, NULL, 0);
		int c = drm_plane_add(&dev, DRM_PLANE_TYPE_CURSOR, 1u << i, NULL, 0, NULL, 0);
		dev.crtc[i].primary_plane_id = dev.planes[p].id;
		dev.crtc[i].cursor_plane_id = dev.planes[c].id;
		drm_object_attach_property(&dev.conn[i].properties,
					   drm_prop_find(&dev, dev.prop_link_status), 0);
		drm_object_attach_property(&dev.crtc[i].properties,
					   drm_prop_find(&dev, dev.prop_out_fence_ptr), 0);
	}
	memset(&bo_big, 0, sizeof(bo_big));
	bo_big.kind = DRM_GEM_BO;
	bo_big.size = 64 * 64 * 4;
	bo_big.refs = 1;
	memset(&bo_small, 0, sizeof(bo_small));
	bo_small.kind = DRM_GEM_BO;
	bo_small.size = 4096;
	bo_small.refs = 1;
	memset(&surface, 0, sizeof(surface));
	surface.kind = DRM_GEM_SURFACE;
	surface.size = 4096; /* its backing store, smaller than the image */
	surface.refs = 1;
	fb_a = fb_new(&dev, &bo_big, 1024, 768, DRM_FORMAT_XRGB8888);
	fb_b = fb_new(&dev, &bo_big, 1024, 768, DRM_FORMAT_XRGB8888);
	commits = 0;
	hook_calls = 0;
	hook_rc = 0;
	commit_rc = 0;
	drv.cursor_obj_check = NULL;
	drv.enable_vblank = NULL;
	sync_fd = -EINVAL;
	vblank_gets = vblank_puts = 0;
	vblank_catch_up = 0;
}

static struct drm_plane *primary(int crtc)
{
	return drm_crtc_primary(&dev, crtc);
}

static struct drm_plane *cursor(int crtc)
{
	return drm_crtc_cursor(&dev, crtc);
}

static int set_dpms(int conn, int level)
{
	return drm_atomic_legacy_set_property(&dev, NULL, DRM_MODE_OBJECT_CONNECTOR,
					      dev.conn[conn].id, dev.prop_dpms,
					      (uint64_t)level);
}

/* ---- DPMS: active without losing what is shown ------------------------------ */

static void test_dpms(void)
{
	uint32_t blob;
	int rc, n;

	dev_init();
	rc = drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0);
	CHECK(rc == 0, "mode set: %d", rc);
	CHECK(dev.crtc[0].active && dev.crtc[0].enabled, "on and enabled");
	CHECK(dev.conn[0].crtc_id == dev.crtc[0].id && dev.conn[0].dpms == 0,
	      "connector on the crtc, DPMS on: %d", dev.conn[0].dpms);
	CHECK(primary(0)->fb_id == fb_a && dev.crtc[0].fb_id == fb_a, "primary shows fb a");
	blob = dev.crtc[0].mode_blob;
	CHECK(blob != 0, "MODE_ID blob");

	/* the cursor on, so it can be seen to survive */
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				      &bo_big, 64, 64, 3, 4, 0, 0);
	CHECK(rc == 0 && cursor(0)->fb_id && cursor(0)->hot_x == 3, "cursor set: %d", rc);

	/* off */
	n = commits;
	rc = set_dpms(0, 3);
	CHECK(rc == 0, "DPMS off: %d", rc);
	CHECK(commits == n + 1, "one commit");
	CHECK(!last.active[0] && last.enabled[0] && last.active_changed[0],
	      "the driver sees an enabled crtc going inactive");
	CHECK(last.prim_fb[0] == (int)fb_a && last.cur_fb[0], "with its planes");
	CHECK(!dev.crtc[0].active && dev.crtc[0].enabled, "committed: inactive, enabled");
	CHECK(primary(0)->crtc == 0 && primary(0)->fb_id == fb_a && dev.crtc[0].fb_id == fb_a,
	      "the primary keeps its framebuffer");
	CHECK(cursor(0)->crtc == 0 && cursor(0)->fb_id, "the cursor stays");
	CHECK(dev.conn[0].crtc_id == dev.crtc[0].id, "the connector stays");
	CHECK(dev.conn[0].dpms == 3, "DPMS reads off: %d", dev.conn[0].dpms);
	CHECK(dev.crtc[0].mode_blob == blob, "MODE_ID unchanged: %u vs %u",
	      dev.crtc[0].mode_blob, blob);

	/* a blanked screen still takes cursor updates (X moves it) */
	n = commits;
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_MOVE,
				      NULL, 0, 0, 0, 0, 10, 20);
	CHECK(rc == 0 && commits == n + 1, "cursor move while off: %d", rc);
	CHECK(cursor(0)->crtc_x == 10 && cursor(0)->crtc_y == 20, "moved");
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				      &bo_big, 64, 64, 5, 6, 0, 0);
	CHECK(rc == 0, "cursor image while off: %d", rc);
	/* ...and plane updates */
	rc = drm_atomic_legacy_set_plane(&dev, NULL, primary(0), dev.crtc[0].id, fb_b,
					 0, 0, 1024, 768, 0, 0, 1024 << 16, 768 << 16);
	CHECK(rc == 0 && primary(0)->fb_id == fb_b, "flip while off: %d", rc);
	/* but no flip event: nothing would send it */
	rc = drm_atomic_legacy_page_flip(&dev, NULL, &dev.crtc[0], drm_fb_lookup(&dev, fb_a), 1, 0);
	CHECK(rc == -EINVAL, "page flip on an inactive crtc: %d", rc);

	/* standby is off too: nothing to do */
	n = commits;
	rc = set_dpms(0, 1);
	CHECK(rc == 0 && commits == n, "standby when off: no commit (%d)", rc);
	CHECK(dev.conn[0].dpms == 3, "still reads off: %d", dev.conn[0].dpms);

	/* on again: a full mode set with the planes as they are now */
	n = commits;
	rc = set_dpms(0, 0);
	CHECK(rc == 0 && commits == n + 1, "DPMS on: %d", rc);
	CHECK(last.active[0] && last.modeset[0], "the driver sees a mode set");
	CHECK(last.prim_fb[0] == (int)fb_b && last.cur_fb[0] && last.cur_hot_x == 5,
	      "of the current planes");
	CHECK(dev.crtc[0].active && dev.conn[0].dpms == 0, "on, DPMS reads on");
	CHECK(dev.crtc[0].mode_blob == blob, "MODE_ID still the same blob");

	/* on when on: nothing */
	n = commits;
	CHECK(set_dpms(0, 0) == 0 && commits == n, "on when on: no commit");
}

/* ---- enabled, connectors and planes --------------------------------------- */

static void test_enable_rules(void)
{
	struct drm_atomic_state *st;
	int rc;

	dev_init();
	CHECK(drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0) == 0,
	      "mode set");

	/* no mode, the connector left on: refused */
	st = drm_atomic_state_alloc(&dev, NULL, 0);
	st->allow_modeset = 1;
	CHECK(drm_atomic_crtc_set(st, &dev.crtc[0], dev.prop_active, 0) == 0, "ACTIVE 0");
	CHECK(drm_atomic_crtc_set(st, &dev.crtc[0], dev.prop_mode_id, 0) == 0, "MODE_ID 0");
	CHECK(drm_atomic_plane_set(st, primary(0), dev.prop_fb_id, 0) == 0 &&
	      drm_atomic_plane_set(st, primary(0), dev.prop_crtc_id, 0) == 0, "primary off");
	CHECK(drm_atomic_plane_set(st, cursor(0), dev.prop_fb_id, 0) == 0 &&
	      drm_atomic_plane_set(st, cursor(0), dev.prop_crtc_id, 0) == 0, "cursor off");
	rc = drm_atomic_check(st);
	CHECK(rc == -EINVAL, "a connector on a crtc without a mode: %d", rc);
	/* the connector off as well: fine */
	CHECK(drm_atomic_conn_set(st, &dev.conn[0], dev.prop_crtc_id, 0) == 0, "connector off");
	rc = drm_atomic_check(st);
	CHECK(rc == 0, "all of it off: %d", rc);
	if (rc == 0)
		rc = drm_atomic_commit(st);
	drm_atomic_state_free(st);
	CHECK(rc == 0 && !dev.crtc[0].enabled && !dev.crtc[0].active && !dev.crtc[0].mode_blob,
	      "off and no MODE_ID: %d", rc);
	CHECK(dev.conn[0].dpms == 3, "the connector reads off: %d", dev.conn[0].dpms);

	/* a plane on a crtc without a mode: refused */
	st = drm_atomic_state_alloc(&dev, NULL, 0);
	st->allow_modeset = 1;
	CHECK(drm_atomic_plane_set(st, primary(0), dev.prop_fb_id, fb_a) == 0 &&
	      drm_atomic_plane_set(st, primary(0), dev.prop_crtc_id, dev.crtc[0].id) == 0,
	      "primary on");
	rc = drm_atomic_check(st);
	CHECK(rc == -EINVAL, "a plane on a crtc without a mode: %d", rc);
	drm_atomic_state_free(st);

	/* a mode and no connector: refused */
	CHECK(drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0) == 0,
	      "mode set again");
	st = drm_atomic_state_alloc(&dev, NULL, 0);
	st->allow_modeset = 1;
	CHECK(drm_atomic_conn_set(st, &dev.conn[0], dev.prop_crtc_id, 0) == 0, "connector off");
	CHECK(drm_atomic_crtc_set(st, &dev.crtc[0], dev.prop_active, 0) == 0, "ACTIVE 0");
	rc = drm_atomic_check(st);
	CHECK(rc == -EINVAL, "an enabled crtc without a connector: %d", rc);
	drm_atomic_state_free(st);

	/* active without a mode: refused */
	st = drm_atomic_state_alloc(&dev, NULL, 0);
	st->allow_modeset = 1;
	CHECK(drm_atomic_crtc_set(st, &dev.crtc[0], dev.prop_mode_id, 0) == 0, "MODE_ID 0");
	rc = drm_atomic_check(st);
	CHECK(rc == -EINVAL, "active without a mode: %d", rc);
	drm_atomic_state_free(st);

	/* the legacy switch-off takes mode, planes and connectors */
	rc = drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], NULL, 0, 0, 0, NULL, 0);
	CHECK(rc == 0, "SETCRTC off: %d", rc);
	CHECK(!dev.crtc[0].enabled && !dev.crtc[0].active && !dev.crtc[0].mode_blob,
	      "crtc off, no mode");
	CHECK(primary(0)->crtc < 0 && !primary(0)->fb_id && !dev.conn[0].crtc_id,
	      "planes and connector detached");
}

/* A legacy mode set naming a connector another crtc drives: that crtc,
 * left with none, is switched off rather than refused. */
static void test_connector_steal(void)
{
	uint32_t ids[1];
	int rc;

	dev_init();
	CHECK(drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0) == 0,
	      "crtc 0 on connector 0");
	ids[0] = dev.conn[0].id;
	rc = drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[1], &mode_a, fb_b, 0, 0, ids, 1);
	CHECK(rc == 0, "crtc 1 takes connector 0: %d", rc);
	CHECK(dev.crtc[1].active && dev.conn[0].crtc_id == dev.crtc[1].id, "crtc 1 drives it");
	CHECK(!dev.crtc[0].enabled && !dev.crtc[0].active && primary(0)->crtc < 0,
	      "crtc 0 switched off with its planes");
}

/* ---- the legacy cursor's object --------------------------------------------- */

static void test_cursor_obj(void)
{
	int rc;

	dev_init();
	CHECK(drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0) == 0,
	      "mode set");
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				      &bo_small, 64, 64, 0, 0, 0, 0);
	CHECK(rc == -EINVAL, "a buffer too small for the image: %d", rc);
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				      &bo_small, 32, 32, 0, 0, 0, 0);
	CHECK(rc == 0, "a buffer that holds it: %d", rc);
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				      &surface, 64, 64, 0, 0, 0, 0);
	CHECK(rc == (DRM_CURSOR_OBJ_CHECK ? 0 : -EINVAL),
	      "a surface whose backing store is smaller than the image: %d", rc);
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				      &surface, 0, 64, 0, 0, 0, 0);
	CHECK(rc == -EINVAL, "no size: %d", rc);

	/* the driver's say */
	drv.cursor_obj_check = test_cursor_obj_check;
	hook_rc = -ENOSPC;
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				      &bo_big, 64, 64, 0, 0, 0, 0);
	CHECK(rc == (DRM_CURSOR_OBJ_CHECK ? -ENOSPC : 0) &&
	      hook_calls == (DRM_CURSOR_OBJ_CHECK ? 1 : 0), "the hook refuses: %d", rc);
	hook_rc = 0;
	rc = drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				      &bo_small, 64, 64, 0, 0, 0, 0);
	CHECK(rc == (DRM_CURSOR_OBJ_CHECK ? 0 : -EINVAL), "the hook accepts: %d", rc);
	drv.cursor_obj_check = NULL;
}

/* ---- the cursor hot spot ----------------------------------------------------- */

static void test_hotspot(void)
{
	struct drm_atomic_state *st;
	struct drm_file fp;
	struct drm_mode_get_plane_res res;
	uint32_t ids[DRM_MAX_PLANES];
	uint64_t v;
	int rc, ncur;

	dev_init();
	if (!DRM_CURSOR_HOTSPOT) {
		CHECK(!cursor(0)->hotspot_x_property, "switched off: no HOTSPOT_X");
		return;
	}
	CHECK(cursor(0)->hotspot_x_property && cursor(0)->hotspot_y_property,
	      "cursor planes carry HOTSPOT_X/Y");
	CHECK(cursor(0)->hotspot_x_property == cursor(1)->hotspot_x_property,
	      "one property shared by the cursor planes");
	CHECK(!primary(0)->hotspot_x_property, "a primary plane does not");
	CHECK(drm_object_property_get_value(&cursor(1)->properties,
					    cursor(1)->hotspot_y_property, &v) == 0 && v == 0,
	      "attached, 0");
	CHECK(cursor(0)->hotspot_x_property->flags ==
	      (DRM_MODE_PROP_SIGNED_RANGE | 0), "a signed range, not atomic-only");

	CHECK(drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0) == 0,
	      "mode set");
	CHECK(drm_atomic_legacy_cursor(&dev, NULL, &dev.crtc[0], DRM_MODE_CURSOR_BO,
				       &bo_big, 64, 64, 0, 0, 0, 0) == 0, "cursor");
	st = drm_atomic_state_alloc(&dev, NULL, 0);
	rc = drm_atomic_plane_set(st, cursor(0), cursor(0)->hotspot_x_property->id,
				  (uint64_t)(int64_t)-7);
	CHECK(rc == 0 && st->planes[cursor(0)->index].hot_x == -7, "HOTSPOT_X written: %d", rc);
	rc = drm_atomic_plane_set(st, cursor(0), cursor(0)->hotspot_y_property->id, 9);
	CHECK(rc == 0, "HOTSPOT_Y written: %d", rc);
	rc = drm_atomic_plane_set(st, primary(0), cursor(0)->hotspot_x_property->id, 1);
	CHECK(rc == -EINVAL, "not on a primary plane: %d", rc);
	st->planes[primary(0)->index].changed = 0;
	rc = drm_atomic_check(st);
	if (rc == 0)
		rc = drm_atomic_commit(st);
	drm_atomic_state_free(st);
	CHECK(rc == 0 && cursor(0)->hot_x == -7 && cursor(0)->hot_y == 9 && last.cur_hot_x == -7,
	      "committed: %d", rc);
	CHECK(drm_object_property_get_value(&cursor(0)->properties,
					    cursor(0)->hotspot_x_property, &v) == 0 &&
	      (int64_t)v == -7, "and listed");

	/* GETPLANERESOURCES */
	memset(&fp, 0, sizeof(fp));
	fp.dev = &dev;
	fp.client_caps = 1ULL << DRM_CLIENT_CAP_UNIVERSAL_PLANES;
	memset(&res, 0, sizeof(res));
	res.plane_id_ptr = (uint64_t)(uintptr_t)ids;
	res.count_planes = DRM_MAX_PLANES;
	CHECK(drm_mode_getplane_res(&dev, &res, &fp) == 0 && res.count_planes == 4,
	      "a universal-planes client sees all four: %u", res.count_planes);
	fp.client_caps |= 1ULL << DRM_CLIENT_CAP_ATOMIC;
	res.count_planes = DRM_MAX_PLANES;
	CHECK(drm_mode_getplane_res(&dev, &res, &fp) == 0, "atomic client");
	ncur = 0;
	for (uint32_t i = 0; i < res.count_planes; i++)
		if (drm_plane_find(&dev, ids[i])->type == DRM_PLANE_TYPE_CURSOR)
			ncur++;
	CHECK(res.count_planes == (DRM_CURSOR_HOTSPOT ? 2u : 4u) &&
	      ncur == (DRM_CURSOR_HOTSPOT ? 0 : 2),
	      "an atomic client without the capability sees no cursor plane: %u", res.count_planes);
	fp.client_caps |= 1ULL << DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT;
	res.count_planes = DRM_MAX_PLANES;
	CHECK(drm_mode_getplane_res(&dev, &res, &fp) == 0 && res.count_planes == 4,
	      "with it, all four: %u", res.count_planes);

	/* a driver without the feature: no properties, nothing hidden */
	drv.features = 0;
	dev_init();
	CHECK(!cursor(0)->hotspot_x_property, "no HOTSPOT_X without the feature");
	fp.dev = &dev;
	fp.client_caps = (1ULL << DRM_CLIENT_CAP_UNIVERSAL_PLANES) | (1ULL << DRM_CLIENT_CAP_ATOMIC);
	res.count_planes = DRM_MAX_PLANES;
	CHECK(drm_mode_getplane_res(&dev, &res, &fp) == 0 && res.count_planes == 4,
	      "all four: %u", res.count_planes);
	drv.features = DRM_FEATURE_CURSOR_HOTSPOT;
}

/* ---- blobs: the largest lookup table, and a bound per file ------------------ */

static void test_blobs(void)
{
	struct drm_mode_create_blob cb;
	struct drm_mode_destroy_blob db;
	struct drm_file fp;
	static uint8_t data[DRM_PROPERTY_BLOB_MAX_LENGTH + 1];
	uint32_t first = 0;
	int rc, made = 0;

	dev_init();
	memset(&fp, 0, sizeof(fp));
	fp.dev = &dev;
	memset(&cb, 0, sizeof(cb));
	cb.data = (uint64_t)(uintptr_t)data;
	/* 262145 entries of struct drm_color_lut */
	cb.length = 262145 * 8;
	rc = drm_mode_createblob_ioctl(&dev, &cb, &fp);
	CHECK(rc == (DRM_PROPERTY_BLOB_LARGE ? 0 : -EINVAL),
	      "a multi-segment gamma table: %d", rc);
#if !DRM_PROPERTY_BLOB_LARGE
	(void)first;
	(void)made;
	(void)db;
	return;
#else
	first = cb.blob_id;
	made++;
	cb.length = DRM_PROPERTY_BLOB_MAX_LENGTH + 1;
	CHECK(drm_mode_createblob_ioctl(&dev, &cb, &fp) == -EINVAL, "one byte too many");
	/* the bound per file */
	cb.length = 2 * 1024 * 1024;
	for (;;) {
		rc = drm_mode_createblob_ioctl(&dev, &cb, &fp);
		if (rc)
			break;
		made++;
	}
	CHECK(rc == -ENOSPC && fp.blob_bytes <= DRM_PROPERTY_BLOB_FILE_MAX && made >= 15,
	      "bounded at %d blobs, %llu bytes: %d", made,
	      (unsigned long long)fp.blob_bytes, rc);
	memset(&db, 0, sizeof(db));
	db.blob_id = first;
	CHECK(drm_mode_destroyblob_ioctl(&dev, &db, &fp) == 0, "destroy one");
	CHECK(drm_mode_createblob_ioctl(&dev, &cb, &fp) == 0, "room again");
	drm_property_destroy_user_blobs(&dev, &fp);
	CHECK(fp.blob_bytes == 0, "nothing owned after close: %llu",
	      (unsigned long long)fp.blob_bytes);
#endif
}

/* ---- the legacy gamma table's own size ------------------------------------ */

static void test_gamma_size(void)
{
	struct drm_crtc c;
	int w = warnings;

	memset(&c, 0, sizeof(c));
	CHECK(drm_crtc_legacy_gamma_size(&dev, &c) == 256, "256 by default");
	CHECK(drm_mode_crtc_set_gamma_size(&c, 129) == 0 && c.gamma_size == 129,
	      "a 129-entry table");
	CHECK(c.gamma[0][0] == 0 && c.gamma[1][128] == 0xffff && c.gamma[2][64] == 0x7fff,
	      "a linear ramp: %x %x %x", c.gamma[0][0], c.gamma[1][128], c.gamma[2][64]);
	CHECK(drm_crtc_legacy_gamma_size(&dev, &c) == (DRM_COLOR_LEGACY_GAMMA_SPLIT ? 129u : 256u),
	      "what the legacy calls see");
	CHECK(drm_mode_crtc_set_gamma_size(&c, 257) == -EINVAL && warnings > w, "at most 256");
	CHECK(drm_mode_crtc_set_gamma_size(&c, 256) == 0 && c.gamma[0][255] == 0xffff &&
	      c.gamma[0][1] == 0x0101, "256: i << 8 | i");
}

/* ---- link-status ------------------------------------------------------------- */

/* One link-status write to connector 0 in a request of its own; the
 * check's answer, and the commit's when the check passes and `commit'. */
static int link_request(uint64_t val, int allow_modeset, int commit)
{
	struct drm_atomic_state *st = drm_atomic_state_alloc(&dev, NULL, 0);
	struct drm_prop *ls = drm_prop_find(&dev, dev.prop_link_status);
	int rc;

	st->allow_modeset = allow_modeset;
	rc = drm_atomic_set_property(st, DRM_MODE_OBJECT_CONNECTOR, &dev.conn[0], ls, val);
	if (rc == 0)
		rc = drm_atomic_check(st);
	if (rc == 0 && commit)
		rc = drm_atomic_commit(st);
	drm_atomic_state_free(st);
	return rc;
}

static void test_link_status(void)
{
	int rc;

	dev_init();
	CHECK(drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0) == 0,
	      "mode set");

	/* a good link: a client's BAD is accepted and changes nothing */
	rc = link_request(DRM_MODE_LINK_STATUS_BAD, 0, 1);
	CHECK(rc == 0 && dev.conn[0].link_status == DRM_MODE_LINK_STATUS_GOOD && !last.modeset[0],
	      "BAD on a good link: %d, link %llu", rc,
	      (unsigned long long)dev.conn[0].link_status);
#if DRM_LINK_STATUS_SEMANTICS
	/* the driver finds the link bad */
	dev.conn[0].link_status = DRM_MODE_LINK_STATUS_BAD;
	/* BAD written back (a state restored): nothing to do */
	rc = link_request(DRM_MODE_LINK_STATUS_BAD, 0, 1);
	CHECK(rc == 0 && !last.modeset[0] &&
	      dev.conn[0].link_status == DRM_MODE_LINK_STATUS_BAD, "BAD on a bad link: %d", rc);
	/* GOOD is training it again, a mode set */
	rc = link_request(DRM_MODE_LINK_STATUS_GOOD, 0, 0);
	CHECK(rc == -EINVAL, "GOOD without ALLOW_MODESET: %d", rc);
	CHECK(dev.conn[0].link_status == DRM_MODE_LINK_STATUS_BAD, "still bad after a refusal");
	rc = link_request(DRM_MODE_LINK_STATUS_GOOD, 1, 1);
	CHECK(rc == 0 && last.modeset[0], "GOOD with ALLOW_MODESET: a mode set (%d)", rc);
	CHECK(commit_saw_link == DRM_MODE_LINK_STATUS_GOOD, "the driver commits a good link");
	{
		struct drm_prop *ls = drm_prop_find(&dev, dev.prop_link_status);
		uint64_t v = 1;

		CHECK(dev.conn[0].link_status == DRM_MODE_LINK_STATUS_GOOD &&
		      drm_object_property_get_value(&dev.conn[0].properties, ls, &v) == 0 &&
		      v == DRM_MODE_LINK_STATUS_GOOD, "link-status reads GOOD");
	}

	/* a commit that fails gives the bad link back */
	dev.conn[0].link_status = DRM_MODE_LINK_STATUS_BAD;
	commit_rc = -EIO;
	rc = link_request(DRM_MODE_LINK_STATUS_GOOD, 1, 1);
	commit_rc = 0;
	CHECK(rc == -EIO && commit_saw_link == DRM_MODE_LINK_STATUS_GOOD &&
	      dev.conn[0].link_status == DRM_MODE_LINK_STATUS_BAD,
	      "a failed commit leaves the link bad: %d", rc);

	/* the legacy SETCRTC sets it GOOD, as a full mode set */
	rc = drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0);
	CHECK(rc == 0 && last.modeset[0] && dev.conn[0].link_status == DRM_MODE_LINK_STATUS_GOOD,
	      "SETCRTC trains a bad link: %d", rc);
	/* the same SETCRTC on a good link is no mode set */
	rc = drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0);
	CHECK(rc == 0 && !last.modeset[0], "SETCRTC on a good link: %d", rc);
#else
	dev.conn[0].link_status = DRM_MODE_LINK_STATUS_BAD;
	rc = link_request(DRM_MODE_LINK_STATUS_GOOD, 0, 1);
	CHECK(rc == 0 && !last.modeset[0] && dev.conn[0].link_status == DRM_MODE_LINK_STATUS_BAD,
	      "link-status writes change nothing: %d", rc);
#endif
}

/* ---- the out-fence's vblank reference ------------------------------------------ */

static int test_enable_vblank(struct drm_device *d, int crtc)
{
	(void)d;
	(void)crtc;
	return 0;
}

/* An ATOMIC request on crtc 0 with nothing but OUT_FENCE_PTR. */
static int out_fence_request(int32_t *out)
{
	uint32_t objs[1] = { dev.crtc[0].id };
	uint32_t counts[1] = { 1 };
	uint32_t props[1] = { dev.prop_out_fence_ptr };
	uint64_t vals[1] = { (uint64_t)(uintptr_t)out };
	struct drm_mode_atomic a;

	memset(&a, 0, sizeof(a));
	a.count_objs = 1;
	a.objs_ptr = (uint64_t)(uintptr_t)objs;
	a.count_props_ptr = (uint64_t)(uintptr_t)counts;
	a.props_ptr = (uint64_t)(uintptr_t)props;
	a.prop_values_ptr = (uint64_t)(uintptr_t)vals;
	return (int)drm_atomic_ioctl(&dev, NULL, &a);
}

static void test_out_fence(void)
{
	int32_t out = 0;
	hrtimer_t *t;
	int rc, sig;

	dev_init();
	CHECK(drm_atomic_legacy_crtc_set(&dev, NULL, &dev.crtc[0], &mode_a, fb_a, 0, 0, NULL, 0) == 0,
	      "mode set");
	sync_fd = 5;

	/* a driver that switches its vblank interrupt: the counter stood
	 * at 10 while it was off, 100 vblanks really passed */
	drv.enable_vblank = test_enable_vblank;
	dev.vbl[0].count = 10;
	vblank_catch_up = 90;
	timer_started = NULL;
	sig = fences_signaled;
	rc = out_fence_request(&out);
	t = timer_started;
	CHECK(rc == 0 && out == 5 && t, "OUT_FENCE_PTR: %d, fd %d", rc, out);
#if DRM_OUT_FENCE_VBLANK_REF
	CHECK(vblank_gets == 1 && vblank_puts == 0 && dev.vbl[0].count == 100,
	      "the armed fence holds a vblank reference (%d gets, %d puts)", vblank_gets,
	      vblank_puts);
	if (t) {
		t->fn(t);
		CHECK(fences_signaled == sig && vblank_puts == 0,
		      "not signalled before the vblank after the up-to-date count");
		dev.vbl[0].count = 101;
		t->fn(t);
		CHECK(fences_signaled == sig + 1 && vblank_puts == 1,
		      "signalled at that vblank, reference dropped (%d puts)", vblank_puts);
	}
#else
	CHECK(vblank_gets == 0, "no vblank reference");
#endif

	/* a driver without the switch: no reference, the count as it is */
	drv.enable_vblank = NULL;
	vblank_gets = vblank_puts = 0;
	timer_started = NULL;
	sig = fences_signaled;
	rc = out_fence_request(&out);
	t = timer_started;
	CHECK(rc == 0 && t && vblank_gets == 0, "OUT_FENCE_PTR without enable_vblank: %d", rc);
	if (t) {
		dev.vbl[0].count++;
		t->fn(t);
		CHECK(fences_signaled == sig + 1 && vblank_puts == 0,
		      "signalled at the next count");
	}
}

int main(void)
{
	test_dpms();
	test_enable_rules();
	test_connector_steal();
	test_cursor_obj();
	test_hotspot();
	test_blobs();
	test_gamma_size();
	test_link_status();
	test_out_fence();
	printf("drm_atomic: %d checks, %d failed, %d warnings logged\n", checks, fails,
	       warnings);
	return fails ? 1 : 0;
}
