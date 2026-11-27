// LikeOS pinctrl - AMD FCH GPIO controller
//
// The GPIO block of AMD's fusion controller hub (AMDI0030 on Ryzen
// notebooks, AMD0030 on older firmware, AMDI0031 and AMDI0033 on newer
// parts) is a plain array: pin n is the 32-bit register at offset n * 4 of
// the first memory resource, holding its whole configuration and state:
//
//    3:0  debounce timer count      4 debounce timer unit
//    6:5  debounce control (0 off, 1 keep low glitch, 2 keep high
//         glitch, 3 remove glitches)
//    7    debounce timer large
//    8    level trigger (else edge)
//   10:9  active level (0 high/rising, 1 low/falling, 2 both edges)
//   11    interrupt enable (the pad generates status)
//   12    interrupt mask (1 = delivered to the controller output)
//   13-15 wake in S0i3 / S3 / S4
//   16    pin status (the input level)
//   18:17 drive strength            20 pull-up enable  21 pull-down enable
//   22    output value              23 output enable
//   28    interrupt status (write 1 to clear)
//   29    wake status (write 1 to clear)
//
// The pins are 0-183 without 63 (offset 0xfc is WAKE_INT_MASTER); a
// controller whose _DSD names an "rgpio" resource in its
// "pinctrl-resource-names" has 16 more pins, 256-271, in that block
// (registers from 0, status summary at 0xf4, one bit per pin).
//
// Interrupt summary: WAKE_INT_STATUS (0x2f8/0x2fc, 46 bits) has one bit
// per four pins with a pending interrupt or wake; WAKE_INT_MASTER (0xfc)
// takes the end-of-interrupt (bit 29) after the pins were cleared.  The
// controller's own interrupt is an I/O APIC line shared with other
// devices.  Writing a pin register back with its status bits set clears
// them; a pending pin that nobody uses as an interrupt is masked so it
// cannot storm.  At probe only the S0i3/S3 wake enables are cleared (S4
// too when the firmware has GPIO events, _AEI): interrupt enables the
// firmware set stay, and fire at most once.
//
// Changing a pin's trigger or debounce may, depending on WAKE_INT_MASTER
// MaskStsEn, block status generation of ALL pins for a while; during that
// time the interrupt-enable bit reads 0.  set_type therefore enables the
// pin (masked) and waits up to a millisecond for the enable to read back
// before restoring.  GPIO 0 (the power button) may be debounced by the
// hardware itself (WAKE_INT_MASTER bit 15): then software debounce is
// off for it.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2014, 2015 AMD Corporation; Ken Xue

#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/io.h>
#include <kernel/ke/list.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#ifndef pinctrl_readl
#define pinctrl_readl(addr) readl(addr)
#define pinctrl_writel(val, addr) writel((val), (addr))
#endif
#ifndef pinctrl_udelay
#define pinctrl_udelay(us) udelay(us)
#endif

#define AMD_GPIO_PINS_BANK4 16
#define AMD_GPIO_RGPIO_PIN_BASE 256
#define AMD_GPIO_MAIN_PINS 184 /* 0-183, without 63 */

#define WAKE_INT_MASTER_REG 0xfc
#define INTERNAL_GPIO0_DEBOUNCE (1u << 15)
#define EOI_MASK (1u << 29)

#define WAKE_INT_STATUS_REG0 0x2f8
#define WAKE_INT_STATUS_REG1 0x2fc
#define WAKE_INT_STATUS_REG_RGPIO 0x0f4

#define DB_TMR_OUT_OFF 0
#define DB_TMR_OUT_UNIT_OFF 4
#define DB_CNTRL_OFF 5
#define DB_TMR_LARGE_OFF 7
#define LEVEL_TRIG_OFF 8
#define ACTIVE_LEVEL_OFF 9
#define INTERRUPT_ENABLE_OFF 11
#define INTERRUPT_MASK_OFF 12
#define WAKE_CNTRL_OFF_S0I3 13
#define WAKE_CNTRL_OFF_S3 14
#define WAKE_CNTRL_OFF_S4 15
#define PIN_STS_OFF 16
#define DRV_STRENGTH_SEL_OFF 17
#define PULL_UP_ENABLE_OFF 20
#define PULL_DOWN_ENABLE_OFF 21
#define OUTPUT_VALUE_OFF 22
#define OUTPUT_ENABLE_OFF 23
#define INTERRUPT_STS_OFF 28
#define WAKE_STS_OFF 29

