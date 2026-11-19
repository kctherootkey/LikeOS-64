/* LikeOS installer -- the wizard: pages, the scenes they live in, history
 * and the buttons that move between them.
 *
 * A page belongs to one of four scenes, each drawn in its own style:
 *  - SCENE_SPLASH  black, the logo and a spinner;
 *  - SCENE_SETUP   a deep gradient with a centred "LikeOS Setup" panel;
 *  - SCENE_CARD    a soft light backdrop with one big rounded card, an
 *                  illustration on the left, the questions on the right;
 *  - SCENE_ACCENT  the full-screen accent colour of the copy phase.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_WIZARD_H
#define LIKEOS_INSTALLER_WIZARD_H

#include "app.h"
#include "art/art.h"

typedef enum {
	SCENE_SPLASH = 0,
	SCENE_SETUP,
	SCENE_CARD,
	SCENE_ACCENT,
	SCENE_COUNT
} SceneKind;

typedef struct Page Page;

typedef enum {
	AUTOFILL_NEXT = 0,   /* filled in: press Next */
	AUTOFILL_DONE,       /* the page moved on (or acted) by itself */
	AUTOFILL_WAIT,       /* not ready yet: ask again in a moment */
	AUTOFILL_FAIL,       /* the answers cannot be given here */
} AutofillResult;

struct Page {
	int index;
	const char *id;
	SceneKind scene;
	gboolean transient;          /* never returned to with Back */

	/* Built once, the first time the page is shown. */
	GtkWidget *(*build)(Page *p);
	/* direction: +1 forward, -1 back, 0 jump */
	void (*enter)(Page *p, int direction);
	void (*leave)(Page *p);
	/* TRUE: go to next_index; FALSE: the page moved on by itself (a
	 * sub-step) or refused. */
	gboolean (*go_next)(Page *p);
	/* TRUE: the page went back a sub-step itself. */
	gboolean (*go_back)(Page *p);
	void (*secondary)(Page *p);
	/* The layout tier changed (compact <-> regular). */
	void (*relayout)(Page *p);
	/* The autopilot (autopilot.c): fill in the answers the way a person
	 * would, for the step the page is on; returns an AutofillResult. */
	int (*autofill)(Page *p, const struct il_answers *a);

	/* Button state, changed through page_update(). */
	const char *next_label;      /* NULL: "_Next" */
	gboolean next_ok;
	gboolean next_visible;
	gboolean back_ok;
	const char *secondary_label; /* NULL: hidden */
	ArtKind art;                 /* card illustration */
	int next_index;              /* -1: index + 1 */
	int substep;                 /* > 0: Back stays on the page */

	App *app;
	GtkWidget *root;
	gpointer priv;
};

Wizard *wizard_new(App *app);
GtkWidget *wizard_widget(Wizard *w);
Page *wizard_page(Wizard *w, int index);
Page *wizard_current(Wizard *w);
int wizard_current_index(Wizard *w);

/* Moves to a page; transient pages are not put on the history. */
void wizard_go(Wizard *w, int index, int direction);
void wizard_next(Wizard *w);
void wizard_back(Wizard *w);
/* Pretends the pages before index were visited (for --page). */
void wizard_seed_history(Wizard *w, int index);
/* Forgets the history (after the installation started). */
void wizard_clear_history(Wizard *w);
/* Drops history entries back to (and including) a page. */
void wizard_history_truncate(Wizard *w, int index);

/* A page tells the wizard that its button state changed. */
void page_update(Page *p);
/* The widget that gets the keyboard focus when the page is shown. */
void page_set_focus(Page *p, GtkWidget *widget);
/* Grabs the focus for a widget once the page is on screen. */
void page_focus_later(GtkWidget *widget);

/* The tier or the narrow flag changed. */
void wizard_relayout(Wizard *w);

/* Key handling for the window (Alt+B, Alt+N, Esc). */
gboolean wizard_key_press(Wizard *w, GdkEventKey *ev);

/* The scene widget for a scene kind (for the overlay status tray). */
GtkWidget *wizard_scene(Wizard *w, SceneKind kind);

/* Page constructors (pages/page-*.c). */
void page_splash_init(Page *p);
void page_language_init(Page *p);
void page_keyboard_init(Page *p);
void page_preparing_init(Page *p);
void page_disk_init(Page *p);
void page_region_init(Page *p);
void page_layout_init(Page *p);
void page_hostname_init(Page *p);
void page_root_init(Page *p);
void page_account_init(Page *p);
void page_summary_init(Page *p);
void page_installing_init(Page *p);
void page_finished_init(Page *p);
void page_error_init(Page *p);

#endif
