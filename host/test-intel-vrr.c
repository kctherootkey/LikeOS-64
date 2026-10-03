/*
 * Tests for the variable refresh timing generator of the Intel display
 * transcoders (kernel/dev/gpu/i915/display/intel_vrr.c), compiled into
 * this file and run over a recorded register file: the range, guard band
 * and pipeline-full arithmetic against an independent restatement of the
 * generator's rules and against worked numbers, the capability verdict,
 * the register sequences of display 12, 14 and 30 (set up with the
 * transcoder, on, push, off, cleared after the transcoder), the display
 * 30 fixed-rate path that must stay exactly what it always wrote, a sink
 * without a range that must see no register touched, and the MSA
 * timing-ignore record.  With -DI915_FEAT_VRR=0 only the fixed-rate path
 * may write.  See test-intel-vrr.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "../kernel/dev/gpu/i915/display/intel_vrr.c"

/* The host C library's declarations, spelled out: its headers disagree
 * with the kernel's fixed-width types, which come first. */
extern int printf(const char *, ...);
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);
extern void abort(void);
extern void *memset(void *, int, unsigned long);

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
static int kprintfs;

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
	kprintfs++;
	return n;
}

#define NREGS (0x80000 / 4)
#define LOG_MAX 4096
#define LOG_READ 0x80000000u
static uint32_t regs[NREGS];
static struct {
	uint32_t reg, val;
} wlog[LOG_MAX];
static int nlog;

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

void lapic_delay_us(uint32_t us)
{
	(void)us;
}

int intel_display_legacy_part(const struct i915_device *i915)
{
	if (i915->info->platform == I915_PLATFORM_HASWELL)
		return 0;
	return i915->info->gen < 8 || i915->info->platform == I915_PLATFORM_VALLEYVIEW ||
	       i915->info->platform == I915_PLATFORM_CHERRYVIEW;
}

uint32_t intel_set_context_latency(struct i915_device *i915)
{
	return i915->info->display_ver >= 30 ? 1 : 0;
}

static int msa_calls, msa_rc;

int intel_dp_set_msa_timing_par_ignore(struct intel_output *o, bool enable)
{
	msa_calls++;
	o->msa_timing_par_ignore = enable ? 1 : 0;
	return msa_rc;
}

static struct drm_prop fake_prop;
static int attach_calls, set_calls, set_value;

int drm_connector_attach_vrr_capable_property(struct drm_connector *c)
{
	attach_calls++;
	c->vrr_capable_property = &fake_prop;
	return 0;
}

void drm_connector_set_vrr_capable_property(struct drm_connector *c, bool capable)
{
	if (!c->vrr_capable_property)
		return;
	set_calls++;
	set_value = capable;
}

/* ---- the device under test ------------------------------------------------ */

static struct i915_device dev;
static struct intel_device_info info;

static struct i915_device *mk(int ver)
{
	memset(&dev, 0, sizeof(dev));
	memset(&info, 0, sizeof(info));
	memset(regs, 0, sizeof(regs));
	info.platform = I915_PLATFORM_TIGERLAKE;
	info.gen = 12;
	info.display_ver = (uint8_t)ver;
	info.num_pipes = 3;
	dev.info = &info;
	dev.drm.nconn = 2;
	dev.display.nout = 2;
	dev.display.vbt.lfp_vrr = 1;
	for (int i = 0; i < 2; i++) {
		dev.display.outputs[i].conn = i;
		dev.display.outputs[i].pipe = -1;
		dev.display.outputs[i].crtc = -1;
		dev.display.pipes[i].output = -1;
	}
	nlog = 0;
	msa_calls = msa_rc = 0;
	attach_calls = set_calls = set_value = 0;
	kprintfs = 0;
	return &dev;
}

/* 2560x1440 at 144 Hz (2720 x 1525 total, 597.15 MHz). */
static const struct drm_mode_modeinfo mode_qhd144 = {
	.clock = 597150,
	.hdisplay = 2560, .hsync_start = 2608, .hsync_end = 2640, .htotal = 2720,
	.vdisplay = 1440, .vsync_start = 1443, .vsync_end = 1448, .vtotal = 1525,
	.vrefresh = 144,
};

