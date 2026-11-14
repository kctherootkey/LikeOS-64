/* LikeOS installer -- page 6: keyboard layouts of the new system.
 *
 * Three steps on one card:
 *  1. "Is this the right keyboard layout?" -- the layout from the Setup
 *     panel, preselected in the list of all layouts;
 *  2. "Want to add a second keyboard layout?" -- the layouts chosen so far,
 *     Skip (or Next) and Add layout;
 *  3. "Choose a layout to add" -- the list again, with a field to try it.
 * Up to four layouts (what XKB switches between); Alt+Shift switches.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#include <string.h>

enum { STEP_CONFIRM, STEP_SECOND, STEP_ADD };

typedef struct {
	GtkWidget *stack;
	GtkWidget *steps[3];
	GtkWidget *confirm_list;
	GtkWidget *add_list;
	GtkWidget *add_search;
	GtkWidget *chosen_box;
	GtkWidget *switch_hint;
	GtkWidget *test;
	int step;
	int auto_adds;           /* layouts the autopilot added */
} Layout;

static void show_step(Page *p, int step);

static GtkWidget *layout_list(Page *p)
{
	GtkWidget *list = choice_list_new();
	int n = keyboard_count(p->app);

	for (int i = 0; i < n; i++) {
		const struct kbd_layout *l = keyboard_get(p->app, i);
		char *key = keyboard_key(l);

		choice_list_add(list, key, l->name,
				strstr(l->name, l->language) ? NULL : l->language);
		g_free(key);
	}
	return list;
}

static void on_confirm_changed(GtkWidget *list, const char *id, gpointer data)
{
	Page *p = data;
	App *app = p->app;
	const struct kbd_layout *l = keyboard_find_key(app, id);

	(void)list;
	if (!l)
		return;
	g_strlcpy(app->cfg.layouts[0], l->layout, sizeof(app->cfg.layouts[0]));
	g_strlcpy(app->cfg.variants[0], l->variant, sizeof(app->cfg.variants[0]));
	/* a second layout equal to the new first one goes */
	for (int i = 1; i < app->cfg.nlayouts; i++) {
		if (strcmp(app->cfg.layouts[i], l->layout) == 0 &&
		    strcmp(app->cfg.variants[i], l->variant) == 0) {
			for (int j = i; j + 1 < app->cfg.nlayouts; j++) {
				memcpy(app->cfg.layouts[j], app->cfg.layouts[j + 1],
				       sizeof(app->cfg.layouts[j]));
				memcpy(app->cfg.variants[j], app->cfg.variants[j + 1],
				       sizeof(app->cfg.variants[j]));
			}
			app->cfg.nlayouts--;
			break;
		}
	}
	keyboard_apply(app);
	p->next_ok = TRUE;
	page_update(p);
}

static void on_add_changed(GtkWidget *list, const char *id, gpointer data)
{
	Page *p = data;
	const struct kbd_layout *l = keyboard_find_key(p->app, id);

	(void)list;
	if (l)
		keyboard_apply_one(p->app, l->layout, l->variant);
	p->next_ok = l != NULL;
	page_update(p);
}

static void on_activated(GtkWidget *list, const char *id, gpointer data)
{
	(void)list;
	(void)id;
	(void)data;
	wizard_next(pages_wizard());
}

static void on_remove(GtkButton *b, gpointer data)
{
	Page *p = data;
	App *app = p->app;
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "layout-index"));

	if (i <= 0 || i >= app->cfg.nlayouts)
		return;
	for (int j = i; j + 1 < app->cfg.nlayouts; j++) {
		memcpy(app->cfg.layouts[j], app->cfg.layouts[j + 1],
		       sizeof(app->cfg.layouts[j]));
		memcpy(app->cfg.variants[j], app->cfg.variants[j + 1],
		       sizeof(app->cfg.variants[j]));
	}
	app->cfg.nlayouts--;
	app->cfg.layouts[app->cfg.nlayouts][0] = '\0';
	app->cfg.variants[app->cfg.nlayouts][0] = '\0';
	keyboard_apply(app);
	show_step(p, STEP_SECOND);
}

