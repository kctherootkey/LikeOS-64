// LikeOS sound core - PCM streams: parameter negotiation, the DMA ring,
// position tracking, underrun handling, the read/write/poll/mmap data paths
// and the native AUDIO_* stream requests.
//
// Model.  A substream owns one ring of `buffer_frames' frames.  Two
// monotonically growing 64-bit counters describe it:
//
//   hw_frames    frames the hardware has consumed (playback) or produced
//                (capture), derived from the driver's DMA position
//   appl_frames  frames the application has written (playback) or read
//                (capture)
//
// The position inside the ring of either counter is the counter modulo
// buffer_frames.  For playback, appl - hw frames are queued and the rest of
// the ring is free; for capture, hw - appl frames wait to be read.  When
// the hardware overtakes the application (playback) or laps it (capture)
// the stream is in XRUN: it is stopped, the condition is counted and
// reported as an event, and the next write/read (or AUDIO_PREPARE) starts
// it again from a clean ring.
//
// The DMA position only says where in the ring the hardware is, not how
// many times it went round.  Updates normally come once a period (the
// driver's interrupt), so a forward step of less than one ring is
// unambiguous; if interrupts were held off for longer than a whole ring the
// elapsed time tells how many laps were missed.
//
// Locking.  ss->lock (spinlock, interrupts off) covers the counters and
// the state; the driver's trigger/pointer run under it.  rt->mtx (a sleeping
// lock) serialises the control path (parameters, prepare, start, drain);
// rt->io_mtx serialises readers/writers of one substream.  Lock order:
// io_mtx, then mtx, then ss->lock.
//
// Copyright (C) 2026 The LikeOS Project

#include "snd_internal.h"

#include <kernel/uapi/ioctl.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

#define NSEC_PER_SEC 1000000000ULL

/* Update result bits. */
#define UPD_MOVED (1u << 0)
#define UPD_XRUN (1u << 1)
#define UPD_DRAINED (1u << 2)
#define UPD_WAKE (1u << 3)
#define UPD_BOGUS (1u << 4)

#define SND_PARAM_KNOWN_FLAGS (AUDIO_PARAM_NO_AUTOSTART | AUDIO_PARAM_MMAP)

static void snd_pcm_timer_fn(hrtimer_t *t);

/* ======================================================================
 * Names
 * ====================================================================== */

const char *snd_pcm_state_name(uint32_t state)
{
	static const char *const names[] = {
		"OPEN",	    "SETUP",	"PREPARED", "RUNNING",
		"PAUSED",   "DRAINING", "XRUN",	    "DISCONNECTED",
	};
	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

static const char *snd_format_name(uint32_t fmt)
{
	switch (fmt) {
	case AUDIO_FMT_S16_LE: return "S16_LE";
	case AUDIO_FMT_S24_LE: return "S24_LE";
	case AUDIO_FMT_S32_LE: return "S32_LE";
	case AUDIO_FMT_U8: return "U8";
	case AUDIO_FMT_S24_3LE: return "S24_3LE";
	case AUDIO_FMT_FLOAT_LE: return "FLOAT_LE";
	case AUDIO_FMT_S20_LE: return "S20_LE";
	default: return "none";
	}
}

/* ======================================================================
 * Capabilities and negotiation
 * ====================================================================== */

static const uint32_t g_rate_table[] = {
	8000, 11025, 16000, 22050, 32000, 44100,
	48000, 88200, 96000, 176400, 192000, 384000,
};
#define SND_NRATES (sizeof(g_rate_table) / sizeof(g_rate_table[0]))

#define SND_FORMAT_MASK (((1u << (AUDIO_FMT_MAX + 1)) - 1) & ~1u)

static uint32_t snd_gcd(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}

/* Least common multiple, 0 when it does not fit in 32 bits. */
static uint32_t snd_lcm(uint32_t a, uint32_t b)
{
	if (!a || !b)
		return a ? a : b;
	uint64_t l = (uint64_t)(a / snd_gcd(a, b)) * b;
	return l > 0xffffffffULL ? 0 : (uint32_t)l;
}

/* Bring a driver's description into a shape the negotiation can trust. */
static void snd_hw_sanitize(struct snd_pcm_hw *hw, const char *what)
{
	if (!hw->formats)
		return;
	if (hw->formats & ~SND_FORMAT_MASK) {
		kprintf("[snd] core: %s: unknown format bits 0x%x ignored\n", what,
			hw->formats & ~SND_FORMAT_MASK);
		hw->formats &= SND_FORMAT_MASK;
		if (!hw->formats) {
			hw->formats = 0;
			return;
		}
	}
	hw->rates &= (1u << SND_NRATES) - 1;
	if (!hw->rates) {
		kprintf("[snd] core: %s: no rates given, assuming 48 kHz\n", what);
		hw->rates = AUDIO_RATE_48000;
	}
	if (hw->channels_min == 0)
		hw->channels_min = 1;
	if (hw->channels_min > SND_MAX_CHANNELS)
		hw->channels_min = SND_MAX_CHANNELS;
	if (hw->channels_max < hw->channels_min)
		hw->channels_max = hw->channels_min;
	if (hw->channels_max > SND_MAX_CHANNELS)
		hw->channels_max = SND_MAX_CHANNELS;
	if (hw->period_bytes_min < 16)
		hw->period_bytes_min = hw->period_bytes_min ? 16 : 64;
	if (hw->period_bytes_max == 0)
		hw->period_bytes_max = 128 * 1024;
	if (hw->period_bytes_max > SND_RING_MAX_BYTES / 2)
		hw->period_bytes_max = SND_RING_MAX_BYTES / 2;
	if (hw->period_bytes_max < hw->period_bytes_min)
		hw->period_bytes_max = hw->period_bytes_min;
	if (hw->periods_min == 0)
		hw->periods_min = 2;
	if (hw->periods_max == 0)
		hw->periods_max = 1024;
	if (hw->periods_max < hw->periods_min)
		hw->periods_max = hw->periods_min;
	if (hw->buffer_bytes_max == 0)
		hw->buffer_bytes_max = 4u * 1024u * 1024u;
	if (hw->buffer_bytes_max > SND_RING_MAX_BYTES)
		hw->buffer_bytes_max = SND_RING_MAX_BYTES;
	{
		uint64_t need = (uint64_t)hw->period_bytes_min * hw->periods_min;
		if (need > hw->buffer_bytes_max) {
			kprintf("[snd] core: %s: smallest buffer (%llu bytes) exceeds the maximum; maximum raised\n",
				what, (unsigned long long)need);
			hw->buffer_bytes_max = need > SND_RING_MAX_BYTES ?
						       SND_RING_MAX_BYTES :
						       (uint32_t)need;
		}
	}
	if (hw->buffer_align == 0)
		hw->buffer_align = 1;
	if (hw->period_align == 0)
		hw->period_align = 1;
	if (hw->buffer_align > 64 * 1024)
		hw->buffer_align = 64 * 1024;
	if (hw->period_align > 64 * 1024)
		hw->period_align = 64 * 1024;
}

uint32_t snd_pcm_pick_format(const struct snd_pcm_hw *hw, uint32_t want)
{
	/* Preference when the wanted format is missing: something at least
	 * as wide first (no precision lost), then narrower. */
	static const uint32_t wider[] = { AUDIO_FMT_S32_LE, AUDIO_FMT_S24_LE,
					  AUDIO_FMT_S24_3LE, AUDIO_FMT_S20_LE,
					  AUDIO_FMT_S16_LE, AUDIO_FMT_U8,
					  AUDIO_FMT_FLOAT_LE };
	static const uint32_t common[] = { AUDIO_FMT_S16_LE, AUDIO_FMT_S32_LE,
					   AUDIO_FMT_S24_LE, AUDIO_FMT_S24_3LE,
					   AUDIO_FMT_S20_LE, AUDIO_FMT_U8,
					   AUDIO_FMT_FLOAT_LE };
	uint32_t wb;

	if (!hw || !hw->formats)
		return 0;
	if (want >= 1 && want <= AUDIO_FMT_MAX && (hw->formats & AUDIO_FMT_BIT(want)))
		return want;
	wb = audio_format_bytes(want);
	if (wb > 2 && want != AUDIO_FMT_FLOAT_LE) {
		for (unsigned i = 0; i < sizeof(wider) / sizeof(wider[0]); i++)
			if (hw->formats & AUDIO_FMT_BIT(wider[i]))
				return wider[i];
	}
	for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); i++)
		if (hw->formats & AUDIO_FMT_BIT(common[i]))
			return common[i];
	return 0;
}

uint32_t snd_pcm_pick_rate(const struct snd_pcm_hw *hw, uint32_t want)
{
	uint32_t best = 0, best_diff = 0xffffffffu;

	if (!hw || !hw->rates)
		return 0;
	if (want == 0)
		want = 48000;
	for (unsigned i = 0; i < SND_NRATES; i++) {
		if (!(hw->rates & (1u << i)))
			continue;
		uint32_t r = g_rate_table[i];
		uint32_t d = r > want ? r - want : want - r;
		/* Ties go to the higher rate: up-sampling loses nothing. */
		if (d < best_diff || (d == best_diff && r > best)) {
			best = r;
			best_diff = d;
		}
	}
	return best;
}

uint32_t snd_pcm_pick_channels(const struct snd_pcm_hw *hw, uint32_t want)
{
	if (!hw || !hw->formats)
		return 0;
	if (want == 0)
		want = 2;
	if (want < hw->channels_min)
		return hw->channels_min;
	if (want > hw->channels_max)
		return hw->channels_max;
	return want;
}

