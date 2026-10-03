/*
 * Tests for the display-manager core's plane blending and colour
 * management helpers (kernel/dev/gpu/drm/drm_blend.c, drm_color_mgmt.c),
 * run over the real property table (drm_property.c, drm_mode_object.c):
 * rotation simplification against a model of the transforms, zpos
 * normalisation, the plane properties and their sharing, attaching the
 * crtc's colour pipeline, the older gamma call as a blob, lookup table
 * checks, and the integer conversions (table entries, CTM coefficients),
 * plus the table-loading helpers.  See test-drm-blend.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
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
extern int strcmp(const char *, const char *);
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

void *kalloc(size_t size)
{
	return malloc(size ? size : 1);
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

struct drm_crtc *drm_crtc_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->ncrtc; i++)
		if (dev->crtc[i].id == id)
			return &dev->crtc[i];
	return NULL;
}

struct drm_connector *drm_conn_find(struct drm_device *dev, uint32_t id)
{
	(void)dev;
	(void)id;
	return NULL;
}

struct drm_encoder *drm_enc_find(struct drm_device *dev, uint32_t id)
{
	(void)dev;
	(void)id;
	return NULL;
}

struct drm_plane *drm_plane_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nplanes; i++)
		if (dev->planes[i].id == id)
			return &dev->planes[i];
	return NULL;
}

struct drm_framebuffer *drm_fb_lookup(struct drm_device *dev, uint32_t id)
{
	(void)dev;
	(void)id;
	return NULL;
}

struct drm_crtc *drm_plane_crtc(struct drm_device *dev, uint32_t plane_id,
				int *is_cursor)
{
	(void)dev;
	(void)plane_id;
	*is_cursor = 0;
	return NULL;
}

int drm_atomic_legacy_set_property(struct drm_device *dev, struct drm_file *fp,
				   uint32_t obj_type, uint32_t obj_id,
				   uint32_t prop, uint64_t val)
{
	(void)dev;
	(void)fp;
	(void)obj_type;
	(void)obj_id;
	(void)prop;
	(void)val;
	return -EINVAL;
}

/* The core's crtc properties as drm_kms.c attaches them: GAMMA_LUT and its
 * size where the device has them. */
