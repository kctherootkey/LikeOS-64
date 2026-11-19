/* LikeOS installer -- link buttons and toolbar actions.
 *
 * A link button is a flat button whose label looks like a link: the
 * accent colour, underlined while the pointer is on it, optionally led by
 * a small icon.  A toolbar action is the same thing for the disk page's
 * toolbar: icon and label side by side, grey when the action does not
 * apply to the selected row.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "widgets.h"

static GtkWidget *icon_label_button(const char *label, IconKind icon,
				    int icon_size, const char *css_class)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *l = gtk_label_new_with_mnemonic(label);

	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_style_context_add_class(gtk_widget_get_style_context(b), css_class);
	if (icon != ICON_NONE) {
		GtkWidget *i = icon_new(icon, icon_size);

		gtk_box_pack_start(GTK_BOX(box), i, FALSE, FALSE, 0);
		g_object_set_data(G_OBJECT(b), "button-icon", i);
	}
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_mnemonic_widget(GTK_LABEL(l), b);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "il-button-label");
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(b), "button-label", l);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_widget_set_halign(b, GTK_ALIGN_START);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	return b;
}

GtkWidget *link_button_new(const char *label, IconKind icon)
{
	return icon_label_button(label, icon, 16, "il-link");
}

void link_button_set_label(GtkWidget *b, const char *label)
{
	GtkWidget *l = g_object_get_data(G_OBJECT(b), "button-label");

	if (l)
		gtk_label_set_text_with_mnemonic(GTK_LABEL(l), label);
}

GtkWidget *toolbar_action_new(const char *label, IconKind icon,
			      const char *tooltip)
{
	GtkWidget *b = icon_label_button(label, icon, 18, "il-action");

	if (tooltip)
		gtk_widget_set_tooltip_text(b, tooltip);
	gtk_widget_set_halign(b, GTK_ALIGN_FILL);
	return b;
}

void toolbar_action_set_style(GtkWidget *b, const char *style)
{
	GtkWidget *i = g_object_get_data(G_OBJECT(b), "button-icon");
	char cls[48];

	if (!i || !style)
		return;
	g_snprintf(cls, sizeof(cls), "il-icon-%s", style);
	gtk_style_context_add_class(gtk_widget_get_style_context(i), cls);
}
