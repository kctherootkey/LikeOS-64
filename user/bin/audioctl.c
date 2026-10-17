/* audioctl -- control the system audio service from the command line.
 *
 * Everything here goes through libaudio's control connection to audiod:
 * the master volume and mute, which output the sound goes to (speaker,
 * headphone, HDMI, ...), the streams that are playing now and their own
 * volumes, which sound card the service drives, and change notifications.
 * Nothing opens /dev/audioN; ordinary users have no access to it and do not
 * need any -- audiod decides what a client may change (its own streams; the
 * card choice is root's).
 *
 * Usage: audioctl [-q] [--json] [COMMAND [ARGS]]
 *
 *   (none) | status            summary: card, codec, volume, outputs, streams
 *   volume [N|+N|-N|N%|L,R|up|down]
 *   mute | unmute | toggle
 *   output [auto|all|speaker|headphone|line|hdmi|dp|spdif|headset|ID|NAME]
 *   outputs                    list outputs with their jack state
 *   streams                    list playing streams
 *   stream-volume ID N[,R]     set one stream's volume (+N/-N relative)
 *   kill ID                    end a stream
 *   cards                      list the sound cards the service knows
 *   card N                     drive card N (root only)
 *   info                       everything the service reports about itself
 *   watch [COUNT]              print change notifications as they come
 *
 * A change of volume or mute plays the short "volume" event sound so the
 * new level can be heard; -q suppresses that and all normal output.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <pwd.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>

#include <sys/audio.h>
#include <audio/audio.h>

#define VOLUME_STEP 5 /* "up" / "down" */

static const char *prog = "audioctl";
static int opt_quiet;
static int opt_json;
static la_ctl *ctl;
static volatile sig_atomic_t stop_requested;

/* ---- small helpers -------------------------------------------------------- */

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: %s [-q] [--json] [COMMAND [ARGS]]\n"
		"\n"
		"Commands:\n"
		"  status                      card, volume, outputs and streams (default)\n"
		"  volume [N|+N|-N|N%%|L,R|up|down]\n"
		"                              show or set the master volume (0..100)\n"
		"  mute | unmute | toggle      master mute\n"
		"  output [MODE|TYPE|ID|NAME]  show or choose the output:\n"
		"                              auto, all, speaker, headphone, line, hdmi,\n"
		"                              dp, spdif, headset, an output id or name\n"
		"  outputs                     list outputs and their jack state\n"
		"  streams                     list the streams playing now\n"
		"  stream-volume ID N[,R]      set one stream's volume (+N/-N relative)\n"
		"  kill ID                     end a stream (owner or root)\n"
		"  cards                       list sound cards\n"
		"  card N                      use card N (root only)\n"
		"  info                        what the audio service reports about itself\n"
		"  watch [COUNT]               print change notifications\n"
		"\n"
		"Options:\n"
		"  -q, --quiet                 no output and no feedback sound\n"
		"  -j, --json                  machine-readable output\n"
		"  -h, --help                  this text\n"
		"  -V, --version               version\n",
		prog);
}

static int fail(int err, const char *what)
{
	if (err == -EPERM || err == -EACCES)
		fprintf(stderr, "%s: %s: not permitted (%s)\n", prog, what,
			la_strerror(err));
	else
		fprintf(stderr, "%s: %s: %s\n", prog, what, la_strerror(err));
	return 1;
}

/* Strict unsigned parse: the whole string, base 10, within [0, max]. */
static int parse_uint(const char *s, uint32_t max, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (!s || !*s || !isdigit((unsigned char)*s))
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || *end || v > max)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static uint32_t clamp_vol(long v)
{
	if (v < 0)
		return 0;
	if (v > 100)
		return 100;
	return (uint32_t)v;
}

/* JSON string with every control character and quote escaped.  Names come
 * from the service, which takes them from clients, so nothing in them can
 * be trusted to be printable. */
static void json_str(const char *s, size_t max)
{
	size_t i;

	putchar('"');
	for (i = 0; i < max && s[i]; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"': fputs("\\\"", stdout); break;
		case '\\': fputs("\\\\", stdout); break;
		case '\n': fputs("\\n", stdout); break;
		case '\r': fputs("\\r", stdout); break;
		case '\t': fputs("\\t", stdout); break;
		default:
			if (c < 0x20 || c == 0x7f)
				printf("\\u%04x", c);
			else
				putchar(c);
		}
	}
	putchar('"');
}

/* Copy a fixed-size name from the service into a printable C string:
 * terminate it and replace control characters, so a stream name cannot
 * move the cursor or rewrite the terminal. */
static const char *safe_name(const char *s, size_t max, char *buf, size_t bufsz)
{
	size_t i, n = 0;

	if (bufsz == 0)
		return "";
	for (i = 0; i < max && s[i] && n + 1 < bufsz; i++) {
		unsigned char c = (unsigned char)s[i];
		buf[n++] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
	}
	buf[n] = 0;
	return buf;
}

#define SAFE(field, buf) safe_name((field), sizeof(field), (buf), sizeof(buf))

static const char *out_type_name(uint32_t type)
{
	switch (type) {
	case AUDIO_OUT_SPEAKER: return "speaker";
	case AUDIO_OUT_HEADPHONE: return "headphone";
	case AUDIO_OUT_LINE: return "line";
	case AUDIO_OUT_HDMI: return "hdmi";
	case AUDIO_OUT_DP: return "dp";
	case AUDIO_OUT_SPDIF: return "spdif";
	case AUDIO_OUT_HEADSET: return "headset";
	default: return "unknown";
	}
}

static const char *outsel_name(uint32_t mode)
{
	switch (mode) {
	case AUDIO_OUTSEL_AUTO: return "auto";
	case AUDIO_OUTSEL_ID: return "fixed";
	case AUDIO_OUTSEL_ALL: return "all";
	default: return "unknown";
	}
}

static const char *stream_state_name(uint32_t st)
{
	switch (st) {
	case 0: return "playing";
	case 1: return "paused";
	case 2: return "draining";
	case 3: return "underrun";
	default: return "unknown";
	}
}

