# LikeOS -- LikeOS Image Viewer, the system's own GTK3 picture viewer and
# editor.
#
# An in-tree port like the media player and Explorer: the sources live here
# and the GTK3 port driver builds them with meson against the port sysroot.
#
# Two parts:
#
#   src/ivproc  the image processing library -- scaling, the adjustments and
#               styles, geometry, inpainting, background estimation, the
#               EXIF reader, the WebP codec.  Plain C, no GTK, so it is
#               tested on its own (tests/, meson test).
#   src         the program: the viewer, the editor shell and its tools.
#
# Copyright (C) 2026 The LikeOS Project

project('likeos-imageviewer', 'c',
  version : '1.0',
  license : 'GPL-2.0-only',
  meson_version : '>= 0.60',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=release',
  ])

cc = meson.get_compiler('c')

gtk_dep = dependency('gtk+-3.0', version : '>= 3.22')
deps = [
  gtk_dep,
  dependency('gio-2.0', version : '>= 2.66'),
  dependency('gio-unix-2.0', version : '>= 2.66'),
  dependency('gdk-pixbuf-2.0', version : '>= 2.40'),
]

# The X11 backend gives the window manager hints of the title row; without
# it the program still runs, with the window manager's own title bar.
gtk_x11_dep = dependency('gtk+-x11-3.0', required : false)
if gtk_x11_dep.found()
  deps += gtk_x11_dep
endif

m_dep = cc.find_library('m', required : false)
if m_dep.found()
  deps += m_dep
endif

add_project_arguments(
  '-DG_LOG_DOMAIN="imageviewer"',
  '-DIV_VERSION="@0@"'.format(meson.project_version()),
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66',
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_22',
  # GTK's own headers trip this one; ours do not.
  '-Wno-unused-parameter',
  language : 'c')

# The processing library (ivproc_dep) and its tests.
subdir('src/ivproc')
if get_option('tests')
  subdir('tests')
endif

sources = files(
  'src/actions.c',
  'src/canvas.c',
  'src/dialogs.c',
  'src/editor.c',
  'src/editstate.c',
  'src/filmstrip.c',
  'src/folder.c',
  'src/glyphs.c',
  'src/infopanel.c',
  'src/loader.c',
  'src/main.c',
  'src/menus.c',
  'src/render.c',
  'src/saver.c',
  'src/settings.c',
  'src/strip.c',
  'src/style.c',
  'src/thumbs.c',
  'src/toast.c',
  'src/tool-adjust.c',
  'src/tool-background.c',
  'src/tool-brush.c',
  'src/tool-crop.c',
  'src/tool-crop-dial.c',
  'src/tool-draw.c',
  'src/tool-retouch.c',
  'src/tool-styles.c',
  'src/trash.c',
  'src/util.c',
  'src/viewer.c',
  'src/window.c',
)

executable('imageviewer', sources,
  dependencies : deps + [ivproc_dep],
  install : true)

install_data('data/likeos-imageviewer.desktop',
  install_dir : get_option('datadir') / 'applications')
