/*
 * su - run a command with a substitute user and group ID
 *
 * Usage: su [options] [-] [user [argument...]]
 *
 * Authenticates through the PAM framework, switches credentials, sets up the
 * environment and executes the target user's shell.  When invoked by root, no
 * password is required.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <termios.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <security/pam_appl.h>

#define VERSION_STRING "su (LikeOS su) 0.2"
#define DEFAULT_USER   "root"
#define DEFAULT_SHELL  "/bin/sh"
#define DEFAULT_PATH   "/bin:/usr/local/bin"
#define ROOT_PATH      "/sbin:/bin:/usr/local/bin"

extern char **environ;

static char *g_password; /* captured once, replayed to PAM conversation */

/* ---- password prompt (echo off) ---- */
static char *read_password(const char *prompt)
{
	struct termios old, raw;
	int have_tty = (tcgetattr(0, &old) == 0);
	static char buf[256];
	size_t len = 0;

	fputs(prompt, stderr);
	fflush(stderr);
	if (have_tty) {
		raw = old;
		raw.c_lflag &= ~ECHO;
		tcsetattr(0, TCSAFLUSH, &raw);
	}
	for (;;) {
		char ch;
		ssize_t n = read(0, &ch, 1);
		if (n <= 0 || ch == '\n')
			break;
		if (len + 1 < sizeof(buf))
			buf[len++] = ch;
	}
	buf[len] = '\0';
	if (have_tty) {
		tcsetattr(0, TCSAFLUSH, &old);
		fputc('\n', stderr);
	}
	return buf;
}

