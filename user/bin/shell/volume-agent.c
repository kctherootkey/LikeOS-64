// likeos-volume-agent -- mounts USB drives for the desktop session.
//
// Started from the X session (xinitrc).  It talks to the device manager,
// devd, over /var/run/devd/socket: HELLO, SUBSCRIBE, LIST, and then follows
// the device events.  A USB volume that appears and that LikeOS can open
// is mounted for the user (devd decides whether this user may: the one
// logged in at the console); its folder is /media/<user>/<label>.  Each
// drive plugged in plays the "device-added" sound once -- when its first
// volume is mounted, or shortly after it appears when it has nothing to
// mount -- and "device-removed" when it goes away.  Drives that are already
// there when the agent starts are mounted without a sound, and a volume is
// mounted automatically only once: one the user unmounted or ejected stays
// that way.
//
// It leaves when the session does (its parent, the session script, is
// gone), and comes back to devd with growing pauses when devd restarts.
//
// Copyright (C) 2026 The LikeOS Project

#include <audio/audio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "devproto.h"

#define MAX_VOLS 128
#define MAX_DISKS 32
#define MAX_PENDING 64
#define NAME_MAX_ 32
#define SOUND_DELAY_MS 1500
#define BACKOFF_MAX_MS 30000

struct info {
	char name[NAME_MAX_], disk[NAME_MAX_], kind[16], fstype[16];
	char state[16];
	int usb, removable, system, supported, ejected, mounted;
};

struct vol {
	int used;
	int handled; /* mounted automatically once (or found mounted) */
	int seen;
	char name[NAME_MAX_];
	char disk[NAME_MAX_];
};

struct disk {
	int used;
	int seen;
	int usb; /* a removable USB drive of the user's */
	int sound_pending;
	int inflight;
	uint64_t deadline;
	char name[NAME_MAX_];
};

struct req {
	char verb[12];
	char name[NAME_MAX_];
};

static struct vol g_vols[MAX_VOLS];
static struct disk g_disks[MAX_DISKS];
static struct req g_q[MAX_PENDING];
static int g_qhead, g_qlen;
static int g_fd = -1;
static char g_in[DEVP_LINE_MAX * 2];
static size_t g_inlen;
static int g_started; /* the first LIST is done: what comes now is new */
static volatile sig_atomic_t g_quit;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

static void copy(char *dst, const char *src, size_t cap)
{
	size_t n = strlen(src);

	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void play(const char *sound)
{
	la_play_event_simple(sound, 0);
}

/* ---- tables ---------------------------------------------------------------------- */

static struct disk *disk_get(const char *name, int create)
{
	struct disk *free_slot = NULL;

	for (int i = 0; i < MAX_DISKS; i++) {
		if (g_disks[i].used && strcmp(g_disks[i].name, name) == 0)
			return &g_disks[i];
		if (!g_disks[i].used && !free_slot)
			free_slot = &g_disks[i];
	}
	if (!create || !free_slot)
		return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = 1;
	copy(free_slot->name, name, sizeof(free_slot->name));
	return free_slot;
}

static struct vol *vol_get(const char *name, int create)
{
	struct vol *free_slot = NULL;

	for (int i = 0; i < MAX_VOLS; i++) {
		if (g_vols[i].used && strcmp(g_vols[i].name, name) == 0)
			return &g_vols[i];
		if (!g_vols[i].used && !free_slot)
			free_slot = &g_vols[i];
	}
	if (!create || !free_slot)
		return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = 1;
	copy(free_slot->name, name, sizeof(free_slot->name));
	return free_slot;
}

static void disk_forget(const char *name)
{
	struct disk *d = disk_get(name, 0);

	if (d)
		d->used = 0;
	for (int i = 0; i < MAX_VOLS; i++)
		if (g_vols[i].used && strcmp(g_vols[i].disk, name) == 0)
			g_vols[i].used = 0;
}

/* ---- the connection -------------------------------------------------------------- */

static void disconnect(void)
{
	if (g_fd >= 0)
		close(g_fd);
	g_fd = -1;
	g_inlen = 0;
	g_qhead = g_qlen = 0;
	for (int i = 0; i < MAX_DISKS; i++)
		g_disks[i].inflight = 0;
}

static int send_req(const char *verb, const char *name)
{
	char line[128], enc[NAME_MAX_ * 3 + 1];
	size_t n, off = 0;
	struct req *r;

	if (g_fd < 0 || g_qlen >= MAX_PENDING)
		return -1;
	if (name) {
		if (devp_encode(enc, sizeof(enc), name, strlen(name)) >=
		    sizeof(enc))
			return -1;
		snprintf(line, sizeof(line), "%s %s\n", verb, enc);
	} else {
		snprintf(line, sizeof(line), "%s\n", verb);
	}
	n = strlen(line);
	while (off < n) {
		ssize_t w = send(g_fd, line + off, n - off, MSG_NOSIGNAL);

		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd p = { g_fd, POLLOUT, 0 };
			if (poll(&p, 1, 1000) <= 0) {
				disconnect();
				return -1;
			}
			continue;
		}
		if (w <= 0) {
			disconnect();
			return -1;
		}
		off += (size_t)w;
	}
	r = &g_q[(g_qhead + g_qlen) % MAX_PENDING];
	copy(r->verb, verb, sizeof(r->verb));
	copy(r->name, name ? name : "", sizeof(r->name));
	g_qlen++;
	return 0;
}

