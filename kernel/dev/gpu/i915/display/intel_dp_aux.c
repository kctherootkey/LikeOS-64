// LikeOS -- the DisplayPort AUX channel.
//
// A slow serial side channel to a DisplayPort sink: native reads and
// writes of its DPCD register space, and I2C tunnelled over it (how an
// eDP panel's EDID is read).  Each DDI port on Skylake has one AUX
// channel with a control register and five data registers; a
// transaction is written into the data registers as a header plus
// payload, started with the busy bit, and its reply read back the same
// way.  Tiger Lake numbers its Type-C ports' channels after A-C (USBC1
// is the fourth); Meteor Lake moved them into PICA, 0x200 apart, and powers
// every channel through a request bit in its control register, which a
// transfer must therefore keep set when it writes the register.  Display
// version 20 (Lunar Lake on) renumbered PICA's blocks so that the Type-C
// channels come first and A and B follow USBC4; Nova Lake's channels
// also report when their power is up.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2020-2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define AUX_NATIVE_WRITE 0x8
#define AUX_NATIVE_READ 0x9
#define AUX_I2C_WRITE 0x0
#define AUX_I2C_READ 0x1
#define AUX_I2C_WRITE_STATUS_UPDATE 0x2
#define AUX_I2C_MOT 0x4
#define AUX_NATIVE_REPLY_ACK 0x0
#define AUX_NATIVE_REPLY_NACK 0x1
#define AUX_NATIVE_REPLY_DEFER 0x2
#define AUX_I2C_REPLY_ACK 0x0
#define AUX_I2C_REPLY_NACK 0x4
#define AUX_I2C_REPLY_DEFER 0x8
#define AUX_MAX_PAYLOAD 16
#define AUX_RETRIES 32

static uint32_t pack_be(const uint8_t *b, unsigned n)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < 4; i++)
		v |= (uint32_t)(i < n ? b[i] : 0) << (24 - 8 * i);
	return v;
}

static void unpack_be(uint32_t v, uint8_t *b, unsigned n)
{
	for (unsigned i = 0; i < n && i < 4; i++)
		b[i] = (uint8_t)(v >> (24 - 8 * i));
}

static int aux_is_xelpdp(struct intel_dp_aux *aux)
{
	return aux->i915->display.model == INTEL_DISPLAY_MTL;
}

/* The sync pulses a transfer starts with: 16 precharge plus 16 preamble
 * clocks for an ordinary transfer, 10 plus 8 for a fast-wake one. */
#define AUX_SYNC_LEN (16 + 16)
#define AUX_FW_SYNC_LEN (10 + 8)

static uint32_t aux_ctl_value(struct intel_dp_aux *aux, unsigned send_bytes)
{
	uint32_t v = DP_AUX_CH_CTL_SEND_BUSY | DP_AUX_CH_CTL_DONE |
		     DP_AUX_CH_CTL_INTERRUPT | DP_AUX_CH_CTL_TIME_OUT_ERROR |
		     DP_AUX_CH_CTL_TIME_OUT_MAX | DP_AUX_CH_CTL_RECEIVE_ERROR |
		     (send_bytes << DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT) |
		     DP_AUX_CH_CTL_FW_SYNC_PULSE_SKL(AUX_FW_SYNC_LEN) |
		     DP_AUX_CH_CTL_SYNC_PULSE_SKL(AUX_SYNC_LEN);

	/* a Type-C port in Thunderbolt mode talks through the TBT I/O */
	if (aux->tbt_io)
		v |= DP_AUX_CH_CTL_TBT_IO;
	/* the channel's power request lives in this register too */
	if (aux_is_xelpdp(aux))
		v |= XELPDP_DP_AUX_CH_CTL_POWER_REQUEST;
	return v;
}

/* Meteor Lake: the channel must be powered before it is used.  The
 * power domain normally keeps the request up; a transfer made without
 * it (a probe before the output is enabled) raises it here and gives
 * the channel the fixed 600 us it needs to come up. */
static void aux_power_up(struct intel_dp_aux *aux)
{
	struct i915_device *i915 = aux->i915;
	uint32_t v;

	if (!aux_is_xelpdp(aux))
		return;
	v = i915_read32(i915, aux->ctl_reg);
	if (v & XELPDP_DP_AUX_CH_CTL_POWER_REQUEST)
		return;
	i915_write32(i915, aux->ctl_reg, v | XELPDP_DP_AUX_CH_CTL_POWER_REQUEST);
	/* Nova Lake's LT PHYs report the channel up (within 2 ms); before
	 * them the status bit means nothing and the wait is fixed. */
	if (intel_has_lt_phy(i915)) {
		for (int t = 0; t < 200; t++) {
			if (i915_read32(i915, aux->ctl_reg) & XELPDP_DP_AUX_CH_CTL_POWER_STATUS)
				return;
			lapic_delay_us(10);
		}
		kprintf("[drm] i915: AUX %s: channel power did not come up\n", aux->i2c.name);
		return;
	}
	lapic_delay_us(600);
}