static int su_conv(int n, const struct pam_message **msg,
                   struct pam_response **resp, void *appdata)
{
	struct pam_response *r;
	int i;
	(void)appdata;
	if (n <= 0)
		return PAM_CONV_ERR;
	r = calloc(n, sizeof(*r));
	if (!r)
		return PAM_BUF_ERR;
	for (i = 0; i < n; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			r[i].resp = strdup(g_password ? g_password
			                              : read_password("Password: "));
			break;
		case PAM_PROMPT_ECHO_ON: {
			char line[256];
			fputs(msg[i]->msg, stderr);
			fflush(stderr);
			if (fgets(line, sizeof(line), stdin)) {
				size_t l = strlen(line);
				if (l && line[l - 1] == '\n')
					line[l - 1] = '\0';
				r[i].resp = strdup(line);
			}
			break;
		}
		case PAM_ERROR_MSG:
			fprintf(stderr, "%s\n", msg[i]->msg);
			break;
		case PAM_TEXT_INFO:
			fprintf(stdout, "%s\n", msg[i]->msg);
			break;
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

static const char *base_name(const char *p)
{
	const char *b = strrchr(p, '/');
	return b ? b + 1 : p;
}

static void usage(int status) __attribute__((noreturn));
static void usage(int status)
{
	FILE *o = status ? stderr : stdout;
	fprintf(o,
	    "Usage: su [options] [-] [user [argument...]]\n\n"
	    "Run commands with a substitute user and group ID.\n\n"
	    "  -c, --command=COMMAND       pass COMMAND to the shell with -c\n"
	    "  -f, --fast                  pass -f to the shell\n"
	    "  -g, --group=GROUP           specify the primary group (root only)\n"
	    "  -G, --supp-group=GROUP      specify a supplementary group (root only)\n"
	    "  -, -l, --login              make the shell a login shell\n"
	    "  -m, -p, --preserve-environment  do not reset the environment\n"
	    "  -P, --pty                   create a new pseudo-terminal\n"
	    "  -s, --shell=SHELL           run SHELL instead of the default\n"
	    "  -w, --whitelist-environment=LIST  keep these variables on --login\n"
	    "      --session-command=COMMAND  like -c but do not create a session\n"
	    "  -h, --help                  display this help\n"
	    "  -V, --version               display version\n");
	exit(status);
}

/* Preserve variables named in a comma-separated whitelist across an env clear. */
struct kept { char *name; char *val; };

int main(int argc, char **argv)
{
	int opt_login = 0, opt_preserve = 0, opt_fast = 0, opt_pty = 0;
	int opt_new_session = 1;
	const char *opt_command = NULL, *opt_shell = NULL;
	const char *opt_group = NULL, *opt_suppgroup = NULL;
	const char *opt_whitelist = NULL;
	int c;

	/* A leading "-" (its own argument) means --login. */
	static struct option long_opts[] = {
		{ "command", required_argument, 0, 'c' },
		{ "session-command", required_argument, 0, 3 },
		{ "fast", no_argument, 0, 'f' },
		{ "group", required_argument, 0, 'g' },
		{ "supp-group", required_argument, 0, 'G' },
		{ "login", no_argument, 0, 'l' },
		{ "preserve-environment", no_argument, 0, 'm' },
		{ "pty", no_argument, 0, 'P' },
		{ "shell", required_argument, 0, 's' },
		{ "whitelist-environment", required_argument, 0, 'w' },
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'V' },
		{ 0, 0, 0, 0 }
	};

	while ((c = getopt_long(argc, argv, "+c:fg:G:lmpPs:w:hV", long_opts,
	                        NULL)) != -1) {
		switch (c) {
		case 'c': opt_command = optarg; break;
		case 3:   opt_command = optarg; opt_new_session = 0; break;
		case 'f': opt_fast = 1; break;
		case 'g': opt_group = optarg; break;
		case 'G': opt_suppgroup = optarg; break;
		case 'l': opt_login = 1; break;
		case 'm':
		case 'p': opt_preserve = 1; break;
		case 'P': opt_pty = 1; break;
		case 's': opt_shell = optarg; break;
		case 'w': opt_whitelist = optarg; break;
		case 'h': usage(0);
		case 'V': printf("%s\n", VERSION_STRING); return 0;
		default: usage(1);
		}
	}

	/* A bare "-" operand also means login shell. */
	if (optind < argc && strcmp(argv[optind], "-") == 0) {
		opt_login = 1;
		optind++;
	}

	const char *target = DEFAULT_USER;
	if (optind < argc)
		target = argv[optind++];

	struct passwd *pw = getpwnam(target);
	if (!pw) {
		fprintf(stderr, "su: user %s does not exist\n", target);
		return 1;
	}

	uid_t caller = getuid();

	if ((opt_group || opt_suppgroup) && caller != 0) {
		fprintf(stderr, "su: only root can specify a group\n");
		return 1;
	}

	/* ---- Authenticate through PAM (root is exempt). ---- */
	struct pam_conv conv = { su_conv, NULL };
	pam_handle_t *pamh = NULL;
	int pr = pam_start("su", pw->pw_name, &conv, &pamh);
	if (pr != PAM_SUCCESS) {
		fprintf(stderr, "su: PAM failure: %s\n", pam_strerror(NULL, pr));
		return 1;
	}
	if (caller != 0) {
		pr = pam_authenticate(pamh, 0);
		if (pr == PAM_SUCCESS)
			pr = pam_acct_mgmt(pamh, 0);
		if (pr != PAM_SUCCESS) {
			fprintf(stderr, "su: Authentication failure\n");
			pam_end(pamh, pr);
			return 1;
		}
	}
	pam_open_session(pamh, 0);

	/* ---- Resolve the shell to run. ---- */
	const char *shell = opt_shell;
	if (!shell && opt_preserve)
		shell = getenv("SHELL");
	if (!shell || !*shell)
		shell = (pw->pw_shell && *pw->pw_shell) ? pw->pw_shell
		                                        : DEFAULT_SHELL;

	/* ---- Determine group ids. ---- */
	gid_t primary = pw->pw_gid;
	gid_t supp[NGROUPS_MAX];
	int nsupp = -1; /* -1 = use initgroups */
	if (opt_group) {
		struct group *g = getgrnam(opt_group);
		if (!g) {
			char *e;
			long v = strtol(opt_group, &e, 10);
			if (*e == '\0')
				g = getgrgid((gid_t)v);
		}
		if (!g) {
			fprintf(stderr, "su: group %s does not exist\n", opt_group);
			return 1;
		}
		primary = g->gr_gid;
	}
	if (opt_suppgroup) {
		struct group *g = getgrnam(opt_suppgroup);
		if (!g) {
			fprintf(stderr, "su: group %s does not exist\n",
			        opt_suppgroup);
			return 1;
		}
		nsupp = 0;
		supp[nsupp++] = g->gr_gid;
		if (!opt_group)
			primary = g->gr_gid;
	}

	/* ---- Build the environment BEFORE dropping privileges. ---- */
	char home[256], shellenv[256], userenv[128], lognameenv[128];
	snprintf(home, sizeof(home), "%s", pw->pw_dir);

	if (opt_login) {
		/* Clear the environment, keeping TERM and any whitelist. */
		char *term = getenv("TERM");
		char *termcopy = term ? strdup(term) : NULL;

		struct kept keep[32];
		int nkeep = 0;
		if (opt_whitelist) {
			char *list = strdup(opt_whitelist), *save = list, *tok;
			while ((tok = strsep(&save, ",")) && nkeep < 32) {
				if (!*tok)
					continue;
				char *v = getenv(tok);
				if (v) {
					keep[nkeep].name = strdup(tok);
					keep[nkeep].val = strdup(v);
					nkeep++;
				}
			}
			free(list);
		}
		clearenv();
		if (termcopy)
			setenv("TERM", termcopy, 1);
		for (int i = 0; i < nkeep; i++)
			setenv(keep[i].name, keep[i].val, 1);
		setenv("HOME", pw->pw_dir, 1);
		setenv("SHELL", shell, 1);
		setenv("USER", pw->pw_name, 1);
		setenv("LOGNAME", pw->pw_name, 1);
		setenv("PATH", pw->pw_uid == 0 ? ROOT_PATH : DEFAULT_PATH, 1);
	} else if (!opt_preserve) {
		/* Keep the environment but set HOME/SHELL (and USER/LOGNAME for
		 * a non-root target). */
		setenv("HOME", pw->pw_dir, 1);
		setenv("SHELL", shell, 1);
		if (pw->pw_uid != 0) {
			setenv("USER", pw->pw_name, 1);
			setenv("LOGNAME", pw->pw_name, 1);
		}
	}
	(void)home; (void)shellenv; (void)userenv; (void)lognameenv;

	/* ---- Switch credentials: groups, gid, uid (never reversed). ---- */
	if (nsupp >= 0) {
		if (setgroups(nsupp, (int *)supp) != 0)
			fprintf(stderr, "su: setgroups: %s\n", strerror(errno));
	} else {
		if (initgroups(pw->pw_name, primary) != 0)
			fprintf(stderr, "su: initgroups: %s\n", strerror(errno));
	}
	if (setgid(primary) != 0) {
		fprintf(stderr, "su: cannot set group id: %s\n", strerror(errno));
		return 1;
	}
	if (setuid(pw->pw_uid) != 0) {
		fprintf(stderr, "su: cannot set user id: %s\n", strerror(errno));
		return 1;
	}

	if (opt_login && chdir(pw->pw_dir) != 0) {
		fprintf(stderr, "su: warning: cannot change directory to %s: %s\n",
		        pw->pw_dir, strerror(errno));
		chdir("/");
	}

	/* ---- Assemble the shell argv. ---- */
	char argv0[64];
	if (opt_login)
		snprintf(argv0, sizeof(argv0), "-%s", base_name(shell));
	else
		snprintf(argv0, sizeof(argv0), "%s", base_name(shell));

	char *sh_argv[8];
	int a = 0;
	sh_argv[a++] = argv0;
	if (opt_fast)
		sh_argv[a++] = (char *)"-f";
	if (opt_command) {
		sh_argv[a++] = (char *)"-c";
		sh_argv[a++] = (char *)opt_command;
	}
	/* Extra operands after the user name are passed to the shell. */
	while (optind < argc && a < 7)
		sh_argv[a++] = argv[optind++];
	sh_argv[a] = NULL;

	/* ---- --pty: run the shell on a fresh pseudo-terminal and proxy. ---- */
	if (opt_pty) {
		int master = posix_openpt(O_RDWR);
		if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
			fprintf(stderr, "su: cannot allocate pty: %s\n",
			        strerror(errno));
			return 1;
		}
		char *slavename = ptsname(master);
		pid_t pid = fork();
		if (pid < 0) {
			fprintf(stderr, "su: fork: %s\n", strerror(errno));
			return 1;
		}
		if (pid == 0) {
			int slave = open(slavename, O_RDWR);
			if (opt_new_session)
				setsid();
			ioctl(slave, TIOCSCTTY, 0);
			dup2(slave, 0);
			dup2(slave, 1);
			dup2(slave, 2);
			if (slave > 2)
				close(slave);
			close(master);
			execve(shell, sh_argv, environ);
			fprintf(stderr, "su: cannot run %s: %s\n", shell,
			        strerror(errno));
			_exit(127);
		}
		/* Parent: shuttle bytes between our terminal and the pty. */
		struct termios raw, save;
		int rawok = (tcgetattr(0, &save) == 0);
		if (rawok) {
			raw = save;
			raw.c_lflag &= ~(ICANON | ECHO);
			tcsetattr(0, TCSANOW, &raw);
		}
		for (;;) {
			char b[512];
			int done = 0;
			ssize_t n = read(master, b, sizeof(b));
			if (n > 0)
				write(1, b, n);
			else
				done = 1;
			int st;
			if (waitpid(pid, &st, WNOHANG) == pid)
				done = 1;
			if (done)
				break;
			n = read(0, b, sizeof(b));
			if (n > 0)
				write(master, b, n);
		}
		if (rawok)
			tcsetattr(0, TCSANOW, &save);
		int status = 0;
		waitpid(pid, &status, 0);
		pam_close_session(pamh, 0);
		pam_end(pamh, PAM_SUCCESS);
		return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
	}

	if (opt_new_session)
		setsid();

	/* No PAM close here: we hand the session to the shell via exec. */
	execve(shell, sh_argv, environ);
	fprintf(stderr, "su: cannot run %s: %s\n", shell, strerror(errno));
	return 127;
}
