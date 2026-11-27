// likeos-input-devices - keeps the X server's touchpad configuration in step
// with the touchpads the kernel reports.
//
// The X server here has no hotplug backend: every input device it uses is
// named in its configuration, and its usual ones -- the keyboard on
// /dev/input/event0 and the mouse on /dev/input/event1 -- are fixed.  A
// touchpad's event node is numbered when it appears (/dev/input/event2 and
// up), so it cannot be written into a shipped file.  This program writes it
// into /etc/X11/xorg.conf.d/50-likeos-touchpads.conf instead: one
// InputDevice section per touchpad, driven by libinput, added to the
// server's layout with AutoServerLayout.  With no touchpad the file is
// removed and the server starts exactly as it would without this program.
//
// The X server is started by an ordinary user (startx) and cannot write
// /etc; neither may it read a configuration that user could write, since it
// runs setuid root.  So the file is kept by root: init starts this program
// at boot (-f), it writes the file, then rewrites it whenever an input
// device appears or goes away (the kernel's /dev/devevents, through
// libudev's monitor).  A touchpad that appears late in the boot -- I2C
// controllers are probed after the PS/2 devices -- is therefore in the file
// before anyone gets to type startx.
//
//   likeos-input-devices          write (or remove) the file once
//   likeos-input-devices -f       ... and keep it current (for init)
//   likeos-input-devices -n       print what would be written; change nothing
//   likeos-input-devices -o FILE  maintain FILE instead
//
// Settings (tapping, scrolling, click method ...) come from
// /etc/likeos/touchpad.conf when it exists; see xconf.h.
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

#include <libudev.h>

#include "xconf.h"

#define SETTINGS_MAX 16384

static volatile sig_atomic_t g_stop, g_reload;

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		g_reload = 1;
	else
		g_stop = 1;
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: likeos-input-devices [-f] [-n] [-o FILE]\n"
		"  Write the X server's touchpad configuration (%s),\n"
		"  or remove it when the machine has no touchpad.\n"
		"  -f       keep it current as input devices come and go (for init)\n"
		"  -n       print the configuration instead of writing it\n"
		"  -o FILE  write FILE instead\n"
		"  Settings: %s (KEY=VALUE: Tapping, TappingDrag,\n"
		"  TappingDragLock, NaturalScrolling, ScrollMethod, HorizontalScrolling,\n"
		"  ClickMethod, DisableWhileTyping, MiddleEmulation, LeftHanded,\n"
		"  AccelProfile, AccelSpeed)\n",
		LID_DEFAULT_OUTPUT, LID_DEFAULT_SETTINGS);
}

// The settings file, if any, over the defaults.
static void load_options(struct lid_options *o)
{
	char buf[SETTINGS_MAX + 1], err[128];
	ssize_t n;
	size_t len = 0;
	int fd;

	lid_options_defaults(o);
	fd = open(LID_DEFAULT_SETTINGS, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return;
	while (len < SETTINGS_MAX &&
	       (n = read(fd, buf + len, SETTINGS_MAX - len)) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		len += (size_t)n;
	}
	close(fd);
	buf[len] = '\0';
	if (lid_options_parse(o, buf, err, sizeof(err)) > 0)
		fprintf(stderr, "likeos-input-devices: %s: %s (ignored)\n",
			LID_DEFAULT_SETTINGS, err);
}

static int mkdir_parents(const char *file)
{
	char dir[4096];
	char *p;

	if (snprintf(dir, sizeof(dir), "%s", file) >= (int)sizeof(dir))
		return -ENAMETOOLONG;
	p = strrchr(dir, '/');
	if (!p || p == dir)
		return 0;
	*p = '\0';
	for (p = dir + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(dir, 0755) != 0 && errno != EEXIST)
			return -errno;
		*p = '/';
	}
	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		return -errno;
	return 0;
}

// Does `path' hold exactly `text'?
static bool same_content(const char *path, const char *text)
{
	size_t want = strlen(text), got = 0;
	char buf[4096];
	bool same = true;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return false;
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			same = false;
			break;
		}
		if (n == 0)
			break;
		if (got + (size_t)n > want || memcmp(text + got, buf, (size_t)n) != 0) {
			same = false;
			break;
		}
		got += (size_t)n;
	}
	close(fd);
	return same && got == want;
}

// Replace `path' with `text' atomically: a reader sees the old file or the
// new one, never half of either.
static int write_atomic(const char *path, const char *text)
{
	char tmp[4096];
	size_t len = strlen(text), off = 0;
	int fd, rc;

	if (same_content(path, text))
		return 0;
	rc = mkdir_parents(path);
	if (rc)
		return rc;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;
	unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0)
		return -errno;
	while (off < len) {
		ssize_t n = write(fd, text + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			goto fail;
		}
		off += (size_t)n;
	}
	if (fchmod(fd, 0644) != 0 || fsync(fd) != 0) {
		rc = -errno;
		goto fail;
	}
	if (close(fd) != 0) {
		fd = -1;
		rc = -errno;
		goto fail;
	}
	fd = -1;
	if (rename(tmp, path) != 0) {
		rc = -errno;
		goto fail;
	}
	return 0;

