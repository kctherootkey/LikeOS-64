// LikeOS -- segmented control.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/widgets/lx-segmented.h"

struct _LxSegmented {
	GtkBox parent;
	GPtrArray *buttons;
	int active;
	gboolean updating;
};

G_DEFINE_TYPE(LxSegmented, lx_segmented, GTK_TYPE_BOX)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void lx_segmented_finalize(GObject *o)
{
	g_ptr_array_unref(LX_SEGMENTED(o)->buttons);
	G_OBJECT_CLASS(lx_segmented_parent_class)->finalize(o);
}

static void lx_segmented_class_init(LxSegmentedClass *k)
{
	G_OBJECT_CLASS(k)->finalize = lx_segmented_finalize;
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(k), "lx-segmented");
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void lx_segmented_init(LxSegmented *s)
{
	s->buttons = g_ptr_array_new();
	s->active = -1;
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(s)),
				    GTK_STYLE_CLASS_LINKED);
}

GtkWidget *lx_segmented_new(void)
{
	return g_object_new(LX_TYPE_SEGMENTED, "orientation",
			    GTK_ORIENTATION_HORIZONTAL, NULL);
}

static void sync_buttons(LxSegmented *s)
{
	s->updating = TRUE;
	for (guint i = 0; i < s->buttons->len; i++)
		gtk_toggle_button_set_active(g_ptr_array_index(s->buttons, i),
					     (int)i == s->active);
	s->updating = FALSE;
}

static void toggled(GtkToggleButton *t, LxSegmented *s)
{
	int idx;

	if (s->updating)
		return;
	idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "lx-index"));
	if (idx == s->active) {
		/* Clicking the active one keeps it active. */
		sync_buttons(s);
		return;
	}
	s->active = idx;
	sync_buttons(s);
	g_signal_emit(s, sigs[SIG_CHANGED], 0, idx);
}

int lx_segmented_append(LxSegmented *s, const char *icon_name,
			const char *tooltip)
{
	GtkWidget *b = gtk_toggle_button_new();
	int idx = (int)s->buttons->len;

	gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(
						    icon_name, GTK_ICON_SIZE_MENU));
	gtk_widget_set_tooltip_text(b, tooltip);
	atk_object_set_name(gtk_widget_get_accessible(b), tooltip);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	g_object_set_data(G_OBJECT(b), "lx-index", GINT_TO_POINTER(idx));
	g_signal_connect(b, "toggled", G_CALLBACK(toggled), s);
	g_ptr_array_add(s->buttons, b);
	gtk_box_pack_start(GTK_BOX(s), b, FALSE, FALSE, 0);
	gtk_widget_show(b);
	return idx;
}

void lx_segmented_set_active(LxSegmented *s, int index)
{
	s->active = index;
	sync_buttons(s);
}

int lx_segmented_get_active(LxSegmented *s)
{
	return s->active;
}
