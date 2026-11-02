// LikeOS -- breadcrumb address bar.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/widgets/lx-breadcrumb.h"

static void destroy_child(GtkWidget *w, gpointer data)
{
	gtk_widget_destroy(w);
}

/* The LikeOS line icons where the theme has them, GTK's own otherwise. */
static void set_chevron(GtkImage *img, gboolean open)
{
	const char *names[] = { open ? "likeos-chevron-down-symbolic" :
				       "likeos-chevron-right-symbolic",
				open ? "go-down-symbolic" : "go-next-symbolic",
				NULL };
	GIcon *icon = g_themed_icon_new_from_names((char **)names, 2);

	gtk_image_set_from_gicon(img, icon, GTK_ICON_SIZE_MENU);
	gtk_image_set_pixel_size(img, 12);
	g_object_unref(icon);
}

static GtkWidget *chevron_image(void)
{
	GtkWidget *img = gtk_image_new();

	set_chevron(GTK_IMAGE(img), FALSE);
	return img;
}

struct _LxBreadcrumb {
	GtkBin parent;
	GtkWidget *stack;
	GtkWidget *scroller;
	GtkWidget *crumbs; /* GtkBox */
	GtkWidget *entry;
	char *edit_text;
	guint n;
	guint scroll_id;
};

G_DEFINE_TYPE(LxBreadcrumb, lx_breadcrumb, GTK_TYPE_BIN)

enum { SIG_CRUMB, SIG_TEXT, SIG_CHILDREN, N_SIGS };
static guint sigs[N_SIGS];

static void lx_breadcrumb_finalize(GObject *o)
{
	LxBreadcrumb *b = LX_BREADCRUMB(o);

	if (b->scroll_id)
		g_source_remove(b->scroll_id);
	g_free(b->edit_text);
	G_OBJECT_CLASS(lx_breadcrumb_parent_class)->finalize(o);
}

/* A GtkBin draws only its child: the bar's own frame and fill, from the
 * theme's lx-breadcrumb rule, are drawn here. */
static gboolean lx_breadcrumb_draw(GtkWidget *w, cairo_t *cr)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	int width = gtk_widget_get_allocated_width(w);
	int height = gtk_widget_get_allocated_height(w);

	gtk_render_background(sc, cr, 0, 0, width, height);
	gtk_render_frame(sc, cr, 0, 0, width, height);
	return GTK_WIDGET_CLASS(lx_breadcrumb_parent_class)->draw(w, cr);
}

static void lx_breadcrumb_class_init(LxBreadcrumbClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->finalize = lx_breadcrumb_finalize;
	GTK_WIDGET_CLASS(k)->draw = lx_breadcrumb_draw;
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(k), "lx-breadcrumb");
	sigs[SIG_CRUMB] = g_signal_new("crumb-activated", t, G_SIGNAL_RUN_LAST,
				       0, NULL, NULL, NULL, G_TYPE_NONE, 1,
				       G_TYPE_UINT);
	sigs[SIG_TEXT] = g_signal_new("text-activated", t, G_SIGNAL_RUN_LAST, 0,
				      NULL, NULL, NULL, G_TYPE_NONE, 1,
				      G_TYPE_STRING);
	sigs[SIG_CHILDREN] = g_signal_new("children-wanted", t,
					  G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					  NULL, G_TYPE_NONE, 2, G_TYPE_UINT,
					  GTK_TYPE_WIDGET);
}

static gboolean scroll_to_end(gpointer data)
{
	LxBreadcrumb *b = data;
	GtkAdjustment *a = gtk_scrolled_window_get_hadjustment(
		GTK_SCROLLED_WINDOW(b->scroller));

	b->scroll_id = 0;
	/* The innermost crumb is the one that matters: keep it in view. */
	gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a) -
					    gtk_adjustment_get_page_size(a));
	return G_SOURCE_REMOVE;
}

static void crumbs_resized(GtkWidget *w, GdkRectangle *r, LxBreadcrumb *b)
{
	if (!b->scroll_id)
		b->scroll_id = g_idle_add(scroll_to_end, b);
}

static void entry_activate(GtkEntry *e, LxBreadcrumb *b)
{
	char *text = g_strdup(gtk_entry_get_text(e));

	lx_breadcrumb_stop_edit(b);
	g_signal_emit(b, sigs[SIG_TEXT], 0, text);
	g_free(text);
}

static gboolean entry_key(GtkWidget *w, GdkEventKey *ev, LxBreadcrumb *b)
{
	if (ev->keyval == GDK_KEY_Escape) {
		lx_breadcrumb_stop_edit(b);
		return TRUE;
	}
	return FALSE;
}

