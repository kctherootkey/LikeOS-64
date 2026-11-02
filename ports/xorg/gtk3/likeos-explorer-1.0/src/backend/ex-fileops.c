// LikeOS -- File Explorer: file operations.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-fileops.h"
#include "backend/ex-location.h"
#include "backend/ex-trash.h"
#include "backend/ex-util.h"

#include <string.h>
#include <unistd.h>

/* The longest a worker waits between looks at its cancellable while a
 * question is open. */
#define EX_ASK_POLL_US (200 * 1000)
/* Least time between two "progress" signals. */
#define EX_PROGRESS_US (100 * 1000)

typedef struct {
	GFile *src;
	GFile *dst; /* exact destination, or NULL: dst_dir + src's name */
} Item;

struct _ExOperation {
	GObject parent;
	ExOpKind kind;
	ExOpState state;
	char *title;
	GPtrArray *sources; /* GFile */
	GFile *dest_dir;
	GPtrArray *items; /* Item */
	char *name; /* rename, mkdir, newfile */
	GFile *template_file;
	guint32 mode;
	GCancellable *cancel;
	GMainContext *ctx;

	GMutex lock;
	GCond cond;
	/* Progress (under lock). */
	char *current;
	guint64 bytes_done, bytes_total, bytes_base;
	guint items_done, items_total;
	gboolean counting;
	gboolean progress_pending;
	gint64 last_emit;
	gint64 rate_t0;
	guint64 rate_b0;
	double rate;
	/* The open question (under lock). */
	gboolean waiting;
	gboolean answered;
	int answer;
	ExConflict conflict;
	ExIssue issue;
	gboolean conflict_all, issue_all;
	ExConflictChoice conflict_all_choice;
	ExIssueChoice issue_all_choice;
	ExConflictChoice def_conflict;
	ExIssueChoice def_issue;

	GPtrArray *results; /* ExOpResult */
	GPtrArray *errors; /* char */
};

G_DEFINE_TYPE(ExOperation, ex_operation, G_TYPE_OBJECT)

enum { SIG_PROGRESS, SIG_CONFLICT, SIG_ISSUE, SIG_FINISHED, N_SIGS };
static guint sigs[N_SIGS];

static void item_free(Item *it)
{
	g_clear_object(&it->src);
	g_clear_object(&it->dst);
	g_free(it);
}

static void result_free(ExOpResult *r)
{
	g_clear_object(&r->source);
	g_clear_object(&r->dest);
	g_free(r);
}

static void conflict_clear(ExConflict *c)
{
	g_clear_object(&c->source);
	g_clear_object(&c->dest);
	memset(c, 0, sizeof(*c));
}

static void issue_clear(ExIssue *i)
{
	g_clear_object(&i->file);
	g_free(i->message);
	g_clear_error(&i->error);
	memset(i, 0, sizeof(*i));
}

static void ex_operation_finalize(GObject *o)
{
	ExOperation *op = EX_OPERATION(o);

	g_free(op->title);
	g_ptr_array_unref(op->sources);
	g_clear_object(&op->dest_dir);
	g_ptr_array_unref(op->items);
	g_free(op->name);
	g_clear_object(&op->template_file);
	g_clear_object(&op->cancel);
	g_main_context_unref(op->ctx);
	g_mutex_clear(&op->lock);
	g_cond_clear(&op->cond);
	g_free(op->current);
	conflict_clear(&op->conflict);
	issue_clear(&op->issue);
	g_ptr_array_unref(op->results);
	g_ptr_array_unref(op->errors);
	G_OBJECT_CLASS(ex_operation_parent_class)->finalize(o);
}

