// LikeOS -- panel power sequencer, backlight PWM and panel fitters before DDI.
//
// A built-in panel needs three things the display block provides beside
// the pipe.  The panel power sequencer switches the panel's supply, its
// logic (VDD, which an eDP panel needs for its AUX channel) and the
// backlight enable in the order and with the delays the panel's maker
// asks for (T1 to T12, kept in the sequencer in 100 us units): there is
// one at 0x61200 on the GMCH parts (used by LVDS), one in the PCH of
// Ironlake to Ivy Bridge (LVDS or eDP, with a port select), and one per
// pipe A and B on Valleyview and Cherryview, each of which must be tied
// to the eDP port it drives and can be taken from another port.  The
// backlight is a PWM whose period is counted in display or raw clocks:
// gen2/3 keep frequency and duty in one register (with a "combination
// mode" that multiplies the duty by a byte in PCI config space), gen4 adds
// a second register with the enable, the pipe select and the polarity,
// Ironlake to Ivy Bridge split it into a CPU half (duty) and a PCH half
// (period, polarity), and Valleyview/Cherryview have one controller per
// pipe.  Last, the panel fitter scales a smaller picture onto the panel's
// one timing: a single shared fitter on the GMCH parts (pipe B only before
// gen4), one per pipe from Ironlake on.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2020-2024 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/hal/pci.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

static char pipe_name(int pipe)
{
	return pipe >= 0 && pipe < LG_MAX_PIPES ? (char)('A' + pipe) : '?';
}

static int is_vlv_chv(const struct lg_display *d)
{
	return d->is_vlv || d->is_chv;
}

/* ===================================================================== */
/* ---- the panel power sequencer -------------------------------------- */
/* ===================================================================== */

/* Valleyview/Cherryview: the sequencer of pipe A or B (display-relative). */
#define PNL_VLV_PPS_BASE(pipe) (PPS_BASE_GMCH + (uint32_t)(pipe) * 0x100u)

/* The sequencer's delays, in its own 100 us units. */
struct pps_delays {
	uint32_t power_up; /* T1+T3: supply on to AUX ready */
	uint32_t backlight_on; /* T8: video on to backlight on */
	uint32_t backlight_off; /* T9: backlight off to video off */
	uint32_t power_down; /* T10: video off to supply off */
	uint32_t power_cycle; /* T11+T12: supply off to supply on again */
};

/* What the sequencer code keeps per output (indexed like d->out[]). */
struct pps_state {
	int inited;
	int is_edp;
	/* Valleyview/Cherryview: the pipe whose sequencer drives this eDP
	 * port, and the pipe the port is enabled on (any DP port), -1 none. */
	int vlv_pps_pipe;
	int vlv_active_pipe;
	int bios_valid, delays_valid;
	struct pps_delays bios; /* what the firmware left programmed */
	struct pps_delays final; /* what is programmed */
	/* the same delays in ms, for the waits done in software */
	uint32_t power_up_ms, backlight_on_ms, backlight_off_ms;
	uint32_t power_down_ms, power_cycle_ms;
	int vdd_refs;
	/* The panel's supply went off: the full power cycle delay must pass
	 * before it may come on again.  There is no clock here to measure how
	 * much of it already passed, so the whole of it is waited out. */
	int cycle_pending;
	/* The panel was powered up: T8 before the backlight. */
	int backlight_wait_pending;
};

static struct pps_state g_pps[LG_MAX_OUTPUTS];

static int vlv_power_sequencer_pipe(struct lg_display *d, struct lg_output *o);
static void pps_init_delays(struct lg_display *d, struct lg_output *o);
static void pps_init_registers(struct lg_display *d, struct lg_output *o,
			       int force_disable_vdd);

static int out_index(struct lg_display *d, struct lg_output *o)
{
	long i = o - d->out;

	return (i >= 0 && i < LG_MAX_OUTPUTS) ? (int)i : 0;
}

static struct pps_state *pps_st(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = &g_pps[out_index(d, o)];

	if (!s->inited) {
		mm_memset(s, 0, sizeof(*s));
		s->inited = 1;
		s->vlv_pps_pipe = -1;
		s->vlv_active_pipe = -1;
	}
	return s;
}

static int pps_is_edp(struct lg_display *d, struct lg_output *o)
{
	return o->type == LG_OUTPUT_EDP || pps_st(d, o)->is_edp;
}

/* The sequencer the output's panel is on.  On Valleyview/Cherryview this
 * picks one (and ties it to the port) when the output has none yet. */
static uint32_t pps_base(struct lg_display *d, struct lg_output *o)
{
	if (is_vlv_chv(d))
		return PNL_VLV_PPS_BASE(vlv_power_sequencer_pipe(d, o));
	return d->gmch ? PPS_BASE_GMCH : PPS_BASE_PCH;
}

static const char *pps_name(struct lg_display *d, struct lg_output *o)
{
	if (is_vlv_chv(d)) {
		switch (pps_st(d, o)->vlv_pps_pipe) {
		case 0:
			return "PPS A";
		case 1:
			return "PPS B";
		default:
			return "PPS <none>";
		}
	}
	return "PPS";
}

/* PP_CONTROL with the unlock key in place: the sequencer ignores writes
 * to its other registers (and the PLLs refuse theirs) while it is locked
 * and the panel is on. */
static uint32_t pps_get_control(struct lg_display *d, uint32_t base)
{
	uint32_t v = lg_rd(d, PP_CONTROL(base));

	if ((v & PANEL_UNLOCK_MASK) != PANEL_UNLOCK_REGS) {
		v &= ~PANEL_UNLOCK_MASK;
		v |= PANEL_UNLOCK_REGS;
	}
	return v;
}

static int edp_have_panel_power(struct lg_display *d, struct lg_output *o)
{
	if (is_vlv_chv(d) && pps_st(d, o)->vlv_pps_pipe < 0)
		return 0;
	return !!(lg_rd(d, PP_STATUS(pps_base(d, o))) & PP_ON);
}

static int edp_have_panel_vdd(struct lg_display *d, struct lg_output *o)
{
	if (is_vlv_chv(d) && pps_st(d, o)->vlv_pps_pipe < 0)
		return 0;
	return !!(lg_rd(d, PP_CONTROL(pps_base(d, o))) & EDP_FORCE_VDD);
}

#define IDLE_ON_MASK (PP_ON | PP_SEQUENCE_MASK | PP_SEQUENCE_STATE_MASK)
#define IDLE_ON_VALUE (PP_ON | PP_SEQUENCE_NONE | PP_SEQUENCE_STATE_ON_IDLE)
#define IDLE_OFF_MASK (PP_ON | PP_SEQUENCE_MASK)
#define IDLE_OFF_VALUE (PP_SEQUENCE_NONE)
#define IDLE_CYCLE_MASK (PP_ON | PP_SEQUENCE_MASK | PP_CYCLE_DELAY_ACTIVE | PP_SEQUENCE_STATE_MASK)
#define IDLE_CYCLE_VALUE (PP_SEQUENCE_NONE | PP_SEQUENCE_STATE_OFF_IDLE)

static void wait_panel_status(struct lg_display *d, struct lg_output *o,
			      uint32_t mask, uint32_t value)
{
	uint32_t base = pps_base(d, o);

	i915_dbg("[drm] i915: %s %s: mask %08x value %08x PP_STATUS %08x PP_CONTROL %08x\n",
		 o->name, pps_name(d, o), mask, value, lg_rd(d, PP_STATUS(base)),
		 lg_rd(d, PP_CONTROL(base)));
	if (lg_wait(d, PP_STATUS(base), mask, value, 5000u * 1000u))
		kprintf("[drm] i915: %s %s: panel status timeout: PP_STATUS %08x PP_CONTROL %08x\n",
			o->name, pps_name(d, o), lg_rd(d, PP_STATUS(base)),
			lg_rd(d, PP_CONTROL(base)));
}

static void wait_panel_on(struct lg_display *d, struct lg_output *o)
{
	wait_panel_status(d, o, IDLE_ON_MASK, IDLE_ON_VALUE);
}

static void wait_panel_off(struct lg_display *d, struct lg_output *o)
{
	wait_panel_status(d, o, IDLE_OFF_MASK, IDLE_OFF_VALUE);
}

/* T11+T12 since the panel's supply last went off.  The sequencer counts
 * the cycle itself only when it switched the supply off; when VDD was
 * released last, the wait is ours. */
static void wait_panel_power_cycle(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);

	if (s->cycle_pending) {
		i915_dbg("[drm] i915: %s %s: waiting %u ms for the panel power cycle\n",
			 o->name, pps_name(d, o), s->power_cycle_ms);
		lg_mdelay(s->power_cycle_ms);
		s->cycle_pending = 0;
	}
	wait_panel_status(d, o, IDLE_CYCLE_MASK, IDLE_CYCLE_VALUE);
}

