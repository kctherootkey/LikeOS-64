// LikeOS Intel audio DSP host driver - the sound card: one PCM device per
// topology PCM, each direction backed by an HDA host DMA engine that feeds
// (or drains) the DSP's host copier, plus the mixer controls, which go to
// the SoundWire codecs where they have hardware volume and to the DSP's
// gain modules otherwise.
//
// Data path of a playback PCM:
//   ring (core-owned) -> host DMA engine (decoupled, stream tag T)
//   -> DSP host copier (gateway T-1) -> pipeline (gain, mixer, ...)
//   -> DAI copier -> SoundWire/ALH, HDA link or SSP -> codec
// The position the PCM core sees is the host engine's (DPIB), the period
// interrupts are the engine's buffer completions.
//
// Copyright (C) 2026 The LikeOS Project

#include "sof_regs.h"

#include <kernel/mm/memory.h>

/* Per-direction runtime of one PCM device. */
struct sof_pcm_rt {
	struct sof_hstream *st;
	int pipe_prepared;
	int triggered;
	uint32_t rate, channels, bits;
	uint16_t format;
};

struct sof_pcm_dev {
	struct sof_host *h;
	int idx; /* topology PCM index */
	struct snd_pcm *pcm;
	struct sof_pcm_rt rt[2];
	uint32_t output_type;
	int dai_type;
};

struct sof_card_state {
	struct sof_pcm_dev *pd[SND_MAX_PCMS];
	int npd;
	/* the DSP gain fallback for cards without codec volume */
	uint32_t dsp_left, dsp_right;
	uint32_t dsp_mute;
	int dsp_valid;
};

static struct sof_card_state *sof_card_state_of(struct sof_host *h)
{
	return (struct sof_card_state *)h->s.pcm_priv;
}

/* sample format -> valid bits for the DSP and the HDA format word */
static uint32_t sof_fmt_bits(uint32_t fmt)
{
	switch (fmt) {
	case AUDIO_FMT_S16_LE:
		return 16;
	case AUDIO_FMT_S20_LE:
		return 20;
	case AUDIO_FMT_S24_LE:
		return 24;
	case AUDIO_FMT_S32_LE:
	case AUDIO_FMT_FLOAT_LE:
		return 32;
	default:
		return 0;
	}
}

#define SOF_PCM_FORMATS                                                    \
	(AUDIO_FMT_BIT(AUDIO_FMT_S16_LE) | AUDIO_FMT_BIT(AUDIO_FMT_S24_LE) | \
	 AUDIO_FMT_BIT(AUDIO_FMT_S32_LE))

/* ===================================================================== */
/* SoundWire DAI configuration (contract entry point)                     */
/* ===================================================================== */

static void sof_sdw_pcmsycm(struct sof_host *h, int link, int pdi,
			    int channels, int tag, int dir)
{
	int l = sof_ml_find_alt(h, SOF_ML_ID_SDW);
	uint32_t reg;
	uint16_t v = 0;

	if (l < 0 || !h->link[l].base || link >= h->link[l].slcount ||
	    pdi < 0 || pdi > 15)
		return;
	reg = h->link[l].base + SOF_ML_SDW_INSTANCE_STRIDE * (uint32_t)link +
	      SOF_ML_SDW_PCMSYCM(pdi);
	if (reg + 2 > h->bar0_map_size)
		return;
	if (channels > 0) {
		v = (uint16_t)(0 | ((uint32_t)(channels - 1) << 4) |
			       (((uint32_t)tag & 0x3f) << 8) |
			       (dir == AUDIO_DIR_CAPTURE ? 0x8000u : 0));
	}
	sof_hda_w16(h, reg, v);
}

int sof_dsp_sdw_dai_config(struct sof_dev *sdev, int link, int alh_stream,
			   uint32_t rate, uint32_t channels, uint32_t bits)
{
	struct sof_host *h = sof_host_of(sdev);
	struct sof_sdw_cfg *c = NULL;
	int dir, i;

	if (!sdev || !h->chip)
		return -ENODEV;
	if (link < 0 || link >= SOF_MAX_SDW_LINKS || alh_stream < 0 ||
	    channels == 0 || channels > 16 || !audio_rate_bit(rate) ||
	    (bits != 16 && bits != 24 && bits != 32))
		return -EINVAL;
	dir = h->prep_dir == AUDIO_DIR_CAPTURE ? AUDIO_DIR_CAPTURE :
						 AUDIO_DIR_PLAYBACK;

	for (i = 0; i < SOF_MAX_SDW_CFG; i++) {
		struct sof_sdw_cfg *x = &h->sdwcfg[i];

		if (x->used && x->link == link && x->alh_stream == alh_stream &&
		    x->pcm_idx == h->prep_pcm && x->pcm_dir == dir) {
			c = x;
			break;
		}
	}
	for (i = 0; i < SOF_MAX_SDW_CFG && !c; i++)
		if (!h->sdwcfg[i].used)
			c = &h->sdwcfg[i];
	if (!c)
		return -ENOSPC;

	c->used = 1;
	c->link = link;
	c->alh_stream = alh_stream;
	c->rate = rate;
	c->channels = channels;
	c->bits = bits;
	c->pcm_idx = h->prep_pcm;
	c->pcm_dir = dir;
	c->format = sof_hda_format(rate, channels, bits);

	/* ACE 2.x+: SoundWire data moves through an HDA link DMA whose
	 * stream id the link's PCM stream channel map must carry. */
	if (sof_is_ace2(h)) {
		int rc, pdi;

		if (!c->link_dma) {
			c->link_dma = sof_link_stream_get(h, dir);
			if (!c->link_dma) {
				c->used = 0;
				return -EBUSY;
			}
		}
		rc = sof_link_dma_setup(c->link_dma, c->format);
		if (rc) {
			sof_link_stream_put(c->link_dma);
			c->link_dma = NULL;
			c->used = 0;
			return rc;
		}
		pdi = sdw_intel_alh_to_pdi(link, alh_stream);
		if (pdi >= 0)
			sof_sdw_pcmsycm(h, link, pdi, (int)channels,
					c->link_dma->tag, dir);
	}
	return 0;
}