/* A DisplayPort sink (no branch device, ignores the MSA timing) on
 * output 0 with the EDID range [min, max]. */
static struct intel_output *dp_sink(int edp, int min, int max)
{
	struct intel_output *o = &dev.display.outputs[0];

	o->present = 1;
	o->type = edp ? INTEL_OUTPUT_EDP : INTEL_OUTPUT_DP;
	o->is_edp = edp;
	o->detected = 1;
	o->dpcd[DP_DPCD_REV] = 0x14;
	o->dpcd[DP_DOWN_STREAM_PORT_COUNT] = DP_MSA_TIMING_PAR_IGNORED | 1;
	dev.drm.conn[0].display_info.monitor_range.min_vfreq = (uint16_t)min;
	dev.drm.conn[0].display_info.monitor_range.max_vfreq = (uint16_t)max;
	return o;
}

/* The pipe as crtc_modeset leaves it before transcoder_enable. */
static struct intel_pipe *pipe_for(int t, const struct drm_mode_modeinfo *m)
{
	struct intel_pipe *p = &dev.display.pipes[0];

	p->pipe = 0;
	p->transcoder = t;
	p->output = 0;
	p->mode = *m;
	p->active = 1;
	dev.display.outputs[0].active = 1;
	dev.display.outputs[0].pipe = 0;
	regs[TRANS_DDI_FUNC_CTL(t) / 4] = TRANS_DDI_FUNC_ENABLE;
	return p;
}

static int writes(void)
{
	int n = 0;

	for (int i = 0; i < nlog; i++)
		if (!(wlog[i].reg & LOG_READ))
			n++;
	return n;
}

/* The i-th write in the log (reads skipped); 0xdead.. when none. */
static uint32_t wreg(int i, uint32_t *val)
{
	for (int k = 0; k < nlog; k++) {
		if (wlog[k].reg & LOG_READ)
			continue;
		if (i-- == 0) {
			*val = wlog[k].val;
			return wlog[k].reg;
		}
	}
	*val = 0;
	return 0xdeadbeef;
}

#define EXPECT_WRITE(i, r, v)                                                  \
	do {                                                                   \
		uint32_t got_v, got_r = wreg((i), &got_v);                    \
		CHECK(got_r == (uint32_t)(r) && got_v == (uint32_t)(v),       \
		      "write %d: %#x = %#x, expected %#x = %#x", (i), got_r,   \
		      got_v, (uint32_t)(r), (uint32_t)(v));                    \
	} while (0)

/* ---- an independent restatement of the generator's rules ------------------ */

struct ref {
	int in_range, enable, vmin, vmax, flipline, guardband, pipeline_full;
	uint32_t hw_vmin, hw_vmax, hw_flipline, ctl;
};

static void ref_compute(int ver, const struct drm_mode_modeinfo *m, int min, int max,
			int vrr_enabled, struct ref *r)
{
	int scl = ver >= 30 ? 1 : 0;
	long long refresh = ((long long)m->clock * 1000 + (long long)m->htotal * m->vtotal / 2) /
			    ((long long)m->htotal * m->vtotal);
	int vmin = m->vtotal, vmax;

	memset(r, 0, sizeof(*r));
	if (!(max - min > 10) || refresh < min || refresh > max)
		return;
	r->in_range = 1;
	vmax = (int)((long long)m->clock * 1000 / ((long long)m->htotal * min));
	if (vmax < m->vtotal)
		vmax = m->vtotal;
	if (vrr_enabled && vmin < vmax) {
		r->enable = 1;
		r->vmin = vmin;
		r->vmax = vmax;
		r->flipline = vmin;
	} else {
		r->vmin = r->vmax = r->flipline = m->vtotal;
	}
	int gb = r->vmin - m->vdisplay;
	int hwmax = ver >= 13 ? 0xffff : 255 + 1 + 1;
	int vblmax = r->vmin - m->vdisplay - scl - (ver < 13 ? 1 : 0);
	if (gb > hwmax)
		gb = hwmax;
	if (gb > vblmax)
		gb = vblmax;
	r->guardband = gb;
	if (ver < 13)
		r->pipeline_full = gb - 1 - 1;
	int hv = ver >= 13 ? 0 : scl;
	r->hw_vmin = (uint32_t)(r->vmin - hv - (ver < 13 ? 1 : 0) - 1);
	r->hw_vmax = (uint32_t)(r->vmax - hv - 1);
	r->hw_flipline = (uint32_t)(r->flipline - hv - 1);
	if (ver >= 14)
		r->ctl = (1u << 29) | (uint32_t)gb;
	else if (ver == 13)
		r->ctl = (1u << 30) | (1u << 29) | (uint32_t)gb;
	else
		r->ctl = (1u << 30) | (1u << 29) | ((uint32_t)r->pipeline_full << 3) | 1u;
}

