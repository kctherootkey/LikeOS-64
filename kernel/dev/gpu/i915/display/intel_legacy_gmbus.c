// LikeOS -- GMBUS and the GPIO I2C pairs of the Intel parts before DDI.
//
// Every DDC wire of these parts -- the analog connector's, the panel's,
// the SDVO and HDMI ports' -- and the control bus of an SDVO or DVO
// encoder chip ends at one of six GPIO pin pairs.  The display engine's
// GMBUS controller can drive any one of them as an I2C master: GMBUS0
// selects the pair and the rate, GMBUS1 takes a command (target address,
// direction, byte count, cycle type), and data moves through GMBUS3 four
// bytes at a time, the controller raising HW_RDY whenever it has filled
// or emptied the register.  A "wait" cycle parks the bus after a message
// so the next follows with a repeated START; a one- or two-byte write
// immediately followed by a transfer to the same target folds into one
// "index" cycle.  A target that does not acknowledge shows as SATOER and
// leaves the controller in an error state that only the software-clear
// bit undoes.
//
// The controller is not always usable: on the i830 it does not work at
// all, and some pins on some boards never answer through it.  Each pair
// can also be driven directly as two open-drain lines through its GPIO
// control register, so a transfer that times out on the controller is
// retried by bit-banging the same wires, and a pin can be forced to
// bit-banging for good (the i830, and the SDVO control bus when its
// encoder needs it).  On Pineview the GMBUS unit's clock gating must be
// held off while the GPIO lines are driven by hand.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2010 Intel Corporation
// Portions Copyright (C) 2006 Dave Airlie

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The controller gave up on the pins: retry the transfer bit-banged.  Kept
 * in the force_bit word above the counter lg_gmbus_force_bit() keeps. */
#define GMB_FORCE_BIT_RETRY (1u << 31)

/* Rate bits of GMBUS0. */
#define GMB_RATE_MASK (3u << 8)

/* Half a clock period of the bit-banged bus: 10 us, about 50 kHz with
 * the rise and fall times of the lines. */
#define GMB_BIT_UDELAY 10u
/* How long a target may stretch the clock low. */
#define GMB_BIT_STRETCH_TIMEOUT_US 2200u
/* Address retries of the bit-banged master (the controller retries a
 * NAK on the first message itself). */
#define GMB_BIT_ADDR_RETRIES 1

/* Status polls of the controller: the bit (or a NAK) within 50 ms, idle
 * within 10 ms. */
#define GMB_WAIT_TIMEOUT_US 50000u
#define GMB_IDLE_TIMEOUT_US 10000u

/* The byte counter of GMBUS1 takes up to 256 on these parts. */
#define GMB_MAX_XFER_SIZE GMBUS_BYTE_COUNT_MAX

/* The GPIO register each pin pair is wired to (GPIOA = 0 .. GPIOF = 5). */
enum gmb_gpio {
	GMB_GPIOA = 0,
	GMB_GPIOB,
	GMB_GPIOC,
	GMB_GPIOD,
	GMB_GPIOE,
	GMB_GPIOF,
};

struct gmb_pin {
	const char *name;
	int gpio;
};

/* The pin table of every part here: the GMCH parts, Ironlake to Ivy
 * Bridge (Ibex Peak, Cougar/Panther Point) and Valleyview/Cherryview.
 * Cherryview wires HDMI D's DDC to the panel pair (pin 3). */
static const struct gmb_pin gmb_pins[LG_GMBUS_NUM_PINS] = {
	[LG_GMBUS_PIN_SSC] = { "ssc", GMB_GPIOB },
	[LG_GMBUS_PIN_VGADDC] = { "vga", GMB_GPIOA },
	[LG_GMBUS_PIN_PANEL] = { "panel", GMB_GPIOC },
	[LG_GMBUS_PIN_DPC] = { "dpc", GMB_GPIOD },
	[LG_GMBUS_PIN_DPB] = { "dpb", GMB_GPIOE },
	[LG_GMBUS_PIN_DPD] = { "dpd", GMB_GPIOF },
};

struct gmb_bus {
	struct i2c_adapter adapter;
	struct lg_display *d;
	unsigned pin;
	/* Bit-bang instead of using the controller: a counter of callers
	 * that asked for it, plus GMB_FORCE_BIT_RETRY after a timeout. */
	uint32_t force_bit;
	/* GMBUS0 for this pin: the pin number and the rate. */
	uint32_t reg0;
	/* The pin pair's GPIO control register. */
	uint32_t gpio_reg;
};

