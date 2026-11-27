/* Host test of the Bay Trail, Cherry View and AMD GPIO drivers over the
 * register model (host/test-pinctrl.sh).  The drivers' sources are
 * included here so their private state can be inspected.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "kernel/dev/gpio/pinctrl/intel/pinctrl-baytrail.c"
#include "kernel/dev/gpio/pinctrl/intel/pinctrl-cherryview.c"
#include "kernel/dev/gpio/pinctrl/amd/pinctrl-amd.c"

int irqm_run_threads(void);
void test_byt(void);
void test_chv(void);
void test_amd(void);

static volatile int g_thr;
static int thr_fn(void *arg)
{
	(void)arg;
	g_thr++;
	return IRQ_HANDLED;
}

static int call_handler(int (*h)(void *), void *arg)
{
	uint64_t fl = local_irq_save();
	int r = h(arg);

	local_irq_restore(fl);
	return r;
}

/* ==========================================================================
 * Bay Trail
 * ========================================================================== */

/* The direct IRQ mux at 0x980: APIC input `slot' carries `pin'. */
static void byt_set_direct_mux(uint8_t *w, unsigned int slot, unsigned int pin)
{
	uint8_t *r = w + BYT_DIRECT_IRQ_REG + (slot & ~3u);
	uint32_t v = rm_peek(r);

	v &= ~(0xffu << ((slot & 3u) * 8));
	v |= (uint32_t)pin << ((slot & 3u) * 8);
	rm_poke(r, v);
}

