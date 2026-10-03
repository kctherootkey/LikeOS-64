// LikeOS -- hotplug: noticing a sink being plugged in or pulled.
//
// Each DDI has a hotplug detect pin.  The south display engine (the
// PCH) filters its pulses -- a long one is a plug or an unplug, a short
// one a DisplayPort sink asking for attention -- and raises an interrupt
// naming the port.  The interrupt handler only records which ports
// changed and wakes a worker; the worker waits out the bounce, asks the
// port what is there now (DPCD over AUX, the EDID over DDC), and updates
// the connector's status and mode list.  A client that asks the
// connector again (a display server re-probing) sees the new sink.
//
// From display version 14 the Type-C ports' DP-alt and Thunderbolt pins
// are PICA's, whose interrupt bank reaches the CPU as one bit of the
// south display's.  Lunar Lake and the parts after it (the "Lunar Lake"
// kind of south display, on the SoC) keep Meteor Point's pins, but their
// pulse filter and pin polarity need no programming; Wildcat Lake has
// two Type-C pins.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2023-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_hdmi_feat.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/waitq.h>

#define HPD_DEBOUNCE_NS 150000000ULL /* 150 ms after the last pulse */

/* ---- the pins --------------------------------------------------------------- */

enum hpd_style {
	HPD_STYLE_NONE = 0,
	HPD_STYLE_SPT, /* Lynx/Wildcat/Sunrise/Kaby/Cannon/Comet Point */
	HPD_STYLE_ICP, /* Ice/Jasper/Tiger/Alder Point (+ the north Type-C pins) */
	HPD_STYLE_DG1, /* DG1 and DG2: the south display on the card */
	HPD_STYLE_MTP, /* Meteor Point and Lunar Lake's kind, with PICA */
	HPD_STYLE_BXT, /* Broxton/Gemini Lake: the pins in the north display */
};

static enum hpd_style hpd_style(struct i915_device *i915)
{
	if (i915->display.model == INTEL_DISPLAY_BXT)
		return HPD_STYLE_BXT;
	if ((i915->pch == I915_PCH_MTP || i915->pch == I915_PCH_LNL) &&
	    i915->info->display_ver >= 14)
		return HPD_STYLE_MTP;
	if (i915->pch == I915_PCH_DG1 || i915->pch == I915_PCH_DG2)
		return HPD_STYLE_DG1;
	if (i915->pch >= I915_PCH_ICP && i915->pch <= I915_PCH_ADP)
		return HPD_STYLE_ICP;
	if ((i915->info->flags & I915_INFO_HAS_PCH) && i915->pch < I915_PCH_ICP)
		return HPD_STYLE_SPT;
	return HPD_STYLE_NONE;
}

/* Elkhart Lake's Mule Creek Canyon: a Tiger Point kind of south display
 * behind a Jasper Lake display. */
static int pch_is_mcc(struct i915_device *i915)
{
	return (i915->pch_devid & 0xff80) == 0x4b00;
}

/* The hotplug pin of a port: a DDI pin (A..E) or a Type-C pin (TC1..6),
 * by the platform's wiring.  Returns the pin's index, *tc says which
 * kind; -1 for a port without one. */
static int port_hpd_pin(struct i915_device *i915, int port, int *tc)
{
	int ver = i915->info->display_ver;
	uint8_t plat = i915->info->platform;

	*tc = 0;
	if (port < 0)
		return -1;
	if (ver >= 13) {
		if (port >= PORT_H) /* DDI D/E of display 13 */
			return PORT_D + port - PORT_H;
		if (port >= PORT_TC1) {
			*tc = 1;
			return port - PORT_TC1;
		}
		return port;
	}
	if (plat == I915_PLATFORM_DG1)
		return port >= PORT_TC1 ? PORT_C + port - PORT_TC1 : port;
	if (plat == I915_PLATFORM_ROCKETLAKE && i915->pch != I915_PCH_TGP)
		return port >= PORT_TC1 ? PORT_C + port - PORT_TC1 : port;
	if (ver == 12) {
		if (port >= PORT_TC1) {
			*tc = 1;
			return port - PORT_TC1;
		}
		return port;
	}
	if (plat == I915_PLATFORM_JASPERLAKE || plat == I915_PLATFORM_ELKHARTLAKE) {
		if (port == PORT_D)
			return PORT_A;
		if (!pch_is_mcc(i915))
			return port;
	} else if (ver == 9 && i915->pch != I915_PCH_TGP) {
		return port;
	} else if (ver != 11 && ver != 9) {
		return port;
	}
	/* Ice Lake's rule: C onwards are the Type-C pins */
	if (port >= PORT_C) {
		*tc = 1;
		return port - PORT_C;
	}
	return port;
}