fail:
	if (fd >= 0)
		close(fd);
	unlink(tmp);
	return rc;
}

// Bring the file in line with the touchpads that exist now.
static int update(struct udev *udev, const char *path, bool dry_run)
{
	struct lid_options opts;
	struct lid_touchpad *tp;
	size_t n;
	char *text;
	int rc;

	load_options(&opts);
	rc = lid_collect(udev, &tp, &n);
	if (rc) {
		fprintf(stderr, "likeos-input-devices: cannot list input devices: %s\n",
			strerror(-rc));
		return rc;
	}
	if (n == 0) {
		free(tp);
		if (dry_run) {
			printf("# no touchpad: %s is not needed\n", path);
			return 0;
		}
		if (unlink(path) != 0 && errno != ENOENT) {
			rc = -errno;
			fprintf(stderr, "likeos-input-devices: cannot remove %s: %s\n",
				path, strerror(-rc));
			return rc;
		}
		return 0;
	}
	text = lid_render(tp, n, &opts);
	free(tp);
	if (!text) {
		fprintf(stderr, "likeos-input-devices: out of memory\n");
		return -ENOMEM;
	}
	if (dry_run) {
		fputs(text, stdout);
		free(text);
		return 0;
	}
	rc = write_atomic(path, text);
	free(text);
	if (rc)
		fprintf(stderr, "likeos-input-devices: cannot write %s: %s\n", path,
			strerror(-rc));
	return rc;
}

static int watch(struct udev *udev, const char *path)
{
	struct udev_monitor *mon;
	struct sigaction sa;
	sigset_t block, waitmask;
	int fd;

	// The monitor would be created on an idle descriptor without access
	// to the kernel's event file; for a watcher that is no use.
	fd = open("/dev/devevents", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "likeos-input-devices: /dev/devevents: %s\n",
			strerror(errno));
		return 1;
	}
	close(fd);

	// The signals are blocked except while waiting, so one that arrives
	// between a check of the flags and the wait still ends the wait.
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGHUP);
	sigprocmask(SIG_BLOCK, &block, &waitmask);
	sigdelset(&waitmask, SIGTERM);
	sigdelset(&waitmask, SIGINT);
	sigdelset(&waitmask, SIGHUP);

	mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!mon || udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL) ||
	    udev_monitor_enable_receiving(mon)) {
		fprintf(stderr, "likeos-input-devices: cannot watch input devices: %s\n",
			strerror(errno));
		udev_monitor_unref(mon);
		return 1;
	}
	// Everything that happened before the monitor existed is covered by
	// this first pass.
	update(udev, path, false);

	while (!g_stop) {
		struct pollfd pfd = { .fd = udev_monitor_get_fd(mon), .events = POLLIN };
		struct udev_device *d;
		bool changed = false;
		int r = ppoll(&pfd, 1, NULL, &waitmask);

		if (r < 0 && errno != EINTR) {
			fprintf(stderr, "likeos-input-devices: ppoll: %s\n", strerror(errno));
			break;
		}
		if (g_reload) {
			g_reload = 0;
			changed = true;
		}
		if (r > 0 && (pfd.revents & (POLLERR | POLLNVAL))) {
			fprintf(stderr, "likeos-input-devices: event file failed\n");
			break;
		}
		while ((d = udev_monitor_receive_device(mon)) != NULL) {
			changed = true;
			udev_device_unref(d);
		}
		if (changed)
			update(udev, path, false);
	}
	udev_monitor_unref(mon);
	return g_stop ? 0 : 1;
}

int main(int argc, char **argv)
{
	const char *path = LID_DEFAULT_OUTPUT;
	bool follow = false, dry_run = false;
	struct udev *udev;
	int opt, rc;

	while ((opt = getopt(argc, argv, "fno:h")) != -1) {
		switch (opt) {
		case 'f':
			follow = true;
			break;
		case 'n':
			dry_run = true;
			break;
		case 'o':
			path = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind != argc || (follow && dry_run) || !path[0]) {
		usage(stderr);
		return 2;
	}
	umask(022);

	udev = udev_new();
	if (!udev) {
		fprintf(stderr, "likeos-input-devices: out of memory\n");
		return 1;
	}
	if (follow)
		rc = watch(udev, path);
	else
		rc = update(udev, path, dry_run) ? 1 : 0;
	udev_unref(udev);
	return rc;
}
