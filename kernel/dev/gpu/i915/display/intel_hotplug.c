// LikeOS-64 -- hotplug: noticing a sink being plugged in or pulled.
//
// Each DDI has a hotplug detect pin.  The south display engine (the
// PCH) filters its pulses -- a long one is a plug or an unplug, a short
// one a DisplayPort sink asking for attention -- and raises an interrupt
// naming the port.  The interrupt handler only records which ports
// changed and wakes a worker; the worker waits out the bounce, asks the
// port what is there now (DPCD over AUX, the EDID over DDC), and updates
// the connector's status and mode list.  A client that asks the
// connector again (a display server re-probing) sees the new sink.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/waitq.h>

#define HPD_DEBOUNCE_NS 150000000ULL /* 150 ms after the last pulse */

/* ---- the pins --------------------------------------------------------------- */

static int pch_style(struct i915_device *i915)
{
	/* 0: no PCH pins; 1: Lynx/Wildcat/Sunrise/Cannon Point; 2: Ice Point+ */
	if (!(i915->info->flags & I915_INFO_HAS_PCH))
		return 0;
	return i915->pch >= I915_PCH_ICP ? 2 : 1;
}

/* Ice Point+: the DDI pins are A..C (D on some), the Type-C pins TC1..6. */
static int icp_pin_is_tc(struct i915_device *i915, int port)
{
	if (i915->display.model == INTEL_DISPLAY_TGL)
		return port >= PORT_TC1;
	return port >= PORT_C; /* Ice Lake: C..F are TC1..4 */
}