/* The south display's interrupt bit of a pin. */
static uint32_t sde_pin_bit(struct i915_device *i915, int pin, int tc)
{
	if (pin < 0)
		return 0;
	if (hpd_style(i915) == HPD_STYLE_DG1)
		return tc ? DG1_SDE_TC_HOTPLUG(pin) : ICP_SDE_DDI_HOTPLUG(pin);
	return tc ? ICP_SDE_TC_HOTPLUG(pin) : ICP_SDE_DDI_HOTPLUG(pin);
}

/* Do the Type-C pins also come through the north display (DP-alt and
 * Thunderbolt, Gen11 to 13) or PICA (version 14)? */
static int has_north_tc_pins(struct i915_device *i915)
{
	return i915->info->display_ver >= 11 && hpd_style(i915) != HPD_STYLE_DG1;
}

static struct intel_output *output_on_port(struct i915_device *i915, int port)
{
	struct intel_display *d = &i915->display;
	for (int i = 0; i < d->nout; i++)
		if (d->outputs[i].port == port)
			return &d->outputs[i];
	return NULL;
}

int intel_hpd_live(struct i915_device *i915, int port)
{
	struct intel_display *d = &i915->display;
	enum hpd_style style = hpd_style(i915);
	int tc;

	if (style == HPD_STYLE_BXT) {
		uint32_t isr = i915_read32(i915, GEN8_DE_PORT_ISR);
		switch (port) {
		case PORT_A: return !!(isr & BXT_DE_PORT_HP_DDIA);
		case PORT_B: return !!(isr & BXT_DE_PORT_HP_DDIB);
		case PORT_C: return !!(isr & BXT_DE_PORT_HP_DDIC);
		default: return 0;
		}
	}
	if (style == HPD_STYLE_ICP || style == HPD_STYLE_DG1 || style == HPD_STYLE_MTP) {
		int pin = port_hpd_pin(i915, port, &tc);
		if (pin < 0)
			return 0;
		/* A port behind a Type-C PHY: its mode decides which pin
		 * counts (intel_tc.c). */
		struct intel_output *o = output_on_port(i915, port);
		if (tc && o && o->is_tc && has_north_tc_pins(i915))
			return intel_tc_hpd_live(i915, o);
		return !!(i915_read32(i915, SDEISR) & sde_pin_bit(i915, pin, tc));
	}
	if (style == HPD_STYLE_SPT) {
		uint32_t isr = i915_read32(i915, SDEISR);
		switch (port) {
		case PORT_A: return !!(isr & SDE_PORTA_HOTPLUG_LIVE);
		case PORT_B: return !!(isr & SDE_PORTB_HOTPLUG_LIVE);
		case PORT_C: return !!(isr & SDE_PORTC_HOTPLUG_LIVE);
		case PORT_D: return !!(isr & SDE_PORTD_HOTPLUG_LIVE);
		case PORT_E: return !!(isr & SDE_PORTE_HOTPLUG_LIVE);
		default: return 0;
		}
	}
	(void)d;
	return 0;
}

static int pch_has_port_a_hpd(struct i915_device *i915)
{
	/* Sunrise Point and later route DDI A's pin through the PCH too */
	return i915->pch >= I915_PCH_SPT;
}

