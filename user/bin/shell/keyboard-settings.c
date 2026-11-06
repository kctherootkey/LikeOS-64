// LikeOS -- likeos-keyboard-settings, the Keyboard settings window.
//
// The keyboard layouts the desktop types with, in order: the first is the
// primary layout, the one a session starts in and the one the text console
// uses as well; the others are a shortcut away (XKB's layout groups, at
// most four).  The window lists them with buttons to move each up or down
// or remove it, offers every layout of the catalogue to add -- a searchable
// list of names and languages -- and, once there is more than one, the
// shortcut that goes to the next.  A field at the bottom is for trying the
// keyboard out, and says which of the layouts it is typing in.
//
// Nothing changes until Apply.  Apply hands the setting to the setuid
// helper likeos-keyboard-save, which checks it against the catalogue,
// writes /etc/likeos/keyboard.conf and loads the primary layout's keymap
// into the console, and then runs likeos-xkb-apply, which sets the layouts
// in this X session.  Other sessions -- and this one, should the console
// change the setting with loadkeys -- follow through the taskbar, which
// watches the file (taskbar.c, "the keyboard setting").  A change made
// elsewhere while this window is open is shown at once, unless there are
// edits here that are not applied yet.
//
// The setting's options (XKB's -option list) may hold more than the
// layout-switching shortcut: whatever else is there is kept as it is.
//
// A layout with no Latin letters (Russian, Greek, Thai, ...) that is the only
// one set is given English (US) as a second layout wherever it is used --
// in the X session by likeos-xkb-apply, on the console by its keymap -- so
// a web address or a command can still be typed.  The window says so, and
// offers the shortcut for that pair as it does for layouts added here.
//
// Copyright (C) 2026 The LikeOS Project

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>
#include <stdio.h>
#include <string.h>

#include "keyboard-conf.h"

#ifndef KBD_SAVE_HELPER
#define KBD_SAVE_HELPER "/usr/libexec/likeos-keyboard-save"
#endif
#ifndef KBD_XKB_APPLY
#define KBD_XKB_APPLY "/usr/bin/likeos-xkb-apply"
#endif

#define TITLE "Keyboard settings"

/* The shortcuts offered for going to the next layout: XKB's grp: options
 * with their names in words.  The first is the one a second layout starts
 * with. */
static const struct {
	const char *option, *label;
} shortcuts[] = {
	{ "grp:alt_shift_toggle", "Alt+Shift" },
	{ "grp:ctrl_shift_toggle", "Ctrl+Shift" },
	{ "grp:win_space_toggle", "Super+Space" },
	{ "grp:ctrl_space_toggle", "Ctrl+Space" },
	{ "grp:alt_space_toggle", "Alt+Space" },
	{ "grp:caps_toggle", "Caps Lock" },
	{ "grp:shift_caps_toggle", "Shift+Caps Lock" },
	{ "grp:alt_caps_toggle", "Alt+Caps Lock" },
	{ "grp:shifts_toggle", "Both Shift keys together" },
	{ "grp:alts_toggle", "Both Alt keys together" },
	{ "grp:ctrls_toggle", "Both Ctrl keys together" },
	{ "grp:lalt_lshift_toggle", "Left Alt+Left Shift" },
	{ "grp:lctrl_lshift_toggle", "Left Ctrl+Left Shift" },
	{ "grp:toggle", "Right Alt" },
	{ "grp:menu_toggle", "Menu key" },
	{ "grp:sclk_toggle", "Scroll Lock" },
};

static struct {
	GPtrArray *cat;		/* the catalogue */
	struct kbd_conf saved;	/* what keyboard.conf says */

	/* The setting as edited here. */
	int n;
	char *layout[KBD_MAX_LAYOUTS], *variant[KBD_MAX_LAYOUTS];
	char *shortcut;		/* the grp: option chosen */
	char *other_options;	/* the setting's other options, kept */

	GtkWidget *win, *list, *add, *combo, *shortcut_row, *shortcut_note;
	GtkWidget *note;	/* under the layouts: what being first means */
	GtkWidget *apply, *status, *test, *typing;
	GFileMonitor *monitor;
} K;

static void add_class(GtkWidget *w, const char *cls)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

static GtkWidget *label_new(const char *text, const char *cls)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	if (cls)
		add_class(l, cls);
	return l;
}

/* ---- the setting ---- */

/* The setting's options split in two: its grp: option (the first, if it
 * has several -- XKB acts on one), and everything else. */
static void split_options(const char *options, char **grp, char **other)
{
	char **parts = g_strsplit(options ? options : "", ",", -1);
	GString *rest = g_string_new(NULL);

	*grp = NULL;
	for (char **p = parts; *p; p++) {
		if (!**p)
			continue;
		if (g_str_has_prefix(*p, "grp:")) {
			if (!*grp)
				*grp = g_strdup(*p);
			continue;
		}
		if (rest->len)
			g_string_append_c(rest, ',');
		g_string_append(rest, *p);
	}
	g_strfreev(parts);
	*other = g_string_free(rest, FALSE);
}

