// likeos-dm - what the console runs once the system is up: the graphical
// login (LightDM), or a text login.  Started by init from inittab (respawn),
// as root, in place of a getty.
//
// Three modes:
//
//   graphical     lightdm, in the foreground.  It needs the system message
//                 bus, which init starts on the line before this one, so
//                 the bus's socket is waited for first.  Its login screen,
//                 sessions and X servers are its own business; when it
//                 exits by itself it is started again, after 2 s and then
//                 5 s -- and after three starts in a row that did not
//                 come up, the console gets a text login instead and the
//                 reason is written on it, so a broken login screen can
//                 never lock anybody out of the machine.
//   console user  login -f <user> on the console: the desktop's "Console"
//                 button leaves the graphical session for a shell already
//                 logged in as that user, with no password asked again.
//   console getty getty on the console, the text login prompt, as init
//                 used to run it: after the console user's shell exits, or
//                 when the graphical login was given up.
//
// Switching is asked for with likeos-session-switch {console|graphical}, a
// setuid-root helper that only the user logged in at the console may run.
// It writes the request, with the CALLER's user name, to
// /run/likeos-dm/request and signals this program (SIGUSR1):
//
//   console    (graphical mode)  lightdm is stopped (SIGTERM; it ends its
//              sessions and X servers), the X server is waited for, then
//              login -f <user> runs on the console.
//   graphical  (either console mode)  the X server a startx desktop ran is
//              waited for (likeos-logout asks before it ends the desktop),
//              the console login is hung up, then lightdm runs again.
//
// A request that does not fit the current mode is ignored; one arriving in
// the middle of a switch is looked at once the switch is done.
//
// Messages go to the console.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "dmcore.h"

#define LIGHTDM_PATH "/usr/sbin/lightdm"
#define LIGHTDM_PID_FILE "/var/run/lightdm.pid"
#define LOGIN_PATH "/bin/login"
#define GETTY_PATH "/sbin/getty"
#define CONSOLE "/dev/console"
#define DBUS_SOCKET "/run/dbus/system_bus_socket"
#define GREETER_USER "lightdm"
#define GREETER_HOME "/var/lib/lightdm"

#define DBUS_WAIT_SECS 30   // for the system bus at boot
#define STOP_GRACE_SECS 20  // for lightdm to end its sessions on SIGTERM
#define X_GONE_SECS 30      // for an X server to go away
#define HANGUP_GRACE_SECS 5 // for a hung-up console login to exit
#define X_DISPLAYS 10       // /tmp/.X0-lock ... /tmp/.X9-lock

static struct dm_state g_state;
static pid_t g_child;          // the supervised program, 0 when none
static long g_child_started;   // when it was started (monotonic seconds)
static int g_child_gone;       // it exited (reaped) since the last look
static int g_child_status;
static int g_term;             // SIGTERM/SIGINT: stop everything and exit
static int g_request;          // SIGUSR1: a request may be waiting
static sigset_t g_waitset;     // the signals waited for, kept blocked

static long now_secs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec;
}

static void say(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n, fd;

	n = snprintf(buf, sizeof(buf), "likeos-dm: ");
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
	va_end(ap);
	n = (int)strlen(buf);
	if (n < (int)sizeof(buf) - 1)
		buf[n++] = '\n';
	fd = open(CONSOLE, O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t w = write(fd, buf, (size_t)n);
		(void)w;
		close(fd);
	}
}

// ---- children and signals -------------------------------------------------

static void reap(void)
{
	int status;
	pid_t w;

	while ((w = waitpid(-1, &status, WNOHANG)) > 0) {
		if (w == g_child) {
			g_child = 0;
			g_child_gone = 1;
			g_child_status = status;
		}
		// Anything else was an orphan adopted from a session (a
		// subreaper we are not, but a child's child can still be
		// ours after a double fork): reaping it was all.
	}
}

// Wait up to `secs' seconds (0: just look) for a signal; reap and note what
// arrived.  Returns early on any event.
static void wait_events(long secs)
{
	long deadline = now_secs() + secs;

	for (;;) {
		struct timespec ts = { 1, 0 };
		siginfo_t si;
		int sig;

		reap();
		if (g_child_gone || g_term || g_request)
			return;
		if (now_secs() >= deadline)
			return;
		sig = sigtimedwait(&g_waitset, &si, &ts);
		if (sig == SIGTERM || sig == SIGINT)
			g_term = 1;
		else if (sig == SIGUSR1)
			g_request = 1;
	}
}