/* ---- the tests ------------------------------------------------------------ */

static void test_worked_numbers(void)
{
	struct vrr_config c;

	/* refresh 597150000 / (2720 * 1525) = 143.96 -> 144; the longest
	 * frame at 48 Hz is 597150000 / (2720 * 48) = 4573 lines */
	CHECK(vrr_mode_vrefresh(&mode_qhd144) == 144, "refresh %d", vrr_mode_vrefresh(&mode_qhd144));
	vrr_compute_timings(12, 0, &mode_qhd144, 1, 48, 1, &c);
	CHECK(c.enable && c.vmin == 1525 && c.vmax == 4573 && c.flipline == 1525,
	      "display 12 range %d %d %d", c.vmin, c.vmax, c.flipline);
	/* 85 lines of vblank, one lost to the extra delay; pipeline full
	 * two less */
	CHECK(c.guardband == 84 && c.pipeline_full == 82, "display 12 guard band %d / %d",
	      c.guardband, c.pipeline_full);
	CHECK(vrr_trans_ctl(12, &c) == 0x60000291u, "display 12 control %#x", vrr_trans_ctl(12, &c));
	vrr_compute_timings(14, 0, &mode_qhd144, 1, 48, 1, &c);
	CHECK(c.guardband == 85 && c.pipeline_full == 0, "display 14 guard band %d", c.guardband);
	CHECK(vrr_trans_ctl(14, &c) == 0x20000055u, "display 14 control %#x", vrr_trans_ctl(14, &c));
	CHECK(vrr_trans_ctl(13, &c) == 0x60000055u, "display 13 control %#x", vrr_trans_ctl(13, &c));
	vrr_compute_timings(30, 1, &mode_qhd144, 1, 48, 1, &c);
	CHECK(c.guardband == 84, "display 30 guard band %d", c.guardband);
	/* not asked for: the fixed timing, same guard band */
	vrr_compute_timings(12, 0, &mode_qhd144, 1, 48, 0, &c);
	CHECK(!c.enable && c.in_range && c.vmin == 1525 && c.vmax == 1525 && c.guardband == 84,
	      "fixed %d %d %d", c.enable, c.vmax, c.guardband);
	/* no range: nothing */
	vrr_compute_timings(12, 0, &mode_qhd144, 0, 48, 1, &c);
	CHECK(!c.in_range && !c.enable && !c.vmin, "no range");
	/* a vblank of one line leaves no guard band on display 12 */
	struct drm_mode_modeinfo tight = mode_qhd144;
	tight.vtotal = 1442;
	vrr_compute_timings(12, 0, &tight, 1, 48, 1, &c);
	CHECK(!c.in_range && !c.enable, "too short a vblank");
	/* a range whose lowest refresh is the mode's: nothing to vary */
	vrr_compute_timings(14, 0, &mode_qhd144, 1, 144, 1, &c);
	CHECK(!c.enable && c.vmax == 1525, "lowest refresh == mode refresh");
	/* the maximum's register bounds the frame */
	vrr_compute_timings(14, 0, &mode_qhd144, 1, 1, 1, &c);
	CHECK(c.vmax == 219540, "1 Hz: %d", c.vmax);
	tight = mode_qhd144;
	tight.htotal = 200;
	vrr_compute_timings(14, 0, &tight, 1, 1, 1, &c);
	CHECK(c.vmax == 0x100000, "vmax bounded %d", c.vmax);
}

