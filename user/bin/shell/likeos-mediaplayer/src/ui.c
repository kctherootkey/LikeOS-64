// LikeOS -- LikeOS Media Player: the small building blocks of the interface.
//
// Glyphs are drawn as vector geometry (glyphs.c) in whatever colour the
// style sheet gives the widget they sit in, so one drawing serves the dark
// glyph on a light page, the white one on the accent play button and the
// light one on the dark transport strip.  A name glyphs.c does not draw is
// taken from the icon theme and used as a mask the same way.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "ui.h"

void mp_add_class(GtkWidget *w, const char *cls)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

const char *mp_icon(const char *likeos_name, const char *fallback)
{
	GtkIconTheme *th = gtk_icon_theme_get_default();
	if (likeos_name != NULL && gtk_icon_theme_has_icon(th, likeos_name))
		return likeos_name;
	return fallback != NULL ? fallback : likeos_name;
}

/* ---- glyphs ----------------------------------------------------------- */

typedef struct {
	gchar *likeos;
	gchar *fallback;
	int size;
} Glyph;

/* Loaded icons, by name, size and scale.  Emptied when the icon theme
 * changes (an icon theme installed while the program runs). */
static GHashTable *glyph_cache;

static void on_icon_theme_changed(GtkIconTheme *th, gpointer data)
{
	(void)th;
	(void)data;
	if (glyph_cache != NULL)
		g_hash_table_remove_all(glyph_cache);
}

static cairo_surface_t *glyph_surface(const char *name, int size, int scale)
{
	if (glyph_cache == NULL) {
		glyph_cache = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)cairo_surface_destroy);
		g_signal_connect(gtk_icon_theme_get_default(), "changed",
				 G_CALLBACK(on_icon_theme_changed), NULL);
	}
	gchar *key = g_strdup_printf("%s\n%d\n%d", name, size, scale);
	cairo_surface_t *s = g_hash_table_lookup(glyph_cache, key);
	if (s != NULL || g_hash_table_contains(glyph_cache, key)) {
		g_free(key);
		return s;
	}
	GdkPixbuf *pb = gtk_icon_theme_load_icon_for_scale(
		gtk_icon_theme_get_default(), name, size, scale,
		GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
	if (pb != NULL) {
		s = gdk_cairo_surface_create_from_pixbuf(pb, scale, NULL);
		g_object_unref(pb);
	}
	/* A missing icon is remembered as missing, not looked up again on
	 * every redraw. */
	g_hash_table_insert(glyph_cache, key, s);
	return s;
}

/* Draws a glyph in `color', centred on (cx, cy): our own drawing of
 * `likeos_name' when there is one, else the icon theme's picture of the
 * first of the two names it has, as a mask. */
static void draw_glyph_at(cairo_t *cr, const char *likeos_name,
			  const char *fallback, int size, int scale,
			  const GdkRGBA *color, double cx, double cy)
{
	cairo_save(cr);
	gdk_cairo_set_source_rgba(cr, color);
	if (!mp_glyph_paint(cr, likeos_name, cx, cy, size)) {
		cairo_surface_t *s = glyph_surface(
			mp_icon(likeos_name, fallback), size, scale);
		if (s != NULL)
			cairo_mask_surface(cr, s, round(cx - size / 2.0),
					   round(cy - size / 2.0));
	}
	cairo_restore(cr);
}

cairo_surface_t *mp_glyph_surface(GtkWidget *w, const char *likeos_name,
				  int size, const GdkRGBA *color)
{
	int scale = gtk_widget_get_scale_factor(w);
	cairo_surface_t *s = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, size * scale, size * scale);
	cairo_surface_set_device_scale(s, scale, scale);
	cairo_t *cr = cairo_create(s);
	draw_glyph_at(cr, likeos_name, NULL, size, scale, color, size / 2.0,
		      size / 2.0);
	cairo_destroy(cr);
	return s;
}

static gboolean glyph_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	Glyph *g = data;
	GtkStyleContext *ctx = gtk_widget_get_style_context(w);
	GdkRGBA c;
	gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &c);
	draw_glyph_at(cr, g->likeos, g->fallback, g->size,
		      gtk_widget_get_scale_factor(w), &c,
		      gtk_widget_get_allocated_width(w) / 2.0,
		      gtk_widget_get_allocated_height(w) / 2.0);
	return FALSE;
}

