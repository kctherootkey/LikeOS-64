#!/usr/bin/env python3
# LikeOS Task Viewer -- rasterize the application icon.
#
# LikeOS has no SVG loader at run time, so the icon is installed as PNG
# files, written by this script on the build host from the two drawings
# next to it and committed: likeos-taskviewer-16.svg for 16 to 24 pixels,
# likeos-taskviewer.svg for 32 and up.  Run it again after changing a
# drawing.  Needs Python 3 with PyGObject (GdkPixbuf 2.0 with its SVG
# loader); nothing here runs on LikeOS.
#
# Usage: render-icons.py   (writes data/icons/<size>/likeos-taskviewer.png)
#
# Copyright (C) 2026 The LikeOS Project

import os

import gi
gi.require_version('GdkPixbuf', '2.0')
from gi.repository import GdkPixbuf  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [16, 22, 24, 32, 48, 64, 96, 128, 256]

for size in SIZES:
    src = 'likeos-taskviewer-16.svg' if size <= 24 else 'likeos-taskviewer.svg'
    pb = GdkPixbuf.Pixbuf.new_from_file_at_scale(os.path.join(HERE, src),
                                                  size, size, True)
    out = os.path.join(HERE, 'icons', '%dx%d' % (size, size))
    os.makedirs(out, exist_ok=True)
    pb.savev(os.path.join(out, 'likeos-taskviewer.png'), 'png', [], [])
    print('%3d  %s' % (size, src))
