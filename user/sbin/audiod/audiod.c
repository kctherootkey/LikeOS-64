// audiod - the LikeOS audio service.
//
// Started by init as root ("respawn /usr/sbin/audiod -f").  It reads its
// configuration, creates /var/run/audiod and /var/lib/audiod, opens the
// sound device and the listening socket, and then gives up root for good:
// supplementary groups reduced to `audio', gid audio, uid audiod.  It checks
// that root cannot be regained before it accepts a single client.
//
// The rest is one poll loop:
//   - the device asks for frames (POLLOUT), the engine mixes exactly that
//     many from the active streams and writes them;
//   - clients send DATA into their stream rings and get REQUEST credits
//     back as the mixer frees room;
//   - jack, volume and hot-plug events come from the card's event queue;
//   - with no hardware a clock-paced null sink keeps clients moving, and
//     the device is looked for again every few seconds;
//   - after a few seconds of silence the device is stopped (no wakeups
//     while idle) and restarted when a stream has data.
// SIGTERM fades out and closes the device; SIGHUP reloads the
// configuration; SIGUSR1 logs the state of every stream.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/audio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "audiod.h"

struct server srv;

static volatile sig_atomic_t g_term, g_hup, g_usr1;
static int g_dev_lost;
static int g_fill_now;
static uint64_t g_dev_backoff_until; /* no POLLOUT before this (ms) */
static uint32_t g_reopen_interval = REOPEN_INTERVAL_MS;
static uint64_t g_prepared_written; /* d->written at the last prepare */

/* ---- signals ---------------------------------------------------------------- */

static void on_signal(int sig)
{
	if (sig == SIGTERM || sig == SIGINT)
		g_term = 1;
	else if (sig == SIGHUP)
		g_hup = 1;
	else if (sig == SIGUSR1)
		g_usr1 = 1;
}

static void setup_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART: poll() must return */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, NULL);
}

/* ---- state ---------------------------------------------------------------------- */

void state_mark_dirty(void)
{
	if (!srv.state_dirty) {
		srv.state_dirty = 1;
		srv.state_due_ms = now_ms() + STATE_SAVE_DELAY_MS;
	}
}

static void state_flush(void)
{
	struct audiod_state st;
	if (!srv.state_dirty)
		return;
	st.vol_l = srv.vol_l;
	st.vol_r = srv.vol_r;
	st.mute = srv.mute;
	st.out_mode = srv.out_mode;
	st.out_id = srv.out_id;
	st.pcm_output = srv.dev.null ? 0 : srv.dev.pcm_output_id;
	st.card = srv.card_pref;
	if (state_save(AUDIOD_STATE_PATH, &st) == 0)
		log_debug("state saved");
	srv.state_dirty = 0;
}

/* ---- the engine ----------------------------------------------------------------- */

static size_t pend_frames(void)
{
	if (!srv.dev.frame_bytes)
		return 0;
	return srv.dev.pend_len / srv.dev.frame_bytes;
}

uint32_t engine_delay_frames(void)
{
	if (srv.suspended)
		return 0;
	return srv.dev.delay + (uint32_t)pend_frames();
}

uint64_t engine_played(void)
{
	uint64_t q = engine_delay_frames();
	return srv.engine_written > q ? srv.engine_written - q : 0;
}

static void engine_render(size_t frames)
{
	struct audio_dev *d = &srv.dev;
	if (frames > srv.mix.cap_frames)
		frames = srv.mix.cap_frames;
	uint8_t *buf = dev_pend_buffer(d, frames);
	if (!buf || !frames)
		return;
	float *acc = mixer_begin(&srv.mix, frames);
	size_t active = streams_render(acc, frames);
	mixer_finish(&srv.mix, frames, buf);
	dev_pend_commit(d, frames);
	srv.engine_written += frames;
	if (active)
		srv.last_audible_ms = now_ms();
	streams_after_render();
}

void engine_wake(void)
{
	g_fill_now = 1;
	srv.last_audible_ms = now_ms();
	if (!srv.suspended)
		return;
	srv.suspended = 0;
	g_dev_backoff_until = 0;
	if (dev_prepare(&srv.dev) == -ENODEV)
		g_dev_lost = 1;
	g_prepared_written = srv.dev.written;
	log_debug("device resumed");
}

static void engine_suspend(void)
{
	if (srv.suspended)
		return;
	dev_stop(&srv.dev);
	srv.suspended = 1;
	log_debug("device suspended after %u ms of silence", srv.cfg.suspend_ms);
}

/* (Re)build the mixer for the current device format. */
static int engine_setup_mixer(void)
{
	struct audio_dev *d = &srv.dev;
	if (srv.mix_ready && srv.mix.rate == d->rate &&
	    srv.mix.channels == d->channels && srv.mix.out_fmt == d->fmt &&
	    srv.mix.cap_frames >= d->buffer_frames)
		return 0;
	if (srv.mix_ready)
		mixer_free(&srv.mix);
	srv.mix_ready = 0;
	if (mixer_init(&srv.mix, d->rate, d->channels, d->fmt,
		       d->buffer_frames) < 0) {
		log_err("cannot set up the mixer (%u Hz, %d ch)", d->rate,
			d->channels);
		return -1;
	}
	srv.mix_ready = 1;
	return 0;
}

