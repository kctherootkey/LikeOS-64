// audiod control requests: volume, mute, outputs, cards, the stream list,
// per-stream volume and kill, event sounds and subscriptions.
//
// Authorization uses the peer credentials taken from the socket when the
// client connected (SO_PEERCRED), which the client cannot forge:
//   - master volume, mute and output selection: any local user (it is the
//     user sitting at the machine who turns the volume down);
//   - another stream's volume, or killing it: its owner (same uid) or root;
//   - switching the card: root only;
//   - the stream list: everyone sees every stream, but the names of other
//     users' streams (window titles, file names) are blanked unless the
//     caller is root.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audiod.h"

static int may_control(const struct client *c, const struct stream *s)
{
	return c->cred.uid == 0 || c->cred.uid == s->uid;
}

static uint32_t stream_state(const struct stream *s)
{
	if (s->paused)
		return STRM_PAUSED;
	if (s->draining)
		return STRM_DRAINING;
	if (s->underrun_sent || (s->prebuffering && s->underruns))
		return STRM_UNDERRUN;
	return STRM_PLAYING;
}

void control_fill_server_info(struct aud_server_info *si)
{
	memset(si, 0, sizeof(*si));
	si->version = AUDIOD_PROTOCOL_VERSION;
	si->rate = srv.dev.rate;
	si->channels = (uint32_t)srv.dev.channels;
	si->format = sf_to_proto(srv.dev.fmt);
	si->period_frames = srv.dev.period_frames;
	si->periods = srv.dev.periods;
	si->card = srv.dev.card >= 0 ? (uint32_t)srv.dev.card : 0xffffffffu;
	uint32_t n = 0;
	for (struct stream *s = srv.streams; s; s = s->next)
		if (!s->done)
			n++;
	si->streams = n;
	si->uptime_ms = now_ms() - srv.start_ms;
	si->underruns = srv.underruns + srv.dev.xruns;
	snprintf(si->card_name, sizeof(si->card_name), "%s", srv.dev.card_name);
	snprintf(si->codec, sizeof(si->codec), "%s", srv.dev.codec);
	snprintf(si->driver, sizeof(si->driver), "%s", srv.dev.driver);
}

static void list_streams(struct client *c)
{
	uint32_t n = 0;
	for (struct stream *s = srv.streams; s; s = s->next)
		if (!s->done)
			n++;
	if (n > AUD_MAX_STREAMS + MAX_EVENT_STREAMS)
		n = AUD_MAX_STREAMS + MAX_EVENT_STREAMS;

	size_t size = sizeof(struct aud_stream_list) +
		      (size_t)n * sizeof(struct aud_stream_entry);
	struct aud_stream_list *l = calloc(1, size);
	if (!l) {
		client_send_error(c, ENOMEM, "out of memory");
		return;
	}
	uint32_t i = 0;
	for (struct stream *s = srv.streams; s && i < n; s = s->next) {
		if (s->done)
			continue;
		struct aud_stream_entry *e = &l->entry[i++];
		e->stream_id = s->id;
		e->client_id = s->client_id;
		e->uid = s->uid;
		e->pid = (uint32_t)s->pid;
		e->role = s->role;
		e->format = s->aud_fmt;
		e->rate = s->dsp.rate;
		e->channels = (uint32_t)s->dsp.channels;
		e->left = s->vol_l;
		e->right = s->vol_r;
		e->mute = s->mute;
		e->state = stream_state(s);
		e->played_frames = s->dsp.in_frames;
		if (may_control(c, s)) {
			memcpy(e->name, s->name, sizeof(e->name));
			memcpy(e->app, s->app, sizeof(e->app));
		}
		/* other users' titles stay private: left empty */
	}
	l->count = i;
	size = sizeof(struct aud_stream_list) +
	       (size_t)i * sizeof(struct aud_stream_entry);
	client_send(c, AUD_MSG_LIST_STREAMS, l, (uint32_t)size);
	free(l);
}

