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
 * Before any of that, init sets the machine up from /etc:
 *
 *	/proc/cmdline       likeos.mode=install (the installation medium's
 *	                    "Install LikeOS" boot entry) makes init read
 *	                    /etc/inittab.install instead of /etc/inittab and
 *	                    tells every program through LIKEOS_MODE=install
 *	/etc/locale.conf    LANG= and LC_*= for every program
 *	/etc/hostname       the host name (the kernel's own when absent)
 *	/etc/fstab          the filesystems to mount: the root is mounted by
 *	                    the kernel already (and only remounted when its
 *	                    line asks for other flags); every other line that
 *	                    is not noauto or swap is checked by its fsck pass
 *	                    and mounted, the way "mount -a" would
 *
 * Every step there is bounded in time: a device that never appears, a
 * checker that never finishes or a mount that hangs is given up on with a
 * message on the console, and the boot goes on.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <pwd.h>

#include "fstab-util.h"

#define INITTAB      "/etc/inittab"
#define INITTAB_INSTALL "/etc/inittab.install"
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

/* The inittab in use: INITTAB, or INITTAB_INSTALL in installer mode. */
static const char *inittab = INITTAB;

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
			inittab, MAX_ENTRIES);
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
			inittab, tok[0]);
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
	FILE *f = fopen(inittab, "r");

	if (!f) {
		fprintf(stderr, "init: %s: %s, starting a getty on %s\n",
			inittab, strerror(errno), CONSOLE);
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
			inittab, CONSOLE);
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

/* The zone name from /etc/timezone, as TZ, for every process on the system.
 *
 * libc reads that file itself, so its clock is right without this.  ICU does
 * not: on a system it does not recognise it consults nothing but $TZ, and
 * without it falls back to a fixed-offset zone with no daylight-saving rule.
 * A browser then has two clocks -- Date from libc, Intl from ICU -- that
 * disagree by an hour half the year, which web pages notice.  The login
 * profile exports TZ for login shells; this covers what does not descend from
 * one, and is inherited by everything init starts. */
static void export_timezone(void)
{
	char zone[64];
	FILE *f;

	if (getenv("TZ"))
		return;
	f = fopen("/etc/timezone", "r");
	if (!f)
		return;
	if (fgets(zone, sizeof(zone), f)) {
		zone[strcspn(zone, "\r\n")] = '\0';
		if (zone[0])
			setenv("TZ", zone, 0);
	}
	fclose(f);
}

/* The machine id: 128 random bits naming this installation, written once
 * as 32 lower-case hex digits to /etc/machine-id and kept for good.  D-Bus
 * refuses to start a bus without one, and it has to be made by root: an
 * account's first `startx' cannot write /etc or /var/lib.  So it is made
 * here, at the first boot, as the image ships none.  An id D-Bus already
 * recorded in /var/lib/dbus/machine-id is taken over rather than replaced,
 * and that file becomes a link to /etc/machine-id, so there is one id. */
#define MACHINE_ID      "/etc/machine-id"
#define MACHINE_ID_TMP  "/etc/machine-id.tmp"
#define DBUS_DIR        "/var/lib/dbus"
#define DBUS_MACHINE_ID DBUS_DIR "/machine-id"

/* 32 lower-case hex digits and a newline, or nothing else. */
static int read_machine_id(const char *path, char id[33])
{
	char buf[40];
	ssize_t n;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n != 33 || buf[32] != '\n')
		return -1;
	for (int i = 0; i < 32; i++)
		if (!((buf[i] >= '0' && buf[i] <= '9') ||
		      (buf[i] >= 'a' && buf[i] <= 'f')))
			return -1;
	/* All zeros is "no id", not an id. */
	if (strspn(buf, "0") == 32)
		return -1;
	memcpy(id, buf, 32);
	id[32] = '\0';
	return 0;
}

static int new_machine_id(char id[33])
{
	static const char hex[] = "0123456789abcdef";
	unsigned char b[16];
	ssize_t n;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof(b));
	close(fd);
	if (n != (ssize_t)sizeof(b))
		return -1;
	/* Formatted as a version 4 (random) UUID, like every other id of
	 * its kind. */
	b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
	b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
	for (int i = 0; i < 16; i++) {
		id[2 * i] = hex[b[i] >> 4];
		id[2 * i + 1] = hex[b[i] & 15];
	}
	id[32] = '\0';
	return 0;
}

static int write_machine_id(const char id[33])
{
	char line[33];
	int fd;

	memcpy(line, id, 32);
	line[32] = '\n';
	unlink(MACHINE_ID_TMP);
	fd = open(MACHINE_ID_TMP, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
		  0444);
	if (fd < 0)
		return -1;
	if (write(fd, line, sizeof(line)) != (ssize_t)sizeof(line) ||
	    fsync(fd) != 0) {
		close(fd);
		unlink(MACHINE_ID_TMP);
		return -1;
	}
	close(fd);
	if (rename(MACHINE_ID_TMP, MACHINE_ID) != 0) {
		unlink(MACHINE_ID_TMP);
		return -1;
	}
	return 0;
}