static uint64_t snd_round_to(uint64_t v, uint32_t unit)
{
	return ((v + unit / 2) / unit) * unit;
}

int snd_pcm_refine(const struct snd_pcm_hw *hw, struct audio_params *p,
		   uint32_t *period_bytes, uint32_t *periods,
		   uint32_t *buffer_bytes)
{
	uint32_t fmt, rate, ch, fb, unit, lo, hi, nper, bmax;
	uint64_t pb, buf;

	if (!hw || !hw->formats)
		return -EINVAL;
	fmt = snd_pcm_pick_format(hw, p->format);
	rate = snd_pcm_pick_rate(hw, p->rate);
	ch = snd_pcm_pick_channels(hw, p->channels);
	if (!fmt || !rate || !ch)
		return -EINVAL;
	fb = audio_format_bytes(fmt) * ch;
	if (!fb)
		return -EINVAL;

	/* A period is a whole number of frames and a multiple of the
	 * driver's period alignment. */
	unit = snd_lcm(fb, hw->period_align);
	if (!unit || unit > hw->period_bytes_max)
		return -EINVAL;
	lo = ((hw->period_bytes_min + unit - 1) / unit) * unit;
	hi = (hw->period_bytes_max / unit) * unit;
	bmax = hw->buffer_bytes_max;
	if (bmax > SND_RING_MAX_BYTES)
		bmax = SND_RING_MAX_BYTES;

	/* The ring is a whole number of periods; when the ring must also be
	 * a multiple of buffer_align, making each period one is the simple
	 * way to get there. */
	if (hw->buffer_align > 1 && (hw->periods_min > 1 || hw->buffer_align > unit)) {
		uint32_t u2 = snd_lcm(unit, hw->buffer_align);
		if (u2 && u2 <= hi) {
			unit = u2;
			lo = ((hw->period_bytes_min + unit - 1) / unit) * unit;
			hi = (hw->period_bytes_max / unit) * unit;
		}
	}
	if (lo == 0)
		lo = unit;
	if (lo > hi)
		return -EINVAL;

	/* Period: what was asked for, else about 10 ms. */
	if (p->period_frames) {
		pb = (uint64_t)p->period_frames * fb;
	} else {
		uint32_t frames = rate / 100;
		if (frames < 32)
			frames = 32;
		pb = (uint64_t)frames * fb;
	}
	pb = snd_round_to(pb, unit);
	if (pb < lo)
		pb = lo;
	if (pb > hi)
		pb = hi;

	nper = p->periods ? p->periods : 4;
	if (nper < hw->periods_min)
		nper = hw->periods_min;
	if (nper > hw->periods_max)
		nper = hw->periods_max;

	buf = pb * nper;
	if (buf > bmax) {
		nper = (uint32_t)(bmax / pb);
		if (nper < hw->periods_min) {
			/* Smaller periods so the minimum count fits. */
			nper = hw->periods_min;
			pb = (bmax / nper / unit) * unit;
			if (pb < lo)
				return -EINVAL;
		}
		buf = pb * nper;
	}
	if (buf == 0 || buf > bmax)
		return -EINVAL;
	if (hw->buffer_align > 1 && (buf % hw->buffer_align)) {
		/* Drop periods until the ring is aligned (the case where the
		 * period itself could not be made a multiple). */
		while (nper > hw->periods_min && ((pb * nper) % hw->buffer_align))
			nper--;
		buf = pb * nper;
		if (buf % hw->buffer_align)
			return -EINVAL;
	}

	p->format = fmt;
	p->rate = rate;
	p->channels = ch;
	p->period_frames = (uint32_t)(pb / fb);
	p->periods = nper;
	p->buffer_bytes = (uint32_t)buf;
	p->frame_bytes = fb;
	p->flags &= SND_PARAM_KNOWN_FLAGS;
	if (period_bytes)
		*period_bytes = (uint32_t)pb;
	if (periods)
		*periods = nper;
	if (buffer_bytes)
		*buffer_bytes = (uint32_t)buf;
	return 0;
}

/* ======================================================================
 * Creating PCM devices
 * ====================================================================== */

int snd_pcm_new(struct snd_card *card, const char *name,
		const struct snd_pcm_ops *ops, const struct snd_pcm_hw *play,
		const struct snd_pcm_hw *capture, void *private_data,
		struct snd_pcm **out)
{
	struct snd_pcm_priv *pp;
	struct snd_pcm *pcm;
	char what[48];

	if (out)
		*out = NULL;
	if (!card || !ops)
		return -EINVAL;
	if (snd_card_priv(card)->magic != SND_CARD_MAGIC)
		return -EINVAL;
	if (card->registered)
		return -EBUSY;
	if (card->npcm >= SND_MAX_PCMS)
		return -ENOSPC;
	if ((!play || !play->formats) && (!capture || !capture->formats))
		return -EINVAL;
	if (!ops->pointer || !ops->trigger) {
		kprintf("[snd] %s: PCM \"%s\" lacks pointer/trigger operations\n",
			card->driver, name ? name : "?");
		return -EINVAL;
	}

	pp = kalloc(sizeof(*pp));
	if (!pp)
		return -ENOMEM;
	mm_memset(pp, 0, sizeof(*pp));
	pcm = &pp->pcm;
	pcm->card = card;
	pcm->device = card->npcm;
	pcm->ops = ops;
	pcm->private_data = private_data;
	snd_strlcpy(pcm->name, name ? name : card->name, sizeof(pcm->name));
	if (play)
		pcm->hw[AUDIO_DIR_PLAYBACK] = *play;
	if (capture)
		pcm->hw[AUDIO_DIR_CAPTURE] = *capture;
	for (int d = 0; d < 2; d++) {
		ksnprintf(what, sizeof(what), "%s pcm%d %s", card->driver,
			  pcm->device, d ? "capture" : "playback");
		snd_hw_sanitize(&pcm->hw[d], what);

		struct snd_pcm_substream *ss = &pcm->sub[d];
		struct snd_ss_rt *rt = &pp->rt[d];
		ss->pcm = pcm;
		ss->direction = d;
		spinlock_init(&ss->lock, d ? "snd_cap" : "snd_play");
		wq_head_init(&ss->wq, "snd_pcm");
		ss->state = AUDIO_STATE_OPEN;
		rt->ss = ss;
		snd_mutex_init(&rt->mtx, "snd_pcm_ctl");
		snd_mutex_init(&rt->io_mtx, "snd_pcm_io");
	}
	/* The timer callbacks find their runtime through the argument. */
	hrtimer_init(&pp->rt[0].timer, snd_pcm_timer_fn, &pp->rt[0]);
	hrtimer_init(&pp->rt[1].timer, snd_pcm_timer_fn, &pp->rt[1]);

	card->pcm[card->npcm++] = pcm;
	if (out)
		*out = pcm;
	return 0;
}

void snd_pcm_set_timer_driven(struct snd_pcm_substream *ss, int on)
{
	if (!ss || !ss->pcm)
		return;
	__atomic_store_n(&snd_rt(ss)->timer_driven, on ? 1 : 0, __ATOMIC_RELEASE);
}

/* ======================================================================
 * Position tracking
 * ====================================================================== */

static int snd_card_gone(struct snd_pcm_substream *ss)
{
	return __atomic_load_n(&ss->pcm->card->disconnected, __ATOMIC_ACQUIRE);
}

static int snd_trigger_locked(struct snd_pcm_substream *ss, int cmd)
{
	if (snd_card_gone(ss))
		return -ENODEV;
	return ss->pcm->ops->trigger(ss, cmd);
}

/* Frames the application may move now (lock held). */
static uint32_t snd_avail_locked(struct snd_pcm_substream *ss,
				 struct snd_ss_rt *rt)
{
	if (!rt->buffer_frames)
		return 0;
	if (ss->direction == AUDIO_DIR_PLAYBACK) {
		if (ss->appl_frames <= ss->hw_frames)
			return rt->buffer_frames;
		uint64_t q = ss->appl_frames - ss->hw_frames;
		return q >= rt->buffer_frames ? 0 : rt->buffer_frames - (uint32_t)q;
	}
	if (ss->hw_frames <= ss->appl_frames)
		return 0;
	uint64_t r = ss->hw_frames - ss->appl_frames;
	return r > rt->buffer_frames ? rt->buffer_frames : (uint32_t)r;
}

static int snd_running(uint32_t st)
{
	return st == AUDIO_STATE_RUNNING || st == AUDIO_STATE_DRAINING;
}

static void snd_timer_arm_locked(struct snd_pcm_substream *ss,
				 struct snd_ss_rt *rt, uint64_t now)
{
	uint64_t interval;

	if (!snd_running(ss->state) || !rt->period_ns)
		return;
	if (rt->timer_mode == SND_TIMER_PERIODIC) {
		interval = rt->period_ns;
	} else {
		interval = rt->period_ns * 4;
		if (interval < 50000000ULL)
			interval = 50000000ULL;
	}
	if (interval < 500000ULL)
		interval = 500000ULL; /* never faster than 2 kHz */
	uint64_t next = now + interval;
	if (ss->state == AUDIO_STATE_DRAINING && rt->drain_deadline_ns) {
		if (rt->drain_deadline_ns > now) {
			if (rt->drain_deadline_ns < next)
				next = rt->drain_deadline_ns;
		} else {
			/* Past the computed end and the hardware has not got
			 * there yet (FIFO, a slow emulation): look again
			 * soon. */
			uint64_t soon = rt->period_ns / 4;
			if (soon < 1000000ULL)
				soon = 1000000ULL;
			if (now + soon < next)
				next = now + soon;
		}
	}
	hrtimer_start(&rt->timer, next);
}