static void pps_vdd_off_sync(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);
	uint32_t base, pp;

	if (!edp_have_panel_vdd(d, o))
		return;

	base = pps_base(d, o);
	i915_dbg("[drm] i915: %s %s: VDD off\n", o->name, pps_name(d, o));
	pp = pps_get_control(d, base);
	pp &= ~EDP_FORCE_VDD;
	lg_wr(d, PP_CONTROL(base), pp);
	lg_posting_read(d, PP_CONTROL(base));

	/* With the panel off, dropping VDD is a power off of the panel. */
	if (!(pp & PANEL_POWER_ON))
		s->cycle_pending = 1;
}

/* ---- Valleyview / Cherryview: which pipe's sequencer drives which port -- */

/* The pipe a DisplayPort port is enabled on, -1 when it is off. */
static int vlv_port_hw_pipe(struct lg_display *d, struct lg_output *o)
{
	uint32_t v;

	if (!o->reg)
		return -1;
	v = lg_rd(d, o->reg);
	if (!(v & DP_PORT_EN))
		return -1;
	if (d->is_chv)
		return (int)((v & DP_PIPE_SEL_MASK_CHV) >> DP_PIPE_SEL_SHIFT_CHV);
	return (int)((v & DP_PIPE_SEL_MASK) >> 30);
}

static int vlv_active_pipe(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);

	if (s->vlv_active_pipe >= 0)
		return s->vlv_active_pipe;
	return vlv_port_hw_pipe(d, o);
}

static int is_dp_output(struct lg_display *d, int i)
{
	const struct lg_output *x = &d->out[i];

	return x->type == LG_OUTPUT_DP || x->type == LG_OUTPUT_EDP ||
	       (g_pps[i].inited && g_pps[i].is_edp);
}

/* A sequencer no port uses: neither an eDP port's own one nor the pipe a
 * plain DP port runs on. */
static int vlv_find_free_pps(struct lg_display *d)
{
	unsigned pipes = (1u << 0) | (1u << 1);

	for (int i = 0; i < d->nout && i < LG_MAX_OUTPUTS; i++) {
		struct lg_output *x = &d->out[i];
		struct pps_state *s;
		int p;

		if (!is_dp_output(d, i))
			continue;
		s = pps_st(d, x);
		if (pps_is_edp(d, x))
			p = s->vlv_pps_pipe;
		else
			p = vlv_active_pipe(d, x);
		if (p >= 0 && p < 2)
			pipes &= ~(1u << p);
	}
	if (!pipes)
		return -1;
	return (pipes & 1u) ? 0 : 1;
}

/* Let go of the output's sequencer: VDD off while it is still ours, and
 * the port select cleared.  Valleyview gets confused when two sequencers
 * select the same port (the port never reports ready); Cherryview does
 * not mind, but it is cleared there too. */
static void vlv_detach_power_sequencer(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);
	int pipe = s->vlv_pps_pipe;

	if (pipe != 0 && pipe != 1)
		return;

	pps_vdd_off_sync(d, o);

	i915_dbg("[drm] i915: detaching PPS %c from %s\n", pipe_name(pipe), o->name);
	lg_wr(d, PP_ON_DELAYS(PNL_VLV_PPS_BASE(pipe)), 0);
	lg_posting_read(d, PP_ON_DELAYS(PNL_VLV_PPS_BASE(pipe)));

	s->vlv_pps_pipe = -1;
}

static void vlv_steal_power_sequencer(struct lg_display *d, int pipe)
{
	for (int i = 0; i < d->nout && i < LG_MAX_OUTPUTS; i++) {
		struct lg_output *x = &d->out[i];
		struct pps_state *s;

		if (!is_dp_output(d, i))
			continue;
		s = pps_st(d, x);
		if (s->vlv_active_pipe == pipe)
			i915_dbg("[drm] i915: stealing PPS %c from active %s\n",
				 pipe_name(pipe), x->name);
		if (s->vlv_pps_pipe != pipe)
			continue;
		i915_dbg("[drm] i915: stealing PPS %c from %s\n", pipe_name(pipe), x->name);
		/* VDD off before it changes hands */
		vlv_detach_power_sequencer(d, x);
	}
}

/* The DP 1.62 GHz dividers the pipe's PLL runs for the sequencer kick. */
static const struct lg_dpll pnl_vlv_kick_dpll = {
	.dot = 162000, .p1 = 3, .p2 = 2, .n = 5, .m1 = 3, .m2 = 81,
};
static const struct lg_dpll pnl_chv_kick_dpll = {
	/* m2 is 22.22 fixed point: 32.4 */
	.dot = 162000, .p1 = 4, .p2 = 2, .n = 1, .m1 = 2, .m2 = 0x819999a,
};

/* A sequencer locks onto its port only once the port was enabled with
 * the sequencer's pipe selected; until then not even forced VDD reaches
 * the panel.  So the port is switched on and off once, with the pipe's
 * PLL running (forced on for the moment if it is not). */
static void vlv_power_sequencer_kick(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);
	int pipe = s->vlv_pps_pipe;
	int pll_enabled;
	uint32_t dp;

	if (pipe < 0)
		return;
	if (lg_rd(d, o->reg) & DP_PORT_EN) {
		i915_dbg("[drm] i915: skipping PPS %c kick, %s is active\n",
			 pipe_name(pipe), o->name);
		return;
	}
	i915_dbg("[drm] i915: kicking PPS %c for %s\n", pipe_name(pipe), o->name);

	/* keep the strap's detected bit, which is read-only */
	dp = lg_rd(d, o->reg) & DP_DETECTED;
	dp |= DP_VOLTAGE_0_4 | DP_PRE_EMPHASIS_0;
	dp |= DP_PORT_WIDTH(1);
	dp |= DP_LINK_TRAIN_PAT_1;
	if (d->is_chv)
		dp |= DP_PIPE_SEL_CHV(pipe);
	else
		dp |= DP_PIPE_SEL(pipe);

	pll_enabled = !!(lg_rd(d, DPLL(d, pipe)) & DPLL_VCO_ENABLE);
	if (!pll_enabled &&
	    lg_vlv_force_pll_on(d, pipe, d->is_chv ? &pnl_chv_kick_dpll : &pnl_vlv_kick_dpll)) {
		kprintf("[drm] i915: failed to force on the PLL of pipe %c\n", pipe_name(pipe));
		return;
	}

	lg_wr(d, o->reg, dp);
	lg_posting_read(d, o->reg);
	lg_wr(d, o->reg, dp | DP_PORT_EN);
	lg_posting_read(d, o->reg);
	lg_wr(d, o->reg, dp & ~DP_PORT_EN);
	lg_posting_read(d, o->reg);

	if (!pll_enabled)
		lg_vlv_force_pll_off(d, pipe);
}

/* The output's sequencer; when it has none, a free one (or A), taken from
 * whoever had it, set up and locked onto the port. */
static int vlv_power_sequencer_pipe(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);
	int pipe;

	if (s->vlv_pps_pipe >= 0)
		return s->vlv_pps_pipe;

	pipe = vlv_find_free_pps(d);
	/* Two sequencers for at most two eDP ports: there always is one. */
	if (pipe < 0)
		pipe = 0;

	vlv_steal_power_sequencer(d, pipe);
	s->vlv_pps_pipe = pipe;
	i915_dbg("[drm] i915: picked PPS %c for %s\n", pipe_name(pipe), o->name);

	pps_init_delays(d, o);
	pps_init_registers(d, o, 1);
	vlv_power_sequencer_kick(d, o);

	return s->vlv_pps_pipe;
}

/* The sequencer the firmware left on this port: the one with the panel
 * on, else the one with VDD forced, else any selecting the port. */
static void vlv_initial_power_sequencer_setup(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);

	s->vlv_pps_pipe = -1;
	for (int check = 0; check < 3 && s->vlv_pps_pipe < 0; check++) {
		for (int pipe = 0; pipe < 2; pipe++) {
			uint32_t base = PNL_VLV_PPS_BASE(pipe);
			uint32_t sel = lg_rd(d, PP_ON_DELAYS(base)) & PANEL_PORT_SELECT_MASK;

			if (sel != PANEL_PORT_SELECT_VLV(o->port))
				continue;
			if (check == 0 && !(lg_rd(d, PP_STATUS(base)) & PP_ON))
				continue;
			if (check == 1 && !(lg_rd(d, PP_CONTROL(base)) & EDP_FORCE_VDD))
				continue;
			s->vlv_pps_pipe = pipe;
			break;
		}
	}
	if (s->vlv_pps_pipe < 0)
		i915_dbg("[drm] i915: %s: no initial power sequencer\n", o->name);
	else
		i915_dbg("[drm] i915: %s: initial power sequencer: PPS %c\n", o->name,
			 pipe_name(s->vlv_pps_pipe));
}

/* ---- the delays -------------------------------------------------------- */

static void pps_readout_hw_state(struct lg_display *d, struct lg_output *o,
				 struct pps_delays *seq)
{
	uint32_t base = pps_base(d, o);
	uint32_t ctl = pps_get_control(d, base);
	uint32_t on, off, div, cycle;

