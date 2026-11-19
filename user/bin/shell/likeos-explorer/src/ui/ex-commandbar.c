// LikeOS Explorer: the command bar.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-commandbar.h"
#include "ui/ex-context-menu.h"
#include "ui/ex-style.h"

/* Groups shown or hidden by place. */
typedef struct {
	GtkWidget *new_btn, *clip, *paste, *edit, *trash_grp, *sep1, *sep2;
	GtkWidget *sep_edit;
	GtkWidget *sort, *view, *sep3;
	GtkWidget *filter, *collection, *images;
	ExLocationKind kind;
} Bar;

/* The commands' pictures are the theme's symbolic line icons, so they take
 * the button's colour: the text colour at rest, grey when the command does
 * not apply, the accent on New. */
static GtkWidget *bar_icon(const char *icon, const char *fallback)
{
	char *sym = g_strconcat(icon, "-symbolic", NULL);
	GtkWidget *img = ex_style_icon(sym, fallback, 16);

	g_free(sym);
	return img;
}

/* A square button with its picture alone, named by its tooltip. */
static GtkWidget *icon_button(const char *icon, const char *fallback,
			      const char *tip, const char *action)
{
	GtkWidget *b = ex_style_icon_button(bar_icon(icon, fallback), tip,
					    action);

	ex_style_add(b, "ex-command");
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	return b;
}

static GtkWidget *label_box(const char *icon, const char *fallback,
			    const char *label, gboolean arrow)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *l = gtk_label_new_with_mnemonic(label);

	gtk_box_pack_start(GTK_BOX(row), bar_icon(icon, fallback), FALSE, FALSE,
			   0);
	gtk_widget_set_margin_start(l, 6);
	gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
	if (arrow) {
		GtkWidget *a = ex_style_icon("likeos-chevron-down-symbolic",
					     "pan-down-symbolic", 12);

		ex_style_add(a, "ex-dropdown-arrow");
		gtk_widget_set_margin_start(a, 6);
		gtk_box_pack_start(GTK_BOX(row), a, FALSE, FALSE, 0);
	}
	return row;
}

static GtkWidget *label_button(const char *icon, const char *fallback,
			       const char *label, const char *action)
{
	GtkWidget *b = ex_style_icon_button(label_box(icon, fallback, label, FALSE),
					    NULL, action);

	ex_style_add(b, "ex-command");
	ex_style_add(b, "text");
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	atk_object_set_name(gtk_widget_get_accessible(b), label);
	return b;
}

static void menu_clicked(GtkButton *b, GMenuModel *menu)
{
	ex_context_menu_popup_below(GTK_WIDGET(b), menu);
}

/* "Sort ⌄": a button with a word that opens a menu -- the same LikeOS menu
 * a right click opens, hanging from the button. */
static GtkWidget *menu_button(const char *icon, const char *fallback,
			      const char *label, const char *tip,
			      GMenuModel *menu)
{
	GtkWidget *b = gtk_button_new();

	gtk_container_add(GTK_CONTAINER(b), label ? label_box(icon, fallback,
							      label, TRUE) :
						    bar_icon(icon, fallback));
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_focus_on_click(b, FALSE);
	g_object_set_data_full(G_OBJECT(b), "ex-menu", g_object_ref(menu),
			       g_object_unref);
	g_signal_connect(b, "clicked", G_CALLBACK(menu_clicked), menu);
	atk_object_set_role(gtk_widget_get_accessible(b), ATK_ROLE_PUSH_BUTTON);
	ex_style_add(b, EX_CLASS_SUBTLE);
	ex_style_add(b, "ex-command");
	if (label)
		ex_style_add(b, "text");
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	if (tip)
		gtk_widget_set_tooltip_text(b, tip);
	atk_object_set_name(gtk_widget_get_accessible(b), label ? label : tip);
	return b;
}