static const char *fmt_name(uint32_t f)
{
	const char *n = la_format_name(f);
	return n ? n : "?";
}

static const char *role_name(uint32_t r)
{
	const char *n = la_role_name(r);
	return n ? n : "?";
}

static const char *user_name(uint32_t uid, char *buf, size_t sz)
{
	struct passwd *pw = getpwuid((uid_t)uid);

	if (pw && pw->pw_name && pw->pw_name[0])
		snprintf(buf, sz, "%s", pw->pw_name);
	else
		snprintf(buf, sz, "%u", uid);
	return buf;
}

static void fmt_duration_ms(uint64_t ms, char *buf, size_t sz)
{
	uint64_t s = ms / 1000;
	uint64_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;

	if (d)
		snprintf(buf, sz, "%llud %02lluh %02llum", (unsigned long long)d,
			 (unsigned long long)h, (unsigned long long)m);
	else if (h)
		snprintf(buf, sz, "%lluh %02llum %02llus", (unsigned long long)h,
			 (unsigned long long)m, (unsigned long long)(s % 60));
	else
		snprintf(buf, sz, "%llum %02llus", (unsigned long long)m,
			 (unsigned long long)(s % 60));
}

static void fmt_frames_time(uint64_t frames, uint32_t rate, char *buf, size_t sz)
{
	uint64_t s;

	if (!rate) {
		snprintf(buf, sz, "-");
		return;
	}
	s = frames / rate;
	snprintf(buf, sz, "%llu:%02llu", (unsigned long long)(s / 60),
		 (unsigned long long)(s % 60));
}

/* A short horizontal bar for the volume, 20 cells. */
static void vol_bar(uint32_t v, char *buf, size_t sz)
{
	size_t i, cells = 20, fill = (size_t)((v * cells + 50) / 100);

	if (sz < cells + 3) {
		if (sz)
			buf[0] = 0;
		return;
	}
	buf[0] = '[';
	for (i = 0; i < cells; i++)
		buf[1 + i] = i < fill ? '#' : '-';
	buf[cells + 1] = ']';
	buf[cells + 2] = 0;
}

/* The feedback tick after a change of level, unless -q. */
static void feedback_sound(void)
{
	if (opt_quiet)
		return;
	(void)la_play_event(ctl, "volume", 100, 0);
}

/* ---- queries shared by several commands ----------------------------------- */

struct snapshot {
	int have_info, have_vol, have_mute, have_out, have_cards;
	struct aud_server_info info;
	uint32_t left, right;
	int mute;
	struct aud_outputs outs;
	struct aud_cards cards;
	struct aud_stream_entry streams[AUD_MAX_STREAMS];
	int nstreams;
};

static void take_snapshot(struct snapshot *s)
{
	memset(s, 0, sizeof(*s));
	s->have_info = la_get_server_info(ctl, &s->info) >= 0;
	s->have_vol = la_get_volume(ctl, &s->left, &s->right) >= 0;
	s->have_mute = la_get_mute(ctl, &s->mute) >= 0;
	s->have_out = la_get_outputs(ctl, &s->outs) >= 0;
	if (s->have_out && s->outs.count > AUD_MAX_OUTPUTS)
		s->outs.count = AUD_MAX_OUTPUTS;
	s->have_cards = la_get_cards(ctl, &s->cards) >= 0;
	if (s->have_cards && s->cards.count > AUD_MAX_CARDS)
		s->cards.count = AUD_MAX_CARDS;
	s->nstreams = la_list_streams(ctl, s->streams, AUD_MAX_STREAMS);
	if (s->nstreams < 0)
		s->nstreams = -1;
	else if (s->nstreams > AUD_MAX_STREAMS)
		s->nstreams = AUD_MAX_STREAMS;
}

static const struct aud_output *active_output(const struct aud_outputs *o)
{
	uint32_t i;

	for (i = 0; i < o->count && i < AUD_MAX_OUTPUTS; i++)
		if (o->out[i].active)
			return &o->out[i];
	return NULL;
}

static uint32_t avg_vol(uint32_t l, uint32_t r)
{
	return (l + r + 1) / 2;
}

/* ---- printing ------------------------------------------------------------- */

static void print_outputs_text(const struct aud_outputs *o, int indent)
{
	uint32_t i;
	char nb[40], mb[40];

	if (o->count == 0) {
		printf("%*s(no outputs)\n", indent, "");
		return;
	}
	for (i = 0; i < o->count && i < AUD_MAX_OUTPUTS; i++) {
		const struct aud_output *x = &o->out[i];
		const char *jack;

		if (x->type == AUDIO_OUT_SPEAKER && x->present)
			jack = "built in";
		else
			jack = x->present ? "plugged in" : "unplugged";
		printf("%*s%c %2u  %-16s %-10s %-11s", indent, "",
		       x->active ? '*' : ' ', x->id, SAFE(x->name, nb),
		       out_type_name(x->type), jack);
		if (x->monitor[0])
			printf(" (%s)", SAFE(x->monitor, mb));
		if (o->mode == AUDIO_OUTSEL_ID && o->selected == x->id)
			printf(" [selected]");
		putchar('\n');
	}
}

static void print_outputs_json(const struct aud_outputs *o)
{
	uint32_t i;

	printf("{\"mode\":\"%s\",\"selected\":%u,\"outputs\":[",
	       outsel_name(o->mode), o->selected);
	for (i = 0; i < o->count && i < AUD_MAX_OUTPUTS; i++) {
		const struct aud_output *x = &o->out[i];
		if (i)
			putchar(',');
		printf("{\"id\":%u,\"type\":\"%s\",\"name\":", x->id,
		       out_type_name(x->type));
		json_str(x->name, sizeof(x->name));
		printf(",\"monitor\":");
		json_str(x->monitor, sizeof(x->monitor));
		printf(",\"present\":%s,\"active\":%s}",
		       x->present ? "true" : "false",
		       x->active ? "true" : "false");
	}
	printf("]}");
}

