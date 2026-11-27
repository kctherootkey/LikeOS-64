// LikeOS pinctrl - Intel Bay Trail GPIO controller
//
// Bay Trail (Atom Z3700/E3800, Celeron/Pentium N2800/J2900) has three
// GPIO banks, each its own ACPI device INT33FC with _UID "1" (SCORE, 102
// pads), "2" (NCORE, 28 pads) and "3" (SUS, 44 pads), one memory resource
// and one interrupt each.  The register design predates the community
// controllers:
//
//   * every pad has 16 bytes of registers -- CONF0 (+0), CONF1 (+4), VAL
//     (+8), DFT (+12) -- at an offset given by a per-bank pad map (the GPIO
//     number is NOT the pad's position in the register block);
//   * CONF0: 31 IODEN, 27 DIRECT_IRQ_EN (pad wired to an APIC line, not to
//     the bank's interrupt), 26 TRIG_NEG, 25 TRIG_POS, 24 TRIG_LVL, 20
//     DEBOUNCE_EN, 19 GLITCH_FILTER_EN, 17/16 glitch filter clocks, 10:9
//     pull strength (2k/10k/20k/40k), 8:7 pull assignment (1 up, 2
//     down), 2:0 function (GPIO is function 0, or 1 on a few pads);
//   * VAL: 2 INPUT_EN (active low), 1 OUTPUT_EN (active low), 0 LEVEL;
//   * interrupt status: one bit per GPIO number at 0x800 (32 per
//     register, write 1 to clear); the trigger bits in CONF0 are also the
//     enable: a pad is masked by clearing TRIG_POS/NEG/LVL and unmasked by
//     setting the bits of its type (level: TRIG_LVL plus POS for high or
//     NEG for low);
//   * debounce: one pulse width per bank at 0x9d0 (3 bits: 375 us .. 24
//     ms), enabled per pad with CONF0.DEBOUNCE_EN.
//
// Pads with DIRECT_IRQ_EN set are excluded from the bank's interrupts when
// the firmware set them up consistently (the pad is in the direct IRQ mux
// at 0x980 and has a level trigger); otherwise the flag is a firmware bug
// and is cleared.
//
// All banks share one register lock: the hardware does not take
// concurrent accesses to its GPIO banks.  System sleep keeps the mux,
// trigger and direct-IRQ bits and the direction/level of every pad
// (byt_gpio_suspend/resume).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2012-2013 Intel Corporation

#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define BYT_CONF0_REG 0x000
#define BYT_CONF1_REG 0x004
#define BYT_VAL_REG 0x008
#define BYT_DFT_REG 0x00c
#define BYT_INT_STAT_REG 0x800
#define BYT_DIRECT_IRQ_REG 0x980
#define BYT_DEBOUNCE_REG 0x9d0

#define BYT_IODEN (1u << 31)
#define BYT_DIRECT_IRQ_EN (1u << 27)
#define BYT_TRIG_NEG (1u << 26)
#define BYT_TRIG_POS (1u << 25)
#define BYT_TRIG_LVL (1u << 24)
#define BYT_TRIG_MASK (BYT_TRIG_NEG | BYT_TRIG_POS | BYT_TRIG_LVL)
#define BYT_DEBOUNCE_EN (1u << 20)
#define BYT_GLITCH_FILTER_EN (1u << 19)
#define BYT_GLITCH_F_SLOW_CLK (1u << 17)
#define BYT_GLITCH_F_FAST_CLK (1u << 16)
#define BYT_PULL_STR_SHIFT 9
#define BYT_PULL_STR_MASK (3u << 9)
#define BYT_PULL_STR_2K (0u << 9)
#define BYT_PULL_STR_10K (1u << 9)
#define BYT_PULL_STR_20K (2u << 9)
#define BYT_PULL_STR_40K (3u << 9)
#define BYT_PULL_ASSIGN_MASK (3u << 7)
#define BYT_PULL_ASSIGN_UP (1u << 7)
#define BYT_PULL_ASSIGN_DOWN (2u << 7)
#define BYT_PIN_MUX 0x07u