// Wait up to `secs' seconds, or until cond() (when given) holds; a SIGTERM
// ends the wait early.  Requests and the child's exit that arrive meanwhile
// are kept for whoever looks next.
static void wait_for(long secs, int (*cond)(void))
{
	long deadline = now_secs() + secs;
	int req = 0, gone = 0;

	while (!g_term && !(cond && cond()) && now_secs() < deadline) {
		req |= g_request;
		gone |= g_child_gone;
		g_request = 0;
		g_child_gone = 0;
		wait_events(1); // cond() is looked at once a second
	}
	g_request |= req;
	g_child_gone |= gone;
}

static int child_is_gone(void)
{
	reap();
	return g_child == 0;
}

// Signal the child -- and, with `group', its process group, which it leads
// -- and wait for it for up to `grace' seconds, then kill it and its group.
// Returns once it is gone.
static void stop_child(int sig, long grace, int group)
{
	pid_t pid = g_child;

	if (pid) {
		kill(pid, sig);
		if (group)
			kill(-pid, sig);
		wait_for(grace, child_is_gone);
		if (g_child) {
			kill(pid, SIGKILL);
			kill(-pid, SIGKILL);
		}
		while (g_child) {
			int term = g_term;

			g_term = 0;
			wait_for(1, child_is_gone);
			g_term |= term;
		}
	}
	g_child_gone = 0;
}

// ---- X servers -------------------------------------------------------------

static int read_small(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return (int)n;
}

// The pid of the live X server holding display `d', or 0.  The lock file
// names it; a stale file (a server that crashed) can name a pid that has
// been reused since, so the process has to be an X server.
static pid_t x_server_pid(int d)
{
	char path[32], buf[64], comm[32];
	pid_t pid;
	int n;

	snprintf(path, sizeof(path), "/tmp/.X%d-lock", d);
	n = read_small(path, buf, sizeof(buf));
	if (n <= 0)
		return 0;
	pid = dm_parse_pid(buf, (size_t)n);
	if (pid <= 0 || kill(pid, 0) != 0)
		return 0;
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	n = read_small(path, comm, sizeof(comm));
	if (n <= 0)
		return 0;
	comm[strcspn(comm, "\n")] = '\0';
	if (comm[0] != 'X')
		return 0;
	return pid;
}

static int x_running(void)
{
	for (int d = 0; d < X_DISPLAYS; d++)
		if (x_server_pid(d))
			return 1;
	return 0;
}

static int no_x(void)
{
	return !x_running();
}

// Wait up to `secs' seconds for every X server to be gone.
static void wait_x_gone(long secs)
{
	wait_for(secs, no_x);
}

// End whatever X server is still there: one left behind by a lightdm that
// crashed, or by a console login that was hung up.  This program owns the
// display whenever it calls this.
static void stop_stray_x(void)
{
	pid_t pids[X_DISPLAYS];
	int n = 0;

	for (int d = 0; d < X_DISPLAYS; d++) {
		pid_t p = x_server_pid(d);

		if (p) {
			kill(p, SIGTERM);
			pids[n++] = p;
		}
	}
	if (!n)
		return;
	wait_x_gone(5);
	for (int i = 0; i < n; i++)
		kill(pids[i], SIGKILL);
	wait_x_gone(5);
}

// A lightdm left running by an earlier likeos-dm (one that crashed) would
// fight a new one for the display.  Its pid file names it; the process has
// to be a lightdm.
static void stop_stale_lightdm(void)
{
	char buf[64], path[32], comm[32];
	pid_t pid;
	int n;

	n = read_small(LIGHTDM_PID_FILE, buf, sizeof(buf));
	if (n <= 0)
		return;
	pid = dm_parse_pid(buf, (size_t)n);
	if (pid <= 1 || kill(pid, 0) != 0)
		return;
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	n = read_small(path, buf, sizeof(buf));
	if (n <= 0)
		return;
	buf[strcspn(buf, "\n")] = '\0';
	snprintf(comm, sizeof(comm), "%s", buf);
	if (strcmp(comm, "lightdm") != 0)
		return;
	say("stopping a lightdm left running (pid %d)", (int)pid);
	kill(pid, SIGTERM);
	for (int i = 0; i < STOP_GRACE_SECS && kill(pid, 0) == 0; i++) {
		struct timespec ts = { 1, 0 };

		nanosleep(&ts, NULL);
	}
	kill(pid, SIGKILL);
}

