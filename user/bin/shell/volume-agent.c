// likeos-volume-agent -- mounts USB drives and CDs/DVDs for the desktop
// session.
//
// Started from the X session (xinitrc).  It talks to the device manager,
// devd, over /var/run/devd/socket: HELLO, SUBSCRIBE, LIST, and then follows
// the device events.  A USB volume or a disc that appears and that LikeOS
// can open is mounted for the user (devd decides whether this user may: the
// one logged in at the console); its folder is /media/<user>/<label>.  The
// partitions of built-in disks are never mounted by the agent -- only when
// the user opens one in the file manager.  Each drive plugged in, and each
// disc put into a drive, plays the "device-added" sound once -- when its
// first volume is mounted, or shortly after it appears when it has nothing
// to mount -- and "device-removed" when a drive goes away.  Drives that are
// already there when the agent starts are mounted without a sound, and a
// volume is mounted automatically only once: one the user unmounted or
// ejected stays that way -- until another medium is in the drive (another
// disc, another card: a new filesystem identity), which is mounted afresh.
//
// A volume whose journal must be replayed first (an ext4 or NTFS file
// system that was not closed properly) is not replayed behind the user's
// back: devd refuses the mount (EUCLEAN), and the agent runs
// likeos-journal-prompt, which asks.  When the user agrees, the volume is
// mounted with "MOUNT <name> recover"; when not, it stays unmounted until it
// is plugged in again.  The drive's sound plays when the question appears.
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
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "devproto.h"

#define MAX_VOLS 128
#define MAX_DISKS 32
#define MAX_PENDING 64
#define NAME_MAX_ 32
#define SOUND_DELAY_MS 1500
#define BACKOFF_MAX_MS 30000

#define UUID_MAX_ 64
#define IDENT_MAX (16 + UUID_MAX_ + 24)
#define LABEL_MAX_ 192
#define TEXT_MAX_ 64 /* vendor, model */
#define MAX_PROMPTS 8

#define PROMPT_PATH "/usr/bin/likeos-journal-prompt"

struct info {
	char name[NAME_MAX_], disk[NAME_MAX_], kind[16], fstype[16];
	char state[16], uuid[UUID_MAX_];
	char label[LABEL_MAX_], vendor[TEXT_MAX_], model[TEXT_MAX_];
	uint64_t size;
	struct devp_flags f; /* keys an older devd does not send: 0 */
	int mounted;
};

struct vol {
	int used;
	int handled; /* mounted automatically once (or found mounted) */
	int seen;
	char name[NAME_MAX_];
	char disk[NAME_MAX_];
	char ident[IDENT_MAX]; /* the medium: fstype, uuid, size */
	/* what the journal question shows */
	char fstype[16], label[LABEL_MAX_], drive[2 * TEXT_MAX_];
	uint64_t size;
};

/* A journal question on the screen: likeos-journal-prompt for a volume
 * whose journal must be replayed before it can be mounted.  At most one per
 * volume; `gone' when the volume (or its medium) went while it was asked:
 * the program was told to quit and its answer counts for nothing. */
struct prompt {
	pid_t pid; /* 0: free */
	int gone;
	char name[NAME_MAX_];
	char ident[IDENT_MAX];
};

struct disk {
	int used;
	int seen;
	int usb; /* a removable USB drive (or CD/DVD drive) of the user's */
	int medium; /* a CD/DVD drive: a disc is in it */
	int sound_pending;
	int inflight;
	uint64_t deadline;
	char name[NAME_MAX_];
};