void engine_master_changed(void)
{
	int hw = 0;
	if (!srv.dev.null && srv.dev.hw_vol_ctl >= 0)
		hw = dev_set_volume(&srv.dev, srv.vol_l, srv.vol_r) == 0;
	if (!srv.dev.null && srv.dev.hw_mute_ctl >= 0)
		dev_set_mute(&srv.dev, (int)srv.mute);
	if (srv.mix_ready)
		mixer_set_master(&srv.mix, srv.vol_l, srv.vol_r, (int)srv.mute,
				 !hw);
	state_mark_dirty();
	notify_all(AUD_SUB_VOLUME, AUD_NOTIFY_VOLUME, srv.vol_l, srv.vol_r,
		   srv.mute);
}

static void engine_go_null(const char *why);

/* The handle moved to another PCM device of the card: the format may
 * differ (an HDMI sink often offers other rates and more channels). */
static void engine_pcm_changed(void)
{
	if (engine_setup_mixer() < 0) {
		engine_go_null("cannot mix for the selected output");
		return;
	}
	streams_retarget();
	engine_master_changed();
	srv.suspended = 0;
	srv.dev.delay = 0;
	g_prepared_written = srv.dev.written;
	g_dev_backoff_until = 0;
	g_fill_now = 1;
	srv.last_audible_ms = now_ms();
	state_mark_dirty();
}

/* Automatic mode on a card whose headphone jack is a PCM device of its own
 * (the Intel DSP machines: the speaker and the jack are separate DSP
 * paths): the jack's device while headphones are plugged in, else the
 * general one (0).  HDMI/DP is never chosen automatically. */
static uint32_t auto_jack_target(struct audio_dev *d)
{
	if (!d->have_outs)
		return 0;
	for (uint32_t i = 0; i < d->outs.count; i++) {
		const struct aud_output *o = &d->outs.out[i];
		if (o->id == 0 || !o->present ||
		    (o->type != AUDIO_OUT_HEADPHONE &&
		     o->type != AUDIO_OUT_HEADSET))
			continue;
		if (dev_has_pcm_for(d, o->id))
			return o->id;
	}
	return 0;
}

int engine_route_output(uint32_t mode, uint32_t id)
{
	if (srv.dev.null || srv.dev.fd < 0)
		return -ENODEV;

	/* An output with a PCM device of its own (HDMI/DP) is reached by
	 * moving the handle there; everything else plays through the
	 * general device and the codec's pin routing. */
	uint32_t target = 0;
	if (mode == OUTSEL_ID && id != 0 && dev_has_pcm_for(&srv.dev, id))
		target = id;
	else if (mode == OUTSEL_AUTO)
		target = auto_jack_target(&srv.dev);
	int rc = dev_select_pcm(&srv.dev, &srv.cfg, target);
	if (rc < 0) {
		if (target != 0)
			return rc;
		if (rc != -ENOENT)
			log_warn("cannot return to the analog device: %s",
				 strerror(-rc));
	} else if (rc > 0) {
		engine_pcm_changed();
		if (srv.dev.null)
			return -ENODEV;
	}

	int r2 = dev_set_output(&srv.dev, mode, id);
	if (r2 < 0 && target == 0 && r2 != -ENOTTY && r2 != -ENOTSUP)
		return r2;
	/* for an HDMI/DP PCM the pin selection is informational */
	return 0;
}

static void engine_apply_output(void)
{
	if (srv.dev.null || srv.dev.fd < 0)
		return;
	/* AUTO is the driver default; only push an explicit choice */
	if (srv.out_mode != OUTSEL_AUTO) {
		int rc = engine_route_output(srv.out_mode, srv.out_id);
		if (rc < 0)
			log_warn("cannot restore output mode %u id 0x%x: %s",
				 srv.out_mode, srv.out_id, strerror(-rc));
	}
}

/* After a jack or hot-plug event: leave an HDMI/DP device whose monitor
 * went away, and return to the chosen one when it comes back. */