int intel_hpd_live(struct i915_device *i915, int port)
{
	struct intel_display *d = &i915->display;
	int style = pch_style(i915);

	if (d->model == INTEL_DISPLAY_BXT) {
		uint32_t isr = i915_read32(i915, GEN8_DE_PORT_ISR);
		switch (port) {
		case PORT_A: return !!(isr & BXT_DE_PORT_HP_DDIA);
		case PORT_B: return !!(isr & BXT_DE_PORT_HP_DDIB);
		case PORT_C: return !!(isr & BXT_DE_PORT_HP_DDIC);
		default: return 0;
		}
	}
	if (style == 2) {
		uint32_t isr = i915_read32(i915, SDEISR);
		if (icp_pin_is_tc(i915, port)) {
			int tc = d->model == INTEL_DISPLAY_TGL ? port - PORT_TC1 : port - PORT_C;
			/* a Type-C sink shows in the PCH's TC pin (legacy mode)
			 * or in the north display's (DP-alt / Thunderbolt) */
			uint32_t de = i915_read32(i915, GEN11_DE_HPD_ISR);
			return !!(isr & SDE_TC_HOTPLUG_ICP(tc)) ||
			       !!(de & (GEN11_TC_HOTPLUG(tc) | GEN11_TBT_HOTPLUG(tc)));
		}
		return !!(isr & SDE_DDI_HOTPLUG_ICP(port));
	}
	if (style == 1) {
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

int intel_hpd_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	int style = pch_style(i915);

	wq_head_init(&d->hpd_wq, "i915_hpd");
	d->hpd_pending = 0;
	d->hpd_irq_mask = 0;
	d->hpd_irq_mask_de = 0;
	d->hpd_irq_mask_port = 0;

	if (d->model == INTEL_DISPLAY_BXT) {
		uint32_t hp = i915_read32(i915, BXT_HOTPLUG_CTL);
		hp |= BXT_DDIA_HPD_ENABLE | BXT_DDIB_HPD_ENABLE | BXT_DDIC_HPD_ENABLE;
		i915_write32(i915, BXT_HOTPLUG_CTL, hp);
		uint32_t mask = BXT_DE_PORT_HP_DDIA | BXT_DE_PORT_HP_DDIB | BXT_DE_PORT_HP_DDIC;
		d->hpd_irq_mask_port = mask;
		if (i915->irq_vector >= 0) {
			i915_write32_fw(i915, GEN8_DE_PORT_IIR, mask);
			i915_write32_fw(i915, GEN8_DE_PORT_IMR,
					i915_read32_fw(i915, GEN8_DE_PORT_IMR) & ~mask);
			i915_write32_fw(i915, GEN8_DE_PORT_IER,
					i915_read32_fw(i915, GEN8_DE_PORT_IER) | mask);
			(void)i915_read32_fw(i915, GEN8_DE_PORT_IER);
		}
		return 0;
	}
	if (style == 2) {
		/* the DDI pins the board has, and every Type-C pin */
		uint32_t ddi = i915_read32(i915, SHOTPLUG_CTL_DDI);
		uint32_t tc = i915_read32(i915, SHOTPLUG_CTL_TC);
		uint32_t mask = 0, de = 0;
		int ntc = d->model == INTEL_DISPLAY_TGL ? 6 : 4;
		int nddi = d->model == INTEL_DISPLAY_TGL ? 3 : 2;
		for (int i = 0; i < d->nout; i++) {
			int port = d->outputs[i].port;
			if (icp_pin_is_tc(i915, port)) {
				int t = d->model == INTEL_DISPLAY_TGL ? port - PORT_TC1 : port - PORT_C;
				if (t < 0 || t >= ntc)
					continue;
				tc |= ICP_HPD_ENABLE(t);
				mask |= SDE_TC_HOTPLUG_ICP(t);
				de |= GEN11_TC_HOTPLUG(t) | GEN11_TBT_HOTPLUG(t);
			} else if (port < nddi + 1) {
				ddi |= ICP_HPD_ENABLE(port);
				mask |= SDE_DDI_HOTPLUG_ICP(port);
			}
		}
		i915_write32(i915, SHOTPLUG_CTL_DDI, ddi);
		i915_write32(i915, SHOTPLUG_CTL_TC, tc);
		if (de) {
			uint32_t ctl = i915_read32(i915, GEN11_TC_HOTPLUG_CTL);
			uint32_t tbt = i915_read32(i915, GEN11_TBT_HOTPLUG_CTL);
			for (int t = 0; t < ntc; t++)
				if (de & GEN11_TC_HOTPLUG(t)) {
					ctl |= GEN11_HOTPLUG_CTL_ENABLE(t);
					tbt |= GEN11_HOTPLUG_CTL_ENABLE(t);
				}
			i915_write32(i915, GEN11_TC_HOTPLUG_CTL, ctl);
			i915_write32(i915, GEN11_TBT_HOTPLUG_CTL, tbt);
		}
		d->hpd_irq_mask = mask;
		d->hpd_irq_mask_de = de;
		sde_irq_enable(i915, mask);
		if (i915->irq_vector >= 0 && de) {
			i915_write32_fw(i915, GEN11_DE_HPD_IIR, de);
			i915_write32_fw(i915, GEN11_DE_HPD_IMR,
					i915_read32_fw(i915, GEN11_DE_HPD_IMR) & ~de);
			i915_write32_fw(i915, GEN11_DE_HPD_IER,
					i915_read32_fw(i915, GEN11_DE_HPD_IER) | de);
			(void)i915_read32_fw(i915, GEN11_DE_HPD_IER);
		}
		return 0;
	}
	if (style != 1)
		return 0;
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

/* Interrupt context. */
void intel_display_hpd_irq(struct i915_device *i915, uint32_t de_port_iir,
			   uint32_t pch_iir, uint32_t de_hpd_iir)
{
	struct intel_display *d = &i915->display;
	uint32_t ports = 0;
	int style = pch_style(i915);

	if (d->model == INTEL_DISPLAY_BXT) {
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
	} else if (style == 2) {
		uint32_t trig = pch_iir & d->hpd_irq_mask;
		uint32_t trig_de = de_hpd_iir & d->hpd_irq_mask_de;
		if (!trig && !trig_de)
			return;
		/* the status bits clear on write-back */
		uint32_t st = i915_read32_fw(i915, SHOTPLUG_CTL_DDI);
		i915_write32_fw(i915, SHOTPLUG_CTL_DDI, st);
		st = i915_read32_fw(i915, SHOTPLUG_CTL_TC);
		i915_write32_fw(i915, SHOTPLUG_CTL_TC, st);
		if (trig_de) {
			st = i915_read32_fw(i915, GEN11_TC_HOTPLUG_CTL);
			i915_write32_fw(i915, GEN11_TC_HOTPLUG_CTL, st);
			st = i915_read32_fw(i915, GEN11_TBT_HOTPLUG_CTL);
			i915_write32_fw(i915, GEN11_TBT_HOTPLUG_CTL, st);
		}
		for (int i = 0; i < d->nout; i++) {
			int port = d->outputs[i].port;
			if (icp_pin_is_tc(i915, port)) {
				int t = d->model == INTEL_DISPLAY_TGL ? port - PORT_TC1 : port - PORT_C;
				if (t < 0 || t >= 6)
					continue;
				if ((trig & SDE_TC_HOTPLUG_ICP(t)) ||
				    (trig_de & (GEN11_TC_HOTPLUG(t) | GEN11_TBT_HOTPLUG(t))))
					ports |= 1u << port;
			} else if (trig & SDE_DDI_HOTPLUG_ICP(port)) {
				ports |= 1u << port;
			}
		}
	} else if (style == 1) {
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
			for (int i = 0; i < d->nout; i++) {
				struct intel_output *o = &d->outputs[i];
				if (!o->present || !(pending & (1u << o->port)))
					continue;
				/* Looked at again by the core: it re-probes the
				 * connector, bumps the epoch clients read from
				 * /sys and logs a change of status. */
				o->detected = 0;
				o->edid_len = 0;
				drm_connector_hotplug(&i915->drm, o->conn);
			}
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