void test_byt(void)
{
	uint8_t *w;
	struct byt_gpio *vg = kcalloc(1, sizeof(*vg));
	const struct byt_bank *score = byt_bank_by_uid("1");
	struct gpio_desc *d;
	uint8_t *c0, *val;
	int irq;

	rm_reset();
	w = rm_window(0x1000);
	rm_set_attr_range(w + BYT_INT_STAT_REG, 16, RM_W1C);
	T_ASSERT(score && score->npins == 102);
	T_ASSERT(byt_bank_by_uid("2")->npins == 28);
	T_ASSERT(byt_bank_by_uid("3")->npins == 44);
	T_ASSERT(!byt_bank_by_uid("4"));
	/* every pad block distinct and inside the 0x800 pad area */
	for (unsigned int b = 0; b < 3; b++) {
		const struct byt_bank *bk = &g_byt_banks[b];

		for (unsigned int i = 0; i < bk->npins; i++) {
			T_ASSERT(bk->pad_map[i] * 16u + 16 <= BYT_INT_STAT_REG);
			for (unsigned int j = 0; j < i; j++)
				T_ASSERT(bk->pad_map[i] != bk->pad_map[j]);
		}
	}
	/* the published pad maps, sampled */
	T_EQ(byt_score_map[0], 85);
	T_EQ(byt_score_map[101], 100);
	T_EQ(byt_ncore_map[0], 19);
	T_EQ(byt_ncore_map[27], 7);
	T_EQ(byt_sus_map[0], 29);
	T_EQ(byt_sus_map[43], 40);
	/* firmware state: pin 7 a proper direct interrupt (mux slot 2,
	 * level), pin 8 DIRECT_IRQ_EN without a mux slot, pin 9 with an
	 * edge trigger -- both firmware bugs; pin 5 armed */
	rm_poke(w + byt_score_map[7] * 16,
		BYT_DIRECT_IRQ_EN | BYT_TRIG_LVL | BYT_TRIG_POS);
	byt_set_direct_mux(w, 2, 7);
	rm_poke(w + byt_score_map[8] * 16,
		BYT_DIRECT_IRQ_EN | BYT_TRIG_LVL | BYT_TRIG_NEG);
	rm_poke(w + byt_score_map[9] * 16, BYT_DIRECT_IRQ_EN | BYT_TRIG_POS);
	byt_set_direct_mux(w, 3, 9);
	rm_poke(w + byt_score_map[5] * 16, BYT_TRIG_POS);
	rm_poke(w + BYT_INT_STAT_REG, 1u << 5);
	T_EQ(byt_gpio_setup(vg, score, w), 0);
	T_EQ(rm_peek(w + byt_score_map[5] * 16) & BYT_TRIG_MASK, 0);
	T_EQ(rm_peek(w + BYT_INT_STAT_REG), 0);
	T_ASSERT(vg->direct_irq_mask[0] & (1u << 7));
	/* the direct interrupt keeps its trigger, the bad ones lose all */
	T_EQ(rm_peek(w + byt_score_map[7] * 16) &
		     (BYT_DIRECT_IRQ_EN | BYT_TRIG_MASK),
	     BYT_DIRECT_IRQ_EN | BYT_TRIG_LVL | BYT_TRIG_POS);
	T_EQ(rm_peek(w + byt_score_map[8] * 16) &
		     (BYT_DIRECT_IRQ_EN | BYT_TRIG_MASK),
	     0);
	T_EQ(rm_peek(w + byt_score_map[9] * 16) &
		     (BYT_DIRECT_IRQ_EN | BYT_TRIG_MASK),
	     0);
	T_ASSERT(!(vg->direct_irq_mask[0] & ((1u << 8) | (1u << 9))));
	T_EQ(gpiochip_add(&vg->chip), 0);

	/* pin 92 is GPIO in function 1 */
	d = gpiochip_get_desc(&vg->chip, 92);
	T_EQ(gpiod_request(d, "alt"), 0);
	T_EQ(rm_peek(w + byt_score_map[92] * 16) & BYT_PIN_MUX, 1);
	gpiod_free(d);
	/* freeing a direct interrupt pad keeps its trigger */
	d = gpiochip_get_desc(&vg->chip, 7);
	T_EQ(gpiod_request(d, "direct"), 0);
	gpiod_free(d);
	T_EQ(rm_peek(w + byt_score_map[7] * 16) & BYT_TRIG_MASK,
	     BYT_TRIG_LVL | BYT_TRIG_POS);

	d = gpiochip_get_desc(&vg->chip, 3);
	c0 = w + byt_score_map[3] * 16;
	val = c0 + BYT_VAL_REG;
	rm_poke(val, BYT_INPUT_EN | BYT_OUTPUT_EN); /* both off */
	T_EQ(gpiod_request(d, "p3"), 0);
	T_EQ(gpiod_direction_output(d, 1), 0);
	T_EQ(rm_peek(val) & (BYT_DIR_MASK | BYT_LEVEL), BYT_LEVEL);
	T_EQ(gpiod_get_direction(d), GPIO_LINE_DIRECTION_OUT);
	T_EQ(gpiod_direction_input(d), 0);
	T_EQ(rm_peek(val) & BYT_DIR_MASK, BYT_OUTPUT_EN);
	T_EQ(gpiod_get_direction(d), GPIO_LINE_DIRECTION_IN);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 1), 0);
	T_EQ(rm_peek(c0) & (BYT_PULL_ASSIGN_MASK | BYT_PULL_STR_MASK),
	     BYT_PULL_ASSIGN_UP | BYT_PULL_STR_2K);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_DOWN, 40000), 0);
	T_EQ(rm_peek(c0) & (BYT_PULL_ASSIGN_MASK | BYT_PULL_STR_MASK),
	     BYT_PULL_ASSIGN_DOWN | BYT_PULL_STR_40K);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 1234), -EINVAL);
	T_EQ(gpiod_set_debounce(d, 1500), 0);
	T_EQ(rm_peek(w + BYT_DEBOUNCE_REG) & 7, 3);
	T_ASSERT(rm_peek(c0) & BYT_DEBOUNCE_EN);
	T_EQ(gpiod_set_debounce(d, 1000), -EINVAL);
	T_EQ(gpiod_set_debounce(d, 0), 0);
	T_EQ(rm_peek(c0) & BYT_DEBOUNCE_EN, 0);
	gpiod_free(d);

	/* interrupts */
	T_EQ(gpiochip_irq_map(&vg->chip, 7), -EBUSY);
	irq = gpiochip_irq_map(&vg->chip, 5);
	T_ASSERT(irq > 0);
	T_EQ(irq_set_irq_type(irq, IRQ_TYPE_LEVEL_LOW), 0);
	c0 = w + byt_score_map[5] * 16;
	T_EQ(rm_peek(c0) & (BYT_TRIG_MASK | BYT_GLITCH_FILTER_EN),
	     BYT_GLITCH_FILTER_EN);
	T_EQ(request_threaded_irq(irq, NULL, thr_fn, IRQF_ONESHOT, "byt",
				  (void *)&g_thr),
	     0);
	T_EQ(rm_peek(c0) & BYT_TRIG_MASK, BYT_TRIG_LVL | BYT_TRIG_NEG);
	T_EQ(call_handler(byt_irq_handler, vg), IRQ_NONE);
	g_thr = 0;
	rm_poke(w + BYT_INT_STAT_REG, 1u << 5);
	T_EQ(call_handler(byt_irq_handler, vg), IRQ_HANDLED);
	T_EQ(rm_peek(w + BYT_INT_STAT_REG), 0); /* acknowledged */
	T_EQ(rm_peek(c0) & BYT_TRIG_MASK, 0); /* masked for the thread */
	T_EQ(irqm_run_threads(), 1);
	T_EQ(g_thr, 1);
	T_EQ(rm_peek(c0) & BYT_TRIG_MASK, BYT_TRIG_LVL | BYT_TRIG_NEG);

	/* system sleep: firmware scrambles mux/trigger/direction */
	byt_gpio_suspend(vg);
	val = c0 + BYT_VAL_REG;
	rm_poke(c0, (rm_peek(c0) & ~BYT_TRIG_MASK) | 2u);
	rm_poke(val, BYT_LEVEL);
	byt_gpio_resume(vg);
	T_EQ(rm_peek(c0) & (BYT_TRIG_MASK | BYT_PIN_MUX),
	     BYT_TRIG_LVL | BYT_TRIG_NEG);
	T_EQ(rm_peek(val) & (BYT_DIR_MASK | BYT_LEVEL), 0);

	(void)free_irq(irq, (void *)&g_thr);
	T_EQ(rm_peek(c0) & BYT_TRIG_MASK, 0);
	gpiochip_remove(&vg->chip);
	kfree(vg);
}

