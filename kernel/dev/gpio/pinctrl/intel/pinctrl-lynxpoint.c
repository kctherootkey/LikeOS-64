// LikeOS pinctrl - Intel Lynx Point-LP / Wildcat Point-LP GPIO controller
//
// The GPIO block of the Haswell-ULT (Lynx Point-LP, ACPI INT33C7) and
// Broadwell-U (Wildcat Point-LP, INT3437) PCHs predates the community
// design: 95 GPIOs behind one I/O port window (the controller's IO
// resource), with
//
//   * bitmapped registers, one dword per 32 GPIOs:
//       0x00 GPIO_OWNER   set by firmware, 0 = pin reserved for ACPI
//       0x10 IRQ2IOXAPIC  set by firmware, 1 = routed to the IOxAPIC
//                         (only GPIO 8-10, 13-14, 45-55 can be)
//       0x80 INT_STAT     interrupt status, write 1 to clear
//       0x90 INT_ENABLE   interrupt enable
//   * two dwords per GPIO from 0x100 (GPIO n at 0x100 + 8n):
//       CONFIG1: 31 output level, 30 input level, 4 TRIG_SEL (0 edge,
//                1 level), 3 INT_INV, 2 direction (1 = input),
//                1:0 USE_SEL (0 native, 1 GPIO)
//       CONFIG2: 2 GPINDIS (input sensing off), 1:0 weak pull (0 none,
//                1 down, 2 up)
//
// GPIO numbers are pin numbers.  The interrupt enable bits are the mask:
// a masked GPIO keeps latching INT_STAT, and the parent interrupt (the
// controller's Interrupt resource) is demultiplexed from INT_STAT &
// INT_ENABLE.  Pins the firmware reserved for ACPI cannot be interrupts.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2012-2019 Intel Corporation

#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* Port I/O; the host test harness substitutes a register model. */
#ifndef lp_ioread32
#define lp_ioread32(addr) inl((uint16_t)(addr))
#define lp_iowrite32(val, addr) outl((uint16_t)(addr), (val))
#endif

#define LPTLP_COMMUNITY(p, n, g)                                           \
	{                                                                  \
		.pin_base = (p), .npins = (n), .gpps = (g),                \
		.ngpps = ARRAY_SIZE(g),                                    \
	}

