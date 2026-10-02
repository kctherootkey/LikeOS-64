// LikeOS -- integrated DisplayPort and eDP before DDI.
//
// G4X put DisplayPort on its digital ports B, C and D; Ironlake moved
// those into the PCH (where Cougar Point keeps the training and pipe
// select fields elsewhere and lets the transcoder's DP control register
// carry the rest) and added a port A on the CPU for an embedded panel,
// clocked by its own small PLL in the port register.  Valleyview and
// Cherryview went back to the G4X register layout on a DPIO PHY that
// needs its lanes prepared around the pipe PLL, and Cherryview added a
// port D on its own PHY.  Every port has an AUX channel with a control
// register and five data registers, clocked from a divider of the raw
// (or, for port A, the CD) clock; native DPCD accesses and I2C to the
// sink's EDID go over it.  The link runs at 1.62 or 2.7 Gbit/s per lane
// from fixed PLL dividers, and is trained with patterns 1 and 2 while
// the sink reports how far clock recovery and equalisation got and asks
// for more swing or pre-emphasis.  An eDP panel needs its power
// sequencer for everything, including AUX before it is switched on.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2021 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- AUX channel registers ------------------------------------------------------- */

/* G4X and Valleyview/Cherryview (display-relative), and the CPU port A of
 * Ironlake to Ivy Bridge: 0x64010 + 0x100 * channel.  The PCH ports' at
 * 0xe4110 + 0x100 * (channel - B).  The five data registers follow the
 * control register. */
#define LGDP_AUX_CH_CTL(ch) (0x64010u + (uint32_t)(ch) * 0x100u)
#define LGDP_PCH_AUX_CH_CTL(ch) (0xe4110u + ((uint32_t)(ch) - 1u) * 0x100u)
#define LGDP_AUX_CH_DATA(ctl, i) ((ctl) + 4u + (uint32_t)(i) * 4u)
#define LGDP_AUX_CTL_SEND_BUSY (1u << 31)
#define LGDP_AUX_CTL_DONE (1u << 30)
#define LGDP_AUX_CTL_INTERRUPT (1u << 29)
#define LGDP_AUX_CTL_TIME_OUT_ERROR (1u << 28)
#define LGDP_AUX_CTL_TIME_OUT_400us (0u << 26)
#define LGDP_AUX_CTL_RECEIVE_ERROR (1u << 25)
#define LGDP_AUX_CTL_MESSAGE_SIZE_SHIFT 20
#define LGDP_AUX_CTL_MESSAGE_SIZE_MASK (0x1fu << 20)
#define LGDP_AUX_CTL_PRECHARGE_2US_SHIFT 16
#define LGDP_AUX_CTL_PRECHARGE_2US_MASK (0xfu << 16)
#define LGDP_AUX_CTL_BIT_CLOCK_2X_MASK 0x7ffu

/* ---- AUX protocol --------------------------------------------------------------- */

#define LGDP_AUX_I2C_WRITE 0x0
#define LGDP_AUX_I2C_READ 0x1
#define LGDP_AUX_I2C_WRITE_STATUS_UPDATE 0x2
#define LGDP_AUX_I2C_MOT 0x4
#define LGDP_AUX_NATIVE_WRITE 0x8
#define LGDP_AUX_NATIVE_READ 0x9
#define LGDP_AUX_NATIVE_REPLY_ACK 0x0
#define LGDP_AUX_NATIVE_REPLY_NACK 0x1
#define LGDP_AUX_NATIVE_REPLY_DEFER 0x2
#define LGDP_AUX_NATIVE_REPLY_MASK 0x3
#define LGDP_AUX_I2C_REPLY_ACK 0x0
#define LGDP_AUX_I2C_REPLY_NACK 0x4
#define LGDP_AUX_I2C_REPLY_DEFER 0x8
#define LGDP_AUX_I2C_REPLY_MASK 0xc
#define LGDP_AUX_MAX_PAYLOAD 16
#define LGDP_AUX_RETRY_INTERVAL_US 500

/* ---- DPCD fields not in the shared headers --------------------------------------- */

#define LGDP_DPCD_REV_11 0x11
#define LGDP_DPCD_REV_14 0x14
#define LGDP_EDP_14 0x03
#define LGDP_DWN_STRM_PORT_PRESENT 0x01
#define LGDP_DWN_STRM_PORT_TYPE_MASK 0x06
#define LGDP_DWN_STRM_PORT_TYPE_ANALOG (1u << 1)
#define LGDP_DWN_STRM_PORT_TYPE_OTHER (3u << 1)
#define LGDP_DETAILED_CAP_INFO_AVAILABLE 0x10
#define LGDP_TRAINING_AUX_RD_MASK 0x7f
#define LGDP_EXT_RECEIVER_CAP_PRESENT 0x80
#define LGDP_DP13_DPCD_REV 0x2200
#define LGDP_DEVICE_SERVICE_IRQ_VECTOR 0x201
#define LGDP_DOWNSTREAM_PORT_0 0x80
#define LGDP_DS_PORT_TYPE_MASK 0x7
#define LGDP_DS_PORT_TYPE_VGA 1
#define LGDP_DS_PORT_TYPE_NON_EDID 4
#define LGDP_DS_PORT_HPD (1u << 3)
#define LGDP_LINK_STATUS_SIZE 6
#define LGDP_EDP_DPCD_SIZE 5
#define LGDP_MAX_RATES 8

/* Swing and pre-emphasis levels as DP_TRAINING_LANEx_SET carries them. */
#define LGDP_SWING(n) ((uint8_t)(n))
#define LGDP_PREEMPH(n) ((uint8_t)((n) << DP_TRAIN_PRE_EMPHASIS_SHIFT))

/* ---- per-output state ------------------------------------------------------------- */

struct lg_dp {
	struct lg_display *d;
	struct lg_output *o;
	int is_edp;
	/* The port register as last programmed (the hardware's DP_DETECTED
	 * bit is kept from the read at prepare time). */
	uint32_t DP;
	/* AUX channel */
	int aux_ch;
	uint32_t aux_ctl;
	uint32_t aux_busy_last_status;
	struct i2c_adapter ddc;
	/* the sink's capabilities */
	uint8_t dpcd[DP_RECEIVER_CAP_SIZE];
	uint8_t edp_dpcd[LGDP_EDP_DPCD_SIZE];
	uint8_t ds_port[4]; /* first downstream port of a branch device */
	int has_dpcd;
	int sink_count;
	int sink_rates[LGDP_MAX_RATES];
	int num_sink_rates;
	int use_rate_select; /* eDP 1.4: LINK_RATE_SET instead of LINK_BW_SET */
	int max_sink_lanes;
	int source_rates[2];
	int num_source_rates;
	int common_rates[LGDP_MAX_RATES];
	int num_common_rates;
	int max_lanes;
	int edid_bpc;
	/* eDP before 1.4: panels train at one rate and width, their maximum */
	int use_max_params;
	/* after a failed training: the limits the next mode set keeps to
	 * (0 = none) */
	int fb_max_rate;
	int fb_max_lanes;
	/* the link */
	int link_rate;
	int lane_count;
	uint8_t train_set[4];
	int link_active;
	uint8_t voltage_max, preemph_max;
	/* eDP: forced VDD held by this file */
	int vdd_ref;
};

static const struct lg_output_funcs lg_dp_funcs;

static inline struct lg_dp *to_dp(struct lg_output *o)
{
	return (struct lg_dp *)o->priv;
}

static char port_name(int port)
{
	return (char)('A' + port);
}

static int dp_pch_split(struct lg_display *d)
{
	return !d->gmch;
}

static int dp_is_g4x(struct lg_display *d)
{
	return d->is_g4x || d->is_gm45;
}

/* ---- eDP panel VDD ---------------------------------------------------------------- */

/* The panel's logic needs power before its AUX channel answers.  The
 * sequencer's forced VDD is taken around every AUX conversation; longer
 * sequences (probe, EDID) hold it across, so it is raised once. */
static void edp_vdd_get(struct lg_dp *dp)
{
	if (!dp->is_edp)
		return;
	if (dp->vdd_ref++ == 0)
		lg_pps_vdd_on(dp->d, dp->o);
}

static void edp_vdd_put(struct lg_dp *dp)
{
	if (!dp->is_edp || dp->vdd_ref <= 0)
		return;
	if (--dp->vdd_ref == 0)
		lg_pps_vdd_off(dp->d, dp->o);
}

/* ---- AUX transfers ---------------------------------------------------------------- */

static uint32_t aux_pack(const uint8_t *src, int src_bytes)
{
	uint32_t v = 0;

	if (src_bytes > 4)
		src_bytes = 4;
	for (int i = 0; i < src_bytes; i++)
		v |= (uint32_t)src[i] << ((3 - i) * 8);
	return v;
}

static void aux_unpack(uint32_t src, uint8_t *dst, int dst_bytes)
{
	if (dst_bytes > 4)
		dst_bytes = 4;
	for (int i = 0; i < dst_bytes; i++)
		dst[i] = (uint8_t)(src >> ((3 - i) * 8));
}

/* The AUX bit clock should run at 2 MHz: the divider of the raw clock on
 * the GMCH parts and the PCH ports, of CDCLK for the CPU port A. */
static uint32_t aux_clock_divider(struct lg_dp *dp)
{
	struct lg_display *d = dp->d;
	uint32_t freq;

	if (dp_pch_split(d) && dp->aux_ch == LG_PORT_A)
		freq = d->cdclk_khz;
	else
		freq = d->rawclk_khz;
	return DIV_ROUND_CLOSEST(freq, 2000u);
}

/* Precharge is given in 2 us units beyond the 10 us minimum: the sync
 * pattern wants 16 us of precharge plus a 16 us preamble. */
static uint32_t aux_precharge_len(void)
{
	int precharge = 16, preamble = 16;
	int precharge_min = 10;

	return (uint32_t)((precharge + preamble - precharge_min - preamble) / 2);
}

static uint32_t aux_send_ctl(int send_bytes, uint32_t divider)
{
	/* The status-done interrupt is not enabled: completion is polled. */
	return LGDP_AUX_CTL_SEND_BUSY | LGDP_AUX_CTL_DONE |
	       LGDP_AUX_CTL_TIME_OUT_ERROR | LGDP_AUX_CTL_TIME_OUT_400us |
	       LGDP_AUX_CTL_RECEIVE_ERROR |
	       ((uint32_t)send_bytes << LGDP_AUX_CTL_MESSAGE_SIZE_SHIFT) |
	       (aux_precharge_len() << LGDP_AUX_CTL_PRECHARGE_2US_SHIFT) |
	       (divider & LGDP_AUX_CTL_BIT_CLOCK_2X_MASK);
}

static uint32_t aux_wait_done(struct lg_dp *dp)
{
	struct lg_display *d = dp->d;
	uint32_t status;

	if (lg_wait(d, dp->aux_ctl, LGDP_AUX_CTL_SEND_BUSY, 0, 10000)) {
		status = lg_rd(d, dp->aux_ctl);
		kprintf("[drm] i915: %s: AUX %c did not complete within 10 ms (status 0x%08x)\n",
			dp->o->name, port_name(dp->aux_ch), status);
		return status;
	}
	return lg_rd(d, dp->aux_ctl);
}

/* One transaction on the wire: `send' out, the reply into `recv'.
 * Returns the reply's length (header byte included) or a negative
 * errno. */