static void setup_machine_id(void)
{
	char id[33];
	struct stat st;

	if (read_machine_id(MACHINE_ID, id) != 0) {
		if ((lstat(DBUS_MACHINE_ID, &st) != 0 || !S_ISREG(st.st_mode) ||
		     read_machine_id(DBUS_MACHINE_ID, id) != 0) &&
		    new_machine_id(id) != 0) {
			fprintf(stderr, "init: no random bytes for %s\n",
				MACHINE_ID);
			return;
		}
		if (write_machine_id(id) != 0) {
			fprintf(stderr, "init: cannot write %s: %s\n",
				MACHINE_ID, strerror(errno));
			return;
		}
	}
	if (mkdir(DBUS_DIR, 0755) != 0 && errno != EEXIST)
		return;
	/* Not already the link, and not a different id of its own: D-Bus
	 * must see the same id as everything reading /etc/machine-id. */
	if (lstat(DBUS_MACHINE_ID, &st) == 0) {
		char cur[33];

		if (S_ISLNK(st.st_mode) ||
		    (read_machine_id(DBUS_MACHINE_ID, cur) == 0 &&
		     strcmp(cur, id) == 0))
			return;
		unlink(DBUS_MACHINE_ID);
	}
	if (symlink(MACHINE_ID, DBUS_MACHINE_ID) != 0)
		fprintf(stderr, "init: cannot link %s: %s\n", DBUS_MACHINE_ID,
			strerror(errno));
}

/* The console keymap of the machine's keyboard setting, loaded before the
 * first getty so that the login prompt already types what the keyboard says.
 *
 * /etc/likeos/keyboard.conf names XKB layouts (LAYOUT=de,us, VARIANT=...);
 * the console uses the first one, and the layout catalogue says which
 * console keymap that is.  Nothing is loaded when the file is missing or
 * names the US layout: the kernel's built-in keymap is the US one.  The
 * keymap loader is given a few seconds and then abandoned -- a keyboard
 * setting must never be what keeps the machine from reaching a login
 * prompt, and the built-in keymap still types. */
#define KEYBOARD_CONF      "/etc/likeos/keyboard.conf"
#define KEYBOARD_CATALOGUE "/usr/share/likeos/keyboard/layouts.tsv"
#define LOADKEYS_PATH      "/bin/loadkeys"
#define LOADKEYS_TIMEOUT_MS 10000

/* The first item of a "KEY=a,b,c" value, without quotes or blanks. */
static void first_item(const char *value, char *out, size_t cap)
{
	size_t n = 0;

	while (*value == ' ' || *value == '\t' || *value == '"' ||
	       *value == '\'')
		value++;
	while (value[n] && strchr(",\"' \t\r\n#", value[n]) == NULL)
		n++;
	if (n >= cap)
		n = 0;
	memcpy(out, value, n);
	out[n] = '\0';
}

/* Field `idx' of a TAB-separated catalogue line. */
static int catalogue_field(const char *line, int idx, char *out, size_t cap)
{
	const char *p = line;
	size_t n;

	for (int i = 0; i < idx; i++) {
		p = strchr(p, '\t');
		if (!p)
			return 0;
		p++;
	}
	n = strcspn(p, "\t\r\n");
	if (n >= cap)
		return 0;
	memcpy(out, p, n);
	out[n] = '\0';
	return 1;
}

static void load_console_keymap(void)
{
	char line[512], layout[64] = "", variant[64] = "", id[64] = "";
	char argv_id[64];
	char *argv[5];
	FILE *f;
	pid_t pid;
	int waited = 0;

	f = fopen(KEYBOARD_CONF, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "LAYOUT=", 7) == 0)
			first_item(line + 7, layout, sizeof(layout));
		else if (strncmp(line, "VARIANT=", 8) == 0)
			first_item(line + 8, variant, sizeof(variant));
	}
	fclose(f);
	if (!layout[0] || (strcmp(layout, "us") == 0 && !variant[0]))
		return;

	f = fopen(KEYBOARD_CATALOGUE, "r");
	if (!f) {
		fprintf(stderr, "init: %s: %s\n", KEYBOARD_CATALOGUE,
			strerror(errno));
		return;
	}
	while (!id[0] && fgets(line, sizeof(line), f)) {
		char lid[64], llay[64], lvar[64];

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (catalogue_field(line, 0, lid, sizeof(lid)) &&
		    catalogue_field(line, 3, llay, sizeof(llay)) &&
		    catalogue_field(line, 4, lvar, sizeof(lvar)) &&
		    strcmp(llay, layout) == 0 && strcmp(lvar, variant) == 0)
			memcpy(id, lid, sizeof(id));
	}
	fclose(f);
	if (!id[0]) {
		fprintf(stderr, "init: %s: layout %s%s%s%s is not in %s\n",
			KEYBOARD_CONF, layout, variant[0] ? "(" : "", variant,
			variant[0] ? ")" : "", KEYBOARD_CATALOGUE);
		return;
	}
	if (strcmp(id, "us") == 0)
		return;

	memcpy(argv_id, id, sizeof(argv_id));
	argv[0] = "loadkeys";
	argv[1] = "-q";
	argv[2] = "--no-save";
	argv[3] = argv_id;
	argv[4] = NULL;
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "init: fork for %s: %s\n", LOADKEYS_PATH,
			strerror(errno));
		return;
	}
	if (pid == 0) {
		execv(LOADKEYS_PATH, argv);
		fprintf(stderr, "init: %s: %s\n", LOADKEYS_PATH,
			strerror(errno));
		_exit(127);
	}
	for (;;) {
		int status;
		pid_t w = waitpid(pid, &status, WNOHANG);

		if (w == pid) {
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
				fprintf(stderr, "init: loading the console "
					"keymap %s failed\n", id);
			return;
		}
		if (w < 0 && errno != EINTR)
			return;
		if (waited >= LOADKEYS_TIMEOUT_MS) {
			fprintf(stderr, "init: %s %s did not finish; "
				"keeping the built-in keymap\n",
				LOADKEYS_PATH, id);
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return;
		}
		usleep(50 * 1000);
		waited += 50;
	}
}

