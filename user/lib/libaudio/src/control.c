// libaudio control connections: master volume and mute, outputs, cards,
// the list of playing streams, other streams' volume, and change
// notifications.
//
// Notifications (AUD_MSG_NOTIFY) can arrive at any time once subscribed,
// also while a request waits for its reply.  They are queued on the handle
// (bounded; the oldest are dropped when a program never reads them) and
// handed out by la_next_notify().  A program polling la_ctl_fd() must call
// la_next_notify() until it returns 0, since notifications that arrived
// together with a reply are already in the queue and no longer make the
// descriptor readable.
//
// Everything the service returns is checked: exact sizes, counts within the
// protocol's limits, strings terminated, volumes clamped to 0..100.
//
// Copyright (C) 2026 The LikeOS Project

#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "la_internal.h"

struct la_ctl {
	struct lai_conn conn;
	uint32_t sub_mask;

	/* Ring of pending notifications. */
	struct aud_notify q[LAI_NOTIFY_QUEUE];
	unsigned q_head; /* oldest */
	unsigned q_count;
	uint32_t q_dropped; /* lost to overflow, for diagnostics */
};

/* ============================================================================
 * Notifications
 * ========================================================================== */

static void notify_push(la_ctl *c, const struct aud_notify *n)
{
	unsigned slot;

	if (c->q_count == LAI_NOTIFY_QUEUE) {
		/* Full: the newest state matters more than the oldest
		 * history, so the oldest entry goes. */
		c->q_head = (c->q_head + 1) % LAI_NOTIFY_QUEUE;
		c->q_count--;
		if (c->q_dropped != UINT32_MAX)
			c->q_dropped++;
	}
	slot = (c->q_head + c->q_count) % LAI_NOTIFY_QUEUE;
	c->q[slot] = *n;
	c->q_count++;
}

static int notify_pop(la_ctl *c, struct aud_notify *n)
{
	if (c->q_count == 0)
		return 0;
	*n = c->q[c->q_head];
	c->q_head = (c->q_head + 1) % LAI_NOTIFY_QUEUE;
	c->q_count--;
	return 1;
}

/* Bring a notification into a defined shape before a program sees it. */
static int notify_sanitize(struct aud_notify *n)
{
	switch (n->what) {
	case AUD_NOTIFY_VOLUME:
		n->a = lai_clamp100(n->a);
		n->b = lai_clamp100(n->b);
		n->c = n->c ? 1 : 0;
		return 1;
	case AUD_NOTIFY_OUTPUT:
		n->c = 0;
		return 1;
	case AUD_NOTIFY_STREAM_NEW:
	case AUD_NOTIFY_STREAM_GONE:
		n->b = n->c = 0;
		return 1;
	case AUD_NOTIFY_CARD:
		n->b = n->b ? 1 : 0;
		n->c = 0;
		return 1;
	default:
		/* A kind this library does not know (newer service):
		 * dropped rather than handed out undefined. */
		return 0;
	}
}

static int ctl_async(void *ctx, const struct lai_msg *m)
{
	la_ctl *c = ctx;
	struct aud_notify n;

	if (m->type != AUD_MSG_NOTIFY)
		return 0;
	if (m->len != sizeof(n))
		return -EPROTO;
	memcpy(&n, m->payload, sizeof(n));
	if (notify_sanitize(&n))
		notify_push(c, &n);
	return 1;
}

/* ============================================================================
 * Opening and closing
 * ========================================================================== */

la_ctl *lai_ctl_open_fd(int fd, const char *app_name, int *err)
{
	la_ctl *c;
	int r;

	if (err)
		*err = 0;
	c = calloc(1, sizeof(*c));
	if (!c) {
		if (fd >= 0)
			close(fd);
		r = -ENOMEM;
		goto fail;
	}
	r = lai_conn_init(&c->conn, fd, ctl_async, c);
	if (r < 0) {
		free(c);
		c = NULL;
		goto fail;
	}
	r = lai_conn_hello(&c->conn, AUD_CLIENT_CONTROL, app_name);
	if (r < 0) {
		lai_conn_destroy(&c->conn);
		free(c);
		c = NULL;
		goto fail;
	}
	return c;
fail:
	if (err)
		*err = r;
	return NULL;
}