static void sde_irq_enable(struct i915_device *i915, uint32_t mask)
{
	if (i915->irq_vector < 0 || !mask)
		return;
	i915_write32(i915, SDEIIR, mask);
	i915_write32(i915, SDEIMR, i915_read32(i915, SDEIMR) & ~mask);
	i915_write32(i915, SDEIER, i915_read32(i915, SDEIER) | mask);
	(void)i915_read32(i915, SDEIER);
}

/* The bank the north display's Type-C hotplug comes through. */
static void north_hpd_regs(struct i915_device *i915, uint32_t *imr, uint32_t *iir, uint32_t *ier)
{
	if (i915->info->display_ver >= 14) {
		*imr = PICAINTERRUPT_IMR;
		*iir = PICAINTERRUPT_IIR;
		*ier = PICAINTERRUPT_IER;
	} else {
		*imr = GEN11_DE_HPD_IMR;
		*iir = GEN11_DE_HPD_IIR;
		*ier = GEN11_DE_HPD_IER;
	}
}

static void north_irq_enable(struct i915_device *i915, uint32_t mask)
{
	uint32_t imr, iir, ier;

	if (i915->irq_vector < 0 || !mask)
		return;
	north_hpd_regs(i915, &imr, &iir, &ier);
	i915_write32_fw(i915, iir, mask);
	i915_write32_fw(i915, imr, i915_read32_fw(i915, imr) & ~mask);
	i915_write32_fw(i915, ier, i915_read32_fw(i915, ier) | mask);
	(void)i915_read32_fw(i915, ier);
}

/* The Ice Point family, DG1/DG2 and Meteor Point: the DDI and Type-C
 * pins of the outputs the board has in the PCH, the Type-C ports'
 * DP-alt/Thunderbolt pins in the north display or PICA. */
static void icp_hpd_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	enum hpd_style style = hpd_style(i915);
	uint32_t ddi = i915_read32(i915, SHOTPLUG_CTL_DDI);
	uint32_t tcctl = i915_read32(i915, SHOTPLUG_CTL_TC);
	uint32_t mask = 0, de = 0;

	/* 250 us is what DP 1.4a asks of the hotplug filter (Lunar Lake's
	 * kind of south display has it so already) */
	if (i915->pch != I915_PCH_LNL)
		i915_write32(i915, SHPD_FILTER_CNT, SHPD_FILTER_CNT_250);
	if (style == HPD_STYLE_DG1 && i915->pch == I915_PCH_DG1)
		i915_write32(i915, SOUTH_CHICKEN1,
			     i915_read32(i915, SOUTH_CHICKEN1) | INVERT_DDIA_HPD | INVERT_DDIB_HPD |
				     INVERT_DDIC_HPD | INVERT_DDID_HPD);
	if (style == HPD_STYLE_MTP && i915->pch == I915_PCH_MTP)
		i915_write32(i915, SOUTH_CHICKEN1,
			     i915_read32(i915, SOUTH_CHICKEN1) | INVERT_DDIA_HPD | INVERT_DDIB_HPD |
				     INVERT_DDIC_HPD | INVERT_TC1_HPD | INVERT_TC2_HPD |
				     INVERT_TC3_HPD | INVERT_TC4_HPD | INVERT_DDID_HPD_MTP |
				     INVERT_DDIE_HPD);
	for (int i = 0; i < d->nout; i++) {
		int tc;
		int pin = port_hpd_pin(i915, d->outputs[i].port, &tc);
		if (pin < 0)
			continue;
		if (tc) {
			if (pin > 5 || (style == HPD_STYLE_MTP && pin > 3) ||
			    (intel_display_verx100(i915) == 3002 && pin > 1))
				continue;
			tcctl |= ICP_HPD_ENABLE(pin);
			mask |= sde_pin_bit(i915, pin, 1);
			if (has_north_tc_pins(i915))
				de |= GEN11_TC_HOTPLUG(pin) | GEN11_TBT_HOTPLUG(pin);
		} else {
			if (pin > 3 || (style == HPD_STYLE_MTP && pin > 1))
				continue;
			ddi |= ICP_HPD_ENABLE(pin);
			mask |= sde_pin_bit(i915, pin, 0);
		}
	}
	i915_write32(i915, SHOTPLUG_CTL_DDI, ddi);
	i915_write32(i915, SHOTPLUG_CTL_TC, tcctl);
	if (de && style == HPD_STYLE_MTP) {
		for (int t = 0; t < 4; t++) {
			if (!(de & XELPDP_TBT_HOTPLUG(t)))
				continue;
			uint32_t v = i915_read32(i915, XELPDP_PORT_HOTPLUG_CTL(t));
			i915_write32(i915, XELPDP_PORT_HOTPLUG_CTL(t),
				     v | XELPDP_TBT_HOTPLUG_ENABLE | XELPDP_DP_ALT_HOTPLUG_ENABLE);
		}
	} else if (de) {
		uint32_t ctl = i915_read32(i915, GEN11_TC_HOTPLUG_CTL);
		uint32_t tbt = i915_read32(i915, GEN11_TBT_HOTPLUG_CTL);
		for (int t = 0; t < 6; t++)
			if (de & GEN11_TC_HOTPLUG(t)) {
				ctl |= GEN11_HOTPLUG_CTL_ENABLE(t);
				tbt |= GEN11_HOTPLUG_CTL_ENABLE(t);
			}
		i915_write32(i915, GEN11_TC_HOTPLUG_CTL, ctl);
		i915_write32(i915, GEN11_TBT_HOTPLUG_CTL, tbt);
	}
	d->hpd_irq_mask = mask;
	d->hpd_irq_mask_de = de;
	/* Meteor Point: PICA's bank arrives as one bit of the PCH's */
	sde_irq_enable(i915, mask | (style == HPD_STYLE_MTP && de ? SDE_PICAINTERRUPT : 0));
	north_irq_enable(i915, de);
}

