// audiod device access: /dev/audioN through the native ioctl interface.
//
// The device is opened non-blocking with AUDIO_PARAM_NO_AUTOSTART: the
// engine fills the whole ring first and then starts it, so the first
// period never underruns.  After that, writes follow POLLOUT and every
// write is preceded by an AUDIO_GETPOS so the engine renders exactly what
// fits.  An underrun (EPIPE or state XRUN) is recovered with
// AUDIO_PREPARE plus a refill; a disconnect (ENODEV, state DISCONNECTED or
// the event) closes the device and the engine falls back to a null sink
// paced by the monotonic clock, so clients keep running at the right speed
// until the hardware comes back.
//
// Mixer controls and the event queue live on the card's mixer node; the
// PCM node is used as a fallback for both when the mixer node is missing.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/audio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "audiod.h"

/* The protocol's output selection codes are the device's. */
_Static_assert(OUTSEL_AUTO == AUDIO_OUTSEL_AUTO, "outsel auto");
_Static_assert(OUTSEL_ID == AUDIO_OUTSEL_ID, "outsel id");
_Static_assert(OUTSEL_ALL == AUDIO_OUTSEL_ALL, "outsel all");
_Static_assert(AUD_MAX_OUTPUTS == AUDIO_MAX_OUTPUTS, "output count");

uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t now_ms(void)
{
	return now_ns() / 1000000ull;
}

/* ---- format mapping ------------------------------------------------------- */

static const struct {
	int sf;
	uint32_t hw;
} fmt_map[] = {
	{ SF_S16LE, AUDIO_FMT_S16_LE },	  { SF_S24LE, AUDIO_FMT_S24_LE },
	{ SF_S32LE, AUDIO_FMT_S32_LE },	  { SF_U8, AUDIO_FMT_U8 },
	{ SF_S24_3LE, AUDIO_FMT_S24_3LE }, { SF_F32LE, AUDIO_FMT_FLOAT_LE },
	{ SF_S20LE, AUDIO_FMT_S20_LE },
};

static uint32_t sf_to_hw(int sf)
{
	for (size_t i = 0; i < sizeof(fmt_map) / sizeof(fmt_map[0]); i++)
		if (fmt_map[i].sf == sf)
			return fmt_map[i].hw;
	return 0;
}

static int hw_to_sf(uint32_t hw)
{
	for (size_t i = 0; i < sizeof(fmt_map) / sizeof(fmt_map[0]); i++)
		if (fmt_map[i].hw == hw)
			return fmt_map[i].sf;
	return SF_NONE;
}

/* Preference when the configuration says "auto": 16 bit first (what every
 * codec does natively and what the plan asks for), then the deeper
 * formats, then the odd ones. */
static const int auto_formats[] = { SF_S16LE, SF_S32LE, SF_S24LE,
				    SF_S24_3LE, SF_S20LE, SF_F32LE, SF_U8 };

static const uint32_t auto_rates[] = { 48000, 44100, 96000, 88200,
				       32000, 192000, 176400, 22050,
				       16000, 11025, 8000, 384000 };

static void copy_str(char *dst, size_t cap, const char *src, size_t srccap)
{
	size_t n = 0;
	while (n < srccap && n + 1 < cap && src[n]) {
		unsigned char c = (unsigned char)src[n];
		dst[n] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
		n++;
	}
	dst[n] = '\0';
}

static void mixer_path_for(int card, char *buf, size_t cap)
{
	if (card == 0)
		snprintf(buf, cap, "/dev/mixer");
	else
		snprintf(buf, cap, "/dev/mixer%d", card);
}

/* ioctl on the control node, falling back to the PCM node */
static int ctl_ioctl(struct audio_dev *d, unsigned long req, void *arg)
{
	int r = -1;
	if (d->evfd >= 0) {
		r = ioctl(d->evfd, req, arg);
		if (r == 0 || (errno != ENOTTY && errno != EINVAL))
			return r;
	}
	if (d->fd >= 0)
		r = ioctl(d->fd, req, arg);
	return r;
}

