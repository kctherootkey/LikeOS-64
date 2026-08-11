/*
 * sys/fcntl.h - the older spelling of <fcntl.h>.
 *
 * The header POSIX standardised is <fcntl.h>; <sys/fcntl.h> is where it lived
 * on the BSDs before that, and enough software still writes it that every
 * general-purpose libc keeps this one-line file around.  A program that uses
 * it is not doing anything unusual -- menu-cache is one, and it reaches for it
 * on every system it builds on, correctly, because everywhere else it is
 * there.
 *
 * Including the real header rather than repeating any of it: there is one
 * definition of O_RDONLY in this libc and this is not a second place to keep
 * it in step.
 */
#ifndef _SYS_FCNTL_H
#define _SYS_FCNTL_H

#include <fcntl.h>

#endif /* _SYS_FCNTL_H */