/* Broxton: the three pins in PCH_PORT_HOTPLUG, each with the polarity
 * the VBT asks for. */
static void bxt_hpd_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	struct intel_vbt *vbt = &d->vbt;
	uint32_t hp = i915_read32(i915, BXT_HOTPLUG_CTL);
	static const uint32_t en[3] = { HPD_PORTA_HOTPLUG_ENABLE, HPD_PORTB_HOTPLUG_ENABLE,
					HPD_PORTC_HOTPLUG_ENABLE };
	static const uint32_t inv[3] = { HPD_BXT_DDIA_INVERT, HPD_BXT_DDIB_INVERT,
					 HPD_BXT_DDIC_INVERT };
	static const uint32_t irq[3] = { BXT_DE_PORT_HP_DDIA, BXT_DE_PORT_HP_DDIB,
					 BXT_DE_PORT_HP_DDIC };
	uint32_t mask = 0;

	for (int p = 0; p < 3; p++) {
		hp &= ~(en[p] | inv[p]);
		if (!output_on_port(i915, p))
			continue;
		hp |= en[p];
		if (vbt->valid && vbt->port[p].hpd_invert)
			hp |= inv[p];
		mask |= irq[p];
	}
	i915_write32(i915, BXT_HOTPLUG_CTL, hp);
	d->hpd_irq_mask_port = mask;
	if (i915->irq_vector >= 0 && mask) {
		i915_write32_fw(i915, GEN8_DE_PORT_IIR, mask);
		i915_write32_fw(i915, GEN8_DE_PORT_IMR,
				i915_read32_fw(i915, GEN8_DE_PORT_IMR) & ~mask);
		i915_write32_fw(i915, GEN8_DE_PORT_IER,
				i915_read32_fw(i915, GEN8_DE_PORT_IER) | mask);
		(void)i915_read32_fw(i915, GEN8_DE_PORT_IER);
	}
}

