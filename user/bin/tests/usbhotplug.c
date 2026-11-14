/*
 * usbhotplug -- watch disks come and go (USB storage hot-plug test)
 *
 * Watches /sys/block and prints a line for every disk that appears or goes
 * away, with its model, size and whether it is removable.  Plug USB sticks
 * in and pull them out while it runs (QEMU: device_add / device_del in the
 * monitor; VMware: connect / disconnect the stick).
 *
 * With --io, every disk that appears while the program runs is checked:
 *   - the first MiB (less for a smaller disk) is read, read again, and the
 *     two reads must be identical;
 *   - the device stays open, and when the disk goes away a read through
 *     that open descriptor must fail with EIO -- promptly: it runs in a
 *     child process that has TIMEOUT seconds (default 10) to come back,
 *     else the check fails as a hang;
 *   - /dev/<disk> must be gone after the removal.
 *
 * Usage:
 *   usbhotplug [--io] [--all] [--events N] [--timeout SECONDS]
 *     --io          the I/O checks above (as root: the devices are 0660)
 *     --all         also check disks present at the start (only with --io;
 *                   they are read, never written)
 *     --events N    stop after N attach/detach events (default: run until
 *                   interrupted)
 *     --timeout S   how long a read on a removed disk may take
 *
 * Nothing is ever written to a disk.  The exit status is the number of
 * failed checks (capped at 125); every check prints a PASS or FAIL line.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_DISKS 64
#define NAME_LEN 32
#define CHECK_BYTES (1024u * 1024u)
#define POLL_MS 250

struct disk {
	char name[NAME_LEN];
	int present; /* seen in the last scan */
	int fd; /* open for the removal check, -1 none */
	unsigned long long sectors;
};

static struct disk g_disks[MAX_DISKS];
static int g_ndisks;
static int g_fail, g_pass;
static int g_io, g_all;
static int g_timeout = 10;
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void check(int ok, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	printf("  %s: ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	if (ok)
		g_pass++;
	else
		g_fail++;
}

/* The first line of a sysfs file, without the newline ("" when missing). */
static void read_attr(const char *disk, const char *attr, char *out,
		      size_t cap)
{
	char path[128];
	FILE *f;

	out[0] = 0;
	snprintf(path, sizeof(path), "/sys/block/%s/%s", disk, attr);
	f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(out, (int)cap, f)) {
		size_t n = strlen(out);
		while (n && (out[n - 1] == '\n' || out[n - 1] == ' '))
			out[--n] = 0;
	}
	fclose(f);
}

static void format_size(unsigned long long bytes, char *out, size_t cap)
{
	static const char *units[] = { "B", "kB", "MB", "GB", "TB" };
	unsigned long long div = 1;
	int u = 0;

	while (u < 4 && bytes / div >= 1000) {
		div *= 1000;
		u++;
	}
	if (u == 0)
		snprintf(out, cap, "%llu B", bytes);
	else
		snprintf(out, cap, "%llu.%llu %s", bytes / div,
			 (bytes % div) * 10 / div, units[u]);
}

static struct disk *find_disk(const char *name)
{
	for (int i = 0; i < g_ndisks; i++)
		if (strcmp(g_disks[i].name, name) == 0)
			return &g_disks[i];
	return NULL;
}

static const char *now_str(void)
{
	static char buf[32];
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	snprintf(buf, sizeof(buf), "%5ld.%03ld", (long)ts.tv_sec,
		 ts.tv_nsec / 1000000);
	return buf;
}

/* Read `len' bytes at offset 0, all of them.  0 or -errno. */
static int read_all(int fd, unsigned char *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = pread(fd, buf + done, len - done, (off_t)done);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -ENODATA;
		done += (size_t)n;
	}
	return 0;
}

/* The I/O check of a disk that appeared: read, read again, compare; the
 * descriptor stays open for the removal check. */
static void io_attach(struct disk *d)
{
	char path[64];
	unsigned char *a, *b;
	size_t len = CHECK_BYTES;
	int rc;

	if (d->sectors == 0) {
		printf("  (no medium: no I/O check)\n");
		return;
	}
	if (d->sectors * 512ULL < len)
		len = (size_t)(d->sectors * 512ULL);
	snprintf(path, sizeof(path), "/dev/%s", d->name);
	d->fd = open(path, O_RDONLY);
	check(d->fd >= 0, "%s: open: %s", path,
	      d->fd >= 0 ? "ok" : strerror(errno));
	if (d->fd < 0)
		return;
	a = malloc(len);
	b = malloc(len);
	if (!a || !b) {
		check(0, "%s: out of memory", d->name);
		free(a);
		free(b);
		return;
	}
	memset(a, 0x5a, len);
	memset(b, 0xa5, len);
	rc = read_all(d->fd, a, len);
	check(rc == 0, "%s: read of the first %zu KiB: %s", d->name,
	      len / 1024, rc ? strerror(-rc) : "ok");
	if (rc == 0) {
		rc = read_all(d->fd, b, len);
		check(rc == 0, "%s: second read: %s", d->name,
		      rc ? strerror(-rc) : "ok");
		if (rc == 0)
			check(memcmp(a, b, len) == 0,
			      "%s: both reads return the same data", d->name);
	}
	free(a);
	free(b);
}

/* The removal check: a read through the descriptor that was open across
 * the removal fails with EIO, within the timeout. */
