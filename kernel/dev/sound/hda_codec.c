// LikeOS Intel High Definition Audio - codec core: verb helpers, codec and
// widget enumeration, vendor driver selection, jack events, resume, and the
// fixup-table helpers vendor drivers use.
//
// A codec on the HDA link is a tree of nodes.  Node 0 (the root) names the
// vendor and device and lists the function groups; the audio function group
// (AFG) in turn lists the widgets: converters (DAC/ADC), mixers, selectors,
// pin complexes, the beep generator and vendor widgets.  Probing reads all
// of that once into struct hda_codec, then lets a vendor driver correct the
// BIOS pin configuration before the generic parser (hda_generic.c) works out
// which DAC feeds which pin.
//
// Locking.  hda_cmd() busy-waits for the response before the scheduler runs
// and may sleep afterwards, so no spinlock is ever held across a verb.  The
// codec spinlock `c->lock' only protects the fields of a small recursive
// "sequence lock" (hda_codec_lock/unlock) that serialises multi-verb
// sequences of one codec: a COEF index/value pair, an amp read-modify-write,
// a routing update.  Interrupt handlers never send codec verbs; unsolicited
// responses and jack polling run in the sound work thread.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda_codec_priv.h"
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/sched.h>
#include <kernel/io/console.h>

/* How long the AFG gets to reach D0 (it may need to restart clocks). */
#define HDA_POWER_WAIT_MS 300

/* ======================================================================== */
/* sequence lock                                                            */
/* ======================================================================== */

static void *hda_lock_token(void)
{
	void *me = (void *)sched_current();

	/* Before the scheduler there is exactly one context: use a fixed,
	 * non-NULL token so recursion still works. */
	return me ? me : (void *)(uintptr_t)1;
}

void hda_codec_lock(struct hda_codec *c)
{
	struct hda_codec_priv *p = hda_priv(c);
	void *me = hda_lock_token();

	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&c->lock, &fl);
		if (!p->seq_busy) {
			p->seq_busy = 1;
			p->seq_owner = me;
			p->seq_depth = 1;
			spin_unlock_irqrestore(&c->lock, fl);
			return;
		}
		if (p->seq_owner == me) {
			p->seq_depth++;
			spin_unlock_irqrestore(&c->lock, fl);
			return;
		}
		spin_unlock_irqrestore(&c->lock, fl);
		/* The holder may be asleep waiting for a codec response:
		 * give it the CPU when we can, otherwise just spin. */
		if (irqs_enabled() && sched_current())
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause" ::: "memory");
	}
}

void hda_codec_unlock(struct hda_codec *c)
{
	struct hda_codec_priv *p = hda_priv(c);
	uint64_t fl;

	spin_lock_irqsave(&c->lock, &fl);
	if (p->seq_busy && p->seq_depth > 0) {
		if (--p->seq_depth == 0) {
			p->seq_owner = 0;
			p->seq_busy = 0;
		}
	}
	spin_unlock_irqrestore(&c->lock, fl);
}

/* ======================================================================== */
/* verb helpers                                                             */
/* ======================================================================== */

/* A 12-bit verb (0x7xx set, 0xfxx get) carries an 8-bit payload; the 4-bit
 * ones (0x2-0x5, 0xa-0xd) a 16-bit payload.  Mask so a bad payload can never
 * spill into the verb field. */
static uint32_t hda_payload_mask(uint32_t verb)
{
	uint32_t hi = (verb >> 8) & 0xf;

	if (hi == 0x7 || hi == 0xf)
		return 0xff;
	return 0xffff;
}

uint32_t hda_codec_read(struct hda_codec *c, uint8_t nid, uint32_t verb,
			uint32_t payload)
{
	uint32_t res = 0;

	if (!c || !c->ctrl)
		return 0xffffffffu;
	payload &= hda_payload_mask(verb);
	if (hda_cmd(c->ctrl, HDA_CMD(c->addr, nid, verb, payload), &res) < 0)
		return 0xffffffffu;
	return res;
}

int hda_codec_write(struct hda_codec *c, uint8_t nid, uint32_t verb,
		    uint32_t payload)
{
	uint32_t res = 0;

	if (!c || !c->ctrl)
		return -ENODEV;
	payload &= hda_payload_mask(verb);
	return hda_cmd(c->ctrl, HDA_CMD(c->addr, nid, verb, payload), &res);
}

uint32_t hda_param(struct hda_codec *c, uint8_t nid, uint32_t param)
{
	return hda_codec_read(c, nid, AC_VERB_PARAMETERS, param & 0xff);
}

/* A parameter read where "no response" means "not supported". */
static uint32_t hda_param0(struct hda_codec *c, uint8_t nid, uint32_t param)
{
	uint32_t v = hda_param(c, nid, param);

	return v == 0xffffffffu ? 0 : v;
}

struct hda_widget *hda_widget(struct hda_codec *c, uint8_t nid)
{
	if (!c || !c->w)
		return 0;
	if (nid < c->start_nid ||
	    (uint32_t)nid >= (uint32_t)c->start_nid + c->num_nodes)
		return 0;
	return &c->w[nid - c->start_nid];
}

uint32_t hda_widget_type(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);

	return w ? w->type : 0xffffffffu;
}

uint32_t hda_widget_channels(const struct hda_widget *w)
{
	uint32_t ext;

	if (!w)
		return 2;
	ext = (w->caps & AC_WCAP_CHAN_CNT_EXT) >> 13;
	return ((ext << 1) | (w->caps & AC_WCAP_STEREO)) + 1;
}

uint32_t hda_conv_pcm(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (w && w->pcm)
		return w->pcm;
	return c->afg_pcm;
}

uint32_t hda_conv_stream_fmts(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (w && w->stream_fmts)
		return w->stream_fmts;
	return c->afg_stream_fmts;
}