static void fill_chosen(Page *p)
{
	Layout *ly = p->priv;
	App *app = p->app;
	GList *kids = gtk_container_get_children(GTK_CONTAINER(ly->chosen_box));

	for (GList *k = kids; k; k = k->next)
		gtk_widget_destroy(k->data);
	g_list_free(kids);

	for (int i = 0; i < app->cfg.nlayouts; i++) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		char *name = app_layout_label(app, app->cfg.layouts[i],
					      app->cfg.variants[i]);
		char *code = keyboard_short_code(app->cfg.layouts[i]);
		GtkWidget *badge = gtk_label_new(code);

		ui_add_class(row, "il-layout-row");
		ui_add_class(badge, "il-layout-badge");
		gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(row), badge, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(texts), ui_label(name, "il-layout-name"),
				   FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(texts),
				   ui_label(i == 0 ? "Main layout, also used on the "
					    "text console" : "Additional layout",
					    "il-caption"), FALSE, FALSE, 0);
		gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(row), texts, TRUE, TRUE, 0);
		if (i > 0) {
			GtkWidget *rm = gtk_button_new();

			gtk_container_add(GTK_CONTAINER(rm), icon_new(ICON_CLOSE, 14));
			gtk_button_set_relief(GTK_BUTTON(rm), GTK_RELIEF_NONE);
			gtk_widget_set_tooltip_text(rm, "Remove this layout");
			gtk_widget_set_valign(rm, GTK_ALIGN_CENTER);
			ui_add_class(rm, "il-icon-button");
			g_object_set_data(G_OBJECT(rm), "layout-index", GINT_TO_POINTER(i));
			g_signal_connect(rm, "clicked", G_CALLBACK(on_remove), p);
			gtk_box_pack_end(GTK_BOX(row), rm, FALSE, FALSE, 0);
		}
		gtk_widget_show_all(row);
		gtk_box_pack_start(GTK_BOX(ly->chosen_box), row, FALSE, FALSE, 0);
		g_free(name);
		g_free(code);
	}
	gtk_widget_set_visible(ly->switch_hint, app->cfg.nlayouts > 1);
}

static void show_step(Page *p, int step)
{
	Layout *ly = p->priv;
	App *app = p->app;

	ly->step = step;
	p->substep = step;
	gtk_stack_set_visible_child(GTK_STACK(ly->stack), ly->steps[step]);
	switch (step) {
	case STEP_CONFIRM: {
		const struct kbd_layout *l = keyboard_find(app, app->cfg.layouts[0],
							   app->cfg.variants[0]);
		char *key = keyboard_key(l);

		choice_list_select(ly->confirm_list, key);
		g_free(key);
		p->art = ART_KEYBOARD;
		p->next_label = "_Yes";
		p->secondary_label = NULL;
		p->next_ok = choice_list_selected(ly->confirm_list) != NULL;
		page_set_focus(p, choice_list_focus_target(ly->confirm_list));
		page_focus_later(choice_list_focus_target(ly->confirm_list));
		break;
	}
	case STEP_SECOND:
		fill_chosen(p);
		p->art = ART_KEYBOARD_PLUS;
		p->next_label = app->cfg.nlayouts > 1 ? "_Next" : "_Skip";
		p->secondary_label = app->cfg.nlayouts < IL_MAX_LAYOUTS ?
			"_Add layout" : NULL;
		p->next_ok = TRUE;
		page_set_focus(p, NULL);
		break;
	case STEP_ADD:
		gtk_entry_set_text(GTK_ENTRY(ly->add_search), "");
		choice_list_select(ly->add_list, NULL);
		p->art = ART_KEYBOARD;
		p->next_label = "_Add layout";
		p->secondary_label = NULL;
		p->next_ok = FALSE;
		page_set_focus(p, ly->add_search);
		page_focus_later(ly->add_search);
		break;
	}
	page_update(p);
}

static void layout_secondary(Page *p)
{
	Layout *ly = p->priv;

	if (ly->step == STEP_SECOND && p->app->cfg.nlayouts < IL_MAX_LAYOUTS)
		show_step(p, STEP_ADD);
}

