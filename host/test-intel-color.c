/*
 * Tests for the colour management of the Intel display pipes
 * (kernel/dev/gpu/i915/display/intel_color.c), compiled into this file
 * and run over a recorded register file: the entry formats and the CTM
 * coefficient conversion against independent arithmetic, the sizes each
 * platform offers, the checks, what the pipe gets written for each
 * platform's tables and matrix, the order of the double-buffered writes,
 * the vblank and the tables on a pipe that stays up, and the palette path
 * that must stay exactly what it always wrote.  See test-intel-color.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "../kernel/dev/gpu/i915/display/intel_color.c"

/* The host C library's declarations, spelled out: its headers disagree
 * with the kernel's fixed-width types, which come first. */
extern int printf(const char *, ...);
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);
extern void *malloc(unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void free(void *);
extern void abort(void);
extern void *memset(void *, int, unsigned long);
extern int strncmp(const char *, const char *, unsigned long);

static int fails, checks;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                 \
			fails++;                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

/* ---- what the file under test needs from the kernel ---------------------- */

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

void mm_memset(void *dest, int val, size_t len)
{
	memset(dest, val, len);
}

/* The register file: every write is logged; a read is logged with the
 * READ bit so that the order of reads and writes can be checked.  A
 * vblank (the frame counters advancing) is logged as VBLANK. */
#define NREGS (0x80000 / 4)
#define LOG_MAX (1 << 18)
#define LOG_READ 0x80000000u
#define LOG_VBLANK 0xffffffffu
static uint32_t regs[NREGS];
static struct {
	uint32_t reg, val;
} wlog[LOG_MAX];
static int nlog;
static unsigned delay_calls;

static void log_add(uint32_t reg, uint32_t val)
{
	if (nlog < LOG_MAX) {
		wlog[nlog].reg = reg;
		wlog[nlog].val = val;
		nlog++;
	}
}

uint32_t i915_read32(struct i915_device *i915, uint32_t reg)
{
	(void)i915;
	if (reg / 4 >= NREGS)
		test_fatal("read beyond the register file", "i915_read32");
	log_add(reg | LOG_READ, regs[reg / 4]);
	return regs[reg / 4];
}

void i915_write32(struct i915_device *i915, uint32_t reg, uint32_t val)
{
	(void)i915;
	if (reg / 4 >= NREGS)
		test_fatal("write beyond the register file", "i915_write32");
	regs[reg / 4] = val;
	log_add(reg, val);
}

/* Time passes: every 50 calls a frame ends on every pipe. */
void lapic_delay_us(uint32_t us)
{
	(void)us;
	if (++delay_calls % 50)
		return;
	for (int pipe = 0; pipe < 4; pipe++)
		regs[PIPE_FRMCOUNT(pipe) / 4]++;
	log_add(LOG_VBLANK, 0);
}

int intel_display_legacy_part(const struct i915_device *i915)
{
	if (i915->info->platform == I915_PLATFORM_HASWELL)
		return 0;
	return i915->info->gen < 8 || i915->info->platform == I915_PLATFORM_VALLEYVIEW ||
	       i915->info->platform == I915_PLATFORM_CHERRYVIEW;
}

static const char *rejected;

int intel_display_check_reject(struct i915_device *i915, const char *why)
{
	(void)i915;
	rejected = why;
	return -EINVAL;
}

/* The core's table check, as the core does it. */
int drm_color_lut_check(const struct drm_blob *lut, u32 tests)
{
	const struct drm_color_lut *entry;

	if (!lut || !tests)
		return 0;
	entry = lut->data;
	for (int i = 0; i < drm_color_lut_size(lut); i++) {
		if ((tests & DRM_COLOR_LUT_EQUAL_CHANNELS) &&
		    (entry[i].red != entry[i].blue || entry[i].red != entry[i].green))
			return -EINVAL;
		if (i > 0 && (tests & DRM_COLOR_LUT_NON_DECREASING) &&
		    (entry[i].red < entry[i - 1].red || entry[i].green < entry[i - 1].green ||
		     entry[i].blue < entry[i - 1].blue))
			return -EINVAL;
	}
	return 0;
}

static struct {
	int calls;
	uint32_t degamma, gamma;
	bool ctm;
	int legacy_size;
} enabled;

int drm_crtc_enable_color_mgmt(struct drm_device *dev, struct drm_crtc *crtc,
			       uint32_t degamma_lut_size, bool has_ctm, uint32_t gamma_lut_size)
{
	(void)dev;
	(void)crtc;
	enabled.calls++;
	enabled.degamma = degamma_lut_size;
	enabled.ctm = has_ctm;
	enabled.gamma = gamma_lut_size;
	return 0;
}

int drm_mode_crtc_set_gamma_size(struct drm_crtc *crtc, int gamma_size)
{
	crtc->gamma_size = (uint32_t)gamma_size;
	enabled.legacy_size = gamma_size;
	return 0;
}

/* ---- the device under test ------------------------------------------------ */

static struct i915_device dev;
static struct intel_device_info info;

static struct i915_device *mk(int platform, int gen, int ver)
{
	memset(&dev, 0, sizeof(dev));
	memset(&info, 0, sizeof(info));
	memset(regs, 0, sizeof(regs));
	info.platform = (uint8_t)platform;
	info.gen = (uint8_t)gen;
	info.display_ver = (uint8_t)ver;
	info.num_pipes = 3;
	dev.info = &info;
	dev.drm.ncrtc = 2;
	nlog = 0;
	rejected = NULL;
	memset(&enabled, 0, sizeof(enabled));
	return &dev;
}

static struct intel_pipe *pipe_of(int pipe)
{
	struct intel_pipe *p = &dev.display.pipes[pipe];

	p->pipe = pipe;
	p->active = 1;
	return p;
}

/* blobs */
static struct drm_blob *lut_blob(uint32_t n, int kind)
{
	struct drm_blob *b = calloc(1, sizeof(*b));
	struct drm_color_lut *l = calloc(n, sizeof(*l));

	for (uint32_t i = 0; i < n; i++) {
		uint32_t v = (uint32_t)((uint64_t)i * 0xffff / (n - 1));

		switch (kind) {
		case 0: /* linear ramp, equal channels */
			l[i].red = l[i].green = l[i].blue = (uint16_t)v;
			break;
		case 1: /* channels differ */
			l[i].red = (uint16_t)v;
			l[i].green = (uint16_t)(v / 2);
			l[i].blue = (uint16_t)(0xffff - v);
			break;
		case 2: /* falls at the end */
			l[i].red = l[i].green = l[i].blue = (uint16_t)(i == n - 1 ? 0 : v);
			break;
		}
	}
	b->data = l;
	b->length = n * (uint32_t)sizeof(*l);
	return b;
}

static void blob_free(struct drm_blob *b)
{
	if (b) {
		free(b->data);
		free(b);
	}
}

/* S31.32 sign-magnitude */
static u64 s3132(double v)
{
	u64 sign = v < 0 ? CTM_COEFF_SIGN : 0;
	double a = v < 0 ? -v : v;

	return sign | (u64)(a * 4294967296.0 + 0.5);
}

static struct drm_blob *ctm_blob(const double m[9])
{
	struct drm_blob *b = calloc(1, sizeof(*b));
	struct drm_color_ctm *c = calloc(1, sizeof(*c));

	for (int i = 0; i < 9; i++)
		c->matrix[i] = s3132(m[i]);
	b->data = c;
	b->length = sizeof(*c);
	return b;
}

static struct drm_crtc_state *new_state(void)
{
	struct drm_crtc_state *cs = calloc(1, sizeof(*cs));

	for (int i = 0; i < 256; i++)
		cs->gamma[0][i] = cs->gamma[1][i] = cs->gamma[2][i] = (uint16_t)(i << 8 | i);
	cs->active = 1;
	cs->mode_valid = 1;
	return cs;
}

static int log_count(uint32_t reg)
{
	int n = 0;

	for (int i = 0; i < nlog; i++)
		if (wlog[i].reg == reg)
			n++;
	return n;
}

static int log_first(uint32_t reg)
{
	for (int i = 0; i < nlog; i++)
		if (wlog[i].reg == reg)
			return i;
	return -1;
}

static double fabs_(double v)
{
	return v < 0 ? -v : v;
}

/* ---- entry formats -------------------------------------------------------- */

static void test_entry_formats(void)
{
	struct drm_color_lut e = { 0xffff, 0x8080, 0, 0 };

	CHECK(i9xx_lut_8(&e) == 0xff8000, "8-bit entry %#x", i9xx_lut_8(&e));
	e = (struct drm_color_lut){ 0xffff, 0xffff, 0xffff, 0 };
	CHECK(ilk_lut_10(&e) == 0x3fffffff, "10-bit full scale %#x", ilk_lut_10(&e));

	/* every 16-bit value: 8 and 10 bits are the nearest code */
	for (uint32_t v = 0; v <= 0xffff; v += 7) {
		struct drm_color_lut x = { (uint16_t)v, (uint16_t)v, (uint16_t)v, 0 };
		uint32_t r10 = (ilk_lut_10(&x) >> 20) & 0x3ff;
		uint32_t r8 = (i9xx_lut_8(&x) >> 16) & 0xff;
		double want10 = (double)v * 1023.0 / 65535.0, want8 = (double)v * 255.0 / 65535.0;

		CHECK(fabs_((double)r10 - want10) <= 0.5 + 1e-9, "10-bit %u -> %u", v, r10);
		CHECK(fabs_((double)r8 - want8) <= 0.5 + 1e-9, "8-bit %u -> %u", v, r8);
		CHECK(((ilk_lut_10(&x) >> 10) & 0x3ff) == r10 && (ilk_lut_10(&x) & 0x3ff) == r10,
		      "10-bit channels equal for %u", v);

		/* 12.4: the two dwords hold the high 10 and the low 6 bits */
		uint32_t ldw = ilk_lut_12p4_ldw(&x), udw = ilk_lut_12p4_udw(&x);
		uint32_t red = ((udw >> 20) & 0x3ff) << 6 | ((ldw >> 24) & 0x3f);
		uint32_t green = ((udw >> 10) & 0x3ff) << 6 | ((ldw >> 14) & 0x3f);
		uint32_t blue = (udw & 0x3ff) << 6 | ((ldw >> 4) & 0x3f);

		CHECK(red == v && green == v && blue == v, "12.4 of %u: %u %u %u", v, red, green,
		      blue);

		/* degamma: 0.16 as is, 0.24 from 24 bits */
		CHECK(glk_degamma_lut(&x) == v, "degamma 16 of %u", v);
		CHECK(fabs_((double)mtl_degamma_lut(&x) - (double)v * 16777215.0 / 65535.0) <= 0.5 + 1e-6,
		      "degamma 24 of %u -> %u", v, mtl_degamma_lut(&x));
	}
}

/* ---- the CSC coefficient format ------------------------------------------ */

/* sign | exponent | 9-bit mantissa (bits 11:3) */
static double csc_decode(u16 c)
{
	static const int fbits[8] = { 9, 10, 11, 12, -1, -1, 7, 8 };
	int e = (c >> 12) & 7;
	double v;

	if (fbits[e] < 0)
		return 1e9;
	v = (double)((c & 0xff8) >> 3) / (double)(1u << fbits[e]);
	return (c & 0x8000) ? -v : v;
}

static void test_csc_format(void)
{
	struct intel_csc_matrix csc;
	struct drm_color_ctm ctm;

	/* identity */
	for (int i = 0; i < 9; i++)
		ctm.matrix[i] = s3132(i % 4 == 0 ? 1.0 : 0.0);
	ilk_csc_convert_ctm(&ctm, &csc, 0);
	CHECK(csc.coeff[0] == 0x7800 && csc.coeff[4] == 0x7800 && csc.coeff[8] == 0x7800,
	      "identity diagonal %#x %#x %#x", csc.coeff[0], csc.coeff[4], csc.coeff[8]);
	CHECK(csc.coeff[1] == 0x3000, "zero coefficient %#x", csc.coeff[1]);
	CHECK(csc.preoff[0] == 0 && csc.postoff[2] == 0, "full range offsets");

	/* a sweep: within half a step of the exponent the value falls in,
	 * clamped below 4.0 */
	for (int k = -4500; k <= 4500; k += 3) {
		double v = k / 1000.0;

		for (int i = 0; i < 9; i++)
			ctm.matrix[i] = s3132(v);
		ilk_csc_convert_ctm(&ctm, &csc, 0);
		double a = fabs_(v), got = csc_decode(csc.coeff[0]);
		int fb = a < 0.125 ? 12 : a < 0.25 ? 11 : a < 0.5 ? 10 : a < 1.0 ? 9 : a < 2.0 ? 8 : 7;
		double step = 1.0 / (double)(1u << fb);
		double want = a >= 4.0 ? (v < 0 ? -511.0 / 128.0 : 511.0 / 128.0) : v;
		/* the mantissa saturates at 511: the top of each exponent's range */
		double limit = 511.0 * step;

		if (a < 4.0 && a > limit)
			want = v < 0 ? -limit : limit;
		CHECK(fabs_(got - want) <= step / 2 + 1e-12, "CSC %f -> %#x = %f", v,
		      csc.coeff[0], got);
	}

	/* a limited-range output scales the matrix by 219/255 and adds 16 */
	for (int i = 0; i < 9; i++)
		ctm.matrix[i] = s3132(i % 4 == 0 ? 1.0 : 0.0);
	ilk_csc_convert_ctm(&ctm, &csc, 1);
	CHECK(fabs_(csc_decode(csc.coeff[0]) - 219.0 / 255.0) <= 1.0 / 1024,
	      "limited range 1.0 -> %f", csc_decode(csc.coeff[0]));
	CHECK(csc.postoff[0] == (16 << 4), "limited range post offset %#x", csc.postoff[0]);
}

/* ---- sizes per platform ----------------------------------------------------- */

static void test_sizes(void)
{
	static const struct {
		const char *name;
		int platform, gen, ver;
		uint32_t degamma, gamma; /* 0: not handled here */
	} t[] = {
		{ "Ivy Bridge", I915_PLATFORM_IVYBRIDGE, 7, 7, 0, 0 },
		{ "Valleyview", I915_PLATFORM_VALLEYVIEW, 7, 7, 0, 0 },
		{ "Cherryview", I915_PLATFORM_CHERRYVIEW, 8, 8, 0, 0 },
		{ "Haswell", I915_PLATFORM_HASWELL, 7, 7, 1024, 1024 },
		{ "Broadwell", I915_PLATFORM_BROADWELL, 8, 8, 1024, 1024 },
		{ "Skylake", I915_PLATFORM_SKYLAKE, 9, 9, 1024, 1024 },
		{ "Broxton", I915_PLATFORM_BROXTON, 9, 9, 1024, 1024 },
		{ "Gemini Lake", I915_PLATFORM_GEMINILAKE, 9, 10, 33, 1024 },
		{ "Ice Lake", I915_PLATFORM_ICELAKE, 11, 11, 33, 262145 },
		{ "Tiger Lake", I915_PLATFORM_TIGERLAKE, 12, 12, 33, 262145 },
		{ "Alder Lake-P", I915_PLATFORM_ALDERLAKE_P, 12, 13, 129, 1024 },
		{ "Meteor Lake", I915_PLATFORM_METEORLAKE, 12, 14, 129, 1024 },
		{ "Lunar Lake", I915_PLATFORM_LUNARLAKE, 20, 20, 129, 1024 },
		{ "Panther Lake", I915_PLATFORM_PANTHERLAKE, 30, 30, 129, 1024 },
	};

	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		struct i915_device *i915 = mk(t[i].platform, t[i].gen, t[i].ver);
		struct drm_driver drv;
		uint32_t degamma = t[i].degamma, gamma = t[i].gamma;

		memset(&drv, 0, sizeof(drv));
		drv.gamma_size = 256;
#if !I915_FEAT_COLOR_MGMT
		degamma = gamma = 0;
#endif
#if !I915_FEAT_COLOR_ICL_MULTISEG
		if (gamma == 262145)
			gamma = 1024;
#endif
		intel_color_driver_setup(i915, &drv);
		int rc = intel_color_crtc_init(i915, 0);

		if (!degamma) {
			CHECK(drv.features == 0 && drv.gamma_size == 256 && drv.degamma_size == 0 &&
				      !drv.color_has_ctm,
			      "%s: driver table left alone", t[i].name);
			CHECK(rc == 0 && enabled.calls == 0, "%s: no crtc colour properties", t[i].name);
			continue;
		}
		CHECK(drv.features & DRM_FEATURE_COLOR_MGMT, "%s: COLOR_MGMT", t[i].name);
		CHECK(drv.color_has_ctm, "%s: CTM", t[i].name);
		CHECK(drv.degamma_size == degamma, "%s: degamma %u", t[i].name, drv.degamma_size);
		CHECK(drv.gamma_size == gamma, "%s: gamma %u", t[i].name, drv.gamma_size);
		CHECK(rc == 0 && enabled.calls == 1 && enabled.degamma == degamma &&
			      enabled.gamma == gamma && enabled.ctm && enabled.legacy_size == 256,
		      "%s: crtc init %d %u/%u/%d legacy %d", t[i].name, rc, enabled.degamma,
		      enabled.gamma, enabled.ctm, enabled.legacy_size);
	}
}

