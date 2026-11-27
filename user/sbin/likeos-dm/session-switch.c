// likeos-session-switch - ask likeos-dm to switch the console between the
// graphical login and a console login.
//
//   likeos-session-switch console    leave the graphical session for a
//                                    shell at the console, logged in as
//                                    the caller (the desktop's "Console")
//   likeos-session-switch graphical  end the console login -- and with it
//                                    a desktop started there with startx
//                                    -- and show the login screen again
//                                    (likeos-logout, on "Sign out")
//
// Installed setuid root in /usr/libexec: likeos-dm's request file and pid
// are root's alone.  It acts only for root and for the user logged in at
// the console, from inside that login (power_policy.h, the rule the power
// commands follow) -- the person at the machine, not a remote login of the
// same account.  The request carries the CALLER's user name, taken from
// the password database by the real uid, never a name given on the command
// line, and only an account with a login shell listed in /etc/shells can
// be asked to be logged in at the console.  Whether the switch fits what
// the console is doing is likeos-dm's decision.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../bin/power_policy.h"
#include "dmcore.h"

#define SHELLS_FILE "/etc/shells"

static void usage(FILE *f)
{
	fprintf(f, "usage: likeos-session-switch console|graphical\n"
		   "  console    leave the desktop for a shell at the console\n"
		   "  graphical  end the console login, show the login screen\n");
}

static int read_file(const char *path, char *buf, size_t cap, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	*len = 0;
	while (*len < cap - 1 &&
	       (n = read(fd, buf + *len, cap - 1 - *len)) > 0)
		*len += (size_t)n;
	close(fd);
	buf[*len] = '\0';
	return 0;
}

int main(int argc, char **argv)
{
	enum dm_req req;
	struct passwd *pw;
	char user[DM_USER_MAX + 1], buf[128], shells[8192];
	size_t len;
	pid_t dm;
	int fd, n;

	if (argc == 2 && strcmp(argv[1], "--help") == 0) {
		usage(stdout);
		return 0;
	}
	if (argc != 2) {
		usage(stderr);
		return 2;
	}
	if (strcmp(argv[1], "console") == 0)
		req = DM_REQ_CONSOLE;
	else if (strcmp(argv[1], "graphical") == 0)
		req = DM_REQ_GRAPHICAL;
	else {
		usage(stderr);
		return 2;
	}

	// Setuid root: an environment of its own, and only for the person
	// at the console.
	power_sanitize_env();
	umask(077);
	if (!power_caller_allowed()) {
		fprintf(stderr, "likeos-session-switch: only the user logged "
				"in at the console may switch the session\n");
		return 1;
	}

	pw = getpwuid(getuid());
	if (!pw || !dm_user_name_valid(pw->pw_name)) {
		fprintf(stderr, "likeos-session-switch: no usable account "
				"for uid %u\n", (unsigned)getuid());
		return 1;
	}
	snprintf(user, sizeof(user), "%s", pw->pw_name);
	if (req == DM_REQ_CONSOLE &&
	    (read_file(SHELLS_FILE, shells, sizeof(shells), &len) != 0 ||
	     !dm_shell_listed(shells, len, pw->pw_shell))) {
		fprintf(stderr, "likeos-session-switch: %s has no login shell "
				"(%s is not in %s)\n", user,
			pw->pw_shell[0] ? pw->pw_shell : "/bin/sh",
			SHELLS_FILE);
		return 1;
	}

	if (read_file(DM_PID_FILE, buf, sizeof(buf), &len) != 0 ||
	    (dm = dm_parse_pid(buf, len)) <= 1 || kill(dm, 0) != 0) {
		fprintf(stderr, "likeos-session-switch: the display manager "
				"(likeos-dm) is not running\n");
		return 1;
	}

	n = dm_format_request(buf, sizeof(buf), req, user);
	if (n < 0)
		return 1;
	// Written whole under another name and renamed into place, so
	// likeos-dm never reads half a request.
	unlink(DM_REQUEST_TMP);
	fd = open(DM_REQUEST_TMP,
		  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		fprintf(stderr, "likeos-session-switch: %s: %s\n",
			DM_REQUEST_TMP, strerror(errno));
		return 1;
	}
	if (write(fd, buf, (size_t)n) != n || close(fd) != 0 ||
	    rename(DM_REQUEST_TMP, DM_REQUEST_FILE) != 0) {
		fprintf(stderr, "likeos-session-switch: cannot write the "
				"request: %s\n", strerror(errno));
		unlink(DM_REQUEST_TMP);
		return 1;
	}
	if (kill(dm, SIGUSR1) != 0) {
		fprintf(stderr, "likeos-session-switch: cannot reach likeos-dm: "
				"%s\n", strerror(errno));
		return 1;
	}
	return 0;
}
