// LikeOS -- LikeOS Image Viewer: the look, and the small widgets made of
// glyphs.
//
// The colours, radii and spacing all come from the LikeOS theme
// (res/xorg/gtk3/theme/src/_imageviewer.scss), through the style classes
// set here and by the window.  Under another theme a small style sheet of
// the program's own gives the dark viewing surface and the spacing, in
// that theme's terms where it can.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "imageviewer.h"

static const char fallback_css[] =
	"window.iv-window { background-color: #161c2a; color: #eef2fa; }\n"
	".iv-titlebar { min-height: 40px; background-color: #161c2a; }\n"
	".iv-strip, .iv-info, .iv-filmstrip, .iv-options, .iv-topbar {"
	"  background-color: #1f2636; color: #eef2fa; border-radius: 14px; }\n"
	".iv-strip { margin: 8px; padding: 4px 8px; }\n"
	".iv-info { margin: 8px 8px 8px 0; padding: 12px; }\n"
	".iv-rail { background-color: #161c2a; }\n"
	".iv-toast, .iv-confirm { background-color: #1f2636; color: #eef2fa;"
	"  border-radius: 14px; padding: 12px 16px; }\n"
	".iv-secondary { color: #a4adbf; }\n";

static GtkCssProvider *fallback;

static void theme_changed(GtkSettings *s, GParamSpec *ps, gpointer data)
{
	char *name = NULL;
	GdkScreen *scr = gdk_screen_get_default();
	gboolean ours;

	g_object_get(s, "gtk-theme-name", &name, NULL);
	ours = name && !strcmp(name, "LikeOS");
	if (!ours && !fallback) {
		fallback = gtk_css_provider_new();
		gtk_css_provider_load_from_data(fallback, fallback_css, -1, NULL);
		gtk_style_context_add_provider_for_screen(
			scr, GTK_STYLE_PROVIDER(fallback),
			GTK_STYLE_PROVIDER_PRIORITY_FALLBACK + 1);
	} else if (ours && fallback) {
		gtk_style_context_remove_provider_for_screen(
			scr, GTK_STYLE_PROVIDER(fallback));
		g_clear_object(&fallback);
	}
	g_free(name);
}

void iv_style_install(void)
{
	GtkSettings *s = gtk_settings_get_default();

	if (!s)
		return;
	g_signal_connect(s, "notify::gtk-theme-name", G_CALLBACK(theme_changed),
			 NULL);
	theme_changed(s, NULL, NULL);
}

void iv_add_class(GtkWidget *w, const char *cls)
{
	if (w && cls)
		gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

void iv_remove_class(GtkWidget *w, const char *cls)
{
	if (w && cls)
		gtk_style_context_remove_class(gtk_widget_get_style_context(w),
					       cls);
}

void iv_set_class(GtkWidget *w, const char *cls, gboolean on)
{
	if (on)
		iv_add_class(w, cls);
	else
		iv_remove_class(w, cls);
}

/* ---- buttons ---------------------------------------------------------------- */

static void name_button(GtkWidget *b, const char *tip, IvGlyph g)
{
	if (tip) {
		gtk_widget_set_tooltip_text(b, tip);
		atk_object_set_name(gtk_widget_get_accessible(b), tip);
	} else {
		atk_object_set_name(gtk_widget_get_accessible(b), iv_glyph_name(g));
	}
}

static GtkWidget *glyph_button_common(GtkWidget *b, IvGlyph g, int size,
				      const char *tip, const char *css)
{
	GtkWidget *glyph = iv_glyph_new(g, size);

	gtk_container_add(GTK_CONTAINER(b), glyph);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_focus_on_click(b, FALSE);
	gtk_widget_set_can_focus(b, FALSE);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	iv_add_class(b, "iv-glyph-button");
	iv_add_class(b, css);
	g_object_set_data(G_OBJECT(b), "iv-glyph-widget", glyph);
	name_button(b, tip, g);
	gtk_widget_show_all(b);
	return b;
}

GtkWidget *iv_glyph_button(IvGlyph g, int size, const char *tooltip,
			   const char *css_class)
{
	return glyph_button_common(gtk_button_new(), g, size, tooltip,
				   css_class);
}

GtkWidget *iv_glyph_toggle(IvGlyph g, int size, const char *tooltip,
			   const char *css_class)
{
	return glyph_button_common(gtk_toggle_button_new(), g, size, tooltip,
				   css_class);
}

void iv_glyph_button_set(GtkWidget *button, IvGlyph g)
{
	GtkWidget *glyph = g_object_get_data(G_OBJECT(button), "iv-glyph-widget");

	if (glyph)
		iv_glyph_set(glyph, g);
}

GtkWidget *iv_text_button(IvGlyph g, const char *label, gboolean chevron,
			  const char *css_class)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *l = gtk_label_new(label);

	if (g != IV_GLYPH_NONE) {
		GtkWidget *glyph = iv_glyph_new(g, 16);

		gtk_box_pack_start(GTK_BOX(box), glyph, FALSE, FALSE, 0);
		g_object_set_data(G_OBJECT(b), "iv-glyph-widget", glyph);
	}
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	if (chevron) {
		GtkWidget *c = iv_glyph_new(IV_GLYPH_CHEVRON_DOWN, 12);

		iv_add_class(c, "iv-chevron");
		gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
	}
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_widget_set_focus_on_click(b, FALSE);
	gtk_widget_set_can_focus(b, FALSE);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	iv_add_class(b, "iv-text-button");
	iv_add_class(b, css_class);
	g_object_set_data(G_OBJECT(b), "iv-label", l);
	gtk_widget_show_all(b);
	return b;
}

