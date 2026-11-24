// LikeOS Task Viewer -- the Performance page.
//
// On the left a list of the resources -- the processor, memory, every disk
// and every network adapter -- each with a small graph and its figures in a
// line or two; on the right the one selected: its name and device, the big
// graph of the last 60 seconds and the figures below it.  Disks and
// adapters come and go with the hardware (a USB drive plugged in shows up
// at the next tick).
//
// Everything comes from one TvSysinfo (tv-sysinfo.c), sampled at every
// tick whether the page is on screen or not, so the graphs have their
// history when the page is opened.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "tv-app.h"
#include "tv-graph.h"
#include "tv-pages.h"
#include "tv-sysinfo.h"

typedef enum { RES_CPU, RES_MEM, RES_DISK, RES_NET } ResKind;

/* Each kind's colour. */
static const double colours[][3] = {
	[RES_CPU] = { 0.00, 0.58, 0.56 }, /* teal */
	[RES_MEM] = { 0.84, 0.58, 0.04 }, /* amber */
	[RES_DISK] = { 0.36, 0.36, 0.78 }, /* indigo */
	[RES_NET] = { 0.86, 0.33, 0.26 }, /* coral */
};

typedef struct {
	const char *caption;
	GtkWidget *value;
} Fig;

typedef struct {
	ResKind kind;
	char key[48]; /* "cpu", "mem", "disk:<name>", "net:<name>" */
	int order;
	gboolean seen;

	GtkWidget *row;
	GtkWidget *mini;
	GtkWidget *row_title, *row_line1, *row_line2;

	GtkWidget *panel;
	GtkWidget *title, *device;
	GtkWidget *graph, *graph2;
	GtkWidget *graph_stack; /* the processor: overall or one per CPU */
	GPtrArray *cpu_graphs;
	GtkWidget *bar; /* memory composition */
	double parts[3]; /* in use, cached, free: fractions of all */
	GPtrArray *figs; /* Fig *, in the order shown */
} Res;

typedef struct {
	TvSysinfo *si;
	GtkWidget *list;
	GtkWidget *stack;
	GPtrArray *res; /* Res * */
	GSimpleActionGroup *actions;
	GMenu *menu;
	gboolean logical; /* the processor as one graph per CPU */
} Perf;

static Perf *perf_of(GtkWidget *page)
{
	return g_object_get_data(G_OBJECT(page), "tv-perf");
}

/* ---- the look ----------------------------------------------------------- */

static const char css[] =
	".tv-perf-list row { padding: 6px 10px; border-radius: 6px; }\n"
	".tv-perf-list { background: transparent; }\n"
	".tv-perf-row-title { font-weight: 600; }\n"
	".tv-perf-row-line { font-size: 90%; }\n"
	".tv-perf-panel { padding: 4px 20px 20px 20px; }\n"
	".tv-perf-title { font-size: 200%; font-weight: 600; }\n"
	".tv-perf-device { font-size: 115%; }\n"
	".tv-perf-caption { font-size: 90%; }\n"
	".tv-perf-big { font-size: 150%; }\n"
	".tv-perf-figcap { font-size: 90%; }\n";

static void install_css(void)
{
	static gboolean done;
	GtkCssProvider *p;

	if (done)
		return;
	done = TRUE;
	p = gtk_css_provider_new();
	gtk_css_provider_load_from_data(p, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(
		gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

static GtkWidget *label_new(const char *text, const char *cls, gboolean dim)
{
	GtkWidget *l = gtk_label_new(text);
	GtkStyleContext *sc = gtk_widget_get_style_context(l);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	if (cls)
		gtk_style_context_add_class(sc, cls);
	if (dim)
		gtk_style_context_add_class(sc, GTK_STYLE_CLASS_DIM_LABEL);
	return l;
}

/* Sets a label to a string the call gives away. */
static void set_text_take(GtkWidget *label, char *text)
{
	if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) != 0)
		gtk_label_set_text(GTK_LABEL(label), text);
	g_free(text);
}

static void set_text(GtkWidget *label, const char *text)
{
	set_text_take(label, g_strdup(text));
}

/* ---- scale labels ------------------------------------------------------- */

static char *fmt_percent(double v)
{
	return g_strdup_printf("%.0f%%", v);
}

static char *fmt_bytes(double v)
{
	return tv_format_bytes((guint64)v);
}

static char *fmt_byte_rate(double v)
{
	return tv_format_byte_rate(v);
}

static char *fmt_bit_rate(double v)
{
	return tv_format_bit_rate(v);
}

/* ---- figures ------------------------------------------------------------ */

static void fig_free(gpointer p)
{
	g_free(p);
}

static GtkWidget *fig_value(Res *r, const char *caption)
{
	for (guint i = 0; i < r->figs->len; i++) {
		Fig *f = r->figs->pdata[i];
		if (!strcmp(f->caption, caption))
			return f->value;
	}
	return NULL;
}

/* A big figure: the caption above the value, at a column and row of the
 * grid on the left. */
static void add_big(Res *r, GtkWidget *grid, int col, int row,
		    const char *caption)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	Fig *f = g_new0(Fig, 1);

	f->caption = caption;
	f->value = label_new("-", "tv-perf-big", FALSE);
	gtk_box_pack_start(GTK_BOX(box),
			   label_new(caption, "tv-perf-figcap", TRUE), FALSE,
			   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), f->value, FALSE, FALSE, 0);
	gtk_widget_show_all(box);
	gtk_grid_attach(GTK_GRID(grid), box, col, row, 1, 1);
	g_ptr_array_add(r->figs, f);
}

