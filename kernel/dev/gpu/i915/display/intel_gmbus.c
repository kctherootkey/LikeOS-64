// LikeOS -- GMBUS, the I2C controller of the display engine.
//
// Every DDC line of the integrated graphics device (the EDID wire of an
// HDMI, DVI or VGA sink) ends at this one controller, which multiplexes
// them by a pin number.  A transfer is a command in GMBUS1 (address,
// direction, byte count, cycle type) and data through GMBUS3 four bytes
// at a time, the controller pausing between words until the driver has
// read or refilled the register.  The "wait" cycle parks the bus after a
// message so the next one follows with a repeated START; an explicit
// "stop" cycle ends the transfer.  A write-then-read pair with a one-byte
// index (the DDC offset) folds into a single "index" cycle.
//
// Which pins exist, which pin the VBT means by the number it gives (from
// Cannon Point on it counts DDC buses, not pins), and which pin a port
// uses when the VBT says nothing, all depend on the PCH and the
// platform's port-to-PHY wiring.  Lunar Lake and the parts after it (the
// south display on the SoC, the "Lunar Lake" kind) and Battlemage keep
// Meteor Point's pins and bus numbering.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The registers are at the south display's offsets on every part this
 * driver drives, Broxton (which has no PCH) and the discrete cards
 * included. */
static uint32_t gmbus_base(struct i915_device *i915)
{
	(void)i915;
	return 0xC5100;
}
#define GMBUS0(i915) (gmbus_base(i915) + 0x00)
#define GMBUS1(i915) (gmbus_base(i915) + 0x04)
#define GMBUS2(i915) (gmbus_base(i915) + 0x08)
#define GMBUS3(i915) (gmbus_base(i915) + 0x0C)
#define GMBUS4(i915) (gmbus_base(i915) + 0x10)
#define GMBUS5(i915) (gmbus_base(i915) + 0x20)

#define GMBUS_WORD_TIMEOUT_US 50000 /* per data word / phase */
#define GMBUS_IDLE_TIMEOUT_US 10000

/* ---- pins --------------------------------------------------------------------- */

static int plat(struct i915_device *i915, int p)
{
	return i915->info->platform == p;
}

static int pch_is_mcc(struct i915_device *i915)
{
	return (i915->pch_devid & 0xff80) == 0x4b00;
}

/* Meteor Point's pins and bus numbering: Meteor Lake, Battlemage, and
 * Lunar Lake's kind of south display (every part from display 20). */
static int pch_mtp_kind(struct i915_device *i915)
{
	return i915->pch == I915_PCH_MTP || i915->pch == I915_PCH_LNL;
}

/* The GMBUS pins the part has. */
static int pin_valid(struct i915_device *i915, uint8_t pin)
{
	switch (i915->pch) {
	case I915_PCH_MTP:
	case I915_PCH_LNL:
		return (pin >= 1 && pin <= 5) || (pin >= 9 && pin <= 12);
	case I915_PCH_DG2:
		return (pin >= 1 && pin <= 4) || pin == 9;
	case I915_PCH_DG1:
		return pin >= 1 && pin <= 4;
	case I915_PCH_ICP:
	case I915_PCH_JSP:
	case I915_PCH_TGP:
	case I915_PCH_ADP:
		return (pin >= 1 && pin <= 3) || (pin >= 9 && pin <= 14);
	case I915_PCH_CNP:
	case I915_PCH_CMP:
		return pin >= 1 && pin <= 4;
	case I915_PCH_NONE:
		/* the low-power Gen9 parts: three pins */
		return pin >= 1 && pin <= 3;
	case I915_PCH_LPT:
	case I915_PCH_WPT:
		/* the panel's and the analogue connector's pins exist
		 * alongside the three digital ones on the H parts */
		return pin >= GMBUS_PIN_SSC && pin <= GMBUS_PIN_DPD;
	default:
		/* Sunrise/Kaby Point: the three digital pins */
		return pin >= GMBUS_PIN_DPC && pin <= GMBUS_PIN_DPD;
	}
}

/* From Cannon Point the VBT names a DDC bus, which is a pin only through
 * a per-platform table (the pin is the table's index of the bus).
 * Returns the pin, 0 when the bus does not exist here. */