/* Read the hardware position and move hw_frames.  Lock held.  Returns
 * UPD_* bits; the caller posts events and wakes after unlocking.
 * `from_irq' is set when a period interrupt is the reason for the call. */
static uint32_t snd_update_locked(struct snd_pcm_substream *ss,
				  struct snd_ss_rt *rt, uint64_t now,
				  int from_irq)
{
	uint32_t bits = 0;
	uint32_t pos, delta_bytes, fb = ss->frame_bytes;
	uint64_t delta;

	if (!snd_running(ss->state) || !rt->buffer_frames || !fb ||
	    !ss->buffer_bytes)
		return 0;
	if (snd_card_gone(ss))
		return 0;

	pos = ss->pcm->ops->pointer(ss);
	if (pos >= ss->buffer_bytes) {
		/* All-ones is what a removed device reads as; anything else
		 * out of range is a driver or hardware glitch.  Either way
		 * the value cannot be used. */
		if (!rt->bogus_ptr_reported) {
			rt->bogus_ptr_reported = 1;
			bits |= UPD_BOGUS;
		}
		if (pos == 0xffffffffu)
			return bits;
		pos %= ss->buffer_bytes;
	}
	pos -= pos % fb;

	if (pos >= ss->last_hw_offset)
		delta_bytes = pos - ss->last_hw_offset;
	else
		delta_bytes = pos + ss->buffer_bytes - ss->last_hw_offset;
	delta = delta_bytes / fb;

	/* Laps the position alone cannot show.  The wall clock is not the
	 * device's clock -- an emulated card in particular runs behind the
	 * guest's time whenever the host is late -- so it only decides when
	 * nothing better is known:
	 *
	 *  - timer-driven (no period interrupts): if far more time went by
	 *    than the observed step accounts for, whole rings were missed;
	 *  - on a period interrupt: at least one period was played since the
	 *    last one, so a position short of the next period boundary means
	 *    the ring wrapped -- if half a ring's time has passed for it.
	 *
	 * Any other update takes the position as it is. */
	if (rt->last_update_ns && now > rt->last_update_ns && ss->params.rate) {
		uint64_t el = now - rt->last_update_ns;
		if (el > 10ULL * NSEC_PER_SEC)
			el = 10ULL * NSEC_PER_SEC;
		if (rt->timer_mode == SND_TIMER_PERIODIC) {
			uint64_t expected = el * ss->params.rate / NSEC_PER_SEC;
			if (expected > delta + rt->buffer_frames) {
				uint64_t k = (expected - delta +
					      rt->buffer_frames / 2) /
					     rt->buffer_frames;
				delta += k * rt->buffer_frames;
			}
		} else if (from_irq && rt->period_frames) {
			uint64_t ring_ns = (uint64_t)rt->buffer_frames *
					   NSEC_PER_SEC / ss->params.rate;
			if (ss->hw_frames + delta <
				    rt->irq_hw_frames + rt->period_frames &&
			    el > ring_ns / 2)
				delta += rt->buffer_frames;
		}
	}
	rt->last_update_ns = now;
	ss->last_hw_offset = pos;
	if (delta == 0)
		return bits;

	ss->hw_frames += delta;
	ss->hw_timestamp_ns = now;
	bits |= UPD_MOVED;
	if (from_irq && rt->period_frames)
		rt->irq_hw_frames =
			ss->hw_frames - ss->hw_frames % rt->period_frames;

	if (rt->free_run && ss->state == AUDIO_STATE_RUNNING) {
		/* The application keeps the mapped ring filled (or drained)
		 * on its own clock: the counters just follow the hardware. */
		if (ss->direction == AUDIO_DIR_PLAYBACK)
			ss->appl_frames = ss->hw_frames + rt->buffer_frames;
		else if (ss->hw_frames - ss->appl_frames > rt->buffer_frames)
			ss->appl_frames = ss->hw_frames - rt->buffer_frames;
	}

	if (ss->direction == AUDIO_DIR_PLAYBACK) {
		if (ss->hw_frames >= ss->appl_frames) {
			/* Everything queued has been played. */
			ss->hw_frames = ss->appl_frames;
			(void)snd_trigger_locked(ss, SND_TRIGGER_STOP);
			rt->need_prepare = 1;
			rt->drain_deadline_ns = 0;
			if (ss->state == AUDIO_STATE_DRAINING) {
				ss->state = AUDIO_STATE_SETUP;
				bits |= UPD_DRAINED | UPD_WAKE;
			} else {
				ss->state = AUDIO_STATE_XRUN;
				ss->xruns++;
				bits |= UPD_XRUN | UPD_WAKE;
			}
		}
	} else {
		if (ss->hw_frames - ss->appl_frames > rt->buffer_frames) {
			/* The hardware lapped the reader: data was lost. */
			(void)snd_trigger_locked(ss, SND_TRIGGER_STOP);
			rt->need_prepare = 1;
			ss->state = AUDIO_STATE_XRUN;
			ss->xruns++;
			bits |= UPD_XRUN | UPD_WAKE;
		}
	}

	uint32_t thresh = ss->avail_min_frames ? ss->avail_min_frames : 1;
	if (ss->hw_frames - rt->last_wake_frames >= thresh) {
		rt->last_wake_frames = ss->hw_frames;
		bits |= UPD_WAKE;
	}
	return bits;
}

static void snd_post_update(struct snd_pcm_substream *ss, uint32_t bits,
			    int wake)
{
	struct snd_card *card = ss->pcm->card;

	if (bits & UPD_BOGUS)
		kprintf("[snd] card%d: pcm%d %s: DMA position out of range (buffer %u bytes)\n",
			card->index, ss->pcm->device,
			ss->direction ? "capture" : "playback", ss->buffer_bytes);
	if (bits & UPD_XRUN)
		snd_card_event(card, AUDIO_EV_XRUN, (uint32_t)ss->pcm->device,
			       (uint32_t)ss->direction);
	if (wake || (bits & (UPD_WAKE | UPD_XRUN | UPD_DRAINED)))
		poll_notify_wq(&ss->wq);
}

void snd_pcm_period_elapsed(struct snd_pcm_substream *ss)
{
	struct snd_ss_rt *rt;
	uint32_t bits = 0;
	uint64_t fl, now;

	if (!ss || !ss->pcm)
		return;
	rt = snd_rt(ss);
	now = hrtimer_now_ns();
	spin_lock_irqsave(&ss->lock, &fl);
	if (!ss->opened || !snd_running(ss->state)) {
		spin_unlock_irqrestore(&ss->lock, fl);
		return;
	}
	rt->last_irq_ns = now;
	rt->irq_count++;
	bits = snd_update_locked(ss, rt, now, 1);
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_post_update(ss, bits, 1);
}

static void snd_pcm_timer_fn(hrtimer_t *t)
{
	struct snd_ss_rt *rt = t->arg;
	struct snd_pcm_substream *ss = rt->ss;
	uint32_t bits = 0;
	uint64_t fl, now;

	/* A stream runs on its period interrupts; this timer only keeps the
	 * position current between them, and drives it outright only for a
	 * driver that asked for that (snd_pcm_set_timer_driven).  Late or
	 * bunched interrupts -- an emulated card serviced on its host's
	 * schedule -- are not missing ones, and are never taken for them. */
	__atomic_fetch_add(&rt->timer_cb_active, 1, __ATOMIC_ACQ_REL);
	now = hrtimer_now_ns();
	spin_lock_irqsave(&ss->lock, &fl);
	if (ss->opened && snd_running(ss->state)) {
		bits = snd_update_locked(ss, rt, now, 0);
		snd_timer_arm_locked(ss, rt, now);
	}
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_post_update(ss, bits, rt->timer_mode == SND_TIMER_PERIODIC);
	__atomic_fetch_sub(&rt->timer_cb_active, 1, __ATOMIC_ACQ_REL);
}

/* Stop the timer from process context and make sure its callback is not
 * still running on another processor.  ss->lock must NOT be held. */
static void snd_timer_quiesce(struct snd_ss_rt *rt)
{
	hrtimer_cancel(&rt->timer);
	while (__atomic_load_n(&rt->timer_cb_active, __ATOMIC_ACQUIRE))
		__asm__ volatile("pause");
	/* A callback that was already past its state check may have re-armed
	 * just before finishing. */
	hrtimer_cancel(&rt->timer);
}

/* PREPARED -> RUNNING.  Lock held. */
static int snd_start_locked(struct snd_pcm_substream *ss, struct snd_ss_rt *rt)
{
	uint64_t now;
	int rc;

	if (ss->state != AUDIO_STATE_PREPARED)
		return -EINVAL;
	rc = snd_trigger_locked(ss, SND_TRIGGER_START);
	if (rc)
		return rc;
	now = hrtimer_now_ns();
	ss->state = AUDIO_STATE_RUNNING;
	ss->start_ns = now;
	ss->hw_timestamp_ns = now;
	rt->last_update_ns = now;
	rt->last_irq_ns = now;
	rt->irq_hw_frames = rt->period_frames ?
		ss->hw_frames - ss->hw_frames % rt->period_frames :
		ss->hw_frames;
	rt->timer_mode = rt->timer_driven ? SND_TIMER_PERIODIC :
					    SND_TIMER_WATCHDOG;
	snd_timer_arm_locked(ss, rt, now);
	return 0;
}

