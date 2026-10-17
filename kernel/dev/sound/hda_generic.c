// LikeOS Intel High Definition Audio - the generic codec parser, path
// programming, routing with headphone auto-mute, the mixer controls, and
// stream binding of the analog function.
//
// The parser works only from what the codec says about itself: the pin
// default configuration words (what is wired where on this board, as the
// BIOS or a vendor fixup describes it) and the widget connection lists.
//
//  1. Pins are classified into speakers, headphones, line outs, S/PDIF,
//     microphones and line inputs.  Unconnected pins are skipped.
//  2. For every output pin a path back to a DAC is searched through mixers
//     and selectors (iterative deepening, so the shortest route wins),
//     preferring a DAC no other output uses yet.  A headphone gets its own
//     DAC whenever the codec has one to spare; otherwise it shares the
//     primary DAC.  A selector already set one way by another path is never
//     switched to another input.
//  3. Inputs are searched from the ADCs back to the pins; the ADC that can
//     reach the most inputs becomes the primary capture converter.
//  4. On each path one amp is chosen for volume (the output amp with steps
//     nearest the DAC) and one for muting (an amp with a mute switch nearest
//     the pin that belongs to this path alone), so turning an output off
//     never silences another output sharing the DAC.
//
// Every converter of every output is bound to the playback stream, so the
// output set can change at any time (a headphone plugged in) without
// touching the stream: routing only switches pin controls and mutes.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda_codec_priv.h"
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>

/* Pin classes */
#define K_SPK 1
#define K_HP 2
#define K_LINE 3
#define K_SPDIF 4
#define K_MIC 5
#define K_LINEIN 6
#define K_CD 7
#define K_SPDIF_IN 8

/* Search budget (node expansions per search depth). */
#define GP_BUDGET 6000

struct gp_pin {
	uint8_t nid;
	uint8_t kind;
	uint8_t dock;
	uint8_t internal;
	uint8_t fixed;
	uint8_t jack;
	uint8_t assoc, seq;
	uint8_t loc;
	uint8_t ok; /* a path was found */
	struct hda_path path;
	uint8_t conv;
};

struct gp_ctx {
	struct hda_codec *c;
	/* how many parsed output paths run through each nid */
	uint8_t out_users[256];
	/* index+1 a selector is fixed to by an output path, 0 = free */
	uint8_t sel_idx[256];
	uint8_t dac_users[256];
	uint8_t in_users[256];

	/* the running search */
	uint8_t stack[HDA_MAX_PATH];
	uint8_t sidx[HDA_MAX_PATH];
	int maxdepth;
	int budget;
	int best_cost;
	int best_n;
	uint8_t best_stack[HDA_MAX_PATH];
	uint8_t best_sidx[HDA_MAX_PATH];
	/* what the search accepts */
	int want_unused; /* only DACs nobody uses */
	int want_digital; /* -1 analog only, 1 digital preferred */
	uint8_t primary_dac;
	uint8_t target; /* input search: the pin to reach */

	struct gp_pin outs[HDA_MAX_OUTPUTS];
	int nouts;
	struct gp_pin ins[HDA_MAX_INPUTS];
	int nins;
};

/* ======================================================================== */
/* small helpers                                                            */
/* ======================================================================== */

static inline uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

int32_t hda_amp_step_db(uint32_t caps, uint32_t step)
{
	int32_t off = (int32_t)hda_amp_offset(caps);
	int32_t sz = (int32_t)hda_amp_stepsize(caps) + 1;

	/* step size is in 0.25 dB units: (size + 1) * 25 hundredths */
	return ((int32_t)step - off) * sz * 25;
}

uint32_t hda_amp_pct_to_step(uint32_t caps, uint32_t pct)
{
	uint32_t n = hda_amp_nsteps(caps);

	if (pct > 100)
		pct = 100;
	if (!n)
		return 0;
	/* Equal steps in dB, so a linear map of percent to steps is linear
	 * in dB as well. */
	return (pct * n + 50) / 100;
}

/* The percentage that lands on 0 dB (or 100 if 0 dB is out of range). */
static uint32_t hda_amp_0db_pct(uint32_t caps)
{
	uint32_t n = hda_amp_nsteps(caps), off = hda_amp_offset(caps);

	if (!n || off >= n)
		return 100;
	return (off * 100 + n / 2) / n;
}

/* The gain value that is 0 dB, clamped into the amp's range. */
static uint32_t hda_amp_0db_step(uint32_t caps)
{
	uint32_t n = hda_amp_nsteps(caps), off = hda_amp_offset(caps);

	return off > n ? n : off;
}

static int amp_has_steps(struct hda_codec *c, uint8_t nid, int dir)
{
	return hda_amp_nsteps(hda_amp_caps(c, nid, dir)) > 0;
}

static int amp_has_mute(struct hda_codec *c, uint8_t nid, int dir)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w)
		return 0;
	if (!(w->caps & (dir == HDA_OUTPUT ? AC_WCAP_OUT_AMP : AC_WCAP_IN_AMP)))
		return 0;
	return (hda_amp_caps(c, nid, dir) & AC_AMPCAP_MUTE) != 0;
}

static int amp_present(struct hda_codec *c, uint8_t nid, int dir)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w)
		return 0;
	return (w->caps &
		(dir == HDA_OUTPUT ? AC_WCAP_OUT_AMP : AC_WCAP_IN_AMP)) != 0;
}

static uint32_t out_type_of(int kind)
{
	switch (kind) {
	case K_SPK:
		return AUDIO_OUT_SPEAKER;
	case K_HP:
		return AUDIO_OUT_HEADPHONE;
	case K_LINE:
		return AUDIO_OUT_LINE;
	case K_SPDIF:
		return AUDIO_OUT_SPDIF;
	default:
		return AUDIO_OUT_LINE;
	}
}

/* The per-type mixer control of an output, or -1. */
static int out_ctl_of(const struct hda_out *o)
{
	switch (o->type) {
	case AUDIO_OUT_SPEAKER:
		return AUDIO_CTL_SPEAKER;
	case AUDIO_OUT_HEADPHONE:
	case AUDIO_OUT_HEADSET:
		return AUDIO_CTL_HEADPHONE;
	case AUDIO_OUT_LINE:
		return AUDIO_CTL_LINE;
	default:
		return -1;
	}
}

static int is_analog_out(const struct hda_out *o)
{
	return o->type != AUDIO_OUT_SPDIF && o->type != AUDIO_OUT_HDMI &&
	       o->type != AUDIO_OUT_DP;
}

/* ======================================================================== */
/* pin classification                                                       */
/* ======================================================================== */

static int gp_classify(struct hda_codec *c, struct hda_widget *w,
		       struct gp_pin *pin)
{
	uint32_t cfg = w->pin_cfg;
	uint32_t port = hda_cfg_port(cfg);
	uint32_t dev = hda_cfg_device(cfg);
	uint32_t loc = hda_cfg_location(cfg);
	uint32_t chassis = loc & 0x30;
	int digital = (w->caps & AC_WCAP_DIGITAL) != 0;
	int kind = 0;

	if (port == AC_JACK_PORT_NONE)
		return 0;
	/* HDMI/DP pins belong to the display codec code. */
	if (w->pincap & (AC_PINCAP_HDMI | AC_PINCAP_DP))
		return 0;

	switch (dev) {
	case AC_JACK_LINE_OUT:
		if (!(w->pincap & AC_PINCAP_OUT))
			return 0;
		if (digital)
			kind = K_SPDIF;
		/* Many laptop BIOSes describe the built-in speakers as a
		 * fixed internal line out. */
		else if (port == AC_JACK_PORT_FIXED &&
			 chassis == AC_JACK_LOC_INTERNAL)
			kind = K_SPK;
		else
			kind = K_LINE;
		break;
	case AC_JACK_SPEAKER:
		if (!(w->pincap & AC_PINCAP_OUT))
			return 0;
		kind = digital ? K_SPDIF : K_SPK;
		break;
	case AC_JACK_HP_OUT:
		if (!(w->pincap & AC_PINCAP_OUT))
			return 0;
		kind = digital ? K_SPDIF : K_HP;
		break;
	case AC_JACK_SPDIF_OUT:
	case AC_JACK_DIG_OTHER_OUT:
		if (!(w->pincap & AC_PINCAP_OUT))
			return 0;
		kind = K_SPDIF;
		break;
	case AC_JACK_MIC_IN:
		if (!(w->pincap & AC_PINCAP_IN))
			return 0;
		kind = K_MIC;
		break;
	case AC_JACK_LINE_IN:
	case AC_JACK_AUX:
		if (!(w->pincap & AC_PINCAP_IN))
			return 0;
		kind = K_LINEIN;
		break;
	case AC_JACK_CD:
		if (!(w->pincap & AC_PINCAP_IN))
			return 0;
		kind = K_CD;
		break;
	case AC_JACK_SPDIF_IN:
	case AC_JACK_DIG_OTHER_IN:
		if (!(w->pincap & AC_PINCAP_IN))
			return 0;
		kind = K_SPDIF_IN;
		break;
	default:
		return 0;
	}

	pin->nid = w->nid;
	pin->kind = (uint8_t)kind;
	pin->dock = chassis == AC_JACK_LOC_SEPARATE;
	pin->fixed = port == AC_JACK_PORT_FIXED;
	pin->internal = pin->fixed || chassis == AC_JACK_LOC_INTERNAL;
	pin->jack = (uint8_t)hda_pin_has_presence(c, w->nid);
	pin->assoc = (uint8_t)hda_cfg_assoc(cfg);
	pin->seq = (uint8_t)hda_cfg_seq(cfg);
	pin->loc = (uint8_t)loc;
	return 1;
}

/* Sort key: docks after the built-in ones, then association, sequence,
 * node id.  Association 0xf means "no association" and goes last. */
static uint32_t gp_pin_key(const struct gp_pin *p)
{
	uint32_t assoc = p->assoc ? p->assoc : 0xf;

	return ((uint32_t)p->dock << 24) | (assoc << 16) |
	       ((uint32_t)p->seq << 8) | p->nid;
}

static void gp_sort(struct gp_pin *a, int n)
{
	for (int i = 1; i < n; i++) {
		struct gp_pin t = a[i];
		int j = i - 1;

		while (j >= 0 && gp_pin_key(&a[j]) > gp_pin_key(&t)) {
			a[j + 1] = a[j];
			j--;
		}
		a[j + 1] = t;
	}
}

