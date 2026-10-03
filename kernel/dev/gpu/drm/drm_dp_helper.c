// LikeOS -- DisplayPort helpers: AUX channel transfers, DPCD access, I2C
// over AUX, and decoding of what a sink, branch device or repeater reports.
//
// The AUX channel carries one transaction at a time: a request (native
// DPCD read/write, or an I2C read/write with the middle-of-transaction
// bit) of at most 16 payload bytes, answered by a reply code.  A native
// reply is ACK, NACK or DEFER (busy, ask again); an I2C-over-AUX reply
// adds the I2C ACK/NACK/DEFER of the bus behind the sink.  The driver's
// transfer hook runs exactly one such transaction.  This file builds the
// rest on it:
//
//   - DPCD reads and writes, retried up to 32 times (DEFERs, -EBUSY,
//     timeouts), with the throw-away read some monitors need after they
//     wake from power save;
//   - I2C over AUX as an i2c_adapter: each message opened with a bare
//     address transaction, split into the largest chunks the sink takes,
//     a partial ACK drained by WRITE_STATUS_UPDATE requests, I2C DEFERs
//     retried for as long as the I2C bus could plausibly need, and the
//     transaction closed with a bare address request without MOT;
//   - link status decoders and training delays (8b/10b and 128b/132b, the
//     DPRX and LTTPRs), the extended receiver capabilities, LTTPR mode
//     setting, branch-device limits, the identification quirk table, DSC,
//     FEC, PSR and SDP helpers, the VESA eDP backlight, PCON controls and
//     the link bandwidth arithmetic.
//
// Everything here runs in process context and may sleep (see the delay
// helper below); one channel's transactions are serialised by
// aux->hw_mutex.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Keith Packard's code: HPND-sell-variant
// Portions Copyright (C) 2009 Keith Packard
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008 Intel Corporation

#include <kernel/dev/gpu/drm_dp_helper.h>
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/uapi/bug.h>

/* The I2C "send a STOP after this message" flag.  The kernel's I2C
 * interface has no such flag yet, so every message but the closing bare
 * address request keeps the middle-of-transaction bit, which is what all
 * its users want. */
#ifndef I2C_M_STOP
#define I2C_M_STOP 0x8000
#endif

#ifndef USEC_PER_MSEC
#define USEC_PER_MSEC 1000
#endif

/* ---- logging ---------------------------------------------------------------- */

/* Debug output: compiled in but dead unless DRM_DP_DEBUG is 1, so the
 * arguments are still type-checked. */
#define dp_dbg(fmt, ...)                                                \
	do {                                                            \
		if (DRM_DP_DEBUG)                                       \
			kprintf("[drm] dp: " fmt, ##__VA_ARGS__);       \
	} while (0)

/* Real failures: at most ten messages per call site, so a sink that keeps
 * failing cannot flood the console. */
#define dp_err(fmt, ...)                                                \
	do {                                                            \
		static int __dp_err_count;                              \
		if (__dp_err_count < 10) {                              \
			__dp_err_count++;                               \
			kprintf("[drm] dp: " fmt, ##__VA_ARGS__);       \
			if (__dp_err_count == 10)                       \
				kprintf("[drm] dp: further messages "   \
					"from %s suppressed\n",         \
					__func__);                      \
		}                                                       \
	} while (0)

static const char *aux_name(const struct drm_dp_aux *aux)
{
	return aux && aux->name ? aux->name : "AUX";
}

/* Bytes as "aa bb cc" (debug output only). */
static void dp_dbg_hex(const char *what, const char *name, unsigned int offset,
		       const void *buf, int len)
{
	const u8 *b = buf;
	int i;

	if (!DRM_DP_DEBUG)
		return;
	kprintf("[drm] dp: %s: %s 0x%05x:", name, what, offset);
	for (i = 0; i < len; i++)
		kprintf(" %02x", b[i]);
	kprintf("\n");
}

/* ---- waiting ------------------------------------------------------------------ */

/*
 * Wait at least `min_us' microseconds.  Waits of a millisecond or more
 * sleep when the high-resolution timer has a comparator behind it,
 * interrupts are on and the caller is an ordinary task (never the boot or
 * idle thread, which cannot block); everything else is a calibrated busy
 * wait.  A sleep cut short (a signal, no task) busy-waits the rest, so the
 * sink always gets its full time.
 */
static void dp_usleep_range(unsigned int min_us, unsigned int max_us)
{
	(void)max_us;
#if DRM_DP_SLEEPING_DELAYS
	if (min_us >= 1000 && hrtimer_is_highres() && !irqs_disabled()) {
		task_t *cur = sched_current();

		if (cur && !sched_task_hidden(cur)) {
			uint64_t deadline = hrtimer_now_ns() + (uint64_t)min_us * 1000;
			uint64_t now;

			if (hrtimer_sleep_until(deadline, NULL) == 0)
				return;
			now = hrtimer_now_ns();
			if (now < deadline)
				lapic_delay_us((uint32_t)DIV_ROUND_UP_ULL(deadline - now, 1000));
			return;
		}
	}
#endif
	lapic_delay_us(min_us);
}

/* ---- link training status ------------------------------------------------------ */

static u8 dp_link_status(const u8 link_status[DP_LINK_STATUS_SIZE], int r)
{
	return link_status[r - DP_LANE0_1_STATUS];
}

static u8 dp_get_lane_status(const u8 link_status[DP_LINK_STATUS_SIZE],
			     int lane)
{
	int i = DP_LANE0_1_STATUS + (lane >> 1);
	int s = (lane & 1) * 4;
	u8 l = dp_link_status(link_status, i);

	return (l >> s) & 0xf;
}

bool drm_dp_channel_eq_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			  int lane_count)
{
	u8 lane_align;
	u8 lane_status;
	int lane;

	lane_align = dp_link_status(link_status,
				    DP_LANE_ALIGN_STATUS_UPDATED);
	if ((lane_align & DP_INTERLANE_ALIGN_DONE) == 0)
		return false;
	for (lane = 0; lane < lane_count; lane++) {
		lane_status = dp_get_lane_status(link_status, lane);
		if ((lane_status & DP_CHANNEL_EQ_BITS) != DP_CHANNEL_EQ_BITS)
			return false;
	}
	return true;
}

bool drm_dp_clock_recovery_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			      int lane_count)
{
	int lane;
	u8 lane_status;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = dp_get_lane_status(link_status, lane);
		if ((lane_status & DP_LANE_CR_DONE) == 0)
			return false;
	}
	return true;
}

bool drm_dp_post_lt_adj_req_in_progress(const u8 link_status[DP_LINK_STATUS_SIZE])
{
	u8 lane_align = dp_link_status(link_status, DP_LANE_ALIGN_STATUS_UPDATED);

	return !!(lane_align & DP_POST_LT_ADJ_REQ_IN_PROGRESS);
}

u8 drm_dp_get_adjust_request_voltage(const u8 link_status[DP_LINK_STATUS_SIZE],
				     int lane)
{
	int i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int s = ((lane & 1) ?
		 DP_ADJUST_VOLTAGE_SWING_LANE1_SHIFT :
		 DP_ADJUST_VOLTAGE_SWING_LANE0_SHIFT);
	u8 l = dp_link_status(link_status, i);

	return ((l >> s) & 0x3) << DP_TRAIN_VOLTAGE_SWING_SHIFT;
}

u8 drm_dp_get_adjust_request_pre_emphasis(const u8 link_status[DP_LINK_STATUS_SIZE],
					  int lane)
{
	int i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int s = ((lane & 1) ?
		 DP_ADJUST_PRE_EMPHASIS_LANE1_SHIFT :
		 DP_ADJUST_PRE_EMPHASIS_LANE0_SHIFT);
	u8 l = dp_link_status(link_status, i);

	return ((l >> s) & 0x3) << DP_TRAIN_PRE_EMPHASIS_SHIFT;
}

/* DP 2.0 128b/132b */
u8 drm_dp_get_adjust_tx_ffe_preset(const u8 link_status[DP_LINK_STATUS_SIZE],
				   int lane)
{
	int i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int s = ((lane & 1) ?
		 DP_ADJUST_TX_FFE_PRESET_LANE1_SHIFT :
		 DP_ADJUST_TX_FFE_PRESET_LANE0_SHIFT);
	u8 l = dp_link_status(link_status, i);

	return (l >> s) & 0xf;
}

/* DP 2.0 errata for 128b/132b */
bool drm_dp_128b132b_lane_channel_eq_done(const u8 link_status[DP_LINK_STATUS_SIZE],
					  int lane_count)
{
	u8 lane_align, lane_status;
	int lane;

	lane_align = dp_link_status(link_status, DP_LANE_ALIGN_STATUS_UPDATED);
	if (!(lane_align & DP_INTERLANE_ALIGN_DONE))
		return false;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = dp_get_lane_status(link_status, lane);
		if (!(lane_status & DP_LANE_CHANNEL_EQ_DONE))
			return false;
	}
	return true;
}

/* DP 2.0 errata for 128b/132b */
bool drm_dp_128b132b_lane_symbol_locked(const u8 link_status[DP_LINK_STATUS_SIZE],
					int lane_count)
{
	u8 lane_status;
	int lane;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = dp_get_lane_status(link_status, lane);
		if (!(lane_status & DP_LANE_SYMBOL_LOCKED))
			return false;
	}
	return true;
}

/* DP 2.0 errata for 128b/132b */
bool drm_dp_128b132b_eq_interlane_align_done(const u8 link_status[DP_LINK_STATUS_SIZE])
{
	u8 status = dp_link_status(link_status, DP_LANE_ALIGN_STATUS_UPDATED);

	return !!(status & DP_128B132B_DPRX_EQ_INTERLANE_ALIGN_DONE);
}

/* DP 2.0 errata for 128b/132b */
bool drm_dp_128b132b_cds_interlane_align_done(const u8 link_status[DP_LINK_STATUS_SIZE])
{
	u8 status = dp_link_status(link_status, DP_LANE_ALIGN_STATUS_UPDATED);

	return !!(status & DP_128B132B_DPRX_CDS_INTERLANE_ALIGN_DONE);
}

/* DP 2.0 errata for 128b/132b */
bool drm_dp_128b132b_link_training_failed(const u8 link_status[DP_LINK_STATUS_SIZE])
{
	u8 status = dp_link_status(link_status, DP_LANE_ALIGN_STATUS_UPDATED);

	return !!(status & DP_128B132B_LT_FAILED);
}

/* ---- training delays ------------------------------------------------------------- */

static int __8b10b_clock_recovery_delay_us(const struct drm_dp_aux *aux, u8 rd_interval)
{
	if (rd_interval > 4)
		dp_dbg("%s: invalid AUX interval 0x%02x (max 4)\n",
		       aux_name(aux), rd_interval);

	if (rd_interval == 0)
		return 100;

	return rd_interval * 4 * USEC_PER_MSEC;
}

static int __8b10b_channel_eq_delay_us(const struct drm_dp_aux *aux, u8 rd_interval)
{
	if (rd_interval > 4)
		dp_dbg("%s: invalid AUX interval 0x%02x (max 4)\n",
		       aux_name(aux), rd_interval);

	if (rd_interval == 0)
		return 400;

	return rd_interval * 4 * USEC_PER_MSEC;
}

static int __128b132b_channel_eq_delay_us(const struct drm_dp_aux *aux, u8 rd_interval)
{
	switch (rd_interval) {
	default:
		dp_dbg("%s: invalid AUX interval 0x%02x\n",
		       aux_name(aux), rd_interval);
		fallthrough;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_400_US:
		return 400;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_4_MS:
		return 4000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_8_MS:
		return 8000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_12_MS:
		return 12000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_16_MS:
		return 16000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_32_MS:
		return 32000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_64_MS:
		return 64000;
	}
}

/*
 * The link training delays are different for:
 *
 *  - Clock recovery vs. channel equalization
 *  - DPRX vs. LTTPR
 *  - 128b/132b vs. 8b/10b
 *  - DPCD rev 1.3 vs. later
 *
 * Get the correct delay in us, reading DPCD if necessary.
 */
static int __read_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			enum drm_dp_phy dp_phy, bool uhbr, bool cr)
{
	int (*parse)(const struct drm_dp_aux *aux, u8 rd_interval);
	unsigned int offset;
	u8 rd_interval, mask;

	if (dp_phy == DP_PHY_DPRX) {
		if (uhbr) {
			if (cr)
				return 100;

			offset = DP_128B132B_TRAINING_AUX_RD_INTERVAL;
			mask = DP_128B132B_TRAINING_AUX_RD_INTERVAL_MASK;
			parse = __128b132b_channel_eq_delay_us;
		} else {
			if (cr && dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14)
				return 100;

			offset = DP_TRAINING_AUX_RD_INTERVAL;
			mask = DP_TRAINING_AUX_RD_MASK;
			if (cr)
				parse = __8b10b_clock_recovery_delay_us;
			else
				parse = __8b10b_channel_eq_delay_us;
		}
	} else {
		if (uhbr) {
			offset = DP_128B132B_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER(dp_phy);
			mask = DP_128B132B_TRAINING_AUX_RD_INTERVAL_MASK;
			parse = __128b132b_channel_eq_delay_us;
		} else {
			if (cr)
				return 100;

			offset = DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER(dp_phy);
			mask = DP_TRAINING_AUX_RD_MASK;
			parse = __8b10b_channel_eq_delay_us;
		}
	}

	if (offset < DP_RECEIVER_CAP_SIZE) {
		rd_interval = dpcd[offset];
	} else {
		if (drm_dp_dpcd_read_byte(aux, offset, &rd_interval) < 0) {
			dp_dbg("%s: failed rd interval read\n", aux_name(aux));
			/* arbitrary default delay */
			return 400;
		}
	}

	return parse(aux, rd_interval & mask);
}

int drm_dp_read_clock_recovery_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				     enum drm_dp_phy dp_phy, bool uhbr)
{
	return __read_delay(aux, dpcd, dp_phy, uhbr, true);
}

int drm_dp_read_channel_eq_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				 enum drm_dp_phy dp_phy, bool uhbr)
{
	return __read_delay(aux, dpcd, dp_phy, uhbr, false);
}

/* Per DP 2.0 Errata */
int drm_dp_128b132b_read_aux_rd_interval(struct drm_dp_aux *aux)
{
	int unit;
	u8 val;

	if (drm_dp_dpcd_read_byte(aux, DP_128B132B_TRAINING_AUX_RD_INTERVAL, &val) < 0) {
		dp_err("%s: failed rd interval read\n", aux_name(aux));
		/* default to max */
		val = DP_128B132B_TRAINING_AUX_RD_INTERVAL_MASK;
	}

	unit = (val & DP_128B132B_TRAINING_AUX_RD_INTERVAL_1MS_UNIT) ? 1 : 2;
	val &= DP_128B132B_TRAINING_AUX_RD_INTERVAL_MASK;

	return (val + 1) * unit * 1000;
}

void drm_dp_link_train_clock_recovery_delay(const struct drm_dp_aux *aux,
					    const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	u8 rd_interval = dpcd[DP_TRAINING_AUX_RD_INTERVAL] &
		DP_TRAINING_AUX_RD_MASK;
	int delay_us;

	if (dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14)
		delay_us = 100;
	else
		delay_us = __8b10b_clock_recovery_delay_us(aux, rd_interval);

	dp_usleep_range(delay_us, delay_us * 2);
}

static void __drm_dp_link_train_channel_eq_delay(const struct drm_dp_aux *aux,
						 u8 rd_interval)
{
	int delay_us = __8b10b_channel_eq_delay_us(aux, rd_interval);

	dp_usleep_range(delay_us, delay_us * 2);
}

void drm_dp_link_train_channel_eq_delay(const struct drm_dp_aux *aux,
					const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	__drm_dp_link_train_channel_eq_delay(aux,
					     dpcd[DP_TRAINING_AUX_RD_INTERVAL] &
					     DP_TRAINING_AUX_RD_MASK);
}

const char *drm_dp_phy_name(enum drm_dp_phy dp_phy)
{
	static const char * const phy_names[] = {
		[DP_PHY_DPRX] = "DPRX",
		[DP_PHY_LTTPR1] = "LTTPR 1",
		[DP_PHY_LTTPR2] = "LTTPR 2",
		[DP_PHY_LTTPR3] = "LTTPR 3",
		[DP_PHY_LTTPR4] = "LTTPR 4",
		[DP_PHY_LTTPR5] = "LTTPR 5",
		[DP_PHY_LTTPR6] = "LTTPR 6",
		[DP_PHY_LTTPR7] = "LTTPR 7",
		[DP_PHY_LTTPR8] = "LTTPR 8",
	};

	if ((int)dp_phy < 0 || (size_t)dp_phy >= ARRAY_SIZE(phy_names) ||
	    WARN_ON(!phy_names[dp_phy]))
		return "<INVALID DP PHY>";

	return phy_names[dp_phy];
}

void drm_dp_lttpr_link_train_clock_recovery_delay(void)
{
	dp_usleep_range(100, 200);
}

static u8 dp_lttpr_phy_cap(const u8 phy_cap[DP_LTTPR_PHY_CAP_SIZE], int r)
{
	return phy_cap[r - DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER1];
}

