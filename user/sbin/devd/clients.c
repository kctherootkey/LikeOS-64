// devd: the socket, the connections and the requests.
//
// Every connection is non-blocking, carries the peer credentials the kernel
// recorded at connect (SO_PEERCRED; a peer without them is refused), and
// has a fixed input buffer of one line and an output buffer of 64 KiB: a
// line longer than the protocol allows, or a client that does not read its
// replies, is disconnected.  At most 32 connections, 8 per uid.  A client
// gets a bounded number of requests handled per round, so one cannot starve
// the others.
//
// A device name from a request is only ever compared with the names in the
// device table; nothing is built from it before it matched one.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "devd.h"

struct client *g_clients;
static int g_nclients;
static int g_next_id;

/* ---- the listening socket ------------------------------------------------------- */

int clients_listen(const char *path)
{
	struct sockaddr_un sa;
	struct stat st;
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, path, strlen(path) + 1);

	if (lstat(path, &st) == 0) {
		int t;

		if (!S_ISSOCK(st.st_mode)) {
			log_err("%s exists and is not a socket", path);
			return -EEXIST;
		}
		t = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (t >= 0) {
			if (connect(t, (struct sockaddr *)&sa, sizeof(sa)) ==
			    0) {
				close(t);
				log_err("another devd is already running");
				return -EADDRINUSE;
			}
			close(t);
		}
		if (unlink(path) < 0 && errno != ENOENT)
			return -errno;
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	{
		int fl = fcntl(fd, F_GETFL);
		if (fl >= 0)
			fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	}
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	/* any local user may list devices and ask; the requests decide */
	if (chmod(path, 0666) < 0)
		log_warn("chmod %s: %s", path, strerror(errno));
	if (listen(fd, 16) < 0) {
		int e = errno;
		close(fd);
		unlink(path);
		return -e;
	}
	return fd;
}

static int uid_count(uint32_t uid)
{
	int n = 0;

	for (struct client *c = g_clients; c; c = c->next)
		if (!c->dead && c->uid == uid)
			n++;
	return n;
}

static void refuse(int fd, const char *why)
{
	char line[128];
	int n = snprintf(line, sizeof(line), "ERR HELLO EAGAIN %s\n", why);
	ssize_t w = write(fd, line, (size_t)n);

	(void)w;
	close(fd);
}

void clients_accept(int lfd)
{
	for (int i = 0; i < 16; i++) {
		struct ucred cred;
		socklen_t clen = sizeof(cred);
		struct client *c;
		int fd, fl;

		fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		fl = fcntl(fd, F_GETFL);
		if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
			close(fd);
			continue;
		}
		memset(&cred, 0, sizeof(cred));
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 ||
		    clen < sizeof(cred) || cred.pid <= 0) {
			log_warn("connection without peer credentials refused");
			close(fd);
			continue;
		}
		if (g_nclients >= DEVD_MAX_CLIENTS) {
			refuse(fd, "too%20many%20connections");
			continue;
		}
		if (uid_count(cred.uid) >= DEVD_MAX_PER_UID) {
			refuse(fd, "too%20many%20connections%20for%20this%20user");
			continue;
		}
		c = calloc(1, sizeof(*c));
		if (c)
			c->out = malloc(DEVD_OUT_MAX);
		if (!c || !c->out) {
			if (c)
				free(c);
			close(fd);
			continue;
		}
		c->fd = fd;
		c->uid = cred.uid;
		c->gid = cred.gid;
		c->pid = cred.pid;
		if (++g_next_id <= 0)
			g_next_id = 1;
		c->id = g_next_id;
		c->next = g_clients;
		g_clients = c;
		g_nclients++;
		log_debug("client %d: uid %u pid %d", c->id, (unsigned)c->uid,
			  (int)c->pid);
	}
}

struct client *client_by_id(int id)
{
	if (id <= 0)
		return NULL;
	for (struct client *c = g_clients; c; c = c->next)
		if (c->id == id && !c->dead)
			return c;
	return NULL;
}