int intel_hpd_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	enum hpd_style style = hpd_style(i915);

	wq_head_init(&d->hpd_wq, "i915_hpd");
	d->hpd_pending = 0;
	d->hpd_irq_mask = 0;
	d->hpd_irq_mask_de = 0;
	d->hpd_irq_mask_port = 0;

	if (style == HPD_STYLE_BXT) {
		bxt_hpd_init(i915);
		return 0;
	}
	if (style == HPD_STYLE_ICP || style == HPD_STYLE_DG1 || style == HPD_STYLE_MTP) {
		icp_hpd_init(i915);
		return 0;
	}
	if (style != HPD_STYLE_SPT)
		return 0;
	if (i915->pch == I915_PCH_CNP || i915->pch == I915_PCH_CMP) {
		/* display WA #1179 (hard hang on hotplug) */
		uint32_t c1 = i915_read32(i915, SOUTH_CHICKEN1);
		c1 &= ~CHASSIS_CLK_REQ_DURATION_MASK;
		i915_write32(i915, SOUTH_CHICKEN1, c1 | CHASSIS_CLK_REQ_DURATION(0xf));
		i915_write32(i915, SHPD_FILTER_CNT, SHPD_FILTER_CNT_500_ADJ);
	}
	/* the pins, with the 2 ms pulse filter */
	uint32_t hp = i915_read32(i915, PCH_PORT_HOTPLUG);
	hp &= ~(PORTB_PULSE_DURATION_MASK | PORTC_PULSE_DURATION_MASK | PORTD_PULSE_DURATION_MASK);
	hp |= PORTB_HOTPLUG_ENABLE | PORTC_HOTPLUG_ENABLE | PORTD_HOTPLUG_ENABLE;
	if (pch_has_port_a_hpd(i915))
		hp |= PORTA_HOTPLUG_ENABLE;
	i915_write32(i915, PCH_PORT_HOTPLUG, hp);
	uint32_t mask = SDE_PORTB_HOTPLUG_CPT | SDE_PORTC_HOTPLUG_CPT | SDE_PORTD_HOTPLUG_CPT;
	if (pch_has_port_a_hpd(i915)) {
		uint32_t hp2 = i915_read32(i915, PCH_PORT_HOTPLUG2);
		i915_write32(i915, PCH_PORT_HOTPLUG2, hp2 | PORTE_HOTPLUG_ENABLE);
		mask |= SDE_PORTA_HOTPLUG_SPT | SDE_PORTE_HOTPLUG_SPT;
	}
	d->hpd_irq_mask = mask;
	sde_irq_enable(i915, mask);
	return 0;
}

/* Interrupt context: the north display's hotplug bank read and cleared.
 * On display version 14 that is PICA's, which reaches the CPU through
 * the PCH bank's bit 31: PICA's IER is held off while its IIR and the
 * PCH's summary bit are cleared, so that an event arriving meanwhile
 * sets both again rather than neither. */
uint32_t intel_hpd_de_hpd_ack(struct i915_device *i915)
{
	uint32_t iir;

	if (i915->info->display_ver >= 14) {
		iir = i915_read32_fw(i915, PICAINTERRUPT_IIR);
		if (!iir)
			return 0;
		uint32_t ier = i915_read32_fw(i915, PICAINTERRUPT_IER);
		i915_write32_fw(i915, PICAINTERRUPT_IER, 0);
		iir = i915_read32_fw(i915, PICAINTERRUPT_IIR);
		i915_write32_fw(i915, PICAINTERRUPT_IIR, iir);
		i915_write32_fw(i915, SDEIIR, SDE_PICAINTERRUPT);
		i915_write32_fw(i915, PICAINTERRUPT_IER, ier);
		return iir;
	}
	if (i915->info->display_ver >= 11) {
		iir = i915_read32_fw(i915, GEN11_DE_HPD_IIR);
		if (iir)
			i915_write32_fw(i915, GEN11_DE_HPD_IIR, iir);
		return iir;
	}
	return 0;
}

void intel_hpd_de_hpd_reset(struct i915_device *i915)
{
	uint32_t imr, iir, ier;

	if (i915->info->display_ver < 11)
		return;
	north_hpd_regs(i915, &imr, &iir, &ier);
	i915_write32_fw(i915, imr, ~0u);
	i915_write32_fw(i915, ier, 0);
	/* twice: a second event may have been latched behind the first */
	i915_write32_fw(i915, iir, ~0u);
	i915_write32_fw(i915, iir, ~0u);
	(void)i915_read32_fw(i915, iir);
}

