// LikeOS Explorer: the device service.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-devd.h"

#include <gio/gunixsocketaddress.h>
#include <string.h>

/* How long to wait before trying the service again: doubled after every
 * failure, from the first to the last value. */
#define RETRY_FIRST_MS 500
#define RETRY_LAST_MS 8000
/* Bounds on what the service can make the client keep. */
#define DEVICES_MAX 256
#define FSTYPES_MAX 32
#define PENDING_MAX 64

/* ==== one connection ======================================================
 *
 * A socket to the service with a line reader and a queue of lines to send.
 * Every asynchronous step holds a reference; once the connection is dead
 * (closed by its owner, or broken) its owner hears nothing more from it. */

typedef struct {
	void (*opened)(gpointer owner);
	void (*line)(gpointer owner, const char *line, gsize len);
	void (*closed)(gpointer owner, const GError *why);
} ConnFuncs;

typedef struct {
	gint refs;
	gpointer owner;
	const ConnFuncs *fn;
	GSocketConnection *sc;
	GCancellable *cancel;
	GString *in;
	GQueue out; /* GBytes */
	GBytes *inflight; /* the line being written */
	gboolean writing;
	gboolean dead;
} Conn;

static Conn *conn_ref(Conn *c)
{
	c->refs++;
	return c;
}

static void conn_unref(Conn *c)
{
	if (--c->refs > 0)
		return;
	g_cancellable_cancel(c->cancel);
	g_object_unref(c->cancel);
	g_clear_object(&c->sc);
	g_string_free(c->in, TRUE);
	g_queue_clear_full(&c->out, (GDestroyNotify)g_bytes_unref);
	g_clear_pointer(&c->inflight, g_bytes_unref);
	g_free(c);
}

/* The connection broke: the owner hears it once. */
static void conn_fail(Conn *c, const char *why)
{
	gpointer owner = c->owner;
	GError *e;

	if (c->dead)
		return;
	c->dead = TRUE;
	c->owner = NULL;
	g_cancellable_cancel(c->cancel);
	if (!owner)
		return;
	e = g_error_new_literal(EX_DEVD_ERROR, EX_DEVD_ERROR_NOT_RUNNING, why);
	c->fn->closed(owner, e);
	g_error_free(e);
}

/* The owner is done with it: no more callbacks. */
static void conn_close(Conn *c)
{
	if (!c)
		return;
	c->dead = TRUE;
	c->owner = NULL;
	g_cancellable_cancel(c->cancel);
	conn_unref(c);
}

static void conn_write_next(Conn *c);

static void written(GObject *src, GAsyncResult *res, gpointer data)
{
	Conn *c = data;
	gboolean ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(src), res,
						       NULL, NULL);

	c->writing = FALSE;
	g_clear_pointer(&c->inflight, g_bytes_unref);
	if (!c->dead) {
		if (ok)
			conn_write_next(c);
		else
			conn_fail(c, "The connection to the device service was lost.");
	}
	conn_unref(c);
}

static void conn_write_next(Conn *c)
{
	GBytes *b;
	gsize len;
	gconstpointer p;

	if (c->dead || c->writing || !c->sc || g_queue_is_empty(&c->out))
		return;
	b = g_queue_pop_head(&c->out);
	p = g_bytes_get_data(b, &len);
	c->writing = TRUE;
	c->inflight = b; /* kept until written */
	g_output_stream_write_all_async(
		g_io_stream_get_output_stream(G_IO_STREAM(c->sc)), p, len,
		G_PRIORITY_DEFAULT, c->cancel, written, conn_ref(c));
}

static void conn_send(Conn *c, const char *line)
{
	char *s;

	if (!c || c->dead)
		return;
	s = g_strconcat(line, "\n", NULL);
	g_queue_push_tail(&c->out, g_bytes_new_take(s, strlen(s)));
	conn_write_next(c);
}

static void conn_read(Conn *c);

