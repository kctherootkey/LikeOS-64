// LikeOS -- split button.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/widgets/lx-split-button.h"

static GtkWidget *icon_label(const char *icon_name, const char *label)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	if (icon_name)
		gtk_box_pack_start(GTK_BOX(row),
				   gtk_image_new_from_icon_name(
					   icon_name, GTK_ICON_SIZE_SMALL_TOOLBAR),
				   FALSE, FALSE, 0);
	if (label)
		gtk_box_pack_start(GTK_BOX(row), gtk_label_new(label), FALSE,
				   FALSE, 0);
	return row;
}

GtkWidget *lx_split_button_new(const char *icon_name, const char *label,
			       const char *action, GMenuModel *menu)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *main_btn = gtk_button_new();
	GtkWidget *arrow = gtk_menu_button_new();

	gtk_style_context_add_class(gtk_widget_get_style_context(box),
				    GTK_STYLE_CLASS_LINKED);
	gtk_style_context_add_class(gtk_widget_get_style_context(box),
				    "lx-split-button");
	gtk_container_add(GTK_CONTAINER(main_btn), icon_label(icon_name, label));
	gtk_button_set_relief(GTK_BUTTON(main_btn), GTK_RELIEF_NONE);
	gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(main_btn), action);
	atk_object_set_name(gtk_widget_get_accessible(main_btn), label);
	gtk_button_set_relief(GTK_BUTTON(arrow), GTK_RELIEF_NONE);
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(arrow), menu);
	gtk_menu_button_set_use_popover(GTK_MENU_BUTTON(arrow), FALSE);
	gtk_widget_set_tooltip_text(arrow, "More choices");
	atk_object_set_name(gtk_widget_get_accessible(arrow), "More choices");
	gtk_box_pack_start(GTK_BOX(box), main_btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), arrow, FALSE, FALSE, 0);
	return box;
}

GtkWidget *lx_menu_button_new(const char *icon_name, const char *label,
			      GMenuModel *menu)
{
	GtkWidget *b = gtk_menu_button_new();
	GtkWidget *row = icon_label(icon_name, label);

	gtk_box_pack_start(GTK_BOX(row),
			   gtk_image_new_from_icon_name("pan-down-symbolic",
							GTK_ICON_SIZE_MENU),
			   FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(b), menu);
	gtk_menu_button_set_use_popover(GTK_MENU_BUTTON(b), FALSE);
	if (label)
		atk_object_set_name(gtk_widget_get_accessible(b), label);
	return b;
}
