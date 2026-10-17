// LikeOS sound core - the OSS dialect: SNDCTL_DSP_* on the PCM nodes,
// SOUND_MIXER_* on every node, OSS_GETVERSION and SNDCTL_SYSINFO.
//
// OSS programs set the sample format, channel count and rate one request
// at a time and expect each answer to be the value the device will use, and
// they may start writing without ever saying how big the buffer should be.
// So a handle carries the wanted parameters (each already snapped to what
// the hardware can do, so it can be reported at once) and the ring is
// configured lazily: on the first read/write/mmap or any request that
// needs the real geometry, with the classic defaults (8 kHz, unsigned
// 8-bit, mono, each mapped to the nearest the hardware can do) for
// whatever was never set.
//
// Copyright (C) 2026 The LikeOS Project

#include "snd_internal.h"

#include <kernel/uapi/ioctl.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

/* ======================================================================
 * Helpers
 * ====================================================================== */

static int oss_get_int(void *argp, int *v)
{
	return snd_user_in(v, argp, sizeof(*v));
}

static int oss_put_int(void *argp, int v)
{
	return snd_user_out(argp, &v, sizeof(v));
}

static const struct snd_pcm_hw *oss_hw(struct snd_file *f)
{
	if (!f->pcm)
		return NULL;
	return &f->pcm->hw[f->primary];
}

/* AFMT_* <-> AUDIO_FMT_* */
static uint32_t oss_afmt_to_fmt(int afmt)
{
	switch (afmt) {
	case AFMT_U8: return AUDIO_FMT_U8;
	case AFMT_S16_LE: return AUDIO_FMT_S16_LE;
	case AFMT_S32_LE: return AUDIO_FMT_S32_LE;
	case AFMT_S24_LE: return AUDIO_FMT_S24_LE;
	case AFMT_S24_PACKED: return AUDIO_FMT_S24_3LE;
	case AFMT_FLOAT: return AUDIO_FMT_FLOAT_LE;
	/* Formats the hardware never plays natively: the nearest the
	 * device has will be picked, starting from the same width. */
	case AFMT_S8:
	case AFMT_MU_LAW:
	case AFMT_A_LAW: return AUDIO_FMT_U8;
	case AFMT_S16_BE:
	case AFMT_U16_LE:
	case AFMT_U16_BE: return AUDIO_FMT_S16_LE;
	case AFMT_S32_BE: return AUDIO_FMT_S32_LE;
	case AFMT_S24_BE: return AUDIO_FMT_S24_LE;
	default: return AUDIO_FMT_S16_LE;
	}
}

static int oss_fmt_to_afmt(uint32_t fmt)
{
	switch (fmt) {
	case AUDIO_FMT_U8: return AFMT_U8;
	case AUDIO_FMT_S16_LE: return AFMT_S16_LE;
	case AUDIO_FMT_S32_LE: return AFMT_S32_LE;
	case AUDIO_FMT_S24_LE: return AFMT_S24_LE;
	case AUDIO_FMT_S24_3LE: return AFMT_S24_PACKED;
	case AUDIO_FMT_FLOAT_LE: return AFMT_FLOAT;
	/* 20-bit in 32: closest OSS name is the 32-bit container */
	case AUDIO_FMT_S20_LE: return AFMT_S32_LE;
	default: return 0;
	}
}

static int oss_formats_mask(const struct snd_pcm_hw *hw)
{
	int m = 0;
	if (!hw)
		return 0;
	for (uint32_t f = 1; f <= AUDIO_FMT_MAX; f++) {
		if (!(hw->formats & AUDIO_FMT_BIT(f)))
			continue;
		if (f == AUDIO_FMT_S20_LE)
			continue; /* no OSS name of its own */
		m |= oss_fmt_to_afmt(f);
	}
	return m;
}

/* Something about the stream changed: reconfigure before the next use. */
static void oss_touch(struct snd_file *f)
{
	f->oss_used = 1;
	f->oss_dirty = 1;
}

