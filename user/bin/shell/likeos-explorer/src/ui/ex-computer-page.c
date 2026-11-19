// LikeOS Explorer: Computer.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-computer-page.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-util.h"
#include "backend/ex-volumes.h"

/* How full a drive is before its bar turns red. */
#define EX_NEARLY_FULL 0.9
/* The usage bar, in pixels. */
#define EX_BAR_WIDTH 180
#define EX_BAR_HEIGHT 6

struct _ExComputerPage {
	GtkScrolledWindow parent;
	GtkWidget *box;
	guint reload_id;
};

G_DEFINE_TYPE(ExComputerPage, ex_computer_page, GTK_TYPE_SCROLLED_WINDOW)

enum { SIG_OPEN, SIG_POPUP_LOCATION, N_SIGS };
static guint sigs[N_SIGS];

/* Which groups are folded away, for the life of the program: a reload (a
 * drive came or went) keeps them as they were. */
static gboolean g_folded[2];

static void destroy_child(GtkWidget *w, gpointer data)
{
	gtk_widget_destroy(w);
}

/* An icon of the LikeOS theme, with a standard one to stand in where the
 * theme has none. */
static GtkWidget *themed_image(const char *name, const char *fallback,
			       int size)
{
	const char *names[] = { name, fallback, NULL };
	GIcon *gi = g_themed_icon_new_from_names((char **)names, -1);
	GtkWidget *img = gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_BUTTON);

	gtk_image_set_pixel_size(GTK_IMAGE(img), size);
	g_object_unref(gi);
	return img;
}

/* ---- group headings ------------------------------------------------------ */

static void fold_toggled(GtkButton *b, GtkWidget *revealer)
{
	gboolean open = !gtk_revealer_get_reveal_child(GTK_REVEALER(revealer));
	GtkWidget *chev = g_object_get_data(G_OBJECT(b), "ex-chevron");
	int *folded = g_object_get_data(G_OBJECT(b), "ex-folded");

	gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), open);
	gtk_widget_destroy(chev);
	chev = open ? themed_image("likeos-chevron-down", "pan-down-symbolic", 12) :
		      themed_image("likeos-chevron-right", "pan-end-symbolic", 12);
	ex_style_add(chev, "ex-fold-chevron");
	gtk_box_pack_end(GTK_BOX(gtk_bin_get_child(GTK_BIN(b))), chev, FALSE,
			 FALSE, 0);
	gtk_widget_show(chev);
	g_object_set_data(G_OBJECT(b), "ex-chevron", chev);
	*folded = !open;
	atk_object_set_description(gtk_widget_get_accessible(GTK_WIDGET(b)),
				   open ? "Expanded" : "Collapsed");
}

/* A heading with its chevron after it, and the group under it in a
 * revealer that the heading folds; returns the box the group's content goes
 * into. */
static GtkWidget *group(ExComputerPage *p, const char *title, int *folded)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *chev = *folded ?
				  themed_image("likeos-chevron-right",
					       "pan-end-symbolic", 12) :
				  themed_image("likeos-chevron-down",
					       "pan-down-symbolic", 12);
	GtkWidget *l = gtk_label_new(title);
	GtkWidget *rev = gtk_revealer_new();
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
	ex_style_add(chev, "ex-fold-chevron");
	gtk_box_pack_end(GTK_BOX(row), chev, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_halign(b, GTK_ALIGN_START);
	ex_style_add(b, "ex-group-header");
	g_object_set_data(G_OBJECT(b), "ex-chevron", chev);
	g_object_set_data(G_OBJECT(b), "ex-folded", folded);
	atk_object_set_description(gtk_widget_get_accessible(b),
				   *folded ? "Collapsed" : "Expanded");
	gtk_revealer_set_transition_type(GTK_REVEALER(rev),
					 GTK_REVEALER_TRANSITION_TYPE_NONE);
	gtk_revealer_set_reveal_child(GTK_REVEALER(rev), !*folded);
	gtk_container_add(GTK_CONTAINER(rev), content);
	g_signal_connect(b, "clicked", G_CALLBACK(fold_toggled), rev);
	gtk_box_pack_start(GTK_BOX(p->box), b, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p->box), rev, FALSE, FALSE, 0);
	return content;
}

