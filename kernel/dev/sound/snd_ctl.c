// LikeOS sound core - the control half of the native interface: card
// information, volume, mute, output/input routing, the event queue and the
// diagnostic dump.
//
// These requests work on every node of a card (/dev/mixerN as well as the
// PCM nodes) and reach the driver through its struct snd_ctl_ops.  Every
// driver call is serialised by the card's control mutex, values are
// range-checked before the driver sees them, and every structure handed
// back is built from zeroed memory, so nothing the caller did not ask for
// leaks out.
//
// Copyright (C) 2026 The LikeOS Project

#include "snd_internal.h"

#include <kernel/uapi/ioctl.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

/* ======================================================================
 * Driver call wrappers
 * ====================================================================== */

static int snd_ctl_enter(struct snd_card *card)
{
	if (card->disconnected)
		return -ENODEV;
	if (!card->ctl)
		return -ENOENT;
	return snd_mutex_lock(&snd_card_priv(card)->ctl_mtx, 1);
}

static void snd_ctl_leave(struct snd_card *card)
{
	snd_mutex_unlock(&snd_card_priv(card)->ctl_mtx);
}

static uint32_t snd_clamp100(uint32_t v)
{
	return v > 100 ? 100 : v;
}

int snd_ctl_get_volume(struct snd_card *card, uint32_t ctl,
		       struct audio_volume *v)
{
	int rc;

