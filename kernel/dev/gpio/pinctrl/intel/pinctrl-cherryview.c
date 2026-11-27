// LikeOS pinctrl - Intel Cherry View / Braswell GPIO controller
//
// Cherry View (Atom x5/x7 Z8000, Celeron/Pentium N3000 "Braswell") has
// four communities, each its own ACPI device INT33FF with _UID "1"
// (southwest), "2" (north), "3" (east), "4" (southeast).  Pads are grouped
// into families of up to 15; a pad's number within the community is
// family * 15 + index, so the numbering -- which is also the GPIO
// numbering firmware uses -- has holes after short families.  Registers:
//
//   * per pad, at 0x4400 + family * 0x400 + index * 8:
//       PADCTRL0: 31:28 INTSEL (which of the community's 16 interrupt
//         lines the pad drives), 23 TERM_UP, 22:20 termination (1 20k,
//         2 5k, 4 1k), 19:16 PMODE, 15 GPIOEN, 10:8 GPIOCFG (0 both
//         buffers, 1 output, 2 input, 3 Hi-Z), 1 TXSTATE, 0 RXSTATE
//       PADCTRL1: 31 CFGLOCK, 7:4 INVRXTX (4 = invert RX data),
//         3 ODEN, 2:0 INTWAKECFG (1 falling, 2 rising, 3 both, 4 level)
//   * per community: INTSTAT 0x300 and INTMASK 0x380, one bit per
//     interrupt LINE, not per pad.  The firmware assigns lines to pads
//     (INTSEL); the driver remembers which pad drives which line.
//
// A locked pad (CFGLOCK) keeps the trigger the firmware programmed; only
// the line mapping is recorded.  The communities also back a vendor
// operation region (address space 0x91..0x94) through which AML reads and
// writes the pad registers directly; it is served from the same mapping.
//
// All four communities share one register lock: concurrent accesses to
// different GPIO communities can hang the hardware (erratum CHT34 of the
// Atom Z8000 specification update).
//
// Interrupt lines a community cannot route to its interrupt (8-15 of the
// southwest and north communities) only raise GPEs: they stay unmasked
// for the firmware.  A few Chromebooks with hard-wired interrupt numbers
// keep their whole mask as firmware left it (DMI list below).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2014 Intel Corporation

#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/dmi.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define CHV_INTSTAT 0x300
#define CHV_INTMASK 0x380
#define CHV_FAMILY_PAD_REGS_OFF 0x4400
#define CHV_FAMILY_PAD_REGS_SIZE 0x400
#define CHV_MAX_FAMILY_PAD_GPIO_NO 15
#define CHV_GPIO_REGS_SIZE 8

#define CHV_PADCTRL0 0x000
#define CHV_PADCTRL0_INTSEL_SHIFT 28
#define CHV_PADCTRL0_INTSEL_MASK (0xfu << 28)
#define CHV_PADCTRL0_TERM_UP (1u << 23)
#define CHV_PADCTRL0_TERM_SHIFT 20
#define CHV_PADCTRL0_TERM_MASK (7u << 20)
#define CHV_PADCTRL0_TERM_20K 1u
#define CHV_PADCTRL0_TERM_5K 2u
#define CHV_PADCTRL0_TERM_1K 4u
#define CHV_PADCTRL0_PMODE_SHIFT 16
#define CHV_PADCTRL0_PMODE_MASK (0xfu << 16)
#define CHV_PADCTRL0_GPIOEN (1u << 15)
#define CHV_PADCTRL0_GPIOCFG_SHIFT 8
#define CHV_PADCTRL0_GPIOCFG_MASK (7u << 8)
#define CHV_PADCTRL0_GPIOCFG_GPIO 0u
#define CHV_PADCTRL0_GPIOCFG_GPO 1u
#define CHV_PADCTRL0_GPIOCFG_GPI 2u
#define CHV_PADCTRL0_GPIOCFG_HIZ 3u
#define CHV_PADCTRL0_GPIOTXSTATE (1u << 1)
#define CHV_PADCTRL0_GPIORXSTATE (1u << 0)

#define CHV_PADCTRL1 0x004
#define CHV_PADCTRL1_CFGLOCK (1u << 31)
#define CHV_PADCTRL1_INVRXTX_SHIFT 4
#define CHV_PADCTRL1_INVRXTX_MASK (0xfu << 4)
#define CHV_PADCTRL1_INVRXTX_TXDATA (1u << 7)
#define CHV_PADCTRL1_INVRXTX_RXDATA (1u << 6)
#define CHV_PADCTRL1_INVRXTX_TXENABLE (1u << 5)
#define CHV_PADCTRL1_ODEN (1u << 3)
#define CHV_PADCTRL1_INTWAKECFG_MASK 7u
#define CHV_PADCTRL1_INTWAKECFG_FALLING 1u
#define CHV_PADCTRL1_INTWAKECFG_RISING 2u
#define CHV_PADCTRL1_INTWAKECFG_BOTH 3u
#define CHV_PADCTRL1_INTWAKECFG_LEVEL 4u

