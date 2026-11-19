// LikeOS Explorer: one operation in the progress window.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-progress-item.h"
#include "ui/ex-window.h"
#include "ui/widgets/lx-speed-graph.h"
#include "backend/ex-location.h"
#include "backend/ex-trash.h"

#include <math.h>

/* The item rate (for operations that move no bytes, a delete) is measured
 * over windows of at least this long, and smoothed like the byte rate. */
#define ITEM_RATE_US (500 * 1000)
#define ITEM_RATE_KEEP 0.6

typedef struct {
	ExOperation *op;
	gulong progress_id;
	gboolean more, finished;
	GtkWidget *header, *status, *pause, *pause_img, *cancel;
	GtkWidget *graph, *details, *name_v, *time_v, *items_v;
	GtkWidget *bar;
	GtkWidget *errors;
	/* Items per second, for operations without bytes. */
	gint64 irate_t0;
	guint irate_n0;
	double irate;
	/* The item count the header line was written for. */
	guint header_count;
} Item;

static Item *item_of(GtkWidget *w)
{
	return g_object_get_data(G_OBJECT(w), "ex-progress-item");
}

/* ---- words --------------------------------------------------------------- */

/* "257 MB", "45.2 MB", "3.92 GB": three significant digits, in the
 * decimal units the rest of the system uses. */
static char *size3(guint64 bytes)
{
	static const char *const units[] = { "KB", "MB", "GB", "TB", "PB" };
	double v = (double)bytes;
	int u = -1;

	if (bytes < 1000)
		return g_strdup_printf("%u byte%s", (unsigned)bytes,
				       bytes == 1 ? "" : "s");
	while (v >= 999.5 && u < 4) {
		v /= 1000.0;
		u++;
	}
	if (v >= 99.95)
		return g_strdup_printf("%.0f %s", v, units[u]);
	if (v >= 9.995)
		return g_strdup_printf("%.1f %s", v, units[u]);
	return g_strdup_printf("%.2f %s", v, units[u]);
}

static char *time_left(double seconds)
{
	unsigned s = (unsigned)ceil(seconds), n;

	if (s < 10)
		return g_strdup("About 5 seconds");
	if (s < 60) {
		n = (s + 4) / 5 * 5;
		return g_strdup_printf("About %u seconds", n);
	}
	if (s < 90 * 60) {
		n = (s + 30) / 60;
		return n == 1 ? g_strdup("About 1 minute") :
				g_strdup_printf("About %u minutes", n);
	}
	n = (s + 1800) / 3600;
	return n == 1 ? g_strdup("About 1 hour") :
			g_strdup_printf("About %u hours", n);
}

/* How many items: the counted total once known, the selection before. */
static guint item_count(ExOperation *op)
{
	guint total = 0;

	ex_operation_get_counts(op, NULL, NULL, NULL, &total);
	if (ex_operation_get_fraction(op) < 0 || total == 0)
		total = ex_operation_get_sources(op)->len;
	return total;
}

char *ex_progress_status_text(ExOperation *op)
{
	double f = ex_operation_get_fraction(op);
	int pct = f < 0 ? 0 : (int)floor(f * 100.0 + 1e-9);

	switch (ex_operation_get_state(op)) {
	case EX_OP_QUEUED:
		return g_strdup(ex_operation_is_paused(op) ? "Paused" : "Waiting");
	case EX_OP_RUNNING:
		if (f < 0)
			return g_strdup("Calculating…");
		if (ex_operation_is_paused(op))
			return g_strdup_printf("Paused – %d%% complete", pct);
		return g_strdup_printf("%d%% complete", pct);
	case EX_OP_DONE:
		return g_strdup("Done");
	case EX_OP_CANCELLED:
		return g_strdup("Cancelled");
	case EX_OP_FAILED: {
		guint n = ex_operation_get_errors(op)->len;

		return n == 1 ? g_strdup("1 item could not be done") :
				g_strdup_printf("%u items could not be done", n);
	}
	}
	return g_strdup("");
}

/* ---- the header line, with its links --------------------------------------- */

static gboolean is_trash_files(GFile *dir)
{
	GFile *tf = ex_trash_files_dir();
	gboolean is = dir && g_file_equal(dir, tf);

	g_object_unref(tf);
	return is;
}

/* The location a folder of the operation opens as: the Trash's own folder
 * opens as the Trash. */
static ExLocation *place_of(GFile *dir)
{
	if (!dir)
		return NULL;
	if (is_trash_files(dir))
		return ex_location_new_special(EX_LOC_TRASH);
	return ex_location_new_folder(dir);
}