static void test_against_restatement(void)
{
	static const int vers[] = { 11, 12, 13, 14, 20, 30 };
	static const int ranges[][2] = { { 48, 144 }, { 40, 60 }, { 30, 75 }, { 60, 165 }, { 50, 60 } };
	struct drm_mode_modeinfo m;
	int cases = 0;

	for (unsigned vi = 0; vi < sizeof(vers) / sizeof(vers[0]); vi++) {
		for (unsigned ri = 0; ri < sizeof(ranges) / sizeof(ranges[0]); ri++) {
			for (int vt = 0; vt < 40; vt++) {
				for (int en = 0; en < 2; en++) {
					struct vrr_config c;
					struct ref r;
					int ver = vers[vi], min = ranges[ri][0], max = ranges[ri][1];

					m = mode_qhd144;
					m.vtotal = (uint16_t)(1441 + vt * 37);
					m.vsync_start = 1443;
					m.vsync_end = 1448;
					ref_compute(ver, &m, min, max, en, &r);
					int refresh = vrr_mode_vrefresh(&m);
					vrr_compute_timings(ver, ver >= 30 ? 1 : 0, &m,
							    max - min > 10 && refresh >= min && refresh <= max,
							    min, en, &c);
					/* the restatement has no "too short" rule */
					if (r.in_range && (r.guardband < 1 || r.pipeline_full < 0)) {
						CHECK(!c.in_range, "ver %d vtotal %d: refused", ver, m.vtotal);
						continue;
					}
					cases++;
					CHECK(c.in_range == r.in_range && c.enable == r.enable &&
						      c.vmin == r.vmin && c.vmax == r.vmax &&
						      c.flipline == r.flipline && c.guardband == r.guardband &&
						      c.pipeline_full == r.pipeline_full,
					      "ver %d range %d-%d vtotal %d en %d: %d %d %d %d %d gb %d pf %d vs %d %d %d %d %d gb %d pf %d",
					      ver, min, max, m.vtotal, en, c.in_range, c.enable, c.vmin, c.vmax,
					      c.flipline, c.guardband, c.pipeline_full, r.in_range, r.enable,
					      r.vmin, r.vmax, r.flipline, r.guardband, r.pipeline_full);
					if (c.in_range)
						CHECK(vrr_trans_ctl(ver, &c) == r.ctl, "ver %d control %#x vs %#x",
						      ver, vrr_trans_ctl(ver, &c), r.ctl);
				}
			}
		}
	}
	CHECK(cases > 500, "cases %d", cases);
}

#if I915_FEAT_VRR
static void test_capability(void)
{
	struct intel_output *o;

	mk(12);
	o = dp_sink(0, 48, 144);
	CHECK(vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "a DP sink with a range");
	dev.drm.conn[0].display_info.monitor_range.max_vfreq = 58;
	CHECK(!vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "range of 10 Hz");
	dev.drm.conn[0].display_info.monitor_range.max_vfreq = 59;
	CHECK(vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "range of 11 Hz");
	dev.drm.conn[0].display_info.monitor_range.min_vfreq = 0;
	CHECK(!vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "no minimum");
	o = dp_sink(0, 48, 144);
	o->dpcd[DP_DOWNSTREAMPORT_PRESENT] = DP_DWN_STRM_PORT_PRESENT;
	CHECK(!vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "branch device");
	o->dpcd[DP_DOWNSTREAMPORT_PRESENT] = 0;
	o->dpcd[DP_DOWN_STREAM_PORT_COUNT] = 1;
	CHECK(!vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "MSA timing needed");
	o = dp_sink(1, 48, 120);
	CHECK(vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "panel, VBT allows");
	dev.display.vbt.lfp_vrr = 0;
	CHECK(!vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "panel, VBT forbids");
	mk(9);
	o = dp_sink(0, 48, 144);
	CHECK(!vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "display 9");
	mk(12);
	o = dp_sink(0, 48, 144);
	o->type = INTEL_OUTPUT_HDMI;
	CHECK(!vrr_is_capable(&dev, o, &dev.drm.conn[0].display_info), "HDMI");
}

