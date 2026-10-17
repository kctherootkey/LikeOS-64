// audiod persistent state: master volume, mute, output selection and the
// preferred card, kept in /var/lib/audiod/state across restarts.
//
// The file is written to a temporary name in the same directory, flushed
// and renamed over the old one, so a crash or power loss leaves either the
// old or the new state and never half of each.  The directory belongs to
// the audiod user (mode 0700) and the file is created 0600; reading is
// tolerant (unknown keys are skipped, bad values keep the default) since
// the worst a damaged state file may do is reset the volume.
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

#define STATE_MAX_BYTES 4096

static int num(const char *v, uint32_t max, uint32_t *out)
{
	if (!*v || strlen(v) > 6)
		return -1;
	for (const char *p = v; *p; p++)
		if (*p < '0' || *p > '9')
			return -1;
	unsigned long x = strtoul(v, NULL, 10);
	if (x > max)
		return -1;
	*out = (uint32_t)x;
	return 0;
}

int state_parse(const char *text, size_t len, struct audiod_state *st)
{
	size_t pos = 0;
	int any = 0;

	while (pos < len) {
		char line[128];
		size_t n = 0;
		while (pos < len && text[pos] != '\n') {
			if (n < sizeof(line) - 1)
				line[n++] = text[pos];
			pos++;
		}
		if (pos < len)
			pos++;
		line[n] = '\0';
		if (!line[0] || line[0] == '#')
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		const char *k = line, *v = eq + 1;
		uint32_t u;

		if (!strcmp(k, "volume_left") && num(v, 100, &u) == 0) {
			st->vol_l = u;
			any = 1;
		} else if (!strcmp(k, "volume_right") && num(v, 100, &u) == 0) {
			st->vol_r = u;
			any = 1;
		} else if (!strcmp(k, "mute") && num(v, 1, &u) == 0) {
			st->mute = u;
			any = 1;
		} else if (!strcmp(k, "output_mode")) {
			if (!strcmp(v, "auto"))
				st->out_mode = OUTSEL_AUTO;
			else if (!strcmp(v, "id"))
				st->out_mode = OUTSEL_ID;
			else if (!strcmp(v, "all"))
				st->out_mode = OUTSEL_ALL;
			else
				continue;
			any = 1;
		} else if (!strcmp(k, "output_id") && num(v, 0xffff, &u) == 0) {
			st->out_id = u;
			any = 1;
		} else if (!strcmp(k, "pcm_output") && num(v, 0xffff, &u) == 0) {
			st->pcm_output = u;
			any = 1;
		} else if (!strcmp(k, "card")) {
			if (!strcmp(v, "none")) {
				st->card = -1;
			} else if (num(v, AUD_MAX_CARDS - 1, &u) == 0) {
				st->card = (int32_t)u;
			} else {
				continue;
			}
			any = 1;
		}
		/* anything else: a newer version's key, ignored */
	}
	return any ? 0 : -1;
}

int state_load(const char *path, struct audiod_state *st)
{
	char buf[STATE_MAX_BYTES];
	struct stat sb;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno != ENOENT)
			log_warn("cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &sb) < 0 || !S_ISREG(sb.st_mode) ||
	    sb.st_size > STATE_MAX_BYTES) {
		log_warn("%s is not a small regular file; ignored", path);
		close(fd);
		return -1;
	}
	size_t got = 0;
	while (got < sizeof(buf)) {
		ssize_t r = read(fd, buf + got, sizeof(buf) - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	if (state_parse(buf, got, st) < 0) {
		log_warn("%s holds no usable state", path);
		return -1;
	}
	return 0;
}

static const char *mode_name(uint32_t m)
{
	switch (m) {
	case OUTSEL_ID:
		return "id";
	case OUTSEL_ALL:
		return "all";
	default:
		return "auto";
	}
}

int state_save(const char *path, const struct audiod_state *st)
{
	char tmp[300];
	char buf[512];
	char card[16];

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;
	if (st->card < 0)
		snprintf(card, sizeof(card), "none");
	else
		snprintf(card, sizeof(card), "%d", (int)st->card);

	int n = snprintf(buf, sizeof(buf),
			 "# audiod state - written by the daemon, do not edit "
			 "while it runs\n"
			 "volume_left=%u\nvolume_right=%u\nmute=%u\n"
			 "output_mode=%s\noutput_id=%u\npcm_output=%u\ncard=%s\n",
			 st->vol_l, st->vol_r, st->mute, mode_name(st->out_mode),
			 st->out_id, st->pcm_output, card);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -EINVAL;

	/* a stale temporary from a crash is simply replaced; O_NOFOLLOW
	 * keeps a planted link from redirecting the write (the directory
	 * is ours alone, this is belt and braces) */
	unlink(tmp);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
		      0600);
	if (fd < 0) {
		int e = errno;
		log_warn("cannot write %s: %s", tmp, strerror(e));
		return -e;
	}
	size_t off = 0;
	while (off < (size_t)n) {
		ssize_t w = write(fd, buf + off, (size_t)n - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			close(fd);
			unlink(tmp);
			log_warn("writing %s: %s", tmp, strerror(e));
			return -e;
		}
		off += (size_t)w;
	}
	if (fsync(fd) < 0)
		log_debug("fsync %s: %s", tmp, strerror(errno));
	if (close(fd) < 0) {
		int e = errno;
		unlink(tmp);
		return -e;
	}
	if (rename(tmp, path) < 0) {
		int e = errno;
		log_warn("rename %s -> %s: %s", tmp, path, strerror(e));
		unlink(tmp);
		return -e;
	}
	return 0;
}