// ---- the console while the graphical login runs ------------------------------

// The graphical login keeps the console in graphics mode from the moment
// LightDM is started -- or, signing out of a desktop started with startx,
// from the moment that is asked -- until it is left for a console login.
// The console stays in graphics mode for as long as any program has asked
// for it, so while it is held here the moments between two X servers (the
// desktop's has ended, the login screen's has not started yet) show no
// console: the kernel shows black there.  Without the hold the console
// would be repainted in that moment, old messages and all.  The keyboard mode is left to the servers,
// which switch the console's keyboard off while they run; what is typed in
// the moment between two of them is thrown away when the console is given
// back, so it cannot reach a console login.
//
// The kernel gives the console back if this program dies holding it: the
// hold goes with the file (see the console's mode ioctls).
static int g_console_fd = -1;

static void console_hold(void)
{
	int fd;

	if (g_console_fd >= 0)
		return;
	fd = open(CONSOLE, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		say("cannot open %s: %s", CONSOLE, strerror(errno));
		return;
	}
	if (ioctl(fd, KDSETMODE, KD_GRAPHICS) != 0)
		say("cannot put the console in graphics mode: %s",
		    strerror(errno));
	g_console_fd = fd;
}

static void console_release(void)
{
	if (g_console_fd < 0)
		return;
	(void)tcflush(g_console_fd, TCIFLUSH);
	(void)ioctl(g_console_fd, KDSETMODE, KD_TEXT);
	close(g_console_fd);
	g_console_fd = -1;
}

// ---- preparation -------------------------------------------------------------

// A directory with this owner and mode, made if missing.  Something else
// in its place (a file, a symbolic link) is left alone and reported.
static void ensure_dir(const char *path, uid_t uid, gid_t gid, mode_t mode)
{
	struct stat st;

	if (mkdir(path, mode) != 0 && errno != EEXIST) {
		say("cannot create %s: %s", path, strerror(errno));
		return;
	}
	if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		say("%s is not a directory", path);
		return;
	}
	if (st.st_uid != uid || st.st_gid != gid)
		if (chown(path, uid, gid) != 0)
			say("cannot give %s to its owner: %s", path,
			    strerror(errno));
	if ((st.st_mode & 07777) != mode)
		chmod(path, mode);
}

// What the graphical login needs before it starts: the message bus's
// directory, and the greeter account's home -- writable by it, as the
// login screen keeps the last user signed in there (WebKit's storage).
// LightDM makes its own run, log and cache directories.
static void prepare_graphical(void)
{
	struct passwd *pw = getpwnam(GREETER_USER);

	ensure_dir("/run", 0, 0, 0755);
	ensure_dir("/run/dbus", 0, 0, 0755);
	if (mkdir("/var/lib", 0755) != 0 && errno != EEXIST)
		say("cannot create /var/lib: %s", strerror(errno));
	if (pw)
		ensure_dir(GREETER_HOME, pw->pw_uid, pw->pw_gid, 0750);
	else
		say("no account %s for the login screen", GREETER_USER);
}

// Whether the system message bus answers on its socket.  The socket file
// alone proves nothing: /run is kept on disk, so last boot's is there
// before the bus has started.
static int dbus_up(void)
{
	struct sockaddr_un sa;
	int fd, ok;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", DBUS_SOCKET);
	ok = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
	close(fd);
	return ok;
}

static int wait_dbus(void)
{
	wait_for(DBUS_WAIT_SECS, dbus_up);
	return dbus_up();
}

// ---- starting the programs ---------------------------------------------------

