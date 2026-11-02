// LikeOS Explorer: locations.
//
// A location is what a tab shows.  Most are folders, but Home, Gallery,
// Computer, the Trash and search results are places too, and none of them
// is a path.  So a location has a kind and, for the kinds that have one, a
// GFile; its identity is a stable string id ("likeos:home", "likeos:gallery",
// "likeos:computer", "likeos:trash", "likeos:search?scope=<uri>&q=<query>",
// or a folder's URI) that history, pins and settings keep, rather than the
// text shown in the address bar.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_LOCATION_H
#define EX_LOCATION_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
	EX_LOC_FOLDER,
	EX_LOC_HOME, /* the landing page: pins, favorites, recent files */
	EX_LOC_COMPUTER, /* drives and the personal folders */
	EX_LOC_TRASH,
	EX_LOC_SEARCH, /* results of a query below a scope location */
	EX_LOC_GALLERY, /* the pictures and videos of the collection */
} ExLocationKind;

typedef struct _ExLocation ExLocation;

#define EX_TYPE_LOCATION (ex_location_get_type())
GType ex_location_get_type(void);

ExLocation *ex_location_new_folder(GFile *file);
ExLocation *ex_location_new_special(ExLocationKind kind);
ExLocation *ex_location_new_search(ExLocation *scope, const char *query);

/* An id, URI, absolute path or "~/..." -- whatever a command line, a saved
 * setting or the address bar hands over.  trash:/// and computer:/// map to
 * the Trash and Computer.  NULL for something that names no location. */
ExLocation *ex_location_parse(const char *text);

ExLocation *ex_location_ref(ExLocation *loc);
void ex_location_unref(ExLocation *loc);

ExLocationKind ex_location_kind(const ExLocation *loc);
/* The folder for EX_LOC_FOLDER; the scope's folder for EX_LOC_SEARCH (if the
 * scope is a folder); the trash files/ directory for EX_LOC_TRASH; NULL
 * otherwise.  Not a new reference. */
GFile *ex_location_file(const ExLocation *loc);
const char *ex_location_query(const ExLocation *loc);
ExLocation *ex_location_scope(const ExLocation *loc);

/* The stable id.  Owned by the location. */
const char *ex_location_id(const ExLocation *loc);
gboolean ex_location_equal(const ExLocation *a, const ExLocation *b);
guint ex_location_hash(const ExLocation *loc);

/* What the tab title and the address bar show, and the themed icon name. */
char *ex_location_display_name(const ExLocation *loc);
const char *ex_location_icon_name(const ExLocation *loc);
/* What the address bar puts in its text field when edited. */
char *ex_location_edit_text(const ExLocation *loc);

/* Up: the containing folder, or Computer above the root, or NULL. */
ExLocation *ex_location_parent(const ExLocation *loc);

/* The breadcrumb chain from the outermost place down to `loc', each a new
 * reference: Documents > Letters inside a personal folder, niko > src
 * elsewhere below the home folder, Computer > System disk > usr outside
 * it. */
GPtrArray *ex_location_crumbs(const ExLocation *loc);

/* The trash folder of the home directory (…/Trash/files). */
GFile *ex_trash_files_dir(void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ExLocation, ex_location_unref)

G_END_DECLS

#endif