static struct gmb_bus g_bus[LG_GMBUS_NUM_PINS];

/* ---- pins ---------------------------------------------------------------------- */

int lg_gmbus_pin_valid(struct lg_display *d, unsigned pin)
{
	(void)d;
	return pin < LG_GMBUS_NUM_PINS && gmb_pins[pin].name != NULL;
}

/* ---- the controller's register block --------------------------------------------- */

static uint32_t gmb_rd(struct lg_display *d, uint32_t reg)
{
	return lg_rd(d, reg);
}

static void gmb_wr(struct lg_display *d, uint32_t reg, uint32_t val)
{
	lg_wr(d, reg, val);
}

static void gmb_hw_reset(struct lg_display *d)
{
	gmb_wr(d, GMBUS0_REG(d->gmbus_base), 0);
	gmb_wr(d, GMBUS4_REG(d->gmbus_base), 0);
}

/* Pineview: while the GPIO lines are driven by hand the GMBUS unit must
 * not be clock gated. */
static void pnv_gmbus_clock_gating(struct lg_display *d, int enable)
{
	lg_rmw(d, DSPCLK_GATE_D, PNV_GMBUSUNIT_CLOCK_GATE_DISABLE,
	       enable ? 0 : PNV_GMBUSUNIT_CLOCK_GATE_DISABLE);
}

/* ---- the GPIO pair, line by line ------------------------------------------------- */

/* The register's pull-up disable bits belong to the board setup and must
 * be written back as found (not on the i830 and i845G, which have no such
 * bits).  The direction and value bits only take effect together with
 * their mask bits, so a write without a mask leaves that line alone. */
static uint32_t gpio_reserved(struct gmb_bus *bus)
{
	struct lg_display *d = bus->d;

	if (d->is_i830 || d->is_i845)
		return 0;
	return lg_rd(d, bus->gpio_reg) & (GPIO_DATA_PULLUP_DISABLE | GPIO_CLOCK_PULLUP_DISABLE);
}

static int gpio_get_clock(struct gmb_bus *bus)
{
	struct lg_display *d = bus->d;
	uint32_t reserved = gpio_reserved(bus);

	/* release the line (direction in), then sample it */
	lg_wr(d, bus->gpio_reg, reserved | GPIO_CLOCK_DIR_MASK);
	lg_wr(d, bus->gpio_reg, reserved);
	return (lg_rd(d, bus->gpio_reg) & GPIO_CLOCK_VAL_IN) != 0;
}

static int gpio_get_data(struct gmb_bus *bus)
{
	struct lg_display *d = bus->d;
	uint32_t reserved = gpio_reserved(bus);

	lg_wr(d, bus->gpio_reg, reserved | GPIO_DATA_DIR_MASK);
	lg_wr(d, bus->gpio_reg, reserved);
	return (lg_rd(d, bus->gpio_reg) & GPIO_DATA_VAL_IN) != 0;
}

/* High is the line released to its pull-up (direction in); low is the
 * line driven to zero. */
static void gpio_set_clock(struct gmb_bus *bus, int high)
{
	struct lg_display *d = bus->d;
	uint32_t reserved = gpio_reserved(bus);
	uint32_t bits;

	if (high)
		bits = GPIO_CLOCK_DIR_IN | GPIO_CLOCK_DIR_MASK;
	else
		bits = GPIO_CLOCK_DIR_OUT | GPIO_CLOCK_DIR_MASK | GPIO_CLOCK_VAL_MASK;
	lg_wr(d, bus->gpio_reg, reserved | bits);
	lg_posting_read(d, bus->gpio_reg);
}

static void gpio_set_data(struct gmb_bus *bus, int high)
{
	struct lg_display *d = bus->d;
	uint32_t reserved = gpio_reserved(bus);
	uint32_t bits;

	if (high)
		bits = GPIO_DATA_DIR_IN | GPIO_DATA_DIR_MASK;
	else
		bits = GPIO_DATA_DIR_OUT | GPIO_DATA_DIR_MASK | GPIO_DATA_VAL_MASK;
	lg_wr(d, bus->gpio_reg, reserved | bits);
	lg_posting_read(d, bus->gpio_reg);
}

/* Before the lines are driven by hand: the controller off the pins, the
 * Pineview clock gating held off, both lines released. */