/* ---- the palette path ----------------------------------------------------- */

/* What the pipes have always been given: GAMMA_MODE 8-bit and the 256
 * high bytes, nothing else, nothing read. */
static int palette_exact(int pipe, const struct drm_crtc_state *cs)
{
	if (nlog != 257 || wlog[0].reg != (uint32_t)GAMMA_MODE(pipe) || wlog[0].val != 0)
		return 0;
	for (int i = 0; i < 256; i++) {
		uint32_t want = ((uint32_t)(cs->gamma[0][i] >> 8) << 16) |
				((uint32_t)(cs->gamma[1][i] >> 8) << 8) | (uint32_t)(cs->gamma[2][i] >> 8);

		if (wlog[1 + i].reg != (uint32_t)LGC_PALETTE(pipe, i) || wlog[1 + i].val != want)
			return 0;
	}
	return 1;
}

static void test_palette_path(void)
{
	static const int plat[][3] = {
		{ I915_PLATFORM_HASWELL, 7, 7 },	{ I915_PLATFORM_BROADWELL, 8, 8 },
		{ I915_PLATFORM_SKYLAKE, 9, 9 },	{ I915_PLATFORM_GEMINILAKE, 9, 10 },
		{ I915_PLATFORM_ICELAKE, 11, 11 },	{ I915_PLATFORM_TIGERLAKE, 12, 12 },
		{ I915_PLATFORM_METEORLAKE, 12, 14 }, { I915_PLATFORM_PANTHERLAKE, 30, 30 },
	};

	for (unsigned k = 0; k < sizeof(plat) / sizeof(plat[0]); k++) {
		struct i915_device *i915 = mk(plat[k][0], plat[k][1], plat[k][2]);
		struct intel_pipe *p = pipe_of(1);
		struct drm_crtc_state *cs = new_state();

		for (int i = 0; i < 256; i++) {
			cs->gamma[0][i] = (uint16_t)(i * 200);
			cs->gamma[1][i] = (uint16_t)(0xffff - i * 3);
			cs->gamma[2][i] = (uint16_t)(i << 8);
		}
		CHECK(intel_color_check(i915, NULL, cs) == 0, "plat %d: no tables accepted", k);
		intel_color_commit(i915, p, cs, 1);
		CHECK(palette_exact(1, cs), "plat %d: mode set writes the palette as always (%d)", k,
		      nlog);
		nlog = 0;
		intel_color_commit(i915, p, cs, 0);
		CHECK(nlog == 0, "plat %d: unchanged table, pipe up: nothing written", k);
		cs->gamma_changed = 1;
		intel_color_commit(i915, p, cs, 0);
		CHECK(palette_exact(1, cs), "plat %d: changed table written as always", k);
		CHECK(!p->color.managed, "plat %d: palette path", k);
		CHECK(intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_CTL) ==
				      (PLANE_CTL_PLANE_GAMMA_DISABLE | PLANE_CTL_PIPE_GAMMA_ENABLE) &&
			      intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_COLOR_CTL) ==
				      (PLANE_COLOR_PIPE_GAMMA_ENABLE | PLANE_COLOR_PLANE_GAMMA_DISABLE) &&
			      intel_color_plane_bits(i915, p, INTEL_COLOR_REG_DSPCNTR) ==
				      DISP_PIPE_GAMMA_ENABLE &&
			      intel_color_plane_bits(i915, p, INTEL_COLOR_REG_CUR_CTL) == CUR_GAMMA_ENABLE,
		      "plat %d: plane bits as always", k);
		free(cs);
	}
}

