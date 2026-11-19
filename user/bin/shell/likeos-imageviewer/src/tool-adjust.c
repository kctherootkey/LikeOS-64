// LikeOS -- LikeOS Image Viewer: the Adjust tool.
//
// The light, colour and detail sliders over the edit state's IvAdjust,
// under a small live histogram of the preview.
//
//   Histogram   red, green and blue drawn over each other (where they
//               meet they add up to white), the brightness as a light
//               outline on top, and a mark at either end that lights up
//               when more than a sliver of the picture is pure black or
//               pure white (clipped).  It follows every preview render,
//               so dragging a slider shows the tones moving.  The top
//               bar's Compare (or the \ key) shows the unedited picture
//               while it is held.
//   Light       brightness, exposure, contrast, highlights, shadows,
//               whites, blacks, vignette
//   Color       saturation, vibrance, warmth, tint -- their tracks show
//               which way each one goes (grey to vivid, blue to amber,
//               green to magenta)
//   Detail      clarity, sharpness, noise reduction, fade, grain (with
//               a switch for fine instead of coarse grain)
//
// Each slider is an iv_panel_slider(): a drag is a live preview and its
// release one undo step; keys and the wheel make one step once they rest;
// double-clicking the slider or its label puts it back to neutral.  Every
// section has a reset button of its own (active only while something in
// it is changed) and the panel ends with one for all of them.  The
// sliders take the keyboard focus: Tab moves between them, the arrows
// change the focused one by 1, Page Up/Down by 10.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "editor.h"

typedef enum { SEC_LIGHT, SEC_COLOR, SEC_DETAIL, SEC_COUNT } Section;

/* How a slider's value is kept in IvAdjust. */
typedef enum {
	KIND_PLAIN,	/* the field itself, -100..100 */
	KIND_POSITIVE,	/* the field, 0..100 (negative does nothing) */
	KIND_GRAIN,	/* |grain| 0..100; the sign is the fine-grain switch */
} Kind;

typedef struct {
	Section section;
	const char *label;
	size_t offset;
	Kind kind;
	const char *track;	/* style class of a coloured track, or NULL */
	const char *tip;
} Field;

static const Field fields[] = {
	{ SEC_LIGHT, "Brightness", offsetof(IvAdjust, brightness), KIND_PLAIN,
	  "iv-track-tone", "Lifts or lowers the middle tones; black and white "
			   "stay where they are" },
	{ SEC_LIGHT, "Exposure", offsetof(IvAdjust, exposure), KIND_PLAIN,
	  "iv-track-tone", "Up to two stops more or less light, the highlights "
			   "rolling off softly" },
	{ SEC_LIGHT, "Contrast", offsetof(IvAdjust, contrast), KIND_PLAIN, NULL,
	  "Spreads the tones apart from middle grey, or brings them together" },
	{ SEC_LIGHT, "Highlights", offsetof(IvAdjust, highlights), KIND_PLAIN, NULL,
	  "Brings back detail in bright areas (left) or brightens them (right)" },
	{ SEC_LIGHT, "Shadows", offsetof(IvAdjust, shadows), KIND_PLAIN, NULL,
	  "Opens up dark areas (right) or deepens them (left)" },
	{ SEC_LIGHT, "Whites", offsetof(IvAdjust, whites), KIND_PLAIN, NULL,
	  "Moves the white point" },
	{ SEC_LIGHT, "Blacks", offsetof(IvAdjust, blacks), KIND_PLAIN, NULL,
	  "Moves the black point: lifts or crushes the deepest tones" },
	{ SEC_LIGHT, "Vignette", offsetof(IvAdjust, vignette), KIND_PLAIN,
	  "iv-track-tone", "Darkens (left) or lightens (right) the corners" },
	{ SEC_COLOR, "Saturation", offsetof(IvAdjust, saturation), KIND_PLAIN,
	  "iv-track-saturation", "All colours stronger, or weaker down to grey" },
	{ SEC_COLOR, "Vibrance", offsetof(IvAdjust, vibrance), KIND_PLAIN,
	  "iv-track-saturation", "Strengthens the muted colours and spares skin "
				 "and colours that are strong already" },
	{ SEC_COLOR, "Warmth", offsetof(IvAdjust, warmth), KIND_PLAIN,
	  "iv-track-warmth", "White balance from blue (left) to amber (right)" },
	{ SEC_COLOR, "Tint", offsetof(IvAdjust, tint), KIND_PLAIN, "iv-track-tint",
	  "White balance from green (left) to magenta (right)" },
	{ SEC_DETAIL, "Clarity", offsetof(IvAdjust, clarity), KIND_PLAIN, NULL,
	  "Local contrast of the middle-sized detail; left softens" },
	{ SEC_DETAIL, "Sharpness", offsetof(IvAdjust, sharpness), KIND_PLAIN, NULL,
	  "Crisper edges (right) or a gentle softening (left)" },
	{ SEC_DETAIL, "Noise reduction", offsetof(IvAdjust, denoise),
	  KIND_POSITIVE, NULL, "Smooths grain and colour noise, keeping edges" },
	{ SEC_DETAIL, "Fade", offsetof(IvAdjust, fade), KIND_PLAIN, NULL,
	  "A washed-out matte look (right) or deeper blacks (left)" },
	{ SEC_DETAIL, "Grain", offsetof(IvAdjust, grain), KIND_GRAIN, NULL,
	  "Film grain over the picture" },
};
#define N_FIELDS ((int)G_N_ELEMENTS(fields))