static int aux_xfer(struct lg_dp *dp, const uint8_t *send, int send_bytes,
		    uint8_t *recv, int recv_size)
{
	struct lg_display *d = dp->d;
	uint32_t ctl = dp->aux_ctl;
	uint32_t status = 0, divider;
	int try, ret, recv_bytes;

	if (send_bytes > 20 || recv_size > 20)
		return -E2BIG;

	/* A disconnected port does not answer; spare the long timeouts. */
	if (!dp->is_edp && dp->o->hpd_pin != LG_HPD_NONE &&
	    lg_hpd_live(d, dp->o->hpd_pin) == 0)
		return -ENXIO;

	edp_vdd_get(dp);

	/* Wait out any earlier activity on the channel. */
	for (try = 0; try < 3; try++) {
		status = lg_rd(d, ctl);
		if (!(status & LGDP_AUX_CTL_SEND_BUSY))
			break;
		lg_mdelay(1);
	}
	if (try == 3) {
		status = lg_rd(d, ctl);
		if (status != dp->aux_busy_last_status) {
			kprintf("[drm] i915: %s: AUX %c not started (status 0x%08x)\n",
				dp->o->name, port_name(dp->aux_ch), status);
			dp->aux_busy_last_status = status;
		}
		ret = -EBUSY;
		goto out;
	}

	divider = aux_clock_divider(dp);
	if (divider) {
		uint32_t send_ctl = aux_send_ctl(send_bytes, divider);

		/* The specification wants at least three attempts. */
		for (try = 0; try < 5; try++) {
			for (int i = 0; i < send_bytes; i += 4)
				lg_wr(d, LGDP_AUX_CH_DATA(ctl, i >> 2),
				      aux_pack(send + i, send_bytes - i));
			lg_wr(d, ctl, send_ctl);
			status = aux_wait_done(dp);
			/* clear the done bit and the errors */
			lg_wr(d, ctl, status | LGDP_AUX_CTL_DONE |
					      LGDP_AUX_CTL_TIME_OUT_ERROR |
					      LGDP_AUX_CTL_RECEIVE_ERROR);
			/* A timeout already took the 400 us the
			 * specification wants between attempts; a
			 * receive error has to wait for it. */
			if (status & LGDP_AUX_CTL_TIME_OUT_ERROR)
				continue;
			if (status & LGDP_AUX_CTL_RECEIVE_ERROR) {
				lg_udelay(450);
				continue;
			}
			if (status & LGDP_AUX_CTL_DONE)
				goto done;
		}
	}
	if (!(status & LGDP_AUX_CTL_DONE)) {
		kprintf("[drm] i915: %s: AUX %c not done (status 0x%08x)\n",
			dp->o->name, port_name(dp->aux_ch), status);
		ret = -EBUSY;
		goto out;
	}

done:
	if (status & LGDP_AUX_CTL_RECEIVE_ERROR) {
		kprintf("[drm] i915: %s: AUX %c receive error (status 0x%08x)\n",
			dp->o->name, port_name(dp->aux_ch), status);
		ret = -EIO;
		goto out;
	}
	/* Timeouts are what an absent sink gives: not worth the log. */
	if (status & LGDP_AUX_CTL_TIME_OUT_ERROR) {
		i915_dbg("[drm] i915: %s: AUX %c timeout (status 0x%08x)\n",
			 dp->o->name, port_name(dp->aux_ch), status);
		ret = -ETIMEDOUT;
		goto out;
	}
	recv_bytes = (int)((status & LGDP_AUX_CTL_MESSAGE_SIZE_MASK) >>
			   LGDP_AUX_CTL_MESSAGE_SIZE_SHIFT);
	/* Sizes of 0 or more than 20 are not allowed: try again. */
	if (recv_bytes == 0 || recv_bytes > 20) {
		i915_dbg("[drm] i915: %s: AUX %c forbidden reply size %d\n",
			 dp->o->name, port_name(dp->aux_ch), recv_bytes);
		ret = -EBUSY;
		goto out;
	}
	if (recv_bytes > recv_size)
		recv_bytes = recv_size;
	for (int i = 0; i < recv_bytes; i += 4)
		aux_unpack(lg_rd(d, LGDP_AUX_CH_DATA(ctl, i >> 2)), recv + i,
			   recv_bytes - i);
	ret = recv_bytes;
out:
	edp_vdd_put(dp);
	return ret;
}

/* One AUX message: a request at `address' with `size' payload bytes.
 * Returns the payload bytes moved (and the reply code in *reply) or a
 * negative errno. */
static int aux_transfer(struct lg_dp *dp, uint8_t request, uint32_t address,
			uint8_t *buffer, int size, uint8_t *reply)
{
	uint8_t txbuf[20], rxbuf[20];
	int txsize, rxsize, ret;

	txbuf[0] = (uint8_t)((request << 4) | ((address >> 16) & 0xf));
	txbuf[1] = (uint8_t)(address >> 8);
	txbuf[2] = (uint8_t)address;
	txbuf[3] = (uint8_t)(size - 1);

	switch (request & ~LGDP_AUX_I2C_MOT) {
	case LGDP_AUX_NATIVE_WRITE:
	case LGDP_AUX_I2C_WRITE:
	case LGDP_AUX_I2C_WRITE_STATUS_UPDATE:
		txsize = size ? 4 + size : 3;
		rxsize = 2; /* 0 or 1 data bytes */
		if (txsize > 20)
			return -E2BIG;
		if (size)
			mm_memcpy(txbuf + 4, buffer, (size_t)size);
		ret = aux_xfer(dp, txbuf, txsize, rxbuf, rxsize);
		if (ret > 0) {
			*reply = rxbuf[0] >> 4;
			if (ret > 1) {
				/* how much of a short write went through */
				ret = rxbuf[1];
				if (ret > size)
					ret = size;
			} else {
				ret = size;
			}
		}
		break;
	case LGDP_AUX_NATIVE_READ:
	case LGDP_AUX_I2C_READ:
		txsize = size ? 4 : 3;
		rxsize = size + 1;
		if (rxsize > 20)
			return -E2BIG;
		ret = aux_xfer(dp, txbuf, txsize, rxbuf, rxsize);
		if (ret > 0) {
			*reply = rxbuf[0] >> 4;
			ret--;
			if (ret)
				mm_memcpy(buffer, rxbuf + 1, (size_t)ret);
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

/* A native DPCD access of up to 16 bytes, retried while the sink defers
 * or the channel hiccups.  The first error seen is the one returned. */
static int dpcd_access_chunk(struct lg_dp *dp, uint8_t request, uint32_t addr,
			     uint8_t *buf, int size)
{
	int ret = 0, err = 0;

	for (int retry = 0; retry < 32; retry++) {
		uint8_t reply = 0;

		if (ret != 0 && ret != -ETIMEDOUT)
			lg_udelay(LGDP_AUX_RETRY_INTERVAL_US);
		ret = aux_transfer(dp, request, addr, buf, size, &reply);
		if (ret == -ENXIO)
			return ret;
		if (ret >= 0) {
			if ((reply & LGDP_AUX_NATIVE_REPLY_MASK) == LGDP_AUX_NATIVE_REPLY_ACK) {
				if (ret == size)
					return ret;
				ret = -EPROTO;
			} else {
				ret = -EIO;
			}
		}
		if (!err)
			err = ret;
	}
	return err;
}

static int dpcd_read(struct lg_dp *dp, uint32_t addr, uint8_t *buf, int len)
{
	int done = 0;

	while (done < len) {
		int chunk = len - done > LGDP_AUX_MAX_PAYLOAD ? LGDP_AUX_MAX_PAYLOAD : len - done;
		int ret = dpcd_access_chunk(dp, LGDP_AUX_NATIVE_READ, addr + (uint32_t)done,
					    buf + done, chunk);
		if (ret < 0)
			return ret;
		done += ret;
	}
	return done;
}

static int dpcd_write(struct lg_dp *dp, uint32_t addr, const uint8_t *buf, int len)
{
	uint8_t tmp[LGDP_AUX_MAX_PAYLOAD];
	int done = 0;

	while (done < len) {
		int chunk = len - done > LGDP_AUX_MAX_PAYLOAD ? LGDP_AUX_MAX_PAYLOAD : len - done;
		int ret;

		mm_memcpy(tmp, buf + done, (size_t)chunk);
		ret = dpcd_access_chunk(dp, LGDP_AUX_NATIVE_WRITE, addr + (uint32_t)done,
					tmp, chunk);
		if (ret < 0)
			return ret;
		done += ret;
	}
	return done;
}

static int dpcd_writeb(struct lg_dp *dp, uint32_t addr, uint8_t v)
{
	return dpcd_write(dp, addr, &v, 1);
}

/* ---- I2C over AUX ------------------------------------------------------------------- */

static void i2c_set_request(uint8_t *request, const struct i2c_msg *m)
{
	*request = (uint8_t)(((m->flags & I2C_M_RD) ? LGDP_AUX_I2C_READ : LGDP_AUX_I2C_WRITE) |
			     LGDP_AUX_I2C_MOT);
}

/* After a short or deferred write the sink is asked how far it got. */
static void i2c_write_status_update(uint8_t *request)
{
	if ((*request & ~LGDP_AUX_I2C_MOT) == LGDP_AUX_I2C_WRITE)
		*request = (uint8_t)((*request & LGDP_AUX_I2C_MOT) |
				     LGDP_AUX_I2C_WRITE_STATUS_UPDATE);
}

/* One I2C-over-AUX message with its native and I2C level retries. */
static int i2c_do_msg(struct lg_dp *dp, uint8_t *request, uint16_t addr,
		      uint8_t *buf, int size)
{
	int max_retries = 7, defer_i2c = 0;

	for (int retry = 0; retry < max_retries + defer_i2c; retry++) {
		uint8_t reply = 0;
		int ret = aux_transfer(dp, *request, addr, buf, size, &reply);

		if (ret < 0) {
			if (ret == -EBUSY)
				continue;
			/* timeouts and the rest are not retried here */
			return ret;
		}
		switch (reply & LGDP_AUX_NATIVE_REPLY_MASK) {
		case LGDP_AUX_NATIVE_REPLY_ACK:
			break;
		case LGDP_AUX_NATIVE_REPLY_NACK:
			return -EIO;
		case LGDP_AUX_NATIVE_REPLY_DEFER:
			lg_udelay(LGDP_AUX_RETRY_INTERVAL_US);
			continue;
		default:
			return -EIO;
		}
		switch (reply & LGDP_AUX_I2C_REPLY_MASK) {
		case LGDP_AUX_I2C_REPLY_ACK:
			if (ret != size)
				i2c_write_status_update(request);
			return ret;
		case LGDP_AUX_I2C_REPLY_NACK:
			return -EIO;
		case LGDP_AUX_I2C_REPLY_DEFER:
			/* The sink's I2C side is slow: grant it more
			 * attempts than a native defer gets. */
			if (defer_i2c < 7)
				defer_i2c++;
			lg_udelay(LGDP_AUX_RETRY_INTERVAL_US);
			i2c_write_status_update(request);
			continue;
		default:
			return -EIO;
		}
	}
	return -EIO;
}

/* Keep sending until the whole chunk went, however short the replies. */
static int i2c_drain_msg(struct lg_dp *dp, uint8_t *request, uint16_t addr,
			 uint8_t *buf, int size)
{
	int done = 0;

	while (size > 0) {
		int ret = i2c_do_msg(dp, request, addr, buf + done, size);

		if (ret <= 0)
			return ret == 0 ? -EPROTO : ret;
		done += ret;
		size -= ret;
	}
	return done;
}

static int dp_i2c_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int num)
{
	struct lg_dp *dp = a->priv;
	uint8_t request = 0;
	uint16_t addr = 0;
	int err = 0;

	edp_vdd_get(dp);
	for (int i = 0; i < num; i++) {
		int transfer_size = LGDP_AUX_MAX_PAYLOAD;

		addr = msgs[i].addr;
		i2c_set_request(&request, &msgs[i]);
		/* a bare address starts the transaction */
		err = i2c_do_msg(dp, &request, addr, NULL, 0);
		i2c_set_request(&request, &msgs[i]);
		if (err < 0)
			break;
		/* As large as the sink takes, smaller after a short
		 * reply. */
		for (int j = 0; j < msgs[i].len;) {
			int size = msgs[i].len - j;

			if (size > transfer_size)
				size = transfer_size;
			err = i2c_do_msg(dp, &request, addr, msgs[i].buf + j, size);
			if (err > 0 && err < size) {
				int rest = i2c_drain_msg(dp, &request, addr,
							 msgs[i].buf + j + err, size - err);
				if (rest < 0) {
					err = rest;
				} else {
					transfer_size = err;
					err += rest;
				}
			}
			i2c_set_request(&request, &msgs[i]);
			if (err < 0)
				break;
			if (err == 0) {
				err = -EPROTO;
				break;
			}
			j += err;
		}
		if (err < 0)
			break;
	}
	if (err >= 0)
		err = num;
	/* a bare address without MOT closes the transaction */
	request &= (uint8_t)~LGDP_AUX_I2C_MOT;
	(void)i2c_do_msg(dp, &request, addr, NULL, 0);
	edp_vdd_put(dp);
	return err;
}

/* ---- sink capabilities -------------------------------------------------------------- */

static int dp_branch(const struct lg_dp *dp)
{
	return dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] & LGDP_DWN_STRM_PORT_PRESENT;
}

static int dp_enhanced_frame_cap(const struct lg_dp *dp)
{
	return dp->dpcd[DP_DPCD_REV] >= LGDP_DPCD_REV_11 &&
	       (dp->dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP);
}

static int dp_has_sink_count(const struct lg_dp *dp)
{
	return dp->dpcd[DP_DPCD_REV] >= LGDP_DPCD_REV_11 && dp_branch(dp);
}

/* The receiver capabilities, the extended copy at 0x2200 preferred when
 * the sink has one. */
static int dp_read_dpcd_caps(struct lg_dp *dp)
{
	uint8_t dpcd[DP_RECEIVER_CAP_SIZE], ext[DP_RECEIVER_CAP_SIZE];
	uint8_t probe;

	/* Some sinks need a first access to wake their AUX side up. */
	(void)dpcd_read(dp, DP_DPCD_REV, &probe, 1);
	if (dpcd_read(dp, DP_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE) != DP_RECEIVER_CAP_SIZE)
		return -EIO;
	if (dpcd[DP_DPCD_REV] == 0)
		return -EIO;
	if ((dpcd[DP_TRAINING_AUX_RD_INTERVAL] & LGDP_EXT_RECEIVER_CAP_PRESENT) &&
	    dpcd_read(dp, LGDP_DP13_DPCD_REV, ext, DP_RECEIVER_CAP_SIZE) == DP_RECEIVER_CAP_SIZE &&
	    ext[DP_DPCD_REV] >= dpcd[DP_DPCD_REV])
		mm_memcpy(dpcd, ext, sizeof(dpcd));
	mm_memcpy(dp->dpcd, dpcd, sizeof(dpcd));
	return 0;
}

static void dp_set_sink_rates(struct lg_dp *dp)
{
	static const int dp_rates[] = { 162000, 270000, 540000, 810000 };
	int max_rate = dp->dpcd[DP_MAX_LINK_RATE] * 27000;
	int i;

	dp->num_sink_rates = 0;
	for (i = 0; i < (int)(sizeof(dp_rates) / sizeof(dp_rates[0])); i++) {
		if (dp_rates[i] > max_rate)
			break;
		dp->sink_rates[dp->num_sink_rates++] = dp_rates[i];
	}
	if (!dp->num_sink_rates) {
		kprintf("[drm] i915: %s: DPCD names no link rate, assuming 1.62 Gbit/s\n",
			dp->o->name);
		dp->sink_rates[0] = 162000;
		dp->num_sink_rates = 1;
	}
}

/* eDP 1.4 panels may list their rates (in 200 kHz units of the bit
 * rate); otherwise the DPCD maximum stands. */
static void edp_set_sink_rates(struct lg_dp *dp)
{
	dp->use_rate_select = 0;
	dp->num_sink_rates = 0;
	if (dp->edp_dpcd[0] >= LGDP_EDP_14) {
		uint8_t raw[16];

		if (dpcd_read(dp, DP_SUPPORTED_LINK_RATES, raw, sizeof(raw)) == (int)sizeof(raw)) {
			for (int i = 0; i < LGDP_MAX_RATES; i++) {
				int val = raw[2 * i] | (raw[2 * i + 1] << 8);

				if (!val)
					break;
				/* bits per second to the link symbol clock */
				dp->sink_rates[dp->num_sink_rates++] = (val * 200) / 10;
			}
		}
		if (dp->num_sink_rates) {
			dp->use_rate_select = 1;
			return;
		}
	}
	dp_set_sink_rates(dp);
}

static void dp_set_max_sink_lanes(struct lg_dp *dp)
{
	int lanes = dp->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;

	if (lanes != 1 && lanes != 2 && lanes != 4) {
		kprintf("[drm] i915: %s: DPCD names %d lanes, assuming 1\n", dp->o->name, lanes);
		lanes = 1;
	}
	dp->max_sink_lanes = lanes;
}

/* What the port can do: 1.62 and 2.7 Gbit/s on every part before DDI,
 * capped by what the VBT allows an eDP panel. */
static void dp_set_source_rates(struct lg_dp *dp)
{
	struct lg_display *d = dp->d;
	int max_rate = 0;

	dp->source_rates[0] = 162000;
	dp->source_rates[1] = 270000;
	dp->num_source_rates = 2;
	if (dp->is_edp && d->vbt.edp_rate_khz)
		max_rate = (int)d->vbt.edp_rate_khz;
	if (max_rate) {
		while (dp->num_source_rates > 1 &&
		       dp->source_rates[dp->num_source_rates - 1] > max_rate)
			dp->num_source_rates--;
	}
}

static void dp_set_common_link_params(struct lg_dp *dp)
{
	int i = 0, j = 0, k = 0;

	while (i < dp->num_source_rates && j < dp->num_sink_rates && k < LGDP_MAX_RATES) {
		if (dp->source_rates[i] == dp->sink_rates[j]) {
			dp->common_rates[k++] = dp->source_rates[i];
			i++;
			j++;
		} else if (dp->source_rates[i] < dp->sink_rates[j]) {
			i++;
		} else {
			j++;
		}
	}
	if (!k)
		dp->common_rates[k++] = 162000;
	dp->num_common_rates = k;

	dp->max_lanes = 4;
	if (dp->max_sink_lanes < dp->max_lanes)
		dp->max_lanes = dp->max_sink_lanes;
	if (dp->is_edp && dp->d->vbt.edp_lanes > 0 && dp->d->vbt.edp_lanes < dp->max_lanes)
		dp->max_lanes = dp->d->vbt.edp_lanes;
	if (dp->max_lanes < 1)
		dp->max_lanes = 1;
}

/* The largest rate and width a mode set may use, after any training
 * failure narrowed them. */
static void dp_link_limits(const struct lg_dp *dp, int *max_rate, int *max_lanes)
{
	int rate = dp->common_rates[dp->num_common_rates - 1];
	int lanes = dp->max_lanes;

	if (dp->fb_max_rate && dp->fb_max_rate < rate) {
		rate = dp->common_rates[0];
		for (int i = 0; i < dp->num_common_rates; i++)
			if (dp->common_rates[i] <= dp->fb_max_rate)
				rate = dp->common_rates[i];
	}
	if (dp->fb_max_lanes && dp->fb_max_lanes < lanes)
		lanes = dp->fb_max_lanes;
	*max_rate = rate;
	*max_lanes = lanes;
}

static void dp_reset_link_params(struct lg_dp *dp)
{
	dp->fb_max_rate = 0;
	dp->fb_max_lanes = 0;
	dp->use_max_params = dp->is_edp && dp->edp_dpcd[0] < LGDP_EDP_14;
}

/* The receiver capabilities of an external sink, and what follows from
 * them; 0 when a sink is there. */
static int dp_get_dpcd(struct lg_dp *dp)
{
	uint8_t count;

	if (dp_read_dpcd_caps(dp))
		return -EIO;
	dp_set_sink_rates(dp);
	dp_set_max_sink_lanes(dp);
	dp_set_common_link_params(dp);

	mm_memset(dp->ds_port, 0, sizeof(dp->ds_port));
	if (dp_has_sink_count(dp)) {
		if (dpcd_read(dp, DP_SINK_COUNT, &count, 1) != 1)
			return -EIO;
		/* bit 7 is the sink count's bit 6 */
		dp->sink_count = ((count & 0x80) >> 1) | (count & 0x3f);
		/* a dongle with nothing behind it */
		if (!dp->sink_count)
			return -ENODEV;
	}
	if (dp_branch(dp)) {
		int len = (dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] & LGDP_DETAILED_CAP_INFO_AVAILABLE) ?
				  4 : 1;
		if (dpcd_read(dp, LGDP_DOWNSTREAM_PORT_0, dp->ds_port, len) != len)
			return -EIO;
	}
	return 0;
}

