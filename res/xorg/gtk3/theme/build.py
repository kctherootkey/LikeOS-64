#!/usr/bin/env python3
# Compile the LikeOS GTK 3 theme: src/*.scss -> gtk-3.0/gtk.css, and draw the
# theme's glyph images -> gtk-3.0/assets/*.png.
#
# Needs libsass for Python (pip install libsass; sassc on the same input gives
# the same output) and pycairo.  The compiled CSS and the images are kept in
# the repository, so this only has to be run after changing the sources.
#
# The glyphs are drawn here rather than taken from GTK: GTK's own check mark
# is a 16-pixel picture of a thick stroke that reads blurred and blocky at
# the theme's sizes, and stretching it to another size only makes that
# worse.  These are plain geometric strokes drawn for the exact box the
# theme gives them (16 pixels, and 32 for a doubled screen), so they are
# shown at their own size and never scaled:
#
#   check    the mark of a check box (white, on the accent fill) and of a
#            checked menu item
#   dash     the "mixed" state of a check box
#   bullet   the chosen item of a group in a menu
#   chevron  combo boxes, submenus, expanders, tree views, tab scrolling
#
# The menu marks and chevrons are symbolic images, which GTK recolours to the
# text colour of wherever they are shown.
#
# Copyright (C) 2026 The LikeOS Project

import os
import sys

import cairo
import sass

here = os.path.dirname(os.path.abspath(__file__))
assets = os.path.join(here, "gtk-3.0", "assets")

# The glyph box: the content box of a check (src/_likeos.scss, check) and of
# a menu item's check.  Coordinates below are in this 16-pixel space.
BOX = 16


def draw(name, scale, rgba, paint, symbolic=False):
    """One glyph image, BOX*scale pixels square, transparent around it."""
    size = BOX * scale
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
    cr = cairo.Context(surf)
    cr.scale(scale, scale)
    cr.set_source_rgba(*rgba)
    paint(cr, scale)
    suffix = "" if scale == 1 else "@%d" % scale
    if symbolic:
        suffix += ".symbolic"
    surf.write_to_png(os.path.join(assets, "%s%s.png" % (name, suffix)))


def stroke(points, width):
    """A polyline with round ends and joins."""
    def paint(cr, scale):
        cr.set_line_width(width)
        cr.set_line_cap(cairo.LINE_CAP_ROUND)
        cr.set_line_join(cairo.LINE_JOIN_ROUND)
        cr.move_to(*points[0])
        for p in points[1:]:
            cr.line_to(*p)
        cr.stroke()
    return paint


# The check: a short stroke down into the corner and a long one up to the
# right, the corner a little left of centre.  Two units wide on the
# accent-filled box, where it has to read white on blue at a glance; one and
# three quarters in a menu, beside text of the same weight.
CHECK = [(4.25, 8.5), (7.0, 11.25), (11.75, 5.0)]
check_box = stroke(CHECK, 2.0)
check_menu = stroke(CHECK, 1.75)

# The "mixed" state: a level bar with round ends, centred.
dash = stroke([(4.5, 8), (11.5, 8)], 2.0)


def bullet(cr, scale):
    # A small solid dot, centred.
    cr.arc(8, 8, 2.75, 0, 2 * 3.141592653589793)
    cr.fill()


# Chevrons: eight units across and four deep, one and a quarter units wide
# with round ends and a round point, centred in the 16-unit box.
CHEVRONS = {
    "chevron-down": [(4, 6), (8, 10), (12, 6)],
    "chevron-up": [(4, 10), (8, 6), (12, 10)],
    "chevron-right": [(6, 4), (10, 8), (6, 12)],
    "chevron-left": [(10, 4), (6, 8), (10, 12)],
}

WHITE = (1, 1, 1, 1)
# Symbolic glyphs are drawn in black: GTK replaces black in a file named
# *.symbolic.png with the colour of the text where it is shown
# (-gtk-recolor()), so one image serves every state -- secondary grey at
# rest, the text colour under the pointer, the disabled grey.
BLACK = (0, 0, 0, 1)

