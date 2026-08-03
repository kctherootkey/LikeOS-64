/*
 * init - process 1.
 *
 * Reads /etc/inittab and supervises the programs listed there: starts them at
 * boot, reaps them when they exit, and restarts the ones marked "respawn".
 * Both halves of that matter -- a getty has to come back after every logout,
 * and a daemon that crashes should not leave its service dead until the next
 * reboot.  Orphans reparented to init are reaped as well, and SIGTERM/SIGINT
 * shut the machine down.
 *
 * /etc/inittab lines are
 *
 *	[action] program [arguments...]
 *
 * with blank lines and #-comments ignored.  The action is one of
 *
 *	respawn   restart the program whenever it exits (the default)
 *	once      start it at boot, do not restart it
 *	wait      run it at boot and wait for it to finish before continuing
 *
 * and may be left out, so a bare "sshd -D" line means "respawn sshd -D".  A
 * program name without a slash is resolved through PATH.
 *
 * Services start with stdin/stdout/stderr on /dev/null, because anything they
 * print would otherwise land in the middle of the login prompt -- syslog()
 * writes to stderr here, so sshd's "Server listening on ..." would appear
 * right after "login: ".  An entry that should keep init's console says so:
 *
 *	respawn console /usr/sbin/sshd -D
 *
 * A getty is unaffected either way: it opens its own terminal and puts that on
 * its standard descriptors itself.
 *
 * A program that keeps exiting immediately is held back for a while rather
 * than restarted in a tight loop: without that, one mistyped inittab line
 * would spin a CPU forever.  And if /etc/inittab is missing or has nothing
 * usable in it, init falls back to a built-in getty entry, so a broken config
 * can never leave the machine with no way to log in.
 *
 * Filesystems are assumed already mounted (the root is mounted by the kernel);
 * userspace mounting is a future addition -- a "wait" entry running a mount
 * script is the intended place for it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/reboot.h>

#define INITTAB      "/etc/inittab"
#define GETTY_PATH   "/sbin/getty"
#define CONSOLE      "/dev/console"
#define PATH_DEFAULT "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

/* The system locale.  Its only job is to name a character set: the encoding
 * everything here speaks -- console, terminal emulator, filenames, the network
 * tools -- is UTF-8, and a program that does not see a UTF-8 locale name turns
 * its multibyte handling off and mangles anything above ASCII.  Set from PID 1
 * so that every service inherits it, not only login shells. */
#define LANG_DEFAULT "en_US.UTF-8"

#define MAX_ENTRIES 32
#define MAX_ARGS    15
#define MAX_LINE    256

/* A program started RESPAWN_LIMIT times inside RESPAWN_WINDOW seconds is
 * looping rather than serving; hold it back RESPAWN_HOLD seconds before
 * trying again. */
#define RESPAWN_LIMIT  5
#define RESPAWN_WINDOW 10
#define RESPAWN_HOLD   30

enum action { ACT_RESPAWN, ACT_ONCE, ACT_WAIT };

struct service {
	enum action action;
	int console; /* keep init's terminal instead of /dev/null */
	char line[MAX_LINE]; /* argv[] points into this */
	char *argv[MAX_ARGS + 1];
	pid_t pid; /* 0 when not running */
	int done; /* once/wait entry that has already run */
	time_t hold_until; /* respawn suppressed until this time */
	time_t starts[RESPAWN_LIMIT]; /* ring of recent start times */
	unsigned nstarts;
};

static struct service svc[MAX_ENTRIES];
static int nsvc;

static volatile sig_atomic_t want_poweroff = 0;
static volatile sig_atomic_t want_reboot = 0;

static void on_term(int sig) { (void)sig; want_poweroff = 1; }
static void on_int(int sig)  { (void)sig; want_reboot = 1; }  /* Ctrl-Alt-Del */

/* Split a line into whitespace-separated tokens in place.  A token starting
 * with '#' begins a comment and ends the line. */
static int tokenize(char *s, char **argv, int max)
{
	int n = 0;

	while (n < max) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '\0' || *s == '#')
			break;
		argv[n++] = s;
		while (*s && *s != ' ' && *s != '\t')
			s++;
		if (*s)
			*s++ = '\0';
	}
	argv[n] = NULL;
	return n;
}

