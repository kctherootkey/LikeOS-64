// LikeOS I2C - DesignWare controller: register access, timing, enable/
// disable, abort decoding, probe, power management, diagnostics
//
// Register access.  The controller is normally reached with 32-bit MMIO,
// but some integrations byte-swap the bus or only implement 16-bit
// accesses.  The mode is detected once from IC_COMP_TYPE, which reads
// 0x44570140 ("DW" + 0x0140): the swapped value means a swapped bus, the
// low half alone a 16-bit bus (every register is then two 16-bit
// accesses, low half first).
//
// Timing.  The SCL high/low counts of each speed come from, in order of
// preference: counts the glue gives (legacy PCI tables), the ACPI methods
// SSCN/FMCN/FPCN/HSCN (HCNT, LCNT, SDA hold), and the formulas
//   HCNT = round(ic_clk * (tHIGH + tf) / 10^6) - 3
//   LCNT = round(ic_clk * (tLOW + tf) / 10^6) - 1
// over the input clock (kHz) and the spec's minimum high/low times plus
// the fall times (300 ns unless firmware says otherwise).  The bus speed
// is the slowest ACPI connection to the bus (or _DSD "clock-frequency"),
// 400 kHz when nothing says.  SDA hold (controllers >= v1.11) comes from
// ACPI or the platform's hold time in ns, with a non-zero RX hold always
// set (a target that drives SDA too soon after SCL falls otherwise makes
// the controller see a lost arbitration).
//
// Power.  After autosuspend_ms without a transfer the controller is
// disabled and, on Intel LPSS, its private registers are saved and the
// function put in reset; the next transfer restores and re-initialises it
// (with the bus lock held, so no transfer can race with it).  System
// suspend marks the adapter suspended first, so a late transfer fails
// with -ESHUTDOWN instead of touching a powered-down controller.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2006 Texas Instruments, 2007 MontaVista Software
// Inc., 2009 Provigent Ltd., 2011-2016 Intel Corporation

#include <kernel/dev/i2c/busses/i2c-designware-core.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/dmi.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define DW_IC_ABORT_TIMEOUT_US 10
#define DW_IC_BUSY_POLL_TIMEOUT_US 1000

static const char *const abort_sources[DW_IC_ABRT_SOURCES] = {
	[ABRT_7B_ADDR_NOACK] = "slave address not acknowledged (7bit mode)",
	[ABRT_10ADDR1_NOACK] =
		"first address byte not acknowledged (10bit mode)",
	[ABRT_10ADDR2_NOACK] =
		"second address byte not acknowledged (10bit mode)",
	[ABRT_TXDATA_NOACK] = "data not acknowledged",
	[ABRT_GCALL_NOACK] = "no acknowledgement for a general call",
	[ABRT_GCALL_READ] = "read after general call",
	[ABRT_HS_ACKDET] = "high speed master code acknowledged",
	[ABRT_SBYTE_ACKDET] = "start byte acknowledged",
	[ABRT_HS_NORSTRT] = "high speed mode when restart is disabled",
	[ABRT_SBYTE_NORSTRT] =
		"trying to send start byte when restart is disabled",
	[ABRT_10B_RD_NORSTRT] =
		"trying to read when restart is disabled (10bit mode)",
	[ABRT_MASTER_DIS] = "trying to use disabled adapter",
	[ARB_LOST] = "lost arbitration",
	[ABRT_SLAVE_FLUSH_TXFIFO] =
		"read command so flush old data in the TX FIFO",
	[ABRT_SLAVE_ARBLOST] =
		"slave lost the bus while transmitting data to a remote master",
	[ABRT_SLAVE_RD_INTX] = "incorrect slave-transmitter mode configuration",
	[ABRT_USER_ABRT] = "transfer aborted by the driver",
	[ABRT_SDA_STUCK_AT_LOW] = "SDA stuck at low",
};

const char *i2c_dw_abort_name(int bit)
{
	if (bit < 0 || bit >= DW_IC_ABRT_SOURCES || !abort_sources[bit])
		return "unknown";
	return abort_sources[bit];
}

/* ==========================================================================
 * Register access
 * ========================================================================== */

static uint32_t swab32(uint32_t v)
{
	return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) |
	       (v << 24);
}

uint32_t dw_readl(struct dw_i2c_dev *dev, unsigned int reg)
{
	volatile uint8_t *a = dev->base + reg;

	switch (dev->access) {
	case DW_ACCESS_SWAP:
		return swab32(dw_mmio_readl(a));
	case DW_ACCESS_16:
		return (uint32_t)dw_mmio_readw(a) |
		       ((uint32_t)dw_mmio_readw(a + 2) << 16);
	case DW_ACCESS_32:
	default:
		return dw_mmio_readl(a);
	}
}

void dw_writel(struct dw_i2c_dev *dev, uint32_t val, unsigned int reg)
{
	volatile uint8_t *a = dev->base + reg;

	switch (dev->access) {
	case DW_ACCESS_SWAP:
		dw_mmio_writel(swab32(val), a);
		break;
	case DW_ACCESS_16:
		dw_mmio_writew((uint16_t)val, a);
		dw_mmio_writew((uint16_t)(val >> 16), a + 2);
		break;
	case DW_ACCESS_32:
	default:
		dw_mmio_writel(val, a);
		break;
	}
}