static void dp_note_edid(struct lg_dp *dp)
{
	struct lg_output *o = dp->o;
	struct drm_edid_info *info;

	dp->edid_bpc = 0;
	if (!o->edid_len)
		return;
	info = kalloc(sizeof(*info));
	if (!info)
		return;
	if (drm_edid_parse(o->edid, (unsigned)o->edid_len, info) == 0)
		dp->edid_bpc = info->bpc;
	kfree(info);
}

/* ---- sink power -------------------------------------------------------------------- */

static int dp_downstream_hpd_needs_d0(const struct lg_dp *dp)
{
	/* DPCD 1.2 branch devices signal downstream changes with a long
	 * pulse even in D3; 1.1 ones with HPD-aware ports must stay up. */
	return dp->dpcd[DP_DPCD_REV] == LGDP_DPCD_REV_11 && dp_branch(dp) &&
	       (dp->ds_port[0] & LGDP_DS_PORT_HPD);
}

static void dp_set_power(struct lg_dp *dp, uint8_t mode)
{
	int ret = 0;

	if (dp->dpcd[DP_DPCD_REV] < LGDP_DPCD_REV_11)
		return;
	if (mode != DP_SET_POWER_D0) {
		if (dp_downstream_hpd_needs_d0(dp))
			return;
		ret = dpcd_writeb(dp, DP_SET_POWER, mode);
	} else {
		/* The sink may take a millisecond to wake up. */
		for (int i = 0; i < 3; i++) {
			ret = dpcd_writeb(dp, DP_SET_POWER, mode);
			if (ret == 1)
				break;
			lg_mdelay(1);
		}
	}
	if (ret != 1)
		i915_dbg("[drm] i915: %s: setting sink power to %s failed\n", dp->o->name,
			 mode == DP_SET_POWER_D0 ? "D0" : "D3");
}

/* ---- the port register -------------------------------------------------------------- */

static void dp_write_port(struct lg_dp *dp)
{
	lg_wr(dp->d, dp->o->reg, dp->DP);
	lg_posting_read(dp->d, dp->o->reg);
}

/* Ivy Bridge's port A and the Cougar Point ports keep the training
 * pattern in bits 8..10. */
static int dp_cpt_training(const struct lg_dp *dp)
{
	struct lg_display *d = dp->d;

	return (d->is_ivb && dp->o->port == LG_PORT_A) ||
	       (d->pch == LG_PCH_CPT && dp->o->port != LG_PORT_A);
}

static void dp_set_link_train(struct lg_dp *dp, uint8_t dp_train_pat)
{
	uint8_t pat = dp_train_pat & 0x3;

	if (dp_cpt_training(dp)) {
		dp->DP &= ~DP_LINK_TRAIN_MASK_CPT;
		switch (pat) {
		case DP_TRAINING_PATTERN_DISABLE:
			dp->DP |= DP_LINK_TRAIN_OFF_CPT;
			break;
		case DP_TRAINING_PATTERN_1:
			dp->DP |= DP_LINK_TRAIN_PAT_1_CPT;
			break;
		case DP_TRAINING_PATTERN_2:
			dp->DP |= DP_LINK_TRAIN_PAT_2_CPT;
			break;
		default:
			return;
		}
	} else {
		dp->DP &= ~DP_LINK_TRAIN_MASK;
		switch (pat) {
		case DP_TRAINING_PATTERN_DISABLE:
			dp->DP |= DP_LINK_TRAIN_OFF;
			break;
		case DP_TRAINING_PATTERN_1:
			dp->DP |= DP_LINK_TRAIN_PAT_1;
			break;
		case DP_TRAINING_PATTERN_2:
			dp->DP |= DP_LINK_TRAIN_PAT_2;
			break;
		default:
			return;
		}
	}
	dp_write_port(dp);
}

static void dp_set_idle_link_train(struct lg_dp *dp)
{
	if (dp_cpt_training(dp)) {
		dp->DP &= ~DP_LINK_TRAIN_MASK_CPT;
		dp->DP |= DP_LINK_TRAIN_PAT_IDLE_CPT;
	} else {
		dp->DP &= ~DP_LINK_TRAIN_MASK;
		dp->DP |= DP_LINK_TRAIN_PAT_IDLE;
	}
	dp_write_port(dp);
}

/* The port register for the configuration: width, framing, sync
 * polarities and the pipe, in whichever of the layouts the port has. */
static void dp_prepare(struct lg_dp *dp, const struct lg_config *cfg)
{
	struct lg_display *d = dp->d;
	int port = dp->o->port;
	int pipe = cfg->pipe;

	/* the link parameters the training will use */
	mm_memset(dp->train_set, 0, sizeof(dp->train_set));
	dp->link_active = 0;
	dp->link_rate = (int)cfg->port_clock;
	dp->lane_count = cfg->lane_count;

	/* Keep the detected bit the firmware left; it is read-only. */
	dp->DP = lg_rd(d, dp->o->reg) & DP_DETECTED;
	dp->DP |= DP_VOLTAGE_0_4 | DP_PRE_EMPHASIS_0;
	dp->DP |= DP_PORT_WIDTH(cfg->lane_count);

	if (d->is_ivb && port == LG_PORT_A) {
		if (cfg->mode.flags & DRM_MODE_FLAG_PHSYNC)
			dp->DP |= DP_SYNC_HS_HIGH;
		if (cfg->mode.flags & DRM_MODE_FLAG_PVSYNC)
			dp->DP |= DP_SYNC_VS_HIGH;
		dp->DP |= DP_LINK_TRAIN_OFF_CPT;
		if (cfg->enhanced_framing)
			dp->DP |= DP_ENHANCED_FRAMING;
		dp->DP |= DP_PIPE_SEL_IVB(pipe);
	} else if (d->pch == LG_PCH_CPT && port != LG_PORT_A) {
		/* Cougar Point: sync, framing and pipe live in the
		 * transcoder's DP control register. */
		dp->DP |= DP_LINK_TRAIN_OFF_CPT;
		lg_rmw(d, TRANS_DP_CTL(pipe), TRANS_DP_ENH_FRAMING,
		       cfg->enhanced_framing ? TRANS_DP_ENH_FRAMING : 0);
	} else {
		if (dp_is_g4x(d) && cfg->limited_color_range)
			dp->DP |= DP_COLOR_RANGE_16_235;
		if (cfg->mode.flags & DRM_MODE_FLAG_PHSYNC)
			dp->DP |= DP_SYNC_HS_HIGH;
		if (cfg->mode.flags & DRM_MODE_FLAG_PVSYNC)
			dp->DP |= DP_SYNC_VS_HIGH;
		dp->DP |= DP_LINK_TRAIN_OFF;
		if (cfg->enhanced_framing)
			dp->DP |= DP_ENHANCED_FRAMING;
		if (d->is_chv)
			dp->DP |= DP_PIPE_SEL_CHV(pipe);
		else
			dp->DP |= DP_PIPE_SEL(pipe);
	}
}