/* The edited setting from the saved one. */
static void edit_from_saved(void)
{
	char *grp;

	for (int i = 0; i < K.n; i++) {
		g_free(K.layout[i]);
		g_free(K.variant[i]);
	}
	K.n = K.saved.n;
	for (int i = 0; i < K.n; i++) {
		K.layout[i] = g_strdup(K.saved.layout[i]);
		K.variant[i] = g_strdup(K.saved.variant[i]);
	}
	g_free(K.shortcut);
	g_free(K.other_options);
	split_options(K.saved.options, &grp, &K.other_options);
	K.shortcut = grp ? grp : g_strdup(shortcuts[0].option);
}

/* A single layout with no Latin letters: English (US) is added to it. */
static gboolean latin_added(int n, char **layout, char **variant)
{
	return n == 1 && kbd_layout_nonlatin(K.cat, layout[0], variant[0]);
}

/* Whether there is something to switch between, edited or saved. */
static gboolean edit_switches(void)
{
	return K.n > 1 || latin_added(K.n, K.layout, K.variant);
}

static gboolean saved_switches(void)
{
	return K.saved.n > 1 ||
	       latin_added(K.saved.n, K.saved.layout, K.saved.variant);
}

/* The three arguments of likeos-keyboard-save, as the file will hold them:
 * the layouts, their variants (one entry per layout, empty for none) and
 * the options -- the shortcut only while there is something to switch
 * between (which a lone layout without Latin letters has). */
static void edit_strings(char **layout, char **variant, char **options)
{
	GString *l = g_string_new(NULL), *v = g_string_new(NULL);
	GString *o = g_string_new(K.other_options);

	for (int i = 0; i < K.n; i++) {
		if (i) {
			g_string_append_c(l, ',');
			g_string_append_c(v, ',');
		}
		g_string_append(l, K.layout[i]);
		g_string_append(v, K.variant[i]);
	}
	if (edit_switches() && K.shortcut && *K.shortcut) {
		if (o->len)
			g_string_append_c(o, ',');
		g_string_append(o, K.shortcut);
	}
	*layout = g_string_free(l, FALSE);
	*variant = g_string_free(v, FALSE);
	*options = g_string_free(o, FALSE);
}

/* Whether the edits differ from what is saved.  The saved setting is
 * compared the same way it would be written. */
static gboolean edited(void)
{
	char *l, *v, *o, *sg, *so, *sl, *sv;
	gboolean diff;
	GString *a = g_string_new(NULL), *b = g_string_new(NULL);

	edit_strings(&l, &v, &o);
	for (int i = 0; i < K.saved.n; i++) {
		if (i) {
			g_string_append_c(a, ',');
			g_string_append_c(b, ',');
		}
		g_string_append(a, K.saved.layout[i]);
		g_string_append(b, K.saved.variant[i]);
	}
	sl = g_string_free(a, FALSE);
	sv = g_string_free(b, FALSE);
	split_options(K.saved.options, &sg, &so);
	diff = strcmp(l, sl) != 0 || strcmp(v, sv) != 0;
	if (!diff) {
		/* The options as a set of the other options plus the
		 * shortcut, which only counts with something to switch
		 * between.  A lone layout without Latin letters that names
		 * no shortcut has the first of the list (likeos-xkb-apply
		 * gives it that one). */
		char *eg, *eo;

		split_options(o, &eg, &eo);
		if (!sg && K.n == 1 && edit_switches())
			sg = g_strdup(shortcuts[0].option);
		diff = strcmp(eo, so) != 0 ||
		       (edit_switches() && g_strcmp0(eg, sg) != 0);
		g_free(eg);
		g_free(eo);
	}
	g_free(l);
	g_free(v);
	g_free(o);
	g_free(sl);
	g_free(sv);
	g_free(sg);
	g_free(so);
	return diff;
}

static void status_set(const char *text, const char *cls)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(K.status);

	gtk_style_context_remove_class(sc, "st-status-ok");
	gtk_style_context_remove_class(sc, "st-status-error");
	if (cls)
		gtk_style_context_add_class(sc, cls);
	gtk_label_set_text(GTK_LABEL(K.status), text ? text : "");
}

/* Apply is there for edits, and the footer says that there are some. */
static void apply_update(void)
{
	gboolean dirty = edited();

	gtk_widget_set_sensitive(K.apply, dirty);
	status_set(dirty ? "Changes not applied yet" : NULL, NULL);
}

/* ---- the test field ---- */

/* Which of the session's layouts the keyboard types in now: XKB's current
 * group, named from the applied setting. */
