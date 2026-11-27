// LikeOS pinctrl - Intel PCH/SoC GPIO controller core
//
// The register design is described in pinctrl-intel.h; the per-SoC
// tables live in pinctrl-<soc>.c.  This file probes a controller found in
// the ACPI namespace, maps its communities, and exposes it as a gpio_chip
// with interrupts.
//
// What the driver does to pads -- and what it leaves alone:
//
//   * pads owned by someone else (PAD_OWN != host: the ME, the ISH, a
//     sensor hub) are never requested or used: requesting them fails with
//     -EBUSY and so does mapping their interrupt;
//   * pads in ACPI mode (HOSTSW_OWN = 0) signal through GPE/SCI and never
//     set GPI_IS: they may be read and driven as GPIOs, but refuse to be
//     an interrupt (-EPERM).  The driver does not take a pad away from
//     the firmware by flipping HOSTSW_OWN;
//   * locked pads (PADCFGLOCK) keep their configuration: requests
//     succeed without touching the pad, pull and debounce settings are
//     refused (-ENOTSUPP, which the GPIO core treats as "not available"),
//     and a requested trigger that differs from the firmware's is
//     reported rather than written;
//   * requesting a pad switches it to GPIO mode only when it is in a
//     native function and unlocked (input, IOxAPIC/SCI/SMI/NMI routes
//     off); a pad already in GPIO mode keeps the firmware's setup, so no
//     glitch on lines in use;
//   * the trigger programming writes RXEVCFG and RXINV (level with RXINV
//     for active-low, edge with RXINV for falling, both edges) and puts
//     the pad into GPIO input mode with its direct routes off;
//   * masking clears the pad's GPI_IS bit before changing GPI_IE;
//   * at probe every GPI_IE is cleared and every GPI_IS acknowledged, so
//     nothing left by the firmware fires before a driver asked for it.
//
// The parent interrupt (the controller's own Interrupt resource, an I/O
// APIC line often shared by several controllers) is demultiplexed here:
// per community and group, pending = GPI_IS & GPI_IE, and every pending
// pad's GPIO number goes to gpiochip_handle_irq().
//
// System sleep: intel_pinctrl_suspend() saves the configuration of the
// pads in use (requested, used as interrupt, or set up by firmware as a
// direct IOxAPIC interrupt input) and the GPI_IE / HOSTSW_OWN registers;
// intel_pinctrl_resume() masks everything and writes back what firmware
// changed, then lets gpiolib put each mapped line's mask back as it holds
// it (gpiochip_irq_resync_masks: the saved enables can be older than a
// mask or unmask done meanwhile).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2015 Intel Corporation

#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* Offset from regs */
#define REVID 0x000
#define REVID_SHIFT 16
#define REVID_MASK 0xffff0000u

#define CAPLIST 0x004
#define CAPLIST_ID_SHIFT 16
#define CAPLIST_ID_MASK 0x00ff0000u
#define CAPLIST_ID_GPIO_HW_INFO 1
#define CAPLIST_ID_PWM 2
#define CAPLIST_ID_BLINK 3
#define CAPLIST_ID_EXP 4
#define CAPLIST_NEXT_SHIFT 0
#define CAPLIST_NEXT_MASK 0x0000ffffu
#define CAPLIST_MAX_WALK 16

#define PADBAR 0x00c

#define PADOWN_BITS 4
#define PADOWN_SHIFT(p) ((p) % 8 * PADOWN_BITS)
#define PADOWN_MASK(p) (0xfu << PADOWN_SHIFT(p))
#define PADOWN_GPP(p) ((p) / 8)

/* Offset from pad_regs */
#define PADCFG0 INTEL_PADCFG0
#define PADCFG0_RXEVCFG_MASK (3u << 25)
#define PADCFG0_RXEVCFG_LEVEL (0u << 25)
#define PADCFG0_RXEVCFG_EDGE (1u << 25)
#define PADCFG0_RXEVCFG_DISABLED (2u << 25)
#define PADCFG0_RXEVCFG_EDGE_BOTH (3u << 25)
#define PADCFG0_PREGFRXSEL (1u << 24)
#define PADCFG0_RXINV (1u << 23)
#define PADCFG0_GPIROUTIOXAPIC (1u << 20)
#define PADCFG0_GPIROUTSCI (1u << 19)
#define PADCFG0_GPIROUTSMI (1u << 18)
#define PADCFG0_GPIROUTNMI (1u << 17)
#define PADCFG0_PMODE_SHIFT 10
#define PADCFG0_PMODE_MASK (0xfu << 10)
#define PADCFG0_PMODE_GPIO 0u
#define PADCFG0_GPIODIS_SHIFT 8
#define PADCFG0_GPIODIS_MASK (3u << 8)
#define PADCFG0_GPIODIS_NONE 0u
#define PADCFG0_GPIODIS_OUTPUT 1u
#define PADCFG0_GPIODIS_INPUT 2u
#define PADCFG0_GPIODIS_FULL 3u
#define PADCFG0_GPIORXDIS (1u << 9)
#define PADCFG0_GPIOTXDIS (1u << 8)
#define PADCFG0_GPIORXSTATE (1u << 1)
#define PADCFG0_GPIOTXSTATE (1u << 0)

#define PADCFG1 INTEL_PADCFG1
#define PADCFG1_TERM_UP (1u << 13)
#define PADCFG1_TERM_SHIFT 10
#define PADCFG1_TERM_MASK (7u << 10)
/*
 * Bit 0  Bit 1  Bit 2			Value, Ohms
 *
 *   0      0      0			-
 *   0      0      1			20000
 *   0      1      0			5000
 *   0      1      1			~4000
 *   1      0      0			1000 (if supported)
 *   1      0      1			~952 (if supported)
 *   1      1      0			~833 (if supported)
 *   1      1      1			~800 (if supported)
 */
#define PADCFG1_TERM_20K (1u << 2)
#define PADCFG1_TERM_5K (1u << 1)
#define PADCFG1_TERM_4K ((1u << 2) | (1u << 1))
#define PADCFG1_TERM_1K (1u << 0)
#define PADCFG1_TERM_952 ((1u << 2) | (1u << 0))
#define PADCFG1_TERM_833 ((1u << 1) | (1u << 0))
#define PADCFG1_TERM_800 ((1u << 2) | (1u << 1) | (1u << 0))

#define PADCFG2 INTEL_PADCFG2
#define PADCFG2_DEBOUNCE_SHIFT 1
#define PADCFG2_DEBOUNCE_MASK (0xfu << 1)
#define PADCFG2_DEBEN (1u << 0)

#define DEBOUNCE_PERIOD_NSEC 31250u

/* Possible pad physical connections */
#define PAD_CONNECT_NONE 0
#define PAD_CONNECT_INPUT 1
#define PAD_CONNECT_OUTPUT 2
#define PAD_CONNECT_FULL (PAD_CONNECT_INPUT | PAD_CONNECT_OUTPUT)

/* A community window when the resource gives no length. */
#define INTEL_DEFAULT_WINDOW 0x10000u
#define INTEL_MAX_WINDOWS 8

static DEFINE_MUTEX(g_intel_list_lock);
static struct list_head g_intel_list = LIST_HEAD_INIT(g_intel_list);

static struct intel_pinctrl *gc_to_pctrl(struct gpio_chip *gc)
{
	return container_of(gc, struct intel_pinctrl, chip);
}

static const char *pctrl_name(const struct intel_pinctrl *pctrl)
{
	return pctrl->label[0] ? pctrl->label : "intel-gpio";
}

#define pin_to_padno(c, p) ((p) - (c)->pin_base)
#define padgroup_offset(g, p) ((p) - (g)->base)

/* ==========================================================================
 * Pin / group / register lookup
 * ========================================================================== */

const struct intel_community *
intel_get_community(const struct intel_pinctrl *pctrl, unsigned int pin)
{
	for (size_t i = 0; i < pctrl->ncommunities; i++) {
		const struct intel_community *c = &pctrl->communities[i];

		if (pin >= c->pin_base && pin < c->pin_base + c->npins)
			return c;
	}
	return NULL;
}

static const struct intel_padgroup *
intel_community_get_padgroup(const struct intel_community *c, unsigned int pin)
{
	for (size_t i = 0; i < c->ngpps; i++) {
		const struct intel_padgroup *g = &c->gpps[i];

		if (pin >= g->base && pin < g->base + g->size)
			return g;
	}
	return NULL;
}

