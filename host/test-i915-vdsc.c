/* Host test for the Intel display's stream compression engine and its
 * DisplayPort/eDP configuration (kernel/dev/gpu/i915/display/
 * intel_vdsc.c, intel_dp_dsc.c), with the DSC library underneath
 * (drm_dsc_helper.c, intel_dsc_caps.c, intel_qp_tables.c) and the DP
 * helper library compiled in.
 *
 * Behind the i915 register interface sits a plain register file, behind
 * the AUX channel a DPCD array.  Checked:
 *  - the engine's register values against the picture parameter set the
 *    sink gets as a packet (two independent packings of one set: every
 *    field the registers hold must decode to the packet's), and a few
 *    register words worked out by hand;
 *  - where they go: the pipe's DSC0/DSC1 (Tiger Lake on, version 11
 *    pipes B/C), the transcoder-level DSCA/DSCC of version 11's embedded
 *    panel transcoder, PPS 17/18 only from version 14, the splitter
 *    control, the PPS packet in the transcoder's DIP, FEC in DP_TP_CTL,
 *    the extra power domain where the engines are in power well 2;
 *  - the choice: compressed bpp, link, slices, FEC for external DP and
 *    not for eDP, the input depth from the EDID, the refusals (no FEC in
 *    the sink, version 11 transcoder A, a sink without DSC), a mode that
 *    fits uncompressed left alone;
 *  - the sink: decompression and FEC_READY before training, off at
 *    disable, a firmware-left decompression switch cleared, an engine
 *    the firmware left running switched off.
 *
 * Run by host/test-i915-vdsc.sh from the repository root; "-v" prints
 * the kernel messages.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/uapi/types.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/lapic.h>

extern void *malloc(unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void *realloc(void *, unsigned long);
extern void free(void *);
extern int printf(const char *, ...);
extern int vprintf(const char *, __builtin_va_list);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);

int sched_task_hidden(const task_t *t);

#include "../kernel/dev/gpu/drm/drm_dp_helper.c"
#include "../kernel/dev/gpu/i915/display/intel_dp_dsc.c"
#include "../kernel/dev/gpu/i915/display/intel_vdsc.c"

/* ---- what the code calls of the kernel ------------------------------------- */

uint64_t test_irq_flag = 0x200;
int test_locks_held;
static int verbose;
static int failures, checks;

#define CHECK(c, ...)                                                   \
	do {                                                            \
		checks++;                                               \
		if (!(c)) {                                             \
			failures++;                                     \
			printf("FAIL %s:%d: ", __func__, __LINE__);     \
			printf(__VA_ARGS__);                            \
			printf("\n");                                   \
		}                                                       \
	} while (0)
#define CHECK_EQ(a, b) CHECK((long)(a) == (long)(b), "%s = %ld (0x%lx), want %ld (0x%lx)", #a, \
			     (long)(a), (long)(a), (long)(b), (long)(b))

void test_fatal(const char *what, const char *name)
{
	printf("FATAL: %s (%s)\n", what, name ? name : "?");
	__builtin_trap();
}

void *kalloc(size_t size) { return malloc(size); }
void *kcalloc(size_t count, size_t size) { return calloc(count, size); }
void *krealloc(void *ptr, size_t new_size) { return realloc(ptr, new_size); }
void kfree(void *ptr) { free(ptr); }
void mm_memset(void *dest, int val, size_t len) { memset(dest, val, len); }
void mm_memcpy(void *dest, const void *src, size_t len) { memcpy(dest, src, len); }
int kmemcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

int kprintf(const char *format, ...)
{
	__builtin_va_list ap;
	int n = 0;

	if (verbose) {
		__builtin_va_start(ap, format);
		n = vprintf(format, ap);
		__builtin_va_end(ap);
	}
	return n;
}

void panic(const char *fmt, ...)
{
	(void)fmt;
	printf("panic\n");
	__builtin_trap();
}

static uint64_t now_ns = 1000000000ull;
static task_t test_task;

void lapic_delay_us(uint32_t us) { now_ns += (uint64_t)us * 1000; }
void lapic_delay_ms(uint32_t ms) { lapic_delay_us(ms * 1000); }
uint64_t hrtimer_now_ns(void) { return now_ns; }
int hrtimer_is_highres(void) { return 1; }
int hrtimer_sleep_until(uint64_t abs_ns, uint64_t *remaining_ns)
{
	if (abs_ns > now_ns)
		now_ns = abs_ns;
	if (remaining_ns)
		*remaining_ns = 0;
	return 0;
}
task_t *sched_current(void) { return &test_task; }
int sched_task_hidden(const task_t *t) { (void)t; return 0; }

/* ---- the register file ------------------------------------------------------------ */

#define NREGS 1024
static uint32_t reg_addr[NREGS], reg_val[NREGS];
static int nregs;
static int reg_writes;

static int reg_slot(uint32_t reg, int create)
{
	for (int i = 0; i < nregs; i++)
		if (reg_addr[i] == reg)
			return i;
	if (!create || nregs == NREGS)
		return -1;
	reg_addr[nregs] = reg;
	reg_val[nregs] = 0;
	return nregs++;
}

/* a register the test sets: FEC "live" follows DP_TP_CTL's enable */
#define TEST_TP_CTL 0x60540
#define TEST_TP_STATUS 0x60544

uint32_t i915_read32(struct i915_device *i915, uint32_t reg)
{
	(void)i915;
	if (reg == TEST_TP_STATUS) {
		int s = reg_slot(TEST_TP_CTL, 0);
		return (s >= 0 && (reg_val[s] & DP_TP_CTL_FEC_ENABLE)) ? DP_TP_STATUS_FEC_ENABLE_LIVE : 0;
	}
	int s = reg_slot(reg, 0);
	return s >= 0 ? reg_val[s] : 0;
}