static char *link_markup(const char *key, ExLocation *loc)
{
	char *name = ex_location_display_name(loc);
	/* Not underlined: the accent colour marks the link, and a label's
	 * links are underlined whatever the theme says. */
	char *m = g_markup_printf_escaped(
		"<a href=\"%s\"><span underline=\"none\">%s</span></a>", key,
		name);

	g_free(name);
	return m;
}

static void set_place(GtkWidget *label, const char *key, ExLocation *loc)
{
	g_object_set_data_full(G_OBJECT(label), key, loc,
			       loc ? (GDestroyNotify)ex_location_unref : NULL);
}

void ex_progress_header_update(GtkWidget *label, ExOperation *op)
{
	ExOpKind kind = ex_operation_get_kind(op);
	GFile *src = ex_operation_get_source_dir(op);
	GFile *dst = ex_operation_get_dest_dir(op);
	ExLocation *from = place_of(src), *to = NULL;
	guint n = item_count(op);
	char *what = n == 1 ? g_strdup("1 item") : g_strdup_printf("%u items", n);
	char *from_m = NULL, *to_m = NULL, *m = NULL;
	const char *verb = NULL;

	switch (kind) {
	case EX_OP_COPY:
		verb = "Copying";
		to = place_of(dst);
		break;
	case EX_OP_MOVE:
		verb = "Moving";
		to = place_of(dst);
		break;
	case EX_OP_TRASH:
		verb = "Moving";
		to = ex_location_new_special(EX_LOC_TRASH);
		break;
	case EX_OP_DELETE:
		verb = "Deleting";
		break;
	case EX_OP_RESTORE:
		verb = "Restoring";
		break;
	case EX_OP_EMPTY_TRASH:
		verb = "Deleting";
		g_clear_pointer(&from, ex_location_unref);
		from = ex_location_new_special(EX_LOC_TRASH);
		break;
	default:
		break;
	}
	if (from)
		from_m = link_markup("from", from);
	if (to)
		to_m = link_markup("to", to);
	if (verb && kind == EX_OP_TRASH && to_m)
		m = from_m ? g_strdup_printf("%s %s from %s to the %s", verb, what,
					     from_m, to_m) :
			     g_strdup_printf("%s %s to the %s", verb, what, to_m);
	else if (verb && to_m)
		m = from_m ? g_strdup_printf("%s %s from %s to %s", verb, what,
					     from_m, to_m) :
			     g_strdup_printf("%s %s to %s", verb, what, to_m);
	else if (verb && kind != EX_OP_COPY && kind != EX_OP_MOVE)
		m = from_m ? g_strdup_printf("%s %s from %s", verb, what, from_m) :
			     g_strdup_printf("%s %s", verb, what);
	if (!m) /* rename, new folder, an undo's move back: its own title */
		m = g_markup_escape_text(ex_operation_get_title(op), -1);
	/* Only when it changed: setting the same markup again would drop the
	 * pointer's hover on a link ten times a second. */
	if (g_strcmp0(m, g_object_get_data(G_OBJECT(label), "ex-markup"))) {
		gtk_label_set_markup(GTK_LABEL(label), m);
		g_object_set_data_full(G_OBJECT(label), "ex-markup", m, g_free);
		m = NULL;
	}
	set_place(label, "from", from);
	set_place(label, "to", to);
	gtk_widget_set_tooltip_text(label, ex_operation_get_title(op));
	g_free(m);
	g_free(from_m);
	g_free(to_m);
	g_free(what);
}

static gboolean open_link(GtkLabel *label, const char *uri, gpointer data)
{
	ExLocation *loc = g_object_get_data(G_OBJECT(label), uri);
	GApplication *app = g_application_get_default();

	if (loc && app && GTK_IS_APPLICATION(app)) {
		ExWindow *w = ex_window_new(GTK_APPLICATION(app), loc);

		gtk_window_present(GTK_WINDOW(w));
	}
	return TRUE;
}

GtkWidget *ex_progress_header_new(ExOperation *op)
{
	GtkWidget *l = gtk_label_new(NULL);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	/* As wide as the window makes it, never wider. */
	gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
	gtk_label_set_track_visited_links(GTK_LABEL(l), FALSE);
	gtk_style_context_add_class(gtk_widget_get_style_context(l),
				    "ex-op-header");
	g_signal_connect(l, "activate-link", G_CALLBACK(open_link), NULL);
	ex_progress_header_update(l, op);
	return l;
}