static void typing_update(void)
{
	Display *dpy = gdk_x11_display_get_xdisplay(gdk_display_get_default());
	XkbStateRec st;
	char *name, *text;
	int g = 0, n = K.saved.n;

	/* A lone layout without Latin letters runs with English (US) as
	 * the second group. */
	if (saved_switches() && n == 1)
		n = 2;
	if (XkbGetState(dpy, XkbUseCoreKbd, &st) == Success)
		g = st.group;
	if (g < 0 || g >= n)
		g = 0;
	if (g < K.saved.n)
		name = kbd_layout_label(K.cat, K.saved.layout[g],
					K.saved.variant[g]);
	else
		name = kbd_layout_label(K.cat, "us", "");
	if (n > 1)
		text = g_strdup_printf("Typing in %s, layout %d of %d", name,
				       g + 1, n);
	else
		text = g_strdup_printf("Typing in %s", name);
	gtk_label_set_text(GTK_LABEL(K.typing), text);
	g_free(text);
	g_free(name);
}

static gboolean on_test_key(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)w;
	(void)ev;
	(void)d;
	typing_update();
	return FALSE;
}

/* ---- the list of layouts ---- */

static void refresh(void);

static void on_move(GtkButton *b, gpointer data)
{
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index"));
	int to = i + GPOINTER_TO_INT(data);

	if (to < 0 || to >= K.n)
		return;

	char *l = K.layout[i], *v = K.variant[i];

	K.layout[i] = K.layout[to];
	K.variant[i] = K.variant[to];
	K.layout[to] = l;
	K.variant[to] = v;
	refresh();
}

static void on_remove(GtkButton *b, gpointer data)
{
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index"));

	(void)data;
	if (K.n <= 1 || i < 0 || i >= K.n)
		return;
	g_free(K.layout[i]);
	g_free(K.variant[i]);
	for (int k = i; k < K.n - 1; k++) {
		K.layout[k] = K.layout[k + 1];
		K.variant[k] = K.variant[k + 1];
	}
	K.n--;
	refresh();
}

/* A flat round button with a symbolic icon, for a row. */
static GtkWidget *row_button(const char *icon, const char *tip, int index,
			     gboolean sensitive, GCallback cb, gpointer data)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);

	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	gtk_widget_set_sensitive(b, sensitive);
	add_class(b, "st-icon-button");
	g_object_set_data(G_OBJECT(b), "index", GINT_TO_POINTER(index));
	g_signal_connect(b, "clicked", cb, data);
	return b;
}

/* The short code on a layout's badge: its XKB layout, upper case, at most
 * three letters (DE, US, ARA). */
static char *badge_text(const char *layout)
{
	char *up = g_ascii_strup(layout, -1);

	if (strlen(up) > 3)
		up[3] = '\0';
	return up;
}

/* One layout of the list: its badge, its name over its language (the
 * primary one says what being first means), and the buttons that move and
 * remove it. */
static GtkWidget *layout_row(int i)
{
	const struct kbd_layout *l =
		kbd_catalogue_find(K.cat, K.layout[i], K.variant[i]);
	char *name = kbd_layout_label(K.cat, K.layout[i], K.variant[i]);
	char *code = badge_text(K.layout[i]);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *badge = gtk_label_new(code);
	GtkWidget *detail;

	add_class(row, "st-row");
	add_class(badge, "st-badge");
	if (i == 0)
		add_class(badge, "st-badge-primary");
	gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), badge, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(head), label_new(name, "st-row-title"),
			   FALSE, FALSE, 0);
	if (i == 0) {
		GtkWidget *pill = gtk_label_new("Primary");

		add_class(pill, "st-pill");
		gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(head), pill, FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(col), head, FALSE, FALSE, 0);
	if (i == 0) {
		char *t = g_strdup_printf("%s \xc2\xb7 Also used on the text "
					  "console",
					  l ? l->language : "Not in the list");

		detail = label_new(t, "st-row-detail");
		g_free(t);
	} else {
		detail = label_new(l ? l->language : "Not in the list",
				   "st-row-detail");
	}
	gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(col), detail, FALSE, FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);

	gtk_box_pack_end(GTK_BOX(row),
			 row_button("edit-delete-symbolic",
				    K.n > 1 ? "Remove" :
					      "The last layout cannot be "
					      "removed",
				    i, K.n > 1, G_CALLBACK(on_remove), NULL),
			 FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row),
			 row_button("go-down-symbolic", "Move down", i,
				    i < K.n - 1, G_CALLBACK(on_move),
				    GINT_TO_POINTER(1)),
			 FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row),
			 row_button("go-up-symbolic",
				    i == 1 ? "Make this the primary layout" :
					     "Move up",
				    i, i > 0, G_CALLBACK(on_move),
				    GINT_TO_POINTER(-1)),
			 FALSE, FALSE, 0);
	g_free(name);
	g_free(code);
	return row;
}