	/* make sure the sequencer is unlocked */
	lg_wr(d, PP_CONTROL(base), ctl);

	on = lg_rd(d, PP_ON_DELAYS(base));
	off = lg_rd(d, PP_OFF_DELAYS(base));
	div = lg_rd(d, PP_DIVISOR(base));

	seq->power_up = (on & PANEL_POWER_UP_DELAY_MASK) >> PANEL_POWER_UP_DELAY_SHIFT;
	seq->backlight_on = on & PANEL_LIGHT_ON_DELAY_MASK;
	seq->backlight_off = off & PANEL_LIGHT_OFF_DELAY_MASK;
	seq->power_down = (off & PANEL_POWER_DOWN_DELAY_MASK) >> PANEL_POWER_DOWN_DELAY_SHIFT;
	/* the register holds <delay> + 1 in 100 ms units */
	cycle = div & PANEL_POWER_CYCLE_DELAY_MASK;
	seq->power_cycle = cycle ? (cycle - 1) * 1000 : 0;
}

static int pps_delays_valid(const struct pps_delays *p)
{
	return p->power_up || p->backlight_on || p->backlight_off || p->power_down ||
	       p->power_cycle;
}

static uint32_t pps_units_to_ms(uint32_t v)
{
	return DIV_ROUND_UP(v, 10);
}

static void pps_dump(const char *what, const struct pps_delays *p)
{
	i915_dbg("[drm] i915: PPS %s: power_up %u backlight_on %u backlight_off %u power_down %u power_cycle %u\n",
		 what, p->power_up, p->backlight_on, p->backlight_off, p->power_down,
		 p->power_cycle);
	(void)what;
	(void)p;
}

#define PPS_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Each delay is the longest of what the firmware programmed and what the
 * VBT says; when both are zero, the eDP 1.3 upper limit. */
static void pps_init_delays(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);
	struct pps_delays cur, vbt, spec, *f = &s->final;

	if (s->delays_valid)
		return;

	if (!s->bios_valid || !pps_delays_valid(&s->bios)) {
		pps_readout_hw_state(d, o, &s->bios);
		s->bios_valid = 1;
	}
	cur = s->bios;
	pps_dump("bios", &cur);

	mm_memset(&vbt, 0, sizeof(vbt));
	if (d->vbt.edp_valid) {
		vbt.power_up = d->vbt.edp_t1_t3;
		vbt.backlight_on = d->vbt.edp_t8;
		vbt.backlight_off = d->vbt.edp_t9;
		vbt.power_down = d->vbt.edp_t10;
		vbt.power_cycle = d->vbt.edp_t11_t12;
		pps_dump("vbt", &vbt);
	}

	/* upper limits from the eDP 1.3 spec, in 100 us units */
	spec.power_up = (10 + 200) * 10; /* T1+T3 */
	spec.backlight_on = 50 * 10; /* no limit for T8: T7's */
	spec.backlight_off = 50 * 10; /* no limit for T9: as T8 */
	spec.power_down = 500 * 10; /* T10 */
	spec.power_cycle = (10 + 500) * 10; /* T11+T12 */

#define ASSIGN_FINAL(field)                                                    \
	f->field = PPS_MAX(cur.field, vbt.field) == 0 ? spec.field :           \
						      PPS_MAX(cur.field, vbt.field)
	ASSIGN_FINAL(power_up);
	ASSIGN_FINAL(backlight_on);
	ASSIGN_FINAL(backlight_off);
	ASSIGN_FINAL(power_down);
	ASSIGN_FINAL(power_cycle);
#undef ASSIGN_FINAL

	s->power_up_ms = pps_units_to_ms(f->power_up);
	s->backlight_on_ms = pps_units_to_ms(f->backlight_on);
	s->backlight_off_ms = pps_units_to_ms(f->backlight_off);
	s->power_down_ms = pps_units_to_ms(f->power_down);
	s->power_cycle_ms = pps_units_to_ms(f->power_cycle);

	i915_dbg("[drm] i915: %s: panel power up delay %u, power down delay %u, power cycle delay %u, backlight on delay %u, off delay %u (ms)\n",
		 o->name, s->power_up_ms, s->power_down_ms, s->power_cycle_ms,
		 s->backlight_on_ms, s->backlight_off_ms);

	/* The backlight delays are waited out in software (T8 because the
	 * picture must be stable first, T9 because otherwise it would be
	 * waited twice: once here and once more by the sequencer when the
	 * panel goes off), so the hardware gets the shortest. */
	f->backlight_on = 1;
	f->backlight_off = 1;

	/* the hardware counts the power cycle in 100 ms steps */
	f->power_cycle = DIV_ROUND_UP(f->power_cycle, 1000) * 1000;

	s->delays_valid = 1;
}

static void pps_init_registers(struct lg_display *d, struct lg_output *o,
			       int force_disable_vdd)
{
	struct pps_state *s = pps_st(d, o);
	const struct pps_delays *seq = &s->final;
	uint32_t base = pps_base(d, o);
	uint32_t pp_on, pp_off, pp_div, port_sel = 0;
	uint32_t div_mhz = d->rawclk_khz / 1000;

	/* Some Valleyview firmware leaves VDD forced on sequencers no port
	 * uses; taking such a sequencer over must not inherit that (it would
	 * also switch the new port's VDD on behind our back). */
	if (force_disable_vdd) {
		uint32_t pp = pps_get_control(d, base);

		if (pp & PANEL_POWER_ON)
			i915_dbg("[drm] i915: %s %s: panel power already on\n", o->name,
				 pps_name(d, o));
		if (pp & EDP_FORCE_VDD)
			i915_dbg("[drm] i915: %s %s: VDD already on, disabling first\n",
				 o->name, pps_name(d, o));
		pp &= ~EDP_FORCE_VDD;
		lg_wr(d, PP_CONTROL(base), pp);
	}

	pp_on = ((seq->power_up << PANEL_POWER_UP_DELAY_SHIFT) & PANEL_POWER_UP_DELAY_MASK) |
		(seq->backlight_on & PANEL_LIGHT_ON_DELAY_MASK);
	pp_off = (seq->backlight_off & PANEL_LIGHT_OFF_DELAY_MASK) |
		 ((seq->power_down << PANEL_POWER_DOWN_DELAY_SHIFT) &
		  PANEL_POWER_DOWN_DELAY_MASK);

	if (is_vlv_chv(d)) {
		port_sel = PANEL_PORT_SELECT_VLV(o->port);
	} else if (!d->gmch) {
		switch (o->port) {
		case LG_PORT_A:
			port_sel = PANEL_PORT_SELECT_DPA;
			break;
		case LG_PORT_C:
			port_sel = PANEL_PORT_SELECT_DPC;
			break;
		case LG_PORT_D:
			port_sel = PANEL_PORT_SELECT_DPD;
			break;
		default:
			kprintf("[drm] i915: %s: no panel power sequencer port select for port %d\n",
				o->name, o->port);
			break;
		}
	}
	pp_on |= port_sel;

	lg_wr(d, PP_ON_DELAYS(base), pp_on);
	lg_wr(d, PP_OFF_DELAYS(base), pp_off);

	/* the sequencer's clock: the raw clock divided to 10 kHz, as the
	 * hardware documentation gives the formula */
	if (div_mhz)
		pp_div = (((100 * div_mhz) / 2 - 1) << PP_REFERENCE_DIVIDER_SHIFT) &
			 PP_REFERENCE_DIVIDER_MASK;
	else
		pp_div = lg_rd(d, PP_DIVISOR(base)) & PP_REFERENCE_DIVIDER_MASK;
	pp_div |= (DIV_ROUND_UP(seq->power_cycle, 1000) + 1) & PANEL_POWER_CYCLE_DELAY_MASK;
	lg_wr(d, PP_DIVISOR(base), pp_div);

	i915_dbg("[drm] i915: %s %s: PP_ON %08x PP_OFF %08x PP_DIV %08x\n", o->name,
		 pps_name(d, o), lg_rd(d, PP_ON_DELAYS(base)), lg_rd(d, PP_OFF_DELAYS(base)),
		 lg_rd(d, PP_DIVISOR(base)));
}

/* ---- the entry points ------------------------------------------------- */

void lg_pps_init(struct lg_display *d)
{
	int n = is_vlv_chv(d) ? 2 : 1;

	/* Unlock every sequencer the part has: Cougar/Panther Point is known
	 * to need it, and it does no harm wherever the registers can be write
	 * protected (the DPLLs refuse writes behind a locked sequencer with
	 * the panel on). */
	for (int i = 0; i < n; i++) {
		uint32_t base;

		if (is_vlv_chv(d))
			base = PNL_VLV_PPS_BASE(i);
		else
			base = d->gmch ? PPS_BASE_GMCH : PPS_BASE_PCH;
		lg_rmw(d, PP_CONTROL(base), PANEL_UNLOCK_MASK, PANEL_UNLOCK_REGS);
	}

	/* eDP panels already known (after a resume, say): their sequencer
	 * set up again in case the firmware changed it.  On Valleyview and
	 * Cherryview the sequencers forget which port they served, so each
	 * idle eDP port finds (or later picks) its sequencer anew. */
	for (int i = 0; i < d->nout && i < LG_MAX_OUTPUTS; i++) {
		struct lg_output *o = &d->out[i];
		struct pps_state *s;

		if (!g_pps[i].inited || !g_pps[i].is_edp)
			continue;
		s = pps_st(d, o);
		if (is_vlv_chv(d)) {
			if (o->active)
				continue;
			vlv_initial_power_sequencer_setup(d, o);
			if (s->vlv_pps_pipe < 0)
				continue;
		}
		pps_init_delays(d, o);
		pps_init_registers(d, o, 0);
	}
}