/* ---- the kernel command line ---------------------------------------------- */

/* likeos.mode=install: the installation medium was started with its
 * "Install LikeOS" entry.  init then runs /etc/inittab.install, which starts
 * the installer instead of a login prompt, and every program it starts sees
 * LIKEOS_MODE=install.  Anything else -- no mode, another mode, no
 * /proc/cmdline -- is the normal system. */
#define CMDLINE      "/proc/cmdline"
#define MODE_KEY     "likeos.mode="

static int install_mode;

static void read_cmdline(void)
{
	char buf[2048];
	ssize_t n;
	char *p;
	int fd = open(CMDLINE, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = '\0';
	p = buf;
	for (;;) {
		char *word, *end;
		const char *val;

		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (!*p)
			break;
		word = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		end = p;
		if (*p)
			p++;
		*end = '\0';
		if (strncmp(word, MODE_KEY, sizeof(MODE_KEY) - 1) != 0)
			continue;
		val = word + sizeof(MODE_KEY) - 1;
		/* The last one given counts, as with every kernel option. */
		install_mode = strcmp(val, "install") == 0 ||
			       strcmp(val, "\"install\"") == 0;
	}
	if (!install_mode)
		return;
	setenv("LIKEOS_MODE", "install", 1);
	if (access(INITTAB_INSTALL, R_OK) == 0) {
		inittab = INITTAB_INSTALL;
		fprintf(stderr, "init: installation mode, using %s\n",
			INITTAB_INSTALL);
	} else {
		fprintf(stderr, "init: installation mode, but %s: %s; using %s\n",
			INITTAB_INSTALL, strerror(errno), INITTAB);
	}
}

/* ---- /etc/locale.conf ----------------------------------------------------- */

/* The language and the formats of the system, as LANG= and LC_*= lines
 * (values optionally in quotes, blanks around the key and the value
 * ignored), exported to every program init starts.
 * Only LANG replaces a default: the LANG_DEFAULT above stays when the file
 * sets none, so the character set is UTF-8 whatever it says.  LC_ALL is
 * not taken from here -- it overrides every other setting and is meant
 * for a single command, not for the whole system. */
#define LOCALE_CONF "/etc/locale.conf"

static const char *const locale_keys[] = {
	"LANG", "LANGUAGE", "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE",
	"LC_MONETARY", "LC_MESSAGES", "LC_PAPER", "LC_NAME", "LC_ADDRESS",
	"LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION", NULL,
};

/* A locale name: letters, digits and _.@:- only, like en_US.UTF-8 or
 * de_DE@euro (LANGUAGE also takes a colon-separated list). */
static int locale_value_ok(const char *v)
{
	size_t n = strlen(v);

	if (n == 0 || n > 64)
		return 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)v[i];

		if (!isalnum(c) && !strchr("_.@:-", c))
			return 0;
	}
	return 1;
}

static void load_locale_conf(void)
{
	char line[256];
	int lineno = 0;
	FILE *f = fopen(LOCALE_CONF, "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *key, *val, *eq;
		size_t n;
		int known = 0;

		lineno++;
		p[strcspn(p, "\r\n")] = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p || *p == '#')
			continue;
		if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t'))
			p += 7;
		eq = strchr(p, '=');
		if (!eq) {
			fprintf(stderr, "init: %s:%d: not a KEY=value line\n",
				LOCALE_CONF, lineno);
			continue;
		}
		*eq = '\0';
		/* Blanks around the key and the value do not count
		 * ("LANG = en_US.UTF-8" is LANG=en_US.UTF-8). */
		key = p;
		while (*key == ' ' || *key == '\t')
			key++;
		n = strlen(key);
		while (n > 0 && (key[n - 1] == ' ' || key[n - 1] == '\t'))
			key[--n] = '\0';
		val = eq + 1;
		while (*val == ' ' || *val == '\t')
			val++;
		n = strlen(val);
		while (n > 0 && (val[n - 1] == ' ' || val[n - 1] == '\t'))
			val[--n] = '\0';
		if (n >= 2 && (val[0] == '"' || val[0] == '\'') &&
		    val[n - 1] == val[0]) {
			val[n - 1] = '\0';
			val++;
		}
		for (int i = 0; locale_keys[i]; i++)
			if (strcmp(key, locale_keys[i]) == 0)
				known = 1;
		if (!known) {
			fprintf(stderr, "init: %s:%d: %s is not a locale "
				"setting, ignored\n", LOCALE_CONF, lineno, key);
			continue;
		}
		if (!val[0]) {
			/* An empty value means "not set". */
			if (strcmp(key, "LANG") != 0)
				unsetenv(key);
			continue;
		}
		if (!locale_value_ok(val)) {
			fprintf(stderr, "init: %s:%d: \"%s\" is not a locale "
				"name, %s left as it is\n", LOCALE_CONF, lineno,
				val, key);
			continue;
		}
		setenv(key, val, 1);
	}
	fclose(f);
}