static void gpio_pre_xfer(struct gmb_bus *bus)
{
	struct lg_display *d = bus->d;

	gmb_hw_reset(d);
	if (d->is_pnv)
		pnv_gmbus_clock_gating(d, 0);
	gpio_set_data(bus, 1);
	gpio_set_clock(bus, 1);
	lg_udelay(GMB_BIT_UDELAY);
}

static void gpio_post_xfer(struct gmb_bus *bus)
{
	struct lg_display *d = bus->d;

	gpio_set_data(bus, 1);
	gpio_set_clock(bus, 1);
	if (d->is_pnv)
		pnv_gmbus_clock_gating(d, 1);
}

/* ---- the bit-banged I2C master ------------------------------------------------- */

static void bit_sdalo(struct gmb_bus *bus)
{
	gpio_set_data(bus, 0);
	lg_udelay((GMB_BIT_UDELAY + 1) / 2);
}

static void bit_sdahi(struct gmb_bus *bus)
{
	gpio_set_data(bus, 1);
	lg_udelay((GMB_BIT_UDELAY + 1) / 2);
}

static void bit_scllo(struct gmb_bus *bus)
{
	gpio_set_clock(bus, 0);
	lg_udelay(GMB_BIT_UDELAY / 2);
}

/* Release the clock and wait until it is really high: a target holds it
 * low while it is not ready (clock stretching). */
static int bit_sclhi(struct gmb_bus *bus)
{
	uint32_t waited = 0;

	gpio_set_clock(bus, 1);
	while (!gpio_get_clock(bus)) {
		if (waited >= GMB_BIT_STRETCH_TIMEOUT_US) {
			/* one last look: we may have been away a while */
			if (gpio_get_clock(bus))
				break;
			return -ETIMEDOUT;
		}
		lg_udelay(1);
		waited++;
	}
	lg_udelay(GMB_BIT_UDELAY);
	return 0;
}

/* START: data falls while the clock is high (both are high on entry). */
static void bit_start(struct gmb_bus *bus)
{
	gpio_set_data(bus, 0);
	lg_udelay(GMB_BIT_UDELAY);
	bit_scllo(bus);
}

/* Repeated START: release data, raise the clock, then pull data low. */
static int bit_repstart(struct gmb_bus *bus)
{
	int rc;

	bit_sdahi(bus);
	rc = bit_sclhi(bus);
	if (rc)
		return rc;
	gpio_set_data(bus, 0);
	lg_udelay(GMB_BIT_UDELAY);
	bit_scllo(bus);
	return 0;
}

/* STOP: data rises while the clock is high. */
static void bit_stop(struct gmb_bus *bus)
{
	bit_sdalo(bus);
	(void)bit_sclhi(bus);
	gpio_set_data(bus, 1);
	lg_udelay(GMB_BIT_UDELAY);
}

/* Shift a byte out, MSB first, and clock in the acknowledge: 1 when the
 * target pulled data low, 0 for a NAK, -ETIMEDOUT when the clock stuck. */
static int bit_outb(struct gmb_bus *bus, uint8_t c)
{
	int i, ack;

	for (i = 7; i >= 0; i--) {
		gpio_set_data(bus, (c >> i) & 1);
		lg_udelay((GMB_BIT_UDELAY + 1) / 2);
		if (bit_sclhi(bus) < 0)
			return -ETIMEDOUT;
		bit_scllo(bus);
	}
	bit_sdahi(bus);
	if (bit_sclhi(bus) < 0)
		return -ETIMEDOUT;
	ack = !gpio_get_data(bus);
	bit_scllo(bus);
	return ack;
}

/* Clock a byte in, MSB first; the caller sends the ACK or NAK. */
static int bit_inb(struct gmb_bus *bus)
{
	int i;
	uint8_t v = 0;

	bit_sdahi(bus);
	for (i = 0; i < 8; i++) {
		if (bit_sclhi(bus) < 0)
			return -ETIMEDOUT;
		v <<= 1;
		if (gpio_get_data(bus))
			v |= 1;
		gpio_set_clock(bus, 0);
		lg_udelay(i == 7 ? GMB_BIT_UDELAY / 2 : GMB_BIT_UDELAY);
	}
	return v;
}

static int bit_acknak(struct gmb_bus *bus, int ack)
{
	if (ack)
		gpio_set_data(bus, 0);
	lg_udelay((GMB_BIT_UDELAY + 1) / 2);
	if (bit_sclhi(bus) < 0)
		return -ETIMEDOUT;
	bit_scllo(bus);
	return 0;
}

