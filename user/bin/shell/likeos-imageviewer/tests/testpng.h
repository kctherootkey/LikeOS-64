// LikeOS -- LikeOS Image Viewer: PNG output (and, with libpng, input) for
// the processing library's unit tests.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TESTPNG_H
#define TESTPNG_H

#include <stddef.h>
#include <stdint.h>

#include "ivproc.h"

/* Write an RGBA PNG (uncompressed deflate blocks, no library needed).
 * Returns 0 / -1. */
int tp_write_png(const char *path, const IvImage *img);
/* Write a mask as a greyscale PNG. */
int tp_write_mask_png(const char *path, const uint8_t *mask, int w, int h);
/* Read a PNG through libpng; NULL when unavailable or on error. */
IvImage *tp_read_png(const char *path);
/* CRC-32 as used by PNG chunks (also used to build test blobs). */
uint32_t tp_crc32(const uint8_t *data, size_t len, uint32_t crc);

#endif
