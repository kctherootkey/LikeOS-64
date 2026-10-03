/* Host test for the connector probe (kernel/dev/gpu/drm/drm_probe_helper.c)
 * on the EDID library: the mode list a connector gets from an EDID, the
 * checks every mode passes, the preferred mode at modes[0], the probe's
 * fallbacks, and the epoch / hotplug counting.
 *
 * Run by host/test-drm-probe.sh from the repository root.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_probe_helper.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include "test-edid-corpus.h"

extern void *malloc(unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void *realloc(void *, unsigned long);
extern void free(void *);
extern int printf(const char *, ...);
extern int vprintf(const char *, __builtin_va_list);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);

/* ---- what the probe calls of the kernel -------------------------------- */

uint64_t test_irq_flag = 0x200;
int test_locks_held;

void test_fatal(const char *what, const char *name)
{
	printf("FATAL: %s (%s)\n", what, name ? name : "?");
	__builtin_trap();
}

void *kalloc(size_t size)
{
	return malloc(size);
}

void *kcalloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void *krealloc(void *ptr, size_t new_size)
{
	return realloc(ptr, new_size);
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

static int warnings;

int kprintf(const char *format, ...)
{
	__builtin_va_list ap;
	int n;

	warnings++;
	__builtin_va_start(ap, format);
	n = vprintf(format, ap);
	__builtin_va_end(ap);
	return n;
}

/* The connector calls of drm_connector.c, as far as the probe uses them. */
static int set_edid_null_calls;

int drm_connector_set_edid(struct drm_device *dev, int conn,
			   const uint8_t *edid, unsigned len)
{
	(void)dev;
	(void)conn;
	if (!edid || !len)
		set_edid_null_calls++;
	return 0;
}

void drm_connector_clear_modes(struct drm_device *dev, int conn)
{
	dev->conn[conn].nmodes = 0;
}

int drm_connector_add_mode(struct drm_device *dev, int conn,
			   const struct drm_mode_modeinfo *m)
{
	struct drm_connector *c = &dev->conn[conn];

	if (c->nmodes >= DRM_MAX_MODES)
		return -1;
	c->modes[c->nmodes++] = *m;
	return 0;
}

/* ---- the test ------------------------------------------------------------ */

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct drm_device g_dev;
static struct drm_driver g_drv;

static struct drm_connector *fresh_connector(uint32_t type, uint32_t max_w,
					     uint32_t max_h)
{
	struct drm_connector *c = &g_dev.conn[0];

	drm_display_info_reset(&c->display_info);
	memset(&g_dev.conn[0], 0, sizeof(g_dev.conn[0]));
	g_dev.drv = &g_drv;
	g_dev.nconn = 1;
	g_dev.max_width = max_w;
	g_dev.max_height = max_h;
	c->dev = &g_dev;
	c->id = 40;
	c->type = type;
	c->connected = 1;
	c->interlace_allowed = true;
	c->doublescan_allowed = true;
	c->display_info.panel_orientation = DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
	return c;
}

/* The connector's list from the EDID, as drm_connector_set_edid builds it.
 * Returns the number of modes; *summary gets drm_edid_parse's view. */
static int list_from_edid(struct drm_connector *c, const uint8_t *edid,
			  unsigned len, struct drm_edid_info *summary)
{
	static struct drm_display_mode modes[DRM_PROBE_MAX_MODES];
	const struct drm_edid *drm_edid;
	struct drm_edid_conn ec;
	int n;

	CHECK(drm_edid_parse(edid, len, summary) == 0, "summary parse");
	drm_edid = drm_edid_alloc_checked(edid, len);
	CHECK(drm_edid != NULL, "checked EDID");
	if (!drm_edid)
		return 0;
	memset(&ec, 0, sizeof(ec));
	ec.connector_type = c->type;
	ec.info = &c->display_info;
	ec.eld = c->eld;
	ec.modes = modes;
	ec.max_modes = DRM_PROBE_MAX_MODES;
	drm_edid_update_display_info(&ec, drm_edid);
	drm_edid_add_modes(&ec, drm_edid);
	n = drm_helper_probe_edid_list(&g_dev, c, modes, ec.nmodes, summary);
	drm_edid_free(drm_edid);
	return n;
}

static int size_of(const struct drm_mode_modeinfo *m)
{
	return m->hdisplay * m->vdisplay;
}

/* What every list must be: one preferred mode, at modes[0], the summary's
 * when it fits the limits; nothing the connector cannot send; no two
 * equal modes; sorted after modes[0]; every mode of the summary that fits
 * the limits present. */
static void check_list(const char *name, struct drm_connector *c,
		       const struct drm_edid_info *summary, uint32_t max_w,
		       uint32_t max_h)
{
	int npref = 0;
	bool summary_pref_fits = false;

	if (summary->preferred >= 0) {
		const struct drm_mode_modeinfo *p = &summary->modes[summary->preferred];
		summary_pref_fits = p->hdisplay <= max_w && p->vdisplay <= max_h;
	}
	CHECK(c->nmodes > 0 || summary->nmodes == 0, "%s: no modes", name);
	if (!c->nmodes)
		return;
	CHECK(c->modes[0].type & DRM_MODE_TYPE_PREFERRED, "%s: modes[0] not preferred", name);
	if (summary_pref_fits)
		CHECK(drm_mode_equal(&c->modes[0], &summary->modes[summary->preferred]),
		      "%s: modes[0] %ux%u@%u is not the summary's preferred %ux%u@%u", name,
		      c->modes[0].hdisplay, c->modes[0].vdisplay, c->modes[0].vrefresh,
		      summary->modes[summary->preferred].hdisplay,
		      summary->modes[summary->preferred].vdisplay,
		      summary->modes[summary->preferred].vrefresh);
	for (uint32_t i = 0; i < c->nmodes; i++) {
		const struct drm_mode_modeinfo *m = &c->modes[i];
		struct drm_display_mode dm;

		if (m->type & DRM_MODE_TYPE_PREFERRED)
			npref++;
		CHECK(!(m->flags & DRM_MODE_FLAG_3D_MASK), "%s: stereo mode %s", name, m->name);
		CHECK(!(m->flags & DRM_MODE_FLAG_DBLCLK), "%s: pixel-repeated mode %s", name,
		      m->name);
		CHECK(m->hdisplay <= max_w && m->vdisplay <= max_h, "%s: %s too large", name,
		      m->name);
		CHECK(drm_mode_from_umode(&dm, m) == 0, "%s: %s does not convert", name, m->name);
		if (!c->ycbcr_420_allowed)
			CHECK(!drm_mode_is_420_only(&c->display_info, &dm),
			      "%s: 4:2:0-only mode %s", name, m->name);
		CHECK(drm_mode_validate_basic(&dm) == MODE_OK, "%s: %s fails the basic checks",
		      name, m->name);
		for (uint32_t j = 0; j < i; j++) {
			struct drm_display_mode dj;

			drm_mode_from_umode(&dj, &c->modes[j]);
			CHECK(!drm_display_mode_equal(&dj, &dm), "%s: %s listed twice", name,
			      m->name);
		}
		if (i >= 2) {
			const struct drm_mode_modeinfo *p = &c->modes[i - 1];
			CHECK(size_of(p) > size_of(m) ||
				      (size_of(p) == size_of(m) && p->vrefresh >= m->vrefresh),
			      "%s: %s sorted after %s", name, m->name, p->name);
		}
	}
	CHECK(npref == 1, "%s: %d preferred modes", name, npref);
	for (int i = 0; i < summary->nmodes; i++) {
		const struct drm_mode_modeinfo *s = &summary->modes[i];
		bool found = false;

		if (s->hdisplay > max_w || s->vdisplay > max_h)
			continue;
		for (uint32_t j = 0; j < c->nmodes && !found; j++)
			found = drm_mode_equal(&c->modes[j], s);
		CHECK(found, "%s: the summary's %s@%u is missing", name, s->name, s->vrefresh);
	}
}

struct corpus_entry {
	const char *name;
	const unsigned char *edid;
	unsigned len;
};

static const struct corpus_entry corpus[] = {
	{ "dvi_1080p", test_edid_dvi_1080p, 128 },
	{ "hdmi_1080p_100mhz", test_edid_hdmi_1080p_rgb_max_100mhz, 256 },
	{ "hdmi_1080p_200mhz", test_edid_hdmi_1080p_rgb_max_200mhz, 256 },
	{ "hdmi_1080p_200mhz_hdr", test_edid_hdmi_1080p_rgb_max_200mhz_hdr, 256 },
	{ "hdmi_1080p_340mhz", test_edid_hdmi_1080p_rgb_max_340mhz, 256 },
	{ "hdmi_1080p_yuv_dc_200mhz", test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz, 256 },
	{ "hdmi_1080p_yuv_dc_340mhz", test_edid_hdmi_1080p_rgb_yuv_dc_max_340mhz, 256 },
	{ "hdmi_1080p_4k_yuv420", test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz, 256 },
	{ "hdmi_4k_yuv420_340mhz", test_edid_hdmi_4k_rgb_yuv420_dc_max_340mhz, 256 },
};

static enum drm_mode_status no_wider_than_1920(struct drm_device *dev,
					       struct drm_connector *c,
					       const struct drm_display_mode *mode)
{
	(void)dev;
	(void)c;
	return mode->hdisplay > 1920 ? MODE_CLOCK_HIGH : MODE_OK;
}

static void test_corpus(int verbose)
{
	static struct drm_edid_info summary;

	for (unsigned int k = 0; k < sizeof(corpus) / sizeof(corpus[0]); k++) {
		const struct corpus_entry *e = &corpus[k];
		struct drm_connector *c;
		uint32_t plain;

		/* as i915 sets the device up */
		g_drv.mode_valid = NULL;
		c = fresh_connector(DRM_MODE_CONNECTOR_HDMIA, 4096, 4096);
		list_from_edid(c, e->edid, e->len, &summary);
		check_list(e->name, c, &summary, 4096, 4096);
		plain = c->nmodes;
		if (verbose) {
			printf("%-26s summary %2d modes, connector %3u modes, modes[0] %s@%u\n",
			       e->name, summary.nmodes, c->nmodes,
			       c->nmodes ? c->modes[0].name : "-",
			       c->nmodes ? c->modes[0].vrefresh : 0);
		}

		/* a source that sends 4:2:0: never fewer modes */
		c = fresh_connector(DRM_MODE_CONNECTOR_HDMIA, 4096, 4096);
		c->ycbcr_420_allowed = true;
		list_from_edid(c, e->edid, e->len, &summary);
		check_list(e->name, c, &summary, 4096, 4096);
		CHECK(c->nmodes >= plain, "%s: 4:2:0 allowed lists fewer modes (%u < %u)",
		      e->name, c->nmodes, plain);

		/* a driver that refuses everything wider than 1920 */
		g_drv.mode_valid = no_wider_than_1920;
		c = fresh_connector(DRM_MODE_CONNECTOR_HDMIA, 4096, 4096);
		list_from_edid(c, e->edid, e->len, &summary);
		check_list(e->name, c, &summary, 1920, 4096);
		g_drv.mode_valid = NULL;

		drm_display_info_reset(&c->display_info);
	}
}

/* The 4K sink with a 4:2:0 source has 4:2:0-only modes the plain one
 * does not get. */
static void test_420(void)
{
	static struct drm_edid_info summary;
	struct drm_connector *c;
	uint32_t plain;

	c = fresh_connector(DRM_MODE_CONNECTOR_HDMIA, 4096, 4096);
	list_from_edid(c, test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz, 256,
		       &summary);
	plain = c->nmodes;
	c = fresh_connector(DRM_MODE_CONNECTOR_HDMIA, 4096, 4096);
	c->ycbcr_420_allowed = true;
	list_from_edid(c, test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz, 256,
		       &summary);
	CHECK(c->nmodes > plain, "4:2:0 source: %u modes, plain %u", c->nmodes, plain);
	drm_display_info_reset(&c->display_info);
}

/* Equal modes merge; refused ones go; the preferred one leads. */
static void test_validate_list(void)
{
	static struct drm_display_mode modes[8];
	struct drm_connector *c = fresh_connector(DRM_MODE_CONNECTOR_DisplayPort, 4096, 4096);
	int n = 0;

	/* 1024x768@60, twice; 1920x1080@60 preferred; 8192x4320 too large;
	 * an interlaced one the connector cannot send */
	drm_mode_copy(&modes[n++], drm_mode_find_dmt(1024, 768, 60, false));
	drm_mode_copy(&modes[n++], drm_mode_find_dmt(1024, 768, 60, false));
	drm_mode_copy(&modes[n], drm_mode_find_dmt(1920, 1080, 60, false));
	modes[n++].type |= DRM_MODE_TYPE_PREFERRED;
	CHECK(drm_cvt_mode(&modes[n++], 8192, 4320, 30, true, false, false) == 0, "cvt 8k");
	CHECK(drm_cvt_mode(&modes[n++], 1920, 1080, 60, false, true, false) == 0, "cvt 1080i");
	c->interlace_allowed = false;
	n = drm_helper_probe_validate_list(&g_dev, c, modes, n, 4096, 4096);
	CHECK(n == 2, "validate_list left %d modes, want 2", n);
	CHECK(n >= 1 && modes[0].hdisplay == 1920 && (modes[0].type & DRM_MODE_TYPE_PREFERRED),
	      "validate_list: 1920x1080 preferred not first");
	CHECK(n >= 2 && modes[1].hdisplay == 1024, "validate_list: 1024x768 not second");
}

/* ---- the probe -------------------------------------------------------------- */

static int fake_status = DRM_MODE_CONNECTED;
static int fake_modes; /* how many 800x600 modes get_modes adds */

static int fake_detect(struct drm_device *dev, struct drm_connector *c)
{
	(void)dev;
	(void)c;
	return fake_status;
}

static int fake_get_modes(struct drm_device *dev, struct drm_connector *c)
{
	struct drm_mode_modeinfo m;

	c->nmodes = 0;
	for (int i = 0; i < fake_modes; i++) {
		drm_mode_fill(&m, 800, 600, 60 + (uint32_t)i, i == 0);
		drm_connector_add_mode(dev, (int)(c - dev->conn), &m);
	}
	return (int)c->nmodes;
}

/* drm_mode_fill lives in drm_connector.c; the same here. */
void drm_mode_fill(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		   uint32_t hz, int preferred)
{
	memset(m, 0, sizeof(*m));
	uint32_t hbl = w / 5 + 64, vbl = 30;
	m->hdisplay = (uint16_t)w;
	m->hsync_start = (uint16_t)(w + hbl / 4);
	m->hsync_end = (uint16_t)(w + hbl / 2);
	m->htotal = (uint16_t)(w + hbl);
	m->vdisplay = (uint16_t)h;
	m->vsync_start = (uint16_t)(h + 3);
	m->vsync_end = (uint16_t)(h + 8);
	m->vtotal = (uint16_t)(h + vbl);
	m->vrefresh = hz;
	m->clock = (uint32_t)(((uint64_t)m->htotal * m->vtotal * hz) / 1000);
	m->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC;
	m->type = DRM_MODE_TYPE_DRIVER | (preferred ? DRM_MODE_TYPE_PREFERRED : 0);
}

static void test_probe(void)
{
	struct drm_connector *c;
	uint64_t epoch;
	uint32_t hotplug;
	int count;

	g_drv.detect = fake_detect;
	g_drv.get_modes = fake_get_modes;

	/* connected, the driver finds three modes */
	c = fresh_connector(DRM_MODE_CONNECTOR_HDMIA, 4096, 4096);
	fake_status = DRM_MODE_CONNECTED;
	fake_modes = 3;
	count = drm_helper_probe_single_connector_modes(&g_dev, c, 4096, 4096);
	CHECK(count == 3 && c->nmodes == 3, "probe: %d / %u modes, want 3", count, c->nmodes);
	CHECK(c->nmodes && (c->modes[0].type & DRM_MODE_TYPE_PREFERRED),
	      "probe: modes[0] not preferred");
	CHECK(c->nmodes == 3 && c->modes[1].vrefresh > c->modes[2].vrefresh,
	      "probe: not sorted by refresh");

	/* connected, nothing found: the standard modes up to 1024x768 */
	fake_modes = 0;
	count = drm_helper_probe_single_connector_modes(&g_dev, c, 4096, 4096);
	CHECK(count > 0 && c->nmodes > 0, "probe: no fallback modes");
	for (uint32_t i = 0; i < c->nmodes; i++)
		CHECK(c->modes[i].hdisplay <= 1024 && c->modes[i].vdisplay <= 768 &&
			      c->modes[i].vrefresh <= 61,
		      "probe: fallback mode %ux%u@%u", c->modes[i].hdisplay,
		      c->modes[i].vdisplay, c->modes[i].vrefresh);

	/* DisplayPort without modes: 640x480 preferred, first */
	c = fresh_connector(DRM_MODE_CONNECTOR_DisplayPort, 4096, 4096);
	count = drm_helper_probe_single_connector_modes(&g_dev, c, 4096, 4096);
	CHECK(c->nmodes > 0 && c->modes[0].hdisplay == 640 && c->modes[0].vdisplay == 480 &&
		      (c->modes[0].type & DRM_MODE_TYPE_PREFERRED),
	      "probe: DisplayPort fail-safe 640x480 not first");

	/* unplugged: modes and EDID go, the epoch and the hotplug count move */
	epoch = c->epoch_counter;
	hotplug = g_dev.hotplug_epoch;
	set_edid_null_calls = 0;
	fake_status = DRM_MODE_DISCONNECTED;
	count = drm_helper_probe_single_connector_modes(&g_dev, c, 4096, 4096);
	CHECK(count == 0 && c->nmodes == 0 && !c->connected, "probe: disconnected keeps modes");
	CHECK(set_edid_null_calls == 1, "probe: EDID not removed");
	CHECK(c->epoch_counter == epoch + 1, "probe: epoch did not move");
	CHECK(g_dev.hotplug_epoch == hotplug + 1, "probe: no hotplug event");

	/* the hotplug interrupt helper: changed once, then not */
	fake_status = DRM_MODE_CONNECTED;
	CHECK(drm_connector_helper_hpd_irq_event(&g_dev, c), "hpd: change not seen");
	CHECK(c->connected, "hpd: status not updated");
	CHECK(!drm_connector_helper_hpd_irq_event(&g_dev, c), "hpd: change seen twice");

	/* a fixed panel */
	c = fresh_connector(DRM_MODE_CONNECTOR_eDP, 4096, 4096);
	{
		struct drm_display_mode fixed;

		drm_mode_copy(&fixed, drm_mode_find_dmt(1920, 1080, 60, false));
		fixed.width_mm = 344;
		fixed.height_mm = 194;
		fixed.type = DRM_MODE_TYPE_DRIVER;
		CHECK(drm_connector_helper_get_modes_fixed(&g_dev, c, &fixed) == 1,
		      "fixed: not added");
		CHECK(c->nmodes == 1 && (c->modes[0].type & DRM_MODE_TYPE_PREFERRED) &&
			      c->mm_width == 344 && c->display_info.height_mm == 194,
		      "fixed: mode or size wrong");
		CHECK(drm_crtc_helper_mode_valid_fixed(drm_mode_find_dmt(1280, 1024, 60, false),
						       &fixed) == MODE_ONE_SIZE,
		      "fixed: other size accepted");
	}

	g_drv.detect = NULL;
	g_drv.get_modes = NULL;
}

int main(int argc, char **argv)
{
	int verbose = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v';

	test_corpus(verbose);
	test_420();
	test_validate_list();
	test_probe();
	if (fails) {
		printf("probe: %d checks failed\n", fails);
		return 1;
	}
	printf("probe: all tests passed\n");
	return 0;
}