static void print_streams_text(const struct aud_stream_entry *e, int n, int indent)
{
	int i;
	char nb[80], ab[80], ub[40], tb[24];

	if (n <= 0) {
		printf("%*s(nothing playing)\n", indent, "");
		return;
	}
	printf("%*s%4s  %-6s %-9s %-24s %-6s %-10s %s\n", indent, "", "ID",
	       "ROLE", "STATE", "FORMAT", "VOLUME", "PLAYED", "NAME");
	for (i = 0; i < n; i++) {
		const struct aud_stream_entry *x = &e[i];
		char fb[40], vb[16];

		snprintf(fb, sizeof(fb), "%s %uHz %uch", fmt_name(x->format),
			 x->rate, x->channels);
		if (x->mute)
			snprintf(vb, sizeof(vb), "muted");
		else if (x->left == x->right)
			snprintf(vb, sizeof(vb), "%u%%", x->left);
		else
			snprintf(vb, sizeof(vb), "%u/%u", x->left, x->right);
		fmt_frames_time(x->played_frames, x->rate, tb, sizeof(tb));
		printf("%*s%4u  %-6s %-9s %-24s %-6s %-10s %s", indent, "",
		       x->stream_id, role_name(x->role),
		       stream_state_name(x->state), fb, vb, tb,
		       SAFE(x->name, nb));
		printf(" (%s, %s, pid %u)\n", SAFE(x->app, ab),
		       user_name(x->uid, ub, sizeof(ub)), x->pid);
	}
}

static void print_stream_json(const struct aud_stream_entry *x)
{
	printf("{\"id\":%u,\"client\":%u,\"uid\":%u,\"pid\":%u,"
	       "\"role\":\"%s\",\"format\":\"%s\",\"rate\":%u,"
	       "\"channels\":%u,\"left\":%u,\"right\":%u,\"mute\":%s,"
	       "\"state\":\"%s\",\"played_frames\":%llu,\"name\":",
	       x->stream_id, x->client_id, x->uid, x->pid, role_name(x->role),
	       fmt_name(x->format), x->rate, x->channels, x->left, x->right,
	       x->mute ? "true" : "false", stream_state_name(x->state),
	       (unsigned long long)x->played_frames);
	json_str(x->name, sizeof(x->name));
	printf(",\"app\":");
	json_str(x->app, sizeof(x->app));
	putchar('}');
}

static void print_streams_json(const struct aud_stream_entry *e, int n)
{
	int i;

	putchar('[');
	for (i = 0; i < n; i++) {
		if (i)
			putchar(',');
		print_stream_json(&e[i]);
	}
	putchar(']');
}

static void print_cards_text(const struct aud_cards *c)
{
	uint32_t i;
	char nb[80], cb[80], db[40];

	if (c->count == 0) {
		printf("(no sound cards)\n");
		return;
	}
	for (i = 0; i < c->count && i < AUD_MAX_CARDS; i++) {
		const struct aud_card_entry *x = &c->card[i];
		printf("%c %u  %-28s %-8s %s\n", x->in_use ? '*' : ' ', x->index,
		       SAFE(x->name, nb), SAFE(x->driver, db), SAFE(x->codec, cb));
	}
}

static void print_cards_json(const struct aud_cards *c)
{
	uint32_t i;

	putchar('[');
	for (i = 0; i < c->count && i < AUD_MAX_CARDS; i++) {
		const struct aud_card_entry *x = &c->card[i];
		if (i)
			putchar(',');
		printf("{\"index\":%u,\"in_use\":%s,\"name\":", x->index,
		       x->in_use ? "true" : "false");
		json_str(x->name, sizeof(x->name));
		printf(",\"codec\":");
		json_str(x->codec, sizeof(x->codec));
		printf(",\"driver\":");
		json_str(x->driver, sizeof(x->driver));
		putchar('}');
	}
	putchar(']');
}

static void print_info_json(const struct aud_server_info *i)
{
	printf("{\"version\":%u,\"rate\":%u,\"channels\":%u,\"format\":\"%s\","
	       "\"period_frames\":%u,\"periods\":%u,\"card\":%u,\"streams\":%u,"
	       "\"uptime_ms\":%llu,\"underruns\":%llu,\"card_name\":",
	       i->version, i->rate, i->channels, fmt_name(i->format),
	       i->period_frames, i->periods, i->card, i->streams,
	       (unsigned long long)i->uptime_ms,
	       (unsigned long long)i->underruns);
	json_str(i->card_name, sizeof(i->card_name));
	printf(",\"codec\":");
	json_str(i->codec, sizeof(i->codec));
	printf(",\"driver\":");
	json_str(i->driver, sizeof(i->driver));
	putchar('}');
}

static void print_volume_json(uint32_t l, uint32_t r, int mute)
{
	printf("{\"volume\":%u,\"left\":%u,\"right\":%u,\"mute\":%s}\n",
	       avg_vol(l, r), l, r, mute ? "true" : "false");
}

static void print_volume_text(uint32_t l, uint32_t r, int have_mute, int mute)
{
	char bar[32];

	vol_bar(avg_vol(l, r), bar, sizeof(bar));
	if (l == r)
		printf("Volume: %3u%% %s", l, bar);
	else
		printf("Volume: %3u%% %s (left %u%%, right %u%%)", avg_vol(l, r),
		       bar, l, r);
	if (have_mute && mute)
		printf("  [muted]");
	putchar('\n');
}

/* ---- commands --------------------------------------------------------------- */