/* The contract entry point.  Weak: the topology side keeps its own
 * definition, which must then call sof_dsp_sdw_dai_config() so the link
 * DMA of ACE 2.x+ is set up. */
__attribute__((weak)) int sof_dai_sdw_config(struct sof_dev *sdev, int link,
					     int alh_stream, uint32_t rate,
					     uint32_t channels, uint32_t bits)
{
	return sof_dsp_sdw_dai_config(sdev, link, alh_stream, rate, channels,
				      bits);
}

void sof_pcm_link_dmas(struct sof_host *h, int idx, int dir, int run)
{
	int i;

	for (i = 0; i < SOF_MAX_SDW_CFG; i++) {
		struct sof_sdw_cfg *c = &h->sdwcfg[i];

		if (c->used && c->link_dma && c->pcm_idx == idx &&
		    c->pcm_dir == dir)
			sof_link_dma_run(c->link_dma, run);
	}
}

void sof_pcm_link_dmas_release(struct sof_host *h, int idx, int dir)
{
	int i;

	for (i = 0; i < SOF_MAX_SDW_CFG; i++) {
		struct sof_sdw_cfg *c = &h->sdwcfg[i];

		if (!c->used || c->pcm_idx != idx || c->pcm_dir != dir)
			continue;
		if (c->link_dma) {
			int pdi = sdw_intel_alh_to_pdi(c->link, c->alh_stream);

			if (pdi >= 0)
				sof_sdw_pcmsycm(h, c->link, pdi, 0, 0, dir);
			sof_link_stream_put(c->link_dma);
			c->link_dma = NULL;
		}
		c->used = 0;
	}
}

/* ===================================================================== */
/* PCM operations                                                         */
/* ===================================================================== */

static struct sof_pcm_dev *sof_pd(struct snd_pcm_substream *ss)
{
	return ss->pcm ? (struct sof_pcm_dev *)ss->pcm->private_data : NULL;
}

static int sof_pcm_open(struct snd_pcm_substream *ss)
{
	struct sof_pcm_dev *pd = sof_pd(ss);

	if (!pd)
		return -ENODEV;
	if (pd->h->crashed || !pd->h->fw_ready)
		return -EIO;
	if (ss->direction != AUDIO_DIR_PLAYBACK &&
	    ss->direction != AUDIO_DIR_CAPTURE)
		return -EINVAL;
	ss->private_data = &pd->rt[ss->direction];
	return 0;
}

/* Tear down whatever this direction holds (process context). */
static void sof_pcm_teardown(struct sof_pcm_dev *pd, int dir)
{
	struct sof_host *h = pd->h;
	struct sof_pcm_rt *rt = &pd->rt[dir];

	sof_mutex_lock(&h->pcm_mutex);
	if (rt->st && rt->st->running)
		sof_stream_stop(rt->st);
	if (rt->pipe_prepared) {
		int rc = sof_tplg_pcm_free(&h->s, pd->idx, dir);

		if (rc && !h->crashed)
			sof_log(h, "PCM %d: freeing the pipelines failed: %d\n",
				pd->idx, rc);
		rt->pipe_prepared = 0;
	}
	sof_pcm_link_dmas_release(h, pd->idx, dir);
	if (rt->st) {
		uint64_t fl;

		spin_lock_irqsave(&h->reg_lock, &fl);
		rt->st->substream = NULL;
		spin_unlock_irqrestore(&h->reg_lock, fl);
		sof_stream_release_hw(rt->st);
		sof_stream_put(rt->st);
		rt->st = NULL;
	}
	rt->triggered = 0;
	sof_mutex_unlock(&h->pcm_mutex);
}

static void sof_pcm_close(struct snd_pcm_substream *ss)
{
	struct sof_pcm_dev *pd = sof_pd(ss);

	if (!pd)
		return;
	sof_pcm_teardown(pd, ss->direction);
	ss->private_data = NULL;
}

static int sof_pcm_program(struct sof_pcm_dev *pd,
			   struct snd_pcm_substream *ss)
{
	struct sof_pcm_rt *rt = &pd->rt[ss->direction];
	struct sof_dma_chunk ring;
	uint64_t fl;
	int rc;

	ring.virt = ss->dma_area;
	ring.phys = ss->dma_phys;
	ring.bytes = ss->buffer_bytes;
	ring.alloc = ss->buffer_bytes;
	rc = sof_stream_program(rt->st, &ring, 1, ss->buffer_bytes,
				ss->period_bytes, rt->format, 1);
	if (rc)
		return rc;
	spin_lock_irqsave(&pd->h->reg_lock, &fl);
	rt->st->substream = ss;
	rt->st->owner = pd;
	spin_unlock_irqrestore(&pd->h->reg_lock, fl);
	return 0;
}

