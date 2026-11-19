/* LikeOS installer -- a password with its confirmation.
 *
 * Shared by the root password page and the user's password step: two
 * password entries, the first with a strength meter; checked with the
 * backend's il_validate_password and il_password_strength while typing.
 * A short password is accepted with a warning; a mismatch is an error only
 * once something was typed into the confirmation.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "password-pair.h"
#include "widgets/widgets.h"

#include <string.h>

struct PwPair {
	GtkWidget *box;
	GtkWidget *first, *second;
	GtkWidget *first_field, *second_field;
	void (*changed)(gpointer data);
	gpointer data;
	gboolean valid;
};

static void pair_check(PwPair *pp)
{
	const char *a = password_entry_get_text(pp->first);
	const char *b = password_entry_get_text(pp->second);
	char why[256];
	int rc = il_validate_password(a, why, sizeof(why));

	if (!*a) {
		password_entry_set_strength(pp->first, -1);
		field_set_hint(pp->first_field, NULL);
	} else {
		char *t = app_sentence(why);

		password_entry_set_strength(pp->first, il_password_strength(a));
		/* a short password is valid, with a warning (installer.h) */
		if (rc != 0)
			field_set_error(pp->first_field, t);
		else if (why[0])
			field_set_warning(pp->first_field, t);
		else
			field_set_hint(pp->first_field, NULL);
		g_free(t);
	}

	if (!*b)
		field_set_hint(pp->second_field, NULL);
	else if (strcmp(a, b) != 0)
		field_set_error(pp->second_field, strlen(b) < strlen(a) &&
				strncmp(a, b, strlen(b)) == 0 ?
				"Keep typing: the passwords do not match yet." :
				"The passwords do not match.");
	else if (rc == 0)
		field_set_ok(pp->second_field, "The passwords match.");
	else
		field_set_hint(pp->second_field, NULL);

	pp->valid = rc == 0 && *a && strcmp(a, b) == 0;
	if (pp->changed)
		pp->changed(pp->data);
}

static void on_changed(GtkEditable *e, gpointer data)
{
	(void)e;
	pair_check(data);
}

static void on_first_activate(GtkEntry *e, gpointer data)
{
	PwPair *pp = data;

	(void)e;
	/* Enter in the first entry moves to the confirmation */
	if (!*password_entry_get_text(pp->second))
		gtk_widget_grab_focus(password_entry_get_entry(pp->second));
	else
		gtk_window_activate_default(GTK_WINDOW(gtk_widget_get_toplevel(
			pp->box)));
}

PwPair *pw_pair_new(const char *label1, const char *label2,
		    void (*changed)(gpointer data), gpointer data)
{
	PwPair *pp = g_new0(PwPair, 1);
	GtkWidget *e1;

	pp->changed = changed;
	pp->data = data;
	pp->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
	ui_add_class(pp->box, "il-password-pair");

	pp->first = password_entry_new("Password", TRUE);
	pp->first_field = field_new(label1, pp->first);
	gtk_box_pack_start(GTK_BOX(pp->box), pp->first_field, FALSE, FALSE, 0);
	pp->second = password_entry_new("Password again", FALSE);
	pp->second_field = field_new(label2, pp->second);
	gtk_box_pack_start(GTK_BOX(pp->box), pp->second_field, FALSE, FALSE, 0);

	e1 = password_entry_get_entry(pp->first);
	gtk_entry_set_activates_default(GTK_ENTRY(e1), FALSE);
	g_signal_connect(e1, "activate", G_CALLBACK(on_first_activate), pp);
	g_signal_connect(e1, "changed", G_CALLBACK(on_changed), pp);
	g_signal_connect(password_entry_get_entry(pp->second), "changed",
			 G_CALLBACK(on_changed), pp);
	g_object_set_data_full(G_OBJECT(pp->box), "pw-pair", pp, g_free);
	return pp;
}

GtkWidget *pw_pair_widget(PwPair *pp)
{
	return pp->box;
}

GtkWidget *pw_pair_first_entry(PwPair *pp)
{
	return password_entry_get_entry(pp->first);
}

gboolean pw_pair_valid(PwPair *pp)
{
	return pp->valid;
}

const char *pw_pair_text(PwPair *pp)
{
	return password_entry_get_text(pp->first);
}

void pw_pair_set(PwPair *pp, const char *text)
{
	password_entry_set_text(pp->first, text);
	password_entry_set_text(pp->second, text);
	pair_check(pp);
}

void pw_pair_revalidate(PwPair *pp)
{
	pair_check(pp);
}