int i2c_dw_init_regmap(struct dw_i2c_dev *dev)
{
	uint32_t reg;
	int ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;
	reg = dw_mmio_readl(dev->base + DW_IC_COMP_TYPE);
	i2c_dw_release_lock(dev);

	if (reg == swab32(DW_IC_COMP_TYPE_VALUE)) {
		dev->access = DW_ACCESS_SWAP;
	} else if (reg == (DW_IC_COMP_TYPE_VALUE & 0xffffu)) {
		dev->access = DW_ACCESS_16;
	} else if (reg == DW_IC_COMP_TYPE_VALUE) {
		dev->access = DW_ACCESS_32;
	} else {
		kprintf("%s: Unknown Synopsys component type: 0x%08x\n",
			dev->name, reg);
		return -ENODEV;
	}
	return 0;
}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

void i2c_dw_delay(struct dw_i2c_dev *dev, unsigned long min_us,
		  unsigned long max_us)
{
	if (dev->atomic_now || !delay_can_sleep())
		udelay(min_us);
	else
		usleep_range(min_us, max_us);
}

/* Read `reg' until (value & mask) == want, sleeping `sleep_us' between
 * reads, for up to `timeout_us'.  0 or -ETIMEDOUT; *last gets the last
 * value read. */
static int dw_poll(struct dw_i2c_dev *dev, unsigned int reg, uint32_t mask,
		   uint32_t want, unsigned long sleep_us,
		   unsigned long timeout_us, uint32_t *last)
{
	uint64_t deadline = hrtimer_now_ns() + (uint64_t)timeout_us * 1000;
	uint32_t v;

	for (;;) {
		v = dw_readl(dev, reg);
		if ((v & mask) == want)
			break;
		if (hrtimer_now_ns() > deadline) {
			/* one last look after the deadline */
			v = dw_readl(dev, reg);
			if ((v & mask) == want)
				break;
			if (last)
				*last = v;
			return -ETIMEDOUT;
		}
		if (sleep_us)
			i2c_dw_delay(dev, sleep_us / 4 + 1, sleep_us);
	}
	if (last)
		*last = v;
	return 0;
}

int i2c_dw_acquire_lock(struct dw_i2c_dev *dev)
{
	int ret;

	if (!dev->acquire_lock)
		return 0;
	ret = dev->acquire_lock(dev);
	if (!ret)
		return 0;
	kprintf("%s: couldn't acquire bus ownership\n", dev->name);
	return ret;
}

void i2c_dw_release_lock(struct dw_i2c_dev *dev)
{
	if (dev->release_lock)
		dev->release_lock(dev);
}

uint32_t i2c_dw_clk_rate(struct dw_i2c_dev *dev)
{
	/* Without a clock only firmware or table counts are used. */
	if (!dev->get_clk_rate_khz)
		return 0;
	return dev->get_clk_rate_khz(dev);
}

static uint32_t div_round_closest_u64(uint64_t n, uint64_t d)
{
	return (uint32_t)((n + d / 2) / d);
}

static uint32_t i2c_dw_read_scl_reg(struct dw_i2c_dev *dev, unsigned int reg)
{
	uint32_t val;

	if (i2c_dw_acquire_lock(dev))
		return 0;
	val = dw_readl(dev, reg);
	i2c_dw_release_lock(dev);
	return val;
}

uint32_t i2c_dw_scl_hcnt(struct dw_i2c_dev *dev, unsigned int reg,
			 uint32_t ic_clk, uint32_t tSYMBOL, uint32_t tf,
			 int offset)
{
	if (!ic_clk)
		return i2c_dw_read_scl_reg(dev, reg);
	/* IC_[FS]S_SCL_HCNT + 3 >= IC_CLK * (tHD;STA + tf): the tHD;STA
	 * period turned out to be proportional to (HCNT + 3); with tf
	 * included both tHIGH and tHD;STA are met. */
	return div_round_closest_u64((uint64_t)ic_clk * (tSYMBOL + tf),
				     1000000ull) -
	       3 + (uint32_t)offset;
}

uint32_t i2c_dw_scl_lcnt(struct dw_i2c_dev *dev, unsigned int reg,
			 uint32_t ic_clk, uint32_t tLOW, uint32_t tf,
			 int offset)
{
	if (!ic_clk)
		return i2c_dw_read_scl_reg(dev, reg);
	/* IC_[FS]S_SCL_LCNT + 1 >= IC_CLK * (tLOW + tf): the count starts
	 * as soon as the controller pulls SCL, so the fall time is part of
	 * the low period. */
	return div_round_closest_u64((uint64_t)ic_clk * (tLOW + tf),
				     1000000ull) -
	       1 + (uint32_t)offset;
}

/* ==========================================================================
 * Firmware configuration
 * ========================================================================== */

static const uint32_t supported_speeds[] = {
	I2C_MAX_HIGH_SPEED_MODE_FREQ,
	I2C_MAX_FAST_MODE_PLUS_FREQ,
	I2C_MAX_FAST_MODE_FREQ,
	I2C_MAX_STANDARD_MODE_FREQ,
};

static int i2c_dw_validate_speed(struct dw_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;

	/* Only 100 kHz, 400 kHz, 1 MHz and 3.4 MHz. */
	for (unsigned int i = 0;
	     i < sizeof(supported_speeds) / sizeof(supported_speeds[0]); i++)
		if (t->bus_freq_hz == supported_speeds[i])
			return 0;
	kprintf("%s: %u Hz is unsupported, only 100kHz, 400kHz, 1MHz and 3.4MHz are supported\n",
		dev->name, t->bus_freq_hz);
	return -EINVAL;
}

/* The counts ACPI gives should be the most accurate for a platform, but
 * some systems get them wrong; there the counts computed from the input
 * clock work better. */