static int cmd_status(void)
{
	struct snapshot *s = calloc(1, sizeof(*s));
	char b1[80], b2[80], b3[40];
	const struct aud_output *act;

	if (!s) {
		fprintf(stderr, "%s: out of memory\n", prog);
		return 1;
	}
	take_snapshot(s);
	if (!s->have_info && !s->have_vol) {
		fprintf(stderr, "%s: the audio service does not answer\n", prog);
		free(s);
		return 1;
	}
	if (opt_quiet) {
		free(s);
		return 0;
	}
	if (opt_json) {
		printf("{\"server\":");
		if (s->have_info)
			print_info_json(&s->info);
		else
			printf("null");
		if (s->have_vol)
			printf(",\"volume\":%u,\"left\":%u,\"right\":%u",
			       avg_vol(s->left, s->right), s->left, s->right);
		if (s->have_mute)
			printf(",\"mute\":%s", s->mute ? "true" : "false");
		printf(",\"outputs\":");
		if (s->have_out)
			print_outputs_json(&s->outs);
		else
			printf("null");
		printf(",\"streams\":");
		if (s->nstreams >= 0)
			print_streams_json(s->streams, s->nstreams);
		else
			printf("null");
		printf(",\"cards\":");
		if (s->have_cards)
			print_cards_json(&s->cards);
		else
			printf("null");
		printf("}\n");
		free(s);
		return 0;
	}

	if (s->have_info) {
		printf("Card:    %s", s->info.card_name[0] ?
		       SAFE(s->info.card_name, b1) : "(none)");
		if (s->info.driver[0])
			printf(" [%s, card %u]", SAFE(s->info.driver, b3),
			       s->info.card);
		putchar('\n');
		if (s->info.codec[0])
			printf("Codec:   %s\n", SAFE(s->info.codec, b2));
		printf("Mixer:   %u Hz, %u channels, %s\n", s->info.rate,
		       s->info.channels, fmt_name(s->info.format));
	}
	if (s->have_vol) {
		char bar[32];
		vol_bar(avg_vol(s->left, s->right), bar, sizeof(bar));
		printf("Volume:  %u%% %s", avg_vol(s->left, s->right), bar);
		if (s->left != s->right)
			printf(" (left %u%%, right %u%%)", s->left, s->right);
		putchar('\n');
	}
	if (s->have_mute)
		printf("Mute:    %s\n", s->mute ? "yes" : "no");
	if (s->have_out) {
		act = active_output(&s->outs);
		printf("Output:  %s", outsel_name(s->outs.mode));
		if (act)
			printf(" -> %s", SAFE(act->name, b1));
		else
			printf(" -> (none active)");
		putchar('\n');
		printf("Outputs:\n");
		print_outputs_text(&s->outs, 2);
	}
	if (s->nstreams >= 0) {
		printf("Streams: %d\n", s->nstreams);
		if (s->nstreams > 0)
			print_streams_text(s->streams, s->nstreams, 2);
	}
	if (s->have_cards && s->cards.count > 1)
		printf("Cards:   %u (see '%s cards')\n", s->cards.count, prog);
	free(s);
	return 0;
}

/* Parse a volume argument against the current level.
 *   N, N%      absolute, both sides
 *   +N, -N     relative, both sides (balance kept as far as possible)
 *   L,R / L:R  absolute per side
 *   up, down   VOLUME_STEP
 *   max, min   100 / 0
 * Returns 0 and the new pair, or -1 on a malformed argument. */
static int parse_volume_arg(const char *arg, uint32_t cur_l, uint32_t cur_r,
			    uint32_t *nl, uint32_t *nr)
{
	char buf[32];
	size_t len = strlen(arg);
	const char *sep;
	uint32_t a, b;
	int sign = 0;

	if (len == 0 || len >= sizeof(buf))
		return -1;
	if (!strcasecmp(arg, "up")) {
		*nl = clamp_vol((long)cur_l + VOLUME_STEP);
		*nr = clamp_vol((long)cur_r + VOLUME_STEP);
		return 0;
	}
	if (!strcasecmp(arg, "down")) {
		*nl = clamp_vol((long)cur_l - VOLUME_STEP);
		*nr = clamp_vol((long)cur_r - VOLUME_STEP);
		return 0;
	}
	if (!strcasecmp(arg, "max") || !strcasecmp(arg, "full")) {
		*nl = *nr = 100;
		return 0;
	}
	if (!strcasecmp(arg, "min") || !strcasecmp(arg, "off")) {
		*nl = *nr = 0;
		return 0;
	}

	memcpy(buf, arg, len + 1);
	sep = strpbrk(buf, ",:");
	if (sep) {
		char *rs = (char *)sep + 1;
		*(char *)sep = 0;
		if (buf[0] && buf[strlen(buf) - 1] == '%')
			buf[strlen(buf) - 1] = 0;
		if (*rs && rs[strlen(rs) - 1] == '%')
			rs[strlen(rs) - 1] = 0;
		if (parse_uint(buf, 100, &a) || parse_uint(rs, 100, &b))
			return -1;
		*nl = a;
		*nr = b;
		return 0;
	}

	if (buf[len - 1] == '%')
		buf[--len] = 0;
	if (buf[0] == '+' || buf[0] == '-') {
		sign = buf[0] == '+' ? 1 : -1;
		if (parse_uint(buf + 1, 100, &a))
			return -1;
		*nl = clamp_vol((long)cur_l + sign * (long)a);
		*nr = clamp_vol((long)cur_r + sign * (long)a);
		return 0;
	}
	if (parse_uint(buf, 100, &a))
		return -1;
	*nl = *nr = a;
	return 0;
}

static int cmd_volume(int argc, char **argv)
{
	uint32_t l, r, nl, nr;
	int mute = 0, have_mute, err;

	err = la_get_volume(ctl, &l, &r);
	if (err < 0)
		return fail(err, "reading the volume");
	have_mute = la_get_mute(ctl, &mute) >= 0;

	if (argc < 1) {
		if (opt_quiet)
			return 0;
		if (opt_json)
			print_volume_json(l, r, mute);
		else
			print_volume_text(l, r, have_mute, mute);
		return 0;
	}
	if (argc > 1) {
		fprintf(stderr, "%s: volume takes one argument\n", prog);
		return 2;
	}
	if (parse_volume_arg(argv[0], l, r, &nl, &nr)) {
		fprintf(stderr,
			"%s: bad volume '%s' (use 0..100, +N, -N, N%%, L,R, up or down)\n",
			prog, argv[0]);
		return 2;
	}
	err = la_set_volume_master(ctl, nl, nr);
	if (err < 0)
		return fail(err, "setting the volume");
	/* Turning the volume up while muted would otherwise look broken. */
	if (have_mute && mute && (nl > l || nr > r)) {
		if (la_set_mute_master(ctl, 0) >= 0)
			mute = 0;
	}
	/* Read back what the service actually applied (it may round). */
	if (la_get_volume(ctl, &l, &r) < 0) {
		l = nl;
		r = nr;
	}
	feedback_sound();
	if (opt_quiet)
		return 0;
	if (opt_json)
		print_volume_json(l, r, mute);
	else
		print_volume_text(l, r, have_mute, mute);
	return 0;
}

