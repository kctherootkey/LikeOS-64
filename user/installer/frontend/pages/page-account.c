/* LikeOS installer -- page 9: your own account.
 *
 * Two steps: who (full name and user name -- the user name follows the
 * full name until it is edited by hand), then the account's password.  The
 * account joins the groups for sound and graphics acceleration unless the
 * box is cleared.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"
#include "password-pair.h"

#include <string.h>

enum { STEP_WHO, STEP_PASSWORD };

typedef struct {
	GtkWidget *stack;
	GtkWidget *steps[2];
	GtkWidget *fullname, *fullname_field;
	GtkWidget *username, *username_field;
	GtkWidget *groups;
	GtkWidget *same_as_root;
	PwPair *pair;
	gboolean user_edited;   /* the user name was typed by hand */
	gboolean quiet;
	int step;
} Account;

static void check_who(Page *p)
{
	Account *a = p->priv;
	const char *user = gtk_entry_get_text(GTK_ENTRY(a->username));
	const char *full = gtk_entry_get_text(GTK_ENTRY(a->fullname));
	char why[256];
	int rc = il_validate_username(user, why, sizeof(why));

	if (!*user)
		field_set_hint(a->username_field, "Lower-case letters, digits, "
			       "'_' and '-'; up to 32 characters. You sign in "
			       "with it.");
	else if (rc != 0) {
		char *t = app_sentence(why);

		field_set_error(a->username_field, t);
		g_free(t);
	} else {
		char *t = g_strdup_printf("Your home folder will be /home/%s.", user);

		field_set_ok(a->username_field, t);
		g_free(t);
	}
	if (strchr(full, ':'))
		field_set_error(a->fullname_field, "A name cannot contain a colon.");
	else
		field_set_hint(a->fullname_field, "Used as your display name in "
			       "the desktop and in e-mail programs. You may "
			       "leave it empty.");
	p->next_ok = rc == 0 && !strchr(full, ':');
	page_update(p);
}

static void on_fullname(GtkEditable *e, gpointer data)
{
	Page *p = data;
	Account *a = p->priv;
	char sug[IL_USERNAME_MAX + 1];

	(void)e;
	if (!a->user_edited) {
		il_suggest_username(gtk_entry_get_text(GTK_ENTRY(a->fullname)), sug,
				    sizeof(sug));
		a->quiet = TRUE;
		gtk_entry_set_text(GTK_ENTRY(a->username), sug);
		a->quiet = FALSE;
	}
	check_who(p);
}

static void on_username(GtkEditable *e, gpointer data)
{
	Page *p = data;
	Account *a = p->priv;

	(void)e;
	if (!a->quiet)
		a->user_edited = *gtk_entry_get_text(GTK_ENTRY(a->username)) != '\0';
	check_who(p);
}

static void on_pair_changed(gpointer data)
{
	Page *p = data;
	Account *a = p->priv;

	if (a->step == STEP_PASSWORD) {
		p->next_ok = pw_pair_valid(a->pair);
		page_update(p);
	}
}

static void on_same_as_root(GtkToggleButton *t, gpointer data)
{
	Page *p = data;
	Account *a = p->priv;

	if (gtk_toggle_button_get_active(t))
		pw_pair_set(a->pair, p->app->cfg.root_password);
	else
		pw_pair_set(a->pair, "");
}

static void show_step(Page *p, int step)
{
	Account *a = p->priv;

	a->step = step;
	p->substep = step;
	gtk_stack_set_visible_child(GTK_STACK(a->stack), a->steps[step]);
	if (step == STEP_WHO) {
		p->art = ART_AVATAR;
		check_who(p);
		page_focus_later(a->fullname);
	} else {
		const char *full = gtk_entry_get_text(GTK_ENTRY(a->fullname));
		const char *user = gtk_entry_get_text(GTK_ENTRY(a->username));
		char *h = g_strdup_printf("Create a password for %s",
					  *full ? full : user);

		page_set_heading(a->steps[STEP_PASSWORD], h,
				 "You type it to sign in at the login prompt.");
		g_free(h);
		/* root's password may have changed since the box was ticked */
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->same_as_root)) &&
		    strcmp(pw_pair_text(a->pair), p->app->cfg.root_password) != 0)
			pw_pair_set(a->pair, p->app->cfg.root_password);
		p->art = ART_AVATAR_KEY;
		p->next_ok = pw_pair_valid(a->pair);
		page_update(p);
		page_focus_later(pw_pair_first_entry(a->pair));
	}
}