/* Collect output pins in the list order: the primary group (speakers on a
 * laptop, line outs on a desktop), headphones, the remaining analog groups,
 * S/PDIF last. */
static void gp_collect(struct gp_ctx *x)
{
	struct hda_codec *c = x->c;
	struct gp_pin spk[HDA_MAX_OUTPUTS], hp[HDA_MAX_OUTPUTS],
		line[HDA_MAX_OUTPUTS], dig[HDA_MAX_OUTPUTS];
	int nspk = 0, nhp = 0, nline = 0, ndig = 0;

	x->nouts = 0;
	x->nins = 0;
	for (uint32_t i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];
		struct gp_pin p;

		if (w->type != AC_WID_PIN)
			continue;
		kmemset(&p, 0, sizeof(p));
		if (!gp_classify(c, w, &p))
			continue;
		switch (p.kind) {
		case K_SPK:
			if (nspk < HDA_MAX_OUTPUTS)
				spk[nspk++] = p;
			break;
		case K_HP:
			if (nhp < HDA_MAX_OUTPUTS)
				hp[nhp++] = p;
			break;
		case K_LINE:
			if (nline < HDA_MAX_OUTPUTS)
				line[nline++] = p;
			break;
		case K_SPDIF:
			if (ndig < HDA_MAX_OUTPUTS)
				dig[ndig++] = p;
			break;
		case K_SPDIF_IN:
			/* digital inputs are not handled by the analog PCM */
			break;
		default:
			if (x->nins < HDA_MAX_INPUTS)
				x->ins[x->nins++] = p;
			break;
		}
	}
	gp_sort(spk, nspk);
	gp_sort(hp, nhp);
	gp_sort(line, nline);
	gp_sort(dig, ndig);

#define GP_ADD(arr, n)                                                       \
	do {                                                                 \
		for (int k_ = 0; k_ < (n) && x->nouts < HDA_MAX_OUTPUTS; k_++) \
			x->outs[x->nouts++] = (arr)[k_];                     \
	} while (0)
	if (nspk) {
		GP_ADD(spk, nspk);
		GP_ADD(hp, nhp);
		GP_ADD(line, nline);
	} else if (nline) {
		GP_ADD(line, nline);
		GP_ADD(hp, nhp);
	} else {
		GP_ADD(hp, nhp);
	}
	/* Only one S/PDIF output: the stream feeds a single digital
	 * converter. */
	if (ndig)
		GP_ADD(dig, 1);
#undef GP_ADD
}

/* ======================================================================== */
/* path search                                                              */
/* ======================================================================== */

static int gp_on_stack(struct gp_ctx *x, int d, uint8_t nid)
{
	for (int k = 0; k <= d; k++)
		if (x->stack[k] == nid)
			return 1;
	return 0;
}

static void gp_take(struct gp_ctx *x, int n, int cost)
{
	if (cost >= x->best_cost)
		return;
	x->best_cost = cost;
	x->best_n = n;
	for (int k = 0; k < n; k++) {
		x->best_stack[k] = x->stack[k];
		x->best_sidx[k] = x->sidx[k];
	}
}

static int gp_dac_ok(struct gp_ctx *x, uint8_t dac)
{
	struct hda_widget *w = hda_widget(x->c, dac);
	int dig;

	if (!w || w->type != AC_WID_AUD_OUT)
		return 0;
	dig = (w->caps & AC_WCAP_DIGITAL) != 0;
	if (x->want_digital < 0 && dig)
		return 0;
	if (x->want_unused && x->dac_users[dac])
		return 0;
	return 1;
}

/* Output search: stack[0] is the pin, stack[d] the node being expanded. */
static void gp_out_dfs(struct gp_ctx *x, int d)
{
	struct hda_codec *c = x->c;
	struct hda_widget *w = hda_widget(c, x->stack[d]);

	if (!w || x->budget-- <= 0)
		return;
	for (int i = 0; i < w->nconn; i++) {
		uint8_t nn = w->conn[i];
		struct hda_widget *nw = hda_widget(c, nn);

		if (!nw || gp_on_stack(x, d, nn))
			continue;
		/* A selector another path already set must keep its input. */
		if (d > 0 && w->type == AC_WID_AUD_SEL && x->sel_idx[w->nid] &&
		    x->sel_idx[w->nid] != i + 1)
			continue;
		x->sidx[d] = (uint8_t)i;
		x->stack[d + 1] = nn;
		if (nw->type == AC_WID_AUD_OUT) {
			int n = d + 2, cost;

			if (n > x->maxdepth || !gp_dac_ok(x, nn))
				continue;
			cost = n * 10;
			for (int k = 1; k < n - 1; k++)
				if (x->out_users[x->stack[k]])
					cost += 4;
			if (x->dac_users[nn])
				cost += 100;
			if (x->primary_dac && nn != x->primary_dac)
				cost += x->want_unused ? 0 : 50;
			if (x->want_digital > 0 && !(nw->caps & AC_WCAP_DIGITAL))
				cost += 200;
			gp_take(x, n, cost);
		} else if (nw->type == AC_WID_AUD_MIX ||
			   nw->type == AC_WID_AUD_SEL) {
			if (d + 3 > x->maxdepth)
				continue;
			gp_out_dfs(x, d + 1);
		}
	}
}

/* Turn the best stack (pin ... converter or converter ... pin) into a
 * path in signal order. */
static void gp_stack_to_path(struct gp_ctx *x, struct hda_path *p)
{
	int n = x->best_n;

	kmemset(p, 0, sizeof(*p));
	p->depth = (uint8_t)n;
	for (int j = 0; j < n; j++) {
		p->nid[j] = x->best_stack[n - 1 - j];
		p->idx[j] = j ? x->best_sidx[n - 1 - j] : 0;
	}
}

static int gp_search_out(struct gp_ctx *x, uint8_t pin, int unused,
			 int digital, struct hda_path *out)
{
	x->want_unused = unused;
	x->want_digital = digital;
	x->best_cost = 0x7fffffff;
	x->best_n = 0;
	x->stack[0] = pin;
	for (int depth = 2; depth <= HDA_MAX_PATH; depth++) {
		x->maxdepth = depth;
		x->budget = GP_BUDGET;
		gp_out_dfs(x, 0);
		if (x->best_n)
			break;
	}
	if (!x->best_n)
		return 0;
	gp_stack_to_path(x, out);
	return 1;
}

static void gp_claim_out_path(struct gp_ctx *x, const struct hda_path *p)
{
	for (int j = 0; j < p->depth; j++) {
		uint8_t nid = p->nid[j];
		struct hda_widget *w = hda_widget(x->c, nid);

		if (x->out_users[nid] < 255)
			x->out_users[nid]++;
		if (w && j > 0 && w->type == AC_WID_AUD_SEL)
			x->sel_idx[nid] = (uint8_t)(p->idx[j] + 1);
		if (w)
			w->used = 1;
	}
	if (p->depth && x->dac_users[p->nid[0]] < 255)
		x->dac_users[p->nid[0]]++;
}

/* Find a path for output `i' of the candidate list. */
static void gp_route_out(struct gp_ctx *x, int i)
{
	struct gp_pin *o = &x->outs[i];
	int dig = o->kind == K_SPDIF ? 1 : -1;
	struct hda_path p;

	if (o->ok)
		return;
	/* An unused converter first, then share (the primary preferred). */
	if (gp_search_out(x, o->nid, 1, dig, &p) ||
	    gp_search_out(x, o->nid, 0, dig, &p)) {
		o->path = p;
		o->conv = p.nid[0];
		o->ok = 1;
		gp_claim_out_path(x, &p);
		if (!x->primary_dac && dig < 0)
			x->primary_dac = o->conv;
	} else {
		snd_info(HDA_LOG,
			 "codec#%d: no route from pin 0x%02x to a converter\n",
			 x->c->addr, o->nid);
	}
}

/* Input search: stack[0] is the ADC, the target is x->target. */
static void gp_in_dfs(struct gp_ctx *x, int d)
{
	struct hda_codec *c = x->c;
	struct hda_widget *w = hda_widget(c, x->stack[d]);

	if (!w || x->budget-- <= 0)
		return;
	for (int i = 0; i < w->nconn; i++) {
		uint8_t nn = w->conn[i];
		struct hda_widget *nw = hda_widget(c, nn);

		if (!nw || gp_on_stack(x, d, nn))
			continue;
		x->sidx[d] = (uint8_t)i;
		x->stack[d + 1] = nn;
		if (nn == x->target) {
			int n = d + 2, cost = n * 10;

			if (n > x->maxdepth)
				continue;
			for (int k = 1; k < n - 1; k++)
				if (x->out_users[x->stack[k]])
					cost += 30;
			gp_take(x, n, cost);
		} else if (nw->type == AC_WID_AUD_MIX ||
			   nw->type == AC_WID_AUD_SEL) {
			if (d + 3 > x->maxdepth)
				continue;
			gp_in_dfs(x, d + 1);
		}
	}
}

static int gp_search_in(struct gp_ctx *x, uint8_t adc, uint8_t pin,
			struct hda_path *out)
{
	x->target = pin;
	x->best_cost = 0x7fffffff;
	x->best_n = 0;
	x->stack[0] = adc;
	for (int depth = 2; depth <= HDA_MAX_PATH; depth++) {
		x->maxdepth = depth;
		x->budget = GP_BUDGET;
		gp_in_dfs(x, 0);
		if (x->best_n)
			break;
	}
	if (!x->best_n)
		return 0;
	gp_stack_to_path(x, out);
	return 1;
}

static int gp_is_capture_adc(struct hda_widget *w)
{
	return w->type == AC_WID_AUD_IN && !(w->caps & AC_WCAP_DIGITAL) &&
	       w->nconn > 0;
}

