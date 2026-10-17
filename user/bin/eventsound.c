/* eventsound -- play a system event sound.
 *
 *   eventsound NAME [-w] [-t SECONDS] [-v VOLUME] [-q]
 *   eventsound --list
 *
 * Asks the audio service to play /usr/share/sounds/likeos/NAME.wav (the
 * service opens the file itself; this program passes only the name).  It
 * is meant for scripts -- the desktop start-up, log-in, shut-down -- so it
 * never makes them fail: when the audio service is not running, there is
 * no sound card, or event sounds are switched off, it exits 0 without a
 * word.  Only a malformed name or option is an error.
 *
 * -w waits until the sound has been played, but never longer than the
 * timeout (default 10 s), so a shut-down cannot hang on it.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/stat.h>

#include <audio/audio.h>

#define SOUND_DIR "/usr/share/sounds/likeos"
#define DEFAULT_TIMEOUT 10
#define MAX_LIST 256

static const char *prog = "eventsound";

/* Names are a single path component of plain characters: the service
 * enforces the same, this just gives a clear message first. */
static int valid_name(const char *n)
{
	size_t i, len = strlen(n);

	if (len == 0 || len >= AUD_EVENT_NAME_MAX)
		return 0;
	if (n[0] == '.' || n[0] == '-')
		return 0;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)n[i];
		if (!(isalnum(c) || c == '-' || c == '_' || c == '.'))
			return 0;
	}
	if (strstr(n, ".."))
		return 0;
	return 1;
}

static void on_alarm(int sig)
{
	(void)sig;
	/* The wait ran out: leave quietly; the sound may still finish. */
	_exit(0);
}

/* Errors that mean "no sound available here" rather than "you did
 * something wrong": stay silent and succeed. */
static int service_unavailable(int err)
{
	switch (-err) {
	case ENOENT: /* no socket, or no such sound file */
	case ECONNREFUSED:
	case ECONNRESET:
	case EPIPE:
	case ENOTCONN:
	case ETIMEDOUT:
	case EAGAIN:
	case EINTR:
	case ENODEV:
	case ENXIO:
	case EIO:
	case EBUSY:
	case ENOSYS:
	case EACCES:
	case EPERM:
	case ECANCELED:
	case ENOMEM:
		return 1;
	default:
		return 0;
	}
}

/* ---- --list ----------------------------------------------------------------- */

struct entry {
	char name[AUD_EVENT_NAME_MAX];
	uint64_t ms; /* duration, UINT64_MAX unknown */
};

/* Read just enough of a WAV header to know how long it plays. */
static uint64_t wav_duration_ms(const char *path)
{
	uint8_t h[12], ch[8], fmt[16];
	uint32_t byte_rate = 0;
	int fd, i;
	uint64_t r = UINT64_MAX;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return r;
	if (read(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		goto out;
	for (i = 0; i < 64; i++) { /* bounded chunk walk */
		uint32_t len;
		if (read(fd, ch, 8) != 8)
			break;
		len = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
		      ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
		if (!memcmp(ch, "fmt ", 4) && len >= 16) {
			if (read(fd, fmt, 16) != 16)
				break;
			byte_rate = (uint32_t)fmt[8] | ((uint32_t)fmt[9] << 8) |
				    ((uint32_t)fmt[10] << 16) | ((uint32_t)fmt[11] << 24);
			if (lseek(fd, (off_t)(len - 16 + (len & 1)), SEEK_CUR) < 0)
				break;
		} else if (!memcmp(ch, "data", 4)) {
			if (byte_rate)
				r = (uint64_t)len * 1000 / byte_rate;
			break;
		} else if (lseek(fd, (off_t)len + (len & 1), SEEK_CUR) < 0) {
			break;
		}
	}
out:
	close(fd);
	return r;
}

static int cmp_entry(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->name,
		      ((const struct entry *)b)->name);
}

static int list_sounds(int quiet)
{
	DIR *d = opendir(SOUND_DIR);
	struct dirent *de;
	struct entry *list;
	int n = 0, i;

	if (!d) {
		if (!quiet)
			fprintf(stderr, "%s: %s: %s\n", prog, SOUND_DIR, strerror(errno));
		return 1;
	}
	list = calloc(MAX_LIST, sizeof(*list));
	if (!list) {
		closedir(d);
		return 1;
	}
	while ((de = readdir(d)) != NULL && n < MAX_LIST) {
		size_t len = strlen(de->d_name);
		char path[512];
		struct stat st;

		if (len < 5 || strcmp(de->d_name + len - 4, ".wav"))
			continue;
		if (len - 4 >= AUD_EVENT_NAME_MAX)
			continue;
		memcpy(list[n].name, de->d_name, len - 4);
		list[n].name[len - 4] = 0;
		if (!valid_name(list[n].name))
			continue; /* the service would refuse it anyway */
		snprintf(path, sizeof(path), "%s/%s", SOUND_DIR, de->d_name);
		if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
			continue;
		list[n].ms = wav_duration_ms(path);
		n++;
	}
	closedir(d);
	qsort(list, (size_t)n, sizeof(*list), cmp_entry);
	for (i = 0; i < n; i++) {
		if (list[i].ms == UINT64_MAX)
			printf("%s\n", list[i].name);
		else
			printf("%-20s %llu.%01llu s\n", list[i].name,
			       (unsigned long long)(list[i].ms / 1000),
			       (unsigned long long)(list[i].ms % 1000 / 100));
	}
	if (n == 0 && !quiet)
		fprintf(stderr, "%s: no sounds in %s\n", prog, SOUND_DIR);
	free(list);
	return 0;
}

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: %s NAME [-w] [-t SECONDS] [-v VOLUME] [-q]\n"
		"       %s --list\n"
		"\n"
		"Plays %s/NAME.wav through the audio service.\n"
		"\n"
		"  -w, --wait          return only after the sound was played\n"
		"  -t, --timeout SEC   longest wait with -w (default %d, 0 = no limit)\n"
		"  -v, --volume N      0..100 (default 100)\n"
		"  -q, --quiet         no error messages at all\n"
		"  -l, --list          list the available sounds\n"
		"  -h, --help          this text\n",
		prog, prog, SOUND_DIR, DEFAULT_TIMEOUT);
}

