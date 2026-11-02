// LikeOS -- File Explorer: locations.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-location.h"
#include "backend/ex-util.h"

#include <string.h>

struct _ExLocation {
	gint refs;
	ExLocationKind kind;
	GFile *file; /* folder, or the trash files/ directory */
	ExLocation *scope; /* search */
	char *query; /* search */
	char *id;
};

G_DEFINE_BOXED_TYPE(ExLocation, ex_location, ex_location_ref,
		    ex_location_unref)

GFile *ex_trash_files_dir(void)
{
	char *p = g_build_filename(g_get_user_data_dir(), "Trash", "files",
				   NULL);
	GFile *f = g_file_new_for_path(p);

	g_free(p);
	return f;
}

static ExLocation *loc_alloc(ExLocationKind kind)
{
	ExLocation *l = g_new0(ExLocation, 1);

	l->refs = 1;
	l->kind = kind;
	return l;
}

ExLocation *ex_location_new_folder(GFile *file)
{
	g_return_val_if_fail(G_IS_FILE(file), NULL);
	ExLocation *l = loc_alloc(EX_LOC_FOLDER);

	l->file = g_object_ref(file);
	l->id = g_file_get_uri(file);
	return l;
}

ExLocation *ex_location_new_special(ExLocationKind kind)
{
	ExLocation *l;

	g_return_val_if_fail(kind != EX_LOC_FOLDER && kind != EX_LOC_SEARCH,
			     NULL);
	l = loc_alloc(kind);
	switch (kind) {
	case EX_LOC_HOME:
		l->id = g_strdup("likeos:home");
		break;
	case EX_LOC_THISPC:
		l->id = g_strdup("likeos:thispc");
		break;
	case EX_LOC_TRASH:
		l->id = g_strdup("likeos:trash");
		l->file = ex_trash_files_dir();
		break;
	default:
		break;
	}
	return l;
}

ExLocation *ex_location_new_search(ExLocation *scope, const char *query)
{
	g_return_val_if_fail(scope != NULL, NULL);
	ExLocation *l = loc_alloc(EX_LOC_SEARCH);
	char *esc_scope, *esc_q;

	/* A search of search results searches the same scope again. */
	if (scope->kind == EX_LOC_SEARCH)
		scope = scope->scope;
	l->scope = ex_location_ref(scope);
	l->query = g_strdup(query ? query : "");
	if (scope->file)
		l->file = g_object_ref(scope->file);
	esc_scope = g_uri_escape_string(scope->id, NULL, FALSE);
	esc_q = g_uri_escape_string(l->query, NULL, FALSE);
	l->id = g_strdup_printf("likeos:search?scope=%s&q=%s", esc_scope,
				esc_q);
	g_free(esc_scope);
	g_free(esc_q);
	return l;
}

static ExLocation *parse_search(const char *args)
{
	char *scope_s = NULL, *q = NULL;
	char **parts = g_strsplit(args, "&", -1);
	ExLocation *l = NULL;

	for (int i = 0; parts[i]; i++) {
		if (g_str_has_prefix(parts[i], "scope="))
			scope_s = g_uri_unescape_string(parts[i] + 6, NULL);
		else if (g_str_has_prefix(parts[i], "q="))
			q = g_uri_unescape_string(parts[i] + 2, NULL);
	}
	g_strfreev(parts);
	if (scope_s) {
		ExLocation *scope = ex_location_parse(scope_s);

		if (scope) {
			l = ex_location_new_search(scope, q);
			ex_location_unref(scope);
		}
	}
	g_free(scope_s);
	g_free(q);
	return l;
}

