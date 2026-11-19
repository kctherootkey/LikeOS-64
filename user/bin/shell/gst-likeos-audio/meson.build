# gst-likeos-audio: the GStreamer plugin "likeosaudio" with the element
# likeosaudiosink, audio output through the LikeOS audio service (audiod).
#
# Part of the desktop shell (user/bin/shell): the top-level Makefile builds it
# with meson, cross-compiled against the GTK3 port sysroot once that port has
# built, and installs it into the sysroot.  The plugin lands in
# /usr/lib/gstreamer-1.0, which ports/xorg/gtk3/stage.sh copies onto the image
# wholesale.
#
# libaudio is the system's own client library (user/lib/libaudio); its
# headers and libaudio.pc are published into the sysroot by
# ports/xorg/import-libaudio.sh, which is why it is an ordinary dependency()
# here.
#
# Copyright (C) 2026 The LikeOS Project

project('gst-likeos-audio', 'c',
  version : '1.0',
  license : 'GPL-2.0-only',
  meson_version : '>= 0.62',
  default_options : [
    'buildtype=debugoptimized',
    'warning_level=2',
    'c_std=gnu11',
    'b_ndebug=if-release',
  ])

gst_req = '>= 1.24.0'

gst_dep = dependency('gstreamer-1.0', version : gst_req)
gst_base_dep = dependency('gstreamer-base-1.0', version : gst_req)
gst_audio_dep = dependency('gstreamer-audio-1.0', version : gst_req)
glib_dep = dependency('glib-2.0', version : '>= 2.64')
libaudio_dep = dependency('libaudio')

plugin_c_args = [
  '-DPACKAGE="@0@"'.format(meson.project_name()),
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DGST_PACKAGE_NAME="LikeOS GStreamer audio output"',
  '-DGST_PACKAGE_ORIGIN="https://likeos.systemtrap.com/"',
  '-DG_LOG_DOMAIN="GStreamer-LikeOSAudio"',
  # GST_PLUGIN_DEFINE leaves the descriptor's padding to zero-filling,
  # which -Wextra reports; GStreamer's own plugins turn it off too.
  '-Wno-missing-field-initializers',
]

plugins_install_dir = get_option('libdir') / 'gstreamer-1.0'

gstlikeosaudio = shared_library('gstlikeosaudio',
  'plugin.c',
  'gstlikeosaudiosink.c',
  c_args : plugin_c_args,
  dependencies : [gst_dep, gst_base_dep, gst_audio_dep, glib_dep,
                  libaudio_dep],
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir : plugins_install_dir,
)