static void engine_follow_outputs(void)
{
	struct audio_dev *d = &srv.dev;
	if (d->null || d->fd < 0 || !d->have_outs)
		return;
	const struct aud_output *cur = NULL, *want = NULL;
	for (uint32_t i = 0; i < d->outs.count; i++) {
		if (d->pcm_output_id && d->outs.out[i].id == d->pcm_output_id)
			cur = &d->outs.out[i];
		if (srv.out_mode == OUTSEL_ID && d->outs.out[i].id == srv.out_id)
			want = &d->outs.out[i];
	}
	if (d->pcm_output_id && cur && !cur->present) {
		log_info("output 0x%x unplugged: back to the analog device",
			 d->pcm_output_id);
		int rc = dev_select_pcm(d, &srv.cfg, 0);
		if (rc > 0)
			engine_pcm_changed();
		return;
	}
	if (want && want->present && srv.out_id != 0 &&
	    d->pcm_output_id != srv.out_id && dev_has_pcm_for(d, srv.out_id)) {
		log_info("output 0x%x is back", srv.out_id);
		int rc = dev_select_pcm(d, &srv.cfg, srv.out_id);
		if (rc > 0)
			engine_pcm_changed();
		return;
	}
	/* Automatic mode: follow a headphone jack that has its own device. */
	if (srv.out_mode == OUTSEL_AUTO) {
		uint32_t target = auto_jack_target(d);
		if (target != d->pcm_output_id) {
			log_info("headphones %s: output 0x%x",
				 target ? "plugged in" : "removed", target);
			int rc = dev_select_pcm(d, &srv.cfg, target);
			if (rc > 0)
				engine_pcm_changed();
		}
	}
}

/* The device in srv.dev changed (opened, replaced, gone to null). */
static void engine_device_changed(void)
{
	if (engine_setup_mixer() < 0) {
		/* cannot mix for this device: fall back to a null sink with
		 * the stock format, which always works */
		struct audiod_config c = srv.cfg;
		c.format = SF_S16LE;
		c.rate = 48000;
		c.channels = 2;
		dev_close(&srv.dev);
		dev_open_null(&srv.dev, &c);
		engine_setup_mixer();
	}
	streams_retarget();
	engine_master_changed();
	engine_apply_output();
	srv.suspended = 0;
	srv.dev.delay = 0;
	g_prepared_written = srv.dev.written;
	g_dev_backoff_until = 0;
	g_fill_now = 1;
	srv.last_audible_ms = now_ms();
	if (!srv.dev.null) {
		srv.card_pref = srv.dev.card;
		state_mark_dirty();
	}
	notify_all(AUD_SUB_CARDS, AUD_NOTIFY_CARD,
		   srv.dev.card >= 0 ? (uint32_t)srv.dev.card : 0xffffffffu,
		   srv.dev.null ? 0 : 1, 0);
}

static void engine_go_null(const char *why)
{
	if (srv.dev.null)
		return;
	log_warn("%s: %s; continuing without sound output", srv.dev.path, why);
	dev_close(&srv.dev);
	dev_open_null(&srv.dev, &srv.cfg);
	engine_device_changed();
	g_reopen_interval = REOPEN_INTERVAL_MS;
	srv.reopen_due_ms = now_ms() + g_reopen_interval;
}

/* Try to get real hardware back while on the null sink. */
static void engine_try_reopen(void)
{
	struct audio_dev nd;
	if (dev_open(&nd, &srv.cfg, srv.card_pref) < 0) {
		/* back off to at most 30 s between attempts */
		if (g_reopen_interval < 30000)
			g_reopen_interval *= 2;
		srv.reopen_due_ms = now_ms() + g_reopen_interval;
		return;
	}
	dev_prepare(&nd);
	dev_close(&srv.dev);
	srv.dev = nd;
	log_warn("sound output on %s (%s)", srv.dev.path, srv.dev.card_name);
	engine_device_changed();
	srv.reopen_due_ms = 0;
	g_reopen_interval = REOPEN_INTERVAL_MS;
}

/* Close and reopen with the current configuration (SIGHUP). */
void engine_reopen(int card_hint)
{
	struct audio_dev nd;
	/* one writer per PCM: the old handle must go before the new open */
	dev_close(&srv.dev);
	if (dev_open(&nd, &srv.cfg, card_hint) < 0) {
		dev_open_null(&srv.dev, &srv.cfg);
		log_warn("no usable sound device; running without output");
		srv.reopen_due_ms = now_ms() + g_reopen_interval;
	} else {
		dev_prepare(&nd);
		srv.dev = nd;
		srv.reopen_due_ms = 0;
	}
	engine_device_changed();
}

int engine_switch_card(int card)
{
	struct audio_dev nd;
	struct audiod_config c = srv.cfg;

	if (!srv.dev.null && srv.dev.card == card)
		return 0;
	snprintf(c.device, sizeof(c.device), "%d", card);
	/* open the new card first so a failure keeps the old one */
	int rc = dev_open(&nd, &c, card);
	if (rc < 0)
		return rc;
	dev_prepare(&nd);
	dev_close(&srv.dev);
	srv.dev = nd;
	log_warn("switched to card %d (%s)", card, srv.dev.card_name);
	engine_device_changed();
	return 0;
}

