#!/usr/bin/env python3
# Compile the LikeOS GTK 3 theme: src/*.scss -> gtk-3.0/gtk.css.
#
# Needs libsass for Python (pip install libsass); sassc on the same input
# gives the same output.  The compiled CSS is kept in the repository, so this
# only has to be run after changing the sources.
#
# Copyright (C) 2026 The LikeOS Project

import os
import sys

import sass

here = os.path.dirname(os.path.abspath(__file__))
css = sass.compile(filename=os.path.join(here, "src", "gtk-contained.scss"),
                   output_style="expanded")
header = ("/* LikeOS GTK 3 theme (Windows 11 look).  GENERATED from src/ by "
          "build.py -- edit the .scss sources, not this file. */\n")
out = os.path.join(here, "gtk-3.0", "gtk.css")
with open(out, "w") as f:
    f.write(header + css)
print("wrote", out, len(css.splitlines()), "lines", file=sys.stderr)
