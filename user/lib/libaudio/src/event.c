// libaudio system event sounds: ask audiod to play one of the named sounds
// from its sound directory (startup, shutdown, login, notify, error,
// volume, ...).  Only a name crosses the socket, never a path; names are
// checked here already so a bad one fails fast and never reaches the
// service.
//
// Copyright (C) 2026 The LikeOS Project

#define _GNU_SOURCE

#include <errno.h>
#include <string.h>

#include "la_internal.h"

LA_EXPORT int la_play_event(la_ctl *c, const char *name, uint32_t volume,
			    int wait)
{
	struct aud_play_event ev;
	struct lai_conn *conn;
	size_t n;

	if (!c)
		return -EBADF;
	if (!name)
		return -EINVAL;
	n = strlen(name);
	if (n >= AUD_EVENT_NAME_MAX)
		return -ENAMETOOLONG;
	if (!lai_event_name_ok(name))
		return -EINVAL;
	if (volume > 100)
		return -EINVAL;
	conn = lai_ctl_conn(c);
	if (conn->dead)
		return conn->dead;
	(void)lai_conn_take_async_err(conn);

	memset(&ev, 0, sizeof(ev));
	memcpy(ev.name, name, n); /* validated: printable ASCII, fits */
	ev.volume = volume;
	ev.wait = wait ? 1 : 0;
	/* With `wait' the reply comes when the sound has finished. */
	return lai_conn_request(conn, AUD_MSG_PLAY_EVENT, &ev, sizeof(ev),
				AUD_MSG_OK, 0, 0, NULL, NULL,
				wait ? LAI_EVENT_WAIT_TIMEOUT_MS :
				       conn->reply_timeout_ms);
}

LA_EXPORT int la_play_event_simple(const char *name, int wait)
{
	la_ctl *c;
	int err = 0, r;

	if (!name)
		return -EINVAL;
	if (strlen(name) >= AUD_EVENT_NAME_MAX)
		return -ENAMETOOLONG;
	if (!lai_event_name_ok(name))
		return -EINVAL;
	c = la_ctl_open(NULL, &err);
	if (!c)
		return err ? err : -EIO;
	r = la_play_event(c, name, 100, wait);
	la_ctl_close(c);
	return r;
}