ExLocation *ex_location_parse(const char *text)
{
	GFile *f = NULL;
	ExLocation *l;

	if (!text || !*text)
		return NULL;
	if (strcmp(text, "likeos:home") == 0)
		return ex_location_new_special(EX_LOC_HOME);
	if (strcmp(text, "likeos:thispc") == 0 ||
	    g_str_has_prefix(text, "computer:"))
		return ex_location_new_special(EX_LOC_THISPC);
	if (strcmp(text, "likeos:trash") == 0 ||
	    g_str_has_prefix(text, "trash:"))
		return ex_location_new_special(EX_LOC_TRASH);
	if (g_str_has_prefix(text, "likeos:search?"))
		return parse_search(text + strlen("likeos:search?"));
	if (text[0] == '~' && (text[1] == '\0' || text[1] == '/')) {
		char *p = g_build_filename(g_get_home_dir(), text + 1, NULL);

		f = g_file_new_for_path(p);
		g_free(p);
	} else if (text[0] == '/') {
		f = g_file_new_for_path(text);
	} else if (g_uri_is_valid(text, G_URI_FLAGS_NONE, NULL) ||
		   strstr(text, "://")) {
		f = g_file_new_for_uri(text);
	} else {
		/* Relative to nothing: read it against the home folder, as a
		 * shell started there would. */
		char *p = g_build_filename(g_get_home_dir(), text, NULL);

		f = g_file_new_for_path(p);
		g_free(p);
	}
	/* The trash folder, reached by its path, is the Trash. */
	GFile *trash = ex_trash_files_dir();
	if (g_file_equal(f, trash)) {
		g_object_unref(trash);
		g_object_unref(f);
		return ex_location_new_special(EX_LOC_TRASH);
	}
	g_object_unref(trash);
	l = ex_location_new_folder(f);
	g_object_unref(f);
	return l;
}

ExLocation *ex_location_ref(ExLocation *loc)
{
	g_return_val_if_fail(loc != NULL, NULL);
	g_atomic_int_inc(&loc->refs);
	return loc;
}

void ex_location_unref(ExLocation *loc)
{
	if (!loc || !g_atomic_int_dec_and_test(&loc->refs))
		return;
	g_clear_object(&loc->file);
	if (loc->scope)
		ex_location_unref(loc->scope);
	g_free(loc->query);
	g_free(loc->id);
	g_free(loc);
}

ExLocationKind ex_location_kind(const ExLocation *loc)
{
	return loc->kind;
}

GFile *ex_location_file(const ExLocation *loc)
{
	return loc->file;
}

const char *ex_location_query(const ExLocation *loc)
{
	return loc->query;
}

ExLocation *ex_location_scope(const ExLocation *loc)
{
	return loc->scope;
}

const char *ex_location_id(const ExLocation *loc)
{
	return loc->id;
}

gboolean ex_location_equal(const ExLocation *a, const ExLocation *b)
{
	if (a == b)
		return TRUE;
	if (!a || !b)
		return FALSE;
	return strcmp(a->id, b->id) == 0;
}

guint ex_location_hash(const ExLocation *loc)
{
	return g_str_hash(loc->id);
}

/* The name a folder is shown under. */
static char *folder_name(GFile *f)
{
	char *path = g_file_get_path(f);
	char *name;

	if (path && strcmp(path, "/") == 0) {
		g_free(path);
		return g_strdup("Local Disk");
	}
	if (path && strcmp(path, g_get_home_dir()) == 0) {
		g_free(path);
		const char *real = g_get_real_name();
		if (real && *real && strcmp(real, "Unknown") != 0)
			return g_strdup(real);
		return g_strdup(g_get_user_name());
	}
	/* From the name alone: no I/O, so a tab title or a crumb never waits
	 * for a disk or a network share. */
	if (path) {
		name = g_filename_display_basename(path);
		g_free(path);
		return name;
	}
	name = g_file_get_basename(f);
	return name ? name : g_file_get_uri(f);
}

char *ex_location_display_name(const ExLocation *loc)
{
	switch (loc->kind) {
	case EX_LOC_HOME:
		return g_strdup("Home");
	case EX_LOC_THISPC:
		return g_strdup("This PC");
	case EX_LOC_TRASH:
		return g_strdup("Trash");
	case EX_LOC_SEARCH: {
		char *scope = ex_location_display_name(loc->scope);
		char *s = g_strdup_printf("Search results in %s", scope);

		g_free(scope);
		return s;
	}
	case EX_LOC_FOLDER:
	default:
		return folder_name(loc->file);
	}
}

/* The XDG directory `path' is, or -1. */
static int special_dir_of(const char *path)
{
	for (int d = 0; d < G_USER_N_DIRECTORIES; d++) {
		const char *p = ex_user_dir((GUserDirectory)d);

		if (p && strcmp(p, path) == 0)
			return d;
	}
	return -1;
}