static void add_entry(const char *line)
{
	struct service *e;
	char *tok[MAX_ARGS + 1];
	int n, first = 0, i;

	if (nsvc >= MAX_ENTRIES) {
		fprintf(stderr, "init: %s: more than %d entries, ignoring the rest\n",
			INITTAB, MAX_ENTRIES);
		return;
	}

	e = &svc[nsvc];
	memset(e, 0, sizeof(*e));
	strncpy(e->line, line, sizeof(e->line) - 1);

	n = tokenize(e->line, tok, MAX_ARGS);
	if (n == 0)
		return; /* blank or comment-only */

	if (strcmp(tok[0], "respawn") == 0) {
		e->action = ACT_RESPAWN;
		first = 1;
	} else if (strcmp(tok[0], "once") == 0) {
		e->action = ACT_ONCE;
		first = 1;
	} else if (strcmp(tok[0], "wait") == 0) {
		e->action = ACT_WAIT;
		first = 1;
	} else {
		/* No action word: the line is just a command to respawn. */
		e->action = ACT_RESPAWN;
	}

	/* Optional modifier: run this one on init's console rather than
	 * /dev/null, for a service being brought up or debugged. */
	if (first < n && strcmp(tok[first], "console") == 0) {
		e->console = 1;
		first++;
	}

	if (n == first) {
		fprintf(stderr, "init: %s: \"%s\" without a program, ignored\n",
			INITTAB, tok[0]);
		return;
	}

	for (i = first; i < n; i++)
		e->argv[i - first] = tok[i];
	e->argv[n - first] = NULL;
	nsvc++;
}

/* Fall back to a console getty so a missing or unusable inittab can never
 * leave the machine without a way to log in. */
static void add_default_getty(void)
{
	static char def[] = "respawn " GETTY_PATH " " CONSOLE;

	add_entry(def);
}

static void load_inittab(void)
{
	char line[MAX_LINE];
	FILE *f = fopen(INITTAB, "r");

	if (!f) {
		fprintf(stderr, "init: %s: %s, starting a getty on %s\n",
			INITTAB, strerror(errno), CONSOLE);
		add_default_getty();
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		add_entry(line);
	}
	fclose(f);

	if (nsvc == 0) {
		fprintf(stderr, "init: %s: nothing to start, starting a getty on %s\n",
			INITTAB, CONSOLE);
		add_default_getty();
	}
}

/* True once the oldest of the last RESPAWN_LIMIT starts is still inside the
 * window, i.e. the program has been restarted that many times in that time. */
static int respawning_too_fast(const struct service *e, time_t now)
{
	time_t oldest;

	if (e->nstarts < RESPAWN_LIMIT)
		return 0;
	oldest = e->starts[e->nstarts % RESPAWN_LIMIT];
	return now - oldest < RESPAWN_WINDOW;
}

/* Point the standard descriptors at /dev/null so a service cannot scribble
 * over the login prompt.  A getty does not care: it opens its terminal itself
 * and dup2()s that onto 0/1/2 before printing anything. */
static void detach_stdio(void)
{
	int fd = open("/dev/null", O_RDWR);

	if (fd < 0)
		return; /* no better option than keeping what we have */
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
}

/* Returns 1 if the child was forked, 0 if it could not be. */
static int start_service(struct service *e)
{
	pid_t pid = fork();

	if (pid < 0) {
		fprintf(stderr, "init: fork for %s: %s\n", e->argv[0],
			strerror(errno));
		return 0;
	}
	if (pid == 0) {
		/* Hold one handle on init's console across the redirect so a
		 * service that cannot even start still says why somewhere
		 * visible.  FD_CLOEXEC closes it the moment exec succeeds, so
		 * the service itself never inherits a console descriptor. */
		int errfd = 2;

		if (!e->console) {
			errfd = dup(2);
			if (errfd >= 0)
				fcntl(errfd, F_SETFD, FD_CLOEXEC);
			detach_stdio();
		}

		/* No setsid()/setpgid() here on purpose: getty creates its own
		 * session so it can claim the console as a controlling
		 * terminal, and setsid() fails for a process group leader. */
		execvp(e->argv[0], e->argv);
		if (errfd >= 0)
			dprintf(errfd, "init: %s: %s\n", e->argv[0],
				strerror(errno));
		_exit(127);
	}

	e->pid = pid;
	e->starts[e->nstarts % RESPAWN_LIMIT] = time(NULL);
	e->nstarts++;
	return 1;
}