static const struct pinctrl_pin_desc lptlp_pins[] = {
	PINCTRL_PIN(0, "GP0_UART1_RXD"),
	PINCTRL_PIN(1, "GP1_UART1_TXD"),
	PINCTRL_PIN(2, "GP2_UART1_RTSB"),
	PINCTRL_PIN(3, "GP3_UART1_CTSB"),
	PINCTRL_PIN(4, "GP4_I2C0_SDA"),
	PINCTRL_PIN(5, "GP5_I2C0_SCL"),
	PINCTRL_PIN(6, "GP6_I2C1_SDA"),
	PINCTRL_PIN(7, "GP7_I2C1_SCL"),
	PINCTRL_PIN(8, "GP8"),
	PINCTRL_PIN(9, "GP9"),
	PINCTRL_PIN(10, "GP10"),
	PINCTRL_PIN(11, "GP11_SMBALERTB"),
	PINCTRL_PIN(12, "GP12_LANPHYPC"),
	PINCTRL_PIN(13, "GP13"),
	PINCTRL_PIN(14, "GP14"),
	PINCTRL_PIN(15, "GP15"),
	PINCTRL_PIN(16, "GP16_MGPIO9"),
	PINCTRL_PIN(17, "GP17_MGPIO10"),
	PINCTRL_PIN(18, "GP18_SRC0CLKRQB"),
	PINCTRL_PIN(19, "GP19_SRC1CLKRQB"),
	PINCTRL_PIN(20, "GP20_SRC2CLKRQB"),
	PINCTRL_PIN(21, "GP21_SRC3CLKRQB"),
	PINCTRL_PIN(22, "GP22_SRC4CLKRQB_TRST2"),
	PINCTRL_PIN(23, "GP23_SRC5CLKRQB_TDI2"),
	PINCTRL_PIN(24, "GP24_MGPIO0"),
	PINCTRL_PIN(25, "GP25_USBWAKEOUTB"),
	PINCTRL_PIN(26, "GP26_MGPIO5"),
	PINCTRL_PIN(27, "GP27_MGPIO6"),
	PINCTRL_PIN(28, "GP28_MGPIO7"),
	PINCTRL_PIN(29, "GP29_SLP_WLANB_MGPIO3"),
	PINCTRL_PIN(30, "GP30_SUSWARNB_SUSPWRDNACK_MGPIO1"),
	PINCTRL_PIN(31, "GP31_ACPRESENT_MGPIO2"),
	PINCTRL_PIN(32, "GP32_CLKRUNB"),
	PINCTRL_PIN(33, "GP33_DEVSLP0"),
	PINCTRL_PIN(34, "GP34_SATA0XPCIE6L3B_SATA0GP"),
	PINCTRL_PIN(35, "GP35_SATA1XPCIE6L2B_SATA1GP"),
	PINCTRL_PIN(36, "GP36_SATA2XPCIE6L1B_SATA2GP"),
	PINCTRL_PIN(37, "GP37_SATA3XPCIE6L0B_SATA3GP"),
	PINCTRL_PIN(38, "GP38_DEVSLP1"),
	PINCTRL_PIN(39, "GP39_DEVSLP2"),
	PINCTRL_PIN(40, "GP40_OC0B"),
	PINCTRL_PIN(41, "GP41_OC1B"),
	PINCTRL_PIN(42, "GP42_OC2B"),
	PINCTRL_PIN(43, "GP43_OC3B"),
	PINCTRL_PIN(44, "GP44"),
	PINCTRL_PIN(45, "GP45_TMS2"),
	PINCTRL_PIN(46, "GP46_TDO2"),
	PINCTRL_PIN(47, "GP47"),
	PINCTRL_PIN(48, "GP48"),
	PINCTRL_PIN(49, "GP49"),
	PINCTRL_PIN(50, "GP50"),
	PINCTRL_PIN(51, "GP51_GSXDOUT"),
	PINCTRL_PIN(52, "GP52_GSXSLOAD"),
	PINCTRL_PIN(53, "GP53_GSXDIN"),
	PINCTRL_PIN(54, "GP54_GSXSRESETB"),
	PINCTRL_PIN(55, "GP55_GSXCLK"),
	PINCTRL_PIN(56, "GP56"),
	PINCTRL_PIN(57, "GP57"),
	PINCTRL_PIN(58, "GP58"),
	PINCTRL_PIN(59, "GP59"),
	PINCTRL_PIN(60, "GP60_SML0ALERTB_MGPIO4"),
	PINCTRL_PIN(61, "GP61_SUS_STATB"),
	PINCTRL_PIN(62, "GP62_SUSCLK"),
	PINCTRL_PIN(63, "GP63_SLP_S5B"),
	PINCTRL_PIN(64, "GP64_SDIO_CLK"),
	PINCTRL_PIN(65, "GP65_SDIO_CMD"),
	PINCTRL_PIN(66, "GP66_SDIO_D0"),
	PINCTRL_PIN(67, "GP67_SDIO_D1"),
	PINCTRL_PIN(68, "GP68_SDIO_D2"),
	PINCTRL_PIN(69, "GP69_SDIO_D3"),
	PINCTRL_PIN(70, "GP70_SDIO_POWER_EN"),
	PINCTRL_PIN(71, "GP71_MPHYPC"),
	PINCTRL_PIN(72, "GP72_BATLOWB"),
	PINCTRL_PIN(73, "GP73_SML1ALERTB_PCHHOTB_MGPIO8"),
	PINCTRL_PIN(74, "GP74_SML1DATA_MGPIO12"),
	PINCTRL_PIN(75, "GP75_SML1CLK_MGPIO11"),
	PINCTRL_PIN(76, "GP76_BMBUSYB"),
	PINCTRL_PIN(77, "GP77_PIRQAB"),
	PINCTRL_PIN(78, "GP78_PIRQBB"),
	PINCTRL_PIN(79, "GP79_PIRQCB"),
	PINCTRL_PIN(80, "GP80_PIRQDB"),
	PINCTRL_PIN(81, "GP81_SPKR"),
	PINCTRL_PIN(82, "GP82_RCINB"),
	PINCTRL_PIN(83, "GP83_GSPI0_CSB"),
	PINCTRL_PIN(84, "GP84_GSPI0_CLK"),
	PINCTRL_PIN(85, "GP85_GSPI0_MISO"),
	PINCTRL_PIN(86, "GP86_GSPI0_MOSI"),
	PINCTRL_PIN(87, "GP87_GSPI1_CSB"),
	PINCTRL_PIN(88, "GP88_GSPI1_CLK"),
	PINCTRL_PIN(89, "GP89_GSPI1_MISO"),
	PINCTRL_PIN(90, "GP90_GSPI1_MOSI"),
	PINCTRL_PIN(91, "GP91_UART0_RXD"),
	PINCTRL_PIN(92, "GP92_UART0_TXD"),
	PINCTRL_PIN(93, "GP93_UART0_RTSB"),
	PINCTRL_PIN(94, "GP94_UART0_CTSB"),
};

