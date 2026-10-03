// LikeOS -- DisplayPort helpers: the AUX channel, DPCD access, I2C over
// AUX, and the link, sink and panel capability decoding drivers share.
//
// A DisplayPort source talks to its sink over two paths.  The main link
// carries the pixels; the AUX channel is a slow half-duplex side channel
// that carries native reads and writes of the sink's DPCD register space
// (drm_dp.h) and I2C transactions tunnelled to the sink's DDC bus (the
// EDID).  A driver describes its AUX hardware with one hook,
// drm_dp_aux.transfer, which runs a single AUX transaction of at most 16
// payload bytes and reports the sink's reply code; everything above it is
// here: retrying a DEFERred or busy transaction, splitting I2C messages
// and draining short replies, reading the receiver capabilities (and their
// extended copy at 0x2200), the link-training status decoders and delays,
// LTTPR (repeater) discovery and mode setting, branch-device
// (downstream-port) limits, the sink/branch identification and its quirk
// table, DSC and FEC capability decoding, the VSC and Adaptive-Sync
// secondary-data packets, the VESA eDP backlight interface, and the link
// bandwidth arithmetic.
//
// Every AUX access may sleep: callers run in process context with
// interrupts enabled (a driver's probe, detect or modeset path, the
// hotplug worker).  drm_dp_aux.hw_mutex serialises the transactions of one
// channel; a driver whose channels share hardware adds its own lock.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Keith Packard's code: HPND-sell-variant
// Portions Copyright (C) 2008 Keith Packard
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_DP_HELPER_H
#define KERNEL_DEV_GPU_DRM_DP_HELPER_H

#include <kernel/uapi/types.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/dev/gpu/drm_dp.h>
#include <kernel/dev/i2c.h>
#include <kernel/mm/rwsem.h>
#include <kernel/ke/syscall.h> /* errno values; after rwsem.h (FD_CLOEXEC) */
#include <kernel/uapi/drm/drm_mode.h>

/* ---- compile-time switches ---------------------------------------------- */

/* DRM_DP_DEBUG: 1 logs every DPCD access (offset, direction, bytes), each
 * AUX/I2C retry and the decoded capabilities through kprintf.  0 (the
 * default) keeps the AUX paths silent; only real failures are reported,
 * and those rate-limited per call site. */
#ifndef DRM_DP_DEBUG
#define DRM_DP_DEBUG 0
#endif

/* DRM_DP_SLEEPING_DELAYS: the helpers wait between retries (0.5 ms) and
 * across link-training phases (0.1 to 64 ms).  1 (the default) sleeps for
 * waits of a millisecond or more when an HPET comparator drives the
 * high-resolution timer and interrupts are on, so a 16 ms training delay
 * does not hold the CPU; shorter waits, and every wait on a machine whose
 * timer only ticks, are busy waits (a tick-granular sleep would stretch a
 * 0.5 ms retry to a whole tick).  0 busy-waits always, the way the
 * display drivers waited before these helpers existed. */
#ifndef DRM_DP_SLEEPING_DELAYS
#define DRM_DP_SLEEPING_DELAYS 1
#endif

#ifndef EREMOTEIO
#define EREMOTEIO 121 /* the remote end NACKed */
#endif

typedef long ssize_t;

struct drm_device;
struct drm_connector;
struct drm_display_mode;
struct drm_edid;
struct drm_dp_aux;

/* ---- link status and training ------------------------------------------- */

/* All lanes report CR done, channel EQ done and symbol lock, and the
 * inter-lane alignment is done (8b/10b). */
bool drm_dp_channel_eq_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			  int lane_count);
/* All lanes report clock recovery done (8b/10b). */
bool drm_dp_clock_recovery_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			      int lane_count);
bool drm_dp_post_lt_adj_req_in_progress(const u8 link_status[DP_LINK_STATUS_SIZE]);
/* The swing / pre-emphasis the sink asks for on `lane', already shifted
 * into their DP_TRAINING_LANEx_SET positions. */
u8 drm_dp_get_adjust_request_voltage(const u8 link_status[DP_LINK_STATUS_SIZE],
				     int lane);
u8 drm_dp_get_adjust_request_pre_emphasis(const u8 link_status[DP_LINK_STATUS_SIZE],
					  int lane);
/* 128b/132b: the TX FFE preset requested for `lane'. */
u8 drm_dp_get_adjust_tx_ffe_preset(const u8 link_status[DP_LINK_STATUS_SIZE],
				   int lane);