/* ==========================================================================
 * Cherry View
 * ========================================================================== */

static uint8_t *chv_pad(struct chv_gpio *cg, unsigned int pad, unsigned int r)
{
	uint8_t *p = chv_padreg(cg, pad, r);

	T_ASSERT(p);
	return p;
}

void test_chv(void)
{
	struct chv_gpio *cg = kcalloc(1, sizeof(*cg));
	const struct chv_community *sw = chv_community_by_uid("1");
	struct gpio_desc *d;
	uint8_t *w;
	int irq, irq21, irq30, warn0;

	rm_reset();
	w = rm_window(0x8000);
	rm_set_attr(w + CHV_INTSTAT, RM_W1C);
	T_ASSERT(sw && chv_community_by_uid("4") && !chv_community_by_uid("5"));
	/* interrupt lines and pin counts per community */
	T_EQ(chv_community_by_uid("1")->nirqs, 8);
	T_EQ(chv_community_by_uid("2")->nirqs, 8);
	T_EQ(chv_community_by_uid("3")->nirqs, 16);
	T_EQ(chv_community_by_uid("4")->nirqs, 16);
	T_EQ(chv_community_by_uid("1")->npins, 98);
	T_EQ(chv_community_by_uid("2")->npins, 73);
	T_EQ(chv_community_by_uid("3")->npins, 27);
	T_EQ(chv_community_by_uid("4")->npins, 86);
	T_EQ(CHV_PADCTRL1_INVRXTX_RXDATA, 1u << 6);
	/* the pad blocks of the families */
	T_EQ(chv_padreg(&(struct chv_gpio){ .comm = sw, .regs = w }, 92, 0) - w,
	     0x4400 + 6 * 0x400 + 2 * 8);
	/* firmware: pad 20 and 21 on line 3, pad 30 locked on line 4,
	 * pad 22 a GPIO with inverted TX data */
	cg->comm = sw;
	cg->regs = w;
	rm_poke(chv_pad(cg, 20, CHV_PADCTRL0), (3u << 28) | CHV_PADCTRL0_GPIOEN);
	rm_poke(chv_pad(cg, 21, CHV_PADCTRL0), (3u << 28) | CHV_PADCTRL0_GPIOEN);
	rm_poke(chv_pad(cg, 30, CHV_PADCTRL0), (4u << 28) | CHV_PADCTRL0_GPIOEN);
	rm_poke(chv_pad(cg, 30, CHV_PADCTRL1),
		CHV_PADCTRL1_CFGLOCK | CHV_PADCTRL1_INTWAKECFG_LEVEL);
	rm_set_attr(chv_pad(cg, 30, CHV_PADCTRL1), RM_RO);
	rm_poke(chv_pad(cg, 15, CHV_PADCTRL0),
		CHV_PADCTRL0_GPIOCFG_HIZ << CHV_PADCTRL0_GPIOCFG_SHIFT);
	rm_poke(chv_pad(cg, 22, CHV_PADCTRL0), CHV_PADCTRL0_GPIOEN);
	rm_poke(chv_pad(cg, 22, CHV_PADCTRL1),
		CHV_PADCTRL1_INVRXTX_TXDATA | CHV_PADCTRL1_INVRXTX_RXDATA |
			CHV_PADCTRL1_INTWAKECFG_BOTH);
	rm_poke(w + CHV_INTMASK, 0xffff);
	T_EQ(chv_gpio_setup(cg, sw, w, 0x8000), 0);
	/* lines 0-7 masked, the GPE-only lines 8-15 left on */
	T_EQ(rm_peek(w + CHV_INTMASK), 0xffffff00u);
	T_EQ(gpiochip_add(&cg->chip), 0);
	T_ASSERT(!cg->chip.valid(&cg->chip, 8));
	T_ASSERT(cg->chip.valid(&cg->chip, 15));
	T_ASSERT(cg->chip.valid(&cg->chip, 97));
	T_ASSERT(!cg->chip.valid(&cg->chip, 98));

	d = gpiochip_get_desc(&cg->chip, 15);
	T_EQ(gpiod_request(d, "hiz"), 0);
	T_EQ(rm_peek(chv_pad(cg, 15, CHV_PADCTRL0)) &
		     (CHV_PADCTRL0_GPIOCFG_MASK | CHV_PADCTRL0_GPIOEN),
	     (CHV_PADCTRL0_GPIOCFG_GPI << CHV_PADCTRL0_GPIOCFG_SHIFT) |
		     CHV_PADCTRL0_GPIOEN);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 1000), 0);
	T_EQ(rm_peek(chv_pad(cg, 15, 0)) &
		     (CHV_PADCTRL0_TERM_MASK | CHV_PADCTRL0_TERM_UP),
	     (CHV_PADCTRL0_TERM_1K << 20) | CHV_PADCTRL0_TERM_UP);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_DOWN, 1000), -EINVAL);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_DOWN, 1), 0);
	T_EQ(rm_peek(chv_pad(cg, 15, 0)) &
		     (CHV_PADCTRL0_TERM_MASK | CHV_PADCTRL0_TERM_UP),
	     CHV_PADCTRL0_TERM_20K << 20);
	T_EQ(gpiod_direction_output(d, 1), 0);
	T_EQ(gpiod_get_value(d), 1);
	gpiod_free(d);
	/* requesting a GPIO pad keeps its TX inversion, drops the rest */
	d = gpiochip_get_desc(&cg->chip, 22);
	T_EQ(gpiod_request(d, "inv"), 0);
	T_EQ(rm_peek(chv_pad(cg, 22, CHV_PADCTRL1)) &
		     (CHV_PADCTRL1_INVRXTX_MASK | CHV_PADCTRL1_INTWAKECFG_MASK),
	     CHV_PADCTRL1_INVRXTX_TXDATA);
	gpiod_free(d);

	irq = gpiochip_irq_map(&cg->chip, 20);
	T_ASSERT(irq > 0);
	T_EQ(cg->intr_lines[3], 20);
	T_EQ(irq_set_irq_type(irq, IRQ_TYPE_LEVEL_LOW), 0);
	T_EQ(rm_peek(chv_pad(cg, 20, CHV_PADCTRL1)) &
		     (CHV_PADCTRL1_INTWAKECFG_MASK | CHV_PADCTRL1_INVRXTX_MASK),
	     CHV_PADCTRL1_INTWAKECFG_LEVEL | CHV_PADCTRL1_INVRXTX_RXDATA);
	/* pad 21 shares line 3 (a firmware bug): moved to the top free
	 * line, 7 */
	irq21 = gpiochip_irq_map(&cg->chip, 21);
	T_ASSERT(irq21 > 0);
	T_EQ(cg->intr_lines[7], 21);
	T_EQ((rm_peek(chv_pad(cg, 21, CHV_PADCTRL0)) &
	      CHV_PADCTRL0_INTSEL_MASK) >> 28,
	     7);
	T_EQ(request_threaded_irq(irq, NULL, thr_fn, IRQF_ONESHOT, "chv",
				  (void *)&g_thr),
	     0);
	T_EQ(rm_peek(w + CHV_INTMASK) & 0xff, 1u << 3);
	g_thr = 0;
	rm_poke(w + CHV_INTSTAT, 1u << 3);
	T_EQ(call_handler(chv_irq_handler, cg), IRQ_HANDLED);
	T_EQ(rm_peek(w + CHV_INTSTAT), 0);
	T_EQ(rm_peek(w + CHV_INTMASK) & 0xff, 0);
	T_EQ(irqm_run_threads(), 1);
	T_EQ(rm_peek(w + CHV_INTMASK) & 0xff, 1u << 3);

	/* locked pad: the line is mapped, its trigger left alone */
	rm_log_clear();
	irq30 = gpiochip_irq_map(&cg->chip, 30);
	T_ASSERT(irq30 > 0);
	T_EQ(irq_set_irq_type(irq30, IRQ_TYPE_EDGE_RISING), 0);
	T_EQ(rm_writes_in(chv_pad(cg, 30, CHV_PADCTRL1), 4), 0);
	T_EQ(cg->intr_lines[4], 30);

	/* a line nobody mapped while pin 0 is no interrupt: masked */
	rm_poke(w + CHV_INTMASK, rm_peek(w + CHV_INTMASK) | (1u << 6));
	rm_poke(w + CHV_INTSTAT, 1u << 6);
	warn0 = g_test_warnings;
	T_EQ(call_handler(chv_irq_handler, cg), IRQ_HANDLED);
	T_EQ(g_test_warnings - warn0, 1);
	g_test_warnings = warn0;
	T_EQ(rm_peek(w + CHV_INTMASK) & (1u << 6), 0);

	/* system sleep */
	cg->ctx = kcalloc(sw->npins, sizeof(*cg->ctx));
	chv_gpio_suspend(cg);
	rm_poke(chv_pad(cg, 20, CHV_PADCTRL1), 0);
	rm_poke(w + CHV_INTMASK, 0);
	chv_gpio_resume(cg);
	T_EQ(rm_peek(chv_pad(cg, 20, CHV_PADCTRL1)) &
		     (CHV_PADCTRL1_INTWAKECFG_MASK | CHV_PADCTRL1_INVRXTX_MASK),
	     CHV_PADCTRL1_INTWAKECFG_LEVEL | CHV_PADCTRL1_INVRXTX_RXDATA);
	T_EQ(rm_peek(w + CHV_INTMASK) & 0xff, 1u << 3);

	(void)free_irq(irq, (void *)&g_thr);
	gpiochip_remove(&cg->chip);
	kfree(cg->ctx);
	kfree(cg);
}