#define BYT_INPUT_EN (1u << 2) /* 0 = enabled */
#define BYT_OUTPUT_EN (1u << 1) /* 0 = enabled */
#define BYT_LEVEL (1u << 0)
#define BYT_DIR_MASK (BYT_INPUT_EN | BYT_OUTPUT_EN)

#define BYT_DEBOUNCE_PULSE_MASK 0x7u

#define BYT_CONF0_RESTORE_MASK (BYT_DIRECT_IRQ_EN | BYT_TRIG_MASK | BYT_PIN_MUX)
#define BYT_VAL_RESTORE_MASK (BYT_DIR_MASK | BYT_LEVEL)

#define BYT_DEFAULT_GPIO_MUX 0u
#define BYT_ALTER_GPIO_MUX 1u

#define BYT_SCORE_UID "1"
#define BYT_NCORE_UID "2"
#define BYT_SUS_UID "3"

struct byt_bank {
	const char *uid;
	const char *name;
	const uint8_t *pad_map; /* GPIO number -> register block index */
	unsigned int npins;
};

/* Register block index (x16 bytes) of each GPIO number. */
static const uint8_t byt_score_map[] = {
	85,  89,  93,  96,  99,	 102, 98,  101, 34,  37,  36,  38,  39,	 35,
	40,  84,  62,  61,  64,	 59,  54,  56,	60,  55,  63,  57,  51,	 50,
	53,  47,  52,  49,  48,	 43,  46,  41,	45,  42,  58,  44,  95,	 105,
	70,  68,  67,  66,  69,	 71,  65,  72,	86,  90,  88,  92,  103, 77,
	79,  83,  78,  81,  80,	 82,  13,  12,	15,  14,  17,  18,  19,	 16,
	2,   1,	  0,   4,   6,	 7,   9,   8,	33,  32,  31,  30,  29,	 27,
	25,  28,  26,  23,  21,	 20,  24,  22,	5,   3,	  10,  11,  106, 87,
	91,  104, 97,  100,
};

static const uint8_t byt_ncore_map[] = {
	19, 18, 17, 20, 21, 22, 24, 25, 23, 16, 14, 15, 12, 26,
	27, 1,	4,  8,	11, 0,	3,  6,	10, 13, 2,  5,	9,  7,
};

static const uint8_t byt_sus_map[] = {
	29, 33, 30, 31, 32, 34, 36, 35, 38, 37, 18, 7,	11, 20, 17,
	1,  8,	10, 19, 12, 0,	2,  23, 39, 28, 27, 22, 21, 24, 25,
	26, 51, 56, 54, 49, 55, 48, 57, 50, 58, 52, 53, 59, 40,
};

static const struct byt_bank g_byt_banks[] = {
	{ BYT_SCORE_UID, "Bay Trail SCORE", byt_score_map,
	  sizeof(byt_score_map) },
	{ BYT_NCORE_UID, "Bay Trail NCORE", byt_ncore_map,
	  sizeof(byt_ncore_map) },
	{ BYT_SUS_UID, "Bay Trail SUS", byt_sus_map, sizeof(byt_sus_map) },
};

struct byt_pad_context {
	uint32_t conf0;
	uint32_t val;
};

struct byt_gpio {
	struct gpio_chip chip;
	const struct byt_bank *bank;
	struct acpi_device *adev;
	uint8_t *regs;
	int parent_irq;
	uint32_t direct_irq_mask[4]; /* pads excluded from the bank irq */
	unsigned int irq_type[128]; /* last set_type, applied on unmask */
	struct byt_pad_context ctx[128]; /* over system sleep */
	struct list_head node;
	char label[40];
};

/* One lock for the registers of every bank. */
static spinlock_t g_byt_lock = SPINLOCK_INIT("byt_gpio");
static DEFINE_MUTEX(g_byt_list_lock);
static struct list_head g_byt_list = LIST_HEAD_INIT(g_byt_list);

static struct byt_gpio *gc_to_byt(struct gpio_chip *gc)
{
	return container_of(gc, struct byt_gpio, chip);
}