static const struct {
	const char *title;
	IvGlyph glyph;
	const char *undo;
} sections[SEC_COUNT] = {
	[SEC_LIGHT] = { "LIGHT", IV_GLYPH_LIGHT, "Reset light" },
	[SEC_COLOR] = { "COLOR", IV_GLYPH_COLOR, "Reset color" },
	[SEC_DETAIL] = { "DETAIL", IV_GLYPH_DETAIL, "Reset detail" },
};

/* Histogram heights are drawn against this share of the tallest bins, so
 * one huge spike (a sky, a white wall) does not flatten the rest. */
#define HIST_PEAK_QUANTILE 0.985
/* Samples taken for the histogram, at most. */
#define HIST_SAMPLES 160000
/* A clip mark lights up above this fraction of the picture. */
#define CLIP_FRACTION 0.004

static struct {
	IvEditor *ed;
	GtkWidget *panel;
	GtkWidget *rows[G_N_ELEMENTS(fields)];
	GtkWidget *reset[SEC_COUNT];
	GtkWidget *reset_all;
	GtkWidget *fine_switch;
	gboolean fine_grain;	/* the grain is fine (negative) */
	gboolean syncing;

	/* the histogram */
	GtkWidget *hist;
	gboolean have_hist;
	double bins[4][256];	/* R, G, B, luma; fractions of the samples */
	double clip_low, clip_high;	/* fractions at 0 and at 255 */
} A;

/* ---- the values ---------------------------------------------------------------- */

static double *field_ptr(IvAdjust *a, const Field *f)
{
	return (double *)((char *)a + f->offset);
}

static double field_get(const IvAdjust *a, const Field *f)
{
	return *(const double *)((const char *)a + f->offset);
}

static double slider_get(const IvEditState *s, gpointer data)
{
	const Field *f = &fields[GPOINTER_TO_INT(data)];
	double v = field_get(&s->adjust, f);

	switch (f->kind) {
	case KIND_POSITIVE:
		return MAX(0, v);
	case KIND_GRAIN:
		return fabs(v);
	default:
		return v;
	}
}

static void slider_set(IvEditState *s, double v, gpointer data)
{
	const Field *f = &fields[GPOINTER_TO_INT(data)];

	if (f->kind == KIND_GRAIN)
		v = A.fine_grain ? -fabs(v) : fabs(v);
	*field_ptr(&s->adjust, f) = v;
}

static gboolean section_neutral(const IvAdjust *a, Section sec)
{
	for (int i = 0; i < N_FIELDS; i++)
		if (fields[i].section == sec && fabs(field_get(a, &fields[i])) > 1e-9)
			return FALSE;
	return TRUE;
}