/* The clock-recovery / channel-EQ wait in microseconds for the DPRX or an
 * LTTPR, 8b/10b or 128b/132b, reading the interval register when it lies
 * outside `dpcd'. */
int drm_dp_read_clock_recovery_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				     enum drm_dp_phy dp_phy, bool uhbr);
int drm_dp_read_channel_eq_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				 enum drm_dp_phy dp_phy, bool uhbr);

/* Wait out the training phases (8b/10b DPRX, or an LTTPR). */
void drm_dp_link_train_clock_recovery_delay(const struct drm_dp_aux *aux,
					    const u8 dpcd[DP_RECEIVER_CAP_SIZE]);
void drm_dp_lttpr_link_train_clock_recovery_delay(void);
void drm_dp_link_train_channel_eq_delay(const struct drm_dp_aux *aux,
					const u8 dpcd[DP_RECEIVER_CAP_SIZE]);
void drm_dp_lttpr_link_train_channel_eq_delay(const struct drm_dp_aux *aux,
					      const u8 caps[DP_LTTPR_PHY_CAP_SIZE]);

/* 128b/132b training status and its AUX read interval (microseconds). */
int drm_dp_128b132b_read_aux_rd_interval(struct drm_dp_aux *aux);
bool drm_dp_128b132b_lane_channel_eq_done(const u8 link_status[DP_LINK_STATUS_SIZE],
					  int lane_count);
bool drm_dp_128b132b_lane_symbol_locked(const u8 link_status[DP_LINK_STATUS_SIZE],
					int lane_count);
bool drm_dp_128b132b_eq_interlane_align_done(const u8 link_status[DP_LINK_STATUS_SIZE]);
bool drm_dp_128b132b_cds_interlane_align_done(const u8 link_status[DP_LINK_STATUS_SIZE]);
bool drm_dp_128b132b_link_training_failed(const u8 link_status[DP_LINK_STATUS_SIZE]);

/* Link rate in 10 kbit/s units (162000 = RBR, 1000000 = UHBR10) to and
 * from the DP_LINK_BW_SET / DP_MAX_LINK_RATE code. */
u8 drm_dp_link_rate_to_bw_code(int link_rate);
int drm_dp_bw_code_to_link_rate(u8 link_bw);

/* "DPRX", "LTTPR 1".."LTTPR 8"; never NULL. */
const char *drm_dp_phy_name(enum drm_dp_phy dp_phy);

/* ---- secondary-data packets --------------------------------------------- */

/* A VSC SDP (DP 1.4 Tables 2-116/2-117): pixel encoding, colorimetry,
 * depth, range and content type in revision 5/7; the PSR state in 2..4. */
struct drm_dp_vsc_sdp {
	unsigned char sdp_type;
	unsigned char revision;
	unsigned char length;
	enum dp_pixelformat pixelformat;
	enum dp_colorimetry colorimetry;
	int bpc;
	enum dp_dynamic_range dynamic_range;
	enum dp_content_type content_type;
};

/* An Adaptive-Sync SDP (DP 2.1 Tables 2-126/2-127). */
struct drm_dp_as_sdp {
	unsigned char sdp_type;
	unsigned char revision;
	unsigned char length;
	int vtotal;
	int target_rr;
	int duration_incr_ms;
	int duration_decr_ms;
	bool target_rr_divider;
	enum operation_mode mode;
	int coasting_vtotal;
};

/* Print a decoded SDP through kprintf, each line led by `prefix'. */
void drm_dp_as_sdp_log(const char *prefix, const struct drm_dp_as_sdp *as_sdp);
void drm_dp_vsc_sdp_log(const char *prefix, const struct drm_dp_vsc_sdp *vsc);

/* Does the sink take a VSC SDP with colorimetry / an Adaptive-Sync SDP? */
bool drm_dp_vsc_sdp_supported(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE]);
bool drm_dp_as_sdp_supported(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE]);

/* The eDP PSR setup time in microseconds, or -EINVAL. */
int drm_dp_psr_setup_time(const u8 psr_cap[EDP_PSR_RECEIVER_CAP_SIZE]);

/* ---- receiver capability decoding --------------------------------------- */

static inline int
drm_dp_max_link_rate(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return drm_dp_bw_code_to_link_rate(dpcd[DP_MAX_LINK_RATE]);
}

