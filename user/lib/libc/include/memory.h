/*
 * memory.h - the pre-standard name for <string.h>
 *
 * System V put memcpy, memset and their neighbours in this header before C89
 * moved them into <string.h>.  Nothing new has been added to it since, and no
 * standard has required it for thirty-five years -- but a great deal of
 * portable software still includes it, usually behind a configure test for
 * HAVE_MEMORY_H, and some (fribidi's command-line tool among them) includes it
 * unconditionally.
 *
 * Every system still ships it for that reason, and every one of them ships it
 * as exactly this: a redirection to <string.h>.
 */

#ifndef _MEMORY_H
#define _MEMORY_H

#include <string.h>

#endif /* _MEMORY_H */