static GtkWidget *flow(void)
{
	GtkWidget *f = gtk_flow_box_new();

	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(f), GTK_SELECTION_NONE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(f), TRUE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(f), 1);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(f), 8);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(f), 8);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(f), 8);
	gtk_widget_set_halign(f, GTK_ALIGN_START);
	gtk_widget_set_valign(f, GTK_ALIGN_START);
	return f;
}

/* ---- drive cards ---------------------------------------------------------- */

static void tile_clicked(GtkButton *b, ExComputerPage *p)
{
	g_signal_emit(p, sigs[SIG_OPEN], 0,
		      g_object_get_data(G_OBJECT(b), "ex-location"), FALSE);
}

static gboolean tile_button(GtkWidget *w, GdkEventButton *ev, ExComputerPage *p)
{
	ExLocation *l = g_object_get_data(G_OBJECT(w), "ex-location");

	if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
		g_signal_emit(p, sigs[SIG_OPEN], 0, l, TRUE);
		return TRUE;
	}
	if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
		gtk_widget_grab_focus(w);
		g_signal_emit(p, sigs[SIG_POPUP_LOCATION], 0, l, ev);
		return TRUE;
	}
	return FALSE;
}

/* The menu key (or Shift+F10) on a focused card. */
static gboolean tile_menu_key(GtkWidget *w, ExComputerPage *p)
{
	g_signal_emit(p, sigs[SIG_POPUP_LOCATION], 0,
		      g_object_get_data(G_OBJECT(w), "ex-location"), NULL);
	return TRUE;
}

static GtkWidget *drive_tile(ExComputerPage *p, const ExVolume *v)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *name = gtk_label_new(v->name);
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(p));
	GdkPixbuf *pb = ex_icons_named(v->icon_name, 48, scale);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;
	GtkWidget *img = gtk_image_new_from_surface(s);
	ExLocation *loc = ex_location_new_folder(v->root);
	char *tip;

	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
	gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(name), 24);
	gtk_box_pack_start(GTK_BOX(text), name, FALSE, FALSE, 0);
	if (v->size) {
		double used = 1.0 - (double)v->free / (double)v->size;
		GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);
		char *fr = ex_format_size(v->free);
		char *sz = ex_format_size(v->size);
		char *sub = g_strdup_printf("%s free of %s", fr, sz);
		GtkWidget *sl = gtk_label_new(sub);

		/* The theme colours the bar; its own low/high/full steps
		 * would colour it by other rules. */
		gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar),
						  GTK_LEVEL_BAR_OFFSET_LOW);
		gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar),
						  GTK_LEVEL_BAR_OFFSET_HIGH);
		gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar),
						  GTK_LEVEL_BAR_OFFSET_FULL);
		gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), CLAMP(used, 0, 1));
		gtk_widget_set_size_request(bar, EX_BAR_WIDTH, EX_BAR_HEIGHT);
		gtk_widget_set_halign(bar, GTK_ALIGN_START);
		ex_style_add(bar, "ex-usage");
		if (used > EX_NEARLY_FULL)
			ex_style_add(bar, "ex-nearly-full");
		gtk_box_pack_start(GTK_BOX(text), bar, FALSE, FALSE, 0);
		gtk_label_set_xalign(GTK_LABEL(sl), 0);
		gtk_label_set_ellipsize(GTK_LABEL(sl), PANGO_ELLIPSIZE_END);
		ex_style_add(sl, EX_CLASS_DIM);
		gtk_box_pack_start(GTK_BOX(text), sl, FALSE, FALSE, 0);
		tip = g_strdup_printf("%s\n%s", v->name, sub);
		g_free(sub);
		g_free(fr);
		g_free(sz);
	} else {
		tip = g_strdup(v->name);
	}
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	ex_style_add(b, "ex-drive-tile");
	gtk_widget_set_tooltip_text(b, tip);
	atk_object_set_name(gtk_widget_get_accessible(b), tip);
	g_free(tip);
	g_object_set_data_full(G_OBJECT(b), "ex-location", loc,
			       (GDestroyNotify)ex_location_unref);
	g_signal_connect(b, "clicked", G_CALLBACK(tile_clicked), p);
	g_signal_connect(b, "button-press-event", G_CALLBACK(tile_button), p);
	g_signal_connect(b, "popup-menu", G_CALLBACK(tile_menu_key), p);
	return b;
}