/* A fact: caption and value side by side, in the grid on the right. */
static void add_info(Res *r, GtkWidget *grid, int row, const char *caption)
{
	Fig *f = g_new0(Fig, 1);

	f->caption = caption;
	f->value = label_new("-", NULL, FALSE);
	gtk_label_set_selectable(GTK_LABEL(f->value), TRUE);
	gtk_label_set_ellipsize(GTK_LABEL(f->value), PANGO_ELLIPSIZE_END);
	gtk_grid_attach(GTK_GRID(grid), label_new(caption, NULL, TRUE), 0, row,
			1, 1);
	gtk_grid_attach(GTK_GRID(grid), f->value, 1, row, 1, 1);
	g_ptr_array_add(r->figs, f);
}

static void fig_set_take(Res *r, const char *caption, char *text)
{
	GtkWidget *v = fig_value(r, caption);

	if (v)
		set_text_take(v, text);
	else
		g_free(text);
}

static void fig_set(Res *r, const char *caption, const char *text)
{
	fig_set_take(r, caption, g_strdup(text));
}

/* Shows or hides a figure with its caption.  What is hidden stays hidden
 * through a show_all of the page. */
static void show_alone(GtkWidget *w, gboolean show)
{
	gtk_widget_set_no_show_all(w, TRUE);
	gtk_widget_set_visible(w, show);
}

static void fig_show(Res *r, const char *caption, gboolean show)
{
	GtkWidget *v = fig_value(r, caption);
	GtkWidget *parent;

	if (!v)
		return;
	parent = gtk_widget_get_parent(v);
	if (GTK_IS_GRID(parent)) {
		int top = 0;
		GtkWidget *cap;

		gtk_container_child_get(GTK_CONTAINER(parent), v, "top-attach",
					&top, NULL);
		cap = gtk_grid_get_child_at(GTK_GRID(parent), 0, top);
		show_alone(v, show);
		if (cap)
			show_alone(cap, show);
	} else if (parent) {
		/* A big figure: its box, the labels inside stay shown. */
		show_alone(parent, show);
	}
}

/* ---- panel pieces ------------------------------------------------------- */

/* "caption ............ scale" above a graph. */
static GtkWidget *caption_row(const char *caption, GtkWidget **scale)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	gtk_box_pack_start(GTK_BOX(box),
			   label_new(caption, "tv-perf-caption", TRUE), TRUE,
			   TRUE, 0);
	*scale = label_new("", "tv-perf-caption", TRUE);
	gtk_label_set_xalign(GTK_LABEL(*scale), 1);
	gtk_box_pack_end(GTK_BOX(box), *scale, FALSE, FALSE, 0);
	return box;
}

/* "60 seconds ............ 0" below a graph. */
static GtkWidget *time_row(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *zero = label_new("0", "tv-perf-caption", TRUE);
	char *t = g_strdup_printf("%d seconds", TV_GRAPH_SECONDS);

	gtk_box_pack_start(GTK_BOX(box), label_new(t, "tv-perf-caption", TRUE),
			   TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(box), zero, FALSE, FALSE, 0);
	g_free(t);
	return box;
}

/* A short line in the series style, for a legend. */
static gboolean swatch_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	const double *c = g_object_get_data(G_OBJECT(w), "colour");
	double h = gtk_widget_get_allocated_height(w);
	double wd = gtk_widget_get_allocated_width(w);

	cairo_set_source_rgb(cr, c[0], c[1], c[2]);
	cairo_set_line_width(cr, 2);
	if (GPOINTER_TO_INT(data)) {
		static const double dash[] = { 4, 3 };
		cairo_set_dash(cr, dash, 2, 0);
	}
	cairo_move_to(cr, 0, floor(h / 2) + 0.5);
	cairo_line_to(cr, wd, floor(h / 2) + 0.5);
	cairo_stroke(cr);
	return FALSE;
}

static GtkWidget *legend(ResKind kind, const char *solid, const char *dashed)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	const char *names[2] = { solid, dashed };

	for (int i = 0; i < 2; i++) {
		GtkWidget *sw = gtk_drawing_area_new();

		gtk_widget_set_size_request(sw, 22, 12);
		gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
		g_object_set_data(G_OBJECT(sw), "colour",
				  (gpointer)colours[kind]);
		g_signal_connect(sw, "draw", G_CALLBACK(swatch_draw),
				 GINT_TO_POINTER(i));
		gtk_box_pack_start(GTK_BOX(box), sw, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(box),
				   label_new(names[i], "tv-perf-caption", TRUE),
				   FALSE, FALSE, i ? 0 : 8);
	}
	return box;
}

static GtkWidget *graph_new(ResKind kind, TvGraphStyle style)
{
	GtkWidget *g = tv_graph_new(style);

	tv_graph_set_color(g, colours[kind][0], colours[kind][1],
			   colours[kind][2]);
	return g;
}