void i915_write32(struct i915_device *i915, uint32_t reg, uint32_t v)
{
	(void)i915;
	int s = reg_slot(reg, 1);
	if (s >= 0)
		reg_val[s] = v;
	reg_writes++;
}

static int reg_written(uint32_t reg) { return reg_slot(reg, 0) >= 0; }
static uint32_t reg(uint32_t r) { return i915_read32(NULL, r); }
static void regs_clear(void) { nregs = 0; reg_writes = 0; }

/* ---- the sink's DPCD ----------------------------------------------------------------- */

static u8 dpcd[0x1000];
static int dpcd_writes;

int intel_dp_aux_native_read(struct intel_dp_aux *aux, uint32_t addr, uint8_t *buf, unsigned len)
{
	(void)aux;
	if (addr + len > sizeof(dpcd))
		return -EIO;
	memcpy(buf, &dpcd[addr], len);
	/* the sink saw FEC switched on */
	if (addr == DP_FEC_STATUS)
		buf[0] |= DP_FEC_DECODE_EN_DETECTED;
	return (int)len;
}

int intel_dp_aux_native_write(struct intel_dp_aux *aux, uint32_t addr, const uint8_t *buf,
			      unsigned len)
{
	(void)aux;
	if (addr + len > sizeof(dpcd))
		return -EIO;
	memcpy(&dpcd[addr], buf, len);
	dpcd_writes++;
	return (int)len;
}

/* ---- what the DSC files call of the rest of the display ---------------------------- */

static int power_count[INTEL_PW_COUNT];
static int link_carries_answer;

void intel_power_get(struct i915_device *i915, enum intel_power_domain d) { (void)i915; power_count[d]++; }
void intel_power_put(struct i915_device *i915, enum intel_power_domain d) { (void)i915; power_count[d]--; }
int intel_display_verx100(struct i915_device *i915) { return i915->info->display_ver * 100; }
int intel_dpll_output_rate_supported(struct i915_device *i915, const struct intel_output *o,
				     uint32_t link_rate_khz)
{
	(void)i915; (void)o;
	return link_rate_khz <= 810000;
}
uint32_t intel_cdclk_max_khz(struct i915_device *i915) { (void)i915; return 652800; }
uint32_t intel_dp_tp_ctl_reg(struct i915_device *i915, const struct intel_output *o)
{
	(void)i915; (void)o;
	return TEST_TP_CTL;
}
uint32_t intel_dp_tp_status_reg(struct i915_device *i915, const struct intel_output *o)
{
	(void)i915; (void)o;
	return TEST_TP_STATUS;
}
int intel_dp_link_carries(struct i915_device *i915, const struct intel_output *o,
			  const struct drm_mode_modeinfo *m)
{
	(void)i915; (void)o; (void)m;
	return link_carries_answer;
}
void intel_dp_dsc_source_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	if (d->dsc_src.display_ver)
		return;
	intel_dsc_source_caps_init(&d->dsc_src, intel_display_verx100(i915), 0, 0);
}
int intel_dp_max_lanes(const struct intel_output *o)
{
	int lanes = o->dpcd[DP_MAX_LANE_COUNT] & 0x1f;
	return lanes < 1 ? 1 : lanes > 4 ? 4 : lanes;
}

/* ---- set-up ------------------------------------------------------------------------ */

static struct i915_device *i915;
static struct intel_device_info info;
static struct intel_output *out;

/* A DP 1.4 sink with DSC 1.2: 1, 2 or 4 slices up to 2560 pixels, 8 and
 * 10 bpc input, RGB, line buffer 13 bits, block prediction, 1000 Mpixel/s
 * a slice, FEC. */
static void sink_caps(int edp, int fec)
{
	struct intel_dsc_sink_caps *c = &out->dsc_caps;
	u8 *d = c->dsc_dpcd;

	intel_dsc_sink_caps_clear(c);
	d[DP_DSC_SUPPORT - DP_DSC_SUPPORT] = DP_DSC_DECOMPRESSION_IS_SUPPORTED;
	d[DP_DSC_REV - DP_DSC_SUPPORT] = 0x21;
	d[DP_DSC_SLICE_CAP_1 - DP_DSC_SUPPORT] = DP_DSC_1_PER_DP_DSC_SINK |
						 DP_DSC_2_PER_DP_DSC_SINK | DP_DSC_4_PER_DP_DSC_SINK;
	d[DP_DSC_LINE_BUF_BIT_DEPTH - DP_DSC_SUPPORT] = DP_DSC_LINE_BUF_BIT_DEPTH_13;
	d[DP_DSC_BLK_PREDICTION_SUPPORT - DP_DSC_SUPPORT] = DP_DSC_BLK_PREDICTION_IS_SUPPORTED;
	d[DP_DSC_DEC_COLOR_FORMAT_CAP - DP_DSC_SUPPORT] = DP_DSC_RGB;
	d[DP_DSC_DEC_COLOR_DEPTH_CAP - DP_DSC_SUPPORT] = DP_DSC_8_BPC | DP_DSC_10_BPC;
	d[DP_DSC_PEAK_THROUGHPUT - DP_DSC_SUPPORT] = DP_DSC_THROUGHPUT_MODE_0_1000 |
						     DP_DSC_THROUGHPUT_MODE_1_1000;
	d[DP_DSC_MAX_SLICE_WIDTH - DP_DSC_SUPPORT] = 8;
	if (edp) {
		/* eDP: a maximum compressed rate of 12 bpp */
		d[DP_DSC_MAX_BITS_PER_PIXEL_LOW - DP_DSC_SUPPORT] = 12 * 16;
		d[DP_DSC_MAX_BITS_PER_PIXEL_HI - DP_DSC_SUPPORT] = 0;
	}
	c->fec_capability = fec ? DP_FEC_CAPABLE : 0;
	c->is_edp = edp;
	c->valid = true;
}