#define CHV_NIRQ_LINES 16
#define CHV_INVALID_HWIRQ 0xffffu

struct chv_family {
	uint16_t first, last; /* pad numbers */
};

struct chv_community {
	const char *uid;
	const char *name;
	const struct chv_family *families;
	unsigned int nfamilies;
	unsigned int npins; /* last pad + 1 */
	unsigned int nirqs;
	uint8_t acpi_space_id;
};

static const struct chv_family chv_southwest_fam[] = {
	{ 0, 7 },   { 15, 22 }, { 30, 37 }, { 45, 52 },
	{ 60, 67 }, { 75, 82 }, { 90, 97 },
};
static const struct chv_family chv_north_fam[] = {
	{ 0, 8 }, { 15, 27 }, { 30, 41 }, { 45, 56 }, { 60, 72 },
};
static const struct chv_family chv_east_fam[] = {
	{ 0, 11 },
	{ 15, 26 },
};
static const struct chv_family chv_southeast_fam[] = {
	{ 0, 7 }, { 15, 26 }, { 30, 35 }, { 45, 52 }, { 60, 69 }, { 75, 85 },
};

#define CHV_FAMS(f) (f), (sizeof(f) / sizeof((f)[0]))

static const struct chv_community g_chv_communities[] = {
	{ "1", "Cherry View southwest", CHV_FAMS(chv_southwest_fam), 98, 8,
	  0x91 },
	{ "2", "Cherry View north", CHV_FAMS(chv_north_fam), 73, 8, 0x92 },
	{ "3", "Cherry View east", CHV_FAMS(chv_east_fam), 27, 16, 0x93 },
	{ "4", "Cherry View southeast", CHV_FAMS(chv_southeast_fam), 86, 16,
	  0x94 },
};

struct chv_pad_context {
	uint32_t padctrl0;
	uint32_t padctrl1;
};

struct chv_gpio {
	struct gpio_chip chip;
	const struct chv_community *comm;
	struct acpi_device *adev;
	uint8_t *regs;
	uint64_t length;
	int parent_irq;
	uint16_t intr_lines[CHV_NIRQ_LINES]; /* line -> pad */
	uint32_t saved_intmask; /* over system sleep */
	struct chv_pad_context *ctx; /* per pad number, over system sleep */
	struct list_head node;
	char label[40];
};

/* One lock for the registers of all four communities (erratum CHT34). */
static spinlock_t g_chv_lock = SPINLOCK_INIT("chv_gpio");
static DEFINE_MUTEX(g_chv_list_lock);
static struct list_head g_chv_list = LIST_HEAD_INIT(g_chv_list);

/* Machines that hard-code interrupt numbers and have pads misconfigured
 * to fire: leave the interrupt mask as the firmware set it. */
static const struct dmi_system_id chv_no_valid_mask[] = {
	{
		.ident = "Intel_Strago based Chromebooks (All models)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GOOGLE"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "Intel_Strago"),
		},
	},
	{
		.ident = "HP Chromebook 11 G5 (Setzer)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Setzer"),
		},
	},
	{
		.ident = "Acer Chromebook R11 (Cyan)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GOOGLE"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Cyan"),
		},
	},
	{
		.ident = "Samsung Chromebook 3 (Celes)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GOOGLE"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Celes"),
		},
	},
	{ 0 }
};

static struct chv_gpio *gc_to_chv(struct gpio_chip *gc)
{
	return container_of(gc, struct chv_gpio, chip);
}

static int chv_pad_exists(const struct chv_community *c, unsigned int pad)
{
	for (unsigned int i = 0; i < c->nfamilies; i++)
		if (pad >= c->families[i].first && pad <= c->families[i].last)
			return 1;
	return 0;
}

static uint8_t *chv_padreg(struct chv_gpio *cg, unsigned int pad,
			   unsigned int reg)
{
	if (!chv_pad_exists(cg->comm, pad))
		return NULL;
	return cg->regs + CHV_FAMILY_PAD_REGS_OFF +
	       CHV_FAMILY_PAD_REGS_SIZE * (pad / CHV_MAX_FAMILY_PAD_GPIO_NO) +
	       CHV_GPIO_REGS_SIZE * (pad % CHV_MAX_FAMILY_PAD_GPIO_NO) + reg;
}