static gboolean entry_focus_out(GtkWidget *w, GdkEventFocus *ev,
				LxBreadcrumb *b)
{
	/* GTK's completion popup keeps the keyboard focus on the entry, so
	 * leaving the field really means leaving it. */
	lx_breadcrumb_stop_edit(b);
	return FALSE;
}

static gboolean blank_clicked(GtkWidget *w, GdkEventButton *ev,
			      LxBreadcrumb *b)
{
	if (ev->button == 1 && ev->type == GDK_BUTTON_PRESS) {
		lx_breadcrumb_edit(b);
		return TRUE;
	}
	return FALSE;
}

static void lx_breadcrumb_init(LxBreadcrumb *b)
{
	GtkWidget *eb = gtk_event_box_new();

	b->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(b->stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(b->stack), 80);

	b->crumbs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(b->crumbs),
				    "lx-crumbs");
	b->scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(b->scroller),
				       GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_propagate_natural_height(
		GTK_SCROLLED_WINDOW(b->scroller), TRUE);
	gtk_container_add(GTK_CONTAINER(b->scroller), b->crumbs);
	g_signal_connect(b->crumbs, "size-allocate",
			 G_CALLBACK(crumbs_resized), b);
	/* The empty part of the bar starts editing. */
	gtk_container_add(GTK_CONTAINER(eb), b->scroller);
	gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(eb, "button-press-event", G_CALLBACK(blank_clicked),
			 b);

	b->entry = gtk_entry_new();
	gtk_entry_set_icon_from_icon_name(GTK_ENTRY(b->entry),
					  GTK_ENTRY_ICON_PRIMARY, "folder");
	g_signal_connect(b->entry, "activate", G_CALLBACK(entry_activate), b);
	g_signal_connect(b->entry, "key-press-event", G_CALLBACK(entry_key), b);
	g_signal_connect(b->entry, "focus-out-event",
			 G_CALLBACK(entry_focus_out), b);
	atk_object_set_name(gtk_widget_get_accessible(b->entry), "Address");

	gtk_stack_add_named(GTK_STACK(b->stack), eb, "crumbs");
	gtk_stack_add_named(GTK_STACK(b->stack), b->entry, "edit");
	gtk_container_add(GTK_CONTAINER(b), b->stack);
	gtk_widget_show_all(b->stack);
}

GtkWidget *lx_breadcrumb_new(void)
{
	return g_object_new(LX_TYPE_BREADCRUMB, NULL);
}

static void crumb_clicked(GtkButton *btn, LxBreadcrumb *b)
{
	guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn),
						       "lx-index"));

	g_signal_emit(b, sigs[SIG_CRUMB], 0, idx);
}

static void chevron_toggled(GtkToggleButton *t, LxBreadcrumb *b)
{
	GtkWidget *pop = g_object_get_data(G_OBJECT(t), "lx-popover");
	GtkWidget *box;

	/* Open, the chevron points down at the list. */
	set_chevron(GTK_IMAGE(gtk_button_get_image(GTK_BUTTON(t))),
		    gtk_toggle_button_get_active(t));
	if (!gtk_toggle_button_get_active(t))
		return;
	box = gtk_bin_get_child(GTK_BIN(gtk_bin_get_child(GTK_BIN(pop))));
	gtk_container_foreach(GTK_CONTAINER(box),
			      destroy_child, NULL);
	g_signal_emit(b, sigs[SIG_CHILDREN], 0,
		      GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(t),
							 "lx-index")),
		      box);
	gtk_widget_show_all(box);
}