/* ---- /etc/hostname ---------------------------------------------------------- */

/* The machine's name, one line, set with sethostname() before any program
 * that prints or sends it starts.  It must be a valid host name (RFC 1123:
 * dot-separated labels of letters, digits and inner hyphens, each 1 to 63
 * characters) of at most 63 characters, which is all the kernel keeps.
 * Without the file, or with a name that is not one, the kernel's own name
 * stays. */
#define HOSTNAME_FILE "/etc/hostname"
#define HOSTNAME_MAX  63

static int hostname_ok(const char *name)
{
	size_t n = strlen(name), label = 0;

	if (n == 0 || n > HOSTNAME_MAX)
		return 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];

		if (c == '.') {
			/* no empty label, none ending in a hyphen */
			if (label == 0 || name[i - 1] == '-')
				return 0;
			label = 0;
			continue;
		}
		if (!isalnum(c) && c != '-')
			return 0;
		if (c == '-' && label == 0)
			return 0; /* a label starts with a letter or digit */
		if (++label > 63)
			return 0;
	}
	return label > 0 && name[n - 1] != '-';
}

static void setup_hostname(void)
{
	char line[256], cur[HOSTNAME_MAX + 2] = "";
	char *name = NULL;
	FILE *f = fopen(HOSTNAME_FILE, "r");

	if (gethostname(cur, sizeof(cur)) != 0)
		cur[0] = '\0';
	cur[sizeof(cur) - 1] = '\0';
	if (!f) {
		if (errno != ENOENT)
			fprintf(stderr, "init: %s: %s; the host name stays "
				"%s\n", HOSTNAME_FILE, strerror(errno), cur);
		return;
	}
	/* The first line that is not empty or a comment. */
	while (!name && fgets(line, sizeof(line), f)) {
		char *p = line, *e;

		while (*p == ' ' || *p == '\t')
			p++;
		e = p + strlen(p);
		while (e > p && (e[-1] == '\n' || e[-1] == '\r' ||
				 e[-1] == ' ' || e[-1] == '\t'))
			*--e = '\0';
		if (*p && *p != '#')
			name = p;
	}
	fclose(f);
	if (!name) {
		fprintf(stderr, "init: %s is empty; the host name stays %s\n",
			HOSTNAME_FILE, cur);
		return;
	}
	if (!hostname_ok(name)) {
		fprintf(stderr, "init: %s: \"%s\" is not a valid host name "
			"(letters, digits and hyphens, at most %d characters); "
			"the host name stays %s\n", HOSTNAME_FILE, name,
			HOSTNAME_MAX, cur);
		return;
	}
	if (strcmp(name, cur) == 0)
		return;
	if (sethostname(name, strlen(name)) != 0) {
		fprintf(stderr, "init: setting the host name to %s: %s\n", name,
			strerror(errno));
		return;
	}
	fprintf(stderr, "init: host name %s\n", name);
}

/* ---- /etc/fstab ---------------------------------------------------------------- */

/* The filesystems of /etc/fstab, mounted the way "mount -a" would, before
 * anything in inittab runs:
 *
 *   1. the root line (mount point "/") is not mounted -- the kernel did
 *      that -- but when its options name other flags than the root has
 *      (ro where it is rw, noatime, ...) the root is remounted with them;
 *   2. noauto and swap lines, and mount points something is mounted on
 *      already, are passed over;
 *   3. the devices are looked up: UUID=, LABEL=, PARTUUID= and PARTLABEL=
 *      through /dev/disk/by-*, and every device is waited for up to
 *      FSTAB_DEVICE_WAIT_MS (USB and NVMe disks come up after init starts);
 *   4. the filesystems with a pass number above 0 are checked, pass 1
 *      before pass 2 and so on, by /sbin/fsck.<type> -p (fsck.vfat -a for
 *      FAT).  0 and 1 are fine, 2 asks for a reboot (said on the console,
 *      and the boot goes on), 4 and above mean errors were left: the
 *      filesystem is then mounted read-only;
 *   5. the lines are mounted in the order of the file, the mount point
 *      made first when it is missing.
 *
 * Every wait is bounded, and a failure costs that one filesystem, never
 * the boot. */
#define FSTAB_DEVICE_WAIT_MS 10000
#define FSCK_TIMEOUT_MS      120000
#define FSCK_TERM_GRACE_MS   10000
#define MOUNT_TIMEOUT_MS     60000

/* What waiting for a child came to. */
#define CHILD_TIMEOUT  (-2) /* still running when the time ran out */
#define CHILD_NOSTART  (-1) /* could not be started */

struct fs_line {
	const struct fsu_entry *ent;
	struct fsu_opts opts;
	char dev[FSU_PATH_MAX]; /* what mount(2) is given as the source */
	char dir[FSU_PATH_MAX]; /* the canonical mount point */
	int is_dev;
	int skip;
	int read_only; /* forced: the check left errors */
};