static void setup(int display_ver, int platform, int port, int edp, int fec)
{
	if (!i915)
		i915 = calloc(1, sizeof(*i915));
	memset(i915, 0, sizeof(*i915));
	memset(&info, 0, sizeof(info));
	info.display_ver = (uint8_t)display_ver;
	info.platform = (uint8_t)platform;
	i915->info = &info;
	i915->display.model = display_ver >= 14 ? INTEL_DISPLAY_MTL :
			      display_ver >= 12 ? INTEL_DISPLAY_TGL : INTEL_DISPLAY_ICL;
	i915->drm.nconn = 2;
	out = &i915->display.outputs[0];
	memset(out, 0, sizeof(*out));
	out->present = 1;
	out->port = port;
	out->conn = 1;
	out->pipe = -1;
	out->is_edp = edp;
	out->type = edp ? INTEL_OUTPUT_EDP : INTEL_OUTPUT_DP;
	out->dpcd[DP_DPCD_REV] = 0x14;
	out->dpcd[DP_MAX_LANE_COUNT] = 4;
	out->sink_rates_khz[0] = 162000;
	out->sink_rates_khz[1] = 270000;
	out->sink_rates_khz[2] = 540000;
	out->sink_rates_khz[3] = 810000;
	out->nsink_rates = 4;
	i915->drm.conn[1].display_info.bpc = 10;
	sink_caps(edp, fec);
	memset(dpcd, 0, sizeof(dpcd));
	memset(power_count, 0, sizeof(power_count));
	memset(g_dsc_logged, 0, sizeof(g_dsc_logged));
	regs_clear();
	dpcd_writes = 0;
	link_carries_answer = 0;
}

static struct drm_mode_modeinfo mode_of(int h, int v, int clock)
{
	struct drm_mode_modeinfo m;

	memset(&m, 0, sizeof(m));
	m.hdisplay = (uint16_t)h;
	m.vdisplay = (uint16_t)v;
	m.htotal = (uint16_t)(h + 160);
	m.vtotal = (uint16_t)(v + 60);
	m.clock = (uint32_t)clock;
	return m;
}

/* ---- the register values against the packet ---------------------------------------- */

static unsigned be16(const u8 *b) { return (unsigned)b[0] << 8 | b[1]; }

