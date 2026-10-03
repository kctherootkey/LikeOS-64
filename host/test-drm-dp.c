/* Host test for the DisplayPort helpers (kernel/dev/gpu/drm/drm_dp_helper.c).
 *
 * The helper file is compiled into this one, so its static parts (the
 * I2C retry estimate) can be checked too.  A fake sink stands behind the
 * transfer hook: a DPCD register space, an EDID EEPROM at I2C address
 * 0x50, and a script of faults (native/I2C DEFER and NACK, -EBUSY,
 * timeouts, short replies, block reads that fail) applied one per AUX
 * transaction.  Every transaction is logged, so the request sequence --
 * bare address packets, MOT bits, WRITE_STATUS_UPDATE -- can be checked,
 * and the delays are counted instead of waited.
 *
 * Run by host/test-drm-dp.sh from the repository root; "-v" prints the
 * helpers' own messages.
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

/* The scheduler predicate the delay helper asks; the real declaration is
 * in the scheduler header the prelude replaces. */
int sched_task_hidden(const task_t *t);

#include "../kernel/dev/gpu/drm/drm_dp_helper.c"

/* ---- what the helpers call of the kernel ---------------------------------- */

uint64_t test_irq_flag = 0x200;
int test_locks_held;
static int verbose;
static int kprintf_lines;

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

	kprintf_lines++;
	if (verbose) {
		__builtin_va_start(ap, format);
		n = vprintf(format, ap);
		__builtin_va_end(ap);
	}
	return n;
}

/* Time: busy waits and sleeps are counted, never taken. */
static uint64_t busy_us, busy_calls, slept_us, sleep_calls;
static uint64_t now_ns = 1000000000ull;
static int highres;
static task_t test_task;
static int task_hidden;

void lapic_delay_us(uint32_t us)
{
	busy_us += us;
	busy_calls++;
	now_ns += (uint64_t)us * 1000;
}

void lapic_delay_ms(uint32_t ms)
{
	lapic_delay_us(ms * 1000);
}

uint64_t hrtimer_now_ns(void)
{
	return now_ns;
}

int hrtimer_is_highres(void)
{
	return highres;
}

int hrtimer_sleep_until(uint64_t abs_ns, uint64_t *remaining_ns)
{
	sleep_calls++;
	if (abs_ns > now_ns) {
		slept_us += (abs_ns - now_ns) / 1000;
		now_ns = abs_ns;
	}
	if (remaining_ns)
		*remaining_ns = 0;
	return 0;
}

task_t *sched_current(void)
{
	return &test_task;
}

int sched_task_hidden(const task_t *t)
{
	(void)t;
	return task_hidden;
}

/* ---- the fake sink ------------------------------------------------------------ */

enum fault {
	F_NONE,
	F_NATIVE_DEFER,
	F_NATIVE_NACK,
	F_I2C_DEFER,
	F_I2C_NACK,
	F_BUSY,
	F_TIMEOUT,
	F_SHORT, /* ACK with fault_arg bytes */
	F_BAD_REPLY, /* reply code 0x3: invalid */
};

#define DPCD_SPACE 0x100000
static u8 *dpcd; /* the sink's whole DPCD */
static u8 edid_rom[256]; /* behind I2C address 0x50 */
static unsigned edid_off;

static enum fault script[256];
static int script_arg[256];
static int script_len, script_pos;
/* Block reads (size > 1) fail with -EIO from the hook: an adapter that
 * only answers single bytes. */
static int fail_block_reads;
/* I2C reads and writes return at most this many bytes per transaction. */
static unsigned i2c_chunk_limit = 16;
/* Native writes to this address are NACKed (0 = none). */
static unsigned nack_write_addr;

struct xlog {
	u8 request;
	unsigned address;
	size_t size;
};
static struct xlog xlog[4096];
static int nxlog;
static int lock_was_held_in_transfer = 1;

static void sink_reset(void)
{
	memset(dpcd, 0, DPCD_SPACE);
	script_len = script_pos = 0;
	fail_block_reads = 0;
	i2c_chunk_limit = 16;
	nack_write_addr = 0;
	nxlog = 0;
	edid_off = 0;
	busy_us = busy_calls = slept_us = sleep_calls = 0;
}

static void script_add(enum fault f, int n, int arg)
{
	while (n-- > 0) {
		script[script_len] = f;
		script_arg[script_len] = arg;
		script_len++;
	}
}

static ssize_t fake_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	enum fault f = F_NONE;
	int arg = 0;
	u8 req = msg->request;
	u8 *buf = msg->buffer;
	size_t n;

	if (!aux->hw_mutex.writer)
		lock_was_held_in_transfer = 0;
	if (nxlog < (int)ARRAY_SIZE(xlog)) {
		xlog[nxlog].request = req;
		xlog[nxlog].address = msg->address;
		xlog[nxlog].size = msg->size;
		nxlog++;
	}
	if (msg->size > DP_AUX_MAX_PAYLOAD_BYTES)
		return -E2BIG;
	if (script_pos < script_len) {
		f = script[script_pos];
		arg = script_arg[script_pos];
		script_pos++;
	}

	switch (f) {
	case F_BUSY:
		return -EBUSY;
	case F_TIMEOUT:
		return -ETIMEDOUT;
	case F_NATIVE_DEFER:
		msg->reply = DP_AUX_NATIVE_REPLY_DEFER;
		return 0;
	case F_NATIVE_NACK:
		msg->reply = DP_AUX_NATIVE_REPLY_NACK;
		return 0;
	case F_BAD_REPLY:
		msg->reply = 0x3;
		return 0;
	default:
		break;
	}

	if (req == DP_AUX_NATIVE_READ) {
		if (fail_block_reads && msg->size > 1)
			return -EIO;
		n = msg->size;
		if (f == F_SHORT)
			n = (size_t)arg;
		if (msg->address + n <= DPCD_SPACE)
			memcpy(buf, dpcd + msg->address, n);
		msg->reply = DP_AUX_NATIVE_REPLY_ACK;
		return (ssize_t)n;
	}
	if (req == DP_AUX_NATIVE_WRITE) {
		if (nack_write_addr && msg->address == nack_write_addr) {
			msg->reply = DP_AUX_NATIVE_REPLY_NACK;
			return 0;
		}
		n = msg->size;
		if (f == F_SHORT)
			n = (size_t)arg;
		if (msg->address + n <= DPCD_SPACE)
			memcpy(dpcd + msg->address, buf, n);
		msg->reply = DP_AUX_NATIVE_REPLY_ACK;
		return (ssize_t)n;
	}

	/* I2C over AUX */
	if (f == F_I2C_DEFER) {
		msg->reply = DP_AUX_NATIVE_REPLY_ACK | DP_AUX_I2C_REPLY_DEFER;
		return 0;
	}
	if (f == F_I2C_NACK || msg->address != 0x50) {
		msg->reply = DP_AUX_NATIVE_REPLY_ACK | DP_AUX_I2C_REPLY_NACK;
		return 0;
	}
	msg->reply = DP_AUX_NATIVE_REPLY_ACK | DP_AUX_I2C_REPLY_ACK;
	n = msg->size;
	if (n > i2c_chunk_limit)
		n = i2c_chunk_limit;
	if (f == F_SHORT)
		n = (size_t)arg;
	switch (req & ~DP_AUX_I2C_MOT) {
	case DP_AUX_I2C_READ:
		for (size_t i = 0; i < n; i++)
			buf[i] = edid_rom[(edid_off + i) & 0xff];
		edid_off = (edid_off + n) & 0xff;
		return (ssize_t)n;
	case DP_AUX_I2C_WRITE:
	case DP_AUX_I2C_WRITE_STATUS_UPDATE:
		/* The first byte written sets the EEPROM offset. */
		if (n)
			edid_off = buf[0];
		return (ssize_t)n;
	}
	msg->reply = DP_AUX_NATIVE_REPLY_NACK;
	return 0;
}

static struct drm_dp_aux g_aux;

static void aux_setup(void)
{
	memset(&g_aux, 0, sizeof(g_aux));
	g_aux.name = "AUX TEST";
	g_aux.transfer = fake_transfer;
	drm_dp_aux_register(&g_aux);
}

/* ---- checks --------------------------------------------------------------- */

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

#define CHECK_EQ(a, b) \
	CHECK((long long)(a) == (long long)(b), "%s = %lld, want %lld", #a, \
	      (long long)(a), (long long)(b))

static int count_requests(u8 request, unsigned address)
{
	int i, n = 0;

	for (i = 0; i < nxlog; i++)
		if (xlog[i].request == request && xlog[i].address == address)
			n++;
	return n;
}