static void engine_event(uint32_t type, uint32_t id, uint32_t value)
{
	switch (type) {
	case AUDIO_EV_JACK:
	case AUDIO_EV_OUTPUT:
	case AUDIO_EV_HOTPLUG: {
		dev_refresh_outputs(&srv.dev);
		uint32_t present = 0;
		for (uint32_t i = 0; i < srv.dev.outs.count && i < 32; i++)
			if (srv.dev.outs.out[i].present)
				present |= 1u << i;
		log_info("output event %u (id %u value %u)", type, id, value);
		engine_follow_outputs();
		notify_all(AUD_SUB_OUTPUTS, AUD_NOTIFY_OUTPUT,
			   srv.dev.outs.selected, present, srv.dev.outs.mode);
		break;
	}
	case AUDIO_EV_VOLUME: {
		uint32_t l, r;
		if (srv.dev.hw_vol_ctl >= 0 && (int)id == srv.dev.hw_vol_ctl &&
		    dev_get_volume(&srv.dev, &l, &r) == 0 &&
		    (l != srv.vol_l || r != srv.vol_r)) {
			srv.vol_l = l;
			srv.vol_r = r;
			state_mark_dirty();
			notify_all(AUD_SUB_VOLUME, AUD_NOTIFY_VOLUME, l, r,
				   srv.mute);
		}
		break;
	}
	case AUDIO_EV_XRUN:
		log_debug("device reports an underrun");
		break;
	case AUDIO_EV_DISCONNECT:
		g_dev_lost = 1;
		break;
	default:
		break;
	}
}

static void handle_dev_error(int rc)
{
	if (rc == -ENODEV)
		engine_go_null("the device is gone");
	else
		engine_go_null(strerror(-rc));
}

/* Keep the device fed: render whatever it has room for. */
static void engine_fill(void)
{
	struct audio_dev *d = &srv.dev;
	int wrote = 0;

	if (srv.suspended || !srv.mix_ready)
		return;

	for (int iter = 0; iter < 32; iter++) {
		int rc = dev_flush(d);
		if (rc == 1)
			break; /* full; POLLOUT will tell */
		if (rc < 0) {
			if (rc == -EPIPE) {
				srv.underruns++;
				log_debug("underrun; restarting the device");
				if ((rc = dev_prepare(d)) < 0) {
					handle_dev_error(rc);
					return;
				}
				g_prepared_written = d->written;
				continue;
			}
			handle_dev_error(rc);
			return;
		}
		rc = dev_update(d);
		if (rc == -EPIPE) {
			srv.underruns++;
			log_debug("underrun; restarting the device");
			if ((rc = dev_prepare(d)) < 0) {
				handle_dev_error(rc);
				return;
			}
			g_prepared_written = d->written;
			continue;
		}
		if (rc < 0) {
			handle_dev_error(rc);
			return;
		}
		uint32_t min = d->null ? d->period_frames : d->period_frames / 4;
		if (min == 0)
			min = 1;
		if (d->avail < min)
			break;
		size_t frames = d->avail;
		if (frames > d->period_frames * 2u)
			frames = d->period_frames * 2u;
		engine_render(frames);
		wrote = 1;
	}

	/* start once the ring has been filled */
	if (!d->running && d->written > g_prepared_written) {
		int rc = dev_start(d);
		if (rc == -ENODEV) {
			handle_dev_error(rc);
			return;
		}
		if (rc == -EPIPE) {
			dev_prepare(d);
			g_prepared_written = d->written;
		}
	}

	/* POLLOUT without room for a quarter period would spin: wait, on the
	 * clock, until a whole period is free (what the device's own wakeup
	 * threshold does), so a client waking us early does not split the
	 * device's periods into small writes */
	if (!wrote && !d->null && !d->pend_len && d->rate) {
		uint32_t want = d->period_frames;
		uint32_t missing = want > d->avail ? want - d->avail : 1;
		g_dev_backoff_until =
			now_ms() + (uint64_t)missing * 1000u / d->rate + 1;
	} else {
		g_dev_backoff_until = 0;
	}
	streams_check_drains();
}

/* ---- status dump (SIGUSR1) -------------------------------------------------- */

static void log_status(void)
{
	log_warn("status: %s %s %u Hz %d ch, period %u x %u, %s, volume %u/%u%s, "
		 "%u clients, %u streams, %llu underruns",
		 srv.dev.path, sf_name(srv.dev.fmt), srv.dev.rate,
		 srv.dev.channels, srv.dev.period_frames, srv.dev.periods,
		 srv.suspended ? "suspended" : "running", srv.vol_l, srv.vol_r,
		 srv.mute ? " muted" : "", srv.nclients, srv.nstreams,
		 (unsigned long long)(srv.underruns + srv.dev.xruns));
	for (struct stream *s = srv.streams; s; s = s->next)
		log_warn("  stream %u uid %u pid %d \"%s\" %u Hz %d ch, "
			 "queued %zu frames, %u underruns%s%s%s",
			 s->id, s->uid, (int)s->pid, s->name, s->dsp.rate,
			 s->dsp.channels, dsp_stream_queued_frames(&s->dsp),
			 s->underruns, s->paused ? " paused" : "",
			 s->prebuffering ? " prebuffering" : "",
			 s->draining ? " draining" : "");
}

/* ---- configuration reload (SIGHUP) -------------------------------------------- */