/* One of the two pane switches at the far end. */
static GtkWidget *pane_toggle(const char *icon, const char *fallback,
			      const char *tip, const char *name,
			      const char *action)
{
	GtkWidget *b = gtk_toggle_button_new();

	gtk_container_add(GTK_CONTAINER(b), bar_icon(icon, fallback));
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_focus_on_click(b, FALSE);
	gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(b), action);
	gtk_widget_set_tooltip_text(b, tip);
	atk_object_set_name(gtk_widget_get_accessible(b), name);
	ex_style_add(b, EX_CLASS_SUBTLE);
	ex_style_add(b, "ex-command");
	ex_style_add(b, "ex-pane-toggle");
	return b;
}

static GtkWidget *group(void)
{
	return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
}

static GtkWidget *sep(void)
{
	return gtk_separator_new(GTK_ORIENTATION_VERTICAL);
}

/* ---- a group that gives way ------------------------------------------------
 *
 * Holds two versions of one group of commands: the full one (icons and words)
 * and a compact one (icons alone).  It asks for no width of its own -- only
 * for the full version's width as what it would like -- and shows the full
 * version when it gets that much, the compact one when it gets that much, and
 * nothing when it does not.  So commands that come and go with the selection
 * use only the room the bar has: they never widen the window or squeeze the
 * sidebar to make room for themselves. */
#define EX_TYPE_FIT_BIN (ex_fit_bin_get_type())
G_DECLARE_FINAL_TYPE(ExFitBin, ex_fit_bin, EX, FIT_BIN, GtkContainer)

struct _ExFitBin {
	GtkContainer parent;
	GtkWidget *full, *compact;
};

G_DEFINE_TYPE(ExFitBin, ex_fit_bin, GTK_TYPE_CONTAINER)

static int fit_natural_width(GtkWidget *w)
{
	int nat = 0;

	if (w && gtk_widget_get_visible(w))
		gtk_widget_get_preferred_width(w, NULL, &nat);
	return nat;
}