static void got_bytes(GObject *src, GAsyncResult *res, gpointer data)
{
	Conn *c = data;
	GBytes *b = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res,
						     NULL);
	gsize len = b ? g_bytes_get_size(b) : 0;

	if (c->dead) {
		g_clear_pointer(&b, g_bytes_unref);
		conn_unref(c);
		return;
	}
	if (!b || len == 0) {
		g_clear_pointer(&b, g_bytes_unref);
		conn_fail(c, "The device service closed the connection.");
		conn_unref(c);
		return;
	}
	g_string_append_len(c->in, g_bytes_get_data(b, NULL), (gssize)len);
	g_bytes_unref(b);
	for (;;) {
		char *nl = memchr(c->in->str, '\n', c->in->len);
		gsize n;

		if (!nl)
			break;
		n = (gsize)(nl - c->in->str);
		if (n > EX_DEVD_LINE_MAX) {
			conn_fail(c, "The device service sent a line that is too "
				     "long.");
			break;
		}
		*nl = '\0';
		if (n > 0 && c->owner)
			c->fn->line(c->owner, c->in->str, n);
		if (c->dead)
			break;
		g_string_erase(c->in, 0, (gssize)n + 1);
	}
	if (!c->dead && c->in->len > EX_DEVD_LINE_MAX)
		conn_fail(c, "The device service sent a line that is too long.");
	if (!c->dead)
		conn_read(c);
	conn_unref(c);
}

static void conn_read(Conn *c)
{
	g_input_stream_read_bytes_async(
		g_io_stream_get_input_stream(G_IO_STREAM(c->sc)),
		EX_DEVD_LINE_MAX, G_PRIORITY_DEFAULT, c->cancel, got_bytes,
		conn_ref(c));
}

static void connected(GObject *src, GAsyncResult *res, gpointer data)
{
	Conn *c = data;
	GSocketConnection *sc = g_socket_client_connect_finish(
		G_SOCKET_CLIENT(src), res, NULL);

	if (c->dead) {
		g_clear_object(&sc);
	} else if (!sc) {
		conn_fail(c, "The device service is not running.");
	} else {
		c->sc = sc;
		if (c->fn->opened)
			c->fn->opened(c->owner);
		if (!c->dead) {
			conn_read(c);
			conn_write_next(c);
		}
	}
	conn_unref(c);
}

static Conn *conn_open(gpointer owner, const ConnFuncs *fn, const char *path)
{
	Conn *c = g_new0(Conn, 1);
	GSocketClient *client = g_socket_client_new();
	GSocketAddress *addr = g_unix_socket_address_new(path);

	c->refs = 1;
	c->owner = owner;
	c->fn = fn;
	c->cancel = g_cancellable_new();
	c->in = g_string_sized_new(EX_DEVD_LINE_MAX);
	g_queue_init(&c->out);
	g_socket_client_set_family(client, G_SOCKET_FAMILY_UNIX);
	g_socket_client_set_enable_proxy(client, FALSE);
	g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(addr),
				      c->cancel, connected, conn_ref(c));
	g_object_unref(addr);
	g_object_unref(client);
	return c;
}

/* ==== the client ============================================================ */

typedef enum {
	R_HELLO,
	R_SUBSCRIBE,
	R_LIST,
	R_FSTYPES,
	R_MOUNT,
	R_UNMOUNT,
	R_EJECT,
} ReqKind;

static const char *const verbs[] = {
	[R_HELLO] = "HELLO",	 [R_SUBSCRIBE] = "SUBSCRIBE",
	[R_LIST] = "LIST",	 [R_FSTYPES] = "FSTYPES",
	[R_MOUNT] = "MOUNT",	 [R_UNMOUNT] = "UNMOUNT",
	[R_EJECT] = "EJECT",
};

typedef struct {
	ReqKind kind;
	char *name; /* MOUNT, UNMOUNT, EJECT */
	gboolean recover; /* MOUNT <name> recover */
	GTask *task; /* the caller's, or NULL */
} Req;

struct _ExDevd {
	GObject parent;
	char *path; /* the service's socket */
	Conn *conn;
	GQueue pending; /* Req, in the order sent */
	GPtrArray *devices; /* ExDevdDevice */
	GPtrArray *fstypes; /* ExDevdFs */
	GPtrArray *list_acc, *fs_acc; /* lines of a LIST / FSTYPES reply */
	gboolean ready; /* HELLO answered and a LIST arrived */
	guint retry_id;
	guint retry_ms;
};