static const struct dmi_system_id i2c_dw_no_acpi_params[] = {
	{
		.ident = "Dell Inspiron 7348",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Inspiron 7348"),
		},
	},
	{ 0 }
};

/* HCNT, LCNT and SDA hold of one speed from an ACPI method returning a
 * package of exactly three integers. */
static void i2c_dw_acpi_params(struct dw_i2c_dev *dev, const char *method,
			       uint16_t *hcnt, uint16_t *lcnt,
			       uint32_t *sda_hold)
{
	struct acpi_value *v = NULL;

	if (dmi_first_match(i2c_dw_no_acpi_params))
		return;
	if (!dev->acpi_dev || !acpi_has_method(dev->acpi_dev->handle, method))
		return;
	if (acpi_evaluate(dev->acpi_dev->handle, method, NULL, 0, &v) || !v)
		return;
	if (v->type == ACPI_VALUE_PACKAGE && v->length == 3 &&
	    v->elements[0].type == ACPI_VALUE_INTEGER &&
	    v->elements[1].type == ACPI_VALUE_INTEGER &&
	    v->elements[2].type == ACPI_VALUE_INTEGER) {
		*hcnt = (uint16_t)v->elements[0].integer;
		*lcnt = (uint16_t)v->elements[1].integer;
		*sda_hold = (uint32_t)v->elements[2].integer;
	}
	acpi_value_free(v);
}

static void i2c_dw_acpi_configure(struct dw_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;
	uint32_t ss_ht = 0, fp_ht = 0, hs_ht = 0, fs_ht = 0;

	/* SDA hold time and counts of each speed mode, as firmware has
	 * them. */
	i2c_dw_acpi_params(dev, "SSCN", &dev->ss_hcnt, &dev->ss_lcnt, &ss_ht);
	i2c_dw_acpi_params(dev, "FMCN", &dev->fs_hcnt, &dev->fs_lcnt, &fs_ht);
	i2c_dw_acpi_params(dev, "FPCN", &dev->fp_hcnt, &dev->fp_lcnt, &fp_ht);
	i2c_dw_acpi_params(dev, "HSCN", &dev->hs_hcnt, &dev->hs_lcnt, &hs_ht);

	switch (t->bus_freq_hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
		dev->sda_hold_time = ss_ht;
		break;
	case I2C_MAX_FAST_MODE_PLUS_FREQ:
		dev->sda_hold_time = fp_ht;
		break;
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		dev->sda_hold_time = hs_ht;
		break;
	case I2C_MAX_FAST_MODE_FREQ:
	default:
		dev->sda_hold_time = fs_ht;
		break;
	}
}

static uint32_t i2c_dw_acpi_round_bus_speed(struct dw_i2c_dev *dev)
{
	uint32_t acpi_speed;

	if (!dev->acpi_dev)
		return 0;
	acpi_speed = i2c_acpi_find_bus_speed(dev->acpi_dev->handle);
	/* Some tables use a speed between the standard ones: round down. */
	for (unsigned int i = 0;
	     i < sizeof(supported_speeds) / sizeof(supported_speeds[0]); i++)
		if (acpi_speed >= supported_speeds[i])
			return supported_speeds[i];
	return 0;
}

static void i2c_dw_adjust_bus_speed(struct dw_i2c_dev *dev)
{
	uint32_t acpi_speed = i2c_dw_acpi_round_bus_speed(dev);
	struct i2c_timings *t = &dev->timings;

	/* The "clock-frequency" property, the ACPI connections, or fast
	 * mode when neither says. */
	if (acpi_speed && t->bus_freq_hz)
		t->bus_freq_hz = t->bus_freq_hz < acpi_speed ? t->bus_freq_hz :
							       acpi_speed;
	else if (acpi_speed || t->bus_freq_hz)
		t->bus_freq_hz = t->bus_freq_hz > acpi_speed ? t->bus_freq_hz :
							       acpi_speed;
	else
		t->bus_freq_hz = I2C_MAX_FAST_MODE_FREQ;
}

/* A device property overriding the glue's default. */
static void dw_prop_u32(struct dw_i2c_dev *dev, const char *name,
			uint32_t *val)
{
	uint32_t v;

	if (dev->acpi_dev &&
	    !acpi_dev_get_property_u32(dev->acpi_dev->handle, name, &v))
		*val = v;
}

int i2c_dw_fw_parse_and_configure(struct dw_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;
	uint32_t optimized = (uint32_t)dev->clk_freq_optimized;

	/* Firmware properties take precedence over the platform's
	 * defaults. */
	dw_prop_u32(dev, "clock-frequency", &t->bus_freq_hz);
	dw_prop_u32(dev, "i2c-scl-rising-time-ns", &t->scl_rise_ns);
	dw_prop_u32(dev, "i2c-scl-falling-time-ns", &t->scl_fall_ns);
	dw_prop_u32(dev, "i2c-scl-internal-delay-ns", &t->scl_int_delay_ns);
	dw_prop_u32(dev, "i2c-sda-falling-time-ns", &t->sda_fall_ns);
	dw_prop_u32(dev, "i2c-sda-hold-time-ns", &t->sda_hold_ns);
	if (!dev->bus_capacitance_pF)
		dev->bus_capacitance_pF = DW_IC_DEFAULT_BUS_CAPACITANCE_PF;
	dw_prop_u32(dev, "snps,bus-capacitance-pf", &dev->bus_capacitance_pF);
	if (dev->acpi_dev &&
	    acpi_dev_has_property(dev->acpi_dev->handle,
				  "snps,clk-freq-optimized"))
		optimized = 1;
	dev->clk_freq_optimized = optimized != 0;
	dev->emptyfifo_hold_master = 1;

	i2c_dw_adjust_bus_speed(dev);
	if (dev->acpi_dev)
		i2c_dw_acpi_configure(dev);
	return i2c_dw_validate_speed(dev);
}