void drm_crtc_attach_core_properties(struct drm_device *dev, struct drm_crtc *crtc)
{
	struct drm_object_properties *props = &crtc->properties;

	if (props->core_attached)
		return;
	props->core_attached = true;
	if (dev->prop_gamma_lut) {
		drm_object_attach_property(props, drm_prop_find(dev, dev->prop_gamma_lut), 0);
		drm_object_attach_property(props, drm_prop_find(dev, dev->prop_gamma_lut_size),
					   dev->drv->gamma_size);
	}
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

static int dummy_commit(struct drm_device *dev, struct drm_atomic_state *st)
{
	(void)dev;
	(void)st;
	return 0;
}

static struct drm_driver drv = {
	.name = "test",
	.atomic_commit = dummy_commit,
	.gamma_size = 256,
	.features = DRM_FEATURE_BLEND | DRM_FEATURE_COLOR_MGMT,
};

static struct drm_device dev;
static struct drm_atomic_state st;

static uint32_t id_of(struct drm_prop *p)
{
	return p ? p->id : 0;
}

/* What drm_kms_init makes that these helpers rely on. */
static void dev_init(void)
{
	memset(&dev, 0, sizeof(dev));
	dev.drv = &drv;
	dev.next_mode_id = 1; /* as drm_dev_register starts it */
	dev.prop_crtc_id = id_of(drm_property_create_object(&dev, DRM_MODE_PROP_ATOMIC,
							    "CRTC_ID", DRM_MODE_OBJECT_CRTC));
	dev.prop_gamma_lut = id_of(drm_property_create(&dev, DRM_MODE_PROP_BLOB |
						       DRM_MODE_PROP_ATOMIC, "GAMMA_LUT", 0));
	dev.prop_gamma_lut_size = id_of(drm_property_create_range(&dev, DRM_MODE_PROP_IMMUTABLE |
								  DRM_MODE_PROP_ATOMIC,
								  "GAMMA_LUT_SIZE", 256, 256));
	dev.prop_degamma_lut = id_of(drm_property_create(&dev, DRM_MODE_PROP_BLOB,
							 "DEGAMMA_LUT", 0));
	dev.prop_degamma_lut_size = id_of(drm_property_create_range(&dev, DRM_MODE_PROP_IMMUTABLE,
								    "DEGAMMA_LUT_SIZE", 0,
								    0xffffffffu));
	dev.prop_ctm = id_of(drm_property_create(&dev, DRM_MODE_PROP_BLOB, "CTM", 0));
	dev.ncrtc = 3;
	for (int i = 0; i < 3; i++) {
		dev.crtc[i].id = drm_mode_id_alloc(&dev);
		dev.crtc[i].index = i;
	}
	dev.nplanes = 6;
	for (int i = 0; i < 6; i++) {
		struct drm_plane *p = &dev.planes[i];

		p->id = drm_mode_id_alloc(&dev);
		p->index = i;
		p->crtc = -1;
		/* drm_plane_add's defaults */
		p->rotation = DRM_MODE_ROTATE_0;
		p->alpha = 0xffff;
	}
}

/* ---- rotation ---------------------------------------------------------------
 *
 * A model: the transform as a 2x2 integer matrix acting on the source's
 * axes -- the reflections first, then the counter-clockwise rotation. */

struct mat {
	int a, b, c, d;
};

static struct mat mul(struct mat l, struct mat r)
{
	struct mat m = { l.a * r.a + l.b * r.c, l.a * r.b + l.b * r.d,
			 l.c * r.a + l.d * r.c, l.c * r.b + l.d * r.d };
	return m;
}

static struct mat transform_of(unsigned int rotation)
{
	struct mat m = { 1, 0, 0, 1 };
	struct mat rot90 = { 0, -1, 1, 0 };

	if (rotation & DRM_MODE_REFLECT_X)
		m = mul((struct mat){ -1, 0, 0, 1 }, m);
	if (rotation & DRM_MODE_REFLECT_Y)
		m = mul((struct mat){ 1, 0, 0, -1 }, m);
	for (int k = 0; k < 4; k++)
		if (rotation & (DRM_MODE_ROTATE_0 << k))
			for (int j = 0; j < k; j++)
				m = mul(rot90, m);
	return m;
}

static int mat_eq(struct mat x, struct mat y)
{
	return x.a == y.a && x.b == y.b && x.c == y.c && x.d == y.d;
}

static void test_rotation(void)
{
	const unsigned int all = DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK;
	static const struct {
		unsigned int in, supported, out;
	} v[] = {
		{ DRM_MODE_ROTATE_0, DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK, DRM_MODE_ROTATE_0 },
		{ DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X,
		  DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_Y,
		  DRM_MODE_ROTATE_180 | DRM_MODE_REFLECT_Y },
		{ DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_Y,
		  DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_X,
		  DRM_MODE_ROTATE_270 | DRM_MODE_REFLECT_X },
		{ DRM_MODE_ROTATE_180, DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_MASK,
		  DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y },
		{ DRM_MODE_ROTATE_270, DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_MASK,
		  DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y },
		{ DRM_MODE_ROTATE_180 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y,
		  DRM_MODE_ROTATE_0, DRM_MODE_ROTATE_0 },
		/* already supported: untouched even when another form exists */
		{ DRM_MODE_ROTATE_180, all, DRM_MODE_ROTATE_180 },
	};

	for (unsigned int i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		unsigned int r = drm_rotation_simplify(v[i].in, v[i].supported);

		CHECK(r == v[i].out, "simplify(%#x, %#x) = %#x, want %#x",
		      v[i].in, v[i].supported, r, v[i].out);
	}

	/* Whatever it does, the picture stays the same: every rotation with
	 * every reflection, against every subset of supported bits. */
	for (unsigned int rot = 0; rot < 4; rot++) {
		for (unsigned int refl = 0; refl < 4; refl++) {
			unsigned int in = (DRM_MODE_ROTATE_0 << rot) | (refl << 4);

			for (unsigned int sup = 0; sup < 64; sup++) {
				unsigned int out = drm_rotation_simplify(in, sup);

				CHECK(mat_eq(transform_of(in), transform_of(out)),
				      "simplify(%#x, %#x) = %#x changes the picture", in, sup, out);
				CHECK(is_power_of_2(out & DRM_MODE_ROTATE_MASK),
				      "simplify(%#x, %#x) = %#x has no single rotation", in, sup, out);
			}
		}
	}

	CHECK(!drm_rotation_90_or_270(DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X), "0 is not 90/270");
	CHECK(drm_rotation_90_or_270(DRM_MODE_ROTATE_90), "90");
	CHECK(drm_rotation_90_or_270(DRM_MODE_ROTATE_270 | DRM_MODE_REFLECT_Y), "270");
	CHECK(!drm_rotation_90_or_270(DRM_MODE_ROTATE_180), "180 is not 90/270");
}

/* ---- zpos ---------------------------------------------------------------------- */

static void state_from_committed(void)
{
	memset(&st, 0, sizeof(st));
	st.dev = &dev;
	for (uint32_t i = 0; i < dev.nplanes; i++) {
		st.planes[i].plane = &dev.planes[i];
		st.planes[i].crtc = dev.planes[i].crtc;
		st.planes[i].zpos = dev.planes[i].zpos;
		st.planes[i].normalized_zpos = dev.planes[i].normalized_zpos;
	}
}

static void commit_state(void)
{
	for (uint32_t i = 0; i < dev.nplanes; i++) {
		dev.planes[i].crtc = st.planes[i].crtc;
		dev.planes[i].zpos = st.planes[i].zpos;
		dev.planes[i].normalized_zpos = st.planes[i].normalized_zpos;
	}
}

static void test_zpos(void)
{
	uint32_t changed = 0xdead;
	int rc;

	dev_init();
	state_from_committed();
	/* crtc 0: planes 0 (zpos 2), 1 (zpos 0), 3 (zpos 2, same as plane 0);
	 * crtc 1: plane 2 (zpos 7), plane 4 (zpos 1); plane 5 on no crtc */
	st.planes[0].crtc = 0, st.planes[0].zpos = 2;
	st.planes[1].crtc = 0, st.planes[1].zpos = 0;
	st.planes[3].crtc = 0, st.planes[3].zpos = 2;
	st.planes[2].crtc = 1, st.planes[2].zpos = 7;
	st.planes[4].crtc = 1, st.planes[4].zpos = 1;
	st.planes[5].crtc = -1, st.planes[5].zpos = 9, st.planes[5].normalized_zpos = 42;

	rc = drm_atomic_normalize_zpos_crtcs(&dev, &st, &changed);
	CHECK(rc == 0, "normalize = %d", rc);
	CHECK(st.planes[1].normalized_zpos == 0, "plane 1 at the bottom: %u", st.planes[1].normalized_zpos);
	CHECK(st.planes[0].normalized_zpos == 1, "plane 0 (lower id of the tie): %u", st.planes[0].normalized_zpos);
	CHECK(st.planes[3].normalized_zpos == 2, "plane 3 on top: %u", st.planes[3].normalized_zpos);
	CHECK(st.planes[4].normalized_zpos == 0 && st.planes[2].normalized_zpos == 1,
	      "crtc 1: %u %u", st.planes[4].normalized_zpos, st.planes[2].normalized_zpos);
	CHECK(st.planes[5].normalized_zpos == 42, "plane on no crtc untouched: %u", st.planes[5].normalized_zpos);
	CHECK(changed == 3, "crtcs 0 and 1 changed: %#x", changed);

	/* committed; the same request again changes nothing */
	commit_state();
	state_from_committed();
	st.planes[0].normalized_zpos = st.planes[0].zpos; /* as a fresh state may have it */
	rc = drm_atomic_normalize_zpos_crtcs(&dev, &st, &changed);
	CHECK(rc == 0 && changed == 0, "nothing changed: rc %d mask %#x", rc, changed);
	CHECK(st.planes[0].normalized_zpos == 1, "recomputed as committed: %u", st.planes[0].normalized_zpos);

	/* a zpos change on crtc 1 only */
	state_from_committed();
	st.planes[4].zpos = 8;
	rc = drm_atomic_normalize_zpos_crtcs(&dev, &st, &changed);
	CHECK(rc == 0 && changed == 2, "zpos change on crtc 1: rc %d mask %#x", rc, changed);
	CHECK(st.planes[2].normalized_zpos == 0 && st.planes[4].normalized_zpos == 1,
	      "crtc 1 reordered: %u %u", st.planes[2].normalized_zpos, st.planes[4].normalized_zpos);

	/* a plane moving from crtc 0 to crtc 2 */
	state_from_committed();
	st.planes[1].crtc = 2;
	rc = drm_atomic_normalize_zpos_crtcs(&dev, &st, &changed);
	CHECK(rc == 0 && changed == 5, "move: rc %d mask %#x", rc, changed);
	CHECK(st.planes[1].normalized_zpos == 0 && st.planes[0].normalized_zpos == 0 &&
	      st.planes[3].normalized_zpos == 1, "after the move: %u %u %u",
	      st.planes[1].normalized_zpos, st.planes[0].normalized_zpos,
	      st.planes[3].normalized_zpos);

	/* a crtc that does not exist */
	state_from_committed();
	st.planes[2].crtc = 3;
	rc = drm_atomic_normalize_zpos(&dev, &st);
	CHECK(rc == -EINVAL, "crtc 3 of 3: %d", rc);
}

/* ---- plane properties ------------------------------------------------------------ */

static uint64_t list_value(struct drm_object_properties *props, struct drm_prop *p)
{
	uint64_t v = 0xbad;

	CHECK(drm_object_property_get_value(props, p, &v) == 0, "%s attached", p ? p->name : "?");
	return v;
}

static bool valid(struct drm_prop *p, uint64_t v)
{
	void *ref;
	bool ok;

	if (!p)
		return false;
	ok = drm_property_change_valid_get(p, v, &ref);

	if (ok)
		drm_property_change_valid_put(p, ref);
	return ok;
}

static void test_plane_props(void)
{
	struct drm_plane *p0 = &dev.planes[0], *p1 = &dev.planes[1], *p2 = &dev.planes[2];
	uint32_t n;
	int rc, w;

	dev_init();

	/* alpha: one property for every plane */
	rc = drm_plane_create_alpha_property(&dev, p0);
	CHECK(rc == 0 && p0->alpha_property, "alpha 0: %d", rc);
	n = dev.nprops;
	rc = drm_plane_create_alpha_property(&dev, p1);
	CHECK(rc == 0 && p1->alpha_property == p0->alpha_property, "alpha shared: %d", rc);
	CHECK(dev.nprops == n, "no second alpha in the table: %u vs %u", dev.nprops, n);
	CHECK(p1->alpha == 0xffff && list_value(&p1->properties, p1->alpha_property) == 0xffff,
	      "alpha starts opaque");
	CHECK(valid(p0->alpha_property, 0) && valid(p0->alpha_property, 0xffff) &&
	      !valid(p0->alpha_property, 0x10000), "alpha range");
	w = warnings;
	rc = drm_plane_create_alpha_property(&dev, p0);
	CHECK(rc == -EINVAL && warnings > w, "alpha twice on a plane: %d", rc);

	/* rotation: shared where the supported sets match */
	rc = drm_plane_create_rotation_property(&dev, p0, DRM_MODE_ROTATE_0,
						DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180 |
						DRM_MODE_REFLECT_X);
	CHECK(rc == 0 && p0->rotation_property && p0->rotation == DRM_MODE_ROTATE_0, "rotation 0: %d", rc);
	CHECK(p0->rotation_property->nenums == 3, "three entries: %u", p0->rotation_property->nenums);
	CHECK(valid(p0->rotation_property, DRM_MODE_ROTATE_180 | DRM_MODE_REFLECT_X) &&
	      !valid(p0->rotation_property, DRM_MODE_ROTATE_90), "rotation bits");
	rc = drm_plane_create_rotation_property(&dev, p1, DRM_MODE_ROTATE_180,
						DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180 |
						DRM_MODE_REFLECT_X);
	CHECK(rc == 0 && p1->rotation_property == p0->rotation_property, "rotation shared");
	CHECK(p1->rotation == DRM_MODE_ROTATE_180 &&
	      list_value(&p1->properties, p1->rotation_property) == DRM_MODE_ROTATE_180,
	      "plane 1's own initial rotation");
	CHECK(list_value(&p0->properties, p0->rotation_property) == DRM_MODE_ROTATE_0,
	      "plane 0 keeps its value");
	rc = drm_plane_create_rotation_property(&dev, p2, DRM_MODE_ROTATE_0,
						DRM_MODE_ROTATE_MASK);
	CHECK(rc == 0 && p2->rotation_property != p0->rotation_property, "other set, other property");
	CHECK(!strcmp(drm_property_enums(p2->rotation_property)[3].name, "rotate-270") &&
	      drm_property_enums(p2->rotation_property)[3].value == 3, "rotate-270 is bit 3");

	/* zpos: mutable ranges shared, fixed values each their own */
	rc = drm_plane_create_zpos_property(&dev, p0, 1, 0, 3);
	CHECK(rc == 0 && p0->zpos == 1 && p0->normalized_zpos == 1, "zpos 0: %d", rc);
	rc = drm_plane_create_zpos_property(&dev, p1, 2, 0, 3);
	CHECK(rc == 0 && p1->zpos_property == p0->zpos_property && p1->zpos == 2, "zpos shared");
	rc = drm_plane_create_zpos_immutable_property(&dev, p2, 5);
	CHECK(rc == 0 && p2->zpos_property != p0->zpos_property && p2->zpos == 5, "fixed zpos");
	CHECK((p2->zpos_property->flags & DRM_MODE_PROP_IMMUTABLE) && !valid(p2->zpos_property, 5),
	      "a fixed zpos cannot be written");
	CHECK(valid(p0->zpos_property, 3) && !valid(p0->zpos_property, 4), "zpos range");
	w = warnings;
	rc = drm_plane_create_zpos_immutable_property(&dev, p2, 6);
	CHECK(rc == -EINVAL && warnings > w && p2->zpos == 5, "zpos twice: %d", rc);

	/* pixel blend mode */
	rc = drm_plane_create_blend_mode_property(&dev, p0, BIT(DRM_MODE_BLEND_PREMULTI) |
							     BIT(DRM_MODE_BLEND_PIXEL_NONE));
	CHECK(rc == 0 && p0->pixel_blend_mode == DRM_MODE_BLEND_PREMULTI, "blend 0: %d", rc);
	CHECK(p0->blend_mode_property->nenums == 2 &&
	      !strcmp(drm_property_enums(p0->blend_mode_property)[0].name, "None") &&
	      !strcmp(drm_property_enums(p0->blend_mode_property)[1].name, "Pre-multiplied"),
	      "blend entries");
	CHECK(valid(p0->blend_mode_property, DRM_MODE_BLEND_PIXEL_NONE) &&
	      !valid(p0->blend_mode_property, DRM_MODE_BLEND_COVERAGE), "blend values");
	rc = drm_plane_create_blend_mode_property(&dev, p1, BIT(DRM_MODE_BLEND_COVERAGE) |
							     BIT(DRM_MODE_BLEND_PIXEL_NONE));
	CHECK(rc == 0 && p1->pixel_blend_mode == DRM_MODE_BLEND_COVERAGE &&
	      p1->blend_mode_property != p0->blend_mode_property, "coverage default");
	n = dev.nprops;
	w = warnings;
	rc = drm_plane_create_blend_mode_property(&dev, p2, BIT(5));
	CHECK(rc == -EINVAL && warnings > w && dev.nprops == n && !p2->blend_mode_property,
	      "unknown blend mode: %d", rc);
	rc = drm_plane_create_blend_mode_property(&dev, p2, BIT(DRM_MODE_BLEND_PIXEL_NONE));
	CHECK(rc == 0 && p2->pixel_blend_mode == DRM_MODE_BLEND_PIXEL_NONE, "none only");

	/* YCbCr conversion */
	rc = drm_plane_create_color_properties(&dev, p0,
					       BIT(DRM_COLOR_YCBCR_BT601) | BIT(DRM_COLOR_YCBCR_BT709),
					       BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
					       BIT(DRM_COLOR_YCBCR_FULL_RANGE),
					       DRM_COLOR_YCBCR_BT709, DRM_COLOR_YCBCR_FULL_RANGE);
	CHECK(rc == 0 && p0->color_encoding == DRM_COLOR_YCBCR_BT709 &&
	      p0->color_range == DRM_COLOR_YCBCR_FULL_RANGE, "color props: %d", rc);
	CHECK(p0->color_encoding_property->nenums == 2 &&
	      !strcmp(drm_property_enums(p0->color_encoding_property)[1].name, "ITU-R BT.709 YCbCr"),
	      "encoding entries");
	CHECK(!strcmp(drm_property_enums(p0->color_range_property)[0].name, "YCbCr limited range"),
	      "range entries");
	CHECK(list_value(&p0->properties, p0->color_encoding_property) == DRM_COLOR_YCBCR_BT709,
	      "encoding attached with its default");
	CHECK(!valid(p0->color_encoding_property, DRM_COLOR_YCBCR_BT2020), "BT.2020 not offered");
	rc = drm_plane_create_color_properties(&dev, p1,
					       BIT(DRM_COLOR_YCBCR_BT601) | BIT(DRM_COLOR_YCBCR_BT709),
					       BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
					       BIT(DRM_COLOR_YCBCR_FULL_RANGE),
					       DRM_COLOR_YCBCR_BT601, DRM_COLOR_YCBCR_LIMITED_RANGE);
	CHECK(rc == 0 && p1->color_encoding_property == p0->color_encoding_property &&
	      p1->color_range_property == p0->color_range_property, "color props shared");
	w = warnings;
	rc = drm_plane_create_color_properties(&dev, p2, BIT(DRM_COLOR_YCBCR_BT601),
					       BIT(DRM_COLOR_YCBCR_LIMITED_RANGE),
					       DRM_COLOR_YCBCR_BT709,
					       DRM_COLOR_YCBCR_LIMITED_RANGE);
	CHECK(rc == -EINVAL && warnings > w, "default not supported: %d", rc);
	rc = drm_plane_create_color_properties(&dev, p2, BIT(DRM_COLOR_ENCODING_MAX),
					       BIT(DRM_COLOR_YCBCR_LIMITED_RANGE),
					       DRM_COLOR_YCBCR_BT601,
					       DRM_COLOR_YCBCR_LIMITED_RANGE);
	CHECK(rc == -EINVAL, "unknown encoding: %d", rc);

	CHECK(!strcmp(drm_get_color_encoding_name(DRM_COLOR_YCBCR_BT2020), "ITU-R BT.2020 YCbCr"),
	      "encoding name");
	CHECK(!strcmp(drm_get_color_range_name(DRM_COLOR_YCBCR_FULL_RANGE), "YCbCr full range"),
	      "range name");
	w = warnings;
	CHECK(!strcmp(drm_get_color_range_name(DRM_COLOR_RANGE_MAX), "unknown") && warnings > w,
	      "range out of bounds");

	/* every attached property is the plane's own pointer */
	CHECK(drm_mode_obj_find_prop_id(&dev, &p2->properties, p2->rotation_property->id) ==
	      p2->rotation_property, "rotation in plane 2's list");

	/* a device not registered yet */
	{
		static struct drm_device raw;
		struct drm_plane *rp = &raw.planes[0];

		raw.drv = &drv;
		w = warnings;
		rc = drm_plane_create_alpha_property(&raw, rp);
		CHECK(rc == -EINVAL && warnings > w && raw.nprops == 0, "unregistered: %d", rc);
	}
}

/* ---- the crtc's colour pipeline ----------------------------------------------------- */

static int index_in_list(struct drm_object_properties *props, uint32_t id)
{
	for (int i = 0; i < props->count; i++)
		if (props->ids[i] == id)
			return i;
	return -1;
}

static void test_crtc_color(void)
{
	struct drm_crtc *c0, *c1, *c2;
	struct drm_object_properties *pr;
	uint32_t saved;
	int rc, w;

	dev_init();
	c0 = &dev.crtc[0];
	c1 = &dev.crtc[1];
	c2 = &dev.crtc[2];

	rc = drm_crtc_enable_color_mgmt(&dev, c0, 33, true, 256);
	pr = &c0->properties;
	CHECK(rc == 0, "enable: %d", rc);
	CHECK(pr->count == 5, "GAMMA_LUT pair once, degamma pair, CTM: %d", pr->count);
	CHECK(index_in_list(pr, dev.prop_gamma_lut) == 0 &&
	      index_in_list(pr, dev.prop_gamma_lut_size) == 1, "the core's first");
	CHECK(index_in_list(pr, dev.prop_degamma_lut) == 2 &&
	      index_in_list(pr, dev.prop_degamma_lut_size) == 3 &&
	      index_in_list(pr, dev.prop_ctm) == 4, "then ours in order");
	CHECK(pr->values[3] == 33 && pr->values[1] == 256 && pr->values[2] == 0 && pr->values[4] == 0,
	      "values: sizes and empty tables");

	/* a gamma size other than the driver's: this crtc's own GAMMA_LUT_SIZE
	 * with the legacy table split off, refused without */
	w = warnings;
	rc = drm_crtc_enable_color_mgmt(&dev, c1, 0, true, 1024);
#if DRM_COLOR_LEGACY_GAMMA_SPLIT
	{
		int k = index_in_list(&c1->properties, dev.prop_gamma_lut_size);

		CHECK(rc == 0 && warnings == w, "per-crtc gamma size: %d", rc);
		CHECK(k >= 0 && c1->properties.values[k] == 1024,
		      "c1's GAMMA_LUT_SIZE: %llu",
		      k >= 0 ? (unsigned long long)c1->properties.values[k] : 0ULL);
		CHECK(index_in_list(&c0->properties, dev.prop_gamma_lut_size) == 1 &&
		      c0->properties.values[1] == 256, "c0 keeps its own");
	}
#else
	CHECK(rc == -EINVAL && warnings > w, "gamma size mismatch: %d", rc);
#endif
	CHECK(index_in_list(&c1->properties, dev.prop_ctm) >= 0 &&
	      index_in_list(&c1->properties, dev.prop_degamma_lut) < 0, "CTM attached all the same");

	/* a device without CTM */
	saved = dev.prop_ctm;
	dev.prop_ctm = 0;
	w = warnings;
	rc = drm_crtc_enable_color_mgmt(&dev, c2, 17, true, 0);
	CHECK(rc == -EINVAL && warnings > w, "no CTM on the device: %d", rc);
	CHECK(index_in_list(&c2->properties, dev.prop_degamma_lut) >= 0, "degamma attached");
	dev.prop_ctm = saved;
}

static void test_legacy_gamma(void)
{
	static uint16_t r[256], g[256], b[256];
	struct drm_crtc_state cs;
	struct drm_blob *ctm, *gl;
	int rc;

	dev_init();
	CHECK(drm_crtc_enable_color_mgmt(&dev, &dev.crtc[0], 0, true, 256) == 0, "crtc 0");
	/* crtc 1 has no GAMMA_LUT of its own in this test: the core never
	 * attached it (as for a crtc set up before the table existed) */
	dev.crtc[1].properties.core_attached = true;
	CHECK(drm_crtc_enable_color_mgmt(&dev, &dev.crtc[1], 64, false, 0) == 0, "crtc 1");
	dev.crtc[2].properties.core_attached = true;

	for (int i = 0; i < 256; i++) {
		r[i] = (uint16_t)(i << 8);
		g[i] = (uint16_t)(i << 8 | i);
		b[i] = (uint16_t)(0xffff - (i << 8));
	}

	memset(&cs, 0, sizeof(cs));
	cs.crtc = &dev.crtc[0];
	/* a CTM the client had set: the older call resets it */
	ctm = drm_property_create_blob(&dev, sizeof(struct drm_color_ctm), NULL);
	CHECK(drm_property_replace_blob(&cs.ctm, ctm), "ctm in the state");
	CHECK(ctm->refs == 2, "creator + state: %d", ctm->refs);

	rc = drm_crtc_legacy_gamma_set_state(&dev, &cs, r, g, b, 256);
	CHECK(rc == 0 && cs.gamma_lut && !cs.degamma_lut && !cs.ctm && cs.color_mgmt_changed,
	      "gamma via GAMMA_LUT: %d", rc);
	CHECK(ctm->refs == 1, "the state let go of the CTM: %d", ctm->refs);
	drm_property_blob_put(ctm);
	gl = cs.gamma_lut;
	CHECK(gl->refs == 1 && gl->owner == NULL && drm_color_lut_size(gl) == 256,
	      "a kernel blob only the state holds: refs %d size %d", gl->refs, drm_color_lut_size(gl));
	CHECK(drm_color_lut_data(gl)[255].red == 0xff00 && drm_color_lut_data(gl)[1].green == 0x0101 &&
	      drm_color_lut_data(gl)[0].blue == 0xffff, "the values");
	drm_property_replace_blob(&cs.gamma_lut, NULL);
	CHECK(!gl->in_use, "freed with the state's reference");

	memset(&cs, 0, sizeof(cs));
	cs.crtc = &dev.crtc[1];
	rc = drm_crtc_legacy_gamma_set_state(&dev, &cs, r, g, b, 256);
	CHECK(rc == 0 && cs.degamma_lut && !cs.gamma_lut, "only a degamma table: %d", rc);
	drm_property_replace_blob(&cs.degamma_lut, NULL);

	memset(&cs, 0, sizeof(cs));
	cs.crtc = &dev.crtc[2];
	rc = drm_crtc_legacy_gamma_set_state(&dev, &cs, r, g, b, 256);
	CHECK(rc == -ENODEV && !cs.color_mgmt_changed, "no table: %d", rc);

	cs.crtc = &dev.crtc[0];
	CHECK(drm_crtc_legacy_gamma_set_state(&dev, &cs, r, g, b, 0) == -EINVAL, "no entries");

	for (int i = 0; i < DRM_MAX_BLOBS; i++)
		CHECK(!dev.blobs[i].in_use, "blob %d left over", i);
}

/* ---- tables ----------------------------------------------------------------------- */

static struct drm_blob lut_blob(struct drm_color_lut *e, int n)
{
	struct drm_blob b;

	memset(&b, 0, sizeof(b));
	b.data = e;
	b.length = (uint32_t)(n * sizeof(*e));
	return b;
}

static void test_lut_check(void)
{
	struct drm_color_lut e[4] = {
		{ 0, 0, 0, 0 }, { 100, 100, 100, 0 }, { 100, 100, 100, 0 }, { 0xffff, 0xffff, 0xffff, 0 },
	};
	struct drm_blob b = lut_blob(e, 4);
	const uint32_t both = DRM_COLOR_LUT_EQUAL_CHANNELS | DRM_COLOR_LUT_NON_DECREASING;

	CHECK(drm_color_lut_size(&b) == 4, "4 entries");
	CHECK(drm_color_lut_check(&b, both) == 0, "grey, flat steps allowed");
	CHECK(drm_color_lut_check(NULL, both) == 0, "no table");
	e[2].green = 101;
	CHECK(drm_color_lut_check(&b, DRM_COLOR_LUT_EQUAL_CHANNELS) == -EINVAL, "unequal channels");
	CHECK(drm_color_lut_check(&b, DRM_COLOR_LUT_NON_DECREASING) == 0, "still increasing");
	CHECK(drm_color_lut_check(&b, 0) == 0, "no tests");
	e[2].green = 100;
	e[2].blue = 99;
	e[2].red = 99;
	e[2].green = 99;
	CHECK(drm_color_lut_check(&b, DRM_COLOR_LUT_NON_DECREASING) == -EINVAL, "decreasing");
	CHECK(drm_color_lut_check(&b, DRM_COLOR_LUT_EQUAL_CHANNELS) == 0, "but grey");
	e[2].red = e[2].green = e[2].blue = 100;
	e[3].blue = 50;
	CHECK(drm_color_lut_check(&b, both) == -EINVAL, "last entry: both fail");
	/* the first entry is never compared with a previous one */
	e[3].blue = 0xffff;
	e[0].red = e[0].green = e[0].blue = 0xffff;
	CHECK(drm_color_lut_check(&b, DRM_COLOR_LUT_NON_DECREASING) == -EINVAL, "entry 1 below entry 0");
}

/* value * (2^bits - 1) / 0xffff to nearest, worked out independently */
static uint32_t ref_extract(uint32_t v, int bits)
{
	unsigned __int128 num = (unsigned __int128)v * ((1ull << bits) - 1);

	return (uint32_t)((2 * num + 0xffff) / (2 * 0xffff));
}

static void test_extract(void)
{
	int bad = 0;

	CHECK(drm_color_lut_extract(0xffff, 8) == 0xff, "1.0 in 8 bits");
	CHECK(drm_color_lut_extract(0, 8) == 0, "0.0");
	CHECK(drm_color_lut_extract(0x8080, 8) == 0x80, "0x8080 -> 0x80");
	CHECK(drm_color_lut_extract(0x7f7f, 8) == 0x7f, "0x7f7f -> 0x7f");
	CHECK(drm_color_lut_extract(0xffff, 10) == 0x3ff, "1.0 in 10 bits");
	CHECK(drm_color_lut_extract(0x8000, 10) == 0x200, "half in 10 bits: %#x",
	      drm_color_lut_extract(0x8000, 10));
	CHECK(drm_color_lut_extract(0x1234, 16) == 0x1234, "16 bits as they are");
	CHECK(drm_color_lut_extract(0xffff, 24) == 0xffffff, "1.0 in 24 bits");
	CHECK(drm_color_lut_extract(0xffff, 31) == 0x7fffffff, "1.0 in 31 bits");

	for (int bits = 1; bits <= 31; bits++)
		for (uint32_t v = 0; v <= 0xffff; v++)
			if (drm_color_lut_extract(v, bits) != ref_extract(v, bits) && bad++ < 5)
				printf("extract(%#x, %d) = %#x, want %#x\n", v, bits,
				       drm_color_lut_extract(v, bits), ref_extract(v, bits));
	checks++;
	if (bad)
		fails++;

	CHECK(drm_color_lut32_extract(0xffffffffu, 16) == 0xffff, "32-bit 1.0 in 16 bits");
	CHECK(drm_color_lut32_extract(0x80000000u, 8) == 0x80, "32-bit half in 8 bits: %#x",
	      drm_color_lut32_extract(0x80000000u, 8));
	CHECK(drm_color_lut32_extract(0x12345678u, 32) == 0x12345678u, "32 bits as they are");
	CHECK(drm_color_lut32_extract(0, 12) == 0, "32-bit 0.0");
}

static void test_ctm(void)
{
	const uint64_t one = 1ull << 32, neg = 1ull << 63;
	int w;

	/* S31.32 -> Q2.14 (16 bits, [-2, 2 - 2^-14]) */
	CHECK(drm_color_ctm_s31_32_to_qm_n(one, 2, 14) == 0x4000, "1.0");
	CHECK(drm_color_ctm_s31_32_to_qm_n(neg | one, 2, 14) == (uint64_t)-0x4000, "-1.0");
	CHECK(drm_color_ctm_s31_32_to_qm_n(one / 2, 2, 14) == 0x2000, "0.5");
	CHECK(drm_color_ctm_s31_32_to_qm_n(neg | (one / 4), 2, 14) == (uint64_t)-0x1000, "-0.25");
	CHECK(drm_color_ctm_s31_32_to_qm_n(0, 2, 14) == 0, "0");
	CHECK(drm_color_ctm_s31_32_to_qm_n(neg, 2, 14) == 0, "-0");
	/* below the precision: truncated */
	CHECK(drm_color_ctm_s31_32_to_qm_n(1, 2, 14) == 0, "2^-32");
	CHECK(drm_color_ctm_s31_32_to_qm_n((one >> 14) + (one >> 15), 2, 14) == 1, "1.5 lsb");
	/* clamped */
	CHECK(drm_color_ctm_s31_32_to_qm_n(100 * one, 2, 14) == 0x7fff, "100 -> 2 - 2^-14");
	CHECK(drm_color_ctm_s31_32_to_qm_n(neg | (100 * one), 2, 14) == (uint64_t)-0x8000, "-100 -> -2");
	CHECK(drm_color_ctm_s31_32_to_qm_n(2 * one, 2, 14) == 0x7fff, "2.0 does not fit");
	CHECK(drm_color_ctm_s31_32_to_qm_n(neg | (2 * one), 2, 14) == (uint64_t)-0x8000, "-2.0 fits");
	/* Q3.12, Q1.31, Q0.32 */
	CHECK(drm_color_ctm_s31_32_to_qm_n(3 * one, 3, 12) == 3 * 0x1000, "3.0 in Q3.12");
	CHECK(drm_color_ctm_s31_32_to_qm_n(5 * one, 3, 12) == 0x3fff, "5.0 clamps in Q3.12");
	CHECK(drm_color_ctm_s31_32_to_qm_n(one / 2, 1, 31) == 0x40000000, "0.5 in Q1.31");
	CHECK(drm_color_ctm_s31_32_to_qm_n(one / 2, 0, 32) == 0x7fffffff, "0.5 in Q0.32 clamps");
	CHECK(drm_color_ctm_s31_32_to_qm_n(one / 4, 0, 32) == 0x40000000, "0.25 in Q0.32");
	w = warnings;
	(void)drm_color_ctm_s31_32_to_qm_n(one, 33, 12);
	CHECK(warnings > w, "m > 32 warned about");
}

/* ---- loading tables ------------------------------------------------------------------ */

static uint16_t got[256][3];
static int got_n, got_max;

static void record(struct drm_crtc *crtc, unsigned int i, u16 r, u16 g, u16 b)
{
	(void)crtc;
	if (i < 256) {
		got[i][0] = r;
		got[i][1] = g;
		got[i][2] = b;
	}
	got_n++;
	if ((int)i > got_max)
		got_max = (int)i;
}

static void reset_record(void)
{
	memset(got, 0xaa, sizeof(got));
	got_n = 0;
	got_max = -1;
}

static void test_load(void)
{
	static struct drm_color_lut lut[256];
	int seen[256];
	int bad;

	for (int i = 0; i < 256; i++)
		lut[i] = (struct drm_color_lut){ (u16)(i * 3), (u16)(i * 5), (u16)(i * 7), 0 };

	reset_record();
	drm_crtc_load_gamma_888(NULL, lut, record);
	CHECK(got_n == 256 && got[200][1] == 1000, "888: %d", got_n);

	reset_record();
	drm_crtc_load_gamma_565_from_888(NULL, lut, record);
	CHECK(got_n == 64 && got_max == 63, "565: %d entries", got_n);
	CHECK(got[31][0] == lut[255].red && got[63][1] == lut[255].green &&
	      got[31][2] == lut[255].blue, "565 ends at the last entry");
	CHECK(got[40][0] == 0 && got[40][2] == 0, "565 padding");

	reset_record();
	drm_crtc_load_gamma_555_from_888(NULL, lut, record);
	CHECK(got_n == 32 && got[31][1] == lut[255].green && got[16][0] == lut[132].red,
	      "555");

	reset_record();
	drm_crtc_fill_gamma_888(NULL, record);
	CHECK(got_n == 256 && got[0][0] == 0 && got[255][2] == 0xffff && got[0x12][1] == 0x1212,
	      "888 ramp");

	reset_record();
	drm_crtc_fill_gamma_565(NULL, record);
	CHECK(got_n == 64 && got[31][0] == 0xffff && got[63][1] == 0xffff && got[31][1] == 0x7df7 &&
	      got[63][0] == 0 && got[0][2] == 0, "565 ramp (%#x)", got[31][1]);

	reset_record();
	drm_crtc_fill_gamma_555(NULL, record);
	CHECK(got_n == 32 && got[31][0] == 0xffff && got[31][2] == 0xffff && got[16][1] == 0x8421,
	      "555 ramp (%#x)", got[16][1]);

	reset_record();
	drm_crtc_load_palette_8(NULL, lut, record);
	CHECK(got_n == 256 && got[17][2] == 119, "palette");

	reset_record();
	drm_crtc_fill_palette_332(NULL, record);
	memset(seen, 0, sizeof(seen));
	bad = 0;
	CHECK(got_n == 256, "332: %d", got_n);
	for (int i = 0; i < 256; i++)
		if (got[i][0] == 0xaaaa)
			bad++;
	CHECK(!bad, "332: every index once (%d missing)", bad);
	CHECK(got[0xff][0] == 0xffff && got[0xff][1] == 0xffff && got[0xff][2] == 0xffff &&
	      got[0][0] == 0 && got[0xe0][0] == 0xffff && got[0xe0][1] == 0 && got[0x03][2] == 0xffff,
	      "332 corners");

	reset_record();
	drm_crtc_fill_palette_8(NULL, record);
	CHECK(got_n == 256 && got[0x80][0] == 0x8080 && got[0x80][2] == 0x8080, "grey palette");
}

int main(void)
{
	test_rotation();
	test_zpos();
	test_plane_props();
	test_crtc_color();
	test_legacy_gamma();
	test_lut_check();
	test_extract();
	test_ctm();
	test_load();

	printf("%d checks, %d failed, %d warnings logged\n", checks, fails, warnings);
	return fails ? 1 : 0;
}