/* Wait up to `timeout_ms' for `pid'.  The exit status (waitpid's) in
 * *status, 0; CHILD_TIMEOUT when it did not end in time.  A shutdown
 * request ends the wait early, as a timeout. */
static int wait_child(pid_t pid, int timeout_ms, int *status)
{
	int waited = 0;

	for (;;) {
		pid_t w = waitpid(pid, status, WNOHANG);

		if (w == pid)
			return 0;
		if (w < 0 && errno != EINTR)
			return CHILD_NOSTART;
		if (waited >= timeout_ms || want_poweroff || want_reboot)
			return CHILD_TIMEOUT;
		usleep(50 * 1000);
		waited += 50;
	}
}

/* Stop a child that overran: SIGTERM, a grace period, then SIGKILL.  A
 * child that does not go even then (stuck in the kernel) is left to the
 * supervisor loop, which reaps whatever ends later. */
static void stop_child(pid_t pid, int grace_ms)
{
	int status;

	kill(pid, SIGTERM);
	if (wait_child(pid, grace_ms, &status) == 0)
		return;
	kill(pid, SIGKILL);
	wait_child(pid, 2000, &status);
}

/* Run a checker: stdin on /dev/null (it must never wait for an answer),
 * stdout and stderr on init's console so its report is seen.  Returns its
 * exit code, CHILD_NOSTART, CHILD_TIMEOUT, or 128 + the signal that
 * killed it. */
static int run_fsck(const char *prog, const char *arg, const char *dev)
{
	char *argv[4];
	char aprog[FSU_PATH_MAX], aarg[8], adev[FSU_PATH_MAX];
	pid_t pid;
	int status, rc;

	snprintf(aprog, sizeof(aprog), "%s", prog);
	snprintf(aarg, sizeof(aarg), "%s", arg);
	snprintf(adev, sizeof(adev), "%s", dev);
	argv[0] = aprog;
	argv[1] = aarg;
	argv[2] = adev;
	argv[3] = NULL;
	fflush(stderr);
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "init: fork for %s: %s\n", prog,
			strerror(errno));
		return CHILD_NOSTART;
	}
	if (pid == 0) {
		int fd = open("/dev/null", O_RDONLY);

		if (fd >= 0 && fd != 0) {
			dup2(fd, 0);
			close(fd);
		}
		execv(prog, argv);
		fprintf(stderr, "init: %s: %s\n", prog, strerror(errno));
		_exit(127);
	}
	rc = wait_child(pid, FSCK_TIMEOUT_MS, &status);
	if (rc == CHILD_TIMEOUT) {
		fprintf(stderr, "init: %s %s did not finish within %d seconds; "
			"stopping it\n", prog, dev, FSCK_TIMEOUT_MS / 1000);
		stop_child(pid, FSCK_TERM_GRACE_MS);
		return CHILD_TIMEOUT;
	}
	if (rc != 0)
		return CHILD_NOSTART;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return CHILD_NOSTART;
}

/* mount(2) in a child, so a mount that never returns (a device that stopped
 * answering) cannot hold PID 1: 0 or an errno value; ETIMEDOUT when the
 * child did not finish in time. */
static int bounded_mount(const char *src, const char *dir, const char *type,
			 unsigned long flags, const char *data)
{
	pid_t pid;
	int status, rc;

	fflush(stderr);
	pid = fork();
	if (pid < 0)
		return errno;
	if (pid == 0) {
		if (mount(src, dir, type, flags,
			  data && data[0] ? data : NULL) == 0)
			_exit(0);
		_exit(errno > 0 && errno < 256 ? errno : EIO);
	}
	rc = wait_child(pid, MOUNT_TIMEOUT_MS, &status);
	if (rc == CHILD_TIMEOUT) {
		/* Not killed: a mount is not interruptible.  It ends -- or
		 * not -- on its own, and the supervisor reaps it then. */
		return ETIMEDOUT;
	}
	if (rc != 0 || !WIFEXITED(status))
		return EIO;
	return WEXITSTATUS(status);
}

/* The current options of what is mounted at `dir' (from /proc/mounts),
 * NULL when nothing is. */
static const struct fsu_entry *mounted_at(const struct fsu_table *mounts,
					  const char *dir)
{
	return fsu_table_find_dir(mounts, dir);
}

/* Check one filesystem; says on the console what came of it.  Returns 1
 * when it has to be mounted read-only. */