static int chv_pad_locked(struct chv_gpio *cg, unsigned int pad)
{
	uint8_t *r = chv_padreg(cg, pad, CHV_PADCTRL1);

	return r && (pinctrl_readl(r) & CHV_PADCTRL1_CFGLOCK);
}

/* Disarm a pad's interrupt detector (unlocked pads).  Some boards need
 * the TX data inverted (an inverting external buffer): the firmware's
 * INVRXTX_TXDATA stays on a pad that already is a GPIO.  g_chv_lock
 * held. */
static void chv_clear_triggering(struct chv_gpio *cg, unsigned int pad)
{
	uint8_t *r0 = chv_padreg(cg, pad, CHV_PADCTRL0);
	uint8_t *r = chv_padreg(cg, pad, CHV_PADCTRL1);
	uint32_t invrxtx_mask = CHV_PADCTRL1_INVRXTX_MASK;
	uint32_t v;

	lockdep_assert_held(&g_chv_lock);
	if (!r || chv_pad_locked(cg, pad))
		return;
	if (pinctrl_readl(r0) & CHV_PADCTRL0_GPIOEN)
		invrxtx_mask &= ~CHV_PADCTRL1_INVRXTX_TXDATA;
	v = pinctrl_readl(r);
	v &= ~CHV_PADCTRL1_INTWAKECFG_MASK;
	v &= ~invrxtx_mask;
	pinctrl_writel(v, r);
}

static void chv_gpio_free(struct gpio_chip *gc, unsigned int offset)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint64_t fl;

	/* A line freed while still an interrupt keeps its trigger. */
	if (gpiochip_line_is_irq(gc, offset))
		return;
	spin_lock_irqsave(&g_chv_lock, &fl);
	chv_clear_triggering(cg, offset);
	spin_unlock_irqrestore(&g_chv_lock, fl);
}

/* ---- GPIO operations ---------------------------------------------------- */

static int chv_gpio_valid(struct gpio_chip *gc, unsigned int offset)
{
	return chv_pad_exists(gc_to_chv(gc)->comm, offset);
}

static int chv_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint8_t *r0 = chv_padreg(cg, offset, CHV_PADCTRL0);
	uint64_t fl;
	uint32_t v;

	if (!r0)
		return -EINVAL;
	spin_lock_irqsave(&g_chv_lock, &fl);
	if (chv_pad_locked(cg, offset)) {
		v = pinctrl_readl(r0);
		spin_unlock_irqrestore(&g_chv_lock, fl);
		/* Locked in a native function: cannot become a GPIO. */
		return (v & CHV_PADCTRL0_GPIOEN) ? 0 : -EBUSY;
	}
	for (unsigned int i = 0; i < CHV_NIRQ_LINES; i++)
		if (cg->intr_lines[i] == offset &&
		    !gpiochip_line_is_irq(gc, offset))
			cg->intr_lines[i] = CHV_INVALID_HWIRQ;
	if (!gpiochip_line_is_irq(gc, offset))
		chv_clear_triggering(cg, offset);
	v = pinctrl_readl(r0);
	/* Hi-Z (both buffers off) becomes an input. */
	if (((v & CHV_PADCTRL0_GPIOCFG_MASK) >> CHV_PADCTRL0_GPIOCFG_SHIFT) ==
	    CHV_PADCTRL0_GPIOCFG_HIZ) {
		v &= ~CHV_PADCTRL0_GPIOCFG_MASK;
		v |= CHV_PADCTRL0_GPIOCFG_GPI << CHV_PADCTRL0_GPIOCFG_SHIFT;
	}
	v |= CHV_PADCTRL0_GPIOEN;
	pinctrl_writel(v, r0);
	spin_unlock_irqrestore(&g_chv_lock, fl);
	return 0;
}

static int chv_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint8_t *r0 = chv_padreg(cg, offset, CHV_PADCTRL0);
	uint32_t cfg;

	if (!r0)
		return -EINVAL;
	cfg = (pinctrl_readl(r0) & CHV_PADCTRL0_GPIOCFG_MASK) >>
	      CHV_PADCTRL0_GPIOCFG_SHIFT;
	return cfg == CHV_PADCTRL0_GPIOCFG_GPO ? GPIO_LINE_DIRECTION_OUT :
						 GPIO_LINE_DIRECTION_IN;
}