#if I915_FEAT_COLOR_MGMT

/* ---- the checks --------------------------------------------------------------- */

static void test_checks(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_SKYLAKE, 9, 9);
	struct drm_crtc_state *cs = new_state();
	static const double ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

	cs->color_mgmt_changed = 1;
	cs->gamma_lut = lut_blob(1024, 1);
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Skylake: 1024 gamma, channels differ");
	blob_free(cs->gamma_lut);
	cs->gamma_lut = lut_blob(512, 0);
	CHECK(intel_color_check(i915, NULL, cs) == -EINVAL && rejected, "Skylake: 512 refused");
	blob_free(cs->gamma_lut);
	cs->gamma_lut = lut_blob(256, 2);
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Skylake: legacy table never tested");
	blob_free(cs->gamma_lut);
	cs->gamma_lut = NULL;
	cs->degamma_lut = lut_blob(1024, 1);
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Skylake: degamma channels may differ");
	blob_free(cs->degamma_lut);
	cs->degamma_lut = NULL;
	cs->ctm = ctm_blob(ident);
	cs->ctm->length = 8;
	CHECK(intel_color_check(i915, NULL, cs) == -EINVAL, "short CTM refused");
	blob_free(cs->ctm);
	cs->ctm = NULL;

	/* nothing changed: not looked at */
	cs->color_mgmt_changed = 0;
	cs->gamma_lut = lut_blob(77, 0);
	CHECK(intel_color_check(i915, NULL, cs) == 0, "unchanged state not checked again");
	cs->mode_changed = 1;
	CHECK(intel_color_check(i915, NULL, cs) == -EINVAL, "a mode set checks again");
	blob_free(cs->gamma_lut);
	cs->gamma_lut = NULL;
	cs->mode_changed = 0;
	cs->color_mgmt_changed = 1;

	i915 = mk(I915_PLATFORM_GEMINILAKE, 9, 10);
	cs->degamma_lut = lut_blob(33, 1);
	CHECK(intel_color_check(i915, NULL, cs) == -EINVAL, "Gemini Lake: degamma channels equal");
	blob_free(cs->degamma_lut);
	cs->degamma_lut = lut_blob(33, 2);
	CHECK(intel_color_check(i915, NULL, cs) == -EINVAL, "Gemini Lake: degamma must not fall");
	blob_free(cs->degamma_lut);
	cs->degamma_lut = lut_blob(33, 0);
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Gemini Lake: linear degamma");
	blob_free(cs->degamma_lut);
	cs->degamma_lut = NULL;

	i915 = mk(I915_PLATFORM_ICELAKE, 11, 11);
