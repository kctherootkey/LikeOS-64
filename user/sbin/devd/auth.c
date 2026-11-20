// devd: who may mount, unmount, eject and format.
//
// Root, and the user logged in at the console: the uid in
// /sys/kernel/console_user, and the peer process must belong to the login
// session /sys/kernel/console_session names (its /proc/<pid>/sessionid).
// The same rule the power commands apply: a remote login of the same
// account, or a process left over from an earlier login, is refused.
// Decided again for every request, so a login that ended loses the right at
// once.  The texts are parsed strictly: digits, one blank where one belongs,
// an optional final newline, nothing else.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdio.h>
#include <string.h>

#include "devd.h"

#define SESSION_UNSET 4294967295UL

/* A decimal number ending at `stop' (or at the end, with an optional single
 * newline when stop is '\0').  Returns the text after it, or NULL. */
static const char *number(const char *s, char stop, unsigned long *out)
{
	unsigned long v = 0;
	int digits = 0;

	while (*s >= '0' && *s <= '9') {
		if (++digits > 10)
			return NULL;
		v = v * 10 + (unsigned long)(*s - '0');
		s++;
	}
	if (!digits || v > SESSION_UNSET)
		return NULL;
	if (stop == '\0') {
		if (*s == '\n')
			s++;
		if (*s)
			return NULL;
	} else if (*s != stop) {
		return NULL;
	} else {
		s++;
	}
	*out = v;
	return s;
}

int auth_console_decide(uint32_t uid, const char *console_user,
			const char *console_session, const char *peer_session)
{
	unsigned long cu, session, su, mine;
	const char *p;

	if (uid == 0)
		return 1;
	if (!console_user || !console_session || !peer_session)
		return 0;
	if (!number(console_user, '\0', &cu))
		return 0;
	p = number(console_session, ' ', &session);
	if (!p || !number(p, '\0', &su))
		return 0;
	if (!number(peer_session, '\0', &mine))
		return 0;
	return session != SESSION_UNSET && mine == session && cu == su &&
	       su == (unsigned long)uid;
}

int auth_may_modify(uint32_t uid, int32_t pid)
{
	char cu[32], cs[64], ps[32], path[64];

	if (uid == 0)
		return 1;
	if (pid <= 0)
		return 0;
	if (read_file("/sys/kernel/console_user", cu, sizeof(cu)) <= 0 ||
	    read_file("/sys/kernel/console_session", cs, sizeof(cs)) <= 0)
		return 0; /* nobody is logged in at the console */
	snprintf(path, sizeof(path), "/proc/%d/sessionid", (int)pid);
	if (read_file(path, ps, sizeof(ps)) <= 0)
		return 0;
	return auth_console_decide(uid, cu, cs, ps);
}