static int try_connect(void)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	copy(sa.sun_path, DEVP_SOCKET_PATH, sizeof(sa.sun_path));
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	g_fd = fd;
	/* the LIST reply marks what still exists */
	for (int i = 0; i < MAX_VOLS; i++)
		g_vols[i].seen = 0;
	for (int i = 0; i < MAX_DISKS; i++)
		g_disks[i].seen = 0;
	if (send_req("HELLO", "1") < 0 || send_req("SUBSCRIBE", NULL) < 0 ||
	    send_req("LIST", NULL) < 0) {
		disconnect();
		return -1;
	}
	return 0;
}

/* ---- lines from devd ------------------------------------------------------------- */

static int parse_info(char **tok, int n, int first, struct info *in)
{
	static const char *const keys[] = { "name", "disk", "kind", "fstype",
					    "state" };
	char *dst[] = { in->name, in->disk, in->kind, in->fstype, in->state };
	size_t cap[] = { sizeof(in->name), sizeof(in->disk), sizeof(in->kind),
			 sizeof(in->fstype), sizeof(in->state) };
	const char *v;
	char tmp[DEVP_LINE_MAX];

	memset(in, 0, sizeof(*in));
	for (int i = 0; i < 5; i++) {
		v = devp_find(tok, n, first, keys[i], NULL);
		if (!v || devp_decode(dst[i], cap[i], v) < 0)
			return -1;
	}
#define FLAG(key, field)                                         \
	v = devp_find(tok, n, first, key, NULL);                 \
	in->field = v && strcmp(v, "1") == 0;
	FLAG("usb", usb)
	FLAG("removable", removable)
	FLAG("system", system)
	FLAG("supported", supported)
	FLAG("ejected", ejected)
#undef FLAG
	v = devp_find(tok, n, first, "mounted", NULL);
	in->mounted = v && devp_decode(tmp, sizeof(tmp), v) > 0;
	return in->name[0] && in->disk[0] ? 0 : -1;
}

static int is_volume(const struct info *in)
{
	return strcmp(in->kind, "partition") == 0 ||
	       strcmp(in->fstype, "none") != 0;
}

static int user_drive(const struct info *in)
{
	return in->usb && in->removable && !in->system;
}

/* A device line from LIST (startup = 1: silently) or an ADDED/CHANGED. */
static void device_seen(const struct info *in, int startup)
{
	struct disk *d;
	struct vol *v;

	if (strcmp(in->kind, "disk") == 0) {
		d = disk_get(in->name, 1);
		if (!d)
			return;
		d->seen = 1;
		if (user_drive(in) && !d->usb) {
			d->usb = 1;
			if (!startup && g_started) {
				d->sound_pending = 1;
				d->deadline = now_ms() + SOUND_DELAY_MS;
			}
		}
	}
	if (!is_volume(in))
		return;
	v = vol_get(in->name, 1);
	if (!v)
		return;
	v->seen = 1;
	copy(v->disk, in->disk, sizeof(v->disk));
	if (in->mounted) {
		v->handled = 1; /* never mount it again by itself */
		return;
	}
	if (v->handled || !user_drive(in) || !in->supported || in->ejected ||
	    strcmp(in->state, "ready") != 0)
		return;
	v->handled = 1;
	if (send_req("MOUNT", in->name) == 0) {
		d = disk_get(in->disk, 1);
		if (d)
			d->inflight++;
	}
}

static void device_removed(const struct info *in)
{
	if (strcmp(in->kind, "disk") == 0) {
		struct disk *d = disk_get(in->name, 0);

		if (d && d->usb)
			play("device-removed");
		disk_forget(in->name);
	} else {
		struct vol *v = vol_get(in->name, 0);

		if (v)
			v->used = 0;
	}
}