static int i2c_dw_set_sda_hold(struct dw_i2c_dev *dev)
{
	uint32_t reg;
	int ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;
	/* SDA hold is programmable from v1.11 on. */
	reg = dw_readl(dev, DW_IC_COMP_VERSION);
	dev->comp_version = reg;
	if (reg >= DW_IC_SDA_HOLD_MIN_VERS) {
		if (!dev->sda_hold_time) {
			/* Keep the setting firmware made. */
			dev->sda_hold_time = dw_readl(dev, DW_IC_SDA_HOLD);
		}
		/* A non-zero RX hold avoids the lost arbitrations a target
		 * driving SDA too soon after SCL falls causes. */
		if (!(dev->sda_hold_time & DW_IC_SDA_HOLD_RX_MASK))
			dev->sda_hold_time |= 1u << DW_IC_SDA_HOLD_RX_SHIFT;
	} else if (dev->sda_hold_time) {
		kprintf("%s: Hardware too old to adjust SDA hold time.\n",
			dev->name);
		dev->sda_hold_time = 0;
	}
	i2c_dw_release_lock(dev);
	return 0;
}

static int i2c_dw_set_fifo_size(struct dw_i2c_dev *dev)
{
	uint32_t tx_fifo_depth, rx_fifo_depth, param;
	int ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;
	param = dw_readl(dev, DW_IC_COMP_PARAM_1);
	i2c_dw_release_lock(dev);
	dev->comp_param1 = param;

	/* Depth - 1 in the parameter register: 2 .. 256 by the hardware
	 * specification. */
	tx_fifo_depth = ((param >> DW_IC_FIFO_TX_SHIFT) & 0xffu) + 1;
	rx_fifo_depth = ((param >> DW_IC_FIFO_RX_SHIFT) & 0xffu) + 1;
	if (param == 0 || param == 0xffffffffu) {
		/* Not implemented (or not readable): the glue's depths, or
		 * the smallest legal ones. */
		if (!dev->tx_fifo_depth) {
			dev->tx_fifo_depth = DW_IC_FIFO_MIN_DEPTH;
			dev->rx_fifo_depth = DW_IC_FIFO_MIN_DEPTH;
		}
		kprintf("%s: COMP_PARAM_1 not implemented, FIFO depth %u/%u assumed\n",
			dev->name, dev->tx_fifo_depth, dev->rx_fifo_depth);
	} else if (!dev->tx_fifo_depth) {
		dev->tx_fifo_depth = tx_fifo_depth;
		dev->rx_fifo_depth = rx_fifo_depth;
	} else if (tx_fifo_depth >= DW_IC_FIFO_MIN_DEPTH) {
		dev->tx_fifo_depth = dev->tx_fifo_depth < tx_fifo_depth ?
					     dev->tx_fifo_depth :
					     tx_fifo_depth;
		dev->rx_fifo_depth = dev->rx_fifo_depth < rx_fifo_depth ?
					     dev->rx_fifo_depth :
					     rx_fifo_depth;
	}
	if (!dev->rx_fifo_depth)
		dev->rx_fifo_depth = DW_IC_FIFO_MIN_DEPTH;
	BUG_ON(!dev->tx_fifo_depth || dev->tx_fifo_depth > 256 ||
	       dev->rx_fifo_depth > 256);
	return 0;
}

/* ==========================================================================
 * Enable / disable / init
 * ========================================================================== */

void __i2c_dw_disable(struct dw_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;
	uint32_t raw_intr_stats, ic_stats, enable, status;
	int timeout = 100;
	int abort_needed;

	raw_intr_stats = dw_readl(dev, DW_IC_RAW_INTR_STAT);
	ic_stats = dw_readl(dev, DW_IC_STATUS);
	enable = dw_readl(dev, DW_IC_ENABLE);

	/* A master holding the bus on an empty FIFO only lets go after an
	 * abort. */
	abort_needed = (raw_intr_stats & DW_IC_INTR_MST_ON_HOLD) ||
		       (ic_stats & DW_IC_STATUS_MASTER_HOLD_TX_FIFO_EMPTY);
	if (abort_needed) {
		if (!(enable & DW_IC_ENABLE_ENABLE)) {
			dw_writel(dev, DW_IC_ENABLE_ENABLE, DW_IC_ENABLE);
			/* Ten periods of the bus clock for ENABLE to take
			 * effect, as the databook asks. */
			i2c_dw_delay(dev,
				     div_round_closest_u64(10ull * 1000000ull,
							   t->bus_freq_hz ?
								   t->bus_freq_hz :
								   I2C_MAX_STANDARD_MODE_FREQ),
				     100);
			/* ENABLE set before ABORT */
			enable |= DW_IC_ENABLE_ENABLE;
		}
		dw_writel(dev, enable | DW_IC_ENABLE_ABORT, DW_IC_ENABLE);
		if (dw_poll(dev, DW_IC_ENABLE, DW_IC_ENABLE_ABORT, 0,
			    DW_IC_ABORT_TIMEOUT_US,
			    10 * DW_IC_ABORT_TIMEOUT_US, NULL))
			kprintf("%s: timeout while trying to abort current transfer\n",
				dev->name);
	}

	do {
		dw_writel(dev, 0, DW_IC_ENABLE);
		dev->status &= ~DW_STATUS_ACTIVE;
		/* An unimplemented status register reads 0 and ends the
		 * loop at once. */
		status = dw_readl(dev, DW_IC_ENABLE_STATUS);
		if (!(status & 1))
			return;
		/* Ten periods of the fastest supported signalling (25 us
		 * at 400 kHz). */
		i2c_dw_delay(dev, 25, 250);
	} while (timeout--);

	i2c_stat_add(&dev->st.disable_timeouts, 1);
	kprintf("%s: timeout in disabling adapter\n", dev->name);
}