static void outputs_from_hw(struct aud_outputs *o, const struct audio_outputs *h)
{
	memset(o, 0, sizeof(*o));
	uint32_t n = h->count;
	if (n > AUD_MAX_OUTPUTS)
		n = AUD_MAX_OUTPUTS;
	o->count = n;
	o->mode = h->mode;
	o->selected = h->selected;
	for (uint32_t i = 0; i < n; i++) {
		o->out[i].id = h->out[i].id;
		o->out[i].type = h->out[i].type;
		o->out[i].present = h->out[i].present ? 1 : 0;
		o->out[i].active = h->out[i].active ? 1 : 0;
		copy_str(o->out[i].name, sizeof(o->out[i].name),
			 h->out[i].name, sizeof(h->out[i].name));
		copy_str(o->out[i].monitor, sizeof(o->out[i].monitor),
			 h->out[i].monitor, sizeof(h->out[i].monitor));
	}
}

int dev_refresh_outputs(struct audio_dev *d)
{
	struct audio_outputs h;
	if (d->null || d->fd < 0) {
		memset(&d->outs, 0, sizeof(d->outs));
		d->have_outs = 0;
		return -ENODEV;
	}
	memset(&h, 0, sizeof(h));
	if (ctl_ioctl(d, AUDIO_GETOUTPUTS, &h) < 0) {
		memset(&d->outs, 0, sizeof(d->outs));
		d->have_outs = 0;
		return -errno;
	}
	outputs_from_hw(&d->outs, &h);
	d->have_outs = 1;
	return 0;
}

int dev_set_output(struct audio_dev *d, uint32_t mode, uint32_t id)
{
	struct audio_setoutput s;
	if (d->null || d->fd < 0)
		return -ENODEV;
	memset(&s, 0, sizeof(s));
	s.mode = mode;
	s.id = id;
	if (ctl_ioctl(d, AUDIO_SETOUTPUT, &s) < 0)
		return -errno;
	dev_refresh_outputs(d);
	return 0;
}

static void probe_controls(struct audio_dev *d, const struct audiod_config *cfg)
{
	struct audio_volume v;
	struct audio_mute m;

	d->hw_vol_ctl = -1;
	d->hw_mute_ctl = -1;
	if (cfg->hw_volume == HWVOL_NO)
		return;
	if (cfg->hw_volume == HWVOL_AUTO &&
	    !(d->info_flags & AUDIO_INFO_HW_VOLUME))
		return;

	static const uint32_t ctls[] = { AUDIO_CTL_MASTER, AUDIO_CTL_PCM };
	for (size_t i = 0; i < sizeof(ctls) / sizeof(ctls[0]); i++) {
		memset(&v, 0, sizeof(v));
		v.control = ctls[i];
		if (ctl_ioctl(d, AUDIO_GETVOL, &v) == 0 && v.steps > 0) {
			d->hw_vol_ctl = (int)ctls[i];
			break;
		}
	}
	memset(&m, 0, sizeof(m));
	m.control = AUDIO_CTL_MASTER;
	if (ctl_ioctl(d, AUDIO_GETMUTE, &m) == 0)
		d->hw_mute_ctl = AUDIO_CTL_MASTER;

	if (d->hw_vol_ctl >= 0)
		log_info("%s: hardware volume control %d", d->path,
			 d->hw_vol_ctl);
	else
		log_info("%s: software volume", d->path);
}

int dev_set_volume(struct audio_dev *d, uint32_t l, uint32_t r)
{
	struct audio_volume v;
	if (d->null || d->fd < 0 || d->hw_vol_ctl < 0)
		return -ENOTSUP;
	memset(&v, 0, sizeof(v));
	v.control = (uint32_t)d->hw_vol_ctl;
	v.left = l > 100 ? 100 : l;
	v.right = r > 100 ? 100 : r;
	if (ctl_ioctl(d, AUDIO_SETVOL, &v) < 0)
		return -errno;
	return 0;
}

