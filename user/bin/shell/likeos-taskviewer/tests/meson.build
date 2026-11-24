# LikeOS -- LikeOS Task Viewer: unit tests of the pure parts.
#
# Built with -Dtests=true; on the build host (meson setup -Dtests=true,
# meson test) they run there, in a LikeOS build they are only compiled.
#
# Copyright (C) 2026 The LikeOS Project

test_deps = [glib_dep]
if m_dep.found()
  test_deps += m_dep
endif

test_model = executable('test-model',
  files('test-model.c', '../src/tv-model.c'),
  dependencies : test_deps,
  install : false)
test('model', test_model)

test_sysinfo = executable('test-sysinfo',
  files('test-sysinfo.c', '../src/tv-sysinfo.c'),
  dependencies : test_deps,
  install : false)
test('sysinfo', test_sysinfo)

test_launch = executable('test-launch',
  files('test-launch.c', '../src/tv-launch.c'),
  dependencies : test_deps,
  install : false)
test('launch', test_launch)
