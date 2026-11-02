# LikeOS -- File Explorer, the system's own file manager.
#
# An in-tree port like the media player: the sources live here and the GTK3
# port driver builds them with meson against the port sysroot.
#
# Three layers, each a static library the next one links:
#
#   src/backend   file management on GIO alone -- locations, enumeration,
#                 monitoring, operations, trash, thumbnails, volumes,
#                 applications, search and its index.  No GTK, so it can be
#                 reused by any program and tested without a display.
#   src/services  what the application keeps per window or per session --
#                 navigation history, selection, clipboard, undo, the
#                 operation queue, settings.
#   src/ui        the GTK3 interface.  It never calls a GIO file function
#                 itself; everything goes through the two layers below.
#
# Copyright (C) 2026 The LikeOS Project

project('likeos-explorer', 'c',
  version : '1.0',
  license : 'GPL-2.0-only',
  meson_version : '>= 0.60',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=release',
  ])

cc = meson.get_compiler('c')

glib_dep = dependency('glib-2.0', version : '>= 2.72')
gio_dep = dependency('gio-2.0', version : '>= 2.72')
gio_unix_dep = dependency('gio-unix-2.0', version : '>= 2.72')
pixbuf_dep = dependency('gdk-pixbuf-2.0')
gtk_dep = dependency('gtk+-3.0', version : '>= 3.22')

backend_deps = [glib_dep, gio_dep, gio_unix_dep, pixbuf_dep]
media_args = []
gst_feature = get_option('gstreamer')
gst_dep = dependency('gstreamer-1.0', required : gst_feature)
gst_app_dep = dependency('gstreamer-app-1.0', required : gst_feature)
gst_video_dep = dependency('gstreamer-video-1.0', required : gst_feature)
gst_pbutils_dep = dependency('gstreamer-pbutils-1.0', required : gst_feature)
if gst_dep.found() and gst_app_dep.found() and gst_video_dep.found() and gst_pbutils_dep.found()
  backend_deps += [gst_dep, gst_app_dep, gst_video_dep, gst_pbutils_dep]
  media_args += '-DEX_HAVE_GSTREAMER=1'
endif

m_dep = cc.find_library('m', required : false)
if m_dep.found()
  backend_deps += m_dep
endif

add_project_arguments(
  '-DG_LOG_DOMAIN="explorer"',
  '-DEX_VERSION="@0@"'.format(meson.project_version()),
  '-DEX_DATADIR="@0@"'.format(get_option('prefix') / get_option('datadir') / 'likeos-explorer'),
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_72',
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_22',
  # GTK's and GLib's own headers trip this one; ours do not.
  '-Wno-unused-parameter',
  # GActionEntry and GOptionEntry tables name only the fields they use,
  # as GLib's own documentation writes them.
  '-Wno-missing-field-initializers',
  media_args,
  language : 'c')

inc = include_directories('src')

backend = static_library('explorer-backend',
  files(
    'src/backend/ex-apps.c',
    'src/backend/ex-bookmarks.c',
    'src/backend/ex-dir-loader.c',
    'src/backend/ex-file.c',
    'src/backend/ex-fileops.c',
    'src/backend/ex-index.c',
    'src/backend/ex-location.c',
    'src/backend/ex-media.c',
    'src/backend/ex-monitor.c',
    'src/backend/ex-recent.c',
    'src/backend/ex-search.c',
    'src/backend/ex-thumbs.c',
    'src/backend/ex-trash.c',
    'src/backend/ex-util.c',
    'src/backend/ex-volumes.c',
  ),
  include_directories : inc,
  dependencies : backend_deps)
backend_dep = declare_dependency(link_with : backend,
  include_directories : inc,
  dependencies : backend_deps)

services = static_library('explorer-services',
  files(
    'src/services/ex-clipboard.c',
    'src/services/ex-navigator.c',
    'src/services/ex-opqueue.c',
    'src/services/ex-selection.c',
    'src/services/ex-settings.c',
    'src/services/ex-undo.c',
  ),
  include_directories : inc,
  dependencies : [backend_dep, gtk_dep])
services_dep = declare_dependency(link_with : services,
  include_directories : inc,
  dependencies : [backend_dep, gtk_dep])

ui_sources = files(
  'src/main.c',
  'src/ui/ex-actions.c',
  'src/ui/ex-commandbar.c',
  'src/ui/ex-conflict-dialog.c',
  'src/ui/ex-context-menu.c',
  'src/ui/ex-file-view.c',
  'src/ui/ex-folder-model.c',
  'src/ui/ex-home-page.c',
  'src/ui/ex-icons.c',
  'src/ui/ex-operation-center.c',
  'src/ui/ex-preview-pane.c',
  'src/ui/ex-properties.c',
  'src/ui/ex-share.c',
  'src/ui/ex-sidebar.c',
  'src/ui/ex-style.c',
  'src/ui/ex-tab.c',
  'src/ui/ex-window.c',
  'src/ui/widgets/lx-breadcrumb.c',
  'src/ui/widgets/lx-flyout.c',
  'src/ui/widgets/lx-segmented.c',
  'src/ui/widgets/lx-split-button.c',
)

executable('likeos-explorer', ui_sources,
  dependencies : [services_dep],
  install : true)

install_data('data/likeos-explorer.desktop',
  install_dir : get_option('datadir') / 'applications')
install_data('data/org.likeos.Explorer.gschema.xml',
  install_dir : get_option('datadir') / 'glib-2.0' / 'schemas')

if get_option('tests')
  executable('explorer-backend-test', files('tests/test-backend.c'),
    dependencies : [backend_dep],
    install : true)
endif