#define BIT32(n) (1u << (n))
#define DB_TMR_OUT_MASK 0xfu
#define DB_CNTRL_MASK 0x3u
#define ACTIVE_LEVEL_MASK 0x3u
#define DRV_STRENGTH_SEL_MASK 0x3u
#define DB_TYPE_REMOVE_GLITCH 0x3u
#define LEVEL_TRIGGER 0x1u
#define ACTIVE_HIGH 0x0u
#define ACTIVE_LOW 0x1u
#define BOTH_EDGES 0x2u
#define CLR_INTR_STAT 0x1u

#define WAKE_SOURCE_SUSPEND (BIT32(WAKE_CNTRL_OFF_S0I3) | BIT32(WAKE_CNTRL_OFF_S3))
#define PIN_IRQ_PENDING (BIT32(INTERRUPT_STS_OFF) | BIT32(WAKE_STS_OFF))

#define AMD_SET_TYPE_POLL_US 1000

struct amd_gpio {
	struct gpio_chip chip;
	struct acpi_device *adev;
	uint8_t *base;
	uint8_t *base_rgpio; /* NULL: no remote GPIO block */
	uint64_t length;
	spinlock_t lock;
	int parent_irq;
	uint32_t *saved_regs; /* per GPIO number, over system sleep */
	struct list_head node;
	char label[40];
};

static DEFINE_MUTEX(g_amd_list_lock);
static struct list_head g_amd_list = LIST_HEAD_INIT(g_amd_list);

static struct amd_gpio *gc_to_amd(struct gpio_chip *gc)
{
	return container_of(gc, struct amd_gpio, chip);
}

/* A pin with a register: 0-183 except 63 inside the window, and the
 * remote block's 16 pins when there is one. */
static int amd_pin_valid(const struct amd_gpio *ag, unsigned int pin)
{
	if (pin >= AMD_GPIO_RGPIO_PIN_BASE)
		return ag->base_rgpio &&
		       pin < AMD_GPIO_RGPIO_PIN_BASE + AMD_GPIO_PINS_BANK4;
	return pin < AMD_GPIO_MAIN_PINS && pin != 63 &&
	       (uint64_t)pin * 4 + 4 <= ag->length;
}

static uint8_t *amd_gpio_pin_reg(struct amd_gpio *ag, unsigned int pin)
{
	if (!amd_pin_valid(ag, pin))
		return NULL;
	if (ag->base_rgpio && pin >= AMD_GPIO_RGPIO_PIN_BASE)
		return ag->base_rgpio + (pin - AMD_GPIO_RGPIO_PIN_BASE) * 4;
	return ag->base + pin * 4;
}

/* ---- GPIO ---------------------------------------------------------------- */

static int amd_gpio_valid(struct gpio_chip *gc, unsigned int offset)
{
	return amd_pin_valid(gc_to_amd(gc), offset);
}

static int amd_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, offset);
	uint32_t pin_reg;
	uint64_t fl;

	if (!r)
		return -EINVAL;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r);
	spin_unlock_irqrestore(&ag->lock, fl);
	if (pin_reg & BIT32(OUTPUT_ENABLE_OFF))
		return GPIO_LINE_DIRECTION_OUT;
	return GPIO_LINE_DIRECTION_IN;
}

static int amd_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, offset);
	uint32_t pin_reg;
	uint64_t fl;

	if (!r)
		return -EINVAL;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r);
	pin_reg &= ~BIT32(OUTPUT_ENABLE_OFF);
	pinctrl_writel(pin_reg, r);
	spin_unlock_irqrestore(&ag->lock, fl);
	return 0;
}

static int amd_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				     int value)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, offset);
	uint32_t pin_reg;
	uint64_t fl;

	if (!r)
		return -EINVAL;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r);
	pin_reg |= BIT32(OUTPUT_ENABLE_OFF);
	if (value)
		pin_reg |= BIT32(OUTPUT_VALUE_OFF);
	else
		pin_reg &= ~BIT32(OUTPUT_VALUE_OFF);
	pinctrl_writel(pin_reg, r);
	spin_unlock_irqrestore(&ag->lock, fl);
	return 0;
}

static int amd_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, offset);
	uint32_t pin_reg;
	uint64_t fl;

	if (!r)
		return -EINVAL;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r);
	spin_unlock_irqrestore(&ag->lock, fl);
	return !!(pin_reg & BIT32(PIN_STS_OFF));
}