static void glyph_free(gpointer p)
{
	Glyph *g = p;
	g_free(g->likeos);
	g_free(g->fallback);
	g_free(g);
}

GtkWidget *mp_glyph_new(const char *likeos_name, const char *fallback,
			int size)
{
	GtkWidget *w = gtk_drawing_area_new();
	Glyph *g = g_new0(Glyph, 1);
	g->likeos = g_strdup(likeos_name);
	g->fallback = g_strdup(fallback);
	g->size = size;
	g_object_set_data_full(G_OBJECT(w), "mp-glyph", g, glyph_free);
	gtk_widget_set_size_request(w, size, size);
	gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
	mp_add_class(w, "mp-glyph");
	g_signal_connect(w, "draw", G_CALLBACK(glyph_draw), g);
	return w;
}

void mp_glyph_set(GtkWidget *glyph, const char *likeos_name,
		  const char *fallback)
{
	Glyph *g = g_object_get_data(G_OBJECT(glyph), "mp-glyph");
	if (g == NULL)
		return;
	if (g_strcmp0(g->likeos, likeos_name) == 0 &&
	    g_strcmp0(g->fallback, fallback) == 0)
		return;
	g_free(g->likeos);
	g_free(g->fallback);
	g->likeos = g_strdup(likeos_name);
	g->fallback = g_strdup(fallback);
	gtk_widget_queue_draw(glyph);
}

GtkWidget *mp_glyph_button(const char *likeos_name, const char *fallback,
			   int size, const char *tooltip,
			   const char *css_class, gboolean toggle)
{
	GtkWidget *b = toggle ? gtk_toggle_button_new() : gtk_button_new();
	gtk_container_add(GTK_CONTAINER(b),
			  mp_glyph_new(likeos_name, fallback, size));
	if (tooltip != NULL)
		gtk_widget_set_tooltip_text(b, tooltip);
	gtk_widget_set_can_focus(b, FALSE);
	gtk_widget_set_focus_on_click(b, FALSE);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	mp_add_class(b, "mp-glyph-button");
	if (css_class != NULL)
		mp_add_class(b, css_class);
	return b;
}

void mp_glyph_button_set(GtkWidget *button, const char *likeos_name,
			 const char *fallback)
{
	GtkWidget *child = gtk_bin_get_child(GTK_BIN(button));
	if (child != NULL)
		mp_glyph_set(child, likeos_name, fallback);
}

GtkWidget *mp_text_button(const char *likeos_name, const char *fallback,
			  const char *label, gboolean accent)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	if (likeos_name != NULL || fallback != NULL)
		gtk_box_pack_start(GTK_BOX(box),
				   mp_glyph_new(likeos_name, fallback, 16),
				   FALSE, FALSE, 0);
	GtkWidget *l = gtk_label_new_with_mnemonic(label);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	gtk_widget_set_focus_on_click(b, FALSE);
	mp_add_class(b, "mp-text-button");
	if (accent) {
		/* "suggested-action" is what any GTK theme fills with its
		 * accent colour. */
		mp_add_class(b, "suggested-action");
		mp_add_class(b, "mp-accent");
	}
	return b;
}

/* ---- headers ---------------------------------------------------------- */

GtkWidget *mp_page_header(const char *title, GtkWidget **actions_out)
{
	/* One row: the title with a line of status under it at the left,
	 * the page's commands at the right. */
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	mp_add_class(box, "mp-page-header");
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	GtkWidget *t = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(t), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
	mp_add_class(t, "mp-page-title");
	gtk_box_pack_start(GTK_BOX(text), t, FALSE, FALSE, 0);
	GtkWidget *sub = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
	mp_add_class(sub, "mp-page-subtitle");
	gtk_widget_set_no_show_all(sub, TRUE);
	gtk_box_pack_start(GTK_BOX(text), sub, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
	g_object_set_data(G_OBJECT(box), "mp-title", t);
	g_object_set_data(G_OBJECT(box), "mp-subtitle", sub);
	if (actions_out != NULL) {
		GtkWidget *a = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		mp_add_class(a, "mp-page-actions");
		gtk_widget_set_valign(a, GTK_ALIGN_CENTER);
		gtk_box_pack_end(GTK_BOX(box), a, FALSE, FALSE, 0);
		*actions_out = a;
	}
	return box;
}

void mp_page_set_subtitle(GtkWidget *header, const char *text)
{
	GtkWidget *sub = g_object_get_data(G_OBJECT(header), "mp-subtitle");
	if (sub == NULL)
		return;
	gtk_label_set_text(GTK_LABEL(sub), text != NULL ? text : "");
	gtk_widget_set_visible(sub, text != NULL && *text);
}

GtkWidget *mp_section_label(const char *text)
{
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0);
	mp_add_class(l, "mp-section-title");
	return l;
}