static inline u8
drm_dp_max_lane_count(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;
}

static inline bool
drm_dp_enhanced_frame_cap(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_DPCD_REV] >= 0x11 &&
		(dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP);
}

static inline bool
drm_dp_post_lt_adj_req_supported(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_DPCD_REV] >= 0x13 &&
		(dpcd[DP_MAX_LANE_COUNT] & DP_POST_LT_ADJ_REQ_SUPPORTED);
}

static inline bool
drm_dp_fast_training_cap(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_DPCD_REV] >= 0x11 &&
		(dpcd[DP_MAX_DOWNSPREAD] & DP_NO_AUX_HANDSHAKE_LINK_TRAINING);
}

static inline bool
drm_dp_tps3_supported(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_DPCD_REV] >= 0x12 &&
		dpcd[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED;
}

static inline bool
drm_dp_max_downspread(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_DPCD_REV] >= 0x11 ||
		dpcd[DP_MAX_DOWNSPREAD] & DP_MAX_DOWNSPREAD_0_5;
}

static inline bool
drm_dp_tps4_supported(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_DPCD_REV] >= 0x14 &&
		dpcd[DP_MAX_DOWNSPREAD] & DP_TPS4_SUPPORTED;
}

static inline u8
drm_dp_training_pattern_mask(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return (dpcd[DP_DPCD_REV] >= 0x14) ? DP_TRAINING_PATTERN_MASK_1_4 :
		DP_TRAINING_PATTERN_MASK;
}

static inline bool
drm_dp_is_branch(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_PRESENT;
}

/* ---- DSC and FEC capability decoding ------------------------------------ */

/* The bpp step the sink accepts: 16 means 1/16 bpp, 1 means whole bpp. */
u8 drm_dp_dsc_sink_bpp_incr(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE]);
u32 drm_dp_dsc_slice_count_to_mask(int slice_count);
/* Bit n-1 set when the sink supports n slices per line. */
u32 drm_dp_dsc_sink_slice_count_mask(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
				     bool is_edp);
u8 drm_dp_dsc_sink_max_slice_count(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
				   bool is_edp);
u8 drm_dp_dsc_sink_line_buf_depth(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE]);
/* Fills dsc_bpc[] with 12/10/8 as supported, returns the count. */
int drm_dp_dsc_sink_supported_input_bpcs(const u8 dsc_dpc[DP_DSC_RECEIVER_CAP_SIZE],
					 u8 dsc_bpc[3]);
/* kPixels/s per slice, for a cumulative `peak_pixel_rate' in kHz. */
int drm_dp_dsc_sink_max_slice_throughput(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
					 int peak_pixel_rate, bool is_rgb_yuv444);
int drm_dp_dsc_branch_max_overall_throughput(const u8 dsc_branch_dpcd[DP_DSC_BRANCH_CAP_SIZE],
					     bool is_rgb_yuv444);
int drm_dp_dsc_branch_max_line_width(const u8 dsc_branch_dpcd[DP_DSC_BRANCH_CAP_SIZE]);

static inline bool
drm_dp_sink_supports_dsc(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	return dsc_dpcd[DP_DSC_SUPPORT - DP_DSC_SUPPORT] &
		DP_DSC_DECOMPRESSION_IS_SUPPORTED;
}

static inline u16
drm_edp_dsc_sink_output_bpp(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	return dsc_dpcd[DP_DSC_MAX_BITS_PER_PIXEL_LOW - DP_DSC_SUPPORT] |
		((dsc_dpcd[DP_DSC_MAX_BITS_PER_PIXEL_HI - DP_DSC_SUPPORT] &
		  DP_DSC_MAX_BITS_PER_PIXEL_HI_MASK) << 8);
}

static inline u32
drm_dp_dsc_sink_max_slice_width(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	/* Max Slicewidth = Number of Pixels * 320 */
	return dsc_dpcd[DP_DSC_MAX_SLICE_WIDTH - DP_DSC_SUPPORT] *
		DP_DSC_SLICE_WIDTH_MULTIPLIER;
}

/* Does the sink decompress DSC in this output format (DP_DSC_RGB, ...)? */
static inline bool
drm_dp_dsc_sink_supports_format(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE], u8 output_format)
{
	return dsc_dpcd[DP_DSC_DEC_COLOR_FORMAT_CAP - DP_DSC_SUPPORT] & output_format;
}

