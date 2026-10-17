# LikeOS -- Media Player, the system's own GTK3 music and video player.
#
# An in-tree port: the sources live here rather than in a tarball, and the
# GTK3 port driver builds it with meson like any other package, against the
# port sysroot (GTK 3, GStreamer 1.24 and its base libraries).
#
# Copyright (C) 2026 The LikeOS Project

project('likeos-mediaplayer', 'c',
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
gst_ver = '>= 1.20'
deps = [
  gtk_dep,
  dependency('gio-2.0', version : '>= 2.66'),
  dependency('gstreamer-1.0', version : gst_ver),
  dependency('gstreamer-video-1.0', version : gst_ver),
  dependency('gstreamer-audio-1.0', version : gst_ver),
  dependency('gstreamer-pbutils-1.0', version : gst_ver),
  dependency('gstreamer-tag-1.0', version : gst_ver),
]

# GDK_WINDOW_XID for the video overlay.  Without the X11 backend the
# overlay sinks are simply never given a window and the GTK sink is used.
gtk_x11_dep = dependency('gtk+-x11-3.0', required : false)
if gtk_x11_dep.found()
  deps += gtk_x11_dep
endif

# lround/trunc.  On this system they live in the C library itself, so a
# separate libm is optional.
m_dep = cc.find_library('m', required : false)
if m_dep.found()
  deps += m_dep
endif

add_project_arguments(
  '-DG_LOG_DOMAIN="mediaplayer"',
  '-DMP_VERSION="@0@"'.format(meson.project_version()),
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66',
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_22',
  # GTK's and GStreamer's own headers trip this one; ours do not.
  '-Wno-unused-parameter',
  language : 'c')

sources = files(
  'src/cover.c',
  'src/dialogs.c',
  'src/discover.c',
  'src/main.c',
  'src/player.c',
  'src/playlist.c',
  'src/playlist_io.c',
  'src/scan.c',
  'src/settings.c',
  'src/style.c',
  'src/util.c',
  'src/window.c',
)

executable('mediaplayer', sources,
  dependencies : deps,
  install : true)

install_data('data/mediaplayer.desktop',
  install_dir : get_option('datadir') / 'applications')