int dev_get_volume(struct audio_dev *d, uint32_t *l, uint32_t *r)
{
	struct audio_volume v;
	if (d->null || d->fd < 0 || d->hw_vol_ctl < 0)
		return -ENOTSUP;
	memset(&v, 0, sizeof(v));
	v.control = (uint32_t)d->hw_vol_ctl;
	if (ctl_ioctl(d, AUDIO_GETVOL, &v) < 0)
		return -errno;
	*l = v.left > 100 ? 100 : v.left;
	*r = v.right > 100 ? 100 : v.right;
	return 0;
}

int dev_set_mute(struct audio_dev *d, int mute)
{
	struct audio_mute m;
	if (d->null || d->fd < 0 || d->hw_mute_ctl < 0)
		return -ENOTSUP;
	memset(&m, 0, sizeof(m));
	m.control = (uint32_t)d->hw_mute_ctl;
	m.mute = mute ? 1 : 0;
	if (ctl_ioctl(d, AUDIO_SETMUTE, &m) < 0)
		return -errno;
	return 0;
}

/* ---- opening -------------------------------------------------------------- */

static void dev_reset_struct(struct audio_dev *d)
{
	memset(d, 0, sizeof(*d));
	d->fd = -1;
	d->evfd = -1;
	d->card = -1;
	d->hw_vol_ctl = -1;
	d->hw_mute_ctl = -1;
}

static int alloc_pend(struct audio_dev *d)
{
	size_t need = (size_t)d->buffer_frames * d->frame_bytes;
	if (need == 0)
		return -EINVAL;
	d->pend = malloc(need);
	if (!d->pend)
		return -ENOMEM;
	d->pend_cap = need;
	d->pend_off = d->pend_len = 0;
	return 0;
}

static int choose_format(const struct audio_info *info,
			 const struct audiod_config *cfg)
{
	if (cfg->format != SF_NONE) {
		uint32_t hw = sf_to_hw(cfg->format);
		if (hw && (info->formats & AUDIO_FMT_BIT(hw)))
			return cfg->format;
		log_warn("configured format %s not supported by the device; "
			 "choosing automatically",
			 sf_name(cfg->format));
	}
	for (size_t i = 0; i < sizeof(auto_formats) / sizeof(auto_formats[0]);
	     i++) {
		uint32_t hw = sf_to_hw(auto_formats[i]);
		if (info->formats & AUDIO_FMT_BIT(hw))
			return auto_formats[i];
	}
	/* the driver did not say: ask for 16 bit and see what comes back */
	return SF_S16LE;
}

static uint32_t choose_rate(const struct audio_info *info,
			    const struct audiod_config *cfg)
{
	if (cfg->rate) {
		if (!info->rates || (info->rates & audio_rate_bit(cfg->rate)))
			return cfg->rate;
		log_warn("configured rate %u not supported by the device; "
			 "choosing automatically",
			 cfg->rate);
	}
	if (!info->rates)
		return 48000;
	for (size_t i = 0; i < sizeof(auto_rates) / sizeof(auto_rates[0]); i++)
		if (info->rates & audio_rate_bit(auto_rates[i]))
			return auto_rates[i];
	return 48000;
}

/* Choose and set the stream parameters on the open handle (after open,
 * and again after AUDIO_SELECTPCM, which drops them).  Fills the format
 * fields of `d' and (re)allocates the render buffer. */