#if I915_FEAT_COLOR_ICL_MULTISEG
	cs->gamma_lut = lut_blob(262145, 2);
	CHECK(intel_color_check(i915, NULL, cs) == -EINVAL, "Ice Lake: multi-segment must not fall");
	blob_free(cs->gamma_lut);
	cs->gamma_lut = lut_blob(1024, 0);
	CHECK(intel_color_check(i915, NULL, cs) == -EINVAL, "Ice Lake: 1024 refused");
	blob_free(cs->gamma_lut);
#else
	cs->gamma_lut = lut_blob(1024, 2);
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Ice Lake 10-bit: may fall");
	blob_free(cs->gamma_lut);
#endif
	cs->gamma_lut = lut_blob(256, 1);
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Ice Lake: legacy table");
	blob_free(cs->gamma_lut);
	cs->gamma_lut = NULL;

	i915 = mk(I915_PLATFORM_METEORLAKE, 12, 14);
	cs->degamma_lut = lut_blob(33, 0);
	CHECK(intel_color_check(i915, NULL, cs) == -EINVAL, "Meteor Lake: degamma 33 refused");
	blob_free(cs->degamma_lut);
	cs->degamma_lut = lut_blob(129, 0);
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Meteor Lake: degamma 129");
	blob_free(cs->degamma_lut);
	free(cs);
}