uint8_t *intel_get_padcfg(const struct intel_pinctrl *pctrl, unsigned int pin,
			  unsigned int reg)
{
	const struct intel_community *c = intel_get_community(pctrl, pin);
	unsigned int padno;
	size_t nregs;

	if (!c)
		return NULL;
	padno = pin_to_padno(c, pin);
	nregs = (c->features & PINCTRL_FEATURE_DEBOUNCE) ? 4 : 2;
	if (reg >= nregs * 4)
		return NULL;
	return c->pad_regs + reg + padno * nregs * 4;
}

int intel_pad_owned_by_host(const struct intel_pinctrl *pctrl,
			    unsigned int pin)
{
	const struct intel_community *c = intel_get_community(pctrl, pin);
	const struct intel_padgroup *g;
	unsigned int gpp, offset, gpp_offset;
	uint8_t *padown;

	if (!c)
		return 0;
	/* No PAD_OWN registers: the host owns every pad. */
	if (!c->padown_offset)
		return 1;
	padown = c->regs + c->padown_offset;
	/* Newer generations: one register per pad, 3 owner bits. */
	if (c->features & PINCTRL_FEATURE_3BIT_PAD_OWN)
		return !(pinctrl_readl(padown + pin_to_padno(c, pin) * 4) & 7);
	g = intel_community_get_padgroup(c, pin);
	if (!g)
		return 0;
	gpp_offset = padgroup_offset(g, pin);
	gpp = PADOWN_GPP(gpp_offset);
	offset = g->padown_num * 4 + gpp * 4;
	return !(pinctrl_readl(padown + offset) & PADOWN_MASK(gpp_offset));
}

int intel_pad_acpi_mode(const struct intel_pinctrl *pctrl, unsigned int pin)
{
	const struct intel_community *c = intel_get_community(pctrl, pin);
	const struct intel_padgroup *g;
	unsigned int gpp_offset;

	if (!c)
		return 1;
	if (!c->hostown_offset)
		return 0;
	g = intel_community_get_padgroup(c, pin);
	if (!g)
		return 1;
	gpp_offset = padgroup_offset(g, pin);
	return !(pinctrl_readl(c->regs + c->hostown_offset + g->reg_num * 4) &
		 (1u << gpp_offset));
}

int intel_pad_locked(const struct intel_pinctrl *pctrl, unsigned int pin)
{
	const struct intel_community *c = intel_get_community(pctrl, pin);
	const struct intel_padgroup *g;
	unsigned int gpp_offset, offset;
	int ret = INTEL_PAD_UNLOCKED;

	if (!c)
		return INTEL_PAD_LOCKED_FULL;
	if (!c->padcfglock_offset)
		return INTEL_PAD_UNLOCKED;
	g = intel_community_get_padgroup(c, pin);
	if (!g)
		return INTEL_PAD_LOCKED_FULL;
	gpp_offset = padgroup_offset(g, pin);
	/* PADCFGLOCK and PADCFGLOCKTX, one pair per group: both clear
	 * means unlocked, anything else (partly) locked. */
	offset = c->padcfglock_offset + 0 + g->reg_num * 8;
	if (pinctrl_readl(c->regs + offset) & (1u << gpp_offset))
		ret |= INTEL_PAD_LOCKED;
	offset = c->padcfglock_offset + 4 + g->reg_num * 8;
	if (pinctrl_readl(c->regs + offset) & (1u << gpp_offset))
		ret |= INTEL_PAD_LOCKED_TX;
	return ret;
}

static int intel_pad_is_unlocked(const struct intel_pinctrl *pctrl,
				 unsigned int pin)
{
	return (intel_pad_locked(pctrl, pin) & INTEL_PAD_LOCKED) ==
	       INTEL_PAD_UNLOCKED;
}

static int intel_pad_usable(const struct intel_pinctrl *pctrl,
			    unsigned int pin)
{
	return intel_pad_owned_by_host(pctrl, pin) &&
	       intel_pad_is_unlocked(pctrl, pin);
}

int intel_gpio_to_pin(const struct intel_pinctrl *pctrl, unsigned int offset,
		      const struct intel_community **community,
		      const struct intel_padgroup **padgrp)
{
	for (size_t i = 0; i < pctrl->ncommunities; i++) {
		const struct intel_community *c = &pctrl->communities[i];

		for (size_t j = 0; j < c->ngpps; j++) {
			const struct intel_padgroup *g = &c->gpps[j];

			if (g->gpio_base == INTEL_GPIO_BASE_NOMAP)
				continue;
			if (offset >= (unsigned int)g->gpio_base &&
			    offset < (unsigned int)g->gpio_base + g->size) {
				if (community)
					*community = c;
				if (padgrp)
					*padgrp = g;
				return (int)(g->base + offset -
					     (unsigned int)g->gpio_base);
			}
		}
	}
	return -EINVAL;
}

int intel_pin_to_gpio(const struct intel_pinctrl *pctrl, unsigned int pin)
{
	const struct intel_community *c = intel_get_community(pctrl, pin);
	const struct intel_padgroup *g;

	if (!c)
		return -EINVAL;
	g = intel_community_get_padgroup(c, pin);
	if (!g || g->gpio_base == INTEL_GPIO_BASE_NOMAP)
		return -EINVAL;
	return (int)(pin - g->base) + g->gpio_base;
}

/* The name of a pin (the tables number pins densely from 0, so the
 * pin's own entry is usually at its index). */
static const char *intel_pin_name(const struct intel_pinctrl *pctrl,
				  unsigned int pin)
{
	const struct intel_pinctrl_soc_data *soc = pctrl->soc;

	if (!soc->pins)
		return NULL;
	if (pin < soc->npins && soc->pins[pin].number == pin)
		return soc->pins[pin].name;
	for (size_t i = 0; i < soc->npins; i++)
		if (soc->pins[i].number == pin)
			return soc->pins[i].name;
	return NULL;
}

/* ==========================================================================
 * Pad mode and direction
 * ========================================================================== */

static int __intel_gpio_get_direction(uint32_t value)
{
	switch ((value & PADCFG0_GPIODIS_MASK) >> PADCFG0_GPIODIS_SHIFT) {
	case PADCFG0_GPIODIS_FULL:
		return PAD_CONNECT_NONE;
	case PADCFG0_GPIODIS_OUTPUT:
		return PAD_CONNECT_INPUT;
	case PADCFG0_GPIODIS_INPUT:
		return PAD_CONNECT_OUTPUT;
	case PADCFG0_GPIODIS_NONE:
	default:
		return PAD_CONNECT_FULL;
	}
}

static uint32_t __intel_gpio_set_direction(uint32_t value, int input,
					   int output)
{
	if (input)
		value &= ~PADCFG0_GPIORXDIS;
	else
		value |= PADCFG0_GPIORXDIS;
	if (output)
		value &= ~PADCFG0_GPIOTXDIS;
	else
		value |= PADCFG0_GPIOTXDIS;
	return value;
}

static unsigned int __intel_gpio_get_gpio_mode(uint32_t value)
{
	return (value & PADCFG0_PMODE_MASK) >> PADCFG0_PMODE_SHIFT;
}

/* GPIO mode, input, every route to the platform (IOxAPIC, SCI, SMI,
 * NMI) off.  pctrl->lock held. */
static void intel_gpio_set_gpio_mode(struct intel_pinctrl *pctrl,
				     uint8_t *padcfg0)
{
	uint32_t value = pinctrl_readl(padcfg0);

	lockdep_assert_held(&pctrl->lock);
	value &= ~PADCFG0_PMODE_MASK;
	value |= PADCFG0_PMODE_GPIO << PADCFG0_PMODE_SHIFT;
	value = __intel_gpio_set_direction(value, 1, 0);
	value &= ~(PADCFG0_GPIROUTIOXAPIC | PADCFG0_GPIROUTSCI);
	value &= ~(PADCFG0_GPIROUTSMI | PADCFG0_GPIROUTNMI);
	pinctrl_writel(value, padcfg0);
}

/* A firmware-made direct interrupt: GPIO input routed to the IOxAPIC. */
static int __intel_gpio_is_direct_irq(uint32_t value)
{
	return (value & PADCFG0_GPIROUTIOXAPIC) &&
	       __intel_gpio_get_direction(value) == PAD_CONNECT_INPUT &&
	       __intel_gpio_get_gpio_mode(value) == PADCFG0_PMODE_GPIO;
}