static int sof_pcm_hw_params(struct snd_pcm_substream *ss)
{
	struct sof_pcm_dev *pd = sof_pd(ss);
	struct sof_host *h;
	struct sof_pcm_rt *rt;
	uint32_t bits;
	uint16_t fmt;
	int rc;

	if (!pd)
		return -ENODEV;
	h = pd->h;
	rt = &pd->rt[ss->direction];
	if (h->crashed)
		return -EIO;
	bits = sof_fmt_bits(ss->params.format);
	fmt = sof_hda_format(ss->params.rate, ss->params.channels, bits);
	if (!bits || !fmt)
		return -EINVAL;
	if (!ss->dma_phys || !ss->buffer_bytes || !ss->period_bytes ||
	    (ss->period_bytes & 127) || ss->buffer_bytes % ss->period_bytes ||
	    ss->buffer_bytes / ss->period_bytes > SOF_BDL_MAX_ENTRIES)
		return -EINVAL;

	/* new parameters: the old pipelines no longer fit */
	if (rt->pipe_prepared)
		sof_pcm_teardown(pd, ss->direction);

	sof_mutex_lock(&h->pcm_mutex);
	if (!rt->st) {
		rt->st = sof_stream_get(h, ss->direction);
		if (!rt->st) {
			sof_mutex_unlock(&h->pcm_mutex);
			return -EBUSY;
		}
	}
	rt->rate = ss->params.rate;
	rt->channels = ss->params.channels;
	rt->bits = bits;
	rt->format = fmt;
	rc = sof_pcm_program(pd, ss);
	if (rc) {
		sof_stream_put(rt->st);
		rt->st = NULL;
	}
	sof_mutex_unlock(&h->pcm_mutex);
	return rc;
}

static int sof_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct sof_pcm_dev *pd = sof_pd(ss);

	if (pd)
		sof_pcm_teardown(pd, ss->direction);
	return 0;
}

static int sof_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct sof_pcm_dev *pd = sof_pd(ss);
	struct sof_host *h;
	struct sof_pcm_rt *rt;
	int rc;

	if (!pd)
		return -ENODEV;
	h = pd->h;
	rt = &pd->rt[ss->direction];
	if (!rt->st)
		return -EINVAL;
	if (h->crashed)
		return -EIO;

	sof_mutex_lock(&h->pcm_mutex);
	if (rt->pipe_prepared) {
		/* restart after drain/xrun: rebuild the pipelines too, so
		 * the DSP buffers start empty */
		sof_tplg_pcm_free(&h->s, pd->idx, ss->direction);
		rt->pipe_prepared = 0;
		sof_pcm_link_dmas_release(h, pd->idx, ss->direction);
	}
	rc = sof_pcm_program(pd, ss);
	if (rc)
		goto out;
	h->prep_pcm = pd->idx;
	h->prep_dir = ss->direction;
	rc = sof_tplg_pcm_prepare(&h->s, pd->idx, ss->direction, rt->rate,
				  rt->channels, rt->bits, rt->st->tag);
	h->prep_pcm = -1;
	h->prep_dir = -1;
	if (rc) {
		sof_log(h, "PCM %d: pipeline setup for %u Hz/%u ch/%u bit failed: %d\n",
			pd->idx, rt->rate, rt->channels, rt->bits, rc);
		sof_pcm_link_dmas_release(h, pd->idx, ss->direction);
		goto out;
	}
	rt->pipe_prepared = 1;
	rt->triggered = 0;
out:
	sof_mutex_unlock(&h->pcm_mutex);
	return rc;
}

/* Lock held, interrupts off: the IPC polls for its replies. */
static int sof_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct sof_pcm_dev *pd = sof_pd(ss);
	struct sof_host *h;
	struct sof_pcm_rt *rt;
	int rc = 0;

	if (!pd)
		return -ENODEV;
	h = pd->h;
	rt = &pd->rt[ss->direction];
	if (!rt->st || !rt->pipe_prepared)
		return -EINVAL;

	switch (cmd) {
	case SND_TRIGGER_START:
		/* host DMA first, so data is there when the pipeline runs */
		rc = sof_stream_start(rt->st);
		if (rc)
			return rc;
		sof_pcm_link_dmas(h, pd->idx, ss->direction, 1);
		rc = sof_tplg_pcm_trigger(&h->s, pd->idx, ss->direction, cmd);
		if (rc) {
			sof_pcm_link_dmas(h, pd->idx, ss->direction, 0);
			sof_stream_stop(rt->st);
			return rc;
		}
		rt->triggered = 1;
		break;
	case SND_TRIGGER_STOP:
		/* pipeline first, then the DMA it pulls from */
		rc = sof_tplg_pcm_trigger(&h->s, pd->idx, ss->direction, cmd);
		sof_pcm_link_dmas(h, pd->idx, ss->direction, 0);
		sof_stream_stop(rt->st);
		rt->triggered = 0;
		if (h->crashed)
			rc = 0; /* nothing left to stop */
		break;
	case SND_TRIGGER_PAUSE:
		/* the paused gateway stops pulling; the engine just waits */
		rc = sof_tplg_pcm_trigger(&h->s, pd->idx, ss->direction, cmd);
		break;
	case SND_TRIGGER_RESUME:
		rc = sof_tplg_pcm_trigger(&h->s, pd->idx, ss->direction, cmd);
		break;
	default:
		return -EINVAL;
	}
	return rc;
}

