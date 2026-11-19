/* LikeOS installer -- fitting the wizard to any screen (see layout.h).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "layout.h"
#include "widgets/widgets.h"

struct _FrameBin {
	GtkBin parent;
	double frac_w, frac_h;
	int max_w, max_h;
	int margin;
	int extra_bottom;
	gboolean compact;
	gboolean fit_height;     /* no taller than the child wants */
};

G_DEFINE_TYPE(FrameBin, frame_bin, GTK_TYPE_BIN)

static int effective_margin(FrameBin *b)
{
	return b->compact ? MIN(b->margin, 12) : b->margin;
}

static void frame_bin_get_preferred_width(GtkWidget *w, int *min, int *nat)
{
	FrameBin *b = FRAME_BIN(w);
	GtkWidget *child = gtk_bin_get_child(GTK_BIN(w));
	int cmin = 0, cnat = 0;

	if (child && gtk_widget_get_visible(child))
		gtk_widget_get_preferred_width(child, &cmin, &cnat);
	*min = cmin + 2 * effective_margin(b);
	*nat = MAX(*min, cnat + 2 * effective_margin(b));
}

static void frame_bin_get_preferred_height(GtkWidget *w, int *min, int *nat)
{
	FrameBin *b = FRAME_BIN(w);
	GtkWidget *child = gtk_bin_get_child(GTK_BIN(w));
	int cmin = 0, cnat = 0, m = effective_margin(b);

	if (child && gtk_widget_get_visible(child))
		gtk_widget_get_preferred_height(child, &cmin, &cnat);
	*min = cmin + 2 * m + (b->compact ? b->extra_bottom / 2 : b->extra_bottom);
	*nat = MAX(*min, cnat + 2 * m + b->extra_bottom);
}

static void frame_bin_size_allocate(GtkWidget *w, GtkAllocation *a)
{
	FrameBin *b = FRAME_BIN(w);
	GtkWidget *child = gtk_bin_get_child(GTK_BIN(w));
	int m = effective_margin(b);
	int extra = b->compact ? b->extra_bottom / 2 : b->extra_bottom;
	int avail_w = MAX(1, a->width - 2 * m);
	int avail_h = MAX(1, a->height - 2 * m - extra);
	int cmin_w = 0, cnat_w = 0, cmin_h = 0, cnat_h = 0;
	GtkAllocation ca;
	int cw, ch;

	gtk_widget_set_allocation(w, a);
	if (!child || !gtk_widget_get_visible(child))
		return;

	gtk_widget_get_preferred_width(child, &cmin_w, &cnat_w);
	cw = (int)(avail_w * (b->compact ? 1.0 : b->frac_w));
	if (b->max_w > 0 && cw > b->max_w)
		cw = b->max_w;
	if (cw > avail_w)
		cw = avail_w;
	if (cw < cmin_w)
		cw = cmin_w;

	gtk_widget_get_preferred_height_for_width(child, cw, &cmin_h, &cnat_h);
	ch = (int)(avail_h * (b->compact ? 1.0 : b->frac_h));
	if (b->max_h > 0 && ch > b->max_h)
		ch = b->max_h;
	if (ch > avail_h)
		ch = avail_h;
	if (b->fit_height && cnat_h > 0 && ch > cnat_h)
		ch = cnat_h;
	if (ch < cmin_h)
		ch = cmin_h;

	ca.width = cw;
	ca.height = ch;
	ca.x = a->x + MAX(0, (a->width - cw) / 2);
	ca.y = a->y + MAX(0, (a->height - extra - ch) / 2);
	gtk_widget_size_allocate(child, &ca);

	/* the child's shadow reaches past its allocation: draw it all */
	{
		GtkAllocation clip;

		gtk_widget_get_clip(child, &clip);
		gdk_rectangle_union(&clip, a, &clip);
		gtk_widget_set_clip(w, &clip);
	}
}

static void frame_bin_class_init(FrameBinClass *klass)
{
	GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

	wc->get_preferred_width = frame_bin_get_preferred_width;
	wc->get_preferred_height = frame_bin_get_preferred_height;
	wc->size_allocate = frame_bin_size_allocate;
}

static void frame_bin_init(FrameBin *b)
{
	gtk_widget_set_has_window(GTK_WIDGET(b), FALSE);
	b->frac_w = 0.7;
	b->frac_h = 0.75;
	b->margin = 24;
}