int hda_conn_index(struct hda_codec *c, uint8_t from, uint8_t to)
{
	struct hda_widget *w = hda_widget(c, from);

	if (!w)
		return -1;
	for (int i = 0; i < w->nconn; i++)
		if (w->conn[i] == to)
			return i;
	return -1;
}

uint32_t hda_amp_caps(struct hda_codec *c, uint8_t nid, int dir)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w) {
		if (c && nid == c->afg && nid)
			return dir == HDA_OUTPUT ? c->afg_amp_out_caps :
						   c->afg_amp_in_caps;
		return 0;
	}
	if (dir == HDA_OUTPUT)
		return (w->caps & AC_WCAP_OUT_AMP) ? w->amp_out_caps : 0;
	return (w->caps & AC_WCAP_IN_AMP) ? w->amp_in_caps : 0;
}

static int hda_has_amp(struct hda_widget *w, int dir)
{
	if (!w)
		return 0;
	return (w->caps & (dir == HDA_OUTPUT ? AC_WCAP_OUT_AMP :
					       AC_WCAP_IN_AMP)) != 0;
}

void hda_amp_set(struct hda_codec *c, uint8_t nid, int dir, int idx,
		 uint32_t gain_l, uint32_t gain_r, int mute)
{
	struct hda_widget *w = hda_widget(c, nid);
	uint32_t caps, nsteps, base, mbit = 0;

	if (!w || !hda_has_amp(w, dir))
		return;
	caps = hda_amp_caps(c, nid, dir);
	nsteps = hda_amp_nsteps(caps);
	if (gain_l > nsteps)
		gain_l = nsteps;
	if (gain_r > nsteps)
		gain_r = nsteps;
	if (mute) {
		/* An amp without a mute switch is at least turned all the
		 * way down; the bit itself is ignored by such amps. */
		mbit = AC_AMP_MUTE;
		if (!(caps & AC_AMPCAP_MUTE))
			gain_l = gain_r = 0;
	}
	base = (dir == HDA_OUTPUT ? AC_AMP_SET_OUTPUT : AC_AMP_SET_INPUT) |
	       (((uint32_t)idx << AC_AMP_SET_INDEX_SHIFT) & AC_AMP_SET_INDEX);
	if (gain_l == gain_r || !(w->caps & AC_WCAP_STEREO)) {
		hda_codec_write(c, nid, AC_VERB_SET_AMP_GAIN_MUTE,
				base | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT |
					mbit | (gain_l & AC_AMP_GAIN));
	} else {
		hda_codec_write(c, nid, AC_VERB_SET_AMP_GAIN_MUTE,
				base | AC_AMP_SET_LEFT | mbit |
					(gain_l & AC_AMP_GAIN));
		hda_codec_write(c, nid, AC_VERB_SET_AMP_GAIN_MUTE,
				base | AC_AMP_SET_RIGHT | mbit |
					(gain_r & AC_AMP_GAIN));
	}
}

uint32_t hda_amp_get(struct hda_codec *c, uint8_t nid, int dir, int idx,
		     int left)
{
	uint32_t pl, r;

	pl = (dir == HDA_OUTPUT ? AC_AMP_GET_OUTPUT : AC_AMP_GET_INPUT) |
	     (left ? AC_AMP_GET_LEFT : AC_AMP_GET_RIGHT) | ((uint32_t)idx & 0xf);
	r = hda_codec_read(c, nid, AC_VERB_GET_AMP_GAIN_MUTE, pl);
	if (r == 0xffffffffu)
		return r;
	return r & 0xff;
}

/* ---- processing coefficients -------------------------------------------- */

uint32_t hda_coef_read(struct hda_codec *c, uint8_t nid, uint16_t idx)
{
	uint32_t r;

	hda_codec_lock(c);
	if (hda_codec_write(c, nid, AC_VERB_SET_COEF_INDEX, idx) < 0) {
		hda_codec_unlock(c);
		return 0xffffffffu;
	}
	r = hda_codec_read(c, nid, AC_VERB_GET_PROC_COEF, 0);
	hda_codec_unlock(c);
	if (r == 0xffffffffu)
		return r;
	return r & 0xffff;
}

void hda_coef_write(struct hda_codec *c, uint8_t nid, uint16_t idx,
		    uint16_t val)
{
	hda_codec_lock(c);
	if (hda_codec_write(c, nid, AC_VERB_SET_COEF_INDEX, idx) == 0)
		hda_codec_write(c, nid, AC_VERB_SET_PROC_COEF, val);
	if (idx < 64)
		c->coef_cache[idx] = val;
	hda_codec_unlock(c);
}

void hda_coef_update(struct hda_codec *c, uint8_t nid, uint16_t idx,
		     uint16_t mask, uint16_t val)
{
	uint32_t old, nv;

	hda_codec_lock(c);
	old = hda_coef_read(c, nid, idx);
	if (old == 0xffffffffu) {
		/* Never write back a value we failed to read. */
		hda_codec_unlock(c);
		return;
	}
	nv = (old & ~(uint32_t)mask) | ((uint32_t)val & mask);
	if (nv != old)
		hda_coef_write(c, nid, idx, (uint16_t)nv);
	else if (idx < 64)
		c->coef_cache[idx] = nv;
	hda_codec_unlock(c);
}

/* ---- pins ----------------------------------------------------------------- */

static int hda_pin_is_digital(struct hda_widget *w)
{
	return (w->caps & AC_WCAP_DIGITAL) ||
	       (w->pincap & (AC_PINCAP_HDMI | AC_PINCAP_DP));
}

