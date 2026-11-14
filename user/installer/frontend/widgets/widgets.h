/* LikeOS installer -- the installer's own widgets.
 *
 *  ring-spinner.c   an arc chasing round a faint track, with a bright head
 *  progress-ring.c  a determinate ring with the percentage in its middle
 *  choice-list.c    the large, selectable rows of the card pages
 *  password-entry.c an entry with a show/hide toggle and a strength meter
 *  buttons.c        link buttons (flat text buttons with an optional icon)
 *                   and toolbar actions (icon and label, for the disk page)
 *  notice.c         coloured bars (information, warning, error, success)
 *                   and the step list with its state marks
 *  field.c          a label, an input and a line of help or error text;
 *                   small helpers for labels, buttons and scrollers
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_WIDGETS_H
#define LIKEOS_INSTALLER_WIDGETS_H

#include <gtk/gtk.h>

#include "art/art.h"

/* ---- ring-spinner.c -------------------------------------------------- */

GtkWidget *ring_spinner_new(int size);
void ring_spinner_set_active(GtkWidget *spinner, gboolean active);

/* ---- progress-ring.c ------------------------------------------------- */

GtkWidget *progress_ring_new(int size);
void progress_ring_set_fraction(GtkWidget *ring, double fraction);
/* The fraction and the number shown inside: the given percentage, the
 * same one the page shows elsewhere, instead of a rounding of the arc. */
void progress_ring_set_progress(GtkWidget *ring, double fraction, int percent);
double progress_ring_get_fraction(GtkWidget *ring);
/* The diameter (a bigger ring on a big screen). */
void progress_ring_set_size(GtkWidget *ring, int size);
/* Shows the percentage inside the ring (default on). */
void progress_ring_set_show_text(GtkWidget *ring, gboolean show);

/* ---- choice-list.c --------------------------------------------------- */

typedef void (*ChoiceChanged)(GtkWidget *list, const char *id, gpointer data);

GtkWidget *choice_list_new(void);
/* The scrolled window around the list (pack this one). */
GtkWidget *choice_list_scroller(GtkWidget *list);
void choice_list_add(GtkWidget *list, const char *id, const char *label,
		     const char *sublabel);
void choice_list_clear(GtkWidget *list);
void choice_list_select(GtkWidget *list, const char *id);
/* Selects a row as a click does: the changed callback runs.  FALSE when
 * there is no row of that id. */
gboolean choice_list_choose(GtkWidget *list, const char *id);
const char *choice_list_selected(GtkWidget *list);
/* Only rows whose label or sublabel contains text (case-insensitive). */
void choice_list_filter(GtkWidget *list, const char *text);
int choice_list_visible_count(GtkWidget *list);
void choice_list_on_changed(GtkWidget *list, ChoiceChanged cb, gpointer data);
/* Called on a double click or Enter on a row. */
void choice_list_on_activated(GtkWidget *list, ChoiceChanged cb, gpointer data);
void choice_list_scroll_to_selected(GtkWidget *list);
/* Keeps a selected row that the filter shows, else selects the first row
 * shown; FALSE when the filter hides every row. */
gboolean choice_list_select_first_visible(GtkWidget *list);
/* Whether the row of id passes the current filter. */
gboolean choice_list_is_shown(GtkWidget *list, const char *id);
/* What to give the keyboard focus: the selected row, or the list. */
GtkWidget *choice_list_focus_target(GtkWidget *list);

/* ---- password-entry.c ------------------------------------------------ */

GtkWidget *password_entry_new(const char *placeholder, gboolean show_meter);
GtkWidget *password_entry_get_entry(GtkWidget *pe);
const char *password_entry_get_text(GtkWidget *pe);
void password_entry_set_text(GtkWidget *pe, const char *text);
/* 0..4 as il_password_strength() returns; -1 hides the meter. */
void password_entry_set_strength(GtkWidget *pe, int strength);
void password_entry_set_visible_text(GtkWidget *pe, gboolean visible);

/* ---- buttons.c: link buttons ----------------------------------------- */

GtkWidget *link_button_new(const char *label, IconKind icon);
void link_button_set_label(GtkWidget *b, const char *label);

/* ---- buttons.c: toolbar actions -------------------------------------- */

GtkWidget *toolbar_action_new(const char *label, IconKind icon,
			      const char *tooltip);
/* "danger" paints the icon in the error colour (delete). */
void toolbar_action_set_style(GtkWidget *action, const char *style);

/* ---- notice.c -------------------------------------------------------- */

typedef enum {
	NOTICE_INFO = 0,
	NOTICE_WARNING,
	NOTICE_ERROR,
	NOTICE_SUCCESS,
} NoticeKind;

GtkWidget *notice_new(void);
void notice_set(GtkWidget *notice, NoticeKind kind, const char *markup);
void notice_hide(GtkWidget *notice);

/* ---- notice.c: the step list ----------------------------------------- */

typedef enum {
	STEP_PENDING = 0,
	STEP_RUNNING,
	STEP_DONE,
	STEP_FAILED,
} StepState;

GtkWidget *step_list_new(void);
void step_list_add(GtkWidget *list, const char *title);
void step_list_set_state(GtkWidget *list, int index, StepState state);
void step_list_reset(GtkWidget *list);
int step_list_count(GtkWidget *list);

/* ---- field.c --------------------------------------------------------- */

GtkWidget *field_new(const char *label, GtkWidget *input);
/* A line under the input: help in the secondary colour. */
void field_set_hint(GtkWidget *field, const char *text);
/* An error replaces the hint and colours the input's underline. */
void field_set_error(GtkWidget *field, const char *text);
/* A warning: amber text, the input stays as it is. */
void field_set_warning(GtkWidget *field, const char *text);
void field_set_ok(GtkWidget *field, const char *text);
GtkWidget *field_get_input(GtkWidget *field);

/* ---- field.c: helpers ------------------------------------------------ */

/* A label with a style class, wrapping, left aligned. */
GtkWidget *ui_label(const char *text, const char *css_class);
GtkWidget *ui_markup(const char *markup, const char *css_class);
GtkWidget *ui_heading(const char *text);
void ui_add_class(GtkWidget *w, const char *css_class);
void ui_remove_class(GtkWidget *w, const char *css_class);
void ui_toggle_class(GtkWidget *w, const char *css_class, gboolean on);
GtkWidget *ui_button(const char *mnemonic_label, const char *css_class);
/* A scrolled window for a page body: vertical scrolling only. */
GtkWidget *ui_scroller(GtkWidget *child);
GtkWidget *ui_box(GtkOrientation o, int spacing, const char *css_class);
/* A check mark row: a check icon and a label (the summary). */
GtkWidget *ui_check_row(const char *markup, IconKind icon);

#endif