static uint8_t *byt_reg(struct byt_gpio *vg, unsigned int offset,
			unsigned int reg)
{
	if (offset >= vg->bank->npins)
		return NULL;
	if (reg == BYT_INT_STAT_REG)
		return vg->regs + reg + (offset / 32) * 4;
	if (reg == BYT_DEBOUNCE_REG)
		return vg->regs + reg;
	return vg->regs + (unsigned int)vg->bank->pad_map[offset] * 16 + reg;
}

/* The function number that is GPIO for this pad. */
static uint32_t byt_gpio_mux(const struct byt_gpio *vg, unsigned int offset)
{
	if (!kstrcmp(vg->bank->uid, BYT_SCORE_UID) && offset >= 92 &&
	    offset <= 93)
		return BYT_ALTER_GPIO_MUX;
	if (!kstrcmp(vg->bank->uid, BYT_SUS_UID) && offset >= 11 &&
	    offset <= 21)
		return BYT_ALTER_GPIO_MUX;
	return BYT_DEFAULT_GPIO_MUX;
}

/* Disarm a pad's trigger, unless the pad is a direct interrupt (whose
 * trigger the APIC line needs).  g_byt_lock held. */
static void byt_gpio_clear_triggering(struct byt_gpio *vg, unsigned int offset)
{
	uint8_t *reg = byt_reg(vg, offset, BYT_CONF0_REG);
	uint32_t v;

	lockdep_assert_held(&g_byt_lock);
	if (!reg)
		return;
	v = pinctrl_readl(reg);
	if (!(v & BYT_DIRECT_IRQ_EN))
		v &= ~BYT_TRIG_MASK;
	pinctrl_writel(v, reg);
}

/* ---- GPIO operations ---------------------------------------------------- */

static void byt_gpio_free(struct gpio_chip *gc, unsigned int offset)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint64_t fl;

	/* A line freed while still an interrupt keeps its trigger. */
	if (gpiochip_line_is_irq(gc, offset))
		return;
	spin_lock_irqsave(&g_byt_lock, &fl);
	byt_gpio_clear_triggering(vg, offset);
	spin_unlock_irqrestore(&g_byt_lock, fl);
}

static int byt_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *reg = byt_reg(vg, offset, BYT_CONF0_REG);
	uint32_t v, mux = byt_gpio_mux(vg, offset);
	uint64_t fl;

	if (!reg)
		return -EINVAL;
	spin_lock_irqsave(&g_byt_lock, &fl);
	v = pinctrl_readl(reg);
	if ((v & BYT_PIN_MUX) != mux) {
		/* The firmware left a pad that a GPIO consumer was given
		 * in another function. */
		pinctrl_writel((v & ~BYT_PIN_MUX) | mux, reg);
		kprintf("%s: pin %u forcibly reconfigured as GPIO\n",
			vg->label, offset);
	}
	spin_unlock_irqrestore(&g_byt_lock, fl);
	return 0;
}

static int byt_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *reg = byt_reg(vg, offset, BYT_VAL_REG);
	uint32_t v;

	if (!reg)
		return -EINVAL;
	v = pinctrl_readl(reg);
	if (!(v & BYT_OUTPUT_EN))
		return GPIO_LINE_DIRECTION_OUT;
	if (!(v & BYT_INPUT_EN))
		return GPIO_LINE_DIRECTION_IN;
	return -EINVAL;
}

static int byt_gpio_set_direction(struct byt_gpio *vg, unsigned int offset,
				  int input)
{
	uint8_t *val = byt_reg(vg, offset, BYT_VAL_REG);
	uint8_t *conf = byt_reg(vg, offset, BYT_CONF0_REG);
	uint64_t fl;
	uint32_t v;

	if (!val || !conf)
		return -EINVAL;
	spin_lock_irqsave(&g_byt_lock, &fl);
	v = pinctrl_readl(val) & ~BYT_DIR_MASK;
	if (input) {
		v |= BYT_OUTPUT_EN; /* output disabled, input enabled */
	} else if (pinctrl_readl(conf) & BYT_DIRECT_IRQ_EN) {
		/* An output on a pad wired to an APIC line is a firmware
		 * bug; keep the input enabled so the line still reads. */
		kprintf("%s: pin %u has DIRECT_IRQ_EN but is made an output\n",
			vg->label, offset);
	}
	pinctrl_writel(v, val);
	spin_unlock_irqrestore(&g_byt_lock, fl);
	return 0;
}