static void amd_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, offset);
	uint32_t pin_reg;
	uint64_t fl;

	if (!r)
		return;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r);
	if (value)
		pin_reg |= BIT32(OUTPUT_VALUE_OFF);
	else
		pin_reg &= ~BIT32(OUTPUT_VALUE_OFF);
	pinctrl_writel(pin_reg, r);
	spin_unlock_irqrestore(&ag->lock, fl);
}

/* Debounce in microseconds, 0 off.  ag->lock held. */
static int amd_gpio_set_debounce(struct amd_gpio *ag, unsigned int offset,
				 unsigned int debounce)
{
	uint8_t *r = amd_gpio_pin_reg(ag, offset);
	uint32_t time, pin_reg;
	int ret = 0;

	lockdep_assert_held(&ag->lock);
	if (!r)
		return -EINVAL;
	/* GPIO 0 may be debounced by the hardware already. */
	if (offset == 0 && (pinctrl_readl(ag->base + WAKE_INT_MASTER_REG) &
			    INTERNAL_GPIO0_DEBOUNCE))
		debounce = 0;
	pin_reg = pinctrl_readl(r);
	if (debounce) {
		pin_reg |= DB_TYPE_REMOVE_GLITCH << DB_CNTRL_OFF;
		pin_reg &= ~DB_TMR_OUT_MASK;
		/*
		 * TmrLarge TmrOutUnit  Unit                  Max debounce
		 *   0        0         61 us (2 RtcClk)      976 us
		 *   0        1         244 us (8 RtcClk)     3.9 ms
		 *   1        0         15.6 ms (512 RtcClk)  250 ms
		 *   1        1         62.5 ms (2048 RtcClk) 1 s
		 */
		if (debounce < 61) {
			pin_reg |= 1;
			pin_reg &= ~BIT32(DB_TMR_OUT_UNIT_OFF);
			pin_reg &= ~BIT32(DB_TMR_LARGE_OFF);
		} else if (debounce < 976) {
			time = debounce / 61;
			pin_reg |= time & DB_TMR_OUT_MASK;
			pin_reg &= ~BIT32(DB_TMR_OUT_UNIT_OFF);
			pin_reg &= ~BIT32(DB_TMR_LARGE_OFF);
		} else if (debounce < 3900) {
			time = debounce / 244;
			pin_reg |= time & DB_TMR_OUT_MASK;
			pin_reg |= BIT32(DB_TMR_OUT_UNIT_OFF);
			pin_reg &= ~BIT32(DB_TMR_LARGE_OFF);
		} else if (debounce < 250000) {
			time = debounce / 15625;
			pin_reg |= time & DB_TMR_OUT_MASK;
			pin_reg &= ~BIT32(DB_TMR_OUT_UNIT_OFF);
			pin_reg |= BIT32(DB_TMR_LARGE_OFF);
		} else if (debounce < 1000000) {
			time = debounce / 62500;
			pin_reg |= time & DB_TMR_OUT_MASK;
			pin_reg |= BIT32(DB_TMR_OUT_UNIT_OFF);
			pin_reg |= BIT32(DB_TMR_LARGE_OFF);
		} else {
			pin_reg &= ~(DB_CNTRL_MASK << DB_CNTRL_OFF);
			ret = -EINVAL;
		}
	} else {
		pin_reg &= ~BIT32(DB_TMR_OUT_UNIT_OFF);
		pin_reg &= ~BIT32(DB_TMR_LARGE_OFF);
		pin_reg &= ~DB_TMR_OUT_MASK;
		pin_reg &= ~(DB_CNTRL_MASK << DB_CNTRL_OFF);
	}
	/* Pending status bits are not written back (that would clear an
	 * interrupt the handler has not seen yet). */
	pinctrl_writel(pin_reg & ~PIN_IRQ_PENDING, r);
	return ret;
}

/* Pulls are single enables (arg bit 0); there is no "no pull" setting,
 * so a pull the firmware set stays unless asked off explicitly. */