static void reply_done(const struct req *r, int ok)
{
	struct vol *v;
	struct disk *d;

	if (strcmp(r->verb, "LIST") == 0) {
		/* forget what devd no longer has */
		for (int i = 0; i < MAX_VOLS; i++)
			if (g_vols[i].used && !g_vols[i].seen)
				g_vols[i].used = 0;
		for (int i = 0; i < MAX_DISKS; i++)
			if (g_disks[i].used && !g_disks[i].seen)
				g_disks[i].used = 0;
		g_started = 1;
		return;
	}
	if (strcmp(r->verb, "MOUNT") != 0)
		return;
	v = vol_get(r->name, 0);
	d = v ? disk_get(v->disk, 0) : NULL;
	if (!d)
		return;
	if (d->inflight > 0)
		d->inflight--;
	if (ok && d->sound_pending) {
		d->sound_pending = 0;
		play("device-added");
	}
}

static void handle_line(char *line)
{
	char *tok[DEVP_MAX_TOKENS];
	int n = devp_split(line, tok, DEVP_MAX_TOKENS);
	struct info in;

	if (n <= 0)
		return;
	if (strcmp(tok[0], "OK") == 0 || strcmp(tok[0], "ERR") == 0) {
		struct req r;

		if (!g_qlen)
			return;
		r = g_q[g_qhead];
		g_qhead = (g_qhead + 1) % MAX_PENDING;
		g_qlen--;
		reply_done(&r, tok[0][0] == 'O');
		return;
	}
	if (strcmp(tok[0], "DEV") == 0) {
		/* only a LIST reply has these: what is there already, so
		 * no sound */
		if (parse_info(tok, n, 1, &in) == 0)
			device_seen(&in, 1);
		return;
	}
	if (strcmp(tok[0], "EVENT") == 0 && n >= 3 &&
	    parse_info(tok, n, 2, &in) == 0) {
		if (strcmp(tok[1], "REMOVED") == 0)
			device_removed(&in);
		else if (strcmp(tok[1], "ADDED") == 0 ||
			 strcmp(tok[1], "CHANGED") == 0)
			device_seen(&in, 0);
	}
}

static void read_input(void)
{
	for (;;) {
		ssize_t n;
		char *nl;

		while ((nl = memchr(g_in, '\n', g_inlen))) {
			char line[DEVP_LINE_MAX];
			size_t len = (size_t)(nl - g_in);

			if (len < sizeof(line)) {
				memcpy(line, g_in, len);
				line[len] = '\0';
				handle_line(line);
			}
			memmove(g_in, nl + 1, g_inlen - len - 1);
			g_inlen -= len + 1;
			if (g_fd < 0)
				return;
		}
		if (g_inlen == sizeof(g_in)) {
			disconnect(); /* no line is that long */
			return;
		}
		n = read(g_fd, g_in + g_inlen, sizeof(g_in) - g_inlen);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			disconnect();
			return;
		}
		g_inlen += (size_t)n;
	}
}

static void sound_timers(void)
{
	uint64_t now = now_ms();

	for (int i = 0; i < MAX_DISKS; i++) {
		struct disk *d = &g_disks[i];

		if (d->used && d->sound_pending && !d->inflight &&
		    now >= d->deadline) {
			d->sound_pending = 0;
			play("device-added");
		}
	}
}

int main(void)
{
	struct sigaction sa;
	pid_t parent = getppid();
	uint64_t next_try = 0;
	int backoff = 1000;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	while (!g_quit) {
		struct pollfd p;
		int timeout = 1000;

		/* the session is over when the script that started us is */
		if (parent > 1 && getppid() != parent)
			break;
		if (g_fd < 0 && now_ms() >= next_try) {
			if (try_connect() == 0) {
				backoff = 1000;
			} else {
				next_try = now_ms() + (uint64_t)backoff;
				backoff *= 2;
				if (backoff > BACKOFF_MAX_MS)
					backoff = BACKOFF_MAX_MS;
			}
		}
		for (int i = 0; i < MAX_DISKS; i++)
			if (g_disks[i].used && g_disks[i].sound_pending)
				timeout = 250;
		if (g_fd < 0) {
			poll(NULL, 0, timeout);
			continue;
		}
		p.fd = g_fd;
		p.events = POLLIN;
		p.revents = 0;
		if (poll(&p, 1, timeout) > 0 && (p.revents & (POLLIN | POLLHUP |
							     POLLERR)))
			read_input();
		if (g_fd < 0)
			next_try = now_ms() + 1000;
		sound_timers();
	}
	disconnect();
	return 0;
}