static void check_regs_vs_packet(const struct drm_dsc_config *c, int n, int ver)
{
	struct intel_vdsc_regs r;
	struct drm_dsc_picture_parameter_set pps;
	const u8 *b = (const u8 *)&pps;

	intel_vdsc_regs_pack(c, n, ver, &r);
	drm_dsc_pps_payload_pack(&pps, c);

	/* PPS 0: the version major is always 1 in the register */
	CHECK_EQ(FIELD_GET(DSC_PPS0_VER_MAJOR_MASK, r.pps[0]), 1);
	CHECK_EQ(FIELD_GET(DSC_PPS0_VER_MINOR_MASK, r.pps[0]), b[0] & 0xf);
	CHECK_EQ(FIELD_GET(DSC_PPS0_BPC_MASK, r.pps[0]), b[3] >> 4);
	CHECK_EQ(FIELD_GET(DSC_PPS0_LINE_BUF_DEPTH_MASK, r.pps[0]), b[3] & 0xf);
	CHECK_EQ(!!(r.pps[0] & DSC_PPS0_BLOCK_PREDICTION), (b[4] >> 5) & 1);
	CHECK_EQ(!!(r.pps[0] & DSC_PPS0_COLOR_SPACE_CONVERSION), (b[4] >> 4) & 1);
	CHECK_EQ(!!(r.pps[0] & DSC_PPS0_422_ENABLE), (b[4] >> 3) & 1);
	CHECK_EQ(!!(r.pps[0] & DSC_PPS0_VBR_ENABLE), (b[4] >> 2) & 1);
	CHECK_EQ(!!(r.pps[0] & DSC_PPS0_ALT_ICH_SEL), (b[0] & 0xf) == 2);
	if ((b[0] & 0xf) == 2) {
		CHECK_EQ(!!(r.pps[0] & DSC_PPS0_NATIVE_420_ENABLE), (b[88] >> 1) & 1);
		CHECK_EQ(!!(r.pps[0] & DSC_PPS0_NATIVE_422_ENABLE), b[88] & 1);
	}
	/* PPS 1 */
	CHECK_EQ(r.pps[1], ((b[4] & 3u) << 8) | b[5]);
	/* PPS 2: each engine's share of the width */
	CHECK_EQ(FIELD_GET(DSC_PPS2_PIC_HEIGHT_MASK, r.pps[2]), be16(&b[6]));
	CHECK_EQ(FIELD_GET(DSC_PPS2_PIC_WIDTH_MASK, r.pps[2]) * (unsigned)n, be16(&b[8]));
	/* PPS 3 */
	CHECK_EQ(FIELD_GET(DSC_PPS3_SLICE_HEIGHT_MASK, r.pps[3]), be16(&b[10]));
	CHECK_EQ(FIELD_GET(DSC_PPS3_SLICE_WIDTH_MASK, r.pps[3]), be16(&b[12]));
	/* PPS 4 */
	CHECK_EQ(FIELD_GET(DSC_PPS4_INITIAL_XMIT_DELAY_MASK, r.pps[4]), ((b[16] & 3u) << 8) | b[17]);
	CHECK_EQ(FIELD_GET(DSC_PPS4_INITIAL_DEC_DELAY_MASK, r.pps[4]), be16(&b[18]));
	/* PPS 5 */
	CHECK_EQ(FIELD_GET(DSC_PPS5_SCALE_INC_INT_MASK, r.pps[5]), be16(&b[22]));
	CHECK_EQ(FIELD_GET(DSC_PPS5_SCALE_DEC_INT_MASK, r.pps[5]), ((b[24] & 0xfu) << 8) | b[25]);
	/* PPS 6 */
	CHECK_EQ(FIELD_GET(DSC_PPS6_INITIAL_SCALE_VALUE_MASK, r.pps[6]), b[21] & 0x3f);
	CHECK_EQ(FIELD_GET(DSC_PPS6_FIRST_LINE_BPG_OFFSET_MASK, r.pps[6]), b[27] & 0x1f);
	CHECK_EQ(FIELD_GET(DSC_PPS6_FLATNESS_MIN_QP_MASK, r.pps[6]), b[36] & 0x1f);
	CHECK_EQ(FIELD_GET(DSC_PPS6_FLATNESS_MAX_QP_MASK, r.pps[6]), b[37] & 0x1f);
	/* PPS 7 */
	CHECK_EQ(FIELD_GET(DSC_PPS7_NFL_BPG_OFFSET_MASK, r.pps[7]), be16(&b[28]));
	CHECK_EQ(FIELD_GET(DSC_PPS7_SLICE_BPG_OFFSET_MASK, r.pps[7]), be16(&b[30]));
	/* PPS 8 */
	CHECK_EQ(FIELD_GET(DSC_PPS8_INITIAL_OFFSET_MASK, r.pps[8]), be16(&b[32]));
	CHECK_EQ(FIELD_GET(DSC_PPS8_FINAL_OFFSET_MASK, r.pps[8]), be16(&b[34]));
	/* PPS 9 */
	CHECK_EQ(FIELD_GET(DSC_PPS9_RC_MODEL_SIZE_MASK, r.pps[9]), be16(&b[38]));
	CHECK_EQ(FIELD_GET(DSC_PPS9_RC_EDGE_FACTOR_MASK, r.pps[9]), b[40] & 0xf);
	/* PPS 10 */
	CHECK_EQ(FIELD_GET(DSC_PPS10_RC_QUANT_INC_LIMIT0_MASK, r.pps[10]), b[41] & 0x1f);
	CHECK_EQ(FIELD_GET(DSC_PPS10_RC_QUANT_INC_LIMIT1_MASK, r.pps[10]), b[42] & 0x1f);
	CHECK_EQ(FIELD_GET(DSC_PPS10_RC_TGT_OFF_HIGH_MASK, r.pps[10]), b[43] >> 4);
	CHECK_EQ(FIELD_GET(DSC_PPS10_RC_TGT_OFF_LOW_MASK, r.pps[10]), b[43] & 0xf);
	/* PPS 16 */
	CHECK_EQ(FIELD_GET(DSC_PPS16_SLICE_CHUNK_SIZE_MASK, r.pps[16]), be16(&b[14]));
	CHECK_EQ(FIELD_GET(DSC_PPS16_SLICE_PER_LINE_MASK, r.pps[16]) * (unsigned)n,
		 be16(&b[8]) / be16(&b[12]));
	CHECK_EQ(FIELD_GET(DSC_PPS16_SLICE_ROW_PR_FRME_MASK, r.pps[16]), be16(&b[6]) / be16(&b[10]));
	/* PPS 17/18: from version 14 on, else left alone */
	if (ver >= 14) {
		CHECK_EQ(FIELD_GET(DSC_PPS17_SL_BPG_OFFSET_MASK, r.pps[17]), b[89] & 0x1f);
		CHECK_EQ(FIELD_GET(DSC_PPS18_NSL_BPG_OFFSET_MASK, r.pps[18]), be16(&b[90]));
		CHECK_EQ(FIELD_GET(DSC_PPS18_SL_OFFSET_ADJ_MASK, r.pps[18]), be16(&b[92]));
	} else {
		CHECK_EQ(r.pps[17], 0);
		CHECK_EQ(r.pps[18], 0);
	}
	/* the buffer thresholds, a byte each */
	for (int i = 0; i < 14; i++)
		CHECK_EQ((r.rc_buf_thresh[i / 4] >> (8 * (i % 4))) & 0xff, b[44 + i]);
	CHECK_EQ(r.rc_buf_thresh[3] >> 16, 0);
	/* the ranges: min QP, max QP, BPG offset */
	for (int i = 0; i < 15; i++) {
		unsigned w = (r.rc_range[i / 2] >> (16 * (i % 2))) & 0xffff;
		unsigned p = be16(&b[58 + 2 * i]);

		CHECK_EQ((w >> RC_MIN_QP_SHIFT) & 0x1f, p >> 11);
		CHECK_EQ((w >> RC_MAX_QP_SHIFT) & 0x1f, (p >> 6) & 0x1f);
		CHECK_EQ((w >> RC_BPG_OFFSET_SHIFT) & 0x3f, p & 0x3f);
	}
	CHECK_EQ(r.rc_range[7] >> 16, 0);
}