static int chv_gpio_set_direction(struct chv_gpio *cg, unsigned int offset,
				  int input)
{
	uint8_t *r0 = chv_padreg(cg, offset, CHV_PADCTRL0);
	uint64_t fl;
	uint32_t v;

	if (!r0)
		return -EINVAL;
	spin_lock_irqsave(&g_chv_lock, &fl);
	v = pinctrl_readl(r0) & ~CHV_PADCTRL0_GPIOCFG_MASK;
	v |= (input ? CHV_PADCTRL0_GPIOCFG_GPI : CHV_PADCTRL0_GPIOCFG_GPO)
	     << CHV_PADCTRL0_GPIOCFG_SHIFT;
	pinctrl_writel(v, r0);
	spin_unlock_irqrestore(&g_chv_lock, fl);
	return 0;
}

static int chv_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	return chv_gpio_set_direction(gc_to_chv(gc), offset, 1);
}

static void chv_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint8_t *r0 = chv_padreg(cg, offset, CHV_PADCTRL0);
	uint64_t fl;
	uint32_t v;

	if (!r0)
		return;
	spin_lock_irqsave(&g_chv_lock, &fl);
	v = pinctrl_readl(r0);
	if (value)
		v |= CHV_PADCTRL0_GPIOTXSTATE;
	else
		v &= ~CHV_PADCTRL0_GPIOTXSTATE;
	pinctrl_writel(v, r0);
	spin_unlock_irqrestore(&g_chv_lock, fl);
}

static int chv_gpio_direction_output(struct gpio_chip *gc,
				     unsigned int offset, int value)
{
	chv_gpio_set(gc, offset, value);
	return chv_gpio_set_direction(gc_to_chv(gc), offset, 0);
}

static int chv_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint8_t *r0 = chv_padreg(cg, offset, CHV_PADCTRL0);
	uint32_t v, cfg;

	if (!r0)
		return -EINVAL;
	v = pinctrl_readl(r0);
	cfg = (v & CHV_PADCTRL0_GPIOCFG_MASK) >> CHV_PADCTRL0_GPIOCFG_SHIFT;
	if (cfg == CHV_PADCTRL0_GPIOCFG_GPO)
		return !!(v & CHV_PADCTRL0_GPIOTXSTATE);
	return !!(v & CHV_PADCTRL0_GPIORXSTATE);
}

static int chv_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
			       enum pin_config_param param, uint32_t arg)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint8_t *r0 = chv_padreg(cg, offset, CHV_PADCTRL0);
	uint8_t *r1 = chv_padreg(cg, offset, CHV_PADCTRL1);
	uint32_t term = 0, v;
	uint64_t fl;

	if (!r0 || !r1)
		return -EINVAL;
	if (chv_pad_locked(cg, offset))
		return -EBUSY;
	if (arg == 1)
		arg = 20000; /* the default strength */
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (arg == 20000)
			term = CHV_PADCTRL0_TERM_20K;
		else if (arg == 5000)
			term = CHV_PADCTRL0_TERM_5K;
		else if (arg == 1000 && param == PIN_CONFIG_BIAS_PULL_UP)
			term = CHV_PADCTRL0_TERM_1K; /* 1k pulls up only */
		else
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		spin_lock_irqsave(&g_chv_lock, &fl);
		v = pinctrl_readl(r1);
		if (param == PIN_CONFIG_DRIVE_OPEN_DRAIN)
			v |= CHV_PADCTRL1_ODEN;
		else
			v &= ~CHV_PADCTRL1_ODEN;
		pinctrl_writel(v, r1);
		spin_unlock_irqrestore(&g_chv_lock, fl);
		return 0;
	default:
		return -ENOTSUPP;
	}
	spin_lock_irqsave(&g_chv_lock, &fl);
	v = pinctrl_readl(r0) & ~(CHV_PADCTRL0_TERM_MASK | CHV_PADCTRL0_TERM_UP);
	if (param != PIN_CONFIG_BIAS_DISABLE)
		v |= term << CHV_PADCTRL0_TERM_SHIFT;
	if (param == PIN_CONFIG_BIAS_PULL_UP)
		v |= CHV_PADCTRL0_TERM_UP;
	pinctrl_writel(v, r0);
	spin_unlock_irqrestore(&g_chv_lock, fl);
	return 0;
}

static void chv_gpio_describe(struct gpio_chip *gc, unsigned int offset,
			      char *buf, size_t len)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint8_t *r0 = chv_padreg(cg, offset, CHV_PADCTRL0);
	uint8_t *r1 = chv_padreg(cg, offset, CHV_PADCTRL1);

	ksnprintf(buf, len, "ctrl0 %08x ctrl1 %08x",
		  r0 ? pinctrl_readl(r0) : 0, r1 ? pinctrl_readl(r1) : 0);
}