static void gp_route_inputs(struct gp_ctx *x)
{
	struct hda_codec *c = x->c;
	int best_count = 0;
	uint8_t best_adc = 0;

	if (!x->nins)
		return;
	/* The ADC that reaches the most inputs. */
	for (uint32_t i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];
		struct hda_path p;
		int n = 0;

		if (!gp_is_capture_adc(w))
			continue;
		for (int k = 0; k < x->nins; k++)
			if (gp_search_in(x, w->nid, x->ins[k].nid, &p))
				n++;
		if (n > best_count) {
			best_count = n;
			best_adc = w->nid;
		}
	}
	c->spec.adc = best_adc;
	if (!best_adc)
		return;
	for (int k = 0; k < x->nins; k++) {
		struct gp_pin *in = &x->ins[k];
		struct hda_path p;

		if (gp_search_in(x, best_adc, in->nid, &p)) {
			in->path = p;
			in->conv = best_adc;
			in->ok = 1;
			continue;
		}
		for (uint32_t i = 0; i < c->num_nodes; i++) {
			struct hda_widget *w = &c->w[i];

			if (!gp_is_capture_adc(w) || w->nid == best_adc)
				continue;
			if (gp_search_in(x, w->nid, in->nid, &p)) {
				in->path = p;
				in->conv = w->nid;
				in->ok = 1;
				break;
			}
		}
	}
	for (int k = 0; k < x->nins; k++) {
		if (!x->ins[k].ok)
			continue;
		for (int j = 0; j < x->ins[k].path.depth; j++) {
			struct hda_widget *w =
				hda_widget(c, x->ins[k].path.nid[j]);

			if (w)
				w->used = 1;
			if (x->in_users[x->ins[k].path.nid[j]] < 255)
				x->in_users[x->ins[k].path.nid[j]]++;
		}
	}
}

/* ---- volume and mute points ---------------------------------------------- */

/* Is (nid, dir, idx) used by output paths other than `self'? */
static int gp_amp_shared(struct gp_ctx *x, const struct hda_path *self,
			 uint8_t nid, int dir, uint8_t idx)
{
	struct hda_codec *c = x->c;

	for (int i = 0; i < c->spec.nout; i++) {
		const struct hda_path *p = &c->spec.out[i].path;

		if (p == self)
			continue;
		for (int j = 0; j < p->depth; j++) {
			if (p->nid[j] != nid)
				continue;
			/* an input amp is shared only through the same
			 * input; the output amp by anyone passing the node */
			if (dir == HDA_OUTPUT || p->idx[j] == idx || j == 0)
				return 1;
		}
	}
	return 0;
}

static void gp_pick_out_amps(struct gp_ctx *x, struct hda_path *p)
{
	struct hda_codec *c = x->c;
	int n = p->depth;

	p->vol_nid = p->mute_nid = 0;
	/* volume: an output amp with steps nearest the converter, then an
	 * input amp with steps */
	for (int j = 0; j < n && !p->vol_nid; j++) {
		if (amp_has_steps(c, p->nid[j], HDA_OUTPUT)) {
			p->vol_nid = p->nid[j];
			p->vol_dir = HDA_OUTPUT;
			p->vol_idx = 0;
		}
	}
	for (int j = 1; j < n && !p->vol_nid; j++) {
		if (amp_has_steps(c, p->nid[j], HDA_INPUT)) {
			p->vol_nid = p->nid[j];
			p->vol_dir = HDA_INPUT;
			p->vol_idx = p->idx[j];
		}
	}
	/* mute: a mute switch nearest the pin, on this path alone */
	for (int j = n - 1; j >= 0 && !p->mute_nid; j--) {
		uint8_t nid = p->nid[j];

		if (amp_has_mute(c, nid, HDA_OUTPUT) &&
		    !gp_amp_shared(x, p, nid, HDA_OUTPUT, 0)) {
			p->mute_nid = nid;
			p->mute_dir = HDA_OUTPUT;
			p->mute_idx = 0;
			break;
		}
		if (j > 0 && amp_has_mute(c, nid, HDA_INPUT) &&
		    !gp_amp_shared(x, p, nid, HDA_INPUT, p->idx[j])) {
			p->mute_nid = nid;
			p->mute_dir = HDA_INPUT;
			p->mute_idx = p->idx[j];
		}
	}
}

static void gp_pick_in_amps(struct hda_codec *c, struct hda_path *p)
{
	int n = p->depth;

	p->vol_nid = p->mute_nid = 0;
	/* capture volume: nearest the ADC, never the pin (that is the
	 * boost) */
	for (int j = n - 1; j >= 1 && !p->vol_nid; j--) {
		uint8_t nid = p->nid[j];

		if (amp_has_steps(c, nid, HDA_INPUT)) {
			p->vol_nid = nid;
			p->vol_dir = HDA_INPUT;
			p->vol_idx = p->idx[j];
		} else if (amp_has_steps(c, nid, HDA_OUTPUT)) {
			p->vol_nid = nid;
			p->vol_dir = HDA_OUTPUT;
			p->vol_idx = 0;
		}
	}
	for (int j = n - 1; j >= 1 && !p->mute_nid; j--) {
		uint8_t nid = p->nid[j];

		if (amp_has_mute(c, nid, HDA_INPUT)) {
			p->mute_nid = nid;
			p->mute_dir = HDA_INPUT;
			p->mute_idx = p->idx[j];
		} else if (amp_has_mute(c, nid, HDA_OUTPUT)) {
			p->mute_nid = nid;
			p->mute_dir = HDA_OUTPUT;
			p->mute_idx = 0;
		}
	}
}

/* ---- naming ------------------------------------------------------------------ */

static void gp_name_outputs(struct hda_codec *c)
{
	int cnt[8] = { 0 }, seen[8] = { 0 };

	for (int i = 0; i < c->spec.nout; i++) {
		struct hda_out *o = &c->spec.out[i];
		int t = (int)o->type & 7;

		cnt[t]++;
	}
	for (int i = 0; i < c->spec.nout; i++) {
		struct hda_out *o = &c->spec.out[i];
		int t = (int)o->type & 7;
		const char *base;
		char tmp[32];

		switch (o->type) {
		case AUDIO_OUT_SPEAKER:
			base = "Speaker";
			break;
		case AUDIO_OUT_HEADPHONE:
			base = "Headphone";
			break;
		case AUDIO_OUT_SPDIF:
			base = "S/PDIF";
			break;
		default:
			base = "Line Out";
			break;
		}
		seen[t]++;
		if (o->dock)
			ksnprintf(tmp, sizeof(tmp), "Dock %s", base);
		else if (cnt[t] > 1 && seen[t] > 1)
			ksnprintf(tmp, sizeof(tmp), "%s %d", base, seen[t]);
		else
			ksnprintf(tmp, sizeof(tmp), "%s", base);
		hda_strlcpy(o->name, tmp, sizeof(o->name));
	}
}

static void gp_name_inputs(struct hda_codec *c, struct gp_ctx *x,
			   const int *map)
{
	int mic_jacks = 0, mics_seen = 0;

	for (int i = 0; i < c->spec.nin; i++) {
		const struct gp_pin *g = &x->ins[map[i]];

		if (g->kind == K_MIC && !g->internal && !g->dock)
			mic_jacks++;
	}
	for (int i = 0; i < c->spec.nin; i++) {
		struct hda_in *in = &c->spec.in[i];
		const struct gp_pin *g = &x->ins[map[i]];
		const char *name;

		switch (g->kind) {
		case K_MIC:
			if (g->dock)
				name = "Dock Mic";
			else if (g->internal)
				name = "Internal Mic";
			else if (mic_jacks > 1) {
				mics_seen++;
				if ((g->loc & 0xf) == AC_JACK_LOC_FRONT)
					name = "Front Mic";
				else if ((g->loc & 0xf) == AC_JACK_LOC_REAR)
					name = "Rear Mic";
				else
					name = mics_seen > 1 ? "Mic 2" : "Mic";
			} else
				name = "Mic";
			break;
		case K_CD:
			name = "CD";
			break;
		default:
			name = g->dock ? "Dock Line In" : "Line In";
			break;
		}
		hda_strlcpy(in->name, name, sizeof(in->name));
	}
}

/* ======================================================================== */
/* hda_generic_parse                                                        */
/* ======================================================================== */