/* ---- what the pipe gets ------------------------------------------------------- */

static void test_skl_gamma_modeset(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_SKYLAKE, 9, 9);
	struct intel_pipe *p = pipe_of(0);
	struct drm_crtc_state *cs = new_state();

	cs->gamma_lut = lut_blob(1024, 1);
	cs->color_mgmt_changed = 1;
	intel_color_commit(i915, p, cs, 1);
	CHECK(regs[GAMMA_MODE(0) / 4] == GAMMA_MODE_MODE_10BIT, "Skylake: 10-bit mode");
	CHECK(regs[PIPE_CSC_MODE(0) / 4] == CSC_POSITION_BEFORE_GAMMA, "Skylake: CSC before gamma");
	CHECK(regs[SKL_BOTTOM_COLOR(0) / 4] == SKL_BOTTOM_COLOR_GAMMA_ENABLE, "bottom colour %#x",
	      regs[SKL_BOTTOM_COLOR(0) / 4]);
	CHECK(log_count(PREC_PAL_DATA(0)) == 1024, "1024 entries, %d", log_count(PREC_PAL_DATA(0)));
	int i0 = log_first(PREC_PAL_INDEX(0));
	CHECK(i0 >= 0 && wlog[i0].val == 0 && wlog[i0 + 1].val == PAL_PREC_AUTO_INCREMENT,
	      "index then auto-increment");
	const struct drm_color_lut *l = cs->gamma_lut->data;
	int ok = 1, k = 0;
	for (int i = 0; i < nlog; i++)
		if (wlog[i].reg == PREC_PAL_DATA(0))
			ok &= wlog[i].val == ilk_lut_10(&l[k++]);
	CHECK(ok, "entries in 10:10:10");
	CHECK(regs[PREC_PAL_INDEX(0) / 4] == 0, "index left at 0");
	CHECK(regs[PREC_PAL_EXT_GC_MAX(0, 2) / 4] == 1u << 16, "extended maximum 1.0");
	CHECK(log_first(PREC_PAL_DATA(0)) < log_first(GAMMA_MODE(0)), "mode set: table first");
	CHECK(log_count(LOG_VBLANK) == 0, "mode set: no vblank wait");
	CHECK(p->color.managed && p->color.gamma_enabled && !p->color.csc_enabled, "pipe state");
	CHECK(intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_CTL) ==
		      (PLANE_CTL_PLANE_GAMMA_DISABLE | PLANE_CTL_PIPE_GAMMA_ENABLE),
	      "plane bits");
	blob_free(cs->gamma_lut);
	free(cs);
}