static int device_config_differs(const struct audiod_config *a,
				 const struct audiod_config *b)
{
	return strcmp(a->device, b->device) != 0 || a->rate != b->rate ||
	       a->format != b->format || a->channels != b->channels ||
	       a->period_ms != b->period_ms || a->periods != b->periods ||
	       a->hw_volume != b->hw_volume;
}

static void reload_config(int verbose_floor)
{
	struct audiod_config nc;
	int rc = config_load(srv.cfg_path, &nc);
	if (rc < 0) {
		log_err("configuration reload failed; keeping the old one");
		return;
	}
	if (srv.dev_override[0])
		snprintf(nc.device, sizeof(nc.device), "%s", srv.dev_override);
	int lvl = nc.log_level > verbose_floor ? nc.log_level : verbose_floor;
	log_set_level(lvl);
	int reopen = device_config_differs(&srv.cfg, &nc);
	srv.cfg = nc;
	events_cache_clear();
	rs_cache_trim();
	log_info("configuration reloaded%s", reopen ? "; reopening the device"
						    : "");
	if (reopen)
		engine_reopen(srv.card_pref);
}

/* ---- privileges ------------------------------------------------------------------ */

static int ensure_dir(const char *path, mode_t mode, uid_t uid, gid_t gid,
		      int chown_it)
{
	struct stat st;
	if (mkdir(path, mode) < 0 && errno != EEXIST) {
		log_err("mkdir %s: %s", path, strerror(errno));
		return -1;
	}
	if (lstat(path, &st) < 0) {
		log_err("%s: %s", path, strerror(errno));
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		/* a symlink or file here would redirect our writes */
		log_err("%s is not a directory", path);
		return -1;
	}
	if (chown_it) {
		if (chown(path, (int)uid, (int)gid) < 0)
			log_warn("chown %s: %s", path, strerror(errno));
		if (chmod(path, mode) < 0)
			log_warn("chmod %s: %s", path, strerror(errno));
	}
	return 0;
}

static void resolve_ids(uid_t *uid, gid_t *gid)
{
	struct passwd *pw = getpwnam(AUDIOD_USER);
	struct group *gr = getgrnam(AUDIOD_GROUP);
	*uid = pw ? pw->pw_uid : AUDIOD_FALLBACK_UID;
	*gid = gr ? gr->gr_gid : AUDIOD_FALLBACK_GID;
	if (!pw)
		log_warn("no user '%s'; using uid %d", AUDIOD_USER,
			 AUDIOD_FALLBACK_UID);
	if (!gr)
		log_warn("no group '%s'; using gid %d", AUDIOD_GROUP,
			 AUDIOD_FALLBACK_GID);
}

static int drop_privileges(uid_t uid, gid_t gid)
{
	int groups[1] = { (int)gid };

	if (uid == 0 || gid == 0) {
		log_err("refusing to run as root: user %s resolves to uid %u "
			"gid %u",
			AUDIOD_USER, (unsigned)uid, (unsigned)gid);
		return -1;
	}
	/* (void *): the group list is int[] here, gid_t[] elsewhere */
	if (setgroups(1, (void *)groups) < 0) {
		log_err("setgroups: %s", strerror(errno));
		return -1;
	}
	if (setresgid((int)gid, (int)gid, (int)gid) < 0) {
		log_err("setresgid(%u): %s", (unsigned)gid, strerror(errno));
		return -1;
	}
	if (setresuid((int)uid, (int)uid, (int)uid) < 0) {
		log_err("setresuid(%u): %s", (unsigned)uid, strerror(errno));
		return -1;
	}

	/* Verify: every id is the unprivileged one and there is no way
	 * back.  A kernel that let any of these through must not be trusted
	 * with a daemon that talks to every user. */
	uid_t ru, eu, su;
	gid_t rg, eg, sg;
	if (getresuid(&ru, &eu, &su) < 0 || ru != uid || eu != uid ||
	    su != uid) {
		log_err("uid did not stick");
		return -1;
	}
	if (getresgid(&rg, &eg, &sg) < 0 || rg != gid || eg != gid ||
	    sg != gid) {
		log_err("gid did not stick");
		return -1;
	}
	int list[16];
	int n = getgroups(16, (void *)list);
	if (n < 0 || n > 1 || (n == 1 && list[0] != (int)gid)) {
		log_err("supplementary groups were not dropped");
		return -1;
	}
	if (setuid(0) == 0 || seteuid(0) == 0 || setgid(0) == 0) {
		log_err("root could be regained after dropping it; exiting");
		return -1;
	}
	if (getuid() == 0 || geteuid() == 0) {
		log_err("still root after dropping privileges");
		return -1;
	}
	return 0;
}

static void daemonize(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		log_err("fork: %s", strerror(errno));
		exit(1);
	}
	if (pid > 0)
		_exit(0);
	setsid();
	int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			close(fd);
	}
	log_set_console(1);
}

/* ---- usage ---------------------------------------------------------------------- */