static GtkSizeRequestMode fit_request_mode(GtkWidget *w)
{
	(void)w;
	return GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

static void fit_preferred_width(GtkWidget *w, int *min, int *nat)
{
	*min = 0;
	*nat = fit_natural_width(EX_FIT_BIN(w)->full);
}

static void fit_preferred_height(GtkWidget *w, int *min, int *nat)
{
	ExFitBin *f = EX_FIT_BIN(w);
	GtkWidget *kids[2] = { f->full, f->compact };

	*min = *nat = 0;
	for (int i = 0; i < 2; i++) {
		int m, n;

		if (!kids[i] || !gtk_widget_get_visible(kids[i]))
			continue;
		gtk_widget_get_preferred_height(kids[i], &m, &n);
		*min = MAX(*min, m);
		*nat = MAX(*nat, n);
	}
}

static void fit_size_allocate(GtkWidget *w, GtkAllocation *a)
{
	ExFitBin *f = EX_FIT_BIN(w);
	int full = fit_natural_width(f->full);
	int compact = fit_natural_width(f->compact);
	GtkWidget *use = NULL;

	gtk_widget_set_allocation(w, a);
	if (f->full && full > 0 && a->width >= full)
		use = f->full;
	else if (f->compact && compact > 0 && a->width >= compact)
		use = f->compact;
	if (f->full)
		gtk_widget_set_child_visible(f->full, use == f->full);
	if (f->compact)
		gtk_widget_set_child_visible(f->compact, use == f->compact);
	if (use) {
		GtkAllocation c = *a;

		c.width = use == f->full ? full : compact;
		gtk_widget_size_allocate(use, &c);
	}
}

static void fit_forall(GtkContainer *c, gboolean internals, GtkCallback cb,
		       gpointer data)
{
	ExFitBin *f = EX_FIT_BIN(c);
	GtkWidget *full = f->full, *compact = f->compact;

	(void)internals;
	/* The callback may remove the child (destroy does). */
	if (full)
		cb(full, data);
	if (compact)
		cb(compact, data);
}

static void fit_remove(GtkContainer *c, GtkWidget *child)
{
	ExFitBin *f = EX_FIT_BIN(c);

	if (child == f->full)
		f->full = NULL;
	else if (child == f->compact)
		f->compact = NULL;
	else
		return;
	gtk_widget_unparent(child);
	gtk_widget_queue_resize(GTK_WIDGET(c));
}

static void ex_fit_bin_class_init(ExFitBinClass *k)
{
	GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
	GtkContainerClass *cc = GTK_CONTAINER_CLASS(k);

	wc->get_request_mode = fit_request_mode;
	wc->get_preferred_width = fit_preferred_width;
	wc->get_preferred_height = fit_preferred_height;
	wc->size_allocate = fit_size_allocate;
	cc->forall = fit_forall;
	cc->remove = fit_remove;
}

static void ex_fit_bin_init(ExFitBin *f)
{
	gtk_widget_set_has_window(GTK_WIDGET(f), FALSE);
}

static GtkWidget *fit_bin_new(GtkWidget *full, GtkWidget *compact)
{
	ExFitBin *f = g_object_new(EX_TYPE_FIT_BIN, NULL);

	f->full = full;
	f->compact = compact;
	gtk_widget_set_parent(full, GTK_WIDGET(f));
	gtk_widget_set_parent(compact, GTK_WIDGET(f));
	/* Nothing shown until the first allocation picks a version. */
	gtk_widget_set_child_visible(full, FALSE);
	gtk_widget_set_child_visible(compact, FALSE);
	return GTK_WIDGET(f);
}

/* ---- the menus -------------------------------------------------------------- */

static void entry(GMenu *m, const char *label, const char *action,
		  const char *icon, const char *fallback)
{
	GMenuItem *it = g_menu_item_new(label, action);

	if (icon) {
		GIcon *gi = ex_style_gicon(icon, fallback);

		g_menu_item_set_icon(it, gi);
		g_object_unref(gi);
	}
	g_menu_append_item(m, it);
	g_object_unref(it);
}

static GMenu *section(GMenu *m)
{
	GMenu *s = g_menu_new();

	g_menu_append_section(m, NULL, G_MENU_MODEL(s));
	g_object_unref(s);
	return s;
}

/* Home's Filter: the kinds of file Recent and Favorites can be narrowed
 * to. */
static GMenuModel *filter_menu(void)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);

	entry(s, "_All files", "win.home-filter::all", NULL, NULL);
	s = section(m);
	entry(s, "_Documents", "win.home-filter::document", "x-office-document",
	      "text-x-generic");
	entry(s, "_Pictures", "win.home-filter::image", "image-x-generic", NULL);
	entry(s, "_Videos", "win.home-filter::video", "video-x-generic", NULL);
	entry(s, "_Music", "win.home-filter::audio", "audio-x-generic", NULL);
	entry(s, "_Folders", "win.home-filter::folder", "folder", NULL);
	entry(s, "_Compressed folders", "win.home-filter::archive",
	      "package-x-generic", NULL);
	return G_MENU_MODEL(m);
}

/* Gallery's Collection: the folders whose pictures and videos it shows. */
static GMenuModel *collection_menu(void)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);

	entry(s, "_Pictures", "win.gallery-pictures", "folder-pictures", NULL);
	entry(s, "_Videos", "win.gallery-videos", "folder-videos", NULL);
	entry(s, "_Desktop", "win.gallery-desktop", "user-desktop", NULL);
	entry(s, "Do_wnloads", "win.gallery-downloads", "folder-download", NULL);
	entry(s, "D_ocuments", "win.gallery-documents", "folder-documents", NULL);
	return G_MENU_MODEL(m);
}