static void ex_operation_class_init(ExOperationClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->finalize = ex_operation_finalize;
	sigs[SIG_PROGRESS] = g_signal_new("progress", t, G_SIGNAL_RUN_LAST, 0,
					  NULL, NULL, NULL, G_TYPE_NONE, 0);
	sigs[SIG_CONFLICT] = g_signal_new("conflict", t, G_SIGNAL_RUN_LAST, 0,
					  NULL, NULL, NULL, G_TYPE_NONE, 1,
					  G_TYPE_POINTER);
	sigs[SIG_ISSUE] = g_signal_new("issue", t, G_SIGNAL_RUN_LAST, 0, NULL,
				       NULL, NULL, G_TYPE_NONE, 1,
				       G_TYPE_POINTER);
	sigs[SIG_FINISHED] = g_signal_new("finished", t, G_SIGNAL_RUN_LAST, 0,
					  NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void ex_operation_init(ExOperation *op)
{
	op->sources = g_ptr_array_new_with_free_func(g_object_unref);
	op->items = g_ptr_array_new_with_free_func((GDestroyNotify)item_free);
	op->results =
		g_ptr_array_new_with_free_func((GDestroyNotify)result_free);
	op->errors = g_ptr_array_new_with_free_func(g_free);
	op->cancel = g_cancellable_new();
	op->ctx = g_main_context_ref_thread_default();
	g_mutex_init(&op->lock);
	g_cond_init(&op->cond);
	op->def_conflict = EX_CONFLICT_SKIP;
	op->def_issue = EX_ISSUE_SKIP;
	op->state = EX_OP_QUEUED;
}

/* ---- construction -------------------------------------------------------- */

static char *dir_label(GFile *dir)
{
	ExLocation *l = ex_location_new_folder(dir);
	char *s = ex_location_display_name(l);

	ex_location_unref(l);
	return s;
}

static char *items_label(GPtrArray *srcs)
{
	if (srcs->len == 1) {
		char *b = g_file_get_basename(g_ptr_array_index(srcs, 0));
		char *s = g_strdup_printf("“%s”", b ? b : "item");

		g_free(b);
		return s;
	}
	return g_strdup_printf("%u items", srcs->len);
}

static ExOperation *op_new(ExOpKind kind, GPtrArray *sources)
{
	ExOperation *op = g_object_new(EX_TYPE_OPERATION, NULL);

	op->kind = kind;
	if (sources)
		for (guint i = 0; i < sources->len; i++) {
			GFile *f = g_ptr_array_index(sources, i);
			Item *it = g_new0(Item, 1);

			g_ptr_array_add(op->sources, g_object_ref(f));
			it->src = g_object_ref(f);
			g_ptr_array_add(op->items, it);
		}
	return op;
}

static ExOperation *op_new_to_dir(ExOpKind kind, const char *verb,
				  GPtrArray *sources, GFile *dest_dir)
{
	ExOperation *op = op_new(kind, sources);
	char *what = items_label(sources), *where = dir_label(dest_dir);

	op->dest_dir = g_object_ref(dest_dir);
	op->title = g_strdup_printf("%s %s to %s", verb, what, where);
	g_free(what);
	g_free(where);
	return op;
}

ExOperation *ex_operation_new_copy(GPtrArray *sources, GFile *dest_dir)
{
	return op_new_to_dir(EX_OP_COPY, "Copying", sources, dest_dir);
}

ExOperation *ex_operation_new_move(GPtrArray *sources, GFile *dest_dir)
{
	return op_new_to_dir(EX_OP_MOVE, "Moving", sources, dest_dir);
}

ExOperation *ex_operation_new_link(GPtrArray *sources, GFile *dest_dir)
{
	return op_new_to_dir(EX_OP_LINK, "Creating shortcuts for", sources,
			     dest_dir);
}

ExOperation *ex_operation_new_move_exact(GPtrArray *sources, GPtrArray *dests)
{
	g_return_val_if_fail(sources->len == dests->len, NULL);
	ExOperation *op = op_new(EX_OP_MOVE, sources);
	char *what = items_label(sources);

	for (guint i = 0; i < op->items->len; i++) {
		Item *it = g_ptr_array_index(op->items, i);

		it->dst = g_object_ref(g_ptr_array_index(dests, i));
	}
	op->title = g_strdup_printf("Moving %s back", what);
	g_free(what);
	return op;
}

ExOperation *ex_operation_new_trash(GPtrArray *sources)
{
	ExOperation *op = op_new(EX_OP_TRASH, sources);
	char *what = items_label(sources);

	op->title = g_strdup_printf("Moving %s to the Trash", what);
	g_free(what);
	return op;
}

ExOperation *ex_operation_new_delete(GPtrArray *sources)
{
	ExOperation *op = op_new(EX_OP_DELETE, sources);
	char *what = items_label(sources);

	op->title = g_strdup_printf("Deleting %s", what);
	g_free(what);
	return op;
}

ExOperation *ex_operation_new_rename(GFile *file, const char *new_name)
{
	GPtrArray *one = g_ptr_array_new();
	ExOperation *op;
	char *old = g_file_get_basename(file);

	g_ptr_array_add(one, file);
	op = op_new(EX_OP_RENAME, one);
	g_ptr_array_unref(one);
	op->name = g_strdup(new_name);
	op->title = g_strdup_printf("Renaming “%s” to “%s”", old, new_name);
	g_free(old);
	return op;
}

ExOperation *ex_operation_new_mkdir(GFile *dir, const char *name)
{
	ExOperation *op = op_new(EX_OP_MKDIR, NULL);

	op->dest_dir = g_object_ref(dir);
	op->name = g_strdup(name);
	op->title = g_strdup("Creating a folder");
	return op;
}

ExOperation *ex_operation_new_newfile(GFile *dir, const char *name,
				      GFile *template_file)
{
	ExOperation *op = op_new(EX_OP_NEWFILE, NULL);

	op->dest_dir = g_object_ref(dir);
	op->name = g_strdup(name ? name : "New file");
	op->template_file = template_file ? g_object_ref(template_file) : NULL;
	op->title = g_strdup_printf("Creating “%s”", op->name);
	return op;
}

ExOperation *ex_operation_new_restore(GPtrArray *trash_items)
{
	ExOperation *op = op_new(EX_OP_RESTORE, trash_items);
	char *what = items_label(trash_items);

	op->title = g_strdup_printf("Restoring %s", what);
	g_free(what);
	return op;
}

ExOperation *ex_operation_new_empty_trash(void)
{
	ExOperation *op = op_new(EX_OP_EMPTY_TRASH, NULL);

	op->title = g_strdup("Emptying the Trash");
	return op;
}

ExOperation *ex_operation_new_chmod(GFile *file, guint32 mode)
{
	GPtrArray *one = g_ptr_array_new();
	ExOperation *op;

	g_ptr_array_add(one, file);
	op = op_new(EX_OP_CHMOD, one);
	g_ptr_array_unref(one);
	op->mode = mode;
	op->title = g_strdup("Changing permissions");
	return op;
}

void ex_operation_set_defaults(ExOperation *op, ExConflictChoice conflict,
			       ExIssueChoice issue)
{
	op->def_conflict = conflict;
	op->def_issue = issue;
}

/* ---- talking to the main loop -------------------------------------------- */

typedef struct {
	ExOperation *op;
	guint sig;
	gpointer arg;
} Emit;

static gboolean do_emit(gpointer data)
{
	Emit *e = data;

	if (e->sig == sigs[SIG_PROGRESS]) {
		g_mutex_lock(&e->op->lock);
		e->op->progress_pending = FALSE;
		e->op->last_emit = g_get_monotonic_time();
		g_mutex_unlock(&e->op->lock);
	}
	if (e->arg)
		g_signal_emit(e->op, e->sig, 0, e->arg);
	else
		g_signal_emit(e->op, e->sig, 0);
	return G_SOURCE_REMOVE;
}

static void emit_free(gpointer data)
{
	Emit *e = data;

	g_object_unref(e->op);
	g_free(e);
}

static void emit_on_main(ExOperation *op, guint sig, gpointer arg)
{
	Emit *e = g_new0(Emit, 1);

	e->op = g_object_ref(op);
	e->sig = sig;
	e->arg = arg;
	ex_run_on(op->ctx, do_emit, e, emit_free);
}

/* From the worker, with op->lock held. */
static void progress_locked(ExOperation *op)
{
	gint64 now = g_get_monotonic_time();

	if (now - op->rate_t0 >= G_USEC_PER_SEC) {
		if (op->rate_t0)
			op->rate = (double)(op->bytes_done - op->rate_b0) *
				   G_USEC_PER_SEC / (double)(now - op->rate_t0);
		op->rate_t0 = now;
		op->rate_b0 = op->bytes_done;
	}
	if (op->progress_pending || now - op->last_emit < EX_PROGRESS_US)
		return;
	op->progress_pending = TRUE;
	emit_on_main(op, sigs[SIG_PROGRESS], NULL);
}

static void set_current(ExOperation *op, GFile *f)
{
	char *name = f ? g_file_get_basename(f) : NULL;

	g_mutex_lock(&op->lock);
	g_free(op->current);
	op->current = name;
	progress_locked(op);
	g_mutex_unlock(&op->lock);
}

static void item_done(ExOperation *op, guint64 bytes)
{
	g_mutex_lock(&op->lock);
	op->items_done++;
	op->bytes_base += bytes;
	op->bytes_done = op->bytes_base;
	progress_locked(op);
	g_mutex_unlock(&op->lock);
}

static gboolean cancelled(ExOperation *op)
{
	return g_cancellable_is_cancelled(op->cancel);
}

/* Wait for the main loop's answer to the question just posted. */
static int wait_answer(ExOperation *op, int fallback)
{
	int a;

	g_mutex_lock(&op->lock);
	while (!op->answered && !cancelled(op)) {
		gint64 until = g_get_monotonic_time() + EX_ASK_POLL_US;

		g_cond_wait_until(&op->cond, &op->lock, until);
	}
	a = op->answered ? op->answer : fallback;
	op->waiting = FALSE;
	op->answered = FALSE;
	g_mutex_unlock(&op->lock);
	return a;
}

static void query_basic(GFile *f, gboolean *is_dir, guint64 *size,
			gint64 *mtime)
{
	GFileInfo *i = g_file_query_info(
		f,
		G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE
		"," G_FILE_ATTRIBUTE_TIME_MODIFIED,
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);

	*is_dir = FALSE;
	*size = 0;
	*mtime = 0;
	if (!i)
		return;
	*is_dir = g_file_info_get_file_type(i) == G_FILE_TYPE_DIRECTORY;
	*size = (guint64)g_file_info_get_size(i);
	*mtime = (gint64)g_file_info_get_attribute_uint64(
		i, G_FILE_ATTRIBUTE_TIME_MODIFIED);
	g_object_unref(i);
}

static ExConflictChoice ask_conflict(ExOperation *op, GFile *src, GFile *dst)
{
	ExConflictChoice c;

	g_mutex_lock(&op->lock);
	if (op->conflict_all) {
		c = op->conflict_all_choice;
		g_mutex_unlock(&op->lock);
		return c;
	}
	g_mutex_unlock(&op->lock);
	if (!g_signal_has_handler_pending(op, sigs[SIG_CONFLICT], 0, TRUE))
		return op->def_conflict;

	g_mutex_lock(&op->lock);
	conflict_clear(&op->conflict);
	op->conflict.source = g_object_ref(src);
	op->conflict.dest = g_object_ref(dst);
	g_mutex_unlock(&op->lock);
	/* Queried outside the lock: the main loop never waits on it. */
	ExConflict tmp = { 0 };
	query_basic(src, &tmp.source_is_dir, &tmp.source_size,
		    &tmp.source_mtime);
	query_basic(dst, &tmp.dest_is_dir, &tmp.dest_size, &tmp.dest_mtime);
	g_mutex_lock(&op->lock);
	op->conflict.source_is_dir = tmp.source_is_dir;
	op->conflict.source_size = tmp.source_size;
	op->conflict.source_mtime = tmp.source_mtime;
	op->conflict.dest_is_dir = tmp.dest_is_dir;
	op->conflict.dest_size = tmp.dest_size;
	op->conflict.dest_mtime = tmp.dest_mtime;
	op->conflict.same_file = g_file_equal(src, dst);
	op->waiting = TRUE;
	op->answered = FALSE;
	g_mutex_unlock(&op->lock);
	emit_on_main(op, sigs[SIG_CONFLICT], &op->conflict);
	c = (ExConflictChoice)wait_answer(op, EX_CONFLICT_CANCEL);
	if (c == EX_CONFLICT_CANCEL)
		g_cancellable_cancel(op->cancel);
	return c;
}

/* Why, in a few words: the error's own message repeats the whole path
 * ("Error removing file /home/…/x: Permission denied"), which the
 * question already names. */
static const char *reason(const GError *err)
{
	const char *colon;

	if (err->domain == G_IO_ERROR) {
		switch (err->code) {
		case G_IO_ERROR_PERMISSION_DENIED:
			return "You do not have permission";
		case G_IO_ERROR_NOT_EMPTY:
			return "The folder is not empty";
		case G_IO_ERROR_NO_SPACE:
			return "There is not enough space";
		case G_IO_ERROR_NOT_FOUND:
			return "It no longer exists";
		case G_IO_ERROR_READ_ONLY:
			return "The disk is read-only";
		case G_IO_ERROR_BUSY:
			return "It is in use";
		case G_IO_ERROR_FILENAME_TOO_LONG:
			return "The name is too long";
		default:
			break;
		}
	}
	colon = strrchr(err->message, ':');
	return colon && colon[1] == ' ' && colon[2] ? colon + 2 : err->message;
}

static ExIssueChoice ask_issue(ExOperation *op, GFile *f, const char *verb,
			       const GError *err, gboolean can_retry,
			       gboolean can_delete_instead)
{
	ExIssueChoice c;
	char *name = g_file_get_basename(f);
	char *msg = g_strdup_printf("Could not %s “%s”: %s", verb,
				    name ? name : "?", reason(err));

	g_free(name);
	g_mutex_lock(&op->lock);
	if (op->issue_all &&
	    (op->issue_all_choice != EX_ISSUE_DELETE_INSTEAD ||
	     can_delete_instead) &&
	    (op->issue_all_choice != EX_ISSUE_RETRY || can_retry)) {
		c = op->issue_all_choice;
		g_mutex_unlock(&op->lock);
		if (c == EX_ISSUE_SKIP)
			g_ptr_array_add(op->errors, msg);
		else
			g_free(msg);
		return c;
	}
	g_mutex_unlock(&op->lock);
	if (!g_signal_has_handler_pending(op, sigs[SIG_ISSUE], 0, TRUE)) {
		c = op->def_issue;
		if ((c == EX_ISSUE_RETRY && !can_retry) ||
		    (c == EX_ISSUE_DELETE_INSTEAD && !can_delete_instead))
			c = EX_ISSUE_SKIP;
	} else {
		g_mutex_lock(&op->lock);
		issue_clear(&op->issue);
		op->issue.file = g_object_ref(f);
		op->issue.message = g_strdup(msg);
		op->issue.error = g_error_copy(err);
		op->issue.can_retry = can_retry;
		op->issue.can_delete_instead = can_delete_instead;
		op->waiting = TRUE;
		op->answered = FALSE;
		g_mutex_unlock(&op->lock);
		emit_on_main(op, sigs[SIG_ISSUE], &op->issue);
		c = (ExIssueChoice)wait_answer(op, EX_ISSUE_CANCEL);
	}
	if (c == EX_ISSUE_CANCEL)
		g_cancellable_cancel(op->cancel);
	if (c == EX_ISSUE_SKIP || c == EX_ISSUE_CANCEL)
		g_ptr_array_add(op->errors, msg);
	else
		g_free(msg);
	return c;
}

void ex_conflict_resolve(ExOperation *op, ExConflictChoice choice,
			 gboolean apply_to_all)
{
	g_mutex_lock(&op->lock);
	if (op->waiting) {
		op->answer = (int)choice;
		op->answered = TRUE;
		if (apply_to_all && choice != EX_CONFLICT_CANCEL) {
			op->conflict_all = TRUE;
			op->conflict_all_choice = choice;
		}
		g_cond_broadcast(&op->cond);
	}
	g_mutex_unlock(&op->lock);
}

void ex_issue_resolve(ExOperation *op, ExIssueChoice choice,
		      gboolean apply_to_all)
{
	g_mutex_lock(&op->lock);
	if (op->waiting) {
		op->answer = (int)choice;
		op->answered = TRUE;
		if (apply_to_all && choice != EX_ISSUE_CANCEL) {
			op->issue_all = TRUE;
			op->issue_all_choice = choice;
		}
		g_cond_broadcast(&op->cond);
	}
	g_mutex_unlock(&op->lock);
}

static void add_result(ExOperation *op, GFile *src, GFile *dst)
{
	ExOpResult *r = g_new0(ExOpResult, 1);

	r->source = src ? g_object_ref(src) : NULL;
	r->dest = dst ? g_object_ref(dst) : NULL;
	g_mutex_lock(&op->lock);
	g_ptr_array_add(op->results, r);
	g_mutex_unlock(&op->lock);
}

/* ---- counting ------------------------------------------------------------ */

/* Items and bytes below `f', links not followed. */
static void count_tree(ExOperation *op, GFile *f, guint64 *bytes,
		       guint *items)
{
	GFileInfo *i = g_file_query_info(
		f, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, op->cancel, NULL);

	if (!i)
		return;
	(*items)++;
	if (g_file_info_get_file_type(i) != G_FILE_TYPE_DIRECTORY) {
		*bytes += (guint64)g_file_info_get_size(i);
		g_object_unref(i);
		return;
	}
	g_object_unref(i);
	GFileEnumerator *en = g_file_enumerate_children(
		f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		op->cancel, NULL);
	GFileInfo *ci;

	if (!en)
		return;
	while (!cancelled(op) &&
	       (ci = g_file_enumerator_next_file(en, op->cancel, NULL))) {
		GFile *c = g_file_enumerator_get_child(en, ci);

		count_tree(op, c, bytes, items);
		g_object_unref(c);
		g_object_unref(ci);
	}
	g_object_unref(en);
}

static void count_all(ExOperation *op, gboolean deep)
{
	guint64 bytes = 0;
	guint items = 0;

	g_mutex_lock(&op->lock);
	op->counting = TRUE;
	g_mutex_unlock(&op->lock);
	for (guint i = 0; i < op->items->len && !cancelled(op); i++) {
		Item *it = g_ptr_array_index(op->items, i);

		if (deep)
			count_tree(op, it->src, &bytes, &items);
		else
			items++;
	}
	g_mutex_lock(&op->lock);
	op->counting = FALSE;
	op->bytes_total = bytes;
	op->items_total = items;
	progress_locked(op);
	g_mutex_unlock(&op->lock);
}

/* ---- the work ------------------------------------------------------------ */

static GFile *item_dest(ExOperation *op, Item *it)
{
	if (it->dst)
		return g_object_ref(it->dst);
	char *b = g_file_get_basename(it->src);
	GFile *d = g_file_get_child(op->dest_dir, b);

	g_free(b);
	return d;
}

static gboolean exists_nofollow(GFile *f)
{
	GFileInfo *i = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_TYPE,
					 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
					 NULL, NULL);

	if (!i)
		return FALSE;
	g_object_unref(i);
	return TRUE;
}

static GFileType type_nofollow(GFile *f)
{
	GFileInfo *i = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_TYPE,
					 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
					 NULL, NULL);
	GFileType t;

	if (!i)
		return G_FILE_TYPE_UNKNOWN;
	t = g_file_info_get_file_type(i);
	g_object_unref(i);
	return t;
}