static void test_link_status(void)
{
	/* 0x202..0x207 */
	u8 ls[DP_LINK_STATUS_SIZE] = { 0x77, 0x77, DP_INTERLANE_ALIGN_DONE, 0, 0x00, 0x00 };

	CHECK(drm_dp_clock_recovery_ok(ls, 4), "CR 4 lanes");
	CHECK(drm_dp_channel_eq_ok(ls, 4), "EQ 4 lanes");
	ls[1] = 0x17; /* lane 3 lost symbol lock and EQ, lane 2 fine */
	CHECK(drm_dp_channel_eq_ok(ls, 2), "EQ 2 lanes");
	CHECK(drm_dp_channel_eq_ok(ls, 3), "EQ 3 lanes");
	CHECK(!drm_dp_channel_eq_ok(ls, 4), "EQ 4 lanes with lane 3 bad");
	CHECK(drm_dp_clock_recovery_ok(ls, 4), "CR 4 lanes, lane 3 CR done");
	ls[1] = 0x07;
	CHECK(!drm_dp_clock_recovery_ok(ls, 4), "CR lane 3 lost");
	ls[1] = 0x77;
	ls[2] = 0;
	CHECK(!drm_dp_channel_eq_ok(ls, 1), "EQ without interlane align");
	ls[2] = DP_POST_LT_ADJ_REQ_IN_PROGRESS;
	CHECK(drm_dp_post_lt_adj_req_in_progress(ls), "post-LT adjust in progress");

	/* Adjust requests: lane0 swing 1 pre 2, lane1 swing 3 pre 0,
	 * lane2 swing 2 pre 1, lane3 swing 0 pre 3. */
	ls[4] = (1 << 0) | (2 << 2) | (3 << 4) | (0 << 6);
	ls[5] = (2 << 0) | (1 << 2) | (0 << 4) | (3 << 6);
	CHECK_EQ(drm_dp_get_adjust_request_voltage(ls, 0), DP_TRAIN_VOLTAGE_SWING_LEVEL_1);
	CHECK_EQ(drm_dp_get_adjust_request_pre_emphasis(ls, 0), DP_TRAIN_PRE_EMPH_LEVEL_2);
	CHECK_EQ(drm_dp_get_adjust_request_voltage(ls, 1), DP_TRAIN_VOLTAGE_SWING_LEVEL_3);
	CHECK_EQ(drm_dp_get_adjust_request_pre_emphasis(ls, 1), DP_TRAIN_PRE_EMPH_LEVEL_0);
	CHECK_EQ(drm_dp_get_adjust_request_voltage(ls, 2), DP_TRAIN_VOLTAGE_SWING_LEVEL_2);
	CHECK_EQ(drm_dp_get_adjust_request_pre_emphasis(ls, 2), DP_TRAIN_PRE_EMPH_LEVEL_1);
	CHECK_EQ(drm_dp_get_adjust_request_voltage(ls, 3), DP_TRAIN_VOLTAGE_SWING_LEVEL_0);
	CHECK_EQ(drm_dp_get_adjust_request_pre_emphasis(ls, 3), DP_TRAIN_PRE_EMPH_LEVEL_3);
	/* 128b/132b: TX FFE presets share the adjust bytes, 4 bits a lane. */
	ls[4] = 0x5a;
	ls[5] = 0xf3;
	CHECK_EQ(drm_dp_get_adjust_tx_ffe_preset(ls, 0), 0xa);
	CHECK_EQ(drm_dp_get_adjust_tx_ffe_preset(ls, 1), 0x5);
	CHECK_EQ(drm_dp_get_adjust_tx_ffe_preset(ls, 2), 0x3);
	CHECK_EQ(drm_dp_get_adjust_tx_ffe_preset(ls, 3), 0xf);

	memset(ls, 0, sizeof(ls));
	ls[0] = (DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED) * 0x11;
	ls[2] = DP_INTERLANE_ALIGN_DONE | DP_128B132B_DPRX_EQ_INTERLANE_ALIGN_DONE;
	CHECK(drm_dp_128b132b_lane_channel_eq_done(ls, 2), "128b132b EQ done");
	CHECK(!drm_dp_128b132b_lane_channel_eq_done(ls, 4), "128b132b EQ lanes 2-3");
	CHECK(drm_dp_128b132b_lane_symbol_locked(ls, 2), "128b132b locked");
	CHECK(drm_dp_128b132b_eq_interlane_align_done(ls), "128b132b EQ align");
	CHECK(!drm_dp_128b132b_cds_interlane_align_done(ls), "128b132b CDS align");
	CHECK(!drm_dp_128b132b_link_training_failed(ls), "128b132b LT ok");
	ls[2] |= DP_128B132B_LT_FAILED;
	CHECK(drm_dp_128b132b_link_training_failed(ls), "128b132b LT failed");
}

static void test_bw_codes(void)
{
	static const struct { int rate; u8 code; } v[] = {
		{ 162000, DP_LINK_BW_1_62 }, { 270000, DP_LINK_BW_2_7 },
		{ 540000, DP_LINK_BW_5_4 }, { 810000, DP_LINK_BW_8_1 },
		{ 1000000, DP_LINK_BW_10 }, { 1350000, DP_LINK_BW_13_5 },
		{ 2000000, DP_LINK_BW_20 }, { 324000, 0x0c },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(v); i++) {
		CHECK_EQ(drm_dp_link_rate_to_bw_code(v[i].rate), v[i].code);
		CHECK_EQ(drm_dp_bw_code_to_link_rate(v[i].code), v[i].rate);
	}
	CHECK(drm_dp_is_uhbr_rate(1000000) && !drm_dp_is_uhbr_rate(810000), "uhbr");
	CHECK(!strcmp(drm_dp_phy_name(DP_PHY_DPRX), "DPRX"), "phy name");
	CHECK(!strcmp(drm_dp_phy_name(DP_PHY_LTTPR8), "LTTPR 8"), "phy name 8");
	CHECK(!strcmp(drm_dp_phy_name((enum drm_dp_phy)42), "<INVALID DP PHY>"), "phy name bad");
}

static void test_dpcd_access(void)
{
	u8 buf[32];
	int ret;

	aux_setup();
	sink_reset();
	for (int i = 0; i < 32; i++)
		dpcd[0x100 + i] = (u8)(0xa0 + i);

	/* A plain read: one throw-away probe of 0x102, then the read. */
	ret = (int)drm_dp_dpcd_read(&g_aux, 0x100, buf, 8);
	CHECK_EQ(ret, 8);
	CHECK(!memcmp(buf, dpcd + 0x100, 8), "read data");
	CHECK_EQ(nxlog, 2);
	CHECK_EQ(xlog[0].address, DP_TRAINING_PATTERN_SET);
	CHECK_EQ(xlog[0].size, 1);
	CHECK(lock_was_held_in_transfer, "transfers run under hw_mutex");
	CHECK(!g_aux.hw_mutex.writer, "hw_mutex released");

	/* Probing off: no throw-away read. */
	drm_dp_dpcd_set_probe(&g_aux, false);
	nxlog = 0;
	ret = (int)drm_dp_dpcd_read(&g_aux, 0x100, buf, 4);
	CHECK_EQ(ret, 4);
	CHECK_EQ(nxlog, 1);

	/* More than 16 bytes: split into AUX-sized transactions. */
	nxlog = 0;
	ret = drm_dp_dpcd_read_data(&g_aux, 0x100, buf, 32);
	CHECK_EQ(ret, 0);
	CHECK(!memcmp(buf, dpcd + 0x100, 32), "32-byte read");
	CHECK_EQ(nxlog, 2);

	/* DEFERs are retried (with the 0.5 ms pause), then the read works. */
	sink_reset();
	script_add(F_NATIVE_DEFER, 5, 0);
	ret = drm_dp_dpcd_read_byte(&g_aux, 0x0, buf);
	CHECK_EQ(ret, 0);
	CHECK_EQ(nxlog, 6);
	CHECK_EQ(busy_calls, 5);
	CHECK(busy_us >= 5 * AUX_RETRY_INTERVAL, "defer pause %llu", (unsigned long long)busy_us);

	/* 32 DEFERs: give up with -EIO after exactly 32 attempts. */
	sink_reset();
	script_add(F_NATIVE_DEFER, 40, 0);
	ret = (int)drm_dp_dpcd_readb(&g_aux, 0x0, buf);
	CHECK_EQ(ret, -EIO);
	CHECK_EQ(nxlog, 32);

	/* The first error is the one reported. */
	sink_reset();
	script_add(F_SHORT, 1, 0);
	script_add(F_NATIVE_NACK, 40, 0);
	ret = (int)drm_dp_dpcd_read(&g_aux, 0x0, buf, 2);
	CHECK_EQ(ret, -EPROTO);

	/* Timeouts are retried without the pause. */
	sink_reset();
	script_add(F_TIMEOUT, 3, 0);
	ret = drm_dp_dpcd_writeb(&g_aux, 0x600, 1);
	CHECK_EQ(ret, 1);
	CHECK_EQ(busy_calls, 0);
	CHECK_EQ(dpcd[0x600], 1);

	/* -EBUSY is retried with the pause. */
	sink_reset();
	script_add(F_BUSY, 2, 0);
	ret = drm_dp_dpcd_writeb(&g_aux, 0x600, 2);
	CHECK_EQ(ret, 1);
	CHECK_EQ(busy_calls, 2);

	/* A short write is -EPROTO for the _data form. */
	sink_reset();
	script_add(F_SHORT, 32, 1);
	buf[0] = buf[1] = 7;
	ret = drm_dp_dpcd_write_data(&g_aux, 0x100, buf, 2);
	CHECK_EQ(ret, -EPROTO);

	/* Powered down: fails at once, nothing on the wire. */
	sink_reset();
	drm_dp_dpcd_set_powered(&g_aux, false);
	ret = (int)drm_dp_dpcd_read(&g_aux, 0x0, buf, 1);
	CHECK_EQ(ret, -EBUSY);
	CHECK_EQ(nxlog, 0);
	drm_dp_dpcd_set_powered(&g_aux, true);

	/* An adapter that fails block reads: read_data falls back to bytes. */
	sink_reset();
	fail_block_reads = 1;
	for (int i = 0; i < 6; i++)
		dpcd[0x400 + i] = (u8)(0x10 + i);
	ret = drm_dp_dpcd_read_data(&g_aux, 0x400, buf, 6);
	CHECK_EQ(ret, 0);
	CHECK(!memcmp(buf, dpcd + 0x400, 6), "byte-wise fallback data");

	/* Uninitialised channel: refused, not crashed. */
	{
		struct drm_dp_aux raw;
		int k = kprintf_lines;

		memset(&raw, 0, sizeof(raw));
		raw.transfer = fake_transfer;
		raw.dpcd_probe_disabled = true;
		ret = (int)drm_dp_dpcd_read(&raw, 0, buf, 1);
		CHECK_EQ(ret, -ENODEV);
		CHECK(kprintf_lines > k, "warned");
	}
}

