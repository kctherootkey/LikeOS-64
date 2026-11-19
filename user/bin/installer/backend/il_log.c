/* libinstaller -- the installation log.
 *
 * Every line goes to the log file (IL_DEFAULT_LOG unless the options name
 * another), to stderr when the context is verbose, to the front end's
 * callback, and -- in a build for a serial console (-DLIKEOS_SERIAL_LOG,
 * set by the build for SERIAL=1 / USB_SERIAL=1) -- to /dev/kmsg, which the
 * kernel prints on the serial line and keeps in its log.  Lines carry the
 * "likeos-install: " prefix and, in the file, the time since the start.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "il_internal.h"

#define IL_LINE_MAX 1024

static uint64_t g_log_t0;

static const char *il_level_tag(int level)
{
	switch (level) {
	case IL_LOG_DEBUG:
		return "debug";
	case IL_LOG_INFO:
		return "info";
	case IL_LOG_WARN:
		return "warning";
	case IL_LOG_ERROR:
		return "error";
	default:
		return "log";
	}
}

/* write() all of buf; errors are ignored (a log must never fail a step). */
static void il_write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (n == 0)
			return;
		buf += n;
		len -= (size_t)n;
	}
}

void il_log_open(il_ctx *ctx)
{
	char dir[IL_PATH_MAX];
	char *slash;

	if (!g_log_t0)
		g_log_t0 = il_now_ms();
	il_strlcpy(dir, ctx->log_path, sizeof(dir));
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		il_mkdir_p(dir, 0755);
	}
	ctx->log_fd = open(ctx->log_path,
			   O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC,
			   0600);
	if (ctx->log_fd < 0)
		ctx->log_open_failed = errno ? errno : EIO;
#ifdef LIKEOS_SERIAL_LOG
	/* The kernel log device; it may not exist on an older kernel, and it
	 * may refuse writes -- either way the installation goes on. */
	ctx->kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NOCTTY);
#else
	ctx->kmsg_fd = -1;
#endif
	if (ctx->log_open_failed && ctx->verbose)
		fprintf(stderr, IL_LOG_PREFIX "cannot open the log %s: %s\n",
			ctx->log_path, strerror(ctx->log_open_failed));
}

void il_log_flush(il_ctx *ctx)
{
	if (ctx && ctx->log_fd >= 0)
		fsync(ctx->log_fd);
}

void il_log_close(il_ctx *ctx)
{
	if (ctx->log_fd >= 0) {
		fsync(ctx->log_fd);
		close(ctx->log_fd);
		ctx->log_fd = -1;
	}
	if (ctx->kmsg_fd >= 0) {
		close(ctx->kmsg_fd);
		ctx->kmsg_fd = -1;
	}
}

/* One finished line (no line end in it) to every sink. */
static void il_emit(il_ctx *ctx, int level, const char *msg)
{
	char line[IL_LINE_MAX + 64];
	uint64_t t = il_now_ms() - g_log_t0;
	int n;

	if (ctx->log_fd >= 0) {
		n = snprintf(line, sizeof(line), "[%5llu.%03llu] %-7s %s\n",
			     (unsigned long long)(t / 1000),
			     (unsigned long long)(t % 1000), il_level_tag(level),
			     msg);
		if (n > 0)
			il_write_all(ctx->log_fd, line,
				     (size_t)n < sizeof(line) ? (size_t)n
							       : sizeof(line) - 1);
	}
	if (ctx->kmsg_fd >= 0) {
		/* The kernel log takes one record per write; the priority
		 * prefix is the syslog convention (3 error, 4 warning,
		 * 6 info, 7 debug). */
		int pri = level == IL_LOG_ERROR ? 3 :
			  level == IL_LOG_WARN ? 4 :
			  level == IL_LOG_INFO ? 6 : 7;

		n = snprintf(line, sizeof(line), "<%d>" IL_LOG_PREFIX "%s\n",
			     pri, msg);
		if (n > 0)
			il_write_all(ctx->kmsg_fd, line,
				     (size_t)n < sizeof(line) ? (size_t)n
							       : sizeof(line) - 1);
	}
	if (ctx->verbose || level >= IL_LOG_WARN) {
		if (ctx->verbose || ctx->log_fd < 0)
			fprintf(stderr, IL_LOG_PREFIX "%s%s\n",
				level == IL_LOG_ERROR ? "error: " :
				level == IL_LOG_WARN ? "warning: " : "",
				msg);
	}
	if (ctx->log_cb && !ctx->in_log) {
		ctx->in_log = 1;
		n = snprintf(line, sizeof(line), IL_LOG_PREFIX "%s", msg);
		(void)n;
		ctx->log_cb(level, line, ctx->log_user);
		ctx->in_log = 0;
	}
}

void il_logv(il_ctx *ctx, int level, const char *fmt, va_list ap)
{
	char msg[IL_LINE_MAX];
	char *p, *nl;

	if (!ctx)
		return;
	if (level == IL_LOG_DEBUG && !ctx->verbose && ctx->log_fd < 0)
		return;
	vsnprintf(msg, sizeof(msg), fmt, ap);
	/* Control characters (other than line ends) are shown as '?': a log
	 * line is meant for a terminal and a serial line. */
	for (p = msg; *p; p++)
		if ((unsigned char)*p < 0x20 && *p != '\n' && *p != '\t')
			*p = '?';
	p = msg;
	while ((nl = strchr(p, '\n')) != NULL) {
		*nl = '\0';
		if (*p)
			il_emit(ctx, level, p);
		p = nl + 1;
	}
	if (*p)
		il_emit(ctx, level, p);
}

void il_log(il_ctx *ctx, int level, const char *fmt, ...)
{
	va_list ap;

	/* Debug lines always go to the file; to stderr only when verbose. */
	if (!ctx)
		return;
	va_start(ap, fmt);
	il_logv(ctx, level, fmt, ap);
	va_end(ap);
}

void il_log_text(il_ctx *ctx, int level, const char *prefix,
		 const char *text, size_t len)
{
	size_t i = 0;

	while (i < len) {
		size_t j = i;
		char line[IL_LINE_MAX];
		size_t n;

		while (j < len && text[j] != '\n')
			j++;
		n = j - i;
		if (n >= sizeof(line))
			n = sizeof(line) - 1;
		memcpy(line, text + i, n);
		line[n] = '\0';
		/* A carriage return in the middle (progress bars of tools)
		 * keeps the last state only. */
		{
			char *cr = strrchr(line, '\r');

			if (cr && cr[1])
				memmove(line, cr + 1, strlen(cr + 1) + 1);
			else if (cr)
				*cr = '\0';
		}
		if (line[0])
			il_log(ctx, level, "%s%s", prefix ? prefix : "", line);
		i = j + 1;
	}
}
