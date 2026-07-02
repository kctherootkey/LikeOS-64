/*
 * login - authenticate a user and start their login shell.
 *
 * Core-Unix login: prompt for a name and password, verify via the PAM-like
 * framework, switch credentials, build the login environment, chdir to the
 * home directory and exec the shell as a login shell ("-sh").
 *
 * Usage: login [-f username] [-h host] [username]
 *   -f username   skip authentication for username (root only)
 *   -h host       remote host name (recorded for a future sshd; unused now)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>
#include <security/pam_appl.h>

#define MAX_TRIES 3

/* Read one line from fd into a malloc'd, NUL-terminated string (newline
 * stripped).  Returns NULL on EOF with no data. */
static char *read_line(int fd)
{
	size_t cap = 64, len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	for (;;) {
		char c;
		ssize_t n = read(fd, &c, 1);
		if (n <= 0) {
			if (len == 0) {
				free(buf);
				return NULL;
			}
			break;
		}
		if (c == '\n')
			break;
		if (len + 1 >= cap) {
			char *nb = realloc(buf, cap *= 2);
			if (!nb) {
				free(buf);
				return NULL;
			}
			buf = nb;
		}
		buf[len++] = c;
	}
	buf[len] = '\0';
	return buf;
}

/* Read a line with terminal echo disabled (for passwords). */
static char *read_password(int fd)
{
	struct termios old, raw;
	char *line;
	int have_tty = (tcgetattr(fd, &old) == 0);

	if (have_tty) {
		raw = old;
		raw.c_lflag &= ~ECHO;
		tcsetattr(fd, TCSAFLUSH, &raw);
	}
	line = read_line(fd);
	if (have_tty) {
		tcsetattr(fd, TCSAFLUSH, &old);
		fputc('\n', stdout);
		fflush(stdout);
	}
	return line;
}

/* PAM conversation: drive prompts over the controlling terminal. */
static int login_conv(int num_msg, const struct pam_message **msg,
                      struct pam_response **resp, void *appdata)
{
	struct pam_response *r;
	int i;
	(void)appdata;

	if (num_msg <= 0)
		return PAM_CONV_ERR;
	r = calloc(num_msg, sizeof(*r));
	if (!r)
		return PAM_BUF_ERR;

	for (i = 0; i < num_msg; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			fputs(msg[i]->msg, stdout);
			fflush(stdout);
			r[i].resp = read_password(0);
			break;
		case PAM_PROMPT_ECHO_ON:
			fputs(msg[i]->msg, stdout);
			fflush(stdout);
			r[i].resp = read_line(0);
			break;
		case PAM_ERROR_MSG:
			fputs(msg[i]->msg, stderr);
			fputc('\n', stderr);
			break;
		case PAM_TEXT_INFO:
			fputs(msg[i]->msg, stdout);
			fputc('\n', stdout);
			break;
		default:
			break;
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

static void setup_environment(const struct passwd *pw, char *envp[], int *n)
{
	static char home[256], user[128], logname[128], shell[256], term[64];
	const char *t = getenv("TERM");

	snprintf(home, sizeof(home), "HOME=%s", pw->pw_dir);
	snprintf(user, sizeof(user), "USER=%s", pw->pw_name);
	snprintf(logname, sizeof(logname), "LOGNAME=%s", pw->pw_name);
	snprintf(shell, sizeof(shell), "SHELL=%s",
	         pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
	snprintf(term, sizeof(term), "TERM=%s", t ? t : "linux");

	*n = 0;
	envp[(*n)++] = home;
	envp[(*n)++] = user;
	envp[(*n)++] = logname;
	envp[(*n)++] = shell;
	envp[(*n)++] = (char *)"PATH=/bin:/usr/local/bin";
	envp[(*n)++] = term;
	envp[*n] = NULL;
}

int main(int argc, char *argv[])
{
	struct pam_conv conv = { login_conv, NULL };
	pam_handle_t *pamh = NULL;
	const char *host = NULL;
	const char *forced = NULL;
	char *username = NULL;
	struct passwd *pw;
	int noauth = 0, opt, rc, tries;

	while ((opt = getopt(argc, argv, "f:h:")) != -1) {
		switch (opt) {
		case 'f':
			forced = optarg;
			noauth = 1;
			break;
		case 'h':
			host = optarg;
			break;
		default:
			fprintf(stderr, "usage: login [-f user] [-h host] [user]\n");
			return 1;
		}
	}
	if (optind < argc)
		username = argv[optind];
	if (forced)
		username = (char *)forced;

	if (noauth && getuid() != 0) {
		fprintf(stderr, "login: -f requires root\n");
		return 1;
	}

	rc = pam_start("login", username, &conv, &pamh);
	if (rc != PAM_SUCCESS) {
		fprintf(stderr, "login: pam_start failed\n");
		return 1;
	}
	if (host)
		pam_set_item(pamh, PAM_RHOST, host);

	if (!noauth) {
		for (tries = 0; tries < MAX_TRIES; tries++) {
			rc = pam_authenticate(pamh, 0);
			if (rc == PAM_SUCCESS)
				rc = pam_acct_mgmt(pamh, 0);
			if (rc == PAM_SUCCESS)
				break;
			fprintf(stderr, "\nLogin incorrect\n\n");
			/* Drop the entered name so the next round re-prompts. */
			pam_set_item(pamh, PAM_USER, NULL);
		}
		if (rc != PAM_SUCCESS) {
			pam_end(pamh, rc);
			return 1;
		}
	}

	/* Resolve the (possibly prompted) user name. */
	{
		const void *u = NULL;
		pam_get_item(pamh, PAM_USER, &u);
		username = (char *)u;
	}
	pw = username ? getpwnam(username) : NULL;
	if (!pw) {
		fprintf(stderr, "login: no passwd entry for %s\n",
		        username ? username : "(null)");
		pam_end(pamh, PAM_SYSTEM_ERR);
		return 1;
	}

	pam_open_session(pamh, 0);
	pam_setcred(pamh, PAM_ESTABLISH_CRED);

	/* Switch credentials: groups, then gid, then uid (never the reverse). */
	if (initgroups(pw->pw_name, pw->pw_gid) != 0)
		fprintf(stderr, "login: initgroups failed\n");
	if (setgid(pw->pw_gid) != 0) {
		fprintf(stderr, "login: cannot set gid\n");
		return 1;
	}
	if (setuid(pw->pw_uid) != 0) {
		fprintf(stderr, "login: cannot set uid\n");
		return 1;
	}

	setlogin(pw->pw_name);

	/* Build a clean login environment. */
	char *envp[8];
	int envn;
	setup_environment(pw, envp, &envn);

	if (chdir(pw->pw_dir) != 0)
		chdir("/");

	/* Exec the shell as a login shell: argv[0] = "-<base>". */
	const char *shell = pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
	const char *base = strrchr(shell, '/');
	base = base ? base + 1 : shell;

	char argv0[64];
	snprintf(argv0, sizeof(argv0), "-%s", base);
	char *sh_argv[] = { argv0, NULL };

	/* pamh is intentionally not pam_end()'d: we hand the session to the
	 * shell.  When the shell exits, login (this process) is gone anyway. */
	execve(shell, sh_argv, envp);
	fprintf(stderr, "login: cannot exec %s\n", shell);
	return 1;
}