struct req {
	char verb[12];
	char name[NAME_MAX_];
	int recover; /* MOUNT <name> recover */
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
static struct prompt g_prompts[MAX_PROMPTS];
static int g_chld_pipe[2] = { -1, -1 }; /* SIGCHLD wakes the poll loop */

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

static void on_child(int sig)
{
	int e = errno;
	char c = 0;
	ssize_t w;

	(void)sig;
	if (g_chld_pipe[1] >= 0)
		w = write(g_chld_pipe[1], &c, 1); /* full: already woken */
	(void)w;
	errno = e;
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

/* ---- the journal question ------------------------------------------------------- */

static struct prompt *prompt_find(const char *vol)
{
	for (int i = 0; i < MAX_PROMPTS; i++)
		if (g_prompts[i].pid > 0 && !g_prompts[i].gone &&
		    strcmp(g_prompts[i].name, vol) == 0)
			return &g_prompts[i];
	return NULL;
}

/* The volume went, or another medium is in the drive: the question about
 * it is taken off the screen, and its answer will not count. */
static void prompt_cancel(const char *vol)
{
	struct prompt *p = prompt_find(vol);

	if (!p)
		return;
	p->gone = 1;
	kill(p->pid, SIGTERM);
}

/* Ask whether the journal of `v' may be replayed.  0 when the question is
 * on the screen (or already was). */
static int prompt_start(const struct vol *v)
{
	struct prompt *p = NULL;
	char size[24];
	char *argv[16];
	int argc = 0, r;
	posix_spawnattr_t attr;
	sigset_t def, none;

	if (prompt_find(v->name))
		return 0;
	for (int i = 0; i < MAX_PROMPTS && !p; i++)
		if (g_prompts[i].pid <= 0)
			p = &g_prompts[i];
	if (!p)
		return -1;
	argv[argc++] = PROMPT_PATH;
	argv[argc++] = "--device";
	argv[argc++] = (char *)v->name;
	argv[argc++] = "--fstype";
	argv[argc++] = (char *)v->fstype;
	if (v->label[0]) {
		argv[argc++] = "--label";
		argv[argc++] = (char *)v->label;
	}
	if (v->size) {
		snprintf(size, sizeof(size), "%llu",
			 (unsigned long long)v->size);
		argv[argc++] = "--size";
		argv[argc++] = size;
	}
	if (v->drive[0]) {
		argv[argc++] = "--drive";
		argv[argc++] = (char *)v->drive;
	}
	argv[argc] = NULL;

	/* the program starts with the signals as a program expects them:
	 * none blocked, none ignored */
	sigemptyset(&def);
	sigaddset(&def, SIGPIPE);
	sigaddset(&def, SIGCHLD);
	sigemptyset(&none);
	if (posix_spawnattr_init(&attr) != 0)
		return -1;
	posix_spawnattr_setsigdefault(&attr, &def);
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setflags(&attr,
				 POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
	r = posix_spawn(&p->pid, PROMPT_PATH, NULL, &attr, argv, environ);
	posix_spawnattr_destroy(&attr);
	if (r != 0) {
		p->pid = 0;
		return -1;
	}
	p->gone = 0;
	copy(p->name, v->name, sizeof(p->name));
	copy(p->ident, v->ident, sizeof(p->ident));
	return 0;
}

static void kill_prompts(void)
{
	for (int i = 0; i < MAX_PROMPTS; i++)
		if (g_prompts[i].pid > 0) {
			g_prompts[i].gone = 1;
			kill(g_prompts[i].pid, SIGTERM);
		}
}

static void vol_drop(struct vol *v)
{
	prompt_cancel(v->name);
	v->used = 0;
}

static void disk_forget(const char *name)
{
	struct disk *d = disk_get(name, 0);

	if (d)
		d->used = 0;
	for (int i = 0; i < MAX_VOLS; i++)
		if (g_vols[i].used && strcmp(g_vols[i].disk, name) == 0)
			vol_drop(&g_vols[i]);
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

/* `recover': MOUNT <name> recover -- the journal is replayed (the user
 * agreed). */
static int send_req_ex(const char *verb, const char *name, int recover)
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
		snprintf(line, sizeof(line), "%s %s%s\n", verb, enc,
			 recover ? " recover" : "");
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
	r->recover = recover;
	g_qlen++;
	return 0;
}

static int send_req(const char *verb, const char *name)
{
	return send_req_ex(verb, name, 0);
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
	uint64_t size = 0;
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
	in->f.field = v && strcmp(v, "1") == 0;
	FLAG("usb", usb)
	FLAG("removable", removable)
	FLAG("system", system)
	FLAG("supported", supported)
	FLAG("ejected", ejected)
	FLAG("readonly", readonly)
	FLAG("optical", optical)
	FLAG("internal", internal)
#undef FLAG
	v = devp_find(tok, n, first, "uuid", NULL);
	if (!v || devp_decode(in->uuid, sizeof(in->uuid), v) < 0)
		in->uuid[0] = '\0';
	/* only shown (by the journal question, which cleans them again) */
	v = devp_find(tok, n, first, "label", NULL);
	if (!v || devp_decode(in->label, sizeof(in->label), v) < 0)
		in->label[0] = '\0';
	v = devp_find(tok, n, first, "vendor", NULL);
	if (!v || devp_decode(in->vendor, sizeof(in->vendor), v) < 0)
		in->vendor[0] = '\0';
	v = devp_find(tok, n, first, "model", NULL);
	if (!v || devp_decode(in->model, sizeof(in->model), v) < 0)
		in->model[0] = '\0';
	v = devp_find(tok, n, first, "size", NULL);
	if (v && devp_parse_u64(v, UINT64_MAX, &size) == 0)
		in->size = size;
	v = devp_find(tok, n, first, "mounted", NULL);
	in->mounted = v && devp_decode(tmp, sizeof(tmp), v) > 0;
	return in->name[0] && in->disk[0] ? 0 : -1;
}

static int is_volume(const struct info *in)
{
	return strcmp(in->kind, "partition") == 0 ||
	       strcmp(in->fstype, "none") != 0;
}

/* A drive of the user's: a removable USB drive or a CD/DVD drive, not the
 * system's -- never a built-in disk. */
static int user_drive(const struct info *in)
{
	return !in->f.system && !in->f.internal &&
	       (in->f.optical || (in->f.usb && in->f.removable));
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
			d->medium = in->size > 0;
			if (!startup && g_started && !in->f.optical) {
				d->sound_pending = 1;
				d->deadline = now_ms() + SOUND_DELAY_MS;
			}
		}
		/* a disc put into a CD/DVD drive: the drive was there */
		if (d->usb && in->f.optical) {
			int medium = in->size > 0;

			if (medium && !d->medium && !startup && g_started) {
				d->sound_pending = 1;
				d->deadline = now_ms() + SOUND_DELAY_MS;
			}
			if (!medium)
				d->sound_pending = 0;
			d->medium = medium;
		}
	}
	if (!is_volume(in))
		return;
	v = vol_get(in->name, 1);
	if (!v)
		return;
	v->seen = 1;
	copy(v->disk, in->disk, sizeof(v->disk));
	{
		/* another medium in the same drive (a disc, a card, a
		 * formatted stick): what was decided for the old one is
		 * forgotten */
		char ident[IDENT_MAX];

		snprintf(ident, sizeof(ident), "%s|%s|%llu", in->fstype,
			 in->uuid, (unsigned long long)in->size);
		if (strcmp(ident, v->ident) != 0) {
			copy(v->ident, ident, sizeof(v->ident));
			v->handled = 0;
			prompt_cancel(v->name);
		}
	}
	copy(v->fstype, in->fstype, sizeof(v->fstype));
	copy(v->label, in->label, sizeof(v->label));
	v->size = in->size;
	/* "SanDisk Cruzer Blade": the vendor, unless the model names it */
	if (in->vendor[0] && strncasecmp(in->model, in->vendor,
					 strlen(in->vendor)) != 0)
		snprintf(v->drive, sizeof(v->drive), "%s%s%s", in->vendor,
			 in->model[0] ? " " : "", in->model);
	else
		copy(v->drive, in->model, sizeof(v->drive));
	if (in->mounted) {
		v->handled = 1; /* never mount it again by itself */
		return;
	}
	if (v->handled || !devp_automount(&in->f) ||
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
			vol_drop(v);
	}
}

/* `err': the error name of an ERR line ("EBUSY"), "" for OK. */
static void reply_done(const struct req *r, int ok, const char *err)
{
	struct vol *v;
	struct disk *d;

	if (strcmp(r->verb, "LIST") == 0) {
		/* forget what devd no longer has */
		for (int i = 0; i < MAX_VOLS; i++)
			if (g_vols[i].used && !g_vols[i].seen)
				vol_drop(&g_vols[i]);
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
	/* The journal must be replayed first: the user is asked.  The drive
	 * was found all the same, so its sound plays now, with the
	 * question, and not again when it is mounted after all. */
	if (!ok && !r->recover && strcmp(err, "EUCLEAN") == 0 &&
	    prompt_start(v) == 0 && d->sound_pending) {
		d->sound_pending = 0;
		play("device-added");
	}
}

/* The question about a volume was answered (or the program ended). */
static void prompt_done(struct prompt *p, int status)
{
	int yes = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	struct vol *v = p->gone ? NULL : vol_get(p->name, 0);
	struct disk *d;

	p->pid = 0;
	if (!yes || !v || strcmp(v->ident, p->ident) != 0)
		return; /* not replayed: it stays unmounted (handled) */
	if (send_req_ex("MOUNT", v->name, 1) == 0) {
		d = disk_get(v->disk, 0);
		if (d)
			d->inflight++;
	}
}

static void reap_prompts(void)
{
	char buf[64];

	while (g_chld_pipe[0] >= 0 && read(g_chld_pipe[0], buf, sizeof(buf)) > 0)
		;
	for (int i = 0; i < MAX_PROMPTS; i++) {
		struct prompt *p = &g_prompts[i];
		int status = 0;
		pid_t r;

		if (p->pid <= 0)
			continue;
		r = waitpid(p->pid, &status, WNOHANG);
		if (r == p->pid)
			prompt_done(p, status);
		else if (r < 0 && errno == ECHILD)
			p->pid = 0;
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
		/* ERR <verb> [<name>] <ERRNAME> <message> */
		const char *err = tok[0][0] == 'O' ? "" :
				  n >= 4	     ? tok[n - 2] :
				  n == 3	     ? tok[2] :
							"";

		if (!g_qlen)
			return;
		r = g_q[g_qhead];
		g_qhead = (g_qhead + 1) % MAX_PENDING;
		g_qlen--;
		reply_done(&r, tok[0][0] == 'O', err);
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
	/* the journal questions: their end wakes the loop */
	if (pipe2(g_chld_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
		g_chld_pipe[0] = g_chld_pipe[1] = -1;
	sa.sa_handler = on_child;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	while (!g_quit) {
		struct pollfd p[2];
		int timeout = 1000, np = 0, connected;

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
		/* without the pipe, the questions are looked after this often */
		for (int i = 0; i < MAX_PROMPTS; i++)
			if (g_prompts[i].pid > 0 && g_chld_pipe[0] < 0)
				timeout = 250;
		if (g_chld_pipe[0] >= 0) {
			p[np].fd = g_chld_pipe[0];
			p[np].events = POLLIN;
			p[np].revents = 0;
			np++;
		}
		if (g_fd >= 0) {
			p[np].fd = g_fd;
			p[np].events = POLLIN;
			p[np].revents = 0;
			np++;
		}
		connected = g_fd >= 0;
		if (poll(np ? p : NULL, (nfds_t)np, timeout) > 0 && connected &&
		    (p[np - 1].revents & (POLLIN | POLLHUP | POLLERR)))
			read_input();
		reap_prompts();
		if (!connected)
			continue;
		if (g_fd < 0)
			next_try = now_ms() + 1000;
		sound_timers();
	}
	kill_prompts();
	disconnect();
	return 0;
}