/* One transaction: `send' bytes (header + payload) out, the reply back
 * into `recv' (up to `recv_cap').  Returns the reply length (the first
 * byte is the reply header) or a negative errno. */
static int aux_xfer(struct intel_dp_aux *aux, const uint8_t *send,
		    unsigned send_bytes, uint8_t *recv, unsigned recv_cap)
{
	struct i915_device *i915 = aux->i915;
	uint32_t status = 0;

	if (send_bytes > 20 || recv_cap > 20)
		return -EINVAL;
	aux_power_up(aux);
	/* The channel must be idle. */
	for (int t = 0; t < 1000; t++) {
		status = i915_read32(i915, aux->ctl_reg);
		if (!(status & DP_AUX_CH_CTL_SEND_BUSY))
			break;
		lapic_delay_us(10);
	}
	if (status & DP_AUX_CH_CTL_SEND_BUSY) {
		aux->errors++;
		return -EBUSY;
	}
	int rc = -ETIMEDOUT;
	for (int attempt = 0; attempt < 5; attempt++) {
		for (unsigned i = 0; i < send_bytes; i += 4)
			i915_write32(i915, aux->data_reg + i,
				     pack_be(send + i,
					     send_bytes - i < 4 ? send_bytes - i : 4));
		i915_write32(i915, aux->ctl_reg, aux_ctl_value(aux, send_bytes));
		/* Done or errored, within a few hundred microseconds. */
		for (int t = 0; t < 2000; t++) {
			status = i915_read32(i915, aux->ctl_reg);
			if (!(status & DP_AUX_CH_CTL_SEND_BUSY))
				break;
			lapic_delay_us(5);
		}
		/* Clear the sticky bits. */
		i915_write32(i915, aux->ctl_reg,
			     status | DP_AUX_CH_CTL_DONE |
				     DP_AUX_CH_CTL_TIME_OUT_ERROR |
				     DP_AUX_CH_CTL_RECEIVE_ERROR);
		if (status & DP_AUX_CH_CTL_TIME_OUT_ERROR) {
			rc = -ETIMEDOUT;
			lapic_delay_us(500);
			continue;
		}
		if (status & DP_AUX_CH_CTL_RECEIVE_ERROR) {
			rc = -EIO;
			lapic_delay_us(500);
			continue;
		}
		if (!(status & DP_AUX_CH_CTL_DONE)) {
			rc = -EBUSY;
			continue;
		}
		unsigned got = (status & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >>
			       DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT;
		if (got == 0 || got > 20) {
			rc = -EIO;
			continue;
		}
		if (got > recv_cap)
			got = recv_cap;
		for (unsigned i = 0; i < got; i += 4)
			unpack_be(i915_read32(i915, aux->data_reg + i), recv + i,
				  got - i < 4 ? got - i : 4);
		return (int)got;
	}
	aux->errors++;
	return rc;
}

/* A request with retries on DEFER (the sink is busy) and short replies. */
static int aux_request(struct intel_dp_aux *aux, uint8_t cmd, uint32_t addr,
		       const uint8_t *wbuf, unsigned wlen, uint8_t *rbuf,
		       unsigned rlen, int i2c)
{
	uint8_t send[20], recv[20];
	unsigned n;

	if (wlen > AUX_MAX_PAYLOAD || rlen > AUX_MAX_PAYLOAD)
		return -EINVAL;
	send[0] = (uint8_t)((cmd << 4) | ((addr >> 16) & 0xf));
	send[1] = (uint8_t)(addr >> 8);
	send[2] = (uint8_t)addr;
	n = 3;
	int is_write = i2c ? ((cmd & 0x3) == AUX_I2C_WRITE) :
			     (cmd == AUX_NATIVE_WRITE);
	unsigned len = is_write ? wlen : rlen;
	/* A zero-length transaction (I2C address-only) has no length byte. */
	if (len) {
		send[3] = (uint8_t)(len - 1);
		n = 4;
		if (is_write) {
			for (unsigned i = 0; i < wlen; i++)
				send[4 + i] = wbuf[i];
			n += wlen;
		}
	}
	for (int retry = 0; retry < AUX_RETRIES; retry++) {
		int got = aux_xfer(aux, send, n, recv, sizeof(recv));
		if (got < 0)
			return got;
		uint8_t reply = recv[0] >> 4;
		uint8_t native = reply & 0x3;
		uint8_t i2c_reply = reply & 0xc;
		if (native == AUX_NATIVE_REPLY_NACK)
			return -EIO;
		if (native == AUX_NATIVE_REPLY_DEFER) {
			lapic_delay_us(400);
			continue;
		}
		if (i2c) {
			if (i2c_reply == AUX_I2C_REPLY_NACK)
				return -EIO;
			if (i2c_reply == AUX_I2C_REPLY_DEFER) {
				lapic_delay_us(400);
				continue;
			}
		}
		if (is_write)
			return (int)wlen;
		unsigned data = (unsigned)got - 1;
		if (data > rlen)
			data = rlen;
		for (unsigned i = 0; i < data; i++)
			rbuf[i] = recv[1 + i];
		if (data == 0 && rlen) {
			/* an ACK with no data: the sink wants another go */
			lapic_delay_us(200);
			continue;
		}
		return (int)data;
	}
	return -ETIMEDOUT;
}

int intel_dp_aux_native_read(struct intel_dp_aux *aux, uint32_t addr,
			     uint8_t *buf, unsigned len)
{
	unsigned done = 0;

	while (done < len) {
		unsigned chunk = len - done > AUX_MAX_PAYLOAD ? AUX_MAX_PAYLOAD :
								len - done;
		int got = aux_request(aux, AUX_NATIVE_READ, addr + done, NULL, 0,
				      buf + done, chunk, 0);
		if (got < 0)
			return done ? (int)done : got;
		if (got == 0)
			break;
		done += (unsigned)got;
	}
	return (int)done;
}

int intel_dp_aux_native_write(struct intel_dp_aux *aux, uint32_t addr,
			      const uint8_t *buf, unsigned len)
{
	unsigned done = 0;

	while (done < len) {
		unsigned chunk = len - done > AUX_MAX_PAYLOAD ? AUX_MAX_PAYLOAD :
								len - done;
		int rc = aux_request(aux, AUX_NATIVE_WRITE, addr + done,
				     buf + done, chunk, NULL, 0, 0);
		if (rc < 0)
			return done ? (int)done : rc;
		done += chunk;
	}
	return (int)done;
}

/* ---- I2C over AUX --------------------------------------------------------- */

static int aux_i2c_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int n)
{
	struct intel_dp_aux *aux = a->priv;
	int done = 0;

	for (int i = 0; i < n; i++) {
		struct i2c_msg *m = &msgs[i];
		int rd = !!(m->flags & I2C_M_RD);
		uint8_t cmd = (rd ? AUX_I2C_READ : AUX_I2C_WRITE);
		int last = (i == n - 1);
		unsigned off = 0;

		/* Middle-of-transaction on every message but the last, so
		 * the sink keeps the address between them. */
		if (m->len == 0) {
			int rc = aux_request(aux, cmd | (last ? 0 : AUX_I2C_MOT),
					     m->addr, NULL, 0, NULL, 0, 1);
			if (rc < 0)
				return done ? done : rc;
			done++;
			continue;
		}
		while (off < m->len) {
			unsigned chunk = m->len - off > AUX_MAX_PAYLOAD ?
						 AUX_MAX_PAYLOAD :
						 m->len - off;
			int mot = (last && off + chunk >= m->len) ? 0 : AUX_I2C_MOT;
			int rc = aux_request(aux, cmd | mot, m->addr,
					     rd ? NULL : m->buf + off, rd ? 0 : chunk,
					     rd ? m->buf + off : NULL, rd ? chunk : 0,
					     1);
			if (rc < 0)
				return done ? done : rc;
			if (rc == 0)
				return done ? done : -EIO;
			off += (unsigned)rc;
		}
		done++;
	}
	return done;
}