const char *ex_location_icon_name(const ExLocation *loc)
{
	switch (loc->kind) {
	case EX_LOC_HOME:
		return "go-home";
	case EX_LOC_THISPC:
		return "computer";
	case EX_LOC_TRASH:
		return "user-trash";
	case EX_LOC_SEARCH:
		return "system-search";
	case EX_LOC_FOLDER:
	default:
		break;
	}
	char *path = g_file_get_path(loc->file);
	const char *icon = "folder";

	if (!path)
		return "folder-remote";
	if (strcmp(path, "/") == 0)
		icon = "drive-harddisk";
	else if (strcmp(path, g_get_home_dir()) == 0)
		icon = "user-home";
	else {
		switch (special_dir_of(path)) {
		case G_USER_DIRECTORY_DESKTOP:
			icon = "user-desktop";
			break;
		case G_USER_DIRECTORY_DOCUMENTS:
			icon = "folder-documents";
			break;
		case G_USER_DIRECTORY_DOWNLOAD:
			icon = "folder-download";
			break;
		case G_USER_DIRECTORY_MUSIC:
			icon = "folder-music";
			break;
		case G_USER_DIRECTORY_PICTURES:
			icon = "folder-pictures";
			break;
		case G_USER_DIRECTORY_VIDEOS:
			icon = "folder-videos";
			break;
		case G_USER_DIRECTORY_PUBLIC_SHARE:
			icon = "folder-publicshare";
			break;
		case G_USER_DIRECTORY_TEMPLATES:
			icon = "folder-templates";
			break;
		default:
			break;
		}
	}
	g_free(path);
	return icon;
}

char *ex_location_edit_text(const ExLocation *loc)
{
	switch (loc->kind) {
	case EX_LOC_FOLDER: {
		char *p = g_file_get_path(loc->file);

		return p ? p : g_file_get_uri(loc->file);
	}
	case EX_LOC_SEARCH:
		return ex_location_edit_text(loc->scope);
	default:
		return ex_location_display_name(loc);
	}
}

ExLocation *ex_location_parent(const ExLocation *loc)
{
	switch (loc->kind) {
	case EX_LOC_FOLDER: {
		GFile *p = g_file_get_parent(loc->file);
		ExLocation *l;

		if (!p)
			return ex_location_new_special(EX_LOC_THISPC);
		l = ex_location_new_folder(p);
		g_object_unref(p);
		return l;
	}
	case EX_LOC_SEARCH:
		return ex_location_ref(loc->scope);
	case EX_LOC_TRASH:
	case EX_LOC_THISPC:
		return ex_location_new_special(EX_LOC_HOME);
	default:
		return NULL;
	}
}

GPtrArray *ex_location_crumbs(const ExLocation *loc)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)ex_location_unref);

	if (loc->kind == EX_LOC_SEARCH) {
		g_ptr_array_unref(out);
		out = ex_location_crumbs(loc->scope);
		g_ptr_array_add(out, ex_location_ref((ExLocation *)loc));
		return out;
	}
	if (loc->kind != EX_LOC_FOLDER) {
		g_ptr_array_add(out, ex_location_ref((ExLocation *)loc));
		return out;
	}

	/* Below the home folder the chain starts there; elsewhere at This PC
	 * and the disk. */
	GFile *home = ex_home_dir();
	GFile *stop = NULL;
	GSList *chain = NULL;
	GFile *cur = g_object_ref(loc->file);

	if (g_file_equal(cur, home) || g_file_has_prefix(cur, home))
		stop = home;
	for (;;) {
		chain = g_slist_prepend(chain, cur);
		if (stop && g_file_equal(cur, stop))
			break;
		GFile *p = g_file_get_parent(cur);
		if (!p)
			break;
		cur = p;
	}
	if (!stop && g_file_is_native(loc->file))
		g_ptr_array_add(out, ex_location_new_special(EX_LOC_THISPC));
	for (GSList *s = chain; s; s = s->next) {
		g_ptr_array_add(out, ex_location_new_folder(s->data));
		g_object_unref(s->data);
	}
	g_slist_free(chain);
	g_object_unref(home);
	return out;
}