static int byt_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	return byt_gpio_set_direction(gc_to_byt(gc), offset, 1);
}

static void byt_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *reg = byt_reg(vg, offset, BYT_VAL_REG);
	uint64_t fl;
	uint32_t v;

	if (!reg)
		return;
	spin_lock_irqsave(&g_byt_lock, &fl);
	v = pinctrl_readl(reg);
	if (value)
		v |= BYT_LEVEL;
	else
		v &= ~BYT_LEVEL;
	pinctrl_writel(v, reg);
	spin_unlock_irqrestore(&g_byt_lock, fl);
}

static int byt_gpio_direction_output(struct gpio_chip *gc,
				     unsigned int offset, int value)
{
	byt_gpio_set(gc, offset, value);
	return byt_gpio_set_direction(gc_to_byt(gc), offset, 0);
}

static int byt_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *reg = byt_reg(vg, offset, BYT_VAL_REG);

	if (!reg)
		return -EINVAL;
	return !!(pinctrl_readl(reg) & BYT_LEVEL);
}

static int byt_set_pull_strength(uint32_t *conf, uint32_t ohms)
{
	*conf &= ~BYT_PULL_STR_MASK;
	switch (ohms) {
	case 2000:
		*conf |= BYT_PULL_STR_2K;
		return 0;
	case 10000:
		*conf |= BYT_PULL_STR_10K;
		return 0;
	case 20000:
		*conf |= BYT_PULL_STR_20K;
		return 0;
	case 40000:
		*conf |= BYT_PULL_STR_40K;
		return 0;
	default:
		return -EINVAL;
	}
}

static int byt_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
			       enum pin_config_param param, uint32_t arg)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *conf_reg = byt_reg(vg, offset, BYT_CONF0_REG);
	uint8_t *val_reg = byt_reg(vg, offset, BYT_VAL_REG);
	uint8_t *db_reg = byt_reg(vg, offset, BYT_DEBOUNCE_REG);
	uint32_t conf, val, db;
	uint64_t fl;
	int rc = 0;

	if (!conf_reg || !val_reg)
		return -EINVAL;
	spin_lock_irqsave(&g_byt_lock, &fl);
	conf = pinctrl_readl(conf_reg);
	val = pinctrl_readl(val_reg);
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		conf &= ~BYT_PULL_ASSIGN_MASK;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (arg == 1)
			arg = 2000; /* the default strength */
		/* Pulls only act on an input: make it one. */
		if (val & BYT_INPUT_EN) {
			val &= ~BYT_INPUT_EN;
			pinctrl_writel(val, val_reg);
		}
		conf &= ~BYT_PULL_ASSIGN_MASK;
		conf |= param == PIN_CONFIG_BIAS_PULL_UP ?
				BYT_PULL_ASSIGN_UP :
				BYT_PULL_ASSIGN_DOWN;
		rc = byt_set_pull_strength(&conf, arg);
		break;
	case PIN_CONFIG_INPUT_DEBOUNCE:
		if (arg == 0) {
			conf &= ~BYT_DEBOUNCE_EN;
			break;
		}
		db = pinctrl_readl(db_reg) & ~BYT_DEBOUNCE_PULSE_MASK;
		switch (arg) {
		case 375:
			db |= 1;
			break;
		case 750:
			db |= 2;
			break;
		case 1500:
			db |= 3;
			break;
		case 3000:
			db |= 4;
			break;
		case 6000:
			db |= 5;
			break;
		case 12000:
			db |= 6;
			break;
		case 24000:
			db |= 7;
			break;
		default:
			rc = -EINVAL;
			break;
		}
		if (!rc) {
			pinctrl_writel(db, db_reg);
			conf |= BYT_DEBOUNCE_EN;
		}
		break;
	default:
		rc = -ENOTSUPP;
		break;
	}
	if (!rc)
		pinctrl_writel(conf, conf_reg);
	spin_unlock_irqrestore(&g_byt_lock, fl);
	return rc;
}

static int byt_gpio_valid(struct gpio_chip *gc, unsigned int offset)
{
	return offset < gc_to_byt(gc)->bank->npins;
}

