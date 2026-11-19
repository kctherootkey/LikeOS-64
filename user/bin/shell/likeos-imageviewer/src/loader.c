// LikeOS -- LikeOS Image Viewer: loading pictures.
//
// A picture is decoded in a worker thread: the file's type from its first
// bytes, its EXIF block (ivproc), the pixels (the image library, or ivproc
// for WebP), the turn or mirror its EXIF orientation asks for, and the
// prescaled levels the canvas draws from -- each half the size of the one
// before, made by area-averaging, so a large photograph shown small is
// smooth instead of aliased, and a zoom change never has to scale more
// than twice the pixels on the screen.  Animated pictures (GIF) stay an
// animation and are played by the canvas.
//
// The cache keeps the pictures round the current one, decoded and ready:
// the viewer asks for the current one first and the neighbours after it,
// so stepping through a folder shows the next picture at once.  Two worker
// threads take the most urgent request first; a request nobody waits for
// any more is dropped before it starts.  The cache also has a memory budget
// and lets the least recently used pictures go to stay inside it.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "imageviewer.h"

/* The largest picture decoded at full size: beyond this it is decoded at a
 * reduced size (and the info panel says so).  About 400 MB of pixels. */
#define IV_MAX_PIXELS (100 * 1000 * 1000)
/* Pictures up to this size get their full-size surface in the worker;
 * larger ones only when the view is zoomed in far enough to need it. */
#define IV_EAGER_FULL_PIXELS (12 * 1000 * 1000)
/* The smallest level made: a long edge of about this many pixels. */
#define IV_MIP_FLOOR 320
/* What the cache may keep decoded. */
#define IV_CACHE_BUDGET ((gsize)640 * 1024 * 1024)
#define IV_WORKERS 2

/* ---- pictures ------------------------------------------------------------ */

IvPicture *iv_picture_ref(IvPicture *p)
{
	if (p)
		g_atomic_int_inc(&p->ref);
	return p;
}

void iv_picture_unref(IvPicture *p)
{
	if (!p || !g_atomic_int_dec_and_test(&p->ref))
		return;
	for (int i = 0; i < IV_MIP_MAX; i++)
		if (p->levels[i])
			cairo_surface_destroy(p->levels[i]);
	g_clear_object(&p->pixbuf);
	g_clear_object(&p->anim);
	g_free(p->path);
	g_free(p->name);
	g_free(p->mime);
	g_free(p->format);
	g_free(p->error);
	g_free(p);
}

gboolean iv_picture_is_animated(IvPicture *p)
{
	return p && p->anim != NULL;
}

gboolean iv_picture_writable(IvPicture *p)
{
	return p && !p->error && !p->anim &&
	       iv_format_for_mime(p->mime) != IV_FORMAT_NONE;
}

static gsize picture_bytes(IvPicture *p)
{
	gsize n = 0;

	if (p->pixbuf)
		n += (gsize)gdk_pixbuf_get_byte_length(p->pixbuf);
	for (int i = 0; i < IV_MIP_MAX; i++)
		if (p->levels[i])
			n += (gsize)cairo_image_surface_get_stride(p->levels[i]) *
			     (gsize)cairo_image_surface_get_height(p->levels[i]);
	return n;
}

/* The levels below the full size, each half the one before. */
static void make_levels(IvPicture *p, gboolean full_now)
{
	GdkPixbuf *cur;
	int w, h;

	if (!p->pixbuf)
		return;
	w = gdk_pixbuf_get_width(p->pixbuf);
	h = gdk_pixbuf_get_height(p->pixbuf);
	p->n_levels = 1;
	if (full_now)
		p->levels[0] = iv_surface_from_pixbuf(p->pixbuf);
	cur = g_object_ref(p->pixbuf);
	while (p->n_levels < IV_MIP_MAX && MAX(w, h) / 2 >= IV_MIP_FLOOR) {
		GdkPixbuf *half;

		w = MAX(1, w / 2);
		h = MAX(1, h / 2);
		half = gdk_pixbuf_scale_simple(cur, w, h, GDK_INTERP_BILINEAR);
		g_object_unref(cur);
		if (!half)
			return;
		p->levels[p->n_levels++] = iv_surface_from_pixbuf(half);
		cur = half;
	}
	g_object_unref(cur);
}