/* Playback: start once enough is queued.  Lock held. */
static void snd_autostart_locked(struct snd_pcm_substream *ss,
				 struct snd_ss_rt *rt, int gate)
{
	if (!gate || ss->state != AUDIO_STATE_PREPARED ||
	    (ss->flags & AUDIO_PARAM_NO_AUTOSTART))
		return;
	if (ss->direction == AUDIO_DIR_PLAYBACK) {
		uint32_t th = rt->start_threshold ? rt->start_threshold : 1;
		if (th > rt->buffer_frames)
			th = rt->buffer_frames;
		if (ss->appl_frames - ss->hw_frames >= th)
			(void)snd_start_locked(ss, rt);
	} else {
		(void)snd_start_locked(ss, rt);
	}
}

/* ======================================================================
 * Opening and closing
 * ====================================================================== */

int snd_pcm_claim(struct snd_file *f, struct snd_pcm_substream *ss,
		  uint32_t uid)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	const struct snd_pcm_ops *ops = ss->pcm->ops;
	uint64_t fl;
	int rc;

	if (!ss->pcm->hw[ss->direction].formats)
		return -ENODEV;
	spin_lock_irqsave(&ss->lock, &fl);
	if (ss->opened) {
		spin_unlock_irqrestore(&ss->lock, fl);
		return -EBUSY;
	}
	ss->opened = 1;
	ss->owner_uid = uid;
	ss->state = AUDIO_STATE_OPEN;
	ss->hw_frames = 0;
	ss->appl_frames = 0;
	ss->last_hw_offset = 0;
	ss->start_ns = 0;
	ss->hw_timestamp_ns = 0;
	ss->xruns = 0;
	ss->flags = 0;
	ss->avail_min_frames = 0;
	ss->oss_frag_shift = 0;
	ss->oss_frag_max = 0;
	ss->oss_trigger = 0;
	ss->oss_blocks = 0;
	ss->mmap_count = 0;
	mm_memset(&ss->params, 0, sizeof(ss->params));
	ss->params.direction = (uint32_t)ss->direction;
	rt->owner = f;
	rt->need_prepare = 0;
	rt->io_active = 0;
	rt->irq_count = 0;
	rt->timer_mode = SND_TIMER_OFF;
	rt->bogus_ptr_reported = 0;
	rt->drain_deadline_ns = 0;
	rt->oss_blocks_base = 0;
	rt->last_wake_frames = 0;
	rt->free_run = 0;
	spin_unlock_irqrestore(&ss->lock, fl);

	if (ops->open) {
		rc = ops->open(ss);
		if (rc) {
			spin_lock_irqsave(&ss->lock, &fl);
			ss->opened = 0;
			rt->owner = NULL;
			spin_unlock_irqrestore(&ss->lock, fl);
			return rc;
		}
	}
	return 0;
}

/* Detach and drop the ring (driver already told).  mtx held. */
static void snd_ring_detach(struct snd_pcm_substream *ss, struct snd_ss_rt *rt)
{
	struct snd_ring *r;
	uint64_t fl;

	spin_lock_irqsave(&ss->lock, &fl);
	r = rt->ring;
	rt->ring = NULL;
	ss->dma_area = NULL;
	ss->dma_phys = 0;
	ss->dma_pages = 0;
	ss->buffer_bytes = 0;
	ss->period_bytes = 0;
	ss->periods = 0;
	ss->frame_bytes = 0;
	rt->buffer_frames = 0;
	rt->period_frames = 0;
	rt->period_ns = 0;
	if (ss->state != AUDIO_STATE_DISCONNECTED)
		ss->state = AUDIO_STATE_OPEN;
	spin_unlock_irqrestore(&ss->lock, fl);
	if (r)
		snd_ring_put(r);
}

void snd_pcm_release_ss(struct snd_file *f, struct snd_pcm_substream *ss)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	const struct snd_pcm_ops *ops = ss->pcm->ops;
	uint64_t fl;
	int gone;

	(void)snd_mutex_lock(&rt->mtx, 0);
	spin_lock_irqsave(&ss->lock, &fl);
	if (rt->owner != f) {
		spin_unlock_irqrestore(&ss->lock, fl);
		snd_mutex_unlock(&rt->mtx);
		return;
	}
	gone = snd_card_gone(ss);
	if (!gone && (snd_running(ss->state) || ss->state == AUDIO_STATE_PAUSED))
		(void)snd_trigger_locked(ss, SND_TRIGGER_STOP);
	if (!gone)
		ss->state = AUDIO_STATE_SETUP;
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_timer_quiesce(rt);

	/* After a disconnect the driver may already have torn its side
	 * down: the core makes no more calls into it. */
	if (!gone) {
		if (rt->ring && ops->hw_free)
			(void)ops->hw_free(ss);
		if (ops->close)
			ops->close(ss);
	}
	snd_ring_detach(ss, rt);

	spin_lock_irqsave(&ss->lock, &fl);
	ss->opened = 0;
	ss->owner_uid = 0;
	rt->owner = NULL;
	rt->need_prepare = 0;
	ss->state = gone ? AUDIO_STATE_DISCONNECTED : AUDIO_STATE_OPEN;
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_mutex_unlock(&rt->mtx);
	poll_notify_wq(&ss->wq);
}

/* ======================================================================
 * Parameters, prepare
 * ====================================================================== */

/* Reset to the ring start and call the driver's prepare.  mtx held, the
 * stream not running. */
static int snd_prepare_impl(struct snd_pcm_substream *ss, struct snd_ss_rt *rt)
{
	const struct snd_pcm_ops *ops = ss->pcm->ops;
	uint64_t fl;
	int rc = 0;

	if (snd_card_gone(ss))
		return -ENODEV;
	spin_lock_irqsave(&ss->lock, &fl);
	if (!rt->ring || ss->state == AUDIO_STATE_OPEN) {
		spin_unlock_irqrestore(&ss->lock, fl);
		return -EINVAL;
	}
	if (snd_running(ss->state) || ss->state == AUDIO_STATE_PAUSED)
		(void)snd_trigger_locked(ss, SND_TRIGGER_STOP);
	ss->state = AUDIO_STATE_SETUP;
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_timer_quiesce(rt);

	/* A clean ring: the hardware must never play what a previous run
	 * (or a previous owner) left in it. */
	if (ss->dma_area && rt->ring)
		mm_memset(ss->dma_area, 0, rt->ring->bytes);

	if (ops->prepare)
		rc = ops->prepare(ss);

	spin_lock_irqsave(&ss->lock, &fl);
	ss->hw_frames = 0;
	ss->appl_frames = 0;
	ss->last_hw_offset = 0;
	ss->hw_timestamp_ns = hrtimer_now_ns();
	rt->last_wake_frames = 0;
	rt->last_update_ns = 0;
	rt->need_prepare = rc ? 1 : 0;
	rt->drain_deadline_ns = 0;
	rt->oss_blocks_base = 0;
	ss->oss_blocks = 0;
	if (ss->state != AUDIO_STATE_DISCONNECTED)
		ss->state = rc ? AUDIO_STATE_SETUP : AUDIO_STATE_PREPARED;
	spin_unlock_irqrestore(&ss->lock, fl);
	poll_notify_wq(&ss->wq);
	return rc;
}

static int snd_set_params_impl(struct snd_pcm_substream *ss,
			       struct snd_ss_rt *rt, struct audio_params *p)
{
	const struct snd_pcm_hw *hw = &ss->pcm->hw[ss->direction];
	const struct snd_pcm_ops *ops = ss->pcm->ops;
	struct audio_params want = *p;
	uint32_t pb = 0, nper = 0, bb = 0;
	struct snd_ring *ring;
	uint64_t fl;
	int rc;

	if (snd_card_gone(ss))
		return -ENODEV;
	if (!ss->opened)
		return -EINVAL;
	spin_lock_irqsave(&ss->lock, &fl);
	uint32_t st = ss->state;
	spin_unlock_irqrestore(&ss->lock, fl);
	if (snd_running(st) || st == AUDIO_STATE_PAUSED)
		return -EBUSY;
	if (__atomic_load_n(&rt->io_active, __ATOMIC_ACQUIRE))
		return -EBUSY;
	if (rt->ring && __atomic_load_n(&rt->ring->maps, __ATOMIC_ACQUIRE))
		return -EBUSY; /* the old ring is still mapped somewhere */

	want.direction = (uint32_t)ss->direction;
	rc = snd_pcm_refine(hw, &want, &pb, &nper, &bb);
	if (rc)
		return rc;

	/* Drop the old ring (the driver first lets go of it). */
	if (rt->ring) {
		if (ops->hw_free && !snd_card_gone(ss))
			(void)ops->hw_free(ss);
		snd_ring_detach(ss, rt);
	}

	ring = snd_ring_alloc(bb, hw->buffer_align, hw->dma32, hw->dma_coherent);
	if (!ring) {
		kprintf("[snd] card%d: pcm%d: no memory for a %u-byte ring%s\n",
			ss->pcm->card->index, ss->pcm->device, bb,
			hw->dma32 ? " below 4 GiB" : "");
		return -ENOMEM;
	}