/* Send the address byte, retrying with a fresh START when the target
 * does not answer the first time. */
static int bit_address(struct gmb_bus *bus, uint8_t addr)
{
	int i, rc = 0;

	for (i = 0; i <= GMB_BIT_ADDR_RETRIES; i++) {
		rc = bit_outb(bus, addr);
		if (rc == 1 || i == GMB_BIT_ADDR_RETRIES)
			break;
		bit_stop(bus);
		lg_udelay(GMB_BIT_UDELAY);
		bit_start(bus);
	}
	return rc;
}

static int bit_xfer(struct gmb_bus *bus, struct i2c_msg *msgs, int num)
{
	int i, rc = 0;

	gpio_pre_xfer(bus);
	bit_start(bus);
	for (i = 0; i < num; i++) {
		struct i2c_msg *m = &msgs[i];
		uint8_t addr = (uint8_t)((m->addr << 1) | ((m->flags & I2C_M_RD) ? 1 : 0));
		unsigned k;

		if (i) {
			rc = bit_repstart(bus);
			if (rc)
				goto stop;
		}
		rc = bit_address(bus, addr);
		if (rc != 1) {
			i915_dbg("[drm] i915: gmbus %s: no ack from addr 0x%02x (bit-bang)\n",
				 bus->adapter.name, m->addr);
			rc = rc < 0 ? rc : -ENXIO;
			goto stop;
		}
		if (m->flags & I2C_M_RD) {
			for (k = 0; k < m->len; k++) {
				int v = bit_inb(bus);

				if (v < 0) {
					rc = v;
					goto stop;
				}
				m->buf[k] = (uint8_t)v;
				/* acknowledge every byte but the last */
				rc = bit_acknak(bus, k + 1 < m->len);
				if (rc)
					goto stop;
			}
		} else {
			for (k = 0; k < m->len; k++) {
				rc = bit_outb(bus, m->buf[k]);
				if (rc != 1) {
					rc = rc < 0 ? rc : -EIO;
					goto stop;
				}
			}
		}
	}
	rc = num;
stop:
	bit_stop(bus);
	gpio_post_xfer(bus);
	return rc;
}

/* ---- the controller ---------------------------------------------------------------- */

/* Wait for a GMBUS2 status bit; a NAK (SATOER) ends the wait early.  The
 * controller signals only the first bit it is told to, and the NAK has
 * to be checked as well, so this polls: briefly at first, then in short
 * steps up to 50 ms. */
static int gmbus_wait(struct lg_display *d, uint32_t status)
{
	uint32_t reg = GMBUS2_REG(d->gmbus_base);
	uint32_t gmbus2 = 0;
	uint32_t t;
	int ret = -ETIMEDOUT;

	status |= GMBUS_SATOER;
	for (t = 0; t <= 2; t++) {
		gmbus2 = gmb_rd(d, reg);
		if (gmbus2 & status) {
			ret = 0;
			break;
		}
		lg_udelay(1);
	}
	for (t = 0; ret && t < GMB_WAIT_TIMEOUT_US; t += 10) {
		lg_udelay(10);
		gmbus2 = gmb_rd(d, reg);
		if (gmbus2 & status)
			ret = 0;
	}
	if (gmbus2 & GMBUS_SATOER)
		return -ENXIO;
	return ret;
}

static int gmbus_wait_idle(struct lg_display *d)
{
	return lg_wait(d, GMBUS2_REG(d->gmbus_base), GMBUS_ACTIVE, 0, GMB_IDLE_TIMEOUT_US);
}

