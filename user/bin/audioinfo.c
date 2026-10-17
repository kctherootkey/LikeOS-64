/* audioinfo -- report what the kernel's sound driver found.
 *
 * Asks a card's control node (/dev/mixer for card 0, /dev/mixerN for the
 * others) directly with the native ioctls of <sys/audio.h>: the card and
 * codec names, the formats, rates, channel counts and buffer sizes the
 * hardware accepts, every volume control with its decibel range, the mute
 * switches, the outputs and inputs with their jack state, and the driver's
 * own diagnostic dump (controller, codecs and their widget graph, streams).
 *
 * The control node is used because opening it claims nothing: the audio
 * service keeps playing while this runs.  --probe opens the PCM node as
 * well to see which parameters it grants, which only works while nothing
 * else (audiod) holds it.
 *
 * The nodes are crw-rw---- root:audio.  Without that access the program
 * says so and shows what the audio service reports instead, which needs
 * no privilege.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <grp.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <sys/audio.h>
#include <audio/audio.h>

#define DIAG_START (64 * 1024)
#define DIAG_MAX (4 * 1024 * 1024)

static const char *prog = "audioinfo";
static volatile sig_atomic_t stop_requested;

static struct {
	int all;
	int codecs; /* -v */
	int streams; /* -s */
	int probe;
	int events;
	int no_diag;
	int card; /* -1 = from the device path */
	const char *device;
} opt = { .card = -1 };

/* ---- names ------------------------------------------------------------------ */

static const char *fmt_names[AUDIO_FMT_MAX + 1] = {
	[AUDIO_FMT_S16_LE] = "S16_LE",
	[AUDIO_FMT_S24_LE] = "S24_LE",
	[AUDIO_FMT_S32_LE] = "S32_LE",
	[AUDIO_FMT_U8] = "U8",
	[AUDIO_FMT_S24_3LE] = "S24_3LE",
	[AUDIO_FMT_FLOAT_LE] = "FLOAT_LE",
	[AUDIO_FMT_S20_LE] = "S20_LE",
};

static const uint32_t std_rates[] = {
	8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400,
	192000, 384000
};

static const char *ctl_names[AUDIO_CTL_MAX + 1] = {
	[AUDIO_CTL_MASTER] = "Master",
	[AUDIO_CTL_PCM] = "PCM",
	[AUDIO_CTL_SPEAKER] = "Speaker",
	[AUDIO_CTL_HEADPHONE] = "Headphone",
	[AUDIO_CTL_LINE] = "Line",
	[AUDIO_CTL_HDMI] = "HDMI",
	[AUDIO_CTL_CAPTURE] = "Capture",
	[AUDIO_CTL_MIC_BOOST] = "Mic Boost",
};

static const char *out_type(uint32_t t)
{
	switch (t) {
	case AUDIO_OUT_SPEAKER: return "speaker";
	case AUDIO_OUT_HEADPHONE: return "headphone";
	case AUDIO_OUT_LINE: return "line";
	case AUDIO_OUT_HDMI: return "HDMI";
	case AUDIO_OUT_DP: return "DisplayPort";
	case AUDIO_OUT_SPDIF: return "S/PDIF";
	case AUDIO_OUT_HEADSET: return "headset";
	default: return "unknown";
	}
}

static const char *outsel(uint32_t m)
{
	switch (m) {
	case AUDIO_OUTSEL_AUTO: return "auto";
	case AUDIO_OUTSEL_ID: return "fixed";
	case AUDIO_OUTSEL_ALL: return "all";
	default: return "?";
	}
}