static int cmd_mute(int which) /* 1 mute, 0 unmute, -1 toggle */
{
	int mute = 0, err;
	uint32_t l = 0, r = 0;

	err = la_get_mute(ctl, &mute);
	if (err < 0)
		return fail(err, "reading the mute state");
	if (which < 0)
		which = !mute;
	if (which != mute) {
		err = la_set_mute_master(ctl, which);
		if (err < 0)
			return fail(err, which ? "muting" : "unmuting");
	}
	mute = which;
	if (!mute)
		feedback_sound();
	if (opt_quiet)
		return 0;
	(void)la_get_volume(ctl, &l, &r);
	if (opt_json)
		print_volume_json(l, r, mute);
	else
		printf("%s (volume %u%%)\n", mute ? "Muted" : "Unmuted",
		       avg_vol(l, r));
	return 0;
}

static int type_from_name(const char *s, uint32_t *type)
{
	static const struct {
		const char *name;
		uint32_t type;
	} map[] = {
		{ "speaker", AUDIO_OUT_SPEAKER },
		{ "speakers", AUDIO_OUT_SPEAKER },
		{ "internal", AUDIO_OUT_SPEAKER },
		{ "headphone", AUDIO_OUT_HEADPHONE },
		{ "headphones", AUDIO_OUT_HEADPHONE },
		{ "hp", AUDIO_OUT_HEADPHONE },
		{ "line", AUDIO_OUT_LINE },
		{ "lineout", AUDIO_OUT_LINE },
		{ "line-out", AUDIO_OUT_LINE },
		{ "hdmi", AUDIO_OUT_HDMI },
		{ "dp", AUDIO_OUT_DP },
		{ "displayport", AUDIO_OUT_DP },
		{ "spdif", AUDIO_OUT_SPDIF },
		{ "s/pdif", AUDIO_OUT_SPDIF },
		{ "optical", AUDIO_OUT_SPDIF },
		{ "headset", AUDIO_OUT_HEADSET },
	};
	size_t i;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcasecmp(s, map[i].name)) {
			*type = map[i].type;
			return 0;
		}
	return -1;
}

/* Pick the output of a type: the first one with something plugged in,
 * otherwise the first of that type.  "hdmi" also accepts a DisplayPort
 * sink when there is no HDMI one -- both are "the monitor" to a user. */
static const struct aud_output *find_by_type(const struct aud_outputs *o,
					     uint32_t type)
{
	const struct aud_output *first = NULL;
	uint32_t i, pass;

	for (pass = 0; pass < 2; pass++) {
		uint32_t t = type;
		if (pass == 1) {
			if (type == AUDIO_OUT_HDMI)
				t = AUDIO_OUT_DP;
			else if (type == AUDIO_OUT_DP)
				t = AUDIO_OUT_HDMI;
			else if (type == AUDIO_OUT_HEADPHONE)
				t = AUDIO_OUT_HEADSET;
			else
				break;
		}
		for (i = 0; i < o->count && i < AUD_MAX_OUTPUTS; i++) {
			if (o->out[i].type != t)
				continue;
			if (o->out[i].present)
				return &o->out[i];
			if (!first)
				first = &o->out[i];
		}
		if (first)
			return first;
	}
	return first;
}

static int cmd_output(int argc, char **argv)
{
	struct aud_outputs o;
	const struct aud_output *x = NULL, *act;
	uint32_t mode, id = 0, type, i;
	int err;
	char nb[40];

	err = la_get_outputs(ctl, &o);
	if (err < 0)
		return fail(err, "reading the outputs");
	if (o.count > AUD_MAX_OUTPUTS)
		o.count = AUD_MAX_OUTPUTS;

	if (argc < 1) {
		if (opt_quiet)
			return 0;
		if (opt_json) {
			print_outputs_json(&o);
			putchar('\n');
			return 0;
		}
		act = active_output(&o);
		printf("Output mode: %s\n", outsel_name(o.mode));
		printf("Active:      %s\n", act ? SAFE(act->name, nb) : "(none)");
		return 0;
	}
	if (argc > 1) {
		fprintf(stderr, "%s: output takes one argument\n", prog);
		return 2;
	}

	if (!strcasecmp(argv[0], "auto")) {
		mode = AUDIO_OUTSEL_AUTO;
	} else if (!strcasecmp(argv[0], "all") || !strcasecmp(argv[0], "both")) {
		mode = AUDIO_OUTSEL_ALL;
	} else if (!type_from_name(argv[0], &type)) {
		x = find_by_type(&o, type);
		if (!x) {
			fprintf(stderr, "%s: this card has no %s output\n", prog,
				argv[0]);
			return 1;
		}
		mode = AUDIO_OUTSEL_ID;
		id = x->id;
	} else if (!parse_uint(argv[0], UINT32_MAX, &id)) {
		for (i = 0; i < o.count; i++)
			if (o.out[i].id == id)
				x = &o.out[i];
		if (!x) {
			fprintf(stderr, "%s: no output with id %u (see '%s outputs')\n",
				prog, id, prog);
			return 1;
		}
		mode = AUDIO_OUTSEL_ID;
	} else {
		/* An output's own name, e.g. "HDMI 1", case-insensitive. */
		for (i = 0; i < o.count; i++) {
			char n[sizeof(o.out[i].name) + 1];
			memcpy(n, o.out[i].name, sizeof(o.out[i].name));
			n[sizeof(o.out[i].name)] = 0;
			if (!strcasecmp(n, argv[0])) {
				x = &o.out[i];
				break;
			}
		}
		if (!x) {
			fprintf(stderr, "%s: unknown output '%s' (see '%s outputs')\n",
				prog, argv[0], prog);
			return 2;
		}
		mode = AUDIO_OUTSEL_ID;
		id = x->id;
	}
	if (x && !x->present && !opt_quiet)
		fprintf(stderr, "%s: warning: nothing is plugged into %s\n", prog,
			SAFE(x->name, nb));

	err = la_set_output(ctl, mode, id);
	if (err < 0)
		return fail(err, "choosing the output");
	feedback_sound();
	if (opt_quiet)
		return 0;
	if (la_get_outputs(ctl, &o) >= 0) {
		if (o.count > AUD_MAX_OUTPUTS)
			o.count = AUD_MAX_OUTPUTS;
		if (opt_json) {
			print_outputs_json(&o);
			putchar('\n');
		} else {
			act = active_output(&o);
			printf("Output: %s -> %s\n", outsel_name(o.mode),
			       act ? SAFE(act->name, nb) : "(none active)");
		}
	}
	return 0;
}