static int check_filesystem(struct fs_line *l)
{
	char prog[FSU_PATH_MAX];
	const char *arg = "-p";
	int rc;

	if (fsu_fsck_program(l->ent->type, prog, sizeof(prog), &arg) != 0) {
		fprintf(stderr, "init: %s: no checker for %s filesystems, not "
			"checked\n", l->dir, l->ent->type);
		return 0;
	}
	fprintf(stderr, "init: checking %s (%s) for %s\n", l->dev,
		l->ent->type, l->dir);
	rc = run_fsck(prog, arg, l->dev);
	if (rc == CHILD_NOSTART || rc == 127) {
		fprintf(stderr, "init: %s could not be run for %s; mounting it "
			"unchecked\n", prog, l->dev);
		return 0;
	}
	if (rc == CHILD_TIMEOUT) {
		fprintf(stderr, "init: *** the check of %s was stopped; %s is "
			"mounted READ-ONLY.  Check it by hand: %s %s\n", l->dev,
			l->dir, prog, l->dev);
		return 1;
	}
	if (fsu_type_is_fat(l->ent->type)) {
		/* fsck.fat: 0 clean, 1 errors (repaired with -a), 2 usage */
		if (rc == 0 || rc == 1) {
			if (rc == 1)
				fprintf(stderr, "init: %s: errors were "
					"repaired\n", l->dev);
			return 0;
		}
		if (rc == 2) {
			fprintf(stderr, "init: %s did not accept its arguments "
				"for %s; mounting it unchecked\n", prog, l->dev);
			return 0;
		}
	} else {
		/* fsck: 1 errors corrected, 2 reboot needed, 4 errors left,
		 * 8 operational error, 16 usage, 32 cancelled */
		if (rc == 0 || rc == 1) {
			if (rc == 1)
				fprintf(stderr, "init: %s: errors were "
					"corrected\n", l->dev);
			return 0;
		}
		if (rc == 2 || rc == 3) {
			fprintf(stderr, "init: %s: errors were corrected; the "
				"system should be restarted\n", l->dev);
			return 0;
		}
	}
	fprintf(stderr, "init: *** %s: the check found errors it could not "
		"correct (%s exit %d); %s is mounted READ-ONLY.  Repair it by "
		"hand: %s %s\n", l->dev, prog, rc, l->dir, prog, l->dev);
	return 1;
}

/* The root line: remount when it asks for other flags than the root has. */
static void fstab_root(const struct fsu_entry *ent,
		       const struct fsu_table *mounts)
{
	const struct fsu_entry *cur = mounted_at(mounts, "/");
	struct fsu_opts o;
	unsigned long have, want;
	char hw[160], ww[160];
	int rc;

	if (!cur)
		return;
	fsu_opts_init(&o);
	if (fsu_opts_parse(&o, ent->opts) != 0) {
		fprintf(stderr, "init: %s: the options of / are too long, "
			"ignored\n", FSU_FSTAB);
		return;
	}
	have = fsu_flags_of(cur->opts);
	want = o.flags & FSU_FLAG_MASK;
	if (have == want)
		return;
	/* A root mounted read-only is checked before it becomes writable
	 * (a root mounted read-write cannot be checked at all). */
	if ((have & MS_RDONLY) && !(want & MS_RDONLY) && ent->passno > 0 &&
	    fsu_spec_is_device(cur->spec)) {
		struct fs_line l;

		memset(&l, 0, sizeof(l));
		l.ent = ent;
		snprintf(l.dir, sizeof(l.dir), "/");
		if (fsu_resolve(cur->spec, l.dev, sizeof(l.dev)) == 0 ||
		    fsu_resolve(ent->spec, l.dev, sizeof(l.dev)) == 0) {
			char prog[FSU_PATH_MAX];
			const char *arg;

			if (fsu_fsck_program(ent->type, prog, sizeof(prog),
					     &arg) == 0) {
				fprintf(stderr, "init: checking the root "
					"filesystem %s\n", l.dev);
				rc = run_fsck(prog, arg, l.dev);
				if (rc == 2 || rc == 3) {
					/* Repaired while mounted: what the
					 * kernel holds of it is stale. */
					fprintf(stderr, "init: the root "
						"filesystem was repaired; "
						"restarting\n");
					sync();
					reboot(RB_AUTOBOOT);
				}
				if ((rc >= 4 && rc != 127) ||
				    rc == CHILD_TIMEOUT) {
					fprintf(stderr, "init: *** the root "
						"filesystem %s has errors "
						"the check could not "
						"correct; it stays "
						"READ-ONLY.  Repair it by "
						"hand: %s %s\n", l.dev, prog,
						l.dev);
					want |= MS_RDONLY;
					if (have == want)
						return;
				}
			}
		}
	}
	fsu_flags_text(have, hw, sizeof(hw));
	fsu_flags_text(want, ww, sizeof(ww));
	fprintf(stderr, "init: remounting / %s (was %s)\n", ww, hw);
	rc = bounded_mount(cur->spec, "/", NULL, MS_REMOUNT | want, o.data);
	if (rc != 0)
		fprintf(stderr, "init: remounting /: %s\n", strerror(rc));
}