static void get_outputs(struct client *c)
{
	struct aud_outputs o;
	dev_refresh_outputs(&srv.dev);
	if (srv.dev.have_outs) {
		o = srv.dev.outs;
		if (srv.dev.pcm_output_id) {
			/* playing on an HDMI/DP device: that is the one
			 * output in use, whatever the pin routing says */
			o.mode = OUTSEL_ID;
			o.selected = srv.dev.pcm_output_id;
			for (uint32_t i = 0; i < o.count; i++)
				o.out[i].active =
					o.out[i].id == srv.dev.pcm_output_id;
		}
	} else {
		memset(&o, 0, sizeof(o));
		o.mode = srv.out_mode;
		o.selected = srv.out_id;
	}
	client_send(c, AUD_MSG_GET_OUTPUTS, &o, sizeof(o));
}

static void set_output(struct client *c, const struct aud_set_output *so)
{
	if (proto_check_set_output(so) < 0) {
		client_send_error(c, EINVAL, "invalid output selection");
		return;
	}
	if (so->mode == OUTSEL_ID && srv.dev.have_outs) {
		int found = 0;
		for (uint32_t i = 0; i < srv.dev.outs.count; i++)
			if (srv.dev.outs.out[i].id == so->id)
				found = 1;
		if (!found) {
			client_send_error(c, ENOENT, "no such output");
			return;
		}
	}
	if (!srv.dev.null && srv.dev.fd >= 0) {
		int rc = engine_route_output(so->mode, so->id);
		if (rc < 0) {
			client_send_error(c, -rc,
					  rc == -EBUSY
						  ? "that output is in use"
						  : "the device refused the output");
			return;
		}
	}
	srv.out_mode = so->mode;
	srv.out_id = so->mode == OUTSEL_ID ? so->id : 0;
	state_mark_dirty();
	client_send_ok(c);
	uint32_t present = 0;
	for (uint32_t i = 0; i < srv.dev.outs.count && i < 32; i++)
		if (srv.dev.outs.out[i].present)
			present |= 1u << i;
	notify_all(AUD_SUB_OUTPUTS, AUD_NOTIFY_OUTPUT,
		   srv.dev.have_outs ? srv.dev.outs.selected : srv.out_id,
		   present, srv.out_mode);
	log_info("uid %u set the output to mode %u id %u", c->cred.uid,
		 so->mode, so->id);
}