/* ---- list rows -------------------------------------------------------- */

typedef struct {
	MpRowIsCurrent current;
	gpointer data;
	GtkTreePath *hover;
} ListStyle;

static void list_style_free(gpointer p)
{
	ListStyle *ls = p;
	if (ls->hover != NULL)
		gtk_tree_path_free(ls->hover);
	g_free(ls);
}

/* Runs before the tree view draws itself, which (with the style sheet
 * making its own row backgrounds transparent) leaves these rounded fills
 * visible under the text. */
static gboolean list_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	ListStyle *ls = data;
	GtkTreeView *tv = GTK_TREE_VIEW(w);
	GtkTreeModel *m = gtk_tree_view_get_model(tv);
	GtkTreePath *start = NULL, *end = NULL;
	if (m == NULL || !gtk_tree_view_get_visible_range(tv, &start, &end))
		return FALSE;

	GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
	GtkStyleContext *ctx = gtk_widget_get_style_context(w);
	int width = gtk_widget_get_allocated_width(w);
	GtkTreePath *p = gtk_tree_path_copy(start);
	for (int guard = 0; guard < 2000; guard++) {
		GtkTreeIter it;
		if (!gtk_tree_model_get_iter(m, &it, p))
			break;
		GdkRectangle r;
		gtk_tree_view_get_background_area(tv, p, NULL, &r);
		int wx = 0, wy = 0;
		gtk_tree_view_convert_bin_window_to_widget_coords(tv, r.x, r.y,
								  &wx, &wy);
		gboolean selected = gtk_tree_selection_path_is_selected(sel, p);
		gboolean hover = ls->hover != NULL &&
				 gtk_tree_path_compare(ls->hover, p) == 0;
		gboolean cur = ls->current != NULL &&
			       ls->current(m, &it, ls->data);
		/* Plain rows on the page; only the selected one, the one
		 * under the pointer and the one playing get a fill. */
		const char *cls = selected ? "mp-row-selected"
				  : hover  ? "mp-row-hover"
					   : NULL;
		if (cls != NULL || cur) {
			gtk_style_context_save(ctx);
			if (cls != NULL)
				gtk_style_context_add_class(ctx, cls);
			if (cur)
				gtk_style_context_add_class(ctx,
							    "mp-row-current");
			gtk_render_background(ctx, cr, 0, wy, width, r.height);
			gtk_render_frame(ctx, cr, 0, wy, width, r.height);
			gtk_style_context_restore(ctx);
		}
		if (gtk_tree_path_compare(p, end) >= 0)
			break;
		gtk_tree_path_next(p);
	}
	gtk_tree_path_free(p);
	gtk_tree_path_free(start);
	gtk_tree_path_free(end);
	return FALSE;
}

static void set_hover(GtkWidget *w, ListStyle *ls, GtkTreePath *path)
{
	if (path == NULL && ls->hover == NULL)
		return;
	if (path != NULL && ls->hover != NULL &&
	    gtk_tree_path_compare(path, ls->hover) == 0) {
		gtk_tree_path_free(path);
		return;
	}
	if (ls->hover != NULL)
		gtk_tree_path_free(ls->hover);
	ls->hover = path;
	gtk_widget_queue_draw(w);
}

static gboolean list_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
	GtkTreePath *path = NULL;
	if (ev->window != gtk_tree_view_get_bin_window(GTK_TREE_VIEW(w)) ||
	    !gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (gint)ev->x,
					   (gint)ev->y, &path, NULL, NULL,
					   NULL))
		path = NULL;
	set_hover(w, data, path);
	return FALSE;
}

static gboolean list_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
	(void)ev;
	set_hover(w, data, NULL);
	return FALSE;
}