/* ---- the page ------------------------------------------------------------- */

void ex_computer_page_reload(ExComputerPage *p)
{
	GPtrArray *vl = ex_volumes_list(ex_volumes_get_default());
	GtkWidget *drives, *net = NULL;

	gtk_container_foreach(GTK_CONTAINER(p->box), destroy_child, NULL);
	drives = flow();
	gtk_container_add(GTK_CONTAINER(group(p, "Drives", &g_folded[0])),
			  drives);
	for (guint i = 0; vl && i < vl->len; i++) {
		const ExVolume *v = g_ptr_array_index(vl, i);

		if (v->network) {
			if (!net) {
				net = flow();
				gtk_container_add(
					GTK_CONTAINER(group(p, "Network shares",
							    &g_folded[1])),
					net);
			}
			gtk_container_add(GTK_CONTAINER(net), drive_tile(p, v));
		} else {
			gtk_container_add(GTK_CONTAINER(drives), drive_tile(p, v));
		}
	}
	gtk_widget_show_all(p->box);
}

static gboolean reload_idle(gpointer data)
{
	ExComputerPage *p = data;

	p->reload_id = 0;
	ex_computer_page_reload(p);
	return G_SOURCE_REMOVE;
}

static void volumes_changed(gpointer src, ExComputerPage *p)
{
	if (!p->reload_id)
		p->reload_id = g_timeout_add(300, reload_idle, p);
}

static void ex_computer_page_dispose(GObject *o)
{
	ExComputerPage *p = EX_COMPUTER_PAGE(o);

	if (p->reload_id) {
		g_source_remove(p->reload_id);
		p->reload_id = 0;
	}
	g_signal_handlers_disconnect_by_data(ex_volumes_get_default(), p);
	G_OBJECT_CLASS(ex_computer_page_parent_class)->dispose(o);
}

static void ex_computer_page_class_init(ExComputerPageClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->dispose = ex_computer_page_dispose;
	sigs[SIG_OPEN] = g_signal_new("open", t, G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				      NULL, G_TYPE_NONE, 2, EX_TYPE_LOCATION,
				      G_TYPE_BOOLEAN);
	sigs[SIG_POPUP_LOCATION] = g_signal_new("popup-location", t,
						G_SIGNAL_RUN_LAST, 0, NULL, NULL,
						NULL, G_TYPE_NONE, 2,
						EX_TYPE_LOCATION, G_TYPE_POINTER);
}

static void ex_computer_page_init(ExComputerPage *p)
{
	GtkWidget *clamp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	ex_style_add(p->box, "ex-page-body");
	gtk_box_pack_start(GTK_BOX(clamp), p->box, FALSE, FALSE, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(p), clamp);
	ex_style_add(GTK_WIDGET(p), EX_CLASS_HOME);
	ex_style_add(GTK_WIDGET(p), "ex-computer");
}

GtkWidget *ex_computer_page_new(void)
{
	ExComputerPage *p = g_object_new(EX_TYPE_COMPUTER_PAGE, NULL);

	g_signal_connect(ex_volumes_get_default(), "changed",
			 G_CALLBACK(volumes_changed), p);
	ex_computer_page_reload(p);
	return GTK_WIDGET(p);
}