static int dp_pipe_running(struct lg_display *d, int pipe)
{
	if (pipe < 0 || pipe >= d->num_pipes)
		return 0;
	return !!(lg_rd(d, PIPECONF(d, pipe)) & PIPECONF_ENABLE);
}

/* Port A's own PLL (Ironlake to Ivy Bridge), set to the link rate
 * before the port and pipe come up. */
static void ilk_edp_pll_on(struct lg_dp *dp, const struct lg_config *cfg)
{
	struct lg_display *d = dp->d;

	i915_dbg("[drm] i915: enabling eDP PLL for clock %u\n", cfg->port_clock);
	dp->DP &= ~EDP_PLL_FREQ_MASK;
	if (cfg->port_clock == 162000)
		dp->DP |= EDP_PLL_FREQ_162MHZ;
	else
		dp->DP |= EDP_PLL_FREQ_270MHZ;
	lg_wr(d, DP_A, dp->DP);
	lg_posting_read(d, DP_A);
	lg_udelay(500);

	/* Ironlake: with the other pipe running into FDI, the PLL may only
	 * be switched on at the start of its vertical blank. */
	if (d->is_ilk && dp_pipe_running(d, !cfg->pipe))
		lg_wait_for_vblank(d, !cfg->pipe);

	dp->DP |= EDP_PLL_ENABLE;
	lg_wr(d, DP_A, dp->DP);
	lg_posting_read(d, DP_A);
	lg_udelay(200);
}

static void ilk_edp_pll_off(struct lg_dp *dp)
{
	struct lg_display *d = dp->d;

	i915_dbg("[drm] i915: disabling eDP PLL\n");
	dp->DP &= ~EDP_PLL_ENABLE;
	lg_wr(d, DP_A, dp->DP);
	lg_posting_read(d, DP_A);
	lg_udelay(200);
}

/* Cougar Point ports: the pipe is the transcoder whose DP control
 * selects the port. */
static int cpt_dp_port_selected(struct lg_display *d, int port, int *pipe)
{
	for (int p = 0; p < d->num_pipes; p++) {
		uint32_t val = lg_rd(d, TRANS_DP_CTL(p));

		if ((val & TRANS_DP_PORT_SEL_MASK) == TRANS_DP_PORT_SEL(port - LG_PORT_B)) {
			*pipe = p;
			return 1;
		}
	}
	*pipe = 0;
	return 0;
}

static int dp_port_enabled(struct lg_display *d, uint32_t reg, int port, int *pipe)
{
	uint32_t val = lg_rd(d, reg);
	int ret = !!(val & DP_PORT_EN);

	if (d->is_ivb && port == LG_PORT_A)
		*pipe = (int)((val & DP_PIPE_SEL_MASK_IVB) >> 29);
	else if (d->pch == LG_PCH_CPT && port != LG_PORT_A)
		ret &= cpt_dp_port_selected(d, port, pipe);
	else if (d->is_chv)
		*pipe = (int)((val & DP_PIPE_SEL_MASK_CHV) >> DP_PIPE_SEL_SHIFT_CHV);
	else
		*pipe = (int)((val & DP_PIPE_SEL_MASK) >> 30);
	return ret;
}

static int dp_get_hw_state(struct lg_display *d, struct lg_output *o, int *pipe)
{
	int p = 0;
	int ret = dp_port_enabled(d, o->reg, o->port, &p);

	if (pipe)
		*pipe = p;
	return ret;
}

/* ---- signal levels ------------------------------------------------------------------ */

static uint32_t g4x_signal_levels(uint8_t train_set)
{
	uint32_t signal_levels = 0;

	switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
	case LGDP_SWING(0):
	default:
		signal_levels |= DP_VOLTAGE_0_4;
		break;
	case LGDP_SWING(1):
		signal_levels |= DP_VOLTAGE_0_6;
		break;
	case LGDP_SWING(2):
		signal_levels |= DP_VOLTAGE_0_8;
		break;
	case LGDP_SWING(3):
		signal_levels |= DP_VOLTAGE_1_2;
		break;
	}
	switch (train_set & DP_TRAIN_PRE_EMPHASIS_MASK) {
	case LGDP_PREEMPH(0):
	default:
		signal_levels |= DP_PRE_EMPHASIS_0;
		break;
	case LGDP_PREEMPH(1):
		signal_levels |= DP_PRE_EMPHASIS_3_5;
		break;
	case LGDP_PREEMPH(2):
		signal_levels |= DP_PRE_EMPHASIS_6;
		break;
	case LGDP_PREEMPH(3):
		signal_levels |= DP_PRE_EMPHASIS_9_5;
		break;
	}
	return signal_levels;
}

/* Sandy Bridge's port A: one field for swing and emphasis together. */
static uint32_t snb_cpu_edp_signal_levels(uint8_t train_set)
{
	uint8_t signal_levels = train_set & (DP_TRAIN_VOLTAGE_SWING_MASK | DP_TRAIN_PRE_EMPHASIS_MASK);

	switch (signal_levels) {
	case LGDP_SWING(0) | LGDP_PREEMPH(0):
	case LGDP_SWING(1) | LGDP_PREEMPH(0):
		return EDP_LINK_TRAIN_400_600MV_0DB_SNB_B;
	case LGDP_SWING(0) | LGDP_PREEMPH(1):
		return EDP_LINK_TRAIN_400MV_3_5DB_SNB_B;
	case LGDP_SWING(0) | LGDP_PREEMPH(2):
	case LGDP_SWING(1) | LGDP_PREEMPH(2):
		return EDP_LINK_TRAIN_400_600MV_6DB_SNB_B;
	case LGDP_SWING(1) | LGDP_PREEMPH(1):
	case LGDP_SWING(2) | LGDP_PREEMPH(1):
		return EDP_LINK_TRAIN_600_800MV_3_5DB_SNB_B;
	case LGDP_SWING(2) | LGDP_PREEMPH(0):
	case LGDP_SWING(3) | LGDP_PREEMPH(0):
		return EDP_LINK_TRAIN_800_1200MV_0DB_SNB_B;
	default:
		i915_dbg("[drm] i915: unsupported SNB eDP signal levels 0x%x\n", signal_levels);
		return EDP_LINK_TRAIN_400_600MV_0DB_SNB_B;
	}
}

/* Ivy Bridge's port A. */
static uint32_t ivb_cpu_edp_signal_levels(uint8_t train_set)
{
	uint8_t signal_levels = train_set & (DP_TRAIN_VOLTAGE_SWING_MASK | DP_TRAIN_PRE_EMPHASIS_MASK);

	switch (signal_levels) {
	case LGDP_SWING(0) | LGDP_PREEMPH(0):
		return EDP_LINK_TRAIN_400MV_0DB_IVB;
	case LGDP_SWING(0) | LGDP_PREEMPH(1):
		return EDP_LINK_TRAIN_400MV_3_5DB_IVB;
	case LGDP_SWING(0) | LGDP_PREEMPH(2):
	case LGDP_SWING(1) | LGDP_PREEMPH(2):
		return EDP_LINK_TRAIN_400MV_6DB_IVB;
	case LGDP_SWING(1) | LGDP_PREEMPH(0):
		return EDP_LINK_TRAIN_600MV_0DB_IVB;
	case LGDP_SWING(1) | LGDP_PREEMPH(1):
		return EDP_LINK_TRAIN_600MV_3_5DB_IVB;
	case LGDP_SWING(2) | LGDP_PREEMPH(0):
		return EDP_LINK_TRAIN_800MV_0DB_IVB;
	case LGDP_SWING(2) | LGDP_PREEMPH(1):
		return EDP_LINK_TRAIN_800MV_3_5DB_IVB;
	default:
		i915_dbg("[drm] i915: unsupported IVB eDP signal levels 0x%x\n", signal_levels);
		return EDP_LINK_TRAIN_500MV_0DB_IVB;
	}
}

/* Valleyview: the PHY's de-emphasis, pre-emphasis and transmit scale
 * for each requested level. */
static void vlv_set_signal_levels(struct lg_dp *dp, const struct lg_config *cfg)
{
	uint32_t demph_reg_value, preemph_reg_value, uniqtranscale_reg_value;
	uint8_t train_set = dp->train_set[0];

	switch (train_set & DP_TRAIN_PRE_EMPHASIS_MASK) {
	case LGDP_PREEMPH(0):
		preemph_reg_value = 0x0004000;
		switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
		case LGDP_SWING(0):
			demph_reg_value = 0x2B405555;
			uniqtranscale_reg_value = 0x552AB83A;
			break;
		case LGDP_SWING(1):
			demph_reg_value = 0x2B404040;
			uniqtranscale_reg_value = 0x5548B83A;
			break;
		case LGDP_SWING(2):
			demph_reg_value = 0x2B245555;
			uniqtranscale_reg_value = 0x5560B83A;
			break;
		case LGDP_SWING(3):
			demph_reg_value = 0x2B405555;
			uniqtranscale_reg_value = 0x5598DA3A;
			break;
		default:
			return;
		}
		break;
	case LGDP_PREEMPH(1):
		preemph_reg_value = 0x0002000;
		switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
		case LGDP_SWING(0):
			demph_reg_value = 0x2B404040;
			uniqtranscale_reg_value = 0x5552B83A;
			break;
		case LGDP_SWING(1):
			demph_reg_value = 0x2B404848;
			uniqtranscale_reg_value = 0x5580B83A;
			break;
		case LGDP_SWING(2):
			demph_reg_value = 0x2B404040;
			uniqtranscale_reg_value = 0x55ADDA3A;
			break;
		default:
			return;
		}
		break;
	case LGDP_PREEMPH(2):
		preemph_reg_value = 0x0000000;
		switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
		case LGDP_SWING(0):
			demph_reg_value = 0x2B305555;
			uniqtranscale_reg_value = 0x5570B83A;
			break;
		case LGDP_SWING(1):
			demph_reg_value = 0x2B2B4040;
			uniqtranscale_reg_value = 0x55ADDA3A;
			break;
		default:
			return;
		}
		break;
	case LGDP_PREEMPH(3):
		preemph_reg_value = 0x0006000;
		switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
		case LGDP_SWING(0):
			demph_reg_value = 0x1B405555;
			uniqtranscale_reg_value = 0x55ADDA3A;
			break;
		default:
			return;
		}
		break;
	default:
		return;
	}
	lg_vlv_set_phy_signal_level(dp->d, dp->o, cfg, demph_reg_value, preemph_reg_value,
				    uniqtranscale_reg_value, 0);
}

/* Cherryview: de-emphasis and margin per level. */
static void chv_set_signal_levels(struct lg_dp *dp, const struct lg_config *cfg)
{
	uint32_t deemph_reg_value, margin_reg_value;
	int uniq_trans_scale = 0;
	uint8_t train_set = dp->train_set[0];

	switch (train_set & DP_TRAIN_PRE_EMPHASIS_MASK) {
	case LGDP_PREEMPH(0):
		switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
		case LGDP_SWING(0):
			deemph_reg_value = 128;
			margin_reg_value = 52;
			break;
		case LGDP_SWING(1):
			deemph_reg_value = 128;
			margin_reg_value = 77;
			break;
		case LGDP_SWING(2):
			deemph_reg_value = 128;
			margin_reg_value = 102;
			break;
		case LGDP_SWING(3):
			deemph_reg_value = 128;
			margin_reg_value = 154;
			uniq_trans_scale = 1;
			break;
		default:
			return;
		}
		break;
	case LGDP_PREEMPH(1):
		switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
		case LGDP_SWING(0):
			deemph_reg_value = 85;
			margin_reg_value = 78;
			break;
		case LGDP_SWING(1):
			deemph_reg_value = 85;
			margin_reg_value = 116;
			break;
		case LGDP_SWING(2):
			deemph_reg_value = 85;
			margin_reg_value = 154;
			break;
		default:
			return;
		}
		break;
	case LGDP_PREEMPH(2):
		switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
		case LGDP_SWING(0):
			deemph_reg_value = 64;
			margin_reg_value = 104;
			break;
		case LGDP_SWING(1):
			deemph_reg_value = 64;
			margin_reg_value = 154;
			break;
		default:
			return;
		}
		break;
	case LGDP_PREEMPH(3):
		switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
		case LGDP_SWING(0):
			deemph_reg_value = 43;
			margin_reg_value = 154;
			break;
		default:
			return;
		}
		break;
	default:
		return;
	}
	lg_chv_set_phy_signal_level(dp->d, dp->o, cfg, deemph_reg_value, margin_reg_value,
				    uniq_trans_scale);
}