void snd_oss_file_init(struct snd_file *f)
{
	const struct snd_pcm_hw *hw = oss_hw(f);

	f->oss_used = (f->kind == SND_NODE_DSP);
	f->oss_dirty = 1;
	/* The classic power-on defaults, snapped to the hardware. */
	f->oss_format = snd_pcm_pick_format(hw, AUDIO_FMT_U8);
	f->oss_rate = snd_pcm_pick_rate(hw, 8000);
	f->oss_channels = snd_pcm_pick_channels(hw, 1);
	f->oss_frag_bytes = 0;
	f->oss_frag_count = 0;
	f->oss_subdivide = 1;
	f->oss_low_water = 0;
	f->oss_trigger = 0;
	if (f->dirs & (1u << AUDIO_DIR_PLAYBACK))
		f->oss_trigger |= PCM_ENABLE_OUTPUT;
	if (f->dirs & (1u << AUDIO_DIR_CAPTURE))
		f->oss_trigger |= PCM_ENABLE_INPUT;
	f->oss_partial_len = 0;
}

static uint32_t oss_state(struct snd_pcm_substream *ss)
{
	return __atomic_load_n(&ss->state, __ATOMIC_ACQUIRE);
}

/* Apply the low-water mark (bytes) as the wake-up threshold. */
static void oss_apply_low_water(struct snd_file *f, struct snd_pcm_substream *ss)
{
	uint64_t fl;

	spin_lock_irqsave(&ss->lock, &fl);
	if (ss->frame_bytes && ss->buffer_bytes) {
		uint32_t bf = ss->buffer_bytes / ss->frame_bytes;
		uint32_t frames = f->oss_low_water ?
					  f->oss_low_water / ss->frame_bytes :
					  ss->period_bytes / ss->frame_bytes;
		if (frames < 1)
			frames = 1;
		if (frames > bf)
			frames = bf;
		ss->avail_min_frames = frames;
	}
	spin_unlock_irqrestore(&ss->lock, fl);
}

int snd_oss_configure(struct snd_file *f)
{
	int rc = 0;

	if (!f->pcm)
		return -EINVAL;
	for (int d = 0; d < 2; d++) {
		struct snd_pcm_substream *ss = snd_file_ss(f, d);
		const struct snd_pcm_hw *hw;
		struct audio_params p;
		uint32_t st;

		if (!ss)
			continue;
		st = oss_state(ss);
		if (st == AUDIO_STATE_DISCONNECTED)
			return -ENODEV;
		if (!f->oss_dirty && st != AUDIO_STATE_OPEN)
			continue;
		hw = &f->pcm->hw[d];

		mm_memset(&p, 0, sizeof(p));
		p.direction = (uint32_t)d;
		p.format = snd_pcm_pick_format(hw, f->oss_format);
		p.rate = snd_pcm_pick_rate(hw, f->oss_rate);
		p.channels = snd_pcm_pick_channels(hw, f->oss_channels);
		uint32_t fb = audio_format_bytes(p.format) * p.channels;
		if (!fb)
			return -EINVAL;
		if (f->oss_frag_bytes) {
			uint32_t frag = f->oss_frag_bytes /
					(f->oss_subdivide ? f->oss_subdivide : 1);
			p.period_frames = frag / fb ? frag / fb : 1;
		}
		if (f->oss_frag_count)
			p.periods = f->oss_frag_count;
		/* The output trigger off means "do not start on write". */
		if (!(f->oss_trigger &
		      (d == AUDIO_DIR_PLAYBACK ? PCM_ENABLE_OUTPUT : PCM_ENABLE_INPUT)))
			p.flags |= AUDIO_PARAM_NO_AUTOSTART;

		if (st == AUDIO_STATE_RUNNING || st == AUDIO_STATE_PAUSED ||
		    st == AUDIO_STATE_DRAINING) {
			rc = snd_pcm_drop(ss);
			if (rc)
				return rc;
		}
		rc = snd_pcm_set_params(ss, &p);
		if (rc)
			return rc;
		if (d == f->primary) {
			f->oss_format = p.format;
			f->oss_rate = p.rate;
			f->oss_channels = p.channels;
		}
		oss_apply_low_water(f, ss);
	}
	f->oss_dirty = 0;
	f->oss_partial_len = 0;
	return 0;
}

/* ======================================================================
 * Data
 * ====================================================================== */