/* Run one boot-time "wait" entry to completion. */
static void run_to_completion(struct service *e)
{
	if (!start_service(e)) {
		e->done = 1;
		return;
	}

	for (;;) {
		int status;
		pid_t w = waitpid(e->pid, &status, 0);

		if (w == e->pid)
			break;
		if (w < 0 && errno == EINTR) {
			/* A shutdown request must not be stuck behind a boot
			 * script that never finishes. */
			if (want_poweroff || want_reboot)
				break;
			continue;
		}
		if (w < 0)
			break;
	}
	e->pid = 0;
	e->done = 1;
}

static void do_shutdown(void)
{
	/* Ask everything to exit first (sshd closes its connections, shells
	 * write out their history), then insist.  kill(-1) reaches every
	 * process except init itself. */
	kill(-1, SIGTERM);
	sleep(1);
	kill(-1, SIGKILL);

	sync();
	if (want_reboot)
		reboot(RB_AUTOBOOT);
	else
		reboot(RB_POWER_OFF);

	/* If reboot returns, there is nothing sensible left to do. */
	for (;;)
		pause();
}

static void supervise(void)
{
	for (;;) {
		int holding = 0, status, i;
		time_t now;
		pid_t w;

		if (want_poweroff || want_reboot)
			do_shutdown();

		/* Start everything that is due to run. */
		now = time(NULL);
		for (i = 0; i < nsvc; i++) {
			struct service *e = &svc[i];

			if (e->pid || e->done || e->action != ACT_RESPAWN)
				continue;
			if (e->hold_until > now) {
				holding = 1;
				continue;
			}
			if (respawning_too_fast(e, now)) {
				fprintf(stderr,
					"init: %s respawning too fast, holding off %d seconds\n",
					e->argv[0], RESPAWN_HOLD);
				e->hold_until = now + RESPAWN_HOLD;
				e->nstarts = 0; /* measure a fresh window after the hold */
				holding = 1;
				continue;
			}
			if (!start_service(e)) {
				e->hold_until = now + RESPAWN_HOLD;
				holding = 1;
			}
		}

		if (holding) {
			/* Something is waiting to be restarted, so the wait has
			 * to be bounded.  Poll rather than arm a timer: a
			 * signal that arrives just before the blocking waitpid
			 * would be lost and the entry would never come back. */
			w = waitpid(-1, &status, WNOHANG);
			if (w <= 0) {
				sleep(1);
				continue;
			}
		} else {
			w = waitpid(-1, &status, 0);
			if (w < 0) {
				if (errno == EINTR)
					continue; /* re-check the shutdown flags */
				/* ECHILD: every entry was a once/wait and has
				 * finished, so there is nothing left to
				 * supervise until a signal arrives. */
				pause();
				continue;
			}
		}

		for (i = 0; i < nsvc; i++) {
			if (svc[i].pid != w)
				continue;
			svc[i].pid = 0;
			if (svc[i].action != ACT_RESPAWN)
				svc[i].done = 1;
			break;
		}
		/* Any other pid was an orphan we adopted; reaping it was all
		 * that was needed. */
	}
}

int main(void)
{
	int i;

	signal(SIGTERM, on_term);
	signal(SIGINT, on_int);

	/* Inherited by every service, and used by execvp() to resolve bare
	 * program names from inittab lines. */
	setenv("PATH", PATH_DEFAULT, 1);
	setenv("LANG", LANG_DEFAULT, 1);

	load_inittab();

	/* Boot-time one-shots first, in the order they appear in the file. */
	for (i = 0; i < nsvc && !want_poweroff && !want_reboot; i++)
		if (svc[i].action == ACT_WAIT)
			run_to_completion(&svc[i]);

	/* Then the one-shot background entries; respawn entries are started by
	 * the supervisor loop, which also restarts them. */
	for (i = 0; i < nsvc; i++)
		if (svc[i].action == ACT_ONCE)
			start_service(&svc[i]);

	supervise();
	return 0; /* not reached */
}