/* Everything that follows from the edited setting: the rows, what can be
 * added, whether the shortcut means anything, and Apply. */
static void refresh(void)
{
	GList *kids = gtk_container_get_children(GTK_CONTAINER(K.list));

	for (GList *k = kids; k; k = k->next)
		gtk_widget_destroy(k->data);
	g_list_free(kids);
	for (int i = 0; i < K.n; i++) {
		GtkWidget *r = gtk_list_box_row_new();

		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
		gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(r), FALSE);
		gtk_container_add(GTK_CONTAINER(r), layout_row(i));
		gtk_container_add(GTK_CONTAINER(K.list), r);
	}
	gtk_widget_show_all(K.list);

	gtk_widget_set_sensitive(K.add, K.n < KBD_MAX_LAYOUTS);
	gtk_widget_set_tooltip_text(
		K.add, K.n < KBD_MAX_LAYOUTS ?
			       NULL :
			       "Up to four layouts can be in use at a time");
	gtk_widget_set_sensitive(K.shortcut_row, edit_switches());
	gtk_label_set_text(GTK_LABEL(K.shortcut_note),
			   K.n > 1 ? "Goes to the next layout, and from the "
				     "last back to the first" :
			   edit_switches() ?
				     "Goes to English (US) and back" :
				     "Add a second layout to switch between "
				     "them");
	if (latin_added(K.n, K.layout, K.variant)) {
		char *name = kbd_layout_label(K.cat, K.layout[0],
					      K.variant[0]);
		char *t = g_strdup_printf(
			"%s has no Latin letters, so English (US) is added "
			"as a second layout, on the desktop and on the text "
			"console, for web addresses, file names and commands.",
			name);

		gtk_label_set_text(GTK_LABEL(K.note), t);
		g_free(t);
		g_free(name);
	} else {
		gtk_label_set_text(GTK_LABEL(K.note),
				   "The first layout is the primary one: the "
				   "desktop starts in it, and the text console "
				   "uses it.");
	}
	apply_update();
}

/* ---- adding a layout ---- */

/* The add dialog's list: a row per catalogue entry not in use already,
 * each carrying its entry; the search filters them. */
struct add_dialog {
	GtkWidget *dlg, *search, *list;
	char *needle; /* the search, case-folded */
};

static gboolean in_use(const struct kbd_layout *l)
{
	for (int i = 0; i < K.n; i++)
		if (strcmp(K.layout[i], l->layout) == 0 &&
		    strcmp(K.variant[i], l->variant) == 0)
			return TRUE;
	return FALSE;
}

static gboolean contains_folded(const char *hay, const char *needle)
{
	char *f = g_utf8_casefold(hay, -1);
	gboolean hit = strstr(f, needle) != NULL;

	g_free(f);
	return hit;
}

static gboolean add_filter(GtkListBoxRow *row, gpointer data)
{
	struct add_dialog *A = data;
	const struct kbd_layout *l = g_object_get_data(G_OBJECT(row), "layout");

	if (!A->needle || !*A->needle)
		return TRUE;
	return contains_folded(l->name, A->needle) ||
	       contains_folded(l->language, A->needle) ||
	       contains_folded(l->id, A->needle);
}

/* How well a layout answers the search, best first: its name starts with
 * it, a word of its name does, its name has it somewhere, its language or
 * its code does.  Rows the search does not find are filtered out anyway. */
static int add_rank(const struct kbd_layout *l, const char *needle)
{
	char *name;
	int rank = 4;

	if (!needle || !*needle)
		return 0;
	name = g_utf8_casefold(l->name, -1);
	if (g_str_has_prefix(name, needle)) {
		rank = 0;
	} else {
		const char *hit = strstr(name, needle);

		if (hit && (hit[-1] == ' ' || hit[-1] == '('))
			rank = 1;
		else if (hit)
			rank = 2;
		else if (contains_folded(l->language, needle))
			rank = 3;
	}
	g_free(name);
	return rank;
}

static gint add_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data)
{
	struct add_dialog *A = data;
	const struct kbd_layout *la = g_object_get_data(G_OBJECT(a), "layout");
	const struct kbd_layout *lb = g_object_get_data(G_OBJECT(b), "layout");
	int ra = add_rank(la, A->needle), rb = add_rank(lb, A->needle);

	if (ra != rb)
		return ra - rb;
	/* Otherwise the catalogue's order, which is by name. */
	return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a), "order")) -
	       GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "order"));
}