void mp_list_style(GtkTreeView *tv, MpRowIsCurrent current, gpointer data)
{
	ListStyle *ls = g_new0(ListStyle, 1);
	ls->current = current;
	ls->data = data;
	g_object_set_data_full(G_OBJECT(tv), "mp-list-style", ls,
			       list_style_free);
	mp_add_class(GTK_WIDGET(tv), "mp-list");
	gtk_widget_add_events(GTK_WIDGET(tv),
			      GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
	g_signal_connect(tv, "draw", G_CALLBACK(list_draw), ls);
	g_signal_connect(tv, "motion-notify-event", G_CALLBACK(list_motion),
			 ls);
	g_signal_connect(tv, "leave-notify-event", G_CALLBACK(list_leave), ls);
}

void mp_cell_set_text(GtkCellRenderer *cell, const char *text,
		      const GdkRGBA *color, gboolean strong)
{
	/* As markup: a text cell drops its foreground colour on a selected
	 * row, but not the colour of a span. */
	gchar *m = g_markup_printf_escaped(
		"<span foreground=\"#%02x%02x%02x\" fgalpha=\"%d%%\" "
		"weight=\"%s\">%s</span>",
		(guint)(color->red * 255 + 0.5),
		(guint)(color->green * 255 + 0.5),
		(guint)(color->blue * 255 + 0.5),
		MAX(1, (int)(color->alpha * 100 + 0.5)),
		strong ? "semibold" : "normal", text != NULL ? text : "");
	g_object_set(cell, "markup", m, NULL);
	g_free(m);
}

/* ---- pictures --------------------------------------------------------- */

void mp_rounded_rect(cairo_t *cr, double x, double y, double w, double h,
		     double r)
{
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
}

void mp_draw_picture(cairo_t *cr, GdkPixbuf *pb, double x, double y,
		     double w, double h, double radius)
{
	int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb);
	if (pw <= 0 || ph <= 0 || w < 1 || h < 1)
		return;
	double s = MAX(w / pw, h / ph);
	cairo_save(cr);
	mp_rounded_rect(cr, x, y, w, h, radius);
	cairo_clip(cr);
	cairo_translate(cr, x + (w - pw * s) / 2, y + (h - ph * s) / 2);
	cairo_scale(cr, s, s);
	gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* ---- tiles ------------------------------------------------------------ */

#define TILE_PLAY 44	/* the round play button on a hovered tile */

static void tile_free(gpointer p)
{
	MpTile *t = p;
	g_free(t->key);
	g_free(t->title);
	g_free(t->subtitle);
	g_free(t->placeholder);
	g_free(t->placeholder_fb);
	g_free(t->badge);
	g_free(t);
}

static gboolean tile_hovered(MpTile *t)
{
	return (gtk_widget_get_state_flags(t->child) &
		GTK_STATE_FLAG_PRELIGHT) != 0;
}

/* The play button sits in the middle of the picture. */
static void play_button_rect(MpTile *t, GdkRectangle *r)
{
	r->width = r->height = TILE_PLAY;
	r->x = (t->width - TILE_PLAY) / 2;
	r->y = (t->height - TILE_PLAY) / 2;
}

/* A layout in the widget's own font family, at another size and weight. */
static PangoLayout *layout_sized(GtkWidget *w, const char *text, double pt,
				 PangoWeight weight)
{
	PangoLayout *pl = gtk_widget_create_pango_layout(w, text);
	PangoFontDescription *fd = pango_font_description_copy(
		pango_context_get_font_description(
			gtk_widget_get_pango_context(w)));
	pango_font_description_set_size(fd, (gint)(pt * PANGO_SCALE));
	pango_font_description_set_weight(fd, weight);
	pango_layout_set_font_description(pl, fd);
	pango_font_description_free(fd);
	return pl;
}

static void draw_badge(GtkWidget *w, cairo_t *cr, MpTile *t)
{
	PangoLayout *pl = layout_sized(w, t->badge, 7.5,
				       PANGO_WEIGHT_SEMIBOLD);
	int lw = 0, lh = 0;
	pango_layout_get_pixel_size(pl, &lw, &lh);
	double bw = lw + 10, bh = lh + 4;
	double bx = t->width - bw - 8, by = t->height - bh - 8;
	/* Over a picture of any colour: a dark plate with white text, in
	 * every theme. */
	mp_rounded_rect(cr, bx, by, bw, bh, bh / 2);
	cairo_set_source_rgba(cr, 0.086, 0.110, 0.165, 0.82);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_move_to(cr, bx + 5, by + 2);
	pango_cairo_show_layout(cr, pl);
	g_object_unref(pl);
}

static gboolean tile_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	MpTile *t = data;
	GtkStyleContext *ctx = gtk_widget_get_style_context(w);
	double radius = t->shape == MP_TILE_ROUND ? t->width / 2.0 : 6;
	gboolean hovered = tile_hovered(t) && t->shape != MP_TILE_ROUND;
	GdkPixbuf *pb = t->art != NULL ? t->art(t, t->data) : NULL;
	int scale = gtk_widget_get_scale_factor(w);

	if (pb != NULL) {
		mp_draw_picture(cr, pb, 0, 0, t->width, t->height, radius);
	} else {
		/* The placeholder: the style sheet's fill (a gradient in the
		 * accent colours) with a glyph in its text colour. */
		gtk_render_background(ctx, cr, 0, 0, t->width, t->height);
		GdkRGBA c;
		gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx),
					    &c);
		if (t->shape == MP_TILE_ROUND && t->title != NULL &&
		    *t->title) {
			/* An artist: the initial, like a contact card. */
			gunichar ch = g_unichar_toupper(
				g_utf8_get_char_validated(t->title, -1));
			char buf[8] = { 0 };
			if (ch != (gunichar)-1 && ch != (gunichar)-2)
				g_unichar_to_utf8(ch, buf);
			PangoLayout *pl = layout_sized(
				w, buf, 30, PANGO_WEIGHT_SEMIBOLD);
			int lw = 0, lh = 0;
			pango_layout_get_pixel_size(pl, &lw, &lh);
			gdk_cairo_set_source_rgba(cr, &c);
			cairo_move_to(cr, (t->width - lw) / 2.0,
				      (t->height - lh) / 2.0);
			pango_cairo_show_layout(cr, pl);
			g_object_unref(pl);
		} else {
			int isz = t->width >= 140 ? 48 : t->width >= 90 ? 32 : 24;
			draw_glyph_at(cr, t->placeholder, t->placeholder_fb,
				      isz, scale, &c, t->width / 2.0,
				      t->height / 2.0);
		}
	}
	if (hovered) {
		/* A light veil over the picture under the pointer, so the
		 * button reads on any cover. */
		mp_rounded_rect(cr, 0, 0, t->width, t->height, radius);
		cairo_set_source_rgba(cr, 0.08, 0.10, 0.14, 0.22);
		cairo_fill(cr);
	}
	if (t->badge != NULL && *t->badge)
		draw_badge(w, cr, t);

	if (hovered) {
		GdkRectangle r;
		play_button_rect(t, &r);
		gtk_style_context_save(ctx);
		gtk_style_context_add_class(ctx, "mp-tile-play");
		gtk_render_background(ctx, cr, r.x, r.y, r.width, r.height);
		gtk_render_frame(ctx, cr, r.x, r.y, r.width, r.height);
		GdkRGBA c;
		gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx),
					    &c);
		gtk_style_context_restore(ctx);
		draw_glyph_at(cr, "likeos-play",
			      "media-playback-start-symbolic", 16, scale, &c,
			      r.x + r.width / 2.0 + 0.5, r.y + r.height / 2.0);
	}
	return FALSE;
}