G_DEFINE_TYPE(ExDevd, ex_devd, G_TYPE_OBJECT)

enum { SIG_CHANGED, SIG_FSTYPES, N_SIGS };
static guint sigs[N_SIGS];

static void req_free(Req *r)
{
	g_clear_object(&r->task);
	g_free(r->name);
	g_free(r);
}

static void req_fail(Req *r, GError *e)
{
	if (r->task)
		g_task_return_error(r->task, g_error_copy(e));
	req_free(r);
}

static GPtrArray *devices_new(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)ex_devd_device_free);
}

static GPtrArray *fstypes_new(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)ex_devd_fs_free);
}

static void ex_devd_dispose(GObject *o)
{
	ExDevd *d = EX_DEVD(o);
	GError *e = g_error_new_literal(EX_DEVD_ERROR, EX_DEVD_ERROR_NOT_RUNNING,
					"The device service is not running.");
	Req *r;

	if (d->retry_id) {
		g_source_remove(d->retry_id);
		d->retry_id = 0;
	}
	conn_close(d->conn);
	d->conn = NULL;
	while ((r = g_queue_pop_head(&d->pending)))
		req_fail(r, e);
	g_error_free(e);
	g_clear_pointer(&d->devices, g_ptr_array_unref);
	g_clear_pointer(&d->fstypes, g_ptr_array_unref);
	g_clear_pointer(&d->list_acc, g_ptr_array_unref);
	g_clear_pointer(&d->fs_acc, g_ptr_array_unref);
	G_OBJECT_CLASS(ex_devd_parent_class)->dispose(o);
}

static void ex_devd_finalize(GObject *o)
{
	g_free(EX_DEVD(o)->path);
	G_OBJECT_CLASS(ex_devd_parent_class)->finalize(o);
}

static void ex_devd_class_init(ExDevdClass *k)
{
	G_OBJECT_CLASS(k)->dispose = ex_devd_dispose;
	G_OBJECT_CLASS(k)->finalize = ex_devd_finalize;
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
					 G_TYPE_NONE, 0);
	sigs[SIG_FSTYPES] = g_signal_new("fstypes-changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
					 G_TYPE_NONE, 0);
}

static void ex_devd_init(ExDevd *d)
{
	g_queue_init(&d->pending);
	d->devices = devices_new();
	d->fstypes = fstypes_new();
	d->list_acc = devices_new();
	d->fs_acc = fstypes_new();
	d->retry_ms = RETRY_FIRST_MS;
}

static void connect_now(ExDevd *d);

static gboolean retry(gpointer data)
{
	ExDevd *d = data;

	d->retry_id = 0;
	connect_now(d);
	return G_SOURCE_REMOVE;
}

static void schedule_retry(ExDevd *d)
{
	if (d->retry_id)
		return;
	d->retry_id = g_timeout_add(d->retry_ms, retry, d);
	d->retry_ms = MIN(d->retry_ms * 2, RETRY_LAST_MS);
}

/* The connection is gone (broken, or given up): everything waiting fails,
 * the table empties, and the service is tried again later. */
static void lost(ExDevd *d, const GError *why)
{
	gboolean had = d->ready || d->devices->len > 0;
	GError *e = why ? g_error_copy(why) :
			  g_error_new_literal(EX_DEVD_ERROR,
					      EX_DEVD_ERROR_NOT_RUNNING,
					      "The device service is not running.");
	Req *r;

	conn_close(d->conn);
	d->conn = NULL;
	d->ready = FALSE;
	while ((r = g_queue_pop_head(&d->pending)))
		req_fail(r, e);
	g_error_free(e);
	g_ptr_array_set_size(d->list_acc, 0);
	g_ptr_array_set_size(d->fs_acc, 0);
	g_ptr_array_set_size(d->devices, 0);
	schedule_retry(d);
	if (had)
		g_signal_emit(d, sigs[SIG_CHANGED], 0);
}