/* The header of a panel: the big title, the device at the right. */
static void panel_header(Res *r, GtkWidget *panel)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

	r->title = label_new("", "tv-perf-title", FALSE);
	r->device = label_new("", "tv-perf-device", FALSE);
	gtk_label_set_xalign(GTK_LABEL(r->device), 1);
	gtk_label_set_ellipsize(GTK_LABEL(r->device), PANGO_ELLIPSIZE_END);
	gtk_label_set_selectable(GTK_LABEL(r->device), TRUE);
	gtk_widget_set_valign(r->device, GTK_ALIGN_BASELINE);
	gtk_widget_set_valign(r->title, GTK_ALIGN_BASELINE);
	gtk_box_pack_start(GTK_BOX(box), r->title, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(box), r->device, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(panel), box, FALSE, FALSE, 0);
}

/* The two figure grids below the graphs. */
static void figure_grids(GtkWidget *panel, GtkWidget **big, GtkWidget **info)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 40);

	*big = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(*big), 28);
	gtk_grid_set_row_spacing(GTK_GRID(*big), 10);
	*info = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(*info), 16);
	gtk_grid_set_row_spacing(GTK_GRID(*info), 4);
	gtk_widget_set_valign(*info, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(box), *big, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), *info, FALSE, FALSE, 0);
	gtk_widget_set_margin_top(box, 14);
	gtk_box_pack_start(GTK_BOX(panel), box, FALSE, FALSE, 0);
}

/* ---- the memory composition bar ----------------------------------------- */

static gboolean bar_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	Res *r = data;
	const double *c = colours[RES_MEM];
	double wd = gtk_widget_get_allocated_width(w) - 2;
	double h = gtk_widget_get_allocated_height(w) - 2;
	static const double alpha[3] = { 0.55, 0.20, 0.0 };
	double x = 1;

	gtk_render_background(gtk_widget_get_style_context(w), cr, 0, 0,
			      wd + 2, h + 2);
	for (int i = 0; i < 3; i++) {
		double pw = r->parts[i] * wd;

		if (pw <= 0)
			continue;
		if (alpha[i] > 0) {
			cairo_rectangle(cr, x, 1, pw, h);
			cairo_set_source_rgba(cr, c[0], c[1], c[2], alpha[i]);
			cairo_fill(cr);
		}
		x += pw;
		if (i < 2 && x < wd) {
			cairo_move_to(cr, floor(x) + 0.5, 1);
			cairo_line_to(cr, floor(x) + 0.5, h + 1);
			cairo_set_source_rgba(cr, c[0], c[1], c[2], 0.85);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
	}
	cairo_rectangle(cr, 0.5, 0.5, wd + 1, h + 1);
	cairo_set_source_rgba(cr, c[0], c[1], c[2], 0.85);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	return FALSE;
}

/* ---- building the panels ------------------------------------------------ */

static void cpu_logical_grid(Perf *p, Res *r)
{
	const TvCpuLoad *l = tv_sysinfo_cpu(p->si);
	GtkWidget *grid = gtk_grid_new();
	int n = MAX(l->ncpu, 1), cols = 1;

	while (cols * cols < n)
		cols++;
	/* Wider than tall, like the area they fill. */
	if (cols < n && (cols + 1) * (cols - 1) >= n)
		cols++;
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
	gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
	gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
	r->cpu_graphs = g_ptr_array_new();
	for (int i = 0; i < n; i++) {
		GtkWidget *g = graph_new(RES_CPU, TV_GRAPH_SMALL);
		char *tip = g_strdup_printf("CPU %d", i);

		gtk_widget_set_tooltip_text(g, tip);
		g_free(tip);
		gtk_grid_attach(GTK_GRID(grid), g, i % cols, i / cols, 1, 1);
		g_ptr_array_add(r->cpu_graphs, g);
	}
	gtk_stack_add_named(GTK_STACK(r->graph_stack), grid, "logical");
}