LA_EXPORT la_ctl *la_ctl_open(const char *app_name, int *err)
{
	int fd, r;

	if (err)
		*err = 0;
	r = lai_connect(AUDIOD_SOCKET_PATH, &fd);
	if (r < 0) {
		if (err)
			*err = r;
		return NULL;
	}
	return lai_ctl_open_fd(fd, app_name, err);
}

LA_EXPORT void la_ctl_close(la_ctl *c)
{
	if (!c)
		return;
	lai_conn_destroy(&c->conn);
	free(c);
}

LA_EXPORT int la_ctl_fd(la_ctl *c)
{
	if (!c || c->conn.fd < 0)
		return -EBADF;
	return c->conn.fd;
}

struct lai_conn *lai_ctl_conn(la_ctl *c)
{
	return c ? &c->conn : NULL;
}

/* ============================================================================
 * Request helpers
 * ========================================================================== */

static int ctl_usable(la_ctl *c)
{
	if (!c)
		return -EBADF;
	if (c->conn.dead)
		return c->conn.dead;
	/* An ERROR that came without a request (none is defined for control
	 * connections) is not a reason to fail the next call; forget it. */
	(void)lai_conn_take_async_err(&c->conn);
	return 0;
}

/* A request answered with OK. */
static int ctl_set(la_ctl *c, uint32_t type, const void *req, size_t len,
		   int timeout_ms)
{
	int r = ctl_usable(c);

	if (r < 0)
		return r;
	return lai_conn_request(&c->conn, type, req, len, AUD_MSG_OK, 0, 0,
				NULL, NULL, timeout_ms);
}

/* A request without payload answered with a fixed-size structure. */
static int ctl_get(la_ctl *c, uint32_t type, void *out, size_t size)
{
	const uint8_t *p;
	uint32_t plen;
	int r = ctl_usable(c);

	if (r < 0)
		return r;
	r = lai_conn_request(&c->conn, type, NULL, 0, type, size, size, &p,
			     &plen, c->conn.reply_timeout_ms);
	if (r < 0)
		return r;
	memcpy(out, p, size);
	return 0;
}

/* ============================================================================
 * Server and cards
 * ========================================================================== */

LA_EXPORT int la_get_server_info(la_ctl *c, struct aud_server_info *info)
{
	struct aud_server_info si;
	int r;

	if (!info)
		return -EINVAL;
	r = ctl_get(c, AUD_MSG_GET_SERVER_INFO, &si, sizeof(si));
	if (r < 0)
		return r;
	lai_fix_string(si.card_name, sizeof(si.card_name));
	lai_fix_string(si.codec, sizeof(si.codec));
	lai_fix_string(si.driver, sizeof(si.driver));
	if (si.format > AUD_FMT_MAX)
		si.format = 0;
	*info = si;
	return 0;
}

LA_EXPORT int la_get_cards(la_ctl *c, struct aud_cards *cards)
{
	struct aud_cards cs;
	uint32_t i;
	int r;

	if (!cards)
		return -EINVAL;
	r = ctl_get(c, AUD_MSG_GET_CARDS, &cs, sizeof(cs));
	if (r < 0)
		return r;
	if (cs.count > AUD_MAX_CARDS)
		cs.count = AUD_MAX_CARDS;
	cs.reserved = 0;
	for (i = 0; i < AUD_MAX_CARDS; i++) {
		struct aud_card_entry *e = &cs.card[i];

		if (i >= cs.count) {
			memset(e, 0, sizeof(*e));
			continue;
		}
		e->in_use = e->in_use ? 1 : 0;
		lai_fix_string(e->name, sizeof(e->name));
		lai_fix_string(e->codec, sizeof(e->codec));
		lai_fix_string(e->driver, sizeof(e->driver));
	}
	*cards = cs;
	return 0;
}

LA_EXPORT int la_set_card(la_ctl *c, uint32_t index)
{
	struct aud_stream_ref ref;

	if (!c)
		return -EBADF;
	if (index >= 64)
		return -EINVAL;
	memset(&ref, 0, sizeof(ref));
	ref.id = index;
	return ctl_set(c, AUD_MSG_SET_CARD, &ref, sizeof(ref),
		       c->conn.reply_timeout_ms);
}

