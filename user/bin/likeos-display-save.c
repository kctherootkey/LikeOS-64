/*
 * likeos-display-save - save the display size the machine comes up in
 *
 * Usage: likeos-display-save WIDTHxHEIGHT
 *        likeos-display-save --clear
 *
 * Writes /etc/likeos/display.conf, which the bootloader reads to set the
 * boot framebuffer's mode and passes on to the display drivers, and which
 * the Display settings dialog applies when the X session starts; and tells
 * the running kernel (/sys/kernel/display/saved_size), whose console takes
 * the new size when the X session ends.  Installed setuid root so the
 * dialog can save the setting for any user; it takes no path and no free
 * text, only a size it checks, so that is all it can write.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define CONF_DIR "/etc/likeos"
#define CONF_PATH CONF_DIR "/display.conf"
#define CONF_TMP CONF_DIR "/display.conf.tmp"
#define KERNEL_SAVED "/sys/kernel/display/saved_size"

/* The running kernel's copy of the setting; an older kernel has none. */
static void tell_kernel(unsigned w, unsigned h)
{
	char text[32];
	int len = snprintf(text, sizeof(text), "%ux%u\n", w, h);
	int fd = open(KERNEL_SAVED, O_WRONLY);

	if (fd < 0)
		return;
	if (write(fd, text, (size_t)len) != len)
		fprintf(stderr, "likeos-display-save: %s: %s\n", KERNEL_SAVED,
			strerror(errno));
	close(fd);
}

/* WIDTHxHEIGHT, decimal, nothing else. */
static int parse_size(const char *s, unsigned *w, unsigned *h)
{
	unsigned v[2] = { 0, 0 };
	int part = 0, digits = 0;

	for (; *s; s++) {
		if (*s >= '0' && *s <= '9') {
			if (++digits > 5)
				return -1;
			v[part] = v[part] * 10 + (unsigned)(*s - '0');
		} else if ((*s == 'x' || *s == 'X') && part == 0 && digits) {
			part = 1;
			digits = 0;
		} else {
			return -1;
		}
	}
	if (part != 1 || !digits)
		return -1;
	if (v[0] < 640 || v[1] < 480 || v[0] > 16384 || v[1] > 16384)
		return -1;
	*w = v[0];
	*h = v[1];
	return 0;
}

static int save(unsigned w, unsigned h)
{
	char text[128];
	int len, fd;

	if (mkdir(CONF_DIR, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "likeos-display-save: %s: %s\n", CONF_DIR,
			strerror(errno));
		return -1;
	}
	len = snprintf(text, sizeof(text),
		       "# The display size, saved by Display settings.\n"
		       "resolution=%ux%u\n",
		       w, h);
	fd = open(CONF_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "likeos-display-save: %s: %s\n", CONF_TMP,
			strerror(errno));
		return -1;
	}
	if (write(fd, text, (size_t)len) != len || fsync(fd) != 0) {
		fprintf(stderr, "likeos-display-save: %s: %s\n", CONF_TMP,
			strerror(errno));
		close(fd);
		unlink(CONF_TMP);
		return -1;
	}
	close(fd);
	if (rename(CONF_TMP, CONF_PATH) != 0) {
		fprintf(stderr, "likeos-display-save: %s: %s\n", CONF_PATH,
			strerror(errno));
		unlink(CONF_TMP);
		return -1;
	}
	/* The bootloader reads the disk, not the page cache. */
	sync();
	tell_kernel(w, h);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned w, h;

	umask(022);
	if (argc == 2 && strcmp(argv[1], "--clear") == 0) {
		if (unlink(CONF_PATH) != 0 && errno != ENOENT) {
			fprintf(stderr, "likeos-display-save: %s: %s\n",
				CONF_PATH, strerror(errno));
			return 1;
		}
		sync();
		tell_kernel(0, 0);
		return 0;
	}
	if (argc != 2 || parse_size(argv[1], &w, &h) != 0) {
		fprintf(stderr,
			"usage: likeos-display-save WIDTHxHEIGHT | --clear\n");
		return 2;
	}
	return save(w, h) == 0 ? 0 : 1;
}