/* ==========================================================================
 * GPIO operations (offset = GPIO number)
 * ========================================================================== */

static int intel_gpio_valid(struct gpio_chip *gc, unsigned int offset)
{
	return intel_gpio_to_pin(gc_to_pctrl(gc), offset, NULL, NULL) >= 0;
}

static int intel_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	int pin = intel_gpio_to_pin(pctrl, offset, NULL, NULL);
	uint8_t *padcfg0;
	uint64_t fl;
	int rc = 0;

	if (pin < 0)
		return -EINVAL;
	padcfg0 = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG0);
	if (!padcfg0)
		return -EINVAL;
	spin_lock_irqsave(&pctrl->lock, &fl);
	if (!intel_pad_owned_by_host(pctrl, (unsigned int)pin)) {
		rc = -EBUSY;
	} else if (intel_pad_is_unlocked(pctrl, (unsigned int)pin) &&
		   __intel_gpio_get_gpio_mode(pinctrl_readl(padcfg0)) !=
			   PADCFG0_PMODE_GPIO) {
		/* A pad already in GPIO mode was set up by the firmware
		 * for its user: it is left alone, so the line does not
		 * glitch.  A pad in a native function becomes an input. */
		intel_gpio_set_gpio_mode(pctrl, padcfg0);
	}
	spin_unlock_irqrestore(&pctrl->lock, fl);
	if (rc)
		kprintf("%s: pin %d (GPIO %u) is not owned by the host\n",
			pctrl_name(pctrl), pin, offset);
	return rc;
}

static int intel_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	int pin = intel_gpio_to_pin(pctrl, offset, NULL, NULL);
	uint8_t *reg;
	uint32_t padcfg0;
	uint64_t fl;

	if (pin < 0)
		return -EINVAL;
	reg = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG0);
	if (!reg)
		return -EINVAL;
	spin_lock_irqsave(&pctrl->lock, &fl);
	padcfg0 = pinctrl_readl(reg);
	spin_unlock_irqrestore(&pctrl->lock, fl);
	if (padcfg0 & PADCFG0_PMODE_MASK)
		return -EINVAL; /* a native function, not a GPIO */
	if (__intel_gpio_get_direction(padcfg0) & PAD_CONNECT_OUTPUT)
		return GPIO_LINE_DIRECTION_OUT;
	return GPIO_LINE_DIRECTION_IN;
}

/* Input: RX on, TX off; output: TX on, RX off.  Written as asked; on a
 * locked pad the hardware keeps its configuration. */
static int intel_gpio_set_direction(struct intel_pinctrl *pctrl,
				    unsigned int offset, int input)
{
	int pin = intel_gpio_to_pin(pctrl, offset, NULL, NULL);
	uint8_t *reg;
	uint32_t value;
	uint64_t fl;

	if (pin < 0)
		return -EINVAL;
	reg = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG0);
	if (!reg)
		return -EINVAL;
	spin_lock_irqsave(&pctrl->lock, &fl);
	value = pinctrl_readl(reg);
	value = __intel_gpio_set_direction(value, input, !input);
	pinctrl_writel(value, reg);
	spin_unlock_irqrestore(&pctrl->lock, fl);
	return 0;
}

static int intel_gpio_direction_input(struct gpio_chip *gc,
				      unsigned int offset)
{
	return intel_gpio_set_direction(gc_to_pctrl(gc), offset, 1);
}

static int intel_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	int pin = intel_gpio_to_pin(pctrl, offset, NULL, NULL);
	uint8_t *reg;
	uint32_t padcfg0;

	if (pin < 0)
		return -EINVAL;
	reg = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG0);
	if (!reg)
		return -EINVAL;
	padcfg0 = pinctrl_readl(reg);
	if (__intel_gpio_get_direction(padcfg0) & PAD_CONNECT_OUTPUT)
		return !!(padcfg0 & PADCFG0_GPIOTXSTATE);
	return !!(padcfg0 & PADCFG0_GPIORXSTATE);
}

static void intel_gpio_set(struct gpio_chip *gc, unsigned int offset,
			   int value)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	int pin = intel_gpio_to_pin(pctrl, offset, NULL, NULL);
	uint8_t *reg;
	uint32_t padcfg0;
	uint64_t fl;

	if (pin < 0)
		return;
	reg = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG0);
	if (!reg)
		return;
	spin_lock_irqsave(&pctrl->lock, &fl);
	padcfg0 = pinctrl_readl(reg);
	if (value)
		padcfg0 |= PADCFG0_GPIOTXSTATE;
	else
		padcfg0 &= ~PADCFG0_GPIOTXSTATE;
	pinctrl_writel(padcfg0, reg);
	spin_unlock_irqrestore(&pctrl->lock, fl);
}

static int intel_gpio_direction_output(struct gpio_chip *gc,
				       unsigned int offset, int value)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);

	if (intel_gpio_to_pin(pctrl, offset, NULL, NULL) < 0)
		return -EINVAL;
	intel_gpio_set(gc, offset, value);
	return intel_gpio_set_direction(pctrl, offset, 0);
}

/* ==========================================================================
 * Pin configuration
 * ========================================================================== */

static int intel_config_set_pull(struct intel_pinctrl *pctrl, unsigned int pin,
				 enum pin_config_param param, uint32_t arg)
{
	const struct intel_community *c = intel_get_community(pctrl, pin);
	uint8_t *padcfg1 = intel_get_padcfg(pctrl, pin, PADCFG1);
	uint32_t term = 0, up = 0, value;
	uint64_t fl;

	if (!c || !padcfg1)
		return -EINVAL;
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		switch (arg) {
		case 20000:
			term = PADCFG1_TERM_20K;
			break;
		case 1: /* no strength given: the default */
		case 5000:
			term = PADCFG1_TERM_5K;
			break;
		case 4000:
			term = PADCFG1_TERM_4K;
			break;
		case 1000:
			term = PADCFG1_TERM_1K;
			break;
		case 833:
			term = PADCFG1_TERM_833;
			break;
		default:
			return -EINVAL;
		}
		up = PADCFG1_TERM_UP;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		switch (arg) {
		case 20000:
			term = PADCFG1_TERM_20K;
			break;
		case 1: /* no strength given: the default */
		case 5000:
			term = PADCFG1_TERM_5K;
			break;
		case 4000:
			term = PADCFG1_TERM_4K;
			break;
		case 1000:
			if (!(c->features & PINCTRL_FEATURE_1K_PD))
				return -EINVAL;
			term = PADCFG1_TERM_1K;
			break;
		case 833:
			if (!(c->features & PINCTRL_FEATURE_1K_PD))
				return -EINVAL;
			term = PADCFG1_TERM_833;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	spin_lock_irqsave(&pctrl->lock, &fl);
	value = pinctrl_readl(padcfg1);
	value = (value & ~PADCFG1_TERM_MASK) | (term << PADCFG1_TERM_SHIFT);
	value = (value & ~PADCFG1_TERM_UP) | up;
	pinctrl_writel(value, padcfg1);
	spin_unlock_irqrestore(&pctrl->lock, fl);
	return 0;
}

static int intel_gpio_set_high_impedance(struct intel_pinctrl *pctrl,
					 unsigned int pin)
{
	uint8_t *padcfg0 = intel_get_padcfg(pctrl, pin, PADCFG0);
	uint32_t value;
	uint64_t fl;

	if (!padcfg0)
		return -EINVAL;
	spin_lock_irqsave(&pctrl->lock, &fl);
	value = pinctrl_readl(padcfg0);
	value = __intel_gpio_set_direction(value, 0, 0);
	pinctrl_writel(value, padcfg0);
	spin_unlock_irqrestore(&pctrl->lock, fl);
	return 0;
}

/* ceil(log2(n)), 0 for n <= 1 */
static unsigned int order_base_2_u64(uint64_t n)
{
	unsigned int v = 0;

	while (v < 64 && (1ull << v) < n)
		v++;
	return v;
}

static int intel_config_set_debounce(struct intel_pinctrl *pctrl,
				     unsigned int pin, uint32_t debounce)
{
	uint8_t *padcfg0, *padcfg2;
	uint32_t value0, value2;
	unsigned int v;
	uint64_t fl;

	if (debounce) {
		/* Period = 2^v * 31.25 us, v = 3..15. */
		v = order_base_2_u64((uint64_t)debounce * 1000u /
				     DEBOUNCE_PERIOD_NSEC);
		if (v < 3 || v > 15)
			return -EINVAL;
	} else {
		v = 0;
	}
	padcfg2 = intel_get_padcfg(pctrl, pin, PADCFG2);
	if (!padcfg2)
		return -ENOTSUPP; /* no PADCFG2 before revision 0x092 */
	padcfg0 = intel_get_padcfg(pctrl, pin, PADCFG0);
	spin_lock_irqsave(&pctrl->lock, &fl);
	value0 = pinctrl_readl(padcfg0);
	value2 = pinctrl_readl(padcfg2);
	value2 = (value2 & ~PADCFG2_DEBOUNCE_MASK) |
		 (v << PADCFG2_DEBOUNCE_SHIFT);
	if (v) {
		/* Glitch filter and debouncer on */
		value0 |= PADCFG0_PREGFRXSEL;
		value2 |= PADCFG2_DEBEN;
	} else {
		value0 &= ~PADCFG0_PREGFRXSEL;
		value2 &= ~PADCFG2_DEBEN;
	}
	pinctrl_writel(value0, padcfg0);
	pinctrl_writel(value2, padcfg2);
	spin_unlock_irqrestore(&pctrl->lock, fl);
	return 0;
}

static int intel_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
				 enum pin_config_param param, uint32_t arg)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	int pin = intel_gpio_to_pin(pctrl, offset, NULL, NULL);

	if (pin < 0)
		return -EINVAL;
	if (!intel_pad_usable(pctrl, (unsigned int)pin))
		return -ENOTSUPP;
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_UP:
	case PIN_CONFIG_BIAS_PULL_DOWN:
		return intel_config_set_pull(pctrl, (unsigned int)pin, param,
					     arg);
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		return intel_gpio_set_high_impedance(pctrl, (unsigned int)pin);
	case PIN_CONFIG_INPUT_DEBOUNCE:
		return intel_config_set_debounce(pctrl, (unsigned int)pin, arg);
	default:
		return -ENOTSUPP;
	}
}

