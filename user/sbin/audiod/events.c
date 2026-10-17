// audiod event sounds: PLAY_EVENT "name" plays
// /usr/share/sounds/likeos/<name>.wav as an internal stream with the EVENT
// role.
//
// Clients never supply a path.  The name is checked against a strict
// alphabet ([a-z0-9_-], no leading '-'), so neither '/' nor ".." can ever
// reach the file system, and the file is opened with O_NOFOLLOW, must be a
// regular file and is size-limited before a byte of it is parsed.  Decoded
// sounds are cached (bounded by count and bytes) and shared between the
// streams that play them.  Each uid has a token bucket so one user cannot
// flood the speakers or the daemon.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audiod.h"

#define EV_CACHE_MAX 16
#define EV_CACHE_BYTES (32u * 1024u * 1024u)

struct ev_sound {
	struct ev_sound *next;
	char name[AUD_EVENT_NAME_MAX];
	uint8_t *file;
	size_t file_len;
	struct wav_info wi;
	int refs; /* streams playing it */
	int cached; /* on the cache list */
	uint64_t last_use;
};

static struct ev_sound *cache;
static uint32_t cache_count;
static size_t cache_bytes;

static void ev_free(struct ev_sound *e)
{
	free(e->file);
	free(e);
}

void events_put(struct ev_sound *e)
{
	if (!e)
		return;
	if (e->refs > 0)
		e->refs--;
	if (e->refs == 0 && !e->cached)
		ev_free(e);
}

static void cache_unlink(struct ev_sound *e)
{
	struct ev_sound **pp = &cache;
	while (*pp) {
		if (*pp == e) {
			*pp = e->next;
			e->next = NULL;
			e->cached = 0;
			cache_count--;
			cache_bytes -= e->file_len;
			return;
		}
		pp = &(*pp)->next;
	}
}

void events_cache_clear(void)
{
	while (cache) {
		struct ev_sound *e = cache;
		cache_unlink(e);
		if (e->refs == 0)
			ev_free(e);
		/* otherwise freed by the last stream playing it */
	}
}

/* Make room for `need' bytes: evict least recently used, idle entries. */
static void cache_trim(size_t need)
{
	while (cache && (cache_count >= EV_CACHE_MAX ||
			 cache_bytes + need > EV_CACHE_BYTES)) {
		struct ev_sound *victim = NULL;
		for (struct ev_sound *e = cache; e; e = e->next)
			if (!victim || e->last_use < victim->last_use)
				victim = e;
		if (!victim)
			break;
		cache_unlink(victim);
		if (victim->refs == 0)
			ev_free(victim);
	}
}

static int load_file(const char *name, struct ev_sound **out)
{
	char path[128];
	struct stat st;

	/* the name was validated; the path is built from fixed parts */
	if (!event_name_valid(name))
		return -EINVAL;
	int n = snprintf(path, sizeof(path), "%s/%s.wav", AUDIOD_SOUNDS_DIR,
			 name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -ENAMETOOLONG;

	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
				    O_NOCTTY);
	if (fd < 0) {
		int e = errno;
		if (e != ENOENT)
			log_warn("event sound %s: %s", path, strerror(e));
		return e == ENOENT || e == ELOOP ? -ENOENT : -e;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		log_warn("event sound %s is not a regular file", path);
		return -ENOENT;
	}
	if (st.st_size < 12 || (uint64_t)st.st_size > WAV_MAX_BYTES) {
		close(fd);
		log_warn("event sound %s: size %lld out of range", path,
			 (long long)st.st_size);
		return -EINVAL;
	}

	size_t size = (size_t)st.st_size;
	uint8_t *buf = malloc(size);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	size_t got = 0;
	while (got < size) {
		ssize_t r = read(fd, buf + got, size - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			free(buf);
			close(fd);
			return -e;
		}
		if (r == 0)
			break; /* shrank under us: parse what we have */
		got += (size_t)r;
	}
	close(fd);

	struct ev_sound *e = calloc(1, sizeof(*e));
	if (!e) {
		free(buf);
		return -ENOMEM;
	}
	e->file = buf;
	e->file_len = got;
	snprintf(e->name, sizeof(e->name), "%s", name);
	int rc = wav_parse(buf, got, &e->wi);
	if (rc < 0) {
		log_warn("event sound %s: not a usable WAV file (%s)", path,
			 strerror(-rc));
		ev_free(e);
		return -EINVAL;
	}
	log_debug("event sound %s: %s %u Hz %u ch, %zu frames", name,
		  sf_name(e->wi.fmt), e->wi.rate, e->wi.channels, e->wi.frames);
	*out = e;
	return 0;
}

