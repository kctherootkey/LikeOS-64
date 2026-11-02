// LikeOS -- File Explorer: cut, copy and paste of files.
//
// Copyright (C) 2026 The LikeOS Project

#include "services/ex-clipboard.h"

#include <string.h>

#define GNOME_FILES "x-special/gnome-copied-files"

enum { TARGET_GNOME, TARGET_URIS, TARGET_TEXT };

struct _ExClipboard {
	GObject parent;
	GtkClipboard *cb;
	/* What we put there, while we own it. */
	GPtrArray *files;
	gboolean cut;
	gboolean owner;
	gboolean can_paste;
};

G_DEFINE_TYPE(ExClipboard, ex_clipboard, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void ex_clipboard_class_init(ExClipboardClass *k)
{
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 0);
}

static void ex_clipboard_init(ExClipboard *c)
{
}

static void got_targets(GtkClipboard *cb, GdkAtom *atoms, int n,
			gpointer data)
{
	ExClipboard *c = data;
	/* While the files are ours, they can be pasted whatever the answer
	 * (an answer that came back empty is not a reason to refuse). */
	gboolean can = c->owner && c->files;

	for (int i = 0; i < n; i++) {
		char *name = gdk_atom_name(atoms[i]);

		if (strcmp(name, GNOME_FILES) == 0 ||
		    strcmp(name, "text/uri-list") == 0)
			can = TRUE;
		g_free(name);
	}
	if (can != c->can_paste) {
		c->can_paste = can;
		g_signal_emit(c, sigs[SIG_CHANGED], 0);
	}
	g_object_unref(c);
}

static void owner_change(GtkClipboard *cb, GdkEvent *ev, gpointer data)
{
	ExClipboard *c = data;

	gtk_clipboard_request_targets(cb, got_targets, g_object_ref(c));
	g_signal_emit(c, sigs[SIG_CHANGED], 0);
}

ExClipboard *ex_clipboard_get(void)
{
	static ExClipboard *the;

	if (!the) {
		the = g_object_new(EX_TYPE_CLIPBOARD, NULL);
		the->cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
		g_signal_connect(the->cb, "owner-change",
				 G_CALLBACK(owner_change), the);
		gtk_clipboard_request_targets(the->cb, got_targets,
					      g_object_ref(the));
	}
	return the;
}

static void get_func(GtkClipboard *cb, GtkSelectionData *sd, guint info,
		     gpointer data)
{
	ExClipboard *c = data;
	GString *s = g_string_new(NULL);

	if (!c->files)
		return;
	if (info == TARGET_GNOME)
		g_string_append(s, c->cut ? "cut" : "copy");
	for (guint i = 0; i < c->files->len; i++) {
		GFile *f = g_ptr_array_index(c->files, i);

		if (info == TARGET_TEXT) {
			char *p = g_file_get_path(f);
			char *u = p ? NULL : g_file_get_uri(f);

			if (s->len)
				g_string_append_c(s, '\n');
			g_string_append(s, p ? p : u);
			g_free(p);
			g_free(u);
		} else {
			char *u = g_file_get_uri(f);

			if (info == TARGET_GNOME)
				g_string_append_c(s, '\n');
			g_string_append(s, u);
			if (info == TARGET_URIS)
				g_string_append(s, "\r\n");
			g_free(u);
		}
	}
	if (info == TARGET_TEXT)
		gtk_selection_data_set_text(sd, s->str, (int)s->len);
	else
		gtk_selection_data_set(sd, gtk_selection_data_get_target(sd), 8,
				       (const guchar *)s->str, (int)s->len);
	g_string_free(s, TRUE);
}

static void clear_func(GtkClipboard *cb, gpointer data)
{
	ExClipboard *c = data;

	c->owner = FALSE;
	g_clear_pointer(&c->files, g_ptr_array_unref);
	c->cut = FALSE;
	g_signal_emit(c, sigs[SIG_CHANGED], 0);
}

static void put(ExClipboard *c, GPtrArray *files, gboolean cut)
{
	static const GtkTargetEntry targets[] = {
		{ (char *)GNOME_FILES, 0, TARGET_GNOME },
		{ (char *)"text/uri-list", 0, TARGET_URIS },
		{ (char *)"UTF8_STRING", 0, TARGET_TEXT },
		{ (char *)"text/plain;charset=utf-8", 0, TARGET_TEXT },
		{ (char *)"text/plain", 0, TARGET_TEXT },
	};
	GPtrArray *copy = g_ptr_array_new_with_free_func(g_object_unref);

	for (guint i = 0; i < files->len; i++)
		g_ptr_array_add(copy, g_object_ref(g_ptr_array_index(files, i)));
	if (gtk_clipboard_set_with_owner(c->cb, targets, G_N_ELEMENTS(targets),
					 get_func, clear_func, G_OBJECT(c))) {
		/* The previous owner -- possibly us -- was cleared first. */
		g_clear_pointer(&c->files, g_ptr_array_unref);
		c->files = copy;
		c->cut = cut;
		c->owner = TRUE;
		gtk_clipboard_set_can_store(c->cb, NULL, 0);
		c->can_paste = TRUE;
		g_signal_emit(c, sigs[SIG_CHANGED], 0);
	} else {
		g_ptr_array_unref(copy);
	}
}