static void test_dpcd_caps(void)
{
	u8 caps[DP_RECEIVER_CAP_SIZE];
	int ret;

	aux_setup();
	drm_dp_dpcd_set_probe(&g_aux, false);

	/* Base rev 1.2 announcing the extended copy, which says 1.4. */
	sink_reset();
	dpcd[DP_DPCD_REV] = 0x12;
	dpcd[DP_MAX_LINK_RATE] = DP_LINK_BW_5_4;
	dpcd[DP_MAX_LANE_COUNT] = 4 | DP_ENHANCED_FRAME_CAP | DP_TPS3_SUPPORTED;
	dpcd[DP_TRAINING_AUX_RD_INTERVAL] = DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT;
	memcpy(dpcd + DP_DP13_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE);
	dpcd[DP_DP13_DPCD_REV + DP_DPCD_REV] = 0x14;
	dpcd[DP_DP13_DPCD_REV + DP_MAX_LINK_RATE] = DP_LINK_BW_8_1;
	dpcd[DP_DP13_DPCD_REV + DP_MAX_DOWNSPREAD] = DP_TPS4_SUPPORTED;
	ret = drm_dp_read_dpcd_caps(&g_aux, caps);
	CHECK_EQ(ret, 0);
	CHECK_EQ(caps[DP_DPCD_REV], 0x14);
	CHECK_EQ(drm_dp_max_link_rate(caps), 810000);
	CHECK_EQ(drm_dp_max_lane_count(caps), 4);
	CHECK(drm_dp_enhanced_frame_cap(caps), "efm");
	CHECK(drm_dp_tps3_supported(caps), "tps3");
	CHECK(drm_dp_tps4_supported(caps), "tps4");
	CHECK_EQ(drm_dp_training_pattern_mask(caps), DP_TRAINING_PATTERN_MASK_1_4);

	/* An extended copy with a lower revision is ignored. */
	dpcd[DP_DP13_DPCD_REV + DP_DPCD_REV] = 0x11;
	ret = drm_dp_read_dpcd_caps(&g_aux, caps);
	CHECK_EQ(ret, 0);
	CHECK_EQ(caps[DP_DPCD_REV], 0x12);
	CHECK_EQ(drm_dp_max_link_rate(caps), 540000);

	/* No sink: revision 0 is -EIO. */
	sink_reset();
	CHECK_EQ(drm_dp_read_dpcd_caps(&g_aux, caps), -EIO);

	/* Link status of the DPRX and of an LTTPR (no SINK_STATUS byte). */
	sink_reset();
	for (int i = 0; i < 6; i++)
		dpcd[DP_LANE0_1_STATUS + i] = (u8)(1 + i);
	for (int i = 0; i < 5; i++)
		dpcd[DP_LANE0_1_STATUS_PHY_REPEATER(DP_PHY_LTTPR2) + i] = (u8)(0x11 + i);
	{
		u8 ls[DP_LINK_STATUS_SIZE];

		CHECK_EQ(drm_dp_dpcd_read_link_status(&g_aux, ls), 0);
		CHECK(ls[0] == 1 && ls[5] == 6, "DPRX link status");
		CHECK_EQ(drm_dp_dpcd_read_phy_link_status(&g_aux, DP_PHY_LTTPR2, ls), 0);
		CHECK(ls[0] == 0x11 && ls[1] == 0x12 && ls[2] == 0x13 && ls[3] == 0 &&
		      ls[4] == 0x14 && ls[5] == 0x15,
		      "LTTPR link status %02x %02x %02x %02x %02x %02x",
		      ls[0], ls[1], ls[2], ls[3], ls[4], ls[5]);
	}

	/* Power states (DPCD 1.1+ only). */
	sink_reset();
	dpcd[DP_SET_POWER] = 0xf0 | DP_SET_POWER_D3;
	CHECK_EQ(drm_dp_link_power_up(&g_aux, 0x12), 0);
	CHECK_EQ(dpcd[DP_SET_POWER], 0xf0 | DP_SET_POWER_D0);
	CHECK(busy_us >= 1000, "1 ms wake-up wait");
	CHECK_EQ(drm_dp_link_power_down(&g_aux, 0x12), 0);
	CHECK_EQ(dpcd[DP_SET_POWER], 0xf0 | DP_SET_POWER_D3);
	nxlog = 0;
	CHECK_EQ(drm_dp_link_power_up(&g_aux, 0x10), 0);
	CHECK_EQ(nxlog, 0);
}

static void test_delays(void)
{
	u8 caps[DP_RECEIVER_CAP_SIZE] = { 0 };

	aux_setup();
	drm_dp_dpcd_set_probe(&g_aux, false);
	sink_reset();

	caps[DP_DPCD_REV] = 0x12;
	caps[DP_TRAINING_AUX_RD_INTERVAL] = 0;
	CHECK_EQ(drm_dp_read_clock_recovery_delay(&g_aux, caps, DP_PHY_DPRX, false), 100);
	CHECK_EQ(drm_dp_read_channel_eq_delay(&g_aux, caps, DP_PHY_DPRX, false), 400);
	caps[DP_TRAINING_AUX_RD_INTERVAL] = 2 | DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT;
	CHECK_EQ(drm_dp_read_clock_recovery_delay(&g_aux, caps, DP_PHY_DPRX, false), 8000);
	CHECK_EQ(drm_dp_read_channel_eq_delay(&g_aux, caps, DP_PHY_DPRX, false), 8000);
	caps[DP_DPCD_REV] = 0x14;
	CHECK_EQ(drm_dp_read_clock_recovery_delay(&g_aux, caps, DP_PHY_DPRX, false), 100);
	CHECK_EQ(drm_dp_read_channel_eq_delay(&g_aux, caps, DP_PHY_DPRX, false), 8000);
	/* UHBR: the 128b/132b interval register, read over AUX. */
	dpcd[DP_128B132B_TRAINING_AUX_RD_INTERVAL] = DP_128B132B_TRAINING_AUX_RD_INTERVAL_16_MS;
	CHECK_EQ(drm_dp_read_clock_recovery_delay(&g_aux, caps, DP_PHY_DPRX, true), 100);
	CHECK_EQ(drm_dp_read_channel_eq_delay(&g_aux, caps, DP_PHY_DPRX, true), 16000);
	/* LTTPR: CR always 100 us, EQ from its own interval register. */
	dpcd[DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER(DP_PHY_LTTPR1)] = 1;
	CHECK_EQ(drm_dp_read_clock_recovery_delay(&g_aux, caps, DP_PHY_LTTPR1, false), 100);
	CHECK_EQ(drm_dp_read_channel_eq_delay(&g_aux, caps, DP_PHY_LTTPR1, false), 4000);
	/* Unreadable interval: the 400 us default. */
	script_add(F_NATIVE_NACK, 64, 0);
	CHECK_EQ(drm_dp_read_channel_eq_delay(&g_aux, caps, DP_PHY_LTTPR1, false), 400);
	sink_reset();
	dpcd[DP_128B132B_TRAINING_AUX_RD_INTERVAL] = 0x3 | DP_128B132B_TRAINING_AUX_RD_INTERVAL_1MS_UNIT;
	CHECK_EQ(drm_dp_128b132b_read_aux_rd_interval(&g_aux), 4000);
	dpcd[DP_128B132B_TRAINING_AUX_RD_INTERVAL] = 0x3;
	CHECK_EQ(drm_dp_128b132b_read_aux_rd_interval(&g_aux), 8000);

	/* The waits: busy below 1 ms or without a comparator, asleep else. */
	caps[DP_DPCD_REV] = 0x12;
	caps[DP_TRAINING_AUX_RD_INTERVAL] = 1;
	sink_reset();
	drm_dp_link_train_clock_recovery_delay(&g_aux, caps);
	CHECK_EQ(busy_us, 4000);
	CHECK_EQ(sleep_calls, 0);
	highres = 1;
	sink_reset();
	drm_dp_link_train_channel_eq_delay(&g_aux, caps);
	CHECK_EQ(sleep_calls, DRM_DP_SLEEPING_DELAYS ? 1 : 0);
	CHECK_EQ(busy_us + slept_us, 4000);
	sink_reset();
	drm_dp_lttpr_link_train_clock_recovery_delay();
	CHECK_EQ(sleep_calls, 0);
	CHECK_EQ(busy_us, 100);
	/* The boot and idle threads never sleep. */
	task_hidden = 1;
	sink_reset();
	drm_dp_link_train_channel_eq_delay(&g_aux, caps);
	CHECK_EQ(sleep_calls, 0);
	CHECK_EQ(busy_us, 4000);
	task_hidden = 0;
	highres = 0;
}

/* ---- I2C over AUX ---------------------------------------------------------- */

static int edid_read(u8 *out, unsigned off, unsigned len)
{
	u8 o = (u8)off;
	struct i2c_msg m[2] = {
		{ .addr = 0x50, .flags = 0, .len = 1, .buf = &o },
		{ .addr = 0x50, .flags = I2C_M_RD, .len = (uint16_t)len, .buf = out },
	};

	return i2c_transfer(&g_aux.ddc, m, 2);
}