/* ============================================================================
 * Master volume, mute, outputs
 * ========================================================================== */

LA_EXPORT int la_get_volume(la_ctl *c, uint32_t *left, uint32_t *right)
{
	struct aud_volume v;
	int r;

	r = ctl_get(c, AUD_MSG_GET_MASTER_VOLUME, &v, sizeof(v));
	if (r < 0)
		return r;
	if (left)
		*left = lai_clamp100(v.left);
	if (right)
		*right = lai_clamp100(v.right);
	return 0;
}

LA_EXPORT int la_set_volume_master(la_ctl *c, uint32_t left, uint32_t right)
{
	struct aud_volume v;

	if (!c)
		return -EBADF;
	if (left > 100 || right > 100)
		return -EINVAL;
	memset(&v, 0, sizeof(v));
	v.left = left;
	v.right = right;
	return ctl_set(c, AUD_MSG_SET_MASTER_VOLUME, &v, sizeof(v),
		       c->conn.reply_timeout_ms);
}

LA_EXPORT int la_get_mute(la_ctl *c, int *mute)
{
	struct aud_mute m;
	int r;

	if (!mute)
		return -EINVAL;
	r = ctl_get(c, AUD_MSG_GET_MUTE, &m, sizeof(m));
	if (r < 0)
		return r;
	*mute = m.mute ? 1 : 0;
	return 0;
}

LA_EXPORT int la_set_mute_master(la_ctl *c, int mute)
{
	struct aud_mute m;

	if (!c)
		return -EBADF;
	memset(&m, 0, sizeof(m));
	m.mute = mute ? 1 : 0;
	return ctl_set(c, AUD_MSG_SET_MUTE, &m, sizeof(m),
		       c->conn.reply_timeout_ms);
}

LA_EXPORT int la_get_outputs(la_ctl *c, struct aud_outputs *out)
{
	struct aud_outputs o;
	uint32_t i;
	int r;

	if (!out)
		return -EINVAL;
	r = ctl_get(c, AUD_MSG_GET_OUTPUTS, &o, sizeof(o));
	if (r < 0)
		return r;
	if (o.count > AUD_MAX_OUTPUTS)
		o.count = AUD_MAX_OUTPUTS;
	o.reserved = 0;
	for (i = 0; i < AUD_MAX_OUTPUTS; i++) {
		struct aud_output *e = &o.out[i];

		if (i >= o.count) {
			memset(e, 0, sizeof(*e));
			continue;
		}
		e->present = e->present ? 1 : 0;
		e->active = e->active ? 1 : 0;
		lai_fix_string(e->name, sizeof(e->name));
		lai_fix_string(e->monitor, sizeof(e->monitor));
	}
	*out = o;
	return 0;
}

LA_EXPORT int la_set_output(la_ctl *c, uint32_t mode, uint32_t id)
{
	struct aud_set_output so;

	if (!c)
		return -EBADF;
	/* Mode values belong to <sys/audio.h>; the service validates them
	 * against the card.  Only plainly impossible values stop here. */
	if (mode > 255)
		return -EINVAL;
	memset(&so, 0, sizeof(so));
	so.mode = mode;
	so.id = id;
	return ctl_set(c, AUD_MSG_SET_OUTPUT, &so, sizeof(so),
		       c->conn.reply_timeout_ms);
}

/* ============================================================================
 * Streams
 * ========================================================================== */

static void sanitize_entry(struct aud_stream_entry *e)
{
	if (e->role > AUD_ROLE_OTHER)
		e->role = AUD_ROLE_OTHER;
	if (e->format > AUD_FMT_MAX)
		e->format = 0;
	e->left = lai_clamp100(e->left);
	e->right = lai_clamp100(e->right);
	e->mute = e->mute ? 1 : 0;
	if (e->state > 3)
		e->state = 0;
	lai_fix_string(e->name, sizeof(e->name));
	lai_fix_string(e->app, sizeof(e->app));
}