int hda_generic_parse(struct hda_codec *c)
{
	struct hda_codec_priv *pr;
	struct hda_spec *s;
	struct gp_ctx *x;
	int order[HDA_MAX_OUTPUTS], norder = 0;
	int inmap[HDA_MAX_INPUTS];

	if (!c || !c->w)
		return -EINVAL;
	pr = hda_priv(c);
	s = &c->spec;

	x = kcalloc(1, sizeof(*x));
	if (!x)
		return -ENOMEM;
	x->c = c;

	hda_codec_lock(c);

	for (uint32_t i = 0; i < c->num_nodes; i++)
		c->w[i].used = 0;
	kmemset(s->out, 0, sizeof(s->out));
	kmemset(s->in, 0, sizeof(s->in));
	s->nout = s->nin = 0;
	s->primary_dac = s->hp_dac = s->adc = s->dig_out_nid = 0;

	gp_collect(x);

	/* DAC assignment order: the first (primary) output, headphones, the
	 * rest of the analog outputs, then S/PDIF. */
	if (x->nouts)
		order[norder++] = 0;
	for (int i = 1; i < x->nouts; i++)
		if (x->outs[i].kind == K_HP)
			order[norder++] = i;
	for (int i = 1; i < x->nouts; i++)
		if (x->outs[i].kind != K_HP && x->outs[i].kind != K_SPDIF)
			order[norder++] = i;
	for (int i = 1; i < x->nouts; i++)
		if (x->outs[i].kind == K_SPDIF)
			order[norder++] = i;
	/* An S/PDIF-only codec: out[0] is digital. */
	for (int k = 0; k < norder; k++)
		gp_route_out(x, order[k]);

	/* Publish the outputs that got a route, in list order. */
	for (int i = 0; i < x->nouts && s->nout < HDA_MAX_OUTPUTS; i++) {
		struct gp_pin *g = &x->outs[i];
		struct hda_out *o;
		struct hda_widget *w;

		if (!g->ok)
			continue;
		o = &s->out[s->nout];
		w = hda_widget(c, g->nid);
		o->id = HDA_OUT_ID(c->addr, s->nout);
		o->type = out_type_of(g->kind);
		o->pin = g->nid;
		o->dac = g->conv;
		o->jack_detect = g->jack;
		o->present = 1;
		o->hp_capable = w && (w->pincap & AC_PINCAP_HP_DRV) ? 1 : 0;
		o->eapd = w ? w->eapd : 0;
		o->dock = g->dock;
		o->seq = g->seq;
		o->path = g->path;
		if (o->type == AUDIO_OUT_SPDIF && !s->dig_out_nid)
			s->dig_out_nid = g->conv;
		s->nout++;
	}
	for (int i = 0; i < s->nout; i++) {
		struct hda_out *o = &s->out[i];

		if (is_analog_out(o) && !s->primary_dac)
			s->primary_dac = o->dac;
		if (o->type == AUDIO_OUT_HEADPHONE && !s->hp_dac &&
		    o->dac != s->primary_dac && is_analog_out(o))
			s->hp_dac = o->dac;
	}
	for (int i = 0; i < s->nout; i++)
		gp_pick_out_amps(x, &s->out[i].path);
	gp_name_outputs(c);

	/* Inputs */
	gp_route_inputs(x);
	for (int k = 0; k < x->nins && s->nin < HDA_MAX_INPUTS; k++) {
		struct gp_pin *g = &x->ins[k];
		struct hda_in *in;

		if (!g->ok)
			continue;
		in = &s->in[s->nin];
		in->id = HDA_IN_ID(c->addr, s->nin);
		switch (g->kind) {
		case K_MIC:
			in->type = AC_JACK_MIC_IN;
			break;
		case K_CD:
			in->type = AC_JACK_CD;
			break;
		default:
			in->type = AC_JACK_LINE_IN;
			break;
		}
		in->pin = g->nid;
		in->adc = g->conv;
		in->jack_detect = g->jack;
		in->present = 1;
		in->internal = g->internal;
		in->path = g->path;
		gp_pick_in_amps(c, &in->path);
		inmap[s->nin] = k;
		s->nin++;
	}
	/* Internal mics first among the mics would surprise nobody, but the
	 * list stays in pin order; naming tells them apart. */
	gp_name_inputs(c, x, inmap);

	/* Jack detection by unsolicited response needs the pin to support
	 * it; otherwise the controller polls this codec. */
	for (int i = 0; i < s->nout; i++) {
		struct hda_widget *w = hda_widget(c, s->out[i].pin);

		if (s->out[i].jack_detect && w &&
		    !(w->caps & AC_WCAP_UNSOL_CAP))
			s->jack_poll = 1;
	}
	for (int i = 0; i < s->nin; i++) {
		struct hda_widget *w = hda_widget(c, s->in[i].pin);

		if (s->in[i].jack_detect && w &&
		    !(w->caps & AC_WCAP_UNSOL_CAP))
			s->jack_poll = 1;
	}
	if (pr->fix_flags & HDA_FIX_JACK_POLL)
		s->jack_poll = 1;
	if (pr->fix_flags & HDA_FIX_NO_AUTOMUTE)
		s->auto_mute = 0;

	pr->generic_parsed = 1;
	pr->in_active = -1;
	hda_codec_unlock(c);
	kfree(x);
	return s->nout ? 0 : -ENODEV;
}

/* ======================================================================== */
/* amps                                                                     */
/* ======================================================================== */

struct amp_plan {
	uint8_t nid, dir, idx;
	uint8_t have_gain;
	uint8_t mute_users, mute_votes; /* users wanting it muted */
	uint8_t force_mute; /* a control (PCM) mutes it outright */
	uint32_t gain_l, gain_r;
};

#define AMP_PLAN_MAX (HDA_MAX_OUTPUTS * 3 + 8)

static struct amp_plan *plan_get(struct amp_plan *pl, int *n, uint8_t nid,
				 int dir, uint8_t idx)
{
	for (int i = 0; i < *n; i++)
		if (pl[i].nid == nid && pl[i].dir == dir &&
		    (dir == HDA_OUTPUT || pl[i].idx == idx))
			return &pl[i];
	if (*n >= AMP_PLAN_MAX)
		return 0;
	kmemset(&pl[*n], 0, sizeof(pl[0]));
	pl[*n].nid = nid;
	pl[*n].dir = (uint8_t)dir;
	pl[*n].idx = dir == HDA_OUTPUT ? 0 : idx;
	return &pl[(*n)++];
}

static void plan_gain(struct amp_plan *e, uint32_t l, uint32_t r)
{
	if (!e->have_gain) {
		e->gain_l = l;
		e->gain_r = r;
		e->have_gain = 1;
		return;
	}
	if (l > e->gain_l)
		e->gain_l = l;
	if (r > e->gain_r)
		e->gain_r = r;
}

static void plan_mute(struct amp_plan *e, int muted)
{
	e->mute_users++;
	if (muted)
		e->mute_votes++;
}

/* Effective 0..100 level of an output: master scaled by its own control. */
static void out_level(struct hda_codec *c, const struct hda_out *o,
		      uint32_t *l, uint32_t *r, int *muted)
{
	struct hda_codec_priv *p = hda_priv(c);
	int ctl = out_ctl_of(o);
	uint32_t ml = p->ctl_l[AUDIO_CTL_MASTER], mr = p->ctl_r[AUDIO_CTL_MASTER];
	int m = p->ctl_mute[AUDIO_CTL_MASTER] != 0;

	if (ctl >= 0) {
		ml = ml * p->ctl_l[ctl] / 100;
		mr = mr * p->ctl_r[ctl] / 100;
		if (p->ctl_mute[ctl])
			m = 1;
	}
	if (!ml && !mr)
		m = 1;
	*l = ml;
	*r = mr;
	*muted = m;
}

/* Is a DAC's output amp somebody's path volume? */
static int dac_amp_is_volume(struct hda_codec *c, uint8_t dac)
{
	for (int i = 0; i < c->spec.nout; i++) {
		const struct hda_path *p = &c->spec.out[i].path;

		if (p->vol_nid == dac && p->vol_dir == HDA_OUTPUT)
			return 1;
	}
	return 0;
}

static int boost_amp_of(struct hda_codec *c, const struct hda_in *in)
{
	return in && amp_has_steps(c, in->pin, HDA_INPUT);
}

void hda_generic_apply_amps(struct hda_codec *c)
{
	struct hda_codec_priv *p = hda_priv(c);
	struct hda_spec *s = &c->spec;
	struct amp_plan pl[AMP_PLAN_MAX];
	int n = 0;

	hda_codec_lock(c);
	for (int i = 0; i < s->nout; i++) {
		struct hda_out *o = &s->out[i];
		struct hda_path *path = &o->path;
		uint32_t l, r;
		int muted;

		out_level(c, o, &l, &r, &muted);
		if (!p->out_active[i])
			muted = 1;
		if (path->vol_nid) {
			uint32_t caps = hda_amp_caps(c, path->vol_nid,
						     path->vol_dir);
			struct amp_plan *e = plan_get(pl, &n, path->vol_nid,
						      path->vol_dir,
						      path->vol_idx);

			if (e && p->out_active[i])
				plan_gain(e, hda_amp_pct_to_step(caps, l),
					  hda_amp_pct_to_step(caps, r));
			/* Without a mute point the volume amp mutes, as long
			 * as nobody else still plays through it. */
			if (e && !path->mute_nid)
				plan_mute(e, muted);
		}
		if (path->mute_nid) {
			struct amp_plan *e = plan_get(pl, &n, path->mute_nid,
						      path->mute_dir,
						      path->mute_idx);

			if (e)
				plan_mute(e, muted);
		}
	}

	/* PCM: the converter amps that are not the master volume. */
	for (int i = 0; i < s->nout; i++) {
		uint8_t dac = s->out[i].dac;
		struct amp_plan *e;
		uint32_t caps;

		if (!amp_present(c, dac, HDA_OUTPUT) ||
		    dac_amp_is_volume(c, dac))
			continue;
		e = plan_get(pl, &n, dac, HDA_OUTPUT, 0);
		if (!e)
			continue;
		caps = hda_amp_caps(c, dac, HDA_OUTPUT);
		if (hda_amp_nsteps(caps)) {
			e->have_gain = 0;
			plan_gain(e,
				  hda_amp_pct_to_step(caps,
						      p->ctl_l[AUDIO_CTL_PCM]),
				  hda_amp_pct_to_step(caps,
						      p->ctl_r[AUDIO_CTL_PCM]));
		}
		if (p->ctl_mute[AUDIO_CTL_PCM] ||
		    (!p->ctl_l[AUDIO_CTL_PCM] && !p->ctl_r[AUDIO_CTL_PCM]))
			e->force_mute = 1;
	}

	for (int i = 0; i < n; i++) {
		struct amp_plan *e = &pl[i];
		uint32_t caps = hda_amp_caps(c, e->nid, e->dir);
		uint32_t gl, gr;
		int mute = e->force_mute ||
			   (e->mute_users && e->mute_votes == e->mute_users);

		if (e->have_gain) {
			gl = e->gain_l;
			gr = e->gain_r;
		} else {
			/* No active user sets a level: keep what is there
			 * when it is a volume amp, otherwise 0 dB. */
			uint32_t cur_l = hda_amp_get(c, e->nid, e->dir, e->idx,
						     1);
			uint32_t cur_r = hda_amp_get(c, e->nid, e->dir, e->idx,
						     0);

			if (cur_l == 0xffffffffu || cur_r == 0xffffffffu) {
				gl = gr = hda_amp_0db_step(caps);
			} else {
				gl = cur_l & AC_AMP_GAIN;
				gr = cur_r & AC_AMP_GAIN;
			}
		}
		hda_amp_set(c, e->nid, e->dir, e->idx, gl, gr, mute);
	}

	/* Capture: the active input's capture amp and its boost. */
	if (p->in_active >= 0 && p->in_active < s->nin) {
		struct hda_in *in = &s->in[p->in_active];
		struct hda_path *path = &in->path;

		if (path->vol_nid) {
			uint32_t caps = hda_amp_caps(c, path->vol_nid,
						     path->vol_dir);
			int mute = p->ctl_mute[AUDIO_CTL_CAPTURE] != 0;

			hda_amp_set(c, path->vol_nid, path->vol_dir,
				    path->vol_idx,
				    hda_amp_pct_to_step(
					    caps, p->ctl_l[AUDIO_CTL_CAPTURE]),
				    hda_amp_pct_to_step(
					    caps, p->ctl_r[AUDIO_CTL_CAPTURE]),
				    mute);
		}
		if (path->mute_nid &&
		    !(path->mute_nid == path->vol_nid &&
		      path->mute_dir == path->vol_dir &&
		      path->mute_idx == path->vol_idx)) {
			uint32_t caps = hda_amp_caps(c, path->mute_nid,
						     path->mute_dir);
			uint32_t g = hda_amp_0db_step(caps);

			hda_amp_set(c, path->mute_nid, path->mute_dir,
				    path->mute_idx, g, g,
				    p->ctl_mute[AUDIO_CTL_CAPTURE] != 0);
		}
		if (boost_amp_of(c, in)) {
			uint32_t caps = hda_amp_caps(c, in->pin, HDA_INPUT);

			hda_amp_set(c, in->pin, HDA_INPUT, 0,
				    hda_amp_pct_to_step(
					    caps, p->ctl_l[AUDIO_CTL_MIC_BOOST]),
				    hda_amp_pct_to_step(
					    caps, p->ctl_r[AUDIO_CTL_MIC_BOOST]),
				    0);
		}
	}
	hda_codec_unlock(c);
}