/* `dst' with "(2)", "(3)" ... until the name is free. */
static GFile *keep_both_name(GFile *dst)
{
	GFile *dir = g_file_get_parent(dst);
	char *base = g_file_get_basename(dst);
	gboolean is_dir = type_nofollow(dst) == G_FILE_TYPE_DIRECTORY;
	char *name = ex_unique_name(dir, base, !is_dir);
	GFile *f = g_file_get_child(dir, name);

	g_free(name);
	g_free(base);
	g_object_unref(dir);
	return f;
}

static gboolean delete_tree(ExOperation *op, GFile *f, gboolean count);

typedef struct {
	ExOperation *op;
	guint64 base;
} CopyProg;

static void copy_progress(goffset cur, goffset total, gpointer data)
{
	CopyProg *p = data;

	g_mutex_lock(&p->op->lock);
	p->op->bytes_done = p->base + (guint64)cur;
	progress_locked(p->op);
	g_mutex_unlock(&p->op->lock);
}

/* A hidden name in `dst''s folder for the copy in progress. */
static GFile *part_name(GFile *dst)
{
	static gint seq;
	GFile *dir = g_file_get_parent(dst);
	char *base = g_file_get_basename(dst);
	char *name = g_strdup_printf(".%s.likeos-part-%d-%d", base,
				     (int)getpid(), g_atomic_int_add(&seq, 1));
	GFile *f = g_file_get_child(dir, name);

	g_free(name);
	g_free(base);
	g_object_unref(dir);
	return f;
}