long snd_oss_write(struct snd_file *f, const void *buf, long bytes,
		   int nonblock)
{
	struct snd_pcm_substream *ss;
	long accepted = 0;
	uint32_t fb;
	int gate;
	long r;
	int rc;

	ss = snd_file_ss(f, AUDIO_DIR_PLAYBACK);
	if (!ss)
		return -EINVAL;
	rc = snd_oss_configure(f);
	if (rc)
		return rc;
	fb = ss->frame_bytes;
	if (!fb || fb > SND_OSS_PARTIAL_MAX)
		return -EINVAL;
	if (!validate_user_ptr((uint64_t)buf, (size_t)bytes))
		return -EFAULT;
	gate = (f->oss_trigger & PCM_ENABLE_OUTPUT) != 0;

	/* OSS takes any byte count: a frame split across two writes is
	 * held here until it is complete. */
	if (f->oss_partial_len) {
		uint32_t need = fb - f->oss_partial_len;
		uint32_t take = (uint64_t)bytes < need ? (uint32_t)bytes : need;
		if (take) {
			if (copy_from_user(f->oss_partial + f->oss_partial_len, buf,
					   take) != 0)
				return -EFAULT;
			f->oss_partial_len += take;
			accepted += take;
		}
		if (f->oss_partial_len < fb)
			return accepted;
		r = snd_pcm_transfer(ss, f->oss_partial, fb, nonblock, 0, gate);
		if (r < 0)
			return accepted ? accepted : r;
		f->oss_partial_len = 0;
	}

	long whole = ((bytes - accepted) / fb) * fb;
	if (whole) {
		r = snd_pcm_transfer(ss, (uint8_t *)buf + accepted, whole, nonblock,
				     1, gate);
		if (r < 0)
			return accepted ? accepted : r;
		accepted += r;
		if (r < whole)
			return accepted;
	}
	long rem = bytes - accepted;
	if (rem > 0 && rem < (long)fb) {
		if (copy_from_user(f->oss_partial, (const uint8_t *)buf + accepted,
				   (size_t)rem) != 0)
			return accepted ? accepted : -EFAULT;
		f->oss_partial_len = (uint32_t)rem;
		accepted += rem;
	}
	return accepted;
}

long snd_oss_read(struct snd_file *f, void *buf, long bytes, int nonblock)
{
	struct snd_pcm_substream *ss;
	int rc;

	ss = snd_file_ss(f, AUDIO_DIR_CAPTURE);
	if (!ss)
		return -EINVAL;
	rc = snd_oss_configure(f);
	if (rc)
		return rc;
	if (!ss->frame_bytes)
		return -EINVAL;
	long whole = (bytes / ss->frame_bytes) * ss->frame_bytes;
	if (whole == 0)
		return -EINVAL;
	return snd_pcm_transfer(ss, buf, whole, nonblock, 1,
				(f->oss_trigger & PCM_ENABLE_INPUT) != 0);
}

/* ======================================================================
 * /dev/dsp requests
 * ====================================================================== */

static long oss_space(struct snd_file *f, int dir, void *argp)
{
	struct snd_pcm_substream *ss = snd_file_ss(f, dir);
	audio_buf_info info;
	int rc;

	if (!ss)
		return -EINVAL;
	rc = snd_oss_configure(f);
	if (rc)
		return rc;
	mm_memset(&info, 0, sizeof(info));
	uint32_t avail = snd_pcm_avail(ss);
	uint32_t fb = ss->frame_bytes ? ss->frame_bytes : 1;
	uint32_t pb = ss->period_bytes ? ss->period_bytes : fb;
	uint64_t bytes = (uint64_t)avail * fb;
	if (dir == AUDIO_DIR_PLAYBACK && f->oss_partial_len && bytes >= fb)
		bytes -= fb; /* the held partial frame takes one slot */
	info.fragsize = (int)pb;
	info.fragstotal = (int)ss->periods;
	info.bytes = (int)(bytes > 0x7fffffff ? 0x7fffffff : bytes);
	info.fragments = (int)(bytes / pb);
	return snd_user_out(argp, &info, sizeof(info));
}

static long oss_ptr(struct snd_file *f, int dir, void *argp)
{
	struct snd_pcm_substream *ss = snd_file_ss(f, dir);
	struct snd_ss_rt *rt;
	struct audio_pos pos;
	count_info ci;
	uint64_t fl;

	if (!ss)
		return -EINVAL;
	mm_memset(&ci, 0, sizeof(ci));
	if (oss_state(ss) != AUDIO_STATE_OPEN) {
		rt = snd_rt(ss);
		snd_pcm_get_pos(ss, &pos);
		spin_lock_irqsave(&ss->lock, &fl);
		uint32_t fb = ss->frame_bytes ? ss->frame_bytes : 1;
		uint32_t pf = ss->period_bytes / fb;
		ci.bytes = (int)((pos.hw_frames * fb) & 0x7fffffffULL);
		ci.ptr = (int)pos.hw_offset_bytes;
		if (pf && pos.hw_frames >= rt->oss_blocks_base) {
			uint64_t blocks = (pos.hw_frames - rt->oss_blocks_base) / pf;
			rt->oss_blocks_base += blocks * pf;
			ci.blocks = blocks > 0x7fffffff ? 0x7fffffff : (int)blocks;
		} else {
			rt->oss_blocks_base = pos.hw_frames;
		}
		spin_unlock_irqrestore(&ss->lock, fl);
	}
	return snd_user_out(argp, &ci, sizeof(ci));
}