static void tile_state_changed(GtkWidget *w, GtkStateFlags old, gpointer d)
{
	MpTile *t = d;
	(void)w;
	if (((old ^ gtk_widget_get_state_flags(t->child)) &
	     GTK_STATE_FLAG_PRELIGHT) != 0)
		gtk_widget_queue_draw(t->area);
}

GtkWidget *mp_tile_new(MpTileShape shape, int width, const char *key,
		       const char *title, const char *subtitle,
		       const char *placeholder, const char *placeholder_fb,
		       MpTileArt art, MpTileAction action, gpointer data)
{
	MpTile *t = g_new0(MpTile, 1);
	t->shape = shape;
	t->key = g_strdup(key);
	t->title = g_strdup(title);
	t->subtitle = g_strdup(subtitle);
	t->placeholder = g_strdup(placeholder);
	t->placeholder_fb = g_strdup(placeholder_fb);
	t->art = art;
	t->action = action;
	t->data = data;
	t->width = width;
	t->height = shape == MP_TILE_WIDE ? width * 9 / 16 : width;

	t->child = gtk_flow_box_child_new();
	g_object_set_data_full(G_OBJECT(t->child), "mp-tile", t, tile_free);
	mp_add_class(t->child, "mp-tile");
	if (shape == MP_TILE_ROUND)
		mp_add_class(t->child, "mp-tile-round");

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	/* The grid may make the card wider than the picture; the picture and
	 * its labels stay in the middle of it. */
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	t->area = gtk_drawing_area_new();
	gtk_widget_set_size_request(t->area, t->width, t->height);
	mp_add_class(t->area, "mp-tile-art");
	if (shape == MP_TILE_ROUND)
		mp_add_class(t->area, "mp-tile-art-round");
	else if (shape == MP_TILE_WIDE)
		mp_add_class(t->area, "mp-tile-art-wide");
	g_signal_connect(t->area, "draw", G_CALLBACK(tile_draw), t);
	gtk_box_pack_start(GTK_BOX(box), t->area, FALSE, FALSE, 0);

	GtkWidget *tl = gtk_label_new(title);
	mp_add_class(tl, "mp-tile-title");
	GtkWidget *sl = gtk_label_new(subtitle);
	mp_add_class(sl, "mp-tile-sub");
	GtkWidget *labels[] = { tl, sl };
	for (int i = 0; i < 2; i++) {
		gtk_label_set_ellipsize(GTK_LABEL(labels[i]),
					PANGO_ELLIPSIZE_END);
		gtk_label_set_xalign(GTK_LABEL(labels[i]),
				     shape == MP_TILE_ROUND ? 0.5f : 0.0f);
		gtk_label_set_width_chars(GTK_LABEL(labels[i]), 1);
		gtk_widget_set_size_request(labels[i], t->width, -1);
		gtk_box_pack_start(GTK_BOX(box), labels[i], FALSE, FALSE, 0);
	}
	if (subtitle == NULL || *subtitle == '\0')
		gtk_widget_set_no_show_all(sl, TRUE);
	if (title != NULL)
		gtk_widget_set_tooltip_text(t->child, title);
	gtk_container_add(GTK_CONTAINER(t->child), box);
	g_signal_connect(t->child, "state-flags-changed",
			 G_CALLBACK(tile_state_changed), t);
	gtk_widget_show_all(t->child);
	return t->child;
}