static const char *state_name(uint32_t s)
{
	static const char *const n[] = { "open", "setup", "prepared", "running",
					  "paused", "draining", "xrun",
					  "disconnected" };
	return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

/* Make a fixed-size string from the kernel printable and terminated. */
static const char *pstr(const char *s, size_t max, char *buf, size_t bufsz)
{
	size_t i, o = 0;

	for (i = 0; i < max && s[i] && o + 1 < bufsz; i++) {
		unsigned char c = (unsigned char)s[i];
		buf[o++] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
	}
	buf[o] = 0;
	return buf;
}

#define P(field, buf) pstr((field), sizeof(field), (buf), sizeof(buf))

static void print_fmt_mask(uint32_t m)
{
	int f, any = 0;

	for (f = 1; f <= AUDIO_FMT_MAX; f++)
		if (m & AUDIO_FMT_BIT(f)) {
			printf("%s%s", any ? " " : "", fmt_names[f] ? fmt_names[f] : "?");
			any = 1;
		}
	if (!any)
		printf("none");
	if (m & ~(((1u << (AUDIO_FMT_MAX + 1)) - 1) & ~1u))
		printf(" (+unknown 0x%x)", m);
	putchar('\n');
}

static void print_rate_mask(uint32_t m)
{
	size_t i;
	int any = 0;

	for (i = 0; i < sizeof(std_rates) / sizeof(std_rates[0]); i++)
		if (m & audio_rate_bit(std_rates[i])) {
			printf("%s%u", any ? " " : "", std_rates[i]);
			any = 1;
		}
	if (!any)
		printf("none");
	putchar('\n');
}

static void print_bytes(uint32_t b)
{
	if (b >= 1024 * 1024 && b % (1024 * 1024) == 0)
		printf("%u MB", b >> 20);
	else if (b >= 1024 && b % 1024 == 0)
		printf("%u KB", b >> 10);
	else
		printf("%u bytes", b);
}

/* hundredths of a dB -> "-12.50 dB" */
static void fmt_db(int32_t v, char *buf, size_t sz)
{
	int32_t a = v < 0 ? -v : v;
	snprintf(buf, sz, "%s%d.%02d dB", v < 0 ? "-" : "", a / 100, a % 100);
}

/* ---- device paths --------------------------------------------------------------- */

static void mixer_path(int card, char *buf, size_t sz)
{
	if (card == 0)
		snprintf(buf, sz, "/dev/mixer");
	else
		snprintf(buf, sz, "/dev/mixer%d", card);
}

static void pcm_path(int card, char *buf, size_t sz)
{
	snprintf(buf, sz, "/dev/audio%d", card);
}

/* Card index from a -d argument: "N", "/dev/audioN", "/dev/dspN",
 * "/dev/dsp", "/dev/mixerN", "/dev/mixer" -- or from the node's minor. */
static int card_from_device(const char *dev)
{
	struct stat st;
	const char *p;
	char *end;
	long v;

	v = strtol(dev, &end, 10);
	if (*dev && !*end)
		return (v >= 0 && v < AUDIO_MAX_CARDS) ? (int)v : -1;
	if (stat(dev, &st) == 0 && S_ISCHR(st.st_mode) &&
	    major(st.st_rdev) == AUDIO_MAJOR)
		return (int)(minor(st.st_rdev) >> 4);
	p = strrchr(dev, '/');
	p = p ? p + 1 : dev;
	if (!strncmp(p, "audio", 5))
		p += 5;
	else if (!strncmp(p, "mixer", 5))
		p += 5;
	else if (!strncmp(p, "dsp", 3))
		p += 3;
	else
		return -1;
	if (!*p)
		return 0;
	v = strtol(p, &end, 10);
	if (*end || v < 0 || v >= AUDIO_MAX_CARDS)
		return -1;
	return (int)v;
}

/* ---- the permission explanation and the audiod fallback ----------------------- */

static int in_audio_group(void)
{
	struct group *g = getgrnam("audio");
	int list[64];
	int n, i;

	if (!g)
		return 0;
	if ((gid_t)getegid() == g->gr_gid)
		return 1;
	n = getgroups(64, list);
	for (i = 0; i < n; i++)
		if ((gid_t)list[i] == g->gr_gid)
			return 1;
	return 0;
}

static void explain_denied(const char *path)
{
	struct group *g = getgrnam("audio");

	fprintf(stderr,
		"%s: %s: permission denied.\n"
		"  The sound devices are crw-rw---- root:audio: only root and members of\n"
		"  the 'audio' group (gid %u) may open them.  Programs play sound through\n"
		"  the audio service and never need this access.\n",
		prog, path, g ? (unsigned)g->gr_gid : 29u);
	if (!in_audio_group())
		fprintf(stderr,
			"  To see the hardware details, run this as root, or have an\n"
			"  administrator add you to the group (adduser USER audio) and log\n"
			"  in again.\n");
	else
		fprintf(stderr,
			"  You are in the 'audio' group, but this session started before\n"
			"  you were added: log out and in again.\n");
}

static int show_from_service(void)
{
	la_ctl *c;
	struct aud_server_info si;
	struct aud_cards cards;
	struct aud_outputs outs;
	uint32_t l, r, i;
	int err, mute;
	char b1[80], b2[80], b3[40];

	c = la_ctl_open(prog, &err);
	if (!c) {
		fprintf(stderr, "%s: the audio service is not reachable either: %s\n",
			prog, la_strerror(err));
		return 1;
	}
	printf("From the audio service (%s):\n\n", AUDIOD_SOCKET_PATH);
	if (la_get_server_info(c, &si) >= 0) {
		printf("Card in use      %u: %s\n", si.card, P(si.card_name, b1));
		printf("Driver           %s\n", P(si.driver, b3));
		printf("Codec            %s\n", P(si.codec, b2));
		printf("Mixer            %u Hz, %u channels, %s, period %u x %u\n",
		       si.rate, si.channels,
		       la_format_name(si.format) ? la_format_name(si.format) : "?",
		       si.period_frames, si.periods);
		printf("Underruns        %llu\n", (unsigned long long)si.underruns);
	}
	if (la_get_volume(c, &l, &r) >= 0) {
		printf("Master volume    %u%%", (l + r + 1) / 2);
		if (l != r)
			printf(" (left %u%%, right %u%%)", l, r);
		if (la_get_mute(c, &mute) >= 0 && mute)
			printf(", muted");
		putchar('\n');
	}
	if (la_get_outputs(c, &outs) >= 0) {
		if (outs.count > AUD_MAX_OUTPUTS)
			outs.count = AUD_MAX_OUTPUTS;
		printf("Outputs          mode %s\n", outsel(outs.mode));
		for (i = 0; i < outs.count; i++) {
			const struct aud_output *o = &outs.out[i];
			printf("  %c %2u  %-16s %-11s %s", o->active ? '*' : ' ', o->id,
			       P(o->name, b1), out_type(o->type),
			       o->present ? "present" : "unplugged");
			if (o->monitor[0])
				printf(" (%s)", P(o->monitor, b2));
			putchar('\n');
		}
	}
	if (la_get_cards(c, &cards) >= 0) {
		if (cards.count > AUD_MAX_CARDS)
			cards.count = AUD_MAX_CARDS;
		printf("Cards\n");
		for (i = 0; i < cards.count; i++) {
			const struct aud_card_entry *e = &cards.card[i];
			printf("  %c %u  %-28s %-8s %s\n", e->in_use ? '*' : ' ', e->index,
			       P(e->name, b1), P(e->driver, b3), P(e->codec, b2));
		}
	}
	la_ctl_close(c);
	return 0;
}

/* ---- direct queries ---------------------------------------------------------------- */

static void show_info(const struct audio_info *in)
{
	char b[96];

	printf("Card %u: %s\n", in->card, P(in->card_name, b));
	printf("  driver            %s\n", P(in->driver, b));
	printf("  codec             %s\n", in->codec[0] ? P(in->codec, b) : "-");
	printf("  bus               %s\n", in->bus[0] ? P(in->bus, b) : "-");
	printf("  interface         version %u\n", in->version);
	printf("  capabilities     ");
	if (in->flags & AUDIO_INFO_PLAYBACK) printf(" playback");
	if (in->flags & AUDIO_INFO_CAPTURE) printf(" capture");
	if (in->flags & AUDIO_INFO_MMAP) printf(" mmap");
	if (in->flags & AUDIO_INFO_JACK_DETECT) printf(" jack-detect");
	if (in->flags & AUDIO_INFO_HDMI) printf(" hdmi");
	if (in->flags & AUDIO_INFO_HW_VOLUME) printf(" hw-volume");
	if (!in->flags) printf(" none");
	putchar('\n');
	if (in->flags & AUDIO_INFO_PLAYBACK) {
		printf("Playback\n");
		printf("  formats           ");
		print_fmt_mask(in->formats);
		printf("  rates             ");
		print_rate_mask(in->rates);
		printf("  channels          %u", in->min_channels);
		if (in->max_channels != in->min_channels)
			printf("..%u", in->max_channels);
		putchar('\n');
	}
	if (in->flags & AUDIO_INFO_CAPTURE) {
		printf("Capture\n");
		printf("  formats           ");
		print_fmt_mask(in->capture_formats);
		printf("  rates             ");
		print_rate_mask(in->capture_rates);
		printf("  channels          %u", in->capture_min_channels);
		if (in->capture_max_channels != in->capture_min_channels)
			printf("..%u", in->capture_max_channels);
		putchar('\n');
	}
	if (in->max_period_bytes || in->max_buffer_bytes) {
		printf("Buffering\n");
		printf("  period            ");
		print_bytes(in->min_period_bytes);
		printf(" .. ");
		print_bytes(in->max_period_bytes);
		putchar('\n');
		printf("  periods           %u .. %u\n", in->min_periods, in->max_periods);
		printf("  buffer            up to ");
		print_bytes(in->max_buffer_bytes);
		putchar('\n');
	}
	printf("  outputs/inputs    %u / %u\n", in->num_outputs, in->num_inputs);
}

static void show_volumes(int fd)
{
	uint32_t c;
	int any = 0;

	for (c = 0; c <= AUDIO_CTL_MAX; c++) {
		struct audio_volume v;
		struct audio_mute m;
		char d1[24], d2[24], d3[24], d4[24];
		int have_mute;

		memset(&v, 0, sizeof(v));
		v.control = c;
		if (ioctl(fd, AUDIO_GETVOL, &v) < 0)
			continue;
		memset(&m, 0, sizeof(m));
		m.control = c;
		have_mute = ioctl(fd, AUDIO_GETMUTE, &m) == 0;
		if (!any) {
			printf("Volume controls\n");
			any = 1;
		}
		printf("  %-10s ", ctl_names[c] ? ctl_names[c] : "?");
		if (v.left == v.right)
			printf("%3u%%      ", v.left);
		else
			printf("%3u/%-3u%%  ", v.left, v.right);
		if (v.db_min != 0 || v.db_max != 0) {
			fmt_db(v.db_left, d1, sizeof(d1));
			fmt_db(v.db_right, d2, sizeof(d2));
			fmt_db(v.db_min, d3, sizeof(d3));
			fmt_db(v.db_max, d4, sizeof(d4));
			if (v.db_left == v.db_right)
				printf("%-11s", d1);
			else
				printf("%s/%s ", d1, d2);
			printf(" range %s .. %s", d3, d4);
		} else {
			printf("(no dB scale)");
		}
		if (v.steps)
			printf(", %u steps", v.steps);
		else
			printf(", software");
		if (have_mute)
			printf("%s", m.mute ? ", MUTED" : "");
		putchar('\n');
	}
	if (!any)
		printf("Volume controls\n  (none)\n");
}

static void show_outputs(int fd, int inputs)
{
	struct audio_outputs o;
	uint32_t i;
	char b1[40], b2[40];

	memset(&o, 0, sizeof(o));
	if (ioctl(fd, inputs ? AUDIO_GETINPUTS : AUDIO_GETOUTPUTS, &o) < 0) {
		if (!inputs)
			printf("Outputs\n  (not reported: %s)\n", strerror(errno));
		return;
	}
	if (o.count > AUDIO_MAX_OUTPUTS)
		o.count = AUDIO_MAX_OUTPUTS;
	if (inputs && o.count == 0)
		return;
	printf("%s (selection: %s", inputs ? "Inputs" : "Outputs", outsel(o.mode));
	if (o.mode == AUDIO_OUTSEL_ID)
		printf(", id %u", o.selected);
	printf(")\n");
	if (o.count == 0)
		printf("  (none)\n");
	for (i = 0; i < o.count; i++) {
		const struct audio_output_desc *d = &o.out[i];
		const char *jack;

		if (d->flags & AUDIO_OUTF_FIXED)
			jack = "built in";
		else if (!(d->flags & AUDIO_OUTF_JACK_DETECT))
			jack = "no jack sense";
		else
			jack = d->present ? "plugged in" : "unplugged";
		printf("  %c %2u  %-16s %-11s %-13s", d->active ? '*' : ' ', d->id,
		       P(d->name, b1), out_type(d->type), jack);
		if (d->flags & AUDIO_OUTF_DOCK)
			printf(" dock");
		if (d->monitor[0])
			printf(" monitor \"%s\"", P(d->monitor, b2));
		putchar('\n');
	}
}

/* Run AUDIO_DIAG with a growing buffer until the dump fits. */
static int show_diag(int fd, uint32_t what, const char *title)
{
	uint32_t len = DIAG_START;
	char *buf = NULL;
	struct audio_diag d;
	uint32_t i;

	for (;;) {
		char *nb = realloc(buf, len);
		if (!nb) {
			free(buf);
			fprintf(stderr, "%s: out of memory\n", prog);
			return -1;
		}
		buf = nb;
		memset(&d, 0, sizeof(d));
		d.buf = (uint64_t)(uintptr_t)buf;
		d.len = len;
		d.what = what;
		if (ioctl(fd, AUDIO_DIAG, &d) < 0) {
			int e = errno;
			free(buf);
			if (e == ENOTTY || e == EINVAL || e == ENOSYS)
				return 0; /* the driver has no dump of this kind */
			printf("%s\n  (dump failed: %s)\n", title, strerror(e));
			return -1;
		}
		if (d.written > len)
			d.written = len;
		/* A dump that filled the buffer was probably cut short. */
		if (d.written + 1 >= len && len < DIAG_MAX) {
			len *= 2;
			continue;
		}
		break;
	}
	printf("%s\n", title);
	/* The text is the driver's; still keep control characters off the
	 * terminal (newlines and tabs pass). */
	for (i = 0; i < d.written; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (c == 0)
			break;
		if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7f))
			putchar(c);
		else
			putchar('?');
	}
	if (d.written && buf[d.written - 1] != '\n')
		putchar('\n');
	if (d.written + 1 >= len)
		printf("  (dump truncated at %u bytes)\n", len);
	free(buf);
	return 0;
}

