/* LikeOS installer -- page 5: the time zone.
 *
 * The first card page.  On the way from the Setup panel to the cards a
 * short "one moment" card is shown while the questions are prepared; then
 * the time zones the system knows (il_timezone_*) as large rows, with a
 * search field above them.  The language chosen earlier suggests a zone.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#define INTERLUDE_MS 1500

typedef struct {
	GtkWidget *stack;
	GtkWidget *interlude;
	GtkWidget *question;
	GtkWidget *list;
	GtkWidget *search;
	GtkWidget *clock;
	guint timer;
	guint clock_timer;
	gboolean interlude_shown;
} Region;

static const char *suggest_zone(App *app)
{
	if (g_str_has_prefix(app->cfg.locale, "en_GB"))
		return "Europe/London";
	if (g_str_has_prefix(app->cfg.locale, "en_US"))
		return "America/New_York";
	return "UTC";
}

/* The time now in the selected zone, from its standard offset. */
static gboolean update_clock(gpointer data)
{
	Page *p = data;
	Region *r = p->priv;
	int zone = il_timezone_find(p->app->cfg.timezone);
	GDateTime *now, *there;
	char *t, *when;

	now = g_date_time_new_now_utc();
	there = g_date_time_add_minutes(now, zone >= 0 ?
					il_timezone_offset_minutes(zone) : 0);
	when = g_date_time_format(there, "%H:%M");
	t = g_strdup_printf("Standard time there now: %s (summer time may add an "
			    "hour)", when);
	gtk_label_set_text(GTK_LABEL(r->clock), t);
	g_free(t);
	g_free(when);
	g_date_time_unref(there);
	g_date_time_unref(now);
	return G_SOURCE_CONTINUE;
}

static void on_changed(GtkWidget *list, const char *id, gpointer data)
{
	Page *p = data;

	(void)list;
	if (!id)
		return;
	g_strlcpy(p->app->cfg.timezone, id, sizeof(p->app->cfg.timezone));
	p->next_ok = TRUE;
	update_clock(p);
	page_update(p);
}

static void on_activated(GtkWidget *list, const char *id, gpointer data)
{
	(void)list;
	(void)id;
	(void)data;
	wizard_next(pages_wizard());
}

static GtkWidget *region_build(Page *p)
{
	Region *r = p->priv;
	GtkWidget *body, *box, *spin, *l;

	r->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(r->stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(r->stack), 300);

	/* the interlude */
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 22);
	ui_add_class(box, "il-interlude");
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	spin = ring_spinner_new(44);
	ui_add_class(spin, "il-accent-spinner");
	gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 0);
	l = ui_label("Just a moment while setup prepares a few questions about "
		     "you and this computer", "il-interlude-text");
	gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
	gtk_label_set_xalign(GTK_LABEL(l), 0.5);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 34);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	r->interlude = box;
	gtk_stack_add_named(GTK_STACK(r->stack), box, "interlude");

	/* the question */
	r->question = card_page_new("Which time zone are you in?",
				    "LikeOS shows the time of this zone. It is "
				    "kept in /etc/timezone, where you can change "
				    "it later.", &body);
	r->list = choice_list_new();
	for (int i = 0; i < il_timezone_count(); i++)
		choice_list_add(r->list, il_timezone_name(i), il_timezone_label(i),
				il_timezone_name(i));
	r->search = search_for_list(r->list, "Search for a city or region");
	gtk_box_pack_start(GTK_BOX(body), r->search, FALSE, FALSE, 0);
	gtk_scrolled_window_set_min_content_height(
		GTK_SCROLLED_WINDOW(choice_list_scroller(r->list)), 160);
	gtk_box_pack_start(GTK_BOX(body), choice_list_scroller(r->list), TRUE, TRUE, 0);
	r->clock = ui_label("", "il-caption");
	gtk_box_pack_start(GTK_BOX(body), r->clock, FALSE, FALSE, 0);
	choice_list_on_changed(r->list, on_changed, p);
	choice_list_on_activated(r->list, on_activated, p);
	gtk_stack_add_named(GTK_STACK(r->stack), r->question, "question");
	return r->stack;
}

static gboolean interlude_done(gpointer data)
{
	Page *p = data;
	Region *r = p->priv;

	r->timer = 0;
	r->interlude_shown = TRUE;
	gtk_stack_set_visible_child(GTK_STACK(r->stack), r->question);
	p->art = ART_GLOBE;
	p->next_visible = TRUE;
	p->back_ok = TRUE;
	page_set_focus(p, r->search);
	page_update(p);
	page_focus_later(r->search);
	return G_SOURCE_REMOVE;
}

static void region_enter(Page *p, int direction)
{
	Region *r = p->priv;

	if (!p->app->cfg.timezone[0] || g_str_equal(p->app->cfg.timezone, "UTC"))
		g_strlcpy(p->app->cfg.timezone, suggest_zone(p->app),
			  sizeof(p->app->cfg.timezone));
	choice_list_select(r->list, p->app->cfg.timezone);
	p->next_ok = choice_list_selected(r->list) != NULL;
	update_clock(p);
	if (!r->clock_timer)
		r->clock_timer = g_timeout_add_seconds(20, update_clock, p);

	if (!r->interlude_shown && direction > 0) {
		gtk_stack_set_transition_type(GTK_STACK(r->stack),
					      GTK_STACK_TRANSITION_TYPE_NONE);
		gtk_stack_set_visible_child(GTK_STACK(r->stack), r->interlude);
		gtk_stack_set_transition_type(GTK_STACK(r->stack),
					      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
		p->art = ART_SHAPES;
		p->next_visible = FALSE;
		p->back_ok = FALSE;
		r->timer = g_timeout_add(p->app->no_splash_delay ? 100 : INTERLUDE_MS,
					 interlude_done, p);
	} else {
		gtk_stack_set_visible_child(GTK_STACK(r->stack), r->question);
		p->art = ART_GLOBE;
		p->next_visible = TRUE;
		p->back_ok = TRUE;
		page_set_focus(p, r->search);
	}
	page_update(p);
}

static gboolean region_next(Page *p)
{
	app_log(p->app, IL_LOG_INFO, "ui: time zone %s", p->app->cfg.timezone);
	return TRUE;
}

static int region_autofill(Page *p, const struct il_answers *a)
{
	Region *r = p->priv;

	/* the "one moment" card first */
	if (!p->next_visible)
		return AUTOFILL_WAIT;
	if (!choice_list_choose(r->list, a->cfg.timezone)) {
		app_log(p->app, IL_LOG_WARN, "ui: autopilot: no time zone %s in "
			"the list", a->cfg.timezone);
		return AUTOFILL_FAIL;
	}
	return AUTOFILL_NEXT;
}

static void region_leave(Page *p)
{
	Region *r = p->priv;

	if (r->timer) {
		g_source_remove(r->timer);
		r->timer = 0;
		r->interlude_shown = TRUE;
	}
	if (r->clock_timer) {
		g_source_remove(r->clock_timer);
		r->clock_timer = 0;
	}
}

void page_region_init(Page *p)
{
	p->id = "region";
	p->scene = SCENE_CARD;
	p->build = region_build;
	p->enter = region_enter;
	p->leave = region_leave;
	p->go_next = region_next;
	p->autofill = region_autofill;
	p->art = ART_GLOBE;
	p->next_label = "_Yes";
	p->priv = g_new0(Region, 1);
}