static void byt_gpio_describe(struct gpio_chip *gc, unsigned int offset,
			      char *buf, size_t len)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *c0 = byt_reg(vg, offset, BYT_CONF0_REG);
	uint8_t *v = byt_reg(vg, offset, BYT_VAL_REG);

	ksnprintf(buf, len, "conf0 %08x val %08x", c0 ? pinctrl_readl(c0) : 0,
		  v ? pinctrl_readl(v) : 0);
}

/* ---- interrupts ------------------------------------------------------------ */

static void byt_irq_ack(struct gpio_chip *gc, unsigned int hwirq)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *reg = byt_reg(vg, hwirq, BYT_INT_STAT_REG);
	uint64_t fl;

	if (!reg)
		return;
	spin_lock_irqsave(&g_byt_lock, &fl);
	pinctrl_writel(1u << (hwirq % 32), reg);
	spin_unlock_irqrestore(&g_byt_lock, fl);
}

static void byt_irq_mask(struct gpio_chip *gc, unsigned int hwirq)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint64_t fl;

	/* The trigger bits are the enable. */
	spin_lock_irqsave(&g_byt_lock, &fl);
	byt_gpio_clear_triggering(vg, hwirq);
	spin_unlock_irqrestore(&g_byt_lock, fl);
}

static void byt_irq_unmask(struct gpio_chip *gc, unsigned int hwirq)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *reg = byt_reg(vg, hwirq, BYT_CONF0_REG);
	uint64_t fl;
	uint32_t v;

	if (!reg || hwirq >= 128)
		return;
	spin_lock_irqsave(&g_byt_lock, &fl);
	v = pinctrl_readl(reg);
	switch (vg->irq_type[hwirq]) {
	case IRQ_TYPE_LEVEL_HIGH:
		v |= BYT_TRIG_LVL | BYT_TRIG_POS;
		break;
	case IRQ_TYPE_EDGE_RISING:
		v |= BYT_TRIG_POS;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		v |= BYT_TRIG_LVL | BYT_TRIG_NEG;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		v |= BYT_TRIG_NEG;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		v |= BYT_TRIG_NEG | BYT_TRIG_POS;
		break;
	default:
		break;
	}
	pinctrl_writel(v, reg);
	spin_unlock_irqrestore(&g_byt_lock, fl);
}

static int byt_irq_set_type(struct gpio_chip *gc, unsigned int hwirq,
			    unsigned int type)
{
	struct byt_gpio *vg = gc_to_byt(gc);
	uint8_t *reg = byt_reg(vg, hwirq, BYT_CONF0_REG);
	uint64_t fl;
	uint32_t v;

	if (!reg || hwirq >= 128)
		return -EINVAL;
	spin_lock_irqsave(&g_byt_lock, &fl);
	v = pinctrl_readl(reg);
	if (v & BYT_DIRECT_IRQ_EN)
		kprintf("%s: pin %u: DIRECT_IRQ_EN cleared for the bank interrupt\n",
			vg->label, hwirq);
	/* The trigger bits are set on unmask (they are the enable). */
	v &= ~(BYT_DIRECT_IRQ_EN | BYT_TRIG_MASK);
	v |= BYT_GLITCH_FILTER_EN | BYT_GLITCH_F_SLOW_CLK |
	     BYT_GLITCH_F_FAST_CLK;
	pinctrl_writel(v, reg);
	vg->irq_type[hwirq] = type;
	spin_unlock_irqrestore(&g_byt_lock, fl);
	return 0;
}

static int byt_irq_set_wake(struct gpio_chip *gc, unsigned int hwirq,
			    unsigned int on)
{
	struct byt_gpio *vg = gc_to_byt(gc);

	(void)hwirq;
	return vg->parent_irq > 0 ? irq_set_irq_wake(vg->parent_irq, on) : 0;
}

static int byt_irq_request(struct gpio_chip *gc, unsigned int hwirq)
{
	struct byt_gpio *vg = gc_to_byt(gc);

	if (hwirq >= vg->bank->npins)
		return -EINVAL;
	if (vg->direct_irq_mask[hwirq / 32] & (1u << (hwirq % 32))) {
		kprintf("%s: pin %u is wired to an APIC line, not the bank\n",
			vg->label, hwirq);
		return -EBUSY;
	}
	return 0;
}