static gboolean on_graph_button(GtkWidget *w, GdkEventButton *ev,
				gpointer data)
{
	Perf *p = data;
	GtkWidget *menu;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	menu = gtk_menu_new_from_model(G_MENU_MODEL(p->menu));
	gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
	tv_menu_free_when_closed(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	return TRUE;
}

/* A graph area that offers the page's menu on a right click. */
static GtkWidget *with_menu(Perf *p, GtkWidget *child)
{
	GtkWidget *ev = gtk_event_box_new();

	gtk_container_add(GTK_CONTAINER(ev), child);
	gtk_widget_set_hexpand(ev, TRUE);
	gtk_widget_set_vexpand(ev, TRUE);
	g_signal_connect(ev, "button-press-event", G_CALLBACK(on_graph_button),
			 p);
	return ev;
}

static void build_cpu(Perf *p, Res *r, GtkWidget *panel)
{
	const TvCpuInfo *ci = tv_sysinfo_cpu_info(p->si);
	GtkWidget *scale, *big, *info;
	int row = 0;

	gtk_box_pack_start(GTK_BOX(panel),
			   caption_row("% Utilization over 60 seconds", &scale),
			   FALSE, FALSE, 0);
	r->graph_stack = gtk_stack_new();
	gtk_stack_set_homogeneous(GTK_STACK(r->graph_stack), TRUE);
	r->graph = graph_new(RES_CPU, TV_GRAPH_LARGE);
	tv_graph_set_scale_label(r->graph, GTK_LABEL(scale), fmt_percent);
	gtk_stack_add_named(GTK_STACK(r->graph_stack), r->graph, "overall");
	cpu_logical_grid(p, r);
	gtk_box_pack_start(GTK_BOX(panel), with_menu(p, r->graph_stack), TRUE,
			   TRUE, 0);
	gtk_box_pack_start(GTK_BOX(panel), time_row(), FALSE, FALSE, 0);

	figure_grids(panel, &big, &info);
	add_big(r, big, 0, 0, "Utilization");
	add_big(r, big, 1, 0, "Speed");
	add_big(r, big, 0, 1, "Processes");
	add_big(r, big, 1, 1, "Threads");
	add_big(r, big, 0, 2, "Up time");

	add_info(r, info, row++, "Base speed:");
	add_info(r, info, row++, "Sockets:");
	add_info(r, info, row++, "Cores:");
	add_info(r, info, row++, "Logical processors:");
	add_info(r, info, row++, "Virtual machine:");
	add_info(r, info, row++, "L1 cache:");
	add_info(r, info, row++, "L2 cache:");
	add_info(r, info, row++, "L3 cache:");

	set_text(r->title, "CPU");
	set_text(r->device, ci->model[0] ? ci->model :
			    ci->vendor[0] ? ci->vendor :
					    "Processor");
	fig_set_take(r, "Base speed:", tv_format_mhz(ci->base_mhz));
	fig_set_take(r, "Sockets:", g_strdup_printf("%d", ci->sockets));
	fig_set_take(r, "Cores:", g_strdup_printf("%d", ci->cores));
	fig_set_take(r, "Logical processors:",
		     g_strdup_printf("%d", ci->logical));
	fig_set(r, "Virtual machine:", ci->virtual_machine ? "Yes" : "No");
	fig_set_take(r, "L1 cache:", tv_format_bytes(ci->l1_bytes));
	fig_set_take(r, "L2 cache:", tv_format_bytes(ci->l2_bytes));
	fig_set_take(r, "L3 cache:", tv_format_bytes(ci->l3_bytes));
	fig_show(r, "L1 cache:", ci->l1_bytes > 0);
	fig_show(r, "L2 cache:", ci->l2_bytes > 0);
	fig_show(r, "L3 cache:", ci->l3_bytes > 0);
	fig_show(r, "Base speed:", ci->base_mhz > 0);
}

static void build_mem(Perf *p, Res *r, GtkWidget *panel)
{
	GtkWidget *scale, *big, *info, *dummy;
	int row = 0;

	gtk_box_pack_start(GTK_BOX(panel), caption_row("Memory usage", &scale),
			   FALSE, FALSE, 0);
	r->graph = graph_new(RES_MEM, TV_GRAPH_LARGE);
	tv_graph_set_scale_label(r->graph, GTK_LABEL(scale), fmt_bytes);
	gtk_box_pack_start(GTK_BOX(panel), with_menu(p, r->graph), TRUE, TRUE,
			   0);
	gtk_box_pack_start(GTK_BOX(panel), time_row(), FALSE, FALSE, 0);

	{
		GtkWidget *cap = caption_row("Memory composition", &dummy);
		gtk_widget_set_margin_top(cap, 10);
		gtk_box_pack_start(GTK_BOX(panel), cap, FALSE, FALSE, 0);
	}
	r->bar = gtk_drawing_area_new();
	gtk_widget_set_size_request(r->bar, -1, 34);
	gtk_style_context_add_class(gtk_widget_get_style_context(r->bar),
				    GTK_STYLE_CLASS_VIEW);
	g_signal_connect(r->bar, "draw", G_CALLBACK(bar_draw), r);
	gtk_box_pack_start(GTK_BOX(panel), r->bar, FALSE, FALSE, 0);

	figure_grids(panel, &big, &info);
	add_big(r, big, 0, 0, "In use");
	add_big(r, big, 1, 0, "Available");
	add_big(r, big, 0, 1, "Committed");
	add_big(r, big, 1, 1, "Cached");
	add_info(r, info, row++, "Total:");
	add_info(r, info, row++, "Free:");
	add_info(r, info, row++, "Buffers:");
	set_text(r->title, "Memory");
}

static void build_disk(Perf *p, Res *r, GtkWidget *panel)
{
	GtkWidget *scale, *scale2, *big, *info, *cap;
	int row = 0;

	gtk_box_pack_start(GTK_BOX(panel), caption_row("Active time", &scale),
			   FALSE, FALSE, 0);
	r->graph = graph_new(RES_DISK, TV_GRAPH_LARGE);
	tv_graph_set_scale_label(r->graph, GTK_LABEL(scale), fmt_percent);
	gtk_box_pack_start(GTK_BOX(panel), with_menu(p, r->graph), TRUE, TRUE,
			   0);
	gtk_box_pack_start(GTK_BOX(panel), time_row(), FALSE, FALSE, 0);

	cap = caption_row("Disk transfer rate", &scale2);
	gtk_widget_set_margin_top(cap, 10);
	gtk_box_pack_start(GTK_BOX(panel), cap, FALSE, FALSE, 0);
	r->graph2 = graph_new(RES_DISK, TV_GRAPH_LARGE);
	gtk_widget_set_size_request(r->graph2, 240, 90);
	tv_graph_set_dual(r->graph2, TRUE);
	tv_graph_set_binary(r->graph2, TRUE);
	tv_graph_set_scale(r->graph2, 0, 1024 * 1024);
	tv_graph_set_scale_label(r->graph2, GTK_LABEL(scale2), fmt_byte_rate);
	gtk_box_pack_start(GTK_BOX(panel), with_menu(p, r->graph2), TRUE, TRUE,
			   0);
	gtk_box_pack_start(GTK_BOX(panel), time_row(), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(panel),
			   legend(RES_DISK, "Read speed", "Write speed"), FALSE,
			   FALSE, 4);

	figure_grids(panel, &big, &info);
	add_big(r, big, 0, 0, "Active time");
	add_big(r, big, 1, 0, "Average response time");
	add_big(r, big, 0, 1, "Read speed");
	add_big(r, big, 1, 1, "Write speed");
	add_info(r, info, row++, "Capacity:");
	add_info(r, info, row++, "System disk:");
	add_info(r, info, row++, "Type:");
	add_info(r, info, row++, "Device:");
}

static void build_net(Perf *p, Res *r, GtkWidget *panel)
{
	GtkWidget *scale, *big, *info;
	int row = 0;

	gtk_box_pack_start(GTK_BOX(panel), caption_row("Throughput", &scale),
			   FALSE, FALSE, 0);
	r->graph = graph_new(RES_NET, TV_GRAPH_LARGE);
	tv_graph_set_dual(r->graph, TRUE);
	tv_graph_set_scale(r->graph, 0, 100 * 1000);
	tv_graph_set_scale_label(r->graph, GTK_LABEL(scale), fmt_bit_rate);
	gtk_box_pack_start(GTK_BOX(panel), with_menu(p, r->graph), TRUE, TRUE,
			   0);
	gtk_box_pack_start(GTK_BOX(panel), time_row(), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(panel), legend(RES_NET, "Receive", "Send"),
			   FALSE, FALSE, 4);

	figure_grids(panel, &big, &info);
	add_big(r, big, 0, 0, "Send");
	add_big(r, big, 1, 0, "Receive");
	add_info(r, info, row++, "Adapter name:");
	add_info(r, info, row++, "Connection type:");
	add_info(r, info, row++, "State:");
	add_info(r, info, row++, "IPv4 address:");
	add_info(r, info, row++, "MAC address:");
}

/* ---- resources ---------------------------------------------------------- */

static void res_free(gpointer data)
{
	Res *r = data;

	if (r->cpu_graphs)
		g_ptr_array_unref(r->cpu_graphs);
	g_ptr_array_unref(r->figs);
	g_free(r);
}

static Res *res_find(Perf *p, const char *key)
{
	for (guint i = 0; i < p->res->len; i++) {
		Res *r = p->res->pdata[i];
		if (!strcmp(r->key, key))
			return r;
	}
	return NULL;
}

static Res *res_add(Perf *p, ResKind kind, const char *key)
{
	Res *r = g_new0(Res, 1);
	GtkWidget *box, *text, *panel, *scroll;

	r->kind = kind;
	g_strlcpy(r->key, key, sizeof(r->key));
	r->figs = g_ptr_array_new_with_free_func(fig_free);

	/* The row in the list. */
	r->row = gtk_list_box_row_new();
	g_object_set_data(G_OBJECT(r->row), "tv-res", r);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	r->mini = graph_new(kind, TV_GRAPH_COMPACT);
	gtk_widget_set_valign(r->mini, GTK_ALIGN_CENTER);
	if (kind == RES_NET) {
		tv_graph_set_dual(r->mini, TRUE);
		tv_graph_set_scale(r->mini, 0, 100 * 1000);
	}
	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	r->row_title = label_new("", "tv-perf-row-title", FALSE);
	r->row_line1 = label_new("", "tv-perf-row-line", TRUE);
	r->row_line2 = label_new("", "tv-perf-row-line", TRUE);
	gtk_label_set_ellipsize(GTK_LABEL(r->row_title), PANGO_ELLIPSIZE_END);
	gtk_label_set_ellipsize(GTK_LABEL(r->row_line1), PANGO_ELLIPSIZE_END);
	gtk_label_set_ellipsize(GTK_LABEL(r->row_line2), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(text), r->row_title, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text), r->row_line1, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text), r->row_line2, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), r->mini, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(r->row), box);
	/* The second line only for disks and adapters. */
	gtk_widget_set_no_show_all(r->row_line2, TRUE);
	gtk_widget_show_all(r->row);

	/* Its panel. */
	panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_style_context_add_class(gtk_widget_get_style_context(panel),
				    "tv-perf-panel");
	panel_header(r, panel);
	switch (kind) {
	case RES_CPU:
		build_cpu(p, r, panel);
		r->order = 0;
		break;
	case RES_MEM:
		build_mem(p, r, panel);
		r->order = 1;
		break;
	case RES_DISK:
		build_disk(p, r, panel);
		break;
	case RES_NET:
		build_net(p, r, panel);
		break;
	}
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), panel);
	r->panel = scroll;
	gtk_widget_show_all(scroll);
	if (kind == RES_CPU)
		gtk_stack_set_visible_child_name(GTK_STACK(r->graph_stack),
						 p->logical ? "logical" :
							      "overall");
	gtk_stack_add_named(GTK_STACK(p->stack), scroll, r->key);

	g_ptr_array_add(p->res, r);
	gtk_container_add(GTK_CONTAINER(p->list), r->row);
	return r;
}

