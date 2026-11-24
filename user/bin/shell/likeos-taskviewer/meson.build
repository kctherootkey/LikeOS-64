# LikeOS -- LikeOS Task Viewer, the system's own process viewer: the running
# apps and processes, the performance of the processors, memory, disks and
# network, the users, the services and the startup programs.
#
# Part of the desktop shell (user/bin/shell), like Explorer and the image
# viewer: the top-level Makefile builds it with meson against the GTK3 port
# sysroot and ports/xorg/gtk3/stage.sh puts it on the image.
#
# The program itself only builds for LikeOS (it reads the kernel's process
# listing).  Built for the build host -- meson setup -Dtests=true, then
# meson test -- only the tests of the pure parts are built:
#
#   tests/test-model.c    rates, sizes, names, matching, the grouping of
#                         processes into apps (src/tv-model.c)
#   tests/test-sysinfo.c  the system sampler's parsers (src/tv-sysinfo.c)
#   tests/test-launch.c   whether Start new task starts a program in the
#                         terminal (src/tv-launch.c)
#
# Copyright (C) 2026 The LikeOS Project

project('likeos-taskviewer', 'c',
  version : '1.0',
  license : 'GPL-2.0-only',
  meson_version : '>= 0.60',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=release',
  ])

cc = meson.get_compiler('c')

glib_dep = dependency('glib-2.0', version : '>= 2.66')
m_dep = cc.find_library('m', required : false)

add_project_arguments(
  '-DG_LOG_DOMAIN="taskviewer"',
  '-DTV_VERSION="@0@"'.format(meson.project_version()),
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66',
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_22',
  # GTK's own headers trip this one; ours do not.
  '-Wno-unused-parameter',
  language : 'c')

if host_machine.system() == 'likeos'
  deps = [
    dependency('gtk+-3.0', version : '>= 3.22'),
    dependency('gtk+-x11-3.0'),
    dependency('x11'),
    dependency('gio-2.0', version : '>= 2.66'),
    dependency('gio-unix-2.0', version : '>= 2.66'),
    glib_dep,
  ]
  if m_dep.found()
    deps += m_dep
  endif

  sources = files(
    # The window, the program and the process pages (Processes, Users,
    # Details) with their backend.
    'src/tv-window.c',
    'src/tv-run.c',
    'src/tv-launch.c',
    'src/tv-glyphs.c',
    'src/tv-model.c',
    'src/tv-procs.c',
    'src/tv-procui.c',
    'src/tv-tree.c',
    'src/tv-processes.c',
    'src/tv-users.c',
    'src/tv-details.c',
    # The Performance, Services and Startup apps pages.
    'src/tv-perf.c',
    'src/tv-graph.c',
    'src/tv-sysinfo.c',
    'src/tv-services.c',
    'src/tv-startup.c',
  )

  executable('likeos-taskviewer', sources,
    dependencies : deps,
    install : true)

  install_data('data/likeos-taskviewer.desktop',
    install_dir : get_option('datadir') / 'applications')
  foreach size : ['16x16', '22x22', '24x24', '32x32', '48x48', '64x64',
                  '96x96', '128x128', '256x256']
    install_data('data/icons' / size / 'likeos-taskviewer.png',
      install_dir : get_option('datadir') / 'icons' / 'hicolor' / size / 'apps')
  endforeach
endif

if get_option('tests')
  subdir('tests')
endif