/* The nearest VREF level at or below `want' that the pin supports. */
static uint32_t hda_pin_fix_vref(uint32_t pincap, uint32_t want)
{
	static const uint8_t order[] = { AC_PINCTL_VREF_100,
					 AC_PINCTL_VREF_80,
					 AC_PINCTL_VREF_50,
					 AC_PINCTL_VREF_GRD,
					 AC_PINCTL_VREF_HIZ };
	uint32_t vcaps = (pincap & AC_PINCAP_VREF) >> AC_PINCAP_VREF_SHIFT;
	int start = -1;

	if (want == AC_PINCTL_VREF_HIZ)
		return want;
	if (vcaps & (1u << want))
		return want;
	for (unsigned i = 0; i < sizeof(order); i++) {
		if (order[i] == want) {
			start = (int)i;
			break;
		}
	}
	if (start < 0)
		return AC_PINCTL_VREF_HIZ;
	for (unsigned i = (unsigned)start + 1; i < sizeof(order); i++)
		if (vcaps & (1u << order[i]))
			return order[i];
	return AC_PINCTL_VREF_HIZ;
}

void hda_pin_set_ctl(struct hda_codec *c, uint8_t nid, uint8_t ctl)
{
	struct hda_widget *w = hda_widget(c, nid);
	uint32_t v = ctl;

	if (!w || w->type != AC_WID_PIN)
		return;
	if (!(w->pincap & AC_PINCAP_OUT))
		v &= ~(uint32_t)(AC_PINCTL_OUT_EN | AC_PINCTL_HP_EN);
	if (!(w->pincap & AC_PINCAP_HP_DRV))
		v &= ~(uint32_t)AC_PINCTL_HP_EN;
	if (!(w->pincap & AC_PINCAP_IN))
		v &= ~(uint32_t)AC_PINCTL_IN_EN;
	/* On analog pins the low bits are the VREF level; digital pins use
	 * them as the encoded packet type and are passed through. */
	if (!hda_pin_is_digital(w)) {
		uint32_t vref = v & AC_PINCTL_VREFEN;

		if (vref)
			vref = hda_pin_fix_vref(w->pincap, vref);
		v = (v & ~(uint32_t)AC_PINCTL_VREFEN) | vref;
	}
	if (hda_codec_write(c, nid, AC_VERB_SET_PIN_WIDGET_CONTROL, v) == 0)
		w->pinctl = (uint8_t)v;
}

int hda_pin_sense(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);
	uint32_t r;

	if (!w || w->type != AC_WID_PIN)
		return 0;
	/* Pins that need a trigger only measure after SET_PIN_SENSE. */
	if (w->pincap & AC_PINCAP_TRIG_REQ) {
		hda_codec_write(c, nid, AC_VERB_SET_PIN_SENSE, 0);
		snd_udelay(50);
	}
	r = hda_codec_read(c, nid, AC_VERB_GET_PIN_SENSE, 0);
	if (r == 0xffffffffu)
		return 0;
	return (r & AC_PINSENSE_PRESENCE) ? 1 : 0;
}

int hda_pin_has_presence(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);
	uint32_t port;

	if (!w || w->type != AC_WID_PIN)
		return 0;
	if (!(w->pincap & AC_PINCAP_PRES_DETECT))
		return 0;
	if (hda_cfg_misc(w->pin_cfg) & AC_DEFCFG_MISC_NO_PRESENCE)
		return 0;
	port = hda_cfg_port(w->pin_cfg);
	return port == AC_JACK_PORT_COMPLEX || port == AC_JACK_PORT_BOTH;
}

static void hda_write_pin_cfg(struct hda_codec *c, uint8_t nid, uint32_t cfg)
{
	hda_codec_write(c, nid, AC_VERB_SET_CONFIG_DEFAULT_BYTES_0,
			cfg & 0xff);
	hda_codec_write(c, nid, AC_VERB_SET_CONFIG_DEFAULT_BYTES_1,
			(cfg >> 8) & 0xff);
	hda_codec_write(c, nid, AC_VERB_SET_CONFIG_DEFAULT_BYTES_2,
			(cfg >> 16) & 0xff);
	hda_codec_write(c, nid, AC_VERB_SET_CONFIG_DEFAULT_BYTES_3,
			(cfg >> 24) & 0xff);
}

void hda_pin_override_cfg(struct hda_codec *c, uint8_t nid, uint32_t cfg)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w || w->type != AC_WID_PIN)
		return;
	w->pin_cfg = cfg;
	hda_write_pin_cfg(c, nid, cfg);
}

void hda_set_eapd(struct hda_codec *c, uint8_t nid, int on)
{
	struct hda_widget *w = hda_widget(c, nid);
	uint32_t r;

	if (!w)
		return;
	r = hda_codec_read(c, nid, AC_VERB_GET_EAPD_BTLENABLE, 0);
	if (r == 0xffffffffu)
		r = 0;
	r &= 0xff;
	if (on)
		r |= AC_EAPDBTL_EAPD;
	else
		r &= ~(uint32_t)AC_EAPDBTL_EAPD;
	hda_codec_write(c, nid, AC_VERB_SET_EAPD_BTLENABLE, r);
}

/* Wait until a function group or widget reports the wanted actual state. */
static int hda_wait_power(struct hda_codec *c, uint8_t nid, int state,
			  uint32_t ms)
{
	for (uint32_t i = 0; i <= ms; i++) {
		uint32_t st = hda_codec_read(c, nid, AC_VERB_GET_POWER_STATE, 0);

		if (st != 0xffffffffu && !(st & AC_PWRST_ERROR) &&
		    (int)((st & AC_PWRST_ACTUAL) >> AC_PWRST_ACTUAL_SHIFT) ==
			    state)
			return 0;
		snd_mdelay(1);
	}
	return -ETIMEDOUT;
}

void hda_set_power(struct hda_codec *c, uint8_t nid, int state)
{
	struct hda_widget *w;

	if (!c || state < AC_PWRST_D0 || state > AC_PWRST_D3)
		return;
	if (nid && (nid == c->afg || nid == c->mfg)) {
		hda_codec_write(c, nid, AC_VERB_SET_POWER_STATE, state);
		if (hda_wait_power(c, nid, state, HDA_POWER_WAIT_MS) < 0)
			snd_info(HDA_LOG,
				 "codec#%d: node 0x%02x did not reach D%d\n",
				 c->addr, nid, state);
		return;
	}
	w = hda_widget(c, nid);
	if (!w || !(w->caps & AC_WCAP_POWER))
		return;
	hda_codec_write(c, nid, AC_VERB_SET_POWER_STATE, state);
}