static void test_pack(void)
{
	struct intel_dsc_source_caps src;
	struct drm_dsc_config c;

	/* Tiger Lake: 1920x1080, 8 bpc in, 12 bpp out, 2 slices, the
	 * fixed RC tables */
	setup(12, 0, PORT_B, 0, 1);
	intel_dsc_source_caps_init(&src, 1200, 0, 0);
	CHECK_EQ(intel_dsc_dp_compute_config(&src, &out->dsc_caps, &c, 1920, 1080, 2,
					     12 * 16, 24, INTEL_DSC_FORMAT_RGB), 0);
	check_regs_vs_packet(&c, 2, 12);
	check_regs_vs_packet(&c, 1, 12);

	struct intel_vdsc_regs r;
	intel_vdsc_regs_pack(&c, 2, 12, &r);
	/* by hand: major 1, minor 1 (version 12 source), 8 bpc, 13-bit
	 * line buffer, block prediction, RGB conversion */
	CHECK_EQ(r.pps[0], 0x1u | 1u << 4 | 8u << 8 | 13u << 12 | 1u << 16 | 1u << 17);
	CHECK_EQ(r.pps[1], 12 * 16);
	CHECK_EQ(r.pps[2], (960u << 16) | 1080);
	CHECK_EQ(r.pps[3], (960u << 16) | 108);
	/* one slice per engine, ten rows of slices */
	CHECK_EQ(FIELD_GET(DSC_PPS16_SLICE_PER_LINE_MASK, r.pps[16]), 1);
	CHECK_EQ(FIELD_GET(DSC_PPS16_SLICE_ROW_PR_FRME_MASK, r.pps[16]), 10);
	/* the chunk: 960 pixels at 12 bpp, in bytes */
	CHECK_EQ(FIELD_GET(DSC_PPS16_SLICE_CHUNK_SIZE_MASK, r.pps[16]), 960 * 12 / 8);
	CHECK_EQ(r.pps[9], (6u << 16) | 8192);
	CHECK_EQ(r.pps[10] & 0xff0000, (3u << 20) | (3u << 16));

	/* Meteor Lake: 3840x2160, 10 bpc in, 9.4375 bpp out, 4 slices, the
	 * C-model formulas, DSC 1.2 (PPS 17/18) */
	setup(14, 0, PORT_B, 0, 1);
	intel_dsc_source_caps_init(&src, 1400, 0, 0);
	CHECK_EQ(intel_dsc_dp_compute_config(&src, &out->dsc_caps, &c, 3840, 2160, 4,
					     9 * 16 + 7, 30, INTEL_DSC_FORMAT_RGB), 0);
	CHECK_EQ(c.dsc_version_minor, 2);
	check_regs_vs_packet(&c, 2, 14);
	intel_vdsc_regs_pack(&c, 2, 14, &r);
	CHECK(r.pps[0] & DSC_PPS0_ALT_ICH_SEL, "DSC 1.2 selects the alternate ICH");
	CHECK_EQ(FIELD_GET(DSC_PPS16_SLICE_PER_LINE_MASK, r.pps[16]), 2);
	CHECK_EQ(FIELD_GET(DSC_PPS2_PIC_WIDTH_MASK, r.pps[2]), 1920);

	/* Ice Lake: 2560x1600, 8 bpc, 8 bpp, 1 slice */
	setup(11, 0, PORT_A, 1, 0);
	intel_dsc_source_caps_init(&src, 1100, 0, 0);
	CHECK_EQ(intel_dsc_dp_compute_config(&src, &out->dsc_caps, &c, 2560, 1600, 1,
					     8 * 16, 24, INTEL_DSC_FORMAT_RGB), 0);
	check_regs_vs_packet(&c, 1, 11);
}

/* ---- the engines in place ------------------------------------------------------------ */

static struct intel_pipe *lit(int pipe, int transcoder, const struct drm_mode_modeinfo *m,
			      int expect_rc)
{
	struct intel_pipe *p = &i915->display.pipes[pipe];

	p->pipe = pipe;
	p->transcoder = transcoder;
	out->pipe = pipe;
	int rc = intel_dsc_compute_config(i915, out, m, &p->dsc);
	CHECK_EQ(rc, expect_rc);
	return p;
}

