// dmcore.c - the decisions of likeos-dm and its request format (dmcore.h).
//
// Copyright (C) 2026 The LikeOS Project

#include "dmcore.h"

#include <stdio.h>
#include <string.h>

int dm_user_name_valid(const char *name)
{
	size_t n;

	if (!name || !name[0] || name[0] == '-' || name[0] == '.')
		return 0;
	for (n = 0; name[n]; n++) {
		char c = name[n];

		if (n >= DM_USER_MAX)
			return 0;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '.' ||
		      c == '-'))
			return 0;
	}
	return 1;
}

const char *dm_req_name(enum dm_req req)
{
	switch (req) {
	case DM_REQ_CONSOLE:
		return "console";
	case DM_REQ_GRAPHICAL:
		return "graphical";
	default:
		return NULL;
	}
}

int dm_format_request(char *buf, size_t cap, enum dm_req req,
		      const char *user)
{
	const char *what = dm_req_name(req);
	int n;

	if (!buf || !what || !dm_user_name_valid(user))
		return -1;
	n = snprintf(buf, cap, "%s %s\n", what, user);
	if (n < 0 || (size_t)n >= cap)
		return -1;
	return n;
}

enum dm_req dm_parse_request(const char *buf, size_t len, char *user,
			     size_t cap)
{
	enum dm_req req;
	const char *p, *end, *sp;
	size_t ulen;

	if (!buf || !user || cap < DM_USER_MAX + 1)
		return DM_REQ_NONE;
	user[0] = '\0';
	// Exactly one line, newline-terminated, no NUL inside.
	if (len < 2 || buf[len - 1] != '\n' || memchr(buf, '\0', len) ||
	    memchr(buf, '\n', len - 1))
		return DM_REQ_NONE;
	end = buf + len - 1;
	sp = memchr(buf, ' ', (size_t)(end - buf));
	if (!sp)
		return DM_REQ_NONE;
	if ((size_t)(sp - buf) == 7 && memcmp(buf, "console", 7) == 0)
		req = DM_REQ_CONSOLE;
	else if ((size_t)(sp - buf) == 9 && memcmp(buf, "graphical", 9) == 0)
		req = DM_REQ_GRAPHICAL;
	else
		return DM_REQ_NONE;
	p = sp + 1;
	ulen = (size_t)(end - p);
	if (ulen == 0 || ulen > DM_USER_MAX)
		return DM_REQ_NONE;
	memcpy(user, p, ulen);
	user[ulen] = '\0';
	if (!dm_user_name_valid(user)) {
		user[0] = '\0';
		return DM_REQ_NONE;
	}
	return req;
}

pid_t dm_parse_pid(const char *buf, size_t len)
{
	size_t i = 0;
	long v = 0;
	int digits = 0;

	if (!buf)
		return -1;
	while (i < len && buf[i] == ' ')
		i++;
	for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
		v = v * 10 + (buf[i] - '0');
		if (v > 0x3fffffff)
			return -1;
		digits++;
	}
	if (i < len && buf[i] == '\n')
		i++;
	if (!digits || i != len || v <= 0)
		return -1;
	return (pid_t)v;
}

int dm_shell_listed(const char *shells, size_t len, const char *shell)
{
	size_t i = 0, slen;

	if (!shells)
		return 0;
	if (!shell || !shell[0])
		shell = "/bin/sh";
	slen = strlen(shell);
	while (i < len) {
		size_t start = i, end;

		while (i < len && shells[i] != '\n')
			i++;
		end = i;
		if (i < len)
			i++; // the newline
		// Surrounding blanks do not count; comments and blank lines
		// name no shell.
		while (start < end &&
		       (shells[start] == ' ' || shells[start] == '\t'))
			start++;
		while (end > start &&
		       (shells[end - 1] == ' ' || shells[end - 1] == '\t' ||
			shells[end - 1] == '\r'))
			end--;
		if (start == end || shells[start] == '#' ||
		    shells[start] != '/')
			continue;
		if (end - start == slen &&
		    memcmp(shells + start, shell, slen) == 0)
			return 1;
	}
	return 0;
}

void dm_init(struct dm_state *s)
{
	memset(s, 0, sizeof(*s));
	s->mode = DM_MODE_GRAPHICAL;
}

void dm_enter_graphical(struct dm_state *s)
{
	s->mode = DM_MODE_GRAPHICAL;
	s->user[0] = '\0';
	s->graphical_failures = 0;
	s->getty_quick_exits = 0;
}

void dm_enter_console_user(struct dm_state *s, const char *user)
{
	s->mode = DM_MODE_CONSOLE_USER;
	snprintf(s->user, sizeof(s->user), "%s", user);
	s->getty_quick_exits = 0;
}

enum dm_action dm_request_action(const struct dm_state *s, enum dm_req req,
				 const char *user)
{
	switch (req) {
	case DM_REQ_CONSOLE:
		if (s->mode == DM_MODE_GRAPHICAL && dm_user_name_valid(user))
			return DM_ACT_LEAVE_GRAPHICAL;
		return DM_ACT_IGNORE;
	case DM_REQ_GRAPHICAL:
		if (s->mode == DM_MODE_CONSOLE_USER ||
		    s->mode == DM_MODE_CONSOLE_GETTY)
			return DM_ACT_LEAVE_CONSOLE;
		return DM_ACT_IGNORE;
	default:
		return DM_ACT_IGNORE;
	}
}

unsigned dm_child_exited(struct dm_state *s, long ran, char *why, size_t cap)
{
	if (why && cap)
		why[0] = '\0';
	switch (s->mode) {
	case DM_MODE_GRAPHICAL:
		// A login screen that ran for a while and then went away is
		// started again at once and counts as the first failure of a
		// new run; one that never came up counts towards giving up.
		if (ran >= DM_QUICK_EXIT_SECS)
			s->graphical_failures = 1;
		else
			s->graphical_failures++;
		if (s->graphical_failures >= DM_GRAPHICAL_TRIES) {
			if (why && cap)
				snprintf(why, cap,
					 "the graphical login failed to start "
					 "%d times in a row",
					 s->graphical_failures);
			s->mode = DM_MODE_CONSOLE_GETTY;
			s->graphical_failures = 0;
			s->getty_quick_exits = 0;
			return 0;
		}
		// Never a tight loop: 2 s, then 5 s.
		return s->graphical_failures <= 1 ? 2 : 5;
	case DM_MODE_CONSOLE_USER:
		// The console login ended: a text login prompt follows.
		s->mode = DM_MODE_CONSOLE_GETTY;
		s->user[0] = '\0';
		s->getty_quick_exits = 0;
		return 0;
	case DM_MODE_CONSOLE_GETTY:
	default:
		if (ran >= DM_GETTY_QUICK_SECS) {
			s->getty_quick_exits = 0;
			return 0;
		}
		if (++s->getty_quick_exits >= DM_GETTY_TRIES) {
			s->getty_quick_exits = 0;
			return DM_GETTY_HOLD_SECS;
		}
		return 0;
	}
}