MpTile *mp_tile_of(GtkWidget *child)
{
	return child != NULL ? g_object_get_data(G_OBJECT(child), "mp-tile")
			     : NULL;
}

void mp_tile_set_badge(GtkWidget *child, const char *badge)
{
	MpTile *t = mp_tile_of(child);
	if (t == NULL)
		return;
	g_free(t->badge);
	t->badge = g_strdup(badge);
	gtk_widget_queue_draw(t->area);
}

/* The pointer over the grid, in the grid's coordinates (it has a window
 * of its own): the tile there, or NULL. */
static MpTile *tile_at(GtkWidget *grid, GdkWindow *win, double x, double y)
{
	if (win != gtk_widget_get_window(grid))
		return NULL;
	GtkFlowBoxChild *c = gtk_flow_box_get_child_at_pos(GTK_FLOW_BOX(grid),
							   (gint)x, (gint)y);
	return c != NULL ? mp_tile_of(GTK_WIDGET(c)) : NULL;
}

/* The tile under the pointer is "hovered" -- its :hover style and the
 * round play button.  The grid does this itself only while it has the
 * keyboard focus, so it is done here. */
static void set_hovered(GtkWidget *grid, MpTile *t)
{
	/* A weak pointer: the hovered tile may be destroyed (the grid is
	 * refilled) while it is hovered. */
	gpointer *slot = g_object_get_data(G_OBJECT(grid), "mp-hovered");
	if (slot == NULL) {
		slot = g_new0(gpointer, 1);
		g_object_set_data_full(G_OBJECT(grid), "mp-hovered", slot,
				       g_free);
	}
	GtkWidget *now = t != NULL ? t->child : NULL;
	if (*slot == now)
		return;
	if (*slot != NULL) {
		gtk_widget_unset_state_flags(*slot, GTK_STATE_FLAG_PRELIGHT);
		g_object_remove_weak_pointer(G_OBJECT(*slot), slot);
	}
	*slot = now;
	if (now != NULL) {
		g_object_add_weak_pointer(G_OBJECT(now), slot);
		gtk_widget_set_state_flags(now, GTK_STATE_FLAG_PRELIGHT, FALSE);
	}
}