static void mount_line(struct fs_line *l)
{
	unsigned long flags = l->opts.flags;
	struct stat st;
	char words[160];
	int rc;

	if (l->read_only)
		flags |= MS_RDONLY;
	if (stat(l->dir, &st) != 0) {
		rc = fsu_mkdir_p(l->dir, 0755);
		if (rc != 0) {
			fprintf(stderr, "init: %s: the mount point cannot be "
				"made: %s; not mounted\n", l->dir,
				strerror(-rc));
			return;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "init: %s: the mount point is not a "
			"directory; not mounted\n", l->dir);
		return;
	}
	fsu_flags_text(flags & FSU_FLAG_MASK, words, sizeof(words));
	fprintf(stderr, "init: mounting %s on %s (%s, %s%s%s)\n", l->dev,
		l->dir, l->ent->type, words, l->opts.data[0] ? "," : "",
		l->opts.data);
	rc = bounded_mount(l->dev, l->dir, l->ent->type, flags, l->opts.data);
	if ((rc == EROFS || rc == EACCES) && !(flags & MS_RDONLY) &&
	    !l->opts.rw_given) {
		/* A write-protected device: read-only is better than not at
		 * all, unless the line insists on "rw". */
		fprintf(stderr, "init: %s is write-protected; mounting it "
			"read-only\n", l->dev);
		rc = bounded_mount(l->dev, l->dir, l->ent->type,
				   flags | MS_RDONLY, l->opts.data);
	}
	if (rc == ETIMEDOUT)
		fprintf(stderr, "init: *** mounting %s on %s did not finish "
			"within %d seconds; going on without it\n", l->dev,
			l->dir, MOUNT_TIMEOUT_MS / 1000);
	else if (rc != 0)
		fprintf(stderr, "init: *** mounting %s on %s failed: %s\n",
			l->dev, l->dir,
			rc == EINVAL ? "wrong filesystem type, bad option or "
				       "damaged filesystem" :
			rc == ENODEV ? "unknown filesystem type" :
			strerror(rc));
}

static void mount_fstab(void)
{
	struct fsu_table tab, mounts;
	struct fs_line *lines;
	long long deadline = 0;
	int rc, n = 0, maxpass = 0, waiting_said = 0;

	rc = fsu_table_load(FSU_FSTAB, &tab);
	if (rc == -ENOENT)
		return;
	if (rc != 0) {
		fprintf(stderr, "init: %s: %s\n", FSU_FSTAB, strerror(-rc));
		return;
	}
	if (fsu_table_load(FSU_MOUNTS, &mounts) != 0)
		memset(&mounts, 0, sizeof(mounts));
	lines = calloc((size_t)(tab.n ? tab.n : 1), sizeof(*lines));
	if (!lines) {
		fprintf(stderr, "init: %s: out of memory\n", FSU_FSTAB);
		fsu_table_free(&mounts);
		fsu_table_free(&tab);
		return;
	}

	/* 1-2: what is to be done at all. */
	for (int i = 0; i < tab.n; i++) {
		const struct fsu_entry *e = &tab.e[i];
		struct fs_line *l = &lines[n];

		if (!e->dir[0] || !e->type[0]) {
			fprintf(stderr, "init: %s: entry %d is incomplete "
				"(device, mount point and type are needed), "
				"ignored\n", FSU_FSTAB, e->line);
			continue;
		}
		if (!strcmp(e->type, "swap") || !strcmp(e->type, "ignore") ||
		    !strcmp(e->dir, "none"))
			continue;
		memset(l, 0, sizeof(*l));
		l->ent = e;
		fsu_opts_init(&l->opts);
		if (fsu_opts_parse(&l->opts, e->opts) != 0) {
			fprintf(stderr, "init: %s: the options of %s are too "
				"long, not mounted\n", FSU_FSTAB, e->dir);
			continue;
		}
		if (l->opts.noauto)
			continue;
		if (e->dir[0] != '/') {
			fprintf(stderr, "init: %s: mount point \"%s\" is not an "
				"absolute path, ignored\n", FSU_FSTAB, e->dir);
			continue;
		}
		if (fsu_canon_dir(e->dir, l->dir, sizeof(l->dir)) != 0) {
			fprintf(stderr, "init: %s: mount point %s is not "
				"usable, ignored\n", FSU_FSTAB, e->dir);
			continue;
		}
		if (strcmp(l->dir, "/") == 0) {
			fstab_root(e, &mounts);
			continue;
		}
		if (l->opts.bind || l->opts.loop) {
			fprintf(stderr, "init: %s: %s: %s mounts are not "
				"offered, not mounted\n", FSU_FSTAB, l->dir,
				l->opts.bind ? "bind" : "loop");
			continue;
		}
		if (mounted_at(&mounts, l->dir)) {
			/* /proc, /sys, /dev: the kernel's, mounted already */
			if (!fsu_type_is_kernel(e->type))
				fprintf(stderr, "init: %s: mounted already\n",
					l->dir);
			continue;
		}
		if (fsu_type_is_kernel(e->type)) {
			fprintf(stderr, "init: %s: %s is mounted by the kernel "
				"only, ignored\n", FSU_FSTAB, e->type);
			continue;
		}
		l->is_dev = fsu_spec_is_device(e->spec);
		if (e->passno > maxpass)
			maxpass = e->passno;
		n++;
	}

	/* 3: the devices. */
	for (int i = 0; i < n; i++) {
		struct fs_line *l = &lines[i];

		if (!l->is_dev) {
			snprintf(l->dev, sizeof(l->dev), "%s", l->ent->spec);
			continue;
		}
		rc = fsu_resolve(l->ent->spec, l->dev, sizeof(l->dev));
		if (rc == -ENOENT) {
			if (!deadline)
				deadline = fsu_now_ms() + FSTAB_DEVICE_WAIT_MS;
			if (!waiting_said) {
				fprintf(stderr, "init: waiting for %s (up to %d "
					"seconds)\n", l->ent->spec,
					FSTAB_DEVICE_WAIT_MS / 1000);
				waiting_said = 1;
			}
			rc = fsu_wait_resolve(l->ent->spec, l->dev,
					      sizeof(l->dev), deadline);
		}
		if (rc != 0) {
			if (!l->opts.nofail || rc != -ENOENT)
				fprintf(stderr, "init: *** %s: %s %s; %s is not "
					"mounted\n", FSU_FSTAB, l->ent->spec,
					rc == -ENOENT ? "did not appear" :
					"is not a usable device name", l->dir);
			l->skip = 1;
			continue;
		}
		/* The same device already mounted somewhere else. */
		for (int k = 0; k < mounts.n; k++) {
			char real[FSU_PATH_MAX];

			if (!fsu_spec_is_device(mounts.e[k].spec) ||
			    fsu_resolve(mounts.e[k].spec, real,
					sizeof(real)) != 0 ||
			    strcmp(real, l->dev) != 0)
				continue;
			fprintf(stderr, "init: %s is mounted on %s already; "
				"not mounting it on %s\n", l->dev,
				mounts.e[k].dir, l->dir);
			l->skip = 1;
			break;
		}
	}

	/* 4: the checks, pass by pass. */
	for (int pass = 1; pass <= maxpass && pass <= 99; pass++) {
		for (int i = 0; i < n; i++) {
			struct fs_line *l = &lines[i];

			if (l->skip || !l->is_dev || l->ent->passno != pass)
				continue;
			if (want_poweroff || want_reboot)
				break;
			l->read_only = check_filesystem(l);
		}
	}

	/* 5: the mounts, in the order of the file. */
	for (int i = 0; i < n && !want_poweroff && !want_reboot; i++)
		if (!lines[i].skip)
			mount_line(&lines[i]);

	free(lines);
	fsu_table_free(&mounts);
	fsu_table_free(&tab);
}