static void test_engine_tgl_pipe_b(void)
{
	struct drm_mode_modeinfo m = mode_of(3840, 2160, 1188000);

	/* 4K 144 Hz on HBR3 x4: does not fit at 24 bpp */
	setup(12, 0, PORT_B, 0, 1);
	struct intel_pipe *p = lit(PIPE_B, 1, &m, 0);
	CHECK_EQ(p->dsc.enable, 1);
	CHECK_EQ(p->dsc.pipe_bpp, 30); /* the EDID's 10 bpc */
	CHECK_EQ(p->dsc.fec_enable, 1);
	CHECK_EQ(p->dsc.link_rate_khz, 810000);
	CHECK_EQ(p->dsc.lane_count, 4);
	CHECK(p->dsc.compressed_bpp_x16 % 16 == 0, "whole bits before version 13: %u",
	      p->dsc.compressed_bpp_x16);
	CHECK(p->dsc.slice_count >= 2, "slices %u", p->dsc.slice_count);
	CHECK_EQ(p->dsc.num_vdsc_instances, 2);
	/* the stream fits the link it was sized for, FEC included */
	CHECK(intel_dsc_link_data_rate(p, m.clock) <= 810000ull * 4 * 8,
	      "data rate %llu", (unsigned long long)intel_dsc_link_data_rate(p, m.clock));
	/* the highest rate version 12 takes (VESA 15 of 23) fits HBR3 x4
	 * with FEC, and not HBR3 x2 or HBR2 x4 */
	CHECK_EQ(p->dsc.compressed_bpp_x16, 15 * 16);

	/* before training: the sink decompresses and expects FEC */
	intel_dsc_pre_link_train(i915, out, p);
	CHECK_EQ(dpcd[DP_DSC_ENABLE] & DP_DECOMPRESSION_EN, 1);
	CHECK_EQ(dpcd[DP_FEC_CONFIGURATION], DP_FEC_READY);
	CHECK_EQ(p->dsc.sink_decompression, 1);

	i915_write32(i915, TEST_TP_CTL, 0x80000000);
	intel_dsc_enable(i915, out, p, &m);
	CHECK_EQ(reg(TEST_TP_CTL), 0x80000000 | DP_TP_CTL_FEC_ENABLE);
	/* pipe B's engines: DSC0 at 0x78270.., DSC1 at 0x78370.. */
	CHECK(reg_written(0x78270) && reg_written(0x78370), "PPS 0 of both engines");
	CHECK_EQ(reg(0x78270), reg(0x78370));
	CHECK(reg_written(0x78270 + 16 * 4), "PPS 16");
	CHECK(!reg_written(0x78270 + 17 * 4), "no PPS 17 before version 14");
	CHECK(!reg_written(0x78270 + 11 * 4), "nothing between PPS 10 and 16");
	CHECK(reg_written(0x78254) && reg_written(0x7825C + 4), "buffer thresholds");
	CHECK(reg_written(0x78208) && reg_written(0x78208 + 7 * 4), "RC ranges");
	CHECK(reg_written(0x78354) && reg_written(0x78308 + 7 * 4), "second engine's RC");
	CHECK_EQ(reg(0x78200), JOINER_ENABLE);
	CHECK_EQ(reg(0x78204), VDSC0_ENABLE | VDSC1_ENABLE);
	CHECK(!reg_written(DSS_CTL2), "no transcoder-level engine");
	/* the PPS packet in transcoder B's DIP: header 00 10 7f 00 */
	CHECK(reg(HSW_TVIDEO_DIP_CTL(1)) & VDIP_ENABLE_PPS, "PPS packet sent");
	CHECK_EQ(reg(ICL_VIDEO_DIP_PPS_DATA(1, 0)), 0x007f1000u);
	CHECK(reg_written(ICL_VIDEO_DIP_PPS_DATA(1, 32)), "all 132 bytes written");
	/* the engine's PPS 0 and the packet's version / depth bytes agree */
	CHECK_EQ(reg(ICL_VIDEO_DIP_PPS_DATA(1, 1)) & 0xff, 0x11);
	/* pipe B's own well powers its engines */
	for (int d = 0; d < INTEL_PW_COUNT; d++)
		CHECK_EQ(power_count[d], 0);

	intel_dsc_disable(i915, out, p);
	CHECK_EQ(reg(0x78200), 0);
	CHECK_EQ(reg(0x78204), 0);
	CHECK(!(reg(HSW_TVIDEO_DIP_CTL(1)) & VDIP_ENABLE_PPS), "PPS packet stopped");
	CHECK_EQ(reg(TEST_TP_CTL), 0x80000000);
	CHECK_EQ(dpcd[DP_DSC_ENABLE], 0);
	CHECK_EQ(dpcd[DP_FEC_CONFIGURATION], 0);
	CHECK_EQ(p->dsc.enable, 0);
	CHECK_EQ(p->dsc.pipe_bpp, 0);
}

static void test_engine_tgl_pipe_a(void)
{
	struct drm_mode_modeinfo m = mode_of(3840, 2160, 1188000);

	/* Tiger Lake pipe A: the engines are in power well 2 */
	setup(12, 0, PORT_B, 0, 1);
	struct intel_pipe *p = lit(PIPE_A, 0, &m, 0);
	intel_dsc_pre_link_train(i915, out, p);
	intel_dsc_enable(i915, out, p, &m);
	CHECK_EQ(power_count[DSC_PW2_DOMAIN], 1);
	CHECK(reg_written(0x78070), "pipe A's DSC0 PPS 0 at 0x78070");
	CHECK_EQ(reg(0x78004), VDSC0_ENABLE | VDSC1_ENABLE);
	intel_dsc_disable(i915, out, p);
	CHECK_EQ(power_count[DSC_PW2_DOMAIN], 0);

	/* Rocket Lake: pipe A's own well */
	setup(12, I915_PLATFORM_ROCKETLAKE, PORT_B, 0, 1);
	p = lit(PIPE_A, 0, &m, 0);
	intel_dsc_enable(i915, out, p, &m);
	CHECK_EQ(power_count[DSC_PW2_DOMAIN], 0);
	intel_dsc_disable(i915, out, p);
}

