/* LikeOS installer -- the state every part of the wizard shares.
 *
 * One App exists for the life of the program.  It owns the backend context,
 * the scan results, one partitioning plan per disk, the choices the person
 * makes on the way (struct il_config) and the window with its wizard.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_APP_H
#define LIKEOS_INSTALLER_APP_H

#include <gtk/gtk.h>

#include "backend-api.h"

typedef struct Wizard Wizard;
typedef struct Bridge Bridge;
typedef struct Autopilot Autopilot;

/* How the program ends: the contract with the installer session
 * (user/bin/installer/session/likeos-installer-session), which acts on it. */
enum {
	APP_EXIT_RESTART = 0,    /* the session restarts the computer */
	APP_EXIT_QUIT = 2,       /* left without installing: the session's
				  * console menu (text-mode installer, try
				  * again, log in, restart, power off) */
	APP_EXIT_AUTOPILOT = 3,  /* an autopilot run failed */
	APP_EXIT_POWEROFF = 10,  /* the session switches the computer off */
};

/* One change the person made to a disk on the disk page.  A disk's plan is
 * the scan result with its list of steps replayed (plans.c), so the last
 * step can be undone by replaying all but it. */
typedef enum {
	PLAN_STEP_NEW_TABLE,     /* Convert to GPT */
	PLAN_STEP_WHOLE,         /* Use entire disk */
	PLAN_STEP_DELETE,        /* partno */
	PLAN_STEP_CREATE,        /* in the free region starting at start;
				  * size 0 = all of it */
	PLAN_STEP_EXTEND,        /* partno to size */
	PLAN_STEP_FORMAT,        /* partno */
} PlanStepKind;

typedef struct {
	PlanStepKind kind;
	int partno;
	guint64 start;
	guint64 size;
} PlanStep;

/* Page numbers: the order of the wizard, also what --page=N selects. */
enum {
	PAGE_SPLASH = 0,
	PAGE_LANGUAGE,
	PAGE_KEYBOARD,
	PAGE_PREPARING,
	PAGE_DISK,
	PAGE_REGION,
	PAGE_LAYOUT,
	PAGE_HOSTNAME,
	PAGE_ROOT,
	PAGE_ACCOUNT,
	PAGE_SUMMARY,
	PAGE_INSTALLING,
	PAGE_FINISHED,
	PAGE_ERROR,
	PAGE_COUNT
};

/* How much room the screen gives us; see layout.c. */
typedef enum {
	TIER_COMPACT = 0,   /* below 1000 px wide or 700 px high */
	TIER_REGULAR,
	TIER_LARGE,         /* 1600 x 1000 and up */
} LayoutTier;

typedef struct App {
	GtkWidget *window;
	GtkWidget *overlay;          /* window child: scenes + sheets */
	Wizard *wiz;
	Bridge *bridge;

	/* Command line. */
	gboolean mock;               /* the stand-in backend (--mock) */
	gboolean simulate;           /* libinstaller on image files */
	gboolean windowed;
	int start_page;
	int req_width, req_height;   /* --size, 0 = the screen */
	gboolean no_splash_delay;
	char **sim_disks;            /* --simulate IMG ... (NULL-terminated) */
	char *source_path;           /* --source, else the default */
	char *target_path;           /* --target */
	char *log_path;              /* --log */
	gboolean verbose;
	int exit_code;               /* APP_EXIT_*, what main() returns */

	/* Backend. */
	il_ctx *ctx;
	struct il_config cfg;
	struct il_source_info src;
	gboolean src_ok;
	char src_error[IL_ERROR_MAX];
	struct il_disk_list *disks;
	gboolean disks_ok;
	char disks_error[IL_ERROR_MAX];
	il_plan *plans[IL_MAX_DISKS];
	GArray *steps[IL_MAX_DISKS]; /* PlanStep: how plans[i] was made */
	int target_disk;             /* index into disks, -1 = none chosen */
	char *disk_message;          /* markup the disk page shows next time */
	gboolean disk_rescan;        /* ... after looking at the disks again */

	/* The keyboard catalogue (GPtrArray of struct kbd_layout *). */
	GPtrArray *kbd_cat;

	/* Screen. */
	int screen_w, screen_h;
	LayoutTier tier;
	gboolean narrow;             /* the card shows one column */
	gboolean short_screen;

	/* Result of the last installation run. */
	int install_rc;
	int failed_step;
	char install_error[IL_ERROR_MAX];
	gboolean installed;

	/* Logo, loaded once from the resources. */
	GdkPixbuf *logo;

	/* LIKEOS_INSTALLER_AUTOPILOT / --autopilot (autopilot.c). */
	Autopilot *autopilot;
} App;

/* The single instance (set by main.c). */
extern App *the_app;

/* app.c */
void app_init_config(App *app);
const char *app_locale_label(const char *name);
const char *app_timezone_label(const char *name);
char *app_layout_label(App *app, const char *layout, const char *variant);
char *app_layouts_summary(App *app);
il_plan *app_target_plan(App *app);
const struct il_disk *app_target_disk(App *app);
/* Restart or power off at the user's word (see app.c), or leave setup
 * without either. */
void app_power_action(App *app, gboolean reboot);
void app_exit_setup(App *app);
void app_quit(App *app);
const char *app_log_path(App *app);
GdkPixbuf *app_logo(App *app, int size);
char *app_size_text(guint64 bytes);
/* A backend sentence ("Partition 3 is too small: ...") as one for the
 * screen: with its full stop; g_free. */
char *app_sentence(const char *text);
/* The keyboard options to install: a group switch for two layouts or
 * more, none for one. */
void app_finish_keyboard(App *app);

/* A line for the installation log (and, in a build for a serial console,
 * the serial line): page changes, the person's decisions (never a
 * password), errors shown.  Held back while a job runs on the worker
 * thread and written when it ends, so the backend's context is never used
 * by two threads at once. */
void app_log(App *app, int level, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

/* plans.c: the plans of the disks and the steps they were made of. */
il_plan *plans_get(App *app, int di);
int plans_apply(App *app, int di, const PlanStep *step, int *partno_out);
int plans_nsteps(App *app, int di);
const PlanStep *plans_step(App *app, int di, int i);
/* Replays only the first n steps; 0 or -errno. */
int plans_truncate(App *app, int di, int n);
/* Takes back the last step; its description in *what (g_free). */
int plans_undo(App *app, int di, char **what);
char *plans_step_describe(const PlanStep *step);
/* Forgets every plan (a new scan). */
void plans_free_all(App *app);
/* Drops the choice of a root partition on every disk. */
void plans_clear_roots(App *app);

#endif
