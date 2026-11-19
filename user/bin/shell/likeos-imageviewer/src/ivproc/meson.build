# LikeOS -- LikeOS Image Viewer: the image processing library (ivproc).
#
# A static library with no GTK in it, so the unit tests in tests/ can link
# it on its own.  The top-level meson.build pulls it in with
# subdir('src/ivproc') and links the program against ivproc_dep.
#
# libwebp is required; libwebpdemux is used, when present, to decode the
# first frame of animated WebP files.
#
# Copyright (C) 2026 The LikeOS Project

ivproc_cc = meson.get_compiler('c')

ivproc_deps = [dependency('libwebp')]
ivproc_args = []

ivproc_webpdemux = dependency('libwebpdemux', required : false)
if ivproc_webpdemux.found()
  ivproc_deps += ivproc_webpdemux
  ivproc_args += '-DIV_HAVE_WEBPDEMUX=1'
endif

# On this system the maths functions live in the C library itself, so a
# separate libm is optional.
ivproc_m = ivproc_cc.find_library('m', required : false)
if ivproc_m.found()
  ivproc_deps += ivproc_m
endif

ivproc_sources = files(
  'adjust.c',
  'alloc.c',
  'background.c',
  'blur.c',
  'color.c',
  'enhance.c',
  'exif.c',
  'geometry.c',
  'histogram.c',
  'image.c',
  'inpaint.c',
  'jpegmeta.c',
  'mask.c',
  'scale.c',
  'styles.c',
  'webp.c',
)

ivproc_lib = static_library('ivproc', ivproc_sources,
  c_args : ivproc_args,
  dependencies : ivproc_deps,
  install : false)

ivproc_dep = declare_dependency(
  link_with : ivproc_lib,
  include_directories : include_directories('.'),
  dependencies : ivproc_deps)