static void test_connector(void)
{
	struct drm_driver drv = { 0 };
	struct intel_output *o;

	mk(14);
	intel_vrr_driver_setup(&dev, &drv);
	CHECK(drv.features & DRM_FEATURE_VRR, "feature bit on display 14");
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_init(&dev, o, 0);
	CHECK(attach_calls == 1 && dev.drm.conn[0].vrr_capable_property, "attached at init");
	intel_vrr_connector_update(&dev, o, 0);
	CHECK(o->vrr.capable && o->vrr.min_vrefresh == 48 && o->vrr.max_vrefresh == 144,
	      "range recorded");
	CHECK(set_calls == 1 && set_value == 1, "vrr_capable true");
	o->detected = 0;
	intel_vrr_connector_update(&dev, o, 0);
	CHECK(!o->vrr.capable && set_value == 0, "unplugged");
	/* not attached at init: the update attaches */
	mk(14);
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_update(&dev, o, 0);
	CHECK(attach_calls == 1 && set_value == 1, "attached by the update");
	mk(9);
	memset(&drv, 0, sizeof(drv));
	intel_vrr_driver_setup(&dev, &drv);
	CHECK(!(drv.features & DRM_FEATURE_VRR), "no feature bit on display 9");
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_init(&dev, o, 0);
	intel_vrr_connector_update(&dev, o, 0);
	CHECK(attach_calls == 0 && set_calls == 0 && !o->vrr.capable, "nothing on display 9");
}

/* Display 12 (or 11..29): set up with the transcoder, on, a push, off,
 * cleared after the transcoder. */