void drm_dp_lttpr_link_train_channel_eq_delay(const struct drm_dp_aux *aux,
					      const u8 phy_cap[DP_LTTPR_PHY_CAP_SIZE])
{
	u8 interval = dp_lttpr_phy_cap(phy_cap,
				       DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER1) &
		      DP_TRAINING_AUX_RD_MASK;

	__drm_dp_link_train_channel_eq_delay(aux, interval);
}

/*
 * Grant the sink (transparent mode) or the first repeater (non-transparent
 * mode) the extended wake-up time it asks for.  Without the grant every
 * AUX transaction to a sleeping sink is expected to answer within 1 ms.
 */
void drm_dp_lttpr_wake_timeout_setup(struct drm_dp_aux *aux, bool transparent_mode)
{
	u8 val = 1;
	int ret;

	if (transparent_mode) {
		static const u8 timeout_mapping[] = {
			[DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_1_MS] = 1,
			[DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_20_MS] = 20,
			[DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_40_MS] = 40,
			[DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_60_MS] = 60,
			[DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_80_MS] = 80,
			[DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_100_MS] = 100,
		};

		ret = drm_dp_dpcd_readb(aux, DP_EXTENDED_DPRX_SLEEP_WAKE_TIMEOUT_REQUEST, &val);
		if (ret != 1) {
			dp_dbg("Failed to read Extended sleep wake timeout request\n");
			return;
		}

		val = (val < sizeof(timeout_mapping) && timeout_mapping[val]) ?
			timeout_mapping[val] : 1;

		if (val > 1)
			drm_dp_dpcd_writeb(aux,
					   DP_EXTENDED_DPRX_SLEEP_WAKE_TIMEOUT_GRANT,
					   DP_DPRX_SLEEP_WAKE_TIMEOUT_PERIOD_GRANTED);
	} else {
		ret = drm_dp_dpcd_readb(aux, DP_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT, &val);
		if (ret != 1) {
			dp_dbg("Failed to read Extended sleep wake timeout request\n");
			return;
		}

		val = (val & DP_EXTENDED_WAKE_TIMEOUT_REQUEST_MASK) ?
			(val & DP_EXTENDED_WAKE_TIMEOUT_REQUEST_MASK) * 10 : 1;

		if (val > 1)
			drm_dp_dpcd_writeb(aux, DP_PHY_REPEATER_EXTENDED_WAIT_TIMEOUT,
					   DP_EXTENDED_WAKE_TIMEOUT_GRANT);
	}
}

u8 drm_dp_link_rate_to_bw_code(int link_rate)
{
	switch (link_rate) {
	case 1000000:
		return DP_LINK_BW_10;
	case 1350000:
		return DP_LINK_BW_13_5;
	case 2000000:
		return DP_LINK_BW_20;
	default:
		/* Spec says link_bw = link_rate / 0.27Gbps */
		return link_rate / 27000;
	}
}

int drm_dp_bw_code_to_link_rate(u8 link_bw)
{
	switch (link_bw) {
	case DP_LINK_BW_10:
		return 1000000;
	case DP_LINK_BW_13_5:
		return 1350000;
	case DP_LINK_BW_20:
		return 2000000;
	default:
		/* Spec says link_rate = link_bw * 0.27Gbps */
		return link_bw * 27000;
	}
}

/* ---- native AUX (DPCD) access -------------------------------------------------- */

#define AUX_RETRY_INTERVAL 500 /* us */

static void drm_dp_dump_access(const struct drm_dp_aux *aux,
			       u8 request, unsigned int offset, void *buffer, int ret)
{
	const char *arrow = request == DP_AUX_NATIVE_READ ? "->" : "<-";

	if (!DRM_DP_DEBUG)
		return;
	if (ret > 0)
		dp_dbg_hex(request == DP_AUX_NATIVE_READ ? "AUX ->" : "AUX <-",
			   aux_name(aux), offset, buffer, min(ret, 20));
	else
		dp_dbg("%s: 0x%05x AUX %s (ret=%d)\n",
		       aux_name(aux), offset, arrow, ret);
}

static void aux_lock(struct drm_dp_aux *aux)
{
	mm_write_lock(&aux->hw_mutex);
}

static void aux_unlock(struct drm_dp_aux *aux)
{
	mm_write_unlock(&aux->hw_mutex);
}

/*
 * One native read or write of up to 16 bytes, retried while the sink
 * DEFERs or the transfer fails with anything but a timeout -- and after a
 * timeout too, only without the pause.  The error returned is the one the
 * first attempt saw: later attempts may fail differently.
 */
static int drm_dp_dpcd_access(struct drm_dp_aux *aux, u8 request,
			      unsigned int offset, void *buffer, size_t size)
{
	struct drm_dp_aux_msg msg;
	unsigned int retry, native_reply;
	int err = 0, ret = 0;

	if (WARN_ON_ONCE(!aux->initialized || !aux->transfer))
		return -ENODEV;

	mm_memset(&msg, 0, sizeof(msg));
	msg.address = offset;
	msg.request = request;
	msg.buffer = buffer;
	msg.size = size;

	aux_lock(aux);

	/*
	 * If the device attached to the aux bus is powered down then there's
	 * no reason to attempt a transfer. Error out immediately.
	 */
	if (aux->powered_down) {
		ret = -EBUSY;
		goto unlock;
	}

	/*
	 * The specification doesn't give any recommendation on how often to
	 * retry native transactions.  Seven retries, as for I2C over AUX,
	 * were not enough for real devices (Dell 4K monitors); 32 are.
	 */
	for (retry = 0; retry < 32; retry++) {
		if (ret != 0 && ret != -ETIMEDOUT) {
			dp_usleep_range(AUX_RETRY_INTERVAL,
					AUX_RETRY_INTERVAL + 100);
		}

		ret = aux->transfer(aux, &msg);
		if (ret >= 0) {
			native_reply = msg.reply & DP_AUX_NATIVE_REPLY_MASK;
			if (native_reply == DP_AUX_NATIVE_REPLY_ACK) {
				if ((size_t)ret == size)
					goto unlock;

				ret = -EPROTO;
			} else
				ret = -EIO;
		}

		/*
		 * We want the error we return to be the error we received on
		 * the first transaction, since we may get a different error the
		 * next time we retry
		 */
		if (!err)
			err = ret;
	}

	dp_dbg("%s: Too many retries, giving up. First error: %d\n",
	       aux_name(aux), err);
	ret = err;

unlock:
	aux_unlock(aux);
	return ret;
}

int drm_dp_dpcd_probe(struct drm_dp_aux *aux, unsigned int offset)
{
	u8 buffer;
	int ret;

	ret = drm_dp_dpcd_access(aux, DP_AUX_NATIVE_READ, offset, &buffer, 1);
	WARN_ON(ret == 0);

	drm_dp_dump_access(aux, DP_AUX_NATIVE_READ, offset, &buffer, ret);

	return ret < 0 ? ret : 0;
}

/*
 * Mark the endpoint powered (or not).  While it is down, transfers fail
 * at once with -EBUSY instead of each waiting for its timeout.  A channel
 * starts out powered.
 */
void drm_dp_dpcd_set_powered(struct drm_dp_aux *aux, bool powered)
{
	if (!aux)
		return;

	if (!aux->initialized) {
		aux->powered_down = !powered;
		return;
	}
	aux_lock(aux);
	aux->powered_down = !powered;
	aux_unlock(aux);
}

void drm_dp_dpcd_set_probe(struct drm_dp_aux *aux, bool enable)
{
	__atomic_store_n(&aux->dpcd_probe_disabled, !enable, __ATOMIC_RELAXED);
}

static bool dpcd_access_needs_probe(struct drm_dp_aux *aux)
{
	/*
	 * HP ZR24w corrupts the first DPCD access after entering power save
	 * mode. Eg. on a read, the entire buffer will be filled with the same
	 * byte. Do a throw away read to avoid corrupting anything we care
	 * about. Afterwards things will work correctly until the monitor
	 * gets woken up and subsequently re-enters power save mode.
	 *
	 * The user pressing any button on the monitor is enough to wake it
	 * up, so there is no particularly good place to do the workaround.
	 * We just have to do it before any DPCD access and hope that the
	 * monitor doesn't power down exactly after the throw away read.
	 */
	return !__atomic_load_n(&aux->dpcd_probe_disabled, __ATOMIC_RELAXED);
}

/* Transfers of more than 16 bytes are split here: the transfer hook only
 * ever sees what fits one AUX transaction. */
static ssize_t dpcd_access_chunked(struct drm_dp_aux *aux, u8 request,
				   unsigned int offset, void *buffer, size_t size)
{
	size_t done = 0;
	u8 *buf = buffer;
	int ret;

	if (size <= DP_AUX_MAX_PAYLOAD_BYTES)
		return drm_dp_dpcd_access(aux, request, offset, buffer, size);

	while (done < size) {
		size_t chunk = min_t(size_t, size - done, DP_AUX_MAX_PAYLOAD_BYTES);

		ret = drm_dp_dpcd_access(aux, request, offset + done,
					 buf + done, chunk);
		if (ret < 0)
			return ret;
		done += (size_t)ret;
	}
	return (ssize_t)done;
}

ssize_t drm_dp_dpcd_read(struct drm_dp_aux *aux, unsigned int offset,
			 void *buffer, size_t size)
{
	int ret;

	if (dpcd_access_needs_probe(aux)) {
		ret = drm_dp_dpcd_probe(aux, DP_TRAINING_PATTERN_SET);
		if (ret < 0)
			return ret;
	}

	ret = dpcd_access_chunked(aux, DP_AUX_NATIVE_READ, offset, buffer, size);

	drm_dp_dump_access(aux, DP_AUX_NATIVE_READ, offset, buffer, ret);
	return ret;
}

ssize_t drm_dp_dpcd_write(struct drm_dp_aux *aux, unsigned int offset,
			  void *buffer, size_t size)
{
	int ret;

	ret = dpcd_access_chunked(aux, DP_AUX_NATIVE_WRITE, offset, buffer, size);

	drm_dp_dump_access(aux, DP_AUX_NATIVE_WRITE, offset, buffer, ret);
	return ret;
}

int drm_dp_dpcd_read_link_status(struct drm_dp_aux *aux,
				 u8 status[DP_LINK_STATUS_SIZE])
{
	return drm_dp_dpcd_read_data(aux, DP_LANE0_1_STATUS, status,
				     DP_LINK_STATUS_SIZE);
}

int drm_dp_dpcd_read_phy_link_status(struct drm_dp_aux *aux,
				     enum drm_dp_phy dp_phy,
				     u8 link_status[DP_LINK_STATUS_SIZE])
{
	int ret, i;

	if (dp_phy == DP_PHY_DPRX)
		return drm_dp_dpcd_read_data(aux,
					     DP_LANE0_1_STATUS,
					     link_status,
					     DP_LINK_STATUS_SIZE);

	ret = drm_dp_dpcd_read_data(aux,
				    DP_LANE0_1_STATUS_PHY_REPEATER(dp_phy),
				    link_status,
				    DP_LINK_STATUS_SIZE - 1);

	if (ret < 0)
		return ret;

	/* Convert the LTTPR to the sink PHY link status layout: the repeater
	 * has no SINK_STATUS byte, so the adjust requests move up by one. */
	for (i = DP_LINK_STATUS_SIZE - 1; i > DP_SINK_STATUS - DP_LANE0_1_STATUS; i--)
		link_status[i] = link_status[i - 1];
	link_status[DP_SINK_STATUS - DP_LANE0_1_STATUS] = 0;

	return 0;
}

int drm_dp_link_power_up(struct drm_dp_aux *aux, unsigned char revision)
{
	u8 value;
	int err;

	/* DP_SET_POWER register is only available on DPCD v1.1 and later */
	if (revision < DP_DPCD_REV_11)
		return 0;

	err = drm_dp_dpcd_readb(aux, DP_SET_POWER, &value);
	if (err < 0)
		return err;

	value &= ~DP_SET_POWER_MASK;
	value |= DP_SET_POWER_D0;

	err = drm_dp_dpcd_writeb(aux, DP_SET_POWER, value);
	if (err < 0)
		return err;

	/*
	 * According to the DP 1.1 specification, a "Sink Device must exit the
	 * power saving state within 1 ms" (Section 2.5.3.1, Table 5-52, "Sink
	 * Control Field" (register 0x600).
	 */
	dp_usleep_range(1000, 2000);

	return 0;
}

int drm_dp_link_power_down(struct drm_dp_aux *aux, unsigned char revision)
{
	u8 value;
	int err;

	/* DP_SET_POWER register is only available on DPCD v1.1 and later */
	if (revision < DP_DPCD_REV_11)
		return 0;

	err = drm_dp_dpcd_readb(aux, DP_SET_POWER, &value);
	if (err < 0)
		return err;

	value &= ~DP_SET_POWER_MASK;
	value |= DP_SET_POWER_D3;

	err = drm_dp_dpcd_writeb(aux, DP_SET_POWER, value);
	if (err < 0)
		return err;

	return 0;
}

/* ---- downstream facing ports ------------------------------------------------------ */

static bool is_edid_digital_input_dp(const struct drm_edid *drm_edid)
{
	const struct edid *edid = drm_edid ? drm_edid->edid : NULL;

	return edid && edid->revision >= 4 &&
		edid->input & DRM_EDID_INPUT_DIGITAL &&
		(edid->input & DRM_EDID_DIGITAL_TYPE_MASK) == DRM_EDID_DIGITAL_TYPE_DP;
}

/* Is the downstream facing port of `type' (DP_DS_PORT_TYPE_*)?  DPCD 1.1+
 * port caps only. */
bool drm_dp_downstream_is_type(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			       const u8 port_cap[4], u8 type)
{
	return drm_dp_is_branch(dpcd) &&
		dpcd[DP_DPCD_REV] >= 0x11 &&
		(port_cap[0] & DP_DS_PORT_TYPE_MASK) == type;
}

bool drm_dp_downstream_is_tmds(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			       const u8 port_cap[4],
			       const struct drm_edid *drm_edid)
{
	if (dpcd[DP_DPCD_REV] < 0x11) {
		switch (dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_TYPE_MASK) {
		case DP_DWN_STRM_PORT_TYPE_TMDS:
			return true;
		default:
			return false;
		}
	}

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_DP_DUALMODE:
		if (is_edid_digital_input_dp(drm_edid))
			return false;
		fallthrough;
	case DP_DS_PORT_TYPE_DVI:
	case DP_DS_PORT_TYPE_HDMI:
		return true;
	default:
		return false;
	}
}

bool drm_dp_send_real_edid_checksum(struct drm_dp_aux *aux,
				    u8 real_edid_checksum)
{
	u8 link_edid_read = 0, auto_test_req = 0, test_resp = 0;

	if (drm_dp_dpcd_read_byte(aux, DP_DEVICE_SERVICE_IRQ_VECTOR,
				  &auto_test_req) < 0) {
		dp_err("%s: DPCD failed read at register 0x%x\n",
		       aux_name(aux), DP_DEVICE_SERVICE_IRQ_VECTOR);
		return false;
	}
	auto_test_req &= DP_AUTOMATED_TEST_REQUEST;

	if (drm_dp_dpcd_read_byte(aux, DP_TEST_REQUEST, &link_edid_read) < 0) {
		dp_err("%s: DPCD failed read at register 0x%x\n",
		       aux_name(aux), DP_TEST_REQUEST);
		return false;
	}
	link_edid_read &= DP_TEST_LINK_EDID_READ;

	if (!auto_test_req || !link_edid_read) {
		dp_dbg("%s: Source DUT does not support TEST_EDID_READ\n",
		       aux_name(aux));
		return false;
	}

	if (drm_dp_dpcd_write_byte(aux, DP_DEVICE_SERVICE_IRQ_VECTOR,
				   auto_test_req) < 0) {
		dp_err("%s: DPCD failed write at register 0x%x\n",
		       aux_name(aux), DP_DEVICE_SERVICE_IRQ_VECTOR);
		return false;
	}

	/* send back checksum for the last edid extension block data */
	if (drm_dp_dpcd_write_byte(aux, DP_TEST_EDID_CHECKSUM,
				   real_edid_checksum) < 0) {
		dp_err("%s: DPCD failed write at register 0x%x\n",
		       aux_name(aux), DP_TEST_EDID_CHECKSUM);
		return false;
	}

	test_resp |= DP_TEST_EDID_CHECKSUM_WRITE;
	if (drm_dp_dpcd_write_byte(aux, DP_TEST_RESPONSE, test_resp) < 0) {
		dp_err("%s: DPCD failed write at register 0x%x\n",
		       aux_name(aux), DP_TEST_RESPONSE);
		return false;
	}

	return true;
}

static u8 drm_dp_downstream_port_count(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	u8 port_count = dpcd[DP_DOWN_STREAM_PORT_COUNT] & DP_PORT_COUNT_MASK;

	if (dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE && port_count > 4)
		port_count = 4;

	return port_count;
}

/* ---- receiver capabilities ----------------------------------------------------------- */