/* Interrupt context. */
void intel_display_hpd_irq(struct i915_device *i915, uint32_t de_port_iir,
			   uint32_t pch_iir, uint32_t de_hpd_iir)
{
	struct intel_display *d = &i915->display;
	enum hpd_style style = hpd_style(i915);
	uint32_t ports = 0;

	if (style == HPD_STYLE_BXT) {
		uint32_t trig = de_port_iir & d->hpd_irq_mask_port;
		if (!trig)
			return;
		uint32_t st = i915_read32_fw(i915, BXT_HOTPLUG_CTL);
		i915_write32_fw(i915, BXT_HOTPLUG_CTL, st);
		if (trig & BXT_DE_PORT_HP_DDIA)
			ports |= 1u << PORT_A;
		if (trig & BXT_DE_PORT_HP_DDIB)
			ports |= 1u << PORT_B;
		if (trig & BXT_DE_PORT_HP_DDIC)
			ports |= 1u << PORT_C;
	} else if (style == HPD_STYLE_ICP || style == HPD_STYLE_DG1 || style == HPD_STYLE_MTP) {
		uint32_t trig = pch_iir & d->hpd_irq_mask;
		uint32_t trig_de = de_hpd_iir & d->hpd_irq_mask_de;
		if (!trig && !trig_de)
			return;
		/* the status bits clear on write-back */
		if (trig) {
			uint32_t st = i915_read32_fw(i915, SHOTPLUG_CTL_DDI);
			i915_write32_fw(i915, SHOTPLUG_CTL_DDI, st);
			st = i915_read32_fw(i915, SHOTPLUG_CTL_TC);
			i915_write32_fw(i915, SHOTPLUG_CTL_TC, st);
		}
		if (trig_de && style == HPD_STYLE_MTP) {
			for (int t = 0; t < 4; t++) {
				if (!(trig_de & (XELPDP_TBT_HOTPLUG(t) | XELPDP_DP_ALT_HOTPLUG(t))))
					continue;
				uint32_t st = i915_read32_fw(i915, XELPDP_PORT_HOTPLUG_CTL(t));
				i915_write32_fw(i915, XELPDP_PORT_HOTPLUG_CTL(t), st);
			}
		} else if (trig_de) {
			uint32_t st = i915_read32_fw(i915, GEN11_TC_HOTPLUG_CTL);
			i915_write32_fw(i915, GEN11_TC_HOTPLUG_CTL, st);
			st = i915_read32_fw(i915, GEN11_TBT_HOTPLUG_CTL);
			i915_write32_fw(i915, GEN11_TBT_HOTPLUG_CTL, st);
		}
		for (int i = 0; i < d->nout; i++) {
			int tc, port = d->outputs[i].port;
			int pin = port_hpd_pin(i915, port, &tc);
			if (pin < 0)
				continue;
			if (trig & sde_pin_bit(i915, pin, tc))
				ports |= 1u << port;
			if (tc && pin < 6 &&
			    (trig_de & (GEN11_TC_HOTPLUG(pin) | GEN11_TBT_HOTPLUG(pin))))
				ports |= 1u << port;
		}
	} else if (style == HPD_STYLE_SPT) {
		uint32_t trig = pch_iir & d->hpd_irq_mask;
		if (!trig)
			return;
		/* the per-port long/short status bits clear on write-back */
		uint32_t st = i915_read32_fw(i915, PCH_PORT_HOTPLUG);
		i915_write32_fw(i915, PCH_PORT_HOTPLUG, st);
		if (pch_has_port_a_hpd(i915)) {
			uint32_t st2 = i915_read32_fw(i915, PCH_PORT_HOTPLUG2);
			i915_write32_fw(i915, PCH_PORT_HOTPLUG2, st2);
		}
		if (trig & SDE_PORTA_HOTPLUG_SPT)
			ports |= 1u << PORT_A;
		if (trig & SDE_PORTB_HOTPLUG_CPT)
			ports |= 1u << PORT_B;
		if (trig & SDE_PORTC_HOTPLUG_CPT)
			ports |= 1u << PORT_C;
		if (trig & SDE_PORTD_HOTPLUG_CPT)
			ports |= 1u << PORT_D;
		if (trig & SDE_PORTE_HOTPLUG_SPT)
			ports |= 1u << PORT_E;
	} else {
		return;
	}
	if (!ports)
		return;
	__sync_fetch_and_or(&d->hpd_pending, ports);
	d->hpd_events++;
	if (d->hpd_ready)
		poll_notify_wq(&d->hpd_wq);
}