static int gmbus_xfer_read_chunk(struct lg_display *d, uint16_t addr, uint8_t *buf,
				 unsigned len, uint32_t gmbus0_reg, uint32_t gmbus1_index)
{
	unsigned size = len;
	int burst_read = len > GMB_MAX_XFER_SIZE;
	int extra_byte_added = 0;

	if (burst_read) {
		/* A burst of 512 bytes reads one more and drops it. */
		if (len == 512) {
			extra_byte_added = 1;
			len++;
		}
		size = len % 256 + 256;
		gmb_wr(d, GMBUS0_REG(d->gmbus_base), gmbus0_reg | GMBUS_BYTE_CNT_OVERRIDE);
	}

	gmb_wr(d, GMBUS1_REG(d->gmbus_base),
	       gmbus1_index | GMBUS_CYCLE_WAIT | (size << GMBUS_BYTE_COUNT_SHIFT) |
		       ((uint32_t)addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_READ |
		       GMBUS_SW_RDY);
	while (len) {
		uint32_t val, loop = 0;
		int ret = gmbus_wait(d, GMBUS_HW_RDY);

		if (ret)
			return ret;
		val = gmb_rd(d, GMBUS3_REG(d->gmbus_base));
		do {
			if (extra_byte_added && len == 1) {
				len--;
				break;
			}
			*buf++ = (uint8_t)(val & 0xff);
			val >>= 8;
		} while (--len && ++loop < 4);

		if (burst_read && len == size - 4)
			/* the counter override off again */
			gmb_wr(d, GMBUS0_REG(d->gmbus_base), gmbus0_reg);
	}
	return 0;
}

static int gmbus_xfer_read(struct lg_display *d, struct i2c_msg *msg, uint32_t gmbus0_reg,
			   uint32_t gmbus1_index)
{
	uint8_t *buf = msg->buf;
	unsigned rx_size = msg->len;
	unsigned len;
	int ret;

	do {
		len = rx_size < GMB_MAX_XFER_SIZE ? rx_size : GMB_MAX_XFER_SIZE;
		ret = gmbus_xfer_read_chunk(d, msg->addr, buf, len, gmbus0_reg, gmbus1_index);
		if (ret)
			return ret;
		rx_size -= len;
		buf += len;
	} while (rx_size != 0);
	return 0;
}

static int gmbus_xfer_write_chunk(struct lg_display *d, uint16_t addr, const uint8_t *buf,
				  unsigned len, uint32_t gmbus1_index)
{
	unsigned chunk_size = len;
	uint32_t val = 0, loop = 0;

	/* the first word goes in before the command */
	while (len && loop < 4) {
		val |= (uint32_t)(*buf++) << (8 * loop++);
		len--;
	}
	gmb_wr(d, GMBUS3_REG(d->gmbus_base), val);
	gmb_wr(d, GMBUS1_REG(d->gmbus_base),
	       gmbus1_index | GMBUS_CYCLE_WAIT | (chunk_size << GMBUS_BYTE_COUNT_SHIFT) |
		       ((uint32_t)addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_WRITE |
		       GMBUS_SW_RDY);
	while (len) {
		int ret;

		val = loop = 0;
		do {
			val |= (uint32_t)(*buf++) << (8 * loop);
		} while (--len && ++loop < 4);
		gmb_wr(d, GMBUS3_REG(d->gmbus_base), val);

		ret = gmbus_wait(d, GMBUS_HW_RDY);
		if (ret)
			return ret;
	}
	return 0;
}

static int gmbus_xfer_write(struct lg_display *d, struct i2c_msg *msg, uint32_t gmbus1_index)
{
	const uint8_t *buf = msg->buf;
	unsigned tx_size = msg->len;
	unsigned len;
	int ret;

	do {
		len = tx_size < GMB_MAX_XFER_SIZE ? tx_size : GMB_MAX_XFER_SIZE;
		ret = gmbus_xfer_write_chunk(d, msg->addr, buf, len, gmbus1_index);
		if (ret)
			return ret;
		buf += len;
		tx_size -= len;
	} while (tx_size != 0);
	return 0;
}

/* A one- or two-byte write followed by another transfer to the same
 * target is what an offset read (or write) looks like; the controller
 * does it as one INDEX cycle. */
static int gmbus_is_index_xfer(const struct i2c_msg *msgs, int i, int num)
{
	return i + 1 < num && msgs[i].addr == msgs[i + 1].addr &&
	       !(msgs[i].flags & I2C_M_RD) && (msgs[i].len == 1 || msgs[i].len == 2) &&
	       msgs[i + 1].len > 0;
}

static int gmbus_index_xfer(struct lg_display *d, struct i2c_msg *msgs, uint32_t gmbus0_reg)
{
	uint32_t gmbus1_index = 0;
	uint32_t gmbus5 = 0;
	int ret;

	/* a two-byte index goes through GMBUS5, high byte first on the wire */
	if (msgs[0].len == 2)
		gmbus5 = GMBUS_2BYTE_INDEX_EN | msgs[0].buf[1] | ((uint32_t)msgs[0].buf[0] << 8);
	if (msgs[0].len == 1)
		gmbus1_index = GMBUS_CYCLE_INDEX |
			       ((uint32_t)msgs[0].buf[0] << GMBUS_SLAVE_INDEX_SHIFT);

	if (gmbus5)
		gmb_wr(d, GMBUS5_REG(d->gmbus_base), gmbus5);

	if (msgs[1].flags & I2C_M_RD)
		ret = gmbus_xfer_read(d, &msgs[1], gmbus0_reg, gmbus1_index);
	else
		ret = gmbus_xfer_write(d, &msgs[1], gmbus1_index);

	/* GMBUS5 back to zero after every index transfer */
	if (gmbus5)
		gmb_wr(d, GMBUS5_REG(d->gmbus_base), 0);
	return ret;
}

/* One transfer through the controller: the number of messages, -ENXIO
 * for a NAK, -ETIMEDOUT, or -EAGAIN when the controller stalled and the
 * pins should be bit-banged instead. */
static int do_gmbus_xfer(struct gmb_bus *bus, struct i2c_msg *msgs, int num)
{
	struct lg_display *d = bus->d;
	uint32_t gbase = d->gmbus_base;
	int i = 0, inc, try = 0;
	int ret = 0;

retry:
	gmb_wr(d, GMBUS0_REG(gbase), bus->reg0);

	for (; i < num; i += inc) {
		inc = 1;
		if (gmbus_is_index_xfer(msgs, i, num)) {
			ret = gmbus_index_xfer(d, &msgs[i], bus->reg0);
			inc = 2; /* an index transfer is two messages */
		} else if (msgs[i].flags & I2C_M_RD) {
			ret = gmbus_xfer_read(d, &msgs[i], bus->reg0, 0);
		} else {
			ret = gmbus_xfer_write(d, &msgs[i], 0);
		}

		if (!ret)
			ret = gmbus_wait(d, GMBUS_HW_WAIT_PHASE);
		if (ret == -ETIMEDOUT)
			goto timeout;
		else if (ret)
			goto clear_err;
	}

	/* The controller cannot put a STOP on the very first cycle, so the
	 * STOP always gets a cycle of its own. */
	gmb_wr(d, GMBUS1_REG(gbase), GMBUS_CYCLE_STOP | GMBUS_SW_RDY);

	/* Idle, then the controller off the pins until the next transfer. */
	if (gmbus_wait_idle(d)) {
		i915_dbg("[drm] i915: gmbus %s: timed out waiting for idle\n", bus->adapter.name);
		ret = -ETIMEDOUT;
	}
	gmb_wr(d, GMBUS0_REG(gbase), 0);
	return ret ? ret : i;

clear_err:
	/*
	 * The bus must be idle before the NAK is cleared, or it stays
	 * active and the next transfer fails.  Only a properly quiesced
	 * controller reports -ENXIO: a timeout here tends to mean a slow
	 * target that answers on the second try, and -ENXIO would stop the
	 * EDID reader from retrying.
	 */
	ret = -ENXIO;
	if (gmbus_wait_idle(d)) {
		i915_dbg("[drm] i915: gmbus %s: timed out after NAK\n", bus->adapter.name);
		ret = -ETIMEDOUT;
	}

	/* Toggling the software clear bit resets the controller and with
	 * it the bus error the NAK raised. */
	gmb_wr(d, GMBUS1_REG(gbase), GMBUS_SW_CLR_INT);
	gmb_wr(d, GMBUS1_REG(gbase), 0);
	gmb_wr(d, GMBUS0_REG(gbase), 0);

	i915_dbg("[drm] i915: gmbus %s: NAK for addr %04x %c(%d)\n", bus->adapter.name,
		 msgs[i].addr, (msgs[i].flags & I2C_M_RD) ? 'r' : 'w', msgs[i].len);

	/* Passive adapters sometimes NAK the very first START: retry the
	 * first message once. */
	if (ret == -ENXIO && i == 0 && try++ == 0) {
		i915_dbg("[drm] i915: gmbus %s: NAK on first message, retry\n",
			 bus->adapter.name);
		goto retry;
	}
	return ret;

timeout:
	i915_dbg("[drm] i915: gmbus %s: timed out, falling back to bit banging on pin %u\n",
		 bus->adapter.name, bus->reg0 & 0xff);
	gmb_wr(d, GMBUS0_REG(gbase), 0);
	/* The pins may not work through the controller at all: let the
	 * caller go round again bit-banged. */
	return -EAGAIN;
}

/* ---- the adapter ------------------------------------------------------------------- */

static void gmb_lock(struct lg_display *d)
{
	/* One transfer at a time; every caller runs in process context
	 * (probes, the hotplug worker), so a waiting one just yields. */
	while (__sync_lock_test_and_set(&d->gmbus_busy, 1))
		sched_yield_in_kernel();
}

static void gmb_unlock(struct lg_display *d)
{
	__sync_lock_release(&d->gmbus_busy);
}

static int gmb_xfer_once(struct gmb_bus *bus, struct i2c_msg *msgs, int num)
{
	int ret;

	if (bus->force_bit) {
		ret = bit_xfer(bus, msgs, num);
		/* the controller gets another chance next time */
		if (ret < 0)
			bus->force_bit &= ~GMB_FORCE_BIT_RETRY;
	} else {
		ret = do_gmbus_xfer(bus, msgs, num);
		if (ret == -EAGAIN)
			bus->force_bit |= GMB_FORCE_BIT_RETRY;
	}
	return ret;
}

static int gmb_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int num)
{
	struct gmb_bus *bus = a->priv;
	struct lg_display *d;
	int ret = -EAGAIN, try;

	if (!bus || !bus->d)
		return -ENODEV;
	if (num <= 0)
		return 0;
	d = bus->d;

	gmb_lock(d);
	/* A controller timeout comes back as -EAGAIN with the pin switched
	 * to bit-banging: one more round does it by hand. */
	for (try = 0; try <= 1; try++) {
		ret = gmb_xfer_once(bus, msgs, num);
		if (ret != -EAGAIN)
			break;
	}
	gmb_unlock(d);
	return ret;
}

struct i2c_adapter *lg_gmbus_adapter(struct lg_display *d, unsigned pin)
{
	if (!lg_gmbus_pin_valid(d, pin) || g_bus[pin].d != d)
		return NULL;
	return &g_bus[pin].adapter;
}

void lg_gmbus_force_bit(struct lg_display *d, unsigned pin, int force)
{
	struct gmb_bus *bus;

	if (!lg_gmbus_pin_valid(d, pin) || g_bus[pin].d != d)
		return;
	bus = &g_bus[pin];
	gmb_lock(d);
	if (force)
		bus->force_bit++;
	else if (bus->force_bit & ~GMB_FORCE_BIT_RETRY)
		bus->force_bit--;
	i915_dbg("[drm] i915: %sabling bit-banging on %s, force bit now %u\n",
		 force ? "en" : "dis", bus->adapter.name, bus->force_bit);
	gmb_unlock(d);
}

void lg_gmbus_set_speed(struct lg_display *d, unsigned pin, uint32_t rate)
{
	struct gmb_bus *bus;

	if (!lg_gmbus_pin_valid(d, pin) || g_bus[pin].d != d)
		return;
	bus = &g_bus[pin];
	bus->reg0 = (bus->reg0 & ~GMB_RATE_MASK) | (rate & GMB_RATE_MASK);
}

void lg_gmbus_reset(struct lg_display *d)
{
	unsigned pin;

	gmb_hw_reset(d);
	/* the controller is fresh: let it try every pin again */
	for (pin = 0; pin < LG_GMBUS_NUM_PINS; pin++)
		if (g_bus[pin].d == d)
			g_bus[pin].force_bit &= ~GMB_FORCE_BIT_RETRY;
}

int lg_gmbus_init(struct lg_display *d)
{
	unsigned pin;

	d->gmbus_busy = 0;
	mm_memset(g_bus, 0, sizeof(g_bus));

	for (pin = 0; pin < LG_GMBUS_NUM_PINS; pin++) {
		struct gmb_bus *bus = &g_bus[pin];

		if (!lg_gmbus_pin_valid(d, pin))
			continue;
		bus->d = d;
		bus->pin = pin;
		bus->adapter.xfer = gmb_xfer;
		bus->adapter.priv = bus;
		ksnprintf(bus->adapter.name, sizeof(bus->adapter.name), "i915 gmbus %s",
			  gmb_pins[pin].name);
		/* a conservative rate to start with */
		bus->reg0 = pin | GMBUS_RATE_100KHZ;
		/* the controller does not work on the i830 */
		if (d->is_i830)
			bus->force_bit = 1;
		bus->gpio_reg = GPIO_CTL(d->gmbus_base, gmb_pins[pin].gpio);
	}

	gmb_hw_reset(d);
	return 0;
}