static int oss_odelay(struct snd_file *f)
{
	struct snd_pcm_substream *ss = snd_file_ss(f, AUDIO_DIR_PLAYBACK);
	struct audio_pos pos;

	if (!ss || oss_state(ss) == AUDIO_STATE_OPEN)
		return 0;
	snd_pcm_get_pos(ss, &pos);
	uint64_t d = (uint64_t)pos.delay_frames * ss->frame_bytes + f->oss_partial_len;
	return d > 0x7fffffff ? 0x7fffffff : (int)d;
}

static int oss_drop_dir(struct snd_file *f, int dir)
{
	struct snd_pcm_substream *ss = snd_file_ss(f, dir);
	if (!ss)
		return 0;
	if (dir == AUDIO_DIR_PLAYBACK)
		f->oss_partial_len = 0;
	if (oss_state(ss) == AUDIO_STATE_OPEN)
		return 0;
	return snd_pcm_drop(ss);
}

static int oss_reset(struct snd_file *f)
{
	int r1 = oss_drop_dir(f, AUDIO_DIR_PLAYBACK);
	int r2 = oss_drop_dir(f, AUDIO_DIR_CAPTURE);
	return r1 ? r1 : r2;
}

static int oss_sync(struct snd_file *f)
{
	struct snd_pcm_substream *ss = snd_file_ss(f, AUDIO_DIR_PLAYBACK);
	int rc = 0;

	if (ss && oss_state(ss) != AUDIO_STATE_OPEN) {
		/* A trailing partial frame cannot be played; it is dropped. */
		f->oss_partial_len = 0;
		rc = snd_pcm_drain(ss, 0);
		if (rc == -ERESTARTSYS)
			return rc;
		/* Back to idle with an empty ring, ready for more. */
		int r2 = snd_pcm_drop(ss);
		if (!rc)
			rc = r2;
	}
	return rc;
}

static int oss_caps(struct snd_file *f)
{
	int caps = 1 /* revision */ | DSP_CAP_TRIGGER | DSP_CAP_MMAP |
		   DSP_CAP_REALTIME;
	if (f->pcm) {
		int play = f->pcm->hw[AUDIO_DIR_PLAYBACK].formats != 0;
		int cap = f->pcm->hw[AUDIO_DIR_CAPTURE].formats != 0;
		if (play)
			caps |= DSP_CAP_OUTPUT;
		if (cap)
			caps |= DSP_CAP_INPUT;
		if (play && cap)
			caps |= DSP_CAP_DUPLEX;
	}
	return caps;
}

static long oss_set_trigger(struct snd_file *f, int v)
{
	int rc = 0;

	for (int d = 0; d < 2; d++) {
		struct snd_pcm_substream *ss = snd_file_ss(f, d);
		uint32_t bit = d == AUDIO_DIR_PLAYBACK ? PCM_ENABLE_OUTPUT :
							 PCM_ENABLE_INPUT;
		uint64_t fl;

		if (!ss)
			continue;
		int on = (v & (int)bit) != 0;
		int was = (f->oss_trigger & bit) != 0;
		if (on)
			f->oss_trigger |= bit;
		else
			f->oss_trigger &= ~bit;

		if (!on) {
			/* Disabled: stop, and keep write() from starting it. */
			if (was) {
				int r = oss_drop_dir(f, d);
				if (r && !rc)
					rc = r;
			}
			if (oss_state(ss) != AUDIO_STATE_OPEN) {
				spin_lock_irqsave(&ss->lock, &fl);
				ss->flags |= AUDIO_PARAM_NO_AUTOSTART;
				spin_unlock_irqrestore(&ss->lock, fl);
			}
			continue;
		}
		int r = snd_oss_configure(f);
		if (r) {
			if (!rc)
				rc = r;
			continue;
		}
		struct snd_ss_rt *rt = snd_rt(ss);
		int start;
		spin_lock_irqsave(&ss->lock, &fl);
		ss->flags &= ~AUDIO_PARAM_NO_AUTOSTART;
		start = ss->state == AUDIO_STATE_PREPARED &&
			(d == AUDIO_DIR_CAPTURE || rt->free_run ||
			 ss->appl_frames > ss->hw_frames);
		if (rt->free_run && d == AUDIO_DIR_PLAYBACK)
			ss->appl_frames = ss->hw_frames + rt->buffer_frames;
		spin_unlock_irqrestore(&ss->lock, fl);
		if (start) {
			r = snd_pcm_start(ss);
			if (r && !rc)
				rc = r;
		}
	}
	return rc;
}

