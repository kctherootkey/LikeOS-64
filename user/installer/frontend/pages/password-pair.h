/* LikeOS installer -- a password with its confirmation (password-pair.c).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_PASSWORD_PAIR_H
#define LIKEOS_INSTALLER_PASSWORD_PAIR_H

#include <gtk/gtk.h>

#include "app.h"

typedef struct PwPair PwPair;

/* changed is called after every keystroke, once the pair is re-checked.
 * The pair is freed with its widget. */
PwPair *pw_pair_new(const char *label1, const char *label2,
		    void (*changed)(gpointer data), gpointer data);
GtkWidget *pw_pair_widget(PwPair *pp);
GtkWidget *pw_pair_first_entry(PwPair *pp);
gboolean pw_pair_valid(PwPair *pp);
const char *pw_pair_text(PwPair *pp);
void pw_pair_set(PwPair *pp, const char *text);
void pw_pair_revalidate(PwPair *pp);

#endif