static void res_remove(Perf *p, Res *r)
{
	GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(p->list));
	gboolean was_selected = sel == GTK_LIST_BOX_ROW(r->row);

	gtk_widget_destroy(r->row);
	gtk_widget_destroy(r->panel);
	g_ptr_array_remove(p->res, r);
	if (was_selected) {
		GtkListBoxRow *first =
			gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->list), 0);
		if (first)
			gtk_list_box_select_row(GTK_LIST_BOX(p->list), first);
	}
}

/* ---- updating ----------------------------------------------------------- */

static void update_cpu(Perf *p, Res *r)
{
	const TvCpuInfo *ci = tv_sysinfo_cpu_info(p->si);
	const TvCpuLoad *l = tv_sysinfo_cpu(p->si);
	double v = l->valid ? l->total : 0;
	char *speed = tv_format_mhz(ci->speed_mhz);

	tv_graph_add(r->mini, v, 0);
	tv_graph_add(r->graph, v, 0);
	for (guint i = 0; i < r->cpu_graphs->len && (int)i < l->ncpu; i++)
		tv_graph_add(r->cpu_graphs->pdata[i],
			     l->valid ? l->per_cpu[i] : 0, 0);

	set_text(r->row_title, "CPU");
	set_text_take(r->row_line1, g_strdup_printf("%.0f%%  %s", v, speed));
	fig_set_take(r, "Utilization", g_strdup_printf("%.0f%%", v));
	fig_set(r, "Speed", speed);
	fig_set_take(r, "Processes", g_strdup_printf("%d", l->processes));
	fig_set_take(r, "Threads", g_strdup_printf("%d", l->threads));
	fig_set_take(r, "Up time", tv_format_uptime(l->uptime_s));
	g_free(speed);
}