void i2c_dw_disable(struct dw_i2c_dev *dev)
{
	uint64_t fl;

	if (i2c_dw_acquire_lock(dev))
		return;
	__i2c_dw_disable(dev);
	/* all interrupts off and cleared */
	spin_lock_irqsave(&dev->lock, &fl);
	dev->sw_mask = 0;
	dw_writel(dev, 0, DW_IC_INTR_MASK);
	spin_unlock_irqrestore(&dev->lock, fl);
	(void)dw_readl(dev, DW_IC_CLR_INTR);
	i2c_dw_release_lock(dev);
}

int i2c_dw_init(struct dw_i2c_dev *dev)
{
	int ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;

	__i2c_dw_disable(dev);

	/* SMBus interrupts off: firmware that leaves the SMBus features on
	 * would otherwise raise interrupts nobody services. */
	dw_writel(dev, 0, DW_IC_SMBUS_INTR_MASK);

	dw_writel(dev, dev->ss_hcnt, DW_IC_SS_SCL_HCNT);
	dw_writel(dev, dev->ss_lcnt, DW_IC_SS_SCL_LCNT);
	/* fast mode and fast mode plus share the registers */
	dw_writel(dev, dev->fs_hcnt, DW_IC_FS_SCL_HCNT);
	dw_writel(dev, dev->fs_lcnt, DW_IC_FS_SCL_LCNT);
	dw_writel(dev, dev->hs_hcnt, DW_IC_HS_SCL_HCNT);
	dw_writel(dev, dev->hs_lcnt, DW_IC_HS_SCL_LCNT);

	if (dev->sda_hold_time)
		dw_writel(dev, dev->sda_hold_time, DW_IC_SDA_HOLD);

	/* FIFO thresholds, then the master configuration. */
	dw_writel(dev, dev->tx_fifo_depth / 2, DW_IC_TX_TL);
	dw_writel(dev, 0, DW_IC_RX_TL);
	dw_writel(dev, dev->master_cfg, DW_IC_CON);

	dev->fs_spklen = dw_readl(dev, DW_IC_FS_SPKLEN) & 0xff;
	dev->hs_spklen = dw_readl(dev, DW_IC_HS_SPKLEN) & 0xff;
	i2c_dw_release_lock(dev);
	return 0;
}

/* Waiting for the bus not to be busy. */
int i2c_dw_wait_bus_not_busy(struct dw_i2c_dev *dev)
{
	uint32_t status;
	int ret;

	ret = dw_poll(dev, DW_IC_STATUS, DW_IC_STATUS_ACTIVITY, 0,
		      DW_IC_BUSY_POLL_TIMEOUT_US,
		      20 * DW_IC_BUSY_POLL_TIMEOUT_US, &status);
	if (ret) {
		i2c_stat_add(&dev->st.bus_busy, 1);
		kprintf("%s: timeout waiting for bus ready\n", dev->name);
		i2c_recover_bus(&dev->adapter);
		status = dw_readl(dev, DW_IC_STATUS);
		if (!(status & DW_IC_STATUS_ACTIVITY))
			ret = 0;
	}
	return ret;
}

/* The errno of an abort.  Any missing acknowledge -- of the address
 * (nobody, or nobody awake, is there) or of a data byte -- is -EREMOTEIO,
 * and it is normal enough (probing, a device that wakes on its first
 * address byte) that it is not logged.  Lost arbitration is retried by
 * the core (-EAGAIN). */
int i2c_dw_handle_tx_abort(struct dw_i2c_dev *dev)
{
	unsigned long abort_source = dev->abort_source;

	i2c_stat_add(&dev->st.aborts, 1);
	for (int i = 0; i < DW_IC_ABRT_SOURCES; i++)
		if (abort_source & (1ul << i))
			i2c_stat_add(&dev->st.abort_src[i], 1);

	if (abort_source & DW_IC_TX_ABRT_NOACK) {
		/* Normal when probing for devices: no log line. */
		return -EREMOTEIO;
	}

	for (int i = 0; i < DW_IC_ABRT_SOURCES; i++)
		if (abort_source & (1ul << i))
			kprintf("%s: i2c_dw_handle_tx_abort: %s\n", dev->name,
				abort_sources[i] ? abort_sources[i] :
						   "unknown cause");

	if (abort_source & DW_IC_TX_ARB_LOST)
		return -EAGAIN;
	if (abort_source & DW_IC_TX_ABRT_GCALL_READ)
		return -EINVAL; /* wrong msgs[] data */
	return -EIO;
}

uint32_t i2c_dw_func(struct i2c_adapter *adap)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);

	return dev->functionality;
}

/* ==========================================================================
 * Runtime and system power
 * ========================================================================== */

