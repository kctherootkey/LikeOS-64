// devd: logging and small file helpers.
//
// Log lines go to stderr in one write() each; anything from a device or a
// client is cleaned of control characters before it is written, so a label
// cannot forge log lines or drive a terminal.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "devd.h"

int g_log_level = LOG_INFO;

static const char *const g_level_names[] = { "error", "warning", "info",
					      "debug" };

void log_msg(int level, const char *fmt, ...)
{
	char line[512];
	struct timespec ts;
	va_list ap;
	int n, m;
	size_t len;

	if (level > g_log_level)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	n = snprintf(line, sizeof(line), "devd[%5lu.%03lu] %s: ",
		     (unsigned long)ts.tv_sec,
		     (unsigned long)(ts.tv_nsec / 1000000),
		     g_level_names[level < 0 ? 0 : level > 3 ? 3 : level]);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		n = (int)sizeof(line) - 1;
	va_start(ap, fmt);
	m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	if (m < 0)
		return;
	len = (size_t)n + (size_t)m;
	if (len > sizeof(line) - 2)
		len = sizeof(line) - 2;
	for (size_t i = (size_t)n; i < len; i++) {
		unsigned char c = (unsigned char)line[i];
		if (c < 0x20 || c == 0x7f)
			line[i] = '?';
	}
	line[len++] = '\n';
	{
		ssize_t w = write(STDERR_FILENO, line, len);
		(void)w;
	}
}

long read_file(const char *path, char *buf, size_t cap)
{
	size_t got = 0;
	int fd;

	if (!cap)
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	while (got + 1 < cap) {
		ssize_t n = read(fd, buf + got, cap - 1 - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	buf[got] = '\0';
	return (long)got;
}

long read_attr(const char *path, char *buf, size_t cap)
{
	long n = read_file(path, buf, cap);

	if (n < 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' ||
			 buf[n - 1] == '\t' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return n;
}

uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