static void usage(FILE *f)
{
	fprintf(f,
		"usage: audiod [-f] [-v]... [-c config] [-d device] [-t]\n"
		"  -f         stay in the foreground (as started by init)\n"
		"  -v         more log output (repeat for debug)\n"
		"  -c file    configuration file (default %s)\n"
		"  -d device  sound device: /dev/audioN, a card number or auto\n"
		"  -t         check the configuration and exit\n"
		"  -V         print the version and exit\n",
		AUDIOD_CONFIG_PATH);
}

/* ---- main loop ------------------------------------------------------------------ */

static void poll_loop(int verbose_floor)
{
	struct pollfd *pfd = calloc(3 + HARD_MAX_CLIENTS + 8, sizeof(*pfd));
	struct client **pcl = calloc(3 + HARD_MAX_CLIENTS + 8, sizeof(*pcl));
	uint64_t shutdown_deadline = 0;
	uint64_t next_housekeeping = now_ms() + 1000;

	if (!pfd || !pcl) {
		log_err("out of memory");
		free(pfd);
		free(pcl);
		return;
	}

	for (;;) {
		uint64_t now = now_ms();

		/* ---- signals ---- */
		if (g_term && !srv.terminating) {
			srv.terminating = 1;
			log_info("terminating: fading out");
			if (srv.listen_fd >= 0) {
				close(srv.listen_fd);
				srv.listen_fd = -1;
			}
			if (srv.mix_ready)
				mixer_fade_out(&srv.mix, 60);
			shutdown_deadline = now + 400;
			engine_wake();
		}
		if (g_hup) {
			g_hup = 0;
			reload_config(verbose_floor);
		}
		if (g_usr1) {
			g_usr1 = 0;
			log_status();
		}
		if (g_dev_lost) {
			g_dev_lost = 0;
			engine_go_null("disconnected");
		}

		if (srv.terminating) {
			int faded = !srv.mix_ready || mixer_fade_done(&srv.mix);
			if ((faded && !srv.dev.pend_len) || now >= shutdown_deadline ||
			    srv.dev.null)
				break;
		}

		/* ---- the device ---- */
		if (g_fill_now) {
			g_fill_now = 0;
			engine_fill();
		}

		/* ---- housekeeping ---- */
		if (now >= next_housekeeping) {
			next_housekeeping = now + 1000;
			clients_timeouts(now);
		}
		streams_check_drains();
		clients_reap();
		if (srv.state_dirty && now >= srv.state_due_ms)
			state_flush();
		if (srv.dev.null && srv.reopen_due_ms && now >= srv.reopen_due_ms &&
		    !srv.terminating)
			engine_try_reopen();
		if (!srv.suspended && !srv.terminating && srv.cfg.suspend_ms &&
		    !streams_any_active() &&
		    now - srv.last_audible_ms >= srv.cfg.suspend_ms)
			engine_suspend();

		/* ---- build the poll set ---- */
		int n = 0;
		int i_listen = -1, i_dev = -1, i_ev = -1;
		if (srv.listen_fd >= 0) {
			pfd[n].fd = srv.listen_fd;
			pfd[n].events = POLLIN;
			pfd[n].revents = 0;
			i_listen = n++;
		}
		if (!srv.suspended && !srv.dev.null && srv.dev.fd >= 0 &&
		    now >= g_dev_backoff_until) {
			pfd[n].fd = srv.dev.fd;
			pfd[n].events = POLLOUT;
			pfd[n].revents = 0;
			i_dev = n++;
		}
		short evm = 0;
		int evfd = dev_event_fd(&srv.dev, &evm);
		if (evfd >= 0) {
			if (i_dev >= 0 && evfd == srv.dev.fd) {
				pfd[i_dev].events |= evm;
				i_ev = i_dev;
			} else {
				pfd[n].fd = evfd;
				pfd[n].events = evm;
				pfd[n].revents = 0;
				i_ev = n++;
			}
		}
		int first_client = n;
		for (struct client *c = srv.clients; c; c = c->next) {
			if (c->dead || c->fd < 0)
				continue;
			if (n >= 3 + HARD_MAX_CLIENTS + 8)
				break;
			pfd[n].fd = c->fd;
			pfd[n].events = (short)((c->blocked ? 0 : POLLIN) |
						(c->out_len ? POLLOUT : 0));
			pfd[n].revents = 0;
			pcl[n] = c;
			n++;
		}

		/* ---- timeout: the nearest of everything that is due ---- */
		int64_t timeout = 1000;
		if (!srv.suspended) {
			if (srv.dev.null) {
				dev_update(&srv.dev);
				int t = dev_timeout_ms(&srv.dev);
				if (t < timeout)
					timeout = t;
			} else if (now < g_dev_backoff_until) {
				int64_t t = (int64_t)(g_dev_backoff_until - now);
				if (t < timeout)
					timeout = t;
			} else {
				int t = dev_timeout_ms(&srv.dev);
				if (t < timeout)
					timeout = t;
			}
		}
		if (srv.state_dirty) {
			int64_t t = srv.state_due_ms > now
					    ? (int64_t)(srv.state_due_ms - now)
					    : 0;
			if (t < timeout)
				timeout = t;
		}
		if (srv.dev.null && srv.reopen_due_ms) {
			int64_t t = srv.reopen_due_ms > now
					    ? (int64_t)(srv.reopen_due_ms - now)
					    : 0;
			if (t < timeout)
				timeout = t;
		}
		uint64_t dl = streams_next_deadline();
		if (dl) {
			int64_t t = dl > now ? (int64_t)(dl - now) : 0;
			if (t < timeout)
				timeout = t;
		}
		if (!srv.suspended && srv.cfg.suspend_ms) {
			uint64_t due = srv.last_audible_ms + srv.cfg.suspend_ms;
			int64_t t = due > now ? (int64_t)(due - now) : 0;
			if (t < timeout)
				timeout = t + 1;
		}
		if (srv.terminating)
			timeout = timeout > 10 ? 10 : timeout;
		if (g_fill_now)
			timeout = 0;
		if (timeout < 0)
			timeout = 0;

		int r = poll(pfd, (nfds_t)n, (int)timeout);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			log_err("poll: %s", strerror(errno));
			struct timespec ts = { 0, 10000000 };
			nanosleep(&ts, NULL);
			continue;
		}

		/* device events first: a disconnect must be seen before a
		 * write fails on it */
		if (i_ev >= 0 && (pfd[i_ev].revents & (POLLPRI | POLLIN)))
			dev_read_events(&srv.dev, engine_event);
		if (i_ev >= 0 && i_ev != i_dev &&
		    (pfd[i_ev].revents & (POLLERR | POLLHUP | POLLNVAL)))
			g_dev_lost = 1;
		if (g_dev_lost) {
			g_dev_lost = 0;
			engine_go_null("disconnected");
		}

		/* the device: on POLLOUT, on its timeout, or for the null
		 * sink's clock */
		/* (POLLERR on the device needs nothing special: the position
		 * query in engine_fill reports the underrun or the loss) */
		engine_fill();

		if (i_listen >= 0 && (pfd[i_listen].revents & POLLIN))
			clients_accept();

		for (int k = first_client; k < n; k++) {
			if (!pfd[k].revents)
				continue;
			struct client *c = pcl[k];
			client_io(c, pfd[k].revents);
		}

		/* data that just arrived may let a stream start: fill now
		 * rather than at the next device wakeup */
		if (g_fill_now) {
			g_fill_now = 0;
			engine_fill();
		}
	}

	free(pfd);
	free(pcl);
}