/* ---- interrupts ------------------------------------------------------------ */

static int chv_intsel(struct chv_gpio *cg, unsigned int pad)
{
	uint8_t *r0 = chv_padreg(cg, pad, CHV_PADCTRL0);

	if (!r0)
		return -1;
	return (int)((pinctrl_readl(r0) & CHV_PADCTRL0_INTSEL_MASK) >>
		     CHV_PADCTRL0_INTSEL_SHIFT);
}

static void chv_irq_ack(struct gpio_chip *gc, unsigned int hwirq)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	int line = chv_intsel(cg, hwirq);
	uint64_t fl;

	if (line < 0)
		return;
	spin_lock_irqsave(&g_chv_lock, &fl);
	pinctrl_writel(1u << line, cg->regs + CHV_INTSTAT);
	spin_unlock_irqrestore(&g_chv_lock, fl);
}

static void chv_irq_mask_unmask(struct gpio_chip *gc, unsigned int hwirq,
				int mask)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	int line = chv_intsel(cg, hwirq);
	uint64_t fl;
	uint32_t v;

	if (line < 0)
		return;
	spin_lock_irqsave(&g_chv_lock, &fl);
	v = pinctrl_readl(cg->regs + CHV_INTMASK);
	if (mask)
		v &= ~(1u << line);
	else
		v |= 1u << line;
	pinctrl_writel(v, cg->regs + CHV_INTMASK);
	spin_unlock_irqrestore(&g_chv_lock, fl);
}

static void chv_irq_mask(struct gpio_chip *gc, unsigned int hwirq)
{
	chv_irq_mask_unmask(gc, hwirq, 1);
}

static void chv_irq_unmask(struct gpio_chip *gc, unsigned int hwirq)
{
	chv_irq_mask_unmask(gc, hwirq, 0);
}

/* Record that `pad' drives the interrupt line the firmware selected
 * (INTSEL).  A line another pad already drives is a known firmware bug
 * (or a pad the firmware did not expect to be an interrupt): an unlocked
 * pad then moves to a free line, searched from the top since firmware
 * fills them from 0.  g_chv_lock held. */
static int chv_map_line(struct chv_gpio *cg, unsigned int pad)
{
	uint8_t *r0 = chv_padreg(cg, pad, CHV_PADCTRL0);
	int line = chv_intsel(cg, pad);
	int i;

	lockdep_assert_held(&g_chv_lock);
	if (line < 0 || !r0)
		return -EINVAL;
	/* Lines from nirqs on only raise GPEs. */
	if ((unsigned int)line >= cg->comm->nirqs)
		return -EINVAL;
	if (cg->intr_lines[line] == pad)
		return 0;
	if (cg->intr_lines[line] == CHV_INVALID_HWIRQ) {
		cg->intr_lines[line] = (uint16_t)pad;
		return 0;
	}
	kprintf("%s: interrupt line %d is used by both pin %u and pin %u\n",
		cg->label, line, cg->intr_lines[line], pad);
	if (chv_pad_locked(cg, pad))
		return -EBUSY;
	for (i = (int)cg->comm->nirqs - 1; i >= 0; i--)
		if (cg->intr_lines[i] == CHV_INVALID_HWIRQ)
			break;
	if (i < 0)
		return -EBUSY;
	kprintf("%s: changing the interrupt line for pin %u to %d\n",
		cg->label, pad, i);
	pinctrl_writel((pinctrl_readl(r0) & ~CHV_PADCTRL0_INTSEL_MASK) |
			       ((uint32_t)i << CHV_PADCTRL0_INTSEL_SHIFT),
		       r0);
	cg->intr_lines[i] = (uint16_t)pad;
	return 0;
}

static int chv_irq_set_type(struct gpio_chip *gc, unsigned int hwirq,
			    unsigned int type)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint8_t *r1 = chv_padreg(cg, hwirq, CHV_PADCTRL1);
	uint64_t fl;
	uint32_t v;
	int rc;

	if (!r1)
		return -EINVAL;
	spin_lock_irqsave(&g_chv_lock, &fl);
	rc = chv_map_line(cg, hwirq);
	if (rc) {
		spin_unlock_irqrestore(&g_chv_lock, fl);
		return rc;
	}
	/* A locked pad keeps the trigger the firmware programmed; only
	 * the line mapping is ours to record. */
	if (!chv_pad_locked(cg, hwirq)) {
		v = pinctrl_readl(r1);
		v &= ~(CHV_PADCTRL1_INTWAKECFG_MASK | CHV_PADCTRL1_INVRXTX_MASK);
		if (type & IRQ_TYPE_EDGE_BOTH) {
			if ((type & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH)
				v |= CHV_PADCTRL1_INTWAKECFG_BOTH;
			else if (type & IRQ_TYPE_EDGE_RISING)
				v |= CHV_PADCTRL1_INTWAKECFG_RISING;
			else
				v |= CHV_PADCTRL1_INTWAKECFG_FALLING;
		} else if (type & IRQ_TYPE_LEVEL_MASK) {
			v |= CHV_PADCTRL1_INTWAKECFG_LEVEL;
			if (type & IRQ_TYPE_LEVEL_LOW)
				v |= CHV_PADCTRL1_INVRXTX_RXDATA;
		}
		pinctrl_writel(v, r1);
	}
	spin_unlock_irqrestore(&g_chv_lock, fl);
	return 0;
}

