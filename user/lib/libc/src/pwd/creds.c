/*
 * creds.c - process credential helpers not backed by a dedicated syscall:
 *   setreuid()/setregid() emulated on top of getres*id()/setres*id(),
 *   and getlogin()/getlogin_r()/setlogin() over a stored login name.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef LOGIN_NAME_MAX
#define LOGIN_NAME_MAX 256
#endif

int setreuid(int ruid, int euid)
{
	int r, e, s;
	int nr, ne, ns;

	if (getresuid(&r, &e, &s) != 0)
		return -1;

	nr = (ruid == -1) ? -1 : ruid;
	ne = (euid == -1) ? -1 : euid;
	ns = -1; /* leave saved-uid unchanged by default */

	/* POSIX: the saved-uid follows the new effective-uid when the real-uid
	 * is changed, or when the effective-uid is set to something other than
	 * the previous real-uid. */
	if (ruid != -1 || (euid != -1 && euid != r))
		ns = (euid != -1) ? euid : e;

	return setresuid(nr, ne, ns);
}

int setregid(int rgid, int egid)
{
	int r, e, s;
	int nr, ne, ns;

	if (getresgid(&r, &e, &s) != 0)
		return -1;

	nr = (rgid == -1) ? -1 : rgid;
	ne = (egid == -1) ? -1 : egid;
	ns = -1;

	if (rgid != -1 || (egid != -1 && egid != r))
		ns = (egid != -1) ? egid : e;

	return setresgid(nr, ne, ns);
}

/* ---- login name ---- */
static char g_login[LOGIN_NAME_MAX + 1];
static int  g_login_set;

int setlogin(const char *name)
{
	if (!name) {
		errno = EINVAL;
		return -1;
	}
	if (strlen(name) > LOGIN_NAME_MAX) {
		errno = EINVAL;
		return -1;
	}
	strcpy(g_login, name);
	g_login_set = 1;
	return 0;
}

int getlogin_r(char *buf, size_t bufsize)
{
	const char *name = NULL;

	if (!buf || bufsize == 0) {
		errno = EINVAL;
		return EINVAL;
	}
	if (g_login_set)
		name = g_login;
	if (!name)
		name = getenv("LOGNAME");
	if (!name)
		name = getenv("USER");
	if (!name) {
		errno = ENXIO;
		return ENXIO;
	}
	if (strlen(name) + 1 > bufsize) {
		errno = ERANGE;
		return ERANGE;
	}
	strcpy(buf, name);
	return 0;
}

char *getlogin(void)
{
	static char buf[LOGIN_NAME_MAX + 1];
	if (getlogin_r(buf, sizeof(buf)) != 0)
		return NULL;
	return buf;
}