static void send_req(ExDevd *d, ReqKind kind, const char *name,
		     gboolean recover, GTask *task)
{
	Req *r;
	char *line;

	if (!d->conn || g_queue_get_length(&d->pending) >= PENDING_MAX) {
		if (task)
			g_task_return_new_error(
				task, EX_DEVD_ERROR,
				d->conn ? EX_DEVD_ERROR_AGAIN :
					  EX_DEVD_ERROR_NOT_RUNNING,
				"%s",
				ex_devd_error_explain(
					d->conn ? EX_DEVD_ERROR_AGAIN :
						  EX_DEVD_ERROR_NOT_RUNNING));
		g_clear_object(&task);
		return;
	}
	r = g_new0(Req, 1);
	r->kind = kind;
	r->name = g_strdup(name);
	r->recover = recover;
	r->task = task;
	g_queue_push_tail(&d->pending, r);
	if (kind == R_HELLO)
		line = g_strdup("HELLO 1");
	else if (name)
		line = g_strdup_printf("%s %s%s", verbs[kind], name,
				       recover ? " recover" : "");
	else
		line = g_strdup(verbs[kind]);
	conn_send(d->conn, line);
	g_free(line);
}

/* ---- what the service sends ------------------------------------------------ */

static int device_index(GPtrArray *a, const char *name)
{
	for (guint i = 0; i < a->len; i++)
		if (strcmp(((ExDevdDevice *)g_ptr_array_index(a, i))->name,
			   name) == 0)
			return (int)i;
	return -1;
}

static void event(ExDevd *d, const ExDevdLine *l)
{
	const char *what = ex_devd_line_word(l, 1);
	ExDevdDevice *dev = ex_devd_device_from_line(l);
	int at;

	if (!what || !dev) {
		ex_devd_device_free(dev);
		return;
	}
	at = device_index(d->devices, dev->name);
	if (strcmp(what, "REMOVED") == 0) {
		if (at >= 0)
			g_ptr_array_remove_index(d->devices, (guint)at);
		/* A disk takes its partitions with it. */
		if (dev->is_disk)
			for (guint i = d->devices->len; i-- > 0;)
				if (strcmp(((ExDevdDevice *)g_ptr_array_index(
						    d->devices, i))->disk,
					   dev->name) == 0)
					g_ptr_array_remove_index(d->devices, i);
		ex_devd_device_free(dev);
	} else if (strcmp(what, "ADDED") == 0 || strcmp(what, "CHANGED") == 0) {
		if (at >= 0) {
			ex_devd_device_free(g_ptr_array_index(d->devices, (guint)at));
			g_ptr_array_index(d->devices, (guint)at) = dev;
		} else if (d->devices->len < DEVICES_MAX) {
			g_ptr_array_add(d->devices, dev);
		} else {
			ex_devd_device_free(dev);
			return;
		}
	} else {
		ex_devd_device_free(dev);
		return;
	}
	g_signal_emit(d, sigs[SIG_CHANGED], 0);
}

static Req *first_pending(ExDevd *d, ReqKind kind, const char *name)
{
	for (GList *i = d->pending.head; i; i = i->next) {
		Req *r = i->data;

		if (r->kind == kind && (!name || !r->name || !strcmp(r->name, name)))
			return r;
	}
	return NULL;
}

static int fs_order(gconstpointer a, gconstpointer b)
{
	const ExDevdFs *x = *(ExDevdFs *const *)a, *y = *(ExDevdFs *const *)b;

	return (int)y->is_default - (int)x->is_default;
}