static void on_add_search(GtkSearchEntry *e, gpointer data)
{
	struct add_dialog *A = data;
	GtkListBoxRow *first = NULL;

	g_free(A->needle);
	A->needle = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(e)), -1);
	gtk_list_box_invalidate_filter(GTK_LIST_BOX(A->list));
	gtk_list_box_invalidate_sort(GTK_LIST_BOX(A->list));

	/* The best answer chosen, so Enter adds what is at the top. */
	for (int i = 0;; i++) {
		GtkListBoxRow *r =
			gtk_list_box_get_row_at_index(GTK_LIST_BOX(A->list), i);

		if (!r)
			break;
		if (add_filter(r, A)) {
			first = r;
			break;
		}
	}
	if (first)
		gtk_list_box_select_row(GTK_LIST_BOX(A->list), first);
	/* Nothing found: the row still selected is out of sight, and there
	 * is nothing to add. */
	gtk_dialog_set_response_sensitive(GTK_DIALOG(A->dlg), GTK_RESPONSE_OK,
					  first != NULL);
}

static void on_add_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
	struct add_dialog *A = data;

	(void)lb;
	gtk_dialog_set_response_sensitive(GTK_DIALOG(A->dlg), GTK_RESPONSE_OK,
					  row != NULL);
}

static void on_add_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
	struct add_dialog *A = data;

	(void)lb;
	(void)row;
	gtk_dialog_response(GTK_DIALOG(A->dlg), GTK_RESPONSE_OK);
}

/* Enter in the search adds the layout chosen; the arrows move the choice
 * without leaving the search. */
static gboolean on_add_search_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	struct add_dialog *A = data;
	GtkListBox *lb = GTK_LIST_BOX(A->list);
	GtkListBoxRow *sel = gtk_list_box_get_selected_row(lb);
	int step;

	(void)w;
	switch (ev->keyval) {
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		if (sel && add_filter(sel, A))
			gtk_dialog_response(GTK_DIALOG(A->dlg),
					    GTK_RESPONSE_OK);
		return TRUE;
	case GDK_KEY_Down:
	case GDK_KEY_KP_Down:
		step = 1;
		break;
	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
		step = -1;
		break;
	default:
		return FALSE;
	}

	int i = sel ? gtk_list_box_row_get_index(sel) : -1;

	for (i += step; i >= 0; i += step) {
		GtkListBoxRow *r = gtk_list_box_get_row_at_index(lb, i);

		if (!r)
			break;
		if (add_filter(r, A)) {
			gtk_list_box_select_row(lb, r);
			/* Scroll it into view. */
			GtkAdjustment *adj = gtk_list_box_get_adjustment(lb);
			GtkAllocation a;

			gtk_widget_get_allocation(GTK_WIDGET(r), &a);
			if (adj && a.y < gtk_adjustment_get_value(adj))
				gtk_adjustment_set_value(adj, a.y);
			else if (adj && a.y + a.height >
						gtk_adjustment_get_value(adj) +
							gtk_adjustment_get_page_size(adj))
				gtk_adjustment_set_value(
					adj, a.y + a.height -
						     gtk_adjustment_get_page_size(adj));
			break;
		}
	}
	return TRUE;
}

static GtkWidget *add_row(const struct kbd_layout *l)
{
	GtkWidget *r = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
	char *code = badge_text(l->layout);
	GtkWidget *badge = gtk_label_new(code);

	add_class(box, "st-pick-row");
	add_class(badge, "st-badge");
	gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(box), badge, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), label_new(l->name, "st-row-title"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col),
			   label_new(l->language, "st-row-detail"), FALSE,
			   FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(box), col, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(r), box);
	g_object_set_data(G_OBJECT(r), "layout", (gpointer)l);
	g_free(code);
	return r;
}