static int amd_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
			       enum pin_config_param param, uint32_t arg)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, offset);
	uint32_t pin_reg;
	uint64_t fl;
	int ret = 0;

	if (!r)
		return -EINVAL;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r) & ~PIN_IRQ_PENDING;
	switch (param) {
	case PIN_CONFIG_INPUT_DEBOUNCE:
		ret = amd_gpio_set_debounce(ag, offset, arg);
		spin_unlock_irqrestore(&ag->lock, fl);
		return ret;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		pin_reg &= ~BIT32(PULL_DOWN_ENABLE_OFF);
		pin_reg |= (arg & 1u) << PULL_DOWN_ENABLE_OFF;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		pin_reg &= ~BIT32(PULL_UP_ENABLE_OFF);
		pin_reg |= (arg & 1u) << PULL_UP_ENABLE_OFF;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		pin_reg &= ~(DRV_STRENGTH_SEL_MASK << DRV_STRENGTH_SEL_OFF);
		pin_reg |= (arg & DRV_STRENGTH_SEL_MASK) << DRV_STRENGTH_SEL_OFF;
		break;
	default:
		spin_unlock_irqrestore(&ag->lock, fl);
		return -ENOTSUPP;
	}
	pinctrl_writel(pin_reg, r);
	spin_unlock_irqrestore(&ag->lock, fl);
	return ret;
}

static void amd_gpio_describe(struct gpio_chip *gc, unsigned int offset,
			      char *buf, size_t len)
{
	uint8_t *r = amd_gpio_pin_reg(gc_to_amd(gc), offset);
	uint32_t v = r ? pinctrl_readl(r) : 0;

	ksnprintf(buf, len, "GPIO_%u reg %08x%s%s%s%s", offset, v,
		  (v & BIT32(INTERRUPT_ENABLE_OFF)) ? " irq-en" : "",
		  (v & BIT32(INTERRUPT_MASK_OFF)) ? " unmasked" : "",
		  (v & (DB_CNTRL_MASK << DB_CNTRL_OFF)) ? " debounce" : "",
		  (v & WAKE_SOURCE_SUSPEND) ? " wake" : "");
}

/* ---- interrupts ------------------------------------------------------------ */

static void amd_irq_ack(struct gpio_chip *gc, unsigned int hwirq)
{
	/* No ack before the handlers: the status is cleared by writing
	 * the pin register back after they ran (amd_irq_handler). */
	(void)gc;
	(void)hwirq;
}

static void amd_irq_mask_unmask(struct gpio_chip *gc, unsigned int hwirq,
				int mask)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, hwirq);
	uint32_t pin_reg;
	uint64_t fl;

	if (!r)
		return;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r) & ~PIN_IRQ_PENDING;
	if (mask)
		pin_reg &= ~BIT32(INTERRUPT_MASK_OFF);
	else
		pin_reg |= BIT32(INTERRUPT_MASK_OFF);
	pinctrl_writel(pin_reg, r);
	spin_unlock_irqrestore(&ag->lock, fl);
}

static void amd_irq_mask(struct gpio_chip *gc, unsigned int hwirq)
{
	amd_irq_mask_unmask(gc, hwirq, 1);
}

static void amd_irq_unmask(struct gpio_chip *gc, unsigned int hwirq)
{
	amd_irq_mask_unmask(gc, hwirq, 0);
}

static int amd_irq_set_type(struct gpio_chip *gc, unsigned int hwirq,
			    unsigned int type)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, hwirq);
	uint32_t pin_reg, pin_reg_irq_en, mask = BIT32(INTERRUPT_ENABLE_OFF);
	uint64_t fl;
	int ret = 0, us;

	if (!r)
		return -EINVAL;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r) & ~PIN_IRQ_PENDING;
	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		pin_reg &= ~BIT32(LEVEL_TRIG_OFF);
		pin_reg &= ~(ACTIVE_LEVEL_MASK << ACTIVE_LEVEL_OFF);
		pin_reg |= ACTIVE_HIGH << ACTIVE_LEVEL_OFF;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		pin_reg &= ~BIT32(LEVEL_TRIG_OFF);
		pin_reg &= ~(ACTIVE_LEVEL_MASK << ACTIVE_LEVEL_OFF);
		pin_reg |= ACTIVE_LOW << ACTIVE_LEVEL_OFF;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		pin_reg &= ~BIT32(LEVEL_TRIG_OFF);
		pin_reg &= ~(ACTIVE_LEVEL_MASK << ACTIVE_LEVEL_OFF);
		pin_reg |= BOTH_EDGES << ACTIVE_LEVEL_OFF;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		pin_reg |= LEVEL_TRIGGER << LEVEL_TRIG_OFF;
		pin_reg &= ~(ACTIVE_LEVEL_MASK << ACTIVE_LEVEL_OFF);
		pin_reg |= ACTIVE_HIGH << ACTIVE_LEVEL_OFF;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		pin_reg |= LEVEL_TRIGGER << LEVEL_TRIG_OFF;
		pin_reg &= ~(ACTIVE_LEVEL_MASK << ACTIVE_LEVEL_OFF);
		pin_reg |= ACTIVE_LOW << ACTIVE_LEVEL_OFF;
		break;
	case IRQ_TYPE_NONE:
		break;
	default:
		ret = -EINVAL;
		break;
	}
	/* Clear a stale status with the new configuration. */
	pin_reg |= CLR_INTR_STAT << INTERRUPT_STS_OFF;
	/* Enable (masked) and wait for the enable to read back: see the
	 * header.  The lock stays held so nothing else reads a stale
	 * enable bit meanwhile. */
	pin_reg_irq_en = pin_reg;
	pin_reg_irq_en |= mask;
	pin_reg_irq_en &= ~BIT32(INTERRUPT_MASK_OFF);
	pinctrl_writel(pin_reg_irq_en, r);
	for (us = 0; (pinctrl_readl(r) & mask) != mask; us++) {
		if (us >= AMD_SET_TYPE_POLL_US) {
			ret = -ETIMEDOUT;
			break;
		}
		pinctrl_udelay(1);
	}
	pinctrl_writel(pin_reg, r);
	spin_unlock_irqrestore(&ag->lock, fl);
	if (ret == -ETIMEDOUT)
		kprintf("%s: pin %u: interrupt enable did not settle\n",
			ag->label, hwirq);
	return ret;
}