static uint32_t sof_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct sof_pcm_dev *pd = sof_pd(ss);
	struct sof_pcm_rt *rt;
	uint32_t pos;

	if (!pd)
		return 0;
	rt = &pd->rt[ss->direction];
	if (!rt->st)
		return 0;
	pos = sof_stream_position(rt->st);
	if (ss->buffer_bytes && pos >= ss->buffer_bytes)
		pos %= ss->buffer_bytes;
	return pos;
}

static const struct snd_pcm_ops sof_pcm_ops = {
	.open = sof_pcm_open,
	.close = sof_pcm_close,
	.hw_params = sof_pcm_hw_params,
	.hw_free = sof_pcm_hw_free,
	.prepare = sof_pcm_prepare,
	.trigger = sof_pcm_trigger,
	.pointer = sof_pcm_pointer,
	.delay = NULL,
};

/* ===================================================================== */
/* controls                                                               */
/* ===================================================================== */

static struct sof_host *sof_card_host(struct snd_card *card)
{
	return card ? (struct sof_host *)card->private_data : NULL;
}

/* Apply the DSP gain to every playback PCM. */
static int sof_dsp_gain_all(struct sof_host *h, uint32_t l, uint32_t r)
{
	struct sof_card_state *cs = sof_card_state_of(h);
	int i, ok = 0, err = -ENOENT;

	if (!cs)
		return -ENODEV;
	for (i = 0; i < cs->npd; i++) {
		int rc;

		if (!cs->pd[i] || !cs->pd[i]->pcm ||
		    !cs->pd[i]->pcm->hw[AUDIO_DIR_PLAYBACK].formats)
			continue;
		rc = sof_tplg_set_volume(&h->s, cs->pd[i]->idx, l, r);
		if (rc == 0)
			ok = 1;
		else
			err = rc;
	}
	return ok ? 0 : err;
}

static int sof_ctl_get_volume(struct snd_card *card, uint32_t ctl,
			      struct audio_volume *v)
{
	struct sof_host *h = sof_card_host(card);
	struct sof_card_state *cs;
	int rc;

