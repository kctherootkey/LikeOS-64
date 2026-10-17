// audiod configuration: /etc/audiod.conf.
//
// "key = value" lines, '#' or ';' comments, blank lines ignored.  Parsing
// is strict: an unknown key, a value that is not entirely a number where a
// number belongs, a number out of range, a repeated key or an overlong line
// rejects the whole file with the line number, and the daemon keeps what
// it had (the defaults at start, the previous configuration on SIGHUP).
// Half-applied configuration is worse than none.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audiod.h"

#define CONFIG_MAX_BYTES (64u * 1024u)
#define CONFIG_MAX_LINE 512

void config_defaults(struct audiod_config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->device, sizeof(c->device), "auto");
	c->rate = 0;
	c->format = SF_NONE;
	c->channels = 0;
	c->period_ms = 25;
	c->periods = 4;
	c->default_volume = 75;
	c->resample_quality = RS_QUALITY_GOOD;
	c->event_sounds = 1;
	c->event_rate = 10;
	c->max_streams_per_user = 16;
	c->max_clients = 64;
	c->max_buffer_kb_per_user = 16384;
	c->default_buffer_ms = 40;
	c->suspend_ms = 5000;
	c->hw_volume = HWVOL_AUTO;
	c->log_level = AL_WARN;
}

int config_device_ok(const char *dev)
{
	if (strcmp(dev, "auto") == 0)
		return 1;
	/* a bare card number */
	if (dev[0] >= '0' && dev[0] <= '7' && dev[1] == '\0')
		return 1;
	/* /dev/audioN, /dev/dsp, /dev/dspN: nothing else, no traversal */
	const char *p;
	if (strncmp(dev, "/dev/audio", 10) == 0)
		p = dev + 10;
	else if (strncmp(dev, "/dev/dsp", 8) == 0)
		p = dev + 8;
	else
		return 0;
	if (*p == '\0')
		return strncmp(dev, "/dev/dsp", 8) == 0;
	if (p[0] < '0' || p[0] > '7' || p[1] != '\0')
		return 0;
	return 1;
}

/* Strictly parse a decimal number in [lo, hi]. */
static int parse_u32(const char *v, uint32_t lo, uint32_t hi, uint32_t *out)
{
	if (!*v)
		return -1;
	for (const char *p = v; *p; p++)
		if (*p < '0' || *p > '9')
			return -1;
	if (strlen(v) > 9)
		return -1;
	unsigned long x = strtoul(v, NULL, 10);
	if (x < lo || x > hi)
		return -1;
	*out = (uint32_t)x;
	return 0;
}

static int parse_bool(const char *v, int *out)
{
	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") ||
	    !strcmp(v, "1")) {
		*out = 1;
		return 0;
	}
	if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off") ||
	    !strcmp(v, "0")) {
		*out = 0;
		return 0;
	}
	return -1;
}

enum key_id {
	K_DEVICE,
	K_RATE,
	K_FORMAT,
	K_CHANNELS,
	K_PERIOD_MS,
	K_PERIODS,
	K_DEFAULT_VOLUME,
	K_RESAMPLE_QUALITY,
	K_EVENT_SOUNDS,
	K_EVENT_RATE,
	K_MAX_STREAMS_PER_USER,
	K_MAX_CLIENTS,
	K_MAX_BUFFER_KB_PER_USER,
	K_DEFAULT_BUFFER_MS,
	K_SUSPEND_MS,
	K_HW_VOLUME,
	K_LOG_LEVEL,
	K_COUNT
};

static const char *const key_names[K_COUNT] = {
	[K_DEVICE] = "device",
	[K_RATE] = "rate",
	[K_FORMAT] = "format",
	[K_CHANNELS] = "channels",
	[K_PERIOD_MS] = "period_ms",
	[K_PERIODS] = "periods",
	[K_DEFAULT_VOLUME] = "default_volume",
	[K_RESAMPLE_QUALITY] = "resample_quality",
	[K_EVENT_SOUNDS] = "event_sounds",
	[K_EVENT_RATE] = "event_rate",
	[K_MAX_STREAMS_PER_USER] = "max_streams_per_user",
	[K_MAX_CLIENTS] = "max_clients",
	[K_MAX_BUFFER_KB_PER_USER] = "max_buffer_kb_per_user",
	[K_DEFAULT_BUFFER_MS] = "default_buffer_ms",
	[K_SUSPEND_MS] = "suspend_ms",
	[K_HW_VOLUME] = "hw_volume",
	[K_LOG_LEVEL] = "log_level",
};