/* Forward Error Correction (DP 1.4), from the DP_FEC_CAPABILITY byte. */
static inline bool
drm_dp_sink_supports_fec(const u8 fec_capable)
{
	return fec_capable & DP_FEC_CAPABLE;
}

static inline bool
drm_dp_channel_coding_supported(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_MAIN_LINK_CHANNEL_CODING] & DP_CAP_ANSI_8B10B;
}

static inline bool
drm_dp_128b132b_supported(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_MAIN_LINK_CHANNEL_CODING] & DP_CAP_ANSI_128B132B;
}

static inline bool
drm_dp_alternate_scrambler_reset_cap(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_EDP_CONFIGURATION_CAP] &
			DP_ALTERNATE_SCRAMBLER_RESET_CAP;
}

/* The sink can ignore the MSA timing (DP 1.4): what Adaptive-Sync needs. */
static inline bool
drm_dp_sink_can_do_video_without_timing_msa(const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	return dpcd[DP_DOWN_STREAM_PORT_COUNT] &
		DP_MSA_TIMING_PAR_IGNORED;
}

/* The panel's TCON takes VESA backlight adjustment over AUX.  Panels that
 * have DPCD backlight features but need the level through the PWM pin
 * report false. */
static inline bool
drm_edp_backlight_supported(const u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE])
{
	return !!(edp_dpcd[1] & DP_EDP_TCON_BACKLIGHT_ADJUSTMENT_CAP);
}

/* A UHBR (128b/132b) rate, in 10 kbit/s units. */
static inline bool drm_dp_is_uhbr_rate(int link_rate)
{
	return link_rate >= 1000000;
}

/* ---- the AUX channel ----------------------------------------------------- */

/* One AUX transaction: `request' is a DP_AUX_* code (native or I2C, with
 * DP_AUX_I2C_MOT), `reply' is filled in by the transfer hook. */
struct drm_dp_aux_msg {
	unsigned int address;
	u8 request;
	u8 reply;
	void *buffer;
	size_t size;
};

/*
 * A DisplayPort AUX channel.  The driver fills in `name', `drm_dev' and
 * `transfer' (and the optional fields), then calls drm_dp_aux_init() --
 * or drm_dp_aux_register(), which also names the I2C adapter.  `ddc' is
 * then an I2C adapter that tunnels over this channel, for EDID reads.
 * I2C transfers use the largest message the sink takes; a partial reply
 * lowers the size for the rest of that message only.
 */
struct drm_dp_aux {
	/* Shown in logs and copied into the I2C adapter's name. */
	const char *name;

	/* I2C-over-AUX adapter; i2c_transfer(&aux->ddc, ...). */
	struct i2c_adapter ddc;

	/* The device that owns this channel (only used to tag messages);
	 * may be NULL until drm_dp_aux_register(). */
	struct drm_device *drm_dev;

	/* Serialises the transactions of this channel (a sleeping lock:
	 * transfers wait between retries).  Hardware shared between
	 * channels needs the driver's own locking on top. */
	mm_rwsem_t hw_mutex;

	/*
	 * Run one AUX transaction and return the number of payload bytes
	 * transferred, or a negative errno.  -EBUSY makes the helpers retry;
	 * other errors are passed up; a short transfer becomes -EPROTO for
	 * native accesses.  The hook may change only msg->reply.
	 *
	 * Called whatever the state of the device and panel: a powered-down
	 * AUX block powers itself up for the transfer, and an eDP panel that
	 * is off may make it fail, but not crash.
	 */
	ssize_t (*transfer)(struct drm_dp_aux *aux,
			    struct drm_dp_aux_msg *msg);

	/*
	 * Optional: wait until the HPD signal of this AUX channel is
	 * asserted (an eDP panel finishing its power-up), at most `wait_us'
	 * microseconds (0 = forever).  Returns 0, or -ETIMEDOUT without
	 * logging.  May sleep.
	 */
	int (*wait_hpd_asserted)(struct drm_dp_aux *aux, unsigned long wait_us);

	/* I2C NACKs and DEFERs seen, for DP compliance testing. */
	unsigned i2c_nack_count;
	unsigned i2c_defer_count;

	/* The endpoint is known to be powered down: accesses fail at once
	 * with -EBUSY instead of timing out (drm_dp_dpcd_set_powered()). */
	bool powered_down;

	/* The hardware cannot send zero-sized (address-only) transfers. */
	bool no_zero_sized;