static void update_mem(Perf *p, Res *r)
{
	const TvMemInfo *m = tv_sysinfo_mem(p->si);
	double total = (double)MAX(m->total, 1);
	double pct = (double)m->in_use * 100.0 / total;
	char *used = tv_format_bytes(m->in_use);
	char *all = tv_format_bytes(m->total);
	guint64 standby = m->cached + m->buffers;
	guint64 busy = m->total > m->free + standby ?
			       m->total - m->free - standby :
			       0;

	if (tv_graph_get_scale(r->graph) != total)
		tv_graph_set_scale(r->graph, total, 0);
	tv_graph_add(r->mini, pct, 0);
	tv_graph_add(r->graph, (double)m->in_use, 0);

	set_text(r->row_title, "Memory");
	set_text_take(r->row_line1, g_strdup_printf("%s / %s (%.0f%%)", used,
						    all, pct));
	set_text(r->device, all);
	fig_set(r, "In use", used);
	fig_set_take(r, "Available", tv_format_bytes(m->available));
	fig_set_take(r, "Committed", tv_format_bytes(m->committed));
	fig_set_take(r, "Cached", tv_format_bytes(m->cached));
	fig_set(r, "Total:", all);
	fig_set_take(r, "Free:", tv_format_bytes(m->free));
	fig_set_take(r, "Buffers:", tv_format_bytes(m->buffers));
	fig_show(r, "Committed", m->have_committed);
	fig_show(r, "Cached", m->have_cached);
	fig_show(r, "Buffers:", m->have_buffers);

	r->parts[0] = (double)busy / total;
	r->parts[1] = (double)standby / total;
	r->parts[2] = (double)m->free / total;
	{
		char *c = tv_format_bytes(standby);
		char *f = tv_format_bytes(m->free);
		char *b = tv_format_bytes(busy);
		char *tip = g_strdup_printf(
			"In use: %s\nCached (given back when needed): %s\n"
			"Free: %s",
			b, c, f);
		gtk_widget_set_tooltip_text(r->bar, tip);
		g_free(tip);
		g_free(b);
		g_free(c);
		g_free(f);
	}
	gtk_widget_queue_draw(r->bar);
	g_free(used);
	g_free(all);
}

static void update_disk(Res *r, const TvDisk *d)
{
	char *rd = tv_format_byte_rate(d->read_bps);
	char *wr = tv_format_byte_rate(d->write_bps);
	char *title = g_strdup_printf("Disk %d (%s)", d->index, d->name);

	r->order = 100 + d->index;
	tv_graph_add(r->mini, d->active, 0);
	tv_graph_add(r->graph, d->active, 0);
	tv_graph_add(r->graph2, d->read_bps, d->write_bps);

	set_text(r->row_title, title);
	set_text(r->title, title);
	set_text(r->row_line1, tv_disk_kind_name(d->kind));
	set_text_take(r->row_line2, g_strdup_printf("%.0f%%", d->active));
	gtk_widget_show(r->row_line2);
	set_text(r->device, d->model);
	fig_set_take(r, "Active time", g_strdup_printf("%.0f%%", d->active));
	fig_set_take(r, "Average response time",
		     g_strdup_printf("%.1f ms", d->response_ms));
	fig_set(r, "Read speed", rd);
	fig_set(r, "Write speed", wr);
	fig_set_take(r, "Capacity:", d->size ? tv_format_bytes(d->size) :
					       g_strdup("No medium"));
	fig_set(r, "System disk:", d->system ? "Yes" : "No");
	fig_set(r, "Type:", tv_disk_kind_name(d->kind));
	fig_set_take(r, "Device:", g_strdup_printf("/dev/%s", d->name));
	g_free(rd);
	g_free(wr);
	g_free(title);
}