static int cmd_outputs(void)
{
	struct aud_outputs o;
	int err = la_get_outputs(ctl, &o);

	if (err < 0)
		return fail(err, "reading the outputs");
	if (o.count > AUD_MAX_OUTPUTS)
		o.count = AUD_MAX_OUTPUTS;
	if (opt_quiet)
		return 0;
	if (opt_json) {
		print_outputs_json(&o);
		putchar('\n');
		return 0;
	}
	printf("Mode: %s\n", outsel_name(o.mode));
	printf("    %2s  %-16s %-10s %s\n", "ID", "NAME", "TYPE", "JACK");
	print_outputs_text(&o, 2);
	return 0;
}

static int cmd_streams(void)
{
	struct aud_stream_entry *e = calloc(AUD_MAX_STREAMS, sizeof(*e));
	int n;

	if (!e) {
		fprintf(stderr, "%s: out of memory\n", prog);
		return 1;
	}
	n = la_list_streams(ctl, e, AUD_MAX_STREAMS);
	if (n < 0) {
		free(e);
		return fail(n, "listing streams");
	}
	if (n > AUD_MAX_STREAMS)
		n = AUD_MAX_STREAMS;
	if (!opt_quiet) {
		if (opt_json) {
			print_streams_json(e, n);
			putchar('\n');
		} else {
			print_streams_text(e, n, 0);
		}
	}
	free(e);
	return 0;
}

static int find_stream(uint32_t id, struct aud_stream_entry *out)
{
	struct aud_stream_entry *e = calloc(AUD_MAX_STREAMS, sizeof(*e));
	int n, i, found = 0;

	if (!e)
		return -ENOMEM;
	n = la_list_streams(ctl, e, AUD_MAX_STREAMS);
	if (n > AUD_MAX_STREAMS)
		n = AUD_MAX_STREAMS;
	for (i = 0; i < n; i++)
		if (e[i].stream_id == id) {
			*out = e[i];
			found = 1;
			break;
		}
	free(e);
	if (n < 0)
		return n;
	return found ? 0 : -ESRCH;
}

static int cmd_stream_volume(int argc, char **argv)
{
	struct aud_stream_entry cur;
	uint32_t id, l, r;
	int err;

	if (argc != 2) {
		fprintf(stderr, "%s: usage: stream-volume ID N[,R]\n", prog);
		return 2;
	}
	if (parse_uint(argv[0], UINT32_MAX, &id)) {
		fprintf(stderr, "%s: bad stream id '%s'\n", prog, argv[0]);
		return 2;
	}
	err = find_stream(id, &cur);
	if (err == -ESRCH) {
		fprintf(stderr, "%s: no stream %u (see '%s streams')\n", prog, id,
			prog);
		return 1;
	}
	if (err < 0)
		return fail(err, "listing streams");
	if (parse_volume_arg(argv[1], cur.left, cur.right, &l, &r)) {
		fprintf(stderr, "%s: bad volume '%s'\n", prog, argv[1]);
		return 2;
	}
	err = la_set_stream_volume(ctl, id, l, r);
	if (err < 0)
		return fail(err, "setting the stream volume");
	if (opt_quiet)
		return 0;
	if (opt_json)
		printf("{\"id\":%u,\"left\":%u,\"right\":%u}\n", id, l, r);
	else if (l == r)
		printf("Stream %u: volume %u%%\n", id, l);
	else
		printf("Stream %u: volume left %u%%, right %u%%\n", id, l, r);
	return 0;
}

static int cmd_kill(int argc, char **argv)
{
	uint32_t id;
	int err, i, rc = 0;

	if (argc < 1) {
		fprintf(stderr, "%s: usage: kill ID [ID...]\n", prog);
		return 2;
	}
	for (i = 0; i < argc; i++) {
		if (parse_uint(argv[i], UINT32_MAX, &id)) {
			fprintf(stderr, "%s: bad stream id '%s'\n", prog, argv[i]);
			rc = 2;
			continue;
		}
		err = la_kill_stream(ctl, id);
		if (err < 0) {
			char what[48];
			snprintf(what, sizeof(what), "ending stream %u", id);
			rc = fail(err, what);
			continue;
		}
		if (!opt_quiet && !opt_json)
			printf("Stream %u ended\n", id);
	}
	if (!opt_quiet && opt_json)
		printf("{\"ok\":%s}\n", rc ? "false" : "true");
	return rc;
}

static int cmd_cards(void)
{
	struct aud_cards c;
	int err = la_get_cards(ctl, &c);

	if (err < 0)
		return fail(err, "listing cards");
	if (c.count > AUD_MAX_CARDS)
		c.count = AUD_MAX_CARDS;
	if (opt_quiet)
		return 0;
	if (opt_json) {
		print_cards_json(&c);
		putchar('\n');
	} else {
		print_cards_text(&c);
	}
	return 0;
}

