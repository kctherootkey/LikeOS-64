/*
 * sys/errno.h - the older spelling of <errno.h>.
 *
 * See sys/fcntl.h in this directory for why these compatibility headers exist
 * at all.  This one matters slightly more than the rest: a program that
 * includes <sys/errno.h> and does not get it fails with a wall of "E...
 * undeclared" errors that name every error constant it uses and never mention
 * the header.
 */
#ifndef _SYS_ERRNO_H
#define _SYS_ERRNO_H

#include <errno.h>

#endif /* _SYS_ERRNO_H */