static int drm_dp_read_extended_dpcd_caps(struct drm_dp_aux *aux,
					  u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	u8 dpcd_ext[DP_RECEIVER_CAP_SIZE];
	int ret;

	/*
	 * Prior to DP1.3 the bit represented by
	 * DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT was reserved.
	 * If it is set DP_DPCD_REV at 0000h could be at a value less than
	 * the true capability of the panel. The only way to check is to
	 * then compare 0000h and 2200h.
	 */
	if (!(dpcd[DP_TRAINING_AUX_RD_INTERVAL] &
	      DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT))
		return 0;

	ret = drm_dp_dpcd_read_data(aux, DP_DP13_DPCD_REV, &dpcd_ext,
				    sizeof(dpcd_ext));
	if (ret < 0)
		return ret;

	if (dpcd[DP_DPCD_REV] > dpcd_ext[DP_DPCD_REV]) {
		dp_dbg("%s: Extended DPCD rev less than base DPCD rev (%d > %d)\n",
		       aux_name(aux), dpcd[DP_DPCD_REV], dpcd_ext[DP_DPCD_REV]);
		return 0;
	}

	if (!kmemcmp(dpcd, dpcd_ext, sizeof(dpcd_ext)))
		return 0;

	dp_dbg_hex("base DPCD", aux_name(aux), DP_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE);

	mm_memcpy(dpcd, dpcd_ext, sizeof(dpcd_ext));

	return 0;
}

int drm_dp_read_dpcd_caps(struct drm_dp_aux *aux,
			  u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	int ret;

	ret = drm_dp_dpcd_read_data(aux, DP_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE);
	if (ret < 0)
		return ret;
	if (dpcd[DP_DPCD_REV] == 0)
		return -EIO;

	ret = drm_dp_read_extended_dpcd_caps(aux, dpcd);
	if (ret < 0)
		return ret;

	dp_dbg_hex("DPCD", aux_name(aux), DP_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE);

	return ret;
}

int drm_dp_read_downstream_info(struct drm_dp_aux *aux,
				const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				u8 downstream_ports[DP_MAX_DOWNSTREAM_PORTS])
{
	int ret;
	u8 len;

	mm_memset(downstream_ports, 0, DP_MAX_DOWNSTREAM_PORTS);

	/* No downstream info to read */
	if (!drm_dp_is_branch(dpcd) || dpcd[DP_DPCD_REV] == DP_DPCD_REV_10)
		return 0;

	/* Some branches advertise having 0 downstream ports, despite also advertising they have a
	 * downstream port present. The DP spec isn't clear on if this is allowed or not, but since
	 * some branches do it we need to handle it regardless.
	 */
	len = drm_dp_downstream_port_count(dpcd);
	if (!len)
		return 0;

	if (dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE)
		len *= 4;

	ret = drm_dp_dpcd_read_data(aux, DP_DOWNSTREAM_PORT_0, downstream_ports, len);
	if (ret < 0)
		return ret;

	dp_dbg_hex("DFP", aux_name(aux), DP_DOWNSTREAM_PORT_0, downstream_ports, len);

	return 0;
}

int drm_dp_downstream_max_dotclock(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				   const u8 port_cap[4])
{
	if (!drm_dp_is_branch(dpcd))
		return 0;

	if (dpcd[DP_DPCD_REV] < 0x11)
		return 0;

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_VGA:
		if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
			return 0;
		return port_cap[1] * 8000;
	default:
		return 0;
	}
}

int drm_dp_downstream_max_tmds_clock(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				     const u8 port_cap[4],
				     const struct drm_edid *drm_edid)
{
	if (!drm_dp_is_branch(dpcd))
		return 0;

	if (dpcd[DP_DPCD_REV] < 0x11) {
		switch (dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_TYPE_MASK) {
		case DP_DWN_STRM_PORT_TYPE_TMDS:
			return 165000;
		default:
			return 0;
		}
	}

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_DP_DUALMODE:
		if (is_edid_digital_input_dp(drm_edid))
			return 0;
		/*
		 * It's left up to the driver to check the
		 * DP dual mode adapter's max TMDS clock.
		 *
		 * Unfortunately it looks like branch devices
		 * may not forward the DP dual mode i2c
		 * access, so we usually just get an i2c nak.
		 */
		fallthrough;
	case DP_DS_PORT_TYPE_HDMI:
		/*
		 * We should perhaps assume 165 MHz when detailed cap
		 * info is not available. But looks like many typical
		 * branch devices fall into that category and so we'd
		 * probably end up with users complaining that they can't
		 * get high resolution modes with their favorite dongle.
		 *
		 * So let's limit to 300 MHz instead since DPCD 1.4
		 * HDMI 2.0 DFPs are required to have the detailed cap
		 * info. So it's more likely we're dealing with a HDMI 1.4
		 * compatible* device here.
		 */
		if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
			return 300000;
		return port_cap[1] * 2500;
	case DP_DS_PORT_TYPE_DVI:
		if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
			return 165000;
		/* DVI dual link is not accounted for. */
		return port_cap[1] * 2500;
	default:
		return 0;
	}
}

int drm_dp_downstream_min_tmds_clock(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				     const u8 port_cap[4],
				     const struct drm_edid *drm_edid)
{
	if (!drm_dp_is_branch(dpcd))
		return 0;

	if (dpcd[DP_DPCD_REV] < 0x11) {
		switch (dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_TYPE_MASK) {
		case DP_DWN_STRM_PORT_TYPE_TMDS:
			return 25000;
		default:
			return 0;
		}
	}

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_DP_DUALMODE:
		if (is_edid_digital_input_dp(drm_edid))
			return 0;
		fallthrough;
	case DP_DS_PORT_TYPE_DVI:
	case DP_DS_PORT_TYPE_HDMI:
		/*
		 * Unclear whether the protocol converter could
		 * utilize pixel replication. Assume it won't.
		 */
		return 25000;
	default:
		return 0;
	}
}

int drm_dp_downstream_max_bpc(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			      const u8 port_cap[4],
			      const struct drm_edid *drm_edid)
{
	if (!drm_dp_is_branch(dpcd))
		return 0;

	if (dpcd[DP_DPCD_REV] < 0x11) {
		switch (dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_TYPE_MASK) {
		case DP_DWN_STRM_PORT_TYPE_DP:
			return 0;
		default:
			return 8;
		}
	}

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_DP:
		return 0;
	case DP_DS_PORT_TYPE_DP_DUALMODE:
		if (is_edid_digital_input_dp(drm_edid))
			return 0;
		fallthrough;
	case DP_DS_PORT_TYPE_HDMI:
	case DP_DS_PORT_TYPE_DVI:
	case DP_DS_PORT_TYPE_VGA:
		if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
			return 8;

		switch (port_cap[2] & DP_DS_MAX_BPC_MASK) {
		case DP_DS_8BPC:
			return 8;
		case DP_DS_10BPC:
			return 10;
		case DP_DS_12BPC:
			return 12;
		case DP_DS_16BPC:
			return 16;
		default:
			return 8;
		}
		break;
	default:
		return 8;
	}
}

bool drm_dp_downstream_420_passthrough(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				       const u8 port_cap[4])
{
	if (!drm_dp_is_branch(dpcd))
		return false;

	if (dpcd[DP_DPCD_REV] < 0x13)
		return false;

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_DP:
		return true;
	case DP_DS_PORT_TYPE_HDMI:
		if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
			return false;

		return !!(port_cap[3] & DP_DS_HDMI_YCBCR420_PASS_THROUGH);
	default:
		return false;
	}
}

bool drm_dp_downstream_444_to_420_conversion(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
					     const u8 port_cap[4])
{
	if (!drm_dp_is_branch(dpcd))
		return false;

	if (dpcd[DP_DPCD_REV] < 0x13)
		return false;

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_HDMI:
		if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
			return false;

		return !!(port_cap[3] & DP_DS_HDMI_YCBCR444_TO_420_CONV);
	default:
		return false;
	}
}

bool drm_dp_downstream_rgb_to_ycbcr_conversion(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
					       const u8 port_cap[4],
					       u8 color_spc)
{
	if (!drm_dp_is_branch(dpcd))
		return false;

	if (dpcd[DP_DPCD_REV] < 0x13)
		return false;

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_HDMI:
		if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
			return false;

		return !!(port_cap[3] & color_spc);
	default:
		return false;
	}
}

int drm_dp_downstream_mode(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			   const u8 port_cap[4], struct drm_display_mode *out)
{
	u8 vic;

	if (!drm_dp_is_branch(dpcd))
		return -ENOENT;

	if (dpcd[DP_DPCD_REV] < 0x11)
		return -ENOENT;

	switch (port_cap[0] & DP_DS_PORT_TYPE_MASK) {
	case DP_DS_PORT_TYPE_NON_EDID:
		switch (port_cap[0] & DP_DS_NON_EDID_MASK) {
		case DP_DS_NON_EDID_720x480i_60:
			vic = 6;
			break;
		case DP_DS_NON_EDID_720x480i_50:
			vic = 21;
			break;
		case DP_DS_NON_EDID_1920x1080i_60:
			vic = 5;
			break;
		case DP_DS_NON_EDID_1920x1080i_50:
			vic = 20;
			break;
		case DP_DS_NON_EDID_1280x720_60:
			vic = 4;
			break;
		case DP_DS_NON_EDID_1280x720_50:
			vic = 19;
			break;
		default:
			return -ENOENT;
		}
		return drm_display_mode_from_cea_vic(vic, out) ? -ENOENT : 0;
	default:
		return -ENOENT;
	}
}

int drm_dp_downstream_id(struct drm_dp_aux *aux, char id[6])
{
	return drm_dp_dpcd_read_data(aux, DP_BRANCH_ID, id, 6);
}

void drm_dp_downstream_debug(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			     const u8 port_cap[4],
			     const struct drm_edid *drm_edid,
			     struct drm_dp_aux *aux)
{
	bool detailed_cap_info = dpcd[DP_DOWNSTREAMPORT_PRESENT] &
				 DP_DETAILED_CAP_INFO_AVAILABLE;
	int clk;
	int bpc;
	char id[7];
	int len;
	u8 rev[2];
	int type = port_cap[0] & DP_DS_PORT_TYPE_MASK;
	bool branch_device = drm_dp_is_branch(dpcd);
	const char *name = aux_name(aux);

	kprintf("[drm] dp: %s: branch device present: %s\n", name,
		branch_device ? "yes" : "no");

	if (!branch_device)
		return;

	switch (type) {
	case DP_DS_PORT_TYPE_DP:
		kprintf("[drm] dp: %s:   type: DisplayPort\n", name);
		break;
	case DP_DS_PORT_TYPE_VGA:
		kprintf("[drm] dp: %s:   type: VGA\n", name);
		break;
	case DP_DS_PORT_TYPE_DVI:
		kprintf("[drm] dp: %s:   type: DVI\n", name);
		break;
	case DP_DS_PORT_TYPE_HDMI:
		kprintf("[drm] dp: %s:   type: HDMI\n", name);
		break;
	case DP_DS_PORT_TYPE_NON_EDID:
		kprintf("[drm] dp: %s:   type: others without EDID support\n", name);
		break;
	case DP_DS_PORT_TYPE_DP_DUALMODE:
		kprintf("[drm] dp: %s:   type: DP++\n", name);
		break;
	case DP_DS_PORT_TYPE_WIRELESS:
		kprintf("[drm] dp: %s:   type: Wireless\n", name);
		break;
	default:
		kprintf("[drm] dp: %s:   type: N/A\n", name);
	}

	mm_memset(id, 0, sizeof(id));
	drm_dp_downstream_id(aux, id);
	kprintf("[drm] dp: %s:   ID: %s\n", name, id);

	len = drm_dp_dpcd_read_data(aux, DP_BRANCH_HW_REV, &rev[0], 1);
	if (!len)
		kprintf("[drm] dp: %s:   HW: %d.%d\n", name,
			(rev[0] & 0xf0) >> 4, rev[0] & 0xf);

	len = drm_dp_dpcd_read_data(aux, DP_BRANCH_SW_REV, rev, 2);
	if (!len)
		kprintf("[drm] dp: %s:   SW: %d.%d\n", name, rev[0], rev[1]);

	if (detailed_cap_info) {
		clk = drm_dp_downstream_max_dotclock(dpcd, port_cap);
		if (clk > 0)
			kprintf("[drm] dp: %s:   max dot clock: %d kHz\n", name, clk);

		clk = drm_dp_downstream_max_tmds_clock(dpcd, port_cap, drm_edid);
		if (clk > 0)
			kprintf("[drm] dp: %s:   max TMDS clock: %d kHz\n", name, clk);

		clk = drm_dp_downstream_min_tmds_clock(dpcd, port_cap, drm_edid);
		if (clk > 0)
			kprintf("[drm] dp: %s:   min TMDS clock: %d kHz\n", name, clk);

		bpc = drm_dp_downstream_max_bpc(dpcd, port_cap, drm_edid);

		if (bpc > 0)
			kprintf("[drm] dp: %s:   max bpc: %d\n", name, bpc);
	}
}

enum drm_mode_subconnector
drm_dp_subconnector_type(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			 const u8 port_cap[4])
{
	int type;

	if (!drm_dp_is_branch(dpcd))
		return DRM_MODE_SUBCONNECTOR_Native;
	/* DP 1.0 approach */
	if (dpcd[DP_DPCD_REV] == DP_DPCD_REV_10) {
		type = dpcd[DP_DOWNSTREAMPORT_PRESENT] &
		       DP_DWN_STRM_PORT_TYPE_MASK;

		switch (type) {
		case DP_DWN_STRM_PORT_TYPE_TMDS:
			/* Can be HDMI or DVI-D, DVI-D is a safer option */
			return DRM_MODE_SUBCONNECTOR_DVID;
		case DP_DWN_STRM_PORT_TYPE_ANALOG:
			/* Can be VGA or DVI-A, VGA is more popular */
			return DRM_MODE_SUBCONNECTOR_VGA;
		case DP_DWN_STRM_PORT_TYPE_DP:
			return DRM_MODE_SUBCONNECTOR_DisplayPort;
		case DP_DWN_STRM_PORT_TYPE_OTHER:
		default:
			return DRM_MODE_SUBCONNECTOR_Unknown;
		}
	}
	type = port_cap[0] & DP_DS_PORT_TYPE_MASK;

	switch (type) {
	case DP_DS_PORT_TYPE_DP:
	case DP_DS_PORT_TYPE_DP_DUALMODE:
		return DRM_MODE_SUBCONNECTOR_DisplayPort;
	case DP_DS_PORT_TYPE_VGA:
		return DRM_MODE_SUBCONNECTOR_VGA;
	case DP_DS_PORT_TYPE_DVI:
		return DRM_MODE_SUBCONNECTOR_DVID;
	case DP_DS_PORT_TYPE_HDMI:
		return DRM_MODE_SUBCONNECTOR_HDMIA;
	case DP_DS_PORT_TYPE_WIRELESS:
		return DRM_MODE_SUBCONNECTOR_Wireless;
	case DP_DS_PORT_TYPE_NON_EDID:
	default:
		return DRM_MODE_SUBCONNECTOR_Unknown;
	}
}

bool drm_dp_read_sink_count_cap(struct drm_connector *connector,
				const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				const struct drm_dp_desc *desc)
{
	/* Some eDP panels don't set a valid value for the sink count */
	return connector->type != DRM_MODE_CONNECTOR_eDP &&
		dpcd[DP_DPCD_REV] >= DP_DPCD_REV_11 &&
		dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_PRESENT &&
		!drm_dp_has_quirk(desc, DP_DPCD_QUIRK_NO_SINK_COUNT);
}

int drm_dp_read_sink_count(struct drm_dp_aux *aux)
{
	u8 count;
	int ret;

	ret = drm_dp_dpcd_read_byte(aux, DP_SINK_COUNT, &count);
	if (ret < 0)
		return ret;

	return DP_GET_SINK_COUNT(count);
}

/* ---- I2C over AUX ------------------------------------------------------------------- */

static void drm_dp_i2c_msg_write_status_update(struct drm_dp_aux_msg *msg)
{
	/*
	 * In case of i2c defer or short i2c ack reply to a write,
	 * we need to switch to WRITE_STATUS_UPDATE to drain the
	 * rest of the message
	 */
	if ((msg->request & ~DP_AUX_I2C_MOT) == DP_AUX_I2C_WRITE) {
		msg->request &= DP_AUX_I2C_MOT;
		msg->request |= DP_AUX_I2C_WRITE_STATUS_UPDATE;
	}
}

#define AUX_PRECHARGE_LEN 10 /* 10 to 16 */
#define AUX_SYNC_LEN (16 + 4) /* preamble + AUX_SYNC_END */
#define AUX_STOP_LEN 4
#define AUX_CMD_LEN 4
#define AUX_ADDRESS_LEN 20
#define AUX_REPLY_PAD_LEN 4
#define AUX_LENGTH_LEN 8

/*
 * Calculate the duration of the AUX request/reply in usec. Gives the
 * "best" case estimate, ie. successful while as short as possible.
 */
