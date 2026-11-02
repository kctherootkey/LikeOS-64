#!/usr/bin/env python3
# Compile the LikeOS GTK 3 theme: src/*.scss -> gtk-3.0/gtk.css, and draw the
# theme's glyph images -> gtk-3.0/assets/*.png.
#
# Needs libsass for Python (pip install libsass; sassc on the same input gives
# the same output) and pycairo.  The compiled CSS and the images are kept in
# the repository, so this only has to be run after changing the sources.
#
# The glyphs are drawn here rather than taken from GTK: GTK's own check mark
# is a 16-pixel picture of a thick stroke that reads blurred and blocky next
# to Windows 11's thin, sharp one, and stretching it to another size only
# makes that worse.  These are drawn for the exact box the theme gives them
# (16 pixels, and 32 for a doubled screen), their straight strokes on the
# pixel grid, so they are shown at their own size and never scaled.
#
# Copyright (C) 2026 The LikeOS Project

import os
import sys

import cairo
import sass

here = os.path.dirname(os.path.abspath(__file__))


# The glyph box: the content box of a check (src/_likeos.scss, check) and of
# a menu item's check.  Coordinates below are in this 16-pixel space.
BOX = 16


def draw(name, scale, rgba, paint):
    """One glyph image, BOX*scale pixels square, transparent around it."""
    size = BOX * scale
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
    cr = cairo.Context(surf)
    cr.scale(scale, scale)
    cr.set_source_rgba(*rgba)
    paint(cr, scale)
    suffix = "" if scale == 1 else "@%d" % scale
    path = os.path.join(here, "gtk-3.0", "assets", "%s%s.png" % (name, suffix))
    surf.write_to_png(path)


def check(cr, scale):
    # Windows 11's check: a short stroke down into the corner and a long one
    # up, thin, with rounded ends.  1.5 units: one and a half pixels on a
    # normal screen, three on a doubled one.
    cr.set_line_width(1.5)
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.set_line_join(cairo.LINE_JOIN_ROUND)
    cr.move_to(3.4, 8.4)
    cr.line_to(6.4, 11.4)
    cr.line_to(12.6, 5.2)
    cr.stroke()


def dash(cr, scale):
    # The "mixed" state: a level bar with both edges on pixel rows -- two
    # pixels high on a normal screen (rows 7-8), three on a doubled one
    # (device rows 14-16).
    cr.rectangle(4, 7, 8, 2 if scale == 1 else 1.5)
    cr.fill()


WHITE = (1, 1, 1, 1)
DARK = (0x1b / 255.0, 0x1b / 255.0, 0x1b / 255.0, 1)

os.makedirs(os.path.join(here, "gtk-3.0", "assets"), exist_ok=True)
for scale in (1, 2):
    # On the accent-filled box of a check button (and its grey disabled
    # fill): white.
    draw("check", scale, WHITE, check)
    draw("dash", scale, WHITE, dash)
    # In a menu, where there is no box: the text colour.
    draw("check-menu", scale, DARK, check)
css = sass.compile(filename=os.path.join(here, "src", "gtk-contained.scss"),
                   output_style="expanded")
header = ("/* LikeOS GTK 3 theme (Windows 11 look).  GENERATED from src/ by "
          "build.py -- edit the .scss sources, not this file. */\n")
out = os.path.join(here, "gtk-3.0", "gtk.css")
with open(out, "w") as f:
    f.write(header + css)
print("wrote", out, len(css.splitlines()), "lines", file=sys.stderr)
