/* LikeOS installer -- page 1: language and formats.
 *
 * The language of the installed system and the way it shows dates, times
 * and numbers.  The lists come from the backend (il_locale_*), so a new
 * language needs no change here.  Picking a language also picks the
 * matching formats and keyboard, until the person chooses those
 * themselves.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

typedef struct {
	GtkWidget *rows;
	GtkWidget *language;
	GtkWidget *formats;
	GtkWidget *example;
	gboolean formats_touched;
	gboolean quiet;
} Language;

static GtkWidget *locale_combo(void)
{
	GtkWidget *c = gtk_combo_box_text_new();

	for (int i = 0; i < il_locale_count(); i++)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), il_locale_name(i),
					  il_locale_label(i));
	ui_add_class(c, "il-combo");
	return c;
}

/* "Friday, 13 November 2026, 14:05" in the chosen style -- with English
 * names, whatever language the program itself runs in. */
static void update_example(Language *l, const char *locale)
{
	static const char *const days[] = {
		"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
		"Sunday",
	};
	static const char *const months[] = {
		"January", "February", "March", "April", "May", "June", "July",
		"August", "September", "October", "November", "December",
	};
	GDateTime *now = g_date_time_new_now_local();
	gboolean us = !locale || g_str_has_prefix(locale, "en_US");
	int wd = g_date_time_get_day_of_week(now) - 1;
	int mo = g_date_time_get_month(now) - 1;
	int d = g_date_time_get_day_of_month(now), y = g_date_time_get_year(now);
	int h = g_date_time_get_hour(now), m = g_date_time_get_minute(now);
	char *text;

	if (us)
		text = g_strdup_printf("Example: %s, %s %d, %d \xc2\xb7 %d:%02d %s "
				       "\xc2\xb7 1,234.56", days[wd], months[mo], d, y,
				       h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
	else
		text = g_strdup_printf("Example: %s %d %s %d \xc2\xb7 %02d:%02d "
				       "\xc2\xb7 1,234.56", days[wd], d, months[mo], y,
				       h, m);
	gtk_label_set_text(GTK_LABEL(l->example), text);
	g_free(text);
	g_date_time_unref(now);
}

static void on_language(GtkComboBox *c, gpointer data)
{
	Page *p = data;
	Language *l = p->priv;
	App *app = p->app;
	const char *id = gtk_combo_box_get_active_id(c);

	if (l->quiet || !id)
		return;
	g_strlcpy(app->cfg.locale, id, sizeof(app->cfg.locale));
	if (!l->formats_touched) {
		l->quiet = TRUE;
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(l->formats), id);
		l->quiet = FALSE;
		g_strlcpy(app->cfg.time_format, id, sizeof(app->cfg.time_format));
		update_example(l, id);
	}
	/* the keyboard follows the language until it was chosen */
	if (!g_object_get_data(G_OBJECT(app->window), "keyboard-chosen")) {
		g_strlcpy(app->cfg.layouts[0], keyboard_default_for_locale(id),
			  sizeof(app->cfg.layouts[0]));
		app->cfg.variants[0][0] = '\0';
	}
}

static void on_formats(GtkComboBox *c, gpointer data)
{
	Page *p = data;
	Language *l = p->priv;
	const char *id = gtk_combo_box_get_active_id(c);

	if (l->quiet || !id)
		return;
	l->formats_touched = TRUE;
	g_strlcpy(p->app->cfg.time_format, id, sizeof(p->app->cfg.time_format));
	update_example(l, id);
}

static GtkWidget *language_build(Page *p)
{
	Language *l = p->priv;
	GtkWidget *body, *root, *intro, *note;

	root = setup_page_new("Choose your language and formats", &body);

	intro = ui_label("LikeOS Setup guides you through a few questions and "
			 "then installs LikeOS in one go. Start with the language "
			 "and the way dates and numbers are written.", "il-text");
	gtk_box_pack_start(GTK_BOX(body), intro, FALSE, FALSE, 0);

	l->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
	ui_add_class(l->rows, "il-form");
	l->language = locale_combo();
	l->formats = locale_combo();
	gtk_box_pack_start(GTK_BOX(l->rows), form_row("_Language to install",
						      l->language), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(l->rows), form_row("_Time and date format",
						      l->formats), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), l->rows, FALSE, FALSE, 0);

	l->example = ui_label("", "il-caption");
	gtk_box_pack_start(GTK_BOX(body), l->example, FALSE, FALSE, 0);

	note = ui_check_row("Setup itself is shown in English. Your keyboard "
			    "layout comes next.", ICON_INFO);
	ui_add_class(note, "il-muted-row");
	gtk_box_pack_end(GTK_BOX(body), note, FALSE, FALSE, 0);

	l->quiet = TRUE;
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(l->language), p->app->cfg.locale);
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(l->formats),
				    p->app->cfg.time_format);
	if (gtk_combo_box_get_active(GTK_COMBO_BOX(l->language)) < 0)
		gtk_combo_box_set_active(GTK_COMBO_BOX(l->language), 0);
	if (gtk_combo_box_get_active(GTK_COMBO_BOX(l->formats)) < 0)
		gtk_combo_box_set_active(GTK_COMBO_BOX(l->formats), 0);
	l->quiet = FALSE;
	update_example(l, p->app->cfg.time_format);

	g_signal_connect(l->language, "changed", G_CALLBACK(on_language), p);
	g_signal_connect(l->formats, "changed", G_CALLBACK(on_formats), p);
	return root;
}

static void language_enter(Page *p, int direction)
{
	Language *l = p->priv;

	(void)direction;
	page_set_focus(p, l->language);
}

static gboolean language_next(Page *p)
{
	app_log(p->app, IL_LOG_INFO, "ui: language %s, formats %s",
		p->app->cfg.locale, p->app->cfg.time_format);
	return TRUE;
}

static int language_autofill(Page *p, const struct il_answers *a)
{
	Language *l = p->priv;

	if (a->cfg.locale[0])
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(l->language),
					    a->cfg.locale);
	if (a->cfg.time_format[0])
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(l->formats),
					    a->cfg.time_format);
	return AUTOFILL_NEXT;
}

static void language_relayout(Page *p)
{
	Language *l = p->priv;

	form_relayout(l->rows, p->app->narrow);
}

void page_language_init(Page *p)
{
	p->id = "language";
	p->scene = SCENE_SETUP;
	p->build = language_build;
	p->enter = language_enter;
	p->relayout = language_relayout;
	p->go_next = language_next;
	p->autofill = language_autofill;
	p->priv = g_new0(Language, 1);
}