static void test_i2c(void)
{
	u8 buf[256];
	int ret, i;

	aux_setup();
	sink_reset();
	for (i = 0; i < 256; i++)
		edid_rom[i] = (u8)(i * 7 + 3);
	CHECK(!strcmp(g_aux.ddc.name, "AUX TEST"), "adapter name '%s'", g_aux.ddc.name);

	/* A 128-byte EDID block: bare address write (MOT), the offset byte,
	 * bare address read (MOT), eight 16-byte reads (MOT), and the bare
	 * address read without MOT that ends the transaction. */
	ret = edid_read(buf, 0, 128);
	CHECK_EQ(ret, 2);
	CHECK(!memcmp(buf, edid_rom, 128), "EDID block 0");
	CHECK_EQ(nxlog, 12);
	CHECK(xlog[0].request == (DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT) && xlog[0].size == 0,
	      "bare write start %x/%zu", xlog[0].request, xlog[0].size);
	CHECK(xlog[1].request == (DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT) && xlog[1].size == 1,
	      "offset write");
	CHECK(xlog[2].request == (DP_AUX_I2C_READ | DP_AUX_I2C_MOT) && xlog[2].size == 0,
	      "bare read start");
	for (i = 3; i < 11; i++)
		CHECK(xlog[i].request == (DP_AUX_I2C_READ | DP_AUX_I2C_MOT) && xlog[i].size == 16,
		      "read chunk %d: %x/%zu", i, xlog[i].request, xlog[i].size);
	CHECK(xlog[11].request == DP_AUX_I2C_READ && xlog[11].size == 0,
	      "closing bare read %x/%zu", xlog[11].request, xlog[11].size);
	CHECK(lock_was_held_in_transfer, "I2C under hw_mutex");
	CHECK(!g_aux.hw_mutex.writer, "hw_mutex released after I2C");

	/* The second block, through a sink that answers 8 bytes at a time:
	 * the partial reply shrinks the transfers for the rest of it. */
	sink_reset();
	i2c_chunk_limit = 8;
	ret = edid_read(buf, 128, 128);
	CHECK_EQ(ret, 2);
	CHECK(!memcmp(buf, edid_rom + 128, 128), "EDID block 1 in 8-byte pieces");
	{
		int sixteen = 0, eight = 0;

		for (i = 0; i < nxlog; i++) {
			if ((xlog[i].request & ~DP_AUX_I2C_MOT) != DP_AUX_I2C_READ)
				continue;
			if (xlog[i].size == 16)
				sixteen++;
			if (xlog[i].size == 8)
				eight++;
		}
		/* 16 asked, 8 answered, the other 8 asked again; then
		 * 14 more of 8. */
		CHECK_EQ(sixteen, 1);
		CHECK_EQ(eight, 15);
	}

	/* I2C DEFERs are retried and counted. */
	sink_reset();
	g_aux.i2c_defer_count = 0;
	script_add(F_NONE, 3, 0); /* start, offset, bare read */
	script_add(F_I2C_DEFER, 3, 0);
	ret = edid_read(buf, 0, 16);
	CHECK_EQ(ret, 2);
	CHECK_EQ(g_aux.i2c_defer_count, 3);
	CHECK(!memcmp(buf, edid_rom, 16), "data after I2C defers");

	/* An I2C NACK (nothing at the address) is -EREMOTEIO, counted, and
	 * the transaction is still closed. */
	sink_reset();
	g_aux.i2c_nack_count = 0;
	{
		u8 b = 0;
		struct i2c_msg m = { .addr = 0x37, .flags = I2C_M_RD, .len = 1, .buf = &b };

		ret = i2c_transfer(&g_aux.ddc, &m, 1);
		CHECK_EQ(ret, -EREMOTEIO);
		CHECK_EQ(g_aux.i2c_nack_count, 2);
		CHECK(xlog[nxlog - 1].size == 0 && !(xlog[nxlog - 1].request & DP_AUX_I2C_MOT),
		      "closed after NACK");
	}

	/* Native DEFERs: retried for as long as the I2C bus may need. */
	{
		struct drm_dp_aux_msg msg = { .request = DP_AUX_I2C_READ, .size = 16 };
		int want = DIV_ROUND_UP(155 * 1000 / 10, 66 + 170 + AUX_RETRY_INTERVAL);

		CHECK_EQ(drm_dp_i2c_retry_count(&msg, 10), want);
		CHECK_EQ(want, 22);
		msg.size = 1;
		CHECK_EQ(max(7, drm_dp_i2c_retry_count(&msg, 10)), 7);
	}
	sink_reset();
	script_add(F_NONE, 3, 0);
	script_add(F_NATIVE_DEFER, 21, 0);
	ret = edid_read(buf, 0, 16);
	CHECK_EQ(ret, 2);
	sink_reset();
	script_add(F_NONE, 3, 0);
	script_add(F_NATIVE_DEFER, 22, 0);
	ret = edid_read(buf, 0, 16);
	CHECK_EQ(ret, -EREMOTEIO);

	/* -EBUSY is retried without counting against the limit's pause. */
	sink_reset();
	script_add(F_BUSY, 2, 0);
	ret = edid_read(buf, 0, 4);
	CHECK_EQ(ret, 2);

	/* A timeout is passed up. */
	sink_reset();
	script_add(F_TIMEOUT, 1, 0);
	ret = edid_read(buf, 0, 4);
	CHECK_EQ(ret, -ETIMEDOUT);

	/* An invalid reply code: -EREMOTEIO, and the report is rate-limited. */
	{
		int k = kprintf_lines;

		for (i = 0; i < 15; i++) {
			sink_reset();
			script_add(F_BAD_REPLY, 1, 0);
			ret = edid_read(buf, 0, 4);
			CHECK_EQ(ret, -EREMOTEIO);
		}
		CHECK(kprintf_lines - k <= 11, "rate limit: %d lines", kprintf_lines - k);
	}

	/* A short ACK to a write: the rest goes as WRITE_STATUS_UPDATE. */
	sink_reset();
	{
		u8 w[4] = { 0x20, 1, 2, 3 };
		struct i2c_msg m = { .addr = 0x50, .flags = 0, .len = 4, .buf = w };
		int wsu = 0;

		script_add(F_NONE, 1, 0);
		script_add(F_SHORT, 1, 2);
		ret = i2c_transfer(&g_aux.ddc, &m, 1);
		CHECK_EQ(ret, 1);
		for (i = 0; i < nxlog; i++)
			if ((xlog[i].request & ~DP_AUX_I2C_MOT) == DP_AUX_I2C_WRITE_STATUS_UPDATE)
				wsu++;
		CHECK_EQ(wsu, 1);
		CHECK_EQ(xlog[2].size, 2);
	}

	/* Hardware without zero-sized transfers: no bare address packets,
	 * MOT dropped on the last chunk of each message instead. */
	sink_reset();
	g_aux.no_zero_sized = true;
	ret = edid_read(buf, 0, 32);
	CHECK_EQ(ret, 2);
	CHECK_EQ(nxlog, 3);
	CHECK(xlog[0].request == DP_AUX_I2C_WRITE, "offset without MOT");
	CHECK(xlog[1].request == (DP_AUX_I2C_READ | DP_AUX_I2C_MOT), "first chunk with MOT");
	CHECK(xlog[2].request == DP_AUX_I2C_READ, "last chunk without MOT");
	CHECK(!memcmp(buf, edid_rom, 32), "no_zero_sized data");
	g_aux.no_zero_sized = false;

	/* Powered down: refused at once. */
	sink_reset();
	drm_dp_dpcd_set_powered(&g_aux, false);
	CHECK_EQ(edid_read(buf, 0, 4), -EBUSY);
	CHECK_EQ(nxlog, 0);
	drm_dp_dpcd_set_powered(&g_aux, true);

	/* Unregistered: the adapter is dead. */
	drm_dp_aux_unregister(&g_aux);
	CHECK(edid_read(buf, 0, 4) < 0, "unregistered adapter fails");
}

/* ---- identification, branch devices, LTTPRs --------------------------------- */

static void set_ident(unsigned base, u8 o0, u8 o1, u8 o2, const char *id)
{
	dpcd[base] = o0;
	dpcd[base + 1] = o1;
	dpcd[base + 2] = o2;
	for (int i = 0; i < 6; i++)
		dpcd[base + 3 + i] = (u8)id[i];
}