static gboolean layout_next(Page *p)
{
	Layout *ly = p->priv;
	App *app = p->app;

	switch (ly->step) {
	case STEP_CONFIRM:
		show_step(p, STEP_SECOND);
		return FALSE;
	case STEP_ADD: {
		const struct kbd_layout *l =
			keyboard_find_key(app, choice_list_selected(ly->add_list));

		if (l && app->cfg.nlayouts < IL_MAX_LAYOUTS &&
		    !(strcmp(l->layout, app->cfg.layouts[0]) == 0 &&
		      strcmp(l->variant, app->cfg.variants[0]) == 0)) {
			gboolean dup = FALSE;

			for (int i = 0; i < app->cfg.nlayouts; i++)
				if (strcmp(app->cfg.layouts[i], l->layout) == 0 &&
				    strcmp(app->cfg.variants[i], l->variant) == 0)
					dup = TRUE;
			if (!dup) {
				int n = app->cfg.nlayouts++;

				g_strlcpy(app->cfg.layouts[n], l->layout,
					  sizeof(app->cfg.layouts[n]));
				g_strlcpy(app->cfg.variants[n], l->variant,
					  sizeof(app->cfg.variants[n]));
			}
		}
		keyboard_apply(app);
		show_step(p, STEP_SECOND);
		return FALSE;
	}
	default: {
		char *list = app_layouts_summary(app);
		GString *keys = g_string_new(NULL);

		app_finish_keyboard(app);
		for (int i = 0; i < app->cfg.nlayouts; i++)
			g_string_append_printf(keys, "%s%s%s%s", i ? "," : "",
					       app->cfg.layouts[i],
					       app->cfg.variants[i][0] ? ":" : "",
					       app->cfg.variants[i]);
		app_log(app, IL_LOG_INFO, "ui: keyboard layouts of the new "
			"system: %s (%s)%s%s", keys->str, list,
			app->cfg.xkb_options[0] ? ", options " : "",
			app->cfg.xkb_options);
		g_string_free(keys, TRUE);
		g_free(list);
		keyboard_apply(app);
		return TRUE;
	}
	}
}

static gboolean layout_back(Page *p)
{
	Layout *ly = p->priv;

	if (ly->step == STEP_ADD) {
		keyboard_apply(p->app);
		show_step(p, STEP_SECOND);
		return TRUE;
	}
	if (ly->step == STEP_SECOND) {
		show_step(p, STEP_CONFIRM);
		return TRUE;
	}
	return FALSE;
}

static int layout_autofill(Page *p, const struct il_answers *a)
{
	Layout *ly = p->priv;
	App *app = p->app;
	const struct kbd_layout *l;
	char *key;
	int i;

	switch (ly->step) {
	case STEP_CONFIRM:
		i = 0;
		break;
	case STEP_SECOND:
		if (app->cfg.nlayouts < a->cfg.nlayouts &&
		    app->cfg.nlayouts < IL_MAX_LAYOUTS) {
			if (ly->auto_adds++ >= IL_MAX_LAYOUTS)
				return AUTOFILL_FAIL;
			layout_secondary(p);
			return AUTOFILL_DONE;
		}
		return AUTOFILL_NEXT;
	default:
		i = app->cfg.nlayouts;
		break;
	}
	if (i >= a->cfg.nlayouts)
		return AUTOFILL_FAIL;
	l = keyboard_find(app, a->cfg.layouts[i], a->cfg.variants[i]);
	if (!l) {
		app_log(app, IL_LOG_WARN, "ui: autopilot: no keyboard layout "
			"%s%s%s in the list", a->cfg.layouts[i],
			a->cfg.variants[i][0] ? ":" : "", a->cfg.variants[i]);
		return AUTOFILL_FAIL;
	}
	key = keyboard_key(l);
	choice_list_choose(ly->step == STEP_CONFIRM ? ly->confirm_list :
			   ly->add_list, key);
	g_free(key);
	return AUTOFILL_NEXT;
}