/* Open the PCM node and see which parameters it grants. */
static void probe_pcm(int card)
{
	char path[32];
	struct audio_params p, got;
	struct audio_pos pos;
	int fd;
	size_t i;
	uint32_t f, ch;

	pcm_path(card, path, sizeof(path));
	fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		if (errno == EBUSY)
			printf("Probe\n  %s is in use (normally by the audio service); stop "
			       "audiod to probe it\n", path);
		else
			printf("Probe\n  %s: %s\n", path, strerror(errno));
		return;
	}
	printf("Probe of %s (playback)\n", path);
	memset(&p, 0, sizeof(p));
	p.direction = AUDIO_DIR_PLAYBACK;
	if (ioctl(fd, AUDIO_GETPARAMS, &p) == 0)
		printf("  defaults          %s, %u Hz, %u ch, period %u frames x %u, buffer %u bytes\n",
		       p.format <= AUDIO_FMT_MAX && fmt_names[p.format] ? fmt_names[p.format] : "?",
		       p.rate, p.channels, p.period_frames, p.periods, p.buffer_bytes);
	memset(&pos, 0, sizeof(pos));
	if (ioctl(fd, AUDIO_GETPOS, &pos) == 0)
		printf("  state             %s, %u xruns\n", state_name(pos.state),
		       pos.xruns);

	printf("  rates granted     ");
	for (i = 0; i < sizeof(std_rates) / sizeof(std_rates[0]); i++) {
		memset(&got, 0, sizeof(got));
		got.direction = AUDIO_DIR_PLAYBACK;
		got.format = AUDIO_FMT_S16_LE;
		got.rate = std_rates[i];
		got.channels = 2;
		if (ioctl(fd, AUDIO_SETPARAMS, &got) < 0) {
			printf("%u:err ", std_rates[i]);
			continue;
		}
		if (got.rate == std_rates[i])
			printf("%u ", std_rates[i]);
		else
			printf("%u->%u ", std_rates[i], got.rate);
	}
	putchar('\n');
	printf("  formats granted   ");
	for (f = 1; f <= AUDIO_FMT_MAX; f++) {
		memset(&got, 0, sizeof(got));
		got.direction = AUDIO_DIR_PLAYBACK;
		got.format = f;
		got.rate = 48000;
		got.channels = 2;
		if (ioctl(fd, AUDIO_SETPARAMS, &got) < 0)
			continue;
		if (got.format == f)
			printf("%s ", fmt_names[f] ? fmt_names[f] : "?");
	}
	putchar('\n');
	printf("  channels granted  ");
	for (ch = 1; ch <= 8; ch++) {
		memset(&got, 0, sizeof(got));
		got.direction = AUDIO_DIR_PLAYBACK;
		got.format = AUDIO_FMT_S16_LE;
		got.rate = 48000;
		got.channels = ch;
		if (ioctl(fd, AUDIO_SETPARAMS, &got) < 0)
			continue;
		if (got.channels == ch)
			printf("%u ", ch);
	}
	putchar('\n');
	/* Leave the device as found: back to its defaults. */
	if (p.rate)
		(void)ioctl(fd, AUDIO_SETPARAMS, &p);
	close(fd);
}

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