static const struct intel_padgroup lptlp_gpps[] = {
	INTEL_GPP(0, 0, 31, 0),
	INTEL_GPP(1, 32, 63, 32),
	INTEL_GPP(2, 64, 94, 64),
};

static const struct intel_community lptlp_communities[] = {
	LPTLP_COMMUNITY(0, 95, lptlp_gpps),
};

static const struct intel_pinctrl_soc_data lptlp_soc_data = {
	.pins = lptlp_pins,
	.npins = ARRAY_SIZE(lptlp_pins),
	.communities = lptlp_communities,
	.ncommunities = ARRAY_SIZE(lptlp_communities),
};

const struct intel_pinctrl_match intel_lynxpoint_match[] = {
	{ .hid = "INT33C7", .data = &lptlp_soc_data },
	{ .hid = "INT3437", .data = &lptlp_soc_data },
	{ }
};

/* Lynx Point has 95 GPIO pins */
#define LP_NUM_GPIO 95

/* Bitmapped register offsets */
#define LP_ACPI_OWNED 0x00 /* set by firmware, 0: pin reserved for ACPI */
#define LP_IRQ2IOXAPIC 0x10 /* set by firmware, 1: pin routed to IOxAPIC */
#define LP_GC 0x7C /* APIC IRQ 14 or 15 for all pins */
#define LP_INT_STAT 0x80
#define LP_INT_ENABLE 0x90

/* Two 32-bit config registers per pin, from 0x100 */
#define LP_CONFIG1 0x100
#define LP_CONFIG2 0x104

/* LP_CONFIG1 bits */
#define OUT_LVL_BIT (1u << 31)
#define IN_LVL_BIT (1u << 30)
#define TRIG_SEL_BIT (1u << 4) /* 0: edge, 1: level */
#define INT_INV_BIT (1u << 3) /* invert interrupt triggering */
#define DIR_BIT (1u << 2) /* 0: output, 1: input */
#define USE_SEL_MASK 3u /* 0: native, 1: GPIO, ... */
#define USE_SEL_NATIVE 0u
#define USE_SEL_GPIO 1u

/* LP_CONFIG2 bits */
#define GPINDIS_BIT (1u << 2) /* disable input sensing */
#define GPIWP_MASK 3u /* weak pull options */
#define GPIWP_NONE 0u
#define GPIWP_DOWN 1u
#define GPIWP_UP 2u

struct lp_gpio {
	struct gpio_chip chip;
	const struct intel_pinctrl_soc_data *soc;
	struct acpi_device *adev;
	unsigned long regs; /* the I/O port window (or the model's) */
	uint64_t length;
	spinlock_t lock;
	int irq;
	uint32_t gsi;
	struct list_head node;
	char label[48];
};

static DEFINE_MUTEX(g_lp_list_lock);
static struct list_head g_lp_list = LIST_HEAD_INIT(g_lp_list);

static struct lp_gpio *gc_to_lg(struct gpio_chip *gc)
{
	return container_of(gc, struct lp_gpio, chip);
}