static void dp_set_signal_levels(struct lg_dp *dp, const struct lg_config *cfg)
{
	struct lg_display *d = dp->d;
	uint8_t train_set = dp->train_set[0];
	uint32_t signal_levels, mask;

	i915_dbg("[drm] i915: %s: lanes %d, swing %d%s, pre-emphasis %d%s\n", dp->o->name,
		 dp->lane_count, train_set & DP_TRAIN_VOLTAGE_SWING_MASK,
		 (train_set & DP_TRAIN_MAX_SWING_REACHED) ? "(max)" : "",
		 (train_set & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT,
		 (train_set & DP_TRAIN_MAX_PRE_EMPHASIS_REACHED) ? "(max)" : "");

	if (d->is_chv) {
		chv_set_signal_levels(dp, cfg);
		return;
	}
	if (d->is_vlv) {
		vlv_set_signal_levels(dp, cfg);
		return;
	}
	if (d->is_ivb && dp->o->port == LG_PORT_A) {
		signal_levels = ivb_cpu_edp_signal_levels(train_set);
		mask = EDP_LINK_TRAIN_VOL_EMP_MASK_IVB;
	} else if (d->is_snb && dp->o->port == LG_PORT_A) {
		signal_levels = snb_cpu_edp_signal_levels(train_set);
		mask = EDP_LINK_TRAIN_VOL_EMP_MASK_SNB;
	} else {
		signal_levels = g4x_signal_levels(train_set);
		mask = DP_VOLTAGE_MASK | DP_PRE_EMPHASIS_MASK;
	}
	i915_dbg("[drm] i915: %s: using signal levels %08x\n", dp->o->name, signal_levels);
	dp->DP &= ~mask;
	dp->DP |= signal_levels;
	dp_write_port(dp);
}

/* ---- link training ------------------------------------------------------------------ */

static uint8_t lane_status(const uint8_t *link_status, int lane)
{
	return (uint8_t)((link_status[lane >> 1] >> ((lane & 1) * 4)) & 0xf);
}

static int clock_recovery_ok(const uint8_t *link_status, int lanes)
{
	for (int lane = 0; lane < lanes; lane++)
		if (!(lane_status(link_status, lane) & DP_LANE_CR_DONE))
			return 0;
	return 1;
}

static int channel_eq_ok(const uint8_t *link_status, int lanes)
{
	uint8_t want = DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED;

	if (!(link_status[DP_LANE_ALIGN_STATUS_UPDATED - DP_LANE0_1_STATUS] &
	      DP_INTERLANE_ALIGN_DONE))
		return 0;
	for (int lane = 0; lane < lanes; lane++)
		if ((lane_status(link_status, lane) & want) != want)
			return 0;
	return 1;
}

static uint8_t adjust_request_voltage(const uint8_t *link_status, int lane)
{
	uint8_t l = link_status[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS + (lane >> 1)];
	int s = (lane & 1) * 4;

	return (uint8_t)(((l >> s) & 0x3) << 0);
}

static uint8_t adjust_request_pre_emphasis(const uint8_t *link_status, int lane)
{
	uint8_t l = link_status[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS + (lane >> 1)];
	int s = (lane & 1) * 4;

	return (uint8_t)(((l >> s) & 0xc) << 1);
}

static int dp_read_link_status(struct lg_dp *dp, uint8_t *link_status)
{
	mm_memset(link_status, 0, LGDP_LINK_STATUS_SIZE);
	if (dpcd_read(dp, DP_LANE0_1_STATUS, link_status, LGDP_LINK_STATUS_SIZE) !=
	    LGDP_LINK_STATUS_SIZE)
		return -EIO;
	return 0;
}

static void dp_dump_link_status(const struct lg_dp *dp, const uint8_t *ls)
{
	i915_dbg("[drm] i915: %s: ln0_1:0x%x ln2_3:0x%x align:0x%x sink:0x%x adj_req0_1:0x%x adj_req2_3:0x%x\n",
		 dp->o->name, ls[0], ls[1], ls[2], ls[3], ls[4], ls[5]);
	(void)dp;
	(void)ls;
}

/* The most swing a level of pre-emphasis allows. */
static uint8_t dp_voltage_max_for(uint8_t preemph)
{
	switch (preemph & DP_TRAIN_PRE_EMPHASIS_MASK) {
	case LGDP_PREEMPH(0):
		return LGDP_SWING(3);
	case LGDP_PREEMPH(1):
		return LGDP_SWING(2);
	case LGDP_PREEMPH(2):
		return LGDP_SWING(1);
	case LGDP_PREEMPH(3):
	default:
		return LGDP_SWING(0);
	}
}

/* These ports drive one level on every lane: the highest any lane asks
 * for, within what the port can do. */
static int dp_get_adjust_train(struct lg_dp *dp, const uint8_t *link_status)
{
	uint8_t v = 0, p = 0, newset;
	int changed = 0;

	for (int lane = 0; lane < dp->lane_count; lane++) {
		uint8_t lv = adjust_request_voltage(link_status, lane);
		uint8_t lp = adjust_request_pre_emphasis(link_status, lane);

		if (lv > v)
			v = lv;
		if (lp > p)
			p = lp;
	}
	if (p >= dp->preemph_max)
		p = dp->preemph_max | DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
	if (v > dp_voltage_max_for(p))
		v = dp_voltage_max_for(p);
	if (v >= dp->voltage_max)
		v = dp->voltage_max | DP_TRAIN_MAX_SWING_REACHED;
	newset = v | p;

	for (int lane = 0; lane < 4; lane++) {
		if (dp->train_set[lane] == newset)
			continue;
		dp->train_set[lane] = newset;
		changed = 1;
	}
	return changed;
}

/* Pattern at the port and at the sink, with the lane levels. */
static int dp_set_link_train_dpcd(struct lg_dp *dp, uint8_t dp_train_pat)
{
	uint8_t buf[5];
	int len = dp->lane_count + 1;

	dp_set_link_train(dp, dp_train_pat);
	buf[0] = dp_train_pat;
	mm_memcpy(buf + 1, dp->train_set, (size_t)dp->lane_count);
	return dpcd_write(dp, DP_TRAINING_PATTERN_SET, buf, len) == len;
}

static int dp_reset_link_train(struct lg_dp *dp, const struct lg_config *cfg,
			       uint8_t dp_train_pat)
{
	mm_memset(dp->train_set, 0, sizeof(dp->train_set));
	dp_set_signal_levels(dp, cfg);
	return dp_set_link_train_dpcd(dp, dp_train_pat);
}

static int dp_update_link_train(struct lg_dp *dp, const struct lg_config *cfg)
{
	dp_set_signal_levels(dp, cfg);
	return dpcd_write(dp, DP_TRAINING_LANE0_SET, dp->train_set, dp->lane_count) ==
	       dp->lane_count;
}

static int dp_lane_max_vswing_reached(uint8_t train_set_lane)
{
	uint8_t v = train_set_lane & DP_TRAIN_VOLTAGE_SWING_MASK;
	uint8_t p = (train_set_lane & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT;

	if (!(train_set_lane & DP_TRAIN_MAX_SWING_REACHED))
		return 0;
	return v + p == 3;
}

static int dp_link_max_vswing_reached(const struct lg_dp *dp)
{
	for (int lane = 0; lane < dp->lane_count; lane++)
		if (!dp_lane_max_vswing_reached(dp->train_set[lane]))
			return 0;
	return 1;
}

static int dp_adjust_request_changed(const struct lg_dp *dp, const uint8_t *old_ls,
				     const uint8_t *new_ls)
{
	for (int lane = 0; lane < dp->lane_count; lane++) {
		uint8_t o = adjust_request_voltage(old_ls, lane) |
			    adjust_request_pre_emphasis(old_ls, lane);
		uint8_t n = adjust_request_voltage(new_ls, lane) |
			    adjust_request_pre_emphasis(new_ls, lane);
		if (o != n)
			return 1;
	}
	return 0;
}

/* How long the sink wants between a pattern change and the status read. */
static uint32_t dp_rd_interval(const struct lg_dp *dp)
{
	uint32_t rd = dp->dpcd[DP_TRAINING_AUX_RD_INTERVAL] & LGDP_TRAINING_AUX_RD_MASK;

	if (rd > 4)
		rd = 4;
	return rd;
}

static uint32_t dp_cr_delay_us(const struct lg_dp *dp)
{
	uint32_t rd = dp_rd_interval(dp);

	/* DPCD 1.4 fixes the clock recovery interval at 100 us */
	if (dp->dpcd[DP_DPCD_REV] >= LGDP_DPCD_REV_14 || rd == 0)
		return 100;
	return rd * 4000;
}

static uint32_t dp_eq_delay_us(const struct lg_dp *dp)
{
	uint32_t rd = dp_rd_interval(dp);

	return rd ? rd * 4000 : 400;
}

static void dp_delay_us(uint32_t us)
{
	if (us >= 1000)
		lg_mdelay(us / 1000);
	if (us % 1000)
		lg_udelay(us % 1000);
}

/* Tell the sink the rate, the width and the coding. */
static void dp_prepare_link_train(struct lg_dp *dp, const struct lg_config *cfg)
{
	uint8_t link_config[2];
	uint8_t lane_count = (uint8_t)dp->lane_count;
	uint8_t link_bw = 0, rate_select = 0;

	if (dp->use_rate_select) {
		for (int i = 0; i < dp->num_sink_rates; i++)
			if (dp->sink_rates[i] == dp->link_rate)
				rate_select = (uint8_t)i;
	} else {
		link_bw = (uint8_t)(dp->link_rate / 27000);
	}
	/* Some eDP muxes snoop the rate table and forget it when powered
	 * down: let them read it again. */
	if (!link_bw) {
		uint8_t rates[16];

		i915_dbg("[drm] i915: %s: reloading eDP link rates\n", dp->o->name);
		(void)dpcd_read(dp, DP_SUPPORTED_LINK_RATES, rates, sizeof(rates));
	}
	/* no spread, no MSA timing ignore; 8b/10b */
	link_config[0] = 0;
	link_config[1] = DP_SET_ANSI_8B10B;
	(void)dpcd_write(dp, DP_DOWNSPREAD_CTRL, link_config, 2);

	if (cfg->enhanced_framing)
		lane_count |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	if (link_bw) {
		link_config[0] = link_bw;
		link_config[1] = lane_count;
		(void)dpcd_write(dp, DP_LINK_BW_SET, link_config, 2);
	} else {
		/* eDP 1.4 sinks ignore LINK_RATE_SET once LINK_BW_SET was
		 * written: leave that alone. */
		(void)dpcd_writeb(dp, DP_LANE_COUNT_SET, lane_count);
		(void)dpcd_writeb(dp, DP_LINK_RATE_SET, rate_select);
	}
}

/* Pattern 1 until every lane has recovered the clock. */
static int dp_link_training_clock_recovery(struct lg_dp *dp, const struct lg_config *cfg)
{
	uint8_t old_ls[LGDP_LINK_STATUS_SIZE], ls[LGDP_LINK_STATUS_SIZE];
	uint32_t delay_us = dp_cr_delay_us(dp);
	int voltage_tries, max_cr_tries, max_vswing_reached = 0;

	mm_memset(old_ls, 0, sizeof(old_ls));
	mm_memset(ls, 0, sizeof(ls));
	if (!dp_reset_link_train(dp, cfg, DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE)) {
		kprintf("[drm] i915: %s: failed to enable link training\n", dp->o->name);
		return 0;
	}
	/* DPCD 1.4 limits clock recovery to ten rounds; older sinks get a
	 * tolerant 80 (four swings, four emphases, five repeats each). */
	max_cr_tries = dp->dpcd[DP_DPCD_REV] >= LGDP_DPCD_REV_14 ? 10 : 80;
	voltage_tries = 1;
	for (int cr_tries = 0; cr_tries < max_cr_tries; cr_tries++) {
		dp_delay_us(delay_us);
		if (dp_read_link_status(dp, ls)) {
			kprintf("[drm] i915: %s: failed to get link status\n", dp->o->name);
			return 0;
		}
		if (clock_recovery_ok(ls, dp->lane_count)) {
			i915_dbg("[drm] i915: %s: clock recovery OK\n", dp->o->name);
			return 1;
		}
		if (voltage_tries == 5) {
			dp_dump_link_status(dp, ls);
			i915_dbg("[drm] i915: %s: same voltage tried 5 times\n", dp->o->name);
			return 0;
		}
		if (max_vswing_reached) {
			dp_dump_link_status(dp, ls);
			i915_dbg("[drm] i915: %s: max voltage swing reached\n", dp->o->name);
			return 0;
		}
		dp_get_adjust_train(dp, ls);
		if (!dp_update_link_train(dp, cfg)) {
			kprintf("[drm] i915: %s: failed to update link training\n", dp->o->name);
			return 0;
		}
		if (!dp_adjust_request_changed(dp, old_ls, ls))
			voltage_tries++;
		else
			voltage_tries = 1;
		mm_memcpy(old_ls, ls, sizeof(ls));
		if (dp_link_max_vswing_reached(dp))
			max_vswing_reached = 1;
	}
	dp_dump_link_status(dp, ls);
	kprintf("[drm] i915: %s: clock recovery failed %d times, giving up\n", dp->o->name,
		max_cr_tries);
	return 0;
}

/* Pattern 2 (these ports have no pattern 3) until the lanes are
 * equalised, symbol locked and aligned. */
static int dp_link_training_channel_equalization(struct lg_dp *dp, const struct lg_config *cfg)
{
	uint8_t ls[LGDP_LINK_STATUS_SIZE];
	uint32_t delay_us = dp_eq_delay_us(dp);
	int tries, channel_eq = 0;

	mm_memset(ls, 0, sizeof(ls));
	if (!dp_set_link_train_dpcd(dp, DP_TRAINING_PATTERN_2 | DP_LINK_SCRAMBLING_DISABLE)) {
		kprintf("[drm] i915: %s: failed to start channel equalisation\n", dp->o->name);
		return 0;
	}
	for (tries = 0; tries < 5; tries++) {
		dp_delay_us(delay_us);
		if (dp_read_link_status(dp, ls)) {
			kprintf("[drm] i915: %s: failed to get link status\n", dp->o->name);
			break;
		}
		if (!clock_recovery_ok(ls, dp->lane_count)) {
			dp_dump_link_status(dp, ls);
			i915_dbg("[drm] i915: %s: clock recovery lost during equalisation\n",
				 dp->o->name);
			break;
		}
		if (channel_eq_ok(ls, dp->lane_count)) {
			channel_eq = 1;
			i915_dbg("[drm] i915: %s: channel equalisation done\n", dp->o->name);
			break;
		}
		dp_get_adjust_train(dp, ls);
		if (!dp_update_link_train(dp, cfg)) {
			kprintf("[drm] i915: %s: failed to update link training\n", dp->o->name);
			break;
		}
	}
	if (tries == 5) {
		dp_dump_link_status(dp, ls);
		i915_dbg("[drm] i915: %s: channel equalisation failed 5 times\n", dp->o->name);
	}
	return channel_eq;
}

/* One full training at the link parameters of the last prepare; the
 * port is left sending the idle pattern.  1 when it passed. */
static int dp_link_train(struct lg_dp *dp, const struct lg_config *cfg)
{
	uint8_t off = DP_TRAINING_PATTERN_DISABLE;
	int passed = 0;

	/* the receiver capabilities again, as the sink is now powered */
	if (dp_read_dpcd_caps(dp))
		i915_dbg("[drm] i915: %s: DPCD re-read before training failed\n", dp->o->name);

	dp_prepare_link_train(dp, cfg);
	if (dp_link_training_clock_recovery(dp, cfg) &&
	    dp_link_training_channel_equalization(dp, cfg))
		passed = 1;
	i915_dbg("[drm] i915: %s: link training %s at %d kHz, %d lanes\n", dp->o->name,
		 passed ? "passed" : "failed", dp->link_rate, dp->lane_count);

	(void)dpcd_write(dp, DP_TRAINING_PATTERN_SET, &off, 1);
	dp_set_idle_link_train(dp);
	return passed;
}

static void dp_stop_link_train(struct lg_dp *dp)
{
	dp->link_active = 1;
	dp_set_link_train(dp, DP_TRAINING_PATTERN_DISABLE);
}

/* After a failed training the next mode set narrows the link, as far as
 * the mode still fits: eDP panels first get their maximum, then a lower
 * rate, then fewer lanes. */
static void dp_note_train_failure(struct lg_dp *dp)
{
	if (dp->is_edp && !dp->use_max_params) {
		dp->use_max_params = 1;
		return;
	}
	for (int i = dp->num_common_rates - 1; i > 0; i--) {
		if (dp->common_rates[i] == dp->link_rate) {
			dp->fb_max_rate = dp->common_rates[i - 1];
			return;
		}
	}
	if (dp->lane_count > 1) {
		dp->fb_max_lanes = dp->lane_count / 2;
		dp->fb_max_rate = 0;
	}
}

static void dp_start_and_stop_link_train(struct lg_dp *dp, const struct lg_config *cfg)
{
	if (!dp_link_train(dp, cfg)) {
		/* One more go: a sink that was still waking up usually
		 * trains on the second attempt. */
		kprintf("[drm] i915: %s: link training failed at %d.%02d Gbit/s x%d, retrying\n",
			dp->o->name, dp->link_rate / 100000, (dp->link_rate / 1000) % 100,
			dp->lane_count);
		dp_set_power(dp, DP_SET_POWER_D0);
		if (!dp_link_train(dp, cfg)) {
			kprintf("[drm] i915: %s: link training failed again; the next mode set uses a narrower link\n",
				dp->o->name);
			dp_note_train_failure(dp);
		}
	}
	dp_stop_link_train(dp);
}

/* ---- enable and disable --------------------------------------------------------------- */

static void edp_backlight_on(struct lg_dp *dp, const struct lg_config *cfg)
{
	if (!dp->is_edp)
		return;
	lg_backlight_enable(dp->d, dp->o, cfg);
	lg_pps_backlight_on(dp->d, dp->o);
}

static void edp_backlight_off(struct lg_dp *dp)
{
	if (!dp->is_edp)
		return;
	lg_pps_backlight_off(dp->d, dp->o);
	lg_backlight_disable(dp->d, dp->o);
}

static void dp_enable_port(struct lg_dp *dp)
{
	/* Enable with pattern 1, as the specification wants.  The
	 * register is written once without the enable bit and then again
	 * with it: on Valleyview/Cherryview training fails otherwise when
	 * the power sequencer is freshly used for this port. */
	dp_set_link_train(dp, DP_TRAINING_PATTERN_1);
	dp->DP |= DP_PORT_EN;
	dp_write_port(dp);
}

static void intel_enable_dp(struct lg_dp *dp, const struct lg_config *cfg)
{
	struct lg_display *d = dp->d;

	if (lg_rd(d, dp->o->reg) & DP_PORT_EN) {
		kprintf("[drm] i915: %s: port already enabled\n", dp->o->name);
		return;
	}
	if (d->is_vlv || d->is_chv)
		lg_pps_vlv_port_enable(d, dp->o, cfg->pipe);

	dp_enable_port(dp);

	if (dp->is_edp) {
		edp_vdd_get(dp);
		lg_pps_on(d, dp->o);
		edp_vdd_put(dp);
	}

	if (d->is_vlv || d->is_chv) {
		/* the PHY reports the port's lanes up (a narrow Cherryview
		 * link leaves the unused ones down) */
		unsigned int lane_mask = 0;

		if (d->is_chv)
			lane_mask = ~((1u << cfg->lane_count) - 1) & 0xf;
		lg_vlv_wait_port_ready(d, dp->o, lane_mask);
	}

	edp_vdd_get(dp);
	dp_set_power(dp, DP_SET_POWER_D0);
	dp_start_and_stop_link_train(dp, cfg);
	edp_vdd_put(dp);
}

static void intel_disable_dp(struct lg_dp *dp)
{
	dp->link_active = 0;
	/* The panel goes off with VDD forced, so it can be sequenced. */
	edp_vdd_get(dp);
	edp_backlight_off(dp);
	dp_set_power(dp, DP_SET_POWER_D3);
	if (dp->is_edp) {
		lg_pps_off(dp->d, dp->o);
		/* turning the panel off drops the forced VDD as well */
		dp->vdd_ref = 0;
	}
}

/* T10, the wait after the link stops before the panel may lose power:
 * what the sequencer was programmed with, else the VBT, else the
 * specification's 500 ms. */
static uint32_t edp_power_down_delay_ms(struct lg_dp *dp, int pipe)
{
	struct lg_display *d = dp->d;
	uint32_t base, v;

	if (!dp->is_edp)
		return 0;
	if (d->is_vlv || d->is_chv)
		base = PPS_BASE_GMCH + (uint32_t)pipe * 0x100u;
	else if (dp_pch_split(d))
		base = PPS_BASE_PCH;
	else
		base = PPS_BASE_GMCH;
	v = (lg_rd(d, PP_OFF_DELAYS(base)) & PANEL_POWER_DOWN_DELAY_MASK) >>
	    PANEL_POWER_DOWN_DELAY_SHIFT;
	if (!v)
		v = d->vbt.edp_t10;
	if (!v)
		return 500;
	return DIV_ROUND_UP(v, 10u);
}

static void intel_dp_link_down(struct lg_dp *dp, const struct lg_config *cfg)
{
	struct lg_display *d = dp->d;
	int port = dp->o->port;

	if (!(lg_rd(d, dp->o->reg) & DP_PORT_EN)) {
		i915_dbg("[drm] i915: %s: link down on a disabled port\n", dp->o->name);
		return;
	}

	dp->DP &= ~DP_PORT_EN;
	dp_write_port(dp);

	/* Ibex Peak: a port left selecting transcoder B blocks the HDMI
	 * port sharing it from transcoder A; park it on A (enabled for a
	 * moment with pattern 1, as the specification wants). */
	if (d->pch == LG_PCH_IBX && cfg->pipe == 1 && port != LG_PORT_A) {
		dp->DP &= ~(DP_PIPE_SEL_MASK | DP_LINK_TRAIN_MASK);
		dp->DP |= DP_PORT_EN | DP_PIPE_SEL(0) | DP_LINK_TRAIN_PAT_1;
		dp_write_port(dp);
		dp->DP &= ~DP_PORT_EN;
		dp_write_port(dp);
		if (dp_pipe_running(d, 0))
			lg_wait_for_vblank(d, 0);
	}

	if (dp->is_edp)
		lg_mdelay(edp_power_down_delay_ms(dp, cfg->pipe));

	if (d->is_vlv || d->is_chv)
		lg_pps_vlv_port_disable(d, dp->o);
}

/* ---- the output hooks --------------------------------------------------------------- */

static void dp_pre_pll_enable(struct lg_display *d, struct lg_output *o,
			      const struct lg_config *cfg)
{
	struct lg_dp *dp = to_dp(o);

	if (d->is_chv) {
		dp_prepare(dp, cfg);
		lg_chv_phy_pre_pll_enable(d, o, cfg);
	} else if (d->is_vlv) {
		dp_prepare(dp, cfg);
		lg_vlv_phy_pre_pll_enable(d, o, cfg);
	}
}

static void dp_pre_enable(struct lg_display *d, struct lg_output *o,
			  const struct lg_config *cfg)
{
	struct lg_dp *dp = to_dp(o);

	if (d->is_chv) {
		lg_chv_phy_pre_encoder_enable(d, o, cfg);
		intel_enable_dp(dp, cfg);
		/* the second common lane stays alive on its own now */
		lg_chv_phy_release_cl2_override(d, o);
	} else if (d->is_vlv) {
		lg_vlv_phy_pre_encoder_enable(d, o, cfg);
		intel_enable_dp(dp, cfg);
	} else {
		dp_prepare(dp, cfg);
		/* only Ironlake on has port A */
		if (o->port == LG_PORT_A)
			ilk_edp_pll_on(dp, cfg);
	}
}

static void dp_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	struct lg_dp *dp = to_dp(o);

	/* Valleyview/Cherryview brought the port and link up before the
	 * pipe; the others do it now that the pipe runs. */
	if (!d->is_vlv && !d->is_chv)
		intel_enable_dp(dp, cfg);
	edp_backlight_on(dp, cfg);
}

static void dp_disable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	(void)d;
	(void)cfg;
	intel_disable_dp(to_dp(o));
}

