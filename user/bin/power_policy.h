/*
 * power_policy.h - who may halt, power off or reboot the machine
 *
 * The kernel lets only root call reboot(2).  The power commands (poweroff,
 * reboot, halt, shutdown) are installed setuid root and decide themselves
 * whom they act for, with the policy a desktop's session manager applies:
 *
 *   root                                          allowed
 *   the user logged in at the console, on its     allowed
 *     text console or in the desktop started there
 *   a remote login (ssh)                          denied
 *   any other user                                denied
 *
 * "Logged in at the console" is the kernel's answer: /sys/kernel/
 * console_session names the login session of the console's login and its
 * user.  The caller has to be that user AND belong to that login session
 * (/proc/self/sessionid) -- descended from that login, which a terminal
 * window's setsid() does not change.  The same account logged in over ssh
 * belongs to no console login (sshd's privileged setsid() leaves it), and
 * a process left behind by an earlier login has that login's id: login
 * session ids are never handed out twice.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_POWER_POLICY_H
#define LIKEOS_POWER_POLICY_H

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define POWER_CONSOLE_SESSION "/sys/kernel/console_session"

#define POWER_SELF_SESSION "/proc/self/sessionid"
#define POWER_SESSION_UNSET 4294967295UL

/* Read a small text file into buf (NUL-terminated); 0 when it is empty or
 * unreadable. */
static int power_read(const char *path, char *buf, size_t cap)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return 1;
}

/* A login session id or uid: decimal, followed by `stop'. */
static int power_number(const char *s, char stop, const char **rest,
			unsigned long *out)
{
	char *end;

	if (*s < '0' || *s > '9')
		return 0;
	errno = 0;
	*out = strtoul(s, &end, 10);
	if (errno || *out > POWER_SESSION_UNSET)
		return 0;
	if (*end != stop && !(stop == '\0' && *end == '\n' && !end[1]))
		return 0;
	*rest = end;
	return 1;
}

/* May the process that started this command halt, power off or reboot the
 * machine?  Decided on the REAL uid: the effective one is root's here. */
static int power_caller_allowed(void)
{
	char con[64], self[32];
	const char *p;
	unsigned long session, uid, mine;

	if (getuid() == 0)
		return 1;
	if (!power_read(POWER_CONSOLE_SESSION, con, sizeof(con)))
		return 0; /* nobody is logged in at the console */
	if (!power_number(con, ' ', &p, &session) ||
	    !power_number(p + 1, '\0', &p, &uid))
		return 0;
	if (!power_read(POWER_SELF_SESSION, self, sizeof(self)) ||
	    !power_number(self, '\0', &p, &mine))
		return 0;
	return session != POWER_SESSION_UNSET && mine == session &&
	       uid == (unsigned long)getuid();
}

/* An environment of the command's own for a caller who is not root: the
 * libraries a setuid program loads read it, as root.  Only the time zone is
 * kept (shutdown prints local times), and only when it names a zone rather
 * than a file -- no absolute path, no way up the tree. */
static void power_sanitize_env(void)
{
	const char *tz = getenv("TZ");
	char zone[64];

	if (getuid() == 0)
		return;
	zone[0] = '\0';
	if (tz && tz[0] != '/' && !strstr(tz, "..") &&
	    strlen(tz) < sizeof(zone))
		memcpy(zone, tz, strlen(tz) + 1);
	clearenv();
	setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
	if (zone[0])
		setenv("TZ", zone, 1);
}

#endif /* LIKEOS_POWER_POLICY_H */
