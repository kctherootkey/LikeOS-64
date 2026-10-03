/* Host test for the Intel display's DisplayPort channel and link training
 * on the DisplayPort helper library (kernel/dev/gpu/i915/display/
 * intel_dp_aux.c, intel_dp_link_training.c).
 *
 * The three files are compiled into this one.  Behind the i915 register
 * interface sits a model of the AUX channel hardware (control and data
 * registers, the message packing) and of a sink: a DPCD register space,
 * an EDID behind I2C address 0x50, DEFER replies on request, and a link
 * that trains once the levels written to a receiver reach what it needs
 * -- for the sink and for each link-training repeater, which in
 * non-transparent mode only pass the training on once they are trained
 * themselves.  The port's side (the DDI buffer levels and training
 * pattern) is recorded by stubs.
 *
 * Checked: the transfer hook's reply codes and payload counts, zero-size
 * I2C messages, short writes, DEFER retries, the hotplug abort, the
 * channel lock; the repeater detection and mode setting (and none on a
 * display without the AUX timeout for it, and no mode write with no
 * repeater there); the per-PHY training order, the levels the port and
 * the repeaters get, the pattern disabled at each PHY, the waits, the
 * eDP rate-select path, a sink that never locks.
 *
 * Run by host/test-i915-dp.sh from the repository root; "-v" prints the
 * kernel messages.
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
extern int strcmp(const char *, const char *);

int sched_task_hidden(const task_t *t);

#include "../kernel/dev/gpu/drm/drm_dp_helper.c"
/* file-local in each, one translation unit here */
#undef AUX_SYNC_LEN
#include "../kernel/dev/gpu/i915/display/intel_dp_aux.c"
#include "../kernel/dev/gpu/i915/display/intel_dp_link_training.c"
#include "../kernel/dev/gpu/i915/display/intel_dp.c"

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
#define CHECK_EQ(a, b) CHECK((long)(a) == (long)(b), "%s = %ld, want %ld", #a, (long)(a), (long)(b))

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

static uint64_t busy_us, slept_us;
static uint64_t now_ns = 1000000000ull;
static task_t test_task;

void lapic_delay_us(uint32_t us)
{
	busy_us += us;
	now_ns += (uint64_t)us * 1000;
}
void lapic_delay_ms(uint32_t ms) { lapic_delay_us(ms * 1000); }
uint64_t hrtimer_now_ns(void) { return now_ns; }
int hrtimer_is_highres(void) { return 1; }
int hrtimer_sleep_until(uint64_t abs_ns, uint64_t *remaining_ns)
{
	if (abs_ns > now_ns) {
		slept_us += (abs_ns - now_ns) / 1000;
		now_ns = abs_ns;
	}
	if (remaining_ns)
		*remaining_ns = 0;
	return 0;
}
task_t *sched_current(void) { return &test_task; }
int sched_task_hidden(const task_t *t) { (void)t; return 0; }

/* ---- the sink -------------------------------------------------------------------- */

#define DPCD_SPACE 0x100000
static u8 *dpcd;
static u8 edid_rom[256];
static unsigned edid_off;
static int defer_native; /* the next N native requests are DEFERred */
static int short_write; /* the next native write gets this many through */
static int sink_hpd = 1;
static int lttprs; /* repeaters on the link */
static int repeater_mode_writes;
/* per PHY (0 = the sink, i = LTTPR i): what it needs to lock */
static int need_swing[9], need_pre[9];
/* the order the PHYs reached channel EQ */
static int eq_order[16], neq;
static int phy_eq_seen[9];
/* the receiver holds the trained link after training (until a test
 * takes it away) */
static int phy_locked[9];

struct xlog {
	u8 request;
	unsigned address;
	unsigned size;
};
static struct xlog xlog[8192];
static int nxlog;

static unsigned pattern_reg(int phy)
{
	return phy ? DP_TRAINING_PATTERN_SET_PHY_REPEATER(phy) : DP_TRAINING_PATTERN_SET;
}

static int lane_count_set(void)
{
	int n = dpcd[DP_LANE_COUNT_SET] & 0x1f;
	return n ? n : 1;
}

static bool non_transparent(void)
{
	return lttprs && dpcd[DP_PHY_REPEATER_MODE] == DP_PHY_REPEATER_MODE_NON_TRANSPARENT;
}

/* The receiver `phy' locked (CR) / equalised: the levels written to it
 * (lane sets after its pattern register) against what it needs; in
 * non-transparent mode a PHY further along sees training only once the
 * repeaters before it (higher numbers) are trained and out of training. */