static void test_desc(void)
{
	struct drm_dp_desc d;

	aux_setup();
	drm_dp_dpcd_set_probe(&g_aux, false);

	sink_reset();
	set_ident(DP_BRANCH_OUI, 0x00, 0x22, 0xb9, "\0\0\0\0\0\0");
	CHECK_EQ(drm_dp_read_desc(&g_aux, &d, true), 0);
	CHECK(drm_dp_has_quirk(&d, DP_DPCD_QUIRK_CONSTANT_N), "Analogix branch");
	CHECK_EQ(drm_dp_read_desc(&g_aux, &d, false), 0);
	CHECK_EQ(d.quirks, 0);

	set_ident(DP_SINK_OUI, 0x00, 0x22, 0xb9, "sivarT");
	CHECK_EQ(drm_dp_read_desc(&g_aux, &d, false), 0);
	CHECK_EQ(d.quirks, BIT(DP_DPCD_QUIRK_CONSTANT_N));

	set_ident(DP_SINK_OUI, 0x00, 0x10, 0xfa, "eD!eba");
	dpcd[DP_SINK_OUI + 3] = 101;
	dpcd[DP_SINK_OUI + 4] = 68;
	dpcd[DP_SINK_OUI + 5] = 21;
	dpcd[DP_SINK_OUI + 6] = 101;
	dpcd[DP_SINK_OUI + 7] = 98;
	dpcd[DP_SINK_OUI + 8] = 97;
	CHECK_EQ(drm_dp_read_desc(&g_aux, &d, false), 0);
	CHECK_EQ(d.quirks, BIT(DP_DPCD_QUIRK_NO_PSR) |
			   BIT(DP_DPCD_QUIRK_CAN_DO_MAX_LINK_RATE_3_24_GBPS));

	set_ident(DP_BRANCH_OUI, 0x90, 0xcc, 0x24, "SYNA\x53\x22");
	CHECK_EQ(drm_dp_read_desc(&g_aux, &d, true), 0);
	CHECK_EQ(d.quirks, BIT(DP_DPCD_QUIRK_DSC_WITHOUT_VIRTUAL_DPCD) |
			   BIT(DP_DPCD_QUIRK_HBLANK_EXPANSION_REQUIRES_DSC) |
			   BIT(DP_DPCD_QUIRK_DSC_THROUGHPUT_BPP_LIMIT));

	/* Sink count, with the CH7511 quirk. */
	{
		u8 caps[DP_RECEIVER_CAP_SIZE] = { 0 };
		struct drm_connector conn;

		memset(&conn, 0, sizeof(conn));
		conn.type = DRM_MODE_CONNECTOR_DisplayPort;
		caps[DP_DPCD_REV] = 0x12;
		caps[DP_DOWNSTREAMPORT_PRESENT] = DP_DWN_STRM_PORT_PRESENT;
		set_ident(DP_SINK_OUI, 0, 0, 0, "CH7511");
		CHECK_EQ(drm_dp_read_desc(&g_aux, &d, false), 0);
		CHECK(!drm_dp_read_sink_count_cap(&conn, caps, &d), "CH7511 sink count ignored");
		d.quirks = 0;
		CHECK(drm_dp_read_sink_count_cap(&conn, caps, &d), "sink count read");
		conn.type = DRM_MODE_CONNECTOR_eDP;
		CHECK(!drm_dp_read_sink_count_cap(&conn, caps, &d), "eDP sink count ignored");
		/* Bit 7 is the count's bit 6; bit 6 is CP_READY. */
		dpcd[DP_SINK_COUNT] = 0xc5;
		CHECK_EQ(drm_dp_read_sink_count(&g_aux), 0x45);
	}
}

static void test_downstream(void)
{
	u8 caps[DP_RECEIVER_CAP_SIZE] = { 0 };
	u8 ports[DP_MAX_DOWNSTREAM_PORTS];
	u8 hdmi[4] = { DP_DS_PORT_TYPE_HDMI, 0x78, DP_DS_12BPC,
		       DP_DS_HDMI_YCBCR420_PASS_THROUGH | DP_DS_HDMI_YCBCR444_TO_420_CONV };
	u8 dvi[4] = { DP_DS_PORT_TYPE_DVI, 0x42, DP_DS_10BPC, 0 };
	u8 vga[4] = { DP_DS_PORT_TYPE_VGA, 30, DP_DS_8BPC, 0 };
	u8 dpp[4] = { DP_DS_PORT_TYPE_DP_DUALMODE, 0x78, DP_DS_16BPC, 0 };
	u8 dp[4] = { DP_DS_PORT_TYPE_DP, 0, 0, 0 };
	u8 tv[4] = { DP_DS_PORT_TYPE_NON_EDID | DP_DS_NON_EDID_1280x720_60, 0, 0, 0 };
	struct edid e;
	struct drm_edid de = { .size = sizeof(e), .edid = &e };
	struct drm_display_mode mode;

	memset(&e, 0, sizeof(e));
	e.revision = 4;
	e.input = DRM_EDID_INPUT_DIGITAL | DRM_EDID_DIGITAL_TYPE_DP;

	/* Not a branch: nothing. */
	caps[DP_DPCD_REV] = 0x12;
	CHECK_EQ(drm_dp_downstream_max_tmds_clock(caps, hdmi, NULL), 0);
	CHECK_EQ(drm_dp_subconnector_type(caps, hdmi), DRM_MODE_SUBCONNECTOR_Native);

	caps[DP_DOWNSTREAMPORT_PRESENT] = DP_DWN_STRM_PORT_PRESENT;
	/* Without detailed caps: HDMI 300 MHz, DVI 165 MHz, 8 bpc. */
	CHECK_EQ(drm_dp_downstream_max_tmds_clock(caps, hdmi, NULL), 300000);
	CHECK_EQ(drm_dp_downstream_max_tmds_clock(caps, dvi, NULL), 165000);
	CHECK_EQ(drm_dp_downstream_max_bpc(caps, hdmi, NULL), 8);
	CHECK_EQ(drm_dp_downstream_max_dotclock(caps, vga), 0);
	CHECK(!drm_dp_downstream_420_passthrough(caps, hdmi), "420 needs detailed caps");

	caps[DP_DOWNSTREAMPORT_PRESENT] |= DP_DETAILED_CAP_INFO_AVAILABLE;
	CHECK(!drm_dp_downstream_420_passthrough(caps, hdmi), "420 needs DPCD 1.3");
	caps[DP_DPCD_REV] = 0x13;
	CHECK_EQ(drm_dp_downstream_max_tmds_clock(caps, hdmi, NULL), 300000);
	CHECK_EQ(drm_dp_downstream_max_tmds_clock(caps, dvi, NULL), 0x42 * 2500);
	CHECK_EQ(drm_dp_downstream_min_tmds_clock(caps, dvi, NULL), 25000);
	CHECK_EQ(drm_dp_downstream_max_bpc(caps, hdmi, NULL), 12);
	CHECK_EQ(drm_dp_downstream_max_bpc(caps, dvi, NULL), 10);
	CHECK_EQ(drm_dp_downstream_max_bpc(caps, dp, NULL), 0);
	CHECK_EQ(drm_dp_downstream_max_dotclock(caps, vga), 30 * 8000);
	CHECK(drm_dp_downstream_420_passthrough(caps, hdmi), "420 passthrough");
	CHECK(drm_dp_downstream_444_to_420_conversion(caps, hdmi), "444->420");
	CHECK(drm_dp_downstream_420_passthrough(caps, dp), "DP passes 420");
	CHECK(drm_dp_downstream_is_tmds(caps, hdmi, NULL), "HDMI is TMDS");
	CHECK(!drm_dp_downstream_is_tmds(caps, vga, NULL), "VGA is not TMDS");
	CHECK(drm_dp_downstream_is_type(caps, vga, DP_DS_PORT_TYPE_VGA), "is VGA");
	/* DP++ with a DisplayPort sink behind it is not TMDS. */
	CHECK(drm_dp_downstream_is_tmds(caps, dpp, NULL), "DP++ without EDID");
	CHECK(!drm_dp_downstream_is_tmds(caps, dpp, &de), "DP++ to a DP sink");
	CHECK_EQ(drm_dp_downstream_max_tmds_clock(caps, dpp, &de), 0);
	CHECK_EQ(drm_dp_downstream_max_bpc(caps, dpp, NULL), 16);
	CHECK_EQ(drm_dp_subconnector_type(caps, hdmi), DRM_MODE_SUBCONNECTOR_HDMIA);
	CHECK_EQ(drm_dp_subconnector_type(caps, dpp), DRM_MODE_SUBCONNECTOR_DisplayPort);
	CHECK_EQ(drm_dp_subconnector_type(caps, dvi), DRM_MODE_SUBCONNECTOR_DVID);
	CHECK_EQ(drm_dp_subconnector_type(caps, tv), DRM_MODE_SUBCONNECTOR_Unknown);
	/* PCON FRL: port_cap[2] carries the FRL rate. */
	{
		u8 frl[4] = { DP_DS_PORT_TYPE_HDMI, 0x78, DP_PCON_MAX_40GBPS, 0 };

		CHECK_EQ(drm_dp_get_pcon_max_frl_bw(caps, frl), 40);
	}

	/* A port without EDID: its fixed CEA mode. */
	CHECK_EQ(drm_dp_downstream_mode(caps, tv, &mode), 0);
	CHECK(mode.hdisplay == 1280 && mode.vdisplay == 720 && mode.clock == 74250,
	      "720p60 %dx%d@%d", mode.hdisplay, mode.vdisplay, mode.clock);
	CHECK_EQ(drm_dp_downstream_mode(caps, hdmi, &mode), -ENOENT);

	/* DPCD 1.0: the port type is in DOWNSTREAMPORT_PRESENT. */
	caps[DP_DPCD_REV] = 0x10;
	caps[DP_DOWNSTREAMPORT_PRESENT] = DP_DWN_STRM_PORT_PRESENT | DP_DWN_STRM_PORT_TYPE_TMDS;
	CHECK_EQ(drm_dp_downstream_max_tmds_clock(caps, dp, NULL), 165000);
	CHECK_EQ(drm_dp_subconnector_type(caps, dp), DRM_MODE_SUBCONNECTOR_DVID);

	/* Reading the DFP caps: four bytes a port when detailed. */
	aux_setup();
	drm_dp_dpcd_set_probe(&g_aux, false);
	sink_reset();
	caps[DP_DPCD_REV] = 0x12;
	caps[DP_DOWNSTREAMPORT_PRESENT] = DP_DWN_STRM_PORT_PRESENT | DP_DETAILED_CAP_INFO_AVAILABLE;
	caps[DP_DOWN_STREAM_PORT_COUNT] = 2;
	memcpy(dpcd + DP_DOWNSTREAM_PORT_0, hdmi, 4);
	memcpy(dpcd + DP_DOWNSTREAM_PORT_0 + 4, dvi, 4);
	CHECK_EQ(drm_dp_read_downstream_info(&g_aux, caps, ports), 0);
	CHECK(!memcmp(ports, hdmi, 4) && !memcmp(ports + 4, dvi, 4) && ports[8] == 0,
	      "DFP caps");
	caps[DP_DOWN_STREAM_PORT_COUNT] = 0;
	CHECK_EQ(drm_dp_read_downstream_info(&g_aux, caps, ports), 0);
	CHECK_EQ(ports[0], 0);
	caps[DP_DOWNSTREAMPORT_PRESENT] = 0;
	nxlog = 0;
	CHECK_EQ(drm_dp_read_downstream_info(&g_aux, caps, ports), 0);
	CHECK_EQ(nxlog, 0);
}