GtkWidget *frame_bin_new(double frac_w, double frac_h, int max_w, int max_h)
{
	FrameBin *b = g_object_new(FRAME_TYPE_BIN, NULL);

	frame_bin_set_share(b, frac_w, frac_h, max_w, max_h);
	return GTK_WIDGET(b);
}

void frame_bin_set_share(FrameBin *b, double frac_w, double frac_h, int max_w,
			 int max_h)
{
	b->frac_w = CLAMP(frac_w, 0.1, 1.0);
	b->frac_h = CLAMP(frac_h, 0.1, 1.0);
	b->max_w = max_w;
	b->max_h = max_h;
	gtk_widget_queue_resize(GTK_WIDGET(b));
}

void frame_bin_set_margins(FrameBin *b, int margin, int extra_bottom)
{
	b->margin = MAX(0, margin);
	b->extra_bottom = MAX(0, extra_bottom);
	gtk_widget_queue_resize(GTK_WIDGET(b));
}

void frame_bin_set_fit_height(FrameBin *b, gboolean fit)
{
	b->fit_height = fit;
	gtk_widget_queue_resize(GTK_WIDGET(b));
}

void frame_bin_set_compact(FrameBin *b, gboolean compact)
{
	if (b->compact == compact)
		return;
	b->compact = compact;
	gtk_widget_queue_resize(GTK_WIDGET(b));
}

/* ---- tiers --------------------------------------------------------- */

gboolean layout_update(App *app, int width, int height)
{
	gboolean narrow = width < LAYOUT_NARROW_WIDTH;
	gboolean short_screen = height < LAYOUT_SHORT_HEIGHT;
	LayoutTier tier;
	gboolean changed;

	if (narrow || short_screen)
		tier = TIER_COMPACT;
	else if (width >= LAYOUT_LARGE_WIDTH && height >= LAYOUT_LARGE_HEIGHT)
		tier = TIER_LARGE;
	else
		tier = TIER_REGULAR;

	changed = tier != app->tier || narrow != app->narrow ||
		short_screen != app->short_screen;
	app->tier = tier;
	app->narrow = narrow;
	app->short_screen = short_screen;
	app->screen_w = width;
	app->screen_h = height;

	ui_toggle_class(app->window, "il-compact", tier == TIER_COMPACT);
	ui_toggle_class(app->window, "il-large", tier == TIER_LARGE);
	ui_toggle_class(app->window, "il-narrow", narrow);
	ui_toggle_class(app->window, "il-short", short_screen);
	ui_toggle_class(app->window, "il-tiny", width < 720 || height < 520);
	return changed;
}

void layout_screen_size(GtkWidget *window, int *w, int *h)
{
	GdkDisplay *dpy = gtk_widget_get_display(window);
	GdkMonitor *mon = NULL;
	GdkRectangle geo = { 0, 0, 1024, 768 };

	if (gtk_widget_get_realized(window))
		mon = gdk_display_get_monitor_at_window(
			dpy, gtk_widget_get_window(window));
	if (!mon)
		mon = gdk_display_get_primary_monitor(dpy);
	if (!mon && gdk_display_get_n_monitors(dpy) > 0)
		mon = gdk_display_get_monitor(dpy, 0);
	if (mon)
		gdk_monitor_get_geometry(mon, &geo);
	*w = geo.width;
	*h = geo.height;
}

void layout_cover_screen(App *app)
{
	int w, h;

	if (app->windowed) {
		w = app->req_width > 0 ? app->req_width : 1024;
		h = app->req_height > 0 ? app->req_height : 768;
		gtk_window_set_default_size(GTK_WINDOW(app->window), w, h);
		gtk_window_resize(GTK_WINDOW(app->window), w, h);
		return;
	}
	layout_screen_size(app->window, &w, &h);
	if (app->req_width > 0 && app->req_height > 0) {
		w = app->req_width;
		h = app->req_height;
	}
	/* Without a window manager, fullscreen() is only a wish: place and
	 * size the window ourselves too. */
	gtk_window_move(GTK_WINDOW(app->window), 0, 0);
	gtk_window_set_default_size(GTK_WINDOW(app->window), w, h);
	gtk_window_resize(GTK_WINDOW(app->window), w, h);
	if (app->req_width <= 0)
		gtk_window_fullscreen(GTK_WINDOW(app->window));
}