GtkWidget *ex_commandbar_new(void)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	Bar *b = g_new0(Bar, 1);
	GtkWidget *more, *details, *full, *compact;
	GMenuModel *m;

	ex_style_add(bar, EX_CLASS_COMMANDBAR);
	atk_object_set_role(gtk_widget_get_accessible(bar), ATK_ROLE_TOOL_BAR);
	atk_object_set_name(gtk_widget_get_accessible(bar), "Commands");

	/* New: the list of kinds opens at once. */
	m = ex_context_menu_new_items();
	b->new_btn = menu_button("likeos-new", "list-add-symbolic", "_New",
				 "Create a new item in the current location", m);
	ex_style_add(b->new_btn, "ex-new");
	g_object_unref(m);
	gtk_box_pack_start(GTK_BOX(bar), b->new_btn, FALSE, FALSE, 0);
	b->sep1 = sep();
	gtk_box_pack_start(GTK_BOX(bar), b->sep1, FALSE, FALSE, 0);

	b->clip = group();
	gtk_box_pack_start(GTK_BOX(b->clip),
			   icon_button("likeos-cut", "edit-cut-symbolic",
				       "Cut (Ctrl+X)", "win.cut"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->clip),
			   icon_button("likeos-copy", "edit-copy-symbolic",
				       "Copy (Ctrl+C)", "win.copy"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), b->clip, FALSE, FALSE, 0);
	b->paste = icon_button("likeos-paste", "edit-paste-symbolic",
			       "Paste (Ctrl+V)", "win.paste");
	gtk_box_pack_start(GTK_BOX(bar), b->paste, FALSE, FALSE, 0);
	b->sep_edit = sep();
	gtk_box_pack_start(GTK_BOX(bar), b->sep_edit, FALSE, FALSE, 0);

	b->edit = group();
	gtk_box_pack_start(GTK_BOX(b->edit),
			   icon_button("likeos-rename", "insert-text-symbolic",
				       "Rename (F2)", "win.rename"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->edit),
			   icon_button("likeos-share", "emblem-shared-symbolic",
				       "Share", "win.share"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->edit),
			   icon_button("likeos-delete", "user-trash-symbolic",
				       "Delete (Del)", "win.trash"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), b->edit, FALSE, FALSE, 0);

	b->trash_grp = group();
	gtk_box_pack_start(GTK_BOX(b->trash_grp),
			   label_button("likeos-empty-trash", "user-trash-symbolic",
					"Empty Trash", "win.empty-trash"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->trash_grp),
			   label_button("likeos-restore", "edit-undo-symbolic",
					"Restore", "win.restore"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->trash_grp),
			   label_button("likeos-restore", "edit-undo-symbolic",
					"Restore all", "win.restore-all"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->trash_grp),
			   icon_button("likeos-delete-permanently", "edit-delete-symbolic",
				       "Delete permanently (Shift+Del)",
				       "win.delete"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), b->trash_grp, FALSE, FALSE, 0);

	b->sep2 = sep();
	gtk_box_pack_start(GTK_BOX(bar), b->sep2, FALSE, FALSE, 0);
	m = ex_context_menu_sort();
	b->sort = menu_button("likeos-sort", "view-sort-ascending-symbolic",
			      "_Sort", "Sort items", m);
	gtk_box_pack_start(GTK_BOX(bar), b->sort, FALSE, FALSE, 0);
	g_object_unref(m);
	m = ex_context_menu_view();
	b->view = menu_button("likeos-view", "view-list-symbolic", "_View",
			      "Layout and view options", m);
	gtk_box_pack_start(GTK_BOX(bar), b->view, FALSE, FALSE, 0);
	g_object_unref(m);
	m = filter_menu();
	b->filter = menu_button("likeos-filter", "view-filter-symbolic", "_Filter",
				"Show only one kind of file", m);
	gtk_box_pack_start(GTK_BOX(bar), b->filter, FALSE, FALSE, 0);
	g_object_unref(m);
	m = collection_menu();
	b->collection = menu_button("likeos-pictures", "folder-pictures",
				    "_Collection",
				    "The folders Gallery shows", m);
	gtk_box_pack_start(GTK_BOX(bar), b->collection, FALSE, FALSE, 0);
	g_object_unref(m);

	/* A picture selected: the picture commands, after a hairline -- with
	 * their words where the bar has room for them, as icons named by
	 * their tooltips where it has room for those, and not at all where
	 * it has neither (the right-click menu has all three). */
	full = group();
	gtk_box_pack_start(GTK_BOX(full), sep(), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(full),
			   label_button("likeos-wallpaper", "preferences-desktop-wallpaper",
					"Set as background", "win.set-wallpaper"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(full),
			   label_button("likeos-rotate-left", "object-rotate-left",
					"Rotate left", "win.rotate-left"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(full),
			   label_button("likeos-rotate-right", "object-rotate-right",
					"Rotate right", "win.rotate-right"),
			   FALSE, FALSE, 0);
	compact = group();
	gtk_box_pack_start(GTK_BOX(compact), sep(), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(compact),
			   icon_button("likeos-wallpaper", "preferences-desktop-wallpaper",
				       "Set as background", "win.set-wallpaper"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(compact),
			   icon_button("likeos-rotate-left", "object-rotate-left",
				       "Rotate left", "win.rotate-left"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(compact),
			   icon_button("likeos-rotate-right", "object-rotate-right",
				       "Rotate right", "win.rotate-right"),
			   FALSE, FALSE, 0);
	b->images = fit_bin_new(full, compact);
	gtk_box_pack_start(GTK_BOX(bar), b->images, FALSE, FALSE, 0);
	b->sep3 = sep();
	gtk_box_pack_start(GTK_BOX(bar), b->sep3, FALSE, FALSE, 0);

	m = ex_context_menu_more();
	more = menu_button("likeos-more", "view-more-symbolic", NULL, "See more",
			   m);
	g_object_unref(m);
	gtk_box_pack_start(GTK_BOX(bar), more, FALSE, FALSE, 0);

	/* At the far end, the two panes' switches side by side: choosing
	 * the pane already shown closes it. */
	details = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	ex_style_add(details, "ex-pane-switch");
	gtk_box_pack_start(GTK_BOX(details),
			   pane_toggle("likeos-details-pane", "view-dual-symbolic",
				       "Details pane (Alt+Shift+P)", "Details pane",
				       "win.right-pane::details"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(details),
			   pane_toggle("likeos-preview-pane", "view-paged-symbolic",
				       "Preview pane (Alt+P)", "Preview pane",
				       "win.right-pane::preview"),
			   FALSE, FALSE, 0);
	gtk_widget_set_valign(details, GTK_ALIGN_CENTER);
	gtk_box_pack_end(GTK_BOX(bar), details, FALSE, FALSE, 0);

	g_object_set_data_full(G_OBJECT(bar), "ex-bar", b, g_free);
	gtk_widget_show_all(bar);
	gtk_widget_hide(b->trash_grp);
	gtk_widget_hide(b->filter);
	gtk_widget_hide(b->collection);
	gtk_widget_hide(b->images);
	return bar;
}

void ex_commandbar_set_kind(GtkWidget *bar, ExLocationKind kind)
{
	Bar *b = g_object_get_data(G_OBJECT(bar), "ex-bar");
	gboolean folder = kind == EX_LOC_FOLDER;
	gboolean trash = kind == EX_LOC_TRASH;
	gboolean search = kind == EX_LOC_SEARCH;
	gboolean items = folder || search;
	gboolean gallery = kind == EX_LOC_GALLERY;

	b->kind = kind;

	/* Home and Computer keep the bar whole, with what does not apply
	 * there greyed; the Trash swaps the item commands for its own. */
	gtk_widget_set_visible(b->new_btn, !trash && !search);
	gtk_widget_set_sensitive(b->new_btn, folder);
	gtk_widget_set_visible(b->sep1, !trash && !search);
	gtk_widget_set_visible(b->clip, !trash);
	gtk_widget_set_visible(b->paste, !trash && !search);
	gtk_widget_set_visible(b->sep_edit, !trash);
	gtk_widget_set_visible(b->edit, !trash);
	gtk_widget_set_visible(b->trash_grp, trash);
	gtk_widget_set_visible(b->sep2, TRUE);
	/* Home and Computer are not item lists, and Gallery keeps its
	 * timeline: nothing to sort there. */
	gtk_widget_set_sensitive(b->sort, items || trash);
	gtk_widget_set_sensitive(b->view, !gallery);
	gtk_widget_set_visible(b->filter, kind == EX_LOC_HOME);
	gtk_widget_set_visible(b->collection, gallery);
	gtk_widget_set_visible(b->images, FALSE);
}

void ex_commandbar_set_images(GtkWidget *bar, gboolean images)
{
	Bar *b = g_object_get_data(G_OBJECT(bar), "ex-bar");
	gboolean on = images && b->kind != EX_LOC_TRASH;

	gtk_widget_set_visible(b->images, on);
}