static uint8_t map_vbt_ddc_pin(struct i915_device *i915, uint8_t vbt)
{
	static const uint8_t adlp[] = { 0, 1, 2, 0, 0, 0, 0, 0, 0, 3, 4, 5, 6 };
	static const uint8_t adls[] = { 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 3, 4, 5 };
	static const uint8_t rkl_tgp[] = { 0, 1, 2, 0, 0, 0, 0, 0, 0, 3, 4 };
	static const uint8_t gen9_tgp[] = { 0, 0, 1, 0, 0, 0, 0, 0, 0, 2, 3 };
	static const uint8_t icp[] = { 0, 1, 2, 3, 0, 0, 0, 0, 0, 4, 5, 6, 7, 8, 9 };
	static const uint8_t cnp[] = { 0, 1, 2, 4, 3 };
	const uint8_t *map;
	unsigned n;

	if (pch_mtp_kind(i915) || plat(i915, I915_PLATFORM_ALDERLAKE_P)) {
		map = adlp;
		n = sizeof(adlp);
	} else if (plat(i915, I915_PLATFORM_ALDERLAKE_S)) {
		map = adls;
		n = sizeof(adls);
	} else if (i915->pch == I915_PCH_DG1 || i915->pch == I915_PCH_DG2) {
		return vbt;
	} else if (plat(i915, I915_PLATFORM_ROCKETLAKE) && i915->pch == I915_PCH_TGP) {
		map = rkl_tgp;
		n = sizeof(rkl_tgp);
	} else if (i915->pch == I915_PCH_TGP && i915->info->display_ver == 9) {
		map = gen9_tgp;
		n = sizeof(gen9_tgp);
	} else if (i915->pch >= I915_PCH_ICP && i915->pch <= I915_PCH_ADP) {
		map = icp;
		n = sizeof(icp);
	} else if (i915->pch == I915_PCH_CNP || i915->pch == I915_PCH_CMP) {
		map = cnp;
		n = sizeof(cnp);
	} else {
		return vbt;
	}
	for (unsigned i = 1; i < n; i++)
		if (map[i] == vbt)
			return (uint8_t)i;
	return 0;
}

/* The PHY a port is wired to (index, A = 0). */
static int port_phy(struct i915_device *i915, int port)
{
	int ver = i915->info->display_ver;

	if (ver >= 13 && port >= PORT_H)
		return PORT_D + port - PORT_H;
	if (ver >= 13 && port >= PORT_TC1)
		return PORT_F + port - PORT_TC1;
	if (plat(i915, I915_PLATFORM_ALDERLAKE_S) && port >= PORT_TC1)
		return PORT_B + port - PORT_TC1;
	if ((plat(i915, I915_PLATFORM_DG1) || plat(i915, I915_PLATFORM_ROCKETLAKE)) &&
	    port >= PORT_TC1)
		return PORT_C + port - PORT_TC1;
	if ((plat(i915, I915_PLATFORM_JASPERLAKE) || plat(i915, I915_PLATFORM_ELKHARTLAKE)) &&
	    port == PORT_D)
		return PORT_A;
	return port;
}

/* Is that PHY a Type-C one? */
static int phy_is_tc(struct i915_device *i915, int phy)
{
	if (i915->info->flags & I915_INFO_IS_DGFX)
		return 0;
	if (i915->info->display_ver >= 13)
		return phy >= PORT_F && phy <= PORT_I;
	if (plat(i915, I915_PLATFORM_TIGERLAKE))
		return phy >= PORT_D && phy <= PORT_I;
	if (plat(i915, I915_PLATFORM_ICELAKE))
		return phy >= PORT_C && phy <= PORT_F;
	return 0;
}