static int negotiate(struct audio_dev *d, const struct audio_info *info,
		     const struct audiod_config *cfg)
{
	struct audio_params p;
	const char *path = d->path;

	int sf = choose_format(info, cfg);
	uint32_t rate = choose_rate(info, cfg);
	uint32_t ch = cfg->channels ? cfg->channels : 2;
	if (info->max_channels && ch > info->max_channels)
		ch = info->max_channels;
	if (info->min_channels && ch < info->min_channels)
		ch = info->min_channels;
	if (ch < 1 || ch > DSP_MAX_CH)
		ch = 2;

	size_t fb = sf_bytes(sf) * ch;
	uint32_t period = rate / 1000u * cfg->period_ms;
	if (period == 0)
		period = 64;
	uint32_t periods = cfg->periods;
	if (info->min_period_bytes && period * fb < info->min_period_bytes)
		period = (uint32_t)((info->min_period_bytes + fb - 1) / fb);
	if (info->max_period_bytes && period * fb > info->max_period_bytes)
		period = (uint32_t)(info->max_period_bytes / fb);
	if (info->min_periods && periods < info->min_periods)
		periods = info->min_periods;
	if (info->max_periods && periods > info->max_periods)
		periods = info->max_periods;
	while (info->max_buffer_bytes && periods > 2 &&
	       (uint64_t)period * periods * fb > info->max_buffer_bytes)
		periods--;

	memset(&p, 0, sizeof(p));
	p.direction = AUDIO_DIR_PLAYBACK;
	p.format = sf_to_hw(sf);
	p.rate = rate;
	p.channels = ch;
	p.period_frames = period;
	p.periods = periods;
	p.flags = AUDIO_PARAM_NO_AUTOSTART;
	if (ioctl(d->fd, AUDIO_SETPARAMS, &p) < 0) {
		int e = errno;
		log_err("%s: AUDIO_SETPARAMS (%s %u Hz %u ch) failed: %s", path,
			sf_name(sf), rate, ch, strerror(e));
		return -e;
	}

	/* what was granted may differ: take it, the mixer adapts */
	int gfmt = hw_to_sf(p.format);
	if (gfmt == SF_NONE || p.rate < PROTO_MIN_RATE ||
	    p.rate > PROTO_MAX_RATE || p.channels < 1 ||
	    p.channels > DSP_MAX_CH || p.period_frames == 0 || p.periods < 2 ||
	    p.period_frames > 1u << 20 || p.periods > 1024) {
		log_err("%s: unusable parameters granted (format %u, %u Hz, "
			"%u ch, period %u x %u)",
			path, p.format, p.rate, p.channels, p.period_frames,
			p.periods);
		return -EINVAL;
	}
	d->fmt = gfmt;
	d->hw_fmt = p.format;
	d->rate = p.rate;
	d->channels = (int)p.channels;
	d->period_frames = p.period_frames;
	d->periods = p.periods;
	d->frame_bytes = sf_bytes(d->fmt) * (size_t)d->channels;
	d->buffer_frames = d->period_frames * d->periods;
	if (p.buffer_bytes && p.frame_bytes == d->frame_bytes &&
	    p.buffer_bytes / d->frame_bytes >= d->period_frames * 2)
		d->buffer_frames = (uint32_t)(p.buffer_bytes / d->frame_bytes);

	free(d->pend);
	d->pend = NULL;
	d->pend_cap = 0;
	return alloc_pend(d);
}

/* Which PCM device of the card the handle is on, and what it feeds. */
static void pcm_probe(struct audio_dev *d)
{
	struct audio_pcm_info pi;

	d->pcm_count = 1;
	d->pcm_index = 0;
	d->pcm_output_id = 0;
	memset(&pi, 0, sizeof(pi));
	pi.index = 0;
	if (ioctl(d->fd, AUDIO_GETPCMINFO, &pi) < 0)
		return; /* a card with a single PCM device */
	d->pcm_count = pi.count ? pi.count : 1;
	if (d->pcm_count > 64)
		d->pcm_count = 64;
	for (uint32_t i = 0; i < d->pcm_count; i++) {
		memset(&pi, 0, sizeof(pi));
		pi.index = i;
		if (ioctl(d->fd, AUDIO_GETPCMINFO, &pi) < 0)
			continue;
		if (pi.flags & AUDIO_PCM_SELECTED) {
			d->pcm_index = i;
			d->pcm_output_id = pi.output_id;
			break;
		}
	}
}

/* Find the playback PCM that feeds `output_id' (0: the general/analog
 * device).  Returns its index or a negative errno. */
static int pcm_find(struct audio_dev *d, uint32_t output_id, uint32_t *flags)
{
	struct audio_pcm_info pi;
	memset(&pi, 0, sizeof(pi));
	pi.index = 0;
	if (ioctl(d->fd, AUDIO_GETPCMINFO, &pi) < 0)
		return output_id == 0 ? 0 : -ENOENT;
	uint32_t count = pi.count > 64 ? 64 : pi.count;
	for (uint32_t i = 0; i < count; i++) {
		memset(&pi, 0, sizeof(pi));
		pi.index = i;
		if (ioctl(d->fd, AUDIO_GETPCMINFO, &pi) < 0)
			continue;
		if (!(pi.flags & AUDIO_PCM_PLAYBACK) || pi.output_id != output_id)
			continue;
		if (flags)
			*flags = pi.flags;
		return (int)i;
	}
	return -ENOENT;
}