static int set_key(struct audiod_config *c, int key, const char *v,
		   const char **why)
{
	uint32_t u;
	int b;

	switch (key) {
	case K_DEVICE:
		if (strlen(v) >= sizeof(c->device) || !config_device_ok(v)) {
			*why = "device must be auto, a card number 0-7, "
			       "/dev/audioN or /dev/dsp[N]";
			return -1;
		}
		snprintf(c->device, sizeof(c->device), "%s", v);
		return 0;
	case K_RATE:
		if (!strcmp(v, "auto")) {
			c->rate = 0;
			return 0;
		}
		if (parse_u32(v, 8000, 192000, &u) < 0) {
			*why = "rate must be auto or 8000..192000";
			return -1;
		}
		c->rate = u;
		return 0;
	case K_FORMAT:
		if (!strcmp(v, "auto")) {
			c->format = SF_NONE;
			return 0;
		}
		b = sf_from_name(v);
		/* the device formats only; big-endian and f64 are client-side */
		if (b == SF_NONE || b == SF_S16BE || b == SF_F64LE) {
			*why = "format must be auto, u8, s16, s24, s24_3, s20, "
			       "s32 or float";
			return -1;
		}
		c->format = b;
		return 0;
	case K_CHANNELS:
		if (!strcmp(v, "auto")) {
			c->channels = 0;
			return 0;
		}
		if (parse_u32(v, 1, DSP_MAX_CH, &u) < 0) {
			*why = "channels must be auto or 1..8";
			return -1;
		}
		c->channels = u;
		return 0;
	case K_PERIOD_MS:
		if (parse_u32(v, 1, 100, &u) < 0) {
			*why = "period_ms must be 1..100";
			return -1;
		}
		c->period_ms = u;
		return 0;
	case K_PERIODS:
		if (parse_u32(v, 2, 32, &u) < 0) {
			*why = "periods must be 2..32";
			return -1;
		}
		c->periods = u;
		return 0;
	case K_DEFAULT_VOLUME:
		if (parse_u32(v, 0, 100, &u) < 0) {
			*why = "default_volume must be 0..100";
			return -1;
		}
		c->default_volume = u;
		return 0;
	case K_RESAMPLE_QUALITY:
		if (!strcmp(v, "best"))
			c->resample_quality = RS_QUALITY_BEST;
		else if (!strcmp(v, "good"))
			c->resample_quality = RS_QUALITY_GOOD;
		else if (!strcmp(v, "fast"))
			c->resample_quality = RS_QUALITY_FAST;
		else {
			*why = "resample_quality must be best, good or fast";
			return -1;
		}
		return 0;
	case K_EVENT_SOUNDS:
		if (parse_bool(v, &b) < 0) {
			*why = "event_sounds must be yes or no";
			return -1;
		}
		c->event_sounds = b;
		return 0;
	case K_EVENT_RATE:
		if (parse_u32(v, 1, 1000, &u) < 0) {
			*why = "event_rate must be 1..1000";
			return -1;
		}
		c->event_rate = u;
		return 0;
	case K_MAX_STREAMS_PER_USER:
		if (parse_u32(v, 1, HARD_MAX_STREAMS, &u) < 0) {
			*why = "max_streams_per_user must be 1..64";
			return -1;
		}
		c->max_streams_per_user = u;
		return 0;
	case K_MAX_CLIENTS:
		if (parse_u32(v, 1, HARD_MAX_CLIENTS, &u) < 0) {
			*why = "max_clients must be 1..256";
			return -1;
		}
		c->max_clients = u;
		return 0;
	case K_MAX_BUFFER_KB_PER_USER:
		if (parse_u32(v, 64, 262144, &u) < 0) {
			*why = "max_buffer_kb_per_user must be 64..262144";
			return -1;
		}
		c->max_buffer_kb_per_user = u;
		return 0;
	case K_DEFAULT_BUFFER_MS:
		if (parse_u32(v, 10, 2000, &u) < 0) {
			*why = "default_buffer_ms must be 10..2000";
			return -1;
		}
		c->default_buffer_ms = u;
		return 0;
	case K_SUSPEND_MS:
		if (parse_u32(v, 0, 3600000, &u) < 0) {
			*why = "suspend_ms must be 0..3600000";
			return -1;
		}
		c->suspend_ms = u;
		return 0;
	case K_HW_VOLUME:
		if (!strcmp(v, "auto")) {
			c->hw_volume = HWVOL_AUTO;
			return 0;
		}
		if (parse_bool(v, &b) < 0) {
			*why = "hw_volume must be auto, yes or no";
			return -1;
		}
		c->hw_volume = b ? HWVOL_YES : HWVOL_NO;
		return 0;
	case K_LOG_LEVEL:
		b = log_level_from_name(v);
		if (b < 0) {
			*why = "log_level must be error, warning, info or debug";
			return -1;
		}
		c->log_level = b;
		return 0;
	default:
		*why = "internal error";
		return -1;
	}
}

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