static void i2c_dw_runtime_suspend(struct dw_i2c_dev *dev)
{
	if (dev->shared_with_punit)
		return; /* the PUNIT uses the bus as well */
	i2c_dw_disable(dev);
	/* From here the handler leaves the registers alone: the LPSS
	 * function is about to go into reset. */
	__atomic_store_n(&dev->rpm_suspended, 1, __ATOMIC_RELEASE);
	if (dev->glue_suspend)
		dev->glue_suspend(dev);
	i2c_stat_add(&dev->st.rpm_suspends, 1);
}

static void i2c_dw_runtime_resume(struct dw_i2c_dev *dev)
{
	if (dev->glue_resume && !dev->shared_with_punit)
		dev->glue_resume(dev);
	(void)i2c_dw_init(dev);
	__atomic_store_n(&dev->rpm_suspended, 0, __ATOMIC_RELEASE);
	i2c_stat_add(&dev->st.rpm_resumes, 1);
}

void i2c_dw_rpm_get(struct dw_i2c_dev *dev)
{
	if (__atomic_load_n(&dev->rpm_suspended, __ATOMIC_ACQUIRE))
		i2c_dw_runtime_resume(dev);
	__atomic_add_fetch(&dev->rpm_usage, 1, __ATOMIC_ACQ_REL);
}

void i2c_dw_rpm_put(struct dw_i2c_dev *dev)
{
	int usage = __atomic_sub_fetch(&dev->rpm_usage, 1, __ATOMIC_ACQ_REL);

	WARN_ON_ONCE(usage < 0);
	dev->last_busy_ns = hrtimer_now_ns();
	if (dev->rpm_enabled && usage == 0)
		(void)mod_delayed_work(system_wq, &dev->rpm_work,
				       dev->autosuspend_ms);
}

/* Runtime and system PM take the adapter's own bus lock, never its
 * lock_ops: on a bus the AMD PSP arbitrates those ask the PSP for the bus
 * (a mailbox round trip, retried for seconds while it is busy) -- every
 * idle period would cost two.  The controller registers themselves are
 * reached under the arbitration in i2c_dw_disable()/i2c_dw_init()
 * (i2c_dw_acquire_lock). */

/* With the bus lock held: suspend when idle long enough, else look
 * again when it will have been. */
static void i2c_dw_rpm_check_locked(struct dw_i2c_dev *dev, int force)
{
	uint64_t idle_ns, need_ns;

	if (!dev->rpm_enabled || dev->rpm_suspended || dev->sys_suspended ||
	    __atomic_load_n(&dev->rpm_usage, __ATOMIC_ACQUIRE))
		return;
	idle_ns = hrtimer_now_ns() - dev->last_busy_ns;
	need_ns = (uint64_t)dev->autosuspend_ms * 1000000ull;
	if (force || idle_ns >= need_ns) {
		i2c_dw_runtime_suspend(dev);
		return;
	}
	(void)mod_delayed_work(system_wq, &dev->rpm_work,
			       (unsigned long)((need_ns - idle_ns) / 1000000ull) +
				       1);
}

void i2c_dw_rpm_idle_check(struct dw_i2c_dev *dev)
{
	i2c_adapter_lock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
	i2c_dw_rpm_check_locked(dev, 1);
	i2c_adapter_unlock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
}

static void i2c_dw_rpm_work(struct work_struct *work)
{
	struct delayed_work *dw = to_delayed_work(work);
	struct dw_i2c_dev *dev = container_of(dw, struct dw_i2c_dev, rpm_work);

	/* The bus lock keeps transfers out while the controller goes
	 * down. */
	i2c_adapter_lock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
	i2c_dw_rpm_check_locked(dev, 0);
	i2c_adapter_unlock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
}

void i2c_dw_rpm_enable(struct dw_i2c_dev *dev, unsigned int delay_ms)
{
	if (!delay_ms)
		return;
	dev->autosuspend_ms = delay_ms;
	dev->last_busy_ns = hrtimer_now_ns();
	dev->rpm_enabled = 1;
	(void)mod_delayed_work(system_wq, &dev->rpm_work, delay_ms);
}

int i2c_dw_suspend(struct dw_i2c_dev *dev)
{
	/* A late transfer gets -ESHUTDOWN from now on. */
	i2c_mark_adapter_suspended(&dev->adapter);
	i2c_adapter_lock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
	if (!dev->rpm_suspended)
		i2c_dw_runtime_suspend(dev);
	dev->sys_suspended = 1;
	i2c_adapter_unlock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
	return 0;
}

int i2c_dw_resume(struct dw_i2c_dev *dev)
{
	i2c_adapter_lock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
	dev->sys_suspended = 0;
	if (dev->rpm_suspended || dev->shared_with_punit)
		i2c_dw_runtime_resume(dev);
	dev->last_busy_ns = hrtimer_now_ns();
	i2c_adapter_unlock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
	i2c_mark_adapter_resumed(&dev->adapter);
	if (dev->rpm_enabled)
		(void)mod_delayed_work(system_wq, &dev->rpm_work,
				       dev->autosuspend_ms);
	return 0;
}

void i2c_dw_shutdown(struct dw_i2c_dev *dev)
{
	/* Clients first (their shutdown may still talk to them), then the
	 * controller quiet with its interrupts off. */
	i2c_shutdown_adapter_clients(&dev->adapter);
	i2c_adapter_lock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
	if (!dev->rpm_suspended)
		i2c_dw_disable(dev);
	i2c_adapter_unlock_bus(&dev->adapter, I2C_LOCK_ROOT_ADAPTER);
}