cairo_surface_t *iv_picture_surface(IvPicture *p, int level)
{
	if (!p || level < 0 || level >= p->n_levels)
		return NULL;
	if (!p->levels[level] && level == 0 && p->pixbuf)
		p->levels[0] = iv_surface_from_pixbuf(p->pixbuf);
	return p->levels[level];
}

int iv_picture_level_for(IvPicture *p, double scale)
{
	int level = 0;

	if (!p || p->n_levels <= 1 || scale <= 0)
		return 0;
	/* The smallest level still at least as large as what is drawn:
	 * level k is 1/2^k, good down to a scale of 1/2^k. */
	while (level + 1 < p->n_levels && scale <= 1.0 / (double)(2 << level))
		level++;
	return level;
}

static GdkPixbuf *load_webp(const char *path, gboolean *alpha, GError **e)
{
	int has_alpha = 0;
	IvImage *img = iv_webp_load(path, &has_alpha);
	GdkPixbuf *pb;

	if (!img) {
		g_set_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			    "The WebP picture could not be decoded");
		return NULL;
	}
	pb = iv_pixbuf_from_image(img);
	iv_image_free(img);
	*alpha = has_alpha != 0;
	return pb;
}

static GdkPixbuf *load_stream(const char *path, IvPicture *p, GCancellable *c,
			      GError **e)
{
	GFile *f = g_file_new_for_path(path);
	GFileInputStream *in = g_file_read(f, c, e);
	GdkPixbuf *pb = NULL;
	int w = 0, h = 0;

	g_object_unref(f);
	if (!in)
		return NULL;
	if (gdk_pixbuf_get_file_info(path, &w, &h) && (gint64)w * h > IV_MAX_PIXELS) {
		double k = sqrt((double)IV_MAX_PIXELS / ((double)w * h));

		pb = gdk_pixbuf_new_from_stream_at_scale(G_INPUT_STREAM(in),
							 (int)(w * k), (int)(h * k),
							 TRUE, c, e);
		p->reduced = pb != NULL;
	} else {
		pb = gdk_pixbuf_new_from_stream(G_INPUT_STREAM(in), c, e);
	}
	p->file_width = w;
	p->file_height = h;
	g_object_unref(in);
	return pb;
}

IvPicture *iv_picture_load(const char *path, GCancellable *c, GError **e)
{
	IvPicture *p = g_new0(IvPicture, 1);
	struct stat st;
	gboolean alpha = FALSE;
	GdkPixbuf *raw = NULL;
	int orient = 1;

	p->ref = 1;
	p->path = g_strdup(path);
	p->name = g_filename_display_basename(path);
	if (stat(path, &st) != 0) {
		int err = errno;

		g_set_error(e, G_IO_ERROR, g_io_error_from_errno(err), "%s",
			    g_strerror(err));
		iv_picture_unref(p);
		return NULL;
	}
	p->file_size = st.st_size;
	p->mtime = st.st_mtime;
	p->mime = iv_mime_for_path(path);
	p->format = iv_mime_label(p->mime);
	p->exif_status = iv_exif_read_file(path, &p->exif);
	if (p->exif_status < 0)
		memset(&p->exif, 0, sizeof p->exif);

	if (!strcmp(p->mime, "image/webp")) {
		raw = load_webp(path, &alpha, e);
	} else if (!strcmp(p->mime, "image/gif")) {
		GdkPixbufAnimation *a;

		IV_BEGIN_ANIMATION_API
		a = gdk_pixbuf_animation_new_from_file(path, e);
		if (a && !gdk_pixbuf_animation_is_static_image(a)) {
			p->anim = a;
			raw = g_object_ref(gdk_pixbuf_animation_get_static_image(a));
		} else if (a) {
			raw = g_object_ref(gdk_pixbuf_animation_get_static_image(a));
			g_object_unref(a);
		}
		IV_END_ANIMATION_API
	} else {
		raw = load_stream(path, p, c, e);
	}
	if (!raw) {
		if (e && !*e)
			g_set_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				    "The picture could not be read");
		iv_picture_unref(p);
		return NULL;
	}
	if (g_cancellable_is_cancelled(c)) {
		g_object_unref(raw);
		g_cancellable_set_error_if_cancelled(c, e);
		iv_picture_unref(p);
		return NULL;
	}
	if (!p->file_width) {
		p->file_width = gdk_pixbuf_get_width(raw);
		p->file_height = gdk_pixbuf_get_height(raw);
	}

	/* The orientation: from the EXIF block when there is one, else
	 * whatever the image library found (TIFF's own tag). */
	if (!p->anim) {
		if (p->exif_status >= 0 && p->exif.orientation >= 1 &&
		    p->exif.orientation <= 8) {
			orient = p->exif.orientation;
		} else {
			const char *o = gdk_pixbuf_get_option(raw, "orientation");

			orient = o ? atoi(o) : 1;
		}
	}
	p->orientation = orient >= 1 && orient <= 8 ? orient : 1;
	p->pixbuf = iv_pixbuf_orient(raw, p->orientation);
	g_object_unref(raw);

	p->width = gdk_pixbuf_get_width(p->pixbuf);
	p->height = gdk_pixbuf_get_height(p->pixbuf);
	p->has_alpha = alpha || gdk_pixbuf_get_has_alpha(p->pixbuf);
	p->channels = gdk_pixbuf_get_n_channels(p->pixbuf);
	/* Bits per pixel, as the file stores them (the decoded pixbuf is
	 * always 8 bits per channel). */
	p->bits = p->exif.bits > 0 ? p->exif.bits :
				     gdk_pixbuf_get_bits_per_sample(p->pixbuf) *
					     p->channels;
	if (p->exif.channels > 0)
		p->channels = p->exif.channels;
	if (p->has_alpha && strcmp(p->mime, "image/webp") != 0 &&
	    gdk_pixbuf_get_has_alpha(p->pixbuf)) {
		/* A loader may hand over an alpha channel the file does not
		 * use; the info panel should say what the file has. */
		IvImage probe = {
			.w = p->width, .h = p->height,
			.stride = gdk_pixbuf_get_rowstride(p->pixbuf),
			.px = (uint8_t *)gdk_pixbuf_read_pixels(p->pixbuf),
		};
		if (iv_image_is_opaque(&probe) && p->exif.channels != 4 &&
		    g_strcmp0(p->exif.color_type, "RGBA") != 0 &&
		    g_strcmp0(p->exif.color_type, "Greyscale with alpha") != 0)
			p->has_alpha = strcmp(p->mime, "image/png") == 0;
	}
	make_levels(p, (gint64)p->width * p->height <= IV_EAGER_FULL_PIXELS);
	return p;
}