/* Resolve a taken destination.  Returns the destination to use (a new
 * reference; possibly a "(2)" name), or NULL to skip.  `*replace' says
 * whether the existing item is to be overwritten, `*merge' whether a folder
 * goes into the existing folder. */
static GFile *settle_dest(ExOperation *op, GFile *src, GFile *dst,
			  gboolean *replace, gboolean *merge)
{
	*replace = FALSE;
	*merge = FALSE;
	if (!exists_nofollow(dst))
		return g_object_ref(dst);
	if (g_file_equal(src, dst) &&
	    (op->kind == EX_OP_COPY || op->kind == EX_OP_LINK))
		return keep_both_name(dst); /* a copy next to the original */
	if (g_file_equal(src, dst))
		return NULL; /* moving onto itself: nothing to do */

	switch (ask_conflict(op, src, dst)) {
	case EX_CONFLICT_REPLACE:
		*replace = TRUE;
		return g_object_ref(dst);
	case EX_CONFLICT_KEEP_BOTH:
		return keep_both_name(dst);
	case EX_CONFLICT_MERGE:
		if (type_nofollow(src) == G_FILE_TYPE_DIRECTORY &&
		    type_nofollow(dst) == G_FILE_TYPE_DIRECTORY) {
			*merge = TRUE;
			return g_object_ref(dst);
		}
		*replace = TRUE; /* merge means replace for files */
		return g_object_ref(dst);
	case EX_CONFLICT_SKIP:
	case EX_CONFLICT_CANCEL:
	default:
		return NULL;
	}
}