/* The address of register `reg' for GPIO `offset': per-pin registers
 * (CONFIG1/2) at 8 bytes per pin, bitmapped ones at a dword per 32. */
static unsigned long lp_gpio_reg(struct lp_gpio *lg, unsigned int offset,
			     unsigned int reg)
{
	const struct intel_community *comm = &lg->soc->communities[0];
	unsigned int reg_offset;

	if (offset < comm->pin_base || offset >= comm->pin_base + comm->npins)
		return 0;
	offset -= comm->pin_base;
	if (reg == LP_CONFIG1 || reg == LP_CONFIG2)
		reg_offset = offset * 8;
	else
		reg_offset = (offset / 32) * 4;
	return lg->regs + reg_offset + reg;
}

static int lp_gpio_acpi_use(struct lp_gpio *lg, unsigned int pin)
{
	unsigned long acpi_use = lp_gpio_reg(lg, pin, LP_ACPI_OWNED);

	if (!acpi_use)
		return 1;
	return !(lp_ioread32(acpi_use) & (1u << (pin % 32)));
}

static int lp_gpio_ioxapic_use(struct lp_gpio *lg, unsigned int offset)
{
	unsigned long ioxapic_use = lp_gpio_reg(lg, offset, LP_IRQ2IOXAPIC);
	uint32_t value;

	if (!ioxapic_use)
		return 0;
	value = lp_ioread32(ioxapic_use);
	if (offset >= 8 && offset <= 10)
		return !!(value & (1u << (offset - 8 + 0)));
	if (offset >= 13 && offset <= 14)
		return !!(value & (1u << (offset - 13 + 3)));
	if (offset >= 45 && offset <= 55)
		return !!(value & (1u << (offset - 45 + 5)));
	return 0;
}

static void lp_gpio_enable_input(unsigned long reg)
{
	lp_iowrite32(lp_ioread32(reg) & ~GPINDIS_BIT, reg);
}

static void lp_gpio_disable_input(unsigned long reg)
{
	lp_iowrite32(lp_ioread32(reg) | GPINDIS_BIT, reg);
}

/* ==========================================================================
 * GPIO operations
 * ========================================================================== */

static int lp_gpio_valid(struct gpio_chip *gc, unsigned int offset)
{
	return lp_gpio_reg(gc_to_lg(gc), offset, LP_CONFIG1) != 0;
}

static int lp_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	unsigned long reg = lp_gpio_reg(lg, offset, LP_CONFIG1);
	unsigned long conf2 = lp_gpio_reg(lg, offset, LP_CONFIG2);
	uint32_t value;
	uint64_t fl;
	int forced = 0;

	if (!reg)
		return -EINVAL;
	spin_lock_irqsave(&lg->lock, &fl);
	/* Firmware is expected to have made it a GPIO; if not, do it (the
	 * mode bits only) and say so. */
	value = lp_ioread32(reg);
	if ((value & USE_SEL_MASK) != USE_SEL_GPIO) {
		lp_iowrite32((value & ~USE_SEL_MASK) | USE_SEL_GPIO, reg);
		forced = 1;
	}
	/* Enable input sensing */
	lp_gpio_enable_input(conf2);
	spin_unlock_irqrestore(&lg->lock, fl);
	if (forced)
		kprintf("%s: firmware bug: pin %u forcibly reconfigured as GPIO\n",
			lg->label, offset);
	return 0;
}

static void lp_gpio_free(struct gpio_chip *gc, unsigned int offset)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	unsigned long conf2 = lp_gpio_reg(lg, offset, LP_CONFIG2);
	uint64_t fl;

	if (!conf2)
		return;
	spin_lock_irqsave(&lg->lock, &fl);
	/* Disable input sensing */
	lp_gpio_disable_input(conf2);
	spin_unlock_irqrestore(&lg->lock, fl);
}