	/* Skip the throw-away read before DPCD reads
	 * (drm_dp_dpcd_set_probe()). */
	bool dpcd_probe_disabled;

	/* drm_dp_aux_init() has run (the lock and the adapter are set up). */
	bool initialized;
};

/* Read one byte at `offset' and discard it (wakes a sleeping sink). */
int drm_dp_dpcd_probe(struct drm_dp_aux *aux, unsigned int offset);
/* aux may be NULL. */
void drm_dp_dpcd_set_powered(struct drm_dp_aux *aux, bool powered);
void drm_dp_dpcd_set_probe(struct drm_dp_aux *aux, bool enable);

/*
 * Read / write `size' DPCD bytes at `offset'.  Returns the byte count, or
 * -EIO when the sink NACKed or the retries ran out, -EPROTO for a short
 * transfer, or the transfer hook's error (except -EBUSY, which is
 * retried).  Most callers want the _data/_byte forms, which return 0.
 */
ssize_t drm_dp_dpcd_read(struct drm_dp_aux *aux, unsigned int offset,
			 void *buffer, size_t size);
ssize_t drm_dp_dpcd_write(struct drm_dp_aux *aux, unsigned int offset,
			  void *buffer, size_t size);

/* 1 on success or a negative errno. */
static inline ssize_t drm_dp_dpcd_readb(struct drm_dp_aux *aux,
					unsigned int offset, u8 *valuep)
{
	return drm_dp_dpcd_read(aux, offset, valuep, 1);
}

/* 0, or a negative errno (-EPROTO for a short read).  A failed
 * multi-byte read is retried one byte at a time: some USB-C hubs and
 * adapters (VIA VL817 based VGA adapters, the Dell DA310) fail block
 * reads but answer single-byte ones. */