void lg_pps_edp_init(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s = pps_st(d, o);

	s->is_edp = 1;
	s->vdd_refs = 0;
	/* The panel's supply may have been switched by the firmware just
	 * before; assume enough time passed, as the firmware had to. */
	s->cycle_pending = 0;
	s->backlight_wait_pending = 0;

	if (is_vlv_chv(d)) {
		vlv_initial_power_sequencer_setup(d, o);
		if (s->vlv_pps_pipe < 0) {
			/* none left by the firmware: take one now */
			vlv_power_sequencer_pipe(d, o);
			goto out;
		}
	}

	pps_init_delays(d, o);
	pps_init_registers(d, o, 0);

out:
	/* VDD the firmware left forced on stays until the first
	 * lg_pps_vdd_off releases it. */
	if (edp_have_panel_vdd(d, o))
		i915_dbg("[drm] i915: %s %s: VDD left on by the firmware\n", o->name,
			 pps_name(d, o));
	kprintf("[drm] i915: %s: panel power sequencer %s: T1+T3 %u ms, T8 %u ms, T9 %u ms, T10 %u ms, T11+T12 %u ms\n",
		o->name, pps_name(d, o), s->power_up_ms, s->backlight_on_ms,
		s->backlight_off_ms, s->power_down_ms, s->power_cycle_ms);
}

/* Callers may reach the sequencer before lg_pps_edp_init ran. */
static int pps_ready(struct lg_display *d, struct lg_output *o)
{
	if (!pps_is_edp(d, o))
		return 0;
	if (!pps_st(d, o)->is_edp)
		lg_pps_edp_init(d, o);
	return 1;
}

/* VDD on: the panel's logic powered so its AUX channel answers.  Calls
 * nest; the last lg_pps_vdd_off releases it. */
void lg_pps_vdd_on(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s;
	uint32_t base, pp;

	if (!pps_ready(d, o))
		return;
	s = pps_st(d, o);
	s->vdd_refs++;

	if (edp_have_panel_vdd(d, o))
		return;

	base = pps_base(d, o);
	i915_dbg("[drm] i915: %s %s: VDD on\n", o->name, pps_name(d, o));

	if (!edp_have_panel_power(d, o))
		wait_panel_power_cycle(d, o);

	pp = pps_get_control(d, base);
	pp |= EDP_FORCE_VDD;
	lg_wr(d, PP_CONTROL(base), pp);
	lg_posting_read(d, PP_CONTROL(base));
	i915_dbg("[drm] i915: %s %s: PP_STATUS %08x PP_CONTROL %08x\n", o->name,
		 pps_name(d, o), lg_rd(d, PP_STATUS(base)), lg_rd(d, PP_CONTROL(base)));

	/* with the panel off, give it T1+T3 before the AUX channel is used */
	if (!edp_have_panel_power(d, o)) {
		i915_dbg("[drm] i915: %s %s: panel power was off\n", o->name, pps_name(d, o));
		lg_mdelay(s->power_up_ms);
	}
}

/* VDD released, at once (no deferred release here). */
void lg_pps_vdd_off(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s;

	if (!pps_ready(d, o))
		return;
	s = pps_st(d, o);
	if (s->vdd_refs > 0)
		s->vdd_refs--;
	if (s->vdd_refs > 0)
		return;
	pps_vdd_off_sync(d, o);
}

void lg_pps_on(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s;
	uint32_t base, pp;

	if (!pps_ready(d, o))
		return;
	s = pps_st(d, o);

	i915_dbg("[drm] i915: %s %s: panel power on\n", o->name, pps_name(d, o));
	if (edp_have_panel_power(d, o)) {
		i915_dbg("[drm] i915: %s %s: panel power already on\n", o->name,
			 pps_name(d, o));
		return;
	}

	wait_panel_power_cycle(d, o);

	base = pps_base(d, o);
	pp = pps_get_control(d, base);
	if (d->is_ilk) {
		/* Ironlake: the reset bit must be clear around the power
		 * sequence */
		pp &= ~PANEL_POWER_RESET;
		lg_wr(d, PP_CONTROL(base), pp);
		lg_posting_read(d, PP_CONTROL(base));
	}

	pp |= PANEL_POWER_ON;
	if (!d->is_ilk)
		pp |= PANEL_POWER_RESET;
	lg_wr(d, PP_CONTROL(base), pp);
	lg_posting_read(d, PP_CONTROL(base));

	wait_panel_on(d, o);
	s->backlight_wait_pending = 1;

	if (d->is_ilk) {
		pp |= PANEL_POWER_RESET; /* the reset bit back */
		lg_wr(d, PP_CONTROL(base), pp);
		lg_posting_read(d, PP_CONTROL(base));
	}
}

void lg_pps_off(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s;
	uint32_t base, pp;

	if (!pps_ready(d, o))
		return;
	s = pps_st(d, o);

	i915_dbg("[drm] i915: %s %s: panel power off\n", o->name, pps_name(d, o));
	if (!s->vdd_refs)
		i915_dbg("[drm] i915: %s %s: VDD not held to turn the panel off\n", o->name,
			 pps_name(d, o));

	base = pps_base(d, o);
	pp = pps_get_control(d, base);
	/* Panel power and forced VDD go off together; some panels stop
	 * working when VDD outlives the panel power. */
	pp &= ~(PANEL_POWER_ON | PANEL_POWER_RESET | EDP_FORCE_VDD | EDP_BLC_ENABLE);
	s->vdd_refs = 0;

	lg_wr(d, PP_CONTROL(base), pp);
	lg_posting_read(d, PP_CONTROL(base));

	wait_panel_off(d, o);
	s->cycle_pending = 1;
	s->backlight_wait_pending = 0;
}

void lg_pps_backlight_on(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s;
	uint32_t base, pp;

	if (!pps_ready(d, o))
		return;
	s = pps_st(d, o);

	/* Right after the panel came on the backlight would show it
	 * syncing with the link (a flicker): T8 first. */
	if (s->backlight_wait_pending) {
		lg_mdelay(s->backlight_on_ms);
		s->backlight_wait_pending = 0;
	}

	base = pps_base(d, o);
	pp = pps_get_control(d, base);
	pp |= EDP_BLC_ENABLE;
	lg_wr(d, PP_CONTROL(base), pp);
	lg_posting_read(d, PP_CONTROL(base));
}

void lg_pps_backlight_off(struct lg_display *d, struct lg_output *o)
{
	struct pps_state *s;
	uint32_t base, pp;

	if (!pps_ready(d, o))
		return;
	s = pps_st(d, o);

	base = pps_base(d, o);
	pp = pps_get_control(d, base);
	pp &= ~EDP_BLC_ENABLE;
	lg_wr(d, PP_CONTROL(base), pp);
	lg_posting_read(d, PP_CONTROL(base));

	/* T9 */
	lg_mdelay(s->backlight_off_ms);
}

void lg_pps_vlv_port_enable(struct lg_display *d, struct lg_output *o, int pipe)
{
	struct pps_state *s;

	if (!is_vlv_chv(d))
		return;
	s = pps_st(d, o);

	if (s->vlv_pps_pipe >= 0 && s->vlv_pps_pipe != pipe) {
		/* Another sequencer drove this port before: VDD off there
		 * while it is still ours. */
		vlv_detach_power_sequencer(d, o);
	}

	/* the pipe's sequencer may belong to another port */
	vlv_steal_power_sequencer(d, pipe);

	s->vlv_active_pipe = pipe;

	if (!pps_is_edp(d, o))
		return;
	if (pipe != 0 && pipe != 1) {
		kprintf("[drm] i915: %s: pipe %c has no panel power sequencer\n", o->name,
			pipe_name(pipe));
		return;
	}

	/* now it is all ours */
	s->is_edp = 1;
	s->vlv_pps_pipe = pipe;
	i915_dbg("[drm] i915: initializing PPS %c for %s\n", pipe_name(pipe), o->name);
	pps_init_delays(d, o);
	pps_init_registers(d, o, 1);
}

void lg_pps_vlv_port_disable(struct lg_display *d, struct lg_output *o)
{
	if (!is_vlv_chv(d))
		return;
	pps_st(d, o)->vlv_active_pipe = -1;
}

/* ===================================================================== */
/* ---- the backlight PWM ---------------------------------------------- */
/* ===================================================================== */