static void test_hsw_split(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_HASWELL, 7, 7);
	struct intel_pipe *p = pipe_of(2);
	struct drm_crtc_state *cs = new_state();
	static const double m[9] = { 0.5, 0.25, 0.25, 0, 1, 0, -0.125, 0, 2.5 };

	cs->gamma_lut = lut_blob(1024, 0);
	cs->degamma_lut = lut_blob(1024, 1);
	cs->ctm = ctm_blob(m);
	cs->color_mgmt_changed = 1;
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Haswell: both tables and CTM");
	intel_color_commit(i915, p, cs, 1);
	CHECK(regs[GAMMA_MODE(2) / 4] == GAMMA_MODE_MODE_SPLIT, "split mode");
	CHECK(regs[PIPE_CSC_MODE(2) / 4] == 0, "CSC after the degamma half");
	CHECK(log_count(PREC_PAL_DATA(2)) == 1024, "two halves of 512");
	/* every entry with its own index, no auto-increment */
	int ok = 1, n = 0;
	for (int i = 0; i < nlog; i++)
		if (wlog[i].reg == PREC_PAL_DATA(2)) {
			uint32_t want = PAL_PREC_SPLIT_MODE | (uint32_t)n;
			ok &= wlog[i - 1].reg == PREC_PAL_INDEX(2) && wlog[i - 1].val == want;
			n++;
		}
	CHECK(ok, "index per entry, split, 0..1023");
	/* the degamma half resampled from 1024 to 512 */
	const struct drm_color_lut *dl = cs->degamma_lut->data;
	int first = log_first(PREC_PAL_DATA(2));
	struct drm_color_lut e511 = dl[511 * 1023 / 511];
	CHECK(wlog[first + 2 * 511].val == ilk_lut_10(&e511), "resampled entry 511");
	CHECK(regs[PIPE_CSC_COEFF_RY_GY(2) / 4] >> 16 == 0x0800, "c0 0.5 = %#x",
	      regs[PIPE_CSC_COEFF_RY_GY(2) / 4] >> 16);
	CHECK(csc_decode((u16)(regs[PIPE_CSC_COEFF_BV(2) / 4] >> 16)) == 2.5, "c8 2.5 = %#x",
	      regs[PIPE_CSC_COEFF_BV(2) / 4] >> 16);
	CHECK(csc_decode((u16)(regs[PIPE_CSC_COEFF_RV_GV(2) / 4] >> 16)) == -0.125, "c6 -0.125");
	CHECK(intel_color_plane_bits(i915, p, INTEL_COLOR_REG_DSPCNTR) ==
		      (DISP_PIPE_GAMMA_ENABLE | DISP_PIPE_CSC_ENABLE),
	      "DSPCNTR gamma and CSC");
	CHECK(intel_color_plane_bits(i915, p, INTEL_COLOR_REG_CUR_CTL) ==
		      (CUR_GAMMA_ENABLE | MCURSOR_PIPE_CSC_ENABLE),
	      "cursor gamma and CSC");
	blob_free(cs->gamma_lut);
	blob_free(cs->degamma_lut);
	blob_free(cs->ctm);
	free(cs);
}

static void test_glk_ctm_only(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_GEMINILAKE, 9, 10);
	struct intel_pipe *p = pipe_of(0);
	struct drm_crtc_state *cs = new_state();
	static const double m[9] = { 0, 1, 0, 1, 0, 0, 0, 0, 1 };

	cs->ctm = ctm_blob(m);
	cs->color_mgmt_changed = 1;
	intel_color_commit(i915, p, cs, 1);
	CHECK(regs[GAMMA_MODE(0) / 4] == GAMMA_MODE_MODE_8BIT, "8-bit, gamma off");
	CHECK(regs[SKL_BOTTOM_COLOR(0) / 4] == SKL_BOTTOM_COLOR_CSC_ENABLE, "bottom: CSC only");
	CHECK(log_count(PRE_CSC_GAMC_DATA(0)) == 35, "linear degamma 33 + 2 clamps, %d",
	      log_count(PRE_CSC_GAMC_DATA(0)));
	int ok = 1, n = 0;
	for (int i = 0; i < nlog; i++)
		if (wlog[i].reg == PRE_CSC_GAMC_DATA(0)) {
			uint32_t want = n < 33 ? 0xffffu * (uint32_t)n / 32 : 1u << 16;
			ok &= wlog[i].val == want;
			n++;
		}
	CHECK(ok, "linear degamma values");
	CHECK(log_count(LGC_PALETTE(0, 0)) == 0, "no palette written");
	CHECK(regs[PIPE_CSC_COEFF_RY_GY(0) / 4] == (0x3000u << 16 | 0x7800), "swap matrix row 0 %#x",
	      regs[PIPE_CSC_COEFF_RY_GY(0) / 4]);
	CHECK((regs[PLANE_COLOR_CTL(0, 0) / 4] &
	       (PLANE_COLOR_PIPE_GAMMA_ENABLE | PLANE_COLOR_PIPE_CSC_ENABLE |
		PLANE_COLOR_PLANE_GAMMA_DISABLE)) ==
		      (PLANE_COLOR_PIPE_CSC_ENABLE | PLANE_COLOR_PLANE_GAMMA_DISABLE),
	      "Gemini Lake PLANE_COLOR_CTL %#x", regs[PLANE_COLOR_CTL(0, 0) / 4]);
	CHECK(intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_CTL) == 0,
	      "no pipe bits in PLANE_CTL on Gemini Lake");
	blob_free(cs->ctm);
	free(cs);
}

static void test_icl(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_ICELAKE, 11, 11);
	struct intel_pipe *p = pipe_of(1);
	struct drm_crtc_state *cs = new_state();
	uint32_t n = I915_FEAT_COLOR_ICL_MULTISEG ? 262145 : 1024;

	cs->gamma_lut = lut_blob(n, 0);
	cs->degamma_lut = lut_blob(33, 0);
	cs->color_mgmt_changed = 1;
	CHECK(intel_color_check(i915, NULL, cs) == 0, "Ice Lake tables");
	intel_color_commit(i915, p, cs, 1);
	CHECK(log_count(PRE_CSC_GAMC_DATA(1)) == 35, "degamma 33 + 2");
