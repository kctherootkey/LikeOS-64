// dmcore.h - the decisions of likeos-dm and the request format it shares
// with likeos-session-switch, apart from any process handling so the build
// machine can test them (tests/test_dmcore.c).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_DMCORE_H
#define LIKEOS_DMCORE_H

#include <stddef.h>
#include <sys/types.h>

// Where likeos-dm keeps its pid and receives requests: root's alone (0700).
#define DM_RUN_DIR "/run/likeos-dm"
#define DM_PID_FILE DM_RUN_DIR "/pid"
#define DM_REQUEST_FILE DM_RUN_DIR "/request"
#define DM_REQUEST_TMP DM_RUN_DIR "/request.new"

// The longest user name a request carries (the passwd database's limit).
#define DM_USER_MAX 32

// A lightdm that exits sooner than this after its start did not come up.
#define DM_QUICK_EXIT_SECS 20
// That many such exits in a row and the console gets a text login instead.
#define DM_GRAPHICAL_TRIES 3
// A getty that exits sooner than this, that many times in a row, is held
// off for DM_GETTY_HOLD_SECS -- init's rule for anything it respawns.
#define DM_GETTY_QUICK_SECS 5
#define DM_GETTY_TRIES 5
#define DM_GETTY_HOLD_SECS 30

enum dm_mode {
	DM_MODE_GRAPHICAL,    // lightdm: the login screen and its sessions
	DM_MODE_CONSOLE_USER, // login -f <user> on the console
	DM_MODE_CONSOLE_GETTY // getty on the console
};

enum dm_req {
	DM_REQ_NONE,
	DM_REQ_CONSOLE,  // leave the graphical login for a console login
	DM_REQ_GRAPHICAL // end the console login, show the login screen
};

enum dm_action {
	DM_ACT_IGNORE,         // not applicable in this mode
	DM_ACT_LEAVE_GRAPHICAL, // stop lightdm, then login -f the user
	DM_ACT_LEAVE_CONSOLE   // hang up the console login, then lightdm
};

struct dm_state {
	enum dm_mode mode;
	char user[DM_USER_MAX + 1]; // DM_MODE_CONSOLE_USER's account
	int graphical_failures;     // lightdm starts in a row that failed
	int getty_quick_exits;      // quick getty exits in a row
};

// A user name a request may carry: 1..DM_USER_MAX characters of
// [A-Za-z0-9_.-], not starting with '-' or '.'.
int dm_user_name_valid(const char *name);

const char *dm_req_name(enum dm_req req);

// "console alice\n".  Returns the length written (without the NUL), or -1
// when the request or name is invalid or the buffer too small.
int dm_format_request(char *buf, size_t cap, enum dm_req req,
		      const char *user);

// The reverse; user receives the name (cap >= DM_USER_MAX + 1).  Anything
// that is not exactly one well-formed request is DM_REQ_NONE.
enum dm_req dm_parse_request(const char *buf, size_t len, char *user,
			     size_t cap);

// A pid as the X server writes its lock file ("%10d\n") or likeos-dm its
// pid file ("%d\n").  Returns it, or -1.
pid_t dm_parse_pid(const char *buf, size_t len);

// Whether `shell' is one of the login shells listed in the text of
// /etc/shells (`shells', len bytes): one absolute path per line, '#'
// comments.  An empty shell is /bin/sh, as login(1) takes it.
int dm_shell_listed(const char *shells, size_t len, const char *shell);

// The state at start: the login screen.
void dm_init(struct dm_state *s);
void dm_enter_graphical(struct dm_state *s);
void dm_enter_console_user(struct dm_state *s, const char *user);

// What a request means in the current mode.  A console request needs a
// valid user and the graphical login to be what runs; a graphical one a
// console login (of either kind).
enum dm_action dm_request_action(const struct dm_state *s, enum dm_req req,
				 const char *user);

// The supervised program (lightdm, login, getty) exited by itself after
// running `ran' seconds -- or, for the graphical login, could not be
// started at all (ran 0).  Moves to the next mode and returns how many
// seconds to wait before starting it.  When the graphical login is given
// up, `why' (cap bytes) says so for the console; it is "" otherwise.
unsigned dm_child_exited(struct dm_state *s, long ran, char *why, size_t cap);

#endif