/* ---- the worker --------------------------------------------------------------- */

static uint8_t g_hpd_stack[16384] __attribute__((aligned(16)));

static void hpd_worker(void *arg)
{
	struct i915_device *i915 = arg;
	struct intel_display *d = &i915->display;
	task_t *cur = sched_current();

	d->hpd_ready = 1;
	for (;;) {
		uint32_t pending = __sync_lock_test_and_set(&d->hpd_pending, 0);
		if (pending) {
			uint64_t rem;
			hrtimer_sleep_until(hrtimer_now_ns() + HPD_DEBOUNCE_NS, &rem);
			/* more pulses during the wait fold into this pass */
			pending |= __sync_lock_test_and_set(&d->hpd_pending, 0);
			/* not in the middle of a mode set or a client's probe:
			 * they use the same AUX channel and panel power */
			intel_display_lock(i915);
			for (int i = 0; i < d->nout; i++) {
				struct intel_output *o = &d->outputs[i];
				if (!o->present || !(pending & (1u << o->port)))
					continue;
				/* A panel is never plugged or pulled: a pulse on
				 * its pin is its own power sequencing (a mode set
				 * powering it down and up, VDD for a probe), and
				 * re-probing it then reads a panel that is off --
				 * its modes cleared, the connector reported gone --
				 * and powers VDD again, which pulses the pin again. */
				if (o->is_edp)
					continue;
				/* Looked at again by the core: it re-probes the
				 * connector, bumps the epoch clients read from
				 * /sys and logs a change of status. */
				o->detected = 0;
				o->edid_len = 0;
				drm_connector_hotplug(&i915->drm, o->conn);
				/* The link of an output still lit may need
				 * repairing after its sink went away and came
				 * back; a port whose sink is not settled yet is
				 * looked at again after another debounce. */
				int again = 0;
				if (o->type == INTEL_OUTPUT_DP)
					again = intel_dp_hpd_check(i915, o);
				else if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI)
					again = intel_hdmi_hpd_check(i915, o);
				if (again)
					__sync_fetch_and_or(&d->hpd_pending, 1u << o->port);
			}
			intel_display_unlock(i915);
			continue;
		}
		struct wait_queue_entry we;
		uint64_t fl = local_irq_save();
		wq_entry_init(&we, cur);
		wq_add(&d->hpd_wq, &we);
		if (d->hpd_pending) {
			local_irq_restore(fl);
			wq_remove(&d->hpd_wq, &we);
			continue;
		}
		cur->wait_channel = &d->hpd_wq;
		cur->state = TASK_BLOCKED;
		local_irq_restore(fl);
		sched_schedule();
		cur->wait_channel = NULL;
		wq_remove(&d->hpd_wq, &we);
	}
}

int intel_hpd_start(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	if (!d->ready || d->hpd_worker ||
	    (!d->hpd_irq_mask && !d->hpd_irq_mask_de && !d->hpd_irq_mask_port))
		return 0;
	d->hpd_worker = sched_add_task(hpd_worker, i915, g_hpd_stack, sizeof(g_hpd_stack));
	if (!d->hpd_worker)
		return -ENOMEM;
	i915_dbg("[drm] i915: hotplug detection on (south %08x, north %08x, port %08x)\n",
		 d->hpd_irq_mask, d->hpd_irq_mask_de, d->hpd_irq_mask_port);
	return 0;
}