/* Print hardware events (jack, xrun, volume, hotplug) until Ctrl-C. */
static int watch_events(int fd)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	printf("Hardware events (Ctrl-C to stop)\n");
	fflush(stdout);
	while (!stop_requested) {
		struct pollfd pfd = { .fd = fd, .events = POLLPRI | POLLIN };
		struct audio_event ev;
		int n = poll(&pfd, 1, -1);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "%s: poll: %s\n", prog, strerror(errno));
			return 1;
		}
		if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)) {
			printf("the card went away\n");
			return 1;
		}
		for (;;) {
			const char *what;
			memset(&ev, 0, sizeof(ev));
			if (ioctl(fd, AUDIO_GETEVENT, &ev) < 0)
				break;
			if (ev.type == 0)
				break;
			switch (ev.type) {
			case AUDIO_EV_JACK: what = "jack"; break;
			case AUDIO_EV_XRUN: what = "xrun"; break;
			case AUDIO_EV_VOLUME: what = "volume"; break;
			case AUDIO_EV_OUTPUT: what = "output"; break;
			case AUDIO_EV_HOTPLUG: what = "hotplug"; break;
			case AUDIO_EV_DISCONNECT: what = "disconnect"; break;
			default: what = "event"; break;
			}
			printf("%llu.%06llu  %-10s id %u value %u",
			       (unsigned long long)(ev.timestamp_ns / 1000000000ull),
			       (unsigned long long)(ev.timestamp_ns % 1000000000ull / 1000),
			       what, ev.id, ev.value);
			if (ev.type == AUDIO_EV_JACK)
				printf("  (%s)", ev.value ? "plugged in" : "unplugged");
			else if (ev.type == AUDIO_EV_VOLUME && ev.id <= AUDIO_CTL_MAX)
				printf("  (%s)", ctl_names[ev.id]);
			else if (ev.type == AUDIO_EV_XRUN)
				printf("  (%s)", ev.value == AUDIO_DIR_CAPTURE ? "capture overrun" :
						 "playback underrun");
			putchar('\n');
			fflush(stdout);
			if (ev.type == AUDIO_EV_DISCONNECT)
				return 0;
		}
	}
	return 0;
}

