// LikeOS-64 -- GMBUS, the I2C controller of the display engine.
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
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* register base: south display (PCH) or the north one on parts without */
static uint32_t gmbus_base(struct i915_device *i915)
{
	return (i915->info->flags & I915_INFO_HAS_PCH) ? 0xC5100 : 0x5100;
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

static int pin_valid(struct i915_device *i915, uint8_t pin)
{
	switch (i915->pch) {
	case I915_PCH_ICP:
	case I915_PCH_JSP:
	case I915_PCH_TGP:
	case I915_PCH_ADP:
		return pin >= 1 && pin <= 14 && pin != 4 && pin != 5 && pin != 6 && pin != 7 && pin != 8;
	case I915_PCH_CNP:
	case I915_PCH_CMP:
		return pin >= 1 && pin <= 4;
	case I915_PCH_NONE:
		/* the low-power Gen9 parts: three pins */
		return pin >= 1 && pin <= 3;
	default:
		/* Lynx Point through Kaby Point: the panel's and the analogue
		 * connector's pins exist alongside the three digital ones. */
		return pin >= GMBUS_PIN_SSC && pin <= GMBUS_PIN_DPD;
	}
}

static uint8_t default_pin(struct i915_device *i915, int port)
{
	switch (i915->pch) {
	case I915_PCH_ICP:
	case I915_PCH_JSP:
	case I915_PCH_TGP:
	case I915_PCH_ADP:
		/* combo ports A/B on 1/2, Type-C ports from 9 */
		return port <= PORT_B ? (uint8_t)(1 + port) : (uint8_t)(GMBUS_PIN_9_TC1_ICP + (port - PORT_C));
	case I915_PCH_CNP:
	case I915_PCH_CMP:
		switch (port) {
		case PORT_B: return GMBUS_PIN_1_BXT;
		case PORT_C: return GMBUS_PIN_2_BXT;
		case PORT_D: return GMBUS_PIN_4_CNP;
		default: return GMBUS_PIN_3_BXT;
		}
	case I915_PCH_NONE:
		return port == PORT_B ? GMBUS_PIN_1_BXT : GMBUS_PIN_2_BXT;
	default:
		switch (port) {
		case PORT_B: return GMBUS_PIN_DPB;
		case PORT_C: return GMBUS_PIN_DPC;
		case PORT_D: return GMBUS_PIN_DPD;
		default: return GMBUS_PIN_DISABLED;
		}
	}
}

uint8_t intel_gmbus_pin_for_port(struct i915_device *i915, int port, uint8_t vbt_pin)
{
	if (vbt_pin && pin_valid(i915, vbt_pin))
		return vbt_pin;
	uint8_t pin = default_pin(i915, port);
	if (vbt_pin)
		kprintf("[drm] i915: port %c: VBT DDC pin %u unknown here, using %u\n",
			'A' + port, vbt_pin, pin);
	return pin;
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
	int rc = gmbus_xfer_locked(i915, pv->pin, msgs, n);
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