static int chv_irq_set_wake(struct gpio_chip *gc, unsigned int hwirq,
			    unsigned int on)
{
	struct chv_gpio *cg = gc_to_chv(gc);

	(void)hwirq;
	return cg->parent_irq > 0 ? irq_set_irq_wake(cg->parent_irq, on) : 0;
}

static int chv_irq_request(struct gpio_chip *gc, unsigned int hwirq)
{
	struct chv_gpio *cg = gc_to_chv(gc);
	uint64_t fl;
	int rc;

	if (!chv_pad_exists(cg->comm, hwirq))
		return -EINVAL;
	/* The line the firmware assigned is mapped now, so an interrupt
	 * used with the firmware's trigger (no set_type) is delivered. */
	spin_lock_irqsave(&g_chv_lock, &fl);
	rc = chv_map_line(cg, hwirq);
	spin_unlock_irqrestore(&g_chv_lock, fl);
	return rc;
}

static const struct gpio_irq_chip_ops g_chv_irq_ops = {
	.irq_mask = chv_irq_mask,
	.irq_unmask = chv_irq_unmask,
	.irq_ack = chv_irq_ack,
	.irq_set_type = chv_irq_set_type,
	.irq_set_wake = chv_irq_set_wake,
	.irq_request = chv_irq_request,
};

static int chv_irq_handler(void *arg)
{
	struct chv_gpio *cg = arg;
	uint32_t pending;

	spin_lock(&g_chv_lock);
	pending = pinctrl_readl(cg->regs + CHV_INTSTAT) &
		  pinctrl_readl(cg->regs + CHV_INTMASK);
	spin_unlock(&g_chv_lock);
	pending &= (1u << cg->comm->nirqs) - 1;
	if (!pending)
		return IRQ_NONE;
	while (pending) {
		unsigned int line = (unsigned int)__builtin_ctz(pending);
		uint16_t pad = cg->intr_lines[line];

		pending &= pending - 1;
		if (pad == CHV_INVALID_HWIRQ) {
			WARN_RATELIMIT(1, "%s: interrupt on unmapped interrupt line %u",
				       cg->label, line);
			/* Some boards expect pin 0 to get it then. */
			if (gpiochip_irq_lookup(&cg->chip, 0) > 0) {
				gpiochip_handle_irq(&cg->chip, 0);
				continue;
			}
			/* Enabled by nobody we know: silence it. */
			spin_lock(&g_chv_lock);
			pinctrl_writel(pinctrl_readl(cg->regs + CHV_INTMASK) &
					       ~(1u << line),
				       cg->regs + CHV_INTMASK);
			pinctrl_writel(1u << line, cg->regs + CHV_INTSTAT);
			spin_unlock(&g_chv_lock);
			continue;
		}
		gpiochip_handle_irq(&cg->chip, pad);
	}
	return IRQ_HANDLED;
}

/* ---- probe -------------------------------------------------------------- */

int chv_gpio_setup(struct chv_gpio *cg, const struct chv_community *comm,
		   uint8_t *regs, uint64_t length);