static void intel_gpio_describe(struct gpio_chip *gc, unsigned int offset,
				char *buf, size_t len)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	int pin = intel_gpio_to_pin(pctrl, offset, NULL, NULL);
	const char *name, *lock = "";
	uint8_t *r0, *r1, *r2;
	int locked, acpi;

	if (pin < 0) {
		ksnprintf(buf, len, "-");
		return;
	}
	name = intel_pin_name(pctrl, (unsigned int)pin);
	if (!intel_pad_owned_by_host(pctrl, (unsigned int)pin)) {
		ksnprintf(buf, len, "%s pin %d not available",
			  name ? name : "?", pin);
		return;
	}
	r0 = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG0);
	r1 = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG1);
	r2 = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG2);
	locked = intel_pad_locked(pctrl, (unsigned int)pin);
	acpi = intel_pad_acpi_mode(pctrl, (unsigned int)pin);
	if ((locked & INTEL_PAD_LOCKED_FULL) == INTEL_PAD_LOCKED_FULL)
		lock = " [LOCKED full]";
	else if ((locked & INTEL_PAD_LOCKED_FULL) == INTEL_PAD_LOCKED_TX)
		lock = " [LOCKED tx]";
	else if (locked)
		lock = " [LOCKED]";
	ksnprintf(buf, len, "%s pin %d %s cfg %08x %08x %08x%s%s",
		  name ? name : "?", pin,
		  r0 && !(pinctrl_readl(r0) & PADCFG0_PMODE_MASK) ? "GPIO" :
								    "native",
		  r0 ? pinctrl_readl(r0) : 0, r1 ? pinctrl_readl(r1) : 0,
		  r2 ? pinctrl_readl(r2) : 0, lock, acpi ? " [ACPI]" : "");
}

/* ==========================================================================
 * Interrupt operations (gpiolib calls these with its irq lock held and
 * interrupts off)
 * ========================================================================== */

static void intel_gpio_irq_ack(struct gpio_chip *gc, unsigned int hwirq)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	const struct intel_community *c;
	const struct intel_padgroup *g;
	int pin = intel_gpio_to_pin(pctrl, hwirq, &c, &g);
	uint64_t fl;

	/* gpiolib maps only valid lines: anything else is a bug */
	if (WARN_ON_ONCE(pin < 0))
		return;
	spin_lock_irqsave(&pctrl->lock, &fl);
	pinctrl_writel(1u << padgroup_offset(g, (unsigned int)pin),
		       c->regs + c->is_offset + g->reg_num * 4);
	spin_unlock_irqrestore(&pctrl->lock, fl);
}

static void intel_gpio_irq_mask_unmask(struct gpio_chip *gc,
				       unsigned int hwirq, int mask)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	const struct intel_community *c;
	const struct intel_padgroup *g;
	int pin = intel_gpio_to_pin(pctrl, hwirq, &c, &g);
	uint8_t *reg, *is;
	uint32_t bit, value;
	uint64_t fl;

	/* gpiolib maps only valid lines: anything else is a bug */
	if (WARN_ON_ONCE(pin < 0))
		return;
	bit = 1u << padgroup_offset(g, (unsigned int)pin);
	reg = c->regs + c->ie_offset + g->reg_num * 4;
	is = c->regs + c->is_offset + g->reg_num * 4;
	spin_lock_irqsave(&pctrl->lock, &fl);
	/* Clear the status first: no stale event the moment the pad is
	 * unmasked. */
	pinctrl_writel(bit, is);
	value = pinctrl_readl(reg);
	if (mask)
		value &= ~bit;
	else
		value |= bit;
	pinctrl_writel(value, reg);
	spin_unlock_irqrestore(&pctrl->lock, fl);
}

static void intel_gpio_irq_mask(struct gpio_chip *gc, unsigned int hwirq)
{
	intel_gpio_irq_mask_unmask(gc, hwirq, 1);
}

static void intel_gpio_irq_unmask(struct gpio_chip *gc, unsigned int hwirq)
{
	intel_gpio_irq_mask_unmask(gc, hwirq, 0);
}

static int intel_gpio_irq_type(struct gpio_chip *gc, unsigned int hwirq,
			       unsigned int type)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	int pin = intel_gpio_to_pin(pctrl, hwirq, NULL, NULL);
	uint32_t rxevcfg, rxinv, value;
	uint8_t *reg;
	uint64_t fl;

	if (WARN_ON_ONCE(pin < 0))
		return -EINVAL;
	reg = intel_get_padcfg(pctrl, (unsigned int)pin, PADCFG0);
	if (!reg)
		return -EINVAL;
	/* In ACPI mode the pad still works as a GPIO, but the controller
	 * never sets its GPI_IS bit: it cannot be an interrupt here. */
	if (intel_pad_acpi_mode(pctrl, (unsigned int)pin)) {
		kprintf("%s: pin %d cannot be used as IRQ\n", pctrl_name(pctrl),
			pin);
		return -EPERM;
	}
	if ((type & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH)
		rxevcfg = PADCFG0_RXEVCFG_EDGE_BOTH;
	else if (type & IRQ_TYPE_EDGE_FALLING)
		rxevcfg = PADCFG0_RXEVCFG_EDGE;
	else if (type & IRQ_TYPE_EDGE_RISING)
		rxevcfg = PADCFG0_RXEVCFG_EDGE;
	else if (type & IRQ_TYPE_LEVEL_MASK)
		rxevcfg = PADCFG0_RXEVCFG_LEVEL;
	else
		rxevcfg = PADCFG0_RXEVCFG_DISABLED;
	if (type == IRQ_TYPE_EDGE_FALLING || type == IRQ_TYPE_LEVEL_LOW)
		rxinv = PADCFG0_RXINV;
	else
		rxinv = 0;
	spin_lock_irqsave(&pctrl->lock, &fl);
	if (!intel_pad_is_unlocked(pctrl, (unsigned int)pin)) {
		/* The hardware ignores writes to a locked pad: say so when
		 * the firmware's trigger is not the one asked for. */
		value = pinctrl_readl(reg);
		spin_unlock_irqrestore(&pctrl->lock, fl);
		if ((value & (PADCFG0_RXEVCFG_MASK | PADCFG0_RXINV)) !=
		    (rxevcfg | rxinv))
			kprintf("%s: pin %d locked with cfg0 %08x, trigger %#x not applied\n",
				pctrl_name(pctrl), pin, value, type);
		return 0;
	}
	intel_gpio_set_gpio_mode(pctrl, reg);
	value = pinctrl_readl(reg);
	value = (value & ~PADCFG0_RXEVCFG_MASK) | rxevcfg;
	value = (value & ~PADCFG0_RXINV) | rxinv;
	pinctrl_writel(value, reg);
	spin_unlock_irqrestore(&pctrl->lock, fl);
	return 0;
}

