/* LikeOS installer -- page 2: the keyboard layout.
 *
 * All layouts of the system's catalogue, searchable.  The chosen one is
 * switched on at once (setxkbmap), and a test field below the list shows
 * what the keys type now -- every later answer, the passwords above all,
 * is typed with this layout.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#include <string.h>

typedef struct {
	GtkWidget *search;
	GtkWidget *list;
	GtkWidget *count;
	GtkWidget *test;
	gboolean quiet;
} Keyboard;

static void update_count(Page *p)
{
	Keyboard *k = p->priv;
	int n = choice_list_visible_count(k->list);
	char *t;

	if (n == keyboard_count(p->app))
		t = g_strdup_printf("%d layouts", n);
	else if (n == 0)
		t = g_strdup("No layout matches the search.");
	else
		t = g_strdup_printf("%d of %d layouts", n, keyboard_count(p->app));
	gtk_label_set_text(GTK_LABEL(k->count), t);
	g_free(t);
}

static void on_search(GtkSearchEntry *e, gpointer data)
{
	(void)e;
	update_count(data);
}

static void on_changed(GtkWidget *list, const char *id, gpointer data)
{
	Page *p = data;
	App *app = p->app;
	const struct kbd_layout *l = keyboard_find_key(app, id);

	(void)list;
	if (!l)
		return;
	g_strlcpy(app->cfg.layouts[0], l->layout, sizeof(app->cfg.layouts[0]));
	g_strlcpy(app->cfg.variants[0], l->variant, sizeof(app->cfg.variants[0]));
	g_object_set_data(G_OBJECT(app->window), "keyboard-chosen",
			  GINT_TO_POINTER(1));
	app_log(app, IL_LOG_DEBUG, "ui: trying keyboard layout %s", id);
	keyboard_apply_one(app, l->layout, l->variant);
	p->next_ok = TRUE;
	page_update(p);
}

static void on_activated(GtkWidget *list, const char *id, gpointer data)
{
	(void)list;
	(void)id;
	(void)data;
	wizard_next(pages_wizard());
}

static GtkWidget *keyboard_build(Page *p)
{
	Keyboard *k = p->priv;
	GtkWidget *body, *root, *bar, *test_row;
	int n = keyboard_count(p->app);

	root = setup_page_new("Choose your keyboard layout", &body);
	gtk_box_pack_start(GTK_BOX(body),
			   ui_label("Pick the layout printed on your keyboard. It "
				    "works right away, so you can try it in the "
				    "field below the list.", "il-text"),
			   FALSE, FALSE, 0);

	k->list = choice_list_new();
	ui_add_class(k->list, "il-choice-compact");
	for (int i = 0; i < n; i++) {
		const struct kbd_layout *l = keyboard_get(p->app, i);
		char *key = keyboard_key(l);

		choice_list_add(k->list, key, l->name,
				strstr(l->name, l->language) ? NULL : l->language);
		g_free(key);
	}

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	k->search = search_for_list(k->list, "Search, for example \"German\" or \"de\"");
	gtk_widget_set_hexpand(k->search, TRUE);
	g_signal_connect_after(k->search, "search-changed", G_CALLBACK(on_search), p);
	k->count = ui_label("", "il-caption");
	gtk_label_set_xalign(GTK_LABEL(k->count), 1);
	gtk_widget_set_valign(k->count, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(bar), k->search, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(bar), k->count, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), bar, FALSE, FALSE, 0);

	gtk_scrolled_window_set_min_content_height(
		GTK_SCROLLED_WINDOW(choice_list_scroller(k->list)), 150);
	ui_add_class(choice_list_scroller(k->list), "il-list-frame");
	gtk_box_pack_start(GTK_BOX(body), choice_list_scroller(k->list), TRUE, TRUE, 0);

	k->test = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(k->test),
				       "Type here to try the layout");
	ui_add_class(k->test, "il-entry");
	test_row = form_row("_Try it", k->test);
	gtk_box_pack_start(GTK_BOX(body), test_row, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(root), "form-rows", test_row);

	choice_list_on_changed(k->list, on_changed, p);
	choice_list_on_activated(k->list, on_activated, p);
	update_count(p);
	return root;
}

static void keyboard_enter(Page *p, int direction)
{
	Keyboard *k = p->priv;
	const struct kbd_layout *l = keyboard_find(p->app, p->app->cfg.layouts[0],
						   p->app->cfg.variants[0]);
	char *key = keyboard_key(l);

	(void)direction;
	choice_list_select(k->list, key);
	g_free(key);
	p->next_ok = choice_list_selected(k->list) != NULL;
	page_update(p);
	page_set_focus(p, k->search);
	if (l)
		keyboard_apply_one(p->app, l->layout, l->variant);
}

static gboolean keyboard_next(Page *p)
{
	App *app = p->app;

	app_log(app, IL_LOG_INFO, "ui: keyboard layout %s%s%s",
		app->cfg.layouts[0], app->cfg.variants[0][0] ? ":" : "",
		app->cfg.variants[0]);
	return TRUE;
}

static int keyboard_autofill(Page *p, const struct il_answers *a)
{
	Keyboard *k = p->priv;
	const struct kbd_layout *l = keyboard_find(p->app, a->cfg.layouts[0],
						   a->cfg.variants[0]);
	char *key;

	if (!l) {
		app_log(p->app, IL_LOG_WARN, "ui: autopilot: no keyboard layout "
			"%s in the list", a->cfg.layouts[0]);
		return AUTOFILL_FAIL;
	}
	key = keyboard_key(l);
	choice_list_choose(k->list, key);
	g_free(key);
	return AUTOFILL_NEXT;
}

static void keyboard_relayout(Page *p)
{
	GtkWidget *row = g_object_get_data(G_OBJECT(p->root), "form-rows");
	GtkWidget *parent = row ? gtk_widget_get_parent(row) : NULL;

	if (parent) {
		/* only the test row is a form row on this page */
		GtkWidget *l = g_object_get_data(G_OBJECT(row), "form-label");
		GtkWidget *c = g_object_get_data(G_OBJECT(row), "form-control");
		gboolean stacked = p->app->narrow;

		gtk_orientable_set_orientation(GTK_ORIENTABLE(row), stacked ?
			GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
		gtk_widget_set_size_request(l, stacked ? -1 : 120, -1);
		gtk_widget_set_size_request(c, -1, -1);
		gtk_widget_set_hexpand(c, TRUE);
		gtk_widget_set_halign(c, GTK_ALIGN_FILL);
		gtk_box_set_child_packing(GTK_BOX(row), c, TRUE, TRUE, 0,
					  GTK_PACK_START);
	}
}

void page_keyboard_init(Page *p)
{
	p->id = "keyboard";
	p->scene = SCENE_SETUP;
	p->build = keyboard_build;
	p->enter = keyboard_enter;
	p->relayout = keyboard_relayout;
	p->go_next = keyboard_next;
	p->autofill = keyboard_autofill;
	p->priv = g_new0(Keyboard, 1);
}