/* gen2/3 combination mode: the duty cycle is multiplied by this byte of
 * the device's PCI configuration space */
#define PNL_LBPC 0xf4

enum bl_kind {
	BL_NONE = 0,
	BL_I9XX, /* gen2/3 (Pineview included) */
	BL_I965, /* gen4 */
	BL_PCH, /* Ironlake to Ivy Bridge */
	BL_VLV, /* Valleyview/Cherryview, per pipe */
};

/* One panel per device: one backlight. */
static struct {
	int kind;
	int present;
	int enabled;
	int pipe; /* VLV/CHV: the controller in use */
	int combination_mode;
	int active_low;
	int pwm_enabled; /* found enabled at setup */
	uint32_t min, max, level; /* PWM units */
} g_bl;

/* `v' in [smin..smax] scaled onto [tmin..tmax], rounded. */
static uint32_t bl_scale(uint32_t v, uint32_t smin, uint32_t smax, uint32_t tmin,
			 uint32_t tmax)
{
	uint64_t t;

	if (smin >= smax || tmin > tmax)
		return tmin;
	if (v < smin)
		v = smin;
	if (v > smax)
		v = smax;
	t = (uint64_t)(v - smin) * (tmax - tmin);
	t = (t + (smax - smin) / 2) / (smax - smin);
	return (uint32_t)t + tmin;
}

static uint8_t bl_lbpc_read(struct lg_display *d)
{
	if (!d->i915 || !d->i915->pci)
		return 1;
	return pci_cfg_read8(d->i915->pci, PNL_LBPC);
}

static void bl_lbpc_write(struct lg_display *d, uint8_t v)
{
	if (d->i915 && d->i915->pci)
		pci_cfg_write8(d->i915->pci, PNL_LBPC, v);
}

static int bl_vlv_pipe_ok(int pipe)
{
	return pipe == 0 || pipe == 1;
}

/* The duty cycle as the controller has it. */
static uint32_t bl_hw_get(struct lg_display *d)
{
	uint32_t val;

	switch (g_bl.kind) {
	case BL_PCH:
		return lg_rd(d, BLC_PWM_CPU_CTL) & BACKLIGHT_DUTY_CYCLE_MASK;
	case BL_I9XX:
	case BL_I965:
		val = lg_rd(d, BLC_PWM_CTL) & BACKLIGHT_DUTY_CYCLE_MASK;
		if (d->ver < 4)
			val >>= 1;
		if (g_bl.combination_mode)
			val *= bl_lbpc_read(d);
		return val;
	case BL_VLV:
		if (!bl_vlv_pipe_ok(g_bl.pipe))
			return 0;
		return lg_rd(d, VLV_BLC_PWM_CTL(g_bl.pipe)) & BACKLIGHT_DUTY_CYCLE_MASK;
	default:
		return 0;
	}
}

static void bl_hw_set(struct lg_display *d, uint32_t level)
{
	uint32_t tmp, mask;

	i915_dbg("[drm] i915: backlight PWM = %u\n", level);
	switch (g_bl.kind) {
	case BL_PCH:
		tmp = lg_rd(d, BLC_PWM_CPU_CTL) & ~BACKLIGHT_DUTY_CYCLE_MASK;
		lg_wr(d, BLC_PWM_CPU_CTL, tmp | level);
		break;
	case BL_I9XX:
	case BL_I965:
		if (!g_bl.max)
			return;
		if (g_bl.combination_mode) {
			uint8_t lbpc = (uint8_t)((uint64_t)level * 0xfe / g_bl.max + 1);

			level /= lbpc;
			bl_lbpc_write(d, lbpc);
		}
		if (d->ver == 4) {
			mask = BACKLIGHT_DUTY_CYCLE_MASK;
		} else {
			level <<= 1;
			mask = BACKLIGHT_DUTY_CYCLE_MASK_PNV;
		}
		tmp = lg_rd(d, BLC_PWM_CTL) & ~mask;
		lg_wr(d, BLC_PWM_CTL, tmp | level);
		break;
	case BL_VLV:
		if (!bl_vlv_pipe_ok(g_bl.pipe))
			return;
		tmp = lg_rd(d, VLV_BLC_PWM_CTL(g_bl.pipe)) & ~BACKLIGHT_DUTY_CYCLE_MASK;
		lg_wr(d, VLV_BLC_PWM_CTL(g_bl.pipe), tmp | level);
		break;
	default:
		break;
	}
}

/* The PWM period for `hz', in the controller's units. */
static uint32_t bl_hz_to_pwm(struct lg_display *d, uint32_t hz)
{
	uint64_t clock; /* Hz */
	uint32_t mul;

	switch (g_bl.kind) {
	case BL_PCH:
		/* the period in PCH raw clocks times 128 */
		clock = (uint64_t)d->rawclk_khz * 1000;
		mul = 128;
		break;
	case BL_I9XX:
		/* a time base event is the core clock (Pineview: the raw
		 * clock) divided by 32 */
		clock = (uint64_t)(d->is_pnv ? d->rawclk_khz : d->cdclk_khz) * 1000;
		mul = 32;
		break;
	case BL_I965:
		/* the period in core clocks (G4X: raw clocks) times 128 */
		clock = (uint64_t)((d->is_g4x || d->is_gm45) ? d->rawclk_khz : d->cdclk_khz) *
			1000;
		mul = 128;
		break;
	case BL_VLV:
		/* the 200 MHz raw clock times 128, or the S0ix clock (25 MHz,
		 * Cherryview 19.2 MHz) times 16 */
		if (!(lg_rd(d, CBR1_VLV) & CBR_PWM_CLOCK_MUX_SELECT)) {
			clock = d->is_chv ? 19200000ull : 25000000ull;
			mul = 16;
		} else {
			clock = (uint64_t)d->rawclk_khz * 1000;
			mul = 128;
		}
		break;
	default:
		return 0;
	}
	if (!hz)
		return 0;
	return (uint32_t)DIV_ROUND_CLOSEST(clock, (uint64_t)hz * mul);
}

static uint32_t bl_max_from_vbt(struct lg_display *d)
{
	uint32_t hz = d->vbt.bl_pwm_freq_hz;
	uint32_t pwm;

	if (hz)
		i915_dbg("[drm] i915: VBT backlight frequency %u Hz\n", hz);
	else
		hz = 200;
	pwm = bl_hz_to_pwm(d, hz);
	if (!pwm)
		i915_dbg("[drm] i915: backlight frequency conversion failed\n");
	return pwm;
}

/* The VBT's minimum (of 255), at most a quarter: some VBTs say 255,
 * which would make the minimum the maximum. */
static uint32_t bl_min_from_vbt(struct lg_display *d)
{
	uint32_t min = d->vbt.bl_min_brightness;

	if (min > 64) {
		i915_dbg("[drm] i915: clamping VBT min backlight %u/255 to 64/255\n", min);
		min = 64;
	}
	return bl_scale(min, 0, 255, 0, g_bl.max);
}

static int bl_setup_pch(struct lg_display *d)
{
	uint32_t pch_ctl1 = lg_rd(d, BLC_PWM_PCH_CTL1);
	uint32_t pch_ctl2 = lg_rd(d, BLC_PWM_PCH_CTL2);
	uint32_t cpu_ctl2 = lg_rd(d, BLC_PWM_CPU_CTL2);

	g_bl.active_low = !!(pch_ctl1 & BLM_PCH_POLARITY);
	g_bl.max = pch_ctl2 >> 16;
	if (!g_bl.max) {
		g_bl.max = bl_max_from_vbt(d);
		g_bl.active_low = !!d->vbt.bl_active_low;
	}
	if (!g_bl.max)
		return -ENODEV;
	g_bl.min = bl_min_from_vbt(d);
	g_bl.pwm_enabled = (cpu_ctl2 & BLM_PWM_ENABLE) && (pch_ctl1 & BLM_PCH_PWM_ENABLE);
	return 0;
}

static int bl_setup_i9xx(struct lg_display *d)
{
	uint32_t ctl = lg_rd(d, BLC_PWM_CTL);
	uint32_t val;

	if (d->ver == 2 || d->is_i915gm || d->is_i945gm)
		g_bl.combination_mode = !!(ctl & BLM_LEGACY_MODE);
	if (d->is_pnv)
		g_bl.active_low = !!(ctl & BLM_POLARITY_PNV);

	g_bl.max = ctl >> 17;
	if (!g_bl.max) {
		g_bl.max = bl_max_from_vbt(d) >> 1;
		if (d->is_pnv)
			g_bl.active_low = !!d->vbt.bl_active_low;
	}
	if (!g_bl.max)
		return -ENODEV;
	if (g_bl.combination_mode)
		g_bl.max *= 0xff;

	g_bl.min = bl_min_from_vbt(d);

	val = bl_hw_get(d);
	if (val < g_bl.min)
		val = g_bl.min;
	if (val > g_bl.max)
		val = g_bl.max;
	g_bl.pwm_enabled = val != 0;
	return 0;
}