static int intel_gpio_irq_wake(struct gpio_chip *gc, unsigned int hwirq,
			       unsigned int on)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);

	(void)hwirq;
	/* Wake-up is the controller's interrupt as a whole: forward. */
	if (pctrl->irq > 0)
		return irq_set_irq_wake(pctrl->irq, on);
	return 0;
}

/* May the line be an interrupt here at all (process context, before its
 * descriptor is made). */
static int intel_gpio_irq_request(struct gpio_chip *gc, unsigned int hwirq)
{
	struct intel_pinctrl *pctrl = gc_to_pctrl(gc);
	int pin = intel_gpio_to_pin(pctrl, hwirq, NULL, NULL);

	if (pin < 0)
		return -EINVAL;
	if (!intel_pad_owned_by_host(pctrl, (unsigned int)pin)) {
		kprintf("%s: pin %d (GPIO %u) is not owned by the host, no IRQ\n",
			pctrl_name(pctrl), pin, hwirq);
		return -EBUSY;
	}
	if (intel_pad_acpi_mode(pctrl, (unsigned int)pin)) {
		kprintf("%s: pin %d (GPIO %u) cannot be used as IRQ (ACPI mode)\n",
			pctrl_name(pctrl), pin, hwirq);
		return -EPERM;
	}
	return 0;
}

static const struct gpio_irq_chip_ops g_intel_irq_ops = {
	.irq_mask = intel_gpio_irq_mask,
	.irq_unmask = intel_gpio_irq_unmask,
	.irq_ack = intel_gpio_irq_ack,
	.irq_set_type = intel_gpio_irq_type,
	.irq_set_wake = intel_gpio_irq_wake,
	.irq_request = intel_gpio_irq_request,
};

/* Mask and clear all interrupts.  pctrl->lock held. */
static void intel_gpio_irq_init_locked(struct intel_pinctrl *pctrl)
{
	lockdep_assert_held(&pctrl->lock);
	for (size_t i = 0; i < pctrl->ncommunities; i++) {
		struct intel_community *c = &pctrl->communities[i];

		for (size_t j = 0; j < c->ngpps; j++) {
			unsigned int r = c->gpps[j].reg_num * 4;

			pinctrl_writel(0, c->regs + c->ie_offset + r);
			pinctrl_writel(0xffffffffu, c->regs + c->is_offset + r);
		}
	}
}

void intel_pinctrl_irq_init_hw(struct intel_pinctrl *pctrl)
{
	uint64_t fl;

	spin_lock_irqsave(&pctrl->lock, &fl);
	intel_gpio_irq_init_locked(pctrl);
	spin_unlock_irqrestore(&pctrl->lock, fl);
}