static int i2c_dw_adap_suspend(struct i2c_adapter *adap)
{
	return i2c_dw_suspend(i2c_get_adapdata(adap));
}

static int i2c_dw_adap_resume(struct i2c_adapter *adap)
{
	return i2c_dw_resume(i2c_get_adapdata(adap));
}

static void i2c_dw_adap_shutdown(struct i2c_adapter *adap)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);

	i2c_adapter_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	if (!dev->rpm_suspended)
		i2c_dw_disable(dev);
	i2c_adapter_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
}

static const struct i2c_adapter_pm_ops i2c_dw_pm_ops = {
	.suspend = i2c_dw_adap_suspend,
	.resume = i2c_dw_adap_resume,
	.shutdown = i2c_dw_adap_shutdown,
};

/* ==========================================================================
 * Probe / remove
 * ========================================================================== */

static const struct i2c_algorithm i2c_dw_algo = {
	.master_xfer = i2c_dw_xfer,
	.master_xfer_atomic = i2c_dw_xfer_atomic,
	.functionality = i2c_dw_func,
};

static const struct i2c_adapter_quirks i2c_dw_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
};

static const char *speed_name(uint32_t hz)
{
	switch (hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
		return "standard mode";
	case I2C_MAX_FAST_MODE_FREQ:
		return "fast mode";
	case I2C_MAX_FAST_MODE_PLUS_FREQ:
		return "fast mode plus";
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		return "high speed mode";
	default:
		return "unknown";
	}
}

/* "1.21a" out of 0x3132312A ("121*"). */
static void version_string(uint32_t v, char *out)
{
	char a = (char)(v >> 24), b = (char)(v >> 16), c = (char)(v >> 8);

	if (a < '0' || a > '9' || b < '0' || b > '9' || c < '0' || c > '9') {
		ksnprintf(out, 12, "0x%08x", v);
		return;
	}
	out[0] = a;
	out[1] = '.';
	out[2] = b;
	out[3] = c;
	out[4] = 'a';
	out[5] = '\0';
}

int i2c_dw_probe(struct dw_i2c_dev *dev)
{
	struct i2c_adapter *adap = &dev->adapter;
	char ver[16];
	uint32_t ic_clk;
	int ret;

	might_sleep();
	spinlock_init(&dev->lock, "i2c_dw");
	init_completion(&dev->cmd_complete);
	INIT_DELAYED_WORK(&dev->rpm_work, i2c_dw_rpm_work);
	if (!dev->name[0])
		i2c_strlcpy(dev->name, "i2c_designware", sizeof(dev->name));

	ret = i2c_dw_init_regmap(dev);
	if (ret)
		return ret;
	ret = i2c_dw_set_sda_hold(dev);
	if (ret)
		return ret;
	ret = i2c_dw_set_fifo_size(dev);
	if (ret)
		return ret;
	ret = i2c_dw_probe_master(dev);
	if (ret)
		return ret;
	ret = i2c_dw_init(dev);
	if (ret)
		goto err_recovery;

	if (!adap->name[0])
		i2c_strlcpy(adap->name, "Synopsys DesignWare I2C adapter",
			    sizeof(adap->name));
	adap->retries = 3;
	adap->algo = &i2c_dw_algo;
	adap->quirks = &i2c_dw_quirks;
	adap->pm = &i2c_dw_pm_ops;
	adap->show = i2c_dw_show;
	adap->bus_freq_hz = dev->timings.bus_freq_hz;
	adap->acpi_dev = dev->acpi_dev;
	adap->acpi_handle = dev->acpi_dev ? dev->acpi_dev->handle : NULL;
	i2c_strlcpy(adap->parent_name, dev->name, sizeof(adap->parent_name));
	i2c_set_adapdata(adap, dev);

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		goto err_recovery;
	dev->sw_mask = 0;
	dw_writel(dev, 0, DW_IC_INTR_MASK);
	i2c_dw_release_lock(dev);

	if (dev->irq <= 0)
		dev->flags |= ACCESS_POLLING;
	if (!(dev->flags & ACCESS_POLLING)) {
		/* Shared lines are the rule (LPSS INTx, FCH): the handler
		 * says IRQ_NONE for interrupts that are not ours. */
		ret = request_irq(dev->irq, i2c_dw_isr, IRQF_SHARED, dev->name,
				  dev);
		if (ret) {
			kprintf("%s: cannot request interrupt %d (%d)\n",
				dev->name, dev->irq, ret);
			goto err_recovery;
		}
		dev->irq_requested = 1;
	}

	ic_clk = i2c_dw_clk_rate(dev);
	version_string(dev->comp_version, ver);
	kprintf("%s: DesignWare I2C v%s, %u kHz %s, ic_clk %u kHz, %s %u:%u, SDA hold 0x%x, fifo %u/%u, %s %d%s%s\n",
		dev->name, ver, dev->timings.bus_freq_hz / 1000,
		speed_name(dev->timings.bus_freq_hz), ic_clk,
		(dev->master_cfg & DW_IC_CON_SPEED_MASK) == DW_IC_CON_SPEED_STD ?
			"SS HCNT:LCNT" :
		(dev->master_cfg & DW_IC_CON_SPEED_MASK) == DW_IC_CON_SPEED_HIGH ?
			"HS HCNT:LCNT" :
			"FS HCNT:LCNT",
		(dev->master_cfg & DW_IC_CON_SPEED_MASK) == DW_IC_CON_SPEED_STD ?
			dev->ss_hcnt :
		(dev->master_cfg & DW_IC_CON_SPEED_MASK) == DW_IC_CON_SPEED_HIGH ?
			dev->hs_hcnt :
			dev->fs_hcnt,
		(dev->master_cfg & DW_IC_CON_SPEED_MASK) == DW_IC_CON_SPEED_STD ?
			dev->ss_lcnt :
		(dev->master_cfg & DW_IC_CON_SPEED_MASK) == DW_IC_CON_SPEED_HIGH ?
			dev->hs_lcnt :
			dev->fs_lcnt,
		dev->sda_hold_time, dev->tx_fifo_depth, dev->rx_fifo_depth,
		(dev->flags & ACCESS_POLLING) ? "polled, irq" : "irq",
		dev->irq,
		dev->irq_kind && !(dev->flags & ACCESS_POLLING) ? " " : "",
		dev->irq_kind && !(dev->flags & ACCESS_POLLING) ?
			dev->irq_kind :
			"");

	/* No runtime suspend while the clients probe. */
	__atomic_add_fetch(&dev->rpm_usage, 1, __ATOMIC_ACQ_REL);
	ret = i2c_add_numbered_adapter(adap);
	__atomic_sub_fetch(&dev->rpm_usage, 1, __ATOMIC_ACQ_REL);
	if (ret) {
		kprintf("%s: failure adding adapter: %d\n", dev->name, ret);
		goto err_irq;
	}
	dev->last_busy_ns = hrtimer_now_ns();
	return 0;

err_irq:
	if (dev->irq_requested) {
		(void)free_irq(dev->irq, dev);
		dev->irq_requested = 0;
	}
err_recovery:
	i2c_dw_release_recovery(dev);
	return ret;
}