	mm_memset(v, 0, sizeof(*v));
	v->control = ctl;
	if (ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	rc = snd_ctl_enter(card);
	if (rc)
		return rc;
	rc = card->ctl->get_volume ? card->ctl->get_volume(card, ctl, v) : -ENOENT;
	snd_ctl_leave(card);
	/* Whatever the driver did with the rest, the answer is about the
	 * control that was asked for and within range. */
	v->control = ctl;
	v->left = snd_clamp100(v->left);
	v->right = snd_clamp100(v->right);
	mm_memset(v->reserved, 0, sizeof(v->reserved));
	return rc;
}

int snd_ctl_set_volume(struct snd_card *card, uint32_t ctl, uint32_t left,
		       uint32_t right)
{
	int rc;

	if (ctl > AUDIO_CTL_MAX || left > 100 || right > 100)
		return -EINVAL;
	rc = snd_ctl_enter(card);
	if (rc)
		return rc;
	rc = card->ctl->set_volume ? card->ctl->set_volume(card, ctl, left, right) :
				     -ENOENT;
	snd_ctl_leave(card);
	if (rc == 0)
		snd_card_event(card, AUDIO_EV_VOLUME, ctl, left | (right << 8));
	return rc;
}

int snd_ctl_get_mute(struct snd_card *card, uint32_t ctl, uint32_t *mute)
{
	int rc;

	*mute = 0;
	if (ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	rc = snd_ctl_enter(card);
	if (rc)
		return rc;
	rc = card->ctl->get_mute ? card->ctl->get_mute(card, ctl, mute) : -ENOENT;
	snd_ctl_leave(card);
	*mute = *mute ? 1 : 0;
	return rc;
}

static int snd_ctl_set_mute(struct snd_card *card, uint32_t ctl, uint32_t mute)
{
	int rc;

	if (ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	mute = mute ? 1 : 0;
	rc = snd_ctl_enter(card);
	if (rc)
		return rc;
	rc = card->ctl->set_mute ? card->ctl->set_mute(card, ctl, mute) : -ENOENT;
	snd_ctl_leave(card);
	if (rc == 0)
		snd_card_event(card, AUDIO_EV_VOLUME, ctl, mute ? 0x10000u : 0);
	return rc;
}

/* Make a driver-filled output list safe to hand out. */
static void snd_outputs_sanitize(struct audio_outputs *o)
{
	if (o->count > AUDIO_MAX_OUTPUTS)
		o->count = AUDIO_MAX_OUTPUTS;
	if (o->mode > AUDIO_OUTSEL_ALL)
		o->mode = AUDIO_OUTSEL_AUTO;
	o->reserved = 0;
	for (uint32_t i = 0; i < AUDIO_MAX_OUTPUTS; i++) {
		struct audio_output_desc *d = &o->out[i];
		if (i >= o->count) {
			mm_memset(d, 0, sizeof(*d));
			continue;
		}
		d->name[sizeof(d->name) - 1] = 0;
		d->monitor[sizeof(d->monitor) - 1] = 0;
		/* Bytes after the terminator are zeroed too. */
		snd_strlcpy(d->name, d->name, sizeof(d->name));
		snd_strlcpy(d->monitor, d->monitor, sizeof(d->monitor));
		d->present = d->present ? 1 : 0;
		d->active = d->active ? 1 : 0;
		mm_memset(d->reserved, 0, sizeof(d->reserved));
	}
}

int snd_ctl_get_outputs(struct snd_card *card, struct audio_outputs *o,
			int inputs)
{
	int rc;

	mm_memset(o, 0, sizeof(*o));
	rc = snd_ctl_enter(card);
	if (rc)
		return rc;
	if (inputs)
		rc = card->ctl->get_inputs ? card->ctl->get_inputs(card, o) : -ENOENT;
	else
		rc = card->ctl->get_outputs ? card->ctl->get_outputs(card, o) :
					      -ENOENT;
	snd_ctl_leave(card);
	snd_outputs_sanitize(o);
	if (rc)
		mm_memset(o, 0, sizeof(*o));
	return rc;
}

int snd_ctl_set_output(struct snd_card *card, const struct audio_setoutput *s,
		       int inputs)
{
	struct audio_setoutput k;
	int rc;

	if (s->mode > AUDIO_OUTSEL_ALL)
		return -EINVAL;
	k = *s;
	mm_memset(k.reserved, 0, sizeof(k.reserved));
	if (k.mode != AUDIO_OUTSEL_ID)
		k.id = 0;
	rc = snd_ctl_enter(card);
	if (rc)
		return rc;
	if (inputs)
		rc = card->ctl->set_input ? card->ctl->set_input(card, &k) : -ENOENT;
	else
		rc = card->ctl->set_output ? card->ctl->set_output(card, &k) : -ENOENT;
	snd_ctl_leave(card);
	if (rc == 0)
		snd_card_event(card, AUDIO_EV_OUTPUT, k.id,
			       k.mode | (inputs ? 0x100u : 0));
	return rc;
}

/* ======================================================================
 * Information
 * ====================================================================== */

/* The PCM device the card's information describes: the handle's own, else
 * the first one with playback, else the first one at all. */
static struct snd_pcm *snd_info_pcm(struct snd_card *card, struct snd_pcm *pcm)
{
	if (pcm)
		return pcm;
	for (int i = 0; i < card->npcm && i < SND_MAX_PCMS; i++)
		if (card->pcm[i] && card->pcm[i]->hw[AUDIO_DIR_PLAYBACK].formats)
			return card->pcm[i];
	for (int i = 0; i < card->npcm && i < SND_MAX_PCMS; i++)
		if (card->pcm[i])
			return card->pcm[i];
	return NULL;
}

void snd_ctl_fill_info(struct snd_card *card, struct snd_pcm *pcm,
		       struct audio_info *info)
{
	struct audio_outputs *o;

	mm_memset(info, 0, sizeof(*info));
	info->version = AUDIO_API_VERSION;
	info->card = card->index < 0 ? 0 : (uint32_t)card->index;
	snd_strlcpy(info->card_name, card->name, sizeof(info->card_name));
	snd_strlcpy(info->driver, card->driver, sizeof(info->driver));
	snd_strlcpy(info->codec, card->codec, sizeof(info->codec));
	snd_strlcpy(info->bus, card->bus, sizeof(info->bus));
	info->flags = card->info_flags;

	pcm = snd_info_pcm(card, pcm);
	if (pcm) {
		const struct snd_pcm_hw *ph = &pcm->hw[AUDIO_DIR_PLAYBACK];
		const struct snd_pcm_hw *ch = &pcm->hw[AUDIO_DIR_CAPTURE];
		const struct snd_pcm_hw *main = ph->formats ? ph : ch;

		info->formats = ph->formats;
		info->rates = ph->formats ? ph->rates : 0;
		info->min_channels = ph->formats ? ph->channels_min : 0;
		info->max_channels = ph->formats ? ph->channels_max : 0;
		info->capture_formats = ch->formats;
		info->capture_rates = ch->formats ? ch->rates : 0;
		info->capture_min_channels = ch->formats ? ch->channels_min : 0;
		info->capture_max_channels = ch->formats ? ch->channels_max : 0;
		info->min_period_bytes = main->period_bytes_min;
		info->max_period_bytes = main->period_bytes_max;
		info->min_periods = main->periods_min;
		info->max_periods = main->periods_max;
		info->max_buffer_bytes = main->buffer_bytes_max;
		if (ph->formats)
			info->flags |= AUDIO_INFO_PLAYBACK;
		else
			info->flags &= ~AUDIO_INFO_PLAYBACK;
		if (ch->formats)
			info->flags |= AUDIO_INFO_CAPTURE;
		else
			info->flags &= ~AUDIO_INFO_CAPTURE;
		info->flags |= AUDIO_INFO_MMAP;
	}

	/* The counts agree with what AUDIO_GETOUTPUTS/GETINPUTS return. */
	info->num_outputs = card->num_outputs;
	info->num_inputs = card->num_inputs;
	o = kalloc(sizeof(*o));
	if (o) {
		if (card->ctl && card->ctl->get_outputs) {
			if (snd_ctl_get_outputs(card, o, 0) == 0)
				info->num_outputs = o->count;
			else
				info->num_outputs = 0;
		}
		if (card->ctl && card->ctl->get_inputs) {
			if (snd_ctl_get_outputs(card, o, 1) == 0)
				info->num_inputs = o->count;
			else
				info->num_inputs = 0;
		}
		kfree(o);
	}
	if (info->num_outputs > AUDIO_MAX_OUTPUTS)
		info->num_outputs = AUDIO_MAX_OUTPUTS;
	if (info->num_inputs > AUDIO_MAX_OUTPUTS)
		info->num_inputs = AUDIO_MAX_OUTPUTS;
	mm_memset(info->reserved, 0, sizeof(info->reserved));
}

/* ======================================================================
 * Events
 * ====================================================================== */

int snd_ctl_event_pending(struct snd_file *f)
{
	struct snd_card *card = f->card;
	return __atomic_load_n(&card->ev_head, __ATOMIC_ACQUIRE) != f->ev_seq;
}

int snd_ctl_next_event(struct snd_file *f, struct audio_event *ev)
{
	struct snd_card *card = f->card;
	uint64_t fl;
	int rc = -EAGAIN;

	mm_memset(ev, 0, sizeof(*ev));
	spin_lock_irqsave(&card->ev_lock, &fl);
	/* Fell more than a queue behind: the oldest events are gone; carry
	 * on with the oldest still held. */
	if (card->ev_head - f->ev_seq > SND_EVENT_QUEUE)
		f->ev_seq = card->ev_head - SND_EVENT_QUEUE;
	if (f->ev_seq != card->ev_head) {
		*ev = card->ev[f->ev_seq % SND_EVENT_QUEUE];
		f->ev_seq++;
		rc = 0;
	}
	spin_unlock_irqrestore(&card->ev_lock, fl);
	ev->reserved = 0;
	return rc;
}

/* ======================================================================
 * Diagnostics
 * ====================================================================== */

static void snd_core_diag(struct snd_card *card, uint32_t what,
			  struct snd_text *t)
{
	struct snd_card_priv *cp = snd_card_priv(card);

	if (what == AUDIO_DIAG_SUMMARY) {
		snd_text_printf(t, "card%d: %s\n", card->index, card->name);
		snd_text_printf(t, "  driver %s, codec %s, bus %s\n", card->driver,
				card->codec[0] ? card->codec : "-", card->bus);
		snd_text_printf(t, "  nodes:%s%s%s%s%s%s\n",
				card->path_audio[0] ? " " : "", card->path_audio,
				card->path_dsp[0] ? " " : "", card->path_dsp,
				card->path_mixer[0] ? " " : "", card->path_mixer);
		snd_text_printf(t, "  flags 0x%x, outputs %u, inputs %u, open handles %d%s\n",
				card->info_flags, card->num_outputs,
				card->num_inputs, cp->open_files,
				card->disconnected ? ", DISCONNECTED" : "");
		snd_text_printf(t, "  events queued since start %u, mixer changes %u\n",
				card->ev_head, card->modify_counter);
	}
	if (what == AUDIO_DIAG_CODECS)
		snd_text_printf(t, "card%d: %s: codec %s\n", card->index,
				card->name, card->codec[0] ? card->codec : "-");
	if (what == AUDIO_DIAG_SUMMARY || what == AUDIO_DIAG_STREAMS)
		snd_pcm_diag(card, t);
}

static long snd_ctl_diag(struct snd_card *card, void *argp)
{
	struct audio_diag d;
	struct snd_text t;
	char *kbuf;
	uint32_t cap;
	long rc;

	rc = snd_user_in(&d, argp, sizeof(d));
	if (rc)
		return rc;
	if (d.what > AUDIO_DIAG_STREAMS)
		return -EINVAL;
	d.written = 0;
	mm_memset(d.reserved, 0, sizeof(d.reserved));
	if (d.len == 0)
		return snd_user_out(argp, &d, sizeof(d));
	cap = d.len > SND_DIAG_MAX_BYTES ? SND_DIAG_MAX_BYTES : d.len;
	if (!validate_user_ptr(d.buf, cap))
		return -EFAULT;

	kbuf = kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	mm_memset(kbuf, 0, cap);
	t.buf = kbuf;
	t.cap = cap;
	t.len = 0;

	snd_core_diag(card, d.what, &t);

	/* The driver's part goes after the core's, into what is left. */
	if (card->ctl && card->ctl->diag && !card->disconnected &&
	    t.len + 1 < t.cap) {
		if (snd_mutex_lock(&snd_card_priv(card)->ctl_mtx, 1) == 0) {
			size_t room = t.cap - t.len;
			long n = card->ctl->diag(card, d.what, kbuf + t.len, room);
			snd_mutex_unlock(&snd_card_priv(card)->ctl_mtx);
			if (n > 0) {
				if ((size_t)n >= room)
					n = (long)room - 1;
				t.len += (size_t)n;
			}
		}
	}
	if (t.len > cap)
		t.len = cap;

	rc = snd_user_out((void *)d.buf, kbuf, t.len);
	kfree(kbuf);
	if (rc)
		return rc;
	d.written = (uint32_t)t.len;
	return snd_user_out(argp, &d, sizeof(d));
}

/* ======================================================================
 * Dispatch
 * ====================================================================== */

long snd_ctl_ioctl(struct snd_file *f, unsigned long req, void *argp)
{
	struct snd_card *card = f->card;
	long rc;

	switch (req) {
	case AUDIO_GETVERSION: {
		uint32_t v = AUDIO_API_VERSION;
		return snd_user_out(argp, &v, sizeof(v));
	}
	case AUDIO_GETINFO: {
		struct audio_info *info = kalloc(sizeof(*info));
		if (!info)
			return -ENOMEM;
		snd_ctl_fill_info(card, f->pcm, info);
		rc = snd_user_out(argp, info, sizeof(*info));
		kfree(info);
		return rc;
	}
	case AUDIO_GETVOL: {
		struct audio_volume v;
		rc = snd_user_in(&v, argp, sizeof(v));
		if (rc)
			return rc;
		uint32_t ctl = v.control;
		if (ctl > AUDIO_CTL_MAX)
			return -EINVAL;
		rc = snd_ctl_get_volume(card, ctl, &v);
		if (rc)
			return rc;
		return snd_user_out(argp, &v, sizeof(v));
	}
	case AUDIO_SETVOL: {
		struct audio_volume v;
		rc = snd_user_in(&v, argp, sizeof(v));
		if (rc)
			return rc;
		uint32_t ctl = v.control;
		if (ctl > AUDIO_CTL_MAX || v.left > 100 || v.right > 100)
			return -EINVAL;
		rc = snd_ctl_set_volume(card, ctl, v.left, v.right);
		if (rc)
			return rc;
		/* Report what the hardware actually took (step rounding). */
		struct audio_volume now;
		if (snd_ctl_get_volume(card, ctl, &now) == 0)
			v = now;
		else
			mm_memset(v.reserved, 0, sizeof(v.reserved));
		return snd_user_out(argp, &v, sizeof(v));
	}
	case AUDIO_GETMUTE: {
		struct audio_mute m;
		rc = snd_user_in(&m, argp, sizeof(m));
		if (rc)
			return rc;
		uint32_t ctl = m.control;
		if (ctl > AUDIO_CTL_MAX)
			return -EINVAL;
		mm_memset(&m, 0, sizeof(m));
		m.control = ctl;
		rc = snd_ctl_get_mute(card, ctl, &m.mute);
		if (rc)
			return rc;
		return snd_user_out(argp, &m, sizeof(m));
	}
	case AUDIO_SETMUTE: {
		struct audio_mute m;
		rc = snd_user_in(&m, argp, sizeof(m));
		if (rc)
			return rc;
		if (m.control > AUDIO_CTL_MAX || m.mute > 1)
			return -EINVAL;
		return snd_ctl_set_mute(card, m.control, m.mute);
	}
	case AUDIO_GETOUTPUTS:
	case AUDIO_GETINPUTS: {
		struct audio_outputs *o = kalloc(sizeof(*o));
		if (!o)
			return -ENOMEM;
		rc = snd_ctl_get_outputs(card, o, req == AUDIO_GETINPUTS);
		if (rc == 0)
			rc = snd_user_out(argp, o, sizeof(*o));
		kfree(o);
		return rc;
	}
	case AUDIO_SETOUTPUT:
	case AUDIO_SETINPUT: {
		struct audio_setoutput s;
		rc = snd_user_in(&s, argp, sizeof(s));
		if (rc)
			return rc;
		return snd_ctl_set_output(card, &s, req == AUDIO_SETINPUT);
	}
	case AUDIO_GETEVENT: {
		struct audio_event ev;
		rc = snd_ctl_next_event(f, &ev);
		if (rc)
			return rc;
		return snd_user_out(argp, &ev, sizeof(ev));
	}
	case AUDIO_DIAG:
		return snd_ctl_diag(card, argp);
	default:
		return -ENOTTY;
	}
}