static void dp_post_disable(struct lg_display *d, struct lg_output *o,
			    const struct lg_config *cfg)
{
	struct lg_dp *dp = to_dp(o);

	/* The pipe is already off: the order Ironlake wants, which G4X
	 * DisplayPort takes as well since it does not suffer from the
	 * underruns its other ports have with it. */
	intel_dp_link_down(dp, cfg);
	if (d->is_chv) {
		/* assert the data lane reset */
		lg_chv_data_lane_soft_reset(d, o, cfg, 1);
	} else if (!d->is_vlv && o->port == LG_PORT_A) {
		ilk_edp_pll_off(dp);
	}
}

static void dp_post_pll_disable(struct lg_display *d, struct lg_output *o,
				const struct lg_config *cfg)
{
	if (d->is_chv)
		lg_chv_phy_post_pll_disable(d, o, cfg);
}

/* ---- modes and configuration --------------------------------------------------------- */

static int dp_mode_same_timing(const struct drm_mode_modeinfo *a, const struct drm_mode_modeinfo *b)
{
	uint32_t f = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC |
		     DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_INTERLACE;

	return a->clock == b->clock && a->hdisplay == b->hdisplay &&
	       a->hsync_start == b->hsync_start && a->hsync_end == b->hsync_end &&
	       a->htotal == b->htotal && a->vdisplay == b->vdisplay &&
	       a->vsync_start == b->vsync_start && a->vsync_end == b->vsync_end &&
	       a->vtotal == b->vtotal && (a->flags & f) == (b->flags & f);
}