int control_handle(struct client *c, uint32_t type, const uint8_t *p,
		   uint32_t len)
{
	(void)len; /* every length was checked against the type already */

	switch (type) {
	case AUD_MSG_GET_SERVER_INFO: {
		struct aud_server_info si;
		control_fill_server_info(&si);
		client_send(c, AUD_MSG_GET_SERVER_INFO, &si, sizeof(si));
		return 0;
	}
	case AUD_MSG_LIST_STREAMS:
		list_streams(c);
		return 0;
	case AUD_MSG_GET_MASTER_VOLUME: {
		struct aud_volume v;
		memset(&v, 0, sizeof(v));
		v.left = srv.vol_l;
		v.right = srv.vol_r;
		client_send(c, AUD_MSG_GET_MASTER_VOLUME, &v, sizeof(v));
		return 0;
	}
	case AUD_MSG_SET_MASTER_VOLUME: {
		struct aud_volume v;
		memcpy(&v, p, sizeof(v));
		if (proto_check_volume(&v) < 0) {
			client_send_error(c, EINVAL, "volume must be 0..100");
			return 0;
		}
		srv.vol_l = v.left;
		srv.vol_r = v.right;
		engine_master_changed();
		client_send_ok(c);
		return 0;
	}
	case AUD_MSG_GET_MUTE: {
		struct aud_mute m;
		memset(&m, 0, sizeof(m));
		m.mute = srv.mute;
		client_send(c, AUD_MSG_GET_MUTE, &m, sizeof(m));
		return 0;
	}
	case AUD_MSG_SET_MUTE: {
		struct aud_mute m;
		memcpy(&m, p, sizeof(m));
		if (proto_check_mute(&m) < 0) {
			client_send_error(c, EINVAL, "mute must be 0 or 1");
			return 0;
		}
		srv.mute = m.mute;
		engine_master_changed();
		client_send_ok(c);
		return 0;
	}
	case AUD_MSG_GET_OUTPUTS:
		get_outputs(c);
		return 0;
	case AUD_MSG_SET_OUTPUT: {
		struct aud_set_output so;
		memcpy(&so, p, sizeof(so));
		set_output(c, &so);
		return 0;
	}
	case AUD_MSG_PLAY_EVENT: {
		struct aud_play_event ev;
		char name[AUD_EVENT_NAME_MAX];
		memcpy(&ev, p, sizeof(ev));
		int rc = proto_check_play_event(&ev, name);
		if (rc == -EBADMSG) {
			client_close(c, "PLAY_EVENT with an unterminated name");
			return -1;
		}
		if (rc < 0) {
			client_send_error(c, EINVAL, "invalid event sound name");
			return 0;
		}
		rc = events_play(c, name, ev.volume, (int)ev.wait);
		if (rc < 0)
			client_send_error(c, -rc,
					  rc == -EBUSY	  ? "too many event sounds"
					  : rc == -ENOENT ? "no such event sound"
							  : "cannot play the "
							    "event sound");
		else if (rc > 0)
			client_send_ok(c); /* not waiting, or nothing to wait for */
		/* rc == 0: the OK follows when the sound has finished */
		return 0;
	}
	case AUD_MSG_SUBSCRIBE: {
		struct aud_subscribe sb;
		memcpy(&sb, p, sizeof(sb));
		if (proto_check_subscribe(&sb) < 0) {
			client_send_error(c, EINVAL, "unknown subscription bits");
			return 0;
		}
		c->sub_mask = sb.mask;
		client_send_ok(c);
		return 0;
	}
	case AUD_MSG_SET_VOLUME_OF: {
		struct aud_volume_of v;
		memcpy(&v, p, sizeof(v));
		if (v.left > 100 || v.right > 100) {
			client_send_error(c, EINVAL, "volume must be 0..100");
			return 0;
		}
		struct stream *s = stream_by_id(v.stream_id);
		if (!s) {
			client_send_error(c, ENOENT, "no such stream");
			return 0;
		}
		if (!may_control(c, s)) {
			client_send_error(c, EPERM, "not your stream");
			return 0;
		}
		stream_set_volume(s, v.left, v.right, s->mute);
		client_send_ok(c);
		return 0;
	}
	case AUD_MSG_KILL_STREAM: {
		struct aud_stream_ref r;
		memcpy(&r, p, sizeof(r));
		struct stream *s = stream_by_id(r.id);
		if (!s) {
			client_send_error(c, ENOENT, "no such stream");
			return 0;
		}
		if (!may_control(c, s)) {
			client_send_error(c, EPERM, "not your stream");
			return 0;
		}
		log_info("stream %u killed by uid %u", s->id, c->cred.uid);
		stream_kill(s);
		client_send_ok(c);
		return 0;
	}
	case AUD_MSG_GET_CARDS: {
		struct aud_cards cs;
		dev_list_cards(&srv.dev, &cs);
		client_send(c, AUD_MSG_GET_CARDS, &cs, sizeof(cs));
		return 0;
	}
	case AUD_MSG_SET_CARD: {
		struct aud_stream_ref r;
		memcpy(&r, p, sizeof(r));
		if (c->cred.uid != 0) {
			client_send_error(c, EPERM, "only root may switch cards");
			return 0;
		}
		if (r.id >= AUD_MAX_CARDS) {
			client_send_error(c, EINVAL, "no such card");
			return 0;
		}
		int rc = engine_switch_card((int)r.id);
		if (rc < 0) {
			client_send_error(c, -rc, "cannot open that card");
			return 0;
		}
		client_send_ok(c);
		return 0;
	}
	default:
		client_close(c, "unknown control request");
		return -1;
	}
}
