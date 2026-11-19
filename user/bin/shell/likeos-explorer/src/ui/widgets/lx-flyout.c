// LikeOS -- flyout.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/widgets/lx-flyout.h"

GtkWidget *lx_flyout_new(GtkWidget *relative_to, const char *title,
			 GtkWidget *body)
{
	GtkWidget *pop = gtk_popover_new(relative_to);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

	gtk_style_context_add_class(gtk_widget_get_style_context(pop),
				    "lx-flyout");
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	if (title) {
		GtkWidget *t = gtk_label_new(title);

		gtk_label_set_xalign(GTK_LABEL(t), 0);
		gtk_style_context_add_class(gtk_widget_get_style_context(t),
					    "lx-flyout-title");
		gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(box), body, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(pop), box);
	gtk_widget_show_all(box);
	return pop;
}
