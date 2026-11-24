// devd - the LikeOS device manager.
//
// Started by init as root ("respawn /usr/sbin/devd -f").  It reads the
// kernel's device events from /dev/devevents (or, without them, looks at
// /sys/block every two seconds), keeps the table of disks and volumes, and
// answers clients on /var/run/devd/socket: the session's volume agent, the
// file manager and devctl(1).  It is the only program that mounts, unmounts
// and formats removable drives for users, and it does so only when asked by
// root or by the user logged in at the console, and only on removable USB
// drives that hold nothing the running system uses.
//
// One poll loop: device events, the listening socket, the connections and
// the pipes of running formats.  Requests are handled one after another in
// the loop; only formatting (and clearing a volume whose format was
// cancelled) runs in a child process.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "devd.h"

static volatile sig_atomic_t g_term, g_child;

static void on_signal(int sig)
{
	if (sig == SIGCHLD)
		g_child = 1;
	else
		g_term = 1;
}

static void setup_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* poll() must return */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, NULL);
}

static int run_dir(void)
{
	struct stat st;

	if (mkdir(DEVD_RUN_DIR, 0755) < 0 && errno != EEXIST) {
		log_err("mkdir " DEVD_RUN_DIR ": %s", strerror(errno));
		return -1;
	}
	if (lstat(DEVD_RUN_DIR, &st) < 0 || !S_ISDIR(st.st_mode) ||
	    st.st_uid != 0) {
		log_err(DEVD_RUN_DIR " is not a directory of root's");
		return -1;
	}
	if ((st.st_mode & 07777) != 0755 && chmod(DEVD_RUN_DIR, 0755) < 0)
		log_warn("chmod " DEVD_RUN_DIR ": %s", strerror(errno));
	return 0;
}

static void daemonize(void)
{
	pid_t pid = fork();
	int fd;

	if (pid < 0) {
		log_err("fork: %s", strerror(errno));
		exit(1);
	}
	if (pid > 0)
		_exit(0);
	setsid();
	fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		close(fd);
	}
	fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (fd >= 0) {
		dup2(fd, 2);
		close(fd);
	}
}

static void usage(void)
{
	fprintf(stderr, "usage: devd [-f] [-v]...\n"
			"  -f   stay in the foreground, log to stderr\n"
			"  -v   log more (twice: debugging)\n");
	exit(2);
}

#define MAX_POLL (2 + DEVD_MAX_CLIENTS + 2 * DEVD_MAX_JOBS)

int main(int argc, char **argv)
{
	int foreground = 0, lfd, efd;
	uint64_t next_scan;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0)
			foreground = 1;
		else if (strcmp(argv[i], "-v") == 0)
			g_log_level = LOG_DEBUG;
		else
			usage();
	}
	if (geteuid() != 0) {
		fprintf(stderr, "devd: must be started as root\n");
		return 1;
	}
	umask(022);
	if (chdir("/") < 0)
		return 1;
	setup_signals();
	if (!foreground)
		daemonize();
	if (run_dir() < 0)
		return 1;

	/* events first, then the first look, so nothing falls in between */
	efd = events_open();
	if (media_setup() < 0)
		log_warn("cannot prepare " DEVD_MEDIA "; mounting will fail");
	tab_rescan(NULL, 1);
	lfd = clients_listen(DEVD_SOCKET_PATH);
	if (lfd < 0) {
		log_err("cannot listen on " DEVD_SOCKET_PATH ": %s",
			strerror(-lfd));
		return 1;
	}
	log_info("running: %d device(s), events %s", g_tab.n,
		 efd >= 0 ? "from the kernel" : "by polling");
	next_scan = now_ms() + (efd >= 0 ? DEVD_MOUNT_CHECK_MS :
					   DEVD_POLL_RESCAN_MS);

	while (!g_term) {
		struct pollfd pfd[MAX_POLL];
		struct client *cl[DEVD_MAX_CLIENTS];
		int n = 0, ncl = 0, i_ev = -1, i_l, fmt0, nfmt, timeout;
		uint64_t now;

		i_l = n;
		pfd[n].fd = lfd;
		pfd[n].events = POLLIN;
		pfd[n++].revents = 0;
		if (efd >= 0) {
			i_ev = n;
			pfd[n].fd = efd;
			pfd[n].events = POLLIN;
			pfd[n++].revents = 0;
		}
		timeout = -1;
		for (struct client *c = g_clients;
		     c && ncl < DEVD_MAX_CLIENTS; c = c->next) {
			short ev = 0;

			if (c->dead)
				continue;
			if (!c->waiting && !c->eof)
				ev |= POLLIN;
			if (c->outlen)
				ev |= POLLOUT;
			if (client_has_line(c))
				timeout = 0;
			cl[ncl++] = c;
			pfd[n].fd = c->fd;
			pfd[n].events = ev;
			pfd[n++].revents = 0;
		}
		fmt0 = n;
		nfmt = format_pollfds(pfd + n, MAX_POLL - n);
		n += nfmt;
		now = now_ms();
		if (timeout < 0) {
			timeout = next_scan > now ? (int)(next_scan - now) : 0;
			if (g_child)
				timeout = 0;
		}
		{
			/* a cancelled format's grace period */
			int ft = format_timeout(now);

			if (ft >= 0 && ft < timeout)
				timeout = ft;
		}

		if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR) {
			log_err("poll: %s", strerror(errno));
			break;
		}
		if (g_term)
			break;
		/* SIGCHLD may have come just before poll(): always look */
		g_child = 0;
		format_reap();
		format_tick(now_ms());
		if (i_ev >= 0 && (pfd[i_ev].revents & (POLLERR | POLLNVAL))) {
			log_warn(DEVD_EVENTS_PATH " failed; looking at "
				 "/sys/block every %d s instead",
				 DEVD_POLL_RESCAN_MS / 1000);
			close(efd);
			efd = -1;
		} else if (i_ev >= 0 && (pfd[i_ev].revents & POLLIN)) {
			char force[DEVD_NAME_MAX] = "";
			int all = 0;

			if (events_read(efd, &all, force, sizeof(force))) {
				tab_rescan(force, all);
				/* and once more shortly: the kernel may
				 * announce a removal before /sys shows it */
				if (next_scan > now_ms() + 500)
					next_scan = now_ms() + 500;
			}
		}
		if (nfmt)
			format_handle(pfd + fmt0, nfmt);
		if (pfd[i_l].revents & POLLIN)
			clients_accept(lfd);
		for (int k = 0; k < ncl; k++) {
			struct client *c = cl[k];
			short re = pfd[k + (i_ev >= 0 ? 2 : 1)].revents;

			if (c->dead)
				continue;
			if (re & POLLIN)
				client_read(c);
			else if ((re & (POLLHUP | POLLERR)) &&
				 !(pfd[k + (i_ev >= 0 ? 2 : 1)].events &
				   POLLIN))
				c->dead = 1;
			client_process(c);
		}
		for (struct client *c = g_clients; c; c = c->next)
			client_flush(c);
		clients_reap();

		now = now_ms();
		if (now >= next_scan) {
			/* without events: everything; with them: the mount
			 * table (cheap -- nothing unchanged is probed) */
			tab_rescan(NULL, 0);
			next_scan = now + (efd >= 0 ? DEVD_MOUNT_CHECK_MS :
						      DEVD_POLL_RESCAN_MS);
		}
	}

	log_info("stopping");
	format_kill_all();
	clients_close_all();
	close(lfd);
	unlink(DEVD_SOCKET_PATH);
	if (efd >= 0)
		close(efd);
	return 0;
}