static void on_add_clicked(GtkButton *b, gpointer data)
{
	struct add_dialog A = { 0 };
	GtkWidget *area, *scroll, *empty;
	int shown = 0;

	(void)b;
	(void)data;
	A.dlg = gtk_dialog_new_with_buttons("Add a keyboard layout",
					    GTK_WINDOW(K.win),
					    GTK_DIALOG_MODAL |
						    GTK_DIALOG_DESTROY_WITH_PARENT,
					    "Cancel", GTK_RESPONSE_CANCEL,
					    "Add", GTK_RESPONSE_OK, NULL);
	add_class(A.dlg, "st-window");
	add_class(A.dlg, "st-add-dialog");
	gtk_window_set_default_size(GTK_WINDOW(A.dlg), 460, 560);
	gtk_dialog_set_default_response(GTK_DIALOG(A.dlg), GTK_RESPONSE_OK);
	gtk_dialog_set_response_sensitive(GTK_DIALOG(A.dlg), GTK_RESPONSE_OK,
					  FALSE);
	add_class(gtk_dialog_get_widget_for_response(GTK_DIALOG(A.dlg),
						     GTK_RESPONSE_OK),
		  "suggested-action");

	area = gtk_dialog_get_content_area(GTK_DIALOG(A.dlg));
	add_class(area, "st-add-page");
	gtk_box_set_spacing(GTK_BOX(area), 0);

	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

	add_class(head, "st-add-head");
	gtk_box_pack_start(GTK_BOX(head),
			   label_new("Choose a layout", "st-add-title"), FALSE,
			   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(head),
			   label_new("Search by layout name or by language",
				     "st-row-detail"),
			   FALSE, FALSE, 0);
	A.search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(A.search),
				       "Search, for example German or Thai");
	add_class(A.search, "st-search");
	gtk_box_pack_start(GTK_BOX(head), A.search, FALSE, FALSE, 8);
	gtk_box_pack_start(GTK_BOX(area), head, FALSE, FALSE, 0);

	A.list = gtk_list_box_new();
	add_class(A.list, "st-pick-list");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(A.list),
					GTK_SELECTION_BROWSE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(A.list), FALSE);
	for (guint i = 0; i < K.cat->len; i++) {
		const struct kbd_layout *l = g_ptr_array_index(K.cat, i);

		if (in_use(l))
			continue;
		GtkWidget *r = add_row(l);

		g_object_set_data(G_OBJECT(r), "order", GINT_TO_POINTER(i));
		gtk_container_add(GTK_CONTAINER(A.list), r);
		shown++;
	}
	empty = label_new(K.cat->len ? "No layout matches the search" :
				       "The list of layouts is missing",
			  "st-empty");
	gtk_label_set_xalign(GTK_LABEL(empty), 0.5);
	gtk_widget_show(empty);
	gtk_list_box_set_placeholder(GTK_LIST_BOX(A.list), empty);
	gtk_list_box_set_filter_func(GTK_LIST_BOX(A.list), add_filter, &A,
				     NULL);
	gtk_list_box_set_sort_func(GTK_LIST_BOX(A.list), add_sort, &A, NULL);
	g_signal_connect(A.list, "row-selected", G_CALLBACK(on_add_selected),
			 &A);
	g_signal_connect(A.list, "row-activated", G_CALLBACK(on_add_activated),
			 &A);
	g_signal_connect(A.search, "search-changed", G_CALLBACK(on_add_search),
			 &A);
	g_signal_connect(A.search, "key-press-event",
			 G_CALLBACK(on_add_search_key), &A);

	scroll = gtk_scrolled_window_new(NULL, NULL);
	add_class(scroll, "st-pick-scroll");
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), A.list);
	gtk_box_pack_start(GTK_BOX(area), scroll, TRUE, TRUE, 0);

	gtk_widget_show_all(A.dlg);
	gtk_widget_grab_focus(A.search);
	if (shown)
		gtk_list_box_select_row(
			GTK_LIST_BOX(A.list),
			gtk_list_box_get_row_at_index(GTK_LIST_BOX(A.list), 0));

	if (gtk_dialog_run(GTK_DIALOG(A.dlg)) == GTK_RESPONSE_OK &&
	    K.n < KBD_MAX_LAYOUTS) {
		GtkListBoxRow *sel =
			gtk_list_box_get_selected_row(GTK_LIST_BOX(A.list));
		const struct kbd_layout *l =
			sel ? g_object_get_data(G_OBJECT(sel), "layout") : NULL;

		if (l && add_filter(sel, &A)) {
			K.layout[K.n] = g_strdup(l->layout);
			K.variant[K.n] = g_strdup(l->variant);
			K.n++;
			refresh();
		}
	}
	gtk_widget_destroy(A.dlg);
	g_free(A.needle);
}

/* ---- applying ---- */

static void message(GtkMessageType type, const char *primary,
		    const char *secondary)
{
	GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(K.win),
					      GTK_DIALOG_MODAL, type,
					      GTK_BUTTONS_OK, "%s", primary);

	if (secondary && *secondary)
		gtk_message_dialog_format_secondary_text(
			GTK_MESSAGE_DIALOG(m), "%s", secondary);
	gtk_window_set_title(GTK_WINDOW(m), TITLE);
	gtk_dialog_run(GTK_DIALOG(m));
	gtk_widget_destroy(m);
}

/* Run a program and wait for it.  On failure, `why' is what it said on its
 * error output, or else why it could not run or how it ended. */
static gboolean run(char **argv, char **why)
{
	GError *err = NULL;
	char *errout = NULL;
	gint status = 0;
	gboolean ok;

	*why = NULL;
	ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL,
			  NULL, NULL, &errout, &status, &err) &&
	     g_spawn_check_wait_status(status, &err);
	if (!ok) {
		if (errout && *g_strstrip(errout))
			*why = g_strdup(errout);
		else
			*why = g_strdup(err ? err->message : "It failed.");
	}
	g_clear_error(&err);
	g_free(errout);
	return ok;
}