	spin_lock_irqsave(&ss->lock, &fl);
	mm_memset(want.reserved, 0, sizeof(want.reserved));
	ss->params = want;
	ss->frame_bytes = want.frame_bytes;
	ss->buffer_bytes = bb;
	ss->period_bytes = pb;
	ss->periods = nper;
	ss->dma_phys = ring->phys;
	ss->dma_area = ring->virt;
	ss->dma_pages = ring->pages;
	ss->flags = want.flags;
	ss->hw_frames = 0;
	ss->appl_frames = 0;
	ss->last_hw_offset = 0;
	rt->ring = ring;
	rt->buffer_frames = bb / want.frame_bytes;
	rt->period_frames = pb / want.frame_bytes;
	rt->period_ns = (uint64_t)rt->period_frames * NSEC_PER_SEC / want.rate;
	rt->start_threshold = rt->period_frames;
	ss->avail_min_frames = rt->period_frames;
	ss->state = AUDIO_STATE_OPEN;
	spin_unlock_irqrestore(&ss->lock, fl);

	if (ops->hw_params) {
		rc = ops->hw_params(ss);
		if (rc) {
			snd_ring_detach(ss, rt);
			return rc;
		}
	}
	spin_lock_irqsave(&ss->lock, &fl);
	ss->state = AUDIO_STATE_SETUP;
	spin_unlock_irqrestore(&ss->lock, fl);

	rc = snd_prepare_impl(ss, rt);
	if (rc)
		return rc;
	*p = want;
	return 0;
}

int snd_pcm_set_params(struct snd_pcm_substream *ss, struct audio_params *p)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	int rc = snd_mutex_lock(&rt->mtx, 1);
	if (rc)
		return rc;
	rc = snd_set_params_impl(ss, rt, p);
	snd_mutex_unlock(&rt->mtx);
	return rc;
}

int snd_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	int rc = snd_mutex_lock(&rt->mtx, 1);
	if (rc)
		return rc;
	rc = snd_prepare_impl(ss, rt);
	snd_mutex_unlock(&rt->mtx);
	return rc;
}

/* ======================================================================
 * Start, pause, drop, drain, commit
 * ====================================================================== */

int snd_pcm_start(struct snd_pcm_substream *ss)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	uint64_t fl;
	int rc = snd_mutex_lock(&rt->mtx, 1);

	if (rc)
		return rc;
	spin_lock_irqsave(&ss->lock, &fl);
	uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
	uint32_t st = ss->state;
	int need = rt->need_prepare;
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_post_update(ss, bits, 0);

	switch (st) {
	case AUDIO_STATE_OPEN:
		rc = -EINVAL;
		break;
	case AUDIO_STATE_DISCONNECTED:
		rc = -ENODEV;
		break;
	case AUDIO_STATE_XRUN:
		rc = -EPIPE;
		break;
	case AUDIO_STATE_RUNNING:
	case AUDIO_STATE_DRAINING:
		rc = 0;
		break;
	case AUDIO_STATE_PAUSED:
		rc = -EBUSY;
		break;
	case AUDIO_STATE_SETUP:
		if (!need) {
			rc = -EINVAL;
			break;
		}
		rc = snd_prepare_impl(ss, rt);
		if (rc)
			break;
		/* fall through */
	case AUDIO_STATE_PREPARED:
		spin_lock_irqsave(&ss->lock, &fl);
		rc = snd_start_locked(ss, rt);
		spin_unlock_irqrestore(&ss->lock, fl);
		break;
	default:
		rc = -EINVAL;
	}
	snd_mutex_unlock(&rt->mtx);
	poll_notify_wq(&ss->wq);
	return rc;
}

int snd_pcm_pause(struct snd_pcm_substream *ss, int pause)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	uint64_t fl;
	int rc = snd_mutex_lock(&rt->mtx, 1);

	if (rc)
		return rc;
	spin_lock_irqsave(&ss->lock, &fl);
	if (ss->state == AUDIO_STATE_DISCONNECTED) {
		rc = -ENODEV;
	} else if (pause) {
		if (ss->state == AUDIO_STATE_PAUSED) {
			rc = 0;
		} else if (ss->state != AUDIO_STATE_RUNNING) {
			rc = -EINVAL;
		} else {
			uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
			(void)bits;
			if (ss->state != AUDIO_STATE_RUNNING) {
				rc = -EPIPE;
			} else {
				rc = snd_trigger_locked(ss, SND_TRIGGER_PAUSE);
				if (rc == 0)
					ss->state = AUDIO_STATE_PAUSED;
			}
		}
	} else {
		if (ss->state == AUDIO_STATE_RUNNING) {
			rc = 0;
		} else if (ss->state != AUDIO_STATE_PAUSED) {
			rc = -EINVAL;
		} else {
			rc = snd_trigger_locked(ss, SND_TRIGGER_RESUME);
			if (rc == 0) {
				uint64_t now = hrtimer_now_ns();
				ss->state = AUDIO_STATE_RUNNING;
				/* No lap estimate across the pause. */
				rt->last_update_ns = now;
				rt->last_irq_ns = now;
				ss->start_ns = now;
				snd_timer_arm_locked(ss, rt, now);
			}
		}
	}
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_mutex_unlock(&rt->mtx);
	poll_notify_wq(&ss->wq);
	return rc;
}

int snd_pcm_drop(struct snd_pcm_substream *ss)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	uint64_t fl;
	int rc = snd_mutex_lock(&rt->mtx, 1);

	if (rc)
		return rc;
	spin_lock_irqsave(&ss->lock, &fl);
	uint32_t st = ss->state;
	if (snd_running(st) || st == AUDIO_STATE_PAUSED)
		(void)snd_trigger_locked(ss, SND_TRIGGER_STOP);
	if (st != AUDIO_STATE_OPEN && st != AUDIO_STATE_DISCONNECTED)
		ss->state = AUDIO_STATE_SETUP;
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_timer_quiesce(rt);
	if (st == AUDIO_STATE_DISCONNECTED)
		rc = -ENODEV;
	else if (st != AUDIO_STATE_OPEN)
		rc = snd_prepare_impl(ss, rt);
	snd_mutex_unlock(&rt->mtx);
	poll_notify_wq(&ss->wq);
	return rc;
}

struct snd_drain_wait {
	struct snd_pcm_substream *ss;
};

static int snd_drain_done(void *arg)
{
	struct snd_pcm_substream *ss = arg;
	uint32_t st = __atomic_load_n(&ss->state, __ATOMIC_ACQUIRE);
	return st != AUDIO_STATE_DRAINING;
}

/* Fill the free part of a playback ring with silence, so the hardware
 * plays nothing stale between the last frame and the moment the stream is
 * seen to have finished. */
static void snd_fill_silence(struct snd_pcm_substream *ss, uint64_t appl,
			     uint32_t frames)
{
	uint32_t fb = ss->frame_bytes;
	uint32_t bf = ss->buffer_bytes / (fb ? fb : 1);
	uint8_t fill = ss->params.format == AUDIO_FMT_U8 ? 0x80 : 0;

	if (!ss->dma_area || !fb || !bf || !frames)
		return;
	if (frames > bf)
		frames = bf;
	uint32_t off = (uint32_t)(appl % bf) * fb;
	uint32_t bytes = frames * fb;
	uint32_t first = bytes;
	if (off + first > ss->buffer_bytes)
		first = ss->buffer_bytes - off;
	mm_memset(ss->dma_area + off, fill, first);
	if (bytes > first)
		mm_memset(ss->dma_area, fill, bytes - first);
}

