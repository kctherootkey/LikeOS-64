// LikeOS Explorer: file operations.
//
// Every change Explorer makes to files is an ExOperation: copy, move, link,
// trash, delete, rename, new folder or file, restore from the Trash, empty
// the Trash, change permissions.  An operation runs in a worker thread and
// talks to the main loop through signals:
//
//   "progress" -- counters moved (at most ten times a second)
//   "conflict" -- the destination name is taken: the handler answers with
//                 ex_conflict_resolve(), now or later
//   "issue"    -- something failed: the handler answers with
//                 ex_issue_resolve() (retry, skip, cancel, or for a trash
//                 that cannot be done here, delete permanently instead)
//   "finished" -- done, failed or cancelled; ex_operation_get_state()
//
// Unanswered questions get the operation's defaults (skip), so a program
// with no dialogs -- the test suite -- still runs operations to the end.
//
// What an operation guarantees about its files:
//   - a file is copied to a temporary name in the destination folder and
//     renamed into place when complete, so a destination name only ever
//     holds a whole file: an interrupted or cancelled copy leaves nothing
//     half-written behind;
//   - a move between filesystems deletes the source only after its copy
//     completed with nothing skipped;
//   - what was done is recorded (ex_operation_get_results()), source and
//     destination, which is what undo works from.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_FILEOPS_H
#define EX_FILEOPS_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
	EX_OP_COPY,
	EX_OP_MOVE,
	EX_OP_LINK,
	EX_OP_TRASH,
	EX_OP_DELETE,
	EX_OP_RENAME,
	EX_OP_MKDIR,
	EX_OP_NEWFILE,
	EX_OP_RESTORE,
	EX_OP_EMPTY_TRASH,
	EX_OP_CHMOD,
} ExOpKind;

typedef enum {
	EX_OP_QUEUED,
	EX_OP_RUNNING,
	EX_OP_DONE, /* everything done, or the rest skipped by choice */
	EX_OP_FAILED, /* finished with errors */
	EX_OP_CANCELLED,
} ExOpState;

typedef enum {
	EX_CONFLICT_REPLACE,
	EX_CONFLICT_SKIP,
	EX_CONFLICT_KEEP_BOTH, /* the new item gets "name (2)" */
	EX_CONFLICT_MERGE, /* folder onto folder: combine their contents */
	EX_CONFLICT_CANCEL,
} ExConflictChoice;

typedef enum {
	EX_ISSUE_RETRY,
	EX_ISSUE_SKIP,
	EX_ISSUE_CANCEL,
	EX_ISSUE_DELETE_INSTEAD, /* only offered when trashing failed */
} ExIssueChoice;

/* A destination name that is taken. */
typedef struct {
	GFile *source;
	GFile *dest;
	gboolean source_is_dir, dest_is_dir;
	guint64 source_size, dest_size;
	gint64 source_mtime, dest_mtime;
	gboolean same_file; /* copying a file onto itself */
} ExConflict;

/* A failure, with the choices that make sense for it. */
typedef struct {
	GFile *file;
	char *message; /* "Could not copy “x”: Permission denied" */
	GError *error;
	gboolean can_retry;
	gboolean can_delete_instead;
} ExIssue;

/* One thing done: `source' became `dest' (copy, move, link, rename,
 * restore, trash -- dest is the item in the Trash), or `dest' was created
 * (mkdir, newfile -- source NULL), or `source' is gone (delete -- dest
 * NULL). */
typedef struct {
	GFile *source;
	GFile *dest;
} ExOpResult;

#define EX_TYPE_OPERATION (ex_operation_get_type())
G_DECLARE_FINAL_TYPE(ExOperation, ex_operation, EX, OPERATION, GObject)

/* `sources' is an array of GFile; every one goes into `dest_dir' under its
 * own name. */
ExOperation *ex_operation_new_copy(GPtrArray *sources, GFile *dest_dir);
ExOperation *ex_operation_new_move(GPtrArray *sources, GFile *dest_dir);
ExOperation *ex_operation_new_link(GPtrArray *sources, GFile *dest_dir);
/* Each source to exactly the matching destination (undo uses this). */
ExOperation *ex_operation_new_move_exact(GPtrArray *sources,
					 GPtrArray *dests);
ExOperation *ex_operation_new_trash(GPtrArray *sources);
ExOperation *ex_operation_new_delete(GPtrArray *sources);
ExOperation *ex_operation_new_rename(GFile *file, const char *new_name);
/* `name' NULL: "New folder", made unique. */
ExOperation *ex_operation_new_mkdir(GFile *dir, const char *name);
/* `template' NULL: an empty file. */
ExOperation *ex_operation_new_newfile(GFile *dir, const char *name,
				      GFile *template_file);
/* Items of the Trash (files in …/Trash/files) back where they came from. */
ExOperation *ex_operation_new_restore(GPtrArray *trash_items);
ExOperation *ex_operation_new_empty_trash(void);
ExOperation *ex_operation_new_chmod(GFile *file, guint32 mode);

void ex_operation_start(ExOperation *op);
void ex_operation_cancel(ExOperation *op);
/* Pause or resume.  A paused operation stops at its next progress point --
 * within the block of data it is copying -- and waits, its files open,
 * until resumed or cancelled; one paused while still queued stops as soon
 * as it starts.  Emits "progress" when the state changes.  Main loop
 * only. */
void ex_operation_set_paused(ExOperation *op, gboolean paused);
gboolean ex_operation_is_paused(ExOperation *op);

ExOpKind ex_operation_get_kind(ExOperation *op);
ExOpState ex_operation_get_state(ExOperation *op);
/* Whether it is stopped on a question (a conflict or an issue) until the
 * answer comes. */
gboolean ex_operation_is_waiting(ExOperation *op);
/* "Copying 3 items to Documents" -- for the operation center. */
const char *ex_operation_get_title(ExOperation *op);
/* The destination folder (copy, move, link, mkdir, newfile) or NULL. */
GFile *ex_operation_get_dest_dir(ExOperation *op);
/* The folder every source is in, or NULL when they come from different
 * folders (or there are none) -- the "from" of "Copying 3 items from
 * Downloads to Desktop". */
GFile *ex_operation_get_source_dir(ExOperation *op);
GPtrArray *ex_operation_get_sources(ExOperation *op);

/* Progress: what is being worked on now (a new string), counters, and the
 * fraction done (-1 while still counting). */
char *ex_operation_get_current(ExOperation *op);
void ex_operation_get_counts(ExOperation *op, guint64 *bytes_done,
			     guint64 *bytes_total, guint *items_done,
			     guint *items_total);
double ex_operation_get_fraction(ExOperation *op);
/* Bytes per second, measured over half-second windows and smoothed; 0 until
 * the first measurement (after a moment).  A pause does not count as
 * slow. */
double ex_operation_get_rate(ExOperation *op);

/* After "finished". */
GPtrArray *ex_operation_get_results(ExOperation *op); /* of ExOpResult* */
GPtrArray *ex_operation_get_errors(ExOperation *op); /* of char* */

void ex_conflict_resolve(ExOperation *op, ExConflictChoice choice,
			 gboolean apply_to_all);
void ex_issue_resolve(ExOperation *op, ExIssueChoice choice,
		      gboolean apply_to_all);

/* What an unanswered question gets.  Default skip / skip. */
void ex_operation_set_defaults(ExOperation *op, ExConflictChoice conflict,
			       ExIssueChoice issue);

G_END_DECLS

#endif