static void update_net(Res *r, const TvNet *n, int i)
{
	const char *type = n->wireless ? "Wireless" : "Ethernet";
	char *rx = tv_format_bit_rate(n->rx_bps);
	char *tx = tv_format_bit_rate(n->tx_bps);

	r->order = 1000 + i;
	tv_graph_add(r->mini, n->rx_bps, n->tx_bps);
	tv_graph_add(r->graph, n->rx_bps, n->tx_bps);

	set_text(r->row_title, type);
	set_text(r->title, type);
	set_text(r->row_line1, n->name);
	set_text_take(r->row_line2,
		      n->up ? g_strdup_printf("S: %s  R: %s", tx, rx) :
			      g_strdup("Not connected"));
	gtk_widget_show(r->row_line2);
	set_text(r->device, n->name);
	fig_set(r, "Send", tx);
	fig_set(r, "Receive", rx);
	fig_set(r, "Adapter name:", n->name);
	fig_set(r, "Connection type:", type);
	fig_set(r, "State:", n->up ? "Connected" : "Not connected");
	fig_set(r, "IPv4 address:", n->ipv4[0] ? n->ipv4 : "None");
	fig_set(r, "MAC address:", n->mac);
	g_free(rx);
	g_free(tx);
}

static void update(Perf *p)
{
	char key[48];
	gboolean changed = FALSE;

	for (guint i = 0; i < p->res->len; i++)
		((Res *)p->res->pdata[i])->seen = FALSE;

	update_cpu(p, res_find(p, "cpu"));
	update_mem(p, res_find(p, "mem"));
	res_find(p, "cpu")->seen = TRUE;
	res_find(p, "mem")->seen = TRUE;

	for (int i = 0; i < tv_sysinfo_n_disks(p->si); i++) {
		const TvDisk *d = tv_sysinfo_disk(p->si, i);
		Res *r;
		int order;

		g_snprintf(key, sizeof(key), "disk:%s", d->name);
		r = res_find(p, key);
		if (!r) {
			r = res_add(p, RES_DISK, key);
			changed = TRUE;
		}
		order = r->order;
		update_disk(r, d);
		changed |= order != r->order;
		r->seen = TRUE;
	}
	for (int i = 0; i < tv_sysinfo_n_nets(p->si); i++) {
		const TvNet *n = tv_sysinfo_net(p->si, i);
		Res *r;
		int order;

		g_snprintf(key, sizeof(key), "net:%s", n->name);
		r = res_find(p, key);
		if (!r) {
			r = res_add(p, RES_NET, key);
			changed = TRUE;
		}
		order = r->order;
		update_net(r, n, i);
		changed |= order != r->order;
		r->seen = TRUE;
	}

	for (guint i = 0; i < p->res->len;) {
		Res *r = p->res->pdata[i];
		if (!r->seen) {
			res_remove(p, r);
			changed = TRUE;
		} else {
			i++;
		}
	}
	if (changed)
		gtk_list_box_invalidate_sort(GTK_LIST_BOX(p->list));
}

/* ---- the list ----------------------------------------------------------- */

static int row_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data)
{
	Res *x = g_object_get_data(G_OBJECT(a), "tv-res");
	Res *y = g_object_get_data(G_OBJECT(b), "tv-res");

	(void)data;

	return (x->order > y->order) - (x->order < y->order);
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	Perf *p = data;
	Res *r;

	(void)box;
	if (!row)
		return;
	r = g_object_get_data(G_OBJECT(row), "tv-res");
	gtk_stack_set_visible_child(GTK_STACK(p->stack), r->panel);
}

/* ---- the menu ----------------------------------------------------------- */

static void set_logical(Perf *p, gboolean logical)
{
	Res *r = res_find(p, "cpu");

	p->logical = logical;
	if (r)
		gtk_stack_set_visible_child_name(GTK_STACK(r->graph_stack),
						 logical ? "logical" :
							   "overall");
}

static void on_cpu_graph(GSimpleAction *a, GVariant *v, gpointer data)
{
	Perf *p = data;
	Res *r = res_find(p, "cpu");

	g_simple_action_set_state(a, v);
	set_logical(p, !strcmp(g_variant_get_string(v, NULL), "logical"));
	/* Changing the processor graph shows the processor. */
	if (r)
		gtk_list_box_select_row(GTK_LIST_BOX(p->list),
					GTK_LIST_BOX_ROW(r->row));
}