static GtkWidget *layout_build(Page *p)
{
	Layout *ly = p->priv;
	GtkWidget *body, *search, *test_field;

	ly->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(ly->stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(ly->stack), 220);

	ly->steps[STEP_CONFIRM] = card_page_new(
		"Is this the right keyboard layout?",
		"It is the one you chose at the start. If you also use another "
		"layout, you can add it next.", &body);
	ly->confirm_list = layout_list(p);
	search = search_for_list(ly->confirm_list, "Search layouts");
	gtk_box_pack_start(GTK_BOX(body), search, FALSE, FALSE, 0);
	gtk_scrolled_window_set_min_content_height(
		GTK_SCROLLED_WINDOW(choice_list_scroller(ly->confirm_list)), 160);
	gtk_box_pack_start(GTK_BOX(body), choice_list_scroller(ly->confirm_list),
			   TRUE, TRUE, 0);
	choice_list_on_changed(ly->confirm_list, on_confirm_changed, p);
	choice_list_on_activated(ly->confirm_list, on_activated, p);
	gtk_stack_add_named(GTK_STACK(ly->stack), ly->steps[STEP_CONFIRM], "confirm");

	ly->steps[STEP_SECOND] = card_page_new(
		"Want to add a second keyboard layout?",
		"Add another layout if you write in more than one language or "
		"share this computer.", &body);
	ly->chosen_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	ui_add_class(ly->chosen_box, "il-layout-list");
	gtk_box_pack_start(GTK_BOX(body), ly->chosen_box, FALSE, FALSE, 0);
	ly->switch_hint = ui_check_row("Press <b>Alt+Shift</b> to switch between "
				       "your layouts.", ICON_KEYBOARD);
	gtk_widget_set_no_show_all(ly->switch_hint, TRUE);
	gtk_widget_show_all(ly->switch_hint);
	gtk_widget_hide(ly->switch_hint);
	gtk_box_pack_start(GTK_BOX(body), ly->switch_hint, FALSE, FALSE, 0);
	gtk_stack_add_named(GTK_STACK(ly->stack), ly->steps[STEP_SECOND], "second");

	ly->steps[STEP_ADD] = card_page_new("Choose a layout to add",
					    "It is switched on while you look, so "
					    "you can try it below.", &body);
	ly->add_list = layout_list(p);
	ly->add_search = search_for_list(ly->add_list, "Search layouts");
	gtk_box_pack_start(GTK_BOX(body), ly->add_search, FALSE, FALSE, 0);
	gtk_scrolled_window_set_min_content_height(
		GTK_SCROLLED_WINDOW(choice_list_scroller(ly->add_list)), 140);
	gtk_box_pack_start(GTK_BOX(body), choice_list_scroller(ly->add_list),
			   TRUE, TRUE, 0);
	ly->test = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(ly->test), "Type here to try it");
	ui_add_class(ly->test, "il-entry");
	test_field = field_new(NULL, ly->test);
	gtk_box_pack_start(GTK_BOX(body), test_field, FALSE, FALSE, 0);
	choice_list_on_changed(ly->add_list, on_add_changed, p);
	choice_list_on_activated(ly->add_list, on_activated, p);
	gtk_stack_add_named(GTK_STACK(ly->stack), ly->steps[STEP_ADD], "add");
	return ly->stack;
}

static void layout_enter(Page *p, int direction)
{
	Layout *ly = p->priv;

	/* coming back from a later page: the list of layouts */
	if (direction < 0)
		show_step(p, STEP_SECOND);
	else
		show_step(p, ly->step == STEP_ADD ? STEP_SECOND :
			  direction > 0 ? STEP_CONFIRM : ly->step);
}

void page_layout_init(Page *p)
{
	p->id = "layout";
	p->scene = SCENE_CARD;
	p->build = layout_build;
	p->enter = layout_enter;
	p->go_next = layout_next;
	p->go_back = layout_back;
	p->secondary = layout_secondary;
	p->autofill = layout_autofill;
	p->art = ART_KEYBOARD;
	p->priv = g_new0(Layout, 1);
}