/* Everything about one card; returns 0, or -errno from opening it. */
static int show_card(int card, int quiet_missing)
{
	char path[32];
	struct audio_info info;
	uint32_t ver = 0;
	int fd;

	mixer_path(card, path, sizeof(path));
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		int e = errno;
		if (e == ENOENT || e == ENODEV || e == ENXIO) {
			if (!quiet_missing)
				fprintf(stderr, "%s: no sound card %d (%s does not exist)\n",
					prog, card, path);
		} else if (e == EACCES || e == EPERM) {
			explain_denied(path);
		} else {
			fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(e));
		}
		return -e;
	}

	memset(&info, 0, sizeof(info));
	if (ioctl(fd, AUDIO_GETINFO, &info) < 0) {
		fprintf(stderr, "%s: %s: AUDIO_GETINFO: %s\n", prog, path,
			strerror(errno));
		close(fd);
		return -EIO;
	}
	if (ioctl(fd, AUDIO_GETVERSION, &ver) == 0 && ver != AUDIO_API_VERSION)
		printf("(kernel interface version %u, this program knows %u)\n", ver,
		       AUDIO_API_VERSION);
	show_info(&info);
	putchar('\n');
	show_volumes(fd);
	putchar('\n');
	show_outputs(fd, 0);
	show_outputs(fd, 1);
	if (opt.probe) {
		putchar('\n');
		probe_pcm(card);
	}
	if (!opt.no_diag) {
		putchar('\n');
		show_diag(fd, AUDIO_DIAG_SUMMARY, "Driver summary");
	}
	if (opt.codecs) {
		putchar('\n');
		show_diag(fd, AUDIO_DIAG_CODECS, "Codecs and widgets");
	}
	if (opt.streams) {
		putchar('\n');
		show_diag(fd, AUDIO_DIAG_STREAMS, "Streams");
	}
	if (opt.events) {
		putchar('\n');
		watch_events(fd);
	}
	close(fd);
	return 0;
}

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: %s [-d DEVICE|N] [-a] [-v] [-s] [-p] [-e] [-n]\n"
		"\n"
		"  -d, --device DEV   card number or node (/dev/audio0, /dev/dsp1, ...)\n"
		"  -a, --all          every card\n"
		"  -v, --verbose      the driver's codec and widget dump\n"
		"  -s, --streams      the driver's stream dump\n"
		"  -p, --probe        open the PCM node and list the parameters it grants\n"
		"                     (only while audiod does not hold it)\n"
		"  -e, --events       print jack/xrun/volume events until Ctrl-C\n"
		"  -n, --no-summary   skip the driver summary dump\n"
		"  -h, --help         this text\n",
		prog);
}