GtkWidget *iv_text_button_label(GtkWidget *button)
{
	return g_object_get_data(G_OBJECT(button), "iv-label");
}

/* ---- menus --------------------------------------------------------------------- */

GtkWidget *iv_menu_new(void)
{
	GtkWidget *m = gtk_menu_new();

	iv_add_class(m, "iv-menu");
	gtk_menu_set_reserve_toggle_size(GTK_MENU(m), FALSE);
	return m;
}

GtkWidget *iv_menu_item(GtkWidget *menu, IvGlyph g, const char *label,
			const char *accel, GCallback cb, gpointer data)
{
	GtkWidget *mi = gtk_menu_item_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *l = gtk_label_new_with_mnemonic(label);
	GtkWidget *glyph = iv_glyph_new(g, 16);

	gtk_box_pack_start(GTK_BOX(box), glyph, FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
	if (accel) {
		GtkWidget *a = gtk_label_new(accel);

		iv_add_class(a, "iv-accel");
		gtk_widget_set_margin_start(a, 24);
		gtk_box_pack_end(GTK_BOX(box), a, FALSE, FALSE, 0);
	}
	gtk_container_add(GTK_CONTAINER(mi), box);
	iv_add_class(mi, "iv-menu-item");
	if (cb)
		g_signal_connect_swapped(mi, "activate", cb, data);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
	gtk_widget_show_all(mi);
	return mi;
}

GtkWidget *iv_menu_separator(GtkWidget *menu)
{
	GtkWidget *s = gtk_separator_menu_item_new();

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), s);
	gtk_widget_show(s);
	return s;
}

/* Every menu is made for one showing and goes with it -- but not from
 * inside "deactivate": a menu shell deactivates itself BEFORE it activates
 * the item that was chosen, so a menu destroyed there takes the item and its
 * handlers with it and the choice does nothing.  The menu goes from the main
 * loop instead, once the item has run. */
static gboolean destroy_menu(gpointer m)
{
	gtk_widget_destroy(m);
	g_object_unref(m);
	return G_SOURCE_REMOVE;
}

static void menu_closed(GtkMenuShell *m, gpointer data)
{
	(void)data;
	g_idle_add(destroy_menu, g_object_ref(m));
}

/* A button whose menu is open looks pressed until the menu goes. */
static void menu_gone(GtkMenuShell *m, gpointer button)
{
	if (GTK_IS_WIDGET(button))
		iv_remove_class(button, "iv-menu-open");
}

static void popup_common(GtkWidget *menu, GtkWidget *button,
			 GdkGravity widget_anchor, GdkGravity menu_anchor)
{
	GdkEvent *ev = gtk_get_current_event();
	GtkWidget *top = gtk_widget_get_toplevel(button);

	if (!gtk_menu_get_attach_widget(GTK_MENU(menu)))
		gtk_menu_attach_to_widget(GTK_MENU(menu), top, NULL);
	iv_add_class(button, "iv-menu-open");
	g_signal_connect_object(menu, "deactivate", G_CALLBACK(menu_gone),
				button, 0);
	g_signal_connect(menu, "deactivate", G_CALLBACK(menu_closed), NULL);
	g_object_set(menu, "anchor-hints",
		     GDK_ANCHOR_FLIP | GDK_ANCHOR_SLIDE | GDK_ANCHOR_RESIZE,
		     "rect-anchor-dy", widget_anchor == GDK_GRAVITY_SOUTH_WEST ||
						 widget_anchor == GDK_GRAVITY_SOUTH_EAST ?
					       4 :
					       -4,
		     NULL);
	gtk_menu_popup_at_widget(GTK_MENU(menu), button, widget_anchor,
				 menu_anchor, ev);
	if (ev)
		gdk_event_free(ev);
}

void iv_menu_popup_below(GtkWidget *menu, GtkWidget *button)
{
	popup_common(menu, button, GDK_GRAVITY_SOUTH_WEST,
		     GDK_GRAVITY_NORTH_WEST);
}

void iv_menu_popup_below_end(GtkWidget *menu, GtkWidget *button)
{
	popup_common(menu, button, GDK_GRAVITY_SOUTH_EAST,
		     GDK_GRAVITY_NORTH_EAST);
}

void iv_menu_popup_above(GtkWidget *menu, GtkWidget *button)
{
	popup_common(menu, button, GDK_GRAVITY_NORTH_WEST,
		     GDK_GRAVITY_SOUTH_WEST);
}

void iv_menu_popup_at_pointer(GtkWidget *menu, GtkWidget *top, GdkEvent *ev)
{
	gtk_menu_attach_to_widget(GTK_MENU(menu), top, NULL);
	g_signal_connect(menu, "deactivate", G_CALLBACK(menu_closed), NULL);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
}

/* ---- dialogs and panels ---------------------------------------------------------- */

static void fixed_realized(GtkWidget *w, gpointer data)
{
	gdk_window_set_functions(gtk_widget_get_window(w),
				 GDK_FUNC_MOVE | GDK_FUNC_CLOSE);
}

void iv_fixed_dialog(GtkWidget *dialog)
{
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	iv_add_class(dialog, "iv-dialog");
	if (gtk_widget_get_realized(dialog))
		fixed_realized(dialog, NULL);
	else
		g_signal_connect(dialog, "realize", G_CALLBACK(fixed_realized),
				 NULL);
}

GtkWidget *iv_section_label(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	iv_add_class(l, "iv-section-title");
	return l;
}