static int lp_gpio_set_direction(struct lp_gpio *lg, unsigned int pin,
				 int input)
{
	unsigned long reg = lp_gpio_reg(lg, pin, LP_CONFIG1);
	uint32_t value;
	uint64_t fl;
	int warn = 0;

	if (!reg)
		return -EINVAL;
	spin_lock_irqsave(&lg->lock, &fl);
	value = lp_ioread32(reg);
	value &= ~DIR_BIT;
	if (input)
		value |= DIR_BIT;
	else
		/* An output that is also a direct IOxAPIC interrupt makes
		 * no sense: warn the caller. */
		warn = lp_gpio_ioxapic_use(lg, pin);
	lp_iowrite32(value, reg);
	spin_unlock_irqrestore(&lg->lock, fl);
	WARN(warn, "%s: setting GPIO %u to output with IOxAPIC redirection",
	     lg->label, pin);
	return 0;
}

static int lp_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	unsigned long reg = lp_gpio_reg(gc_to_lg(gc), offset, LP_CONFIG1);

	if (!reg)
		return -EINVAL;
	return !!(lp_ioread32(reg) & IN_LVL_BIT);
}

static void lp_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	unsigned long reg = lp_gpio_reg(lg, offset, LP_CONFIG1);
	uint64_t fl;

	if (!reg)
		return;
	spin_lock_irqsave(&lg->lock, &fl);
	if (value)
		lp_iowrite32(lp_ioread32(reg) | OUT_LVL_BIT, reg);
	else
		lp_iowrite32(lp_ioread32(reg) & ~OUT_LVL_BIT, reg);
	spin_unlock_irqrestore(&lg->lock, fl);
}

static int lp_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	return lp_gpio_set_direction(gc_to_lg(gc), offset, 1);
}

static int lp_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				    int value)
{
	lp_gpio_set(gc, offset, value);
	return lp_gpio_set_direction(gc_to_lg(gc), offset, 0);
}

static int lp_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	unsigned long reg = lp_gpio_reg(gc_to_lg(gc), offset, LP_CONFIG1);

	if (!reg)
		return -EINVAL;
	if (lp_ioread32(reg) & DIR_BIT)
		return GPIO_LINE_DIRECTION_IN;
	return GPIO_LINE_DIRECTION_OUT;
}

static int lp_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
			      enum pin_config_param param, uint32_t arg)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	unsigned long conf2 = lp_gpio_reg(lg, offset, LP_CONFIG2);
	uint32_t value;
	uint64_t fl;

	(void)arg;
	if (!conf2)
		return -EINVAL;
	spin_lock_irqsave(&lg->lock, &fl);
	value = lp_ioread32(conf2);
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		value &= ~GPIWP_MASK;
		value |= GPIWP_NONE;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		value &= ~GPIWP_MASK;
		value |= GPIWP_DOWN;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		value &= ~GPIWP_MASK;
		value |= GPIWP_UP;
		break;
	default:
		spin_unlock_irqrestore(&lg->lock, fl);
		return -ENOTSUPP;
	}
	lp_iowrite32(value, conf2);
	spin_unlock_irqrestore(&lg->lock, fl);
	return 0;
}

static void lp_gpio_describe(struct gpio_chip *gc, unsigned int offset,
			     char *buf, size_t len)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	unsigned long reg = lp_gpio_reg(lg, offset, LP_CONFIG1);
	unsigned long conf2 = lp_gpio_reg(lg, offset, LP_CONFIG2);
	uint32_t value;

	if (!reg) {
		ksnprintf(buf, len, "-");
		return;
	}
	value = lp_ioread32(reg);
	ksnprintf(buf, len, "%s %s %08x %08x%s", lptlp_pins[offset].name,
		  (value & USE_SEL_MASK) == USE_SEL_GPIO ? "GPIO" : "native",
		  value, lp_ioread32(conf2),
		  lp_gpio_acpi_use(lg, offset) ? " [ACPI]" : "");
}

/* ==========================================================================
 * Interrupts (gpiolib's irq lock held, interrupts off)
 * ========================================================================== */

static void lp_irq_ack(struct gpio_chip *gc, unsigned int hwirq)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	unsigned long reg = lp_gpio_reg(lg, hwirq, LP_INT_STAT);
	uint64_t fl;

	if (!reg)
		return;
	spin_lock_irqsave(&lg->lock, &fl);
	lp_iowrite32(1u << (hwirq % 32), reg);
	spin_unlock_irqrestore(&lg->lock, fl);
}