int config_parse(const char *text, size_t len, struct audiod_config *out,
		 char *err, size_t errlen)
{
	struct audiod_config c;
	uint32_t seen = 0;
	size_t pos = 0;
	int lineno = 0;

	config_defaults(&c);
	if (err && errlen)
		err[0] = '\0';
	if (len > CONFIG_MAX_BYTES) {
		if (err)
			snprintf(err, errlen, "file larger than %u bytes",
				 CONFIG_MAX_BYTES);
		return -1;
	}

	while (pos < len) {
		char line[CONFIG_MAX_LINE + 1];
		size_t n = 0;
		lineno++;
		while (pos < len && text[pos] != '\n') {
			if (text[pos] == '\0') {
				if (err)
					snprintf(err, errlen,
						 "line %d: NUL byte", lineno);
				return -1;
			}
			if (n >= CONFIG_MAX_LINE) {
				if (err)
					snprintf(err, errlen,
						 "line %d: longer than %d bytes",
						 lineno, CONFIG_MAX_LINE);
				return -1;
			}
			line[n++] = text[pos++];
		}
		if (pos < len)
			pos++; /* the newline */
		line[n] = '\0';

		/* comments: '#' or ';' anywhere outside a value starts one;
		 * values contain neither */
		char *hash = strpbrk(line, "#;");
		if (hash)
			*hash = '\0';

		char *s = line;
		while (is_space(*s))
			s++;
		char *e = s + strlen(s);
		while (e > s && is_space(e[-1]))
			*--e = '\0';
		if (!*s)
			continue;

		char *eq = strchr(s, '=');
		if (!eq) {
			if (err)
				snprintf(err, errlen,
					 "line %d: expected key = value", lineno);
			return -1;
		}
		char *k = s, *ke = eq;
		while (ke > k && is_space(ke[-1]))
			ke--;
		*ke = '\0';
		char *v = eq + 1;
		while (is_space(*v))
			v++;
		if (!*k || !*v) {
			if (err)
				snprintf(err, errlen,
					 "line %d: empty key or value", lineno);
			return -1;
		}
		for (char *p = v; *p; p++) {
			if (is_space(*p) || (unsigned char)*p < 0x20 ||
			    (unsigned char)*p >= 0x7f) {
				if (err)
					snprintf(err, errlen,
						 "line %d: value may not contain "
						 "spaces or special characters",
						 lineno);
				return -1;
			}
		}

		int key = -1;
		for (int i = 0; i < K_COUNT; i++)
			if (!strcmp(k, key_names[i]))
				key = i;
		if (key < 0) {
			if (err)
				snprintf(err, errlen, "line %d: unknown key '%.40s'",
					 lineno, k);
			return -1;
		}
		if (seen & (1u << key)) {
			if (err)
				snprintf(err, errlen, "line %d: '%s' given twice",
					 lineno, key_names[key]);
			return -1;
		}
		seen |= 1u << key;

		const char *why = NULL;
		if (set_key(&c, key, v, &why) < 0) {
			if (err)
				snprintf(err, errlen, "line %d: %s", lineno,
					 why ? why : "bad value");
			return -1;
		}
	}

	*out = c;
	return 0;
}

int config_load(const char *path, struct audiod_config *out)
{
	char err[160];
	struct stat st;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT) {
			config_defaults(out);
			return 1;
		}
		log_err("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		log_err("%s is not a regular file", path);
		close(fd);
		return -1;
	}
	/* Only root (or whoever runs us, when that is not root) may decide
	 * how the daemon behaves. */
	uid_t me = geteuid();
	if (st.st_uid != 0 && st.st_uid != me) {
		log_err("%s is not owned by root; ignored", path);
		close(fd);
		return -1;
	}
	/* group-writable is tolerated only for group root */
	if ((st.st_mode & S_IWOTH) || ((st.st_mode & S_IWGRP) && st.st_gid != 0)) {
		log_err("%s is writable by group or others; ignored", path);
		close(fd);
		return -1;
	}
	if ((uint64_t)st.st_size > CONFIG_MAX_BYTES) {
		log_err("%s is too large", path);
		close(fd);
		return -1;
	}

	size_t size = (size_t)st.st_size;
	char *buf = malloc(size + 1);
	if (!buf) {
		close(fd);
		return -1;
	}
	size_t got = 0;
	while (got < size) {
		ssize_t r = read(fd, buf + got, size - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			log_err("reading %s: %s", path, strerror(errno));
			free(buf);
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	buf[got] = '\0';

	int rc = config_parse(buf, got, out, err, sizeof(err));
	free(buf);
	if (rc < 0) {
		log_err("%s: %s; configuration not applied", path, err);
		return -1;
	}
	return 0;
}