static int drm_dp_aux_req_duration(const struct drm_dp_aux_msg *msg)
{
	int len = AUX_PRECHARGE_LEN + AUX_SYNC_LEN + AUX_STOP_LEN +
		AUX_CMD_LEN + AUX_ADDRESS_LEN + AUX_LENGTH_LEN;

	if ((msg->request & DP_AUX_I2C_READ) == 0)
		len += msg->size * 8;

	return len;
}

static int drm_dp_aux_reply_duration(const struct drm_dp_aux_msg *msg)
{
	int len = AUX_PRECHARGE_LEN + AUX_SYNC_LEN + AUX_STOP_LEN +
		AUX_CMD_LEN + AUX_REPLY_PAD_LEN;

	/*
	 * For read we expect what was asked. For writes there will
	 * be 0 or 1 data bytes. Assume 0 for the "best" case.
	 */
	if (msg->request & DP_AUX_I2C_READ)
		len += msg->size * 8;

	return len;
}

#define I2C_START_LEN 1
#define I2C_STOP_LEN 1
#define I2C_ADDR_LEN 9 /* ADDRESS + R/W + ACK/NACK */
#define I2C_DATA_LEN 9 /* DATA + ACK/NACK */

/*
 * Calculate the length of the i2c transfer in usec, assuming
 * the i2c bus speed is as specified. Gives the "worst"
 * case estimate, ie. successful while as long as possible.
 * Doesn't account the "MOT" bit, and instead assumes each
 * message includes a START, ADDRESS and STOP. Neither does it
 * account for additional random variables such as clock stretching.
 */
static int drm_dp_i2c_msg_duration(const struct drm_dp_aux_msg *msg,
				   int i2c_speed_khz)
{
	/* AUX bitrate is 1MHz, i2c bitrate as specified */
	return DIV_ROUND_UP((I2C_START_LEN + I2C_ADDR_LEN +
			     (int)msg->size * I2C_DATA_LEN +
			     I2C_STOP_LEN) * 1000, i2c_speed_khz);
}

/*
 * Determine how many retries should be attempted to successfully transfer
 * the specified message, based on the estimated durations of the
 * i2c and AUX transfers.
 */
static int drm_dp_i2c_retry_count(const struct drm_dp_aux_msg *msg,
				  int i2c_speed_khz)
{
	int aux_time_us = drm_dp_aux_req_duration(msg) +
		drm_dp_aux_reply_duration(msg);
	int i2c_time_us = drm_dp_i2c_msg_duration(msg, i2c_speed_khz);

	return DIV_ROUND_UP(i2c_time_us, aux_time_us + AUX_RETRY_INTERVAL);
}

/*
 * The assumed speed of the I2C bus behind the sink, in kHz (1..400).
 * 10 kHz, because some real-world devices seem to need it; the sink could
 * be asked through the DPCD instead.
 */
static int dp_aux_i2c_speed_khz = 10;

/*
 * Transfer a single I2C-over-AUX message and handle various error conditions,
 * retrying the transaction as appropriate.  It is assumed that the
 * &drm_dp_aux.transfer function does not modify anything in the msg other than the
 * reply field.
 *
 * Returns bytes transferred on success, or a negative error code on failure.
 */
static int drm_dp_i2c_do_msg(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	unsigned int retry, defer_i2c;
	int ret;
	/*
	 * DP1.2 sections 2.7.7.1.5.6.1 and 2.7.7.1.6.6.1: A DP Source device
	 * is required to retry at least seven times upon receiving AUX_DEFER
	 * before giving up the AUX transaction.
	 *
	 * We also try to account for the i2c bus speed.
	 */
	int max_retries = max(7, drm_dp_i2c_retry_count(msg, dp_aux_i2c_speed_khz));

	for (retry = 0, defer_i2c = 0; retry < ((unsigned int)max_retries + defer_i2c); retry++) {
		ret = aux->transfer(aux, msg);
		if (ret < 0) {
			if (ret == -EBUSY)
				continue;

			/*
			 * While timeouts can be errors, they're usually normal
			 * behavior (for instance, when a driver tries to
			 * communicate with a non-existent DisplayPort device).
			 * Avoid spamming the kernel log with timeout errors.
			 */
			if (ret == -ETIMEDOUT)
				dp_dbg("%s: transaction timed out\n", aux_name(aux));
			else
				dp_dbg("%s: transaction failed: %d\n", aux_name(aux), ret);
			return ret;
		}

		switch (msg->reply & DP_AUX_NATIVE_REPLY_MASK) {
		case DP_AUX_NATIVE_REPLY_ACK:
			/*
			 * For I2C-over-AUX transactions this isn't enough, we
			 * need to check for the I2C ACK reply.
			 */
			break;

		case DP_AUX_NATIVE_REPLY_NACK:
			dp_dbg("%s: native nack (result=%d, size=%zu)\n",
			       aux_name(aux), ret, msg->size);
			return -EREMOTEIO;

		case DP_AUX_NATIVE_REPLY_DEFER:
			dp_dbg("%s: native defer\n", aux_name(aux));
			/*
			 * We could check for I2C bit rate capabilities and if
			 * available adjust this interval. We could also be
			 * more careful with DP-to-legacy adapters where a
			 * long legacy cable may force very low I2C bit rates.
			 *
			 * For now just defer for long enough to hopefully be
			 * safe for all use-cases.
			 */
			dp_usleep_range(AUX_RETRY_INTERVAL, AUX_RETRY_INTERVAL + 100);
			continue;

		default:
			dp_err("%s: invalid native reply 0x%02x\n",
			       aux_name(aux), msg->reply);
			return -EREMOTEIO;
		}

		switch (msg->reply & DP_AUX_I2C_REPLY_MASK) {
		case DP_AUX_I2C_REPLY_ACK:
			/*
			 * Both native ACK and I2C ACK replies received. We
			 * can assume the transfer was successful.
			 */
			if ((size_t)ret != msg->size)
				drm_dp_i2c_msg_write_status_update(msg);
			return ret;

		case DP_AUX_I2C_REPLY_NACK:
			dp_dbg("%s: I2C nack (result=%d, size=%zu)\n",
			       aux_name(aux), ret, msg->size);
			aux->i2c_nack_count++;
			return -EREMOTEIO;

		case DP_AUX_I2C_REPLY_DEFER:
			dp_dbg("%s: I2C defer\n", aux_name(aux));
			/* DP Compliance Test 4.2.2.5 Requirement:
			 * Must have at least 7 retries for I2C defers on the
			 * transaction to pass this test
			 */
			aux->i2c_defer_count++;
			if (defer_i2c < 7)
				defer_i2c++;
			dp_usleep_range(AUX_RETRY_INTERVAL, AUX_RETRY_INTERVAL + 100);
			drm_dp_i2c_msg_write_status_update(msg);

			continue;

		default:
			dp_err("%s: invalid I2C reply 0x%02x\n",
			       aux_name(aux), msg->reply);
			return -EREMOTEIO;
		}
	}

	dp_dbg("%s: Too many retries, giving up\n", aux_name(aux));
	return -EREMOTEIO;
}

static void drm_dp_i2c_msg_set_request(struct drm_dp_aux_msg *msg,
				       const struct i2c_msg *i2c_msg)
{
	msg->request = (i2c_msg->flags & I2C_M_RD) ?
		DP_AUX_I2C_READ : DP_AUX_I2C_WRITE;
	if (!(i2c_msg->flags & I2C_M_STOP))
		msg->request |= DP_AUX_I2C_MOT;
}

/*
 * Keep retrying drm_dp_i2c_do_msg until all data has been transferred.
 *
 * Returns an error code on failure, or a recommended transfer size on success.
 */
static int drm_dp_i2c_drain_msg(struct drm_dp_aux *aux, struct drm_dp_aux_msg *orig_msg)
{
	int err, ret = (int)orig_msg->size;
	struct drm_dp_aux_msg msg = *orig_msg;

	while (msg.size > 0) {
		err = drm_dp_i2c_do_msg(aux, &msg);
		if (err <= 0)
			return err == 0 ? -EPROTO : err;

		if ((size_t)err < msg.size && err < ret) {
			dp_dbg("%s: Partial I2C reply: requested %zu bytes got %d bytes\n",
			       aux_name(aux), msg.size, err);
			ret = err;
		}

		msg.size -= (size_t)err;
		msg.buffer = (u8 *)msg.buffer + err;
	}

	return ret;
}

/*
 * Bizlink designed DP->DVI-D Dual Link adapters require the I2C over AUX
 * packets to be as large as possible. If not, the I2C transactions never
 * succeed. Hence the default is maximum (1..16 bytes per AUX message).
 */
static int dp_aux_i2c_transfer_size = DP_AUX_MAX_PAYLOAD_BYTES;

/* Called with aux->hw_mutex held. */
static int drm_dp_i2c_xfer_locked(struct drm_dp_aux *aux, struct i2c_msg *msgs,
				  int num)
{
	unsigned int i, j;
	unsigned int transfer_size;
	struct drm_dp_aux_msg msg;
	int err = 0;

	if (aux->powered_down)
		return -EBUSY;

	dp_aux_i2c_transfer_size = clamp(dp_aux_i2c_transfer_size, 1, DP_AUX_MAX_PAYLOAD_BYTES);

	mm_memset(&msg, 0, sizeof(msg));

	for (i = 0; i < (unsigned int)num; i++) {
		msg.address = msgs[i].addr;

		if (!aux->no_zero_sized) {
			drm_dp_i2c_msg_set_request(&msg, &msgs[i]);
			/* Send a bare address packet to start the transaction.
			 * Zero sized messages specify an address only (bare
			 * address) transaction.
			 */
			msg.buffer = NULL;
			msg.size = 0;
			err = drm_dp_i2c_do_msg(aux, &msg);
		}

		/*
		 * Reset msg.request in case it got changed into a
		 * WRITE_STATUS_UPDATE.
		 */
		drm_dp_i2c_msg_set_request(&msg, &msgs[i]);

		if (err < 0)
			break;
		/* We want each transaction to be as large as possible, but
		 * we'll go to smaller sizes if the hardware gives us a
		 * short reply.
		 */
		transfer_size = (unsigned int)dp_aux_i2c_transfer_size;
		for (j = 0; j < msgs[i].len; j += msg.size) {
			msg.buffer = msgs[i].buf + j;
			msg.size = min(transfer_size, msgs[i].len - j);

			if (j + msg.size == msgs[i].len && aux->no_zero_sized)
				msg.request &= ~DP_AUX_I2C_MOT;
			err = drm_dp_i2c_drain_msg(aux, &msg);

			/*
			 * Reset msg.request in case it got changed into a
			 * WRITE_STATUS_UPDATE.
			 */
			drm_dp_i2c_msg_set_request(&msg, &msgs[i]);

			if (err < 0)
				break;
			transfer_size = (unsigned int)err;
		}
		if (err < 0)
			break;
	}
	if (err >= 0)
		err = num;

	if (!aux->no_zero_sized) {
		/* Send a bare address packet to close out the transaction.
		 * Zero sized messages specify an address only (bare
		 * address) transaction.
		 */
		msg.request &= ~DP_AUX_I2C_MOT;
		msg.buffer = NULL;
		msg.size = 0;
		(void)drm_dp_i2c_do_msg(aux, &msg);
	}
	return err;
}

/* The i2c_adapter entry point: the whole message list runs under the
 * channel lock, so no native DPCD access interleaves with it. */
static int drm_dp_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs,
			   int num)
{
	struct drm_dp_aux *aux = adapter->priv;
	int ret;

	if (!aux || !aux->initialized || !aux->transfer)
		return -ENODEV;
	if (num <= 0)
		return 0;

	aux_lock(aux);
	ret = drm_dp_i2c_xfer_locked(aux, msgs, num);
	aux_unlock(aux);
	return ret;
}

/* ---- channel setup ---------------------------------------------------------------------- */

void drm_dp_aux_init(struct drm_dp_aux *aux)
{
	mm_rwsem_init(&aux->hw_mutex, "dp_aux");

	aux->ddc.xfer = drm_dp_i2c_xfer;
	aux->ddc.priv = aux;
	aux->initialized = true;
}

int drm_dp_aux_register(struct drm_dp_aux *aux)
{
	const char *name = aux_name(aux);
	size_t i;

	if (!aux->initialized)
		drm_dp_aux_init(aux);

	for (i = 0; i + 1 < sizeof(aux->ddc.name) && name[i]; i++)
		aux->ddc.name[i] = name[i];
	aux->ddc.name[i] = 0;

	return 0;
}

void drm_dp_aux_unregister(struct drm_dp_aux *aux)
{
	/* No bus to remove the adapter from: just stop it working, so a
	 * stale pointer to it fails cleanly instead of reaching a channel
	 * whose driver is gone. */
	aux->ddc.xfer = NULL;
}

/* ---- PSR -------------------------------------------------------------------------------- */

#define PSR_SETUP_TIME(x) [DP_PSR_SETUP_TIME_ ## x >> DP_PSR_SETUP_TIME_SHIFT] = (x)

int drm_dp_psr_setup_time(const u8 psr_cap[EDP_PSR_RECEIVER_CAP_SIZE])
{
	static const u16 psr_setup_time_us[] = {
		PSR_SETUP_TIME(330),
		PSR_SETUP_TIME(275),
		PSR_SETUP_TIME(220),
		PSR_SETUP_TIME(165),
		PSR_SETUP_TIME(110),
		PSR_SETUP_TIME(55),
		PSR_SETUP_TIME(0),
	};
	size_t i;

	i = (psr_cap[1] & DP_PSR_SETUP_TIME_MASK) >> DP_PSR_SETUP_TIME_SHIFT;
	if (i >= ARRAY_SIZE(psr_setup_time_us))
		return -EINVAL;

	return psr_setup_time_us[i];
}

#undef PSR_SETUP_TIME

/* ---- identification and quirks ----------------------------------------------------------- */

struct dpcd_quirk {
	u8 oui[3];
	u8 device_id[6];
	bool is_branch;
	u32 quirks;
};

#define OUI(first, second, third) { (first), (second), (third) }
#define DEVICE_ID(first, second, third, fourth, fifth, sixth) \
	{ (first), (second), (third), (fourth), (fifth), (sixth) }

#define DEVICE_ID_ANY	DEVICE_ID(0, 0, 0, 0, 0, 0)

static const struct dpcd_quirk dpcd_quirk_list[] = {
	/* Analogix 7737 needs reduced M and N at HBR2 link rates */
	{ OUI(0x00, 0x22, 0xb9), DEVICE_ID_ANY, true, BIT(DP_DPCD_QUIRK_CONSTANT_N) },
	/* LG LP140WF6-SPM1 eDP panel */
	{ OUI(0x00, 0x22, 0xb9), DEVICE_ID('s', 'i', 'v', 'a', 'r', 'T'), false, BIT(DP_DPCD_QUIRK_CONSTANT_N) },
	/* Apple panels need some additional handling to support PSR */
	{ OUI(0x00, 0x10, 0xfa), DEVICE_ID_ANY, false, BIT(DP_DPCD_QUIRK_NO_PSR) },
	/* CH7511 seems to leave SINK_COUNT zeroed */
	{ OUI(0x00, 0x00, 0x00), DEVICE_ID('C', 'H', '7', '5', '1', '1'), false, BIT(DP_DPCD_QUIRK_NO_SINK_COUNT) },
	/* Synaptics DP1.4 MST hubs can support DSC without virtual DPCD */
	{ OUI(0x90, 0xCC, 0x24), DEVICE_ID_ANY, true, BIT(DP_DPCD_QUIRK_DSC_WITHOUT_VIRTUAL_DPCD) },
	/* Realtek DP1.4 MST hubs can support DSC without virtual DPCD */
	{ OUI(0x00, 0xe0, 0x4c), DEVICE_ID('D', 'p', '1', '.', '4', 0), true, BIT(DP_DPCD_QUIRK_DSC_WITHOUT_VIRTUAL_DPCD) },
	/* Synaptics DP1.4 MST hubs require DSC for some modes on which it applies HBLANK expansion. */
	{ OUI(0x90, 0xCC, 0x24), DEVICE_ID_ANY, true, BIT(DP_DPCD_QUIRK_HBLANK_EXPANSION_REQUIRES_DSC) },
	/* MediaTek panels (at least in U3224KBA) require DSC for modes with a short HBLANK on UHBR links. */
	{ OUI(0x00, 0x0C, 0xE7), DEVICE_ID_ANY, false, BIT(DP_DPCD_QUIRK_HBLANK_EXPANSION_REQUIRES_DSC) },
	/* Apple MacBookPro 2017 15 inch eDP Retina panel reports too low DP_MAX_LINK_RATE */
	{ OUI(0x00, 0x10, 0xfa), DEVICE_ID(101, 68, 21, 101, 98, 97), false, BIT(DP_DPCD_QUIRK_CAN_DO_MAX_LINK_RATE_3_24_GBPS) },
	/* Synaptics Panamera supports only a compressed bpp of 12 above 50% of its max DSC pixel throughput */
	{ OUI(0x90, 0xCC, 0x24), DEVICE_ID('S', 'Y', 'N', 'A', 0x53, 0x22), true, BIT(DP_DPCD_QUIRK_DSC_THROUGHPUT_BPP_LIMIT) },
	{ OUI(0x90, 0xCC, 0x24), DEVICE_ID('S', 'Y', 'N', 'A', 0x53, 0x31), true, BIT(DP_DPCD_QUIRK_DSC_THROUGHPUT_BPP_LIMIT) },
	{ OUI(0x90, 0xCC, 0x24), DEVICE_ID('S', 'Y', 'N', 'A', 0x53, 0x33), true, BIT(DP_DPCD_QUIRK_DSC_THROUGHPUT_BPP_LIMIT) },
};