int dev_has_pcm_for(struct audio_dev *d, uint32_t output_id)
{
	if (d->null || d->fd < 0)
		return 0;
	return pcm_find(d, output_id, NULL) >= 0;
}

int dev_select_pcm(struct audio_dev *d, const struct audiod_config *cfg,
		   uint32_t output_id)
{
	uint32_t flags = 0;
	struct audio_info info;

	if (d->null || d->fd < 0)
		return -ENODEV;
	if (d->pcm_output_id == output_id)
		return 0;
	int idx = pcm_find(d, output_id, &flags);
	if (idx < 0)
		return idx;
	if (flags & AUDIO_PCM_SELECTED) {
		d->pcm_index = (uint32_t)idx;
		d->pcm_output_id = output_id;
		return 0;
	}
	if (flags & AUDIO_PCM_BUSY)
		return -EBUSY;

	/* the handle must be idle to move */
	dev_stop(d);
	uint32_t ui = (uint32_t)idx;
	if (ioctl(d->fd, AUDIO_SELECTPCM, &ui) < 0) {
		int e = errno;
		log_warn("%s: AUDIO_SELECTPCM %u: %s", d->path, ui, strerror(e));
		/* still on the old device with its parameters: make it
		 * playable again */
		dev_prepare(d);
		return -e;
	}

	/* the new device has its own capabilities and no parameters */
	memset(&info, 0, sizeof(info));
	if (ioctl(d->fd, AUDIO_GETINFO, &info) < 0) {
		int e = errno;
		log_err("%s: AUDIO_GETINFO after selecting PCM %u: %s", d->path,
			ui, strerror(e));
		return -e;
	}
	int rc = negotiate(d, &info, cfg);
	if (rc < 0)
		return rc;
	d->pcm_index = ui;
	d->pcm_output_id = output_id;
	d->info_flags = info.flags;
	dev_prepare(d);
	log_info("%s: now playing on PCM %u (output 0x%x), %s %u Hz %d ch",
		 d->path, ui, output_id, sf_name(d->fmt), d->rate, d->channels);
	return 1;
}

static int try_open(struct audio_dev *d, const char *path,
		    const struct audiod_config *cfg)
{
	struct audio_info info;
	uint32_t ver = 0;
	int fd;

	fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
	if (fd < 0) {
		int e = errno;
		if (e == EBUSY)
			log_warn("%s is in use by another program", path);
		else if (e == EACCES || e == EPERM)
			log_err("%s: permission denied (is audiod in the audio "
				"group?)",
				path);
		else if (e != ENOENT && e != ENXIO && e != ENODEV)
			log_warn("cannot open %s: %s", path, strerror(e));
		return -e;
	}

	if (ioctl(fd, AUDIO_GETVERSION, &ver) < 0 || ver != AUDIO_API_VERSION) {
		log_warn("%s: not a native audio device (version %u)", path, ver);
		close(fd);
		return -ENOTTY;
	}
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, AUDIO_GETINFO, &info) < 0) {
		int e = errno;
		log_warn("%s: AUDIO_GETINFO failed: %s", path, strerror(e));
		close(fd);
		return -e;
	}
	if (!(info.flags & AUDIO_INFO_PLAYBACK)) {
		log_info("%s: no playback", path);
		close(fd);
		return -ENODEV;
	}

	dev_reset_struct(d);
	d->fd = fd;
	snprintf(d->path, sizeof(d->path), "%s", path);
	d->card = (int)(info.card < AUD_MAX_CARDS ? info.card : 0);
	d->have_info = 1;
	copy_str(d->card_name, sizeof(d->card_name), info.card_name,
		 sizeof(info.card_name));
	copy_str(d->codec, sizeof(d->codec), info.codec, sizeof(info.codec));
	copy_str(d->driver, sizeof(d->driver), info.driver,
		 sizeof(info.driver));
	copy_str(d->bus, sizeof(d->bus), info.bus, sizeof(info.bus));
	d->info_flags = info.flags;

	int rc = negotiate(d, &info, cfg);
	if (rc < 0) {
		close(fd);
		d->fd = -1;
		return rc;
	}
	pcm_probe(d);

	/* ---- control and event node ---- */
	mixer_path_for(d->card, d->mixer_path, sizeof(d->mixer_path));
	d->evfd = open(d->mixer_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC |
					      O_NOCTTY);
	/* Find where the event queue answers.  A probe that returns an
	 * event consumes it; harmless, since the outputs and volume are
	 * read fresh just below. */
	struct audio_event ev;
	d->events_on_pcm = 0;
	d->events_ok = 0;
	if (d->evfd >= 0 && (ioctl(d->evfd, AUDIO_GETEVENT, &ev) == 0 ||
			     errno == EAGAIN)) {
		d->events_ok = 1;
	} else if (ioctl(fd, AUDIO_GETEVENT, &ev) == 0 || errno == EAGAIN) {
		d->events_on_pcm = 1;
		d->events_ok = 1;
	} else {
		log_info("%s: no event queue; jacks are not followed", path);
	}

	probe_controls(d, cfg);
	dev_refresh_outputs(d);

	log_info("%s: %s%s%s, %s %u Hz %d ch, period %u frames x %u "
		 "(%u ms latency)",
		 path, d->card_name, d->codec[0] ? " / " : "", d->codec,
		 sf_name(d->fmt), d->rate, d->channels, d->period_frames,
		 d->periods,
		 (unsigned)((uint64_t)d->buffer_frames * 1000u / d->rate));
	return 0;
}