static void reset_section(GtkButton *b, gpointer data)
{
	Section sec = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "iv-section"));
	IvEditor *ed = data;
	IvEditState *s;

	if (section_neutral(&iv_editor_committed(ed)->adjust, sec))
		return;
	s = iv_edit_state_copy(iv_editor_committed(ed));
	for (int i = 0; i < N_FIELDS; i++)
		if (fields[i].section == sec)
			*field_ptr(&s->adjust, &fields[i]) = 0;
	iv_editor_apply(ed, s, sections[sec].undo);
}

static void reset_all(GtkButton *b, IvEditor *ed)
{
	IvEditState *s;

	if (iv_adjust_is_neutral(&iv_editor_committed(ed)->adjust))
		return;
	s = iv_edit_state_copy(iv_editor_committed(ed));
	iv_adjust_reset(&s->adjust);
	iv_editor_apply(ed, s, "Reset adjustments");
}

/* The fine-grain switch: with grain set it is an edit of its own (the
 * sign of the value); without, it only says how the next grain looks. */
static void fine_toggled(GObject *sw, GParamSpec *pspec, IvEditor *ed)
{
	gboolean fine = gtk_switch_get_active(GTK_SWITCH(sw));
	const IvEditState *st = iv_editor_committed(ed);
	IvEditState *s;

	if (A.syncing)
		return;
	A.fine_grain = fine;
	if (!st || fabs(st->adjust.grain) < 1e-9)
		return;
	s = iv_edit_state_copy(st);
	s->adjust.grain = fine ? -fabs(s->adjust.grain) : fabs(s->adjust.grain);
	iv_editor_apply(ed, s, "Grain size");
}

/* ---- the histogram ------------------------------------------------------------- */

/* Counts the shown preview (cairo ARGB32, premultiplied) on a grid that
 * takes at most HIST_SAMPLES pixels. */
static void histogram_from(cairo_surface_t *s)
{
	int w, h, stride, step;
	const guint8 *data;
	guint32 counts[4][256];
	guint64 n = 0;

	if (!s || cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE ||
	    cairo_image_surface_get_format(s) != CAIRO_FORMAT_ARGB32) {
		A.have_hist = FALSE;
		return;
	}
	cairo_surface_flush(s);
	w = cairo_image_surface_get_width(s);
	h = cairo_image_surface_get_height(s);
	stride = cairo_image_surface_get_stride(s);
	data = cairo_image_surface_get_data(s);
	if (!data || w <= 0 || h <= 0) {
		A.have_hist = FALSE;
		return;
	}
	step = (int)ceil(sqrt((double)w * h / HIST_SAMPLES));
	step = MAX(1, step);
	memset(counts, 0, sizeof counts);
	for (int y = 0; y < h; y += step) {
		const guint32 *row = (const guint32 *)(data + (gsize)y * stride);

		for (int x = 0; x < w; x += step) {
			guint32 p = row[x];
			unsigned a = p >> 24;
			unsigned r, g, b, l;

			if (a == 0)
				continue;
			r = (p >> 16) & 0xff;
			g = (p >> 8) & 0xff;
			b = p & 0xff;
			if (a < 255) {
				/* Back to straight colour. */
				r = MIN(255u, (r * 255 + a / 2) / a);
				g = MIN(255u, (g * 255 + a / 2) / a);
				b = MIN(255u, (b * 255 + a / 2) / a);
			}
			/* Rec. 709 luma, in integers. */
			l = (r * 2126 + g * 7152 + b * 722 + 5000) / 10000;
			counts[0][r]++;
			counts[1][g]++;
			counts[2][b]++;
			counts[3][MIN(l, 255u)]++;
			n++;
		}
	}
	if (n == 0) {
		A.have_hist = FALSE;
		return;
	}
	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 256; i++)
			A.bins[c][i] = (double)counts[c][i] / n;
	A.clip_low = A.bins[3][0];
	A.clip_high = A.bins[3][255];
	/* A channel clipped counts too: a sky gone pure cyan has lost its
	 * red as much as a white one. */
	for (int c = 0; c < 3; c++) {
		A.clip_low = MAX(A.clip_low, A.bins[c][0]);
		A.clip_high = MAX(A.clip_high, A.bins[c][255]);
	}
	A.have_hist = TRUE;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* The height that fills the box: a high quantile of all bins (the ends,
 * where clipping piles up, left out). */