static void test_engine_icl_edp(void)
{
	/* Ice Lake's embedded panel on the eDP transcoder: DSCA/DSCC,
	 * power well 2, no FEC */
	struct drm_mode_modeinfo m = mode_of(3840, 2400, 1100000);

	setup(11, 0, PORT_A, 1, 0);
	out->dpcd[DP_MAX_LANE_COUNT] = 4;
	struct intel_pipe *p = lit(PIPE_A, TRANSCODER_EDP, &m, 0);
	CHECK_EQ(p->dsc.enable, 1);
	CHECK_EQ(p->dsc.fec_enable, 0);
	/* the panel's link at its most and its maximum rate (12 bpp) */
	CHECK_EQ(p->dsc.link_rate_khz, 810000);
	CHECK_EQ(p->dsc.lane_count, 4);
	CHECK_EQ(p->dsc.compressed_bpp_x16, 12 * 16);
	/* an eDP panel gets the most slices it takes */
	CHECK_EQ(p->dsc.slice_count, 4);
	CHECK_EQ(intel_dsc_link_data_rate(p, m.clock), (uint64_t)m.clock * 12);

	intel_dsc_pre_link_train(i915, out, p);
	CHECK_EQ(dpcd[DP_DSC_ENABLE], DP_DECOMPRESSION_EN);
	CHECK_EQ(dpcd[DP_FEC_CONFIGURATION], 0);
	intel_dsc_enable(i915, out, p, &m);
	CHECK_EQ(power_count[DSC_PW2_DOMAIN], 1);
	CHECK(!(reg(TEST_TP_CTL) & DP_TP_CTL_FEC_ENABLE), "no FEC on eDP");
	CHECK(reg_written(DSCA_PPS(0)) && reg_written(DSCC_PPS(0)), "DSCA and DSCC");
	CHECK(reg_written(0x6B270), "PPS 16 after the gap");
	CHECK_EQ(reg(0x6B270), reg(0x6BA70));
	CHECK(reg_written(DSCA_RC_BUF_THRESH_0) && reg_written(DSCC_RC_RANGE_PARAMETERS_0 + 28),
	      "transcoder-level RC registers");
	CHECK_EQ(reg(DSS_CTL1), JOINER_ENABLE);
	CHECK_EQ(reg(DSS_CTL2), VDSC0_ENABLE | VDSC1_ENABLE);
	CHECK(reg(HSW_TVIDEO_DIP_CTL(TRANSCODER_EDP)) & VDIP_ENABLE_PPS, "packet on the eDP transcoder");
	CHECK(reg_written(0x6F350), "eDP transcoder's PPS DIP data");
	intel_dsc_disable(i915, out, p);
	CHECK_EQ(reg(DSS_CTL2), 0);
	CHECK_EQ(power_count[DSC_PW2_DOMAIN], 0);
	CHECK_EQ(dpcd[DP_DSC_ENABLE], 0);
}

static void test_mtl_fractional(void)
{
	/* Meteor Lake with a sink taking 1/16 bpp steps: PPS 17/18 written */
	struct drm_mode_modeinfo m = mode_of(5120, 2880, 1500000);

	setup(14, 0, PORT_B, 0, 1);
	out->dsc_caps.dsc_dpcd[DP_DSC_BITS_PER_PIXEL_INC - DP_DSC_SUPPORT] =
		DP_DSC_BITS_PER_PIXEL_1_16;
	struct intel_pipe *p = lit(PIPE_C, 2, &m, 0);
	CHECK_EQ(p->dsc.enable, 1);
	/* the highest 1/16 step whose stream (FEC included) fits HBR3 x4 */
	{
		struct intel_pipe q = *p;
		uint64_t cap = 3240000ull * 8;

		CHECK(intel_dsc_link_data_rate(p, m.clock) <= cap, "fits");
		q.dsc.compressed_bpp_x16 = (uint16_t)(p->dsc.compressed_bpp_x16 + 1);
		CHECK(intel_dsc_link_data_rate(&q, m.clock) > cap, "1/16 more would not (%u)",
		      p->dsc.compressed_bpp_x16);
		CHECK(p->dsc.compressed_bpp_x16 % 16 != 0, "a fractional rate here (%u)",
		      p->dsc.compressed_bpp_x16);
	}
	intel_dsc_enable(i915, out, p, &m);
	CHECK(reg_written(ICL_DSC0_PPS(PIPE_C, 17)) && reg_written(ICL_DSC0_PPS(PIPE_C, 18)),
	      "PPS 17/18 from version 14");
	intel_dsc_disable(i915, out, p);
}

static void test_refusals(void)
{
	struct drm_mode_modeinfo m = mode_of(3840, 2160, 1188000);
	struct intel_dsc_config cfg;

	/* the uncompressed link carries it: nothing to compress */
	setup(12, 0, PORT_B, 0, 1);
	link_carries_answer = 1;
	CHECK_EQ(intel_dsc_compute_config(i915, out, &m, &cfg), 0);
	CHECK_EQ(cfg.enable, 0);

	/* external DP without FEC in the sink */
	setup(12, 0, PORT_B, 0, 0);
	CHECK(intel_dsc_compute_config(i915, out, &m, &cfg) < 0, "no FEC, no DSC");
	CHECK_EQ(cfg.enable, 0);

	/* a sink without DSC */
	setup(12, 0, PORT_B, 0, 1);
	out->dsc_caps.dsc_dpcd[0] = 0;
	CHECK(intel_dsc_compute_config(i915, out, &m, &cfg) < 0, "sink without DSC");

	/* Ice Lake: no engine behind transcoder A (the connector's own
	 * pipe at the check) */
	setup(11, 0, PORT_B, 0, 1);
	out->conn = 0;
	CHECK(intel_dsc_compute_config(i915, out, &m, &cfg) < 0, "version 11 transcoder A");
	out->conn = 1;
	CHECK_EQ(intel_dsc_compute_config(i915, out, &m, &cfg), 0);
	CHECK_EQ(cfg.enable, 1);

	/* Ice Lake's port A carries no FEC: external DP there cannot */
	setup(11, 0, PORT_A, 0, 1);
	CHECK(intel_dsc_compute_config(i915, out, &m, &cfg) < 0, "version 11 port A, external");

	/* a sink whose EDID says 6 bpc: below the compressor's input */
	setup(12, 0, PORT_B, 0, 1);
	i915->drm.conn[1].display_info.bpc = 6;
	CHECK(intel_dsc_compute_config(i915, out, &m, &cfg) < 0, "6 bpc sink");
	/* no EDID depth: 8 bpc */
	i915->drm.conn[1].display_info.bpc = 0;
	CHECK_EQ(intel_dsc_compute_config(i915, out, &m, &cfg), 0);
	CHECK_EQ(cfg.pipe_bpp, 24);

	/* too much even compressed: 8K 120 on HBR3 x2 */
	setup(12, 0, PORT_B, 0, 1);
	out->dpcd[DP_MAX_LANE_COUNT] = 2;
	m = mode_of(7680, 4320, 4000000);
	CHECK(intel_dsc_compute_config(i915, out, &m, &cfg) < 0, "does not fit at 8 bpp");

	/* not DisplayPort */
	setup(12, 0, PORT_B, 0, 1);
	out->type = INTEL_OUTPUT_HDMI;
	m = mode_of(3840, 2160, 1188000);
	CHECK(intel_dsc_compute_config(i915, out, &m, &cfg) < 0, "HDMI");
}