static uint8_t default_pin(struct i915_device *i915, int port)
{
	int ver = i915->info->display_ver;
	int phy = port_phy(i915, port);

	if (plat(i915, I915_PLATFORM_ALDERLAKE_S))
		return phy == PORT_A ? 1 : (uint8_t)(GMBUS_PIN_9_TC1_ICP + phy - PORT_B);
	if (pch_mtp_kind(i915) && port >= PORT_TC1 && port < PORT_H)
		/* Meteor Point's Type-C pins are 9-12 (the table names them
		 * after the ports) */
		return (uint8_t)(GMBUS_PIN_9_TC1_ICP + port - PORT_TC1);
	if (plat(i915, I915_PLATFORM_DG2) && port == PORT_TC1)
		return GMBUS_PIN_9_TC1_ICP;
	if (i915->pch == I915_PCH_DG1 || i915->pch == I915_PCH_DG2 || pch_mtp_kind(i915))
		return (uint8_t)(phy + 1);
	if (plat(i915, I915_PLATFORM_ROCKETLAKE) ||
	    (ver == 9 && i915->pch == I915_PCH_TGP)) {
		if (i915->pch == I915_PCH_TGP && phy >= PORT_C)
			return (uint8_t)(GMBUS_PIN_9_TC1_ICP + phy - PORT_C);
		return (uint8_t)(1 + phy);
	}
	if ((plat(i915, I915_PLATFORM_JASPERLAKE) || plat(i915, I915_PLATFORM_ELKHARTLAKE)) &&
	    pch_is_mcc(i915)) {
		switch (phy) {
		case PORT_A: return 1;
		case PORT_B: return 2;
		case PORT_C: return GMBUS_PIN_9_TC1_ICP;
		default: return 1;
		}
	}
	if (i915->pch >= I915_PCH_ICP && i915->pch <= I915_PCH_ADP) {
		/* combo ports from pin 1, Type-C ports from 9 */
		if (phy_is_tc(i915, phy))
			return (uint8_t)(GMBUS_PIN_9_TC1_ICP +
					 (ver >= 12 ? port - PORT_TC1 : port - PORT_C));
		return (uint8_t)(1 + port);
	}
	if (i915->pch == I915_PCH_CNP || i915->pch == I915_PCH_CMP) {
		switch (port) {
		case PORT_B: return GMBUS_PIN_1_BXT;
		case PORT_C: return GMBUS_PIN_2_BXT;
		case PORT_D: return GMBUS_PIN_4_CNP;
		default: return GMBUS_PIN_1_BXT;
		}
	}
	if (i915->display.model == INTEL_DISPLAY_BXT || i915->pch == I915_PCH_NONE)
		return port == PORT_C ? GMBUS_PIN_2_BXT : GMBUS_PIN_1_BXT;
	switch (port) {
	case PORT_B: return GMBUS_PIN_DPB;
	case PORT_C: return GMBUS_PIN_DPC;
	case PORT_D: return GMBUS_PIN_DPD;
	default: return GMBUS_PIN_DPB;
	}
}

uint8_t intel_gmbus_pin_for_port(struct i915_device *i915, int port, uint8_t vbt_pin)
{
	uint8_t pin = vbt_pin ? map_vbt_ddc_pin(i915, vbt_pin) : 0;
	if (pin && pin_valid(i915, pin))
		return pin;
	uint8_t def = default_pin(i915, port);
	if (vbt_pin)
		kprintf("[drm] i915: port %s: VBT DDC bus %u is no pin here, using %u\n",
			intel_port_name(i915, port), vbt_pin, def);
	return def;
}

/* Display WA #0868 (Skylake to Gemini Lake): the GMBUS unit's clock
 * gating is held off while a transfer runs. */
static void gmbus_clock_gating(struct i915_device *i915, int enable)
{
	if (i915->display.model == INTEL_DISPLAY_BXT) {
		uint32_t v = i915_read32(i915, GEN9_CLKGATE_DIS_4);
		i915_write32(i915, GEN9_CLKGATE_DIS_4,
			     enable ? (v & ~BXT_GMBUS_GATING_DIS) : (v | BXT_GMBUS_GATING_DIS));
	} else if (i915->pch >= I915_PCH_SPT && i915->pch <= I915_PCH_CMP) {
		uint32_t v = i915_read32(i915, SOUTH_DSPCLK_GATE_D);
		i915_write32(i915, SOUTH_DSPCLK_GATE_D,
			     enable ? (v & ~PCH_GMBUSUNIT_CLOCK_GATE_DISABLE) :
				      (v | PCH_GMBUSUNIT_CLOCK_GATE_DISABLE));
	}
}

/* ---- transfers ---------------------------------------------------------------- */