IvPicture *iv_picture_new_rotated(IvPicture *p, int quarter_turns)
{
	int q = ((quarter_turns % 4) + 4) % 4;
	IvPicture *r;
	GdkPixbuf *pb;

	if (!p || !p->pixbuf || p->anim)
		return NULL;
	switch (q) {
	case 0:
		return iv_picture_ref(p);
	case 1:
		pb = gdk_pixbuf_rotate_simple(p->pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);
		break;
	case 2:
		pb = gdk_pixbuf_rotate_simple(p->pixbuf, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
		break;
	default:
		pb = gdk_pixbuf_rotate_simple(p->pixbuf,
					      GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
		break;
	}
	if (!pb)
		return NULL;
	r = g_new0(IvPicture, 1);
	r->ref = 1;
	r->path = g_strdup(p->path);
	r->name = g_strdup(p->name);
	r->mime = g_strdup(p->mime);
	r->format = g_strdup(p->format);
	r->file_size = p->file_size;
	r->mtime = p->mtime;
	r->file_width = p->file_width;
	r->file_height = p->file_height;
	r->orientation = iv_orientation_rotate(p->orientation, q);
	r->has_alpha = p->has_alpha;
	r->bits = p->bits;
	r->channels = p->channels;
	r->reduced = p->reduced;
	r->exif = p->exif;
	r->exif_status = p->exif_status;
	r->pixbuf = pb;
	r->width = gdk_pixbuf_get_width(pb);
	r->height = gdk_pixbuf_get_height(pb);
	make_levels(r, (gint64)r->width * r->height <= IV_EAGER_FULL_PIXELS);
	return r;
}

/* ---- the cache ------------------------------------------------------------- */

typedef struct {
	guint ticket;
	char *path;
	IvPictureReady cb;
	gpointer data;
} Waiter;

typedef struct {
	struct IvPictureCache *cache;
	char *path;
	int priority;
	guint64 seq;
	gboolean started;
	GCancellable *cancel;
	IvPicture *result;
	char *error;
} Job;

struct IvPictureCache {
	gint ref;
	gboolean dead;
	GHashTable *done;	/* path -> IvPicture* */
	GQueue lru;		/* char* paths, most recent first */
	GHashTable *jobs;	/* path -> Job*, queued or running */
	GMutex lock;		/* `pending' and Job.started */
	GPtrArray *pending;	/* Job* not yet started */
	GThreadPool *pool;
	GList *waiters;
	guint next_ticket;
	guint64 seq;
};

static void cache_unref(IvPictureCache *c)
{
	if (!g_atomic_int_dec_and_test(&c->ref))
		return;
	g_hash_table_unref(c->done);
	g_hash_table_unref(c->jobs);
	g_queue_clear_full(&c->lru, g_free);
	g_ptr_array_unref(c->pending);
	g_mutex_clear(&c->lock);
	g_free(c);
}

static void job_free(Job *j)
{
	g_free(j->path);
	g_free(j->error);
	g_clear_object(&j->cancel);
	if (j->result)
		iv_picture_unref(j->result);
	g_free(j);
}

static void touch(IvPictureCache *c, const char *path)
{
	for (GList *l = c->lru.head; l; l = l->next)
		if (!strcmp(l->data, path)) {
			g_free(l->data);
			g_queue_delete_link(&c->lru, l);
			break;
		}
	g_queue_push_head(&c->lru, g_strdup(path));
}

static void evict_to_budget(IvPictureCache *c)
{
	gsize total = 0;
	GHashTableIter it;
	gpointer k, v;

	g_hash_table_iter_init(&it, c->done);
	while (g_hash_table_iter_next(&it, &k, &v))
		total += picture_bytes(v);
	/* The most recent one always stays, however large. */
	while (total > IV_CACHE_BUDGET && c->lru.length > 1) {
		char *victim = g_queue_pop_tail(&c->lru);
		IvPicture *p = g_hash_table_lookup(c->done, victim);

		if (p) {
			total -= MIN(total, picture_bytes(p));
			g_hash_table_remove(c->done, victim);
		}
		g_free(victim);
	}
}

static void deliver(IvPictureCache *c, const char *path, IvPicture *p)
{
	GList *mine = NULL;

	/* Taken off the list first: a callback may well ask for (or cancel)
	 * other pictures while this runs. */
	for (GList *l = c->waiters; l;) {
		GList *next = l->next;
		Waiter *w = l->data;

		if (!strcmp(w->path, path)) {
			c->waiters = g_list_remove_link(c->waiters, l);
			mine = g_list_concat(mine, l);
		}
		l = next;
	}
	iv_picture_ref(p);
	for (GList *l = mine; l; l = l->next) {
		Waiter *w = l->data;

		if (!c->dead)
			w->cb(p, w->data);
		g_free(w->path);
		g_free(w);
	}
	iv_picture_unref(p);
	g_list_free(mine);
}

static gboolean job_done(gpointer data)
{
	Job *j = data;
	IvPictureCache *c = j->cache;

	if (!c->dead) {
		/* The table may hold a newer job for the same file by now. */
		if (g_hash_table_lookup(c->jobs, j->path) == j)
			g_hash_table_steal(c->jobs, j->path);
		if (g_cancellable_is_cancelled(j->cancel)) {
			/* Forgotten or trimmed meanwhile: stale pixels. */
		} else if (j->result) {
			g_hash_table_replace(c->done, g_strdup(j->path),
					     iv_picture_ref(j->result));
			touch(c, j->path);
			evict_to_budget(c);
			deliver(c, j->path, j->result);
		} else {
			/* A picture standing for the failure, not cached, so
			 * the next visit tries again. */
			IvPicture *p = g_new0(IvPicture, 1);
			struct stat st;

			p->ref = 1;
			p->path = g_strdup(j->path);
			p->name = g_filename_display_basename(j->path);
			p->mime = iv_mime_for_path(j->path);
			p->format = iv_mime_label(p->mime);
			p->error = g_strdup(j->error ? j->error :
						       "The picture could not be read");
			if (stat(j->path, &st) == 0) {
				p->file_size = st.st_size;
				p->mtime = st.st_mtime;
			}
			deliver(c, j->path, p);
			iv_picture_unref(p);
		}
	}
	job_free(j);
	cache_unref(c);
	return G_SOURCE_REMOVE;
}

/* The worker: whatever pending job is most urgent, not necessarily the
 * one it was woken for. */
static void worker(gpointer unused, gpointer data)
{
	IvPictureCache *c = data;
	Job *best = NULL;
	guint best_i = 0;
	GError *e = NULL;

	g_mutex_lock(&c->lock);
	for (guint i = 0; i < c->pending->len; i++) {
		Job *j = g_ptr_array_index(c->pending, i);

		if (!best || j->priority < best->priority ||
		    (j->priority == best->priority && j->seq > best->seq)) {
			best = j;
			best_i = i;
		}
	}
	if (best) {
		g_ptr_array_remove_index(c->pending, best_i);
		best->started = TRUE;
	}
	g_mutex_unlock(&c->lock);
	if (!best) {
		cache_unref(c);
		return;
	}
	best->result = iv_picture_load(best->path, best->cancel, &e);
	if (!best->result)
		best->error = g_strdup(e ? e->message : NULL);
	g_clear_error(&e);
	/* `c' is handed on to job_done with the job's ref. */
	g_idle_add_full(G_PRIORITY_DEFAULT, job_done, best, NULL);
	cache_unref(c);
}

IvPictureCache *iv_cache_new(void)
{
	IvPictureCache *c = g_new0(IvPictureCache, 1);

	c->ref = 1;
	c->done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					(GDestroyNotify)iv_picture_unref);
	c->jobs = g_hash_table_new(g_str_hash, g_str_equal);
	c->pending = g_ptr_array_new();
	g_mutex_init(&c->lock);
	g_queue_init(&c->lru);
	c->pool = g_thread_pool_new(worker, c, IV_WORKERS, FALSE, NULL);
	c->next_ticket = 1;
	return c;
}

void iv_cache_free(IvPictureCache *c)
{
	GHashTableIter it;
	gpointer k, v;

	if (!c)
		return;
	c->dead = TRUE;
	/* Jobs not yet started never will be: they go straight to job_done,
	 * which only drops them now. */
	g_mutex_lock(&c->lock);
	for (guint i = 0; i < c->pending->len; i++) {
		Job *j = g_ptr_array_index(c->pending, i);

		g_cancellable_cancel(j->cancel);
		j->started = TRUE;
		g_idle_add(job_done, j);
	}
	g_ptr_array_set_size(c->pending, 0);
	g_mutex_unlock(&c->lock);
	g_hash_table_iter_init(&it, c->jobs);
	while (g_hash_table_iter_next(&it, &k, &v))
		g_cancellable_cancel(((Job *)v)->cancel);
	g_hash_table_remove_all(c->jobs);
	for (GList *l = c->waiters; l; l = l->next) {
		Waiter *w = l->data;

		g_free(w->path);
		g_free(w);
	}
	g_list_free(c->waiters);
	c->waiters = NULL;
	g_hash_table_remove_all(c->done);
	/* The threads finish what they are doing and go; the cache goes
	 * with the last job that still refers to it. */
	g_thread_pool_free(c->pool, FALSE, FALSE);
	c->pool = NULL;
	cache_unref(c);
}

static void start_job(IvPictureCache *c, const char *path, int priority)
{
	Job *j = g_hash_table_lookup(c->jobs, path);

	if (j) {
		/* Already asked for: only more urgent now, perhaps. */
		g_mutex_lock(&c->lock);
		if (!j->started && priority < j->priority) {
			j->priority = priority;
			j->seq = ++c->seq;
		}
		g_mutex_unlock(&c->lock);
		return;
	}
	j = g_new0(Job, 1);
	j->cache = c;
	j->path = g_strdup(path);
	j->priority = priority;
	j->cancel = g_cancellable_new();
	g_atomic_int_inc(&c->ref); /* the job's */
	g_hash_table_insert(c->jobs, j->path, j);
	g_mutex_lock(&c->lock);
	j->seq = ++c->seq;
	g_ptr_array_add(c->pending, j);
	g_mutex_unlock(&c->lock);
	g_atomic_int_inc(&c->ref); /* the worker's */
	g_thread_pool_push(c->pool, c, NULL);
}

typedef struct {
	IvPictureCache *cache;
	guint ticket;
	char *path;
} Immediate;

static gboolean answer_cached(gpointer data)
{
	Immediate *im = data;
	IvPictureCache *c = im->cache;

	if (!c->dead) {
		IvPicture *p = g_hash_table_lookup(c->done, im->path);

		if (p) {
			/* Only the waiter of this ticket. */
			for (GList *l = c->waiters; l; l = l->next) {
				Waiter *w = l->data;

				if (w->ticket == im->ticket) {
					c->waiters = g_list_delete_link(c->waiters,
									l);
					iv_picture_ref(p);
					w->cb(p, w->data);
					iv_picture_unref(p);
					g_free(w->path);
					g_free(w);
					break;
				}
			}
		} else {
			/* Evicted in between: load it after all. */
			start_job(c, im->path, 0);
		}
	}
	g_free(im->path);
	g_free(im);
	cache_unref(c);
	return G_SOURCE_REMOVE;
}

guint iv_cache_request(IvPictureCache *c, const char *path, int priority,
		       IvPictureReady cb, gpointer data)
{
	Waiter *w = g_new0(Waiter, 1);

	w->ticket = c->next_ticket++;
	w->path = g_strdup(path);
	w->cb = cb;
	w->data = data;
	c->waiters = g_list_append(c->waiters, w);
	if (g_hash_table_contains(c->done, path)) {
		Immediate *im = g_new0(Immediate, 1);

		touch(c, path);
		im->cache = c;
		im->ticket = w->ticket;
		im->path = g_strdup(path);
		g_atomic_int_inc(&c->ref);
		g_idle_add_full(G_PRIORITY_HIGH_IDLE, answer_cached, im, NULL);
	} else {
		start_job(c, path, priority);
	}
	return w->ticket;
}

void iv_cache_cancel(IvPictureCache *c, guint ticket)
{
	if (!c || !ticket)
		return;
	for (GList *l = c->waiters; l; l = l->next) {
		Waiter *w = l->data;

		if (w->ticket == ticket) {
			c->waiters = g_list_delete_link(c->waiters, l);
			g_free(w->path);
			g_free(w);
			return;
		}
	}
}

IvPicture *iv_cache_peek(IvPictureCache *c, const char *path)
{
	IvPicture *p = c && path ? g_hash_table_lookup(c->done, path) : NULL;

	if (p)
		touch(c, path);
	return p;
}

void iv_cache_forget(IvPictureCache *c, const char *path)
{
	Job *j;

	if (!c || !path)
		return;
	g_hash_table_remove(c->done, path);
	for (GList *l = c->lru.head; l; l = l->next)
		if (!strcmp(l->data, path)) {
			g_free(l->data);
			g_queue_delete_link(&c->lru, l);
			break;
		}
	/* A load in flight would bring the old pixels back. */
	j = g_hash_table_lookup(c->jobs, path);
	if (j) {
		g_cancellable_cancel(j->cancel);
		g_hash_table_steal(c->jobs, path);
		/* Waiters on it get the fresh load started below. */
		gboolean waited = FALSE;

		for (GList *l = c->waiters; l; l = l->next)
			if (!strcmp(((Waiter *)l->data)->path, path))
				waited = TRUE;
		if (waited)
			start_job(c, path, 0);
	}
}

void iv_cache_put(IvPictureCache *c, IvPicture *p)
{
	if (!c || !p || p->error)
		return;
	g_hash_table_replace(c->done, g_strdup(p->path), iv_picture_ref(p));
	touch(c, p->path);
	evict_to_budget(c);
}

void iv_cache_trim(IvPictureCache *c, const char *const *keep)
{
	GHashTableIter it;
	gpointer k, v;
	GPtrArray *gone = g_ptr_array_new_with_free_func(g_free);

	g_hash_table_iter_init(&it, c->done);
	while (g_hash_table_iter_next(&it, &k, &v))
		if (!keep || !g_strv_contains(keep, k))
			g_ptr_array_add(gone, g_strdup(k));
	for (guint i = 0; i < gone->len; i++)
		iv_cache_forget(c, g_ptr_array_index(gone, i));
	g_ptr_array_unref(gone);

	/* Queued loads nobody needs any more never start. */
	g_mutex_lock(&c->lock);
	for (guint i = 0; i < c->pending->len;) {
		Job *j = g_ptr_array_index(c->pending, i);
		gboolean waited = FALSE;

		for (GList *l = c->waiters; l; l = l->next)
			if (!strcmp(((Waiter *)l->data)->path, j->path))
				waited = TRUE;
		if (!waited && (!keep || !g_strv_contains(keep, j->path))) {
			g_cancellable_cancel(j->cancel);
			g_ptr_array_remove_index(c->pending, i);
			g_hash_table_remove(c->jobs, j->path);
			/* Its worker will find another job or none; the job
			 * itself goes through job_done for its ref. */
			j->started = TRUE;
			g_idle_add(job_done, j);
			continue;
		}
		i++;
	}
	g_mutex_unlock(&c->lock);
}