int snd_pcm_drain(struct snd_pcm_substream *ss, int nonblock)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	uint64_t fl, appl = 0, timeout_ns = 0;
	uint32_t free_frames = 0;
	int rc = snd_mutex_lock(&rt->mtx, 1);
	int wait = 0;
	(void)nonblock; /* a drain always waits; it is bounded below */

	if (rc)
		return rc;

	if (ss->direction == AUDIO_DIR_CAPTURE) {
		/* Capture: stop; what was recorded stays readable. */
		spin_lock_irqsave(&ss->lock, &fl);
		uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
		if (snd_running(ss->state) || ss->state == AUDIO_STATE_PAUSED) {
			(void)snd_trigger_locked(ss, SND_TRIGGER_STOP);
			ss->state = AUDIO_STATE_SETUP;
			rt->need_prepare = 1;
		}
		rc = ss->state == AUDIO_STATE_DISCONNECTED ? -ENODEV : 0;
		spin_unlock_irqrestore(&ss->lock, fl);
		snd_timer_quiesce(rt);
		snd_post_update(ss, bits, 1);
		snd_mutex_unlock(&rt->mtx);
		return rc;
	}

	spin_lock_irqsave(&ss->lock, &fl);
	uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
	switch (ss->state) {
	case AUDIO_STATE_DISCONNECTED:
		rc = -ENODEV;
		break;
	case AUDIO_STATE_OPEN:
	case AUDIO_STATE_SETUP:
	case AUDIO_STATE_XRUN:
		rc = 0; /* nothing queued that could still play */
		break;
	case AUDIO_STATE_DRAINING:
		wait = 1;
		break;
	case AUDIO_STATE_PREPARED:
		if (ss->appl_frames <= ss->hw_frames) {
			rc = 0;
			break;
		}
		rc = snd_start_locked(ss, rt);
		if (rc)
			break;
		/* fall through */
	case AUDIO_STATE_PAUSED:
		if (ss->state == AUDIO_STATE_PAUSED) {
			rc = snd_trigger_locked(ss, SND_TRIGGER_RESUME);
			if (rc)
				break;
			ss->state = AUDIO_STATE_RUNNING;
			rt->last_update_ns = hrtimer_now_ns();
			rt->last_irq_ns = rt->last_update_ns;
		}
		/* fall through */
	case AUDIO_STATE_RUNNING: {
		uint64_t now = hrtimer_now_ns();
		uint64_t queued = ss->appl_frames - ss->hw_frames;
		appl = ss->appl_frames;
		free_frames = rt->buffer_frames > queued ?
				      rt->buffer_frames - (uint32_t)queued :
				      0;
		ss->state = AUDIO_STATE_DRAINING;
		rt->drain_deadline_ns =
			now + queued * NSEC_PER_SEC / ss->params.rate + 2000000ULL;
		snd_timer_arm_locked(ss, rt, now);
		wait = 2;
		break;
	}
	default:
		rc = -EINVAL;
	}
	if (wait) {
		uint64_t q = ss->appl_frames > ss->hw_frames ?
				     ss->appl_frames - ss->hw_frames :
				     0;
		timeout_ns = q * NSEC_PER_SEC / (ss->params.rate ? ss->params.rate : 48000) * 2 +
			     2ULL * NSEC_PER_SEC;
	}
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_post_update(ss, bits, 0);
	if (wait == 2)
		snd_fill_silence(ss, appl, free_frames);
	snd_mutex_unlock(&rt->mtx);
	if (!wait)
		return rc;

	rc = snd_wait_event(&ss->wq, snd_drain_done, ss, timeout_ns, 1);
	if (rc == -ETIMEDOUT) {
		(void)snd_mutex_lock(&rt->mtx, 0);
		spin_lock_irqsave(&ss->lock, &fl);
		int forced = 0;
		if (ss->state == AUDIO_STATE_DRAINING) {
			(void)snd_trigger_locked(ss, SND_TRIGGER_STOP);
			ss->hw_frames = ss->appl_frames;
			ss->state = AUDIO_STATE_SETUP;
			rt->need_prepare = 1;
			forced = 1;
		}
		spin_unlock_irqrestore(&ss->lock, fl);
		snd_timer_quiesce(rt);
		snd_mutex_unlock(&rt->mtx);
		if (forced)
			kprintf("[snd] card%d: pcm%d: drain did not complete; stream stopped\n",
				ss->pcm->card->index, ss->pcm->device);
		poll_notify_wq(&ss->wq);
		return 0;
	}
	if (rc)
		return rc;
	return __atomic_load_n(&ss->state, __ATOMIC_ACQUIRE) ==
				       AUDIO_STATE_DISCONNECTED ?
		       -ENODEV :
		       0;
}

long snd_pcm_commit(struct snd_pcm_substream *ss, uint32_t frames)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	uint64_t fl;
	long rc;

	spin_lock_irqsave(&ss->lock, &fl);
	uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
	switch (ss->state) {
	case AUDIO_STATE_OPEN:
		rc = -EINVAL;
		break;
	case AUDIO_STATE_DISCONNECTED:
		rc = -ENODEV;
		break;
	case AUDIO_STATE_XRUN:
		rc = -EPIPE;
		break;
	case AUDIO_STATE_DRAINING:
		rc = -EBUSY;
		break;
	default:
		if (ss->state == AUDIO_STATE_SETUP && rt->need_prepare &&
		    ss->direction == AUDIO_DIR_PLAYBACK) {
			rc = -EPIPE; /* drained: AUDIO_PREPARE first */
			break;
		}
		if (frames > snd_avail_locked(ss, rt)) {
			rc = -EINVAL;
			break;
		}
		ss->appl_frames += frames;
		if (ss->direction == AUDIO_DIR_PLAYBACK)
			snd_autostart_locked(ss, rt, 1);
		rc = (long)frames;
	}
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_post_update(ss, bits, 0);
	return rc;
}

/* ======================================================================
 * Position queries
 * ====================================================================== */

void snd_pcm_get_pos(struct snd_pcm_substream *ss, struct audio_pos *pos)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	uint64_t fl;
	uint32_t extra = 0;

	mm_memset(pos, 0, sizeof(*pos));
	spin_lock_irqsave(&ss->lock, &fl);
	uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
	pos->hw_frames = ss->hw_frames;
	pos->appl_frames = ss->appl_frames;
	pos->avail_frames = snd_avail_locked(ss, rt);
	pos->timestamp_ns = ss->hw_timestamp_ns;
	pos->state = ss->state;
	pos->xruns = ss->xruns;
	pos->hw_offset_bytes = ss->last_hw_offset;
	if (snd_running(ss->state) && ss->pcm->ops->delay && !snd_card_gone(ss))
		extra = ss->pcm->ops->delay(ss);
	if (ss->direction == AUDIO_DIR_PLAYBACK) {
		uint64_t q = ss->appl_frames > ss->hw_frames ?
				     ss->appl_frames - ss->hw_frames :
				     0;
		if (q > rt->buffer_frames)
			q = rt->buffer_frames;
		pos->delay_frames = (uint32_t)q + (q ? extra : 0);
	} else {
		uint64_t r = ss->hw_frames > ss->appl_frames ?
				     ss->hw_frames - ss->appl_frames :
				     0;
		if (r > rt->buffer_frames)
			r = rt->buffer_frames;
		pos->delay_frames = (uint32_t)r + extra;
	}
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_post_update(ss, bits, 0);
}

uint32_t snd_pcm_avail(struct snd_pcm_substream *ss)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	uint64_t fl;

	spin_lock_irqsave(&ss->lock, &fl);
	uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
	uint32_t a = snd_avail_locked(ss, rt);
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_post_update(ss, bits, 0);
	return a;
}

/* ======================================================================
 * Data transfer
 * ====================================================================== */

static int snd_xfer_cond(void *arg)
{
	struct snd_pcm_substream *ss = arg;
	struct snd_ss_rt *rt = snd_rt(ss);
	uint64_t fl;
	int ok;

	spin_lock_irqsave(&ss->lock, &fl);
	switch (ss->state) {
	case AUDIO_STATE_RUNNING:
	case AUDIO_STATE_PREPARED:
	case AUDIO_STATE_PAUSED:
		ok = snd_avail_locked(ss, rt) > 0;
		break;
	case AUDIO_STATE_DRAINING:
		ok = 0;
		break;
	case AUDIO_STATE_SETUP:
		/* stopped capture with data left, or a drained playback
		 * stream: the transfer loop decides */
		ok = 1;
		break;
	default:
		ok = 1; /* XRUN, OPEN, DISCONNECTED: let the loop report */
	}
	spin_unlock_irqrestore(&ss->lock, fl);
	return ok || snd_card_gone(ss);
}

static int snd_copy_in(void *dst, const uint8_t *src, size_t n, int user)
{
	if (user)
		return copy_from_user(dst, src, n) == 0 ? 0 : -EFAULT;
	mm_memcpy(dst, src, n);
	return 0;
}

static int snd_copy_out(uint8_t *dst, const void *src, size_t n, int user)
{
	if (user)
		return copy_to_user(dst, src, n) == 0 ? 0 : -EFAULT;
	mm_memcpy(dst, src, n);
	return 0;
}