static inline int drm_dp_dpcd_read_data(struct drm_dp_aux *aux,
					unsigned int offset,
					void *buffer, size_t size)
{
	int ret;
	size_t i;
	u8 *buf = buffer;

	ret = drm_dp_dpcd_read(aux, offset, buffer, size);
	if (ret >= 0) {
		if ((size_t)ret < size)
			return -EPROTO;
		return 0;
	}

	for (i = 0; i < size; i++) {
		ret = drm_dp_dpcd_readb(aux, offset + i, &buf[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* 0, or a negative errno (-EPROTO for a short write). */
static inline int drm_dp_dpcd_write_data(struct drm_dp_aux *aux,
					 unsigned int offset,
					 void *buffer, size_t size)
{
	int ret;

	ret = drm_dp_dpcd_write(aux, offset, buffer, size);
	if (ret < 0)
		return ret;
	if ((size_t)ret < size)
		return -EPROTO;

	return 0;
}

/* 1 on success or a negative errno. */
static inline ssize_t drm_dp_dpcd_writeb(struct drm_dp_aux *aux,
					 unsigned int offset, u8 value)
{
	return drm_dp_dpcd_write(aux, offset, &value, 1);
}

/* 0 or a negative errno. */
static inline int drm_dp_dpcd_read_byte(struct drm_dp_aux *aux,
					unsigned int offset, u8 *valuep)
{
	return drm_dp_dpcd_read_data(aux, offset, valuep, 1);
}

static inline int drm_dp_dpcd_write_byte(struct drm_dp_aux *aux,
					 unsigned int offset, u8 value)
{
	return drm_dp_dpcd_write_data(aux, offset, &value, 1);
}

/* The receiver capabilities (0x000..0x00e), replaced by the extended copy
 * at 0x2200 when the sink has one: 0 or a negative errno. */
int drm_dp_read_dpcd_caps(struct drm_dp_aux *aux,
			  u8 dpcd[DP_RECEIVER_CAP_SIZE]);

/* DP_LANE0_1_STATUS..DP_ADJUST_REQUEST_LANE2_3 of the DPRX, or of an
 * LTTPR converted to the same layout. */
int drm_dp_dpcd_read_link_status(struct drm_dp_aux *aux,
				 u8 status[DP_LINK_STATUS_SIZE]);
int drm_dp_dpcd_read_phy_link_status(struct drm_dp_aux *aux,
				     enum drm_dp_phy dp_phy,
				     u8 link_status[DP_LINK_STATUS_SIZE]);

/* DP_SET_POWER D0 (with the 1 ms the sink may take to wake) / D3. */
int drm_dp_link_power_up(struct drm_dp_aux *aux, unsigned char revision);
int drm_dp_link_power_down(struct drm_dp_aux *aux, unsigned char revision);

/* DP compliance: report the checksum of the last EDID block read. */
bool drm_dp_send_real_edid_checksum(struct drm_dp_aux *aux,
				    u8 real_edid_checksum);

/* ---- branch devices (downstream facing ports) ---------------------------- */

/* The DFP capability bytes (DP_DOWNSTREAM_PORT_0..), zeroed when there
 * are none. */
int drm_dp_read_downstream_info(struct drm_dp_aux *aux,
				const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				u8 downstream_ports[DP_MAX_DOWNSTREAM_PORTS]);
bool drm_dp_downstream_is_type(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			       const u8 port_cap[4], u8 type);
/* `drm_edid' (may be NULL) is the sink's EDID: a DP++ port with a
 * DisplayPort sink behind it is not TMDS. */
bool drm_dp_downstream_is_tmds(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			       const u8 port_cap[4],
			       const struct drm_edid *drm_edid);
/* Limits in kHz / bits per component; 0 when the port states none. */
int drm_dp_downstream_max_dotclock(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				   const u8 port_cap[4]);
int drm_dp_downstream_max_tmds_clock(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				     const u8 port_cap[4],
				     const struct drm_edid *drm_edid);
int drm_dp_downstream_min_tmds_clock(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				     const u8 port_cap[4],
				     const struct drm_edid *drm_edid);
int drm_dp_downstream_max_bpc(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			      const u8 port_cap[4],
			      const struct drm_edid *drm_edid);
bool drm_dp_downstream_420_passthrough(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				       const u8 port_cap[4]);
bool drm_dp_downstream_444_to_420_conversion(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
					     const u8 port_cap[4]);
bool drm_dp_downstream_rgb_to_ycbcr_conversion(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
					       const u8 port_cap[4], u8 color_spc);
/* The fixed mode of a port without EDID (a TV encoder): fills `*out' and
 * returns 0, or -ENOENT when the port has no such mode. */
int drm_dp_downstream_mode(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			   const u8 port_cap[4], struct drm_display_mode *out);
/* The 6-character branch device id: 0 or a negative errno. */
int drm_dp_downstream_id(struct drm_dp_aux *aux, char id[6]);
/* Log what the branch device reports (through kprintf). */
void drm_dp_downstream_debug(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			     const u8 port_cap[4],
			     const struct drm_edid *drm_edid,
			     struct drm_dp_aux *aux);
/* The value for the "subconnector" connector property. */
enum drm_mode_subconnector
drm_dp_subconnector_type(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			 const u8 port_cap[4]);

struct drm_dp_desc;
/* Is the sink count worth reading (not eDP, DPCD 1.1+, a branch, no
 * NO_SINK_COUNT quirk)? */
bool drm_dp_read_sink_count_cap(struct drm_connector *connector,
				const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				const struct drm_dp_desc *desc);
int drm_dp_read_sink_count(struct drm_dp_aux *aux);

/* ---- LTTPRs (link-training tunable PHY repeaters) ------------------------ */

int drm_dp_read_lttpr_common_caps(struct drm_dp_aux *aux,
				  const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				  u8 caps[DP_LTTPR_COMMON_CAP_SIZE]);
int drm_dp_read_lttpr_phy_caps(struct drm_dp_aux *aux,
			       const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			       enum drm_dp_phy dp_phy,
			       u8 caps[DP_LTTPR_PHY_CAP_SIZE]);
/* 0..8 repeaters, -ERANGE (more than 8) or -EINVAL (bad count field). */
int drm_dp_lttpr_count(const u8 cap[DP_LTTPR_COMMON_CAP_SIZE]);
int drm_dp_lttpr_max_link_rate(const u8 caps[DP_LTTPR_COMMON_CAP_SIZE]);
int drm_dp_lttpr_set_transparent_mode(struct drm_dp_aux *aux, bool enable);
/* Transparent, then (for a valid count) non-transparent mode. */
int drm_dp_lttpr_init(struct drm_dp_aux *aux, int lttpr_count);
int drm_dp_lttpr_max_lane_count(const u8 caps[DP_LTTPR_COMMON_CAP_SIZE]);
bool drm_dp_lttpr_voltage_swing_level_3_supported(const u8 caps[DP_LTTPR_PHY_CAP_SIZE]);
bool drm_dp_lttpr_pre_emphasis_level_3_supported(const u8 caps[DP_LTTPR_PHY_CAP_SIZE]);
/* Grant the extended wake time the sink or repeater asks for. */
void drm_dp_lttpr_wake_timeout_setup(struct drm_dp_aux *aux, bool transparent_mode);

/* ---- channel setup -------------------------------------------------------- */

/* Set up the lock and the I2C adapter; usable before registration. */
void drm_dp_aux_init(struct drm_dp_aux *aux);
/* drm_dp_aux_init() if needed, and name the I2C adapter after `name'. */
int drm_dp_aux_register(struct drm_dp_aux *aux);
/* The I2C adapter stops working (i2c_transfer() on it fails); DPCD
 * access through the channel still works. */
void drm_dp_aux_unregister(struct drm_dp_aux *aux);

/* ---- sink / branch identification and quirks ----------------------------- */

struct drm_dp_dpcd_ident {
	u8 oui[3];
	u8 device_id[6];
	u8 hw_rev;
	u8 sw_major_rev;
	u8 sw_minor_rev;
} __packed;

/* DPCD 0x400 (sink) or 0x500 (branch), with the quirks of that device. */
struct drm_dp_desc {
	struct drm_dp_dpcd_ident ident;
	u32 quirks;
};

int drm_dp_read_desc(struct drm_dp_aux *aux, struct drm_dp_desc *desc,
		     bool is_branch);

/* Read and log an LTTPR's identification. */
int drm_dp_dump_lttpr_desc(struct drm_dp_aux *aux, enum drm_dp_phy dp_phy);

/* Sink and branch device bugs; the table is shared, the workarounds are
 * up to each driver. */
enum drm_dp_quirk {
	/* Mvid/Nvid must fit 16 bits: use the constant N 0x8000. */
	DP_DPCD_QUIRK_CONSTANT_N,
	/* PSR claimed but not usable (or not handled yet). */
	DP_DPCD_QUIRK_NO_PSR,
	/* SINK_COUNT stays zero: ignore it on detect
	 * (drm_dp_read_sink_count_cap() checks this). */
	DP_DPCD_QUIRK_NO_SINK_COUNT,
	/* MST DSC without a virtual DPCD: read the DSC caps from the
	 * physical AUX. */
	DP_DPCD_QUIRK_DSC_WITHOUT_VIRTUAL_DPCD,
	/* 3.24 Gbps (multiplier 0xc) works although DP_MAX_LINK_RATE says
	 * less. */
	DP_DPCD_QUIRK_CAN_DO_MAX_LINK_RATE_3_24_GBPS,
	/* HBLANK expansion for some modes needs DSC enabled. */
	DP_DPCD_QUIRK_HBLANK_EXPANSION_REQUIRES_DSC,
	/* The compressed bpp must be limited above a device-specific DSC
	 * pixel throughput. */
	DP_DPCD_QUIRK_DSC_THROUGHPUT_BPP_LIMIT,
};

static inline bool
drm_dp_has_quirk(const struct drm_dp_desc *desc, enum drm_dp_quirk quirk)
{
	return desc->quirks & BIT(quirk);
}

/* ---- eDP backlight over AUX (VESA) --------------------------------------- */

/* What drm_edp_backlight_init() found.  Plain bools on purpose: `bool' is
 * a signed int here, and a one-bit field of it would read back -1. */
struct drm_edp_backlight_info {
	u8 pwmgen_bit_count;
	u8 pwm_freq_pre_divider;
	u32 max;

	bool lsb_reg_used; /* 16-bit level (MSB + LSB registers) */
	bool aux_enable; /* enable/disable through DPCD */
	bool aux_set; /* level through DPCD */
	bool luminance_set; /* level as luminance (nits) */
};

int
drm_edp_backlight_init(struct drm_dp_aux *aux, struct drm_edp_backlight_info *bl,
		       u32 max_luminance,
		       u16 driver_pwm_freq_hz, const u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE],
		       u32 *current_level, u8 *current_mode, bool need_luminance);
int drm_edp_backlight_set_level(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
				u32 level);
int drm_edp_backlight_enable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl,
			     u32 level);
int drm_edp_backlight_disable(struct drm_dp_aux *aux, const struct drm_edp_backlight_info *bl);

/* ---- PHY compliance test patterns ---------------------------------------- */

struct drm_dp_phy_test_params {
	int link_rate;
	u8 num_lanes;
	u8 phy_pattern;
	u8 hbr2_reset[2];
	u8 custom80[10];
	bool enhanced_frame_cap;
};

int drm_dp_get_phy_test_pattern(struct drm_dp_aux *aux,
				struct drm_dp_phy_test_params *data);
int drm_dp_set_phy_test_pattern(struct drm_dp_aux *aux,
				struct drm_dp_phy_test_params *data, u8 dp_rev);

/* ---- protocol converters (DP to HDMI 2.1 PCONs) -------------------------- */

int drm_dp_get_pcon_max_frl_bw(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			       const u8 port_cap[4]);
int drm_dp_pcon_frl_prepare(struct drm_dp_aux *aux, bool enable_frl_ready_hpd);
bool drm_dp_pcon_is_frl_ready(struct drm_dp_aux *aux);
int drm_dp_pcon_frl_configure_1(struct drm_dp_aux *aux, int max_frl_gbps,
				u8 frl_mode);
int drm_dp_pcon_frl_configure_2(struct drm_dp_aux *aux, int max_frl_mask,
				u8 frl_type);
int drm_dp_pcon_reset_frl_config(struct drm_dp_aux *aux);
int drm_dp_pcon_frl_enable(struct drm_dp_aux *aux);

bool drm_dp_pcon_hdmi_link_active(struct drm_dp_aux *aux);
int drm_dp_pcon_hdmi_link_mode(struct drm_dp_aux *aux, u8 *frl_trained_mask);
void drm_dp_pcon_hdmi_frl_link_error_count(struct drm_dp_aux *aux,
					   struct drm_connector *connector);
bool drm_dp_pcon_enc_is_dsc_1_2(const u8 pcon_dsc_dpcd[DP_PCON_DSC_ENCODER_CAP_SIZE]);
int drm_dp_pcon_dsc_max_slices(const u8 pcon_dsc_dpcd[DP_PCON_DSC_ENCODER_CAP_SIZE]);
int drm_dp_pcon_dsc_max_slice_width(const u8 pcon_dsc_dpcd[DP_PCON_DSC_ENCODER_CAP_SIZE]);
int drm_dp_pcon_dsc_bpp_incr(const u8 pcon_dsc_dpcd[DP_PCON_DSC_ENCODER_CAP_SIZE]);
int drm_dp_pcon_pps_default(struct drm_dp_aux *aux);
int drm_dp_pcon_pps_override_buf(struct drm_dp_aux *aux, u8 pps_buf[128]);
int drm_dp_pcon_pps_override_param(struct drm_dp_aux *aux, u8 pps_param[6]);
int drm_dp_pcon_convert_rgb_to_ycbcr(struct drm_dp_aux *aux, u8 color_spc);

/* ---- link bandwidth -------------------------------------------------------- */

#define DRM_DP_BW_OVERHEAD_MST		BIT(0)
#define DRM_DP_BW_OVERHEAD_UHBR		BIT(1)
#define DRM_DP_BW_OVERHEAD_SSC_REF_CLK	BIT(2)
#define DRM_DP_BW_OVERHEAD_FEC		BIT(3)
#define DRM_DP_BW_OVERHEAD_DSC		BIT(4)

/* The allocation overhead of a stream as 100% + overhead%, in ppm. */
int drm_dp_bw_overhead(int lane_count, int hactive,
		       int dsc_slice_count,
		       int bpp_x16, unsigned long flags);
/* 8b/10b or 128b/132b efficiency, in ppm. */
int drm_dp_bw_channel_coding_efficiency(bool is_uhbr);
/* The DPRX payload rate in kB/s for a link rate (10 kbit/s) and lanes. */
int drm_dp_max_dprx_data_rate(int max_link_rate, int max_lanes);

/* Pack an SDP into the generic layout: the length, or a negative errno. */
ssize_t drm_dp_vsc_sdp_pack(const struct drm_dp_vsc_sdp *vsc, struct dp_sdp *sdp);
ssize_t drm_dp_as_sdp_pack(const struct drm_dp_as_sdp *as_sdp,
			   struct dp_sdp *sdp, size_t size);
int drm_dp_link_symbol_cycles(int lane_count, int pixels, int dsc_slice_count,
			      int bpp_x16, int symbol_size, bool is_mst);

#endif /* KERNEL_DEV_GPU_DRM_DP_HELPER_H */
