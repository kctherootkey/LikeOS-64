// LikeOS -- File Explorer: the command bar.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-commandbar.h"
#include "ui/ex-context-menu.h"
#include "ui/ex-style.h"
#include "ui/widgets/lx-split-button.h"

/* Groups shown or hidden by place. */
typedef struct {
	GtkWidget *new_btn, *clip, *paste, *edit, *trash_grp, *sep1, *sep2;
	GtkWidget *sort, *view;
} Bar;

static GtkWidget *icon_button(const char *icon, const char *tip,
			      const char *action)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon,
						     GTK_ICON_SIZE_SMALL_TOOLBAR);

	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(b, tip);
	atk_object_set_name(gtk_widget_get_accessible(b), tip);
	gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(b), action);
	return b;
}

static GtkWidget *label_button(const char *icon, const char *label,
			       const char *action)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	gtk_box_pack_start(GTK_BOX(row),
			   gtk_image_new_from_icon_name(icon,
							GTK_ICON_SIZE_SMALL_TOOLBAR),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), gtk_label_new(label), FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(b), action);
	atk_object_set_name(gtk_widget_get_accessible(b), label);
	return b;
}

static GtkWidget *group(void)
{
	return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
}

static GtkWidget *sep(void)
{
	GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);

	gtk_widget_set_margin_start(s, 6);
	gtk_widget_set_margin_end(s, 6);
	return s;
}

GtkWidget *ex_commandbar_new(void)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	Bar *b = g_new0(Bar, 1);
	GMenuModel *m;

	ex_style_add(bar, EX_CLASS_COMMANDBAR);
	atk_object_set_role(gtk_widget_get_accessible(bar), ATK_ROLE_TOOL_BAR);
	atk_object_set_name(gtk_widget_get_accessible(bar), "Commands");

	m = ex_context_menu_new_items();
	b->new_btn = lx_split_button_new("folder-new", "New", "win.new-folder", m);
	g_object_unref(m);
	gtk_box_pack_start(GTK_BOX(bar), b->new_btn, FALSE, FALSE, 0);
	b->sep1 = sep();
	gtk_box_pack_start(GTK_BOX(bar), b->sep1, FALSE, FALSE, 0);

	b->clip = group();
	gtk_box_pack_start(GTK_BOX(b->clip), icon_button("edit-cut", "Cut (Ctrl+X)",
							 "win.cut"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->clip),
			   icon_button("edit-copy", "Copy (Ctrl+C)", "win.copy"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), b->clip, FALSE, FALSE, 0);
	b->paste = icon_button("edit-paste", "Paste (Ctrl+V)", "win.paste");
	gtk_box_pack_start(GTK_BOX(bar), b->paste, FALSE, FALSE, 0);

	b->edit = group();
	gtk_box_pack_start(GTK_BOX(b->edit),
			   icon_button("insert-text", "Rename (F2)", "win.rename"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->edit),
			   icon_button("emblem-shared", "Share", "win.share"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->edit),
			   icon_button("user-trash", "Delete (Del)", "win.trash"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), b->edit, FALSE, FALSE, 0);

	b->trash_grp = group();
	gtk_box_pack_start(GTK_BOX(b->trash_grp),
			   label_button("user-trash", "Empty Trash", "win.empty-trash"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->trash_grp),
			   label_button("edit-undo", "Restore", "win.restore"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->trash_grp),
			   label_button("edit-undo", "Restore all", "win.restore-all"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->trash_grp),
			   icon_button("edit-delete", "Delete permanently",
				       "win.delete"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), b->trash_grp, FALSE, FALSE, 0);

	b->sep2 = sep();
	gtk_box_pack_start(GTK_BOX(bar), b->sep2, FALSE, FALSE, 0);
	m = ex_context_menu_sort();
	b->sort = lx_menu_button_new("view-sort-ascending", "Sort", m);
	gtk_box_pack_start(GTK_BOX(bar), b->sort, FALSE, FALSE, 0);
	g_object_unref(m);
	m = ex_context_menu_view();
	b->view = lx_menu_button_new("view-grid", "View", m);
	gtk_box_pack_start(GTK_BOX(bar), b->view, FALSE, FALSE, 0);
	g_object_unref(m);

	m = ex_context_menu_more();
	GtkWidget *more = gtk_menu_button_new();
	gtk_button_set_image(GTK_BUTTON(more),
			     gtk_image_new_from_icon_name("view-more",
							  GTK_ICON_SIZE_SMALL_TOOLBAR));
	gtk_button_set_relief(GTK_BUTTON(more), GTK_RELIEF_NONE);
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(more), m);
	gtk_menu_button_set_use_popover(GTK_MENU_BUTTON(more), FALSE);
	gtk_widget_set_tooltip_text(more, "See more");
	atk_object_set_name(gtk_widget_get_accessible(more), "See more");
	g_object_unref(m);
	gtk_box_pack_end(GTK_BOX(bar), more, FALSE, FALSE, 0);

	g_object_set_data_full(G_OBJECT(bar), "ex-bar", b, g_free);
	gtk_widget_show_all(bar);
	gtk_widget_hide(b->trash_grp);
	return bar;
}

void ex_commandbar_set_kind(GtkWidget *bar, ExLocationKind kind)
{
	Bar *b = g_object_get_data(G_OBJECT(bar), "ex-bar");
	gboolean folder = kind == EX_LOC_FOLDER;
	gboolean trash = kind == EX_LOC_TRASH;
	gboolean search = kind == EX_LOC_SEARCH;
	gboolean items = folder || search;

	gtk_widget_set_visible(b->new_btn, folder);
	gtk_widget_set_visible(b->sep1, folder);
	gtk_widget_set_visible(b->clip, items);
	gtk_widget_set_visible(b->paste, folder);
	gtk_widget_set_visible(b->edit, items);
	gtk_widget_set_visible(b->trash_grp, trash);
	gtk_widget_set_visible(b->sep2, items || trash);
	/* Home and This PC are not item lists: nothing to sort or lay out. */
	gtk_widget_set_visible(b->sort, items || trash);
	gtk_widget_set_visible(b->view, items || trash);
}