/* ---- the panel ------------------------------------------------------------- */

static void add_class(GtkWidget *w, const char *klass)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), klass);
}

/* The theme's icon, or a standard one while the theme has none. */
static const char *icon_name(const char *want, const char *fallback)
{
	return gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), want) ?
		       want :
		       fallback;
}

/* Pause and Cancel: round icon buttons in the card's top right corner. */
static GtkWidget *action_button(const char *icon, const char *fallback,
				const char *tip, GtkWidget **img)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *i = gtk_image_new_from_icon_name(icon_name(icon, fallback),
						    GTK_ICON_SIZE_MENU);

	gtk_container_add(GTK_CONTAINER(b), i);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(b, tip);
	atk_object_set_name(gtk_widget_get_accessible(b), tip);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	add_class(b, "ex-op-action");
	if (img)
		*img = i;
	return b;
}

/* One of the facts under the sparkline: a small caption over its value,
 * the three side by side. */
static GtkWidget *detail_line(GtkWidget *box, const char *name)
{
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
	GtkWidget *k = gtk_label_new(name);
	GtkWidget *v = gtk_label_new("");

	gtk_label_set_xalign(GTK_LABEL(k), 0);
	gtk_label_set_xalign(GTK_LABEL(v), 0);
	gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 1);
	add_class(k, "ex-op-caption");
	add_class(v, "ex-op-value");
	gtk_box_pack_start(GTK_BOX(col), k, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), v, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), col, TRUE, TRUE, 0);
	return v;
}

static void set_text(GtkWidget *l, const char *s)
{
	if (g_strcmp0(gtk_label_get_text(GTK_LABEL(l)), s))
		gtk_label_set_text(GTK_LABEL(l), s);
}

/* Items per second, for an operation that moves no bytes. */
static double item_rate(Item *it, guint done)
{
	gint64 now = g_get_monotonic_time();

	if (!it->irate_t0 || done < it->irate_n0) {
		it->irate_t0 = now;
		it->irate_n0 = done;
	} else if (now - it->irate_t0 >= ITEM_RATE_US) {
		double r = (double)(done - it->irate_n0) * G_USEC_PER_SEC /
			   (double)(now - it->irate_t0);

		it->irate = it->irate > 0 ? ITEM_RATE_KEEP * it->irate +
						    (1.0 - ITEM_RATE_KEEP) * r :
					    r;
		it->irate_t0 = now;
		it->irate_n0 = done;
	}
	return it->irate;
}

static void show_parts(Item *it)
{
	ExOpState st = ex_operation_get_state(it->op);
	gboolean active = st == EX_OP_RUNNING || st == EX_OP_QUEUED;

	gtk_widget_set_visible(it->pause, active);
	gtk_widget_set_visible(it->cancel, active);
	gtk_widget_set_visible(it->bar, active);
	gtk_widget_set_visible(it->graph, it->more && active);
	gtk_widget_set_visible(it->details, it->more && active);
	gtk_widget_set_visible(it->errors, !active &&
					   ex_operation_get_errors(it->op)->len);
}