/* Copy `src' to `dst' (dst's name already settled except for replace /
 * merge).  Returns TRUE when everything below src arrived. */
static gboolean copy_tree(ExOperation *op, GFile *src, GFile *dst,
			  gboolean replace, gboolean merge)
{
	GFileType t = type_nofollow(src);

	set_current(op, src);
	if (cancelled(op))
		return FALSE;
	if (t == G_FILE_TYPE_DIRECTORY) {
		if (g_file_has_prefix(dst, src) || g_file_equal(dst, src)) {
			GError *e = g_error_new_literal(
				G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				"The destination folder is inside the source folder");
			ask_issue(op, src, "copy", e, FALSE, FALSE);
			g_error_free(e);
			return FALSE;
		}
		if (replace && !merge && exists_nofollow(dst) &&
		    type_nofollow(dst) != G_FILE_TYPE_DIRECTORY) {
			GError *e = NULL;

			if (!g_file_delete(dst, op->cancel, &e)) {
				ask_issue(op, dst, "replace", e, FALSE, FALSE);
				g_error_free(e);
				return FALSE;
			}
		}
		if (!merge && !(replace && type_nofollow(dst) ==
						   G_FILE_TYPE_DIRECTORY)) {
			for (;;) {
				GError *e = NULL;

				if (g_file_make_directory(dst, op->cancel, &e))
					break;
				ExIssueChoice c = ask_issue(op, dst,
							    "create the folder",
							    e, TRUE, FALSE);
				g_error_free(e);
				if (c != EX_ISSUE_RETRY)
					return FALSE;
			}
		}
		item_done(op, 0);
		gboolean all = TRUE;
		GError *e = NULL;
		GFileEnumerator *en = g_file_enumerate_children(
			src, G_FILE_ATTRIBUTE_STANDARD_NAME,
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, op->cancel, &e);
		if (!en) {
			ask_issue(op, src, "read the folder", e, FALSE, FALSE);
			g_error_free(e);
			return FALSE;
		}
		GFileInfo *ci;
		while (!cancelled(op) &&
		       (ci = g_file_enumerator_next_file(en, op->cancel, NULL))) {
			GFile *c = g_file_enumerator_get_child(en, ci);
			GFile *cd = g_file_get_child(dst, g_file_info_get_name(ci));
			gboolean r = FALSE, m = FALSE;
			GFile *use = settle_dest(op, c, cd, &r, &m);

			if (!use || !copy_tree(op, c, use, r, m))
				all = FALSE;
			g_clear_object(&use);
			g_object_unref(cd);
			g_object_unref(c);
			g_object_unref(ci);
		}
		g_object_unref(en);
		return all && !cancelled(op);
	}

	/* A file, link or special file: to a hidden name first, then into
	 * place in one rename. */
	guint64 size = 0;
	GFileInfo *si = g_file_query_info(src, G_FILE_ATTRIBUTE_STANDARD_SIZE,
					  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
					  NULL, NULL);
	if (si) {
		size = (guint64)g_file_info_get_size(si);
		g_object_unref(si);
	}
	for (;;) {
		GFile *part = part_name(dst);
		GError *e = NULL;
		CopyProg prog = { op, 0 };
		gboolean ok;

		g_mutex_lock(&op->lock);
		prog.base = op->bytes_base;
		g_mutex_unlock(&op->lock);
		ok = g_file_copy(src, part, G_FILE_COPY_NOFOLLOW_SYMLINKS,
				 op->cancel, copy_progress, &prog, &e);
		if (ok) {
			if (replace && type_nofollow(dst) == G_FILE_TYPE_DIRECTORY)
				ok = delete_tree(op, dst, FALSE);
			if (ok)
				ok = g_file_move(part, dst,
						 G_FILE_COPY_NOFOLLOW_SYMLINKS |
							 G_FILE_COPY_NO_FALLBACK_FOR_MOVE |
							 (replace ? G_FILE_COPY_OVERWRITE :
								    0),
						 NULL, NULL, NULL, &e);
		}
		if (!ok)
			(void)g_file_delete(part, NULL, NULL);
		g_object_unref(part);
		if (ok) {
			item_done(op, size);
			return TRUE;
		}
		if (cancelled(op) ||
		    g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_clear_error(&e);
			return FALSE;
		}
		if (!e)
			e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
						"The item could not be replaced");
		ExIssueChoice c = ask_issue(op, src, "copy", e, TRUE, FALSE);
		g_error_free(e);
		if (c != EX_ISSUE_RETRY) {
			item_done(op, size);
			return FALSE;
		}
	}
}

/* Remove `f' and everything below it.  `count': this is the operation's
 * own work (delete, empty trash), not cleanup inside another. */
static gboolean delete_tree(ExOperation *op, GFile *f, gboolean count)
{
	GFileType t = type_nofollow(f);
	gboolean all = TRUE;

	if (cancelled(op))
		return FALSE;
	if (count)
		set_current(op, f);
	if (t == G_FILE_TYPE_DIRECTORY) {
		GFileEnumerator *en = g_file_enumerate_children(
			f, G_FILE_ATTRIBUTE_STANDARD_NAME,
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, op->cancel, NULL);
		GFileInfo *ci;

		if (en) {
			while (!cancelled(op) &&
			       (ci = g_file_enumerator_next_file(en, op->cancel,
								  NULL))) {
				GFile *c = g_file_enumerator_get_child(en, ci);

				if (!delete_tree(op, c, count))
					all = FALSE;
				g_object_unref(c);
				g_object_unref(ci);
			}
			g_object_unref(en);
		}
	}
	if (!all || cancelled(op))
		return FALSE;
	for (;;) {
		GError *e = NULL;

		if (g_file_delete(f, op->cancel, &e)) {
			if (count)
				item_done(op, 0);
			return TRUE;
		}
		if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free(e);
			return FALSE;
		}
		ExIssueChoice c = ask_issue(op, f, "delete", e, TRUE, FALSE);
		g_error_free(e);
		if (c != EX_ISSUE_RETRY)
			return FALSE;
	}
}