void ex_clipboard_copy(ExClipboard *c, GPtrArray *files)
{
	put(c, files, FALSE);
}

void ex_clipboard_cut(ExClipboard *c, GPtrArray *files)
{
	put(c, files, TRUE);
}

void ex_clipboard_copy_paths(ExClipboard *c, GPtrArray *files)
{
	GString *s = g_string_new(NULL);

	for (guint i = 0; i < files->len; i++) {
		char *p = g_file_get_path(g_ptr_array_index(files, i));

		if (!p)
			continue;
		if (s->len)
			g_string_append_c(s, '\n');
		/* Quoted when it has a space, as Windows' "Copy as path". */
		if (strchr(p, ' '))
			g_string_append_printf(s, "\"%s\"", p);
		else
			g_string_append(s, p);
		g_free(p);
	}
	gtk_clipboard_set_text(c->cb, s->str, (int)s->len);
	g_string_free(s, TRUE);
}

void ex_clipboard_refresh(ExClipboard *c)
{
	gtk_clipboard_request_targets(c->cb, got_targets, g_object_ref(c));
}

gboolean ex_clipboard_can_paste(ExClipboard *c)
{
	return c->can_paste;
}

gboolean ex_clipboard_is_cut(ExClipboard *c, GFile *f)
{
	if (!c->owner || !c->cut || !c->files)
		return FALSE;
	for (guint i = 0; i < c->files->len; i++)
		if (g_file_equal(g_ptr_array_index(c->files, i), f))
			return TRUE;
	return FALSE;
}

typedef struct {
	ExPasteFunc done;
	gpointer data;
	gboolean tried_uris;
	ExClipboard *c;
} Req;

/* "copy\nuri\nuri" or a text/uri-list. */
static GPtrArray *parse(const char *text, gboolean gnome, gboolean *cut)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_object_unref);
	char **lines = g_strsplit(text, "\n", -1);
	int i = 0;

	*cut = FALSE;
	if (gnome && lines[0]) {
		*cut = g_str_has_prefix(lines[0], "cut");
		i = 1;
	}
	for (; lines[i]; i++) {
		char *l = g_strstrip(lines[i]);

		if (!*l || l[0] == '#')
			continue;
		g_ptr_array_add(a, g_file_new_for_uri(l));
	}
	g_strfreev(lines);
	if (a->len == 0) {
		g_ptr_array_unref(a);
		return NULL;
	}
	return a;
}

static void got_contents(GtkClipboard *cb, GtkSelectionData *sd, gpointer data)
{
	Req *r = data;
	const guchar *raw = sd ? gtk_selection_data_get_data(sd) : NULL;
	int len = sd ? gtk_selection_data_get_length(sd) : -1;

	if (raw && len > 0) {
		char *text = g_strndup((const char *)raw, (gsize)len);
		gboolean cut = FALSE;
		GPtrArray *files = parse(text, !r->tried_uris, &cut);

		g_free(text);
		if (files) {
			r->done(files, cut, r->data);
			g_ptr_array_unref(files);
			g_object_unref(r->c);
			g_free(r);
			return;
		}
	}
	if (!r->tried_uris) {
		r->tried_uris = TRUE;
		gtk_clipboard_request_contents(
			cb, gdk_atom_intern_static_string("text/uri-list"),
			got_contents, r);
		return;
	}
	r->done(NULL, FALSE, r->data);
	g_object_unref(r->c);
	g_free(r);
}

void ex_clipboard_request(ExClipboard *c, ExPasteFunc done, gpointer user_data)
{
	Req *r = g_new0(Req, 1);

	r->done = done;
	r->data = user_data;
	r->c = g_object_ref(c);
	gtk_clipboard_request_contents(
		c->cb, gdk_atom_intern_static_string(GNOME_FILES), got_contents,
		r);
}

void ex_clipboard_clear_cut(ExClipboard *c)
{
	if (c->owner && c->cut)
		gtk_clipboard_clear(c->cb);
}