static void shutdown_all(void)
{
	/* let what was written (the fade) play out, briefly */
	if (!srv.dev.null && srv.dev.fd >= 0 && srv.dev.running) {
		uint64_t until = now_ms() + 200;
		while (now_ms() < until) {
			if (dev_update(&srv.dev) < 0 || srv.dev.delay == 0)
				break;
			struct timespec ts = { 0, 5000000 };
			nanosleep(&ts, NULL);
		}
	}
	dev_close(&srv.dev);

	for (struct client *c = srv.clients; c; c = c->next)
		client_close(c, NULL);
	for (struct stream *s = srv.streams; s; s = s->next)
		s->done = 1;
	clients_reap();
	events_cache_clear();
	if (srv.mix_ready)
		mixer_free(&srv.mix);
	srv.mix_ready = 0;
	if (srv.listen_fd >= 0)
		close(srv.listen_fd);
	srv.listen_fd = -1;
	unlink(AUDIOD_SOCKET_PATH);
	state_flush();
	rs_cache_trim();
	log_info("stopped");
}

int main(int argc, char **argv)
{
	int foreground = 0, verbose = 0, test_only = 0;
	const char *cfg_path = AUDIOD_CONFIG_PATH;
	const char *dev_override = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "fvc:d:tVh")) != -1) {
		switch (opt) {
		case 'f':
			foreground = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'c':
			cfg_path = optarg;
			break;
		case 'd':
			dev_override = optarg;
			break;
		case 't':
			test_only = 1;
			break;
		case 'V':
			printf("audiod %s (protocol %d)\n", AUDIOD_VERSION,
			       AUDIOD_PROTOCOL_VERSION);
			return 0;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind != argc) {
		usage(stderr);
		return 2;
	}

	memset(&srv, 0, sizeof(srv));
	srv.listen_fd = -1;
	srv.card_pref = -1;
	srv.dev.fd = -1;
	srv.dev.evfd = -1;
	srv.start_ms = now_ms();

	int verbose_floor = AL_WARN + verbose;
	if (verbose_floor > AL_DEBUG)
		verbose_floor = AL_DEBUG;
	log_set_level(verbose_floor);

	if (strlen(cfg_path) >= sizeof(srv.cfg_path)) {
		log_err("configuration path too long");
		return 2;
	}
	snprintf(srv.cfg_path, sizeof(srv.cfg_path), "%s", cfg_path);

	int rc = config_load(srv.cfg_path, &srv.cfg);
	if (rc < 0) {
		if (test_only)
			return 1;
		log_err("using the built-in defaults");
		config_defaults(&srv.cfg);
	} else if (rc == 1) {
		log_info("%s not found; using the built-in defaults",
			 srv.cfg_path);
	}
	if (dev_override) {
		if (strlen(dev_override) >= sizeof(srv.dev_override) ||
		    !config_device_ok(dev_override)) {
			log_err("-d: device must be auto, a card number 0-7, "
				"/dev/audioN or /dev/dsp[N]");
			return 2;
		}
		snprintf(srv.dev_override, sizeof(srv.dev_override), "%s",
			 dev_override);
		snprintf(srv.cfg.device, sizeof(srv.cfg.device), "%s",
			 dev_override);
	}
	log_set_level(srv.cfg.log_level > verbose_floor ? srv.cfg.log_level
							: verbose_floor);

	if (test_only) {
		printf("configuration OK: device=%s rate=%u format=%s "
		       "channels=%u period_ms=%u periods=%u\n",
		       srv.cfg.device, srv.cfg.rate,
		       srv.cfg.format ? sf_name(srv.cfg.format) : "auto",
		       srv.cfg.channels, srv.cfg.period_ms, srv.cfg.periods);
		return 0;
	}

	setup_signals();
	if (!foreground)
		daemonize();

	srv.privileged = geteuid() == 0;
	uid_t uid = (uid_t)getuid();
	gid_t gid = (gid_t)getgid();
	if (srv.privileged) {
		resolve_ids(&uid, &gid);
		umask(022);
		/* /var/run may not exist on a fresh image */
		if (ensure_dir("/var/run", 0755, 0, 0, 0) < 0 ||
		    ensure_dir(AUDIOD_SOCKET_DIR, 0755, uid, gid, 1) < 0)
			return 1;
		if (ensure_dir("/var/lib", 0755, 0, 0, 0) < 0 ||
		    ensure_dir(AUDIOD_STATE_DIR, 0700, uid, gid, 1) < 0)
			log_warn("the volume will not be remembered");
	} else {
		log_warn("not started as root: running as uid %u; the "
			 "socket directory must already be writable",
			 (unsigned)uid);
	}

	/* persisted state, else the configured defaults */
	struct audiod_state st;
	st.vol_l = st.vol_r = srv.cfg.default_volume;
	st.mute = 0;
	st.out_mode = OUTSEL_AUTO;
	st.out_id = 0;
	st.pcm_output = 0;
	st.card = -1;
	state_load(AUDIOD_STATE_PATH, &st);
	srv.vol_l = st.vol_l;
	srv.vol_r = st.vol_r;
	srv.mute = st.mute;
	srv.out_mode = st.out_mode;
	srv.out_id = st.out_id;
	srv.card_pref = st.card;

	/* the device, while we still may */
	if (dev_open(&srv.dev, &srv.cfg, srv.card_pref) < 0) {
		log_warn("no usable sound device; running without output");
		dev_open_null(&srv.dev, &srv.cfg);
		srv.reopen_due_ms = now_ms() + REOPEN_INTERVAL_MS;
	} else {
		dev_prepare(&srv.dev);
	}

	srv.listen_fd = clients_listen(AUDIOD_SOCKET_PATH);
	if (srv.listen_fd < 0) {
		dev_close(&srv.dev);
		return 1;
	}
	if (srv.privileged && chown(AUDIOD_SOCKET_PATH, (int)uid, (int)gid) < 0)
		log_warn("chown %s: %s", AUDIOD_SOCKET_PATH, strerror(errno));

	if (srv.privileged) {
		if (drop_privileges(uid, gid) < 0) {
			unlink(AUDIOD_SOCKET_PATH);
			return 1;
		}
		srv.privileged = 0;
		log_info("running as uid %u gid %u", (unsigned)uid,
			 (unsigned)gid);
	}
	if (chdir("/") < 0)
		log_warn("chdir /: %s", strerror(errno));
	umask(077);

	if (engine_setup_mixer() < 0) {
		dev_close(&srv.dev);
		struct audiod_config c = srv.cfg;
		c.format = SF_S16LE;
		c.rate = 48000;
		c.channels = 2;
		dev_open_null(&srv.dev, &c);
		if (engine_setup_mixer() < 0)
			return 1;
	}
	engine_master_changed();
	srv.state_dirty = 0; /* nothing changed yet */
	engine_apply_output();
	if (!srv.dev.null)
		srv.card_pref = srv.dev.card;
	g_prepared_written = srv.dev.written;
	srv.last_audible_ms = now_ms();
	/* start idle: the device runs only once someone plays */
	if (srv.cfg.suspend_ms) {
		dev_stop(&srv.dev);
		srv.suspended = 1;
	}

	log_info("audiod %s ready on %s (%s, %s %u Hz %d ch)", AUDIOD_VERSION,
		 AUDIOD_SOCKET_PATH, srv.dev.path, sf_name(srv.dev.fmt),
		 srv.dev.rate, srv.dev.channels);

	poll_loop(verbose_floor);
	shutdown_all();
	return 0;
}