long snd_pcm_transfer(struct snd_pcm_substream *ss, void *buf, long bytes,
		      int nonblock, int user, int autostart)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	int playback = ss->direction == AUDIO_DIR_PLAYBACK;
	uint64_t total, done = 0;
	uint32_t fb;
	long err = 0;
	uint64_t fl;
	int rc;

	if (bytes <= 0)
		return bytes == 0 ? 0 : -EINVAL;
	if (user && !validate_user_ptr((uint64_t)buf, (size_t)bytes))
		return -EFAULT;

	rc = snd_mutex_lock(&rt->io_mtx, 1);
	if (rc)
		return rc;
	__atomic_fetch_add(&rt->io_active, 1, __ATOMIC_ACQ_REL);

	spin_lock_irqsave(&ss->lock, &fl);
	fb = ss->frame_bytes;
	uint32_t st0 = ss->state;
	spin_unlock_irqrestore(&ss->lock, fl);
	if (st0 == AUDIO_STATE_DISCONNECTED) {
		err = -ENODEV;
		goto out;
	}
	if (!fb || st0 == AUDIO_STATE_OPEN) {
		err = -EINVAL; /* AUDIO_SETPARAMS first */
		goto out;
	}
	total = (uint64_t)bytes / fb;
	if (total == 0) {
		err = -EINVAL; /* less than one frame */
		goto out;
	}

	while (done < total) {
		if (snd_card_gone(ss)) {
			err = -ENODEV;
			break;
		}
		spin_lock_irqsave(&ss->lock, &fl);
		uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
		uint32_t st = ss->state;
		uint32_t avail = snd_avail_locked(ss, rt);

		if (st == AUDIO_STATE_DISCONNECTED) {
			spin_unlock_irqrestore(&ss->lock, fl);
			snd_post_update(ss, bits, 0);
			err = -ENODEV;
			break;
		}
		if (st == AUDIO_STATE_OPEN || !ss->dma_area || ss->frame_bytes != fb) {
			spin_unlock_irqrestore(&ss->lock, fl);
			snd_post_update(ss, bits, 0);
			err = -EINVAL;
			break;
		}
		/* Recover from an underrun/overrun, or restart a drained or
		 * stopped stream, transparently: the condition has already
		 * been counted and reported as an event.  Not an underrun of
		 * a stream whose writer starts it itself (NO_AUTOSTART): that
		 * writer still takes it for running, and a prepare here would
		 * leave it stopped behind its back -- so it is told (EPIPE)
		 * and recovers, prepare, refill, start, as with any device. */
		if (st == AUDIO_STATE_XRUN ||
		    (st == AUDIO_STATE_SETUP && (playback || avail == 0))) {
			spin_unlock_irqrestore(&ss->lock, fl);
			snd_post_update(ss, bits, 0);
			if (st == AUDIO_STATE_XRUN &&
			    (ss->flags & AUDIO_PARAM_NO_AUTOSTART)) {
				err = -EPIPE;
				break;
			}
			rc = snd_mutex_lock(&rt->mtx, 1);
			if (rc) {
				err = rc;
				break;
			}
			rc = snd_prepare_impl(ss, rt);
			snd_mutex_unlock(&rt->mtx);
			if (rc) {
				err = rc;
				break;
			}
			continue;
		}
		if (!playback && avail == 0 && st == AUDIO_STATE_PREPARED &&
		    autostart && !(ss->flags & AUDIO_PARAM_NO_AUTOSTART))
			(void)snd_start_locked(ss, rt);
		if (st == AUDIO_STATE_DRAINING)
			avail = 0;

		if (avail == 0) {
			spin_unlock_irqrestore(&ss->lock, fl);
			snd_post_update(ss, bits, 0);
			if (nonblock) {
				if (!done)
					err = -EAGAIN;
				break;
			}
			if (done && !playback)
				break; /* a read returns what it has */
			rc = snd_wait_event(&ss->wq, snd_xfer_cond, ss,
					    200000000ULL, 1);
			if (rc == -ERESTARTSYS) {
				if (!done)
					err = -ERESTARTSYS;
				break;
			}
			continue; /* condition met, or the periodic re-check */
		}

		uint64_t n = total - done;
		if (n > avail)
			n = avail;
		uint64_t snap = ss->appl_frames;
		uint32_t bf = rt->buffer_frames;
		uint32_t off = (uint32_t)(snap % bf) * fb;
		uint8_t *area = ss->dma_area;
		uint32_t ring_bytes = ss->buffer_bytes;
		spin_unlock_irqrestore(&ss->lock, fl);
		snd_post_update(ss, bits, 0);

		/* The copy runs unlocked (it may fault on user memory).  The
		 * region belongs to the application side of the ring, which
		 * neither the interrupt path nor the hardware touches. */
		uint32_t nbytes = (uint32_t)n * fb;
		uint32_t first = nbytes;
		uint8_t *ubase = (uint8_t *)buf + done * fb;
		if (off + first > ring_bytes)
			first = ring_bytes - off;
		if (playback) {
			rc = snd_copy_in(area + off, ubase, first, user);
			if (!rc && nbytes > first)
				rc = snd_copy_in(area, ubase + first, nbytes - first, user);
		} else {
			rc = snd_copy_out(ubase, area + off, first, user);
			if (!rc && nbytes > first)
				rc = snd_copy_out(ubase + first, area, nbytes - first, user);
		}
		if (rc) {
			err = rc;
			break;
		}

		spin_lock_irqsave(&ss->lock, &fl);
		/* A DROP/PREPARE that ran meanwhile reset the counters; what
		 * was just copied belongs to nothing now. */
		if (ss->appl_frames == snap && ss->dma_area == area) {
			if (!playback && ss->state == AUDIO_STATE_XRUN) {
				/* overrun during the copy: the data may be
				 * torn; do not hand it out as valid */
				spin_unlock_irqrestore(&ss->lock, fl);
				continue;
			}
			ss->appl_frames += n;
			done += n;
			if (playback)
				snd_autostart_locked(ss, rt, autostart);
		}
		spin_unlock_irqrestore(&ss->lock, fl);
	}
out:
	__atomic_fetch_sub(&rt->io_active, 1, __ATOMIC_ACQ_REL);
	snd_mutex_unlock(&rt->io_mtx);
	if (done)
		return (long)(done * fb);
	return err;
}

/* ======================================================================
 * poll
 * ====================================================================== */

short snd_pcm_poll_ss(struct snd_pcm_substream *ss, short events, int oss)
{
	struct snd_ss_rt *rt = snd_rt(ss);
	short mask = 0;
	uint64_t fl;
	(void)events;

	spin_lock_irqsave(&ss->lock, &fl);
	uint32_t bits = snd_update_locked(ss, rt, hrtimer_now_ns(), 0);
	uint32_t avail = snd_avail_locked(ss, rt);
	uint32_t amin = ss->avail_min_frames ? ss->avail_min_frames : 1;
	if (amin > rt->buffer_frames && rt->buffer_frames)
		amin = rt->buffer_frames;

	switch (ss->state) {
	case AUDIO_STATE_DISCONNECTED:
		mask = POLLERR | POLLHUP;
		break;
	case AUDIO_STATE_OPEN:
		/* Not configured: a read/write answers at once (with an
		 * error natively; the OSS dialect configures itself). */
		mask = ss->direction == AUDIO_DIR_PLAYBACK ?
			       (POLLOUT | POLLWRNORM) :
			       (oss ? 0 : (POLLIN | POLLRDNORM));
		break;
	case AUDIO_STATE_XRUN:
		mask = POLLERR | (ss->direction == AUDIO_DIR_PLAYBACK ?
					  (POLLOUT | POLLWRNORM) :
					  (POLLIN | POLLRDNORM));
		break;
	case AUDIO_STATE_DRAINING:
		mask = 0;
		break;
	case AUDIO_STATE_SETUP:
		if (ss->direction == AUDIO_DIR_PLAYBACK)
			mask = POLLOUT | POLLWRNORM; /* a write restarts it */
		else if (avail)
			mask = POLLIN | POLLRDNORM;
		break;
	default:
		if (ss->direction == AUDIO_DIR_PLAYBACK) {
			if (avail >= amin)
				mask = POLLOUT | POLLWRNORM;
		} else if (avail >= amin) {
			mask = POLLIN | POLLRDNORM;
		}
	}
	spin_unlock_irqrestore(&ss->lock, fl);
	snd_post_update(ss, bits, 0);
	return mask;
}

/* ======================================================================
 * mmap
 * ====================================================================== */

/* One mapping (or the split/forked copies the address space makes of
 * it): pins the ring and remembers which page of it the mapping starts
 * at. */
struct snd_mmap_ctx {
	int refs;
	struct snd_ring *ring;
	uint64_t first_page;
	uint64_t npages;
};

static uint64_t snd_mmap_page_phys(void *obj, uint64_t index)
{
	struct snd_mmap_ctx *c = obj;
	if (!c || !c->ring || index >= c->npages)
		return 0;
	return c->ring->phys + (c->first_page + index) * PAGE_SIZE;
}

static void snd_mmap_get(void *obj)
{
	struct snd_mmap_ctx *c = obj;
	__atomic_fetch_add(&c->refs, 1, __ATOMIC_ACQ_REL);
}

static void snd_mmap_put(void *obj)
{
	struct snd_mmap_ctx *c = obj;
	if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL) == 0) {
		struct snd_ring *r = c->ring;
		__atomic_fetch_sub(&r->maps, 1, __ATOMIC_ACQ_REL);
		c->ring = NULL;
		kfree(c);
		snd_ring_put_async(r);
	}
}