static void do_copy(ExOperation *op)
{
	count_all(op, TRUE);
	for (guint i = 0; i < op->items->len && !cancelled(op); i++) {
		Item *it = g_ptr_array_index(op->items, i);
		GFile *dst = item_dest(op, it);
		gboolean r, m;
		GFile *use = settle_dest(op, it->src, dst, &r, &m);

		if (use && copy_tree(op, it->src, use, r, m))
			add_result(op, it->src, use);
		g_clear_object(&use);
		g_object_unref(dst);
	}
}

static gboolean move_one(ExOperation *op, GFile *src, GFile *dst)
{
	gboolean r = FALSE, m = FALSE;
	GFile *use = settle_dest(op, src, dst, &r, &m);
	gboolean ok = FALSE;

	if (!use)
		return FALSE;
	set_current(op, src);
	if (g_file_has_prefix(use, src)) {
		GError *e = g_error_new_literal(
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"The destination folder is inside the source folder");
		ask_issue(op, src, "move", e, FALSE, FALSE);
		g_error_free(e);
		g_object_unref(use);
		return FALSE;
	}
	if (m) {
		/* Folder into an existing folder: each child moves, then the
		 * emptied source goes. */
		gboolean all = TRUE;
		GFileEnumerator *en = g_file_enumerate_children(
			src, G_FILE_ATTRIBUTE_STANDARD_NAME,
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, op->cancel, NULL);
		GFileInfo *ci;

		if (en) {
			while (!cancelled(op) &&
			       (ci = g_file_enumerator_next_file(en, op->cancel,
								  NULL))) {
				GFile *c = g_file_enumerator_get_child(en, ci);
				GFile *cd = g_file_get_child(
					use, g_file_info_get_name(ci));

				if (!move_one(op, c, cd))
					all = FALSE;
				g_object_unref(cd);
				g_object_unref(c);
				g_object_unref(ci);
			}
			g_object_unref(en);
		}
		if (all && !cancelled(op))
			(void)g_file_delete(src, NULL, NULL);
		g_object_unref(use);
		return all;
	}
	for (;;) {
		GError *e = NULL;
		GFileCopyFlags fl = G_FILE_COPY_NOFOLLOW_SYMLINKS |
				    G_FILE_COPY_NO_FALLBACK_FOR_MOVE;

		if (r && type_nofollow(use) == G_FILE_TYPE_DIRECTORY &&
		    !delete_tree(op, use, FALSE))
			break;
		if (r)
			fl |= G_FILE_COPY_OVERWRITE;
		if (g_file_move(src, use, fl, op->cancel, NULL, NULL, &e)) {
			ok = TRUE;
			item_done(op, 0);
			break;
		}
		if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
		    g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE) ||
		    g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE)) {
			/* Another filesystem: copy, and remove the original
			 * only if all of it arrived. */
			g_error_free(e);
			guint64 b = 0;
			guint n = 0;

			count_tree(op, src, &b, &n);
			g_mutex_lock(&op->lock);
			op->bytes_total += b;
			op->items_total += n;
			g_mutex_unlock(&op->lock);
			if (copy_tree(op, src, use, r, FALSE) && !cancelled(op))
				ok = delete_tree(op, src, FALSE);
			break;
		}
		if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_error_free(e);
			break;
		}
		ExIssueChoice c = ask_issue(op, src, "move", e, TRUE, FALSE);
		g_error_free(e);
		if (c != EX_ISSUE_RETRY)
			break;
	}
	if (ok)
		add_result(op, src, use);
	g_object_unref(use);
	return ok;
}

static void do_move(ExOperation *op)
{
	count_all(op, FALSE);
	for (guint i = 0; i < op->items->len && !cancelled(op); i++) {
		Item *it = g_ptr_array_index(op->items, i);
		GFile *dst = item_dest(op, it);

		if (it->dst) {
			/* An exact destination may need its folder back
			 * (undoing a move out of a folder since deleted). */
			GFile *parent = g_file_get_parent(dst);

			if (parent) {
				(void)g_file_make_directory_with_parents(
					parent, NULL, NULL);
				g_object_unref(parent);
			}
		}
		move_one(op, it->src, dst);
		g_object_unref(dst);
	}
}

static void do_link(ExOperation *op)
{
	count_all(op, FALSE);
	for (guint i = 0; i < op->items->len && !cancelled(op); i++) {
		Item *it = g_ptr_array_index(op->items, i);
		GFile *dst = item_dest(op, it);
		gboolean r, m;
		GFile *use = settle_dest(op, it->src, dst, &r, &m);
		char *target = g_file_get_path(it->src);

		g_object_unref(dst);
		if (!use || !target) {
			g_clear_object(&use);
			g_free(target);
			continue;
		}
		for (;;) {
			GError *e = NULL;

			if (r)
				(void)g_file_delete(use, NULL, NULL);
			if (g_file_make_symbolic_link(use, target, op->cancel,
						      &e)) {
				add_result(op, it->src, use);
				item_done(op, 0);
				break;
			}
			ExIssueChoice c = ask_issue(op, use, "create a link to",
						    e, TRUE, FALSE);
			g_error_free(e);
			if (c != EX_ISSUE_RETRY)
				break;
		}
		g_free(target);
		g_object_unref(use);
	}
}

static void do_trash(ExOperation *op)
{
	count_all(op, FALSE);
	for (guint i = 0; i < op->items->len && !cancelled(op); i++) {
		Item *it = g_ptr_array_index(op->items, i);
		char *path = g_file_get_path(it->src);

		set_current(op, it->src);
		for (;;) {
			GError *e = NULL;

			if (g_file_trash(it->src, op->cancel, &e)) {
				GFile *t = path ? ex_trash_find_item(path) :
						  NULL;

				add_result(op, it->src, t);
				g_clear_object(&t);
				item_done(op, 0);
				break;
			}
			if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
				g_error_free(e);
				break;
			}
			/* No trash for this place (a system mount, /ram):
			 * offer deleting outright instead. */
			gboolean no_trash = g_error_matches(
				e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
			ExIssueChoice c = ask_issue(op, it->src,
						    "move to the Trash", e,
						    !no_trash, no_trash);
			g_error_free(e);
			if (c == EX_ISSUE_DELETE_INSTEAD) {
				if (delete_tree(op, it->src, FALSE)) {
					add_result(op, it->src, NULL);
					item_done(op, 0);
				}
				break;
			}
			if (c != EX_ISSUE_RETRY)
				break;
		}
		g_free(path);
	}
}