void hda_gpio_set(struct hda_codec *c, uint32_t mask, uint32_t dir,
		  uint32_t data)
{
	uint32_t count;

	if (!c || !c->afg)
		return;
	count = c->gpio_caps & AC_GPIO_IO_COUNT;
	if (count && count < 8) {
		uint32_t valid = (1u << count) - 1;

		mask &= valid;
	}
	mask &= 0xff;
	hda_codec_lock(c);
	c->gpio_mask |= mask;
	c->gpio_dir = (c->gpio_dir & ~mask) | (dir & mask);
	c->gpio_data = (c->gpio_data & ~mask) | (data & mask);
	hda_codec_write(c, c->afg, AC_VERB_SET_GPIO_MASK, c->gpio_mask);
	hda_codec_write(c, c->afg, AC_VERB_SET_GPIO_DIRECTION, c->gpio_dir);
	/* Some amplifiers latch the data line only after the direction
	 * settles; a short pause avoids a lost first write. */
	snd_udelay(10);
	hda_codec_write(c, c->afg, AC_VERB_SET_GPIO_DATA, c->gpio_data);
	hda_codec_unlock(c);
}

/* ======================================================================== */
/* enumeration                                                              */
/* ======================================================================== */

static void hda_conn_add(struct hda_widget *w, uint32_t nid, int *dropped)
{
	if (nid == 0 || nid > 0xff) {
		(*dropped)++;
		return;
	}
	if (w->nconn >= HDA_MAX_CONNS) {
		(*dropped)++;
		return;
	}
	w->conn[w->nconn++] = (uint8_t)nid;
}

/* Read the connection list of a widget.  Short form packs four 8-bit
 * entries per response, long form two 16-bit ones.  An entry with the
 * range bit set means "every nid from the previous entry + 1 up to this
 * one"; the connection select index counts the expanded list. */
static void hda_read_conn_list(struct hda_codec *c, struct hda_widget *w)
{
	uint32_t len, total, per, bits, nidmask, rangebit, resp = 0;
	uint32_t prev = 0;
	int dropped = 0;

	w->nconn = 0;
	if (!(w->caps & AC_WCAP_CONN_LIST))
		return;
	len = hda_param0(c, w->nid, AC_PAR_CONNLIST_LEN);
	total = len & AC_CLIST_LENGTH;
	if (!total)
		return;
	if (len & AC_CLIST_LONG) {
		per = 2;
		bits = 16;
		nidmask = 0x7fff;
		rangebit = 0x8000;
	} else {
		per = 4;
		bits = 8;
		nidmask = 0x7f;
		rangebit = 0x80;
	}
	for (uint32_t i = 0; i < total; i++) {
		uint32_t v, nid;

		if (i % per == 0) {
			resp = hda_codec_read(c, w->nid,
					      AC_VERB_GET_CONNECT_LIST, i);
			if (resp == 0xffffffffu)
				break;
		}
		v = (resp >> ((i % per) * bits)) & ((1u << bits) - 1);
		nid = v & nidmask;
		if (v & rangebit) {
			/* A range must follow a plain entry and go up;
			 * anything else is a broken codec response. */
			if (!prev || nid <= prev || nid - prev > 0x80) {
				dropped++;
				continue;
			}
			for (uint32_t n = prev + 1; n <= nid; n++)
				hda_conn_add(w, n, &dropped);
		} else {
			hda_conn_add(w, nid, &dropped);
		}
		prev = nid;
	}
	if (dropped)
		snd_info(HDA_LOG,
			 "codec#%d: node 0x%02x: %d connection entries "
			 "ignored\n",
			 c->addr, w->nid, dropped);
}

