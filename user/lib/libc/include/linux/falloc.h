// LikeOS fallocate(2) mode UAPI
//
// The `mode' argument of fallocate(2): one of the exclusive modes (the
// value 0 is plain allocation), optionally with FALLOC_FL_KEEP_SIZE.  The
// numbers are fixed by the system-call interface programs are built
// against.
//
//   0 (ALLOCATE_RANGE)  allocate the range; the size grows to cover it
//                       unless KEEP_SIZE is given.  New blocks read as
//                       zeros.
//   FALLOC_FL_PUNCH_HOLE  free the range (must come with KEEP_SIZE): it
//                       reads as zeros and owns no space afterwards
//   FALLOC_FL_ZERO_RANGE  make the range read as zeros, keeping (or
//                       allocating) its space; the size grows unless
//                       KEEP_SIZE is given
//   COLLAPSE_RANGE, INSERT_RANGE, UNSHARE_RANGE, WRITE_ZEROES  defined
//                       for completeness; refused with EOPNOTSUPP where a
//                       file system or device does not implement them
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_UAPI_FALLOC_H
#define LIKEOS_UAPI_FALLOC_H

#define FALLOC_FL_ALLOCATE_RANGE 0x00
#define FALLOC_FL_KEEP_SIZE 0x01
#define FALLOC_FL_PUNCH_HOLE 0x02
#define FALLOC_FL_NO_HIDE_STALE 0x04
#define FALLOC_FL_COLLAPSE_RANGE 0x08
#define FALLOC_FL_ZERO_RANGE 0x10
#define FALLOC_FL_INSERT_RANGE 0x20
#define FALLOC_FL_UNSHARE_RANGE 0x40
#define FALLOC_FL_WRITE_ZEROES 0x80

/* The mode bits: exactly one mode (or none) per call. */
#define FALLOC_FL_MODE_MASK                                                  \
	(FALLOC_FL_ALLOCATE_RANGE | FALLOC_FL_PUNCH_HOLE |                   \
	 FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_ZERO_RANGE |                   \
	 FALLOC_FL_INSERT_RANGE | FALLOC_FL_UNSHARE_RANGE |                  \
	 FALLOC_FL_WRITE_ZEROES)

#endif /* LIKEOS_UAPI_FALLOC_H */