#if I915_FEAT_COLOR_ICL_MULTISEG
	CHECK(regs[GAMMA_MODE(1) / 4] ==
		      (PRE_CSC_GAMMA_ENABLE | POST_CSC_GAMMA_ENABLE | GAMMA_MODE_MODE_12BIT_MULTI_SEG),
	      "multi-segment mode %#x", regs[GAMMA_MODE(1) / 4]);
	CHECK(log_count(PREC_PAL_MULTI_SEG_DATA(1)) == 18, "super-fine 9 x 2");
	CHECK(log_count(PREC_PAL_DATA(1)) == 1024, "fine and coarse 2 x 256 x 2");
	const struct drm_color_lut *l = cs->gamma_lut->data;
	int f = log_first(PREC_PAL_DATA(1));
	CHECK(wlog[f].val == ilk_lut_12p4_ldw(&l[8]) && wlog[f + 1].val == ilk_lut_12p4_udw(&l[8]),
	      "fine segment starts at entry 8");
	CHECK(wlog[f + 512 + 2].val == ilk_lut_12p4_ldw(&l[1024]), "coarse entry 1 is entry 1024");
	CHECK(regs[PREC_PAL_GC_MAX(1, 0) / 4] == l[262144].red, "maximum from the last entry");
#else
	CHECK(regs[GAMMA_MODE(1) / 4] ==
		      (PRE_CSC_GAMMA_ENABLE | POST_CSC_GAMMA_ENABLE | GAMMA_MODE_MODE_10BIT),
	      "10-bit mode %#x", regs[GAMMA_MODE(1) / 4]);
	CHECK(log_count(PREC_PAL_DATA(1)) == 1024, "1024 entries");
#endif
	CHECK(regs[PREC_PAL_EXT2_GC_MAX(1, 1) / 4] == 1u << 16, "ext2 maximum");
	CHECK(regs[PIPE_CSC_MODE(1) / 4] == 0, "no CSC");
	CHECK(intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_COLOR_CTL) ==
		      PLANE_COLOR_PLANE_GAMMA_DISABLE,
	      "no pipe bits in the planes");

	/* a CTM on the running pipe: disarm read before the coefficients,
	 * and after the vblank */
	static const double ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	cs->ctm = ctm_blob(ident);
	intel_color_commit(i915, p, cs, 0);
	nlog = 0;
	intel_color_commit(i915, p, cs, 0);
	int rd = log_first(PIPE_CSC_PREOFF_HI(1) | LOG_READ);
	int wr = log_first(PIPE_CSC_PREOFF_HI(1));
	CHECK(rd >= 0 && wr > rd, "Ice Lake: CSC disarmed before it is written (%d %d)", rd, wr);
	blob_free(cs->gamma_lut);
	blob_free(cs->degamma_lut);
	blob_free(cs->ctm);
	free(cs);
}

static void test_mtl_degamma(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_METEORLAKE, 12, 14);
	struct intel_pipe *p = pipe_of(0);
	struct drm_crtc_state *cs = new_state();

	cs->degamma_lut = lut_blob(129, 0);
	cs->color_mgmt_changed = 1;
	intel_color_commit(i915, p, cs, 1);
	CHECK(regs[GAMMA_MODE(0) / 4] == (PRE_CSC_GAMMA_ENABLE | GAMMA_MODE_MODE_8BIT),
	      "degamma only %#x", regs[GAMMA_MODE(0) / 4]);
	CHECK(log_count(PRE_CSC_GAMC_DATA(0)) == 131, "129 + 2");
	const struct drm_color_lut *l = cs->degamma_lut->data;
	int ok = 1, n = 0;
	for (int i = 0; i < nlog; i++)
		if (wlog[i].reg == PRE_CSC_GAMC_DATA(0)) {
			uint32_t want = n < 129 ? drm_color_lut_extract(l[n].green, 24) : 1u << 24;
			ok &= wlog[i].val == want;
			n++;
		}
	CHECK(ok, "24-bit degamma entries");
	blob_free(cs->degamma_lut);
	free(cs);
}

/* ---- a pipe that stays up ----------------------------------------------------- */