void i2c_dw_remove(struct dw_i2c_dev *dev)
{
	might_sleep();
	i2c_del_adapter(&dev->adapter);
	dev->rpm_enabled = 0;
	(void)cancel_delayed_work_sync(&dev->rpm_work);
	if (!dev->rpm_suspended)
		i2c_dw_disable(dev);
	if (dev->irq_requested) {
		(void)free_irq(dev->irq, dev);
		dev->irq_requested = 0;
	}
	i2c_dw_release_recovery(dev);
	/* Last: the disable above still arbitrated the bus. */
	i2c_dw_remove_lock_support(dev);
}

int i2c_dw_isr(void *arg)
{
	return i2c_dw_isr_master(arg);
}

/* ==========================================================================
 * /proc/i2c
 * ========================================================================== */

long i2c_dw_show(struct i2c_adapter *adap, char *buf, long cap, long pos)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);
	const struct dw_i2c_stats *s = &dev->st;
	char ver[16];

	version_string(dev->comp_version, ver);
	pos = i2c_show_printf(buf, cap, pos,
			      "  dw: v%s %s ic_clk %u kHz con 0x%x fifo %u/%u sda_hold 0x%x spklen %u/%u ss %u:%u fs %u:%u hs %u:%u access %s%s%s\n",
			      ver, dev->name, i2c_dw_clk_rate(dev),
			      dev->master_cfg, dev->tx_fifo_depth,
			      dev->rx_fifo_depth, dev->sda_hold_time,
			      dev->fs_spklen, dev->hs_spklen, dev->ss_hcnt,
			      dev->ss_lcnt, dev->fs_hcnt, dev->fs_lcnt,
			      dev->hs_hcnt, dev->hs_lcnt,
			      dev->access == DW_ACCESS_SWAP ? "swapped" :
			      dev->access == DW_ACCESS_16   ? "16-bit" :
							      "32-bit",
			      dev->rpm_suspended ? " runtime-suspended" : "",
			      dev->hw_bus_clear ? " bus-clear" : "");
	pos = i2c_show_printf(buf, cap, pos,
			      "  dw: irq %d (%s) irqs %lu none %lu unexpected %lu parts %lu polled %lu timeouts %lu reinits %lu aborts %lu spurious-stop %lu early %lu active-after %lu busy %lu disable-timeouts %lu bus-clear %lu rpm %lu/%lu\n",
			      dev->irq,
			      (dev->flags & ACCESS_POLLING) ? "polled" :
			      dev->irq_kind			  ? dev->irq_kind :
								    "-",
			      (unsigned long)s->irqs,
			      (unsigned long)s->irq_none,
			      (unsigned long)s->irq_unexpected,
			      (unsigned long)s->xfers,
			      (unsigned long)s->polled,
			      (unsigned long)s->timeouts,
			      (unsigned long)s->reinits,
			      (unsigned long)s->aborts,
			      (unsigned long)s->spurious_stop,
			      (unsigned long)s->early_term,
			      (unsigned long)s->active_after,
			      (unsigned long)s->bus_busy,
			      (unsigned long)s->disable_timeouts,
			      (unsigned long)s->hw_bus_clear,
			      (unsigned long)s->rpm_suspends,
			      (unsigned long)s->rpm_resumes);
	if (s->aborts) {
		pos = i2c_show_printf(buf, cap, pos, "  dw: abort causes:");
		for (int i = 0; i < DW_IC_ABRT_SOURCES; i++)
			if (s->abort_src[i])
				pos = i2c_show_printf(buf, cap, pos,
						      " [%s]=%lu",
						      i2c_dw_abort_name(i),
						      (unsigned long)s->abort_src[i]);
		pos = i2c_show_printf(buf, cap, pos, "\n");
	}
	pos = i2c_dw_baytrail_show(dev, buf, cap, pos);
	pos = i2c_dw_amdpsp_show(dev, buf, cap, pos);
	return pos;
}