static void grid_destroy(GtkWidget *w, gpointer d)
{
	(void)d;
	set_hovered(w, NULL);
}

static gboolean grid_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
	(void)d;
	set_hovered(w, tile_at(w, ev->window, ev->x, ev->y));
	return FALSE;
}

static gboolean grid_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer d)
{
	(void)d;
	if (ev->detail != GDK_NOTIFY_INFERIOR)
		set_hovered(w, NULL);
	return FALSE;
}

/* A click on a hovered tile's round play button plays it; anywhere else
 * on the tile, the grid opens it. */
static gboolean grid_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	(void)d;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_PRIMARY)
		return FALSE;
	MpTile *t = tile_at(w, ev->window, ev->x, ev->y);
	if (t == NULL || t->shape == MP_TILE_ROUND || !tile_hovered(t))
		return FALSE;
	int ax = 0, ay = 0;
	if (!gtk_widget_translate_coordinates(w, t->area, (gint)ev->x,
					      (gint)ev->y, &ax, &ay))
		return FALSE;
	GdkRectangle r;
	play_button_rect(t, &r);
	if (ax >= r.x && ax < r.x + r.width && ay >= r.y &&
	    ay < r.y + r.height) {
		if (t->action != NULL)
			t->action(t, TRUE, t->data);
		return TRUE;
	}
	return FALSE;
}

static void on_tile_activated(GtkFlowBox *fb, GtkFlowBoxChild *child,
			      gpointer data)
{
	(void)fb;
	(void)data;
	MpTile *t = mp_tile_of(GTK_WIDGET(child));
	if (t != NULL && t->action != NULL)
		t->action(t, FALSE, t->data);
}

GtkWidget *mp_tile_grid_new(void)
{
	GtkWidget *fb = gtk_flow_box_new();
	gtk_widget_add_events(fb, GDK_POINTER_MOTION_MASK |
					  GDK_LEAVE_NOTIFY_MASK |
					  GDK_BUTTON_PRESS_MASK);
	g_signal_connect(fb, "motion-notify-event", G_CALLBACK(grid_motion),
			 NULL);
	g_signal_connect(fb, "leave-notify-event", G_CALLBACK(grid_leave),
			 NULL);
	g_signal_connect(fb, "button-press-event", G_CALLBACK(grid_press),
			 NULL);
	g_signal_connect(fb, "destroy", G_CALLBACK(grid_destroy), NULL);
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fb), GTK_SELECTION_NONE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(fb), FALSE);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fb), 8);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fb), 8);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fb), 64);
	gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(fb), TRUE);
	gtk_widget_set_valign(fb, GTK_ALIGN_START);
	gtk_widget_set_halign(fb, GTK_ALIGN_FILL);
	mp_add_class(fb, "mp-grid");
	g_signal_connect(fb, "child-activated", G_CALLBACK(on_tile_activated),
			 NULL);
	return fb;
}

void mp_tile_grid_clear(GtkWidget *grid)
{
	GList *kids = gtk_container_get_children(GTK_CONTAINER(grid));
	for (GList *l = kids; l != NULL; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(kids);
}

/* ---- settings --------------------------------------------------------- */

GtkWidget *mp_setting_row(const char *likeos_name, const char *fallback,
			  const char *title, const char *description,
			  GtkWidget *control)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	mp_add_class(row, "mp-setting-row");
	if (likeos_name != NULL || fallback != NULL)
		gtk_box_pack_start(GTK_BOX(row),
				   mp_glyph_new(likeos_name, fallback, 20),
				   FALSE, FALSE, 0);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	GtkWidget *t = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(t), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
	mp_add_class(t, "mp-setting-title");
	g_object_set_data(G_OBJECT(row), "mp-title", t);
	gtk_box_pack_start(GTK_BOX(text), t, FALSE, FALSE, 0);
	if (description != NULL) {
		GtkWidget *d = gtk_label_new(description);
		gtk_label_set_xalign(GTK_LABEL(d), 0.0);
		gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(d), 70);
		mp_add_class(d, "mp-setting-desc");
		gtk_box_pack_start(GTK_BOX(text), d, FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
	if (control != NULL) {
		gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
		gtk_box_pack_end(GTK_BOX(row), control, FALSE, FALSE, 0);
	}
	return row;
}