static int dp_mode_valid(struct lg_display *d, struct lg_output *o,
			 const struct drm_mode_modeinfo *m)
{
	struct lg_dp *dp = to_dp(o);
	uint32_t target_clock = m->clock;
	int max_rate, max_lanes;

	if (m->flags & (DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_DBLCLK))
		return -EINVAL;
	/* only the PCH parts' DP ports take interlaced modes */
	if ((m->flags & DRM_MODE_FLAG_INTERLACE) && d->gmch)
		return -EINVAL;
	if (m->clock < 10000)
		return -EINVAL;
	/* these ports do not run a 4096 pixel wide picture */
	if (m->hdisplay == 4096)
		return -EINVAL;

	if (dp->is_edp && o->fixed_mode_valid) {
		/* the fitter scales down onto the panel, never up past it */
		if (m->hdisplay > o->fixed_mode.hdisplay || m->vdisplay > o->fixed_mode.vdisplay)
			return -EINVAL;
		target_clock = o->fixed_mode.clock;
	}
	if (d->max_dotclk_khz && target_clock > d->max_dotclk_khz)
		return -EINVAL;

	/* the widest link must carry the mode at 6 bpc at least */
	dp_link_limits(dp, &max_rate, &max_lanes);
	if ((uint64_t)max_rate * (uint64_t)max_lanes <
	    DIV_ROUND_UP((uint64_t)target_clock * 18u, 8u))
		return -EINVAL;
	return 0;
}

/* The fixed dividers of the DisplayPort link rates, per PLL type. */
struct lgdp_divider {
	int dot, p1, p2, n, m1, m2;
};

static const struct lgdp_divider g4x_dpll[] = {
	{ 162000, 2, 10, 2, 23, 8 },
	{ 270000, 1, 10, 1, 14, 2 },
};

static const struct lgdp_divider pch_dpll[] = {
	{ 162000, 2, 10, 1, 12, 9 },
	{ 270000, 1, 10, 2, 14, 8 },
};

static const struct lgdp_divider vlv_dpll[] = {
	{ 162000, 3, 2, 5, 3, 81 },
	{ 270000, 2, 2, 1, 2, 27 },
};

/* m2 a 22.22 fixed-point value: 32.4 and 27.0 */
static const struct lgdp_divider chv_dpll[] = {
	{ 162000, 4, 2, 1, 2, 0x819999a },
	{ 270000, 4, 1, 1, 2, 0x6c00000 },
};

static void dp_set_clock(struct lg_display *d, struct lg_config *cfg)
{
	const struct lgdp_divider *divisor = NULL;
	int count = 0;

	if (dp_is_g4x(d)) {
		divisor = g4x_dpll;
		count = (int)(sizeof(g4x_dpll) / sizeof(g4x_dpll[0]));
	} else if (dp_pch_split(d)) {
		divisor = pch_dpll;
		count = (int)(sizeof(pch_dpll) / sizeof(pch_dpll[0]));
	} else if (d->is_chv) {
		divisor = chv_dpll;
		count = (int)(sizeof(chv_dpll) / sizeof(chv_dpll[0]));
	} else if (d->is_vlv) {
		divisor = vlv_dpll;
		count = (int)(sizeof(vlv_dpll) / sizeof(vlv_dpll[0]));
	}
	for (int i = 0; i < count; i++) {
		if ((int)cfg->port_clock != divisor[i].dot)
			continue;
		mm_memset(&cfg->dpll, 0, sizeof(cfg->dpll));
		cfg->dpll.dot = divisor[i].dot;
		cfg->dpll.p1 = divisor[i].p1;
		cfg->dpll.p2 = divisor[i].p2;
		cfg->dpll.n = divisor[i].n;
		cfg->dpll.m1 = divisor[i].m1;
		cfg->dpll.m2 = divisor[i].m2;
		cfg->clock_set = 1;
		break;
	}
}

/* Most bits per pixel first, then the lowest rate, then the fewest
 * lanes; eDP panels before 1.4 only at their maximum. */
static int dp_compute_link_config(struct lg_dp *dp, uint32_t clock, int max_bpp,
				  int *out_rate, int *out_lanes, int *out_bpp)
{
	int max_rate, max_lanes;

	dp_link_limits(dp, &max_rate, &max_lanes);
	for (int bpp = max_bpp; bpp >= 18; bpp -= 2 * 3) {
		uint64_t need = DIV_ROUND_UP((uint64_t)clock * (uint64_t)bpp, 8u);

		if (dp->use_max_params) {
			if ((uint64_t)max_rate * (uint64_t)max_lanes >= need) {
				*out_rate = max_rate;
				*out_lanes = max_lanes;
				*out_bpp = bpp;
				return 0;
			}
			continue;
		}
		for (int r = 0; r < dp->num_common_rates; r++) {
			int rate = dp->common_rates[r];

			if (rate > max_rate)
				break;
			for (int lanes = 1; lanes <= max_lanes; lanes <<= 1) {
				if ((uint64_t)rate * (uint64_t)lanes >= need) {
					*out_rate = rate;
					*out_lanes = lanes;
					*out_bpp = bpp;
					return 0;
				}
			}
		}
	}
	return -EINVAL;
}

static int dp_compute_config(struct lg_display *d, struct lg_output *o, struct lg_config *cfg)
{
	struct lg_dp *dp = to_dp(o);
	int rate, lanes, bpp, max_bpp;

	if (dp->is_edp && o->fixed_mode_valid && !dp_mode_same_timing(&cfg->mode, &o->fixed_mode)) {
		/* The panel runs its own timing; the fitter scales the
		 * client's picture onto it. */
		cfg->mode = o->fixed_mode;
		lg_timings_from_mode(&cfg->t, &cfg->mode);
		cfg->t_set = 1;
		if (lg_pfit_compute(d, o, cfg))
			return -EINVAL;
	}

	if (cfg->mode.flags & (DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_DBLCLK))
		return -EINVAL;
	if ((cfg->mode.flags & DRM_MODE_FLAG_INTERLACE) && d->gmch)
		return -EINVAL;
	if (cfg->mode.hdisplay == 4096)
		return -EINVAL;

	cfg->has_pch_encoder = d->pch != LG_PCH_NONE && o->port != LG_PORT_A;
	cfg->has_dp_encoder = 1;

	/* The pipe depth: what the core offers, no more than the sink's
	 * EDID names, and for a panel whose EDID is silent no more than
	 * the VBT says. */
	max_bpp = cfg->pipe_bpp ? cfg->pipe_bpp : 24;
	if (dp->edid_bpc > 0 && dp->edid_bpc * 3 < max_bpp)
		max_bpp = dp->edid_bpc * 3;
	if (dp->is_edp && dp->edid_bpc == 0 && d->vbt.edp_bpp && d->vbt.edp_bpp < max_bpp) {
		i915_dbg("[drm] i915: %s: clamping bpp to the VBT's %d\n", o->name, d->vbt.edp_bpp);
		max_bpp = d->vbt.edp_bpp;
	}
	if (max_bpp < 18)
		max_bpp = 18;

	if (dp_compute_link_config(dp, cfg->mode.clock, max_bpp, &rate, &lanes, &bpp)) {
		i915_dbg("[drm] i915: %s: no link carries %u kHz\n", o->name, cfg->mode.clock);
		return -EINVAL;
	}
	cfg->pipe_bpp = bpp;
	cfg->port_clock = (uint32_t)rate;
	cfg->lane_count = lanes;
	cfg->limited_color_range = 0;
	cfg->enhanced_framing = dp_enhanced_frame_cap(dp);
	lg_link_compute_m_n(cfg->pipe_bpp, lanes, cfg->mode.clock, (uint32_t)rate, &cfg->dp_m_n);
	if (dp->is_edp)
		cfg->msa_timing_delay = d->vbt.edp_msa_timing_delay;

	dp_set_clock(d, cfg);
	i915_dbg("[drm] i915: %s: %u kHz at %d bpp over %d lanes at %d kHz\n", o->name,
		 cfg->mode.clock, bpp, lanes, rate);
	return 0;
}

/* ---- detection ---------------------------------------------------------------------- */

static void dp_check_link_state(struct lg_dp *dp)
{
	struct lg_output *o = dp->o;
	uint8_t ls[LGDP_LINK_STATUS_SIZE];

	if (!o->active || !dp->link_active)
		return;
	if (dp->link_rate <= 0 || dp->lane_count <= 0)
		return;
	if (!(lg_rd(dp->d, o->reg) & DP_PORT_EN))
		return;
	edp_vdd_get(dp);
	if (dp_read_link_status(dp, ls) == 0 && !channel_eq_ok(ls, dp->lane_count)) {
		dp_dump_link_status(dp, ls);
		kprintf("[drm] i915: %s: link lost, retraining\n", o->name);
		dp_start_and_stop_link_train(dp, &o->cfg);
	}
	edp_vdd_put(dp);
}

static int dp_detect(struct lg_display *d, struct lg_output *o)
{
	struct lg_dp *dp = to_dp(o);
	int live, connected = 0;

	/* A panel is always there once its DPCD answered. */
	if (dp->is_edp)
		return dp->has_dpcd;

	live = lg_hpd_live(d, o->hpd_pin);
	if (live == 0)
		goto out;

	if (dp_get_dpcd(dp))
		goto out;
	connected = 1;

	if (dp_branch(dp) && !(dp_has_sink_count(dp) && (dp->ds_port[0] & LGDP_DS_PORT_HPD))) {
		/* A branch device without downstream HPD: a display behind
		 * it answers on DDC. */
		if (lg_read_edid(d, o) != 0) {
			uint8_t type;

			if (dp->dpcd[DP_DPCD_REV] >= LGDP_DPCD_REV_11) {
				type = dp->ds_port[0] & LGDP_DS_PORT_TYPE_MASK;
				connected = type == LGDP_DS_PORT_TYPE_VGA ||
					    type == LGDP_DS_PORT_TYPE_NON_EDID;
			} else {
				type = dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] &
				       LGDP_DWN_STRM_PORT_TYPE_MASK;
				connected = type == LGDP_DWN_STRM_PORT_TYPE_ANALOG ||
					    type == LGDP_DWN_STRM_PORT_TYPE_OTHER;
			}
			if (!connected)
				i915_dbg("[drm] i915: %s: broken branch device, ignoring\n", o->name);
		}
	} else {
		(void)lg_read_edid(d, o);
	}
	dp_note_edid(dp);

	/* Some monitors lose the link without a short pulse. */
	dp_check_link_state(dp);
out:
	if (!connected) {
		o->edid_len = 0;
		dp->edid_bpc = 0;
	}
	dp->has_dpcd = connected;
	o->detected = connected;
	return connected;
}

static int dp_get_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	struct lg_dp *dp = to_dp(o);
	int max_rate, max_lanes, n = 0;
	uint32_t max_clock, max_std;

	if (!dp->is_edp) {
		/* what the best link carries at 8 bpc */
		dp_link_limits(dp, &max_rate, &max_lanes);
		max_clock = (uint32_t)((uint64_t)max_rate * (uint64_t)max_lanes * 8u / 24u);
		if (d->max_dotclk_khz && max_clock > d->max_dotclk_khz)
			max_clock = d->max_dotclk_khz;
		return lg_get_modes_edid(d, o, conn, max_clock);
	}

	if (o->edid_len) {
		n = drm_connector_set_edid(d->drm, conn, o->edid, (unsigned)o->edid_len);
		if (n < 0)
			n = 0;
	}
	if (n == 0 && o->fixed_mode_valid) {
		struct drm_mode_modeinfo m = o->fixed_mode;

		m.type |= DRM_MODE_TYPE_PREFERRED;
		if (drm_connector_add_mode(d->drm, conn, &m) >= 0)
			n++;
	}
	if (o->fixed_mode_valid) {
		/* smaller modes: the fitter scales them onto the panel */
		max_std = d->max_dotclk_khz ? d->max_dotclk_khz : 400000;
		int extra = drm_connector_add_std_modes(d->drm, conn, max_std,
							o->fixed_mode.hdisplay,
							o->fixed_mode.vdisplay);
		if (extra > 0)
			n += extra;
	}
	return n;
}