void intel_dp_aux_init(struct i915_device *i915, struct intel_dp_aux *aux,
		       int port)
{
	mm_memset(aux, 0, sizeof(*aux));
	aux->i915 = i915;
	aux->port = port;
	if (i915->display.model == INTEL_DISPLAY_MTL) {
		/* A, B and USBC1-4 (channel 3..6) */
		if (port == 2 || port > 6)
			kprintf("[drm] i915: AUX channel %d does not exist here; using A\n", port);
		int ch = (port == 2 || port > 6) ? 0 : port;
		int ver = i915->info->display_ver;
		aux->ctl_reg = XELPDP_DP_AUX_CH_CTL_VER(ver, ch);
		aux->data_reg = XELPDP_DP_AUX_CH_DATA_VER(ver, ch, 0);
	} else {
		/* Skylake has a DDI E but no AUX E: it shares A.  From Tiger
		 * Lake the Type-C channels (USBC1.., index 3..) follow A-C at
		 * the same stride. */
		int ch = port;
		if (i915->info->display_ver == 9 && port == PORT_E)
			ch = PORT_A;
		aux->ctl_reg = DP_AUX_CH_CTL(ch);
		aux->data_reg = DP_AUX_CH_DATA(ch, 0);
	}
	aux->i2c.xfer = aux_i2c_xfer;
	aux->i2c.priv = aux;
	aux->i2c.name[0] = 'A';
	aux->i2c.name[1] = 'U';
	aux->i2c.name[2] = 'X';
	if ((i915->display.model == INTEL_DISPLAY_TGL ||
	     i915->display.model == INTEL_DISPLAY_MTL) && port >= PORT_TC1) {
		/* "AUX USBC1" .. */
		const char *u = " USBC";
		int i = 3;
		for (; *u; u++)
			aux->i2c.name[i++] = *u;
		aux->i2c.name[i++] = (char)('1' + port - PORT_TC1);
		aux->i2c.name[i] = 0;
	} else {
		aux->i2c.name[3] = (char)('A' + port);
		aux->i2c.name[4] = 0;
	}
}