static void update_routing(struct hda_codec *c, int force);

/* ======================================================================== */
/* init                                                                     */
/* ======================================================================== */

/* Unmute an amp at 0 dB (the neutral setting of amps that are on a path
 * but are neither its volume nor its mute point). */
static void amp_neutral(struct hda_codec *c, uint8_t nid, int dir, int idx)
{
	uint32_t caps;
	uint32_t g;

	if (!amp_present(c, nid, dir))
		return;
	caps = hda_amp_caps(c, nid, dir);
	g = hda_amp_0db_step(caps);
	hda_amp_set(c, nid, dir, idx, g, g, 0);
}

static void path_neutral(struct hda_codec *c, const struct hda_path *p)
{
	for (int j = 0; j < p->depth; j++) {
		amp_neutral(c, p->nid[j], HDA_OUTPUT, 0);
		if (j > 0)
			amp_neutral(c, p->nid[j], HDA_INPUT, p->idx[j]);
	}
}

/* Select the connections along a path (selectors, pins, converters with a
 * connection list; mixers sum and have no selection). */
static void path_select(struct hda_codec *c, const struct hda_path *p)
{
	for (int j = 1; j < p->depth; j++) {
		struct hda_widget *w = hda_widget(c, p->nid[j]);

		if (!w || w->type == AC_WID_AUD_MIX || w->nconn < 2)
			continue;
		if (hda_codec_write(c, w->nid, AC_VERB_SET_CONNECT_SEL,
				    p->idx[j]) == 0)
			w->cur_conn = p->idx[j];
	}
}

/* Does any output path enter mixer `nid' through input `idx'? */
static int mixer_input_on_out_path(struct hda_codec *c, uint8_t nid, int idx)
{
	for (int i = 0; i < c->spec.nout; i++) {
		const struct hda_path *p = &c->spec.out[i].path;

		for (int j = 1; j < p->depth; j++)
			if (p->nid[j] == nid && p->idx[j] == idx)
				return 1;
	}
	return 0;
}

/* Mute every mixer input that no output path uses (analog loopback, CD,
 * beep mixing): they only add noise.  The active input re-opens its own
 * route afterwards. */
static void mute_unused_mixer_inputs(struct hda_codec *c)
{
	for (uint32_t i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_AUD_MIX || !(w->caps & AC_WCAP_IN_AMP))
			continue;
		for (int k = 0; k < w->nconn && k < 16; k++) {
			if (mixer_input_on_out_path(c, w->nid, k))
				continue;
			hda_amp_set(c, w->nid, HDA_INPUT, k, 0, 0, 1);
		}
	}
}

static uint8_t mic_vref(struct hda_widget *w)
{
	uint32_t v = (w->pincap & AC_PINCAP_VREF) >> AC_PINCAP_VREF_SHIFT;

	if (v & (1u << AC_PINCTL_VREF_80))
		return AC_PINCTL_VREF_80;
	if (v & (1u << AC_PINCTL_VREF_50))
		return AC_PINCTL_VREF_50;
	if (v & (1u << AC_PINCTL_VREF_100))
		return AC_PINCTL_VREF_100;
	if (v & (1u << AC_PINCTL_VREF_GRD))
		return AC_PINCTL_VREF_GRD;
	return AC_PINCTL_VREF_HIZ;
}

/* Make input `idx' the one the ADC records. */
static void activate_input(struct hda_codec *c, int idx)
{
	struct hda_codec_priv *p = hda_priv(c);
	struct hda_spec *s = &c->spec;
	struct hda_in *in;
	uint8_t old_adc = 0;

	if (idx < 0 || idx >= s->nin)
		return;
	if (p->in_active >= 0 && p->in_active < s->nin)
		old_adc = s->in[p->in_active].adc;
	in = &s->in[idx];

	/* Close the mixer inputs of the other inputs on shared mixers, open
	 * ours. */
	for (int k = 0; k < s->nin; k++) {
		const struct hda_path *op = &s->in[k].path;

		if (k == idx)
			continue;
		for (int j = 1; j < op->depth; j++) {
			struct hda_widget *w = hda_widget(c, op->nid[j]);

			if (w && w->type == AC_WID_AUD_MIX &&
			    !mixer_input_on_out_path(c, w->nid, op->idx[j]))
				hda_amp_set(c, w->nid, HDA_INPUT, op->idx[j], 0,
					    0, 1);
		}
	}
	path_neutral(c, &in->path);
	path_select(c, &in->path);
	p->in_active = idx;

	/* A running capture stream follows to the new ADC. */
	if (p->cap_tag && p->bound_cap && in->adc != old_adc &&
	    p->bound_cap != in->adc) {
		hda_codec_write(c, p->bound_cap, AC_VERB_SET_CHANNEL_STREAMID,
				0);
		hda_codec_write(c, in->adc, AC_VERB_SET_STREAM_FORMAT,
				p->cap_fmt);
		hda_codec_write(c, in->adc, AC_VERB_SET_CHANNEL_STREAMID,
				(uint32_t)(p->cap_tag & 0xf)
					<< AC_CONV_STREAM_SHIFT);
		p->bound_cap = in->adc;
	}
}

/* AUTO input choice: a plugged external mic, else the internal mic, else
 * a plugged line in, else the first input. */
static int auto_input(struct hda_codec *c)
{
	struct hda_spec *s = &c->spec;

	for (int i = 0; i < s->nin; i++)
		if (s->in[i].type == AC_JACK_MIC_IN && !s->in[i].internal &&
		    s->in[i].jack_detect && s->in[i].present)
			return i;
	for (int i = 0; i < s->nin; i++)
		if (s->in[i].type == AC_JACK_MIC_IN && s->in[i].internal)
			return i;
	for (int i = 0; i < s->nin; i++)
		if (s->in[i].jack_detect && s->in[i].present)
			return i;
	return s->nin ? 0 : -1;
}

static void select_input(struct hda_codec *c)
{
	struct hda_codec_priv *p = hda_priv(c);
	int want = -1;

	if (p->in_mode == AUDIO_OUTSEL_ID) {
		for (int i = 0; i < c->spec.nin; i++)
			if (c->spec.in[i].id == p->in_sel_id)
				want = i;
	}
	if (want < 0)
		want = auto_input(c);
	if (want >= 0 && want != p->in_active)
		activate_input(c, want);
}

static void init_controls(struct hda_codec *c)
{
	struct hda_codec_priv *p = hda_priv(c);
	struct hda_spec *s = &c->spec;
	uint32_t master = 75, pcm = 100, cap = 75;

	if (p->ctl_initialised)
		return;
	/* Master at 75 %, but never above 0 dB on amps that can boost. */
	for (int i = 0; i < s->nout; i++) {
		const struct hda_path *path = &s->out[i].path;

		if (path->vol_nid) {
			uint32_t z = hda_amp_0db_pct(hda_amp_caps(
				c, path->vol_nid, path->vol_dir));

			master = min_u32(master, z);
			break;
		}
	}
	if (s->primary_dac && !dac_amp_is_volume(c, s->primary_dac) &&
	    amp_has_steps(c, s->primary_dac, HDA_OUTPUT))
		pcm = hda_amp_0db_pct(
			hda_amp_caps(c, s->primary_dac, HDA_OUTPUT));
	for (int i = 0; i < s->nin; i++) {
		const struct hda_path *path = &s->in[i].path;

		if (path->vol_nid) {
			cap = hda_amp_0db_pct(hda_amp_caps(c, path->vol_nid,
							   path->vol_dir));
			break;
		}
	}
	for (int k = 0; k < HDA_NCTL; k++) {
		p->ctl_l[k] = p->ctl_r[k] = 100;
		p->ctl_mute[k] = 0;
	}
	p->ctl_l[AUDIO_CTL_MASTER] = p->ctl_r[AUDIO_CTL_MASTER] = master;
	p->ctl_l[AUDIO_CTL_PCM] = p->ctl_r[AUDIO_CTL_PCM] = pcm;
	p->ctl_l[AUDIO_CTL_CAPTURE] = p->ctl_r[AUDIO_CTL_CAPTURE] = cap;
	p->ctl_l[AUDIO_CTL_MIC_BOOST] = p->ctl_r[AUDIO_CTL_MIC_BOOST] = 0;
	s->master_left = s->master_right = master;
	s->master_mute = 0;
	s->pcm_left = s->pcm_right = pcm;
	s->capture_left = s->capture_right = cap;
	p->ctl_initialised = 1;
}