static int parse_uint(const char *s, unsigned long max, unsigned long *out)
{
	char *end;
	unsigned long v;

	if (!s || !isdigit((unsigned char)*s))
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || (*end && strcmp(end, "%")) || v > max)
		return -1;
	*out = v;
	return 0;
}

int main(int argc, char **argv)
{
	const char *name = NULL;
	unsigned long volume = 100, timeout = DEFAULT_TIMEOUT;
	int wait = 0, quiet = 0, list = 0, i, err;
	la_ctl *c;

	if (argv[0] && argv[0][0]) {
		const char *s = strrchr(argv[0], '/');
		prog = s ? s + 1 : argv[0];
	}
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-w") || !strcmp(a, "--wait")) {
			wait = 1;
		} else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
			quiet = 1;
		} else if (!strcmp(a, "-l") || !strcmp(a, "--list")) {
			list = 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "-v") || !strcmp(a, "--volume") ||
			   !strcmp(a, "-t") || !strcmp(a, "--timeout")) {
			int is_vol = a[1] == 'v' || a[2] == 'v';
			if (++i >= argc ||
			    parse_uint(argv[i], is_vol ? 100 : 3600,
				       is_vol ? &volume : &timeout)) {
				if (!quiet)
					fprintf(stderr, "%s: %s needs a number (%s)\n", prog, a,
						is_vol ? "0..100" : "0..3600 seconds");
				return 2;
			}
		} else if (!strncmp(a, "--volume=", 9)) {
			if (parse_uint(a + 9, 100, &volume)) {
				if (!quiet)
					fprintf(stderr, "%s: bad volume '%s'\n", prog, a + 9);
				return 2;
			}
		} else if (!strncmp(a, "--timeout=", 10)) {
			if (parse_uint(a + 10, 3600, &timeout)) {
				if (!quiet)
					fprintf(stderr, "%s: bad timeout '%s'\n", prog, a + 10);
				return 2;
			}
		} else if (!strcmp(a, "-wq") || !strcmp(a, "-qw")) {
			wait = quiet = 1;
		} else if (a[0] == '-' && a[1]) {
			if (!quiet)
				fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
			return 2;
		} else if (!name) {
			name = a;
		} else {
			if (!quiet)
				fprintf(stderr, "%s: one sound at a time\n", prog);
			return 2;
		}
	}
	if (list)
		return list_sounds(quiet);
	if (!name) {
		if (!quiet)
			usage(stderr);
		return 2;
	}
	/* Accept "startup.wav" for "startup". */
	{
		static char trimmed[AUD_EVENT_NAME_MAX + 8];
		size_t len = strlen(name);
		if (len > 4 && len < sizeof(trimmed) && !strcmp(name + len - 4, ".wav")) {
			memcpy(trimmed, name, len - 4);
			trimmed[len - 4] = 0;
			name = trimmed;
		}
	}
	if (!valid_name(name)) {
		if (!quiet)
			fprintf(stderr, "%s: '%s' is not a sound name (letters, digits, "
				"'-', '_' and '.'; at most %d characters; see --list)\n",
				prog, name, AUD_EVENT_NAME_MAX - 1);
		return 2;
	}

	signal(SIGPIPE, SIG_IGN);
	/* Bound the whole run, connecting included, so a wedged service
	 * cannot hold a script up. */
	if (timeout) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_alarm;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGALRM, &sa, NULL);
		alarm((unsigned)(wait ? timeout : (timeout < 5 ? timeout : 5)));
	}

	c = la_ctl_open(prog, &err);
	if (!c)
		return 0; /* no audio service: nothing to play, not an error */
	err = la_play_event(c, name, (uint32_t)volume, wait);
	la_ctl_close(c);
	if (err >= 0 || service_unavailable(err))
		return 0;
	/* The service refused the request itself (a name it rejects). */
	if (!quiet)
		fprintf(stderr, "%s: %s: %s\n", prog, name, la_strerror(err));
	return err == -EINVAL ? 2 : 0;
}