static gboolean account_next(Page *p)
{
	Account *a = p->priv;
	App *app = p->app;

	if (a->step == STEP_WHO) {
		show_step(p, STEP_PASSWORD);
		return FALSE;
	}
	if (!pw_pair_valid(a->pair))
		return FALSE;
	g_strlcpy(app->cfg.user.fullname, gtk_entry_get_text(GTK_ENTRY(a->fullname)),
		  sizeof(app->cfg.user.fullname));
	g_strlcpy(app->cfg.user.username, gtk_entry_get_text(GTK_ENTRY(a->username)),
		  sizeof(app->cfg.user.username));
	g_strlcpy(app->cfg.user.password, pw_pair_text(a->pair),
		  sizeof(app->cfg.user.password));
	app->cfg.user_extra_groups =
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->groups));
	app->cfg.user_password_hash[0] = '\0';
	app_log(app, IL_LOG_INFO, "ui: account %s%s%s%s, %s the groups audio, "
		"render and users", app->cfg.user.username,
		app->cfg.user.fullname[0] ? " (" : "", app->cfg.user.fullname,
		app->cfg.user.fullname[0] ? ")" : "",
		app->cfg.user_extra_groups ? "in" : "not in");
	return TRUE;
}

static int account_autofill(Page *p, const struct il_answers *a)
{
	Account *ac = p->priv;
	App *app = p->app;

	if (ac->step == STEP_WHO) {
		gtk_entry_set_text(GTK_ENTRY(ac->fullname), a->cfg.user.fullname);
		gtk_entry_set_text(GTK_ENTRY(ac->username), a->cfg.user.username);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ac->groups),
					     a->cfg.user_extra_groups);
		return AUTOFILL_NEXT;
	}
	if (!a->cfg.user.password[0] && a->cfg.user_password_hash[0]) {
		g_strlcpy(app->cfg.user.fullname,
			  gtk_entry_get_text(GTK_ENTRY(ac->fullname)),
			  sizeof(app->cfg.user.fullname));
		g_strlcpy(app->cfg.user.username,
			  gtk_entry_get_text(GTK_ENTRY(ac->username)),
			  sizeof(app->cfg.user.username));
		app->cfg.user.password[0] = '\0';
		g_strlcpy(app->cfg.user_password_hash, a->cfg.user_password_hash,
			  sizeof(app->cfg.user_password_hash));
		app->cfg.user_extra_groups = gtk_toggle_button_get_active(
			GTK_TOGGLE_BUTTON(ac->groups));
		app_log(app, IL_LOG_INFO, "ui: autopilot: password of %s from a "
			"hash", app->cfg.user.username);
		wizard_go(pages_wizard(), PAGE_SUMMARY, +1);
		return AUTOFILL_DONE;
	}
	pw_pair_set(ac->pair, a->cfg.user.password);
	return AUTOFILL_NEXT;
}

static gboolean account_back(Page *p)
{
	Account *a = p->priv;

	if (a->step == STEP_PASSWORD) {
		show_step(p, STEP_WHO);
		return TRUE;
	}
	return FALSE;
}