static int amd_irq_set_wake(struct gpio_chip *gc, unsigned int hwirq,
			    unsigned int on)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, hwirq);
	uint32_t wake_mask = WAKE_SOURCE_SUSPEND;
	uint32_t pin_reg;
	uint64_t fl;

	if (!r)
		return -EINVAL;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r) & ~PIN_IRQ_PENDING;
	if (on)
		pin_reg |= wake_mask;
	else
		pin_reg &= ~wake_mask;
	pinctrl_writel(pin_reg, r);
	spin_unlock_irqrestore(&ag->lock, fl);
	if (ag->parent_irq > 0 && irq_set_irq_wake(ag->parent_irq, on) != 0)
		kprintf("%s: failed to %s wake-up interrupt\n", ag->label,
			on ? "enable" : "disable");
	return 0;
}

/* The line becomes an interrupt: status generation on, delivery masked
 * until it is unmasked. */
static int amd_irq_request(struct gpio_chip *gc, unsigned int hwirq)
{
	struct amd_gpio *ag = gc_to_amd(gc);
	uint8_t *r = amd_gpio_pin_reg(ag, hwirq);
	uint32_t pin_reg;
	uint64_t fl;

	if (!r)
		return -EINVAL;
	spin_lock_irqsave(&ag->lock, &fl);
	pin_reg = pinctrl_readl(r) & ~PIN_IRQ_PENDING;
	pin_reg |= BIT32(INTERRUPT_ENABLE_OFF);
	pin_reg &= ~BIT32(INTERRUPT_MASK_OFF);
	pinctrl_writel(pin_reg, r);
	spin_unlock_irqrestore(&ag->lock, fl);
	return 0;
}

static const struct gpio_irq_chip_ops g_amd_irq_ops = {
	.irq_mask = amd_irq_mask,
	.irq_unmask = amd_irq_unmask,
	.irq_ack = amd_irq_ack,
	.irq_set_type = amd_irq_set_type,
	.irq_set_wake = amd_irq_set_wake,
	.irq_request = amd_irq_request,
};

/* One pin whose summary bit is set: run it if it is pending and
 * delivered, then clear its status (reading the register again: the
 * handlers may have changed it).  A pin that is no interrupt here is
 * masked so it cannot storm.  Returns whether it was ours. */
static int amd_irq_pin(struct amd_gpio *ag, unsigned int pin)
{
	uint8_t *r = amd_gpio_pin_reg(ag, pin);
	uint32_t regval;
	int ours = 0;

	if (!r)
		return 0;
	regval = pinctrl_readl(r);
	if (!(regval & PIN_IRQ_PENDING) ||
	    !(regval & BIT32(INTERRUPT_MASK_OFF)))
		return 0;
	gpiochip_handle_irq(&ag->chip, pin);
	spin_lock(&ag->lock);
	regval = pinctrl_readl(r);
	if (!gpiochip_line_is_irq(&ag->chip, pin))
		regval &= ~BIT32(INTERRUPT_MASK_OFF);
	else
		ours = 1;
	pinctrl_writel(regval, r);
	spin_unlock(&ag->lock);
	return ours;
}