static void test_lttpr(void)
{
	u8 caps[DP_RECEIVER_CAP_SIZE] = { 0 };
	u8 common[DP_LTTPR_COMMON_CAP_SIZE];
	u8 phy[DP_LTTPR_PHY_CAP_SIZE];
	static const struct { u8 cnt; int want; } counts[] = {
		{ 0x00, 0 }, { 0x80, 1 }, { 0x40, 2 }, { 0x20, 3 }, { 0x10, 4 },
		{ 0x08, 5 }, { 0x04, 6 }, { 0x02, 7 }, { 0x01, 8 },
		{ 0xff, -ERANGE }, { 0x03, -EINVAL },
	};
	size_t i;

	aux_setup();
	drm_dp_dpcd_set_probe(&g_aux, false);
	sink_reset();
	dpcd[DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV] = 0x14;
	dpcd[DP_MAX_LINK_RATE_PHY_REPEATER] = DP_LINK_BW_5_4;
	dpcd[DP_PHY_REPEATER_CNT] = 0x40;
	dpcd[DP_MAX_LANE_COUNT_PHY_REPEATER] = 4;
	dpcd[DP_TRANSMITTER_CAPABILITY_PHY_REPEATER1] =
		DP_VOLTAGE_SWING_LEVEL_3_SUPPORTED;

	/* DPCD rev < 1.4: read one byte at a time (Dell P2715Q). */
	caps[DP_DPCD_REV] = 0x12;
	CHECK_EQ(drm_dp_read_lttpr_common_caps(&g_aux, caps, common), 0);
	CHECK_EQ(nxlog, DP_LTTPR_COMMON_CAP_SIZE);
	CHECK_EQ(drm_dp_lttpr_count(common), 2);
	CHECK_EQ(drm_dp_lttpr_max_link_rate(common), 540000);
	CHECK_EQ(drm_dp_lttpr_max_lane_count(common), 4);
	caps[DP_DPCD_REV] = 0x14;
	nxlog = 0;
	CHECK_EQ(drm_dp_read_lttpr_phy_caps(&g_aux, caps, DP_PHY_LTTPR1, phy), 0);
	CHECK_EQ(nxlog, 1);
	CHECK(drm_dp_lttpr_voltage_swing_level_3_supported(phy), "vswing 3");
	CHECK(!drm_dp_lttpr_pre_emphasis_level_3_supported(phy), "no preemph 3");

	for (i = 0; i < ARRAY_SIZE(counts); i++) {
		common[DP_PHY_REPEATER_CNT - DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV] =
			counts[i].cnt;
		CHECK_EQ(drm_dp_lttpr_count(common), counts[i].want);
	}

	/* Init: transparent, then non-transparent. */
	sink_reset();
	CHECK_EQ(drm_dp_lttpr_init(&g_aux, 2), 0);
	CHECK_EQ(dpcd[DP_PHY_REPEATER_MODE], DP_PHY_REPEATER_MODE_NON_TRANSPARENT);
	CHECK_EQ(count_requests(DP_AUX_NATIVE_WRITE, DP_PHY_REPEATER_MODE), 2);
	CHECK(xlog[0].address == DP_PHY_REPEATER_MODE, "transparent first");
	/* Invalid count: transparent only. */
	sink_reset();
	CHECK_EQ(drm_dp_lttpr_init(&g_aux, -EINVAL), -ENODEV);
	CHECK_EQ(dpcd[DP_PHY_REPEATER_MODE], DP_PHY_REPEATER_MODE_TRANSPARENT);
	CHECK_EQ(drm_dp_lttpr_init(&g_aux, 0), 0);
	/* Non-transparent refused: rolled back to transparent. */
	sink_reset();
	script_add(F_NONE, 1, 0);
	script_add(F_NATIVE_NACK, 32, 0);
	CHECK_EQ(drm_dp_lttpr_init(&g_aux, 1), -EINVAL);
	CHECK_EQ(dpcd[DP_PHY_REPEATER_MODE], DP_PHY_REPEATER_MODE_TRANSPARENT);

	/* Wake timeout grant (transparent: the DPRX's request). */
	sink_reset();
	dpcd[DP_EXTENDED_DPRX_SLEEP_WAKE_TIMEOUT_REQUEST] = DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_40_MS;
	drm_dp_lttpr_wake_timeout_setup(&g_aux, true);
	CHECK_EQ(dpcd[DP_EXTENDED_DPRX_SLEEP_WAKE_TIMEOUT_GRANT],
		 DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_GRANTED);
	sink_reset();
	dpcd[DP_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT] = 0;
	drm_dp_lttpr_wake_timeout_setup(&g_aux, false);
	CHECK_EQ(count_requests(DP_AUX_NATIVE_WRITE, DP_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT), 0);
	dpcd[DP_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT] = 3;
	drm_dp_lttpr_wake_timeout_setup(&g_aux, false);
	CHECK_EQ(dpcd[DP_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT], DP_EXTENDED_WAKE_TIMEOUT_GRANT);
}

/* ---- DSC, SDPs, PSR, bandwidth ------------------------------------------------- */

static void test_dsc(void)
{
	u8 d[DP_DSC_RECEIVER_CAP_SIZE] = { 0 };
	u8 br[DP_DSC_BRANCH_CAP_SIZE] = { 0 };
	u8 bpc[3];

#define DSC(reg) d[(reg) - DP_DSC_SUPPORT]
	DSC(DP_DSC_SUPPORT) = DP_DSC_DECOMPRESSION_IS_SUPPORTED;
	DSC(DP_DSC_SLICE_CAP_1) = DP_DSC_1_PER_DP_DSC_SINK | DP_DSC_2_PER_DP_DSC_SINK |
				  DP_DSC_4_PER_DP_DSC_SINK | DP_DSC_8_PER_DP_DSC_SINK;
	DSC(DP_DSC_SLICE_CAP_2) = DP_DSC_16_PER_DP_DSC_SINK;
	DSC(DP_DSC_LINE_BUF_BIT_DEPTH) = DP_DSC_LINE_BUF_BIT_DEPTH_9;
	DSC(DP_DSC_DEC_COLOR_DEPTH_CAP) = DP_DSC_10_BPC;
	DSC(DP_DSC_BITS_PER_PIXEL_INC) = DP_DSC_BITS_PER_PIXEL_1_16;
	DSC(DP_DSC_MAX_SLICE_WIDTH) = 8;
	DSC(DP_DSC_MAX_BITS_PER_PIXEL_LOW) = 0x40;
	DSC(DP_DSC_MAX_BITS_PER_PIXEL_HI) = 0x1;
	DSC(DP_DSC_PEAK_THROUGHPUT) = (4 << DP_DSC_THROUGHPUT_MODE_0_SHIFT) |
				      (15 << DP_DSC_THROUGHPUT_MODE_1_SHIFT);
	DSC(DP_DSC_DEC_COLOR_FORMAT_CAP) = DP_DSC_RGB;

	CHECK(drm_dp_sink_supports_dsc(d), "dsc");
	CHECK_EQ(drm_dp_dsc_sink_slice_count_mask(d, false),
		 BIT(0) | BIT(1) | BIT(3) | BIT(7) | BIT(15));
	CHECK_EQ(drm_dp_dsc_sink_max_slice_count(d, false), 16);
	CHECK_EQ(drm_dp_dsc_sink_max_slice_count(d, true), 8);
	CHECK_EQ(drm_dp_dsc_sink_line_buf_depth(d), 9);
	CHECK_EQ(drm_dp_dsc_sink_supported_input_bpcs(d, bpc), 2);
	CHECK(bpc[0] == 10 && bpc[1] == 8, "bpcs %d %d", bpc[0], bpc[1]);
	CHECK_EQ(drm_dp_dsc_sink_bpp_incr(d), 16);
	CHECK_EQ(drm_dp_dsc_sink_max_slice_width(d), 8 * 320);
	CHECK_EQ(drm_edp_dsc_sink_output_bpp(d), 0x140);
	CHECK(drm_dp_dsc_sink_supports_format(d, DP_DSC_RGB), "rgb");
	CHECK(!drm_dp_dsc_sink_supports_format(d, DP_DSC_YCbCr420_Native), "no 420");
	CHECK_EQ(drm_dp_dsc_sink_max_slice_throughput(d, 1000000, true), 400000 + 50000 * 2);
	CHECK_EQ(drm_dp_dsc_sink_max_slice_throughput(d, 1000000, false), 170000);
	DSC(DP_DSC_PEAK_THROUGHPUT) = 0;
	CHECK_EQ(drm_dp_dsc_sink_max_slice_throughput(d, 5000000, true), 600000);
	CHECK_EQ(drm_dp_dsc_sink_max_slice_throughput(d, 3000000, false), 400000);
	CHECK_EQ(drm_dp_dsc_sink_max_slice_throughput(d, 1000000, false), 340000);
	DSC(DP_DSC_SUPPORT) = 0;
	CHECK_EQ(drm_dp_dsc_sink_supported_input_bpcs(d, bpc), 0);
#undef DSC

	br[0] = 1;
	br[1] = 4;
	br[2] = 16;
	CHECK_EQ(drm_dp_dsc_branch_max_overall_throughput(br, true), 680000);
	CHECK_EQ(drm_dp_dsc_branch_max_overall_throughput(br, false), 800000);
	CHECK_EQ(drm_dp_dsc_branch_max_line_width(br), 5120);
	br[2] = 15;
	CHECK_EQ(drm_dp_dsc_branch_max_line_width(br), -EINVAL);
	CHECK(drm_dp_sink_supports_fec(DP_FEC_CAPABLE), "fec");
}