static double histogram_peak(void)
{
	double all[4 * 254];
	int n = 0;

	for (int c = 0; c < 4; c++)
		for (int i = 1; i < 255; i++)
			all[n++] = A.bins[c][i];
	qsort(all, n, sizeof all[0], cmp_double);
	return MAX(1e-6, all[(int)(HIST_PEAK_QUANTILE * (n - 1))] * 1.08);
}

static void hist_path(cairo_t *cr, const double *bins, double x, double y,
		      double w, double h, double peak, gboolean closed)
{
	cairo_new_path(cr);
	if (closed)
		cairo_move_to(cr, x, y + h);
	for (int i = 0; i < 256; i++) {
		/* A light 3-bin smoothing, so sparse samples do not comb. */
		double v = (bins[MAX(i - 1, 0)] + 2 * bins[i] + bins[MIN(i + 1, 255)]) / 4;
		double px = x + w * i / 255.0;
		double py = y + h - h * MIN(1.0, v / peak);

		if (i == 0 && !closed)
			cairo_move_to(cr, px, py);
		else
			cairo_line_to(cr, px, py);
	}
	if (closed) {
		cairo_line_to(cr, x + w, y + h);
		cairo_close_path(cr);
	}
}

static void lookup(GtkWidget *w, const char *name, const char *fallback,
		   GdkRGBA *c)
{
	if (!gtk_style_context_lookup_color(gtk_widget_get_style_context(w), name,
					    c))
		gdk_rgba_parse(c, fallback);
}