/* The enable bit is the mask (the status keeps latching). */
static void lp_irq_mask_unmask(struct gpio_chip *gc, unsigned int hwirq,
			       int mask)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	unsigned long reg = lp_gpio_reg(lg, hwirq, LP_INT_ENABLE);
	uint64_t fl;

	if (!reg)
		return;
	spin_lock_irqsave(&lg->lock, &fl);
	if (mask)
		lp_iowrite32(lp_ioread32(reg) & ~(1u << (hwirq % 32)), reg);
	else
		lp_iowrite32(lp_ioread32(reg) | (1u << (hwirq % 32)), reg);
	spin_unlock_irqrestore(&lg->lock, fl);
}

static void lp_irq_mask(struct gpio_chip *gc, unsigned int hwirq)
{
	lp_irq_mask_unmask(gc, hwirq, 1);
}

static void lp_irq_unmask(struct gpio_chip *gc, unsigned int hwirq)
{
	lp_irq_mask_unmask(gc, hwirq, 0);
}

static int lp_irq_set_type(struct gpio_chip *gc, unsigned int hwirq,
			   unsigned int type)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	unsigned long reg = lp_gpio_reg(lg, hwirq, LP_CONFIG1);
	uint32_t value;
	uint64_t fl;

	if (!reg)
		return -EINVAL;
	/* Fail if firmware reserved the pin for ACPI */
	if (lp_gpio_acpi_use(lg, hwirq)) {
		kprintf("%s: pin %u can't be used as IRQ\n", lg->label, hwirq);
		return -EBUSY;
	}
	spin_lock_irqsave(&lg->lock, &fl);
	value = lp_ioread32(reg);
	/* TRIG_SEL 0, INV 0: rising edge */
	if (type & IRQ_TYPE_EDGE_RISING)
		value &= ~(TRIG_SEL_BIT | INT_INV_BIT);
	/* TRIG_SEL 0, INV 1: falling edge */
	if (type & IRQ_TYPE_EDGE_FALLING)
		value = (value | INT_INV_BIT) & ~TRIG_SEL_BIT;
	/* TRIG_SEL 1, INV 0: level low */
	if (type & IRQ_TYPE_LEVEL_LOW)
		value = (value | TRIG_SEL_BIT) & ~INT_INV_BIT;
	/* TRIG_SEL 1, INV 1: level high */
	if (type & IRQ_TYPE_LEVEL_HIGH)
		value |= TRIG_SEL_BIT | INT_INV_BIT;
	lp_iowrite32(value, reg);
	spin_unlock_irqrestore(&lg->lock, fl);
	return 0;
}

static int lp_irq_request(struct gpio_chip *gc, unsigned int hwirq)
{
	struct lp_gpio *lg = gc_to_lg(gc);

	if (!lp_gpio_reg(lg, hwirq, LP_CONFIG1))
		return -EINVAL;
	if (lp_gpio_acpi_use(lg, hwirq)) {
		kprintf("%s: pin %u is reserved for ACPI, no IRQ\n", lg->label,
			hwirq);
		return -EBUSY;
	}
	return 0;
}

static const struct gpio_irq_chip_ops g_lp_irq_ops = {
	.irq_mask = lp_irq_mask,
	.irq_unmask = lp_irq_unmask,
	.irq_ack = lp_irq_ack,
	.irq_set_type = lp_irq_set_type,
	.irq_set_wake = NULL, /* the parent is not armed per pin */
	.irq_request = lp_irq_request,
};

static void lp_gpio_irq_init_hw(struct lp_gpio *lg)
{
	uint64_t fl;

	spin_lock_irqsave(&lg->lock, &fl);
	for (unsigned int base = 0; base < lg->chip.ngpio; base += 32) {
		/* disable gpio pin interrupts */
		lp_iowrite32(0, lp_gpio_reg(lg, base, LP_INT_ENABLE));
		/* clear interrupt status register */
		lp_iowrite32(0xffffffffu, lp_gpio_reg(lg, base, LP_INT_STAT));
	}
	spin_unlock_irqrestore(&lg->lock, fl);
}

