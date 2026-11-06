// LikeOS -- LikeOS Image Viewer: the editor's tool interface.
//
// Each tool of the editor's rail (Crop, Adjust, Styles, Draw, Retouch,
// Background) is one IvTool in a file of its own (tool-*.c), registered in
// the table in editor.c.  The editor owns everything around the tools --
// the window chrome, the canvas and its zoom, undo and redo, saving -- and
// a tool only says what its options panel looks like and what it does with
// the pointer on the picture.  A tool reads and changes the picture only
// through the edit state (editstate.h) and the editor's functions
// (editor.h); it never touches pixels of its own except for previews it
// draws in draw_overlay.
//
// There is one editor per process, so a tool keeps its own state (the
// widgets of its panel, a stroke being drawn) in static variables of its
// file.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IV_TOOLS_H
#define IV_TOOLS_H

#include <gtk/gtk.h>

#include "glyphs.h"

typedef struct IvEditor IvEditor;

typedef struct IvTool {
	const char *id;		/* "crop": stable, for settings and tests */
	const char *label;	/* "Crop": under the rail button */
	IvGlyph glyph;		/* the rail button's glyph */
	const char *tooltip;	/* "Crop and rotate (C)"; NULL: the label */
	guint key;		/* single-key shortcut (GDK_KEY_c), 0: none */

	/* The options panel, built once on first use; the editor puts it in
	 * the scrolled panel at the right (340 px wide, dark card).  Must
	 * not be NULL. */
	GtkWidget *(*build_panel)(IvEditor *ed);

	/* The tool became the current one / stopped being it.  activate is
	 * where a tool sets the render flags it needs
	 * (iv_editor_set_render_flags), its cursor and whether dragging
	 * pans (iv_editor_set_drag_pan).  The editor resets all three to
	 * their defaults before activate.  deactivate must leave no edit
	 * open (commit or cancel it).  Both may be NULL. */
	void (*activate)(IvEditor *ed);
	void (*deactivate)(IvEditor *ed);

	/* Button, motion, scroll and leave events over the canvas.
	 * (img_x, img_y) is the pointer in DISPLAY coordinates (editor.h).
	 * Return TRUE when handled: the canvas then does not pan or zoom
	 * with the event.  May be NULL. */
	gboolean (*canvas_event)(IvEditor *ed, GdkEvent *ev, double img_x,
				 double img_y);

	/* Draws over the picture.  The cairo context is in WIDGET
	 * coordinates of the canvas, untransformed; iv_editor_display_to_
	 * widget() and iv_editor_view_scale() place things on the picture.
	 * May be NULL. */
	void (*draw_overlay)(IvEditor *ed, cairo_t *cr);

	/* The edit state changed from outside the tool -- undo, redo,
	 * revert, another tool -- so the panel's controls must show it
	 * again.  Called for the current tool and for every tool whose
	 * panel exists.  May be NULL. */
	void (*state_changed)(IvEditor *ed);

	/* Keys while the tool is current, before the editor's own keys.
	 * TRUE when handled.  May be NULL. */
	gboolean (*key_press)(IvEditor *ed, GdkEventKey *ev);
} IvTool;

/* The tools, in rail order (tool-*.c). */
extern const IvTool iv_tool_crop;
extern const IvTool iv_tool_adjust;
extern const IvTool iv_tool_styles;
extern const IvTool iv_tool_draw;
extern const IvTool iv_tool_retouch;
extern const IvTool iv_tool_background;

#endif