static void test_sdp_psr_bw(void)
{
	struct drm_dp_vsc_sdp vsc = {
		.sdp_type = DP_SDP_VSC, .revision = 0x5, .length = 0x13,
		.pixelformat = DP_PIXELFORMAT_YUV420,
		.colorimetry = DP_COLORIMETRY_BT2020_YCC, .bpc = 10,
		.dynamic_range = DP_DYNAMIC_RANGE_CTA,
		.content_type = DP_CONTENT_TYPE_GAME,
	};
	struct drm_dp_as_sdp as = {
		.sdp_type = DP_SDP_ADAPTIVE_SYNC, .revision = 2, .length = 9,
		.vtotal = 0x1234, .target_rr = 0x2f0, .target_rr_divider = true,
		.mode = DP_AS_SDP_FAVT_TRR_REACHED, .coasting_vtotal = 0xabc,
	};
	struct dp_sdp sdp;
	u8 psr[EDP_PSR_RECEIVER_CAP_SIZE] = { 0 };
	int k;

	CHECK_EQ(drm_dp_vsc_sdp_pack(&vsc, &sdp), sizeof(struct dp_sdp));
	CHECK(sdp.sdp_header.HB1 == DP_SDP_VSC && sdp.sdp_header.HB2 == 5 &&
	      sdp.sdp_header.HB3 == 0x13, "VSC header");
	CHECK_EQ(sdp.db[16], 0x37);
	CHECK_EQ(sdp.db[17], 0x82);
	CHECK_EQ(sdp.db[18], DP_CONTENT_TYPE_GAME);
	vsc.revision = 0x6;
	CHECK_EQ(drm_dp_vsc_sdp_pack(&vsc, &sdp), sizeof(struct dp_sdp));
	CHECK(sdp.db[0] == 1 && sdp.db[3] == 1 && sdp.db[16] == 0, "VSC rev 6");
	vsc.revision = 0x5;
	vsc.bpc = 7;
	k = kprintf_lines;
	CHECK_EQ(drm_dp_vsc_sdp_pack(&vsc, &sdp), -EINVAL);
	CHECK(kprintf_lines > k, "bad bpc warned");

	CHECK_EQ(drm_dp_as_sdp_pack(&as, &sdp, sizeof(sdp)), sizeof(struct dp_sdp));
	CHECK(sdp.db[0] == DP_AS_SDP_FAVT_TRR_REACHED && sdp.db[1] == 0x34 &&
	      sdp.db[2] == 0x12 && sdp.db[3] == 0xf0 && sdp.db[4] == (0x2 | 0x20) &&
	      sdp.db[7] == 0xbc && sdp.db[8] == 0x0a, "AS SDP payload");
	CHECK_EQ(drm_dp_as_sdp_pack(&as, &sdp, 8), -ENOSPC);
	k = kprintf_lines;
	drm_dp_vsc_sdp_log("[test]", &vsc);
	drm_dp_as_sdp_log("[test]", &as);
	CHECK_EQ(kprintf_lines - k, 14);

	psr[1] = DP_PSR_SETUP_TIME_330;
	CHECK_EQ(drm_dp_psr_setup_time(psr), 330);
	psr[1] = DP_PSR_SETUP_TIME_55;
	CHECK_EQ(drm_dp_psr_setup_time(psr), 55);
	psr[1] = DP_PSR_SETUP_TIME_0;
	CHECK_EQ(drm_dp_psr_setup_time(psr), 0);
	psr[1] = DP_PSR_SETUP_TIME_MASK;
	CHECK_EQ(drm_dp_psr_setup_time(psr), -EINVAL);

	/* 1920 wide 24 bpp on 4 lanes: 1440 symbol cycles exactly. */
	CHECK_EQ(drm_dp_link_symbol_cycles(4, 1920, 0, 24 * 16, 8, false), 1440);
	CHECK_EQ(drm_dp_bw_overhead(4, 1920, 0, 24 * 16, DRM_DP_BW_OVERHEAD_SSC_REF_CLK), 1006000);
	CHECK_EQ(drm_dp_bw_overhead(4, 1920, 0, 24 * 16,
				    DRM_DP_BW_OVERHEAD_SSC_REF_CLK | DRM_DP_BW_OVERHEAD_FEC),
		 1030016);
	CHECK_EQ(drm_dp_bw_overhead(0, 1920, 0, 24 * 16, 0), 0);
	/* MST on 1 lane aligns the cycles to 4; DSC adds an EOC per slice. */
	CHECK_EQ(drm_dp_link_symbol_cycles(1, 100, 0, 24 * 16, 8, true), 300);
	CHECK_EQ(drm_dp_link_symbol_cycles(1, 101, 0, 24 * 16, 8, true), 304);
	CHECK_EQ(drm_dp_link_symbol_cycles(4, 1920, 2, 8 * 16, 8, false), 2 * (240 + 1));
	CHECK_EQ(drm_dp_bw_channel_coding_efficiency(false), 800000);
	CHECK_EQ(drm_dp_bw_channel_coding_efficiency(true), 967100);
	CHECK_EQ(drm_dp_max_dprx_data_rate(810000, 4), 3240000);
	CHECK_EQ(drm_dp_max_dprx_data_rate(162000, 1), 162000);
	CHECK_EQ(drm_dp_max_dprx_data_rate(1000000, 4), 4835500);
}

/* ---- eDP backlight ------------------------------------------------------------ */

static void test_backlight(void)
{
	u8 edp[EDP_DISPLAY_CTL_CAP_SIZE] = { 0 };
	struct drm_edp_backlight_info bl;
	u32 level;
	u8 mode;

	aux_setup();
	drm_dp_dpcd_set_probe(&g_aux, false);

	/* 16-bit AUX level, AUX enable, the panel in DPCD mode at 0x1234. */
	sink_reset();
	edp[0] = DP_EDP_14;
	edp[1] = DP_EDP_TCON_BACKLIGHT_ADJUSTMENT_CAP | DP_EDP_BACKLIGHT_AUX_ENABLE_CAP;
	edp[2] = DP_EDP_BACKLIGHT_BRIGHTNESS_AUX_SET_CAP | DP_EDP_BACKLIGHT_BRIGHTNESS_BYTE_COUNT |
		 DP_EDP_BACKLIGHT_FREQ_AUX_SET_CAP;
	CHECK(drm_edp_backlight_supported(edp), "VESA backlight");
	dpcd[DP_EDP_PWMGEN_BIT_COUNT] = 16;
	dpcd[DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN] = 8;
	dpcd[DP_EDP_PWMGEN_BIT_COUNT_CAP_MAX] = 16;
	dpcd[DP_EDP_BACKLIGHT_MODE_SET_REGISTER] = DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD;
	dpcd[DP_EDP_BACKLIGHT_BRIGHTNESS_MSB] = 0x12;
	dpcd[DP_EDP_BACKLIGHT_BRIGHTNESS_LSB] = 0x34;
	memset(&bl, 0, sizeof(bl));
	CHECK_EQ(drm_edp_backlight_init(&g_aux, &bl, 0, 0, edp, &level, &mode, false), 0);
	CHECK(bl.aux_set && bl.aux_enable && bl.lsb_reg_used && !bl.luminance_set, "caps");
	CHECK_EQ(bl.max, 0xffff);
	CHECK_EQ(level, 0x1234);
	CHECK_EQ(mode, DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD);
	CHECK_EQ(drm_edp_backlight_set_level(&g_aux, &bl, 0x2001), 0);
	CHECK(dpcd[DP_EDP_BACKLIGHT_BRIGHTNESS_MSB] == 0x20 &&
	      dpcd[DP_EDP_BACKLIGHT_BRIGHTNESS_LSB] == 0x01, "16-bit level");
	CHECK_EQ(drm_edp_backlight_enable(&g_aux, &bl, 0x100), 0);
	CHECK_EQ(dpcd[DP_EDP_BACKLIGHT_MODE_SET_REGISTER], DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD);
	CHECK(dpcd[DP_EDP_DISPLAY_CONTROL_REGISTER] & DP_EDP_BACKLIGHT_ENABLE, "enabled");
	CHECK_EQ(drm_edp_backlight_disable(&g_aux, &bl), 0);
	CHECK(!(dpcd[DP_EDP_DISPLAY_CONTROL_REGISTER] & DP_EDP_BACKLIGHT_ENABLE), "disabled");

	/* Not yet in DPCD mode: the level reads as the maximum. */
	sink_reset();
	dpcd[DP_EDP_PWMGEN_BIT_COUNT] = 16;
	dpcd[DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN] = 8;
	dpcd[DP_EDP_PWMGEN_BIT_COUNT_CAP_MAX] = 16;
	memset(&bl, 0, sizeof(bl));
	CHECK_EQ(drm_edp_backlight_init(&g_aux, &bl, 0, 0, edp, &level, &mode, false), 0);
	CHECK_EQ(level, 0xffff);
	CHECK_EQ(mode, DP_EDP_BACKLIGHT_CONTROL_MODE_PWM);

	/* A bit count below the minimum: clamped up and written back. */
	sink_reset();
	dpcd[DP_EDP_PWMGEN_BIT_COUNT] = 4;
	dpcd[DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN] = 8;
	dpcd[DP_EDP_PWMGEN_BIT_COUNT_CAP_MAX] = 12;
	memset(&bl, 0, sizeof(bl));
	CHECK_EQ(drm_edp_backlight_init(&g_aux, &bl, 0, 0, edp, &level, &mode, false), 0);
	CHECK_EQ(bl.max, 0xff);
	CHECK_EQ(dpcd[DP_EDP_PWMGEN_BIT_COUNT], 8);

	/* A driver PWM frequency: 27 MHz / (F * 2^Pn) near 200 Hz. */
	sink_reset();
	dpcd[DP_EDP_PWMGEN_BIT_COUNT] = 16;
	dpcd[DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN] = 8;
	dpcd[DP_EDP_PWMGEN_BIT_COUNT_CAP_MAX] = 16;
	memset(&bl, 0, sizeof(bl));
	CHECK_EQ(drm_edp_backlight_init(&g_aux, &bl, 0, 200, edp, &level, &mode, false), 0);
	CHECK_EQ(bl.pwmgen_bit_count, 16);
	CHECK_EQ(bl.pwm_freq_pre_divider, 2);
	CHECK_EQ(bl.max, 0xffff);
	CHECK_EQ(drm_edp_backlight_enable(&g_aux, &bl, 0x8000), 0);
	CHECK_EQ(dpcd[DP_EDP_BACKLIGHT_FREQ_SET], 2);
	CHECK_EQ(dpcd[DP_EDP_BACKLIGHT_MODE_SET_REGISTER],
		 DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD | DP_EDP_BACKLIGHT_FREQ_AUX_SET_ENABLE);

	/* Luminance control (eDP 1.5): millinits, three bytes. */
	sink_reset();
	edp[0] = DP_EDP_15;
	edp[3] = DP_EDP_PANEL_LUMINANCE_CONTROL_CAPABLE;
	dpcd[DP_EDP_BACKLIGHT_MODE_SET_REGISTER] = DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD;
	dpcd[DP_EDP_PANEL_TARGET_LUMINANCE_VALUE] = 0xe0; /* 300000 mnits */
	dpcd[DP_EDP_PANEL_TARGET_LUMINANCE_VALUE + 1] = 0x93;
	dpcd[DP_EDP_PANEL_TARGET_LUMINANCE_VALUE + 2] = 0x04;
	memset(&bl, 0, sizeof(bl));
	CHECK_EQ(drm_edp_backlight_init(&g_aux, &bl, 500, 0, edp, &level, &mode, true), 0);
	CHECK(bl.luminance_set, "luminance");
	CHECK_EQ(bl.max, 500);
	CHECK_EQ(level, 300);
	CHECK_EQ(drm_edp_backlight_set_level(&g_aux, &bl, 250), 0);
	CHECK(dpcd[DP_EDP_PANEL_TARGET_LUMINANCE_VALUE] == 0x90 &&
	      dpcd[DP_EDP_PANEL_TARGET_LUMINANCE_VALUE + 1] == 0xd0 &&
	      dpcd[DP_EDP_PANEL_TARGET_LUMINANCE_VALUE + 2] == 0x03, "250 nits");

	/* No usable control at all. */
	memset(edp, 0, sizeof(edp));
	memset(&bl, 0, sizeof(bl));
	CHECK_EQ(drm_edp_backlight_init(&g_aux, &bl, 0, 0, edp, &level, &mode, false), -EINVAL);
}