static void test_least_link(void)
{
	/* 2560x1440 at 240 Hz (970 MHz): 24 bpp needs 2.91 GB/s; 15 bpp
	 * (the most version 12 takes) with FEC 1.87 GB/s, which HBR3 x2
	 * (1.62 GB/s) does not carry and HBR2 x4 (2.16 GB/s) does: the
	 * least link at the highest rate */
	struct drm_mode_modeinfo m = mode_of(2560, 1440, 970000);

	setup(12, 0, PORT_B, 0, 1);
	struct intel_pipe *p = lit(PIPE_B, 1, &m, 0);
	CHECK_EQ(p->dsc.enable, 1);
	CHECK_EQ(p->dsc.compressed_bpp_x16, 15 * 16);
	CHECK_EQ(p->dsc.link_rate_khz, 540000);
	CHECK_EQ(p->dsc.lane_count, 4);

	/* two lanes at most: 15 bpp fits no link, 12 bpp (1.50 GB/s)
	 * fits HBR3 x2 */
	setup(12, 0, PORT_B, 0, 1);
	out->dpcd[DP_MAX_LANE_COUNT] = 2;
	p = lit(PIPE_B, 1, &m, 0);
	CHECK_EQ(p->dsc.compressed_bpp_x16, 12 * 16);
	CHECK_EQ(p->dsc.link_rate_khz, 810000);
	CHECK_EQ(p->dsc.lane_count, 2);
}

static void test_firmware_left(void)
{
	/* the firmware left pipe B compressing, and the sink decompressing */
	struct drm_mode_modeinfo m = mode_of(1920, 1080, 148500);
	struct intel_pipe tmp;

	setup(12, 0, PORT_B, 0, 1);
	i915_write32(i915, ICL_PIPE_DSS_CTL1(PIPE_B), JOINER_ENABLE);
	i915_write32(i915, ICL_PIPE_DSS_CTL2(PIPE_B), VDSC0_ENABLE | VDSC1_ENABLE);
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(1), VDIP_ENABLE_PPS | VIDEO_DIP_ENABLE_AVI_HSW);
	memset(&tmp, 0, sizeof(tmp));
	tmp.pipe = PIPE_B;
	tmp.transcoder = 1;
	intel_dsc_disable(i915, out, &tmp);
	CHECK_EQ(reg(ICL_PIPE_DSS_CTL1(PIPE_B)), 0);
	CHECK_EQ(reg(ICL_PIPE_DSS_CTL2(PIPE_B)), 0);
	CHECK_EQ(reg(HSW_TVIDEO_DIP_CTL(1)), VIDEO_DIP_ENABLE_AVI_HSW);
	CHECK_EQ(dpcd_writes, 0); /* the sink's AUX may be unpowered here */

	/* next enable, uncompressed: the decompression switch goes off */
	dpcd[DP_DSC_ENABLE] = DP_DECOMPRESSION_EN;
	link_carries_answer = 1;
	struct intel_pipe *p = lit(PIPE_B, 1, &m, 0);
	CHECK_EQ(p->dsc.enable, 0);
	intel_dsc_pre_link_train(i915, out, p);
	CHECK_EQ(dpcd[DP_DSC_ENABLE], 0);
	regs_clear();
	intel_dsc_enable(i915, out, p, &m);
	CHECK_EQ(reg_writes, 0);
	intel_dsc_disable(i915, out, p);
	/* nothing compressed, nothing to switch off (one read only) */
	CHECK_EQ(reg_writes, 0);
}

static void test_min_cdclk(void)
{
	struct intel_dsc_config c;

	memset(&c, 0, sizeof(c));
	CHECK_EQ(intel_dsc_min_cdclk_khz(&c, 594000, 4400), 0);
	c.enable = 1;
	c.slice_count = 1;
	c.num_vdsc_instances = 1;
	/* 594000 * (4400 + 14) / 4400 = 595890 */
	CHECK_EQ(intel_dsc_min_cdclk_khz(&c, 594000, 4400), 595890);
	c.slice_count = 2;
	c.num_vdsc_instances = 2;
	/* 594000 * 4428 / 4400 = 597780, two engines */
	CHECK_EQ(intel_dsc_min_cdclk_khz(&c, 594000, 4400), 298890);
}

int main(int argc, char **argv)
{
	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v')
		verbose = 1;
	test_pack();
	test_engine_tgl_pipe_b();
	test_engine_tgl_pipe_a();
	test_engine_icl_edp();
	test_mtl_fractional();
	test_refusals();
	test_least_link();
	test_firmware_left();
	test_min_cdclk();
	printf("test-i915-vdsc: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