int hda_generic_init(struct hda_codec *c)
{
	struct hda_codec_priv *p;
	struct hda_spec *s;

	if (!c || !c->w)
		return -EINVAL;
	p = hda_priv(c);
	s = &c->spec;
	hda_codec_lock(c);

	/* Power: the function group, then every widget a path uses. */
	if (c->afg)
		hda_set_power(c, c->afg, AC_PWRST_D0);
	for (uint32_t i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->used && (w->caps & AC_WCAP_POWER))
			hda_set_power(c, w->nid, AC_PWRST_D0);
	}

	/* Every path amp at a neutral 0 dB, the connections selected. */
	for (int i = 0; i < s->nout; i++) {
		path_neutral(c, &s->out[i].path);
		path_select(c, &s->out[i].path);
	}
	mute_unused_mixer_inputs(c);

	/* Input pins: enabled, microphones with their bias voltage. */
	for (int i = 0; i < s->nin; i++) {
		struct hda_in *in = &s->in[i];
		struct hda_widget *w = hda_widget(c, in->pin);
		uint8_t ctl = AC_PINCTL_IN_EN;

		if (!w)
			continue;
		if (in->type == AC_JACK_MIC_IN)
			ctl |= mic_vref(w);
		hda_pin_set_ctl(c, in->pin, ctl);
	}

	/* External amplifiers on output pins. */
	for (int i = 0; i < s->nout; i++)
		if (s->out[i].eapd)
			hda_set_eapd(c, s->out[i].pin, 1);

	/* The S/PDIF converter must be enabled to send anything. */
	if (s->dig_out_nid) {
		uint32_t d = hda_codec_read(c, s->dig_out_nid,
					    AC_VERB_GET_DIGI_CONVERT_1, 0);

		if (d == 0xffffffffu)
			d = 0;
		d = (d & 0xff) | AC_DIG1_ENABLE;
		d &= ~(uint32_t)AC_DIG1_NONAUDIO;
		hda_codec_write(c, s->dig_out_nid, AC_VERB_SET_DIGI_CONVERT_1,
				d & 0xff);
	}

	/* Unsolicited responses on every jack pin. */
	if (!s->jack_poll) {
		for (int i = 0; i < s->nout + s->nin; i++) {
			uint8_t pin;
			int jd;
			struct hda_widget *w;
			uint8_t tag;

			if (i < s->nout) {
				pin = s->out[i].pin;
				jd = s->out[i].jack_detect;
			} else {
				pin = s->in[i - s->nout].pin;
				jd = s->in[i - s->nout].jack_detect;
			}
			w = hda_widget(c, pin);
			if (!jd || !w || !(w->caps & AC_WCAP_UNSOL_CAP))
				continue;
			tag = hda_jack_tag_for(c, pin);
			if (!tag) {
				s->jack_poll = 1;
				continue;
			}
			hda_codec_write(c, pin, AC_VERB_SET_UNSOLICITED_ENABLE,
					AC_USRSP_EN | (tag & AC_UNSOL_TAG));
		}
	}

	hda_generic_sense_jacks(c, 0);
	init_controls(c);
	p->in_active = -1;
	select_input(c);
	update_routing(c, 1);
	hda_codec_unlock(c);
	return 0;
}

/* ======================================================================== */
/* jacks and routing                                                        */
/* ======================================================================== */

int hda_generic_sense_jacks(struct hda_codec *c, int report)
{
	struct hda_codec_priv *p = hda_priv(c);
	struct hda_spec *s = &c->spec;
	struct snd_card *card = c->ctrl ? c->ctrl->card : 0;
	int changed = 0, in_changed = 0;

	hda_codec_lock(c);
	for (int i = 0; i < s->nout; i++) {
		struct hda_out *o = &s->out[i];
		int now;

		if (!o->jack_detect)
			continue;
		now = hda_pin_sense(c, o->pin);
		if (now != o->present) {
			o->present = (uint8_t)now;
			changed = 1;
			if (report && card)
				snd_jack_report(card, o->id, now);
		}
	}
	for (int i = 0; i < s->nin; i++) {
		struct hda_in *in = &s->in[i];
		int now;

		if (!in->jack_detect)
			continue;
		now = hda_pin_sense(c, in->pin);
		if (now != in->present) {
			in->present = (uint8_t)now;
			changed = 1;
			in_changed = 1;
			if (report && card)
				snd_jack_report(card, in->id, now);
		}
	}
	if (in_changed && p->in_mode == AUDIO_OUTSEL_AUTO)
		select_input(c);
	hda_codec_unlock(c);
	return changed;
}

static int out_usable(const struct hda_out *o)
{
	return !o->jack_detect || o->present;
}

static void update_routing(struct hda_codec *c, int force)
{
	struct hda_codec_priv *p;
	struct hda_spec *s;
	uint32_t mask = 0, old;
	int hp_present = 0, any_analog = 0;
	struct snd_card *card;

	if (!c)
		return;
	p = hda_priv(c);
	s = &c->spec;
	if (hda_is_display(c) || !p->generic_parsed)
		return;
	hda_codec_lock(c);

	for (int i = 0; i < s->nout; i++)
		if (s->out[i].type == AUDIO_OUT_HEADPHONE &&
		    s->out[i].jack_detect && s->out[i].present)
			hp_present = 1;

	if (s->outsel_mode == AUDIO_OUTSEL_ID) {
		for (int i = 0; i < s->nout; i++)
			if (s->out[i].id == s->outsel_id)
				mask |= 1u << i;
	} else if (s->outsel_mode == AUDIO_OUTSEL_ALL) {
		for (int i = 0; i < s->nout; i++)
			if (out_usable(&s->out[i]))
				mask |= 1u << i;
	}
	if (!mask) {
		/* AUTO (or an ID that no longer exists) */
		if (s->auto_mute && hp_present) {
			/* A plugged headphone takes over from speakers and
			 * line outs. */
			for (int i = 0; i < s->nout; i++)
				if (s->out[i].type == AUDIO_OUT_HEADPHONE &&
				    out_usable(&s->out[i]))
					mask |= 1u << i;
		} else {
			for (int i = 0; i < s->nout; i++)
				if (is_analog_out(&s->out[i]) &&
				    out_usable(&s->out[i]))
					mask |= 1u << i;
		}
		for (int i = 0; i < s->nout; i++)
			if (is_analog_out(&s->out[i]))
				any_analog = 1;
		/* Every jack empty (a desktop with nothing plugged, or an
		 * emulated codec that reports nothing): play everywhere
		 * rather than nowhere. */
		if (!mask) {
			for (int i = 0; i < s->nout; i++)
				if (is_analog_out(&s->out[i]) || !any_analog)
					mask |= 1u << i;
		}
	}

	old = p->active_mask;
	p->active_mask = mask;
	for (int i = 0; i < s->nout; i++) {
		struct hda_out *o = &s->out[i];
		struct hda_widget *w = hda_widget(c, o->pin);
		int on = (mask >> i) & 1;
		uint8_t ctl = 0;

		p->out_active[i] = (uint8_t)on;
		o->path.active = (uint8_t)on;
		if (on) {
			ctl = AC_PINCTL_OUT_EN;
			if (o->type == AUDIO_OUT_HEADPHONE && o->hp_capable)
				ctl |= AC_PINCTL_HP_EN;
		}
		/* After a reset the cached value can be stale: (re)init
		 * writes every pin. */
		if (w && (w->pinctl != ctl || force || !p->probe_done))
			hda_pin_set_ctl(c, o->pin, ctl);
	}

	hda_generic_apply_amps(c);

	if (c->vendor && c->vendor->automute)
		c->vendor->automute(c, hp_present);

	card = c->ctrl ? c->ctrl->card : 0;
	if (card && old != mask && p->probe_done) {
		uint32_t first = 0;

		for (int i = 0; i < s->nout; i++)
			if ((mask >> i) & 1) {
				first = s->out[i].id;
				break;
			}
		snd_card_event(card, AUDIO_EV_OUTPUT, first, mask);
	}
	hda_codec_unlock(c);
}

void hda_codec_update_routing(struct hda_codec *c)
{
	update_routing(c, 0);
}

/* ======================================================================== */
/* streams                                                                  */
/* ======================================================================== */

/* Containers above 16 bits are 32 bits wide either way; tell each converter
 * a width it supports. */
static uint16_t adjust_format(struct hda_codec *c, uint8_t nid, uint16_t fmt)
{
	uint32_t pcm = hda_conv_pcm(c, nid);
	uint32_t bits = fmt & AC_FMT_BITS_MASK;
	uint32_t nb = bits;

	if (bits == AC_FMT_BITS_32 && !(pcm & AC_SUPPCM_BITS_32)) {
		if (pcm & AC_SUPPCM_BITS_24)
			nb = AC_FMT_BITS_24;
		else if (pcm & AC_SUPPCM_BITS_20)
			nb = AC_FMT_BITS_20;
	} else if (bits == AC_FMT_BITS_24 && !(pcm & AC_SUPPCM_BITS_24)) {
		if (pcm & AC_SUPPCM_BITS_32)
			nb = AC_FMT_BITS_32;
		else if (pcm & AC_SUPPCM_BITS_20)
			nb = AC_FMT_BITS_20;
	} else if (bits == AC_FMT_BITS_20 && !(pcm & AC_SUPPCM_BITS_20)) {
		if (pcm & AC_SUPPCM_BITS_24)
			nb = AC_FMT_BITS_24;
		else if (pcm & AC_SUPPCM_BITS_32)
			nb = AC_FMT_BITS_32;
	}
	return (uint16_t)((fmt & ~AC_FMT_BITS_MASK) | nb);
}

static void bind_converter(struct hda_codec *c, uint8_t nid, int tag,
			   int channels, uint16_t fmt)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w)
		return;
	/* A multichannel converter is told how many slots the stream
	 * carries (at least a stereo pair). */
	if (hda_widget_channels(w) > 2)
		hda_codec_write(c, nid, AC_VERB_SET_CVT_CHAN_COUNT,
				(uint32_t)((channels < 2 ? 2 : channels) - 1));
	hda_codec_write(c, nid, AC_VERB_SET_STREAM_FORMAT,
			adjust_format(c, nid, fmt));
	hda_codec_write(c, nid, AC_VERB_SET_CHANNEL_STREAMID,
			((uint32_t)(tag & 0xf) << AC_CONV_STREAM_SHIFT) | 0);
}

