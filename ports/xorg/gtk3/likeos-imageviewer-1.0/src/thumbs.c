// LikeOS -- LikeOS Image Viewer: thumbnails for the filmstrip.
//
// A small picture per file, made in a worker thread and kept in memory for
// the folder being browsed.  They are shared with every other program
// through the per-user thumbnail cache (~/.cache/thumbnails/normal, 128
// pixels, named after the MD5 of the file's URI and stamped with its
// modification time -- the same cache LikeOS Explorer reads and writes):
// a thumbnail found there and still current is taken as it is, and one the
// viewer had to make is stored there for next time.
//
// Requests are served most recent first: scrolling the filmstrip asks for
// what is coming into view, and that is what should appear first, not the
// stretch already scrolled past.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "imageviewer.h"

#define IV_THUMBS_KEEP 600	/* in memory */
#define IV_THUMB_CACHE_SIZE 128	/* the "normal" size of the shared cache */

typedef struct {
	IvThumbs *t;
	char *path;
	GdkPixbuf *pb;
} Result;

struct IvThumbs {
	gint ref;
	gboolean dead;
	int size;		/* the long edge wanted */
	IvThumbReady ready;
	gpointer ready_data;
	GHashTable *mem;	/* path -> GdkPixbuf* (NULL value = failed) */
	GQueue order;		/* paths in `mem', oldest first */
	GHashTable *asked;	/* paths requested and not answered */
	GMutex lock;
	GQueue queue;		/* char*, most recent at the head */
	GThreadPool *pool;
};

static void thumbs_unref(IvThumbs *t)
{
	if (!g_atomic_int_dec_and_test(&t->ref))
		return;
	g_hash_table_unref(t->mem);
	g_hash_table_unref(t->asked);
	g_queue_clear_full(&t->order, g_free);
	g_queue_clear_full(&t->queue, g_free);
	g_mutex_clear(&t->lock);
	g_free(t);
}

/* ---- the shared cache ------------------------------------------------------ */

static char *cache_file_for(const char *uri)
{
	char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	char *name = g_strconcat(md5, ".png", NULL);
	char *p = g_build_filename(g_get_user_cache_dir(), "thumbnails",
				   "normal", name, NULL);

	g_free(name);
	g_free(md5);
	return p;
}

static GdkPixbuf *cached_thumb(const char *cache_path, gint64 mtime)
{
	GdkPixbuf *pb = gdk_pixbuf_new_from_file(cache_path, NULL);
	const char *stamp;

	if (!pb)
		return NULL;
	stamp = gdk_pixbuf_get_option(pb, "tEXt::Thumb::MTime");
	if (!stamp || g_ascii_strtoll(stamp, NULL, 10) != mtime) {
		g_object_unref(pb);
		return NULL;
	}
	return pb;
}

static void store_thumb(GdkPixbuf *pb, const char *cache_path, const char *uri,
			gint64 mtime, goffset size)
{
	char *dir = g_path_get_dirname(cache_path);
	char *tmp = g_strdup_printf("%s.%d.tmp", cache_path, (int)getpid());
	char *mt = g_strdup_printf("%" G_GINT64_FORMAT, mtime);
	char *sz = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)size);
	char *w = g_strdup_printf("%d", gdk_pixbuf_get_width(pb));
	char *h = g_strdup_printf("%d", gdk_pixbuf_get_height(pb));

	if (g_mkdir_with_parents(dir, 0700) == 0 &&
	    gdk_pixbuf_save(pb, tmp, "png", NULL, "tEXt::Thumb::URI", uri,
			    "tEXt::Thumb::MTime", mt, "tEXt::Thumb::Size", sz,
			    "tEXt::Software", IV_APP_NAME, NULL)) {
		chmod(tmp, 0600);
		if (rename(tmp, cache_path) != 0)
			unlink(tmp);
	} else {
		unlink(tmp);
	}
	g_free(h);
	g_free(w);
	g_free(sz);
	g_free(mt);
	g_free(tmp);
	g_free(dir);
}

/* A picture decoded straight at about `size' pixels: the JPEG loader
 * scales while it decodes, so a photograph costs a fraction of a full
 * load.  Turned as its EXIF says. */
static GdkPixbuf *make_thumb(const char *path, int size)
{
	GdkPixbuf *pb = NULL, *oriented;
	IvExif ex;
	int orient = 1;
	char *mime = iv_mime_for_path(path);

	if (!strcmp(mime, "image/webp")) {
		IvImage *img = iv_webp_load(path, NULL);

		if (img) {
			double k = (double)size / MAX(img->w, img->h);
			IvImage *s = k < 1 ? iv_image_scale(img, MAX(1, (int)(img->w * k)),
							    MAX(1, (int)(img->h * k)), 1) :
					     iv_image_copy(img);

			pb = iv_pixbuf_from_image(s);
			iv_image_free(s);
			iv_image_free(img);
		}
	} else {
		pb = gdk_pixbuf_new_from_file_at_size(path, size, size, NULL);
	}
	if (!pb) {
		g_free(mime);
		return NULL;
	}
	if (iv_exif_read_file(path, &ex) >= 0 && ex.orientation > 1) {
		orient = ex.orientation;
	} else {
		const char *o = gdk_pixbuf_get_option(pb, "orientation");

		orient = o ? atoi(o) : 1;
	}
	oriented = iv_pixbuf_orient(pb, orient);
	g_object_unref(pb);
	g_free(mime);
	return oriented;
}