/* ---- X state left over from before the last start --------------------------- */

/* Nothing of a display server can be running while init starts, so whatever
 * one left behind belongs to a session that ended with a reset or a power
 * loss, and keeps the next one from starting:
 *
 *  - /tmp/.X<n>-lock and the socket /tmp/.X11-unix/X<n>: the server finds the
 *    lock, reads a process id out of it that a different program may now
 *    have, and refuses display :<n> as taken.
 *  - <home>/.Xauthority-c and -l, the lock pair xauth holds while it rewrites
 *    the authority file.  xauth breaks such a lock only once it is ten
 *    minutes old; until then every xauth call startx makes times out, the
 *    new server's key never reaches the file, and no program can connect to
 *    the server -- a black screen.
 *
 * Only these exact names are removed, only regular files (and the socket),
 * never by following a symbolic link: unlink() acts on the name itself. */
static int x_display_name(const char *name, const char *prefix,
			  const char *suffix)
{
	size_t pl = strlen(prefix), sl = strlen(suffix), n = strlen(name);
	size_t i;

	if (n <= pl + sl || strncmp(name, prefix, pl) != 0 ||
	    strcmp(name + n - sl, suffix) != 0)
		return 0;
	for (i = pl; i < n - sl; i++)
		if (name[i] < '0' || name[i] > '9')
			return 0;
	return 1;
}

static void remove_stale(const char *path, int want_socket)
{
	struct stat st;

	if (lstat(path, &st) != 0)
		return;
	if (want_socket ? !S_ISSOCK(st.st_mode) : !S_ISREG(st.st_mode))
		return;
	if (unlink(path) == 0)
		fprintf(stderr, "init: removed %s, left by an X session "
			"that did not end\n", path);
}

static void clean_x_dir(const char *dir, const char *prefix,
			const char *suffix, int want_socket)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char path[PATH_MAX];

	if (!d)
		return;
	while ((de = readdir(d)) != NULL) {
		if (!x_display_name(de->d_name, prefix, suffix))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >=
		    (int)sizeof(path))
			continue;
		remove_stale(path, want_socket);
	}
	closedir(d);
}

static void clean_stale_x_state(void)
{
	FILE *f;
	struct passwd *pw;
	char path[PATH_MAX];

	clean_x_dir("/tmp", ".X", "-lock", 0);
	clean_x_dir("/tmp/.X11-unix", "X", "", 1);

	/* The authority-file locks of the accounts that sign in: root and the
	 * people's accounts (uid 1000 and up). */
	f = fopen("/etc/passwd", "r");
	if (!f)
		return;
	while ((pw = fgetpwent(f)) != NULL) {
		if (pw->pw_uid != 0 && pw->pw_uid < 1000)
			continue;
		if (!pw->pw_dir || pw->pw_dir[0] != '/')
			continue;
		if (snprintf(path, sizeof(path), "%s/.Xauthority-c",
			     pw->pw_dir) < (int)sizeof(path))
			remove_stale(path, 0);
		if (snprintf(path, sizeof(path), "%s/.Xauthority-l",
			     pw->pw_dir) < (int)sizeof(path))
			remove_stale(path, 0);
	}
	fclose(f);
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
	load_locale_conf();
	export_timezone();
	read_cmdline();
	setup_hostname();
	/* The filesystems before anything that may want them. */
	mount_fstab();
	/* After the mounts: a home directory may live on one of them. */
	clean_stale_x_state();
	setup_machine_id();
	load_console_keymap();

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