static void refresh(Item *it)
{
	ExOperation *op = it->op;
	ExOpState st = ex_operation_get_state(op);
	gboolean paused = ex_operation_is_paused(op);
	gboolean waiting = ex_operation_is_waiting(op);
	double f = ex_operation_get_fraction(op);
	guint64 bd, bt;
	guint id, itot;
	char *s, *cur;

	ex_operation_get_counts(op, &bd, &bt, &id, &itot);
	/* The line changes only with the count (once counting is done). */
	if (item_count(op) != it->header_count) {
		it->header_count = item_count(op);
		ex_progress_header_update(it->header, op);
	}
	s = ex_progress_status_text(op);
	set_text(it->status, s);
	g_free(s);
	if (st != EX_OP_RUNNING && st != EX_OP_QUEUED)
		return;

	/* Pause / Resume. */
	gtk_image_set_from_icon_name(GTK_IMAGE(it->pause_img),
				     paused ? icon_name("likeos-play-symbolic",
							"media-playback-start-symbolic") :
					      icon_name("likeos-pause-symbolic",
							"media-playback-pause-symbolic"),
				     GTK_ICON_SIZE_MENU);
	gtk_widget_set_tooltip_text(it->pause, paused ? "Resume" : "Pause");
	atk_object_set_name(gtk_widget_get_accessible(it->pause),
			    paused ? "Resume" : "Pause");

	/* The progress bar. */
	if (f < 0 && st == EX_OP_RUNNING)
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(it->bar));
	else
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(it->bar),
					      MAX(f, 0.0));
	if (paused)
		add_class(it->bar, "paused");
	else
		gtk_style_context_remove_class(
			gtk_widget_get_style_context(it->bar), "paused");

	if (st == EX_OP_QUEUED) {
		lx_speed_graph_set_current(LX_SPEED_GRAPH(it->graph), 0,
					   paused ? "Paused" : "Waiting to start");
		set_text(it->name_v, "");
		set_text(it->time_v, paused ? "Paused" : "In the queue");
		set_text(it->items_v, "");
		return;
	}

	/* The graph, and how fast it goes now. */
	gboolean bytes = bt > 0;
	double rate = bytes ? ex_operation_get_rate(op) : item_rate(it, id);
	LxSpeedGraph *g = LX_SPEED_GRAPH(it->graph);

	if (f >= 0) {
		lx_speed_graph_set_fraction(g, f);
		if (!paused && !waiting && rate > 0)
			lx_speed_graph_add_sample(g, f, rate);
	}
	if (paused) {
		lx_speed_graph_set_current(g, 0, "Paused");
	} else if (rate > 0) {
		char *sp = bytes ? size3((guint64)rate) : NULL;
		char *label = bytes ? g_strdup_printf("Speed: %s/s", sp) :
				      g_strdup_printf("Speed: %.0f items/s", rate);

		lx_speed_graph_set_current(g, rate, label);
		g_free(label);
		g_free(sp);
	} else {
		/* Until the first speed is known -- a moment after the start,
		 * and while the files are counted -- the graph says so rather
		 * than standing empty. */
		lx_speed_graph_set_current(g, 0, waiting ? "Waiting" :
							   "Measuring speed…");
	}

	/* Name, time and items remaining. */
	cur = ex_operation_get_current(op);
	set_text(it->name_v, cur ? cur : "");
	g_free(cur);
	if (waiting) {
		set_text(it->time_v, "Waiting for your answer");
	} else if (paused) {
		set_text(it->time_v, "Paused");
	} else if (f < 0 || rate <= 0) {
		set_text(it->time_v, "Calculating…");
	} else {
		double left = bytes ? (double)(bt > bd ? bt - bd : 0) / rate :
				      (double)(itot > id ? itot - id : 0) / rate;

		s = time_left(left);
		set_text(it->time_v, s);
		g_free(s);
	}
	if (f < 0) {
		set_text(it->items_v, "Calculating…");
	} else {
		guint left = itot > id ? itot - id : 0;

		if (bytes) {
			char *sz = size3(bt > bd ? bt - bd : 0);

			s = g_strdup_printf("%u (%s)", left, sz);
			g_free(sz);
		} else {
			s = g_strdup_printf("%u", left);
		}
		set_text(it->items_v, s);
		g_free(s);
	}
}

static void progress(ExOperation *op, GtkWidget *w)
{
	Item *it = item_of(w);

	if (!it->finished)
		refresh(it);
	show_parts(it);
}

static void pause_clicked(GtkButton *b, Item *it)
{
	ex_operation_set_paused(it->op, !ex_operation_is_paused(it->op));
}

static void cancel_clicked(GtkButton *b, Item *it)
{
	/* A paused operation is woken by the cancel itself. */
	ex_operation_cancel(it->op);
}

static void item_free(gpointer data)
{
	Item *it = data;

	g_object_unref(it->op);
	g_free(it);
}

static void item_destroyed(GtkWidget *w, gpointer data)
{
	Item *it = item_of(w);

	if (it && it->progress_id) {
		g_signal_handler_disconnect(it->op, it->progress_id);
		it->progress_id = 0;
	}
}

/* The badge at the card's left: what kind of operation it is. */
static GtkWidget *kind_badge(ExOperation *op)
{
	const char *icon = "likeos-copy-symbolic", *fallback = "edit-copy-symbolic";
	GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *img;

	switch (ex_operation_get_kind(op)) {
	case EX_OP_MOVE:
		icon = "likeos-move-to-symbolic";
		fallback = "edit-cut-symbolic";
		break;
	case EX_OP_TRASH:
	case EX_OP_DELETE:
	case EX_OP_EMPTY_TRASH:
		icon = "likeos-delete-symbolic";
		fallback = "user-trash-symbolic";
		break;
	case EX_OP_RESTORE:
		icon = "likeos-restore-symbolic";
		fallback = "edit-undo-symbolic";
		break;
	default:
		break;
	}
	img = gtk_image_new_from_icon_name(icon_name(icon, fallback),
					   GTK_ICON_SIZE_MENU);
	gtk_box_pack_start(GTK_BOX(b), img, TRUE, TRUE, 0);
	gtk_widget_set_size_request(b, 32, 32);
	gtk_widget_set_valign(b, GTK_ALIGN_START);
	add_class(b, "ex-op-badge");
	return b;
}