/* The saved setting in one string, to tell whether a reading changed it. */
static char *saved_key(void)
{
	GString *k = g_string_new(NULL);

	for (int i = 0; i < K.saved.n; i++)
		g_string_append_printf(k, "%s(%s),", K.saved.layout[i],
				       K.saved.variant[i]);
	g_string_append(k, K.saved.options);
	return g_string_free(k, FALSE);
}

static void reload(void)
{
	kbd_conf_clear(&K.saved);
	kbd_conf_read(&K.saved);
}

static void on_apply(GtkButton *b, gpointer data)
{
	char *layout, *variant, *options, *why = NULL;

	(void)b;
	(void)data;
	edit_strings(&layout, &variant, &options);

	char *save[] = { (char *)KBD_SAVE_HELPER, layout, variant, options,
			 NULL };
	char *xkb[] = { (char *)KBD_XKB_APPLY, NULL };

	if (!run(save, &why)) {
		status_set("Not applied", "st-status-error");
		message(GTK_MESSAGE_ERROR,
			"The keyboard layouts could not be saved.", why);
	} else {
		reload();
		edit_from_saved();
		refresh();
		if (!run(xkb, &why)) {
			status_set("Saved, but not in use in this session",
				   "st-status-error");
			message(GTK_MESSAGE_WARNING,
				"The keyboard layouts were saved, but this "
				"session could not switch to them.",
				why);
		} else {
			status_set("Applied", "st-status-ok");
		}
		typing_update();
	}
	g_free(why);
	g_free(layout);
	g_free(variant);
	g_free(options);
}

/* keyboard.conf was replaced -- by loadkeys at the console, or by another
 * Keyboard settings window: show it, unless there are edits here. */
static void on_conf_changed(GFileMonitor *m, GFile *f, GFile *other,
			    GFileMonitorEvent ev, gpointer data)
{
	char *base = g_file_get_basename(f);
	char *obase = other ? g_file_get_basename(other) : NULL;
	gboolean ours = g_strcmp0(base, KBD_CONF_NAME) == 0 ||
			g_strcmp0(obase, KBD_CONF_NAME) == 0;

	(void)m;
	(void)data;
	g_free(base);
	g_free(obase);
	if (!ours || (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
		      ev != G_FILE_MONITOR_EVENT_CREATED &&
		      ev != G_FILE_MONITOR_EVENT_RENAMED &&
		      ev != G_FILE_MONITOR_EVENT_MOVED_IN))
		return;

	gboolean dirty = edited();
	char *before = saved_key(), *after;

	reload();
	after = saved_key();
	/* The file as this window last wrote or read it: nothing new. */
	if (strcmp(before, after) != 0) {
		if (!dirty)
			edit_from_saved();
		refresh();
		typing_update();
	}
	g_free(before);
	g_free(after);
}

/* ---- the window ---- */

static void on_shortcut_changed(GtkComboBox *c, gpointer data)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)data;
	if (!id)
		return;
	g_free(K.shortcut);
	K.shortcut = g_strdup(id);
	apply_update();
}

/* A heading over a card, and the card. */
static GtkWidget *section(GtkWidget *page, const char *title)
{
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	gtk_box_pack_start(GTK_BOX(page), label_new(title, "st-heading"),
			   FALSE, FALSE, 0);
	add_class(card, "st-card");
	gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
	return card;
}

static GtkWidget *build_shortcut_row(void)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *title = label_new("Switch layouts with", "st-row-title");
	gboolean known = FALSE;

	add_class(row, "st-row");
	K.shortcut_note = label_new("", "st-row-detail");
	gtk_label_set_line_wrap(GTK_LABEL(K.shortcut_note), TRUE);
	gtk_box_pack_start(GTK_BOX(col), title, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), K.shortcut_note, FALSE, FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);

	K.combo = gtk_combo_box_text_new();
	for (guint i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(K.combo),
					  shortcuts[i].option,
					  shortcuts[i].label);
		if (strcmp(shortcuts[i].option, K.shortcut) == 0)
			known = TRUE;
	}
	/* A shortcut set some other way stays on offer, by its XKB name. */
	if (!known)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(K.combo),
					  K.shortcut, K.shortcut + 4);
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(K.combo), K.shortcut);
	gtk_widget_set_valign(K.combo, GTK_ALIGN_CENTER);
	gtk_widget_set_size_request(K.combo, 200, -1);
	gtk_label_set_mnemonic_widget(GTK_LABEL(title), K.combo);
	g_signal_connect(K.combo, "changed", G_CALLBACK(on_shortcut_changed),
			 NULL);
	gtk_box_pack_end(GTK_BOX(row), K.combo, FALSE, FALSE, 0);
	K.shortcut_row = row;
	return row;
}