/* ---- output ---------------------------------------------------------------------- */

void client_send(struct client *c, const char *buf, size_t len)
{
	if (c->dead)
		return;
	if (c->outlen + len > DEVD_OUT_MAX) {
		log_info("client %d does not read its replies; disconnected",
			 c->id);
		c->dead = 1;
		return;
	}
	memcpy(c->out + c->outlen, buf, len);
	c->outlen += len;
}

void client_send_line(struct client *c, struct devp_line *l)
{
	client_send(c, l->buf, l->len);
}

void client_err(struct client *c, const char *verb, const char *name, int err,
		const char *msg)
{
	struct devp_line l;

	devp_line_begin(&l, "ERR");
	devp_line_enc(&l, verb);
	if (name)
		devp_line_enc(&l, name);
	devp_line_word(&l, devp_errname(err));
	devp_line_enc(&l, msg ? msg : "");
	if (devp_line_end(&l) < 0) {
		devp_line_begin(&l, "ERR PROTOCOL EINVAL request");
		devp_line_end(&l);
	}
	client_send_line(c, &l);
}

static void client_ok(struct client *c, const char *verb, const char *name)
{
	struct devp_line l;

	devp_line_begin(&l, "OK");
	devp_line_word(&l, verb);
	if (name)
		devp_line_enc(&l, name);
	if (devp_line_end(&l) > 0)
		client_send_line(c, &l);
}

void client_flush(struct client *c)
{
	while (!c->dead && c->outlen) {
		ssize_t n = write(c->fd, c->out, c->outlen);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				c->dead = 1;
			return;
		}
		if (n == 0)
			return;
		memmove(c->out, c->out + n, c->outlen - (size_t)n);
		c->outlen -= (size_t)n;
	}
}

void clients_broadcast(struct devp_line *l)
{
	for (struct client *c = g_clients; c; c = c->next)
		if (c->subscribed && !c->dead)
			client_send_line(c, l);
}

/* ---- requests -------------------------------------------------------------------- */

static void req_list(struct client *c)
{
	for (int i = 0; i < g_tab.n; i++) {
		struct devp_line l;

		if (dev_format_line(&l, "DEV", &g_tab.d[i]) > 0)
			client_send_line(c, &l);
	}
	client_ok(c, "LIST", NULL);
}

static void req_fstypes(struct client *c)
{
	size_t n;
	const struct fs_type *t = fs_table(&n);

	for (size_t i = 0; i < n; i++) {
		struct devp_line l;
		char cl[160];
		size_t used = 0;

		cl[0] = '\0';
		for (const uint32_t *p = t[i].clusters; *p; p++) {
			int w = snprintf(cl + used, sizeof(cl) - used, "%s%u",
					 used ? "," : "", (unsigned)*p);
			if (w < 0 || (size_t)w >= sizeof(cl) - used)
				break;
			used += (size_t)w;
		}
		devp_line_begin(&l, "FS");
		devp_line_kv(&l, "id", t[i].id);
		devp_line_kv(&l, "name", t[i].name);
		devp_line_kv_int(&l, "default", t[i].is_default);
		devp_line_kv_u64(&l, "labelmax", t[i].labelmax);
		devp_line_kv(&l, "clusters", cl);
		devp_line_kv_u64(&l, "defcluster", t[i].defcluster);
		devp_line_kv_int(&l, "mount", t[i].kernel_type ? 1 : 0);
		devp_line_kv_int(&l, "format", t[i].mkfs ? 1 : 0);
		if (devp_line_end(&l) > 0)
			client_send_line(c, &l);
	}
	client_ok(c, "FSTYPES", NULL);
}

/* MOUNT, UNMOUNT, EJECT, FORMAT: the device, the peer, the device rules
 * (policy.c) -- the same for every caller, root included. */