int main(int argc, char **argv)
{
	int i, rc = 0, found = 0, denied = 0;

	if (argv[0] && argv[0][0]) {
		const char *s = strrchr(argv[0], '/');
		prog = s ? s + 1 : argv[0];
	}
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-d") || !strcmp(a, "--device")) {
			if (++i >= argc) {
				fprintf(stderr, "%s: %s needs a device\n", prog, a);
				return 2;
			}
			opt.device = argv[i];
		} else if (!strncmp(a, "--device=", 9)) {
			opt.device = a + 9;
		} else if (!strcmp(a, "-a") || !strcmp(a, "--all")) {
			opt.all = 1;
		} else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
			opt.codecs = 1;
		} else if (!strcmp(a, "-s") || !strcmp(a, "--streams")) {
			opt.streams = 1;
		} else if (!strcmp(a, "-vs") || !strcmp(a, "-sv")) {
			opt.codecs = opt.streams = 1;
		} else if (!strcmp(a, "-p") || !strcmp(a, "--probe")) {
			opt.probe = 1;
		} else if (!strcmp(a, "-e") || !strcmp(a, "--events")) {
			opt.events = 1;
		} else if (!strcmp(a, "-n") || !strcmp(a, "--no-summary")) {
			opt.no_diag = 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (a[0] != '-' && !opt.device) {
			opt.device = a;
		} else {
			fprintf(stderr, "%s: unknown argument '%s'\n", prog, a);
			usage(stderr);
			return 2;
		}
	}
	if (opt.device) {
		opt.card = card_from_device(opt.device);
		if (opt.card < 0) {
			fprintf(stderr, "%s: '%s' is not a sound device or card number\n",
				prog, opt.device);
			return 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);

	if (opt.all) {
		int c;
		for (c = 0; c < AUDIO_MAX_CARDS; c++) {
			int r;
			if (found)
				printf("\n==============================================================\n\n");
			r = show_card(c, 1);
			if (r == 0)
				found++;
			else if (r == -EACCES || r == -EPERM)
				denied = 1;
			if (denied)
				break;
		}
		if (!found && !denied) {
			fprintf(stderr, "%s: no sound card found (no /dev/mixer); the kernel "
				"has no driver for this machine's audio hardware, or it has "
				"none\n", prog);
			return 1;
		}
	} else {
		int r = show_card(opt.card < 0 ? 0 : opt.card, 0);
		if (r == -EACCES || r == -EPERM)
			denied = 1;
		else if (r < 0)
			rc = 1;
	}
	if (denied) {
		putchar('\n');
		return show_from_service() ? 1 : 0;
	}
	return rc;
}