static void do_delete(ExOperation *op)
{
	count_all(op, TRUE);
	for (guint i = 0; i < op->items->len && !cancelled(op); i++) {
		Item *it = g_ptr_array_index(op->items, i);

		if (delete_tree(op, it->src, TRUE)) {
			/* An item of the Trash takes its record with it. */
			GFile *parent = g_file_get_parent(it->src);
			GFile *tf = ex_trash_files_dir();

			if (parent && g_file_equal(parent, tf)) {
				GFile *rec = ex_trash_info_file_for(it->src);

				(void)g_file_delete(rec, NULL, NULL);
				g_object_unref(rec);
			}
			g_clear_object(&parent);
			g_object_unref(tf);
			add_result(op, it->src, NULL);
		}
	}
}

static void do_rename(ExOperation *op)
{
	Item *it = g_ptr_array_index(op->items, 0);
	const char *why = ex_name_problem(op->name);

	op->items_total = 1;
	set_current(op, it->src);
	if (why) {
		GError *e = g_error_new_literal(G_IO_ERROR,
						G_IO_ERROR_INVALID_FILENAME, why);
		ask_issue(op, it->src, "rename", e, FALSE, FALSE);
		g_error_free(e);
		return;
	}
	GFile *parent = g_file_get_parent(it->src);
	GFile *dst = parent ? g_file_get_child(parent, op->name) : NULL;

	/* Another item already has the name -- unless it is this one under a
	 * different capitalisation on a filesystem that ignores case. */
	if (dst && !g_file_equal(dst, it->src) && exists_nofollow(dst)) {
		GError *e = g_error_new(G_IO_ERROR, G_IO_ERROR_EXISTS,
					"There is already an item named “%s”",
					op->name);
		ask_issue(op, it->src, "rename", e, FALSE, FALSE);
		g_error_free(e);
	} else {
		for (;;) {
			GError *e = NULL;
			GFile *nf = g_file_set_display_name(it->src, op->name,
							    op->cancel, &e);
			if (nf) {
				add_result(op, it->src, nf);
				g_object_unref(nf);
				item_done(op, 0);
				break;
			}
			ExIssueChoice c = ask_issue(op, it->src, "rename", e,
						    TRUE, FALSE);
			g_error_free(e);
			if (c != EX_ISSUE_RETRY)
				break;
		}
	}
	g_clear_object(&dst);
	g_clear_object(&parent);
}

static void do_create(ExOperation *op)
{
	gboolean dir = op->kind == EX_OP_MKDIR;
	const char *want = op->name ? op->name : "New folder";
	char *name;
	GFile *f;

	op->items_total = 1;
	for (;;) {
		GError *e = NULL;
		gboolean ok;

		name = ex_unique_name(op->dest_dir, want, !dir);
		f = g_file_get_child(op->dest_dir, name);
		g_free(name);
		set_current(op, f);
		if (dir) {
			ok = g_file_make_directory(f, op->cancel, &e);
		} else if (op->template_file) {
			ok = g_file_copy(op->template_file, f, G_FILE_COPY_NONE,
					 op->cancel, NULL, NULL, &e);
		} else {
			GFileOutputStream *s = g_file_create(
				f, G_FILE_CREATE_NONE, op->cancel, &e);

			ok = s != NULL;
			if (s) {
				ok = g_output_stream_close(G_OUTPUT_STREAM(s),
							   NULL, &e);
				g_object_unref(s);
			}
		}
		if (ok) {
			add_result(op, NULL, f);
			item_done(op, 0);
			g_object_unref(f);
			return;
		}
		/* Taken between the check and the create: next number. */
		if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
			g_error_free(e);
			g_object_unref(f);
			continue;
		}
		ExIssueChoice c = ask_issue(op, f,
					    dir ? "create the folder" :
						  "create the file",
					    e, TRUE, FALSE);
		g_error_free(e);
		g_object_unref(f);
		if (c != EX_ISSUE_RETRY)
			return;
	}
}

static void do_restore(ExOperation *op)
{
	count_all(op, FALSE);
	for (guint i = 0; i < op->items->len && !cancelled(op); i++) {
		Item *it = g_ptr_array_index(op->items, i);
		char *orig = NULL;
		GError *e = NULL;

		set_current(op, it->src);
		if (!ex_trash_read_info(it->src, &orig, NULL, &e) || !*orig) {
			if (!e)
				e = g_error_new_literal(
					G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"Where the item came from is not recorded");
			ask_issue(op, it->src, "restore", e, FALSE, FALSE);
			g_error_free(e);
			g_free(orig);
			continue;
		}
		GFile *dst = g_file_new_for_path(orig);
		GFile *parent = g_file_get_parent(dst);
		gboolean r = FALSE, m = FALSE;

		if (parent) {
			(void)g_file_make_directory_with_parents(parent, NULL,
								 NULL);
			g_object_unref(parent);
		}
		GFile *use = settle_dest(op, it->src, dst, &r, &m);
		if (use) {
			for (;;) {
				GFileCopyFlags fl = G_FILE_COPY_NOFOLLOW_SYMLINKS |
						    (r ? G_FILE_COPY_OVERWRITE : 0);

				if (r && type_nofollow(use) == G_FILE_TYPE_DIRECTORY)
					(void)delete_tree(op, use, FALSE);
				/* Fallback allowed: the original may be on
				 * another filesystem. */
				if (g_file_move(it->src, use, fl, op->cancel, NULL,
						NULL, &e)) {
					GFile *rec = ex_trash_info_file_for(it->src);

					(void)g_file_delete(rec, NULL, NULL);
					g_object_unref(rec);
					add_result(op, it->src, use);
					item_done(op, 0);
					break;
				}
				ExIssueChoice c = ask_issue(op, it->src, "restore",
							    e, TRUE, FALSE);
				g_clear_error(&e);
				if (c != EX_ISSUE_RETRY)
					break;
			}
			g_object_unref(use);
		}
		g_object_unref(dst);
		g_free(orig);
	}
}