static int cmd_card(int argc, char **argv)
{
	struct aud_cards c;
	uint32_t idx, i;
	int err, known = 0;

	if (argc < 1) {
		struct aud_server_info info;
		err = la_get_server_info(ctl, &info);
		if (err < 0)
			return fail(err, "asking the service");
		if (!opt_quiet) {
			if (opt_json)
				printf("{\"card\":%u}\n", info.card);
			else
				printf("Card %u\n", info.card);
		}
		return 0;
	}
	if (parse_uint(argv[0], AUD_MAX_CARDS - 1, &idx)) {
		fprintf(stderr, "%s: bad card number '%s' (0..%d)\n", prog, argv[0],
			AUD_MAX_CARDS - 1);
		return 2;
	}
	if (la_get_cards(ctl, &c) >= 0) {
		if (c.count > AUD_MAX_CARDS)
			c.count = AUD_MAX_CARDS;
		for (i = 0; i < c.count; i++)
			if (c.card[i].index == idx)
				known = 1;
		if (!known) {
			fprintf(stderr, "%s: there is no card %u (see '%s cards')\n",
				prog, idx, prog);
			return 1;
		}
	}
	err = la_set_card(ctl, idx);
	if (err == -EPERM || err == -EACCES) {
		fprintf(stderr, "%s: only root may change the sound card\n", prog);
		return 1;
	}
	if (err < 0)
		return fail(err, "changing the card");
	feedback_sound();
	if (!opt_quiet) {
		if (opt_json)
			printf("{\"card\":%u}\n", idx);
		else
			printf("Using card %u\n", idx);
	}
	return 0;
}

static int cmd_info(void)
{
	struct aud_server_info i;
	struct aud_cards c;
	char b1[80], b2[80], b3[40], up[48];
	uint64_t latency_us = 0;
	int err, have_cards;

	err = la_get_server_info(ctl, &i);
	if (err < 0)
		return fail(err, "asking the service");
	have_cards = la_get_cards(ctl, &c) >= 0;
	if (have_cards && c.count > AUD_MAX_CARDS)
		c.count = AUD_MAX_CARDS;
	if (opt_quiet)
		return 0;
	if (opt_json) {
		printf("{\"library\":\"%d.%d\",\"server\":", LA_VERSION_MAJOR,
		       LA_VERSION_MINOR);
		print_info_json(&i);
		printf(",\"cards\":");
		if (have_cards)
			print_cards_json(&c);
		else
			printf("null");
		printf("}\n");
		return 0;
	}
	if (i.rate)
		latency_us = (uint64_t)i.period_frames * i.periods * 1000000ull /
			     i.rate;
	fmt_duration_ms(i.uptime_ms, up, sizeof(up));
	printf("Audio service\n");
	printf("  protocol version   %u (library %d.%d)\n", i.version,
	       LA_VERSION_MAJOR, LA_VERSION_MINOR);
	printf("  socket             %s\n", AUDIOD_SOCKET_PATH);
	printf("  uptime             %s\n", up);
	printf("  streams open       %u\n", i.streams);
	printf("  device underruns   %llu\n", (unsigned long long)i.underruns);
	printf("Mixer\n");
	printf("  sample rate        %u Hz\n", i.rate);
	printf("  channels           %u\n", i.channels);
	printf("  device format      %s\n", fmt_name(i.format));
	printf("  period             %u frames x %u periods", i.period_frames,
	       i.periods);
	if (latency_us)
		printf(" (%llu.%01llu ms device buffer)",
		       (unsigned long long)(latency_us / 1000),
		       (unsigned long long)((latency_us % 1000) / 100));
	putchar('\n');
	printf("Card %u\n", i.card);
	printf("  name               %s\n", SAFE(i.card_name, b1));
	printf("  driver             %s\n", SAFE(i.driver, b3));
	printf("  codec              %s\n", SAFE(i.codec, b2));
	if (have_cards && c.count > 1) {
		printf("All cards\n");
		print_cards_text(&c);
	}
	return 0;
}

/* ---- watch ----------------------------------------------------------------- */

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void timestamp(char *buf, size_t sz)
{
	struct timespec ts;
	struct tm tm;
	time_t t;

	clock_gettime(CLOCK_REALTIME, &ts);
	t = ts.tv_sec;
	if (localtime_r(&t, &tm))
		snprintf(buf, sz, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min,
			 tm.tm_sec, (long)(ts.tv_nsec / 1000000));
	else
		snprintf(buf, sz, "%lld", (long long)t);
}

static void describe_output(uint32_t id, char *buf, size_t sz)
{
	struct aud_outputs o;
	uint32_t i;
	char nb[40];

	snprintf(buf, sz, "output %u", id);
	if (la_get_outputs(ctl, &o) < 0)
		return;
	for (i = 0; i < o.count && i < AUD_MAX_OUTPUTS; i++)
		if (o.out[i].id == id) {
			snprintf(buf, sz, "%s (%s, id %u)", SAFE(o.out[i].name, nb),
				 out_type_name(o.out[i].type), id);
			return;
		}
}

static void describe_stream(uint32_t id, char *buf, size_t sz)
{
	struct aud_stream_entry e;
	char nb[80], ab[80];

	if (find_stream(id, &e) == 0)
		snprintf(buf, sz, "stream %u \"%s\" (%s, %s, %u Hz %u ch)", id,
			 SAFE(e.name, nb), SAFE(e.app, ab), role_name(e.role),
			 e.rate, e.channels);
	else
		snprintf(buf, sz, "stream %u", id);
}

static void print_notify(const struct aud_notify *n)
{
	char ts[32], d[200];

	timestamp(ts, sizeof(ts));
	if (opt_json) {
		const char *what = "unknown";
		switch (n->what) {
		case AUD_NOTIFY_VOLUME: what = "volume"; break;
		case AUD_NOTIFY_OUTPUT: what = "output"; break;
		case AUD_NOTIFY_STREAM_NEW: what = "stream-new"; break;
		case AUD_NOTIFY_STREAM_GONE: what = "stream-gone"; break;
		case AUD_NOTIFY_CARD: what = "card"; break;
		}
		printf("{\"time\":\"%s\",\"event\":\"%s\",\"a\":%u,\"b\":%u,\"c\":%u}\n",
		       ts, what, n->a, n->b, n->c);
		fflush(stdout);
		return;
	}
	switch (n->what) {
	case AUD_NOTIFY_VOLUME:
		if (n->a == n->b)
			printf("%s volume %u%%%s\n", ts, n->a, n->c ? " (muted)" : "");
		else
			printf("%s volume left %u%% right %u%%%s\n", ts, n->a, n->b,
			       n->c ? " (muted)" : "");
		break;
	case AUD_NOTIFY_OUTPUT: {
		uint32_t bit;
		int first = 1;
		describe_output(n->a, d, sizeof(d));
		printf("%s output now %s; plugged:", ts, d);
		for (bit = 0; bit < 32; bit++)
			if (n->b & (1u << bit)) {
				printf("%s%u", first ? " " : ",", bit);
				first = 0;
			}
		if (first)
			printf(" none");
		putchar('\n');
		break;
	}
	case AUD_NOTIFY_STREAM_NEW:
		describe_stream(n->a, d, sizeof(d));
		printf("%s new %s\n", ts, d);
		break;
	case AUD_NOTIFY_STREAM_GONE:
		printf("%s stream %u ended\n", ts, n->a);
		break;
	case AUD_NOTIFY_CARD:
		printf("%s card %u %s\n", ts, n->a, n->b ? "appeared" : "went away");
		break;
	default:
		printf("%s notification %u (%u, %u, %u)\n", ts, n->what, n->a, n->b,
		       n->c);
	}
	fflush(stdout);
}