/* ==========================================================================
 * AMD
 * ========================================================================== */

#define AMD_STS_ALL (BIT32(INTERRUPT_STS_OFF) | BIT32(WAKE_STS_OFF))

static volatile int g_rg;
static int rg_fn(void *arg)
{
	(void)arg;
	g_rg++;
	return IRQ_HANDLED;
}

void test_amd(void)
{
	struct amd_gpio *ag = kcalloc(1, sizeof(*ag));
	uint8_t *w, *rg, *p5;
	int irq, irq258, warn0;
	uint32_t v;
	struct gpio_desc *d;

	rm_reset();
	w = rm_window(0x400);
	rg = rm_window(0x100);
	rm_amd_set_base(w);
	for (unsigned int pin = 0; pin < 256; pin++)
		if (pin * 4 != WAKE_INT_MASTER_REG &&
		    pin * 4 != WAKE_INT_STATUS_REG0 &&
		    pin * 4 != WAKE_INT_STATUS_REG1)
			rm_set_attr(w + pin * 4, RM_AMDPIN);
	for (unsigned int i = 0; i < 16; i++)
		rm_set_attr(rg + i * 4, RM_AMDPIN);
	rm_set_attr(w + WAKE_INT_STATUS_REG0, RM_RO);
	rm_set_attr(w + WAKE_INT_STATUS_REG1, RM_RO);
	/* firmware left pin 9 armed with a status and S3/S4 wake, pin 10
	 * with S4 wake only */
	rm_poke(w + 9 * 4, BIT32(INTERRUPT_ENABLE_OFF) |
				   BIT32(INTERRUPT_MASK_OFF) |
				   BIT32(WAKE_CNTRL_OFF_S3) |
				   BIT32(WAKE_CNTRL_OFF_S4) |
				   BIT32(INTERRUPT_STS_OFF));
	rm_poke(w + 10 * 4, BIT32(WAKE_CNTRL_OFF_S4));
	/* no _AEI: S4 wake stays, interrupt enables stay */
	T_EQ(amd_gpio_setup(ag, w, 0x400, rg, 0), 0);
	v = rm_peek(w + 9 * 4);
	T_EQ(v & (BIT32(INTERRUPT_ENABLE_OFF) | BIT32(INTERRUPT_MASK_OFF) |
		  WAKE_SOURCE_SUSPEND | BIT32(WAKE_CNTRL_OFF_S4)),
	     BIT32(INTERRUPT_ENABLE_OFF) | BIT32(INTERRUPT_MASK_OFF) |
		     BIT32(WAKE_CNTRL_OFF_S4));
	T_ASSERT(v & BIT32(INTERRUPT_STS_OFF)); /* left for the handler */
	T_ASSERT(rm_peek(w + 10 * 4) & BIT32(WAKE_CNTRL_OFF_S4));
	T_EQ(ag->chip.ngpio, 256 + 16);
	T_ASSERT(ag->base_rgpio == rg);
	T_EQ(gpiochip_add(&ag->chip), 0);
	T_ASSERT(!ag->chip.valid(&ag->chip, 63));
	T_ASSERT(ag->chip.valid(&ag->chip, 62));
	T_ASSERT(ag->chip.valid(&ag->chip, 183));
	T_ASSERT(!ag->chip.valid(&ag->chip, 184));
	T_ASSERT(!ag->chip.valid(&ag->chip, 190)); /* status register */
	T_ASSERT(ag->chip.valid(&ag->chip, 256));
	T_ASSERT(ag->chip.valid(&ag->chip, 271));
	T_ASSERT(!ag->chip.valid(&ag->chip, 272));
	/* firmware's stale pin 9: delivered nowhere, masked on first sight */
	warn0 = g_test_warnings;
	(void)call_handler(amd_irq_handler, ag);
	T_EQ(g_test_warnings - warn0, 1);
	g_test_warnings = warn0;
	T_EQ(rm_peek(w + 9 * 4) & (BIT32(INTERRUPT_MASK_OFF) | AMD_STS_ALL), 0);

	/* debounce encodings */
	d = gpiochip_get_desc(&ag->chip, 20);
	T_EQ(gpiod_request(d, "db"), 0);
	T_EQ(gpiod_set_debounce(d, 100), 0);
	v = rm_peek(w + 20 * 4);
	T_EQ(v & 0xffu, (DB_TYPE_REMOVE_GLITCH << 5) | 1);
	T_EQ(gpiod_set_debounce(d, 2000), 0);
	v = rm_peek(w + 20 * 4);
	T_EQ(v & 0xffu,
	     (DB_TYPE_REMOVE_GLITCH << 5) | BIT32(DB_TMR_OUT_UNIT_OFF) | 8);
	T_EQ(gpiod_set_debounce(d, 100000), 0);
	T_EQ(rm_peek(w + 20 * 4) & 0xffu,
	     (DB_TYPE_REMOVE_GLITCH << 5) | BIT32(DB_TMR_LARGE_OFF) | 6);
	T_EQ(gpiod_set_debounce(d, 500000), 0);
	T_EQ(rm_peek(w + 20 * 4) & 0xffu,
	     (DB_TYPE_REMOVE_GLITCH << 5) | BIT32(DB_TMR_LARGE_OFF) |
		     BIT32(DB_TMR_OUT_UNIT_OFF) | 8);
	T_EQ(gpiod_set_debounce(d, 2000000), -EINVAL);
	T_EQ(rm_peek(w + 20 * 4) & (DB_CNTRL_MASK << DB_CNTRL_OFF), 0);
	T_EQ(gpiod_set_debounce(d, 0), 0);
	T_EQ(rm_peek(w + 20 * 4) & 0xffu, 0);
	T_EQ(gpiod_direction_output(d, 1), 0);
	T_ASSERT(rm_peek(w + 20 * 4) &
		 (BIT32(OUTPUT_ENABLE_OFF) | BIT32(OUTPUT_VALUE_OFF)));
	/* pulls: each its own enable, "no pull" is not a setting */
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 1), 0);
	T_ASSERT(rm_peek(w + 20 * 4) & BIT32(PULL_UP_ENABLE_OFF));
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_DOWN, 1), 0);
	T_EQ(rm_peek(w + 20 * 4) &
		     (BIT32(PULL_UP_ENABLE_OFF) | BIT32(PULL_DOWN_ENABLE_OFF)),
	     BIT32(PULL_UP_ENABLE_OFF) | BIT32(PULL_DOWN_ENABLE_OFF));
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 0), 0);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_DISABLE, 0), -ENOTSUPP);
	T_ASSERT(rm_peek(w + 20 * 4) & BIT32(PULL_DOWN_ENABLE_OFF));
	gpiod_free(d);
	/* GPIO 0 debounced by the hardware: software debounce stays off */
	rm_poke(w + WAKE_INT_MASTER_REG, INTERNAL_GPIO0_DEBOUNCE);
	d = gpiochip_get_desc(&ag->chip, 0);
	T_EQ(gpiod_set_debounce(d, 2000), 0);
	T_EQ(rm_peek(w) & (DB_CNTRL_MASK << DB_CNTRL_OFF), 0);
	rm_poke(w + WAKE_INT_MASTER_REG, 0);

	/* a level-low interrupt with a oneshot thread */
	irq = gpiochip_irq_map(&ag->chip, 5);
	T_ASSERT(irq > 0);
	p5 = w + 5 * 4;
	T_EQ(rm_peek(p5) &
		     (BIT32(INTERRUPT_ENABLE_OFF) | BIT32(INTERRUPT_MASK_OFF)),
	     BIT32(INTERRUPT_ENABLE_OFF));
	rm_amd_drive(5, 1); /* idle high */
	T_EQ(irq_set_irq_type(irq, IRQ_TYPE_LEVEL_LOW), 0);
	T_EQ(rm_peek(p5) & (BIT32(LEVEL_TRIG_OFF) |
			    (ACTIVE_LEVEL_MASK << ACTIVE_LEVEL_OFF)),
	     BIT32(LEVEL_TRIG_OFF) | (ACTIVE_LOW << ACTIVE_LEVEL_OFF));
	T_EQ(request_threaded_irq(irq, NULL, thr_fn, IRQF_ONESHOT, "amd",
				  (void *)&g_thr),
	     0);
	T_ASSERT(rm_peek(p5) & BIT32(INTERRUPT_MASK_OFF));
	T_EQ(call_handler(amd_irq_handler, ag), IRQ_NONE);
	g_thr = 0;
	rm_amd_drive(5, 0); /* asserted */
	T_ASSERT(rm_peek(p5) & BIT32(INTERRUPT_STS_OFF));
	rm_log_clear();
	T_EQ(call_handler(amd_irq_handler, ag), IRQ_HANDLED);
	T_ASSERT(rm_writes_in(w + WAKE_INT_MASTER_REG, 4) >= 1);
	T_ASSERT(rm_peek(w + WAKE_INT_MASTER_REG) & EOI_MASK);
	T_EQ(rm_peek(p5) & BIT32(INTERRUPT_MASK_OFF), 0); /* for the thread */
	rm_amd_drive(5, 1); /* the thread will have serviced it */
	T_EQ(irqm_run_threads(), 1);
	T_ASSERT(rm_peek(p5) & BIT32(INTERRUPT_MASK_OFF));
	/* the status latched meanwhile is served once more, then quiet */
	while (call_handler(amd_irq_handler, ag) == IRQ_HANDLED)
		T_ASSERT(irqm_run_threads() <= 1);
	T_EQ(rm_peek(p5) & BIT32(INTERRUPT_STS_OFF), 0);
	T_ASSERT(g_thr >= 1);

	/* wake bits follow enable_irq_wake */
	T_EQ(enable_irq_wake(irq), 0);
	T_EQ(rm_peek(p5) & WAKE_SOURCE_SUSPEND, WAKE_SOURCE_SUSPEND);
	T_EQ(disable_irq_wake(irq), 0);
	T_EQ(rm_peek(p5) & WAKE_SOURCE_SUSPEND, 0);

	/* system sleep: no wake source, so masked and back */
	amd_gpio_suspend(ag, 0);
	T_EQ(rm_peek(p5) & BIT32(INTERRUPT_MASK_OFF), 0);
	amd_gpio_resume(ag);
	T_ASSERT(rm_peek(p5) & BIT32(INTERRUPT_MASK_OFF));

	/* a remote-block pin: its own summary bit */
	irq258 = gpiochip_irq_map(&ag->chip, 258);
	T_ASSERT(irq258 > 0);
	T_EQ(irq_set_irq_type(irq258, IRQ_TYPE_EDGE_RISING), 0);
	T_EQ(request_irq(irq258, rg_fn, 0, "rg", (void *)&g_rg), 0);
	g_rg = 0;
	rm_poke(rg + 2 * 4, rm_peek(rg + 2 * 4) | BIT32(INTERRUPT_STS_OFF));
	rm_poke(rg + WAKE_INT_STATUS_REG_RGPIO, 1u << 2);
	T_EQ(call_handler(amd_irq_handler, ag), IRQ_HANDLED);
	T_EQ(g_rg, 1);
	T_EQ(rm_peek(rg + 2 * 4) & BIT32(INTERRUPT_STS_OFF), 0);
	rm_poke(rg + WAKE_INT_STATUS_REG_RGPIO, 0);
	(void)free_irq(irq258, (void *)&g_rg);

	/* the enable that never reads back: set_type gives up */
	rm_set_attr(w + 30 * 4, RM_RO);
	{
		int irq30 = gpiochip_irq_map(&ag->chip, 30);

		T_ASSERT(irq30 > 0);
		T_EQ(irq_set_irq_type(irq30, IRQ_TYPE_EDGE_RISING),
		     -ETIMEDOUT);
	}

	/* a pending pin nobody uses is masked */
	rm_poke(w + 12 * 4, BIT32(INTERRUPT_ENABLE_OFF) |
				    BIT32(INTERRUPT_MASK_OFF) |
				    BIT32(INTERRUPT_STS_OFF));
	warn0 = g_test_warnings;
	(void)call_handler(amd_irq_handler, ag);
	T_EQ(g_test_warnings - warn0, 1);
	g_test_warnings = warn0;
	T_EQ(rm_peek(w + 12 * 4) &
		     (BIT32(INTERRUPT_MASK_OFF) | BIT32(INTERRUPT_STS_OFF)),
	     0);
	(void)free_irq(irq, (void *)&g_thr);

	/* a main block of another size has no remote pins */
	{
		struct amd_gpio *a2 = kcalloc(1, sizeof(*a2));
		uint8_t *w2 = rm_window(0x300);

		T_EQ(amd_gpio_setup(a2, w2, 0x300, rg, 1), 0);
		T_ASSERT(a2->base_rgpio == NULL);
		T_EQ(a2->chip.ngpio, 0x300 / 4);
		T_ASSERT(!a2->chip.valid(&a2->chip, 256));
		kfree(a2->saved_regs);
		kfree(a2);
	}
	gpiochip_remove(&ag->chip);
	kfree(ag->saved_regs);
	kfree(ag);
}