static void do_empty_trash(ExOperation *op)
{
	GFile *files = ex_trash_files_dir();
	GFile *info = ex_trash_info_dir();
	GFileEnumerator *en;
	GFileInfo *ci;
	guint64 b = 0;
	guint n = 0;

	count_tree(op, files, &b, &n);
	g_mutex_lock(&op->lock);
	op->items_total = n > 0 ? n - 1 : 0; /* not files/ itself */
	g_mutex_unlock(&op->lock);
	en = g_file_enumerate_children(files, G_FILE_ATTRIBUTE_STANDARD_NAME,
				       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				       op->cancel, NULL);
	if (en) {
		while (!cancelled(op) &&
		       (ci = g_file_enumerator_next_file(en, op->cancel, NULL))) {
			GFile *c = g_file_enumerator_get_child(en, ci);

			if (delete_tree(op, c, TRUE)) {
				GFile *rec = ex_trash_info_file_for(c);

				(void)g_file_delete(rec, NULL, NULL);
				g_object_unref(rec);
			}
			g_object_unref(c);
			g_object_unref(ci);
		}
		g_object_unref(en);
	}
	/* Records whose item is already gone. */
	en = g_file_enumerate_children(info, G_FILE_ATTRIBUTE_STANDARD_NAME,
				       G_FILE_QUERY_INFO_NONE, op->cancel, NULL);
	if (en) {
		while (!cancelled(op) &&
		       (ci = g_file_enumerator_next_file(en, op->cancel, NULL))) {
			GFile *c = g_file_enumerator_get_child(en, ci);

			(void)g_file_delete(c, NULL, NULL);
			g_object_unref(c);
			g_object_unref(ci);
		}
		g_object_unref(en);
	}
	g_object_unref(info);
	g_object_unref(files);
}

static void do_chmod(ExOperation *op)
{
	Item *it = g_ptr_array_index(op->items, 0);

	op->items_total = 1;
	set_current(op, it->src);
	for (;;) {
		GError *e = NULL;

		if (g_file_set_attribute_uint32(it->src, G_FILE_ATTRIBUTE_UNIX_MODE,
						op->mode,
						G_FILE_QUERY_INFO_NONE,
						op->cancel, &e)) {
			add_result(op, it->src, it->src);
			item_done(op, 0);
			return;
		}
		ExIssueChoice c = ask_issue(op, it->src, "change permissions of",
					    e, TRUE, FALSE);
		g_error_free(e);
		if (c != EX_ISSUE_RETRY)
			return;
	}
}

static void worker(GTask *task, gpointer src, gpointer data,
		   GCancellable *cancel)
{
	ExOperation *op = src;

	switch (op->kind) {
	case EX_OP_COPY:
		do_copy(op);
		break;
	case EX_OP_MOVE:
		do_move(op);
		break;
	case EX_OP_LINK:
		do_link(op);
		break;
	case EX_OP_TRASH:
		do_trash(op);
		break;
	case EX_OP_DELETE:
		do_delete(op);
		break;
	case EX_OP_RENAME:
		do_rename(op);
		break;
	case EX_OP_MKDIR:
	case EX_OP_NEWFILE:
		do_create(op);
		break;
	case EX_OP_RESTORE:
		do_restore(op);
		break;
	case EX_OP_EMPTY_TRASH:
		do_empty_trash(op);
		break;
	case EX_OP_CHMOD:
		do_chmod(op);
		break;
	}
	g_mutex_lock(&op->lock);
	if (cancelled(op))
		op->state = EX_OP_CANCELLED;
	else if (op->errors->len)
		op->state = EX_OP_FAILED;
	else
		op->state = EX_OP_DONE;
	g_free(op->current);
	op->current = NULL;
	g_mutex_unlock(&op->lock);
	g_task_return_boolean(task, TRUE);
}

static void finished(GObject *src, GAsyncResult *res, gpointer data)
{
	ExOperation *op = EX_OPERATION(src);

	g_signal_emit(op, sigs[SIG_PROGRESS], 0);
	g_signal_emit(op, sigs[SIG_FINISHED], 0);
}

void ex_operation_start(ExOperation *op)
{
	g_return_if_fail(op->state == EX_OP_QUEUED);
	GTask *task = g_task_new(op, NULL, finished, NULL);

	op->state = EX_OP_RUNNING;
	g_task_run_in_thread(task, worker);
	g_object_unref(task);
}

void ex_operation_cancel(ExOperation *op)
{
	g_cancellable_cancel(op->cancel);
	g_mutex_lock(&op->lock);
	g_cond_broadcast(&op->cond);
	g_mutex_unlock(&op->lock);
	if (op->state == EX_OP_QUEUED) {
		op->state = EX_OP_CANCELLED;
		g_signal_emit(op, sigs[SIG_FINISHED], 0);
	}
}

/* ---- accessors ----------------------------------------------------------- */

ExOpKind ex_operation_get_kind(ExOperation *op)
{
	return op->kind;
}

ExOpState ex_operation_get_state(ExOperation *op)
{
	ExOpState s;

	g_mutex_lock(&op->lock);
	s = op->state;
	g_mutex_unlock(&op->lock);
	return s;
}

gboolean ex_operation_is_waiting(ExOperation *op)
{
	gboolean w;

	g_mutex_lock(&op->lock);
	w = op->waiting;
	g_mutex_unlock(&op->lock);
	return w;
}

const char *ex_operation_get_title(ExOperation *op)
{
	return op->title;
}

GFile *ex_operation_get_dest_dir(ExOperation *op)
{
	return op->dest_dir;
}

GPtrArray *ex_operation_get_sources(ExOperation *op)
{
	return op->sources;
}

char *ex_operation_get_current(ExOperation *op)
{
	char *s;

	g_mutex_lock(&op->lock);
	s = g_strdup(op->current);
	g_mutex_unlock(&op->lock);
	return s;
}

void ex_operation_get_counts(ExOperation *op, guint64 *bytes_done,
			     guint64 *bytes_total, guint *items_done,
			     guint *items_total)
{
	g_mutex_lock(&op->lock);
	if (bytes_done)
		*bytes_done = op->bytes_done;
	if (bytes_total)
		*bytes_total = op->bytes_total;
	if (items_done)
		*items_done = op->items_done;
	if (items_total)
		*items_total = op->items_total;
	g_mutex_unlock(&op->lock);
}

double ex_operation_get_fraction(ExOperation *op)
{
	double f;

	g_mutex_lock(&op->lock);
	if (op->state == EX_OP_DONE || op->state == EX_OP_FAILED)
		f = 1.0;
	else if (op->counting)
		f = -1.0;
	else if (op->bytes_total > 0)
		f = (double)op->bytes_done / (double)op->bytes_total;
	else if (op->items_total > 0)
		f = (double)op->items_done / (double)op->items_total;
	else
		f = 0.0;
	g_mutex_unlock(&op->lock);
	return f > 1.0 ? 1.0 : f;
}

double ex_operation_get_rate(ExOperation *op)
{
	double r;

	g_mutex_lock(&op->lock);
	r = op->rate;
	g_mutex_unlock(&op->lock);
	return r;
}

GPtrArray *ex_operation_get_results(ExOperation *op)
{
	return op->results;
}

GPtrArray *ex_operation_get_errors(ExOperation *op)
{
	return op->errors;
}