int chv_gpio_setup(struct chv_gpio *cg, const struct chv_community *comm,
		   uint8_t *regs, uint64_t length)
{
	uint64_t fl;

	if (!cg || !comm || !regs)
		return -EINVAL;
	/* The pad block of the last family has to be inside the window. */
	if (length && CHV_FAMILY_PAD_REGS_OFF +
				      CHV_FAMILY_PAD_REGS_SIZE *
					      ((comm->npins - 1) /
					       CHV_MAX_FAMILY_PAD_GPIO_NO + 1) >
			      length)
		return -ENODEV;
	cg->comm = comm;
	cg->regs = regs;
	cg->length = length;
	for (unsigned int i = 0; i < CHV_NIRQ_LINES; i++)
		cg->intr_lines[i] = CHV_INVALID_HWIRQ;
	if (!cg->label[0])
		ksnprintf(cg->label, sizeof(cg->label), "INT33FF:%s (%s)",
			  comm->uid, comm->name);
	cg->chip.label = cg->label;
	cg->chip.ngpio = comm->npins;
	cg->chip.data = cg;
	cg->chip.request = chv_gpio_request;
	cg->chip.free = chv_gpio_free;
	cg->chip.get_direction = chv_gpio_get_direction;
	cg->chip.direction_input = chv_gpio_direction_input;
	cg->chip.direction_output = chv_gpio_direction_output;
	cg->chip.get = chv_gpio_get;
	cg->chip.set = chv_gpio_set;
	cg->chip.set_config = chv_gpio_set_config;
	cg->chip.valid = chv_gpio_valid;
	cg->chip.describe = chv_gpio_describe;
	cg->chip.irq = &g_chv_irq_ops;
	INIT_LIST_HEAD(&cg->node);
	/* Nothing fires before a driver asks: mask every line the
	 * community routes to its interrupt, leave the GPE-only lines on
	 * -- except on the machines listed, whose mask stays as is. */
	spin_lock_irqsave(&g_chv_lock, &fl);
	if (!dmi_first_match(chv_no_valid_mask))
		pinctrl_writel(~((1u << comm->nirqs) - 1u),
			       cg->regs + CHV_INTMASK);
	pinctrl_writel(0xffffu, cg->regs + CHV_INTSTAT);
	spin_unlock_irqrestore(&g_chv_lock, fl);
	return 0;
}

/* System sleep: the interrupt mask and the configuration of every
 * unlocked pad. */
void chv_gpio_suspend(struct chv_gpio *cg);
void chv_gpio_suspend(struct chv_gpio *cg)
{
	uint64_t fl;

	if (!cg->ctx)
		return;
	spin_lock_irqsave(&g_chv_lock, &fl);
	cg->saved_intmask = pinctrl_readl(cg->regs + CHV_INTMASK);
	for (unsigned int pad = 0; pad < cg->comm->npins; pad++) {
		uint8_t *r0 = chv_padreg(cg, pad, CHV_PADCTRL0);
		uint8_t *r1 = chv_padreg(cg, pad, CHV_PADCTRL1);

		if (!r0 || chv_pad_locked(cg, pad))
			continue;
		cg->ctx[pad].padctrl0 =
			pinctrl_readl(r0) & ~CHV_PADCTRL0_GPIORXSTATE;
		cg->ctx[pad].padctrl1 = pinctrl_readl(r1);
	}
	spin_unlock_irqrestore(&g_chv_lock, fl);
}

void chv_gpio_resume(struct chv_gpio *cg);
void chv_gpio_resume(struct chv_gpio *cg)
{
	uint64_t fl;

	if (!cg->ctx)
		return;
	spin_lock_irqsave(&g_chv_lock, &fl);
	/* Everything masked while the pads come back: the firmware's
	 * state on resume is unknown. */
	pinctrl_writel(0, cg->regs + CHV_INTMASK);
	for (unsigned int pad = 0; pad < cg->comm->npins; pad++) {
		uint8_t *r0 = chv_padreg(cg, pad, CHV_PADCTRL0);
		uint8_t *r1 = chv_padreg(cg, pad, CHV_PADCTRL1);
		uint32_t v;

		if (!r0 || chv_pad_locked(cg, pad))
			continue;
		v = pinctrl_readl(r0) & ~CHV_PADCTRL0_GPIORXSTATE;
		if (v != cg->ctx[pad].padctrl0)
			pinctrl_writel(cg->ctx[pad].padctrl0, r0);
		if (pinctrl_readl(r1) != cg->ctx[pad].padctrl1)
			pinctrl_writel(cg->ctx[pad].padctrl1, r1);
	}
	pinctrl_writel(0xffffu, cg->regs + CHV_INTSTAT);
	pinctrl_writel(cg->saved_intmask, cg->regs + CHV_INTMASK);
	spin_unlock_irqrestore(&g_chv_lock, fl);
}

void chv_pinctrl_suspend_all(void);
void chv_pinctrl_suspend_all(void)
{
	mutex_lock(&g_chv_list_lock);
	for (struct list_head *p = g_chv_list.next; p != &g_chv_list;
	     p = p->next)
		chv_gpio_suspend(container_of(p, struct chv_gpio, node));
	mutex_unlock(&g_chv_list_lock);
}