static int bl_setup_i965(struct lg_display *d)
{
	uint32_t ctl2 = lg_rd(d, BLC_PWM_CTL2);
	uint32_t ctl = lg_rd(d, BLC_PWM_CTL);

	g_bl.combination_mode = !!(ctl2 & BLM_COMBINATION_MODE);
	g_bl.active_low = !!(ctl2 & BLM_POLARITY_I965);

	g_bl.max = ctl >> 16;
	if (!g_bl.max) {
		g_bl.max = bl_max_from_vbt(d);
		g_bl.active_low = !!d->vbt.bl_active_low;
	}
	if (!g_bl.max)
		return -ENODEV;
	if (g_bl.combination_mode)
		g_bl.max *= 0xff;

	g_bl.min = bl_min_from_vbt(d);
	g_bl.pwm_enabled = !!(ctl2 & BLM_PWM_ENABLE);
	return 0;
}

static int bl_setup_vlv(struct lg_display *d)
{
	uint32_t ctl2, ctl;

	if (!bl_vlv_pipe_ok(g_bl.pipe))
		return -ENODEV;

	ctl2 = lg_rd(d, VLV_BLC_PWM_CTL2(g_bl.pipe));
	g_bl.active_low = !!(ctl2 & BLM_POLARITY_I965);

	ctl = lg_rd(d, VLV_BLC_PWM_CTL(g_bl.pipe));
	g_bl.max = ctl >> 16;
	if (!g_bl.max) {
		g_bl.max = bl_max_from_vbt(d);
		g_bl.active_low = !!d->vbt.bl_active_low;
	}
	if (!g_bl.max)
		return -ENODEV;

	g_bl.min = bl_min_from_vbt(d);
	g_bl.pwm_enabled = !!(ctl2 & BLM_PWM_ENABLE);
	return 0;
}

int lg_backlight_setup(struct lg_display *d, struct lg_output *o, int pipe)
{
	int ret;

	mm_memset(&g_bl, 0, sizeof(g_bl));
	g_bl.pipe = -1;

	if (d->vbt.valid && !d->vbt.bl_present) {
		i915_dbg("[drm] i915: no backlight present per VBT\n");
		return 0;
	}

	if (!d->gmch)
		g_bl.kind = BL_PCH;
	else if (is_vlv_chv(d))
		g_bl.kind = BL_VLV;
	else if (d->ver == 4)
		g_bl.kind = BL_I965;
	else
		g_bl.kind = BL_I9XX;

	if (g_bl.kind == BL_VLV) {
		if (o && o->type == LG_OUTPUT_DSI) {
			/* a DSI panel's backlight is not on these PWMs */
			g_bl.kind = BL_NONE;
			return -ENODEV;
		}
		/* The pipe the panel is on, else its sequencer's, else A. */
		if (!bl_vlv_pipe_ok(pipe) && o) {
			pipe = vlv_active_pipe(d, o);
			if (!bl_vlv_pipe_ok(pipe))
				pipe = pps_st(d, o)->vlv_pps_pipe;
		}
		if (!bl_vlv_pipe_ok(pipe))
			pipe = 0;
	}
	g_bl.pipe = pipe;

	switch (g_bl.kind) {
	case BL_PCH:
		ret = bl_setup_pch(d);
		break;
	case BL_I965:
		ret = bl_setup_i965(d);
		break;
	case BL_VLV:
		ret = bl_setup_vlv(d);
		break;
	default:
		ret = bl_setup_i9xx(d);
		break;
	}
	if (ret) {
		kprintf("[drm] i915: %s: failed to set up the backlight\n", o ? o->name : "panel");
		g_bl.kind = BL_NONE;
		return ret;
	}

	g_bl.level = bl_hw_get(d);
	g_bl.enabled = g_bl.pwm_enabled;
	g_bl.present = 1;

	kprintf("[drm] i915: %s: backlight %s, brightness %u/%u (min %u)%s%s\n",
		o ? o->name : "panel", g_bl.enabled ? "enabled" : "disabled", g_bl.level,
		g_bl.max, g_bl.min, g_bl.combination_mode ? ", combination mode" : "",
		g_bl.active_low ? ", active low" : "");
	return 0;
}

static void bl_enable_pch(struct lg_display *d, int pipe, uint32_t level)
{
	uint32_t cpu_ctl2, pch_ctl1, pch_ctl2;

	cpu_ctl2 = lg_rd(d, BLC_PWM_CPU_CTL2);
	if (cpu_ctl2 & BLM_PWM_ENABLE) {
		i915_dbg("[drm] i915: CPU backlight already enabled\n");
		cpu_ctl2 &= ~BLM_PWM_ENABLE;
		lg_wr(d, BLC_PWM_CPU_CTL2, cpu_ctl2);
	}

	pch_ctl1 = lg_rd(d, BLC_PWM_PCH_CTL1);
	if (pch_ctl1 & BLM_PCH_PWM_ENABLE) {
		i915_dbg("[drm] i915: PCH backlight already enabled\n");
		pch_ctl1 &= ~BLM_PCH_PWM_ENABLE;
		lg_wr(d, BLC_PWM_PCH_CTL1, pch_ctl1);
	}

	cpu_ctl2 = BLM_PIPE(pipe);
	lg_wr(d, BLC_PWM_CPU_CTL2, cpu_ctl2);
	lg_posting_read(d, BLC_PWM_CPU_CTL2);
	lg_wr(d, BLC_PWM_CPU_CTL2, cpu_ctl2 | BLM_PWM_ENABLE);

	/* the duty cycle does not stick before the enable above */
	bl_hw_set(d, level);

	pch_ctl2 = g_bl.max << 16;
	lg_wr(d, BLC_PWM_PCH_CTL2, pch_ctl2);

	pch_ctl1 = 0;
	if (g_bl.active_low)
		pch_ctl1 |= BLM_PCH_POLARITY;
	lg_wr(d, BLC_PWM_PCH_CTL1, pch_ctl1);
	lg_posting_read(d, BLC_PWM_PCH_CTL1);
	lg_wr(d, BLC_PWM_PCH_CTL1, pch_ctl1 | BLM_PCH_PWM_ENABLE);
}

static void bl_enable_i9xx(struct lg_display *d, uint32_t level)
{
	uint32_t ctl, freq;

	ctl = lg_rd(d, BLC_PWM_CTL);
	if (ctl & BACKLIGHT_DUTY_CYCLE_MASK_PNV) {
		i915_dbg("[drm] i915: backlight already enabled\n");
		lg_wr(d, BLC_PWM_CTL, 0);
	}

	freq = g_bl.max;
	if (g_bl.combination_mode)
		freq /= 0xff;

	ctl = freq << 17;
	if (g_bl.combination_mode)
		ctl |= BLM_LEGACY_MODE;
	if (d->is_pnv && g_bl.active_low)
		ctl |= BLM_POLARITY_PNV;

	lg_wr(d, BLC_PWM_CTL, ctl);
	lg_posting_read(d, BLC_PWM_CTL);

	bl_hw_set(d, level);

	/* Some 855GM boards need the histogram on before the backlight
	 * comes on; the 855GM is the only gen2 part with a backlight, so
	 * gen2 is the test. */
	if (d->ver == 2)
		lg_wr(d, BLC_HIST_CTL, BLM_HISTOGRAM_ENABLE);
}

static void bl_enable_i965(struct lg_display *d, int pipe, uint32_t level)
{
	uint32_t ctl, ctl2, freq;

	ctl2 = lg_rd(d, BLC_PWM_CTL2);
	if (ctl2 & BLM_PWM_ENABLE) {
		i915_dbg("[drm] i915: backlight already enabled\n");
		ctl2 &= ~BLM_PWM_ENABLE;
		lg_wr(d, BLC_PWM_CTL2, ctl2);
	}

	freq = g_bl.max;
	if (g_bl.combination_mode)
		freq /= 0xff;

	ctl = freq << 16;
	lg_wr(d, BLC_PWM_CTL, ctl);

	ctl2 = BLM_PIPE(pipe);
	if (g_bl.combination_mode)
		ctl2 |= BLM_COMBINATION_MODE;
	if (g_bl.active_low)
		ctl2 |= BLM_POLARITY_I965;
	lg_wr(d, BLC_PWM_CTL2, ctl2);
	lg_posting_read(d, BLC_PWM_CTL2);
	lg_wr(d, BLC_PWM_CTL2, ctl2 | BLM_PWM_ENABLE);

	bl_hw_set(d, level);
}

