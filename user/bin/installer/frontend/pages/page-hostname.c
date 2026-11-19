/* LikeOS installer -- page 7: the computer's name (its host name).
 *
 * Checked while it is typed with the backend's il_validate_hostname (the
 * rules of an internet host name label).  Upper-case letters are folded and
 * spaces become hyphens as they are typed, since that is nearly always
 * what was meant.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#include <string.h>

typedef struct {
	GtkWidget *entry;
	GtkWidget *field;
	GtkWidget *preview;
	gboolean quiet;
} Hostname;

static void validate(Page *p)
{
	Hostname *h = p->priv;
	const char *name = gtk_entry_get_text(GTK_ENTRY(h->entry));
	char why[256];
	int rc = il_validate_hostname(name, why, sizeof(why));

	if (rc == 0) {
		char *t = g_strdup_printf("This computer will be called "
					  "\xe2\x80\x9c%s\xe2\x80\x9d.", name);
		char *pv = g_strdup_printf("%s login:", name);

		field_set_ok(h->field, t);
		gtk_label_set_text(GTK_LABEL(h->preview), pv);
		g_strlcpy(p->app->cfg.hostname, name, sizeof(p->app->cfg.hostname));
		g_free(t);
		g_free(pv);
	} else if (!*name) {
		field_set_hint(h->field, "Enter a name for this computer.");
		gtk_label_set_text(GTK_LABEL(h->preview), "login:");
	} else {
		char *t = app_sentence(why);

		field_set_error(h->field, t);
		g_free(t);
	}
	p->next_ok = rc == 0;
	page_update(p);
}

static void on_changed(GtkEditable *e, gpointer data)
{
	(void)e;
	validate(data);
}

/* Folds what is typed into what a host name may hold. */
static void on_insert(GtkEditable *e, const char *text, int len, int *pos,
		      gpointer data)
{
	Page *p = data;
	Hostname *h = p->priv;
	GString *out;
	gboolean changed = FALSE;

	if (h->quiet)
		return;
	if (len < 0)
		len = (int)strlen(text);
	out = g_string_sized_new((gsize)len);
	for (int i = 0; i < len; i++) {
		char c = text[i];

		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
			changed = TRUE;
		} else if (c == ' ' || c == '_' || c == '.') {
			c = '-';
			changed = TRUE;
		}
		g_string_append_c(out, c);
	}
	if (changed) {
		h->quiet = TRUE;
		g_signal_stop_emission_by_name(e, "insert-text");
		gtk_editable_insert_text(e, out->str, (int)out->len, pos);
		h->quiet = FALSE;
	}
	g_string_free(out, TRUE);
}

static void on_suggest(GtkButton *b, gpointer data)
{
	Page *p = data;
	Hostname *h = p->priv;
	char name[IL_HOSTNAME_MAX + 1];

	(void)b;
	il_suggest_hostname(name, sizeof(name));
	gtk_entry_set_text(GTK_ENTRY(h->entry), name);
	gtk_widget_grab_focus(h->entry);
	gtk_editable_set_position(GTK_EDITABLE(h->entry), -1);
}

static GtkWidget *hostname_build(Page *p)
{
	Hostname *h = p->priv;
	GtkWidget *body, *root, *rules, *suggest, *term;

	root = card_page_new("Name this computer",
			     "Give it a name you will recognize. It is shown at "
			     "the login prompt and in the prompt of every "
			     "terminal.", &body);

	h->entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(h->entry), IL_HOSTNAME_MAX);
	gtk_entry_set_activates_default(GTK_ENTRY(h->entry), TRUE);
	gtk_entry_set_placeholder_text(GTK_ENTRY(h->entry), "Computer name");
	gtk_entry_set_input_hints(GTK_ENTRY(h->entry), GTK_INPUT_HINT_LOWERCASE |
				  GTK_INPUT_HINT_NO_SPELLCHECK);
	ui_add_class(h->entry, "il-entry");
	ui_add_class(h->entry, "il-entry-large");
	h->field = field_new("Computer _name (host name)", h->entry);
	gtk_box_pack_start(GTK_BOX(body), h->field, FALSE, FALSE, 0);

	rules = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	ui_add_class(rules, "il-rules");
	gtk_box_pack_start(GTK_BOX(rules),
			   ui_label("\xe2\x80\xa2 Lower-case letters a\xe2\x80\x93z, digits "
				    "0\xe2\x80\x93" "9 and hyphens ( - )", "il-rule"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(rules),
			   ui_label("\xe2\x80\xa2 1 to 63 characters, not starting or ending with "
				    "a hyphen", "il-rule"), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(rules),
			   ui_label("\xe2\x80\xa2 Not only digits; no spaces", "il-rule"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), rules, FALSE, FALSE, 0);

	suggest = link_button_new("_Suggest a name", ICON_REFRESH);
	g_signal_connect(suggest, "clicked", G_CALLBACK(on_suggest), p);
	gtk_box_pack_start(GTK_BOX(body), suggest, FALSE, FALSE, 0);

	/* how it will look at the text console */
	term = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	ui_add_class(term, "il-terminal");
	gtk_box_pack_start(GTK_BOX(term), ui_label("LikeOS", "il-terminal-text"),
			   FALSE, FALSE, 0);
	h->preview = ui_label("login:", "il-terminal-text");
	gtk_box_pack_start(GTK_BOX(term), h->preview, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), term, FALSE, FALSE, 0);

	g_signal_connect(h->entry, "insert-text", G_CALLBACK(on_insert), p);
	g_signal_connect(h->entry, "changed", G_CALLBACK(on_changed), p);
	gtk_entry_set_text(GTK_ENTRY(h->entry), p->app->cfg.hostname);
	return root;
}

static gboolean hostname_next(Page *p)
{
	app_log(p->app, IL_LOG_INFO, "ui: computer name %s", p->app->cfg.hostname);
	return TRUE;
}

static int hostname_autofill(Page *p, const struct il_answers *a)
{
	Hostname *h = p->priv;

	if (a->cfg.hostname[0])
		gtk_entry_set_text(GTK_ENTRY(h->entry), a->cfg.hostname);
	return AUTOFILL_NEXT;
}

static void hostname_enter(Page *p, int direction)
{
	Hostname *h = p->priv;

	(void)direction;
	validate(p);
	page_set_focus(p, h->entry);
}

void page_hostname_init(Page *p)
{
	p->id = "hostname";
	p->scene = SCENE_CARD;
	p->build = hostname_build;
	p->enter = hostname_enter;
	p->go_next = hostname_next;
	p->autofill = hostname_autofill;
	p->art = ART_COMPUTER;
	p->priv = g_new0(Hostname, 1);
}