static int gmbus_wait(struct i915_device *i915, uint32_t bit, unsigned timeout_us)
{
	for (unsigned t = 0; t < timeout_us; t += 10) {
		uint32_t st = i915_read32(i915, GMBUS2(i915));
		if (st & GMBUS_SATOER)
			return -ENXIO; /* the sink did not acknowledge */
		if (st & bit)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

static int gmbus_wait_idle(struct i915_device *i915)
{
	for (unsigned t = 0; t < GMBUS_IDLE_TIMEOUT_US; t += 10) {
		if (!(i915_read32(i915, GMBUS2(i915)) & GMBUS_ACTIVE))
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

static int gmbus_read(struct i915_device *i915, uint16_t addr, uint8_t *buf,
		      unsigned len, uint32_t gmbus1_index)
{
	i915_write32(i915, GMBUS1(i915),
		     gmbus1_index | GMBUS_CYCLE_WAIT | (len << GMBUS_BYTE_COUNT_SHIFT) |
			     ((uint32_t)addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_READ |
			     GMBUS_SW_RDY);
	while (len) {
		int rc = gmbus_wait(i915, GMBUS_HW_RDY, GMBUS_WORD_TIMEOUT_US);
		if (rc)
			return rc;
		uint32_t v = i915_read32(i915, GMBUS3(i915));
		for (int b = 0; b < 4 && len; b++, len--) {
			*buf++ = (uint8_t)(v & 0xff);
			v >>= 8;
		}
	}
	return 0;
}

static int gmbus_write(struct i915_device *i915, uint16_t addr, const uint8_t *buf,
		       unsigned len)
{
	uint32_t v = 0;
	unsigned n = len;

	for (int b = 0; b < 4 && n; b++, n--)
		v |= (uint32_t)(*buf++) << (8 * b);
	i915_write32(i915, GMBUS3(i915), v);
	i915_write32(i915, GMBUS1(i915),
		     GMBUS_CYCLE_WAIT | (len << GMBUS_BYTE_COUNT_SHIFT) |
			     ((uint32_t)addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_WRITE |
			     GMBUS_SW_RDY);
	while (n) {
		int rc = gmbus_wait(i915, GMBUS_HW_RDY, GMBUS_WORD_TIMEOUT_US);
		if (rc)
			return rc;
		v = 0;
		for (int b = 0; b < 4 && n; b++, n--)
			v |= (uint32_t)(*buf++) << (8 * b);
		i915_write32(i915, GMBUS3(i915), v);
	}
	return 0;
}

/* A one- or two-byte write immediately followed by a read from the same
 * address is what a DDC offset read looks like; the controller does it
 * as one cycle with the index in GMBUS1 (one byte) or GMBUS5 (two). */
static int gmbus_index_read(struct i915_device *i915, const struct i2c_msg *w,
			    struct i2c_msg *r)
{
	uint32_t gmbus1_index = 0, gmbus5 = 0;

	if (w->len == 2)
		gmbus5 = GMBUS_2BYTE_INDEX_EN | ((uint32_t)w->buf[1] << 8) | w->buf[0];
	else
		gmbus1_index = GMBUS_CYCLE_INDEX | ((uint32_t)w->buf[0] << GMBUS_SLAVE_INDEX_SHIFT);
	i915_write32(i915, GMBUS5(i915), gmbus5);
	int rc = gmbus_read(i915, r->addr, r->buf, r->len, gmbus1_index);
	i915_write32(i915, GMBUS5(i915), 0);
	return rc;
}

static int gmbus_xfer_locked(struct i915_device *i915, uint8_t pin, struct i2c_msg *msgs,
			     int n)
{
	int i, rc = 0;

	i915_write32(i915, GMBUS0(i915), GMBUS_RATE_100KHZ | pin);
	for (i = 0; i < n; i++) {
		struct i2c_msg *m = &msgs[i];
		if (m->len > GMBUS_BYTE_COUNT_MAX) {
			rc = -EINVAL;
			goto out_stop;
		}
		if (!(m->flags & I2C_M_RD) && (m->len == 1 || m->len == 2) && i + 1 < n &&
		    (msgs[i + 1].flags & I2C_M_RD) && msgs[i + 1].addr == m->addr) {
			rc = gmbus_index_read(i915, m, &msgs[i + 1]);
			i++;
		} else if (m->flags & I2C_M_RD) {
			rc = gmbus_read(i915, m->addr, m->buf, m->len, 0);
		} else {
			rc = gmbus_write(i915, m->addr, m->buf, m->len);
		}
		if (rc)
			break;
		/* the bus parks after the message, ready for the next */
		rc = gmbus_wait(i915, GMBUS_HW_WAIT_PHASE, GMBUS_WORD_TIMEOUT_US);
		if (rc)
			break;
	}
	if (rc == -ENXIO) {
		/* No acknowledge: let the controller settle, clear the
		 * error, and the bus is free again.  Nobody home on a DDC
		 * line is the common case, not an error worth a log. */
		gmbus_wait_idle(i915);
		i915_write32(i915, GMBUS1(i915), GMBUS_SW_CLR_INT);
		i915_write32(i915, GMBUS1(i915), 0);
		i915_write32(i915, GMBUS0(i915), 0);
		return -ENXIO;
	}
	if (rc == -ETIMEDOUT) {
		kprintf("[drm] i915: GMBUS pin %u: transfer timed out (GMBUS2 %08x)\n", pin,
			i915_read32(i915, GMBUS2(i915)));
		i915_write32(i915, GMBUS1(i915), GMBUS_SW_CLR_INT);
		i915_write32(i915, GMBUS1(i915), 0);
		i915_write32(i915, GMBUS0(i915), 0);
		return -ETIMEDOUT;
	}
out_stop:
	i915_write32(i915, GMBUS1(i915), GMBUS_CYCLE_STOP | GMBUS_SW_RDY);
	if (gmbus_wait_idle(i915) != 0) {
		kprintf("[drm] i915: GMBUS pin %u: stop did not complete\n", pin);
		i915_write32(i915, GMBUS1(i915), GMBUS_SW_CLR_INT);
		i915_write32(i915, GMBUS1(i915), 0);
		i915_write32(i915, GMBUS0(i915), 0);
		return -ETIMEDOUT;
	}
	i915_write32(i915, GMBUS0(i915), 0);
	return rc ? rc : n;
}

struct gmbus_adapter_priv {
	struct i915_device *i915;
	uint8_t pin;
};

static int gmbus_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int n)
{
	struct gmbus_adapter_priv *pv = a->priv;
	struct i915_device *i915 = pv->i915;
	struct intel_display *d = &i915->display;

	if (!pv->pin)
		return -ENODEV;
	/* One transfer at a time; the callers are all process context
	 * (probes, the hotplug worker), so a waiting one just yields. */
	while (__sync_lock_test_and_set(&d->gmbus_busy, 1))
		sched_yield_in_kernel();
	intel_power_get(i915, INTEL_PW_GMBUS);
	gmbus_clock_gating(i915, 0);
	int rc = gmbus_xfer_locked(i915, pv->pin, msgs, n);
	gmbus_clock_gating(i915, 1);
	intel_power_put(i915, INTEL_PW_GMBUS);
	__sync_lock_release(&d->gmbus_busy);
	return rc;
}

/* One adapter per output; the private block lives in a small static
 * pool since outputs are created once at probe. */
static struct gmbus_adapter_priv g_gmbus_priv[INTEL_MAX_OUTPUTS];
static int g_gmbus_priv_used;

void intel_gmbus_adapter_init(struct i915_device *i915, struct i2c_adapter *a,
			      uint8_t pin, const char *name)
{
	mm_memset(a, 0, sizeof(*a));
	if (g_gmbus_priv_used >= INTEL_MAX_OUTPUTS)
		return;
	struct gmbus_adapter_priv *pv = &g_gmbus_priv[g_gmbus_priv_used++];
	pv->i915 = i915;
	pv->pin = pin;
	a->priv = pv;
	a->xfer = gmbus_xfer;
	unsigned i = 0;
	while (name && name[i] && i < sizeof(a->name) - 1) {
		a->name[i] = name[i];
		i++;
	}
	a->name[i] = 0;
}

int intel_gmbus_init(struct i915_device *i915)
{
	/* whatever the firmware left: no pin selected, no interrupts */
	i915_write32(i915, GMBUS0(i915), 0);
	i915_write32(i915, GMBUS4(i915), 0);
	i915->display.gmbus_busy = 0;
	g_gmbus_priv_used = 0;
	return 0;
}