static int amd_irq_handler(void *arg)
{
	struct amd_gpio *ag = arg;
	uint64_t status;
	uint32_t regval;
	int handled = 0;

	spin_lock(&ag->lock);
	status = (uint64_t)pinctrl_readl(ag->base + WAKE_INT_STATUS_REG1) << 32;
	status |= pinctrl_readl(ag->base + WAKE_INT_STATUS_REG0);
	spin_unlock(&ag->lock);
	/* Bits 0-45: each covers four pins */
	status &= (1ull << 46) - 1;
	while (status) {
		unsigned int bit = (unsigned int)__builtin_ctzll(status);

		status &= status - 1;
		for (unsigned int i = 0; i < 4; i++)
			handled |= amd_irq_pin(ag, bit * 4 + i);
	}
	/* The remote block: one bit per pin, cleared with the pin. */
	if (ag->base_rgpio) {
		uint32_t st;

		spin_lock(&ag->lock);
		st = pinctrl_readl(ag->base_rgpio + WAKE_INT_STATUS_REG_RGPIO);
		spin_unlock(&ag->lock);
		st &= (1u << AMD_GPIO_PINS_BANK4) - 1;
		while (st) {
			unsigned int i = (unsigned int)__builtin_ctz(st);

			st &= st - 1;
			handled |= amd_irq_pin(ag, AMD_GPIO_RGPIO_PIN_BASE + i);
		}
	}
	/* End of interrupt to the GPIO unit (also when nothing was ours:
	 * the line is shared, and a status from before is gone now). */
	spin_lock(&ag->lock);
	regval = pinctrl_readl(ag->base + WAKE_INT_MASTER_REG);
	pinctrl_writel(regval | EOI_MASK, ag->base + WAKE_INT_MASTER_REG);
	spin_unlock(&ag->lock);
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/* ---- system sleep ---------------------------------------------------------- */

/* Pins in use here (requested or an interrupt) are saved; those that are
 * no wake source are masked over the sleep, and their debounce is off
 * (a debounced pin has been seen not to wake the system). */
static int amd_gpio_should_save(struct amd_gpio *ag, unsigned int pin)
{
	return ag->chip.lines &&
	       (gpiochip_line_is_requested(&ag->chip, pin) ||
		gpiochip_line_is_irq(&ag->chip, pin));
}

void amd_gpio_suspend(struct amd_gpio *ag, int hibernate);
void amd_gpio_suspend(struct amd_gpio *ag, int hibernate)
{
	uint32_t wake_mask = hibernate ? BIT32(WAKE_CNTRL_OFF_S4) :
					 WAKE_SOURCE_SUSPEND;
	uint64_t fl;

	if (!ag->saved_regs)
		return;
	for (unsigned int pin = 0; pin < ag->chip.ngpio; pin++) {
		uint8_t *r = amd_gpio_pin_reg(ag, pin);

		if (!r || !amd_gpio_should_save(ag, pin))
			continue;
		spin_lock_irqsave(&ag->lock, &fl);
		ag->saved_regs[pin] = pinctrl_readl(r) & ~PIN_IRQ_PENDING;
		if (!(ag->saved_regs[pin] & wake_mask))
			pinctrl_writel(ag->saved_regs[pin] &
					       ~BIT32(INTERRUPT_MASK_OFF),
				       r);
		if (ag->saved_regs[pin] & (DB_CNTRL_MASK << DB_CNTRL_OFF))
			(void)amd_gpio_set_debounce(ag, pin, 0);
		spin_unlock_irqrestore(&ag->lock, fl);
	}
}

void amd_gpio_resume(struct amd_gpio *ag);
void amd_gpio_resume(struct amd_gpio *ag)
{
	uint64_t fl;

	if (!ag->saved_regs)
		return;
	for (unsigned int pin = 0; pin < ag->chip.ngpio; pin++) {
		uint8_t *r = amd_gpio_pin_reg(ag, pin);

		if (!r || !amd_gpio_should_save(ag, pin))
			continue;
		spin_lock_irqsave(&ag->lock, &fl);
		/* A status that came in meanwhile is kept for the handler
		 * (written back as 0). */
		pinctrl_writel(ag->saved_regs[pin], r);
		spin_unlock_irqrestore(&ag->lock, fl);
	}
}

void amd_pinctrl_suspend_all(int hibernate);
void amd_pinctrl_suspend_all(int hibernate)
{
	mutex_lock(&g_amd_list_lock);
	for (struct list_head *p = g_amd_list.next; p != &g_amd_list;
	     p = p->next)
		amd_gpio_suspend(container_of(p, struct amd_gpio, node),
				 hibernate);
	mutex_unlock(&g_amd_list_lock);
}

void amd_pinctrl_resume_all(void);
void amd_pinctrl_resume_all(void)
{
	mutex_lock(&g_amd_list_lock);
	for (struct list_head *p = g_amd_list.next; p != &g_amd_list;
	     p = p->next)
		amd_gpio_resume(container_of(p, struct amd_gpio, node));
	mutex_unlock(&g_amd_list_lock);
}

/* ---- set-up --------------------------------------------------------------- */

/* Set up a controller over its mapped main window (`length' bytes) and
 * optional remote block; `has_aei': the firmware has GPIO events, so the
 * S4 wake enables are cleared too.  Used by the probe and the host test
 * harness. */
int amd_gpio_setup(struct amd_gpio *ag, uint8_t *base, uint64_t length,
		   uint8_t *base_rgpio, int has_aei);
int amd_gpio_setup(struct amd_gpio *ag, uint8_t *base, uint64_t length,
		   uint8_t *base_rgpio, int has_aei)
{
	uint32_t mask = WAKE_SOURCE_SUSPEND;
	uint64_t fl;

	if (!ag || !base || length < WAKE_INT_STATUS_REG1 + 4)
		return -EINVAL;
	/* The remote pins are numbered from 256: only with a main block
	 * of exactly 256 registers do the numbers line up. */
	if (base_rgpio && length / 4 != AMD_GPIO_RGPIO_PIN_BASE) {
		kprintf("%s: unexpected GPIO bank size, disabling RGPIO\n",
			ag->label[0] ? ag->label : "amd-gpio");
		base_rgpio = NULL;
	}
	ag->base = base;
	ag->base_rgpio = base_rgpio;
	ag->length = length;
	spinlock_init(&ag->lock, "amd_gpio");
	INIT_LIST_HEAD(&ag->node);
	if (!ag->label[0])
		ksnprintf(ag->label, sizeof(ag->label), "AMD GPIO");
	ag->chip.label = ag->label;
	ag->chip.ngpio = (unsigned int)(length / 4);
	if (ag->chip.ngpio > AMD_GPIO_RGPIO_PIN_BASE)
		ag->chip.ngpio = AMD_GPIO_RGPIO_PIN_BASE;
	if (ag->base_rgpio)
		ag->chip.ngpio += AMD_GPIO_PINS_BANK4;
	ag->chip.data = ag;
	ag->chip.get_direction = amd_gpio_get_direction;
	ag->chip.direction_input = amd_gpio_direction_input;
	ag->chip.direction_output = amd_gpio_direction_output;
	ag->chip.get = amd_gpio_get;
	ag->chip.set = amd_gpio_set;
	ag->chip.set_config = amd_gpio_set_config;
	ag->chip.valid = amd_gpio_valid;
	ag->chip.describe = amd_gpio_describe;
	ag->chip.irq = &g_amd_irq_ops;
	ag->saved_regs = kcalloc(ag->chip.ngpio, sizeof(uint32_t));
	/* Platforms without _AEI leave GPIO wake sources to the firmware:
	 * their S4 wake bits stay. */
	if (has_aei)
		mask |= BIT32(WAKE_CNTRL_OFF_S4);
	spin_lock_irqsave(&ag->lock, &fl);
	for (unsigned int pin = 0; pin < ag->chip.ngpio; pin++) {
		uint8_t *r = amd_gpio_pin_reg(ag, pin);
		uint32_t v;

		if (!r)
			continue;
		v = pinctrl_readl(r);
		if (!(v & mask))
			continue;
		pinctrl_writel(v & ~mask & ~PIN_IRQ_PENDING, r);
	}
	spin_unlock_irqrestore(&ag->lock, fl);
	return 0;
}

/* The memory resource the _DSD names `name' in "pinctrl-resource-names",
 * mapped, or NULL. */
static uint8_t *amd_get_named_res(struct acpi_device *adev,
				  const struct acpi_res_list *res,
				  const char *name, uint64_t minsz)
{
	const struct acpi_res *r;
	uint64_t pages, virt;
	int index = acpi_dev_property_match_string(
		adev->handle, "pinctrl-resource-names", name);

	if (index < 0)
		return NULL;
	r = acpi_res_find(res, ACPI_RES_MEM, index);
	if (!r || !r->mem.length)
		return NULL;
	if (r->mem.length < minsz) {
		kprintf("amd-gpio: %s: %s resource too small\n", adev->path,
			name);
		return NULL;
	}
	pages = ((r->mem.start & 0xfffull) + r->mem.length + 0xfffull) >> 12;
	virt = mm_map_device_mmio(r->mem.start, (size_t)pages);
	return (uint8_t *)virt;
}

static int amd_probe(struct acpi_device *adev)
{
	const struct acpi_res *mem, *irqres;
	struct acpi_res_list res;
	struct amd_gpio *ag;
	uint8_t *rgpio;
	uint64_t virt, pages, len;
	int irq;

	if (gpiochip_find_by_acpi_handle(adev->handle))
		return 0;
	if (acpi_dev_get_resources(adev, &res))
		return -ENODEV;
	mem = acpi_res_find(&res, ACPI_RES_MEM, 0);
	if (!mem || !mem->mem.length) {
		kprintf("amd-gpio: %s: no GPIO memory resource\n", adev->path);
		acpi_dev_free_resource_list(&res);
		return -ENODEV;
	}
	len = mem->mem.length;
	pages = ((mem->mem.start & 0xfffull) + len + 0xfffull) >> 12;
	virt = mm_map_device_mmio(mem->mem.start, (size_t)pages);
	ag = virt ? kcalloc(1, sizeof(*ag)) : NULL;
	if (!ag) {
		acpi_dev_free_resource_list(&res);
		return -ENOMEM;
	}
	ksnprintf(ag->label, sizeof(ag->label), "%s:%s", adev->hid,
		  adev->has_uid ? adev->uid : "0");
	rgpio = amd_get_named_res(adev, &res, "rgpio",
				  WAKE_INT_STATUS_REG_RGPIO + 4);
	if (amd_gpio_setup(ag, (uint8_t *)virt, len, rgpio,
			   acpi_has_method(adev->handle, "_AEI")) != 0) {
		kprintf("%s: register window %#lx too small\n", ag->label,
			(unsigned long)len);
		kfree(ag);
		acpi_dev_free_resource_list(&res);
		return -ENODEV;
	}
	ag->adev = acpi_dev_get(adev);
	ag->chip.acpi_handle = adev->handle;
	irqres = acpi_res_find(&res, ACPI_RES_IRQ, 0);
	irq = -ENOENT;
	if (irqres) {
		irq = irq_alloc_desc_gsi(NULL, ag, (int)irqres->irq.gsi,
					 irqres->irq.polarity ==
						 ACPI_RES_ACTIVE_LOW,
					 irqres->irq.triggering ==
						 ACPI_RES_LEVEL);
		if (irq > 0 && request_irq(irq, amd_irq_handler, IRQF_SHARED,
					   ag->label, ag) != 0) {
			irq_free_desc(irq);
			irq = -EBUSY;
		}
	}
	if (irq > 0)
		ag->parent_irq = irq;
	else
		ag->chip.irq = NULL;
	if (gpiochip_add(&ag->chip) != 0) {
		if (ag->parent_irq > 0) {
			(void)free_irq(ag->parent_irq, ag);
			irq_free_desc(ag->parent_irq);
		}
		acpi_dev_put(ag->adev);
		kfree(ag->saved_regs);
		kfree(ag);
		acpi_dev_free_resource_list(&res);
		return -ENODEV;
	}
	mutex_lock(&g_amd_list_lock);
	list_add_tail(&ag->node, &g_amd_list);
	mutex_unlock(&g_amd_list_lock);
	kprintf("%s: %u GPIOs%s%s\n", ag->label, ag->chip.ngpio,
		ag->base_rgpio ? " (with remote block)" : "",
		ag->parent_irq > 0 ? ", interrupts" : "");
	acpi_dev_free_resource_list(&res);
	return 0;
}

void amd_pinctrl_init(void);
void amd_pinctrl_init(void)
{
	static const struct acpi_device_id ids[] = {
		{ "AMD0030", 0 },
		{ "AMDI0030", 0 },
		{ "AMDI0031", 0 },
		{ "AMDI0033", 0 },
		{ "", 0 },
	};
	struct acpi_device *devs[8];
	int n;

	might_sleep();
	if (!acpi_dev_ready())
		return;
	n = acpi_dev_collect_matches(ids, 1, devs, 8);
	for (int i = 0; i < n; i++) {
		(void)amd_probe(devs[i]);
		acpi_dev_put(devs[i]);
	}
}
