/* LikeOS installer -- what the page files share.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_PAGES_H
#define LIKEOS_INSTALLER_PAGES_H

#include "app.h"
#include "backend-bridge.h"
#include "keyboard.h"
#include "layout.h"
#include "sheet.h"
#include "widgets/widgets.h"
#include "wizard.h"

/* A page body for the Setup panel: a big light accent heading, then
 * *body, all in a scrolled window.  Returns the page's root. */
GtkWidget *setup_page_new(const char *heading, GtkWidget **body);

/* A page body for the card: heading (and a subtitle when given), then
 * *body, all in a scrolled window. */
GtkWidget *card_page_new(const char *heading, const char *subtitle,
			 GtkWidget **body);
/* Changes the heading (and subtitle) of a card or Setup page. */
void page_set_heading(GtkWidget *root, const char *heading,
		      const char *subtitle);

/* A "label: control" row of the Setup panel; turns into label above
 * control when the screen is narrow (form_relayout). */
GtkWidget *form_row(const char *mnemonic_label, GtkWidget *control);
void form_relayout(GtkWidget *container, gboolean stacked);

/* A search field that filters a choice list as one types. */
GtkWidget *search_for_list(GtkWidget *choice_list, const char *placeholder);

/* The wizard of the running program (for pages that move on by
 * themselves). */
Wizard *pages_wizard(void);
void pages_set_wizard(Wizard *w);

/* Short markup-safe strings. */
char *pages_escape(const char *text);

#endif