/* The selected resource's figures as text, for the clipboard. */
static void on_copy(GSimpleAction *a, GVariant *v, gpointer data)
{
	Perf *p = data;
	GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(p->list));
	GString *s;
	Res *r;

	(void)a;
	(void)v;
	if (!row)
		return;
	r = g_object_get_data(G_OBJECT(row), "tv-res");
	s = g_string_new(NULL);
	g_string_append_printf(s, "%s\n", gtk_label_get_text(GTK_LABEL(r->title)));
	if (*gtk_label_get_text(GTK_LABEL(r->device)))
		g_string_append_printf(s, "\t%s\n",
				       gtk_label_get_text(GTK_LABEL(r->device)));
	g_string_append_c(s, '\n');
	for (guint i = 0; i < r->figs->len; i++) {
		Fig *f = r->figs->pdata[i];
		size_t n = strlen(f->caption);

		if (!gtk_widget_get_visible(f->value))
			continue;
		g_string_append_printf(s, "\t%.*s:\t%s\n",
				       (int)(n && f->caption[n - 1] == ':' ?
						     n - 1 :
						     n),
				       f->caption,
				       gtk_label_get_text(GTK_LABEL(f->value)));
	}
	gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
			       s->str, (gint)s->len);
	g_string_free(s, TRUE);
}

static void make_actions(Perf *p, GtkWidget *page)
{
	static const GActionEntry entries[] = {
		{ "cpu-graph", NULL, "s", "'overall'", on_cpu_graph, { 0 } },
		{ "copy", on_copy, NULL, NULL, NULL, { 0 } },
	};
	GMenu *graph = g_menu_new();
	GMenu *rest = g_menu_new();

	p->actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(p->actions), entries,
					G_N_ELEMENTS(entries), p);
	gtk_widget_insert_action_group(page, "perf",
				       G_ACTION_GROUP(p->actions));

	p->menu = g_menu_new();
	g_menu_append(graph, "Overall utilization", "perf.cpu-graph::overall");
	g_menu_append(graph, "Logical processors", "perf.cpu-graph::logical");
	g_menu_append_section(p->menu, "Change processor graph to",
			      G_MENU_MODEL(graph));
	g_menu_append(rest, "Copy", "perf.copy");
	g_menu_append_section(p->menu, NULL, G_MENU_MODEL(rest));
	g_object_unref(graph);
	g_object_unref(rest);
}

/* ---- the page ----------------------------------------------------------- */

static void perf_free(gpointer data)
{
	Perf *p = data;

	g_ptr_array_unref(p->res);
	tv_sysinfo_free(p->si);
	g_object_unref(p->actions);
	g_object_unref(p->menu);
	g_free(p);
}

static GtkWidget *create(void)
{
	Perf *p = g_new0(Perf, 1);
	GtkWidget *page, *side;

	install_css();
	p->si = tv_sysinfo_new();
	p->res = g_ptr_array_new_with_free_func(res_free);

	page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	g_object_set_data_full(G_OBJECT(page), "tv-perf", p, perf_free);
	make_actions(p, page);

	p->list = gtk_list_box_new();
	gtk_style_context_add_class(gtk_widget_get_style_context(p->list),
				    "tv-perf-list");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list),
					GTK_SELECTION_BROWSE);
	gtk_list_box_set_sort_func(GTK_LIST_BOX(p->list), row_sort, NULL,
				   NULL);
	side = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(side, 230, -1);
	gtk_widget_set_margin_start(p->list, 6);
	gtk_widget_set_margin_end(p->list, 6);
	gtk_widget_set_margin_top(p->list, 6);
	gtk_container_add(GTK_CONTAINER(side), p->list);

	p->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(p->stack),
				      GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_widget_set_hexpand(p->stack, TRUE);

	gtk_box_pack_start(GTK_BOX(page), side, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page),
			   gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE,
			   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page), p->stack, TRUE, TRUE, 0);

	/* The first sample, which the first tick measures its rates
	 * against; then the resources that are always there. */
	tv_sysinfo_sample(p->si);
	res_add(p, RES_CPU, "cpu");
	res_add(p, RES_MEM, "mem");
	update(p);
	g_signal_connect(p->list, "row-selected", G_CALLBACK(on_row_selected),
			 p);
	gtk_list_box_select_row(GTK_LIST_BOX(p->list),
				gtk_list_box_get_row_at_index(
					GTK_LIST_BOX(p->list), 0));
	gtk_widget_show_all(page);
	return page;
}

static void tick(GtkWidget *page, gboolean visible)
{
	Perf *p = perf_of(page);

	/* Every tick, shown or not: the graphs keep their history. */
	(void)visible;
	tv_sysinfo_sample(p->si);
	update(p);
}

static void fill_commands(GtkWidget *page, GtkWidget *bar)
{
	Perf *p = perf_of(page);
	GtkWidget *b = gtk_menu_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	gtk_box_pack_start(GTK_BOX(box),
			   gtk_image_new_from_icon_name("view-more-symbolic",
							GTK_ICON_SIZE_BUTTON),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), gtk_label_new("View"), FALSE, FALSE,
			   0);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(b, "Change the processor graph, copy the "
				       "figures");
	gtk_style_context_add_class(gtk_widget_get_style_context(b),
				    "tv-command");
	/* The bar is not inside the page: the button gets the actions
	 * itself. */
	gtk_widget_insert_action_group(b, "perf", G_ACTION_GROUP(p->actions));
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(b),
				       G_MENU_MODEL(p->menu));
	gtk_box_pack_end(GTK_BOX(bar), b, FALSE, FALSE, 0);
	gtk_widget_show_all(b);
}

const TvPageDesc tv_page_performance = {
	.id = "performance",
	.title = "Performance",
	.icon = "computer-symbolic",
	.create = create,
	.tick = tick,
	.fill_commands = fill_commands,
	.filter = NULL,
};