static int cmd_watch(int argc, char **argv)
{
	struct sigaction sa;
	struct pollfd pfd;
	struct aud_notify n;
	uint32_t limit = 0, seen = 0;
	int err, fd;

	if (argc > 0 && parse_uint(argv[0], UINT32_MAX, &limit)) {
		fprintf(stderr, "%s: bad count '%s'\n", prog, argv[0]);
		return 2;
	}
	err = la_subscribe(ctl, AUD_SUB_VOLUME | AUD_SUB_OUTPUTS |
				       AUD_SUB_STREAMS | AUD_SUB_CARDS);
	if (err < 0)
		return fail(err, "subscribing");
	fd = la_ctl_fd(ctl);
	if (fd < 0)
		return fail(fd, "subscribing");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART: poll() must return */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (!opt_quiet && !opt_json) {
		printf("Watching the audio service (Ctrl-C to stop)\n");
		fflush(stdout);
	}

	while (!stop_requested) {
		/* Drain what is already queued before sleeping: the library may
		 * have buffered notifications while answering a request. */
		for (;;) {
			err = la_next_notify(ctl, &n);
			if (err < 0) {
				if (err == -EINTR)
					break;
				if (!opt_quiet)
					fprintf(stderr, "%s: the audio service went away (%s)\n",
						prog, la_strerror(err));
				return 1;
			}
			if (err == 0)
				break;
			if (!opt_quiet)
				print_notify(&n);
			if (limit && ++seen >= limit)
				return 0;
		}
		if (stop_requested)
			break;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		err = poll(&pfd, 1, -1);
		if (err < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "%s: poll: %s\n", prog, strerror(errno));
			return 1;
		}
		if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) &&
		    !(pfd.revents & POLLIN)) {
			if (!opt_quiet)
				fprintf(stderr, "%s: the audio service went away\n", prog);
			return 1;
		}
	}
	return 0;
}

/* ---- main ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *cmd;
	int i, err, rc;
	char **rest;
	int nrest;

	if (argv[0] && argv[0][0]) {
		const char *s = strrchr(argv[0], '/');
		prog = s ? s + 1 : argv[0];
	}
	signal(SIGPIPE, SIG_IGN);

	/* Options come before the command; anything after the command is its
	 * arguments, so "audioctl volume -5" means relative, not an option. */
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] != '-' || !a[1])
			break;
		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
			opt_quiet = 1;
		} else if (!strcmp(a, "-j") || !strcmp(a, "--json")) {
			opt_json = 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
			printf("%s (LikeOS) libaudio %d.%d, protocol %d\n", prog,
			       LA_VERSION_MAJOR, LA_VERSION_MINOR,
			       AUDIOD_PROTOCOL_VERSION);
			return 0;
		} else if (!strcmp(a, "-qj") || !strcmp(a, "-jq")) {
			opt_quiet = opt_json = 1;
		} else {
			fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
			usage(stderr);
			return 2;
		}
	}
	cmd = i < argc ? argv[i++] : "status";
	rest = argv + i;
	nrest = argc - i;

	if (!strcmp(cmd, "help")) {
		usage(stdout);
		return 0;
	}

	ctl = la_ctl_open(prog, &err);
	if (!ctl) {
		fprintf(stderr, "%s: cannot reach the audio service at %s: %s\n",
			prog, AUDIOD_SOCKET_PATH, la_strerror(err));
		if (err == -ENOENT || err == -ECONNREFUSED)
			fprintf(stderr, "%s: is audiod running? (it is started by init "
				"when a sound card was found)\n", prog);
		return 1;
	}

	if (!strcmp(cmd, "status"))
		rc = cmd_status();
	else if (!strcmp(cmd, "volume") || !strcmp(cmd, "vol"))
		rc = cmd_volume(nrest, rest);
	else if (!strcmp(cmd, "mute"))
		rc = cmd_mute(1);
	else if (!strcmp(cmd, "unmute"))
		rc = cmd_mute(0);
	else if (!strcmp(cmd, "toggle"))
		rc = cmd_mute(-1);
	else if (!strcmp(cmd, "output"))
		rc = cmd_output(nrest, rest);
	else if (!strcmp(cmd, "outputs"))
		rc = cmd_outputs();
	else if (!strcmp(cmd, "streams"))
		rc = cmd_streams();
	else if (!strcmp(cmd, "stream-volume"))
		rc = cmd_stream_volume(nrest, rest);
	else if (!strcmp(cmd, "kill"))
		rc = cmd_kill(nrest, rest);
	else if (!strcmp(cmd, "cards"))
		rc = cmd_cards();
	else if (!strcmp(cmd, "card"))
		rc = cmd_card(nrest, rest);
	else if (!strcmp(cmd, "info"))
		rc = cmd_info();
	else if (!strcmp(cmd, "watch"))
		rc = cmd_watch(nrest, rest);
	else {
		fprintf(stderr, "%s: unknown command '%s'\n", prog, cmd);
		usage(stderr);
		rc = 2;
	}

	la_ctl_close(ctl);
	fflush(stdout);
	return rc;
}
