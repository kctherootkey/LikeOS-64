/* LikeOS installer -- page 8: the password of root.
 *
 * root can change everything on the system; its password is asked for on
 * its own page, twice, with a strength meter.  Show/hide is on each entry.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"
#include "password-pair.h"

typedef struct {
	PwPair *pair;
} RootPage;

static void on_pair_changed(gpointer data)
{
	Page *p = data;
	RootPage *rp = p->priv;

	p->next_ok = pw_pair_valid(rp->pair);
	page_update(p);
}

static GtkWidget *root_build(Page *p)
{
	RootPage *rp = p->priv;
	GtkWidget *body, *root, *tips;

	root = card_page_new("Protect the administrator account",
			     "root may change everything on this computer. Give "
			     "it a password you keep in a safe place.", &body);
	rp->pair = pw_pair_new("_Password for root", "_Confirm the password",
			       on_pair_changed, p);
	gtk_box_pack_start(GTK_BOX(body), pw_pair_widget(rp->pair), FALSE, FALSE, 0);

	tips = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	ui_add_class(tips, "il-tips");
	gtk_box_pack_start(GTK_BOX(tips), ui_label("A strong password", "il-tips-title"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tips),
			   ui_check_row("is at least 12 characters long", ICON_CHECK),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tips),
			   ui_check_row("mixes letters, digits and other characters",
					ICON_CHECK), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tips),
			   ui_check_row("is different from the password of your own "
					"account", ICON_CHECK), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), tips, FALSE, FALSE, 0);
	return root;
}

static void root_enter(Page *p, int direction)
{
	RootPage *rp = p->priv;

	(void)direction;
	pw_pair_revalidate(rp->pair);
	page_set_focus(p, pw_pair_first_entry(rp->pair));
}

static gboolean root_next(Page *p)
{
	RootPage *rp = p->priv;

	if (!pw_pair_valid(rp->pair))
		return FALSE;
	g_strlcpy(p->app->cfg.root_password, pw_pair_text(rp->pair),
		  sizeof(p->app->cfg.root_password));
	p->app->cfg.root_password_hash[0] = '\0';
	app_log(p->app, IL_LOG_INFO, "ui: root password chosen");
	return TRUE;
}

static int root_autofill(Page *p, const struct il_answers *a)
{
	RootPage *rp = p->priv;

	if (!a->cfg.root_password[0] && a->cfg.root_password_hash[0]) {
		/* a ready hash: nothing to type */
		g_strlcpy(p->app->cfg.root_password_hash,
			  a->cfg.root_password_hash,
			  sizeof(p->app->cfg.root_password_hash));
		p->app->cfg.root_password[0] = '\0';
		app_log(p->app, IL_LOG_INFO, "ui: autopilot: root password "
			"from a hash");
		wizard_go(pages_wizard(), PAGE_ACCOUNT, +1);
		return AUTOFILL_DONE;
	}
	pw_pair_set(rp->pair, a->cfg.root_password);
	return AUTOFILL_NEXT;
}

void page_root_init(Page *p)
{
	p->id = "root";
	p->scene = SCENE_CARD;
	p->build = root_build;
	p->enter = root_enter;
	p->go_next = root_next;
	p->autofill = root_autofill;
	p->art = ART_SHIELD;
	p->next_ok = FALSE;
	p->priv = g_new0(RootPage, 1);
}