static gboolean hist_draw(GtkWidget *w, cairo_t *cr, gpointer unused)
{
	double W = gtk_widget_get_allocated_width(w);
	double H = gtk_widget_get_allocated_height(w);
	double x = 10, y = 8, iw = W - 20, ih = H - 18, peak;
	static const double rgb[3][3] = {
		{ 0.94, 0.33, 0.33 },
		{ 0.33, 0.82, 0.48 },
		{ 0.36, 0.56, 0.98 },
	};
	GdkRGBA bg, text, dim;

	lookup(w, "likeos_shell_bg", "#161c2a", &bg);
	lookup(w, "likeos_shell_text", "#eef2fa", &text);
	lookup(w, "likeos_shell_text_secondary", "#a4adbf", &dim);

	/* The box. */
	iv_rounded_rect(cr, 0.5, 0.5, W - 1, H - 1, 10);
	gdk_cairo_set_source_rgba(cr, &bg);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, text.red, text.green, text.blue, 0.08);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	/* Quarter lines. */
	cairo_set_source_rgba(cr, text.red, text.green, text.blue, 0.06);
	for (int i = 1; i < 4; i++) {
		double gx = floor(x + iw * i / 4) + 0.5;

		cairo_move_to(cr, gx, y);
		cairo_line_to(cr, gx, y + ih);
	}
	cairo_stroke(cr);

	if (!A.have_hist) {
		PangoLayout *l = gtk_widget_create_pango_layout(w, "No preview yet");
		int tw, th;

		pango_layout_get_pixel_size(l, &tw, &th);
		gdk_cairo_set_source_rgba(cr, &dim);
		cairo_move_to(cr, (W - tw) / 2, (H - th) / 2);
		pango_cairo_show_layout(cr, l);
		g_object_unref(l);
		return TRUE;
	}
	peak = histogram_peak();

	/* The colours, added on each other: where all three meet the
	 * histogram is white, where two meet it is their mix. */
	cairo_save(cr);
	cairo_rectangle(cr, x, y, iw, ih);
	cairo_clip(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	for (int c = 0; c < 3; c++) {
		hist_path(cr, A.bins[c], x, y, iw, ih, peak, TRUE);
		cairo_set_source_rgba(cr, rgb[c][0], rgb[c][1], rgb[c][2], 0.55);
		cairo_fill(cr);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	/* The brightness as an outline on top. */
	hist_path(cr, A.bins[3], x, y, iw, ih, peak, FALSE);
	cairo_set_source_rgba(cr, text.red, text.green, text.blue, 0.85);
	cairo_set_line_width(cr, 1.25);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
	cairo_restore(cr);

	/* The clip marks under both ends: a small wedge each, lit when that
	 * end is clipped. */
	for (int end = 0; end < 2; end++) {
		gboolean lit = (end ? A.clip_high : A.clip_low) > CLIP_FRACTION;
		double mx = end ? x + iw - 4 : x + 4;
		double my = H - 5;

		cairo_move_to(cr, mx - 4, my + 2);
		cairo_line_to(cr, mx + 4, my + 2);
		cairo_line_to(cr, mx, my - 4);
		cairo_close_path(cr);
		if (lit)
			cairo_set_source_rgba(cr, end ? 1.0 : 0.36, end ? 0.83 : 0.56,
					      end ? 0.35 : 0.98, 1);
		else
			cairo_set_source_rgba(cr, dim.red, dim.green, dim.blue, 0.35);
		cairo_fill(cr);
	}
	return TRUE;
}

static void update_hist_tip(void)
{
	char *tip;

	if (!A.hist)
		return;
	if (!A.have_hist) {
		gtk_widget_set_tooltip_text(A.hist, "The tones of the picture");
		return;
	}
	tip = g_strdup_printf("The tones of the picture, dark to light.\n"
			      "Pure black: %.1f %%   Pure white: %.1f %%",
			      A.clip_low * 100, A.clip_high * 100);
	gtk_widget_set_tooltip_text(A.hist, tip);
	g_free(tip);
}

static void refresh_histogram(IvEditor *ed)
{
	cairo_surface_t *s;

	if (!A.hist)
		return;
	s = iv_editor_shown_surface(ed, NULL, NULL);
	histogram_from(s);
	update_hist_tip();
	gtk_widget_queue_draw(A.hist);
}

static void sync_resets(IvEditor *ed);

/* Each new preview render, while Adjust is the tool shown.  A slider
 * being dragged is a live edit the panel hears nothing else of, so the
 * reset buttons follow here too. */
static void rendered(IvEditor *ed, gpointer data)
{
	if (iv_editor_current_tool(ed) != &iv_tool_adjust)
		return;
	refresh_histogram(ed);
	sync_resets(ed);
}

/* ---- the panel ------------------------------------------------------------------ */

static GtkWidget *section_header(GtkWidget *p, Section sec, IvEditor *ed)
{
	GtkWidget *reset = NULL;
	GtkWidget *row = iv_panel_section(p, sections[sec].title, &reset);
	GtkWidget *glyph = iv_glyph_new(sections[sec].glyph, 14);
	char *tip = g_strdup_printf("Reset %s",
				    sec == SEC_LIGHT ? "the light" :
				    sec == SEC_COLOR ? "the colour" : "the detail");

	iv_add_class(glyph, "iv-section-glyph");
	gtk_box_pack_start(GTK_BOX(row), glyph, FALSE, FALSE, 0);
	gtk_box_reorder_child(GTK_BOX(row), glyph, 0);
	gtk_widget_set_tooltip_text(reset, tip);
	g_free(tip);
	g_object_set_data(G_OBJECT(reset), "iv-section", GINT_TO_POINTER(sec));
	g_signal_connect(reset, "clicked", G_CALLBACK(reset_section), ed);
	A.reset[sec] = reset;
	return row;
}

static void add_slider(GtkWidget *p, IvEditor *ed, int i)
{
	const Field *f = &fields[i];
	double min = f->kind == KIND_PLAIN ? -100 : 0;
	GtkWidget *row = iv_panel_slider(ed, f->label, min, 100, 1, 0, slider_get,
					 slider_set, GINT_TO_POINTER(i));
	GtkWidget *scale = g_object_get_data(G_OBJECT(row), "iv-scale");

	gtk_range_set_increments(GTK_RANGE(scale), 1, 10);
	if (f->track) {
		iv_add_class(scale, "iv-tracked");
		iv_add_class(scale, f->track);
	}
	gtk_widget_set_tooltip_text(scale, f->tip);
	atk_object_set_name(gtk_widget_get_accessible(scale), f->label);
	A.rows[i] = row;
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);
}