int dev_open(struct audio_dev *d, const struct audiod_config *cfg,
	     int card_hint)
{
	char path[64];
	int rc = -ENODEV;

	if (strcmp(cfg->device, "auto") != 0) {
		if (cfg->device[0] >= '0' && cfg->device[0] <= '7' &&
		    cfg->device[1] == '\0')
			snprintf(path, sizeof(path), "/dev/audio%c",
				 cfg->device[0]);
		else
			snprintf(path, sizeof(path), "%s", cfg->device);
		return try_open(d, path, cfg);
	}

	if (card_hint >= 0 && card_hint < AUD_MAX_CARDS) {
		snprintf(path, sizeof(path), "/dev/audio%d", card_hint);
		rc = try_open(d, path, cfg);
		if (rc == 0)
			return 0;
	}
	for (int i = 0; i < AUD_MAX_CARDS; i++) {
		if (i == card_hint)
			continue;
		snprintf(path, sizeof(path), "/dev/audio%d", i);
		rc = try_open(d, path, cfg);
		if (rc == 0)
			return 0;
	}
	return rc;
}

void dev_open_null(struct audio_dev *d, const struct audiod_config *cfg)
{
	dev_reset_struct(d);
	d->null = 1;
	snprintf(d->path, sizeof(d->path), "null");
	snprintf(d->card_name, sizeof(d->card_name), "Null output");
	snprintf(d->driver, sizeof(d->driver), "null");
	d->fmt = cfg->format != SF_NONE ? cfg->format : SF_S16LE;
	d->rate = cfg->rate ? cfg->rate : 48000;
	d->channels = cfg->channels ? (int)cfg->channels : 2;
	d->frame_bytes = sf_bytes(d->fmt) * (size_t)d->channels;
	d->period_frames = d->rate / 1000u * cfg->period_ms;
	if (!d->period_frames)
		d->period_frames = 64;
	d->periods = cfg->periods;
	d->buffer_frames = d->period_frames * d->periods;
	if (alloc_pend(d) < 0) {
		/* without the buffer the null sink renders nothing; the
		 * engine copes (dev_pend_buffer returns NULL) */
		d->pend_cap = 0;
	}
	d->null_t0_ns = now_ns();
	d->null_base = 0;
}

void dev_close(struct audio_dev *d)
{
	if (d->fd >= 0) {
		if (d->running)
			ioctl(d->fd, AUDIO_DROP);
		close(d->fd);
	}
	if (d->evfd >= 0)
		close(d->evfd);
	free(d->pend);
	dev_reset_struct(d);
}