void lx_breadcrumb_set_crumbs(LxBreadcrumb *b, const char *const *labels,
			      const char *const *icons,
			      const gboolean *expandable, guint n)
{
	gtk_container_foreach(GTK_CONTAINER(b->crumbs),
			      destroy_child, NULL);
	b->n = n;
	/* The place's picture leads the row, then a chevron before the first
	 * name: "[icon] > Computer > System disk >". */
	if (n && icons && icons[0]) {
		GtkWidget *img = gtk_image_new_from_icon_name(icons[0],
							      GTK_ICON_SIZE_MENU);
		GtkWidget *sep = chevron_image();

		gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
		gtk_style_context_add_class(gtk_widget_get_style_context(img),
					    "lx-crumb-icon");
		gtk_style_context_add_class(gtk_widget_get_style_context(sep),
					    "lx-crumb-separator");
		gtk_box_pack_start(GTK_BOX(b->crumbs), img, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(b->crumbs), sep, FALSE, FALSE, 0);
	}
	for (guint i = 0; i < n; i++) {
		GtkWidget *btn = gtk_button_new();
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

		gtk_widget_set_focus_on_click(btn, FALSE);
		gtk_box_pack_start(GTK_BOX(row), gtk_label_new(labels[i]), FALSE,
				   FALSE, 0);
		gtk_container_add(GTK_CONTAINER(btn), row);
		gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
		gtk_style_context_add_class(gtk_widget_get_style_context(btn),
					    "lx-crumb");
		g_object_set_data(G_OBJECT(btn), "lx-index", GUINT_TO_POINTER(i));
		g_signal_connect(btn, "clicked", G_CALLBACK(crumb_clicked), b);
		gtk_widget_set_tooltip_text(btn, labels[i]);
		gtk_box_pack_start(GTK_BOX(b->crumbs), btn, FALSE, FALSE, 0);

		/* The chevron after it lists what is inside. */
		if (expandable && !expandable[i]) {
			/* Nothing to list: a plain separator keeps the row
			 * reading as a path. */
			if (i + 1 < n) {
				GtkWidget *sep = chevron_image();

				gtk_style_context_add_class(
					gtk_widget_get_style_context(sep),
					"lx-crumb-separator");
				gtk_box_pack_start(GTK_BOX(b->crumbs), sep, FALSE,
						   FALSE, 0);
			}
			continue;
		}
		GtkWidget *chev = gtk_menu_button_new();
		GtkWidget *pop = gtk_popover_new(chev);
		GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

		gtk_button_set_image(GTK_BUTTON(chev), chevron_image());
		gtk_button_set_relief(GTK_BUTTON(chev), GTK_RELIEF_NONE);
		gtk_widget_set_focus_on_click(chev, FALSE);
		gtk_style_context_add_class(gtk_widget_get_style_context(chev),
					    "lx-chevron");
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
					       GTK_POLICY_NEVER,
					       GTK_POLICY_AUTOMATIC);
		gtk_scrolled_window_set_max_content_height(
			GTK_SCROLLED_WINDOW(sw), 420);
		gtk_scrolled_window_set_propagate_natural_height(
			GTK_SCROLLED_WINDOW(sw), TRUE);
		gtk_container_add(GTK_CONTAINER(sw), box);
		gtk_container_add(GTK_CONTAINER(pop), sw);
		gtk_widget_show_all(sw);
		gtk_menu_button_set_popover(GTK_MENU_BUTTON(chev), pop);
		g_object_set_data(G_OBJECT(chev), "lx-popover", pop);
		g_object_set_data(G_OBJECT(chev), "lx-index", GUINT_TO_POINTER(i));
		g_signal_connect(chev, "toggled", G_CALLBACK(chevron_toggled), b);
		atk_object_set_name(gtk_widget_get_accessible(chev),
				    "Folders inside");
		gtk_box_pack_start(GTK_BOX(b->crumbs), chev, FALSE, FALSE, 0);
	}
	gtk_widget_show_all(b->crumbs);
}

void lx_breadcrumb_set_edit_text(LxBreadcrumb *b, const char *text)
{
	g_free(b->edit_text);
	b->edit_text = g_strdup(text);
	if (!lx_breadcrumb_is_editing(b))
		gtk_entry_set_text(GTK_ENTRY(b->entry), text ? text : "");
}

void lx_breadcrumb_edit(LxBreadcrumb *b)
{
	gtk_entry_set_text(GTK_ENTRY(b->entry), b->edit_text ? b->edit_text : "");
	gtk_stack_set_visible_child_name(GTK_STACK(b->stack), "edit");
	gtk_widget_grab_focus(b->entry);
	gtk_editable_select_region(GTK_EDITABLE(b->entry), 0, -1);
}

void lx_breadcrumb_stop_edit(LxBreadcrumb *b)
{
	GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));

	/* The field is about to be hidden: it must not keep the keyboard
	 * focus, or keys would go to a widget no longer on screen.  The owner
	 * puts the focus where it belongs next. */
	if (gtk_widget_has_focus(b->entry) && GTK_IS_WINDOW(top))
		gtk_window_set_focus(GTK_WINDOW(top), NULL);
	gtk_stack_set_visible_child_name(GTK_STACK(b->stack), "crumbs");
}

gboolean lx_breadcrumb_is_editing(LxBreadcrumb *b)
{
	return g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(b->stack)),
			 "edit") == 0;
}

GtkEntry *lx_breadcrumb_get_entry(LxBreadcrumb *b)
{
	return GTK_ENTRY(b->entry);
}