static GtkWidget *build_panel(IvEditor *ed)
{
	GtkWidget *p = iv_panel_new("Adjust", "Light, colour and detail of the "
					      "picture.");
	GtkWidget *head, *l, *sw_row;
	Section sec = SEC_COUNT;

	A.ed = ed;
	A.panel = p;
	iv_add_class(p, "iv-adjust-panel");

	/* The histogram.  (Before and after: the top bar's Compare, the
	 * same for every tool.) */
	head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	iv_add_class(head, "iv-hist-head");
	l = gtk_label_new("HISTOGRAM");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	iv_add_class(l, "iv-hist-title");
	gtk_box_pack_start(GTK_BOX(head), l, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(p), head, FALSE, FALSE, 0);
	A.hist = gtk_drawing_area_new();
	iv_add_class(A.hist, "iv-histogram");
	gtk_widget_set_size_request(A.hist, -1, 92);
	g_signal_connect(A.hist, "draw", G_CALLBACK(hist_draw), NULL);
	gtk_box_pack_start(GTK_BOX(p), A.hist, FALSE, FALSE, 0);
	iv_editor_add_rendered_hook(ed, rendered, NULL);

	/* The sliders, a header before each section. */
	for (int i = 0; i < N_FIELDS; i++) {
		if (fields[i].section != sec) {
			sec = fields[i].section;
			section_header(p, sec, ed);
		}
		add_slider(p, ed, i);
		if (fields[i].kind == KIND_GRAIN) {
			sw_row = iv_panel_switch("Fine grain", FALSE, &A.fine_switch);
			iv_add_class(sw_row, "iv-adjust-switch");
			gtk_widget_set_tooltip_text(sw_row, "Small, dense grain "
							    "instead of coarse "
							    "film grain");
			g_signal_connect(A.fine_switch, "notify::active",
					 G_CALLBACK(fine_toggled), ed);
			gtk_box_pack_start(GTK_BOX(p), sw_row, FALSE, FALSE, 0);
		}
	}

	A.reset_all = iv_panel_button(IV_GLYPH_RESET, "Reset all adjustments");
	iv_add_class(A.reset_all, "iv-adjust-reset-all");
	g_signal_connect(A.reset_all, "clicked", G_CALLBACK(reset_all), ed);
	gtk_box_pack_start(GTK_BOX(p), A.reset_all, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p),
			   iv_panel_note("Double-click a slider to put it back. "
					 "Hold Compare or \\ to see the picture "
					 "without the edits."),
			   FALSE, FALSE, 0);
	return p;
}

/* ---- the tool ------------------------------------------------------------------- */

static void sync_resets(IvEditor *ed)
{
	const IvEditState *st = iv_editor_state(ed);

	if (!A.panel || !st)
		return;
	for (int s = 0; s < SEC_COUNT; s++)
		gtk_widget_set_sensitive(A.reset[s],
					 !section_neutral(&st->adjust, (Section)s));
	gtk_widget_set_sensitive(A.reset_all, !iv_adjust_is_neutral(&st->adjust));
}

static void state_changed(IvEditor *ed)
{
	const IvEditState *st = iv_editor_state(ed);

	if (!A.panel || !st)
		return;
	/* The switch shows the grain that is set; with none, it keeps the
	 * choice made last. */
	if (fabs(st->adjust.grain) > 1e-9)
		A.fine_grain = st->adjust.grain < 0;
	A.syncing = TRUE;
	gtk_switch_set_active(GTK_SWITCH(A.fine_switch), A.fine_grain);
	A.syncing = FALSE;
	for (int i = 0; i < N_FIELDS; i++)
		if (A.rows[i])
			iv_panel_slider_sync(A.rows[i]);
	sync_resets(ed);
}

static void activate(IvEditor *ed)
{
	/* What the last render showed may be another tool's view (the crop
	 * tool's whole frame); the editor renders again for the output and
	 * the hook catches it, but until then the last one is better than
	 * nothing. */
	refresh_histogram(ed);
}

static void deactivate(IvEditor *ed)
{
	iv_editor_set_compare(ed, FALSE);
}

const IvTool iv_tool_adjust = {
	.id = "adjust",
	.label = "Adjust",
	.glyph = IV_GLYPH_ADJUST,
	.tooltip = "Light and colour (A)",
	.key = GDK_KEY_a,
	.build_panel = build_panel,
	.activate = activate,
	.deactivate = deactivate,
	.state_changed = state_changed,
};