static void test_misc(void)
{
	struct drm_dp_phy_test_params p;

	aux_setup();
	drm_dp_dpcd_set_probe(&g_aux, false);
	sink_reset();
	dpcd[DP_TEST_LINK_RATE] = DP_LINK_BW_2_7;
	dpcd[DP_TEST_LANE_COUNT] = 2 | DP_ENHANCED_FRAME_CAP;
	dpcd[DP_PHY_TEST_PATTERN] = DP_PHY_TEST_PATTERN_80BIT_CUSTOM;
	for (int i = 0; i < 10; i++)
		dpcd[DP_TEST_80BIT_CUSTOM_PATTERN_7_0 + i] = (u8)(0xc0 + i);
	memset(&p, 0, sizeof(p));
	CHECK_EQ(drm_dp_get_phy_test_pattern(&g_aux, &p), 0);
	CHECK(p.link_rate == 270000 && p.num_lanes == 2 && p.enhanced_frame_cap &&
	      p.custom80[9] == 0xc9, "phy test params");
	CHECK_EQ(drm_dp_set_phy_test_pattern(&g_aux, &p, 0x12), 0);
	CHECK(dpcd[DP_LINK_QUAL_LANE0_SET] == DP_PHY_TEST_PATTERN_80BIT_CUSTOM &&
	      dpcd[DP_LINK_QUAL_LANE0_SET + 1] == DP_PHY_TEST_PATTERN_80BIT_CUSTOM &&
	      dpcd[DP_LINK_QUAL_LANE0_SET + 2] == 0, "pattern per lane");

	/* EDID checksum reporting for compliance tests. */
	sink_reset();
	dpcd[DP_DEVICE_SERVICE_IRQ_VECTOR] = DP_AUTOMATED_TEST_REQUEST;
	dpcd[DP_TEST_REQUEST] = DP_TEST_LINK_EDID_READ;
	CHECK(drm_dp_send_real_edid_checksum(&g_aux, 0x5a), "checksum sent");
	CHECK_EQ(dpcd[DP_TEST_EDID_CHECKSUM], 0x5a);
	CHECK_EQ(dpcd[DP_TEST_RESPONSE], DP_TEST_EDID_CHECKSUM_WRITE);
	dpcd[DP_TEST_REQUEST] = 0;
	CHECK(!drm_dp_send_real_edid_checksum(&g_aux, 0x5a), "no test request");

	/* PCON: FRL configuration and DSC encoder controls. */
	sink_reset();
	CHECK_EQ(drm_dp_pcon_frl_prepare(&g_aux, true), 0);
	CHECK_EQ(dpcd[DP_PCON_HDMI_LINK_CONFIG_1], DP_PCON_ENABLE_SOURCE_CTL_MODE |
		 DP_PCON_ENABLE_LINK_FRL_MODE | DP_PCON_ENABLE_HPD_READY);
	CHECK_EQ(drm_dp_pcon_frl_configure_1(&g_aux, 24, DP_PCON_ENABLE_SEQUENTIAL_LINK), 0);
	CHECK_EQ(dpcd[DP_PCON_HDMI_LINK_CONFIG_1] & DP_PCON_ENABLE_MAX_FRL_BW,
		 DP_PCON_ENABLE_MAX_BW_24GBPS);
	CHECK_EQ(drm_dp_pcon_frl_configure_1(&g_aux, 25, 0), -EINVAL);
	CHECK_EQ(drm_dp_pcon_frl_configure_2(&g_aux, DP_PCON_FRL_BW_MASK_24GBPS,
					     DP_PCON_FRL_LINK_TRAIN_EXTENDED), 0);
	CHECK_EQ(dpcd[DP_PCON_HDMI_LINK_CONFIG_2],
		 DP_PCON_FRL_BW_MASK_24GBPS | DP_PCON_FRL_LINK_TRAIN_EXTENDED);
	CHECK_EQ(drm_dp_pcon_frl_enable(&g_aux), 0);
	CHECK(dpcd[DP_PCON_HDMI_LINK_CONFIG_1] & DP_PCON_ENABLE_HDMI_LINK, "FRL enabled");
	{
		u8 pps[128];

		for (int i = 0; i < 128; i++)
			pps[i] = (u8)i;
		CHECK_EQ(drm_dp_pcon_pps_override_buf(&g_aux, pps), 0);
		CHECK(!memcmp(dpcd + DP_PCON_HDMI_PPS_OVERRIDE_BASE, pps, 128), "PPS buffer");
		CHECK(dpcd[DP_PROTOCOL_CONVERTER_CONTROL_2] & DP_PCON_ENABLE_DSC_ENCODER,
		      "DSC encoder on");
	}
	CHECK_EQ(drm_dp_pcon_convert_rgb_to_ycbcr(&g_aux, DP_CONVERSION_BT709_RGB_YCBCR_ENABLE), 0);
	CHECK(dpcd[DP_PROTOCOL_CONVERTER_CONTROL_2] & DP_CONVERSION_BT709_RGB_YCBCR_ENABLE,
	      "RGB->YCbCr");

	/* The branch debug dump goes to the console. */
	{
		u8 caps[DP_RECEIVER_CAP_SIZE] = { 0 };
		u8 hdmi[4] = { DP_DS_PORT_TYPE_HDMI, 0x78, DP_DS_12BPC, 0 };
		int k = kprintf_lines;

		caps[DP_DPCD_REV] = 0x12;
		caps[DP_DOWNSTREAMPORT_PRESENT] = DP_DWN_STRM_PORT_PRESENT |
						  DP_DETAILED_CAP_INFO_AVAILABLE;
		memcpy(dpcd + DP_BRANCH_ID, "LKOS01", 6);
		drm_dp_downstream_debug(caps, hdmi, NULL, &g_aux);
		CHECK(kprintf_lines - k >= 6, "branch dump");
	}
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-v"))
		verbose = 1;
	dpcd = calloc(DPCD_SPACE, 1);
	if (!dpcd)
		return 2;

	test_link_status();
	test_bw_codes();
	test_dpcd_access();
	test_dpcd_caps();
	test_delays();
	test_i2c();
	test_desc();
	test_downstream();
	test_lttpr();
	test_dsc();
	test_sdp_psr_bw();
	test_backlight();
	test_misc();

	free(dpcd);
	printf("drm_dp_helper: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