static int oss_get_trigger(struct snd_file *f)
{
	int v = 0;
	if (f->dirs & (1u << AUDIO_DIR_PLAYBACK))
		v |= (int)(f->oss_trigger & PCM_ENABLE_OUTPUT);
	if (f->dirs & (1u << AUDIO_DIR_CAPTURE))
		v |= (int)(f->oss_trigger & PCM_ENABLE_INPUT);
	return v;
}

/* SILENCE: what is queued and not yet played becomes silence. */
static int oss_silence(struct snd_file *f)
{
	struct snd_pcm_substream *ss = snd_file_ss(f, AUDIO_DIR_PLAYBACK);
	uint64_t fl, hw, appl;
	uint32_t fb, bb;
	uint8_t *area;

	if (!ss || oss_state(ss) == AUDIO_STATE_OPEN)
		return 0;
	(void)snd_pcm_avail(ss); /* refresh the position */
	spin_lock_irqsave(&ss->lock, &fl);
	hw = ss->hw_frames;
	appl = ss->appl_frames;
	fb = ss->frame_bytes;
	bb = ss->buffer_bytes;
	area = ss->dma_area;
	uint8_t fill = ss->params.format == AUDIO_FMT_U8 ? 0x80 : 0;
	spin_unlock_irqrestore(&ss->lock, fl);
	if (!area || !fb || !bb || appl <= hw)
		return 0;
	uint32_t bf = bb / fb;
	uint64_t q = appl - hw;
	if (q > bf)
		q = bf;
	uint32_t off = (uint32_t)(hw % bf) * fb;
	uint32_t bytes = (uint32_t)q * fb;
	uint32_t first = off + bytes > bb ? bb - off : bytes;
	mm_memset(area + off, fill, first);
	if (bytes > first)
		mm_memset(area, fill, bytes - first);
	return 0;
}

