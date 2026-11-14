/* LikeOS installer -- a password entry.
 *
 * The characters are hidden; the eye at the end of the entry shows them
 * while it is on.  Under the entry, four segments and a word tell how
 * strong the password is (the page computes the strength with the
 * backend's il_password_strength and hands it in).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "widgets.h"

typedef struct {
	GtkWidget *entry;
	GtkWidget *meter_box;
	GtkWidget *meter;
	GtkWidget *meter_label;
	int strength;
	gboolean revealed;
} PwEntry;

static const char *const strength_words[] = {
	"Very weak", "Weak", "Fair", "Good", "Strong",
};

static const guint32 strength_colors[] = {
	0xd63a3a, 0xe0682b, 0xd98a00, 0x4f9d3a, 0x1f9d55,
};

static PwEntry *pw_data(GtkWidget *w)
{
	return g_object_get_data(G_OBJECT(w), "password-entry");
}

static void set_eye(PwEntry *pe)
{
	GdkRGBA c = { 0.36, 0.40, 0.47, 1.0 };
	GdkPixbuf *pb = icon_pixbuf(pe->revealed ? ICON_EYE_OFF : ICON_EYE, 18, &c);

	gtk_entry_set_icon_from_pixbuf(GTK_ENTRY(pe->entry),
				       GTK_ENTRY_ICON_SECONDARY, pb);
	gtk_entry_set_icon_tooltip_text(GTK_ENTRY(pe->entry),
					GTK_ENTRY_ICON_SECONDARY,
					pe->revealed ? "Hide the password" :
					"Show the password");
	gtk_entry_set_icon_activatable(GTK_ENTRY(pe->entry),
				       GTK_ENTRY_ICON_SECONDARY, TRUE);
	g_object_unref(pb);
}

static void on_icon_press(GtkEntry *entry, GtkEntryIconPosition pos,
			  GdkEvent *ev, gpointer data)
{
	PwEntry *pe = data;

	(void)entry;
	(void)ev;
	if (pos != GTK_ENTRY_ICON_SECONDARY)
		return;
	pe->revealed = !pe->revealed;
	gtk_entry_set_visibility(GTK_ENTRY(pe->entry), pe->revealed);
	set_eye(pe);
}

static gboolean meter_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	PwEntry *pe = data;
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	const int segs = 4, gap = 4;
	double sw = (aw - gap * (segs - 1)) / (double)segs;
	int lit = pe->strength <= 0 ? 1 : pe->strength;
	guint32 col = strength_colors[CLAMP(pe->strength, 0, 4)];

	for (int i = 0; i < segs; i++) {
		double x = i * (sw + gap);

		if (i < lit)
			art_set_rgb(cr, col, 1);
		else
			cairo_set_source_rgba(cr, 0.55, 0.60, 0.70, 0.25);
		art_rounded_rect(cr, x, 0, sw, ah, ah / 2.0);
		cairo_fill(cr);
	}
	return FALSE;
}

GtkWidget *password_entry_new(const char *placeholder, gboolean show_meter)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	PwEntry *pe = g_new0(PwEntry, 1);

	pe->strength = -1;
	pe->entry = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(pe->entry), FALSE);
	gtk_entry_set_input_purpose(GTK_ENTRY(pe->entry),
				    GTK_INPUT_PURPOSE_PASSWORD);
	gtk_entry_set_max_length(GTK_ENTRY(pe->entry), 255);
	gtk_entry_set_activates_default(GTK_ENTRY(pe->entry), TRUE);
	if (placeholder)
		gtk_entry_set_placeholder_text(GTK_ENTRY(pe->entry), placeholder);
	gtk_style_context_add_class(gtk_widget_get_style_context(pe->entry),
				    "il-entry");
	g_signal_connect(pe->entry, "icon-press", G_CALLBACK(on_icon_press), pe);
	gtk_box_pack_start(GTK_BOX(box), pe->entry, FALSE, FALSE, 0);

	pe->meter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	pe->meter = gtk_drawing_area_new();
	gtk_widget_set_size_request(pe->meter, -1, 4);
	gtk_widget_set_valign(pe->meter, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(pe->meter, TRUE);
	g_signal_connect(pe->meter, "draw", G_CALLBACK(meter_draw), pe);
	pe->meter_label = gtk_label_new("");
	gtk_label_set_width_chars(GTK_LABEL(pe->meter_label), 9);
	gtk_label_set_xalign(GTK_LABEL(pe->meter_label), 1);
	gtk_style_context_add_class(gtk_widget_get_style_context(pe->meter_label),
				    "il-meter-label");
	gtk_box_pack_start(GTK_BOX(pe->meter_box), pe->meter, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(pe->meter_box), pe->meter_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), pe->meter_box, FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(pe->meter_box, TRUE);
	(void)show_meter;

	gtk_style_context_add_class(gtk_widget_get_style_context(box),
				    "il-password");
	g_object_set_data_full(G_OBJECT(box), "password-entry", pe, g_free);
	set_eye(pe);
	return box;
}

GtkWidget *password_entry_get_entry(GtkWidget *w)
{
	PwEntry *pe = pw_data(w);

	return pe ? pe->entry : NULL;
}

const char *password_entry_get_text(GtkWidget *w)
{
	PwEntry *pe = pw_data(w);

	return pe ? gtk_entry_get_text(GTK_ENTRY(pe->entry)) : "";
}

void password_entry_set_text(GtkWidget *w, const char *text)
{
	PwEntry *pe = pw_data(w);

	if (pe)
		gtk_entry_set_text(GTK_ENTRY(pe->entry), text ? text : "");
}

void password_entry_set_strength(GtkWidget *w, int strength)
{
	PwEntry *pe = pw_data(w);
	GtkStyleContext *sc;

	if (!pe)
		return;
	pe->strength = strength;
	if (strength < 0) {
		gtk_widget_hide(pe->meter_box);
		return;
	}
	strength = CLAMP(strength, 0, 4);
	gtk_label_set_text(GTK_LABEL(pe->meter_label), strength_words[strength]);
	sc = gtk_widget_get_style_context(pe->meter_label);
	for (int i = 0; i <= 4; i++) {
		char cls[24];

		g_snprintf(cls, sizeof(cls), "il-strength-%d", i);
		if (i == strength)
			gtk_style_context_add_class(sc, cls);
		else
			gtk_style_context_remove_class(sc, cls);
	}
	gtk_widget_show(pe->meter);
	gtk_widget_show(pe->meter_label);
	gtk_widget_show(pe->meter_box);
	gtk_widget_queue_draw(pe->meter);
}

void password_entry_set_visible_text(GtkWidget *w, gboolean visible)
{
	PwEntry *pe = pw_data(w);

	if (!pe)
		return;
	pe->revealed = visible;
	gtk_entry_set_visibility(GTK_ENTRY(pe->entry), visible);
	set_eye(pe);
}