static const struct gpio_irq_chip_ops g_byt_irq_ops = {
	.irq_mask = byt_irq_mask,
	.irq_unmask = byt_irq_unmask,
	.irq_ack = byt_irq_ack,
	.irq_set_type = byt_irq_set_type,
	.irq_set_wake = byt_irq_set_wake,
	.irq_request = byt_irq_request,
};

static int byt_irq_handler(void *arg)
{
	struct byt_gpio *vg = arg;
	int handled = 0;

	for (unsigned int base = 0; base < vg->bank->npins; base += 32) {
		uint8_t *reg = vg->regs + BYT_INT_STAT_REG + (base / 32) * 4;
		uint32_t pending;

		spin_lock(&g_byt_lock);
		pending = pinctrl_readl(reg);
		spin_unlock(&g_byt_lock);
		while (pending) {
			unsigned int bit = (unsigned int)__builtin_ctz(pending);

			pending &= pending - 1;
			if (base + bit >= vg->bank->npins)
				continue;
			handled = 1;
			gpiochip_handle_irq(&vg->chip, base + bit);
		}
	}
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/* Is a pad's DIRECT_IRQ_EN set up consistently: the pad is in the
 * direct IRQ mux (16 bytes at 0x980, a pad number per APIC input) and
 * its trigger is a level one -- the trigger block's output goes to the
 * APIC pin as it is, and an edge would never be cleared (nothing writes
 * the status register for it).  g_byt_lock held. */
static int byt_direct_irq_sanity_check(struct byt_gpio *vg, unsigned int pin,
				       uint32_t conf0)
{
	uint32_t trig;
	int found = -1;

	lockdep_assert_held(&g_byt_lock);
	for (unsigned int i = 0; i < 16 && found < 0; i++) {
		uint32_t w = pinctrl_readl(vg->regs + BYT_DIRECT_IRQ_REG +
					   (i & ~3u));

		if (((w >> ((i & 3u) * 8)) & 0xffu) == pin)
			found = (int)i;
	}
	if (found < 0) {
		kprintf("%s: firmware bug: pin %u: DIRECT_IRQ_EN set but no IRQ assigned, clearing\n",
			vg->label, pin);
		return 0;
	}
	trig = conf0 & BYT_TRIG_MASK;
	if (trig != (BYT_TRIG_POS | BYT_TRIG_LVL) &&
	    trig != (BYT_TRIG_NEG | BYT_TRIG_LVL)) {
		kprintf("%s: firmware bug: pin %u: DIRECT_IRQ_EN set without trigger (CONF0 %#08x), clearing\n",
			vg->label, pin, conf0);
		return 0;
	}
	return 1;
}

/* Disarm every GPIO-function pad's trigger and clear the status.  Pads
 * the firmware wired to an APIC line stay out of the bank's interrupts;
 * broken direct-IRQ setups are undone. */
static void byt_irq_init_hw(struct byt_gpio *vg)
{
	uint64_t fl;

	spin_lock_irqsave(&g_byt_lock, &fl);
	for (unsigned int i = 0; i < vg->bank->npins; i++) {
		uint8_t *reg = byt_reg(vg, i, BYT_CONF0_REG);
		uint32_t v = pinctrl_readl(reg);

		if (v & BYT_DIRECT_IRQ_EN) {
			if (byt_direct_irq_sanity_check(vg, i, v)) {
				vg->direct_irq_mask[i / 32] |= 1u << (i % 32);
			} else {
				v &= ~(BYT_DIRECT_IRQ_EN | BYT_TRIG_MASK);
				pinctrl_writel(v, reg);
			}
		} else if ((v & BYT_PIN_MUX) == byt_gpio_mux(vg, i)) {
			byt_gpio_clear_triggering(vg, i);
		}
	}
	for (unsigned int base = 0; base < vg->bank->npins; base += 32) {
		uint8_t *reg = vg->regs + BYT_INT_STAT_REG + (base / 32) * 4;

		pinctrl_writel(0xffffffffu, reg);
		if (pinctrl_readl(reg))
			kprintf("%s: interrupt status %u stays %08x: pins misconfigured\n",
				vg->label, base / 32, pinctrl_readl(reg));
	}
	spin_unlock_irqrestore(&g_byt_lock, fl);
}

/* ---- probe -------------------------------------------------------------- */

/* Shared by the harness: set up a bank over a mapped register block. */
int byt_gpio_setup(struct byt_gpio *vg, const struct byt_bank *bank,
		   uint8_t *regs);
int byt_gpio_setup(struct byt_gpio *vg, const struct byt_bank *bank,
		   uint8_t *regs)
{
	if (!vg || !bank || !regs)
		return -EINVAL;
	vg->bank = bank;
	vg->regs = regs;
	INIT_LIST_HEAD(&vg->node);
	if (!vg->label[0])
		ksnprintf(vg->label, sizeof(vg->label), "INT33FC:%s (%s)",
			  bank->uid, bank->name);
	vg->chip.label = vg->label;
	vg->chip.ngpio = bank->npins;
	vg->chip.data = vg;
	vg->chip.request = byt_gpio_request;
	vg->chip.free = byt_gpio_free;
	vg->chip.get_direction = byt_gpio_get_direction;
	vg->chip.direction_input = byt_gpio_direction_input;
	vg->chip.direction_output = byt_gpio_direction_output;
	vg->chip.get = byt_gpio_get;
	vg->chip.set = byt_gpio_set;
	vg->chip.set_config = byt_gpio_set_config;
	vg->chip.valid = byt_gpio_valid;
	vg->chip.describe = byt_gpio_describe;
	vg->chip.irq = &g_byt_irq_ops;
	byt_irq_init_hw(vg);
	return 0;
}

/* System sleep: the mux, trigger and direct-IRQ bits and the direction
 * and level of every pad. */
void byt_gpio_suspend(struct byt_gpio *vg);
void byt_gpio_suspend(struct byt_gpio *vg)
{
	uint64_t fl;

	spin_lock_irqsave(&g_byt_lock, &fl);
	for (unsigned int i = 0; i < vg->bank->npins; i++) {
		vg->ctx[i].conf0 = pinctrl_readl(byt_reg(vg, i, BYT_CONF0_REG)) &
				   BYT_CONF0_RESTORE_MASK;
		vg->ctx[i].val = pinctrl_readl(byt_reg(vg, i, BYT_VAL_REG)) &
				 BYT_VAL_RESTORE_MASK;
	}
	spin_unlock_irqrestore(&g_byt_lock, fl);
}

void byt_gpio_resume(struct byt_gpio *vg);
void byt_gpio_resume(struct byt_gpio *vg)
{
	uint64_t fl;

	spin_lock_irqsave(&g_byt_lock, &fl);
	for (unsigned int i = 0; i < vg->bank->npins; i++) {
		uint8_t *reg = byt_reg(vg, i, BYT_CONF0_REG);
		uint32_t value = pinctrl_readl(reg);

		if ((value & BYT_CONF0_RESTORE_MASK) != vg->ctx[i].conf0) {
			value &= ~BYT_CONF0_RESTORE_MASK;
			value |= vg->ctx[i].conf0;
			pinctrl_writel(value, reg);
		}
		reg = byt_reg(vg, i, BYT_VAL_REG);
		value = pinctrl_readl(reg);
		if ((value & BYT_VAL_RESTORE_MASK) != vg->ctx[i].val) {
			value &= ~BYT_VAL_RESTORE_MASK;
			value |= vg->ctx[i].val;
			pinctrl_writel(value, reg);
		}
	}
	spin_unlock_irqrestore(&g_byt_lock, fl);
}

void byt_pinctrl_suspend_all(void);
void byt_pinctrl_suspend_all(void)
{
	mutex_lock(&g_byt_list_lock);
	for (struct list_head *p = g_byt_list.next; p != &g_byt_list;
	     p = p->next)
		byt_gpio_suspend(container_of(p, struct byt_gpio, node));
	mutex_unlock(&g_byt_list_lock);
}

void byt_pinctrl_resume_all(void);
void byt_pinctrl_resume_all(void)
{
	mutex_lock(&g_byt_list_lock);
	for (struct list_head *p = g_byt_list.next; p != &g_byt_list;
	     p = p->next)
		byt_gpio_resume(container_of(p, struct byt_gpio, node));
	mutex_unlock(&g_byt_list_lock);
}

const struct byt_bank *byt_bank_by_uid(const char *uid);
const struct byt_bank *byt_bank_by_uid(const char *uid)
{
	for (unsigned int i = 0; i < sizeof(g_byt_banks) / sizeof(g_byt_banks[0]);
	     i++)
		if (uid && !kstrcmp(uid, g_byt_banks[i].uid))
			return &g_byt_banks[i];
	return NULL;
}

static int byt_probe_cb(struct acpi_device *adev,
			const struct acpi_device_id *id, void *ctx)
{
	const struct byt_bank *bank;
	const struct acpi_res *mem, *irqres;
	struct acpi_res_list res;
	struct byt_gpio *vg;
	uint64_t virt, pages;
	int irq;

	(void)id;
	(void)ctx;
	bank = byt_bank_by_uid(adev->has_uid ? adev->uid : NULL);
	if (!bank || gpiochip_find_by_acpi_handle(adev->handle))
		return 0;
	if (acpi_dev_get_resources(adev, &res))
		return 0;
	mem = acpi_res_find(&res, ACPI_RES_MEM, 0);
	/* The block reaches at least to the debounce register. */
	if (!mem || (mem->mem.length && mem->mem.length < BYT_DEBOUNCE_REG + 4)) {
		kprintf("%s: %s: no usable register block\n", bank->name,
			adev->path);
		acpi_dev_free_resource_list(&res);
		return 0;
	}
	pages = ((mem->mem.start & 0xfffull) +
		 (mem->mem.length ? mem->mem.length : 0x1000) + 0xfffull) >> 12;
	virt = mm_map_device_mmio(mem->mem.start, (size_t)pages);
	vg = virt ? kcalloc(1, sizeof(*vg)) : NULL;
	if (!vg) {
		acpi_dev_free_resource_list(&res);
		return 0;
	}
	vg->adev = acpi_dev_get(adev);
	(void)byt_gpio_setup(vg, bank, (uint8_t *)virt);
	vg->chip.acpi_handle = adev->handle;
	irqres = acpi_res_find(&res, ACPI_RES_IRQ, 0);
	irq = -ENOENT;
	if (irqres) {
		irq = irq_alloc_desc_gsi(NULL, vg, (int)irqres->irq.gsi,
					 irqres->irq.polarity ==
						 ACPI_RES_ACTIVE_LOW,
					 irqres->irq.triggering ==
						 ACPI_RES_LEVEL);
		if (irq > 0 && request_irq(irq, byt_irq_handler, IRQF_SHARED,
					   vg->label, vg) != 0) {
			irq_free_desc(irq);
			irq = -EBUSY;
		}
	}
	if (irq > 0)
		vg->parent_irq = irq;
	else
		vg->chip.irq = NULL;
	if (gpiochip_add(&vg->chip) != 0) {
		if (vg->parent_irq > 0) {
			(void)free_irq(vg->parent_irq, vg);
			irq_free_desc(vg->parent_irq);
		}
		acpi_dev_put(vg->adev);
		kfree(vg);
	} else {
		mutex_lock(&g_byt_list_lock);
		list_add_tail(&vg->node, &g_byt_list);
		mutex_unlock(&g_byt_list_lock);
		kprintf("%s: %u GPIOs%s\n", vg->label, bank->npins,
			vg->parent_irq > 0 ? ", interrupts" : "");
	}
	acpi_dev_free_resource_list(&res);
	return 0;
}

void byt_pinctrl_init(void)
{
	static const struct acpi_device_id ids[] = {
		{ "INT33B2", 0 }, /* early firmware */
		{ "INT33FC", 0 },
		{ "", 0 },
	};

	might_sleep();
	if (!acpi_dev_ready())
		return;
	{
		struct acpi_device *devs[8];
		int n = acpi_dev_collect_matches(ids, 1, devs, 8);

		for (int i = 0; i < n; i++) {
			(void)byt_probe_cb(devs[i], NULL, NULL);
			acpi_dev_put(devs[i]);
		}
	}
}