static void dp_hpd_event(struct lg_display *d, struct lg_output *o, int long_pulse)
{
	struct lg_dp *dp = to_dp(o);
	uint8_t vector = 0, count;

	(void)d;
	/* A panel's pulses come from its VDD going on and off; acting on
	 * them would only raise VDD again. */
	if (dp->is_edp && (long_pulse || !o->active))
		return;
	if (long_pulse) {
		/* the core probes the port again; the sink may be new */
		dp_reset_link_params(dp);
		return;
	}
	if (!dp->has_dpcd)
		return;

	edp_vdd_get(dp);
	if (dp->dpcd[DP_DPCD_REV] >= LGDP_DPCD_REV_11) {
		if (dp_has_sink_count(dp) && dpcd_read(dp, DP_SINK_COUNT, &count, 1) == 1)
			dp->sink_count = ((count & 0x80) >> 1) | (count & 0x3f);
		/* acknowledge what the sink raised */
		if (dpcd_read(dp, LGDP_DEVICE_SERVICE_IRQ_VECTOR, &vector, 1) == 1 && vector)
			(void)dpcd_writeb(dp, LGDP_DEVICE_SERVICE_IRQ_VECTOR, vector);
	}
	/* the short pulse is how a sink reports a lost link */
	dp_check_link_state(dp);
	edp_vdd_put(dp);
}

static void dp_suspend(struct lg_display *d, struct lg_output *o)
{
	struct lg_dp *dp = to_dp(o);

	(void)d;
	if (dp->is_edp && dp->vdd_ref > 0) {
		dp->vdd_ref = 1;
		edp_vdd_put(dp);
	}
}

static void dp_resume(struct lg_display *d, struct lg_output *o)
{
	struct lg_dp *dp = to_dp(o);

	dp->DP = lg_rd(d, o->reg);
	dp->link_active = 0;
	dp_reset_link_params(dp);
}

static const struct lg_output_funcs lg_dp_funcs = {
	.detect = dp_detect,
	.get_modes = dp_get_modes,
	.mode_valid = dp_mode_valid,
	.compute_config = dp_compute_config,
	.pre_pll_enable = dp_pre_pll_enable,
	.pre_enable = dp_pre_enable,
	.enable = dp_enable,
	.disable = dp_disable,
	.post_disable = dp_post_disable,
	.post_pll_disable = dp_post_pll_disable,
	.get_hw_state = dp_get_hw_state,
	.hpd_event = dp_hpd_event,
	.suspend = dp_suspend,
	.resume = dp_resume,
};

/* ---- init --------------------------------------------------------------------------- */

static int dp_port_is_edp(struct lg_display *d, int port)
{
	/* G4X has no eDP, whatever the VBT says */
	if (d->ver < 5)
		return 0;
	if (dp_pch_split(d) && port == LG_PORT_A)
		return 1;
	/* Cherryview's port D never carries a panel; on Valleyview and
	 * Cherryview eDP sits on B or C only */
	if ((d->is_vlv || d->is_chv) && port != LG_PORT_B && port != LG_PORT_C)
		return 0;
	return lg_vbt_port_is_edp(d, port);
}

static int dp_pick_aux_ch(struct lg_display *d, int port)
{
	const struct lg_vbt_child *child = lg_vbt_child_for_port(d, port);
	int ch = port;

	if (child && child->aux_ch <= LG_PORT_D) {
		int vch = child->aux_ch;

		/* the GMCH parts have no AUX A */
		if (!(d->gmch && vch == LG_PORT_A))
			ch = vch;
	}
	return ch;
}

static void dp_aux_init(struct lg_dp *dp)
{
	struct lg_display *d = dp->d;

	if (dp_pch_split(d) && dp->aux_ch != LG_PORT_A)
		dp->aux_ctl = LGDP_PCH_AUX_CH_CTL(dp->aux_ch);
	else
		dp->aux_ctl = LGDP_AUX_CH_CTL(dp->aux_ch);
	dp->ddc.xfer = dp_i2c_xfer;
	dp->ddc.priv = dp;
	ksnprintf(dp->ddc.name, sizeof(dp->ddc.name), "AUX %c/DP %c", port_name(dp->aux_ch),
		  port_name(dp->o->port));
}

static void dp_output_release(struct lg_display *d, struct lg_output *o)
{
	kfree(o->priv);
	if (d->nout > 0 && o == &d->out[d->nout - 1])
		d->nout--;
	mm_memset(o, 0, sizeof(*o));
	o->type = LG_OUTPUT_UNUSED;
	o->conn = -1;
	o->crtc = -1;
	o->pipe = -1;
	o->sibling = -1;
}

/* The VLV/CHV backlight controller starts on the pipe the port runs on,
 * else pipe A. */
static int edp_backlight_initial_pipe(struct lg_dp *dp)
{
	struct lg_display *d = dp->d;
	int pipe = -1;

	if (!d->is_vlv && !d->is_chv)
		return -1;
	if (!dp_port_enabled(d, dp->o->reg, dp->o->port, &pipe) || (pipe != 0 && pipe != 1))
		pipe = 0;
	return pipe;
}

/* The panel: its sequencer, its DPCD and EDID (read once, with VDD
 * forced), its fixed mode and backlight.  0 when it is usable. */
static int edp_init(struct lg_dp *dp)
{
	struct lg_display *d = dp->d;
	struct lg_output *o = dp->o;
	struct drm_edid_info *info;

	lg_pps_edp_init(d, o);

	edp_vdd_get(dp);
	if (dp_read_dpcd_caps(dp)) {
		edp_vdd_put(dp);
		kprintf("[drm] i915: %s: failed to retrieve link info, disabling eDP\n", o->name);
		return -ENODEV;
	}
	/* the eDP display control registers (zero when absent) */
	if (dpcd_read(dp, DP_EDP_DPCD_REV, dp->edp_dpcd, LGDP_EDP_DPCD_SIZE) != LGDP_EDP_DPCD_SIZE)
		mm_memset(dp->edp_dpcd, 0, sizeof(dp->edp_dpcd));
	i915_dbg("[drm] i915: %s: eDP DPCD %02x %02x %02x\n", o->name, dp->edp_dpcd[0],
		 dp->edp_dpcd[1], dp->edp_dpcd[2]);
	edp_set_sink_rates(dp);
	dp_set_max_sink_lanes(dp);
	dp->has_dpcd = 1;

	(void)lg_read_edid(d, o);
	edp_vdd_put(dp);

	o->fixed_mode_valid = 0;
	info = kalloc(sizeof(*info));
	if (info && o->edid_len && drm_edid_parse(o->edid, (unsigned)o->edid_len, info) == 0) {
		dp->edid_bpc = info->bpc;
		if (info->preferred >= 0 && info->preferred < info->nmodes) {
			o->fixed_mode = info->modes[info->preferred];
			o->fixed_mode_valid = 1;
		}
		o->mm_width = info->mm_width;
		o->mm_height = info->mm_height;
	}
	if (info)
		kfree(info);
	if (!o->fixed_mode_valid && d->vbt.lfp_mode_valid) {
		o->fixed_mode = d->vbt.lfp_mode;
		o->fixed_mode_valid = 1;
	}
	if (!o->mm_width || !o->mm_height) {
		o->mm_width = d->vbt.lfp_width_mm;
		o->mm_height = d->vbt.lfp_height_mm;
	}
	if (!o->fixed_mode_valid) {
		kprintf("[drm] i915: %s: no fixed mode for the panel, disabling eDP\n", o->name);
		return -ENODEV;
	}
	o->fixed_mode.type |= DRM_MODE_TYPE_PREFERRED;
	o->is_panel = 1;

	lg_backlight_setup(d, o, edp_backlight_initial_pipe(dp));
	return 0;
}

int lg_dp_init(struct lg_display *d, uint32_t reg, int port)
{
	struct lg_output *o;
	struct lg_dp *dp;
	int is_edp = dp_port_is_edp(d, port);
	int aux_ch = dp_pick_aux_ch(d, port);

	if (port < LG_PORT_A || port > LG_PORT_D)
		return 0;
	if (port == LG_PORT_A && !dp_pch_split(d))
		return 0;

	for (int i = 0; i < d->nout; i++) {
		struct lg_output *other = &d->out[i];

		/* The PCH has one panel sequencer: an LVDS panel already
		 * owns it. */
		if (is_edp && other->type == LG_OUTPUT_LVDS && d->pch != LG_PCH_NONE) {
			kprintf("[drm] i915: LVDS was detected, not registering eDP on port %c\n",
				port_name(port));
			return 0;
		}
		if (other->funcs == &lg_dp_funcs && other->priv &&
		    ((struct lg_dp *)other->priv)->aux_ch == aux_ch) {
			kprintf("[drm] i915: DP %c: AUX %c already claimed by %s\n", port_name(port),
				port_name(aux_ch), other->name);
			return 0;
		}
	}

	o = lg_output_new(d);
	if (!o)
		return 0;
	dp = kalloc(sizeof(*dp));
	if (!dp) {
		dp_output_release(d, o);
		return 0;
	}
	mm_memset(dp, 0, sizeof(*dp));
	dp->d = d;
	dp->o = o;
	dp->is_edp = is_edp;
	dp->aux_ch = aux_ch;
	dp->DP = lg_rd(d, reg);

	o->priv = dp;
	o->type = is_edp ? LG_OUTPUT_EDP : LG_OUTPUT_DP;
	o->port = port;
	o->reg = reg;
	if (!is_edp)
		ksnprintf(o->name, sizeof(o->name), "DP-%c", port_name(port));
	else if (port == LG_PORT_A)
		ksnprintf(o->name, sizeof(o->name), "eDP");
	else
		ksnprintf(o->name, sizeof(o->name), "eDP-%c", port_name(port));
	o->funcs = &lg_dp_funcs;
	o->conn_type = is_edp ? DRM_MODE_CONNECTOR_eDP : DRM_MODE_CONNECTOR_DisplayPort;
	o->enc_type = DRM_MODE_ENCODER_TMDS;
	if (d->is_chv)
		o->pipe_mask = port == LG_PORT_D ? (1u << 2) : ((1u << 0) | (1u << 1));
	else
		o->pipe_mask = (1u << d->num_pipes) - 1u;
	if (is_edp) {
		/* a panel is never hotplug-probed */
		o->hpd_pin = LG_HPD_NONE;
		o->polled = 0;
	} else {
		o->hpd_pin = port == LG_PORT_B ? LG_HPD_PORT_B :
			     port == LG_PORT_C ? LG_HPD_PORT_C : LG_HPD_PORT_D;
		o->polled = 0;
	}
	o->ddc_pin = LG_GMBUS_PIN_DISABLED;

	/* G4X and the CPU eDP port reach 0.8 V and 6 dB; the PCH ports,
	 * Valleyview and Cherryview go to 1.2 V and 9.5 dB. */
	if (d->is_vlv || d->is_chv || (dp_pch_split(d) && port != LG_PORT_A)) {
		dp->voltage_max = LGDP_SWING(3);
		dp->preemph_max = LGDP_PREEMPH(3);
	} else {
		dp->voltage_max = LGDP_SWING(2);
		dp->preemph_max = LGDP_PREEMPH(2);
	}

	/* until a sink says otherwise: one lane at 1.62 Gbit/s */
	dp->sink_rates[0] = 162000;
	dp->num_sink_rates = 1;
	dp->max_sink_lanes = 1;

	dp_aux_init(dp);
	o->ddc = &dp->ddc;

	if (is_edp && edp_init(dp)) {
		dp_output_release(d, o);
		return 0;
	}

	dp_set_source_rates(dp);
	dp_set_common_link_params(dp);
	dp_reset_link_params(dp);

	if (is_edp)
		kprintf("[drm] i915: %s: panel %ux%u, link up to %d.%02d Gbit/s x%d (AUX %c)\n",
			o->name, o->fixed_mode.hdisplay, o->fixed_mode.vdisplay,
			dp->common_rates[dp->num_common_rates - 1] / 100000,
			(dp->common_rates[dp->num_common_rates - 1] / 1000) % 100, dp->max_lanes,
			port_name(aux_ch));
	else
		kprintf("[drm] i915: %s: DisplayPort on port %c (AUX %c)\n", o->name,
			port_name(port), port_name(aux_ch));
	return 1;
}
