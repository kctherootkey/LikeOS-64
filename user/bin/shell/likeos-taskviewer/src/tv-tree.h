// LikeOS Task Viewer -- the grouped process tree of the Processes and Users
// pages.
//
// A page describes what the tree should show as a tree of TvNode; the tree
// brings its store in line with that, row by row, keyed by each node's key
// among its siblings: rows that stay are updated in place, so the
// selection, the expanded rows and the scroll position survive every
// update.  The view sorts by any column, within each level; group rows (the
// "Apps (3)" headings) keep their order whichever way it sorts, and an
// app's windows stay above its processes.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_TREE_H
#define TV_TREE_H

#include <gtk/gtk.h>

#include "tv-procs.h"

G_BEGIN_DECLS

typedef enum {
	TV_ROW_GROUP, /* a heading: "Apps (3)" */
	TV_ROW_APP,   /* an app: its main process and its helpers */
	TV_ROW_WIN,   /* one of an app's windows */
	TV_ROW_PROC,  /* a process */
	TV_ROW_USER,  /* a user and their processes */
} TvRowKind;

enum {
	TV_COL_KEY,
	TV_COL_KIND,
	TV_COL_NAME,
	TV_COL_ICON,
	TV_COL_STATUS,
	TV_COL_CPU, /* percent; negative: not shown */
	TV_COL_MEM,
	TV_COL_PID,
	TV_COL_XID,
	TV_COL_ORDER,
	TV_COL_SORT, /* collation key of the name */
	TV_N_COLS
};

typedef struct {
	char *key;
	TvRowKind kind;
	char *name;
	GIcon *icon;
	const char *status;
	double cpu; /* negative: no figures on this row */
	guint64 mem;
	int pid;
	gulong xid;
	int order; /* the place of a group row */
	gboolean expand; /* expanded when it first appears */
	GPtrArray *kids; /* TvNode * */
} TvNode;

/* Takes `key'; refs `icon'. */
TvNode *tv_node_new(TvRowKind kind, char *key, const char *name, GIcon *icon);
void tv_node_add(TvNode *parent, TvNode *kid);
/* A GPtrArray that frees its nodes. */
GPtrArray *tv_nodes_new(void);

typedef struct {
	GtkWidget *scroll; /* the page's widget */
	GtkWidget *view;
	GtkTreeStore *store;
	GtkTreeModel *sorted;
	GtkWidget *cpu_total, *mem_total;
	char *sort_setting;
} TvTree;

/* `setting' names the settings keys the sort order is kept under. */
TvTree *tv_tree_new(const char *setting, const char *name_title,
		    gboolean status_column);
void tv_tree_sync(TvTree *t, GPtrArray *top);
void tv_tree_clear(TvTree *t);
/* The CPU and memory totals in the column headers. */
void tv_tree_set_totals(TvTree *t, const TvSnapshot *s);

/* The selected row: kind, pid and window; FALSE when none. */
gboolean tv_tree_selected(TvTree *t, TvRowKind *kind, int *pid, gulong *xid);
/* Selects the row under (x, y) of the view's bin window, for a context
 * menu; FALSE when there is none. */
gboolean tv_tree_select_at(TvTree *t, double x, double y);
/* The selected row's path in the view's model (free it), or NULL. */
GtkTreePath *tv_tree_selected_path(TvTree *t);
gboolean tv_tree_selected_has_kids(TvTree *t);
/* Expands every group row (after a collapse-all). */
void tv_tree_expand_groups(TvTree *t);

G_END_DECLS

#endif