static int lookup(const char *name, struct ev_sound **out)
{
	for (struct ev_sound *e = cache; e; e = e->next) {
		if (strcmp(e->name, name) == 0) {
			e->last_use = now_ms();
			*out = e;
			return 0;
		}
	}
	struct ev_sound *e = NULL;
	int rc = load_file(name, &e);
	if (rc < 0)
		return rc;
	cache_trim(e->file_len);
	if (cache_count < EV_CACHE_MAX &&
	    cache_bytes + e->file_len <= EV_CACHE_BYTES) {
		e->next = cache;
		cache = e;
		e->cached = 1;
		cache_count++;
		cache_bytes += e->file_len;
	}
	e->last_use = now_ms();
	*out = e;
	return 0;
}

/* Token bucket: `event_rate' sounds per 10 seconds, bursts up to that. */
static int rate_ok(uint32_t uid)
{
	struct uid_usage *u = uid_get(uid, 1);
	if (!u)
		return 0;
	uint32_t cap = srv.cfg.event_rate ? srv.cfg.event_rate : 1;
	uint64_t now = now_ms();
	if (!u->ev_refill_ms) {
		u->ev_tokens = cap;
		u->ev_refill_ms = now;
	} else {
		uint64_t el = now - u->ev_refill_ms;
		uint64_t add = el * cap / 10000u;
		if (add) {
			uint64_t t = (uint64_t)u->ev_tokens + add;
			u->ev_tokens = t > cap ? cap : (uint32_t)t;
			/* advance by the time those tokens represent */
			u->ev_refill_ms += add * 10000u / cap;
		}
	}
	if (u->ev_tokens == 0)
		return 0;
	u->ev_tokens--;
	return 1;
}

int events_play(struct client *c, const char name[AUD_EVENT_NAME_MAX],
		uint32_t volume, int wait)
{
	if (!srv.cfg.event_sounds)
		return 1; /* switched off: acknowledged, nothing plays */
	if (!event_name_valid(name))
		return -EINVAL;
	if (srv.nevent_streams >= MAX_EVENT_STREAMS)
		return -EBUSY;
	if (!rate_ok(c->cred.uid)) {
		log_debug("uid %u: event sounds rate-limited", c->cred.uid);
		return -EBUSY;
	}

	struct ev_sound *e = NULL;
	int rc = lookup(name, &e);
	if (rc < 0)
		return rc;

	struct stream *s = calloc(1, sizeof(*s));
	if (!s) {
		if (!e->cached && e->refs == 0)
			ev_free(e);
		return -ENOMEM;
	}
	if (dsp_stream_init_static(&s->dsp, e->wi.fmt, e->wi.rate,
				   (int)e->wi.channels, e->wi.data,
				   e->wi.data_len, srv.dev.rate,
				   srv.dev.channels,
				   srv.cfg.resample_quality) < 0) {
		free(s);
		if (!e->cached && e->refs == 0)
			ev_free(e);
		return -ENOMEM;
	}
	e->refs++;
	s->evsnd = e;
	s->id = ++srv.next_stream_id;
	if (s->id == 0)
		s->id = ++srv.next_stream_id;
	s->client_id = c->id;
	s->owner = NULL;
	s->uid = c->cred.uid;
	s->gid = c->cred.gid;
	s->pid = c->cred.pid;
	s->role = AUD_ROLE_EVENT;
	s->aud_fmt = sf_to_proto(e->wi.fmt);
	snprintf(s->name, sizeof(s->name), "%s", name);
	memcpy(s->app, c->name, sizeof(s->app));
	stream_set_volume(s, volume, volume, 0);
	for (int ch = 0; ch < DSP_MAX_CH; ch++)
		ramp_init(&s->dsp.gain[ch], s->dsp.gain[ch].target);
	s->prebuffering = 0;
	uint64_t dur_ms = (uint64_t)e->wi.frames * 1000u / e->wi.rate;
	s->drain_deadline_ms = now_ms() + dur_ms + 3000;

	s->next = srv.streams;
	srv.streams = s;
	srv.nstreams++;
	srv.nevent_streams++;

	log_info("event sound \"%s\" for uid %u (%llu ms)", name, c->cred.uid,
		 (unsigned long long)dur_ms);
	notify_all(AUD_SUB_STREAMS, AUD_NOTIFY_STREAM_NEW, s->id, 0, 0);
	engine_wake();

	if (wait) {
		s->waiter_client = c->id;
		c->blocked = 1;
		return 0;
	}
	return 1;
}

void events_stream_done(struct stream *s)
{
	struct ev_sound *e = s->evsnd;
	s->evsnd = NULL;
	if (s->waiter_client) {
		struct client *c = client_by_id(s->waiter_client);
		s->waiter_client = 0;
		if (c && c->blocked) {
			client_send_ok(c);
			client_unblock(c);
		}
	}
	events_put(e);
}
