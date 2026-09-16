/*
 * backlight -- read and set the display backlight.
 *
 * The kernel offers each backlight it drives as a directory under
 * /sys/class/backlight/: brightness (read/write), max_brightness,
 * actual_brightness and bl_power.  This is the shell's hand on it:
 * print the level, set it in raw units or percent, step it, switch it
 * off and on.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#define CLASS_DIR "/sys/class/backlight"

static const char *g_device;

static void usage(FILE *f)
{
	fprintf(f,
		"usage: backlight [-d device] [-l | -q]\n"
		"       backlight [-d device] LEVEL | N%% | +N%% | -N%% | on | off\n"
		"  -l  list backlight devices\n"
		"  -q  print the level only\n"
		"  -d  choose a device under " CLASS_DIR " (default: the first)\n");
}

static int read_attr(const char *dev, const char *attr, char *buf, size_t cap)
{
	char path[512];
	snprintf(path, sizeof(path), CLASS_DIR "/%s/%s", dev, attr);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return 0;
}

static long read_num(const char *dev, const char *attr)
{
	char buf[64];
	if (read_attr(dev, attr, buf, sizeof(buf)) != 0)
		return -1;
	return strtol(buf, NULL, 10);
}

static int write_num(const char *dev, const char *attr, long v)
{
	char path[512], buf[32];
	snprintf(path, sizeof(path), CLASS_DIR "/%s/%s", dev, attr);
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	int n = snprintf(buf, sizeof(buf), "%ld\n", v);
	ssize_t w = write(fd, buf, (size_t)n);
	close(fd);
	return w == n ? 0 : -1;
}

static int first_device(char *out, size_t cap)
{
	DIR *d = opendir(CLASS_DIR);
	if (!d)
		return -1;
	struct dirent *e;
	int found = 0;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(out, cap, "%s", e->d_name);
		found = 1;
		break;
	}
	closedir(d);
	return found ? 0 : -1;
}

static int list_devices(void)
{
	DIR *d = opendir(CLASS_DIR);
	if (!d) {
		fprintf(stderr, "backlight: no backlight devices (%s)\n", strerror(errno));
		return 1;
	}
	struct dirent *e;
	int n = 0;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		char type[32];
		if (read_attr(e->d_name, "type", type, sizeof(type)) != 0)
			strcpy(type, "?");
		long cur = read_num(e->d_name, "brightness");
		long max = read_num(e->d_name, "max_brightness");
		long power = read_num(e->d_name, "bl_power");
		printf("%-24s type %-9s level %ld/%ld%s\n", e->d_name, type, cur, max,
		       power > 0 ? " (off)" : "");
		n++;
	}
	closedir(d);
	if (!n)
		printf("no backlight devices\n");
	return 0;
}

int main(int argc, char **argv)
{
	char dev[128];
	int quiet = 0;
	int i = 1;

	while (i < argc && argv[i][0] == '-' && argv[i][1] != 0 &&
	       !(argv[i][1] >= '0' && argv[i][1] <= '9')) {
		if (!strcmp(argv[i], "-l")) {
			return list_devices();
		} else if (!strcmp(argv[i], "-q")) {
			quiet = 1;
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			g_device = argv[++i];
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else {
			usage(stderr);
			return 2;
		}
		i++;
	}
	if (g_device) {
		snprintf(dev, sizeof(dev), "%s", g_device);
	} else if (first_device(dev, sizeof(dev)) != 0) {
		fprintf(stderr, "backlight: no backlight device under " CLASS_DIR "\n");
		return 1;
	}
	long max = read_num(dev, "max_brightness");
	long cur = read_num(dev, "brightness");
	if (max <= 0 || cur < 0) {
		fprintf(stderr, "backlight: %s: cannot read brightness: %s\n", dev, strerror(errno));
		return 1;
	}
	if (i >= argc) {
		long power = read_num(dev, "bl_power");
		if (quiet)
			printf("%ld\n", cur);
		else
			printf("%s: %ld of %ld (%ld%%)%s\n", dev, cur, max, (cur * 100 + max / 2) / max,
			       power > 0 ? ", off" : "");
		return 0;
	}
	if (i + 1 < argc) {
		usage(stderr);
		return 2;
	}
	const char *a = argv[i];
	if (!strcmp(a, "off"))
		return write_num(dev, "bl_power", 4) == 0 ? 0 : 1;
	if (!strcmp(a, "on"))
		return write_num(dev, "bl_power", 0) == 0 ? 0 : 1;
	char *end;
	long v = strtol(a, &end, 10);
	if (end == a) {
		usage(stderr);
		return 2;
	}
	long target;
	if (*end == '%') {
		long pct_cur = (cur * 100 + max / 2) / max;
		if (a[0] == '+' || a[0] == '-')
			v = pct_cur + v;
		if (v < 0)
			v = 0;
		if (v > 100)
			v = 100;
		target = (v * max + 50) / 100;
		end++;
	} else {
		target = (a[0] == '+' || a[0] == '-') ? cur + v : v;
	}
	if (*end != 0) {
		usage(stderr);
		return 2;
	}
	if (target < 0)
		target = 0;
	if (target > max)
		target = max;
	if (write_num(dev, "brightness", target) != 0) {
		fprintf(stderr, "backlight: %s: cannot set brightness: %s\n", dev, strerror(errno));
		return 1;
	}
	if (!quiet)
		printf("%s: %ld of %ld (%ld%%)\n", dev, target, max, (target * 100 + max / 2) / max);
	return 0;
}