void chv_pinctrl_resume_all(void);
void chv_pinctrl_resume_all(void)
{
	mutex_lock(&g_chv_list_lock);
	for (struct list_head *p = g_chv_list.next; p != &g_chv_list;
	     p = p->next)
		chv_gpio_resume(container_of(p, struct chv_gpio, node));
	mutex_unlock(&g_chv_list_lock);
}

const struct chv_community *chv_community_by_uid(const char *uid);
const struct chv_community *chv_community_by_uid(const char *uid)
{
	for (unsigned int i = 0;
	     i < sizeof(g_chv_communities) / sizeof(g_chv_communities[0]); i++)
		if (uid && !kstrcmp(uid, g_chv_communities[i].uid))
			return &g_chv_communities[i];
	return NULL;
}

static int chv_probe_cb(struct acpi_device *adev,
			const struct acpi_device_id *id, void *ctx)
{
	const struct chv_community *comm;
	const struct acpi_res *mem, *irqres;
	struct acpi_res_list res;
	struct chv_gpio *cg;
	uint64_t virt, pages, len;
	int irq, rc;

	(void)id;
	(void)ctx;
	comm = chv_community_by_uid(adev->has_uid ? adev->uid : NULL);
	if (!comm || gpiochip_find_by_acpi_handle(adev->handle))
		return 0;
	if (acpi_dev_get_resources(adev, &res))
		return 0;
	mem = acpi_res_find(&res, ACPI_RES_MEM, 0);
	if (!mem) {
		acpi_dev_free_resource_list(&res);
		return 0;
	}
	len = mem->mem.length ? mem->mem.length : 0x8000;
	pages = ((mem->mem.start & 0xfffull) + len + 0xfffull) >> 12;
	virt = mm_map_device_mmio(mem->mem.start, (size_t)pages);
	cg = virt ? kcalloc(1, sizeof(*cg)) : NULL;
	if (!cg) {
		acpi_dev_free_resource_list(&res);
		return 0;
	}
	rc = chv_gpio_setup(cg, comm, (uint8_t *)virt, len);
	if (rc) {
		kprintf("%s: register window too small\n", comm->name);
		kfree(cg);
		acpi_dev_free_resource_list(&res);
		return 0;
	}
	cg->adev = acpi_dev_get(adev);
	cg->chip.acpi_handle = adev->handle;
	irqres = acpi_res_find(&res, ACPI_RES_IRQ, 0);
	irq = -ENOENT;
	if (irqres) {
		irq = irq_alloc_desc_gsi(NULL, cg, (int)irqres->irq.gsi,
					 irqres->irq.polarity ==
						 ACPI_RES_ACTIVE_LOW,
					 irqres->irq.triggering ==
						 ACPI_RES_LEVEL);
		if (irq > 0 && request_irq(irq, chv_irq_handler, IRQF_SHARED,
					   cg->label, cg) != 0) {
			irq_free_desc(irq);
			irq = -EBUSY;
		}
	}
	if (irq > 0)
		cg->parent_irq = irq;
	else
		cg->chip.irq = NULL;
	if (gpiochip_add(&cg->chip) != 0) {
		if (cg->parent_irq > 0) {
			(void)free_irq(cg->parent_irq, cg);
			irq_free_desc(cg->parent_irq);
		}
		acpi_dev_put(cg->adev);
		kfree(cg);
		acpi_dev_free_resource_list(&res);
		return 0;
	}
	/* AML reaches the pad registers through the community's own
	 * address space. */
	rc = acpi_install_mmio_region_handler(adev->handle,
					      comm->acpi_space_id, cg->regs,
					      cg->length, &g_chv_lock);
	if (rc)
		kprintf("%s: region %#x handler not installed (%d)\n",
			cg->label, comm->acpi_space_id, rc);
	cg->ctx = kcalloc(comm->npins, sizeof(*cg->ctx));
	mutex_lock(&g_chv_list_lock);
	list_add_tail(&cg->node, &g_chv_list);
	mutex_unlock(&g_chv_list_lock);
	kprintf("%s: %u GPIOs%s\n", cg->label, comm->npins,
		cg->parent_irq > 0 ? ", interrupts" : "");
	acpi_dev_free_resource_list(&res);
	return 0;
}

void chv_pinctrl_init(void)
{
	static const struct acpi_device_id ids[] = {
		{ "INT33FF", 0 },
		{ "", 0 },
	};

	might_sleep();
	if (!acpi_dev_ready())
		return;
	{
		struct acpi_device *devs[8];
		int n = acpi_dev_collect_matches(ids, 1, devs, 8);

		for (int i = 0; i < n; i++) {
			(void)chv_probe_cb(devs[i], NULL, NULL);
			acpi_dev_put(devs[i]);
		}
	}
}