static int hda_enumerate_widgets(struct hda_codec *c)
{
	uint32_t sub, start, num;
	int npins = 0, ndac = 0, nadc = 0, nmix = 0, nsel = 0;

	sub = hda_param(c, c->afg, AC_PAR_NODE_COUNT);
	if (sub == 0xffffffffu)
		return -EIO;
	start = (sub >> 16) & 0xff;
	num = sub & 0xff;
	if (!start || !num)
		return -ENODEV;
	if (start + num > HDA_MAX_NODES)
		num = HDA_MAX_NODES - start;
	c->start_nid = (uint8_t)start;
	c->num_nodes = (uint8_t)num;
	c->w = kcalloc(num, sizeof(struct hda_widget));
	if (!c->w)
		return -ENOMEM;

	for (uint32_t i = 0; i < num; i++) {
		struct hda_widget *w = &c->w[i];
		uint8_t nid = (uint8_t)(start + i);
		uint32_t caps;

		w->nid = nid;
		caps = hda_param(c, nid, AC_PAR_AUDIO_WIDGET_CAP);
		if (caps == 0xffffffffu) {
			/* A node that does not answer: keep it out of every
			 * path (type 0 would make it look like a DAC). */
			w->caps = 0;
			w->type = 0xe;
			continue;
		}
		w->caps = caps;
		w->type = (uint8_t)((caps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT);

		if (caps & AC_WCAP_IN_AMP) {
			uint32_t a = 0;

			if (caps & AC_WCAP_AMP_OVRD)
				a = hda_param0(c, nid, AC_PAR_AMP_IN_CAP);
			w->amp_in_caps = a ? a : c->afg_amp_in_caps;
		}
		if (caps & AC_WCAP_OUT_AMP) {
			uint32_t a = 0;

			if (caps & AC_WCAP_AMP_OVRD)
				a = hda_param0(c, nid, AC_PAR_AMP_OUT_CAP);
			w->amp_out_caps = a ? a : c->afg_amp_out_caps;
		}

		switch (w->type) {
		case AC_WID_AUD_OUT:
		case AC_WID_AUD_IN:
			if (w->type == AC_WID_AUD_OUT)
				ndac++;
			else
				nadc++;
			if ((caps & AC_WCAP_FORMAT_OVRD) || !c->afg_pcm) {
				w->pcm = hda_param0(c, nid, AC_PAR_PCM);
				w->stream_fmts = hda_param0(c, nid,
							    AC_PAR_STREAM);
			}
			break;
		case AC_WID_PIN: {
			uint32_t cfg, ctl;

			npins++;
			w->pincap = hda_param0(c, nid, AC_PAR_PIN_CAP);
			cfg = hda_codec_read(c, nid, AC_VERB_GET_CONFIG_DEFAULT,
					     0);
			if (cfg == 0xffffffffu)
				cfg = (uint32_t)AC_JACK_PORT_NONE
				      << AC_DEFCFG_PORT_CONN_SHIFT;
			w->pin_cfg = cfg;
			w->pin_cfg_orig = cfg;
			ctl = hda_codec_read(c, nid,
					     AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
			w->pinctl = ctl == 0xffffffffu ? 0 : (uint8_t)ctl;
			w->eapd = (w->pincap & AC_PINCAP_EAPD) ? 1 : 0;
			break;
		}
		case AC_WID_AUD_MIX:
			nmix++;
			break;
		case AC_WID_AUD_SEL:
			nsel++;
			break;
		case AC_WID_BEEP:
			if (!c->spec.beep_nid)
				c->spec.beep_nid = nid;
			break;
		default:
			break;
		}

		if (caps & AC_WCAP_POWER)
			w->power_caps = hda_param0(c, nid, AC_PAR_POWER_STATE);

		hda_read_conn_list(c, w);

		w->cur_conn = 0;
		if (w->nconn > 1 && w->type != AC_WID_AUD_MIX &&
		    w->type != AC_WID_POWER && w->type != AC_WID_VOL_KNB) {
			uint32_t s = hda_codec_read(c, nid,
						    AC_VERB_GET_CONNECT_SEL, 0);

			if (s != 0xffffffffu && (s & 0xff) < w->nconn)
				w->cur_conn = (uint8_t)(s & 0xff);
		}
	}

	snd_info(HDA_LOG,
		 "codec#%d: %u widgets from 0x%02x: %d DAC, %d ADC, %d mixer, "
		 "%d selector, %d pin\n",
		 c->addr, num, start, ndac, nadc, nmix, nsel, npins);
	return 0;
}

/* Read the root and function group description. */
static int hda_read_root(struct hda_codec *c)
{
	uint32_t sub, start, num;

	c->vendor_id = hda_param(c, 0, AC_PAR_VENDOR_ID);
	if (c->vendor_id == 0xffffffffu || c->vendor_id == 0 ||
	    (c->vendor_id >> 16) == 0xffff || (c->vendor_id >> 16) == 0)
		return -ENODEV;
	c->revision_id = hda_param0(c, 0, AC_PAR_REV_ID);

	sub = hda_param(c, 0, AC_PAR_NODE_COUNT);
	if (sub == 0xffffffffu)
		return -EIO;
	start = (sub >> 16) & 0xff;
	num = sub & 0xff;
	if (num > 16)
		num = 16; /* a root lists a handful of groups at most */
	for (uint32_t i = 0; i < num && start + i < 256; i++) {
		uint8_t nid = (uint8_t)(start + i);
		uint32_t ft = hda_param(c, nid, AC_PAR_FUNCTION_TYPE);

		if (ft == 0xffffffffu)
			continue;
		switch (ft & 0xff) {
		case AC_GRP_AUDIO_FUNCTION:
			if (!c->afg) {
				c->afg = nid;
				c->unsol_capable = (ft & AC_FGT_UNSOL_CAP) ? 1 :
									    0;
			}
			break;
		case AC_GRP_MODEM_FUNCTION:
			if (!c->mfg)
				c->mfg = nid;
			break;
		default:
			break;
		}
	}
	return 0;
}

static void hda_read_afg(struct hda_codec *c)
{
	uint8_t afg = c->afg;
	uint32_t ss;

	c->afg_caps = hda_param0(c, afg, AC_PAR_AUDIO_FG_CAP);
	c->afg_pcm = hda_param0(c, afg, AC_PAR_PCM);
	c->afg_stream_fmts = hda_param0(c, afg, AC_PAR_STREAM);
	c->afg_amp_in_caps = hda_param0(c, afg, AC_PAR_AMP_IN_CAP);
	c->afg_amp_out_caps = hda_param0(c, afg, AC_PAR_AMP_OUT_CAP);
	c->gpio_caps = hda_param0(c, afg, AC_PAR_GPIO_CAP);
	ss = hda_codec_read(c, afg, AC_VERB_GET_SUBSYSTEM_ID, 0);
	c->subsystem_id = ss == 0xffffffffu ? 0 : ss;

	if (c->gpio_caps & AC_GPIO_IO_COUNT) {
		uint32_t v;

		v = hda_codec_read(c, afg, AC_VERB_GET_GPIO_DATA, 0);
		c->gpio_data = v == 0xffffffffu ? 0 : v & 0xff;
		v = hda_codec_read(c, afg, AC_VERB_GET_GPIO_MASK, 0);
		c->gpio_mask = v == 0xffffffffu ? 0 : v & 0xff;
		v = hda_codec_read(c, afg, AC_VERB_GET_GPIO_DIRECTION, 0);
		c->gpio_dir = v == 0xffffffffu ? 0 : v & 0xff;
	}
}

/* ======================================================================== */
/* vendor drivers                                                           */
/* ======================================================================== */

/* Most specific first: the virtual-machine driver recognises emulated
 * codecs that would otherwise match their real vendor's driver, and the
 * display driver claims HDMI/DP function groups before an analog driver of
 * the same vendor sees them. */
static const struct hda_vendor_driver *const hda_vendor_drivers[] = {
	&hda_vm_driver,	      &hda_hdmi_driver,	   &hda_realtek_driver,
	&hda_conexant_driver, &hda_idt_driver,	   &hda_analog_driver,
	&hda_cirrus_driver,   &hda_via_driver,	   &hda_ca0132_driver,
};

/* A codec whose every usable pin is HDMI/DP is a display codec even when
 * no vendor driver claims it explicitly. */
static int hda_looks_like_display_codec(struct hda_codec *c)
{
	int digital = 0, analog = 0;

	for (uint32_t i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_PIN)
			continue;
		if (hda_cfg_port(w->pin_cfg) == AC_JACK_PORT_NONE)
			continue;
		if (w->pincap & (AC_PINCAP_HDMI | AC_PINCAP_DP))
			digital++;
		else
			analog++;
	}
	return digital > 0 && analog == 0;
}

static int hda_attach_driver(struct hda_codec *c)
{
	const struct hda_vendor_driver *drv = 0;
	int ret;

	for (unsigned i = 0;
	     i < sizeof(hda_vendor_drivers) / sizeof(hda_vendor_drivers[0]);
	     i++) {
		const struct hda_vendor_driver *d = hda_vendor_drivers[i];

		if (d && d->match && d->match(c)) {
			drv = d;
			break;
		}
	}
	if (!drv && hda_looks_like_display_codec(c) && hda_hdmi_driver.probe)
		drv = &hda_hdmi_driver;
	c->vendor = drv;
	if (drv)
		snd_info(HDA_LOG, "codec#%d: using the %s driver\n", c->addr,
			 drv->name ? drv->name : "vendor");

	/* Drivers that handle the codec entirely on their own. */
	if (drv && drv->probe) {
		if (drv == &hda_hdmi_driver)
			c->is_hdmi = 1;
		ret = drv->probe(c);
		if (ret < 0)
			snd_info(HDA_LOG, "codec#%d: %s probe failed (%d)\n",
				 c->addr, drv->name ? drv->name : "vendor",
				 ret);
		return ret;
	}

	if (drv && drv->pre_parse) {
		ret = drv->pre_parse(c);
		if (ret < 0)
			snd_info(HDA_LOG,
				 "codec#%d: %s pre-parse failed (%d), "
				 "continuing\n",
				 c->addr, drv->name ? drv->name : "vendor",
				 ret);
		/* The driver took the codec over (a display function). */
		if (c->is_hdmi)
			return 0;
	}

	ret = hda_generic_parse(c);
	if (ret < 0)
		snd_info(HDA_LOG, "codec#%d: generic parser failed (%d)\n",
			 c->addr, ret);

	if (drv && drv->post_parse) {
		int r = drv->post_parse(c);

		if (r < 0)
			snd_info(HDA_LOG, "codec#%d: %s post-parse failed (%d)\n",
				 c->addr, drv->name ? drv->name : "vendor", r);
	}

	hda_generic_init(c);
	if (drv && drv->init) {
		int r = drv->init(c);

		if (r < 0)
			snd_info(HDA_LOG, "codec#%d: %s init failed (%d)\n",
				 c->addr, drv->name ? drv->name : "vendor", r);
	}
	return 0;
}

/* ======================================================================== */
/* lifecycle                                                                */
/* ======================================================================== */

int hda_codec_probe(struct hda_controller *ctrl, int addr,
		    struct hda_codec **out)
{
	struct hda_codec_priv *p;
	struct hda_codec *c;
	int ret;

	if (!out)
		return -EINVAL;
	*out = 0;
	if (!ctrl || addr < 0 || addr >= HDA_MAX_CODECS)
		return -EINVAL;

	p = kcalloc(1, sizeof(*p));
	if (!p)
		return -ENOMEM;
	c = &p->c;
	c->ctrl = ctrl;
	c->addr = addr;
	spinlock_init(&c->lock, "hda_codec");
	p->in_active = -1;
	p->in_mode = AUDIO_OUTSEL_AUTO;
	c->spec.outsel_mode = AUDIO_OUTSEL_AUTO;
	c->spec.auto_mute = 1;
	if (ctrl->pci) {
		const pci_device_t *pci = ctrl->pci;

		p->pci_ssid = pci_cfg_read32(pci->bus, pci->device,
					     pci->function, 0x2c);
		if (p->pci_ssid == 0xffffffffu)
			p->pci_ssid = 0;
	}

	ret = hda_read_root(c);
	if (ret < 0) {
		snd_info(HDA_LOG, "codec#%d: no response from the root node\n",
			 addr);
		goto fail;
	}
	if (!c->afg) {
		snd_info(HDA_LOG,
			 "codec#%d: %04x:%04x has no audio function (modem "
			 "only), ignored\n",
			 addr, c->vendor_id >> 16, c->vendor_id & 0xffff);
		ret = -ENODEV;
		goto fail;
	}

	/* Power the function up first: some codecs answer parameter reads
	 * of widgets only in D0. */
	hda_codec_write(c, c->afg, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	if (hda_wait_power(c, c->afg, AC_PWRST_D0, HDA_POWER_WAIT_MS) < 0)
		snd_info(HDA_LOG, "codec#%d: AFG 0x%02x slow to reach D0\n",
			 addr, c->afg);

	hda_read_afg(c);
	if (ctrl->flags & HDA_F_NO_UNSOL)
		c->spec.jack_poll = 1;
	if (!c->unsol_capable)
		c->spec.jack_poll = 1;

	ret = hda_enumerate_widgets(c);
	if (ret < 0) {
		snd_info(HDA_LOG, "codec#%d: widget enumeration failed (%d)\n",
			 addr, ret);
		goto fail;
	}

	hda_codec_set_names(c);
	snd_info(HDA_LOG,
		 "codec#%d: %s %s (%08x rev %06x, subsystem %08x, AFG 0x%02x"
		 "%s%s)\n",
		 addr, c->vendor_name, c->chip_name, c->vendor_id,
		 c->revision_id & 0xffffff, c->subsystem_id, c->afg,
		 c->unsol_capable ? ", unsol" : "",
		 c->mfg ? ", modem" : "");

	hda_codec_lock(c);
	ret = hda_attach_driver(c);
	hda_codec_unlock(c);
	if (ret < 0)
		goto fail;

	p->probe_done = 1;
	if (!hda_is_display(c)) {
		snd_info(HDA_LOG,
			 "codec#%d: %d output(s), %d input(s), DAC 0x%02x%s\n",
			 addr, c->spec.nout, c->spec.nin, c->spec.primary_dac,
			 c->spec.jack_poll ? ", jack polling" : "");
		for (int i = 0; i < c->spec.nout; i++) {
			struct hda_out *o = &c->spec.out[i];

			snd_info(HDA_LOG,
				 "codec#%d:   out %s: pin 0x%02x <- DAC 0x%02x"
				 "%s%s\n",
				 addr, o->name, o->pin, o->dac,
				 o->jack_detect ? (o->present ? " [plugged]" :
								" [empty]") :
						  "",
				 o->eapd ? " eapd" : "");
		}
		for (int i = 0; i < c->spec.nin; i++) {
			struct hda_in *in = &c->spec.in[i];

			snd_info(HDA_LOG,
				 "codec#%d:   in %s: pin 0x%02x -> ADC 0x%02x"
				 "%s\n",
				 addr, in->name, in->pin, in->adc,
				 in->jack_detect ? (in->present ? " [plugged]" :
								  " [empty]") :
						   "");
		}
	}
	*out = c;
	return 0;

fail:
	if (c->w)
		kfree(c->w);
	kfree(p);
	return ret;
}

void hda_codec_free(struct hda_codec *c)
{
	if (!c)
		return;
	if (c->vendor && c->vendor->remove)
		c->vendor->remove(c);
	if (c->w)
		kfree(c->w);
	c->w = 0;
	kfree(hda_priv(c));
}

/* ---- jack events -------------------------------------------------------- */

void hda_codec_unsol(struct hda_codec *c, uint32_t res)
{
	struct hda_codec_priv *p;
	uint32_t tag;
	uint8_t nid;

	if (!c)
		return;
	p = hda_priv(c);
	if (!p->probe_done)
		return;
	tag = (res & AC_UNSOL_RES_TAG) >> AC_UNSOL_RES_TAG_SHIFT;

	/* A display codec handles its own events. */
	if (hda_is_display(c) || (c->vendor && c->vendor->probe)) {
		if (c->vendor && c->vendor->unsol)
			c->vendor->unsol(c, res);
		return;
	}

	nid = tag < HDA_TAG_MAX ? p->tag_nid[tag] : 0;
	if (!nid) {
		/* GPIO, volume knob or another vendor-specific source. */
		if (c->vendor && c->vendor->unsol)
			c->vendor->unsol(c, res);
		return;
	}

	/* Re-sense every jack, not just the one that fired: combo jacks
	 * and some codecs report several pins under one event. */
	hda_codec_lock(c);
	if (hda_generic_sense_jacks(c, 1))
		hda_codec_update_routing(c);
	hda_codec_unlock(c);
}

void hda_codec_poll_jacks(struct hda_codec *c)
{
	struct hda_codec_priv *p;

	if (!c)
		return;
	p = hda_priv(c);
	if (!p->probe_done || hda_is_display(c) ||
	    (c->vendor && c->vendor->probe))
		return;
	hda_codec_lock(c);
	if (hda_generic_sense_jacks(c, 1))
		hda_codec_update_routing(c);
	hda_codec_unlock(c);
}

uint8_t hda_jack_tag_for(struct hda_codec *c, uint8_t nid)
{
	struct hda_codec_priv *p = hda_priv(c);

	for (int t = 1; t < HDA_TAG_MAX; t++)
		if (p->tag_nid[t] == nid)
			return (uint8_t)t;
	for (int t = 1; t < HDA_TAG_MAX; t++) {
		if (!p->tag_nid[t]) {
			p->tag_nid[t] = nid;
			p->ntags++;
			return (uint8_t)t;
		}
	}
	return 0;
}

/* ---- fixup tables -------------------------------------------------------- */

static void hda_do_pincfgs(struct hda_codec *c, const struct hda_pincfg *p)
{
	for (int n = 0; p && p->nid && n < HDA_MAX_NODES; p++, n++)
		hda_pin_override_cfg(c, p->nid, p->cfg);
}

static void hda_do_verbs(struct hda_codec *c, const struct hda_verb *v)
{
	for (int n = 0; v && v->nid && n < 1024; v++, n++)
		hda_codec_write(c, v->nid, v->verb, v->payload);
}

static void hda_do_coefs(struct hda_codec *c, const struct hda_coef_fix *f)
{
	for (int n = 0; f && f->nid && n < 1024; f++, n++) {
		/* mask 0 means "write the whole word" */
		if (!f->mask || f->mask == 0xffff)
			hda_coef_write(c, f->nid, f->idx, f->val);
		else
			hda_coef_update(c, f->nid, f->idx, f->mask, f->val);
	}
}

void hda_apply_pincfgs(struct hda_codec *c, const struct hda_pincfg *p)
{
	struct hda_codec_priv *pr;

	if (!c || !p)
		return;
	pr = hda_priv(c);
	hda_codec_lock(c);
	hda_do_pincfgs(c, p);
	for (int i = 0; i < pr->nrp_pins; i++)
		if (pr->rp_pins[i] == p)
			goto out;
	if (pr->nrp_pins < HDA_REPLAY_MAX)
		pr->rp_pins[pr->nrp_pins++] = p;
out:
	hda_codec_unlock(c);
}

void hda_apply_verbs(struct hda_codec *c, const struct hda_verb *v)
{
	struct hda_codec_priv *pr;

	if (!c || !v)
		return;
	pr = hda_priv(c);
	hda_codec_lock(c);
	hda_do_verbs(c, v);
	for (int i = 0; i < pr->nrp_verbs; i++)
		if (pr->rp_verbs[i] == v)
			goto out;
	if (pr->nrp_verbs < HDA_REPLAY_MAX)
		pr->rp_verbs[pr->nrp_verbs++] = v;
out:
	hda_codec_unlock(c);
}

void hda_apply_coefs(struct hda_codec *c, const struct hda_coef_fix *f)
{
	struct hda_codec_priv *pr;

	if (!c || !f)
		return;
	pr = hda_priv(c);
	hda_codec_lock(c);
	hda_do_coefs(c, f);
	for (int i = 0; i < pr->nrp_coefs; i++)
		if (pr->rp_coefs[i] == f)
			goto out;
	if (pr->nrp_coefs < HDA_REPLAY_MAX)
		pr->rp_coefs[pr->nrp_coefs++] = f;
out:
	hda_codec_unlock(c);
}

static int hda_fixup_is_end(const struct hda_fixup *f)
{
	return !f->codec_id && !f->subsys && !f->subsys_mask && !f->name &&
	       !f->pins && !f->verbs && !f->coefs && !f->flags;
}

static int hda_fixup_matches(struct hda_codec *c, const struct hda_fixup *f)
{
	struct hda_codec_priv *p = hda_priv(c);
	uint32_t mask;

	if (f->codec_id && f->codec_id != c->vendor_id)
		return 0;
	if (!f->subsys)
		return 1;
	mask = f->subsys_mask ? f->subsys_mask : 0xffffffffu;
	/* The PCI subsystem of the controller is what the board vendor
	 * programs reliably; the codec's own id is the fallback. */
	if (p->pci_ssid && (p->pci_ssid & mask) == (f->subsys & mask))
		return 1;
	if (c->subsystem_id && (c->subsystem_id & mask) == (f->subsys & mask))
		return 1;
	return 0;
}

static void hda_fixup_apply_one(struct hda_codec *c, const struct hda_fixup *f)
{
	hda_do_pincfgs(c, f->pins);
	hda_do_verbs(c, f->verbs);
	hda_do_coefs(c, f->coefs);
}

const struct hda_fixup *hda_apply_fixups(struct hda_codec *c,
					 const struct hda_fixup *table)
{
	struct hda_codec_priv *p;

	if (!c || !table)
		return 0;
	p = hda_priv(c);
	for (int n = 0; n < 4096 && !hda_fixup_is_end(table); table++, n++) {
		if (!hda_fixup_matches(c, table))
			continue;
		snd_info(HDA_LOG, "codec#%d: applying fixup \"%s\"\n", c->addr,
			 table->name ? table->name : "unnamed");
		hda_codec_lock(c);
		hda_fixup_apply_one(c, table);
		p->fixup = table;
		p->fix_flags |= table->flags;
		if (table->flags & HDA_FIX_JACK_POLL)
			c->spec.jack_poll = 1;
		if (table->flags & HDA_FIX_NO_AUTOMUTE)
			c->spec.auto_mute = 0;
		hda_codec_unlock(c);
		return table;
	}
	return 0;
}

/* ---- resume ---------------------------------------------------------------- */

int hda_codec_resume(struct hda_codec *c)
{
	struct hda_codec_priv *p;

	if (!c)
		return -EINVAL;
	p = hda_priv(c);
	hda_codec_lock(c);

	hda_codec_write(c, c->afg, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	if (hda_wait_power(c, c->afg, AC_PWRST_D0, HDA_POWER_WAIT_MS) < 0)
		snd_info(HDA_LOG, "codec#%d: AFG slow to reach D0 on resume\n",
			 c->addr);

	/* A codec reset restores the BIOS pin configuration: put back every
	 * override, then the vendor tables in the order they were first
	 * applied. */
	for (uint32_t i = 0; c->w && i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type == AC_WID_PIN && w->pin_cfg != w->pin_cfg_orig)
			hda_write_pin_cfg(c, w->nid, w->pin_cfg);
	}
	if (p->fixup)
		hda_fixup_apply_one(c, p->fixup);
	for (int i = 0; i < p->nrp_pins; i++)
		hda_do_pincfgs(c, p->rp_pins[i]);
	for (int i = 0; i < p->nrp_verbs; i++)
		hda_do_verbs(c, p->rp_verbs[i]);
	for (int i = 0; i < p->nrp_coefs; i++)
		hda_do_coefs(c, p->rp_coefs[i]);

	if (c->gpio_mask) {
		hda_codec_write(c, c->afg, AC_VERB_SET_GPIO_MASK, c->gpio_mask);
		hda_codec_write(c, c->afg, AC_VERB_SET_GPIO_DIRECTION,
				c->gpio_dir);
		snd_udelay(10);
		hda_codec_write(c, c->afg, AC_VERB_SET_GPIO_DATA, c->gpio_data);
	}

	if (hda_is_display(c) || (c->vendor && c->vendor->probe)) {
		if (c->vendor && c->vendor->init)
			c->vendor->init(c);
		hda_codec_unlock(c);
		return 0;
	}

	hda_generic_init(c);
	if (c->vendor && c->vendor->init)
		c->vendor->init(c);

	/* Streams that were bound before the reset lost their tags. */
	if (p->play_tag && p->nbound_play)
		hda_codec_setup_stream(c, AUDIO_DIR_PLAYBACK, p->play_tag,
				       p->play_channels, p->play_fmt);
	if (p->cap_tag && p->bound_cap)
		hda_codec_setup_stream(c, AUDIO_DIR_CAPTURE, p->cap_tag,
				       p->cap_channels, p->cap_fmt);
	hda_codec_unlock(c);
	return 0;
}