#undef OUI

/*
 * The DPCD quirks of the sink or branch device `ident' names: matched on
 * the OUI and, unless the entry takes any device, the device id string.
 * The table is shared; each driver acts on the quirks it cares about.
 */
static u32
drm_dp_get_quirks(const struct drm_dp_dpcd_ident *ident, bool is_branch)
{
	const struct dpcd_quirk *quirk;
	u32 quirks = 0;
	size_t i;
	u8 any_device[] = DEVICE_ID_ANY;

	for (i = 0; i < ARRAY_SIZE(dpcd_quirk_list); i++) {
		quirk = &dpcd_quirk_list[i];

		if (quirk->is_branch != is_branch)
			continue;

		if (kmemcmp(quirk->oui, ident->oui, sizeof(ident->oui)) != 0)
			continue;

		if (kmemcmp(quirk->device_id, any_device, sizeof(any_device)) != 0 &&
		    kmemcmp(quirk->device_id, ident->device_id, sizeof(ident->device_id)) != 0)
			continue;

		quirks |= quirk->quirks;
	}

	return quirks;
}

#undef DEVICE_ID_ANY
#undef DEVICE_ID

static int drm_dp_read_ident(struct drm_dp_aux *aux, unsigned int offset,
			     struct drm_dp_dpcd_ident *ident)
{
	return drm_dp_dpcd_read_data(aux, offset, ident, sizeof(*ident));
}

static void drm_dp_dump_desc(struct drm_dp_aux *aux,
			     const char *device_name, const struct drm_dp_desc *desc)
{
	const struct drm_dp_dpcd_ident *ident = &desc->ident;
	char id[sizeof(ident->device_id) + 1];
	size_t i;

	if (!DRM_DP_DEBUG)
		return;
	/* The id is ASCII padded with NULs; show anything else as '.'. */
	for (i = 0; i < sizeof(ident->device_id) && ident->device_id[i]; i++)
		id[i] = (ident->device_id[i] >= 0x20 && ident->device_id[i] < 0x7f) ?
			(char)ident->device_id[i] : '.';
	id[i] = 0;
	dp_dbg("%s: %s: OUI %02x%02x%02x dev-ID %s HW-rev %d.%d SW-rev %d.%d quirks 0x%04x\n",
	       aux_name(aux), device_name,
	       ident->oui[0], ident->oui[1], ident->oui[2], id,
	       ident->hw_rev >> 4, ident->hw_rev & 0xf,
	       ident->sw_major_rev, ident->sw_minor_rev,
	       desc->quirks);
}

int drm_dp_read_desc(struct drm_dp_aux *aux, struct drm_dp_desc *desc,
		     bool is_branch)
{
	struct drm_dp_dpcd_ident *ident = &desc->ident;
	unsigned int offset = is_branch ? DP_BRANCH_OUI : DP_SINK_OUI;
	int ret;

	ret = drm_dp_read_ident(aux, offset, ident);
	if (ret < 0)
		return ret;

	desc->quirks = drm_dp_get_quirks(ident, is_branch);

	drm_dp_dump_desc(aux, is_branch ? "DP branch" : "DP sink", desc);

	return 0;
}

int drm_dp_dump_lttpr_desc(struct drm_dp_aux *aux, enum drm_dp_phy dp_phy)
{
	struct drm_dp_desc desc;
	int ret;

	if (WARN_ON(dp_phy < DP_PHY_LTTPR1 || dp_phy > DP_MAX_LTTPR_COUNT))
		return -EINVAL;

	mm_memset(&desc, 0, sizeof(desc));
	ret = drm_dp_read_ident(aux, DP_OUI_PHY_REPEATER(dp_phy), &desc.ident);
	if (ret < 0)
		return ret;

	drm_dp_dump_desc(aux, drm_dp_phy_name(dp_phy), &desc);

	return 0;
}

/* ---- DSC ------------------------------------------------------------------------------------ */

u8 drm_dp_dsc_sink_bpp_incr(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	u8 bpp_increment_dpcd = dsc_dpcd[DP_DSC_BITS_PER_PIXEL_INC - DP_DSC_SUPPORT];

	switch (bpp_increment_dpcd & DP_DSC_BITS_PER_PIXEL_MASK) {
	case DP_DSC_BITS_PER_PIXEL_1_16:
		return 16;
	case DP_DSC_BITS_PER_PIXEL_1_8:
		return 8;
	case DP_DSC_BITS_PER_PIXEL_1_4:
		return 4;
	case DP_DSC_BITS_PER_PIXEL_1_2:
		return 2;
	case DP_DSC_BITS_PER_PIXEL_1_1:
		return 1;
	}

	return 0;
}

u32 drm_dp_dsc_slice_count_to_mask(int slice_count)
{
	return BIT(slice_count - 1);
}