static void bl_enable_vlv(struct lg_display *d, int pipe, uint32_t level)
{
	uint32_t ctl, ctl2;

	if (!bl_vlv_pipe_ok(pipe)) {
		kprintf("[drm] i915: no backlight controller on pipe %c\n", pipe_name(pipe));
		return;
	}

	ctl2 = lg_rd(d, VLV_BLC_PWM_CTL2(pipe));
	if (ctl2 & BLM_PWM_ENABLE) {
		i915_dbg("[drm] i915: backlight already enabled\n");
		ctl2 &= ~BLM_PWM_ENABLE;
		lg_wr(d, VLV_BLC_PWM_CTL2(pipe), ctl2);
	}

	ctl = g_bl.max << 16;
	lg_wr(d, VLV_BLC_PWM_CTL(pipe), ctl);

	bl_hw_set(d, level);

	ctl2 = 0;
	if (g_bl.active_low)
		ctl2 |= BLM_POLARITY_I965;
	lg_wr(d, VLV_BLC_PWM_CTL2(pipe), ctl2);
	lg_posting_read(d, VLV_BLC_PWM_CTL2(pipe));
	lg_wr(d, VLV_BLC_PWM_CTL2(pipe), ctl2 | BLM_PWM_ENABLE);
}

void lg_backlight_enable(struct lg_display *d, struct lg_output *o,
			 const struct lg_config *cfg)
{
	int pipe;

	(void)o;
	if (!g_bl.present)
		return;

	pipe = cfg ? cfg->pipe : g_bl.pipe;
	i915_dbg("[drm] i915: backlight on, pipe %c\n", pipe_name(pipe));

	if (g_bl.level < g_bl.min)
		g_bl.level = g_bl.min;

	switch (g_bl.kind) {
	case BL_PCH:
		bl_enable_pch(d, pipe, g_bl.level);
		break;
	case BL_I9XX:
		bl_enable_i9xx(d, g_bl.level);
		break;
	case BL_I965:
		bl_enable_i965(d, pipe, g_bl.level);
		break;
	case BL_VLV:
		g_bl.pipe = pipe;
		bl_enable_vlv(d, pipe, g_bl.level);
		break;
	default:
		return;
	}
	g_bl.pipe = pipe;
	g_bl.enabled = 1;
}

void lg_backlight_disable(struct lg_display *d, struct lg_output *o)
{
	(void)o;
	if (!g_bl.present)
		return;

	g_bl.enabled = 0;
	bl_hw_set(d, 0);

	switch (g_bl.kind) {
	case BL_PCH:
		lg_rmw(d, BLC_PWM_CPU_CTL2, BLM_PWM_ENABLE, 0);
		lg_rmw(d, BLC_PWM_PCH_CTL1, BLM_PCH_PWM_ENABLE, 0);
		break;
	case BL_I965:
		lg_rmw(d, BLC_PWM_CTL2, BLM_PWM_ENABLE, 0);
		break;
	case BL_VLV:
		if (bl_vlv_pipe_ok(g_bl.pipe))
			lg_rmw(d, VLV_BLC_PWM_CTL2(g_bl.pipe), BLM_PWM_ENABLE, 0);
		break;
	default:
		/* gen2/3: a zero duty cycle is off */
		break;
	}
}

/* `level' in [0..lg_backlight_max()], kept at least at the minimum. */
int lg_backlight_set(struct lg_display *d, uint32_t level)
{
	if (!g_bl.present)
		return -ENODEV;

	if (level > g_bl.max)
		level = g_bl.max;
	if (level < g_bl.min)
		level = g_bl.min;
	g_bl.level = level;
	i915_dbg("[drm] i915: backlight level = %u\n", level);
	if (g_bl.enabled)
		bl_hw_set(d, level);
	return 0;
}

uint32_t lg_backlight_get(struct lg_display *d)
{
	if (!g_bl.present)
		return 0;
	if (g_bl.enabled)
		return bl_hw_get(d);
	return g_bl.level;
}

uint32_t lg_backlight_max(struct lg_display *d)
{
	(void)d;
	return g_bl.present ? g_bl.max : 0;
}

/* ===================================================================== */
/* ---- panel fitters --------------------------------------------------- */
/* ===================================================================== */

/* GMCH: which parts have the fitter at all. */
static int gmch_has_pfit(const struct lg_display *d)
{
	if (d->is_i830)
		return 0;
	return d->ver >= 4 || d->is_pnv || d->mobile;
}

/* Centre `width' pixels in the panel's active area: a border on each
 * side, hblank and hsync keeping their widths. */
static void centre_horizontally(struct lg_timings *t, uint32_t width)
{
	uint32_t border, sync_pos, blank_width, sync_width;

	sync_width = (uint32_t)t->hsync_end - t->hsync_start;
	blank_width = (uint32_t)t->hblank_end - t->hblank_start;
	sync_pos = (blank_width - sync_width + 1) / 2;

	border = (t->hdisplay - width + 1) / 2;
	border += border & 1; /* an even border */

	t->hdisplay = (uint16_t)width;
	t->hblank_start = (uint16_t)(width + border);
	t->hblank_end = (uint16_t)(t->hblank_start + blank_width);
	t->hsync_start = (uint16_t)(t->hblank_start + sync_pos);
	t->hsync_end = (uint16_t)(t->hsync_start + sync_width);
}

static void centre_vertically(struct lg_timings *t, uint32_t height)
{
	uint32_t border, sync_pos, blank_width, sync_width;

	sync_width = (uint32_t)t->vsync_end - t->vsync_start;
	blank_width = (uint32_t)t->vblank_end - t->vblank_start;
	sync_pos = (blank_width - sync_width + 1) / 2;

	border = (t->vdisplay - height + 1) / 2;

	t->vdisplay = (uint16_t)height;
	t->vblank_start = (uint16_t)(height + border);
	t->vblank_end = (uint16_t)(t->vblank_start + blank_width);
	t->vsync_start = (uint16_t)(t->vblank_start + sync_pos);
	t->vsync_end = (uint16_t)(t->vsync_start + sync_width);
}

/* Pre-965 programmed ratio: source/target as a 12-bit fraction. */
static uint32_t panel_fitter_scaling(uint32_t source, uint32_t target)
{
	const uint32_t factor = 1u << 12;
	uint32_t ratio = source * factor / target;

	return (factor * ratio + factor / 2) / factor;
}

/* 965 on: the hardware keeps the aspect itself. */
static void i965_scale_aspect(struct lg_config *cfg, uint32_t *ctl)
{
	const struct lg_timings *t = &cfg->t;
	uint32_t scaled_width = (uint32_t)t->hdisplay * cfg->src_h;
	uint32_t scaled_height = cfg->src_w * (uint32_t)t->vdisplay;

	if (scaled_width > scaled_height)
		*ctl |= PFIT_ENABLE | PFIT_SCALING_PILLAR;
	else if (scaled_width < scaled_height)
		*ctl |= PFIT_ENABLE | PFIT_SCALING_LETTER;
	else if (t->hdisplay != cfg->src_w)
		*ctl |= PFIT_ENABLE | PFIT_SCALING_AUTO;
}

/* Before 965 the ratio is programmed by hand and the picture centred in
 * the other direction with borders in the pipe's timing. */
static void i9xx_scale_aspect(struct lg_config *cfg, uint32_t *ctl, uint32_t *ratios,
			      uint32_t *border)
{
	struct lg_timings *t = &cfg->t;
	uint32_t src_w = cfg->src_w, src_h = cfg->src_h;
	uint32_t scaled_width = (uint32_t)t->hdisplay * src_h;
	uint32_t scaled_height = src_w * (uint32_t)t->vdisplay;
	uint32_t bits;

	if (scaled_width > scaled_height) { /* pillar */
		centre_horizontally(t, scaled_height / src_h);
		*border = LVDS_BORDER_ENABLE;
		if (src_h != t->vdisplay) {
			bits = panel_fitter_scaling(src_h, t->vdisplay);
			*ratios |= PFIT_HORIZ_SCALE(bits) | PFIT_VERT_SCALE(bits);
			*ctl |= PFIT_ENABLE | PFIT_VERT_INTERP_BILINEAR |
				PFIT_HORIZ_INTERP_BILINEAR;
		}
	} else if (scaled_width < scaled_height) { /* letter */
		centre_vertically(t, scaled_width / src_w);
		*border = LVDS_BORDER_ENABLE;
		if (src_w != t->hdisplay) {
			bits = panel_fitter_scaling(src_w, t->hdisplay);
			*ratios |= PFIT_HORIZ_SCALE(bits) | PFIT_VERT_SCALE(bits);
			*ctl |= PFIT_ENABLE | PFIT_VERT_INTERP_BILINEAR |
				PFIT_HORIZ_INTERP_BILINEAR;
		}
	} else {
		/* same aspect: the hardware scales both directions */
		*ctl |= PFIT_ENABLE | PFIT_VERT_AUTO_SCALE | PFIT_HORIZ_AUTO_SCALE |
			PFIT_VERT_INTERP_BILINEAR | PFIT_HORIZ_INTERP_BILINEAR;
	}
}

