# LikeOS -- LikeOS Image Viewer: unit tests of the processing library.
#
# Built only with -Dtests=true (a host build; the target package never runs
# them).  libpng, when found, lets the tests read PNG files for the
# real-photo checks; without it those checks are skipped.  Set
# IVPROC_OUTDIR to a directory to get before/after PNGs of the retouch and
# background tests, IVPROC_AVATAR to a PNG for the real-photo background
# test.
#
# Copyright (C) 2026 The LikeOS Project

ivtest_deps = [ivproc_dep]
ivtest_args = []
ivtest_png = dependency('libpng', required : false)
if ivtest_png.found()
  ivtest_deps += ivtest_png
  ivtest_args += '-DIV_TEST_HAVE_PNG=1'
endif

ivtest_exe = executable('test-ivproc',
  files('test-ivproc.c', 'testpng.c'),
  c_args : ivtest_args,
  dependencies : ivtest_deps,
  install : false)

foreach suite : ['image', 'scale', 'adjust', 'styles', 'enhance', 'geometry',
                 'blur', 'mask', 'histogram', 'inpaint', 'background',
                 'exif', 'jpegmeta', 'fuzz', 'webp']
  test('ivproc-' + suite, ivtest_exe, args : [suite], timeout : 900)
endforeach