/* ---- position and data ----------------------------------------------------- */

static uint64_t null_played(const struct audio_dev *d, uint64_t now)
{
	uint64_t el = now - d->null_t0_ns;
	return d->null_base + el * d->rate / 1000000000ull;
}

int dev_update(struct audio_dev *d)
{
	if (d->null) {
		uint64_t now = now_ns();
		uint64_t played = null_played(d, now);
		if (played > d->written) {
			/* we fell behind (nothing to render): restart the
			 * clock at the current position rather than letting
			 * a burst catch up */
			d->null_t0_ns = now;
			d->null_base = d->written;
			played = d->written;
		}
		uint64_t queued = d->written - played;
		if (queued > d->buffer_frames)
			queued = d->buffer_frames;
		d->delay = (uint32_t)queued;
		d->avail = d->buffer_frames - (uint32_t)queued;
		return 0;
	}
	if (d->fd < 0)
		return -ENODEV;

	struct audio_pos pos;
	memset(&pos, 0, sizeof(pos));
	if (ioctl(d->fd, AUDIO_GETPOS, &pos) < 0) {
		int e = errno;
		if (e == EPIPE)
			return -EPIPE;
		if (e == ENODEV || e == EIO || e == EBADF || e == ENXIO)
			return -ENODEV;
		return -e;
	}
	if (pos.state == AUDIO_STATE_DISCONNECTED)
		return -ENODEV;
	if (pos.state == AUDIO_STATE_XRUN) {
		d->xruns++;
		return -EPIPE;
	}
	d->avail = pos.avail_frames > d->buffer_frames ? d->buffer_frames
						       : pos.avail_frames;
	d->delay = pos.delay_frames > d->buffer_frames ? d->buffer_frames
						       : pos.delay_frames;
	return 0;
}

int dev_prepare(struct audio_dev *d)
{
	d->running = 0;
	d->pend_off = d->pend_len = 0;
	d->delay = 0;
	if (d->null) {
		d->null_t0_ns = now_ns();
		d->null_base = d->written;
		return 0;
	}
	if (d->fd < 0)
		return -ENODEV;
	if (ioctl(d->fd, AUDIO_PREPARE) < 0) {
		int e = errno;
		if (e == ENODEV || e == EIO)
			return -ENODEV;
		log_warn("%s: AUDIO_PREPARE: %s", d->path, strerror(e));
		return -e;
	}
	return 0;
}

int dev_start(struct audio_dev *d)
{
	if (d->running)
		return 0;
	if (d->null) {
		d->running = 1;
		return 0;
	}
	if (d->fd < 0)
		return -ENODEV;
	if (ioctl(d->fd, AUDIO_START) < 0) {
		int e = errno;
		if (e == ENODEV || e == EIO)
			return -ENODEV;
		if (e == EPIPE)
			return -EPIPE;
		/* EBUSY / EINVAL: already running (a write started it) */
		log_debug("%s: AUDIO_START: %s", d->path, strerror(e));
	}
	d->running = 1;
	return 0;
}

void dev_stop(struct audio_dev *d)
{
	d->pend_off = d->pend_len = 0;
	if (!d->null && d->fd >= 0)
		ioctl(d->fd, AUDIO_DROP);
	d->running = 0;
	d->delay = 0;
}

uint8_t *dev_pend_buffer(struct audio_dev *d, size_t frames)
{
	if (d->pend_len || !d->pend ||
	    frames * d->frame_bytes > d->pend_cap)
		return NULL;
	return d->pend;
}

void dev_pend_commit(struct audio_dev *d, size_t frames)
{
	d->pend_off = 0;
	d->pend_len = frames * d->frame_bytes;
}