static int lp_gpio_irq_handler(void *arg)
{
	struct lp_gpio *lg = arg;
	int handled = 0;

	/* Which pins of the controller triggered the interrupt */
	for (unsigned int base = 0; base < lg->chip.ngpio; base += 32) {
		unsigned long reg = lp_gpio_reg(lg, base, LP_INT_STAT);
		unsigned long ena = lp_gpio_reg(lg, base, LP_INT_ENABLE);
		uint32_t pending;

		spin_lock(&lg->lock);
		/* Only interrupts that are enabled */
		pending = lp_ioread32(reg) & lp_ioread32(ena);
		spin_unlock(&lg->lock);
		if (lg->chip.ngpio - base < 32)
			pending &= (1u << (lg->chip.ngpio - base)) - 1;
		if (pending)
			handled = 1;
		while (pending) {
			unsigned int bit = (unsigned int)__builtin_ctz(pending);

			pending &= pending - 1;
			gpiochip_handle_irq(&lg->chip, base + bit);
		}
	}
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/* ==========================================================================
 * Set-up and probe
 * ========================================================================== */

/* Fill the chip of `lg' over the register window at `regs' and mask
 * every interrupt.  0 or -EINVAL (window too small). */
static int lp_gpio_setup(struct lp_gpio *lg, unsigned long regs, uint64_t length)
{
	if (!lg || !regs || length < LP_CONFIG2 + 4 + (LP_NUM_GPIO - 1) * 8)
		return -EINVAL;
	lg->soc = &lptlp_soc_data;
	lg->regs = regs;
	lg->length = length;
	spinlock_init(&lg->lock, "lp_gpio");
	INIT_LIST_HEAD(&lg->node);
	lg->chip.label = lg->label;
	lg->chip.ngpio = LP_NUM_GPIO;
	lg->chip.data = lg;
	lg->chip.request = lp_gpio_request;
	lg->chip.free = lp_gpio_free;
	lg->chip.get_direction = lp_gpio_get_direction;
	lg->chip.direction_input = lp_gpio_direction_input;
	lg->chip.direction_output = lp_gpio_direction_output;
	lg->chip.get = lp_gpio_get;
	lg->chip.set = lp_gpio_set;
	lg->chip.set_config = lp_gpio_set_config;
	lg->chip.valid = lp_gpio_valid;
	lg->chip.describe = lp_gpio_describe;
	lg->chip.irq = &g_lp_irq_ops;
	lp_gpio_irq_init_hw(lg);
	return 0;
}

struct gpio_chip *lp_gpio_create(unsigned long regs, uint64_t length,
				 const char *label)
{
	struct lp_gpio *lg = kcalloc(1, sizeof(*lg));

	if (!lg)
		return NULL;
	ksnprintf(lg->label, sizeof(lg->label), "%s", label ? label : "lp-gpio");
	if (lp_gpio_setup(lg, regs, length)) {
		kfree(lg);
		return NULL;
	}
	return &lg->chip;
}

void lp_gpio_destroy(struct gpio_chip *gc)
{
	if (gc)
		kfree(gc_to_lg(gc));
}

int lp_gpio_handle_parent(struct gpio_chip *gc)
{
	return lp_gpio_irq_handler(gc_to_lg(gc));
}

/* Input sensing is cleared by some hardware over suspend: turn it back on
 * for every requested line. */
void lp_gpio_resume(struct gpio_chip *gc)
{
	struct lp_gpio *lg = gc_to_lg(gc);
	uint64_t fl;

	for (unsigned int i = 0; i < lg->chip.ngpio; i++) {
		if (!gpiochip_line_is_requested(&lg->chip, i))
			continue;
		spin_lock_irqsave(&lg->lock, &fl);
		lp_gpio_enable_input(lp_gpio_reg(lg, i, LP_CONFIG2));
		spin_unlock_irqrestore(&lg->lock, fl);
	}
}

void lp_gpio_resume_all(void)
{
	struct list_head *p;

	mutex_lock(&g_lp_list_lock);
	for (p = g_lp_list.next; p != &g_lp_list; p = p->next)
		lp_gpio_resume(&container_of(p, struct lp_gpio, node)->chip);
	mutex_unlock(&g_lp_list_lock);
}

static int lp_gpio_probe(struct acpi_device *adev)
{
	struct acpi_res_list res;
	const struct acpi_res *io, *irqres;
	struct lp_gpio *lg;
	int rc;

	might_sleep();
	rc = acpi_dev_get_resources(adev, &res);
	if (rc)
		return rc;
	io = acpi_res_find(&res, ACPI_RES_IO, 0);
	if (!io || io->io.start == 0 || io->io.start + io->io.length > 0x10000) {
		kprintf("lp-gpio: %s: missing IO resources\n", adev->path);
		acpi_dev_free_resource_list(&res);
		return -EINVAL;
	}
	lg = kcalloc(1, sizeof(*lg));
	if (!lg) {
		acpi_dev_free_resource_list(&res);
		return -ENOMEM;
	}
	lg->adev = acpi_dev_get(adev);
	ksnprintf(lg->label, sizeof(lg->label), "%s:%s", adev->hid,
		  adev->has_uid ? adev->uid : "0");
	rc = lp_gpio_setup(lg, (unsigned long)io->io.start, io->io.length);
	if (rc) {
		kprintf("%s: IO window %#lx+%#lx too small for %u GPIOs\n",
			lg->label, (unsigned long)io->io.start,
			(unsigned long)io->io.length, LP_NUM_GPIO);
		goto fail;
	}
	lg->chip.acpi_handle = adev->handle;
	/* Interrupts are optional */
	irqres = acpi_res_find(&res, ACPI_RES_IRQ, 0);
	lg->chip.irq = NULL;
	if (irqres) {
		int irq = irq_alloc_desc_gsi(NULL, lg, (int)irqres->irq.gsi,
					     irqres->irq.polarity ==
						     ACPI_RES_ACTIVE_LOW,
					     irqres->irq.triggering ==
						     ACPI_RES_LEVEL);

		if (irq > 0) {
			rc = request_irq(irq, lp_gpio_irq_handler, IRQF_SHARED,
					 lg->label, lg);
			if (rc) {
				irq_free_desc(irq);
				irq = rc;
			}
		}
		if (irq > 0) {
			lg->irq = irq;
			lg->gsi = irqres->irq.gsi;
			lg->chip.irq = &g_lp_irq_ops;
		} else {
			kprintf("%s: GSI %u unavailable (%d), no interrupts\n",
				lg->label, irqres->irq.gsi, irq);
		}
	}
	rc = gpiochip_add(&lg->chip);
	if (rc) {
		if (lg->irq > 0) {
			(void)free_irq(lg->irq, lg);
			irq_free_desc(lg->irq);
		}
		goto fail;
	}
	mutex_lock(&g_lp_list_lock);
	list_add_tail(&lg->node, &g_lp_list);
	mutex_unlock(&g_lp_list_lock);
	kprintf("%s: %u GPIOs at I/O %#lx%s\n", lg->label, lg->chip.ngpio,
		(unsigned long)io->io.start, lg->irq > 0 ? ", interrupts" : "");
	acpi_dev_free_resource_list(&res);
	return 0;
fail:
	acpi_dev_put(lg->adev);
	kfree(lg);
	acpi_dev_free_resource_list(&res);
	return rc;
}

void lp_gpio_init(void)
{
	static const struct acpi_device_id ids[] = {
		{ "INT33C7", 0 },
		{ "INT3437", 0 },
		{ "", 0 },
	};
	struct acpi_device *devs[4];
	int n;

	might_sleep();
	if (!acpi_dev_ready())
		return;
	n = acpi_dev_collect_matches(ids, 1, devs, 4);
	for (int i = 0; i < n; i++) {
		if (!gpiochip_find_by_acpi_handle(devs[i]->handle))
			(void)lp_gpio_probe(devs[i]);
		acpi_dev_put(devs[i]);
	}
}