static void test_sequence(int ver)
{
	struct drm_crtc_state cs = { 0 };
	struct intel_output *o;
	struct intel_pipe *p;
	struct ref r, fixed;
	int t = 1, w = 0;

	mk(ver);
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_update(&dev, o, 0);
	ref_compute(ver, &mode_qhd144, 48, 144, 1, &r);
	ref_compute(ver, &mode_qhd144, 48, 144, 0, &fixed);
	cs.adjusted_mode = mode_qhd144;
	cs.vrr_enabled = 1;

	/* before the link trains: the sink is to ignore the MSA timing */
	o->active = 0;
	intel_vrr_link_prepare(&dev, o, &cs);
	CHECK(msa_calls == 1 && o->msa_timing_par_ignore == 1, "MSA ignore recorded");

	p = pipe_for(t, &mode_qhd144);
	nlog = 0;
	intel_vrr_transcoder_enable(&dev, p);
	if (ver == 12 || ver == 13) {
		EXPECT_WRITE(w, VRR_CHICKEN_TRANS(t), VRR_PIPE_VBLANK_WITH_DELAY);
		w++;
	}
	EXPECT_WRITE(w, TRANS_VRR_VMIN(t), fixed.hw_vmin);
	EXPECT_WRITE(w + 1, TRANS_VRR_VMAX(t), fixed.hw_vmax);
	EXPECT_WRITE(w + 2, TRANS_VRR_FLIPLINE(t), fixed.hw_flipline);
	EXPECT_WRITE(w + 3, TRANS_VRR_CTL(t), fixed.ctl);
	CHECK(writes() == w + 4, "set up: %d writes", writes());
	CHECK(!p->vrr.enabled && p->vrr.in_range, "set up, not on");

	/* the pipe runs: on */
	nlog = 0;
	intel_vrr_enable(&dev, o, p, &cs);
	CHECK(msa_calls == 1, "no second MSA write");
	EXPECT_WRITE(0, TRANS_VRR_VMIN(t), r.hw_vmin);
	EXPECT_WRITE(1, TRANS_VRR_VMAX(t), r.hw_vmax);
	EXPECT_WRITE(2, TRANS_VRR_FLIPLINE(t), r.hw_flipline);
	EXPECT_WRITE(3, TRANS_PUSH(t), TRANS_PUSH_EN);
	EXPECT_WRITE(4, TRANS_VRR_CTL(t), VRR_CTL_VRR_ENABLE | r.ctl);
	CHECK(writes() == 5, "on: %d writes", writes());
	CHECK(p->vrr.enabled && p->vrr.vmin == 1525 && p->vrr.vmax == 4573 &&
		      p->vrr.guardband == (uint32_t)r.guardband,
	      "pipe record %d %u %u %u", p->vrr.enabled, p->vrr.vmin, p->vrr.vmax, p->vrr.guardband);

	/* a flip: the push */
	nlog = 0;
	intel_vrr_send_push(&dev, p);
	EXPECT_WRITE(0, TRANS_PUSH(t), TRANS_PUSH_EN | TRANS_PUSH_SEND);
	CHECK(writes() == 1, "push");

	/* the next update, still asked for: nothing */
	nlog = 0;
	intel_vrr_disable(&dev, o, p, &cs);
	intel_vrr_enable(&dev, o, p, &cs);
	CHECK(writes() == 0, "unchanged update writes %d", writes());

	/* the client turns it off */
	cs.vrr_enabled = 0;
	nlog = 0;
	regs[TRANS_PUSH(t) / 4] = TRANS_PUSH_EN;
	intel_vrr_disable(&dev, o, p, &cs);
	EXPECT_WRITE(0, TRANS_VRR_CTL(t), r.ctl);
	EXPECT_WRITE(1, TRANS_PUSH(t), 0);
	EXPECT_WRITE(2, TRANS_VRR_VMIN(t), fixed.hw_vmin);
	EXPECT_WRITE(3, TRANS_VRR_VMAX(t), fixed.hw_vmax);
	EXPECT_WRITE(4, TRANS_VRR_FLIPLINE(t), fixed.hw_flipline);
	CHECK(writes() == 5, "off: %d writes", writes());
	CHECK(!p->vrr.enabled, "off recorded");
	nlog = 0;
	intel_vrr_send_push(&dev, p);
	intel_vrr_enable(&dev, o, p, &cs);
	CHECK(writes() == 0, "no push, no enable while off");

	/* on again, then the pipe goes off */
	cs.vrr_enabled = 1;
	intel_vrr_enable(&dev, o, p, &cs);
	CHECK(p->vrr.enabled, "on again");
	nlog = 0;
	msa_calls = 0;
	intel_vrr_disable(&dev, o, p, NULL);
	CHECK(!p->vrr.enabled && msa_calls == 1 && !o->msa_timing_par_ignore,
	      "pipe off: MSA timing followed again");
	nlog = 0;
	intel_vrr_transcoder_disable(&dev, p);
	EXPECT_WRITE(0, TRANS_VRR_CTL(t), 0);
	CHECK(writes() == 2, "cleared after the transcoder: %d writes", writes());
	CHECK(p->vrr.vmin == 0 && !p->vrr.in_range, "record cleared");
}

/* Display 30: the fixed-rate path exactly as it always was; variable
 * refresh only changes the range. */