int snd_pcm_mmap(struct snd_file *f, struct device_mmap *m)
{
	struct snd_pcm_substream *ss;
	struct snd_ss_rt *rt;
	struct snd_mmap_ctx *c;
	uint64_t base;
	int dir;
	int rc;

	if (m->offset >= SND_MMAP_CAPTURE_OFFSET) {
		if (f->dirs != ((1u << AUDIO_DIR_PLAYBACK) | (1u << AUDIO_DIR_CAPTURE)))
			return -EINVAL;
		dir = AUDIO_DIR_CAPTURE;
		base = SND_MMAP_CAPTURE_OFFSET;
	} else {
		dir = f->primary;
		base = 0;
	}
	ss = snd_file_ss(f, dir);
	if (!ss)
		return -EINVAL;
	if ((m->prot & PROT_WRITE) && !f->can_write)
		return -EACCES;
	if (m->length == 0 || (m->length & (PAGE_SIZE - 1)) ||
	    (m->offset & (PAGE_SIZE - 1)))
		return -EINVAL;
	rt = snd_rt(ss);

	c = kalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;
	mm_memset(c, 0, sizeof(*c));

	rc = snd_mutex_lock(&rt->mtx, 1);
	if (rc) {
		kfree(c);
		return rc;
	}
	struct snd_ring *ring = rt->ring;
	uint64_t inner = m->offset - base;
	uint64_t span = ring ? (uint64_t)ring->pages * PAGE_SIZE : 0;
	if (!ring || ss->state == AUDIO_STATE_OPEN ||
	    ss->state == AUDIO_STATE_DISCONNECTED) {
		rc = -EINVAL; /* not set up: AUDIO_SETPARAMS first */
	} else if (inner >= span || m->length > span - inner) {
		rc = -EINVAL; /* never past the ring */
	} else {
		snd_ring_get(ring);
		__atomic_fetch_add(&ring->maps, 1, __ATOMIC_ACQ_REL);
		c->refs = 1; /* handed to the address space */
		c->ring = ring;
		c->first_page = inner / PAGE_SIZE;
		c->npages = m->length / PAGE_SIZE;
		m->obj = c;
		m->page_phys = snd_mmap_page_phys;
		m->get = snd_mmap_get;
		m->put = snd_mmap_put;
		/* The user's view must have the same cache attribute as the
		 * kernel's (and the device's expectation): uncached unless
		 * the device snoops. */
		m->pte_extra = ring->coherent ? 0 :
						(PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
		ss->mmap_count = ring->maps;
		if (f->kind == SND_NODE_DSP)
			rt->free_run = 1;
		rc = 0;
	}
	snd_mutex_unlock(&rt->mtx);
	if (rc)
		kfree(c);
	return rc;
}

/* ======================================================================
 * Native requests
 * ====================================================================== */

static void snd_params_snapshot(struct snd_pcm_substream *ss,
				struct audio_params *p)
{
	uint64_t fl;

	mm_memset(p, 0, sizeof(*p));
	spin_lock_irqsave(&ss->lock, &fl);
	if (ss->state != AUDIO_STATE_OPEN && ss->frame_bytes) {
		*p = ss->params;
		p->buffer_bytes = ss->buffer_bytes;
		p->frame_bytes = ss->frame_bytes;
		p->flags = ss->flags;
		p->period_frames = ss->period_bytes / ss->frame_bytes;
		p->periods = ss->periods;
		mm_memset(p->reserved, 0, sizeof(p->reserved));
	}
	p->direction = (uint32_t)ss->direction;
	spin_unlock_irqrestore(&ss->lock, fl);
}

/* Apply `fn' to every direction the handle holds; the first error wins. */
static long snd_for_each_dir(struct snd_file *f,
			     int (*fn)(struct snd_pcm_substream *))
{
	long rc = 0;
	for (int d = 0; d < 2; d++) {
		struct snd_pcm_substream *ss = snd_file_ss(f, d);
		if (!ss)
			continue;
		int r = fn(ss);
		if (r && !rc)
			rc = r;
	}
	return rc;
}

static int snd_do_pause(struct snd_pcm_substream *ss)
{
	return snd_pcm_pause(ss, 1);
}

static int snd_do_resume(struct snd_pcm_substream *ss)
{
	return snd_pcm_pause(ss, 0);
}

static int snd_do_drain(struct snd_pcm_substream *ss)
{
	return snd_pcm_drain(ss, 0);
}

long snd_pcm_ioctl(struct snd_file *f, unsigned long req, void *argp)
{
	struct snd_pcm_substream *ss;
	long rc;

	switch (req) {
	case AUDIO_SETPARAMS: {
		struct audio_params p;
		rc = snd_user_in(&p, argp, sizeof(p));
		if (rc)
			return rc;
		if (p.direction > AUDIO_DIR_CAPTURE)
			return -EINVAL;
		ss = snd_file_ss(f, (int)p.direction);
		if (!ss)
			return -EINVAL;
		if (p.flags & ~SND_PARAM_KNOWN_FLAGS)
			return -EINVAL;
		if (p.format > AUDIO_FMT_MAX || p.channels > 1024 ||
		    p.rate > 10000000 || p.periods > 65536 ||
		    p.period_frames > (SND_RING_MAX_BYTES / 1))
			return -EINVAL;
		rc = snd_pcm_set_params(ss, &p);
		if (rc)
			return rc;
		/* The native request settles the stream: the OSS shaping
		 * (if any) no longer applies, but its view is kept in step
		 * for SNDCTL_DSP_* queries on the same handle. */
		f->oss_dirty = 0;
		if (f->kind != SND_NODE_DSP)
			f->oss_used = 0;
		f->oss_format = p.format;
		f->oss_rate = p.rate;
		f->oss_channels = p.channels;
		mm_memset(p.reserved, 0, sizeof(p.reserved));
		return snd_user_out(argp, &p, sizeof(p));
	}
	case AUDIO_GETPARAMS: {
		struct audio_params p;
		ss = snd_file_ss(f, f->primary);
		if (!ss)
			return -EINVAL;
		snd_params_snapshot(ss, &p);
		return snd_user_out(argp, &p, sizeof(p));
	}
	case AUDIO_GETPOS: {
		struct audio_pos pos;
		ss = snd_file_ss(f, f->primary);
		if (!ss)
			return -EINVAL;
		snd_pcm_get_pos(ss, &pos);
		return snd_user_out(argp, &pos, sizeof(pos));
	}
	case AUDIO_START:
		return snd_for_each_dir(f, snd_pcm_start);
	case AUDIO_PAUSE:
		return snd_for_each_dir(f, snd_do_pause);
	case AUDIO_RESUME:
		return snd_for_each_dir(f, snd_do_resume);
	case AUDIO_DRAIN:
		return snd_for_each_dir(f, snd_do_drain);
	case AUDIO_DROP:
		f->oss_partial_len = 0;
		return snd_for_each_dir(f, snd_pcm_drop);
	case AUDIO_PREPARE:
		return snd_for_each_dir(f, snd_pcm_prepare);
	case AUDIO_COMMIT: {
		uint32_t frames;
		rc = snd_user_in(&frames, argp, sizeof(frames));
		if (rc)
			return rc;
		ss = snd_file_ss(f, f->primary);
		if (!ss)
			return -EINVAL;
		rc = snd_pcm_commit(ss, frames);
		return rc < 0 ? rc : 0;
	}
	default:
		return -ENOTTY;
	}
}

/* ======================================================================
 * Disconnect and diagnostics
 * ====================================================================== */

void snd_pcm_disconnect_all(struct snd_card *card)
{
	for (int i = 0; i < card->npcm && i < SND_MAX_PCMS; i++) {
		struct snd_pcm *pcm = card->pcm[i];
		if (!pcm)
			continue;
		for (int d = 0; d < 2; d++) {
			struct snd_pcm_substream *ss = &pcm->sub[d];
			struct snd_ss_rt *rt = snd_rt(ss);
			uint64_t fl;

			spin_lock_irqsave(&ss->lock, &fl);
			if (ss->opened) {
				/* The driver is still with us at this point
				 * (it is the caller): stop the engine so no
				 * DMA runs into a ring that will be freed. */
				if (snd_running(ss->state) ||
				    ss->state == AUDIO_STATE_PAUSED)
					(void)pcm->ops->trigger(ss, SND_TRIGGER_STOP);
				ss->state = AUDIO_STATE_DISCONNECTED;
			}
			spin_unlock_irqrestore(&ss->lock, fl);
			snd_timer_quiesce(rt);
			poll_notify_wq(&ss->wq);
		}
	}
}

void snd_pcm_diag(struct snd_card *card, struct snd_text *t)
{
	for (int i = 0; i < card->npcm && i < SND_MAX_PCMS; i++) {
		struct snd_pcm *pcm = card->pcm[i];
		if (!pcm)
			continue;
		snd_text_printf(t, "pcm%d \"%s\"\n", i, pcm->name);
		for (int d = 0; d < 2; d++) {
			const struct snd_pcm_hw *hw = &pcm->hw[d];
			struct snd_pcm_substream *ss = &pcm->sub[d];
			struct snd_ss_rt *rt = snd_rt(ss);
			uint64_t fl;

			if (!hw->formats)
				continue;
			snd_text_printf(t,
					"  %s: formats 0x%x rates 0x%x channels %u-%u period %u-%u bytes x %u-%u, buffer <= %u%s%s\n",
					d ? "capture" : "playback", hw->formats,
					hw->rates, hw->channels_min,
					hw->channels_max, hw->period_bytes_min,
					hw->period_bytes_max, hw->periods_min,
					hw->periods_max, hw->buffer_bytes_max,
					hw->dma32 ? ", dma32" : "",
					hw->dma_coherent ? ", coherent" : "");

			struct audio_params p;
			uint64_t hwf, apf, phys;
			uint32_t st, xr, irqs, mode, off, opened, uid, bb;
			spin_lock_irqsave(&ss->lock, &fl);
			p = ss->params;
			hwf = ss->hw_frames;
			apf = ss->appl_frames;
			st = ss->state;
			xr = ss->xruns;
			irqs = rt->irq_count;
			mode = (uint32_t)rt->timer_mode;
			off = ss->last_hw_offset;
			opened = (uint32_t)ss->opened;
			uid = ss->owner_uid;
			phys = ss->dma_phys;
			bb = ss->buffer_bytes;
			spin_unlock_irqrestore(&ss->lock, fl);

			if (!opened) {
				snd_text_printf(t, "    closed\n");
				continue;
			}
			snd_text_printf(t, "    state %s, owner uid %u\n",
					snd_pcm_state_name(st), uid);
			if (st == AUDIO_STATE_OPEN)
				continue;
			snd_text_printf(t,
					"    %s %u Hz %u ch, period %u frames x %u, ring %u bytes at phys 0x%llx (%s)\n",
					snd_format_name(p.format), p.rate,
					p.channels, p.period_frames, p.periods,
					bb, (unsigned long long)phys,
					rt->ring && rt->ring->coherent ? "write-back" :
									  "uncached");
			snd_text_printf(t,
					"    hw %llu appl %llu offset %u, xruns %u, interrupts %u, timer %s, mappings %d\n",
					(unsigned long long)hwf,
					(unsigned long long)apf, off, xr, irqs,
					mode == SND_TIMER_PERIODIC ? "periodic" :
					mode == SND_TIMER_WATCHDOG ? "watchdog" :
								     "off",
					rt->ring ? rt->ring->maps : 0);
		}
	}
}