static struct dev *modify_target(struct client *c, const char *verb,
				 enum dev_op op, const char *name,
				 int with_name)
{
	struct dev *d = tab_find(name);
	const char *n = with_name ? name : NULL;
	const char *why = "";
	int r;

	if (!d) {
		client_err(c, verb, n, ENOENT, "no such device");
		return NULL;
	}
	if (!auth_may_modify(c->uid, c->pid)) {
		client_err(c, verb, n, EPERM,
			   "only the user logged in at the console may do this");
		return NULL;
	}
	r = dev_policy_check(d, op, &why);
	if (r < 0) {
		client_err(c, verb, n, -r, why);
		return NULL;
	}
	return d;
}

static void req_mount(struct client *c, const char *name, int recover)
{
	struct dev *d = modify_target(c, "MOUNT", OP_MOUNT, name, 0);
	char path[DEVD_PATH_MAX], nm[DEVD_NAME_MAX];
	const char *msg = "";
	struct devp_line l;
	int r;

	if (!d)
		return;
	strlcpy_d(nm, d->name, sizeof(nm));
	r = do_mount(d, c->uid, c->gid, recover, path, sizeof(path), &msg);
	if (r < 0) {
		client_err(c, "MOUNT", NULL, -r, msg);
		return;
	}
	tab_rescan(NULL, 0);
	devp_line_begin(&l, "OK MOUNT");
	devp_line_enc(&l, nm);
	devp_line_kv(&l, "path", path);
	if (devp_line_end(&l) > 0)
		client_send_line(c, &l);
}

static void req_unmount(struct client *c, const char *name)
{
	struct dev *d = modify_target(c, "UNMOUNT", OP_UNMOUNT, name, 0);
	char nm[DEVD_NAME_MAX];
	const char *msg = "";
	int r;

	if (!d)
		return;
	strlcpy_d(nm, d->name, sizeof(nm));
	r = do_unmount(d, c->uid, &msg);
	tab_rescan(NULL, 0);
	if (r < 0)
		client_err(c, "UNMOUNT", NULL, -r, msg);
	else
		client_ok(c, "UNMOUNT", nm);
}

static void req_eject(struct client *c, const char *name)
{
	struct dev *d = modify_target(c, "EJECT", OP_EJECT, name, 0);
	char disk[DEVD_NAME_MAX], nm[DEVD_NAME_MAX];
	const char *msg = "";
	int r;

	if (!d)
		return;
	strlcpy_d(nm, d->name, sizeof(nm));
	strlcpy_d(disk, d->disk, sizeof(disk));
	r = do_eject(disk, c->uid, &msg);
	tab_rescan(NULL, 0);
	if (r < 0) {
		client_err(c, "EJECT", NULL, -r, msg);
		return;
	}
	for (int i = 0; i < g_tab.n; i++) {
		struct dev *e = &g_tab.d[i];
		struct devp_line l;

		if (strcmp(e->disk, disk) != 0 || e->ejected)
			continue;
		e->ejected = 1;
		if (dev_format_line(&l, "EVENT CHANGED", e) > 0)
			clients_broadcast(&l);
	}
	client_ok(c, "EJECT", nm);
}

static void req_format(struct client *c, char **tok, int ntok)
{
	struct dev *d = modify_target(c, "FORMAT", OP_FORMAT, tok[1], 1);
	char nm[DEVD_NAME_MAX];
	const char *msg = "";
	int r;

	if (!d)
		return;
	strlcpy_d(nm, d->name, sizeof(nm));
	r = format_start(c, d, tok, ntok, &msg);
	if (r < 0) {
		/* it may have unmounted the volume before it failed */
		tab_rescan(NULL, 0);
		client_err(c, "FORMAT", nm, -r, msg);
	}
}