	if (!h || !v || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	cs = sof_card_state_of(h);
	sof_mutex_lock(&h->ctl_mutex);
	rc = ctl == AUDIO_CTL_PCM ? -ENOENT :
				    sdw_codec_get_volume(&h->s, ctl, v);
	if (rc && cs && cs->dsp_valid &&
	    (ctl == AUDIO_CTL_PCM || ctl == AUDIO_CTL_MASTER)) {
		kmemset(v, 0, sizeof(*v));
		v->control = ctl;
		v->left = cs->dsp_left;
		v->right = cs->dsp_right;
		rc = 0;
	}
	sof_mutex_unlock(&h->ctl_mutex);
	return rc;
}

static int sof_ctl_set_volume(struct snd_card *card, uint32_t ctl,
			      uint32_t left, uint32_t right)
{
	struct sof_host *h = sof_card_host(card);
	struct sof_card_state *cs;
	int rc;

	if (!h || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	if (left > 100)
		left = 100;
	if (right > 100)
		right = 100;
	cs = sof_card_state_of(h);
	sof_mutex_lock(&h->ctl_mutex);
	rc = ctl == AUDIO_CTL_PCM ? -ENOENT :
				    sdw_codec_set_volume(&h->s, ctl, left, right);
	if (rc && cs && (ctl == AUDIO_CTL_PCM || ctl == AUDIO_CTL_MASTER)) {
		/* no codec amplifier for this: scale in the DSP */
		rc = cs->dsp_mute ? sof_dsp_gain_all(h, 0, 0) :
				    sof_dsp_gain_all(h, left, right);
		if (rc == 0) {
			cs->dsp_left = left;
			cs->dsp_right = right;
			cs->dsp_valid = 1;
		}
	}
	sof_mutex_unlock(&h->ctl_mutex);
	return rc;
}

static int sof_ctl_get_mute(struct snd_card *card, uint32_t ctl,
			    uint32_t *mute)
{
	struct sof_host *h = sof_card_host(card);
	struct sof_card_state *cs;
	int rc;

	if (!h || !mute || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	cs = sof_card_state_of(h);
	sof_mutex_lock(&h->ctl_mutex);
	rc = sdw_codec_get_mute(&h->s, ctl, mute);
	if (rc && cs && cs->dsp_valid &&
	    (ctl == AUDIO_CTL_PCM || ctl == AUDIO_CTL_MASTER)) {
		*mute = cs->dsp_mute;
		rc = 0;
	}
	sof_mutex_unlock(&h->ctl_mutex);
	return rc;
}

static int sof_ctl_set_mute(struct snd_card *card, uint32_t ctl,
			    uint32_t mute)
{
	struct sof_host *h = sof_card_host(card);
	struct sof_card_state *cs;
	int rc;

	if (!h || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	mute = mute ? 1 : 0;
	cs = sof_card_state_of(h);
	sof_mutex_lock(&h->ctl_mutex);
	rc = sdw_codec_set_mute(&h->s, ctl, mute);
	if (rc && cs && (ctl == AUDIO_CTL_PCM || ctl == AUDIO_CTL_MASTER)) {
		uint32_t l = cs->dsp_valid ? cs->dsp_left : 100;
		uint32_t r = cs->dsp_valid ? cs->dsp_right : 100;

		rc = mute ? sof_dsp_gain_all(h, 0, 0) : sof_dsp_gain_all(h, l, r);
		if (rc == 0) {
			cs->dsp_mute = mute;
			cs->dsp_left = l;
			cs->dsp_right = r;
			cs->dsp_valid = 1;
		}
	}
	sof_mutex_unlock(&h->ctl_mutex);
	return rc;
}

/* Outputs from the topology when the codecs cannot tell. */
static void sof_tplg_outputs(struct sof_host *h, struct audio_outputs *o)
{
	int i, n = sof_tplg_pcm_count(&h->s);

	kmemset(o, 0, sizeof(*o));
	o->mode = AUDIO_OUTSEL_ALL;
	for (i = 0; i < n && o->count < AUDIO_MAX_OUTPUTS; i++) {
		const struct sof_tplg_pcm *p = sof_tplg_pcm_get(&h->s, i);
		struct audio_output_desc *d;

		if (!p || !p->has_playback)
			continue;
		d = &o->out[o->count];
		d->id = (uint32_t)p->id;
		d->type = p->output_type ? p->output_type : AUDIO_OUT_SPEAKER;
		d->flags = d->type == AUDIO_OUT_SPEAKER ? AUDIO_OUTF_FIXED : 0;
		d->present = 1;
		d->active = 1;
		sof_strlcpy(d->name, p->name, sizeof(d->name));
		o->count++;
	}
}

static int sof_ctl_get_outputs(struct snd_card *card, struct audio_outputs *o)
{
	struct sof_host *h = sof_card_host(card);
	int rc;

	if (!h || !o)
		return -EINVAL;
	sof_mutex_lock(&h->ctl_mutex);
	rc = sdw_codec_get_outputs(&h->s, o);
	if (rc || o->count == 0) {
		sof_tplg_outputs(h, o);
		rc = o->count ? 0 : -ENOENT;
	}
	if (o->count > AUDIO_MAX_OUTPUTS)
		o->count = AUDIO_MAX_OUTPUTS;
	sof_mutex_unlock(&h->ctl_mutex);
	return rc;
}

static int sof_ctl_set_output(struct snd_card *card,
			      const struct audio_setoutput *s)
{
	struct sof_host *h = sof_card_host(card);
	int rc;

	if (!h || !s || s->mode > AUDIO_OUTSEL_ALL)
		return -EINVAL;
	sof_mutex_lock(&h->ctl_mutex);
	rc = sdw_codec_set_output(&h->s, s);
	sof_mutex_unlock(&h->ctl_mutex);
	return rc;
}

static int sof_ctl_get_inputs(struct snd_card *card, struct audio_outputs *o)
{
	struct sof_host *h = sof_card_host(card);
	int i, n;

	if (!h || !o)
		return -EINVAL;
	kmemset(o, 0, sizeof(*o));
	o->mode = AUDIO_OUTSEL_AUTO;
	n = sof_tplg_pcm_count(&h->s);
	for (i = 0; i < n && o->count < AUDIO_MAX_OUTPUTS; i++) {
		const struct sof_tplg_pcm *p = sof_tplg_pcm_get(&h->s, i);
		struct audio_output_desc *d;

		if (!p || !p->has_capture)
			continue;
		d = &o->out[o->count++];
		d->id = (uint32_t)p->id;
		d->present = 1;
		d->active = 1;
		sof_strlcpy(d->name, p->name, sizeof(d->name));
	}
	return o->count ? 0 : -ENOENT;
}

static int sof_ctl_set_input(struct snd_card *card,
			     const struct audio_setoutput *s)
{
	(void)card;
	(void)s;
	return -EOPNOTSUPP;
}

long sof_pcm_diag(struct sof_host *h, char *buf, size_t len)
{
	struct sof_card_state *cs = sof_card_state_of(h);
	size_t pos = 0;
	int i, d;

	pos = sof_append(buf, len, pos, "PCM devices:\n");
	for (i = 0; cs && i < cs->npd; i++) {
		struct sof_pcm_dev *pd = cs->pd[i];

		if (!pd || !pd->pcm)
			continue;
		pos = sof_append(buf, len, pos, "  pcm%d '%s' (topology PCM %d)\n",
				 i, pd->pcm->name, pd->idx);
		for (d = 0; d < 2; d++) {
			struct sof_pcm_rt *rt = &pd->rt[d];
			struct sof_hstream *st = rt->st;

			if (!pd->pcm->hw[d].formats)
				continue;
			pos = sof_append(buf, len, pos, "    %s: ",
					 d ? "capture" : "playback");
			if (!st) {
				pos = sof_append(buf, len, pos, "idle\n");
				continue;
			}
			pos = sof_append(buf, len, pos,
					 "engine %d tag %d %s, %u Hz %u ch %u bit (fmt %#x), ring %u/%u, pos %u, %u irqs, %u fifo/%u desc errors%s\n",
					 st->index, st->tag,
					 st->running ? "running" : "stopped",
					 rt->rate, rt->channels, rt->bits,
					 st->format, st->period_bytes,
					 st->buffer_bytes,
					 sof_stream_position(st), st->irq_count,
					 st->fifo_errors, st->desc_errors,
					 rt->pipe_prepared ? ", pipelines up" :
							     "");
		}
	}
	for (i = 0; i < SOF_MAX_SDW_CFG; i++) {
		struct sof_sdw_cfg *c = &h->sdwcfg[i];

		if (!c->used)
			continue;
		pos = sof_append(buf, len, pos,
				 "  SoundWire link %d stream %d: %u Hz %u ch %u bit, PCM %d %s%s",
				 c->link, c->alh_stream, c->rate, c->channels,
				 c->bits, c->pcm_idx,
				 c->pcm_dir ? "capture" : "playback",
				 c->link_dma ? ", link DMA" : "");
		if (c->link_dma)
			pos = sof_append(buf, len, pos, " %d (tag %d)%s",
					 c->link_dma->index, c->link_dma->tag,
					 c->link_dma->link_running ? " running" :
								     "");
		pos = sof_append(buf, len, pos, "\n");
	}
	return (long)pos;
}

/* AUDIO_DIAG: the DSP, firmware and IPC state, then the topology and the
 * SoundWire side, as far as the buffer goes. */
static long sof_ctl_diag(struct snd_card *card, uint32_t what, char *buf,
			 size_t len)
{
	struct sof_host *h = sof_card_host(card);
	size_t pos = 0;
	long n;

	if (!h || !buf || len < 2)
		return -EINVAL;
	buf[0] = 0;
	if (h->crashed && !h->panic_dumped)
		sof_dsp_panic_dump(h);

	if (what == AUDIO_DIAG_SUMMARY || what == AUDIO_DIAG_STREAMS) {
		n = sof_dsp_diag(h, buf + pos, len - pos);
		if (n > 0)
			pos += (size_t)n;
		n = sof_fw_diag(h, buf + pos, len - pos);
		if (n > 0)
			pos += (size_t)n;
		n = sof_ipc4_diag(h, buf + pos, len - pos);
		if (n > 0)
			pos += (size_t)n;
		n = sof_pcm_diag(h, buf + pos, len - pos);
		if (n > 0)
			pos += (size_t)n;
	}
	if (what == AUDIO_DIAG_SUMMARY || what == AUDIO_DIAG_CODECS) {
		if (pos + 2 < len) {
			pos = sof_append(buf, len, pos, "--- topology ---\n");
			n = sof_tplg_dump(&h->s, buf + pos, len - pos);
			if (n > 0)
				pos += (size_t)n;
		}
		if (pos + 2 < len && h->sdw_up) {
			pos = sof_append(buf, len, pos, "--- SoundWire ---\n");
			n = sdw_dump(&h->s, buf + pos, len - pos);
			if (n > 0)
				pos += (size_t)n;
		}
	}
	if (pos >= len)
		pos = len - 1;
	buf[pos] = 0;
	return (long)pos;
}

static const struct snd_ctl_ops sof_ctl_ops = {
	.get_volume = sof_ctl_get_volume,
	.set_volume = sof_ctl_set_volume,
	.get_mute = sof_ctl_get_mute,
	.set_mute = sof_ctl_set_mute,
	.get_outputs = sof_ctl_get_outputs,
	.set_output = sof_ctl_set_output,
	.get_inputs = sof_ctl_get_inputs,
	.set_input = sof_ctl_set_input,
	.diag = sof_ctl_diag,
};

/* ===================================================================== */
/* card creation                                                          */
/* ===================================================================== */

static const char *sof_codec_part_name(uint16_t mfg, uint16_t part)
{
	static const struct {
		uint16_t mfg, part;
		const char *name;
	} t[] = {
		{ 0x01fa, 0x4243, "cs42l43" }, { 0x01fa, 0x3556, "cs35l56" },
		{ 0x01fa, 0x3557, "cs35l57" }, { 0x01fa, 0x3563, "cs35l63" },
		{ 0x01fa, 0x4242, "cs42l42" }, { 0x01fa, 0x4245, "cs42l45" },
		{ 0x025d, 0x0711, "rt711" },   { 0x025d, 0x0712, "rt712" },
		{ 0x025d, 0x0713, "rt713" },   { 0x025d, 0x0714, "rt714" },
		{ 0x025d, 0x0715, "rt715" },   { 0x025d, 0x0721, "rt721" },
		{ 0x025d, 0x0722, "rt722" },   { 0x025d, 0x1308, "rt1308" },
		{ 0x025d, 0x1316, "rt1316" },  { 0x025d, 0x1318, "rt1318" },
		{ 0x025d, 0x1320, "rt1320" },  { 0x025d, 0x1712, "rt1712" },
		{ 0x025d, 0x1713, "rt1713" },  { 0x019f, 0x8373, "max98373" },
		{ 0x019f, 0x8363, "max98363" }, { 0x0105, 0xaaaa, "es9356" },
	};
	size_t i;

	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (t[i].part == part && (t[i].mfg == mfg || !mfg))
			return t[i].name;
	return NULL;
}

/* "cs42l43" or "rt711+rt1316+rt714": the codecs named by the machine. */
static void sof_codec_desc(struct sof_host *h, char *out, size_t len)
{
	const struct sof_machine *m = &h->s.machine;
	size_t pos = 0;
	int i, j;

	out[0] = 0;
	for (i = 0; i < m->nperiph && i < SOF_MAX_SDW_PERIPHERALS; i++) {
		const char *n = sof_codec_part_name(m->periph[i].mfg_id,
						    m->periph[i].part_id);
		char tmp[16];
		int dup = 0;

		if (!n) {
			ksnprintf(tmp, sizeof(tmp), "%04x", m->periph[i].part_id);
			n = tmp;
		}
		for (j = 0; j < i; j++)
			if (m->periph[j].part_id == m->periph[i].part_id)
				dup = 1;
		if (dup)
			continue;
		pos = sof_append(out, len, pos, "%s%s", pos ? "+" : "", n);
	}
	if (!pos && m->dmic_count)
		pos = sof_append(out, len, pos, "dmic%u", m->dmic_count);
	if (!pos && (m->hda_codec_mask & (1u << 2)))
		pos = sof_append(out, len, pos, "hdmi");
	if (!pos)
		sof_strlcpy(out, "generic", len);
}

/* Order of preference when the topology has more PCMs than a card holds. */
static int sof_pcm_rank(const struct sof_tplg_pcm *p)
{
	if (p->has_playback) {
		switch (p->output_type) {
		case AUDIO_OUT_SPEAKER:
			return 0;
		case AUDIO_OUT_HEADPHONE:
		case AUDIO_OUT_HEADSET:
		case AUDIO_OUT_LINE:
			return 1;
		case AUDIO_OUT_HDMI:
		case AUDIO_OUT_DP:
			return 3;
		default:
			return p->dai_type == SOF_DAI_HDA ? 3 : 1;
		}
	}
	if (p->has_capture)
		return 2;
	return 9;
}

static void sof_fill_hw(struct snd_pcm_hw *hw, const struct sof_tplg_pcm *p,
			int dma32)
{
	kmemset(hw, 0, sizeof(*hw));
	hw->formats = p->formats & SOF_PCM_FORMATS;
	if (!hw->formats)
		hw->formats = SOF_PCM_FORMATS;
	hw->rates = p->rates;
	if (!hw->rates)
		hw->rates = AUDIO_RATE_48000;
	hw->channels_min = p->channels_min ? p->channels_min : 2;
	hw->channels_max = p->channels_max ? p->channels_max : 2;
	if (hw->channels_max > 8)
		hw->channels_max = 8;
	if (hw->channels_min > hw->channels_max)
		hw->channels_min = hw->channels_max;
	hw->period_bytes_min = 256;
	hw->period_bytes_max = 128u << 10;
	hw->periods_min = 2;
	hw->periods_max = 32;
	hw->buffer_bytes_max = 512u << 10;
	hw->buffer_align = 128;
	hw->period_align = 128;
	hw->dma32 = dma32;
	hw->dma_coherent = 0;
}

int sof_card_create(struct sof_host *h)
{
	struct sof_card_state *cs;
	struct snd_card *card = NULL;
	int order[SOF_MAX_PCMS * 4];
	int n, i, j, norder = 0, rc, first_play_done = 0;
	uint32_t hdmi_n = 0;
	uint32_t flags = AUDIO_INFO_MMAP;

	n = sof_tplg_pcm_count(&h->s);
	if (n <= 0) {
		sof_log(h, "the topology describes no PCM devices\n");
		return -ENODEV;
	}
	if (n > (int)(sizeof(order) / sizeof(order[0])))
		n = (int)(sizeof(order) / sizeof(order[0]));
	for (i = 0; i < n; i++) {
		const struct sof_tplg_pcm *p = sof_tplg_pcm_get(&h->s, i);

		if (p && (p->has_playback || p->has_capture))
			order[norder++] = i;
	}
	/* stable sort by rank */
	for (i = 1; i < norder; i++) {
		int v = order[i];
		int rv = sof_pcm_rank(sof_tplg_pcm_get(&h->s, v));

		for (j = i - 1;
		     j >= 0 && sof_pcm_rank(sof_tplg_pcm_get(&h->s, order[j])) > rv;
		     j--)
			order[j + 1] = order[j];
		order[j + 1] = v;
	}
	if (norder == 0)
		return -ENODEV;
	if (norder > SND_MAX_PCMS) {
		sof_log(h, "topology has %d PCMs, the card keeps %d\n", norder,
			SND_MAX_PCMS);
		norder = SND_MAX_PCMS;
	}

	cs = kcalloc(1, sizeof(*cs));
	if (!cs)
		return -ENOMEM;
	h->s.pcm_priv = cs;

	rc = snd_card_new(SOF_DRV, h->s.pci, &card);
	if (rc || !card) {
		sof_log(h, "cannot create the card: %d\n", rc);
		goto fail;
	}
	sof_codec_desc(h, h->codec_desc, sizeof(h->codec_desc));
	{
		char plat[8];
		size_t k;

		sof_strlcpy(plat, h->id->tag, sizeof(plat));
		for (k = 0; plat[k]; k++)
			if (plat[k] >= 'a' && plat[k] <= 'z')
				plat[k] = (char)(plat[k] - 'a' + 'A');
		ksnprintf(card->name, sizeof(card->name), "SOF %s %s", plat,
			  h->codec_desc);
	}
	sof_strlcpy(card->driver, SOF_DRV, sizeof(card->driver));
	sof_strlcpy(card->codec, h->codec_desc, sizeof(card->codec));
	sof_strlcpy(card->bus, h->bus, sizeof(card->bus));
	card->ctl = &sof_ctl_ops;
	card->private_data = h;

	for (i = 0; i < norder; i++) {
		const struct sof_tplg_pcm *p = sof_tplg_pcm_get(&h->s, order[i]);
		struct snd_pcm_hw hp, hc;
		struct sof_pcm_dev *pd;
		struct snd_pcm *pcm = NULL;

		pd = kcalloc(1, sizeof(*pd));
		if (!pd) {
			rc = -ENOMEM;
			goto fail;
		}
		pd->h = h;
		pd->idx = order[i];
		pd->output_type = p->output_type;
		pd->dai_type = p->dai_type;
		cs->pd[cs->npd++] = pd;
		if (p->has_playback)
			sof_fill_hw(&hp, p, !h->addr64);
		if (p->has_capture)
			sof_fill_hw(&hc, p, !h->addr64);
		rc = snd_pcm_new(card, p->name[0] ? p->name : "SOF PCM",
				 &sof_pcm_ops, p->has_playback ? &hp : NULL,
				 p->has_capture ? &hc : NULL, pd, &pcm);
		if (rc) {
			sof_log(h, "PCM '%s' not added: %d\n", p->name, rc);
			goto fail;
		}
		pd->pcm = pcm;
		/* No interrupt to report periods with: the sound core drives
		 * the position from its own timer instead. */
		if (!h->irq_registered) {
			if (p->has_playback)
				snd_pcm_set_timer_driven(
					&pcm->sub[AUDIO_DIR_PLAYBACK], 1);
			if (p->has_capture)
				snd_pcm_set_timer_driven(
					&pcm->sub[AUDIO_DIR_CAPTURE], 1);
		}
		/* AUDIO_GETPCMINFO: the first playback device is the card's
		 * general one (output 0); a later jack device carries the
		 * headphone output id so the audio service moves there when
		 * headphones are plugged in, and HDMI devices their own. */
		if (p->has_playback) {
			if (!first_play_done) {
				first_play_done = 1;
			} else if (p->output_type == AUDIO_OUT_HEADPHONE ||
				   p->output_type == AUDIO_OUT_HEADSET ||
				   p->output_type == AUDIO_OUT_LINE) {
				pcm->output_id = SOF_OUT_ID_HEADPHONE;
				pcm->output_type = AUDIO_OUT_HEADPHONE;
			} else if (p->output_type == AUDIO_OUT_HDMI ||
				   p->output_type == AUDIO_OUT_DP) {
				pcm->output_id = SOF_OUT_ID_HDMI_BASE + hdmi_n++;
				pcm->output_type = p->output_type;
			}
		}
		if (p->has_playback)
			flags |= AUDIO_INFO_PLAYBACK;
		if (p->has_capture)
			flags |= AUDIO_INFO_CAPTURE;
		if (p->output_type == AUDIO_OUT_HDMI ||
		    p->output_type == AUDIO_OUT_DP)
			flags |= AUDIO_INFO_HDMI;
	}

	{
		struct audio_outputs *o = kcalloc(1, sizeof(*o));
		struct audio_volume v;

		if (o) {
			if (sdw_codec_get_outputs(&h->s, o) == 0 && o->count) {
				card->num_outputs = o->count > AUDIO_MAX_OUTPUTS ?
							    AUDIO_MAX_OUTPUTS :
							    o->count;
				for (j = 0; j < (int)card->num_outputs; j++)
					if (o->out[j].flags & AUDIO_OUTF_JACK_DETECT)
						flags |= AUDIO_INFO_JACK_DETECT;
			} else {
				sof_tplg_outputs(h, o);
				card->num_outputs = o->count;
			}
			kfree(o);
		}
		kmemset(&v, 0, sizeof(v));
		if (sdw_codec_get_volume(&h->s, AUDIO_CTL_MASTER, &v) == 0 &&
		    v.steps)
			flags |= AUDIO_INFO_HW_VOLUME;
		for (j = 0; j < cs->npd; j++)
			if (cs->pd[j]->pcm &&
			    cs->pd[j]->pcm->hw[AUDIO_DIR_CAPTURE].formats)
				card->num_inputs++;
	}
	card->info_flags = flags;

	rc = snd_card_register(card);
	if (rc) {
		sof_log(h, "card registration failed: %d\n", rc);
		goto fail;
	}
	h->s.card = card;
	sof_log(h, "card '%s' with %d PCM device%s\n", card->name, cs->npd,
		cs->npd == 1 ? "" : "s");
	return 0;

fail:
	if (card)
		snd_card_free(card);
	for (i = 0; i < cs->npd; i++)
		kfree(cs->pd[i]);
	kfree(cs);
	h->s.pcm_priv = NULL;
	return rc ? rc : -ENODEV;
}

void sof_card_destroy(struct sof_host *h)
{
	struct sof_card_state *cs = sof_card_state_of(h);
	int i;

	if (h->s.card) {
		snd_card_disconnect(h->s.card);
		h->s.card = NULL;
	}
	if (!cs)
		return;
	for (i = 0; i < cs->npd; i++) {
		if (!cs->pd[i])
			continue;
		sof_pcm_teardown(cs->pd[i], AUDIO_DIR_PLAYBACK);
		sof_pcm_teardown(cs->pd[i], AUDIO_DIR_CAPTURE);
		/* the core may still reference the PCM: keep pd alive */
	}
}