/* An OK or ERR line: the answer to the oldest request of its verb. */
static void answer(ExDevd *d, const ExDevdLine *l, gboolean ok)
{
	const char *verb = ex_devd_line_word(l, 1);
	char *ev = NULL, *ename = NULL, *errname = NULL, *msg = NULL;
	const char *name;
	int kind = -1;
	Req *r;

	if (!ok && !ex_devd_parse_err(l, &ev, &ename, &errname, &msg))
		return;
	for (gsize k = 0; verb && k < G_N_ELEMENTS(verbs); k++)
		if (strcmp(verb, verbs[k]) == 0)
			kind = (int)k;
	name = ok ? ex_devd_line_word(l, 2) : ename;
	r = kind >= 0 ? first_pending(d, (ReqKind)kind,
				      kind >= R_MOUNT ? name : NULL) :
			NULL;
	if (!r) {
		g_debug("devd: an answer to nothing asked: %s", verb ? verb : "");
		goto out;
	}
	g_queue_remove(&d->pending, r);
	if (!ok) {
		ExDevdErrorCode code = ex_devd_error_code(errname);
		GError *e = g_error_new_literal(EX_DEVD_ERROR, code,
						*msg ? msg :
						       ex_devd_error_explain(code));

		if (r->kind == R_HELLO) {
			/* A service that does not speak this version. */
			g_error_free(e);
			req_free(r);
			lost(d, NULL);
			goto out;
		}
		if (r->kind == R_LIST)
			g_ptr_array_set_size(d->list_acc, 0);
		if (r->kind == R_FSTYPES)
			g_ptr_array_set_size(d->fs_acc, 0);
		req_fail(r, e);
		g_error_free(e);
		goto out;
	}
	switch (r->kind) {
	case R_HELLO:
		d->retry_ms = RETRY_FIRST_MS;
		break;
	case R_LIST:
		g_ptr_array_unref(d->devices);
		d->devices = d->list_acc;
		d->list_acc = devices_new();
		d->ready = TRUE;
		g_signal_emit(d, sigs[SIG_CHANGED], 0);
		break;
	case R_FSTYPES:
		g_ptr_array_unref(d->fstypes);
		d->fstypes = d->fs_acc;
		d->fs_acc = fstypes_new();
		g_ptr_array_sort(d->fstypes, fs_order);
		g_signal_emit(d, sigs[SIG_FSTYPES], 0);
		break;
	case R_MOUNT:
		if (r->task) {
			const char *path = ex_devd_line_get(l, "path");

			if (path && path[0] == '/')
				g_task_return_pointer(r->task, g_strdup(path),
						      g_free);
			else
				g_task_return_new_error(
					r->task, EX_DEVD_ERROR,
					EX_DEVD_ERROR_PROTOCOL, "%s",
					ex_devd_error_explain(
						EX_DEVD_ERROR_PROTOCOL));
		}
		break;
	case R_UNMOUNT:
	case R_EJECT:
		if (r->task)
			g_task_return_boolean(r->task, TRUE);
		break;
	case R_SUBSCRIBE:
	default:
		break;
	}
	req_free(r);
out:
	g_free(ev);
	g_free(ename);
	g_free(errname);
	g_free(msg);
}

static void on_line(gpointer owner, const char *text, gsize len)
{
	ExDevd *d = owner;
	g_autoptr(ExDevdLine) l = ex_devd_line_parse(text, len);
	const char *w;

	if (!l) {
		g_debug("devd: a line that is not one was dropped");
		return;
	}
	w = ex_devd_line_word(l, 0);
	if (strcmp(w, "EVENT") == 0) {
		event(d, l);
	} else if (strcmp(w, "DEV") == 0) {
		ExDevdDevice *dev;

		if (!first_pending(d, R_LIST, NULL) ||
		    d->list_acc->len >= DEVICES_MAX)
			return;
		dev = ex_devd_device_from_line(l);
		if (dev && device_index(d->list_acc, dev->name) < 0)
			g_ptr_array_add(d->list_acc, dev);
		else
			ex_devd_device_free(dev);
	} else if (strcmp(w, "FS") == 0) {
		ExDevdFs *fs;

		if (!first_pending(d, R_FSTYPES, NULL) ||
		    d->fs_acc->len >= FSTYPES_MAX)
			return;
		fs = ex_devd_fs_from_line(l);
		if (fs)
			g_ptr_array_add(d->fs_acc, fs);
	} else if (strcmp(w, "OK") == 0) {
		answer(d, l, TRUE);
	} else if (strcmp(w, "ERR") == 0) {
		answer(d, l, FALSE);
	}
	/* PROGRESS belongs to formatting, on its own connection; anything
	 * else is not for this client. */
}

static void on_closed(gpointer owner, const GError *why)
{
	lost(owner, why);
}

static const ConnFuncs client_funcs = { NULL, on_line, on_closed };