static int gmch_panel_fitting(struct lg_display *d, struct lg_config *cfg)
{
	struct lg_timings *t = &cfg->t;
	uint32_t ctl = 0, ratios = 0, border = 0;
	uint32_t src_w = cfg->src_w, src_h = cfg->src_h;
	uint32_t min;

	/* native modes need no fitting */
	if (t->hdisplay == src_w && t->vdisplay == src_h)
		goto out;

	if (!gmch_has_pfit(d)) {
		i915_dbg("[drm] i915: pipe %c: no panel fitter for %ux%u on %ux%u\n",
			 pipe_name(cfg->pipe), src_w, src_h, t->hdisplay, t->vdisplay);
		return -EINVAL;
	}
	/* the fitter only scales up */
	if (t->hdisplay < src_w || t->vdisplay < src_h) {
		i915_dbg("[drm] i915: pipe %c: pfit downscaling (%ux%u->%ux%u) not supported\n",
			 pipe_name(cfg->pipe), src_w, src_h, t->hdisplay, t->vdisplay);
		return -EINVAL;
	}
	/* before gen4 the fitter sits on pipe B */
	if (d->ver < 4 && cfg->pipe != 1) {
		i915_dbg("[drm] i915: pipe %c: the panel fitter is on pipe B only\n",
			 pipe_name(cfg->pipe));
		return -EINVAL;
	}
	if (!src_w || !src_h)
		return -EINVAL;

	/* aspect-preserving; same aspect means full screen */
	if (d->ver >= 4)
		i965_scale_aspect(cfg, &ctl);
	else
		i9xx_scale_aspect(cfg, &ctl, &ratios, &border);

	/* 965 on: fuzzy filtering, and the pipe the fitter serves */
	if (d->ver >= 4)
		ctl |= PFIT_PIPE(cfg->pipe) | PFIT_FILTER_FUZZY;

out:
	if (!(ctl & PFIT_ENABLE)) {
		ctl = 0;
		ratios = 0;
	}
	/* pre-965: the fitter dithers 18 bpp panels */
	if (d->ver < 4 && cfg->pipe_bpp == 18 && gmch_has_pfit(d))
		ctl |= PFIT_PANEL_8TO6_DITHER_ENABLE;

	cfg->gmch_pfit.control = ctl;
	cfg->gmch_pfit.pgm_ratios = ratios;
	cfg->gmch_pfit.lvds_border_bits = border;
	cfg->t_set = 1;

	if (!(ctl & PFIT_ENABLE))
		return 0;

	/* one fitter for all pipes */
	for (int p = 0; p < d->num_pipes && p < LG_MAX_PIPES; p++) {
		if (p == cfg->pipe || !d->pipes[p].active)
			continue;
		if (d->pipes[p].cfg.gmch_pfit.control & PFIT_ENABLE) {
			i915_dbg("[drm] i915: pipe %c: the panel fitter is in use by pipe %c\n",
				 pipe_name(cfg->pipe), pipe_name(p));
			return -EINVAL;
		}
	}

	min = d->ver >= 4 ? 3 : 2;
	if (t->hdisplay < min || t->vdisplay < min) {
		i915_dbg("[drm] i915: pipe %c: active %ux%u below the pfit minimum %u\n",
			 pipe_name(cfg->pipe), t->hdisplay, t->vdisplay, min);
		return -EINVAL;
	}
	return 0;
}

static int pch_panel_fitting(struct lg_display *d, struct lg_config *cfg)
{
	const struct lg_timings *t = &cfg->t;
	uint32_t hd = t->hdisplay, vd = t->vdisplay;
	uint32_t src_w = cfg->src_w, src_h = cfg->src_h;
	uint32_t x, y, width, height, max_src_w;

	cfg->pch_pfit.enabled = 0;
	cfg->pch_pfit.pos = 0;
	cfg->pch_pfit.size = 0;
	cfg->t_set = 1;

	/* native modes need no fitting */
	if (hd == src_w && vd == src_h)
		return 0;
	if (!src_w || !src_h)
		return -EINVAL;

	/* aspect-preserving; same aspect means full screen */
	{
		uint32_t scaled_width = hd * src_h;
		uint32_t scaled_height = src_w * vd;

		if (scaled_width > scaled_height) { /* pillar */
			width = scaled_height / src_h;
			if (width & 1)
				width++;
			x = (hd - width + 1) / 2;
			y = 0;
			height = vd;
		} else if (scaled_width < scaled_height) { /* letter */
			height = scaled_width / src_w;
			if (height & 1)
				height++;
			y = (vd - height + 1) / 2;
			x = 0;
			width = hd;
		} else {
			x = y = 0;
			width = hd;
			height = vd;
		}
	}

	if (!width || !height)
		return -EINVAL;

	cfg->pch_pfit.pos = (x << 16) | y;
	cfg->pch_pfit.size = (width << 16) | height;
	cfg->pch_pfit.enabled = 1;

	if ((t->flags & DRM_MODE_FLAG_INTERLACE) && ((y & 1) || (height & 1))) {
		i915_dbg("[drm] i915: pipe %c: pfit window misaligned for interlace\n",
			 pipe_name(cfg->pipe));
		return -EINVAL;
	}
	/* the scaled picture must fill the pipe's active area: active =
	 * 2 * window position + window size */
	if (hd != 2 * x + width || vd != 2 * y + height) {
		i915_dbg("[drm] i915: pipe %c: pfit window %u,%u %ux%u not centred\n",
			 pipe_name(cfg->pipe), x, y, width, height);
		return -EINVAL;
	}
	/* an X position of 1 is not allowed */
	if (x == 1) {
		i915_dbg("[drm] i915: pipe %c: pfit window badly positioned\n",
			 pipe_name(cfg->pipe));
		return -EINVAL;
	}
	/* Ivy Bridge: only pipe A's fitter has the 7x5 filter that takes
	 * 4096 pixel lines; the others are 3x3 and take 2048. */
	max_src_w = (d->ver >= 7 && cfg->pipe != 0) ? 2048 : 4096;
	if (src_w > max_src_w || src_h > 4096) {
		i915_dbg("[drm] i915: pipe %c: source %ux%u exceeds the pfit maximum %ux4096\n",
			 pipe_name(cfg->pipe), src_w, src_h, max_src_w);
		return -EINVAL;
	}
	/* down by at most 1.125 */
	if (((uint64_t)src_w << 16) / width > 0x12000 ||
	    ((uint64_t)src_h << 16) / height > 0x12000) {
		i915_dbg("[drm] i915: pipe %c: pfit downscaling %ux%u->%ux%u too far\n",
			 pipe_name(cfg->pipe), src_w, src_h, width, height);
		return -EINVAL;
	}
	if (vd < 7) {
		i915_dbg("[drm] i915: pipe %c: vertical active %u below the pfit minimum 7\n",
			 pipe_name(cfg->pipe), vd);
		return -EINVAL;
	}
	return 0;
}

int lg_pfit_compute(struct lg_display *d, struct lg_output *o, struct lg_config *cfg)
{
	(void)o;
	if (d->gmch)
		return gmch_panel_fitting(d, cfg);
	return pch_panel_fitting(d, cfg);
}

void lg_pfit_enable(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;

	if (d->gmch) {
		if (!cfg->gmch_pfit.control)
			return;
		/* The fitter is changed only while the pipe is off.  It is
		 * shared: whoever had it lost it at compute time. */
		lg_wr(d, PFIT_PGM_RATIOS, cfg->gmch_pfit.pgm_ratios);
		lg_wr(d, PFIT_CONTROL, cfg->gmch_pfit.control);
		/* border colour where the picture does not fill the panel:
		 * black */
		lg_wr(d, BCLRPAT(d, pipe), 0);
		return;
	}

	if (!cfg->pch_pfit.enabled)
		return;
	/* The filter coefficients the BIOS left are broken on some machines
	 * (the X201): use the hard-coded medium 3x3 ones.  Ivy Bridge also
	 * wants the pipe the fitter serves. */
	if (d->is_ivb)
		lg_wr(d, PF_CTL(pipe), PF_ENABLE | PF_FILTER_MED_3x3 | PF_PIPE_SEL_IVB(pipe));
	else
		lg_wr(d, PF_CTL(pipe), PF_ENABLE | PF_FILTER_MED_3x3);
	lg_wr(d, PF_WIN_POS(pipe), cfg->pch_pfit.pos);
	lg_wr(d, PF_WIN_SZ(pipe), cfg->pch_pfit.size);
}

void lg_pfit_disable(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;

	if (d->gmch) {
		uint32_t cur;

		if (!cfg->gmch_pfit.control)
			return;
		cur = lg_rd(d, PFIT_CONTROL);
		/* 965 on: leave it alone when it serves another pipe now */
		if (d->ver >= 4 && (cur & PFIT_ENABLE) &&
		    (cur & PFIT_PIPE_MASK) != PFIT_PIPE(pipe))
			return;
		i915_dbg("[drm] i915: disabling pfit, current %08x\n", cur);
		lg_wr(d, PFIT_CONTROL, 0);
		return;
	}

	if (!cfg->pch_pfit.enabled)
		return;
	lg_wr(d, PF_CTL(pipe), 0);
	lg_wr(d, PF_WIN_POS(pipe), 0);
	lg_wr(d, PF_WIN_SZ(pipe), 0);
}