static void build_window(void)
{
	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *htext = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *card, *close, *icon, *box;

	K.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(K.win), TITLE);
	gtk_window_set_icon_name(GTK_WINDOW(K.win),
				 "preferences-desktop-keyboard");
	gtk_window_set_position(GTK_WINDOW(K.win), GTK_WIN_POS_CENTER);
	gtk_window_set_default_size(GTK_WINDOW(K.win), 600, 680);
	add_class(K.win, "st-window");
	add_class(K.win, "st-keyboard");

	/* The header on white: the picture, the title, what it is for. */
	add_class(header, "st-header");
	icon = gtk_image_new_from_icon_name("preferences-desktop-keyboard",
					    GTK_ICON_SIZE_DIALOG);
	gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
	gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(header), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(htext), label_new("Keyboard", "st-title"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(htext),
			   label_new("The layouts you type with, and how to "
				     "switch between them",
				     "st-subtitle"),
			   FALSE, FALSE, 0);
	gtk_widget_set_valign(htext, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(header), htext, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);

	/* The page: the layouts, the shortcut, the test field. */
	add_class(page, "st-page");
	card = section(page, "Keyboard layouts");
	K.list = gtk_list_box_new();
	add_class(K.list, "st-list");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(K.list),
					GTK_SELECTION_NONE);
	gtk_box_pack_start(GTK_BOX(card), K.list, FALSE, FALSE, 0);

	K.add = gtk_button_new();
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	icon = gtk_image_new_from_icon_name("list-add-symbolic",
					    GTK_ICON_SIZE_MENU);
	add_class(icon, "st-add-icon");
	gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
			   label_new("Add a keyboard layout", "st-add-label"),
			   FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(K.add), box);
	gtk_button_set_relief(GTK_BUTTON(K.add), GTK_RELIEF_NONE);
	add_class(K.add, "st-add-row");
	g_signal_connect(K.add, "clicked", G_CALLBACK(on_add_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(card), K.add, FALSE, FALSE, 0);
	K.note = label_new("", "st-note");
	gtk_label_set_line_wrap(GTK_LABEL(K.note), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(K.note), 60);
	gtk_box_pack_start(GTK_BOX(page), K.note, FALSE, FALSE, 0);

	card = section(page, "Switching");
	gtk_box_pack_start(GTK_BOX(card), build_shortcut_row(), FALSE, FALSE,
			   0);

	card = section(page, "Try it out");
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	add_class(box, "st-row");
	add_class(box, "st-test-row");
	K.test = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(K.test),
				       "Type here to test your keyboard");
	add_class(K.test, "st-test");
	g_signal_connect(K.test, "key-release-event", G_CALLBACK(on_test_key),
			 NULL);
	g_signal_connect(K.test, "focus-in-event", G_CALLBACK(on_test_key),
			 NULL);
	gtk_box_pack_start(GTK_BOX(box), K.test, FALSE, FALSE, 0);
	K.typing = label_new("", "st-row-detail");
	gtk_box_pack_start(GTK_BOX(box), K.typing, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(card), box, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page),
			   label_new("The field types with the layouts in use "
				     "now. Apply a change to try it here.",
				     "st-note"),
			   FALSE, FALSE, 0);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), page);
	gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

	/* The footer band: what Apply did, Close and Apply. */
	add_class(footer, "st-footer");
	K.status = label_new("", "st-status");
	gtk_box_pack_start(GTK_BOX(footer), K.status, TRUE, TRUE, 0);
	close = gtk_button_new_with_mnemonic("_Close");
	K.apply = gtk_button_new_with_mnemonic("_Apply");
	add_class(K.apply, "suggested-action");
	add_class(close, "st-footer-button");
	add_class(K.apply, "st-footer-button");
	g_signal_connect_swapped(close, "clicked",
				 G_CALLBACK(gtk_widget_destroy), K.win);
	g_signal_connect(K.apply, "clicked", G_CALLBACK(on_apply), NULL);
	gtk_box_pack_end(GTK_BOX(footer), K.apply, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(footer), close, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(outer), footer, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(K.win), outer);
	g_signal_connect(K.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
}

int main(int argc, char **argv)
{
	GFile *dir;

	gtk_init(&argc, &argv);

	K.cat = kbd_catalogue_load();
	kbd_conf_read(&K.saved);
	edit_from_saved();
	build_window();
	refresh();
	typing_update();

	dir = g_file_new_for_path(KBD_CONF_DIR);
	K.monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES,
					     NULL, NULL);
	if (K.monitor)
		g_signal_connect(K.monitor, "changed",
				 G_CALLBACK(on_conf_changed), NULL);
	g_object_unref(dir);

	gtk_widget_show_all(K.win);
	gtk_window_set_focus(GTK_WINDOW(K.win), NULL);
	gtk_main();
	return 0;
}