/* The thumbnail scaled down to the size the filmstrip draws. */
static GdkPixbuf *fit(GdkPixbuf *pb, int size)
{
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	double k = (double)size / MAX(w, h);

	if (k >= 1)
		return g_object_ref(pb);
	return gdk_pixbuf_scale_simple(pb, MAX(1, (int)(w * k + 0.5)),
				       MAX(1, (int)(h * k + 0.5)),
				       GDK_INTERP_BILINEAR);
}

/* ---- the worker -------------------------------------------------------------- */

static gboolean result_ready(gpointer data)
{
	Result *r = data;
	IvThumbs *t = r->t;

	if (!t->dead) {
		g_hash_table_remove(t->asked, r->path);
		g_hash_table_replace(t->mem, g_strdup(r->path),
				     r->pb ? g_object_ref(r->pb) : NULL);
		g_queue_push_tail(&t->order, g_strdup(r->path));
		while (t->order.length > IV_THUMBS_KEEP) {
			char *old = g_queue_pop_head(&t->order);

			g_hash_table_remove(t->mem, old);
			g_free(old);
		}
		if (t->ready)
			t->ready(r->path, r->pb, t->ready_data);
	}
	g_free(r->path);
	g_clear_object(&r->pb);
	g_free(r);
	thumbs_unref(t);
	return G_SOURCE_REMOVE;
}

static void worker(gpointer unused, gpointer data)
{
	IvThumbs *t = data;
	char *path;

	g_mutex_lock(&t->lock);
	path = g_queue_pop_head(&t->queue);
	g_mutex_unlock(&t->lock);
	if (!path || t->dead) {
		g_free(path);
		thumbs_unref(t);
		return;
	}

	Result *r = g_new0(Result, 1);
	struct stat st;
	GFile *f = g_file_new_for_path(path);
	char *uri = g_file_get_uri(f);
	char *cache_path = cache_file_for(uri);
	GdkPixbuf *pb = NULL;

	g_object_unref(f);
	r->t = t;
	r->path = path;
	if (stat(path, &st) == 0) {
		pb = cached_thumb(cache_path, (gint64)st.st_mtime);
		if (!pb) {
			pb = make_thumb(path, IV_THUMB_CACHE_SIZE);
			if (pb)
				store_thumb(pb, cache_path, uri, (gint64)st.st_mtime,
					    st.st_size);
		}
	}
	if (pb) {
		r->pb = fit(pb, t->size);
		g_object_unref(pb);
	}
	g_free(cache_path);
	g_free(uri);
	/* `t' goes on to result_ready with this worker's ref. */
	g_idle_add(result_ready, r);
}

/* ---- the interface ------------------------------------------------------------- */

/* The memory table keeps NULL for a file that had no thumbnail. */
static void unref_or_null(gpointer p)
{
	if (p)
		g_object_unref(p);
}

IvThumbs *iv_thumbs_new(int size, IvThumbReady ready, gpointer data)
{
	IvThumbs *t = g_new0(IvThumbs, 1);

	t->ref = 1;
	t->size = size;
	t->ready = ready;
	t->ready_data = data;
	t->mem = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
				       unref_or_null);
	t->asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_mutex_init(&t->lock);
	g_queue_init(&t->queue);
	g_queue_init(&t->order);
	t->pool = g_thread_pool_new(worker, t, 1, FALSE, NULL);
	return t;
}

void iv_thumbs_free(IvThumbs *t)
{
	if (!t)
		return;
	t->dead = TRUE;
	t->ready = NULL;
	g_mutex_lock(&t->lock);
	g_queue_clear_full(&t->queue, g_free);
	g_mutex_unlock(&t->lock);
	g_thread_pool_free(t->pool, FALSE, FALSE);
	thumbs_unref(t);
}

GdkPixbuf *iv_thumbs_get(IvThumbs *t, const char *path)
{
	gpointer pb;

	if (g_hash_table_lookup_extended(t->mem, path, NULL, &pb))
		return pb; /* NULL: this one could not be made */
	g_mutex_lock(&t->lock);
	if (g_hash_table_contains(t->asked, path)) {
		/* Asked before: move it to the front of the queue. */
		for (GList *l = t->queue.head; l; l = l->next)
			if (!strcmp(l->data, path)) {
				g_queue_unlink(&t->queue, l);
				g_queue_push_head_link(&t->queue, l);
				break;
			}
		g_mutex_unlock(&t->lock);
		return NULL;
	}
	g_hash_table_add(t->asked, g_strdup(path));
	g_queue_push_head(&t->queue, g_strdup(path));
	g_mutex_unlock(&t->lock);
	g_atomic_int_inc(&t->ref);
	g_thread_pool_push(t->pool, t, NULL);
	return NULL;
}

void iv_thumbs_forget(IvThumbs *t, const char *path)
{
	if (!t || !path)
		return;
	g_hash_table_remove(t->mem, path);
	for (GList *l = t->order.head; l; l = l->next)
		if (!strcmp(l->data, path)) {
			g_free(l->data);
			g_queue_delete_link(&t->order, l);
			break;
		}
}

void iv_thumbs_clear_queue(IvThumbs *t)
{
	g_mutex_lock(&t->lock);
	/* The pushes already made find an empty queue and end. */
	for (GList *l = t->queue.head; l; l = l->next)
		g_hash_table_remove(t->asked, l->data);
	g_queue_clear_full(&t->queue, g_free);
	g_queue_init(&t->queue);
	g_mutex_unlock(&t->lock);
}
