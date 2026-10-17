// audiod logging: leveled messages on stderr and, optionally, the console.
//
// The daemon is quiet by default (warnings and errors only).  Each line is
// written with a single write() so lines from a restarted instance never
// interleave mid-line, and anything that came from a client (names) is
// sanitised before it gets here.
//
// Copyright (C) 2026 The LikeOS Project

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audiod.h"

static int g_level = AL_WARN;
static int g_console = -1;

static const char *const names[] = { "error", "warning", "info", "debug" };

void log_set_level(int level)
{
	if (level < AL_ERR)
		level = AL_ERR;
	if (level > AL_DEBUG)
		level = AL_DEBUG;
	g_level = level;
}

int log_get_level(void)
{
	return g_level;
}

void log_set_console(int on)
{
	if (on && g_console < 0) {
		g_console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	} else if (!on && g_console >= 0) {
		close(g_console);
		g_console = -1;
	}
}

const char *log_level_name(int level)
{
	if (level < AL_ERR || level > AL_DEBUG)
		return "?";
	return names[level];
}

int log_level_from_name(const char *s)
{
	if (!s)
		return -1;
	if (strcmp(s, "error") == 0 || strcmp(s, "err") == 0 ||
	    strcmp(s, "0") == 0)
		return AL_ERR;
	if (strcmp(s, "warning") == 0 || strcmp(s, "warn") == 0 ||
	    strcmp(s, "1") == 0)
		return AL_WARN;
	if (strcmp(s, "info") == 0 || strcmp(s, "2") == 0)
		return AL_INFO;
	if (strcmp(s, "debug") == 0 || strcmp(s, "3") == 0)
		return AL_DEBUG;
	return -1;
}

void vlogmsg(int level, const char *fmt, va_list ap)
{
	char line[512];
	int n;

	if (level > g_level)
		return;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	n = snprintf(line, sizeof(line), "audiod[%5lu.%03lu] %s: ",
		     (unsigned long)ts.tv_sec,
		     (unsigned long)(ts.tv_nsec / 1000000), log_level_name(level));
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		n = (int)sizeof(line) - 1;
	int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	if (m < 0)
		return;
	size_t len = (size_t)n + (size_t)m;
	if (len > sizeof(line) - 2)
		len = sizeof(line) - 2;
	/* strip control characters a format argument may have carried */
	for (size_t i = (size_t)n; i < len; i++) {
		unsigned char ch = (unsigned char)line[i];
		if (ch < 0x20 || ch == 0x7f)
			line[i] = '?';
	}
	line[len++] = '\n';

	ssize_t w = write(STDERR_FILENO, line, len);
	(void)w;
	if (g_console >= 0 && level <= AL_WARN) {
		w = write(g_console, line, len);
		(void)w;
	}
}

void logmsg(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlogmsg(level, fmt, ap);
	va_end(ap);
}