static void connect_now(ExDevd *d)
{
	if (d->conn)
		return;
	d->conn = conn_open(d, &client_funcs, d->path);
	send_req(d, R_HELLO, NULL, FALSE, NULL);
	send_req(d, R_SUBSCRIBE, NULL, FALSE, NULL);
	send_req(d, R_LIST, NULL, FALSE, NULL);
	send_req(d, R_FSTYPES, NULL, FALSE, NULL);
}

/* ---- the interface ------------------------------------------------------------ */

ExDevd *ex_devd_get_default(void)
{
	static ExDevd *the;

	if (!the)
		the = ex_devd_new_for_path(EX_DEVD_SOCKET);
	return the;
}

ExDevd *ex_devd_new_for_path(const char *socket_path)
{
	ExDevd *d = g_object_new(EX_TYPE_DEVD, NULL);

	d->path = g_strdup(socket_path);
	connect_now(d);
	return d;
}

gboolean ex_devd_is_connected(ExDevd *d)
{
	return d->ready;
}

GPtrArray *ex_devd_devices(ExDevd *d)
{
	return d->devices;
}

const ExDevdDevice *ex_devd_find(ExDevd *d, const char *name)
{
	int at = name ? device_index(d->devices, name) : -1;

	return at >= 0 ? g_ptr_array_index(d->devices, (guint)at) : NULL;
}

GPtrArray *ex_devd_fstypes(ExDevd *d)
{
	return d->fstypes;
}

const ExDevdFs *ex_devd_find_fs(ExDevd *d, const char *id)
{
	for (guint i = 0; id && i < d->fstypes->len; i++) {
		const ExDevdFs *f = g_ptr_array_index(d->fstypes, i);

		if (strcmp(f->id, id) == 0)
			return f;
	}
	return NULL;
}

char *ex_devd_fs_display_name(ExDevd *d, const char *fstype)
{
	static const struct {
		const char *id, *name;
	} known[] = {
		{ "vfat", "FAT32" },	   { "fat", "FAT" },	   { "ntfs", "NTFS" },
		{ "exfat", "exFAT" },	   { "ext4", "ext4" },	   { "ext3", "ext3" },
		{ "ext2", "ext2" },	   { "iso9660", "ISO 9660" }, { "udf", "UDF" },
		{ "swap", "Swap space" },
	};
	const ExDevdFs *f = ex_devd_find_fs(d, fstype);

	if (f)
		return g_strdup(f->name);
	if (!fstype || !*fstype || !strcmp(fstype, "unknown") ||
	    !strcmp(fstype, "none"))
		return g_strdup("Unknown");
	for (gsize i = 0; i < G_N_ELEMENTS(known); i++)
		if (strcmp(known[i].id, fstype) == 0)
			return g_strdup(known[i].name);
	return g_strdup(fstype);
}

char *ex_devd_volume_fs_name(ExDevd *d, const char *fstype, gboolean supported)
{
	/* The service reports every FAT as vfat, and can mount FAT32 only:
	 * a FAT it cannot mount is one of the older, smaller ones. */
	if (fstype && !strcmp(fstype, "vfat") && !supported)
		return g_strdup("FAT12/FAT16");
	return ex_devd_fs_display_name(d, fstype);
}

void ex_devd_refresh_fstypes(ExDevd *d)
{
	if (d->conn && !first_pending(d, R_FSTYPES, NULL))
		send_req(d, R_FSTYPES, NULL, FALSE, NULL);
}

static void request(ExDevd *d, ReqKind kind, const char *name,
		    gboolean recover, GCancellable *cancel,
		    GAsyncReadyCallback done, gpointer user_data, gpointer tag)
{
	GTask *t = g_task_new(d, cancel, done, user_data);

	g_task_set_source_tag(t, tag);
	if (!ex_devd_name_valid(name)) {
		g_task_return_new_error(t, EX_DEVD_ERROR, EX_DEVD_ERROR_NOENT,
					"%s",
					ex_devd_error_explain(EX_DEVD_ERROR_NOENT));
		g_object_unref(t);
		return;
	}
	send_req(d, kind, name, recover, t);
}

void ex_devd_mount_async(ExDevd *d, const char *name, GCancellable *cancel,
			 GAsyncReadyCallback done, gpointer user_data)
{
	request(d, R_MOUNT, name, FALSE, cancel, done, user_data,
		ex_devd_mount_async);
}