static void test_display30(void)
{
	struct drm_crtc_state cs = { 0 };
	struct intel_output *o;
	struct intel_pipe *p;
	int t = 0;

	mk(30);
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_update(&dev, o, 0);
	cs.adjusted_mode = mode_qhd144;
	cs.vrr_enabled = 1;
	p = pipe_for(t, &mode_qhd144);
	nlog = 0;
	intel_vrr_transcoder_enable(&dev, p);
	EXPECT_WRITE(0, TRANS_VRR_VMIN(t), 1524);
	EXPECT_WRITE(1, TRANS_VRR_VMAX(t), 1524);
	EXPECT_WRITE(2, TRANS_VRR_FLIPLINE(t), 1524);
	EXPECT_WRITE(3, TRANS_PUSH(t), TRANS_PUSH_EN);
	EXPECT_WRITE(4, TRANS_VRR_CTL(t), VRR_CTL_VRR_ENABLE | VRR_CTL_FLIP_LINE_EN | 84);
	CHECK(writes() == 5, "display 30 transcoder enable: %d writes", writes());
	nlog = 0;
	intel_vrr_enable(&dev, o, p, &cs);
	EXPECT_WRITE(0, TRANS_VRR_VMIN(t), 1524);
	EXPECT_WRITE(1, TRANS_VRR_VMAX(t), 4572);
	EXPECT_WRITE(2, TRANS_VRR_FLIPLINE(t), 1524);
	CHECK(writes() == 3, "display 30 on: %d writes", writes());
	nlog = 0;
	intel_vrr_send_push(&dev, p);
	EXPECT_WRITE(0, TRANS_PUSH(t), TRANS_PUSH_EN | TRANS_PUSH_SEND);
	nlog = 0;
	intel_vrr_disable(&dev, o, p, NULL);
	EXPECT_WRITE(0, TRANS_VRR_VMIN(t), 1524);
	EXPECT_WRITE(1, TRANS_VRR_VMAX(t), 1524);
	EXPECT_WRITE(2, TRANS_VRR_FLIPLINE(t), 1524);
	CHECK(writes() == 3, "display 30 off: %d writes", writes());
	nlog = 0;
	regs[TRANS_PUSH(t) / 4] = TRANS_PUSH_EN;
	intel_vrr_transcoder_disable(&dev, p);
	EXPECT_WRITE(0, TRANS_VRR_CTL(t), VRR_CTL_FLIP_LINE_EN | 84);
	EXPECT_WRITE(1, TRANS_PUSH(t), 0);
	CHECK(writes() == 2, "display 30 transcoder disable: %d writes", writes());
}

/* A sink without a range: no register of the generator is touched on
 * display 11..29, and the fixed path alone runs on 30. */
static void test_no_range(void)
{
	struct drm_crtc_state cs = { 0 };
	struct intel_output *o;
	struct intel_pipe *p;

	mk(14);
	o = dp_sink(0, 0, 0);
	intel_vrr_connector_update(&dev, o, 0);
	CHECK(!o->vrr.capable, "no range, not capable");
	cs.adjusted_mode = mode_qhd144;
	cs.vrr_enabled = 1;
	o->active = 0;
	intel_vrr_link_prepare(&dev, o, &cs);
	p = pipe_for(0, &mode_qhd144);
	nlog = 0;
	intel_vrr_transcoder_enable(&dev, p);
	intel_vrr_enable(&dev, o, p, &cs);
	intel_vrr_send_push(&dev, p);
	intel_vrr_disable(&dev, o, p, &cs);
	intel_vrr_disable(&dev, o, p, NULL);
	intel_vrr_transcoder_disable(&dev, p);
	CHECK(nlog == 0, "no range: %d register accesses", nlog);
	CHECK(msa_calls == 0, "no range: no MSA write");

	/* a mode outside the range: the same */
	mk(14);
	o = dp_sink(0, 30, 60);
	intel_vrr_connector_update(&dev, o, 0);
	CHECK(o->vrr.capable, "30-60 capable");
	p = pipe_for(0, &mode_qhd144);
	nlog = 0;
	intel_vrr_transcoder_enable(&dev, p);
	intel_vrr_enable(&dev, o, p, &cs);
	intel_vrr_transcoder_disable(&dev, p);
	CHECK(nlog == 0 && msa_calls == 0, "out of range: %d accesses", nlog);
}

/* The MSA write fails: no variable refresh. */
static void test_msa_failure(void)
{
	struct drm_crtc_state cs = { 0 };
	struct intel_output *o;
	struct intel_pipe *p;

	mk(14);
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_update(&dev, o, 0);
	cs.adjusted_mode = mode_qhd144;
	cs.vrr_enabled = 1;
	p = pipe_for(0, &mode_qhd144);
	intel_vrr_transcoder_enable(&dev, p);
	msa_rc = -EIO;
	nlog = 0;
	intel_vrr_enable(&dev, o, p, &cs);
	CHECK(msa_calls == 1 && !p->vrr.enabled && writes() == 0, "MSA failure: stays off");
	CHECK(kprintfs == 2, "said once (%d)", kprintfs);
}