static void test_fast_path_order(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_SKYLAKE, 9, 9);
	struct intel_pipe *p = pipe_of(0);
	struct drm_crtc_state *cs = new_state();
	static const double ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

	/* the palette path, then a 1024 table: modes, vblank, table */
	intel_color_commit(i915, p, cs, 1);
	regs[PLANE_CTL(0, 0) / 4] = PLANE_CTL_ENABLE | PLANE_CTL_PLANE_GAMMA_DISABLE |
				     PLANE_CTL_PIPE_GAMMA_ENABLE;
	regs[PLANE_SURF(0, 0) / 4] = 0x123000;
	nlog = 0;
	cs->gamma_lut = lut_blob(1024, 0);
	cs->color_mgmt_changed = 1;
	intel_color_commit(i915, p, cs, 0);
	int mode = log_first(GAMMA_MODE(0)), vbl = log_first(LOG_VBLANK);
	int data = log_first(PREC_PAL_DATA(0));
	CHECK(mode >= 0 && vbl > mode && data > vbl, "modes %d, vblank %d, table %d", mode, vbl,
	      data);
	CHECK(log_count(PLANE_SURF(0, 0)) == 0, "plane bits unchanged: no re-arm");

	/* to the CTM alone: gamma off in the plane, CSC on, re-armed */
	blob_free(cs->gamma_lut);
	cs->gamma_lut = NULL;
	cs->ctm = ctm_blob(ident);
	nlog = 0;
	intel_color_commit(i915, p, cs, 0);
	CHECK(regs[PLANE_CTL(0, 0) / 4] ==
		      (PLANE_CTL_ENABLE | PLANE_CTL_PLANE_GAMMA_DISABLE | PLANE_CTL_PIPE_CSC_ENABLE),
	      "plane CTL %#x", regs[PLANE_CTL(0, 0) / 4]);
	CHECK(log_count(PLANE_SURF(0, 0)) == 1 && regs[PLANE_SURF(0, 0) / 4] == 0x123000,
	      "plane re-armed with its surface");
	CHECK(log_first(PIPE_CSC_COEFF_RY_GY(0)) < log_first(PIPE_CSC_MODE(0)),
	      "coefficients before CSC_MODE");
	CHECK(!p->color.lut_active, "no table read now");

	/* back to a table: loaded first (nothing reads one), no vblank */
	cs->gamma_lut = lut_blob(1024, 0);
	nlog = 0;
	intel_color_commit(i915, p, cs, 0);
	CHECK(log_first(PREC_PAL_DATA(0)) >= 0 &&
		      log_first(PREC_PAL_DATA(0)) < log_first(GAMMA_MODE(0)) &&
		      log_count(LOG_VBLANK) == 0,
	      "table preloaded while unused");

	/* back to the palette: CSC out, bottom through gamma, 8-bit, then
	 * the palette after the vblank; plane bits as always */
	blob_free(cs->gamma_lut);
	blob_free(cs->ctm);
	cs->gamma_lut = cs->ctm = NULL;
	nlog = 0;
	intel_color_commit(i915, p, cs, 0);
	CHECK(regs[PIPE_CSC_MODE(0) / 4] == 0 && regs[GAMMA_MODE(0) / 4] == 0 &&
		      regs[SKL_BOTTOM_COLOR(0) / 4] == SKL_BOTTOM_COLOR_GAMMA_ENABLE,
	      "palette path registers");
	CHECK(log_first(LGC_PALETTE(0, 0)) > log_first(LOG_VBLANK) && log_count(LGC_PALETTE(0, 255)) == 1,
	      "palette after the vblank");
	CHECK(!p->color.managed, "palette path again");
	CHECK(regs[PLANE_CTL(0, 0) / 4] ==
		      (PLANE_CTL_ENABLE | PLANE_CTL_PLANE_GAMMA_DISABLE | PLANE_CTL_PIPE_GAMMA_ENABLE),
	      "plane CTL as always %#x", regs[PLANE_CTL(0, 0) / 4]);
	/* and from there on exactly as always */
	nlog = 0;
	cs->color_mgmt_changed = 0;
	cs->gamma_changed = 1;
	intel_color_commit(i915, p, cs, 0);
	CHECK(palette_exact(0, cs), "palette path writes as always after a client's tables");
	free(cs);

	/* display 30: double-buffered tables, no wait */
	i915 = mk(I915_PLATFORM_PANTHERLAKE, 30, 30);
	p = pipe_of(0);
	cs = new_state();
	intel_color_commit(i915, p, cs, 1);
	nlog = 0;
	cs->gamma_lut = lut_blob(1024, 0);
	cs->color_mgmt_changed = 1;
	intel_color_commit(i915, p, cs, 0);
	CHECK(log_count(LOG_VBLANK) == 0 && log_first(PREC_PAL_DATA(0)) > log_first(GAMMA_MODE(0)),
	      "display 30: table after the modes, no wait");
	blob_free(cs->gamma_lut);
	free(cs);
}

static void test_legacy_lut(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_TIGERLAKE, 12, 12);
	struct intel_pipe *p = pipe_of(0);
	struct drm_crtc_state *cs = new_state();

	cs->gamma_lut = lut_blob(256, 1);
	cs->color_mgmt_changed = 1;
	intel_color_commit(i915, p, cs, 1);
	CHECK(regs[GAMMA_MODE(0) / 4] == (POST_CSC_GAMMA_ENABLE | GAMMA_MODE_MODE_8BIT),
	      "legacy table: 8-bit, enabled");
	const struct drm_color_lut *l = cs->gamma_lut->data;
	int ok = 1;
	for (int i = 0; i < 256; i++)
		ok &= regs[LGC_PALETTE(0, i) / 4] == i9xx_lut_8(&l[i]);
	CHECK(ok, "legacy table into the palette");
	blob_free(cs->gamma_lut);
	free(cs);
}

#else /* !I915_FEAT_COLOR_MGMT */

/* Off: whatever blobs a state holds, the pipe gets the palette from the
 * legacy table, exactly as always. */
static void test_feature_off(void)
{
	struct i915_device *i915 = mk(I915_PLATFORM_SKYLAKE, 9, 9);
	struct intel_pipe *p = pipe_of(0);
	struct drm_crtc_state *cs = new_state();
	static const double ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

	cs->gamma_lut = lut_blob(77, 1);
	cs->degamma_lut = lut_blob(5, 1);
	cs->ctm = ctm_blob(ident);
	cs->color_mgmt_changed = 1;
	CHECK(intel_color_check(i915, NULL, cs) == 0, "off: nothing checked");
	intel_color_commit(i915, p, cs, 1);
	CHECK(palette_exact(0, cs), "off: mode set writes the palette");
	nlog = 0;
	intel_color_commit(i915, p, cs, 0);
	CHECK(nlog == 0 && log_count(PIPE_CSC_MODE(0)) == 0 && log_first(GAMMA_MODE(0)) < 0,
	      "off: a colour change alone writes nothing");
	blob_free(cs->gamma_lut);
	blob_free(cs->degamma_lut);
	blob_free(cs->ctm);
	free(cs);
}

#endif /* I915_FEAT_COLOR_MGMT */

int main(void)
{
	test_entry_formats();
	test_csc_format();
	test_sizes();
	test_palette_path();
#if I915_FEAT_COLOR_MGMT
	test_checks();
	test_skl_gamma_modeset();
	test_hsw_split();
	test_glk_ctm_only();
	test_icl();
	test_mtl_degamma();
	test_fast_path_order();
	test_legacy_lut();
#else
	test_feature_off();
#endif
	CHECK(warnings == 0, "no warnings (%d)", warnings);
	printf("%d checks, %d failures (COLOR_MGMT %d, ICL_MULTISEG %d)\n", checks, fails,
	       I915_FEAT_COLOR_MGMT, I915_FEAT_COLOR_ICL_MULTISEG);
	return fails != 0;
}