os.makedirs(assets, exist_ok=True)
# Images of an earlier version of the theme that nothing uses any more.
for old in ("pill.png", "pill@2.png", "check-menu.png", "check-menu@2.png",
            "caret-down.symbolic.png", "caret-up.symbolic.png",
            "caret-left.symbolic.png", "caret-right.symbolic.png"):
    if os.path.exists(os.path.join(assets, old)):
        os.remove(os.path.join(assets, old))

for scale in (1, 2):
    # On the accent-filled box of a check button (and its pale disabled
    # fill): white.
    draw("check", scale, WHITE, check_box)
    draw("dash", scale, WHITE, dash)

# The menu marks as symbolic images, for places whose colour follows the
# text (a disabled menu item, a check in a popover), and the chevrons.  One
# size only: the theme cannot pick a doubled symbolic image (see glyph() in
# src/_drawing.scss).
draw("check", 1, BLACK, check_menu, symbolic=True)
draw("dash", 1, BLACK, stroke([(4.5, 8), (11.5, 8)], 1.75), symbolic=True)
draw("bullet", 1, BLACK, bullet, symbolic=True)
for name, pts in CHEVRONS.items():
    draw(name, 1, BLACK, stroke(pts, 1.25), symbolic=True)


def split_rules(text):
    """The top-level blocks of expanded CSS: (prelude, body) pairs, comments
    and @define-color lines as (text, None)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.index("*/", i) + 2
            out.append((text[i:j], None))
            i = j
            continue
        if text[i].isspace():
            j = i
            while j < n and text[j].isspace():
                j += 1
            out.append((text[i:j], None))
            i = j
            continue
        # A statement ends at ';' (an @-statement) or with its block.
        brace = text.find("{", i)
        semi = text.find(";", i)
        if semi != -1 and (brace == -1 or semi < brace):
            out.append((text[i:semi + 1], None))
            i = semi + 1
            continue
        depth, j = 0, brace
        while True:
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((text[i:brace], text[brace:j + 1]))
        i = j + 1
    return out


def backdrop(css):
    """Drop the :backdrop selectors of Adwaita's part of the theme.

    GTK puts a window that is not focused in the backdrop state, and
    Adwaita restyles most widgets there -- greyed text and icons, flattened
    buttons, radio buttons that lose their dot, accent selections and
    switches drawn differently.  In LikeOS an inactive window changes nothing
    but its title row (and the tint of a list's selection), so in this theme
    the backdrop state looks like the normal one: Adwaita's :backdrop
    selectors are removed from its rules (between the LIKEOS-ADWAITA marks
    of src/_common.scss), and a rule left with no selector goes.  The LikeOS
    partials after it keep theirs -- the few places where an inactive window
    is marked are written there.
    """

    begin = css.index("/* LIKEOS-ADWAITA-BEGIN */")
    end = css.index("/* LIKEOS-ADWAITA-END */")
    out = []
    for prelude, body in split_rules(css[begin:end]):
        if body is None or prelude.lstrip().startswith("@"):
            out.append(prelude + (body or ""))
            continue
        sels = [x.strip() for x in prelude.split(",")]
        keep = [x for x in sels if ":backdrop" not in x]
        if not keep:
            continue
        if len(keep) == len(sels):
            out.append(prelude + body)
        else:
            out.append(",\n".join(keep) + " " + body)
    return css[:begin] + "".join(out) + css[end:]


css = sass.compile(filename=os.path.join(here, "src", "gtk-contained.scss"),
                   output_style="expanded")
css = backdrop(css)
header = ("/* LikeOS GTK 3 theme.  GENERATED from src/ by build.py -- edit the "
          ".scss sources, not this file. */\n")
out = os.path.join(here, "gtk-3.0", "gtk.css")
with open(out, "w") as f:
    f.write(header + css)
print("wrote", out, len(css.splitlines()), "lines", file=sys.stderr)