long snd_oss_dsp_ioctl(struct snd_file *f, unsigned long req, void *argp)
{
	const struct snd_pcm_hw *hw = oss_hw(f);
	int v = 0;
	long rc;

	switch (req) {
	case SNDCTL_DSP_RESET:
		return oss_reset(f);
	case SNDCTL_DSP_SYNC:
		return oss_sync(f);
	case SNDCTL_DSP_POST: {
		struct snd_pcm_substream *ss = snd_file_ss(f, AUDIO_DIR_PLAYBACK);
		if (!ss || oss_state(ss) != AUDIO_STATE_PREPARED)
			return 0;
		if (!(f->oss_trigger & PCM_ENABLE_OUTPUT))
			return 0;
		struct audio_pos pos;
		snd_pcm_get_pos(ss, &pos);
		if (pos.appl_frames > pos.hw_frames)
			return snd_pcm_start(ss);
		return 0;
	}
	case SNDCTL_DSP_SPEED:
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		if (v > 0) {
			uint32_t r = snd_pcm_pick_rate(hw, (uint32_t)v);
			if (r != f->oss_rate || f->oss_dirty) {
				f->oss_rate = r;
				oss_touch(f);
			} else {
				f->oss_used = 1;
			}
		}
		return oss_put_int(argp, (int)f->oss_rate);
	case SOUND_PCM_READ_RATE:
		return oss_put_int(argp, (int)f->oss_rate);
	case SNDCTL_DSP_STEREO:
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		{
			uint32_t ch = snd_pcm_pick_channels(hw, v ? 2 : 1);
			if (ch != f->oss_channels) {
				f->oss_channels = ch;
				oss_touch(f);
			}
		}
		return oss_put_int(argp, f->oss_channels >= 2 ? 1 : 0);
	case SNDCTL_DSP_CHANNELS:
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		if (v > 0) {
			uint32_t ch = snd_pcm_pick_channels(hw, (uint32_t)v);
			if (ch != f->oss_channels) {
				f->oss_channels = ch;
				oss_touch(f);
			} else {
				f->oss_used = 1;
			}
		}
		return oss_put_int(argp, (int)f->oss_channels);
	case SOUND_PCM_READ_CHANNELS:
		return oss_put_int(argp, (int)f->oss_channels);
	case SNDCTL_DSP_SETFMT:
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		if (v != AFMT_QUERY) {
			uint32_t fmt = snd_pcm_pick_format(hw, oss_afmt_to_fmt(v));
			if (fmt && fmt != f->oss_format) {
				f->oss_format = fmt;
				oss_touch(f);
			} else {
				f->oss_used = 1;
			}
		}
		return oss_put_int(argp, oss_fmt_to_afmt(f->oss_format));
	case SOUND_PCM_READ_BITS:
		return oss_put_int(argp, oss_fmt_to_afmt(f->oss_format));
	case SNDCTL_DSP_GETFMTS: {
		int m = 0;
		for (int d = 0; d < 2; d++)
			if (f->dirs & (1u << d))
				m |= oss_formats_mask(&f->pcm->hw[d]);
		return oss_put_int(argp, m);
	}
	case SNDCTL_DSP_GETBLKSIZE: {
		struct snd_pcm_substream *ss = snd_file_ss(f, f->primary);
		if (!ss)
			return -EINVAL;
		rc = snd_oss_configure(f);
		if (rc)
			return rc;
		return oss_put_int(argp, (int)ss->period_bytes);
	}
	case SOUND_PCM_WRITE_FILTER:
		return -EINVAL;
	case SNDCTL_DSP_SUBDIVIDE:
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		if (v == 0)
			return oss_put_int(argp, (int)f->oss_subdivide);
		if (v != 1 && v != 2 && v != 4 && v != 8 && v != 16)
			return -EINVAL;
		if ((uint32_t)v != f->oss_subdivide) {
			f->oss_subdivide = (uint32_t)v;
			oss_touch(f);
		}
		return oss_put_int(argp, v);
	case SNDCTL_DSP_SETFRAGMENT: {
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		uint32_t shift = (uint32_t)v & 0xffff;
		uint32_t count = ((uint32_t)v >> 16) & 0x7fff;
		if (shift < 4)
			shift = 4;
		if (shift > 17)
			shift = 17; /* 16 bytes .. 128 KiB */
		if (count == 0x7fff)
			count = 0; /* "as many as you like": the default */
		else if (count < 2)
			count = 2;
		f->oss_frag_bytes = 1u << shift;
		f->oss_frag_count = count;
		for (int d = 0; d < 2; d++) {
			struct snd_pcm_substream *ss = snd_file_ss(f, d);
			if (ss) {
				ss->oss_frag_shift = shift;
				ss->oss_frag_max = count;
			}
		}
		oss_touch(f);
		return 0;
	}
	case SNDCTL_DSP_GETOSPACE:
		return oss_space(f, AUDIO_DIR_PLAYBACK, argp);
	case SNDCTL_DSP_GETISPACE:
		return oss_space(f, AUDIO_DIR_CAPTURE, argp);
	case SNDCTL_DSP_NONBLOCK:
		if (f->vfs)
			f->vfs->flags |= O_NONBLOCK;
		return 0;
	case SNDCTL_DSP_GETCAPS:
		return oss_put_int(argp, oss_caps(f));
	case SNDCTL_DSP_GETTRIGGER:
		return oss_put_int(argp, oss_get_trigger(f));
	case SNDCTL_DSP_SETTRIGGER:
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		return oss_set_trigger(f, v);
	case SNDCTL_DSP_GETIPTR:
		return oss_ptr(f, AUDIO_DIR_CAPTURE, argp);
	case SNDCTL_DSP_GETOPTR:
		return oss_ptr(f, AUDIO_DIR_PLAYBACK, argp);
	case SNDCTL_DSP_MAPINBUF:
	case SNDCTL_DSP_MAPOUTBUF:
	case SNDCTL_DSP_SETSYNCRO:
		return -EINVAL;
	case SNDCTL_DSP_SETDUPLEX:
		return f->dirs == ((1u << AUDIO_DIR_PLAYBACK) | (1u << AUDIO_DIR_CAPTURE)) ?
			       0 :
			       -EINVAL;
	case SNDCTL_DSP_GETODELAY:
		if (!snd_file_ss(f, AUDIO_DIR_PLAYBACK))
			return -EINVAL;
		return oss_put_int(argp, oss_odelay(f));
	case SNDCTL_DSP_HALT_INPUT:
		return oss_drop_dir(f, AUDIO_DIR_CAPTURE);
	case SNDCTL_DSP_HALT_OUTPUT:
		return oss_drop_dir(f, AUDIO_DIR_PLAYBACK);
	case SNDCTL_DSP_LOW_WATER:
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		if (v < 0)
			return -EINVAL;
		f->oss_low_water = (uint32_t)v;
		for (int d = 0; d < 2; d++) {
			struct snd_pcm_substream *ss = snd_file_ss(f, d);
			if (ss)
				oss_apply_low_water(f, ss);
		}
		return 0;
	case SNDCTL_DSP_SKIP:
		/* Throw away what is queued; the next write starts afresh. */
		return oss_drop_dir(f, AUDIO_DIR_PLAYBACK);
	case SNDCTL_DSP_SILENCE:
		return oss_silence(f);
	default:
		return -EINVAL; /* the OSS answer for an unsupported request */
	}
}