int hda_codec_setup_stream(struct hda_codec *c, int direction, int tag,
			   int channels, uint16_t format)
{
	struct hda_codec_priv *p;
	struct hda_spec *s;

	if (!c || tag < 1 || tag > 15 || channels < 1 || channels > 16)
		return -EINVAL;
	p = hda_priv(c);
	s = &c->spec;
	if (direction != AUDIO_DIR_PLAYBACK && direction != AUDIO_DIR_CAPTURE)
		return -EINVAL;

	hda_codec_lock(c);
	if (direction == AUDIO_DIR_PLAYBACK) {
		int nb = 0;

		/* Every converter of every output gets the stream so the
		 * output set can change while it plays. */
		for (int i = 0; i < s->nout; i++) {
			uint8_t dac = s->out[i].dac;
			int dup = 0;

			if (!dac)
				continue;
			for (int k = 0; k < nb; k++)
				if (p->bound_play[k] == dac)
					dup = 1;
			if (dup || nb >= (int)sizeof(p->bound_play))
				continue;
			p->bound_play[nb++] = dac;
		}
		if (!nb) {
			hda_codec_unlock(c);
			return -ENODEV;
		}
		for (int k = 0; k < nb; k++)
			bind_converter(c, p->bound_play[k], tag, channels,
				       format);
		p->nbound_play = nb;
		p->play_tag = tag;
		p->play_fmt = format;
		p->play_channels = channels;
		if (c->vendor && c->vendor->playback_hook)
			c->vendor->playback_hook(c, 1);
	} else {
		uint8_t adc = 0;

		if (p->in_active >= 0 && p->in_active < s->nin)
			adc = s->in[p->in_active].adc;
		if (!adc)
			adc = s->adc;
		if (!adc) {
			hda_codec_unlock(c);
			return -ENODEV;
		}
		bind_converter(c, adc, tag, channels, format);
		p->bound_cap = adc;
		p->cap_tag = tag;
		p->cap_fmt = format;
		p->cap_channels = channels;
	}
	hda_codec_unlock(c);
	return 0;
}

void hda_codec_cleanup_stream(struct hda_codec *c, int direction)
{
	struct hda_codec_priv *p;

	if (!c)
		return;
	p = hda_priv(c);
	hda_codec_lock(c);
	if (direction == AUDIO_DIR_PLAYBACK) {
		if (c->vendor && c->vendor->playback_hook && p->nbound_play)
			c->vendor->playback_hook(c, 0);
		for (int k = 0; k < p->nbound_play; k++)
			hda_codec_write(c, p->bound_play[k],
					AC_VERB_SET_CHANNEL_STREAMID, 0);
		p->nbound_play = 0;
		p->play_tag = 0;
	} else if (direction == AUDIO_DIR_CAPTURE) {
		if (p->bound_cap)
			hda_codec_write(c, p->bound_cap,
					AC_VERB_SET_CHANNEL_STREAMID, 0);
		p->bound_cap = 0;
		p->cap_tag = 0;
	}
	hda_codec_unlock(c);
}

static void pcm_caps_to_hw(uint32_t pcm, uint32_t fmts, struct snd_pcm_hw *hw)
{
	uint32_t rates = pcm & AC_SUPPCM_RATES;

	/* The rate bits of the codec and of the UAPI have the same order:
	 * 8k 11.025k 16k 22.05k 32k 44.1k 48k 88.2k 96k 176.4k 192k 384k. */
	hw->rates = rates;
	if (!hw->rates)
		hw->rates = AUDIO_RATE_44100 | AUDIO_RATE_48000;
	hw->formats = 0;
	if (fmts && !(fmts & AC_SUPFMT_PCM)) {
		/* a converter that only takes compressed data */
		return;
	}
	if ((pcm & AC_SUPPCM_BITS_16) ||
	    !(pcm & (AC_SUPPCM_BITS_8 | AC_SUPPCM_BITS_20 | AC_SUPPCM_BITS_24 |
		     AC_SUPPCM_BITS_32)))
		hw->formats |= AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
	/* 20/24/32-bit samples travel in 32-bit containers, MSB aligned:
	 * exactly the S32_LE layout.  The codec gets the width it supports
	 * (adjust_format), the low bits are simply dropped. */
	if (pcm & (AC_SUPPCM_BITS_20 | AC_SUPPCM_BITS_24 | AC_SUPPCM_BITS_32))
		hw->formats |= AUDIO_FMT_BIT(AUDIO_FMT_S32_LE);
}

void hda_codec_pcm_hw(struct hda_codec *c, int direction,
		      struct snd_pcm_hw *hw)
{
	struct hda_codec_priv *p;
	struct hda_spec *s;
	uint32_t pcm = 0, fmts = 0, rates = AC_SUPPCM_RATES;
	uint32_t chmax = 2;
	uint8_t first = 0;
	int any = 0;

	if (!hw)
		return;
	kmemset(hw, 0, sizeof(*hw));
	if (!c)
		return;
	p = hda_priv(c);
	s = &c->spec;

	if (direction == AUDIO_DIR_PLAYBACK) {
		for (int i = 0; i < s->nout; i++) {
			uint8_t dac = s->out[i].dac;

			if (!dac || !is_analog_out(&s->out[i]))
				continue;
			/* All analog DACs share one stream: only the rates
			 * every one of them can play.  Sample widths may
			 * differ (adjust_format picks per converter). */
			rates &= hda_conv_pcm(c, dac) & AC_SUPPCM_RATES;
			if (!first) {
				first = dac;
				pcm = hda_conv_pcm(c, dac);
				fmts = hda_conv_stream_fmts(c, dac);
			}
			any = 1;
		}
		if (any && rates)
			pcm = (pcm & ~(uint32_t)AC_SUPPCM_RATES) | rates;
		if (!any && s->dig_out_nid) {
			first = s->dig_out_nid;
			pcm = hda_conv_pcm(c, first);
			fmts = hda_conv_stream_fmts(c, first);
			any = 1;
		}
	} else if (direction == AUDIO_DIR_CAPTURE) {
		uint8_t adc = s->adc;

		if (p->in_active >= 0 && p->in_active < s->nin)
			adc = s->in[p->in_active].adc;
		if (adc && s->nin) {
			first = adc;
			pcm = hda_conv_pcm(c, adc);
			fmts = hda_conv_stream_fmts(c, adc);
			any = 1;
		}
	}
	if (!any)
		return;

	pcm_caps_to_hw(pcm, fmts, hw);
	if (!hw->formats)
		return;
	{
		struct hda_widget *w = hda_widget(c, first);
		uint32_t ch = hda_widget_channels(w);

		if (ch >= 2)
			chmax = ch;
		hw->channels_min = ch >= 2 ? 2 : 1;
		hw->channels_max = ch >= 2 ? chmax : 1;
	}
	hw->period_bytes_min = 512;
	hw->period_bytes_max = 512 * 1024;
	hw->periods_min = 2;
	hw->periods_max = 32;
	hw->buffer_bytes_max = 2 * 1024 * 1024;
	hw->period_align = 128;
	hw->buffer_align = 128;
	hw->dma32 = 1;
	if (c->ctrl && c->ctrl->addr64 && !(c->ctrl->flags & HDA_F_NO_64BIT))
		hw->dma32 = 0;
	hw->dma_coherent = 0;
}

/* ======================================================================== */
/* mixer controls                                                           */
/* ======================================================================== */

static int has_out_type(struct hda_codec *c, uint32_t type)
{
	for (int i = 0; i < c->spec.nout; i++)
		if (c->spec.out[i].type == type)
			return 1;
	return 0;
}

static int has_boost(struct hda_codec *c)
{
	for (int i = 0; i < c->spec.nin; i++)
		if (boost_amp_of(c, &c->spec.in[i]))
			return 1;
	return 0;
}

static int ctl_exists(struct hda_codec *c, uint32_t ctl)
{
	switch (ctl) {
	case AUDIO_CTL_MASTER:
	case AUDIO_CTL_PCM:
		return c->spec.nout > 0;
	case AUDIO_CTL_SPEAKER:
		return has_out_type(c, AUDIO_OUT_SPEAKER);
	case AUDIO_CTL_HEADPHONE:
		return has_out_type(c, AUDIO_OUT_HEADPHONE);
	case AUDIO_CTL_LINE:
		return has_out_type(c, AUDIO_OUT_LINE);
	case AUDIO_CTL_CAPTURE:
		return c->spec.nin > 0;
	case AUDIO_CTL_MIC_BOOST:
		return has_boost(c);
	default:
		return 0;
	}
}

/* The hardware amp behind a control (for steps and dB), 0 if software. */
static uint32_t ctl_amp_caps(struct hda_codec *c, uint32_t ctl)
{
	struct hda_codec_priv *p = hda_priv(c);
	struct hda_spec *s = &c->spec;
	int pick = -1;

	switch (ctl) {
	case AUDIO_CTL_MASTER:
		for (int i = 0; i < s->nout; i++)
			if (p->out_active[i] && s->out[i].path.vol_nid) {
				pick = i;
				break;
			}
		if (pick < 0)
			for (int i = 0; i < s->nout; i++)
				if (s->out[i].path.vol_nid) {
					pick = i;
					break;
				}
		break;
	case AUDIO_CTL_SPEAKER:
	case AUDIO_CTL_HEADPHONE:
	case AUDIO_CTL_LINE:
		for (int i = 0; i < s->nout; i++)
			if (out_ctl_of(&s->out[i]) == (int)ctl &&
			    s->out[i].path.vol_nid) {
				pick = i;
				break;
			}
		break;
	case AUDIO_CTL_PCM:
		if (s->primary_dac && !dac_amp_is_volume(c, s->primary_dac))
			return hda_amp_caps(c, s->primary_dac, HDA_OUTPUT);
		return 0;
	case AUDIO_CTL_CAPTURE: {
		int k = p->in_active >= 0 ? p->in_active : 0;

		if (k < s->nin && s->in[k].path.vol_nid)
			return hda_amp_caps(c, s->in[k].path.vol_nid,
					    s->in[k].path.vol_dir);
		return 0;
	}
	case AUDIO_CTL_MIC_BOOST: {
		int k = p->in_active >= 0 ? p->in_active : 0;

		if (k < s->nin && boost_amp_of(c, &s->in[k]))
			return hda_amp_caps(c, s->in[k].pin, HDA_INPUT);
		for (int i = 0; i < s->nin; i++)
			if (boost_amp_of(c, &s->in[i]))
				return hda_amp_caps(c, s->in[i].pin,
						    HDA_INPUT);
		return 0;
	}
	default:
		return 0;
	}
	if (pick < 0)
		return 0;
	return hda_amp_caps(c, s->out[pick].path.vol_nid,
			    s->out[pick].path.vol_dir);
}

int hda_codec_get_volume(struct hda_codec *c, uint32_t ctl,
			 struct audio_volume *v)
{
	struct hda_codec_priv *p;
	uint32_t caps, n;