static void phy_state(int phy, int *cr, int *eq)
{
	unsigned base = pattern_reg(phy);
	u8 pat = dpcd[base] & DP_TRAINING_PATTERN_MASK_1_4;
	int lanes = lane_count_set();

	*cr = *eq = 0;
	if (!pat) {
		*cr = *eq = phy_locked[phy];
		return;
	}
	if (pat == DP_TRAINING_PATTERN_1)
		phy_locked[phy] = 0;
	if (non_transparent()) {
		for (int up = lttprs; up > phy; up--)
			if (!phy_eq_seen[up] || (dpcd[pattern_reg(up)] & DP_TRAINING_PATTERN_MASK_1_4))
				return;
	}
	int cr_all = 1, eq_all = 1;
	for (int l = 0; l < lanes; l++) {
		u8 set = dpcd[base + 1 + l];
		int sw = set & DP_TRAIN_VOLTAGE_SWING_MASK;
		int pre = (set & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT;
		if (sw < need_swing[phy])
			cr_all = 0;
		if (pre < need_pre[phy] || pat == DP_TRAINING_PATTERN_1)
			eq_all = 0;
	}
	*cr = cr_all;
	*eq = cr_all && eq_all;
	if (*eq)
		phy_locked[phy] = 1;
	if (*eq && !phy_eq_seen[phy]) {
		phy_eq_seen[phy] = 1;
		if (neq < 16)
			eq_order[neq++] = phy;
	}
}

/* The status block of a PHY, generated when read. */
static void fill_status(int phy)
{
	int cr, eq, lanes = lane_count_set();
	u8 st[6] = { 0 };

	phy_state(phy, &cr, &eq);
	for (int l = 0; l < lanes; l++) {
		u8 bits = (cr ? DP_LANE_CR_DONE : 0) |
			  (eq ? DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED : 0);
		st[l / 2] |= (u8)(bits << (4 * (l & 1)));
	}
	st[2] = eq ? DP_INTERLANE_ALIGN_DONE : 0;
	u8 adj = (u8)((need_swing[phy] > 3 ? 3 : need_swing[phy]) | (need_pre[phy] << 2));
	if (phy == 0) {
		st[4] = (u8)(adj | adj << 4);
		st[5] = st[4];
		memcpy(dpcd + DP_LANE0_1_STATUS, st, 6);
	} else {
		st[3] = (u8)(adj | adj << 4);
		st[4] = st[3];
		memcpy(dpcd + DP_LANE0_1_STATUS_PHY_REPEATER(phy), st, 5);
	}
}

static void sink_read_hook(unsigned addr)
{
	if (addr >= DP_LANE0_1_STATUS && addr <= DP_ADJUST_REQUEST_LANE2_3)
		fill_status(0);
	for (int p = 1; p <= lttprs; p++)
		if (addr >= (unsigned)DP_LANE0_1_STATUS_PHY_REPEATER(p) &&
		    addr <= (unsigned)DP_LANE0_1_STATUS_PHY_REPEATER(p) + 4)
			fill_status(p);
}

/* One AUX transaction the hardware model hands over: returns the reply
 * length, the reply in `rx'. */
static int sink_transaction(const u8 *tx, int txlen, u8 *rx)
{
	u8 req = tx[0] >> 4;
	unsigned addr = ((unsigned)(tx[0] & 0xf) << 16) | ((unsigned)tx[1] << 8) | tx[2];
	unsigned size = txlen > 3 ? (unsigned)tx[3] + 1 : 0;

	if (nxlog < (int)ARRAY_SIZE(xlog)) {
		xlog[nxlog].request = req;
		xlog[nxlog].address = addr;
		xlog[nxlog].size = size;
		nxlog++;
	}
	if (!(req & 0x8)) {
		/* I2C over AUX: the EDID EEPROM */
		u8 i2c = req & 0x3;
		if (addr != 0x50) {
			rx[0] = DP_AUX_I2C_REPLY_NACK << 4;
			return 1;
		}
		if (i2c == DP_AUX_I2C_WRITE || i2c == DP_AUX_I2C_WRITE_STATUS_UPDATE) {
			if (size)
				edid_off = tx[4];
			rx[0] = 0;
			return 1;
		}
		rx[0] = 0;
		for (unsigned i = 0; i < size; i++)
			rx[1 + i] = edid_rom[(edid_off++) & 0xff];
		return 1 + (int)size;
	}
	if (defer_native > 0) {
		defer_native--;
		rx[0] = DP_AUX_NATIVE_REPLY_DEFER << 4;
		return 1;
	}
	if (req == DP_AUX_NATIVE_READ) {
		sink_read_hook(addr);
		rx[0] = 0;
		memcpy(rx + 1, dpcd + addr, size);
		return 1 + (int)size;
	}
	/* native write */
	unsigned n = size;
	if (short_write) {
		n = (unsigned)short_write;
		short_write = 0;
		memcpy(dpcd + addr, tx + 4, n);
		rx[0] = DP_AUX_NATIVE_REPLY_NACK << 4;
		rx[1] = (u8)n;
		return 2;
	}
	memcpy(dpcd + addr, tx + 4, n);
	if (addr == DP_PHY_REPEATER_MODE)
		repeater_mode_writes++;
	rx[0] = 0;
	return 1;
}

/* ---- the i915 register model --------------------------------------------------- */

static uint32_t aux_ctl, aux_data[5];
static int aux_ctl_reg_used = -1;
static int lock_held_in_xfer = 1;
static struct intel_output *cur_out;

static uint32_t regs_reg[64], regs_val[64];
static int nregs;

uint32_t i915_read32(struct i915_device *i915, uint32_t reg)
{
	(void)i915;
	if (reg == (uint32_t)aux_ctl_reg_used)
		return aux_ctl;
	if (aux_ctl_reg_used >= 0 && reg >= (uint32_t)aux_ctl_reg_used + 4 &&
	    reg < (uint32_t)aux_ctl_reg_used + 4 + 20)
		return aux_data[(reg - aux_ctl_reg_used - 4) / 4];
	for (int i = 0; i < nregs; i++)
		if (regs_reg[i] == reg)
			return regs_val[i];
	return 0;
}

void i915_write32(struct i915_device *i915, uint32_t reg, uint32_t v)
{
	(void)i915;
	if (reg == (uint32_t)aux_ctl_reg_used) {
		if (!(v & DP_AUX_CH_CTL_SEND_BUSY)) {
			aux_ctl = 0; /* sticky bits cleared */
			return;
		}
		if (cur_out && cur_out->aux.hw_lock && !cur_out->aux.hw_lock->writer)
			lock_held_in_xfer = 0;
		int send = (int)((v & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >> DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT);
		u8 tx[20] = { 0 }, rx[20];
		for (int i = 0; i < send; i++)
			tx[i] = (u8)(aux_data[i / 4] >> (24 - 8 * (i % 4)));
		int got = sink_transaction(tx, send, rx);
		memset(aux_data, 0, sizeof(aux_data));
		for (int i = 0; i < got; i++)
			aux_data[i / 4] |= (uint32_t)rx[i] << (24 - 8 * (i % 4));
		aux_ctl = DP_AUX_CH_CTL_DONE | ((uint32_t)got << DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT);
		return;
	}
	if (aux_ctl_reg_used >= 0 && reg >= (uint32_t)aux_ctl_reg_used + 4 &&
	    reg < (uint32_t)aux_ctl_reg_used + 4 + 20) {
		aux_data[(reg - aux_ctl_reg_used - 4) / 4] = v;
		return;
	}
	for (int i = 0; i < nregs; i++)
		if (regs_reg[i] == reg) {
			regs_val[i] = v;
			return;
		}
	if (nregs < 64) {
		regs_reg[nregs] = reg;
		regs_val[nregs++] = v;
	}
}

int intel_has_lt_phy(struct i915_device *i915) { (void)i915; return 0; }
int intel_hpd_live(struct i915_device *i915, int port) { (void)i915; (void)port; return sink_hpd; }

/* The port's side of training. */
static int hw_pattern;
static int hw_level, hw_level_writes, hw_level_writes_after_first_hop;
static int in_first_hop_done;
static int link_status_prop = DRM_MODE_LINK_STATUS_GOOD;
static int link_off_calls;

void intel_ddi_set_train_pattern(struct i915_device *i915, struct intel_output *o, uint32_t p)
{
	(void)i915; (void)o;
	hw_pattern = (int)p;
}
void intel_ddi_dp_tp_enable(struct i915_device *i915, struct intel_output *o, int ef)
{
	(void)i915; (void)o; (void)ef;
	hw_pattern = DP_TP_CTL_LINK_TRAIN_PAT1;
}
void intel_ddi_buf_enable(struct i915_device *i915, struct intel_output *o, int lanes, int level)
{
	(void)i915; (void)o; (void)lanes;
	hw_level = level;
}
int intel_ddi_wait_idle_done(struct i915_device *i915, struct intel_output *o)
{
	(void)i915; (void)o;
	return 0;
}
void intel_ddi_set_buf_trans(struct i915_device *i915, struct intel_output *o, int level)
{
	(void)i915; (void)o;
	hw_level = level;
	hw_level_writes++;
	if (in_first_hop_done)
		hw_level_writes_after_first_hop++;
}
int intel_ddi_dp_level_for(int swing, int preemph)
{
	static const int base[4] = { 0, 4, 7, 9 };
	if (swing + preemph > 3)
		preemph = 3 - swing;
	return base[swing] + preemph;
}
int intel_ddi_level_in_buf_ctl(struct i915_device *i915) { (void)i915; return 0; }
uint32_t intel_dp_tp_ctl_reg(struct i915_device *i915, const struct intel_output *o)
{
	(void)i915; (void)o;
	return 0x64040;
}
void intel_ddi_buf_disable(struct i915_device *i915, struct intel_output *o)
{
	(void)i915; (void)o;
	link_off_calls++;
}
void drm_connector_set_link_status_property(struct drm_connector *c, uint64_t s)
{
	c->link_status = s;
	link_status_prop = (int)s;
}


/* What intel_dp.c calls besides. */
static int hotplug_events;
void drm_connector_hotplug(struct drm_device *dev, int conn)
{
	(void)dev; (void)conn;
	hotplug_events++;
}
int intel_display_verx100(struct i915_device *i915) { return i915->info->display_ver * 100; }
int intel_dpll_output_rate_supported(struct i915_device *i915, const struct intel_output *o,
				     uint32_t link_rate_khz)
{
	(void)i915; (void)o;
	return link_rate_khz <= 810000;
}
void intel_power_get(struct i915_device *i915, enum intel_power_domain d) { (void)i915; (void)d; }
void intel_power_put(struct i915_device *i915, enum intel_power_domain d) { (void)i915; (void)d; }
void intel_pps_vdd_on(struct i915_device *i915, struct intel_output *o) { (void)i915; (void)o; }
int intel_tc_max_lanes(struct i915_device *i915, struct intel_output *o) { (void)i915; (void)o; return 4; }
/* the compression side of a (re)training: no stream is compressed here */
void intel_dsc_pre_link_train(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p)
{
	(void)i915;
	(void)o;
	(void)p;
}
void intel_dsc_fec_enable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p)
{
	(void)i915;
	(void)o;
	(void)p;
}

/* ---- set-up ------------------------------------------------------------------------ */

static struct i915_device *i915;
static struct intel_device_info info;
static struct intel_output *out;

static void sink_reset(void)
{
	memset(dpcd, 0, DPCD_SPACE);
	/* an EDID base block that passes the checks: the header, no
	 * extension, the checksum */
	static const u8 hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	u8 sum = 0;
	for (int i = 0; i < 256; i++)
		edid_rom[i] = (u8)(i * 7 + 1);
	memcpy(edid_rom, hdr, 8);
	edid_rom[18] = 1; /* EDID 1.4 */
	edid_rom[19] = 4;
	edid_rom[126] = 0;
	for (int i = 0; i < 127; i++)
		sum = (u8)(sum + edid_rom[i]);
	edid_rom[127] = (u8)(0x100 - sum);
	edid_off = 0;
	defer_native = short_write = 0;
	sink_hpd = 1;
	lttprs = 0;
	repeater_mode_writes = 0;
	memset(need_swing, 0, sizeof(need_swing));
	memset(need_pre, 0, sizeof(need_pre));
	memset(phy_eq_seen, 0, sizeof(phy_eq_seen));
	memset(phy_locked, 0, sizeof(phy_locked));
	neq = 0;
	nxlog = 0;
	busy_us = slept_us = 0;
	hw_level_writes = hw_level_writes_after_first_hop = 0;
	in_first_hop_done = 0;
	link_off_calls = 0;
	/* a DP 1.4 sink: HBR2 x4, TPS3, enhanced framing */
	dpcd[DP_DPCD_REV] = 0x14;
	dpcd[DP_MAX_LINK_RATE] = DP_LINK_BW_5_4;
	dpcd[DP_MAX_LANE_COUNT] = 4 | DP_TPS3_SUPPORTED | DP_ENHANCED_FRAME_CAP;
}

static void setup(int display_ver, enum intel_display_model model, int edp)
{
	if (!i915)
		i915 = calloc(1, sizeof(*i915));
	memset(i915, 0, sizeof(*i915));
	memset(&info, 0, sizeof(info));
	info.display_ver = (uint8_t)display_ver;
	info.platform = 0;
	i915->info = &info;
	i915->display.model = model;
	i915->drm.nconn = 1;
	i915->drm.conn[0].link_status = DRM_MODE_LINK_STATUS_GOOD;
	out = &i915->display.outputs[0];
	memset(out, 0, sizeof(*out));
	out->present = 1;
	out->port = PORT_B;
	out->conn = 0;
	out->is_edp = edp;
	out->type = edp ? INTEL_OUTPUT_EDP : INTEL_OUTPUT_DP;
	intel_dp_aux_init(i915, &out->aux, out->port);
	aux_ctl_reg_used = (int)out->aux.ctl_reg;
	aux_ctl = 0;
	cur_out = out;
	drm_dp_dpcd_set_probe(&out->aux.dp, false);
	sink_reset();
}

/* ---- tests ----------------------------------------------------------------------- */

static void test_native(void)
{
	u8 buf[32];

	setup(9, INTEL_DISPLAY_SKL, 0);
	for (int i = 0; i < 32; i++)
		dpcd[0x400 + i] = (u8)(0xa0 + i);
	CHECK_EQ(intel_dp_aux_native_read(&out->aux, 0x400, buf, 20), 20);
	CHECK(!memcmp(buf, dpcd + 0x400, 20), "read data");
	/* 16 + 4: two transactions */
	CHECK_EQ(nxlog, 2);
	CHECK_EQ(xlog[0].size, 16);
	CHECK_EQ(xlog[1].address, 0x410);

	u8 v = 0x5a;
	nxlog = 0;
	CHECK_EQ(intel_dp_dpcd_write8(&out->aux, 0x107, v), 0);
	CHECK_EQ(dpcd[0x107], 0x5a);
	CHECK_EQ(xlog[0].request, DP_AUX_NATIVE_WRITE);

	/* DEFER is the library's to retry */
	defer_native = 3;
	nxlog = 0;
	u8 r = 0;
	CHECK_EQ(intel_dp_dpcd_read8(&out->aux, 0x107, &r), 0);
	CHECK_EQ(r, 0x5a);
	CHECK_EQ(nxlog, 4);

	/* a NACK after a short write: the library sends the write again */
	u8 w[4] = { 1, 2, 3, 4 };
	short_write = 2;
	nxlog = 0;
	CHECK_EQ(intel_dp_aux_native_write(&out->aux, 0x500, w, 4), 4);
	CHECK_EQ(nxlog, 2);
	CHECK(!memcmp(dpcd + 0x500, w, 4), "written");

	/* the transfer ran under the hardware channel's lock */
	CHECK(out->aux.hw_lock != NULL, "channel lock");
	CHECK(lock_held_in_xfer, "lock held in every transfer");
	CHECK(!out->aux.hw_lock->writer && !out->aux.dp.hw_mutex.writer, "locks released");
}

static void test_hook_direct(void)
{
	struct drm_dp_aux_msg msg;
	u8 b[4] = { 0 };

	setup(9, INTEL_DISPLAY_SKL, 0);
	dpcd[0x10] = 0x77;
	defer_native = 1;
	memset(&msg, 0, sizeof(msg));
	msg.request = DP_AUX_NATIVE_READ;
	msg.address = 0x10;
	msg.buffer = b;
	msg.size = 1;
	CHECK_EQ(intel_dp_aux_transfer(&out->aux.dp, &msg), 0);
	CHECK_EQ(msg.reply, DP_AUX_NATIVE_REPLY_DEFER);
	CHECK_EQ(intel_dp_aux_transfer(&out->aux.dp, &msg), 1);
	CHECK_EQ(msg.reply, DP_AUX_NATIVE_REPLY_ACK);
	CHECK_EQ(b[0], 0x77);

	/* an address-only I2C transaction: 3 bytes, no length */
	memset(&msg, 0, sizeof(msg));
	msg.request = DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT;
	msg.address = 0x50;
	nxlog = 0;
	CHECK_EQ(intel_dp_aux_transfer(&out->aux.dp, &msg), 0);
	CHECK_EQ(xlog[0].size, 0);
	CHECK_EQ(msg.reply, 0);

	/* too big for the five data registers */
	u8 big[20];
	msg.request = DP_AUX_NATIVE_WRITE;
	msg.buffer = big;
	msg.size = 17;
	CHECK_EQ(intel_dp_aux_transfer(&out->aux.dp, &msg), -E2BIG);

	/* an external sink gone: refused, after the glitch grace */
	sink_hpd = 0;
	msg.request = DP_AUX_NATIVE_READ;
	msg.buffer = b;
	msg.size = 1;
	uint64_t t0 = busy_us;
	CHECK_EQ(intel_dp_aux_transfer(&out->aux.dp, &msg), -ENXIO);
	CHECK(busy_us - t0 >= 3900 && busy_us - t0 <= 4200, "4 ms grace (%lu us)",
	      (unsigned long)(busy_us - t0));
	/* a panel is always asked */
	setup(9, INTEL_DISPLAY_SKL, 1);
	sink_hpd = 0;
	memset(&msg, 0, sizeof(msg));
	msg.request = DP_AUX_NATIVE_READ;
	msg.address = 0;
	msg.buffer = b;
	msg.size = 1;
	CHECK_EQ(intel_dp_aux_transfer(&out->aux.dp, &msg), 1);
}

static void test_i2c(void)
{
	u8 off = 0, edid[128];
	struct i2c_msg m[2] = {
		{ .addr = 0x50, .flags = 0, .len = 1, .buf = &off },
		{ .addr = 0x50, .flags = I2C_M_RD, .len = 128, .buf = edid },
	};

	setup(9, INTEL_DISPLAY_SKL, 0);
	CHECK_EQ(i2c_transfer(intel_dp_aux_ddc(&out->aux), m, 2), 2);
	CHECK(!memcmp(edid, edid_rom, 128), "EDID bytes");
	/* bare-address start, the write, a bare start for the read, 8 x 16,
	 * and the closing bare address without MOT */
	int bare = 0, last_mot = -1;
	for (int i = 0; i < nxlog; i++) {
		if (xlog[i].size == 0)
			bare++;
		last_mot = !!(xlog[i].request & DP_AUX_I2C_MOT);
	}
	CHECK(bare >= 3, "bare address packets (%d)", bare);
	CHECK_EQ(last_mot, 0);
	CHECK_EQ(xlog[nxlog - 1].size, 0);
}

static void prepare_link(int lanes, uint32_t rate)
{
	out->lane_count = lanes;
	out->link_rate_khz = rate;
	out->active = 0;
	mm_memcpy(out->dpcd, dpcd, DP_RECEIVER_CAP_SIZE);
}

static void test_train_plain(void)
{
	setup(9, INTEL_DISPLAY_SKL, 0);
	need_swing[0] = 2;
	need_pre[0] = 1;
	prepare_link(4, 270000);
	CHECK_EQ(intel_dp_link_train(i915, out), 0);
	CHECK_EQ(dpcd[DP_LINK_BW_SET], DP_LINK_BW_2_7);
	CHECK_EQ(dpcd[DP_LANE_COUNT_SET], 4 | DP_LANE_COUNT_ENHANCED_FRAME_EN);
	CHECK_EQ(dpcd[DP_MAIN_LINK_CHANNEL_CODING_SET], DP_SET_ANSI_8B10B);
	CHECK_EQ(dpcd[DP_DOWNSPREAD_CTRL], 0);
	CHECK_EQ(dpcd[DP_TRAINING_PATTERN_SET], DP_TRAINING_PATTERN_DISABLE);
	CHECK_EQ(hw_pattern, DP_TP_CTL_LINK_TRAIN_NORMAL);
	CHECK_EQ(out->train_set[0] & 3, 2);
	CHECK_EQ((out->train_set[0] >> 3) & 3, 1);
	CHECK_EQ(hw_level, intel_ddi_dp_level_for(2, 1));
	/* ver 9: the repeater range is never touched */
	int touched = 0;
	for (int i = 0; i < nxlog; i++)
		if (xlog[i].address >= 0xf0000)
			touched = 1;
	CHECK(!touched, "no LTTPR access before display version 10");
	/* DPCD 1.4: 100 us clock recovery waits */
	CHECK(busy_us > 0, "waited");

	/* MSA ignore and spread kept with the link set-up */
	setup(9, INTEL_DISPLAY_SKL, 0);
	prepare_link(2, 162000);
	out->ssc = 1;
	CHECK_EQ(intel_dp_set_msa_timing_par_ignore(out, true), 0);
	CHECK_EQ(dpcd[DP_DOWNSPREAD_CTRL], 0); /* link down: not written yet */
	CHECK_EQ(intel_dp_link_train(i915, out), 0);
	CHECK_EQ(dpcd[DP_DOWNSPREAD_CTRL], DP_SPREAD_AMP_0_5 | DP_MSA_TIMING_PAR_IGNORE_EN);
	out->active = 1;
	CHECK_EQ(intel_dp_set_msa_timing_par_ignore(out, false), 0);
	CHECK_EQ(dpcd[DP_DOWNSPREAD_CTRL], DP_SPREAD_AMP_0_5);
}

static void test_train_fail(void)
{
	setup(9, INTEL_DISPLAY_SKL, 0);
	need_swing[0] = 4; /* never */
	prepare_link(4, 270000);
	int before = nxlog;
	CHECK_EQ(intel_dp_link_train(i915, out), -EIO);
	CHECK_EQ(dpcd[DP_TRAINING_PATTERN_SET], DP_TRAINING_PATTERN_DISABLE);
	int status_reads = 0;
	for (int i = before; i < nxlog; i++)
		if (xlog[i].request == DP_AUX_NATIVE_READ && xlog[i].address == DP_LANE0_1_STATUS)
			status_reads++;
	CHECK(status_reads <= 10, "at most 10 clock recovery passes (%d)", status_reads);

	/* EQ never: 6 passes */
	setup(9, INTEL_DISPLAY_SKL, 0);
	need_pre[0] = 4;
	prepare_link(1, 162000);
	CHECK_EQ(intel_dp_link_train(i915, out), -EIO);
}

static void test_edp_rate_select(void)
{
	setup(9, INTEL_DISPLAY_SKL, 1);
	dpcd[DP_EDP_DPCD_REV] = DP_EDP_14;
	prepare_link(2, 216000);
	out->nsink_rates = 3;
	out->sink_rates_khz[0] = 162000;
	out->sink_rates_khz[1] = 216000;
	out->sink_rates_khz[2] = 270000;
	out->use_rate_select = 1;
	CHECK_EQ(intel_dp_link_train(i915, out), 0);
	CHECK_EQ(dpcd[DP_LINK_RATE_SET], 1);
	CHECK_EQ(dpcd[DP_LINK_BW_SET], 0);
	/* the rate table re-read for muxes that snoop it */
	int table_read = 0;
	for (int i = 0; i < nxlog; i++)
		if (xlog[i].request == DP_AUX_NATIVE_READ && xlog[i].address == DP_SUPPORTED_LINK_RATES)
			table_read = 1;
	CHECK(table_read, "rate table re-read");
}

static void set_lttpr_caps(int n)
{
	lttprs = n;
	dpcd[DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV] = 0x14;
	dpcd[DP_MAX_LINK_RATE_PHY_REPEATER] = DP_LINK_BW_5_4;
	dpcd[DP_PHY_REPEATER_CNT] = n ? (u8)(0x80 >> (n - 1)) : 0;
	dpcd[DP_PHY_REPEATER_MODE] = DP_PHY_REPEATER_MODE_TRANSPARENT;
	dpcd[DP_MAX_LANE_COUNT_PHY_REPEATER] = 2;
}

static void test_lttpr(void)
{
	/* two repeaters, non-transparent */
	setup(12, INTEL_DISPLAY_TGL, 0);
	set_lttpr_caps(2);
	need_swing[2] = 1;
	need_swing[1] = 2;
	need_pre[1] = 1;
	need_swing[0] = 1;
	int n = intel_dp_init_lttpr_and_dprx_caps(i915, out, false);
	CHECK_EQ(n, I915_FEAT_DP_LTTPR == 2 ? 2 : 0);
	CHECK_EQ(intel_dp_lttpr_max_lane_count(out), 2);
	CHECK_EQ(intel_dp_lttpr_max_link_rate(out), 540000);
	if (I915_FEAT_DP_LTTPR == 2) {
		CHECK_EQ(dpcd[DP_PHY_REPEATER_MODE], DP_PHY_REPEATER_MODE_NON_TRANSPARENT);
		CHECK_EQ(repeater_mode_writes, 2); /* transparent first, then non */
	}
	if (I915_FEAT_DP_LTTPR == 1)
		CHECK_EQ(dpcd[DP_PHY_REPEATER_MODE], DP_PHY_REPEATER_MODE_TRANSPARENT);

	prepare_link(2, 270000);
	int writes0 = hw_level_writes;
	CHECK_EQ(intel_dp_link_train(i915, out), 0);
	if (I915_FEAT_DP_LTTPR == 2) {
		CHECK_EQ(neq, 3);
		CHECK_EQ(eq_order[0], 2);
		CHECK_EQ(eq_order[1], 1);
		CHECK_EQ(eq_order[2], 0);
		/* every repeater left without a pattern */
		CHECK_EQ(dpcd[DP_TRAINING_PATTERN_SET_PHY_REPEATER(DP_PHY_LTTPR(0))], 0);
		CHECK_EQ(dpcd[DP_TRAINING_PATTERN_SET_PHY_REPEATER(DP_PHY_LTTPR(1))], 0);
		/* the port's levels are the first hop's (repeater 2 asked for
		 * swing 1), the farther repeater got its own over AUX */
		CHECK_EQ(out->train_set[0] & 3, 1);
		CHECK_EQ(dpcd[DP_TRAINING_LANE0_SET_PHY_REPEATER(DP_PHY_LTTPR(0))] & 3, 2);
		CHECK(hw_level_writes > writes0, "source levels written");
	}
	CHECK_EQ(dpcd[DP_TRAINING_PATTERN_SET], 0);

	/* a repeater field with no repeater: the mode is never written */
	setup(12, INTEL_DISPLAY_TGL, 0);
	set_lttpr_caps(0);
	CHECK_EQ(intel_dp_init_lttpr_and_dprx_caps(i915, out, false), 0);
	CHECK_EQ(repeater_mode_writes, 0);
	prepare_link(4, 270000);
	CHECK_EQ(intel_dp_link_train(i915, out), 0);

	/* an active link keeps the repeaters' mode */
	setup(12, INTEL_DISPLAY_TGL, 0);
	set_lttpr_caps(1);
	CHECK_EQ(intel_dp_init_lttpr_and_dprx_caps(i915, out, true), 0);
	CHECK_EQ(repeater_mode_writes, 0);

	/* Gemini Lake: no repeater detection */
	setup(10, INTEL_DISPLAY_BXT, 0);
	info.platform = I915_PLATFORM_GEMINILAKE;
	set_lttpr_caps(1);
	CHECK_EQ(intel_dp_init_lttpr_and_dprx_caps(i915, out, false), 0);
	for (int i = 0; i < nxlog; i++)
		CHECK(xlog[i].address < 0xf0000, "no LTTPR access on Gemini Lake");
}

static void test_retrain(void)
{
	setup(12, INTEL_DISPLAY_TGL, 0);
	prepare_link(4, 270000);
	out->active = 1;
	i915->drm.conn[0].link_status = DRM_MODE_LINK_STATUS_BAD;
	CHECK_EQ(intel_dp_retrain_link(i915, out), 0);
	CHECK_EQ(link_off_calls, 1);
	CHECK_EQ(link_status_prop, DRM_MODE_LINK_STATUS_GOOD);
	setup(14, INTEL_DISPLAY_MTL, 0);
	prepare_link(4, 270000);
	out->active = 1;
	CHECK_EQ(intel_dp_retrain_link(i915, out), -EOPNOTSUPP);
	CHECK_EQ(link_off_calls, 0);
}


static void test_detect(void)
{
	/* a plain DP 1.4 sink at HBR2 */
	setup(9, INTEL_DISPLAY_SKL, 0);
	drm_dp_dpcd_set_probe(&out->aux.dp, true);
	CHECK_EQ(intel_dp_detect(i915, out), 1);
	CHECK_EQ(out->nsink_rates, 3);
	CHECK_EQ(out->sink_rates_khz[2], 540000);
	CHECK_EQ(out->sink_count, -1);
	CHECK_EQ(out->use_rate_select, 0);
	/* no EDID quirk: the throw-away read is off after detect */
	CHECK(out->aux.dp.dpcd_probe_disabled, "probe off after detect");

	/* the extended caps raise the rate to HBR3 */
	setup(9, INTEL_DISPLAY_SKL, 0);
	dpcd[DP_DPCD_REV] = 0x12;
	dpcd[DP_TRAINING_AUX_RD_INTERVAL] = DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT;
	memcpy(dpcd + DP_DP13_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE);
	dpcd[DP_DP13_DPCD_REV] = 0x14;
	dpcd[DP_DP13_DPCD_REV + DP_MAX_LINK_RATE] = DP_LINK_BW_8_1;
	CHECK_EQ(intel_dp_detect(i915, out), 1);
	CHECK_EQ(out->nsink_rates, I915_FEAT_DP_EXT_CAPS ? 4 : 3);
	CHECK_EQ(out->dpcd[DP_DPCD_REV], I915_FEAT_DP_EXT_CAPS ? 0x14 : 0x12);

	/* a branch device that reports its sink: SINK_COUNT decides */
	setup(9, INTEL_DISPLAY_SKL, 0);
	dpcd[DP_DOWNSTREAMPORT_PRESENT] = DP_DWN_STRM_PORT_PRESENT | DP_DETAILED_CAP_INFO_AVAILABLE;
	dpcd[DP_DOWN_STREAM_PORT_COUNT] = 1;
	dpcd[DP_DOWNSTREAM_PORT_0] = DP_DS_PORT_TYPE_HDMI | DP_DS_PORT_HPD;
	dpcd[DP_SINK_COUNT] = 0;
	i915->drm.conn[0].type = DRM_MODE_CONNECTOR_DisplayPort;
	CHECK_EQ(intel_dp_detect(i915, out), 0);
	dpcd[DP_SINK_COUNT] = 1;
	CHECK_EQ(intel_dp_detect(i915, out), 1);
	CHECK_EQ(out->sink_count, 1);
	CHECK_EQ(out->downstream_ports[0], DP_DS_PORT_TYPE_HDMI | DP_DS_PORT_HPD);
	/* its dot clock limit (DP_DS_MAX_* in the detailed caps) */
	dpcd[DP_DOWNSTREAM_PORT_0 + 1] = 150000 / 2500; /* 150 MHz TMDS */
	CHECK_EQ(intel_dp_detect(i915, out), 1);
	CHECK_EQ(intel_dp_read_edid(i915, out), 0);
	{
		struct drm_display_mode m;
		memset(&m, 0, sizeof(m));
		m.clock = 148500;
		CHECK_EQ(intel_dp_mode_valid_downstream(i915, out, &m), MODE_OK);
		m.clock = 297000;
		CHECK(intel_dp_mode_valid_downstream(i915, out, &m) == MODE_CLOCK_HIGH,
		      "TMDS limit (max %d)", out->dfp.max_tmds_clock);
	}
	/* nothing on the line: not even asked */
	sink_hpd = 0;
	nxlog = 0;
	CHECK_EQ(intel_dp_detect(i915, out), 0);
	CHECK_EQ(nxlog, 0);

	/* a repeater limits the rate and the width */
	setup(12, INTEL_DISPLAY_TGL, 0);
	set_lttpr_caps(1);
	dpcd[DP_MAX_LINK_RATE_PHY_REPEATER] = DP_LINK_BW_2_7;
	CHECK_EQ(intel_dp_detect(i915, out), 1);
	if (I915_FEAT_DP_LTTPR) {
		CHECK_EQ(out->nsink_rates, 2);
		CHECK_EQ(intel_dp_max_lanes(out), 2);
	}

	/* an eDP 1.4 panel with a rate table */
	setup(9, INTEL_DISPLAY_SKL, 1);
	dpcd[DP_EDP_DPCD_REV] = DP_EDP_14;
	dpcd[DP_SUPPORTED_LINK_RATES + 0] = (u8)(8100 & 0xff); /* 1.62 GHz / 200 kHz */
	dpcd[DP_SUPPORTED_LINK_RATES + 1] = (u8)(8100 >> 8);
	dpcd[DP_SUPPORTED_LINK_RATES + 2] = (u8)(10800 & 0xff); /* 2.16 */
	dpcd[DP_SUPPORTED_LINK_RATES + 3] = (u8)(10800 >> 8);
	dpcd[DP_SUPPORTED_LINK_RATES + 4] = (u8)(13500 & 0xff); /* 2.7 */
	dpcd[DP_SUPPORTED_LINK_RATES + 5] = (u8)(13500 >> 8);
	CHECK_EQ(intel_dp_detect(i915, out), 1);
	CHECK_EQ(out->use_rate_select, 1);
	CHECK_EQ(out->nsink_rates, 3);
	CHECK_EQ(out->sink_rates_khz[1], 216000);
	CHECK(out->aux.dp.dpcd_probe_disabled, "no probe on a panel");
	/* an eDP 1.3 panel: the DP_MAX_LINK_RATE rates */
	setup(9, INTEL_DISPLAY_SKL, 1);
	dpcd[DP_EDP_DPCD_REV] = 0x02;
	CHECK_EQ(intel_dp_detect(i915, out), 1);
	CHECK_EQ(out->use_rate_select, 0);
	CHECK_EQ(out->nsink_rates, 3);
}

static void test_hpd_check(void)
{
	/* the link is fine: nothing happens */
	setup(12, INTEL_DISPLAY_TGL, 0);
	prepare_link(4, 270000);
	CHECK_EQ(intel_dp_link_train(i915, out), 0);
	out->active = 1;
	out->detected = 1;
	link_off_calls = 0;
	CHECK_EQ(intel_dp_hpd_check(i915, out), 0);
	CHECK_EQ(link_off_calls, 0);
	/* the sink dropped it: trained again in place */
	phy_locked[0] = 0;
	CHECK_EQ(intel_dp_hpd_check(i915, out), 0);
	CHECK_EQ(link_off_calls, 1);
	CHECK_EQ(i915->drm.conn[0].link_status, DRM_MODE_LINK_STATUS_GOOD);

	/* Meteor Lake: link-status BAD and a hotplug event instead */
	setup(14, INTEL_DISPLAY_MTL, 0);
	prepare_link(4, 270000);
	CHECK_EQ(intel_dp_link_train(i915, out), 0);
	out->active = 1;
	out->detected = 1;
	phy_locked[0] = 0;
	hotplug_events = 0;
	CHECK_EQ(intel_dp_hpd_check(i915, out), 0);
	CHECK_EQ(i915->drm.conn[0].link_status, DRM_MODE_LINK_STATUS_BAD);
	CHECK_EQ(hotplug_events, 1);
	/* the next training reports the link good again */
	CHECK_EQ(intel_dp_link_train(i915, out), 0);
	CHECK_EQ(i915->drm.conn[0].link_status, DRM_MODE_LINK_STATUS_GOOD);

	/* a sink that does not answer: looked at again, three times */
	setup(12, INTEL_DISPLAY_TGL, 0);
	out->lane_count = 4;
	out->active = 1;
	out->detected = 1;
	defer_native = 1000000;
	int again = 0;
	for (int i = 0; i < 4; i++)
		again += intel_dp_hpd_check(i915, out);
	defer_native = 0;
	CHECK_EQ(again, 3);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-v"))
		verbose = 1;
	dpcd = calloc(1, DPCD_SPACE);
	test_native();
	test_hook_direct();
	test_i2c();
	test_train_plain();
	test_train_fail();
	test_edp_rate_select();
	test_lttpr();
	test_retrain();
	test_detect();
	test_hpd_check();
	printf("i915 dp: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