/* Returns the number of entries stored in `out' (at most `max').  With
 * out == NULL and max == 0 it returns how many streams there are, so a
 * caller can size its array. */
LA_EXPORT int la_list_streams(la_ctl *c, struct aud_stream_entry *out,
			      int max)
{
	struct aud_stream_list head;
	const size_t esz = sizeof(struct aud_stream_entry);
	const size_t hsz = offsetof(struct aud_stream_list, entry);
	const uint8_t *p;
	uint32_t plen, i, n;
	int r;

	if (max < 0 || (max > 0 && !out))
		return -EINVAL;
	r = ctl_usable(c);
	if (r < 0)
		return r;
	r = lai_conn_request(&c->conn, AUD_MSG_LIST_STREAMS, NULL, 0,
			     AUD_MSG_LIST_STREAMS, hsz,
			     hsz + (size_t)AUD_MAX_STREAMS * esz, &p, &plen,
			     c->conn.reply_timeout_ms);
	if (r < 0)
		return r;
	memcpy(&head, p, hsz);
	/* The count and the length must agree exactly; a list that claims
	 * more entries than it carries (or carries more than it claims) is
	 * a protocol violation, not something to guess around. */
	if (head.count > AUD_MAX_STREAMS ||
	    (size_t)plen != hsz + (size_t)head.count * esz)
		return lai_conn_fail(&c->conn, -EPROTO);
	if (!out)
		return (int)head.count;
	n = head.count < (uint32_t)max ? head.count : (uint32_t)max;
	for (i = 0; i < n; i++) {
		memcpy(&out[i], p + hsz + (size_t)i * esz, esz);
		sanitize_entry(&out[i]);
	}
	return (int)n;
}

LA_EXPORT int la_set_stream_volume(la_ctl *c, uint32_t stream_id,
				   uint32_t left, uint32_t right)
{
	struct aud_volume_of v;

	if (!c)
		return -EBADF;
	if (left > 100 || right > 100)
		return -EINVAL;
	memset(&v, 0, sizeof(v));
	v.stream_id = stream_id;
	v.left = left;
	v.right = right;
	return ctl_set(c, AUD_MSG_SET_VOLUME_OF, &v, sizeof(v),
		       c->conn.reply_timeout_ms);
}

LA_EXPORT int la_kill_stream(la_ctl *c, uint32_t stream_id)
{
	struct aud_stream_ref ref;

	if (!c)
		return -EBADF;
	memset(&ref, 0, sizeof(ref));
	ref.id = stream_id;
	return ctl_set(c, AUD_MSG_KILL_STREAM, &ref, sizeof(ref),
		       c->conn.reply_timeout_ms);
}

/* ============================================================================
 * Subscriptions
 * ========================================================================== */

#define LAI_SUB_ALL                                                          \
	(AUD_SUB_VOLUME | AUD_SUB_OUTPUTS | AUD_SUB_STREAMS | AUD_SUB_CARDS)

LA_EXPORT int la_subscribe(la_ctl *c, uint32_t mask)
{
	struct aud_subscribe sb;
	int r;

	if (!c)
		return -EBADF;
	if (mask & ~(uint32_t)LAI_SUB_ALL)
		return -EINVAL;
	memset(&sb, 0, sizeof(sb));
	sb.mask = mask;
	r = ctl_set(c, AUD_MSG_SUBSCRIBE, &sb, sizeof(sb),
		    c->conn.reply_timeout_ms);
	if (r == 0)
		c->sub_mask = mask;
	return r;
}

LA_EXPORT int la_next_notify(la_ctl *c, struct aud_notify *n)
{
	int r;

	if (!c)
		return -EBADF;
	if (!n)
		return -EINVAL;
	/* Queued ones first: they arrived before anything still unread. */
	if (notify_pop(c, n))
		return 1;
	if (c->conn.dead)
		return c->conn.dead;
	r = lai_conn_pump(&c->conn);
	/* Even when the connection just ended, what came before the end is
	 * handed out first. */
	if (notify_pop(c, n))
		return 1;
	return r < 0 ? r : 0;
}

LA_EXPORT const char *la_ctl_errmsg(la_ctl *c)
{
	if (!c)
		return "";
	return c->conn.err_msg;
}