/* ======================================================================
 * Mixer
 * ====================================================================== */

/* OSS mixer channel -> AUDIO_CTL_*, -1 = none. */
static int oss_mixer_ctl(int dev)
{
	switch (dev) {
	case SOUND_MIXER_VOLUME: return AUDIO_CTL_MASTER;
	case SOUND_MIXER_PCM: return AUDIO_CTL_PCM;
	case SOUND_MIXER_SPEAKER: return AUDIO_CTL_SPEAKER;
	case SOUND_MIXER_LINE: return AUDIO_CTL_LINE;
	case SOUND_MIXER_MIC: return AUDIO_CTL_MIC_BOOST;
	case SOUND_MIXER_IGAIN: return AUDIO_CTL_CAPTURE;
	case SOUND_MIXER_RECLEV: return AUDIO_CTL_CAPTURE;
	case SOUND_MIXER_PHONEOUT: return AUDIO_CTL_HEADPHONE;
	case SOUND_MIXER_DIGITAL1: return AUDIO_CTL_HDMI;
	default: return -1;
	}
}

static int oss_devmask(struct snd_card *card)
{
	struct audio_volume v;
	int m = 0;

	for (int dev = 0; dev < SOUND_MIXER_NRDEVICES; dev++) {
		int ctl = oss_mixer_ctl(dev);
		if (ctl < 0)
			continue;
		if (snd_ctl_get_volume(card, (uint32_t)ctl, &v) == 0)
			m |= 1 << dev;
	}
	return m;
}

static int oss_has_capture(struct snd_card *card)
{
	for (int i = 0; i < card->npcm && i < SND_MAX_PCMS; i++)
		if (card->pcm[i] && card->pcm[i]->hw[AUDIO_DIR_CAPTURE].formats)
			return 1;
	return 0;
}

static int oss_recmask(struct snd_card *card)
{
	int dm;

	if (!oss_has_capture(card))
		return 0;
	dm = oss_devmask(card);
	int m = SOUND_MASK_MIC;
	if (card->num_inputs > 1 || (dm & SOUND_MASK_LINE))
		m |= SOUND_MASK_LINE;
	return m;
}

static long oss_mixer_info(struct snd_card *card, void *argp)
{
	mixer_info mi;

	mm_memset(&mi, 0, sizeof(mi));
	snd_strlcpy(mi.id, card->driver, sizeof(mi.id));
	snd_strlcpy(mi.name, card->name, sizeof(mi.name));
	mi.modify_counter = (int)card->modify_counter;
	return snd_user_out(argp, &mi, sizeof(mi));
}