static void handle_line(struct client *c, char *line)
{
	char *tok[DEVP_MAX_TOKENS];
	int n = devp_split(line, tok, DEVP_MAX_TOKENS);
	int recover = 0;

	if (n == 0)
		return; /* an empty line asks nothing */
	if (n < 0) {
		client_err(c, "PROTOCOL", NULL, EINVAL, "malformed request");
		return;
	}
	if (strcmp(tok[0], "HELLO") == 0) {
		if (n == 2 && strcmp(tok[1], "1") == 0)
			client_ok(c, "HELLO", "1");
		else
			client_err(c, "HELLO", NULL, EINVAL,
				   "protocol version 1 only");
	} else if (strcmp(tok[0], "LIST") == 0 && n == 1) {
		req_list(c);
	} else if (strcmp(tok[0], "FSTYPES") == 0 && n == 1) {
		req_fstypes(c);
	} else if (strcmp(tok[0], "SUBSCRIBE") == 0 && n == 1) {
		c->subscribed = 1;
		client_ok(c, "SUBSCRIBE", NULL);
	} else if (strcmp(tok[0], "MOUNT") == 0 &&
		   devp_mount_parse(tok, n, &recover) == 0) {
		req_mount(c, tok[1], recover);
	} else if (strcmp(tok[0], "MOUNT") == 0 && n == 3) {
		client_err(c, "MOUNT", NULL, EINVAL, "bad arguments");
	} else if (strcmp(tok[0], "UNMOUNT") == 0 && n == 2) {
		req_unmount(c, tok[1]);
	} else if (strcmp(tok[0], "EJECT") == 0 && n == 2) {
		req_eject(c, tok[1]);
	} else if (strcmp(tok[0], "FORMAT") == 0 && n >= 3) {
		req_format(c, tok, n);
	} else if (strcmp(tok[0], "FORMAT") == 0 && n == 2) {
		client_err(c, "FORMAT", tok[1], EINVAL, "bad arguments");
	} else {
		client_err(c, tok[0], NULL, EINVAL, "unknown request");
	}
}

void client_process(struct client *c)
{
	for (int done = 0; done < DEVD_LINES_PER_ROUND; done++) {
		char line[DEVP_LINE_MAX];
		char *nl;
		size_t len;

		if (c->dead || c->waiting)
			return;
		nl = memchr(c->in, '\n', c->inlen);
		if (!nl)
			return;
		len = (size_t)(nl - c->in);
		memcpy(line, c->in, len);
		line[len] = '\0';
		memmove(c->in, nl + 1, c->inlen - len - 1);
		c->inlen -= len + 1;
		if (memchr(line, '\0', len)) {
			client_err(c, "PROTOCOL", NULL, EINVAL,
				   "malformed request");
			continue;
		}
		handle_line(c, line);
	}
}

void client_read(struct client *c)
{
	for (;;) {
		ssize_t n;

		if (c->inlen >= sizeof(c->in)) {
			if (!memchr(c->in, '\n', c->inlen)) {
				log_info("client %d: line too long; "
					 "disconnected",
					 c->id);
				c->dead = 1;
			}
			return; /* full: take requests first */
		}
		n = read(c->fd, c->in + c->inlen, sizeof(c->in) - c->inlen);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				c->dead = 1;
			return;
		}
		if (n == 0) {
			/* the peer sends no more: answer what it asked,
			 * then close */
			c->eof = 1;
			return;
		}
		c->inlen += (size_t)n;
	}
}

/* Has this connection a request waiting to be handled? */
int client_has_line(const struct client *c)
{
	return !c->dead && !c->waiting && memchr(c->in, '\n', c->inlen);
}

void clients_reap(void)
{
	struct client **pp = &g_clients;

	while (*pp) {
		struct client *c = *pp;

		if (c->eof && !c->waiting && !c->outlen &&
		    !memchr(c->in, '\n', c->inlen))
			c->dead = 1;
		if (!c->dead) {
			pp = &c->next;
			continue;
		}
		*pp = c->next;
		format_client_gone(c->id);
		close(c->fd);
		free(c->out);
		free(c);
		g_nclients--;
	}
}

void clients_close_all(void)
{
	for (struct client *c = g_clients; c; c = c->next)
		c->dead = 1;
	clients_reap();
}