u32 drm_dp_dsc_sink_slice_count_mask(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
				     bool is_edp)
{
	u8 slice_cap1 = dsc_dpcd[DP_DSC_SLICE_CAP_1 - DP_DSC_SUPPORT];
	u32 mask = 0;

	if (!is_edp) {
		/* For DP, use values from DSC_SLICE_CAP_1 and DSC_SLICE_CAP2 */
		u8 slice_cap2 = dsc_dpcd[DP_DSC_SLICE_CAP_2 - DP_DSC_SUPPORT];

		if (slice_cap2 & DP_DSC_24_PER_DP_DSC_SINK)
			mask |= drm_dp_dsc_slice_count_to_mask(24);
		if (slice_cap2 & DP_DSC_20_PER_DP_DSC_SINK)
			mask |= drm_dp_dsc_slice_count_to_mask(20);
		if (slice_cap2 & DP_DSC_16_PER_DP_DSC_SINK)
			mask |= drm_dp_dsc_slice_count_to_mask(16);
	}

	/* DP, eDP v1.5+ */
	if (slice_cap1 & DP_DSC_12_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(12);
	if (slice_cap1 & DP_DSC_10_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(10);
	if (slice_cap1 & DP_DSC_8_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(8);
	if (slice_cap1 & DP_DSC_6_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(6);
	/* DP, eDP v1.4+ */
	if (slice_cap1 & DP_DSC_4_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(4);
	if (slice_cap1 & DP_DSC_2_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(2);
	if (slice_cap1 & DP_DSC_1_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(1);

	return mask;
}

/* The largest slice count the sink supports, 0 for none: what the driver
 * puts into its DSC configuration (and the PPS it sends). */
u8 drm_dp_dsc_sink_max_slice_count(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
				   bool is_edp)
{
	return (u8)gpu_fls(drm_dp_dsc_sink_slice_count_mask(dsc_dpcd, is_edp));
}

/* Bits of precision in the decoder's line buffer, 0 when invalid. */
u8 drm_dp_dsc_sink_line_buf_depth(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	u8 line_buf_depth = dsc_dpcd[DP_DSC_LINE_BUF_BIT_DEPTH - DP_DSC_SUPPORT];

	switch (line_buf_depth & DP_DSC_LINE_BUF_BIT_DEPTH_MASK) {
	case DP_DSC_LINE_BUF_BIT_DEPTH_9:
		return 9;
	case DP_DSC_LINE_BUF_BIT_DEPTH_10:
		return 10;
	case DP_DSC_LINE_BUF_BIT_DEPTH_11:
		return 11;
	case DP_DSC_LINE_BUF_BIT_DEPTH_12:
		return 12;
	case DP_DSC_LINE_BUF_BIT_DEPTH_13:
		return 13;
	case DP_DSC_LINE_BUF_BIT_DEPTH_14:
		return 14;
	case DP_DSC_LINE_BUF_BIT_DEPTH_15:
		return 15;
	case DP_DSC_LINE_BUF_BIT_DEPTH_16:
		return 16;
	case DP_DSC_LINE_BUF_BIT_DEPTH_8:
		return 8;
	}

	return 0;
}

int drm_dp_dsc_sink_supported_input_bpcs(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
					 u8 dsc_bpc[3])
{
	int num_bpc = 0;
	u8 color_depth = dsc_dpcd[DP_DSC_DEC_COLOR_DEPTH_CAP - DP_DSC_SUPPORT];

	if (!drm_dp_sink_supports_dsc(dsc_dpcd))
		return 0;

	if (color_depth & DP_DSC_12_BPC)
		dsc_bpc[num_bpc++] = 12;
	if (color_depth & DP_DSC_10_BPC)
		dsc_bpc[num_bpc++] = 10;

	/* A DP DSC Sink device shall support 8 bpc. */
	dsc_bpc[num_bpc++] = 8;

	return num_bpc;
}

/*
 * See DP Standard v2.1a 2.8.4 Minimum Slices/Display, Table 2-159 and
 * Appendix L.1 Derivation of Slice Count Requirements.
 */
static int dsc_sink_min_slice_throughput(int peak_pixel_rate)
{
	if (peak_pixel_rate >= 4800000)
		return 600000;
	else if (peak_pixel_rate >= 2700000)
		return 400000;
	else
		return 340000;
}

/*
 * The sink's maximum pixel throughput per slice in kPixels/s, from its DSC
 * caps, the cumulative peak pixel rate (kHz) of everything it decodes and
 * whether the format is RGB/YUV444 (else 4:2:2/4:2:0).  For an MST tiled
 * display, peak_pixel_rate is the tile rate times the tile count.
 */
int drm_dp_dsc_sink_max_slice_throughput(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
					 int peak_pixel_rate, bool is_rgb_yuv444)
{
	int throughput;
	int delta = 0;
	int base;

	throughput = dsc_dpcd[DP_DSC_PEAK_THROUGHPUT - DP_DSC_SUPPORT];

	if (is_rgb_yuv444) {
		throughput = (throughput & DP_DSC_THROUGHPUT_MODE_0_MASK) >>
			     DP_DSC_THROUGHPUT_MODE_0_SHIFT;

		delta = ((dsc_dpcd[DP_DSC_RC_BUF_BLK_SIZE - DP_DSC_SUPPORT]) &
			 DP_DSC_THROUGHPUT_MODE_0_DELTA_MASK) >>
			DP_DSC_THROUGHPUT_MODE_0_DELTA_SHIFT;	/* in units of 2 MPixels/sec */
		delta *= 2000;
	} else {
		throughput = (throughput & DP_DSC_THROUGHPUT_MODE_1_MASK) >>
			     DP_DSC_THROUGHPUT_MODE_1_SHIFT;
	}

	switch (throughput) {
	case 0:
		return dsc_sink_min_slice_throughput(peak_pixel_rate);
	case 1:
		base = 340000;
		break;
	case 2 ... 14:
		base = 400000 + 50000 * (throughput - 2);
		break;
	case 15:
	default:
		base = 170000;
		break;
	}

	return base + delta;
}

static u8 dsc_branch_dpcd_cap(const u8 dpcd[DP_DSC_BRANCH_CAP_SIZE], int reg)
{
	return dpcd[reg - DP_DSC_BRANCH_OVERALL_THROUGHPUT_0];
}

/* 0 when the branch does not state an overall limit (use the per-slice
 * limit times the slice count), else kPixels/s. */
int drm_dp_dsc_branch_max_overall_throughput(const u8 dsc_branch_dpcd[DP_DSC_BRANCH_CAP_SIZE],
					     bool is_rgb_yuv444)
{
	int throughput;

	if (is_rgb_yuv444)
		throughput = dsc_branch_dpcd_cap(dsc_branch_dpcd,
						 DP_DSC_BRANCH_OVERALL_THROUGHPUT_0);
	else
		throughput = dsc_branch_dpcd_cap(dsc_branch_dpcd,
						 DP_DSC_BRANCH_OVERALL_THROUGHPUT_1);

	switch (throughput) {
	case 0:
		return 0;
	case 1:
		return 680000;
	default:
		return 600000 + 50000 * throughput;
	}
}

/* 0 when not stated, -EINVAL for an invalid value (< 5120), else pixels. */
int drm_dp_dsc_branch_max_line_width(const u8 dsc_branch_dpcd[DP_DSC_BRANCH_CAP_SIZE])
{
	int line_width = dsc_branch_dpcd_cap(dsc_branch_dpcd, DP_DSC_BRANCH_MAX_LINE_WIDTH);

	switch (line_width) {
	case 0:
		return 0;
	case 1 ... 15:
		return -EINVAL;
	default:
		return line_width * 320;
	}
}

/* ---- LTTPRs ---------------------------------------------------------------------------------- */

static int drm_dp_read_lttpr_regs(struct drm_dp_aux *aux,
				  const u8 dpcd[DP_RECEIVER_CAP_SIZE], int address,
				  u8 *buf, int buf_size)
{
	/*
	 * At least the DELL P2715Q monitor with a DPCD_REV < 0x14 returns
	 * corrupted values when reading from the 0xF0000- range with a block
	 * size bigger than 1.
	 */
	int block_size = dpcd[DP_DPCD_REV] < 0x14 ? 1 : buf_size;
	int offset;
	int ret;

	for (offset = 0; offset < buf_size; offset += block_size) {
		ret = drm_dp_dpcd_read_data(aux,
					    address + offset,
					    &buf[offset], block_size);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int drm_dp_read_lttpr_common_caps(struct drm_dp_aux *aux,
				  const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				  u8 caps[DP_LTTPR_COMMON_CAP_SIZE])
{
	return drm_dp_read_lttpr_regs(aux, dpcd,
				      DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV,
				      caps, DP_LTTPR_COMMON_CAP_SIZE);
}

int drm_dp_read_lttpr_phy_caps(struct drm_dp_aux *aux,
			       const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			       enum drm_dp_phy dp_phy,
			       u8 caps[DP_LTTPR_PHY_CAP_SIZE])
{
	return drm_dp_read_lttpr_regs(aux, dpcd,
				      DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER(dp_phy),
				      caps, DP_LTTPR_PHY_CAP_SIZE);
}

static u8 dp_lttpr_common_cap(const u8 caps[DP_LTTPR_COMMON_CAP_SIZE], int r)
{
	return caps[r - DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV];
}

/* The repeater count field is one-hot: 0x80 = 1 repeater .. 0x01 = 8. */
int drm_dp_lttpr_count(const u8 caps[DP_LTTPR_COMMON_CAP_SIZE])
{
	u8 count = dp_lttpr_common_cap(caps, DP_PHY_REPEATER_CNT);

	switch (gpu_hweight32(count)) {
	case 0:
		return 0;
	case 1:
		return 8 - (gpu_fls(count) - 1);
	case 8:
		return -ERANGE;
	default:
		return -EINVAL;
	}
}

int drm_dp_lttpr_max_link_rate(const u8 caps[DP_LTTPR_COMMON_CAP_SIZE])
{
	u8 rate = dp_lttpr_common_cap(caps, DP_MAX_LINK_RATE_PHY_REPEATER);

	return drm_dp_bw_code_to_link_rate(rate);
}

int drm_dp_lttpr_set_transparent_mode(struct drm_dp_aux *aux, bool enable)
{
	u8 val = enable ? DP_PHY_REPEATER_MODE_TRANSPARENT :
			  DP_PHY_REPEATER_MODE_NON_TRANSPARENT;
	int ret = drm_dp_dpcd_writeb(aux, DP_PHY_REPEATER_MODE, val);

	if (ret < 0)
		return ret;

	return (ret == 1) ? 0 : -EIO;
}

/*
 * Put the repeaters into the mode the DP standard asks for: explicitly
 * transparent first (v2.0 3.6.6.1), then non-transparent for a valid
 * count, rolled back to transparent if that fails.  A negative count (an
 * invalid capability) leaves them transparent and returns -ENODEV.
 */
int drm_dp_lttpr_init(struct drm_dp_aux *aux, int lttpr_count)
{
	int ret;

	if (!lttpr_count)
		return 0;

	ret = drm_dp_lttpr_set_transparent_mode(aux, true);
	if (ret)
		return ret;

	if (lttpr_count < 0)
		return -ENODEV;

	if (drm_dp_lttpr_set_transparent_mode(aux, false)) {
		/*
		 * Roll-back to transparent mode if setting non-transparent
		 * mode has failed
		 */
		drm_dp_lttpr_set_transparent_mode(aux, true);
		return -EINVAL;
	}

	return 0;
}

int drm_dp_lttpr_max_lane_count(const u8 caps[DP_LTTPR_COMMON_CAP_SIZE])
{
	u8 max_lanes = dp_lttpr_common_cap(caps, DP_MAX_LANE_COUNT_PHY_REPEATER);

	return max_lanes & DP_MAX_LANE_COUNT_MASK;
}

bool
drm_dp_lttpr_voltage_swing_level_3_supported(const u8 caps[DP_LTTPR_PHY_CAP_SIZE])
{
	u8 txcap = dp_lttpr_phy_cap(caps, DP_TRANSMITTER_CAPABILITY_PHY_REPEATER1);

	return !!(txcap & DP_VOLTAGE_SWING_LEVEL_3_SUPPORTED);
}

bool
drm_dp_lttpr_pre_emphasis_level_3_supported(const u8 caps[DP_LTTPR_PHY_CAP_SIZE])
{
	u8 txcap = dp_lttpr_phy_cap(caps, DP_TRANSMITTER_CAPABILITY_PHY_REPEATER1);

	return !!(txcap & DP_PRE_EMPHASIS_LEVEL_3_SUPPORTED);
}

/* ---- PHY compliance --------------------------------------------------------------------------- */

int drm_dp_get_phy_test_pattern(struct drm_dp_aux *aux,
				struct drm_dp_phy_test_params *data)
{
	int err;
	u8 rate, lanes;

	err = drm_dp_dpcd_read_byte(aux, DP_TEST_LINK_RATE, &rate);
	if (err < 0)
		return err;
	data->link_rate = drm_dp_bw_code_to_link_rate(rate);

	err = drm_dp_dpcd_read_byte(aux, DP_TEST_LANE_COUNT, &lanes);
	if (err < 0)
		return err;
	data->num_lanes = lanes & DP_MAX_LANE_COUNT_MASK;

	if (lanes & DP_ENHANCED_FRAME_CAP)
		data->enhanced_frame_cap = true;

	err = drm_dp_dpcd_read_byte(aux, DP_PHY_TEST_PATTERN, &data->phy_pattern);
	if (err < 0)
		return err;

	switch (data->phy_pattern) {
	case DP_PHY_TEST_PATTERN_80BIT_CUSTOM:
		err = drm_dp_dpcd_read_data(aux, DP_TEST_80BIT_CUSTOM_PATTERN_7_0,
					    &data->custom80, sizeof(data->custom80));
		if (err < 0)
			return err;

		break;
	case DP_PHY_TEST_PATTERN_CP2520:
		err = drm_dp_dpcd_read_data(aux, DP_TEST_HBR2_SCRAMBLER_RESET,
					    &data->hbr2_reset,
					    sizeof(data->hbr2_reset));
		if (err < 0)
			return err;
	}

	return 0;
}

int drm_dp_set_phy_test_pattern(struct drm_dp_aux *aux,
				struct drm_dp_phy_test_params *data, u8 dp_rev)
{
	int err, i;
	u8 test_pattern;

	test_pattern = data->phy_pattern;
	if (dp_rev < 0x12) {
		test_pattern = (test_pattern << 2) &
			       DP_LINK_QUAL_PATTERN_11_MASK;
		err = drm_dp_dpcd_write_byte(aux, DP_TRAINING_PATTERN_SET,
					     test_pattern);
		if (err < 0)
			return err;
	} else {
		for (i = 0; i < data->num_lanes; i++) {
			err = drm_dp_dpcd_write_byte(aux,
						     DP_LINK_QUAL_LANE0_SET + i,
						     test_pattern);
			if (err < 0)
				return err;
		}
	}

	return 0;
}

/* ---- secondary-data packets ------------------------------------------------------------------- */

static const char *dp_pixelformat_get_name(enum dp_pixelformat pixelformat)
{
	if ((int)pixelformat < 0 || pixelformat > DP_PIXELFORMAT_RESERVED)
		return "Invalid";

	switch (pixelformat) {
	case DP_PIXELFORMAT_RGB:
		return "RGB";
	case DP_PIXELFORMAT_YUV444:
		return "YUV444";
	case DP_PIXELFORMAT_YUV422:
		return "YUV422";
	case DP_PIXELFORMAT_YUV420:
		return "YUV420";
	case DP_PIXELFORMAT_Y_ONLY:
		return "Y_ONLY";
	case DP_PIXELFORMAT_RAW:
		return "RAW";
	default:
		return "Reserved";
	}
}

static const char *dp_colorimetry_get_name(enum dp_pixelformat pixelformat,
					   enum dp_colorimetry colorimetry)
{
	if ((int)pixelformat < 0 || pixelformat > DP_PIXELFORMAT_RESERVED)
		return "Invalid";

	switch (colorimetry) {
	case DP_COLORIMETRY_DEFAULT:
		switch (pixelformat) {
		case DP_PIXELFORMAT_RGB:
			return "sRGB";
		case DP_PIXELFORMAT_YUV444:
		case DP_PIXELFORMAT_YUV422:
		case DP_PIXELFORMAT_YUV420:
			return "BT.601";
		case DP_PIXELFORMAT_Y_ONLY:
			return "DICOM PS3.14";
		case DP_PIXELFORMAT_RAW:
			return "Custom Color Profile";
		default:
			return "Reserved";
		}
	case DP_COLORIMETRY_RGB_WIDE_FIXED: /* and DP_COLORIMETRY_BT709_YCC */
		switch (pixelformat) {
		case DP_PIXELFORMAT_RGB:
			return "Wide Fixed";
		case DP_PIXELFORMAT_YUV444:
		case DP_PIXELFORMAT_YUV422:
		case DP_PIXELFORMAT_YUV420:
			return "BT.709";
		default:
			return "Reserved";
		}
	case DP_COLORIMETRY_RGB_WIDE_FLOAT: /* and DP_COLORIMETRY_XVYCC_601 */
		switch (pixelformat) {
		case DP_PIXELFORMAT_RGB:
			return "Wide Float";
		case DP_PIXELFORMAT_YUV444:
		case DP_PIXELFORMAT_YUV422:
		case DP_PIXELFORMAT_YUV420:
			return "xvYCC 601";
		default:
			return "Reserved";
		}
	case DP_COLORIMETRY_OPRGB: /* and DP_COLORIMETRY_XVYCC_709 */
		switch (pixelformat) {
		case DP_PIXELFORMAT_RGB:
			return "OpRGB";
		case DP_PIXELFORMAT_YUV444:
		case DP_PIXELFORMAT_YUV422:
		case DP_PIXELFORMAT_YUV420:
			return "xvYCC 709";
		default:
			return "Reserved";
		}
	case DP_COLORIMETRY_DCI_P3_RGB: /* and DP_COLORIMETRY_SYCC_601 */
		switch (pixelformat) {
		case DP_PIXELFORMAT_RGB:
			return "DCI-P3";
		case DP_PIXELFORMAT_YUV444:
		case DP_PIXELFORMAT_YUV422:
		case DP_PIXELFORMAT_YUV420:
			return "sYCC 601";
		default:
			return "Reserved";
		}
	case DP_COLORIMETRY_RGB_CUSTOM: /* and DP_COLORIMETRY_OPYCC_601 */
		switch (pixelformat) {
		case DP_PIXELFORMAT_RGB:
			return "Custom Profile";
		case DP_PIXELFORMAT_YUV444:
		case DP_PIXELFORMAT_YUV422:
		case DP_PIXELFORMAT_YUV420:
			return "OpYCC 601";
		default:
			return "Reserved";
		}
	case DP_COLORIMETRY_BT2020_RGB: /* and DP_COLORIMETRY_BT2020_CYCC */
		switch (pixelformat) {
		case DP_PIXELFORMAT_RGB:
			return "BT.2020 RGB";
		case DP_PIXELFORMAT_YUV444:
		case DP_PIXELFORMAT_YUV422:
		case DP_PIXELFORMAT_YUV420:
			return "BT.2020 CYCC";
		default:
			return "Reserved";
		}
	case DP_COLORIMETRY_BT2020_YCC:
		switch (pixelformat) {
		case DP_PIXELFORMAT_YUV444:
		case DP_PIXELFORMAT_YUV422:
		case DP_PIXELFORMAT_YUV420:
			return "BT.2020 YCC";
		default:
			return "Reserved";
		}
	default:
		return "Invalid";
	}
}

static const char *dp_dynamic_range_get_name(enum dp_dynamic_range dynamic_range)
{
	switch (dynamic_range) {
	case DP_DYNAMIC_RANGE_VESA:
		return "VESA range";
	case DP_DYNAMIC_RANGE_CTA:
		return "CTA range";
	default:
		return "Invalid";
	}
}

static const char *dp_content_type_get_name(enum dp_content_type content_type)
{
	switch (content_type) {
	case DP_CONTENT_TYPE_NOT_DEFINED:
		return "Not defined";
	case DP_CONTENT_TYPE_GRAPHICS:
		return "Graphics";
	case DP_CONTENT_TYPE_PHOTO:
		return "Photo";
	case DP_CONTENT_TYPE_VIDEO:
		return "Video";
	case DP_CONTENT_TYPE_GAME:
		return "Game";
	default:
		return "Reserved";
	}
}

static const char *dp_sdp_type_get_name(unsigned char type)
{
	switch (type) {
	case DP_SDP_AUDIO_TIMESTAMP:
		return "Audio_TimeStamp";
	case DP_SDP_AUDIO_STREAM:
		return "Audio_Stream";
	case DP_SDP_EXTENSION:
		return "Extension";
	case DP_SDP_AUDIO_COPYMANAGEMENT:
		return "Audio_CopyManagement";
	case DP_SDP_ISRC:
		return "ISRC";
	case DP_SDP_VSC:
		return "VSC";
	case DP_SDP_PPS:
		return "PPS";
	case DP_SDP_VSC_EXT_VESA:
		return "VSC_EXT_VESA";
	case DP_SDP_VSC_EXT_CEA:
		return "VSC_EXT_CEA";
	case DP_SDP_ADAPTIVE_SYNC:
		return "Adaptive-Sync";
	default:
		return "Unknown";
	}
}

void drm_dp_vsc_sdp_log(const char *prefix, const struct drm_dp_vsc_sdp *vsc)
{
	const char *p = prefix ? prefix : "[drm] dp";

	kprintf("%s: DP SDP: %s, revision %u, length %u\n", p,
		dp_sdp_type_get_name(vsc->sdp_type), vsc->revision, vsc->length);
	kprintf("%s:   pixelformat: %s\n", p,
		dp_pixelformat_get_name(vsc->pixelformat));
	kprintf("%s:   colorimetry: %s\n", p,
		dp_colorimetry_get_name(vsc->pixelformat, vsc->colorimetry));
	kprintf("%s:   bpc: %u\n", p, vsc->bpc);
	kprintf("%s:   dynamic range: %s\n", p,
		dp_dynamic_range_get_name(vsc->dynamic_range));
	kprintf("%s:   content type: %s\n", p,
		dp_content_type_get_name(vsc->content_type));
}

void drm_dp_as_sdp_log(const char *prefix, const struct drm_dp_as_sdp *as_sdp)
{
	const char *p = prefix ? prefix : "[drm] dp";

	kprintf("%s: DP SDP: %s, revision %u, length %u\n", p,
		dp_sdp_type_get_name(as_sdp->sdp_type), as_sdp->revision, as_sdp->length);
	kprintf("%s:   vtotal: %d\n", p, as_sdp->vtotal);
	kprintf("%s:   target rr: %d\n", p, as_sdp->target_rr);
	kprintf("%s:   duration increase ms: %d\n", p, as_sdp->duration_incr_ms);
	kprintf("%s:   duration decrease ms: %d\n", p, as_sdp->duration_decr_ms);
	kprintf("%s:   operation mode: %d\n", p, as_sdp->mode);
	kprintf("%s:   target rr divider: %s\n", p,
		as_sdp->target_rr_divider ? "1.001" : "1.000");
	kprintf("%s:   coasting vtotal: %d\n", p, as_sdp->coasting_vtotal);
}

bool drm_dp_as_sdp_supported(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	u8 rx_feature;

	if (dpcd[DP_DPCD_REV] < DP_DPCD_REV_13)
		return false;

	if (drm_dp_dpcd_read_byte(aux, DP_DPRX_FEATURE_ENUMERATION_LIST_CONT_1,
				  &rx_feature) < 0) {
		dp_dbg("Failed to read DP_DPRX_FEATURE_ENUMERATION_LIST_CONT_1\n");
		return false;
	}

	return !!(rx_feature & DP_ADAPTIVE_SYNC_SDP_SUPPORTED);
}

bool drm_dp_vsc_sdp_supported(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	u8 rx_feature;

	if (dpcd[DP_DPCD_REV] < DP_DPCD_REV_13)
		return false;

	if (drm_dp_dpcd_read_byte(aux, DP_DPRX_FEATURE_ENUMERATION_LIST, &rx_feature) < 0) {
		dp_dbg("failed to read DP_DPRX_FEATURE_ENUMERATION_LIST\n");
		return false;
	}

	return !!(rx_feature & DP_VSC_SDP_EXT_FOR_COLORIMETRY_SUPPORTED);
}

/*
 * Pack a VSC SDP (initialised as DP 1.4a Tables 2-118..2-120 describe)
 * into the generic layout: header bytes, then DB16..DB18 (pixel encoding,
 * colorimetry, depth, range, content type) for revisions 5 and 7.
 */
ssize_t drm_dp_vsc_sdp_pack(const struct drm_dp_vsc_sdp *vsc,
			    struct dp_sdp *sdp)
{
	size_t length = sizeof(struct dp_sdp);

	mm_memset(sdp, 0, sizeof(struct dp_sdp));

	/*
	 * Prepare VSC Header for SU as per DP 1.4a spec, Table 2-119
	 * VSC SDP Header Bytes
	 */
	sdp->sdp_header.HB0 = 0; /* Secondary-Data Packet ID = 0 */
	sdp->sdp_header.HB1 = vsc->sdp_type; /* Secondary-data Packet Type */
	sdp->sdp_header.HB2 = vsc->revision; /* Revision Number */
	sdp->sdp_header.HB3 = vsc->length; /* Number of Valid Data Bytes */

	if (vsc->revision == 0x6) {
		sdp->db[0] = 1;
		sdp->db[3] = 1;
	}

	/*
	 * Revision 0x5 and revision 0x7 supports Pixel Encoding/Colorimetry
	 * Format as per DP 1.4a spec and DP 2.0 respectively.
	 */
	if (!(vsc->revision == 0x5 || vsc->revision == 0x7))
		goto out;

	/* VSC SDP Payload for DB16 through DB18 */
	/* Pixel Encoding and Colorimetry Formats  */
	sdp->db[16] = (vsc->pixelformat & 0xf) << 4; /* DB16[7:4] */
	sdp->db[16] |= vsc->colorimetry & 0xf; /* DB16[3:0] */

	switch (vsc->bpc) {
	case 6:
		/* 6bpc: 0x0 */
		break;
	case 8:
		sdp->db[17] = 0x1; /* DB17[3:0] */
		break;
	case 10:
		sdp->db[17] = 0x2;
		break;
	case 12:
		sdp->db[17] = 0x3;
		break;
	case 16:
		sdp->db[17] = 0x4;
		break;
	default:
		WARN(1, "Missing case %d\n", vsc->bpc);
		return -EINVAL;
	}

	/* Dynamic Range and Component Bit Depth */
	if (vsc->dynamic_range == DP_DYNAMIC_RANGE_CTA)
		sdp->db[17] |= 0x80;  /* DB17[7] */

	/* Content Type */
	sdp->db[18] = vsc->content_type & 0x7;

out:
	return (ssize_t)length;
}

/*
 * Pack an Adaptive-Sync SDP (DP 2.1 Tables 2-126/2-127) into the generic
 * layout: DB0 the operation mode, DB1..2 the vtotal, DB3..4 the target
 * refresh rate (10 bits) with its 1.001 divider flag, DB7..8 the coasting
 * vtotal.  `size' is the room behind `sdp'.
 */
ssize_t drm_dp_as_sdp_pack(const struct drm_dp_as_sdp *as_sdp,
			   struct dp_sdp *sdp, size_t size)
{
	size_t length = sizeof(struct dp_sdp);

	if (size < length)
		return -ENOSPC;

	mm_memset(sdp, 0, size);

	/* Prepare AS (Adaptive Sync) SDP Header */
	sdp->sdp_header.HB0 = 0;
	sdp->sdp_header.HB1 = as_sdp->sdp_type;
	sdp->sdp_header.HB2 = as_sdp->revision;
	sdp->sdp_header.HB3 = as_sdp->length;

	/* Fill AS (Adaptive Sync) SDP Payload */
	sdp->db[0] = as_sdp->mode;
	sdp->db[1] = as_sdp->vtotal & 0xFF;
	sdp->db[2] = (as_sdp->vtotal >> 8) & 0xFF;
	sdp->db[3] = as_sdp->target_rr & 0xFF;
	sdp->db[4] = (as_sdp->target_rr >> 8) & 0x3;

	if (as_sdp->target_rr_divider)
		sdp->db[4] |= 0x20;

	sdp->db[7] = as_sdp->coasting_vtotal & 0xFF;
	sdp->db[8] = (as_sdp->coasting_vtotal >> 8) & 0xFF;

	return (ssize_t)length;
}

/* ---- protocol converters (PCON) ------------------------------------------------------------------ */

/* The FRL bandwidth (Gbps) an HDMI 2.1 PCON offers, 0 when none. */
int drm_dp_get_pcon_max_frl_bw(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			       const u8 port_cap[4])
{
	int bw;
	u8 buf;

	if (!drm_dp_is_branch(dpcd))
		return 0;

	if (dpcd[DP_DPCD_REV] < 0x11)
		return 0;

	if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
		return 0;

	if ((port_cap[0] & DP_DS_PORT_TYPE_MASK) != DP_DS_PORT_TYPE_HDMI)
		return 0;

	buf = port_cap[2];
	bw = buf & DP_PCON_MAX_FRL_BW;

	switch (bw) {
	case DP_PCON_MAX_9GBPS:
		return 9;
	case DP_PCON_MAX_18GBPS:
		return 18;
	case DP_PCON_MAX_24GBPS:
		return 24;
	case DP_PCON_MAX_32GBPS:
		return 32;
	case DP_PCON_MAX_40GBPS:
		return 40;
	case DP_PCON_MAX_48GBPS:
		return 48;
	case DP_PCON_MAX_0GBPS:
	default:
		return 0;
	}
}

int drm_dp_pcon_frl_prepare(struct drm_dp_aux *aux, bool enable_frl_ready_hpd)
{
	u8 buf = DP_PCON_ENABLE_SOURCE_CTL_MODE |
		 DP_PCON_ENABLE_LINK_FRL_MODE;

	if (enable_frl_ready_hpd)
		buf |= DP_PCON_ENABLE_HPD_READY;

	return drm_dp_dpcd_write_byte(aux, DP_PCON_HDMI_LINK_CONFIG_1, buf);
}

bool drm_dp_pcon_is_frl_ready(struct drm_dp_aux *aux)
{
	int ret;
	u8 buf;

	ret = drm_dp_dpcd_read_byte(aux, DP_PCON_HDMI_TX_LINK_STATUS, &buf);
	if (ret < 0)
		return false;

	if (buf & DP_PCON_FRL_READY)
		return true;

	return false;
}

/*
 * HDMI link configuration step 1: the maximum FRL bandwidth between PCON
 * and sink and the training mode -- concurrent (FRL bring-up alongside DP
 * link training) or sequential (FRL first).
 */
int drm_dp_pcon_frl_configure_1(struct drm_dp_aux *aux, int max_frl_gbps,
				u8 frl_mode)
{
	int ret;
	u8 buf;

	ret = drm_dp_dpcd_read_byte(aux, DP_PCON_HDMI_LINK_CONFIG_1, &buf);
	if (ret < 0)
		return ret;

	if (frl_mode == DP_PCON_ENABLE_CONCURRENT_LINK)
		buf |= DP_PCON_ENABLE_CONCURRENT_LINK;
	else
		buf &= ~DP_PCON_ENABLE_CONCURRENT_LINK;

	switch (max_frl_gbps) {
	case 9:
		buf |=  DP_PCON_ENABLE_MAX_BW_9GBPS;
		break;
	case 18:
		buf |=  DP_PCON_ENABLE_MAX_BW_18GBPS;
		break;
	case 24:
		buf |=  DP_PCON_ENABLE_MAX_BW_24GBPS;
		break;
	case 32:
		buf |=  DP_PCON_ENABLE_MAX_BW_32GBPS;
		break;
	case 40:
		buf |=  DP_PCON_ENABLE_MAX_BW_40GBPS;
		break;
	case 48:
		buf |=  DP_PCON_ENABLE_MAX_BW_48GBPS;
		break;
	case 0:
		buf |=  DP_PCON_ENABLE_MAX_BW_0GBPS;
		break;
	default:
		return -EINVAL;
	}

	return drm_dp_dpcd_write_byte(aux, DP_PCON_HDMI_LINK_CONFIG_1, buf);
}

/*
 * HDMI link configuration step 2: the FRL rates the PCON may try, and
 * whether it trains them all (extended) or stops at the first that works
 * from the lowest (normal).
 */
int drm_dp_pcon_frl_configure_2(struct drm_dp_aux *aux, int max_frl_mask,
				u8 frl_type)
{
	u8 buf = (u8)max_frl_mask;

	if (frl_type == DP_PCON_FRL_LINK_TRAIN_EXTENDED)
		buf |= DP_PCON_FRL_LINK_TRAIN_EXTENDED;
	else
		buf &= ~DP_PCON_FRL_LINK_TRAIN_EXTENDED;

	return drm_dp_dpcd_write_byte(aux, DP_PCON_HDMI_LINK_CONFIG_2, buf);
}

int drm_dp_pcon_reset_frl_config(struct drm_dp_aux *aux)
{
	return drm_dp_dpcd_write_byte(aux, DP_PCON_HDMI_LINK_CONFIG_1, 0x0);
}

int drm_dp_pcon_frl_enable(struct drm_dp_aux *aux)
{
	int ret;
	u8 buf = 0;

	ret = drm_dp_dpcd_read_byte(aux, DP_PCON_HDMI_LINK_CONFIG_1, &buf);
	if (ret < 0)
		return ret;
	if (!(buf & DP_PCON_ENABLE_SOURCE_CTL_MODE)) {
		dp_dbg("%s: PCON in Autonomous mode, can't enable FRL\n",
		       aux_name(aux));
		return -EINVAL;
	}
	buf |= DP_PCON_ENABLE_HDMI_LINK;
	return drm_dp_dpcd_write_byte(aux, DP_PCON_HDMI_LINK_CONFIG_1, buf);
}

bool drm_dp_pcon_hdmi_link_active(struct drm_dp_aux *aux)
{
	u8 buf;
	int ret;

	ret = drm_dp_dpcd_read_byte(aux, DP_PCON_HDMI_TX_LINK_STATUS, &buf);
	if (ret < 0)
		return false;

	return !!(buf & DP_PCON_HDMI_TX_LINK_ACTIVE);
}

/* The PCON's HDMI link mode (TMDS or FRL), and for FRL the trained rates
 * (one bit in normal training, possibly several in extended). */
int drm_dp_pcon_hdmi_link_mode(struct drm_dp_aux *aux, u8 *frl_trained_mask)
{
	u8 buf;
	int mode;
	int ret;

	ret = drm_dp_dpcd_read_byte(aux, DP_PCON_HDMI_POST_FRL_STATUS, &buf);
	if (ret < 0)
		return ret;

	mode = buf & DP_PCON_HDMI_LINK_MODE;

	if (frl_trained_mask && DP_PCON_HDMI_MODE_FRL == mode)
		*frl_trained_mask = (buf & DP_PCON_HDMI_FRL_TRAINED_BW) >> 1;

	return mode;
}

/* Log the per-lane error counts after an FRL link failure between PCON
 * and sink. */
void drm_dp_pcon_hdmi_frl_link_error_count(struct drm_dp_aux *aux,
					   struct drm_connector *connector)
{
	u8 buf, error_count;
	int i, num_error;
	struct drm_hdmi_info *hdmi = &connector->display_info.hdmi;

	for (i = 0; i < hdmi->max_lanes; i++) {
		if (drm_dp_dpcd_read_byte(aux, DP_PCON_HDMI_ERROR_STATUS_LN0 + i, &buf) < 0)
			return;

		error_count = buf & DP_PCON_HDMI_ERROR_COUNT_MASK;
		switch (error_count) {
		case DP_PCON_HDMI_ERROR_COUNT_HUNDRED_PLUS:
			num_error = 100;
			break;
		case DP_PCON_HDMI_ERROR_COUNT_TEN_PLUS:
			num_error = 10;
			break;
		case DP_PCON_HDMI_ERROR_COUNT_THREE_PLUS:
			num_error = 3;
			break;
		default:
			num_error = 0;
		}

		dp_err("%s: More than %d errors since the last read for lane %d\n",
		       aux_name(aux), num_error, i);
	}
}

bool drm_dp_pcon_enc_is_dsc_1_2(const u8 pcon_dsc_dpcd[DP_PCON_DSC_ENCODER_CAP_SIZE])
{
	u8 buf;
	u8 major_v, minor_v;

	buf = pcon_dsc_dpcd[DP_PCON_DSC_VERSION - DP_PCON_DSC_ENCODER];
	major_v = (buf & DP_PCON_DSC_MAJOR_MASK) >> DP_PCON_DSC_MAJOR_SHIFT;
	minor_v = (buf & DP_PCON_DSC_MINOR_MASK) >> DP_PCON_DSC_MINOR_SHIFT;

	if (major_v == 1 && minor_v == 2)
		return true;

	return false;
}

int drm_dp_pcon_dsc_max_slices(const u8 pcon_dsc_dpcd[DP_PCON_DSC_ENCODER_CAP_SIZE])
{
	u8 slice_cap1, slice_cap2;

	slice_cap1 = pcon_dsc_dpcd[DP_PCON_DSC_SLICE_CAP_1 - DP_PCON_DSC_ENCODER];
	slice_cap2 = pcon_dsc_dpcd[DP_PCON_DSC_SLICE_CAP_2 - DP_PCON_DSC_ENCODER];

	if (slice_cap2 & DP_PCON_DSC_24_PER_DSC_ENC)
		return 24;
	if (slice_cap2 & DP_PCON_DSC_20_PER_DSC_ENC)
		return 20;
	if (slice_cap2 & DP_PCON_DSC_16_PER_DSC_ENC)
		return 16;
	if (slice_cap1 & DP_PCON_DSC_12_PER_DSC_ENC)
		return 12;
	if (slice_cap1 & DP_PCON_DSC_10_PER_DSC_ENC)
		return 10;
	if (slice_cap1 & DP_PCON_DSC_8_PER_DSC_ENC)
		return 8;
	if (slice_cap1 & DP_PCON_DSC_6_PER_DSC_ENC)
		return 6;
	if (slice_cap1 & DP_PCON_DSC_4_PER_DSC_ENC)
		return 4;
	if (slice_cap1 & DP_PCON_DSC_2_PER_DSC_ENC)
		return 2;
	if (slice_cap1 & DP_PCON_DSC_1_PER_DSC_ENC)
		return 1;

	return 0;
}

int drm_dp_pcon_dsc_max_slice_width(const u8 pcon_dsc_dpcd[DP_PCON_DSC_ENCODER_CAP_SIZE])
{
	u8 buf;

	buf = pcon_dsc_dpcd[DP_PCON_DSC_MAX_SLICE_WIDTH - DP_PCON_DSC_ENCODER];

	return buf * DP_DSC_SLICE_WIDTH_MULTIPLIER;
}

int drm_dp_pcon_dsc_bpp_incr(const u8 pcon_dsc_dpcd[DP_PCON_DSC_ENCODER_CAP_SIZE])
{
	u8 buf;

	buf = pcon_dsc_dpcd[DP_PCON_DSC_BPP_INCR - DP_PCON_DSC_ENCODER];

	switch (buf & DP_PCON_DSC_BPP_INCR_MASK) {
	case DP_PCON_DSC_ONE_16TH_BPP:
		return 16;
	case DP_PCON_DSC_ONE_8TH_BPP:
		return 8;
	case DP_PCON_DSC_ONE_4TH_BPP:
		return 4;
	case DP_PCON_DSC_ONE_HALF_BPP:
		return 2;
	case DP_PCON_DSC_ONE_BPP:
		return 1;
	}

	return 0;
}

static int drm_dp_pcon_configure_dsc_enc(struct drm_dp_aux *aux, u8 pps_buf_config)
{
	u8 buf;
	int ret;

	ret = drm_dp_dpcd_read_byte(aux, DP_PROTOCOL_CONVERTER_CONTROL_2, &buf);
	if (ret < 0)
		return ret;

	buf |= DP_PCON_ENABLE_DSC_ENCODER;

	if (pps_buf_config <= DP_PCON_ENC_PPS_OVERRIDE_EN_BUFFER) {
		buf &= ~DP_PCON_ENCODER_PPS_OVERRIDE_MASK;
		buf |= pps_buf_config << 2;
	}

	return drm_dp_dpcd_write_byte(aux, DP_PROTOCOL_CONVERTER_CONTROL_2, buf);
}

/* Let the PCON pick the PPS parameters for DSC 1.2 towards the sink. */
int drm_dp_pcon_pps_default(struct drm_dp_aux *aux)
{
	return drm_dp_pcon_configure_dsc_enc(aux, DP_PCON_ENC_PPS_OVERRIDE_DISABLED);
}

/* Hand the PCON the 128-byte PPS for the HDMI sink. */
int drm_dp_pcon_pps_override_buf(struct drm_dp_aux *aux, u8 pps_buf[128])
{
	int ret;

	ret = drm_dp_dpcd_write_data(aux, DP_PCON_HDMI_PPS_OVERRIDE_BASE, pps_buf, 128);
	if (ret < 0)
		return ret;

	return drm_dp_pcon_configure_dsc_enc(aux, DP_PCON_ENC_PPS_OVERRIDE_EN_BUFFER);
}

/* Override slice height, slice width and bpp (two bytes each). */
int drm_dp_pcon_pps_override_param(struct drm_dp_aux *aux, u8 pps_param[6])
{
	int ret;

	ret = drm_dp_dpcd_write_data(aux, DP_PCON_HDMI_PPS_OVRD_SLICE_HEIGHT, &pps_param[0], 2);
	if (ret < 0)
		return ret;
	ret = drm_dp_dpcd_write_data(aux, DP_PCON_HDMI_PPS_OVRD_SLICE_WIDTH, &pps_param[2], 2);
	if (ret < 0)
		return ret;
	ret = drm_dp_dpcd_write_data(aux, DP_PCON_HDMI_PPS_OVRD_BPP, &pps_param[4], 2);
	if (ret < 0)
		return ret;

	return drm_dp_pcon_configure_dsc_enc(aux, DP_PCON_ENC_PPS_OVERRIDE_EN_BUFFER);
}

/* Enable RGB to YCbCr conversion in the PCON for `color_spc' (0 off). */
int drm_dp_pcon_convert_rgb_to_ycbcr(struct drm_dp_aux *aux, u8 color_spc)
{
	int ret;
	u8 buf;

	ret = drm_dp_dpcd_read_byte(aux, DP_PROTOCOL_CONVERTER_CONTROL_2, &buf);
	if (ret < 0)
		return ret;

	if (color_spc & DP_CONVERSION_RGB_YCBCR_MASK)
		buf |= (color_spc & DP_CONVERSION_RGB_YCBCR_MASK);
	else
		buf &= ~DP_CONVERSION_RGB_YCBCR_MASK;

	return drm_dp_dpcd_write_byte(aux, DP_PROTOCOL_CONVERTER_CONTROL_2, buf);
}

/* ---- eDP backlight (VESA) --------------------------------------------------------------------------- */

/*
 * Set the backlight level through AUX: one byte, two (MSB first) when the
 * panel uses the LSB register, or three bytes of millinits in luminance
 * mode.  The backlight must already be enabled (drm_edp_backlight_enable).
 * Panels driven by the PWM pin ignore this.
 */
int drm_edp_backlight_set_level(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
				u32 level)
{
	int ret;
	unsigned int offset = DP_EDP_BACKLIGHT_BRIGHTNESS_MSB;
	u8 buf[3] = { 0 };
	size_t len = 2;

	/* The panel uses the PWM for controlling brightness levels */
	if (!(bl->aux_set || bl->luminance_set))
		return 0;

	if (bl->luminance_set) {
		level = level * 1000;
		level &= 0xffffff;
		buf[0] = (level & 0x0000ff);
		buf[1] = (level & 0x00ff00) >> 8;
		buf[2] = (level & 0xff0000) >> 16;
		offset = DP_EDP_PANEL_TARGET_LUMINANCE_VALUE;
		len = 3;
	} else if (bl->lsb_reg_used) {
		buf[0] = (level & 0xff00) >> 8;
		buf[1] = (level & 0x00ff);
	} else {
		buf[0] = level;
	}

	ret = drm_dp_dpcd_write_data(aux, offset, buf, len);
	if (ret < 0) {
		dp_err("%s: Failed to write aux backlight level: %d\n",
		       aux_name(aux), ret);
		return ret;
	}

	return 0;
}

static int
drm_edp_backlight_set_enable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
			     bool enable)
{
	int ret;
	u8 buf;

	/* This panel uses the EDP_BL_PWR GPIO for enablement */
	if (!bl->aux_enable)
		return 0;

	ret = drm_dp_dpcd_read_byte(aux, DP_EDP_DISPLAY_CONTROL_REGISTER, &buf);
	if (ret < 0) {
		dp_err("%s: Failed to read eDP display control register: %d\n",
		       aux_name(aux), ret);
		return ret;
	}
	if (enable)
		buf |= DP_EDP_BACKLIGHT_ENABLE;
	else
		buf &= ~DP_EDP_BACKLIGHT_ENABLE;

	ret = drm_dp_dpcd_write_byte(aux, DP_EDP_DISPLAY_CONTROL_REGISTER, buf);
	if (ret < 0) {
		dp_err("%s: Failed to write eDP display control register: %d\n",
		       aux_name(aux), ret);
		return ret;
	}

	return 0;
}

/*
 * Turn on DPCD backlight control: the control mode (DPCD or PWM, plus
 * luminance), the PWM bit count and frequency divider found at init, the
 * level, and the enable bit.  Panels powered through the EDP_BL_PWR GPIO
 * (aux_enable false) skip the enable bit: the driver handles that pin.
 */
int drm_edp_backlight_enable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
			     const u32 level)
{
	int ret;
	u8 dpcd_buf;

	if (bl->aux_set)
		dpcd_buf = DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD;
	else
		dpcd_buf = DP_EDP_BACKLIGHT_CONTROL_MODE_PWM;

	if (bl->luminance_set)
		dpcd_buf |= DP_EDP_PANEL_LUMINANCE_CONTROL_ENABLE;

	if (bl->pwmgen_bit_count) {
		ret = drm_dp_dpcd_write_byte(aux, DP_EDP_PWMGEN_BIT_COUNT, bl->pwmgen_bit_count);
		if (ret < 0)
			dp_dbg("%s: Failed to write aux pwmgen bit count: %d\n",
			       aux_name(aux), ret);
	}

	if (bl->pwm_freq_pre_divider) {
		ret = drm_dp_dpcd_write_byte(aux, DP_EDP_BACKLIGHT_FREQ_SET,
					     bl->pwm_freq_pre_divider);
		if (ret < 0)
			dp_dbg("%s: Failed to write aux backlight frequency: %d\n",
			       aux_name(aux), ret);
		else
			dpcd_buf |= DP_EDP_BACKLIGHT_FREQ_AUX_SET_ENABLE;
	}

	ret = drm_dp_dpcd_write_byte(aux, DP_EDP_BACKLIGHT_MODE_SET_REGISTER, dpcd_buf);
	if (ret < 0) {
		dp_dbg("%s: Failed to write aux backlight mode: %d\n",
		       aux_name(aux), ret);
		return ret < 0 ? ret : -EIO;
	}

	ret = drm_edp_backlight_set_level(aux, bl, level);
	if (ret < 0)
		return ret;
	ret = drm_edp_backlight_set_enable(aux, bl, true);
	if (ret < 0)
		return ret;

	return 0;
}

/* Clear the DPCD backlight enable bit, when the panel has one. */
int drm_edp_backlight_disable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl)
{
	int ret;

	ret = drm_edp_backlight_set_enable(aux, bl, false);
	if (ret < 0)
		return ret;

	return 0;
}

static int
drm_edp_backlight_probe_max(struct drm_dp_aux *aux, struct drm_edp_backlight_info *bl,
			    u16 driver_pwm_freq_hz, const u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE])
{
	int fxp, fxp_min, fxp_max, fxp_actual, f = 1;
	int ret;
	u8 pn, pn_min, pn_max, bit_count;

	if (!bl->aux_set)
		return 0;

	ret = drm_dp_dpcd_read_byte(aux, DP_EDP_PWMGEN_BIT_COUNT, &bit_count);
	if (ret < 0) {
		dp_dbg("%s: Failed to read pwmgen bit count cap: %d\n",
		       aux_name(aux), ret);
		return -ENODEV;
	}

	bit_count &= DP_EDP_PWMGEN_BIT_COUNT_MASK;

	ret = drm_dp_dpcd_read_byte(aux, DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN, &pn_min);
	if (ret < 0) {
		dp_dbg("%s: Failed to read pwmgen bit count cap min: %d\n",
		       aux_name(aux), ret);
		return -ENODEV;
	}
	pn_min &= DP_EDP_PWMGEN_BIT_COUNT_MASK;

	ret = drm_dp_dpcd_read_byte(aux, DP_EDP_PWMGEN_BIT_COUNT_CAP_MAX, &pn_max);
	if (ret < 0) {
		dp_dbg("%s: Failed to read pwmgen bit count cap max: %d\n",
		       aux_name(aux), ret);
		return -ENODEV;
	}
	pn_max &= DP_EDP_PWMGEN_BIT_COUNT_MASK;

	if (unlikely(pn_min > pn_max)) {
		dp_dbg("%s: Invalid pwmgen bit count cap min/max returned: %d %d\n",
		       aux_name(aux), pn_min, pn_max);
		return -EINVAL;
	}

	/*
	 * Per VESA eDP Spec v1.4b, section 3.3.10.2:
	 * If DP_EDP_PWMGEN_BIT_COUNT is less than DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN,
	 * the sink must use the MIN value as the effective PWM bit count.
	 * Clamp the reported value to the [MIN, MAX] capability range to ensure
	 * correct brightness scaling on compliant eDP panels.
	 * Only enable this logic if the [MIN, MAX] range is valid in regard to Spec.
	 */
	pn = bit_count;
	if (bit_count < pn_min)
		pn = clamp_t(u8, bit_count, pn_min, pn_max);

	bl->max = (1 << pn) - 1;
	if (!driver_pwm_freq_hz) {
		if (pn != bit_count)
			goto bit_count_write_back;

		return 0;
	}

	/*
	 * Set PWM Frequency divider to match desired frequency provided by the driver.
	 * The PWM Frequency is calculated as 27Mhz / (F x P).
	 * - Where F = PWM Frequency Pre-Divider value programmed by field 7:0 of the
	 *             EDP_BACKLIGHT_FREQ_SET register (DPCD Address 00728h)
	 * - Where P = 2^Pn, where Pn is the value programmed by field 4:0 of the
	 *             EDP_PWMGEN_BIT_COUNT register (DPCD Address 00724h)
	 */

	/* Find desired value of (F x P)
	 * Note that, if F x P is out of supported range, the maximum value or minimum value will
	 * applied automatically. So no need to check that.
	 */
	fxp = DIV_ROUND_CLOSEST(1000 * DP_EDP_BACKLIGHT_FREQ_BASE_KHZ, driver_pwm_freq_hz);

	/* Use highest possible value of Pn for more granularity of brightness adjustment while
	 * satisfying the conditions below.
	 * - Pn is in the range of Pn_min and Pn_max
	 * - F is in the range of 1 and 255
	 * - FxP is within 25% of desired value.
	 *   Note: 25% is arbitrary value and may need some tweak.
	 */
	/* Ensure frequency is within 25% of desired value */
	fxp_min = DIV_ROUND_CLOSEST(fxp * 3, 4);
	fxp_max = DIV_ROUND_CLOSEST(fxp * 5, 4);
	if (fxp_min < (1 << pn_min) || (255 << pn_max) < fxp_max) {
		dp_dbg("%s: Driver defined backlight frequency (%d) out of range\n",
		       aux_name(aux), driver_pwm_freq_hz);
		return 0;
	}

	/* pn is unsigned: stop after pn_min instead of decrementing below 0. */
	for (pn = pn_max; ; pn--) {
		f = clamp(DIV_ROUND_CLOSEST(fxp, 1 << pn), 1, 255);
		fxp_actual = f << pn;
		if (fxp_min <= fxp_actual && fxp_actual <= fxp_max)
			break;
		if (pn == pn_min) {
			/* Nothing fits: end one below pn_min, as a loop
			 * that ran past it would -- but never below 0. */
			if (pn > 0)
				pn--;
			break;
		}
	}

bit_count_write_back:
	ret = drm_dp_dpcd_write_byte(aux, DP_EDP_PWMGEN_BIT_COUNT, pn);
	if (ret < 0) {
		dp_dbg("%s: Failed to write aux pwmgen bit count: %d\n",
		       aux_name(aux), ret);
		return 0;
	}

	if (!driver_pwm_freq_hz)
		return 0;

	bl->pwmgen_bit_count = pn;
	bl->max = (1 << pn) - 1;

	if (edp_dpcd[2] & DP_EDP_BACKLIGHT_FREQ_AUX_SET_CAP) {
		bl->pwm_freq_pre_divider = f;
		dp_dbg("%s: Using backlight frequency from driver (%dHz)\n",
		       aux_name(aux), driver_pwm_freq_hz);
	}

	return 0;
}

static int
drm_edp_backlight_probe_state(struct drm_dp_aux *aux, struct drm_edp_backlight_info *bl,
			      u8 *current_mode)
{
	int ret;
	u8 buf[3];
	u8 mode_reg;

	ret = drm_dp_dpcd_read_byte(aux, DP_EDP_BACKLIGHT_MODE_SET_REGISTER, &mode_reg);
	if (ret < 0) {
		dp_dbg("%s: Failed to read backlight mode: %d\n",
		       aux_name(aux), ret);
		return ret < 0 ? ret : -EIO;
	}

	*current_mode = (mode_reg & DP_EDP_BACKLIGHT_CONTROL_MODE_MASK);
	if (!bl->aux_set)
		return 0;

	if (*current_mode == DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD) {
		int size = 1 + (bl->lsb_reg_used ? 1 : 0);

		if (bl->luminance_set) {
			ret = drm_dp_dpcd_read_data(aux, DP_EDP_PANEL_TARGET_LUMINANCE_VALUE,
						    buf, sizeof(buf));
			if (ret < 0) {
				dp_dbg("%s: Failed to read backlight level: %d\n",
				       aux_name(aux), ret);
				return ret;
			}

			/*
			 * The luminance register holds millinits; the caller
			 * works in nits.
			 */
			return (buf[0] | buf[1] << 8 | buf[2] << 16) / 1000;
		} else {
			ret = drm_dp_dpcd_read_data(aux, DP_EDP_BACKLIGHT_BRIGHTNESS_MSB,
						    buf, size);
			if (ret < 0) {
				dp_dbg("%s: Failed to read backlight level: %d\n",
				       aux_name(aux), ret);
				return ret;
			}

			if (bl->lsb_reg_used)
				return (buf[0] << 8) | buf[1];
			else
				return buf[0];
		}
	}

	/*
	 * If we're not in DPCD control mode yet, the programmed brightness value is meaningless and
	 * the driver should assume max brightness
	 */
	return bl->max;
}

/*
 * Probe a panel's TCON for the VESA eDP backlight interface: which
 * controls it has (AUX enable, AUX level, 16-bit level, luminance), the
 * maximum level (from the PWM bit count, or max_luminance in luminance
 * mode), a PWM divider for driver_pwm_freq_hz when non-zero, and the
 * current level and control mode.  0 or a negative errno.
 */
int
drm_edp_backlight_init(struct drm_dp_aux *aux, struct drm_edp_backlight_info *bl,
		       u32 max_luminance,
		       u16 driver_pwm_freq_hz, const u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE],
		       u32 *current_level, u8 *current_mode, bool need_luminance)
{
	int ret;

	if (edp_dpcd[1] & DP_EDP_BACKLIGHT_AUX_ENABLE_CAP)
		bl->aux_enable = true;
	if (edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_AUX_SET_CAP)
		bl->aux_set = true;
	if (edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_BYTE_COUNT)
		bl->lsb_reg_used = true;
	if ((edp_dpcd[0] & DP_EDP_15) && edp_dpcd[3] &
	    (DP_EDP_PANEL_LUMINANCE_CONTROL_CAPABLE) && need_luminance)
		bl->luminance_set = true;

	/* Sanity check caps */
	if (!bl->aux_set && !(edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_PWM_PIN_CAP) &&
	    !bl->luminance_set) {
		dp_dbg("%s: Panel does not support AUX, PWM or luminance-based brightness control. Aborting\n",
		       aux_name(aux));
		return -EINVAL;
	}

	if (bl->luminance_set) {
		bl->max = max_luminance;
	} else {
		ret = drm_edp_backlight_probe_max(aux, bl, driver_pwm_freq_hz, edp_dpcd);
		if (ret < 0)
			return ret;
	}

	ret = drm_edp_backlight_probe_state(aux, bl, current_mode);
	if (ret < 0)
		return ret;
	*current_level = ret;

	dp_dbg("%s: Found backlight: aux_set=%d aux_enable=%d mode=%d\n",
	       aux_name(aux), bl->aux_set, bl->aux_enable, *current_mode);
	if (bl->aux_set) {
		dp_dbg("%s: Backlight caps: level=%d/%d pwm_freq_pre_divider=%d lsb_reg_used=%d\n",
		       aux_name(aux), *current_level, bl->max, bl->pwm_freq_pre_divider,
		       bl->lsb_reg_used);
	}

	return 0;
}

/* ---- link bandwidth ------------------------------------------------------------------------------- */

/* See DP Standard v2.1 2.6.4.4.1.1, 2.8.4.4, 2.8.7 */
static int drm_dp_link_data_symbol_cycles(int lane_count, int pixels,
					  int bpp_x16, int symbol_size,
					  bool is_mst)
{
	int cycles = DIV_ROUND_UP(pixels * bpp_x16, 16 * symbol_size * lane_count);
	int align = is_mst ? 4 / lane_count : 1;

	return ALIGN(cycles, align);
}

/*
 * The link symbol cycles of `pixels' (one scanline), with DSC
 * (dsc_slice_count > 0: per-slice data plus the end-of-chunk symbols) or
 * without, on SST or MST.
 */
int drm_dp_link_symbol_cycles(int lane_count, int pixels, int dsc_slice_count,
			      int bpp_x16, int symbol_size, bool is_mst)
{
	int slice_count = dsc_slice_count ? : 1;
	int slice_pixels = DIV_ROUND_UP(pixels, slice_count);
	int slice_data_cycles = drm_dp_link_data_symbol_cycles(lane_count,
							       slice_pixels,
							       bpp_x16,
							       symbol_size,
							       is_mst);
	int slice_eoc_cycles = 0;

	if (dsc_slice_count)
		slice_eoc_cycles = is_mst ? 4 / lane_count : 1;

	return slice_count * (slice_data_cycles + slice_eoc_cycles);
}

/*
 * The BW allocation overhead of a DP stream as 100% + overhead% in ppm,
 * from the lane count, SST/MST, symbol size (UHBR), FEC, SSC/ref-clock
 * margin, the active width, the bpp (.4 fixed point) and the DSC slice
 * count.  Channel coding efficiency is separate
 * (drm_dp_bw_channel_coding_efficiency()).
 */
int drm_dp_bw_overhead(int lane_count, int hactive,
		       int dsc_slice_count,
		       int bpp_x16, unsigned long flags)
{
	int symbol_size = flags & DRM_DP_BW_OVERHEAD_UHBR ? 32 : 8;
	bool is_mst = !!(flags & DRM_DP_BW_OVERHEAD_MST);
	u32 overhead = 1000000;
	int symbol_cycles;

	if (lane_count == 0 || hactive == 0 || bpp_x16 == 0) {
		dp_dbg("Invalid BW overhead params: lane_count %d, hactive %d, bpp_x16 %d.%04d\n",
		       lane_count, hactive, bpp_x16 >> 4, (bpp_x16 & 0xf) * 625);
		return 0;
	}

	/*
	 * DP Standard v2.1 2.6.4.1
	 * SSC downspread and ref clock variation margin:
	 *   5300ppm + 300ppm ~ 0.6%
	 */
	if (flags & DRM_DP_BW_OVERHEAD_SSC_REF_CLK)
		overhead += 6000;

	/*
	 * DP Standard v2.1 2.6.4.1.1, 3.5.1.5.4:
	 * FEC symbol insertions for 8b/10b channel coding:
	 * After each 250 data symbols on 2-4 lanes:
	 *   250 LL + 5 FEC_PARITY_PH + 1 CD_ADJ   (256 byte FEC block)
	 * After each 2 x 250 data symbols on 1 lane:
	 *   2 * 250 LL + 11 FEC_PARITY_PH + 1 CD_ADJ (512 byte FEC block)
	 * After 256 (2-4 lanes) or 128 (1 lane) FEC blocks:
	 *   256 * 256 bytes + 1 FEC_PM
	 * or
	 *   128 * 512 bytes + 1 FEC_PM
	 * (256 * 6 + 1) / (256 * 250) = 2.4015625 %
	 */
	if (flags & DRM_DP_BW_OVERHEAD_FEC)
		overhead += 24016;

	/*
	 * DP Standard v2.1 2.7.9, 5.9.7
	 * The FEC overhead for UHBR is accounted for in its 96.71% channel
	 * coding efficiency.
	 */
	WARN_ON((flags & DRM_DP_BW_OVERHEAD_UHBR) &&
		(flags & DRM_DP_BW_OVERHEAD_FEC));

	symbol_cycles = drm_dp_link_symbol_cycles(lane_count, hactive,
						  dsc_slice_count,
						  bpp_x16, symbol_size,
						  is_mst);

	return (int)DIV_ROUND_UP_ULL((u64)(u32)(symbol_cycles * symbol_size * lane_count) *
				     (u64)(overhead * 16),
				     (u64)(hactive * bpp_x16));
}

/*
 * The channel coding efficiency in ppm: 8b/10b 80% (on MST really 78.75%,
 * one MTPH in 64, which is not counted here), 128b/132b 96.71% including
 * its link-layer and FEC overhead.
 */
int drm_dp_bw_channel_coding_efficiency(bool is_uhbr)
{
	if (is_uhbr)
		return 967100;
	else
		return 800000;
}

/*
 * The payload rate of a DPRX link in kB/s: the link rate (10 kbit/s
 * units) times the lanes times the channel coding efficiency.  MST and
 * tunnels may limit the rate further.
 */
int drm_dp_max_dprx_data_rate(int max_link_rate, int max_lanes)
{
	int ch_coding_efficiency =
		drm_dp_bw_channel_coding_efficiency(drm_dp_is_uhbr_rate(max_link_rate));

	return (int)div64_u64((u64)(u32)(max_link_rate * 10 * max_lanes) *
			      (u64)(u32)ch_coding_efficiency,
			      1000000 * 8);
}