/* The transcoder set up without a range, the range found later while the
 * pipe runs: set up on the running pipe, then on. */
static void test_late_range(void)
{
	struct drm_crtc_state cs = { 0 };
	struct intel_output *o;
	struct intel_pipe *p;
	struct ref r, fixed;

	mk(14);
	o = dp_sink(0, 0, 0);
	intel_vrr_connector_update(&dev, o, 0);
	p = pipe_for(0, &mode_qhd144);
	intel_vrr_transcoder_enable(&dev, p);
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_update(&dev, o, 0);
	cs.adjusted_mode = mode_qhd144;
	cs.vrr_enabled = 1;
	ref_compute(14, &mode_qhd144, 48, 144, 1, &r);
	ref_compute(14, &mode_qhd144, 48, 144, 0, &fixed);
	nlog = 0;
	intel_vrr_enable(&dev, o, p, &cs);
	CHECK(msa_calls == 1 && o->msa_timing_par_ignore, "MSA ignore on the live link");
	EXPECT_WRITE(0, TRANS_VRR_VMIN(0), fixed.hw_vmin);
	EXPECT_WRITE(3, TRANS_VRR_CTL(0), fixed.ctl);
	EXPECT_WRITE(4, TRANS_VRR_VMIN(0), r.hw_vmin);
	EXPECT_WRITE(7, TRANS_PUSH(0), TRANS_PUSH_EN);
	EXPECT_WRITE(8, TRANS_VRR_CTL(0), VRR_CTL_VRR_ENABLE | r.ctl);
	CHECK(p->vrr.enabled, "on");
}

#else
static void test_feature_off(void)
{
	struct drm_crtc_state cs = { 0 };
	struct drm_driver drv = { 0 };
	struct intel_output *o;
	struct intel_pipe *p;

	mk(14);
	intel_vrr_driver_setup(&dev, &drv);
	CHECK(!(drv.features & DRM_FEATURE_VRR), "no feature bit");
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_init(&dev, o, 0);
	intel_vrr_connector_update(&dev, o, 0);
	CHECK(!o->vrr.capable && attach_calls == 0 && set_calls == 0, "no property");
	cs.adjusted_mode = mode_qhd144;
	cs.vrr_enabled = 1;
	o->active = 0;
	intel_vrr_link_prepare(&dev, o, &cs);
	p = pipe_for(0, &mode_qhd144);
	nlog = 0;
	intel_vrr_transcoder_enable(&dev, p);
	intel_vrr_enable(&dev, o, p, &cs);
	intel_vrr_send_push(&dev, p);
	intel_vrr_disable(&dev, o, p, NULL);
	intel_vrr_transcoder_disable(&dev, p);
	CHECK(nlog == 0 && msa_calls == 0, "display 14: %d accesses", nlog);

	/* display 30: the fixed-rate path alone */
	mk(30);
	o = dp_sink(0, 48, 144);
	intel_vrr_connector_update(&dev, o, 0);
	p = pipe_for(0, &mode_qhd144);
	nlog = 0;
	intel_vrr_transcoder_enable(&dev, p);
	intel_vrr_enable(&dev, o, p, &cs);
	intel_vrr_send_push(&dev, p);
	CHECK(writes() == 5, "display 30 fixed path: %d writes", writes());
	EXPECT_WRITE(4, TRANS_VRR_CTL(0), VRR_CTL_VRR_ENABLE | VRR_CTL_FLIP_LINE_EN | 84);
}
#endif

int main(void)
{
	test_worked_numbers();
	test_against_restatement();
#if I915_FEAT_VRR
	test_capability();
	test_connector();
	test_sequence(11);
	test_sequence(12);
	test_sequence(13);
	test_sequence(14);
	test_sequence(20);
	test_display30();
	test_no_range();
	test_msa_failure();
	test_late_range();
#else
	test_feature_off();
#endif
	printf("%d checks, %d failures (VRR %d)\n", checks, fails, I915_FEAT_VRR);
	return fails != 0;
}