int intel_pinctrl_irq_handler(void *arg)
{
	struct intel_pinctrl *pctrl = arg;
	int handled = 0;

	/* Every community may have pending pads. */
	for (size_t i = 0; i < pctrl->ncommunities; i++) {
		struct intel_community *c = &pctrl->communities[i];

		for (size_t j = 0; j < c->ngpps; j++) {
			const struct intel_padgroup *g = &c->gpps[j];
			unsigned int r = g->reg_num * 4;
			uint32_t pending, enabled;

			spin_lock(&pctrl->lock);
			pending = pinctrl_readl(c->regs + c->is_offset + r);
			enabled = pinctrl_readl(c->regs + c->ie_offset + r);
			spin_unlock(&pctrl->lock);
			/* Only interrupts that are enabled */
			pending &= enabled;
			if (g->size < 32)
				pending &= (1u << g->size) - 1;
			if (!pending)
				continue;
			handled = 1;
			while (pending) {
				unsigned int bit =
					(unsigned int)__builtin_ctz(pending);

				pending &= pending - 1;
				if (g->gpio_base == INTEL_GPIO_BASE_NOMAP) {
					/* Enabled by firmware, never by us:
					 * silence it. */
					spin_lock(&pctrl->lock);
					pinctrl_writel(pinctrl_readl(c->regs +
								     c->ie_offset +
								     r) &
							       ~(1u << bit),
						       c->regs + c->ie_offset + r);
					pinctrl_writel(1u << bit,
						       c->regs + c->is_offset + r);
					spin_unlock(&pctrl->lock);
					WARN_RATELIMIT(1, "%s: interrupt on unmapped pin %u",
						       pctrl_name(pctrl),
						       g->base + bit);
					continue;
				}
				gpiochip_handle_irq(&pctrl->chip,
						    (unsigned int)g->gpio_base +
							    bit);
			}
		}
	}
	if (handled)
		pctrl->irq_count++;
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/* ==========================================================================
 * System sleep
 * ========================================================================== */

/* Is the pad in use, so that its configuration is to survive sleep:
 * requested or used as an interrupt here, or a firmware-made direct
 * IOxAPIC interrupt (firmware does not always restore those right: the
 * RX value comes back inverted on some machines).  Pads the firmware
 * uses while resuming and has not locked are left to it. */
static int intel_pinctrl_should_save(struct intel_pinctrl *pctrl,
				     unsigned int pin)
{
	uint8_t *padcfg0;
	int gpio;

	if (!intel_get_padcfg(pctrl, pin, PADCFG0) ||
	    !intel_pad_usable(pctrl, pin))
		return 0;
	gpio = intel_pin_to_gpio(pctrl, pin);
	if (gpio >= 0 && pctrl->chip.lines &&
	    (gpiochip_line_is_requested(&pctrl->chip, (unsigned int)gpio) ||
	     gpiochip_line_is_irq(&pctrl->chip, (unsigned int)gpio)))
		return 1;
	padcfg0 = intel_get_padcfg(pctrl, pin, PADCFG0);
	return __intel_gpio_is_direct_irq(pinctrl_readl(padcfg0));
}

void intel_pinctrl_suspend(struct intel_pinctrl *pctrl)
{
	const struct intel_pinctrl_soc_data *soc = pctrl->soc;
	uint64_t fl;

	if (!pctrl->pads)
		return;
	for (size_t i = 0; i < soc->npins; i++) {
		unsigned int pin = soc->pins[i].number;
		struct intel_pad_context *ctx = &pctrl->pads[i];
		uint8_t *padcfg;

		/* A pad not saved now must not be restored from an older
		 * suspend. */
		kmemset(ctx, 0, sizeof(*ctx));
		if (!intel_pinctrl_should_save(pctrl, pin))
			continue;
		spin_lock_irqsave(&pctrl->lock, &fl);
		ctx->padcfg0 = pinctrl_readl(intel_get_padcfg(pctrl, pin,
							      PADCFG0)) &
			       ~PADCFG0_GPIORXSTATE;
		ctx->padcfg1 = pinctrl_readl(intel_get_padcfg(pctrl, pin,
							      PADCFG1));
		padcfg = intel_get_padcfg(pctrl, pin, PADCFG2);
		if (padcfg)
			ctx->padcfg2 = pinctrl_readl(padcfg);
		ctx->saved = 1;
		spin_unlock_irqrestore(&pctrl->lock, fl);
	}
	spin_lock_irqsave(&pctrl->lock, &fl);
	for (size_t i = 0; i < pctrl->ncommunities; i++) {
		struct intel_community *c = &pctrl->communities[i];

		for (size_t j = 0; j < c->ngpps; j++) {
			unsigned int r = c->gpps[j].reg_num * 4;

			c->ctx_intmask[j] =
				pinctrl_readl(c->regs + c->ie_offset + r);
			c->ctx_hostown[j] =
				c->hostown_offset ?
					pinctrl_readl(c->regs +
						      c->hostown_offset + r) :
					0;
		}
	}
	spin_unlock_irqrestore(&pctrl->lock, fl);
}

/* Write `value' into the bits `mask' of a register when they differ.
 * Returns whether it wrote. */
static int intel_gpio_update_reg(uint8_t *reg, uint32_t mask, uint32_t value)
{
	uint32_t curr = pinctrl_readl(reg);
	uint32_t updated = (curr & ~mask) | (value & mask);

	if (curr == updated)
		return 0;
	pinctrl_writel(updated, reg);
	return 1;
}

static void intel_restore_padcfg(struct intel_pinctrl *pctrl, unsigned int pin,
				 unsigned int reg, uint32_t saved)
{
	uint32_t mask = (reg == PADCFG0) ? PADCFG0_GPIORXSTATE : 0;
	uint8_t *padcfg = intel_get_padcfg(pctrl, pin, reg);

	lockdep_assert_held(&pctrl->lock);
	if (!padcfg)
		return;
	(void)intel_gpio_update_reg(padcfg, ~mask, saved);
}

void intel_pinctrl_resume(struct intel_pinctrl *pctrl)
{
	const struct intel_pinctrl_soc_data *soc = pctrl->soc;
	uint64_t fl;

	if (!pctrl->pads)
		return;
	/* Mask all interrupts */
	intel_pinctrl_irq_init_hw(pctrl);

	for (size_t i = 0; i < soc->npins; i++) {
		unsigned int pin = soc->pins[i].number;
		const struct intel_pad_context *ctx = &pctrl->pads[i];

		/* A pad in use -- or one whose saved value was a direct
		 * interrupt input, in case firmware mangled the register
		 * too much to tell now. */
		if (!(intel_pinctrl_should_save(pctrl, pin) ||
		      (ctx->saved && __intel_gpio_is_direct_irq(ctx->padcfg0))))
			continue;
		if (!ctx->saved)
			continue; /* came into use after the suspend */
		spin_lock_irqsave(&pctrl->lock, &fl);
		intel_restore_padcfg(pctrl, pin, PADCFG0, ctx->padcfg0);
		intel_restore_padcfg(pctrl, pin, PADCFG1, ctx->padcfg1);
		intel_restore_padcfg(pctrl, pin, PADCFG2, ctx->padcfg2);
		spin_unlock_irqrestore(&pctrl->lock, fl);
	}

	for (size_t i = 0; i < pctrl->ncommunities; i++) {
		struct intel_community *c = &pctrl->communities[i];

		for (size_t j = 0; j < c->ngpps; j++) {
			const struct intel_padgroup *g = &c->gpps[j];
			unsigned int r = g->reg_num * 4;
			uint32_t requested = 0;

			/* HOSTSW_OWN: only the bits of requested lines go
			 * back; the rest is the firmware's to decide. */
			if (c->hostown_offset &&
			    g->gpio_base != INTEL_GPIO_BASE_NOMAP &&
			    pctrl->chip.lines) {
				for (unsigned int k = 0; k < g->size; k++)
					if (gpiochip_line_is_requested(
						    &pctrl->chip,
						    (unsigned int)g->gpio_base +
							    k))
						requested |= 1u << k;
			}
			spin_lock_irqsave(&pctrl->lock, &fl);
			(void)intel_gpio_update_reg(c->regs + c->ie_offset + r,
						    ~0u, c->ctx_intmask[j]);
			if (requested)
				(void)intel_gpio_update_reg(
					c->regs + c->hostown_offset + r,
					requested, c->ctx_hostown[j]);
			spin_unlock_irqrestore(&pctrl->lock, fl);
		}
	}
	/* The enables are the ones of the suspend; a line masked or
	 * unmasked since (a oneshot thread still running, a disabled
	 * event) gets the mask gpiolib holds for it again. */
	gpiochip_irq_resync_masks(&pctrl->chip);
}

/* ==========================================================================
 * Set-up
 * ========================================================================== */

/* Does [off, off + len) lie inside the window. */
static int intel_in_window(const struct intel_community *c, uint64_t off,
			   uint64_t len)
{
	return off + len <= c->length;
}

static int intel_pinctrl_add_padgroups_by_gpps(struct intel_community *c)
{
	const struct intel_padgroup *tbl = c->gpps;
	struct intel_padgroup *gpps;
	unsigned int padown_num = 0;
	size_t ngpps = c->ngpps;

	gpps = kcalloc(ngpps, sizeof(*gpps));
	if (!gpps)
		return -ENOMEM;
	for (size_t i = 0; i < ngpps; i++) {
		gpps[i] = tbl[i];
		if (gpps[i].size == 0 ||
		    gpps[i].size > INTEL_PINCTRL_MAX_GPP_SIZE) {
			kfree(gpps);
			return -EINVAL;
		}
		/* Special treatment for GPIO base */
		switch (gpps[i].gpio_base) {
		case INTEL_GPIO_BASE_MATCH:
			gpps[i].gpio_base = (int)gpps[i].base;
			break;
		case INTEL_GPIO_BASE_ZERO:
			gpps[i].gpio_base = 0;
			break;
		case INTEL_GPIO_BASE_NOMAP:
		default:
			break;
		}
		gpps[i].padown_num = padown_num;
		/* 4 bits per pad, packed into 32-bit registers. */
		padown_num += (gpps[i].size * 4 + INTEL_PINCTRL_MAX_GPP_SIZE -
			       1) / INTEL_PINCTRL_MAX_GPP_SIZE;
	}
	c->gpps = gpps;
	return 0;
}

static int intel_pinctrl_add_padgroups_by_size(struct intel_community *c)
{
	struct intel_padgroup *gpps;
	unsigned int npins = (unsigned int)c->npins;
	unsigned int padown_num = 0;
	size_t ngpps;

	if (c->gpp_size == 0 || c->gpp_size > INTEL_PINCTRL_MAX_GPP_SIZE)
		return -EINVAL;
	ngpps = (c->npins + c->gpp_size - 1) / c->gpp_size;
	gpps = kcalloc(ngpps, sizeof(*gpps));
	if (!gpps)
		return -ENOMEM;
	for (size_t i = 0; i < ngpps; i++) {
		unsigned int gpp_size = c->gpp_size;

		gpps[i].reg_num = (unsigned int)i;
		gpps[i].base = c->pin_base + (unsigned int)i * gpp_size;
		gpps[i].size = npins < gpp_size ? npins : gpp_size;
		npins -= gpps[i].size;
		gpps[i].gpio_base = (int)gpps[i].base;
		gpps[i].padown_num = padown_num;
		padown_num += c->gpp_num_padown_regs;
	}
	c->ngpps = ngpps;
	c->gpps = gpps;
	return 0;
}

/* The groups stay inside the community and the per-group registers
 * inside the window.  Firmware-described (_DSD) controllers can say
 * anything, so this is checked rather than trusted. */
static int intel_community_check(const struct intel_community *c)
{
	unsigned int nregs = 0, padown_regs = 0, covered = 0;

	for (size_t i = 0; i < c->ngpps; i++) {
		const struct intel_padgroup *g = &c->gpps[i];

		if (g->base < c->pin_base ||
		    (uint64_t)g->base + g->size > c->pin_base + c->npins)
			return -EINVAL;
		for (size_t j = 0; j < i; j++) {
			const struct intel_padgroup *h = &c->gpps[j];

			if (g->base < h->base + h->size &&
			    h->base < g->base + g->size)
				return -EINVAL;
		}
		if (g->reg_num + 1 > nregs)
			nregs = g->reg_num + 1;
		if (g->padown_num + (g->size * 4 + 31) / 32 > padown_regs)
			padown_regs = g->padown_num + (g->size * 4 + 31) / 32;
		covered += g->size;
	}
	if (covered > c->npins)
		return -EINVAL;
	if (!intel_in_window(c, c->is_offset, nregs * 4ull) ||
	    !intel_in_window(c, c->ie_offset, nregs * 4ull) ||
	    !intel_in_window(c, c->hostown_offset, nregs * 4ull) ||
	    !intel_in_window(c, c->padcfglock_offset, nregs * 8ull))
		return -EINVAL;
	if (c->features & PINCTRL_FEATURE_3BIT_PAD_OWN) {
		if (!intel_in_window(c, c->padown_offset, c->npins * 4ull))
			return -EINVAL;
	} else if (!intel_in_window(c, c->padown_offset,
				    padown_regs * 4ull)) {
		return -EINVAL;
	}
	return 0;
}

/* Read the capability list: which optional blocks the community has. */
static void intel_probe_caps(struct intel_community *c)
{
	uint32_t offset = CAPLIST;

	for (int n = 0; n < CAPLIST_MAX_WALK && offset; n++) {
		uint32_t value;

		if ((offset & 3) || !intel_in_window(c, offset, 4))
			break;
		value = pinctrl_readl(c->regs + offset);
		switch ((value & CAPLIST_ID_MASK) >> CAPLIST_ID_SHIFT) {
		case CAPLIST_ID_GPIO_HW_INFO:
			c->features |= PINCTRL_FEATURE_GPIO_HW_INFO;
			c->cap_offset[CAPLIST_ID_GPIO_HW_INFO] = (uint16_t)offset;
			break;
		case CAPLIST_ID_PWM:
			c->features |= PINCTRL_FEATURE_PWM;
			c->cap_offset[CAPLIST_ID_PWM] = (uint16_t)offset;
			break;
		case CAPLIST_ID_BLINK:
			c->features |= PINCTRL_FEATURE_BLINK;
			c->cap_offset[CAPLIST_ID_BLINK] = (uint16_t)offset;
			break;
		case CAPLIST_ID_EXP:
			c->features |= PINCTRL_FEATURE_EXP;
			c->cap_offset[CAPLIST_ID_EXP] = (uint16_t)offset;
			break;
		default:
			break;
		}
		offset = (value & CAPLIST_NEXT_MASK) >> CAPLIST_NEXT_SHIFT;
	}
}

void intel_pinctrl_teardown(struct intel_pinctrl *pctrl)
{
	if (pctrl->communities) {
		for (size_t i = 0; i < pctrl->ncommunities; i++) {
			struct intel_community *c = &pctrl->communities[i];

			/* Groups are the core's copies once regs is set. */
			if (c->regs)
				kfree((void *)c->gpps);
			kfree(c->ctx_intmask);
			kfree(c->ctx_hostown);
		}
		kfree(pctrl->communities);
	}
	kfree(pctrl->pads);
	pctrl->communities = NULL;
	pctrl->ncommunities = 0;
	pctrl->pads = NULL;
}

int intel_pinctrl_setup(struct intel_pinctrl *pctrl,
			const struct intel_pinctrl_soc_data *soc,
			uint8_t *const *windows, const uint64_t *lengths,
			unsigned int nwindows)
{
	unsigned int ngpio = 0;
	int rc;

	if (!pctrl || !soc || !windows || soc->ncommunities == 0 ||
	    !soc->communities)
		return -EINVAL;
	pctrl->soc = soc;
	spinlock_init(&pctrl->lock, "intel_pinctrl");
	pctrl->ncommunities = soc->ncommunities;
	pctrl->communities = kcalloc(soc->ncommunities,
				     sizeof(*pctrl->communities));
	if (!pctrl->communities)
		return -ENOMEM;
	for (size_t i = 0; i < soc->ncommunities; i++) {
		struct intel_community *c = &pctrl->communities[i];
		uint32_t value, revision, padbar;
		uint64_t stride;

		/* A copy of the table's community, the core's fields
		 * cleared (a table never sets them). */
		*c = soc->communities[i];
		c->regs = NULL;
		c->pad_regs = NULL;
		c->ctx_intmask = NULL;
		c->ctx_hostown = NULL;
		kmemset(c->cap_offset, 0, sizeof(c->cap_offset));
		if (c->barno >= nwindows || !windows[c->barno]) {
			kprintf("intel-pinctrl: community %zu needs memory resource %u, firmware gives %u\n",
				i, c->barno, nwindows);
			rc = -ENODEV;
			goto fail;
		}
		c->length = lengths && lengths[c->barno] ? lengths[c->barno] :
							   INTEL_DEFAULT_WINDOW;
		if (c->length < 0x10) {
			rc = -ENODEV;
			goto fail;
		}
		/* Determine community features based on the revision.  All
		 * ones means the device is not there. */
		value = pinctrl_readl(windows[c->barno] + REVID);
		if (value == ~0u) {
			kprintf("intel-pinctrl: community %zu does not answer\n",
				i);
			rc = -ENODEV;
			goto fail;
		}
		revision = (value & REVID_MASK) >> REVID_SHIFT;
		c->revision = revision;
		if (revision >= 0x092)
			c->features |= PINCTRL_FEATURE_DEBOUNCE |
				       PINCTRL_FEATURE_1K_PD;
		if (revision >= 0x110)
			c->features |= PINCTRL_FEATURE_3BIT_PAD_OWN;
		/* regs is what tells teardown the groups are ours: set it
		 * only right before they are made. */
		c->regs = windows[c->barno];
		/* Determine community features based on the capabilities */
		intel_probe_caps(c);
		/* The offset of the pad configuration registers */
		padbar = pinctrl_readl(c->regs + PADBAR);
		stride = (c->features & PINCTRL_FEATURE_DEBOUNCE) ? 16 : 8;
		if ((padbar & 3) || padbar < 0x10 ||
		    !intel_in_window(c, padbar, c->npins * stride)) {
			kprintf("intel-pinctrl: community %zu PADBAR %#x does not fit %zu pads in %#lx\n",
				i, padbar, c->npins, (unsigned long)c->length);
			c->gpps = NULL; /* nothing of ours yet */
			rc = -ENODEV;
			goto fail;
		}
		c->pad_regs = c->regs + padbar;
		if (c->gpps)
			rc = intel_pinctrl_add_padgroups_by_gpps(c);
		else
			rc = intel_pinctrl_add_padgroups_by_size(c);
		if (rc) {
			c->gpps = NULL;
			kprintf("intel-pinctrl: community %zu groups are inconsistent\n",
				i);
			goto fail;
		}
		rc = intel_community_check(c);
		if (rc) {
			kprintf("intel-pinctrl: community %zu registers do not fit the window\n",
				i);
			goto fail;
		}
		c->ctx_intmask = kcalloc(c->ngpps, sizeof(uint32_t));
		c->ctx_hostown = kcalloc(c->ngpps, sizeof(uint32_t));
		if (!c->ctx_intmask || !c->ctx_hostown) {
			rc = -ENOMEM;
			goto fail;
		}
		for (size_t j = 0; j < c->ngpps; j++) {
			const struct intel_padgroup *g = &c->gpps[j];

			if (g->gpio_base != INTEL_GPIO_BASE_NOMAP &&
			    (unsigned int)g->gpio_base + g->size > ngpio)
				ngpio = (unsigned int)g->gpio_base + g->size;
		}
	}
	if (ngpio == 0) {
		rc = -EINVAL;
		goto fail;
	}
	if (soc->npins) {
		pctrl->pads = kcalloc(soc->npins, sizeof(*pctrl->pads));
		if (!pctrl->pads) {
			rc = -ENOMEM;
			goto fail;
		}
	}
	pctrl->chip.label = pctrl->label;
	pctrl->chip.ngpio = ngpio;
	pctrl->chip.data = pctrl;
	pctrl->chip.request = intel_gpio_request;
	pctrl->chip.free = NULL;
	pctrl->chip.get_direction = intel_gpio_get_direction;
	pctrl->chip.direction_input = intel_gpio_direction_input;
	pctrl->chip.direction_output = intel_gpio_direction_output;
	pctrl->chip.get = intel_gpio_get;
	pctrl->chip.set = intel_gpio_set;
	pctrl->chip.set_config = intel_gpio_set_config;
	pctrl->chip.valid = intel_gpio_valid;
	pctrl->chip.describe = intel_gpio_describe;
	pctrl->chip.irq = &g_intel_irq_ops;
	return 0;
fail:
	/* Communities after the failing one were never touched: their
	 * regs is NULL and their gpps the table's. */
	intel_pinctrl_teardown(pctrl);
	return rc;
}

/* ==========================================================================
 * Probe
 * ========================================================================== */

const struct intel_pinctrl_match *const intel_pinctrl_tables[] = {
	intel_sunrisepoint_match, intel_cannonlake_match,
	intel_cedarfork_match,	  intel_denverton_match,
	intel_lewisburg_match,	  intel_emmitsburg_match,
	intel_icelake_match,	  intel_lakefield_match,
	intel_tigerlake_match,	  intel_jasperlake_match,
	intel_elkhartlake_match,  intel_alderlake_match,
	intel_meteorlake_match,	  intel_meteorpoint_match,
	intel_broxton_match,	  intel_geminilake_match,
	NULL,
};

const struct intel_pinctrl_soc_data *
intel_pinctrl_lookup(const struct intel_pinctrl_match *match,
		     const struct acpi_device *adev)
{
	for (const struct intel_pinctrl_match *m = match; m->hid; m++) {
		if (!acpi_dev_has_id(adev, m->hid))
			continue;
		if (m->data)
			return m->data;
		if (!m->uid_table)
			return NULL;
		/* One device per community: the _UID picks the table. */
		for (const struct intel_pinctrl_soc_data *const *s =
			     m->uid_table;
		     *s; s++) {
			if ((*s)->uid && adev->has_uid &&
			    kstrcmp(adev->uid, (*s)->uid) == 0)
				return *s;
		}
		return NULL;
	}
	return NULL;
}

int intel_pinctrl_probe(struct acpi_device *adev,
			const struct intel_pinctrl_soc_data *soc,
			struct intel_pinctrl_soc_data *owned)
{
	uint8_t *windows[INTEL_MAX_WINDOWS] = { 0 };
	uint64_t lengths[INTEL_MAX_WINDOWS] = { 0 };
	struct acpi_res_list res;
	const struct acpi_res *irqres;
	struct intel_pinctrl *pctrl;
	unsigned int nwin = 0;
	int rc;

	might_sleep();
	rc = acpi_dev_get_resources(adev, &res);
	if (rc) {
		kprintf("intel-pinctrl: %s: no resources (%d)\n", adev->path,
			rc);
		return rc;
	}
	for (int i = 0; i < res.count && nwin < INTEL_MAX_WINDOWS; i++) {
		const struct acpi_res *r = &res.res[i];
		uint64_t len, pages, virt;

		if (r->type != ACPI_RES_MEM)
			continue;
		len = r->mem.length ? r->mem.length : INTEL_DEFAULT_WINDOW;
		pages = ((r->mem.start & 0xfffull) + len + 0xfffull) >> 12;
		virt = mm_map_device_mmio(r->mem.start, (size_t)pages);
		if (!virt) {
			kprintf("intel-pinctrl: %s: cannot map %#lx\n",
				adev->path, (unsigned long)r->mem.start);
			acpi_dev_free_resource_list(&res);
			return -ENOMEM;
		}
		windows[nwin] = (uint8_t *)virt;
		lengths[nwin] = len;
		nwin++;
	}
	pctrl = kcalloc(1, sizeof(*pctrl));
	if (!pctrl) {
		acpi_dev_free_resource_list(&res);
		return -ENOMEM;
	}
	pctrl->adev = acpi_dev_get(adev);
	pctrl->owned_soc = owned;
	INIT_LIST_HEAD(&pctrl->node);
	ksnprintf(pctrl->label, sizeof(pctrl->label), "%s:%s", adev->hid,
		  adev->has_uid ? adev->uid : "0");
	/* Mapped windows are not unmapped on failure: device memory, a
	 * few pages, and nothing else will claim it. */
	rc = intel_pinctrl_setup(pctrl, soc, windows, lengths, nwin);
	if (rc) {
		kprintf("%s: %s does not fit the table (%d), left alone\n",
			pctrl->label, adev->path, rc);
		goto fail;
	}
	pctrl->chip.acpi_handle = adev->handle;
	/* The interrupt lines in a known state before anything else. */
	intel_pinctrl_irq_init_hw(pctrl);

	irqres = acpi_res_find(&res, ACPI_RES_IRQ, 0);
	if (irqres) {
		int irq;

		pctrl->gsi = irqres->irq.gsi;
		irq = irq_alloc_desc_gsi(NULL, pctrl, (int)irqres->irq.gsi,
					 irqres->irq.polarity ==
						 ACPI_RES_ACTIVE_LOW,
					 irqres->irq.triggering ==
						 ACPI_RES_LEVEL);
		if (irq > 0) {
			/* Several GPIO controllers may share one line. */
			rc = request_irq(irq, intel_pinctrl_irq_handler,
					 IRQF_SHARED, pctrl->label, pctrl);
			if (rc) {
				irq_free_desc(irq);
				irq = rc;
			}
		}
		if (irq > 0) {
			pctrl->irq = irq;
		} else {
			kprintf("%s: GSI %u unavailable (%d), no interrupts\n",
				pctrl->label, irqres->irq.gsi, irq);
			pctrl->chip.irq = NULL;
		}
	} else {
		kprintf("%s: no interrupt resource, GPIO only\n", pctrl->label);
		pctrl->chip.irq = NULL;
	}
	rc = gpiochip_add(&pctrl->chip);
	if (rc) {
		if (pctrl->irq > 0) {
			(void)free_irq(pctrl->irq, pctrl);
			irq_free_desc(pctrl->irq);
		}
		intel_pinctrl_teardown(pctrl);
		goto fail;
	}
	for (size_t i = 0; i < pctrl->ncommunities; i++) {
		const struct intel_community *c = &pctrl->communities[i];

		kprintf("%s: community %zu: pins %u-%u, %zu groups, rev %#x, features %#x\n",
			pctrl->label, i, c->pin_base,
			c->pin_base + (unsigned int)c->npins - 1, c->ngpps,
			c->revision, c->features);
	}
	if (pctrl->irq > 0)
		kprintf("%s: %u GPIOs, interrupt GSI %u\n", pctrl->label,
			pctrl->chip.ngpio, pctrl->gsi);
	mutex_lock(&g_intel_list_lock);
	list_add_tail(&pctrl->node, &g_intel_list);
	mutex_unlock(&g_intel_list_lock);
	acpi_dev_free_resource_list(&res);
	return 0;
fail:
	acpi_dev_put(pctrl->adev);
	kfree(pctrl);
	acpi_dev_free_resource_list(&res);
	return rc;
}

void intel_pinctrl_suspend_all(void)
{
	struct list_head *p;

	mutex_lock(&g_intel_list_lock);
	for (p = g_intel_list.next; p != &g_intel_list; p = p->next)
		intel_pinctrl_suspend(
			container_of(p, struct intel_pinctrl, node));
	mutex_unlock(&g_intel_list_lock);
}

void intel_pinctrl_resume_all(void)
{
	struct list_head *p;

	mutex_lock(&g_intel_list_lock);
	for (p = g_intel_list.next; p != &g_intel_list; p = p->next)
		intel_pinctrl_resume(
			container_of(p, struct intel_pinctrl, node));
	mutex_unlock(&g_intel_list_lock);
}

#define INTEL_MAX_IDS 64
#define INTEL_MAX_DEVS 32

void intel_pinctrl_init(void)
{
	static struct acpi_device_id ids[INTEL_MAX_IDS];
	struct acpi_device *devs[INTEL_MAX_DEVS];
	unsigned int n = 0;
	int ndev;

	might_sleep();
	if (!acpi_dev_ready())
		return;
	kmemset(ids, 0, sizeof(ids));
	for (unsigned int t = 0; intel_pinctrl_tables[t]; t++) {
		for (const struct intel_pinctrl_match *m =
			     intel_pinctrl_tables[t];
		     m->hid; m++) {
			unsigned int k = 0;

			if (WARN_ON_ONCE(n + 1 >= INTEL_MAX_IDS))
				break;
			for (; m->hid[k] && k + 1 < ACPI_ID_LEN; k++)
				ids[n].id[k] = m->hid[k];
			ids[n].id[k] = '\0';
			n++;
		}
	}
	/* Probed after the namespace walk: a probe evaluates AML, maps
	 * registers and requests interrupts. */
	ndev = acpi_dev_collect_matches(ids, 1, devs, INTEL_MAX_DEVS);
	for (int i = 0; i < ndev; i++) {
		const struct intel_pinctrl_soc_data *soc = NULL;

		for (unsigned int t = 0; intel_pinctrl_tables[t] && !soc; t++)
			soc = intel_pinctrl_lookup(intel_pinctrl_tables[t],
						   devs[i]);
		if (!soc) {
			kprintf("intel-pinctrl: %s (%s uid %s): no table for this unit\n",
				devs[i]->path, devs[i]->hid,
				devs[i]->has_uid ? devs[i]->uid : "-");
		} else if (!gpiochip_find_by_acpi_handle(devs[i]->handle)) {
			(void)intel_pinctrl_probe(devs[i], soc, NULL);
		}
		acpi_dev_put(devs[i]);
	}
}
