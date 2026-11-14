/*
 * config.h - the build settings of the zstd port, for the LikeOS libc.
 * Makefile.likeos force-includes it into every source (the C files of the
 * library and of the zstd program, and the x86-64 decoder in assembly).
 *
 * zstd has no configure script: its makefiles pass these as -D options and
 * probe the build host for threads, zlib, liblzma and liblz4.  The probing is
 * replaced by the choices below, each checked against the LikeOS libc:
 *
 *   threads:    on.  The libc's POSIX threads (pthread_create/join, mutexes,
 *               condition variables) are in libc.so, so the library has the
 *               multithreaded compressor (zstd -T#, --adapt) and the program
 *               its asynchronous file I/O, as upstream's default build of
 *               the zstd program has them.  The static library is built the
 *               same way (upstream's "libzstd.a-mt").
 *   legacy:     frames of the v0.5 - v0.7 formats decode (upstream default 5)
 *   zlib, lzma, lz4: off.  zstd reads and writes its own format only;
 *               --format=gzip/xz/lz4 report that the support is not built.
 *   backtrace:  off (no <execinfo.h>).
 *   assembly:   the x86-64 Huffman decoder is used (BMI2 picked at run time).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_ZSTD_CONFIG_H
#define LIKEOS_ZSTD_CONFIG_H

/* lib/libzstd.mk: the xxHash symbols carry a ZSTD_ prefix; no debug code. */
#define XXH_NAMESPACE ZSTD_
#define DEBUGLEVEL 0

#define ZSTD_MULTITHREAD 1
#define ZSTD_LEGACY_SUPPORT 5

/* programs/Makefile BACKTRACE=0 */
#define BACKTRACE_ENABLE 0

#endif /* LIKEOS_ZSTD_CONFIG_H */