void ex_devd_mount_recover_async(ExDevd *d, const char *name,
				 GCancellable *cancel, GAsyncReadyCallback done,
				 gpointer user_data)
{
	request(d, R_MOUNT, name, TRUE, cancel, done, user_data,
		ex_devd_mount_recover_async);
}

char *ex_devd_mount_finish(ExDevd *d, GAsyncResult *res, GError **error)
{
	return g_task_propagate_pointer(G_TASK(res), error);
}

void ex_devd_unmount_async(ExDevd *d, const char *name, GCancellable *cancel,
			   GAsyncReadyCallback done, gpointer user_data)
{
	request(d, R_UNMOUNT, name, FALSE, cancel, done, user_data,
		ex_devd_unmount_async);
}

gboolean ex_devd_unmount_finish(ExDevd *d, GAsyncResult *res, GError **error)
{
	return g_task_propagate_boolean(G_TASK(res), error);
}

void ex_devd_eject_async(ExDevd *d, const char *disk, GCancellable *cancel,
			 GAsyncReadyCallback done, gpointer user_data)
{
	request(d, R_EJECT, disk, FALSE, cancel, done, user_data,
		ex_devd_eject_async);
}

gboolean ex_devd_eject_finish(ExDevd *d, GAsyncResult *res, GError **error)
{
	return g_task_propagate_boolean(G_TASK(res), error);
}

/* ==== formatting ============================================================ */

struct _ExDevdFormat {
	GObject parent;
	Conn *conn;
	char *name;
	char *request;
	gboolean hello_ok;
	gboolean done;
	gboolean counted; /* one of the jobs running */
	ExDevdErrorCode fail_code; /* refused before it started */
	const char *fail_text;
};

/* Formatting jobs running in this process: one at a time, so a program
 * never holds more than two connections to the service (its own and the
 * job's), well within what the service allows each user. */
static guint g_formatting;

G_DEFINE_TYPE(ExDevdFormat, ex_devd_format, G_TYPE_OBJECT)

enum { SIG_PROGRESS, SIG_FINISHED, N_FSIGS };
static guint fsigs[N_FSIGS];

static void ex_devd_format_finalize(GObject *o)
{
	ExDevdFormat *f = EX_DEVD_FORMAT(o);

	conn_close(f->conn);
	g_free(f->name);
	g_free(f->request);
	G_OBJECT_CLASS(ex_devd_format_parent_class)->finalize(o);
}