static GtkWidget *account_build(Page *p)
{
	Account *a = p->priv;
	GtkWidget *body, *note;

	a->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(a->stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(a->stack), 220);

	a->steps[STEP_WHO] = card_page_new(
		"Who is going to use this computer?",
		"Create your own account for every day. It cannot change the "
		"system by itself; root can.", &body);
	a->fullname = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(a->fullname), IL_FULLNAME_MAX);
	gtk_entry_set_placeholder_text(GTK_ENTRY(a->fullname), "Your name");
	gtk_entry_set_activates_default(GTK_ENTRY(a->fullname), TRUE);
	ui_add_class(a->fullname, "il-entry");
	a->fullname_field = field_new("_Full name", a->fullname);
	gtk_box_pack_start(GTK_BOX(body), a->fullname_field, FALSE, FALSE, 0);

	a->username = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(a->username), IL_USERNAME_MAX);
	gtk_entry_set_placeholder_text(GTK_ENTRY(a->username), "User name");
	gtk_entry_set_activates_default(GTK_ENTRY(a->username), TRUE);
	gtk_entry_set_input_hints(GTK_ENTRY(a->username), GTK_INPUT_HINT_LOWERCASE |
				  GTK_INPUT_HINT_NO_SPELLCHECK);
	ui_add_class(a->username, "il-entry");
	a->username_field = field_new("_User name", a->username);
	gtk_box_pack_start(GTK_BOX(body), a->username_field, FALSE, FALSE, 0);

	a->groups = gtk_check_button_new_with_mnemonic(
		"Allow _sound and graphics acceleration for this account");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->groups),
				     p->app->cfg.user_extra_groups);
	ui_add_class(a->groups, "il-check");
	gtk_box_pack_start(GTK_BOX(body), a->groups, FALSE, FALSE, 0);
	note = ui_label("The account joins the groups audio, render and users.",
			"il-caption");
	gtk_widget_set_margin_start(note, 28);
	gtk_box_pack_start(GTK_BOX(body), note, FALSE, FALSE, 0);
	gtk_stack_add_named(GTK_STACK(a->stack), a->steps[STEP_WHO], "who");

	a->steps[STEP_PASSWORD] = card_page_new(
		"Create a password",
		"You type it to sign in at the login prompt.", &body);
	a->pair = pw_pair_new("_Password", "_Confirm the password",
			      on_pair_changed, p);
	gtk_box_pack_start(GTK_BOX(body), pw_pair_widget(a->pair), FALSE, FALSE, 0);
	a->same_as_root = gtk_check_button_new_with_mnemonic(
		"Use the _same password as root (not recommended)");
	ui_add_class(a->same_as_root, "il-check");
	g_signal_connect(a->same_as_root, "toggled", G_CALLBACK(on_same_as_root), p);
	gtk_box_pack_start(GTK_BOX(body), a->same_as_root, FALSE, FALSE, 0);
	gtk_stack_add_named(GTK_STACK(a->stack), a->steps[STEP_PASSWORD],
			    "password");

	g_signal_connect(a->fullname, "changed", G_CALLBACK(on_fullname), p);
	g_signal_connect(a->username, "changed", G_CALLBACK(on_username), p);

	/* answers given before (an answer file, or a page jump) */
	if (p->app->cfg.user.fullname[0])
		gtk_entry_set_text(GTK_ENTRY(a->fullname), p->app->cfg.user.fullname);
	if (p->app->cfg.user.username[0]) {
		gtk_entry_set_text(GTK_ENTRY(a->username), p->app->cfg.user.username);
		a->user_edited = TRUE;
	}
	if (p->app->cfg.user.password[0])
		pw_pair_set(a->pair, p->app->cfg.user.password);
	return a->stack;
}

static void account_enter(Page *p, int direction)
{
	Account *a = p->priv;

	show_step(p, direction < 0 ? STEP_PASSWORD : STEP_WHO);
	if (direction >= 0)
		page_set_focus(p, a->fullname);
}

void page_account_init(Page *p)
{
	p->id = "account";
	p->scene = SCENE_CARD;
	p->build = account_build;
	p->enter = account_enter;
	p->go_next = account_next;
	p->go_back = account_back;
	p->autofill = account_autofill;
	p->art = ART_AVATAR;
	p->next_ok = FALSE;
	p->priv = g_new0(Account, 1);
}