GtkWidget *ex_progress_item_new(ExOperation *op)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
	GtkWidget *facts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	Item *it = g_new0(Item, 1);

	it->op = g_object_ref(op);
	it->more = TRUE;
	g_object_set_data_full(G_OBJECT(box), "ex-progress-item", it, item_free);
	add_class(box, "ex-op-item");

	/* The badge; the line and the status under it; Pause and Cancel. */
	it->header = ex_progress_header_new(op);
	it->header_count = item_count(op);
	it->status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(it->status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(it->status), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(it->status), 1);
	add_class(it->status, "ex-op-status");
	gtk_box_pack_start(GTK_BOX(lines), it->header, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(lines), it->status, FALSE, FALSE, 0);
	gtk_widget_set_valign(lines, GTK_ALIGN_CENTER);
	it->pause = action_button("likeos-pause-symbolic",
				  "media-playback-pause-symbolic", "Pause",
				  &it->pause_img);
	it->cancel = action_button("likeos-close-symbolic",
				   "window-close-symbolic", "Cancel", NULL);
	add_class(it->cancel, "ex-op-cancel");
	g_signal_connect(it->pause, "clicked", G_CALLBACK(pause_clicked), it);
	g_signal_connect(it->cancel, "clicked", G_CALLBACK(cancel_clicked), it);
	gtk_box_pack_start(GTK_BOX(top), kind_badge(op), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(top), lines, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(top), it->pause, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(top), it->cancel, FALSE, FALSE, 0);
	add_class(top, "ex-op-top");
	gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

	it->bar = gtk_progress_bar_new();
	add_class(it->bar, "ex-op-bar");
	gtk_box_pack_start(GTK_BOX(box), it->bar, FALSE, FALSE, 0);

	it->graph = lx_speed_graph_new();
	add_class(it->graph, "ex-op-graph");
	gtk_widget_set_margin_top(it->graph, 12);
	gtk_box_pack_start(GTK_BOX(box), it->graph, FALSE, FALSE, 0);

	it->details = facts;
	add_class(facts, "ex-op-details");
	gtk_box_set_homogeneous(GTK_BOX(facts), TRUE);
	it->name_v = detail_line(facts, "File");
	it->time_v = detail_line(facts, "Time left");
	it->items_v = detail_line(facts, "Remaining");
	gtk_box_pack_start(GTK_BOX(box), facts, FALSE, FALSE, 0);

	it->errors = gtk_expander_new("Show what could not be done");
	add_class(it->errors, "ex-op-errors");
	gtk_box_pack_start(GTK_BOX(box), it->errors, FALSE, FALSE, 0);

	gtk_widget_show_all(box);
	it->progress_id = g_signal_connect(op, "progress", G_CALLBACK(progress),
					   box);
	g_signal_connect(box, "destroy", G_CALLBACK(item_destroyed), NULL);
	refresh(it);
	show_parts(it);
	return box;
}

ExOperation *ex_progress_item_get_operation(GtkWidget *item)
{
	return item_of(item)->op;
}

void ex_progress_item_set_details(GtkWidget *item, gboolean more)
{
	Item *it = item_of(item);

	it->more = more;
	show_parts(it);
}

void ex_progress_item_finish(GtkWidget *item)
{
	Item *it = item_of(item);
	GPtrArray *errs = ex_operation_get_errors(it->op);

	if (it->finished)
		return;
	refresh(it);
	it->finished = TRUE;
	add_class(item, "finished");
	if (errs->len) {
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

		for (guint i = 0; i < errs->len; i++) {
			GtkWidget *l = gtk_label_new(g_ptr_array_index(errs, i));

			gtk_label_set_xalign(GTK_LABEL(l), 0);
			gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
			gtk_label_set_line_wrap_mode(GTK_LABEL(l),
						     PANGO_WRAP_WORD_CHAR);
			gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
			gtk_label_set_selectable(GTK_LABEL(l), TRUE);
			gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
		}
		gtk_container_add(GTK_CONTAINER(it->errors), box);
		gtk_widget_show_all(box);
	}
	show_parts(it);
}