static void ex_devd_format_class_init(ExDevdFormatClass *k)
{
	G_OBJECT_CLASS(k)->finalize = ex_devd_format_finalize;
	fsigs[SIG_PROGRESS] = g_signal_new("progress", G_TYPE_FROM_CLASS(k),
					   G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					   NULL, G_TYPE_NONE, 1, G_TYPE_INT);
	fsigs[SIG_FINISHED] = g_signal_new("finished", G_TYPE_FROM_CLASS(k),
					   G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					   NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void ex_devd_format_init(ExDevdFormat *f)
{
}

/* The end, once: the connection closed, "finished" emitted, the job's own
 * reference dropped. */
static void format_finish(ExDevdFormat *f, GError *e)
{
	if (f->done)
		return;
	f->done = TRUE;
	if (f->counted) {
		f->counted = FALSE;
		g_formatting--;
	}
	conn_close(f->conn);
	f->conn = NULL;
	g_signal_emit(f, fsigs[SIG_FINISHED], 0, e);
	g_object_unref(f);
}

static void format_line(gpointer owner, const char *text, gsize len)
{
	ExDevdFormat *f = owner;
	g_autoptr(ExDevdLine) l = ex_devd_line_parse(text, len);
	const char *w0, *w1, *w2;

	if (!l)
		return;
	w0 = ex_devd_line_word(l, 0);
	w1 = ex_devd_line_word(l, 1);
	w2 = ex_devd_line_word(l, 2);
	if (!strcmp(w0, "PROGRESS")) {
		guint64 pct;

		const char *w3 = ex_devd_line_word(l, 3);

		/* "busy": the step that reports no progress has started */
		if (w1 && w2 && !strcmp(w1, f->name) &&
		    g_ascii_string_to_unsigned(w2, 10, 0, 100, &pct, NULL))
			g_signal_emit(f, fsigs[SIG_PROGRESS], 0,
				      w3 && !strcmp(w3, "busy") ? -1 : (int)pct);
	} else if (!strcmp(w0, "OK") && w1 && !strcmp(w1, "HELLO")) {
		f->hello_ok = TRUE;
	} else if (!strcmp(w0, "OK") && w1 && !strcmp(w1, "FORMAT") &&
		   (!w2 || !strcmp(w2, f->name))) {
		format_finish(f, NULL);
	} else if (!strcmp(w0, "ERR")) {
		char *verb = NULL, *name = NULL, *errname = NULL, *msg = NULL;

		if (ex_devd_parse_err(l, &verb, &name, &errname, &msg) &&
		    (!strcmp(verb, "FORMAT") || !strcmp(verb, "HELLO")) &&
		    (!name || !strcmp(name, f->name))) {
			/* ERR HELLO EAGAIN: the service has too many
			 * connections. */
			ExDevdErrorCode code = ex_devd_error_code(errname);
			GError *e = g_error_new_literal(
				EX_DEVD_ERROR, code,
				*msg ? msg : ex_devd_error_explain(code));

			format_finish(f, e);
			g_error_free(e);
		}
		g_free(verb);
		g_free(name);
		g_free(errname);
		g_free(msg);
	}
}

static void format_closed(gpointer owner, const GError *why)
{
	ExDevdFormat *f = owner;
	GError *e = g_error_new_literal(
		EX_DEVD_ERROR, EX_DEVD_ERROR_NOT_RUNNING,
		f->hello_ok ? "The connection to the device service was lost "
			      "while formatting." :
			      ex_devd_error_explain(EX_DEVD_ERROR_NOT_RUNNING));

	format_finish(f, e);
	g_error_free(e);
}

static const ConnFuncs format_funcs = { NULL, format_line, format_closed };

static gboolean fail_later(gpointer data)
{
	ExDevdFormat *f = data;
	GError *e = g_error_new_literal(EX_DEVD_ERROR, f->fail_code,
					f->fail_text);

	format_finish(f, e);
	g_error_free(e);
	return G_SOURCE_REMOVE;
}

ExDevdFormat *ex_devd_format_start(const char *name, const char *fs,
				   const char *label, gboolean quick,
				   guint64 cluster)
{
	ExDevdFormat *f = g_object_new(EX_TYPE_DEVD_FORMAT, NULL);
	gboolean fs_ok = fs && *fs && strlen(fs) <= 16;

	for (const char *c = fs; fs_ok && *c; c++)
		fs_ok = g_ascii_islower(*c) || g_ascii_isdigit(*c) || *c == '_';
	f->name = g_strdup(name ? name : "");
	/* The job holds itself until "finished"; the caller's reference is
	 * the one returned. */
	g_object_ref(f);
	if (!ex_devd_name_valid(name) || !fs_ok) {
		f->fail_code = EX_DEVD_ERROR_INVAL;
		f->fail_text = ex_devd_error_explain(EX_DEVD_ERROR_INVAL);
		g_idle_add(fail_later, f);
		return f;
	}
	if (g_formatting > 0) {
		f->fail_code = EX_DEVD_ERROR_AGAIN;
		f->fail_text = "Another drive is being formatted. Start this one "
			       "when it is done.";
		g_idle_add(fail_later, f);
		return f;
	}
	g_formatting++;
	f->counted = TRUE;
	{
		char *enc = ex_devd_encode(label ? label : "");

		f->request = g_strdup_printf(
			"FORMAT %s fs=%s label=%s quick=%d cluster=%" G_GUINT64_FORMAT,
			name, fs, enc, quick ? 1 : 0, cluster);
		g_free(enc);
	}
	f->conn = conn_open(f, &format_funcs, EX_DEVD_SOCKET);
	conn_send(f->conn, "HELLO 1");
	conn_send(f->conn, f->request);
	return f;
}

gboolean ex_devd_format_done(ExDevdFormat *f)
{
	return f->done;
}