	if (!c || !v)
		return -EINVAL;
	if (ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	p = hda_priv(c);
	if (hda_is_display(c) || !p->generic_parsed || !ctl_exists(c, ctl))
		return -ENOENT;
	hda_codec_lock(c);
	kmemset(v, 0, sizeof(*v));
	v->control = ctl;
	v->left = p->ctl_l[ctl];
	v->right = p->ctl_r[ctl];
	caps = ctl_amp_caps(c, ctl);
	n = hda_amp_nsteps(caps);
	if (n) {
		v->steps = n + 1;
		v->db_min = hda_amp_step_db(caps, 0);
		v->db_max = hda_amp_step_db(caps, n);
		v->db_left = hda_amp_step_db(caps,
					     hda_amp_pct_to_step(caps, v->left));
		v->db_right = hda_amp_step_db(
			caps, hda_amp_pct_to_step(caps, v->right));
	}
	hda_codec_unlock(c);
	return 0;
}

static void mirror_spec(struct hda_codec *c)
{
	struct hda_codec_priv *p = hda_priv(c);
	struct hda_spec *s = &c->spec;

	s->master_left = p->ctl_l[AUDIO_CTL_MASTER];
	s->master_right = p->ctl_r[AUDIO_CTL_MASTER];
	s->master_mute = p->ctl_mute[AUDIO_CTL_MASTER];
	s->pcm_left = p->ctl_l[AUDIO_CTL_PCM];
	s->pcm_right = p->ctl_r[AUDIO_CTL_PCM];
	s->capture_left = p->ctl_l[AUDIO_CTL_CAPTURE];
	s->capture_right = p->ctl_r[AUDIO_CTL_CAPTURE];
}

int hda_codec_set_volume(struct hda_codec *c, uint32_t ctl, uint32_t l,
			 uint32_t r)
{
	struct hda_codec_priv *p;

	if (!c || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	p = hda_priv(c);
	if (hda_is_display(c) || !p->generic_parsed || !ctl_exists(c, ctl))
		return -ENOENT;
	if (l > 100)
		l = 100;
	if (r > 100)
		r = 100;
	hda_codec_lock(c);
	p->ctl_l[ctl] = l;
	p->ctl_r[ctl] = r;
	mirror_spec(c);
	hda_generic_apply_amps(c);
	hda_codec_unlock(c);
	return 0;
}

int hda_codec_get_mute(struct hda_codec *c, uint32_t ctl, uint32_t *mute)
{
	struct hda_codec_priv *p;

	if (!c || !mute || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	p = hda_priv(c);
	if (hda_is_display(c))
		return ctl == AUDIO_CTL_HDMI ? hda_hdmi_get_mute(c, mute) :
					       -ENOENT;
	if (!p->generic_parsed || !ctl_exists(c, ctl) ||
	    ctl == AUDIO_CTL_MIC_BOOST)
		return -ENOENT;
	*mute = p->ctl_mute[ctl] ? 1 : 0;
	return 0;
}

int hda_codec_set_mute(struct hda_codec *c, uint32_t ctl, uint32_t mute)
{
	struct hda_codec_priv *p;

	if (!c || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	p = hda_priv(c);
	if (hda_is_display(c))
		return ctl == AUDIO_CTL_HDMI ? hda_hdmi_set_mute(c, mute ? 1 : 0) :
					       -ENOENT;
	if (!p->generic_parsed || !ctl_exists(c, ctl) ||
	    ctl == AUDIO_CTL_MIC_BOOST)
		return -ENOENT;
	hda_codec_lock(c);
	p->ctl_mute[ctl] = mute ? 1 : 0;
	mirror_spec(c);
	hda_codec_unlock(c);
	/* The routing pass applies the amps and lets the vendor driver
	 * follow at once (mute LEDs, speaker amps on a GPIO). */
	hda_codec_update_routing(c);
	return 0;
}

int hda_codec_get_outputs(struct hda_codec *c, struct audio_outputs *o)
{
	struct hda_codec_priv *p;
	struct hda_spec *s;
	uint32_t n = 0;

	if (!c || !o)
		return -EINVAL;
	p = hda_priv(c);
	s = &c->spec;
	kmemset(o, 0, sizeof(*o));
	if (hda_is_display(c)) {
		/* one output per HDMI/DP PCM device of the display codec */
		int k = hda_hdmi_get_outputs(c, o->out, AUDIO_MAX_OUTPUTS);

		o->count = k > 0 ? (uint32_t)k : 0;
		if (o->count > AUDIO_MAX_OUTPUTS)
			o->count = AUDIO_MAX_OUTPUTS;
		o->mode = s->outsel_mode;
		o->selected = s->outsel_id;
		return 0;
	}
	if (!p->generic_parsed)
		return 0;
	hda_codec_lock(c);
	o->mode = s->outsel_mode;
	o->selected = s->outsel_id;
	for (int i = 0; i < s->nout && n < AUDIO_MAX_OUTPUTS; i++, n++) {
		const struct hda_out *src = &s->out[i];
		struct audio_output_desc *d = &o->out[n];

		d->id = src->id;
		d->type = src->type;
		d->flags = 0;
		if (src->jack_detect)
			d->flags |= AUDIO_OUTF_JACK_DETECT;
		if (src->dock)
			d->flags |= AUDIO_OUTF_DOCK;
		if (!src->jack_detect) {
			struct hda_widget *w = hda_widget(c, src->pin);

			if (w && hda_cfg_port(w->pin_cfg) == AC_JACK_PORT_FIXED)
				d->flags |= AUDIO_OUTF_FIXED;
		}
		d->present = src->jack_detect ? src->present : 1;
		d->active = p->out_active[i];
		hda_strlcpy(d->name, src->name, sizeof(d->name));
	}
	o->count = n;
	hda_codec_unlock(c);
	return 0;
}

int hda_codec_set_output(struct hda_codec *c, const struct audio_setoutput *sel)
{
	struct hda_codec_priv *p;
	struct hda_spec *s;

	if (!c || !sel)
		return -EINVAL;
	p = hda_priv(c);
	s = &c->spec;
	if (hda_is_display(c)) {
		/* A display codec plays through its own PCM devices; the
		 * choice is only recorded (audiod opens the HDMI device). */
		if (sel->mode == AUDIO_OUTSEL_ID) {
			struct audio_output_desc d[HDA_MAX_HDMI_PINS];
			int k = hda_hdmi_get_outputs(c, d, HDA_MAX_HDMI_PINS);
			int found = 0;

			for (int i = 0; i < k && i < HDA_MAX_HDMI_PINS; i++)
				if (d[i].id == sel->id)
					found = 1;
			if (!found)
				return -ENOENT;
		} else if (sel->mode != AUDIO_OUTSEL_AUTO &&
			   sel->mode != AUDIO_OUTSEL_ALL) {
			return -EINVAL;
		}
		s->outsel_mode = sel->mode;
		s->outsel_id = sel->mode == AUDIO_OUTSEL_ID ? sel->id : 0;
		return 0;
	}
	if (!p->generic_parsed)
		return -ENOENT;
	switch (sel->mode) {
	case AUDIO_OUTSEL_AUTO:
	case AUDIO_OUTSEL_ALL:
		break;
	case AUDIO_OUTSEL_ID: {
		int found = 0;

		for (int i = 0; i < s->nout; i++)
			if (s->out[i].id == sel->id)
				found = 1;
		if (!found)
			return -ENOENT;
		break;
	}
	default:
		return -EINVAL;
	}
	hda_codec_lock(c);
	s->outsel_mode = sel->mode;
	s->outsel_id = sel->mode == AUDIO_OUTSEL_ID ? sel->id : 0;
	hda_codec_update_routing(c);
	hda_codec_unlock(c);
	return 0;
}

static uint32_t in_desc_type(const struct hda_in *in)
{
	switch (in->type) {
	case AC_JACK_MIC_IN:
		return AUDIO_OUT_HEADSET;
	case AC_JACK_SPDIF_IN:
		return AUDIO_OUT_SPDIF;
	default:
		return AUDIO_OUT_LINE;
	}
}

int hda_codec_get_inputs(struct hda_codec *c, struct audio_outputs *o)
{
	struct hda_codec_priv *p;
	struct hda_spec *s;
	uint32_t n = 0;

	if (!c || !o)
		return -EINVAL;
	p = hda_priv(c);
	s = &c->spec;
	kmemset(o, 0, sizeof(*o));
	if (hda_is_display(c) || !p->generic_parsed)
		return 0;
	hda_codec_lock(c);
	o->mode = p->in_mode;
	o->selected = p->in_sel_id;
	for (int i = 0; i < s->nin && n < AUDIO_MAX_OUTPUTS; i++, n++) {
		const struct hda_in *src = &s->in[i];
		struct audio_output_desc *d = &o->out[n];

		d->id = src->id;
		d->type = in_desc_type(src);
		if (src->jack_detect)
			d->flags |= AUDIO_OUTF_JACK_DETECT;
		if (src->internal)
			d->flags |= AUDIO_OUTF_FIXED;
		if (kstrncmp(src->name, "Dock", 4) == 0)
			d->flags |= AUDIO_OUTF_DOCK;
		d->present = src->jack_detect ? src->present : 1;
		d->active = (int)i == p->in_active;
		hda_strlcpy(d->name, src->name, sizeof(d->name));
	}
	o->count = n;
	hda_codec_unlock(c);
	return 0;
}

int hda_codec_set_input(struct hda_codec *c, const struct audio_setoutput *sel)
{
	struct hda_codec_priv *p;
	struct hda_spec *s;

	if (!c || !sel)
		return -EINVAL;
	p = hda_priv(c);
	s = &c->spec;
	if (hda_is_display(c) || !p->generic_parsed || !s->nin)
		return -ENOENT;
	if (sel->mode == AUDIO_OUTSEL_ID) {
		int found = 0;

		for (int i = 0; i < s->nin; i++)
			if (s->in[i].id == sel->id)
				found = 1;
		if (!found)
			return -ENOENT;
	} else if (sel->mode != AUDIO_OUTSEL_AUTO) {
		/* recording from every input at once is not a thing */
		return -EINVAL;
	}
	hda_codec_lock(c);
	p->in_mode = sel->mode;
	p->in_sel_id = sel->mode == AUDIO_OUTSEL_ID ? sel->id : 0;
	select_input(c);
	hda_generic_apply_amps(c);
	hda_codec_unlock(c);
	return 0;
}