static void io_detach(struct disk *d)
{
	char path[64];
	struct stat st;
	pid_t pid;
	int status = 0, waited_ms = 0;

	snprintf(path, sizeof(path), "/dev/%s", d->name);
	check(stat(path, &st) != 0 && errno == ENOENT, "%s is gone", path);
	if (d->fd < 0)
		return;

	pid = fork();
	if (pid == 0) {
		unsigned char buf[4096];
		ssize_t n = pread(d->fd, buf, sizeof(buf), 0);
		_exit(n < 0 ? (errno & 0xff) : 0);
	}
	if (pid < 0) {
		check(0, "%s: fork: %s", d->name, strerror(errno));
	} else {
		for (;;) {
			pid_t r = waitpid(pid, &status, WNOHANG);
			if (r == pid)
				break;
			if (r < 0 && errno != EINTR) {
				status = -1;
				break;
			}
			if (waited_ms >= g_timeout * 1000) {
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				status = -2;
				break;
			}
			usleep(50 * 1000);
			waited_ms += 50;
		}
		if (status == -2) {
			check(0, "%s: a read after the removal hangs (> %d s)",
			      d->name, g_timeout);
		} else if (status == -1 || !WIFEXITED(status)) {
			check(0, "%s: the reading child did not exit normally",
			      d->name);
		} else {
			int e = WEXITSTATUS(status);
			check(e == EIO,
			      "%s: a read after the removal fails with EIO "
			      "(got %s, %d ms)",
			      d->name, e ? strerror(e) : "success", waited_ms);
		}
	}
	close(d->fd);
	d->fd = -1;
}

static void report_attach(struct disk *d, int initial)
{
	char model[64], vendor[64], serial[64], rem[8], size[32];

	read_attr(d->name, "device/vendor", vendor, sizeof(vendor));
	read_attr(d->name, "device/model", model, sizeof(model));
	read_attr(d->name, "device/serial", serial, sizeof(serial));
	read_attr(d->name, "removable", rem, sizeof(rem));
	format_size(d->sectors * 512ULL, size, sizeof(size));
	printf("%s %s %-8s %s %s, %s%s%s%s\n", now_str(),
	       initial ? "present" : "attach ", d->name, vendor, model,
	       d->sectors ? size : "no medium",
	       strcmp(rem, "1") == 0 ? ", removable" : "",
	       serial[0] ? ", serial " : "", serial);
	fflush(stdout);
}

/* One scan of /sys/block: returns the number of attach/detach events. */
static int scan(int initial)
{
	DIR *dir = opendir("/sys/block");
	struct dirent *de;
	int events = 0;

	if (!dir) {
		fprintf(stderr, "usbhotplug: /sys/block: %s\n", strerror(errno));
		return -1;
	}
	for (int i = 0; i < g_ndisks; i++)
		g_disks[i].present = 0;
	while ((de = readdir(dir)) != NULL) {
		struct disk *d;
		char size[32];

		if (de->d_name[0] == '.')
			continue;
		if (strlen(de->d_name) >= NAME_LEN)
			continue;
		d = find_disk(de->d_name);
		if (d) {
			d->present = 1;
			continue;
		}
		if (g_ndisks >= MAX_DISKS)
			continue;
		d = &g_disks[g_ndisks++];
		memset(d, 0, sizeof(*d));
		strcpy(d->name, de->d_name);
		d->present = 1;
		d->fd = -1;
		read_attr(d->name, "size", size, sizeof(size));
		d->sectors = strtoull(size, NULL, 10);
		report_attach(d, initial);
		if (g_io && (!initial || g_all))
			io_attach(d);
		if (!initial)
			events++;
	}
	closedir(dir);

	for (int i = 0; i < g_ndisks;) {
		struct disk *d = &g_disks[i];
		if (d->present) {
			i++;
			continue;
		}
		printf("%s detach  %s\n", now_str(), d->name);
		fflush(stdout);
		if (g_io)
			io_detach(d);
		else if (d->fd >= 0)
			close(d->fd);
		events++;
		g_disks[i] = g_disks[--g_ndisks];
	}
	return events;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: usbhotplug [--io] [--all] [--events N] [--timeout SECONDS]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int max_events = 0, events = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--io") == 0)
			g_io = 1;
		else if (strcmp(argv[i], "--all") == 0)
			g_all = 1;
		else if (strcmp(argv[i], "--events") == 0 && i + 1 < argc)
			max_events = atoi(argv[++i]);
		else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
			g_timeout = atoi(argv[++i]);
		else
			usage();
	}
	if (g_timeout <= 0)
		g_timeout = 10;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	printf("usbhotplug: watching /sys/block%s; plug USB storage in and "
	       "out (Ctrl-C ends)\n",
	       g_io ? " with I/O checks" : "");
	if (scan(1) < 0)
		return 1;
	while (!g_stop && (!max_events || events < max_events)) {
		usleep(POLL_MS * 1000);
		int n = scan(0);
		if (n < 0)
			break;
		events += n;
	}
	for (int i = 0; i < g_ndisks; i++)
		if (g_disks[i].fd >= 0)
			close(g_disks[i].fd);
	printf("usbhotplug: %d event(s), %d check(s) passed, %d failed\n",
	       events, g_pass, g_fail);
	return g_fail > 125 ? 125 : g_fail;
}