long snd_oss_mixer_ioctl(struct snd_file *f, unsigned long req, void *argp)
{
	struct snd_card *card = f->card;
	unsigned nr = _IOC_NR(req);
	unsigned dir = _IOC_DIR(req);
	int v = 0;
	long rc;

	if (req == SOUND_MIXER_INFO)
		return oss_mixer_info(card, argp);
	if (req == OSS_GETVERSION)
		return oss_put_int(argp, SOUND_VERSION);
	if (_IOC_SIZE(req) != sizeof(int))
		return -ENOTTY;
	if (card->disconnected)
		return -ENODEV;

	if (dir == _IOC_READ) {
		switch (nr) {
		case SOUND_MIXER_DEVMASK:
			return oss_put_int(argp, oss_devmask(card));
		case SOUND_MIXER_STEREODEVS:
			return oss_put_int(argp, oss_devmask(card) & ~SOUND_MASK_MIC);
		case SOUND_MIXER_RECMASK:
			return oss_put_int(argp, oss_recmask(card));
		case SOUND_MIXER_RECSRC:
			return oss_put_int(argp, oss_has_capture(card) ? SOUND_MASK_MIC : 0);
		case SOUND_MIXER_CAPS:
			return oss_put_int(argp, SOUND_CAP_EXCL_INPUT);
		case SOUND_MIXER_OUTSRC:
		case SOUND_MIXER_OUTMASK:
			return oss_put_int(argp, 0);
		default:
			break;
		}
		if (nr >= SOUND_MIXER_NRDEVICES)
			return -EINVAL;
		int ctl = oss_mixer_ctl((int)nr);
		if (ctl < 0)
			return -EINVAL;
		struct audio_volume vol;
		rc = snd_ctl_get_volume(card, (uint32_t)ctl, &vol);
		if (rc)
			return rc == -ENOENT ? -EINVAL : rc;
		return oss_put_int(argp, SOUND_MIXER_LEVEL(vol.left, vol.right));
	}

	if (dir == (_IOC_READ | _IOC_WRITE)) {
		rc = oss_get_int(argp, &v);
		if (rc)
			return rc;
		if (nr == SOUND_MIXER_RECSRC) {
			int mask = oss_recmask(card);
			if (!mask)
				return -EINVAL;
			/* One recording source here: the input routing
			 * follows the jacks (AUDIO_OUTSEL_AUTO). */
			struct audio_setoutput s;
			mm_memset(&s, 0, sizeof(s));
			s.mode = AUDIO_OUTSEL_AUTO;
			(void)snd_ctl_set_output(card, &s, 1);
			return oss_put_int(argp, SOUND_MASK_MIC);
		}
		if (nr >= SOUND_MIXER_NRDEVICES)
			return -EINVAL;
		int ctl = oss_mixer_ctl((int)nr);
		if (ctl < 0)
			return -EINVAL;
		uint32_t l = (uint32_t)v & 0xff;
		uint32_t r = ((uint32_t)v >> 8) & 0xff;
		if (l > 100)
			l = 100;
		if (r > 100)
			r = 100;
		rc = snd_ctl_set_volume(card, (uint32_t)ctl, l, r);
		if (rc)
			return rc == -ENOENT ? -EINVAL : rc;
		struct audio_volume vol;
		if (snd_ctl_get_volume(card, (uint32_t)ctl, &vol) == 0) {
			l = vol.left;
			r = vol.right;
		}
		return oss_put_int(argp, SOUND_MIXER_LEVEL(l, r));
	}
	return -ENOTTY;
}

/* ======================================================================
 * SNDCTL_SYSINFO
 * ====================================================================== */

long snd_oss_sysinfo_ioctl(struct snd_file *f, unsigned long req, void *argp)
{
	oss_sysinfo *si;
	long rc;
	(void)f;

	if (req != SNDCTL_SYSINFO)
		return -ENOTTY;
	si = kalloc(sizeof(*si));
	if (!si)
		return -ENOMEM;
	mm_memset(si, 0, sizeof(*si));
	snd_strlcpy(si->product, "LikeOS Sound", sizeof(si->product));
	snd_strlcpy(si->version, "1.0", sizeof(si->version));
	si->versionnum = SOUND_VERSION;
	snd_strlcpy(si->license, "LikeOS", sizeof(si->license));
	snd_strlcpy(si->revision_info, "LikeOS sound core, OSS compatibility layer",
		    sizeof(si->revision_info));

	int n = snd_card_count();
	for (int i = 0; i < n; i++) {
		struct snd_card *c = snd_card_get(i);
		if (!c || c->disconnected)
			continue;
		si->numcards++;
		si->nummixers++;
		int has_pcm = 0;
		for (int p = 0; p < c->npcm && p < SND_MAX_PCMS; p++) {
			struct snd_pcm *pcm = c->pcm[p];
			if (!pcm)
				continue;
			has_pcm = 1;
			for (int d = 0; d < 2; d++) {
				if (!pcm->hw[d].formats)
					continue;
				si->numaudioengines++;
				if (pcm->sub[d].opened && i < 8 * 32)
					si->openedaudio[i / 32] |= 1 << (i % 32);
			}
		}
		if (has_pcm)
			si->numaudios++;
	}
	rc = snd_user_out(argp, si, sizeof(*si));
	kfree(si);
	return rc;
}