int dev_flush(struct audio_dev *d)
{
	if (!d->pend_len)
		return 0;
	if (d->null) {
		d->written += d->pend_len / d->frame_bytes;
		d->pend_off = d->pend_len = 0;
		return 0;
	}
	if (d->fd < 0)
		return -ENODEV;

	while (d->pend_len) {
		ssize_t w = write(d->fd, d->pend + d->pend_off, d->pend_len);
		if (w < 0) {
			int e = errno;
			if (e == EINTR)
				continue;
			if (e == EAGAIN)
				return 1;
			if (e == EPIPE) {
				d->xruns++;
				return -EPIPE;
			}
			if (e == ENODEV || e == EIO || e == ENXIO || e == EBADF)
				return -ENODEV;
			log_warn("%s: write: %s", d->path, strerror(e));
			return -e;
		}
		if (w == 0)
			return 1;
		/* the kernel takes whole frames; count those */
		size_t frames = (size_t)w / d->frame_bytes;
		d->written += frames;
		d->pend_off += (size_t)w;
		d->pend_len -= (size_t)w;
	}
	d->pend_off = 0;
	return 0;
}

int dev_timeout_ms(const struct audio_dev *d)
{
	if (!d->rate)
		return 1000;
	if (d->null) {
		uint32_t want = d->period_frames;
		if (d->avail >= want)
			return 0;
		uint64_t ms = (uint64_t)(want - d->avail) * 1000u / d->rate;
		return (int)ms + 1;
	}
	/* the device wakes us through POLLOUT; this is only a safety net
	 * against a lost wakeup: two periods */
	uint64_t ms = (uint64_t)d->period_frames * 2000u / d->rate;
	return ms < 2 ? 2 : (int)ms;
}

int dev_event_fd(const struct audio_dev *d, short *events)
{
	if (d->null || !d->events_ok)
		return -1;
	/* POLLPRI only: a node whose poll reports itself always readable
	 * or writable must not turn the loop into a spin */
	*events = POLLPRI;
	return d->events_on_pcm ? d->fd : d->evfd;
}

int dev_read_events(struct audio_dev *d, dev_event_fn fn)
{
	int fd = d->events_on_pcm ? d->fd : d->evfd;
	int n = 0;
	if (fd < 0 || d->null)
		return 0;
	/* bounded: a broken queue cannot keep us here */
	for (int i = 0; i < 64; i++) {
		struct audio_event ev;
		memset(&ev, 0, sizeof(ev));
		if (ioctl(fd, AUDIO_GETEVENT, &ev) < 0) {
			if (errno == ENODEV || errno == EIO) {
				fn(AUDIO_EV_DISCONNECT, 0, 0);
				n++;
			}
			break;
		}
		fn(ev.type, ev.id, ev.value);
		n++;
	}
	/* POLLPRI that never yields an event is a broken queue; stop
	 * polling it rather than wake up for nothing forever */
	if (n == 0) {
		if (++d->ev_spurious >= 100) {
			log_warn("%s: event queue signals without events; "
				 "ignoring it",
				 d->path);
			d->events_ok = 0;
		}
	} else {
		d->ev_spurious = 0;
	}
	return n;
}

/* ---- cards ---------------------------------------------------------------- */

int dev_list_cards(const struct audio_dev *d, struct aud_cards *out)
{
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < AUD_MAX_CARDS; i++) {
		struct aud_card_entry *e = &out->card[out->count];
		if (!d->null && d->fd >= 0 && d->card == i) {
			e->index = (uint32_t)i;
			e->in_use = 1;
			snprintf(e->name, sizeof(e->name), "%s", d->card_name);
			snprintf(e->codec, sizeof(e->codec), "%s", d->codec);
			snprintf(e->driver, sizeof(e->driver), "%s", d->driver);
			out->count++;
			continue;
		}
		char path[32];
		mixer_path_for(i, path, sizeof(path));
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
		if (fd < 0)
			continue;
		struct audio_info info;
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, AUDIO_GETINFO, &info) == 0) {
			e->index = (uint32_t)i;
			e->in_use = 0;
			copy_str(e->name, sizeof(e->name), info.card_name,
				 sizeof(info.card_name));
			copy_str(e->codec, sizeof(e->codec), info.codec,
				 sizeof(info.codec));
			copy_str(e->driver, sizeof(e->driver), info.driver,
				 sizeof(info.driver));
			out->count++;
		}
		close(fd);
	}
	return (int)out->count;
}