static void restore_signals(void)
{
	sigset_t none;

	signal(SIGCHLD, SIG_DFL);
	signal(SIGUSR1, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
}

static void null_stdio(void)
{
	int fd = open("/dev/null", O_RDWR);

	if (fd < 0)
		return;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
}

static int spawn(void (*child_fn)(void *), void *arg)
{
	pid_t pid = fork();

	if (pid < 0) {
		say("fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		restore_signals();
		child_fn(arg);
		_exit(127);
	}
	g_child = pid;
	g_child_gone = 0;
	g_child_started = now_secs();
	return 0;
}

static void run_lightdm(void *arg)
{
	(void)arg;
	// A session of its own, so it and its process group can be
	// signalled as one without reaching this program.  It logs to
	// /var/log/lightdm.
	setsid();
	null_stdio();
	execl(LIGHTDM_PATH, "lightdm", (char *)NULL);
	say("cannot run %s: %s", LIGHTDM_PATH, strerror(errno));
}

static void run_getty(void *arg)
{
	(void)arg;
	// As init runs it: getty starts its own session and opens the
	// console itself.
	null_stdio();
	execl(GETTY_PATH, "getty", CONSOLE, (char *)NULL);
	say("cannot run %s: %s", GETTY_PATH, strerror(errno));
}

// login -f <user> on the console, set up the way getty hands the console
// to login: a session of its own whose controlling terminal is the
// console, in the foreground, in canonical mode with echo.  login then
// records the login (the console user), becomes the user and runs the
// shell.
static void run_console_login(void *arg)
{
	const char *user = arg;
	struct termios t;
	char line[160];
	int fd, n;

	setsid();
	fd = open(CONSOLE, O_RDWR);
	if (fd < 0)
		_exit(1);
	// Taken from whatever session still had it (one left over from the
	// graphical login); this one is the console's login now.
	ioctl(fd, TIOCSCTTY, 1);
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
	tcsetpgrp(0, getpid());
	if (tcgetattr(0, &t) == 0) {
		t.c_lflag |= ICANON | ECHO | ECHOE | ISIG;
		t.c_iflag |= ICRNL;
		t.c_oflag |= OPOST | ONLCR;
		tcsetattr(0, TCSANOW, &t);
	}
	n = snprintf(line, sizeof(line),
		     "\nSigned in at the console as %s. "
		     "\"exit\" ends the console login.\n\n", user);
	if (n > 0) {
		ssize_t w = write(1, line, (size_t)n);
		(void)w;
	}
	execl(LOGIN_PATH, "login", "-f", user, (char *)NULL);
	say("cannot run %s: %s", LOGIN_PATH, strerror(errno));
}

// ---- requests ------------------------------------------------------------------

// The waiting request, consumed.  DM_REQ_NONE when there is none or it is
// malformed.
static enum dm_req take_request(char *user, size_t cap)
{
	char buf[128];
	struct stat st;
	int fd, n;

	g_request = 0;
	fd = open(DM_REQUEST_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return DM_REQ_NONE;
	// Only the helper (root) writes it; anything else is not a request.
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0) {
		close(fd);
		unlink(DM_REQUEST_FILE);
		return DM_REQ_NONE;
	}
	n = (int)read(fd, buf, sizeof(buf));
	close(fd);
	unlink(DM_REQUEST_FILE);
	if (n <= 0)
		return DM_REQ_NONE;
	return dm_parse_request(buf, (size_t)n, user, cap);
}

// ---- the modes -----------------------------------------------------------------

// The child exited by itself: on to whatever comes next.
static void child_exited(void)
{
	char why[160];
	enum dm_mode before = g_state.mode;
	long ran = now_secs() - g_child_started;
	unsigned delay;

	g_child_gone = 0;
	delay = dm_child_exited(&g_state, ran, why, sizeof(why));
	if (before == DM_MODE_GRAPHICAL) {
		// Its X server and sessions may have outlived it.
		stop_stray_x();
		if (g_state.mode != DM_MODE_GRAPHICAL) {
			console_release();
			say("%s; starting a text login on the console", why);
			say("the reason is in /var/log/lightdm/lightdm.log; "
			    "\"likeos-session-switch graphical\" tries again");
		} else if (WIFSIGNALED(g_child_status)) {
			say("lightdm was ended by signal %d; starting it again",
			    WTERMSIG(g_child_status));
		} else {
			say("lightdm exited with status %d; starting it again",
			    WEXITSTATUS(g_child_status));
		}
	}
	if (delay)
		wait_for(delay, NULL);
}

static void mode_graphical(void)
{
	char user[DM_USER_MAX + 1];

	prepare_graphical();
	stop_stray_x();
	if (!wait_dbus()) {
		if (g_term)
			return;
		say("the system message bus (dbus-daemon --system) is not "
		    "running");
		g_child_started = now_secs();
		child_exited();
		return;
	}
	console_hold();
	if (spawn(run_lightdm, NULL) != 0) {
		g_child_started = now_secs();
		child_exited();
		return;
	}
	for (;;) {
		wait_events(60);
		if (g_term)
			return;
		if (g_child_gone) {
			child_exited();
			return;
		}
		if (!g_request)
			continue;
		switch (dm_request_action(&g_state,
					  take_request(user, sizeof(user)),
					  user)) {
		case DM_ACT_LEAVE_GRAPHICAL:
			// LightDM ends its sessions and X servers on SIGTERM.
			stop_child(SIGTERM, STOP_GRACE_SECS, 0);
			wait_x_gone(X_GONE_SECS);
			stop_stray_x();
			console_release();
			dm_enter_console_user(&g_state, user);
			return;
		default:
			break;
		}
	}
}

// The console login (of either kind) runs; returns when it ended or was
// ended.
static void mode_console(void)
{
	char user[DM_USER_MAX + 1];
	int r;

	if (g_state.mode == DM_MODE_CONSOLE_USER)
		r = spawn(run_console_login, g_state.user);
	else
		r = spawn(run_getty, NULL);
	if (r != 0) {
		g_child_started = now_secs();
		child_exited();
		return;
	}
	for (;;) {
		wait_events(60);
		if (g_term)
			return;
		if (g_child_gone) {
			child_exited();
			return;
		}
		if (!g_request)
			continue;
		switch (dm_request_action(&g_state,
					  take_request(user, sizeof(user)),
					  user)) {
		case DM_ACT_LEAVE_CONSOLE:
			// A desktop started from this login with startx is on
			// its way out (likeos-logout asks first, then ends it):
			// let its X server go, then hang the login up -- the
			// shell passes the hangup on to its jobs.  The console
			// is held in graphics mode first, so it stays out of
			// sight when that server ends and puts back the text
			// mode it found: the graphical login comes next.
			console_hold();
			wait_x_gone(X_GONE_SECS);
			stop_child(SIGHUP, HANGUP_GRACE_SECS, 1);
			stop_stray_x();
			dm_enter_graphical(&g_state);
			return;
		default:
			break;
		}
	}
}

static void write_pid_file(void)
{
	char buf[32];
	int fd, n;

	ensure_dir("/run", 0, 0, 0755);
	ensure_dir(DM_RUN_DIR, 0, 0, 0700);
	unlink(DM_REQUEST_FILE);
	unlink(DM_REQUEST_TMP);
	n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
	fd = open(DM_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
				       O_NOFOLLOW, 0600);
	if (fd < 0) {
		say("cannot write %s: %s", DM_PID_FILE, strerror(errno));
		return;
	}
	if (write(fd, buf, (size_t)n) != n)
		say("cannot write %s", DM_PID_FILE);
	close(fd);
}

static void on_signal(int sig)
{
	(void)sig; // the signals are taken with sigtimedwait()
}

int main(int argc, char **argv)
{
	struct sigaction sa;

	(void)argv;
	if (argc > 1) {
		fprintf(stderr, "usage: likeos-dm   (started by init)\n");
		return 2;
	}
	if (getuid() != 0) {
		fprintf(stderr, "likeos-dm: must be started by root (init)\n");
		return 1;
	}
	umask(022);

	// Every signal this program waits for is blocked and taken with
	// sigtimedwait(); the handler exists so none of them is ignored
	// (SIGCHLD's default is to be discarded).
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&g_waitset);
	sigaddset(&g_waitset, SIGCHLD);
	sigaddset(&g_waitset, SIGUSR1);
	sigaddset(&g_waitset, SIGTERM);
	sigaddset(&g_waitset, SIGINT);
	sigprocmask(SIG_BLOCK, &g_waitset, NULL);

	write_pid_file();
	stop_stale_lightdm();
	dm_init(&g_state);

	while (!g_term) {
		if (g_state.mode == DM_MODE_GRAPHICAL) {
			mode_graphical();
		} else {
			console_release(); // however graphical mode ended
			mode_console();
		}
	}

	// The system is going down (init sends SIGTERM to everything anyway).
	g_term = 0; // let the child end properly within the grace period
	if (g_child)
		stop_child(SIGTERM, 5, 0);
	console_release(); // the shutdown messages are the console's
	unlink(DM_PID_FILE);
	return 0;
}
