/* <sys/auxv.h> -- the ELF auxiliary vector.
 *
 * The kernel places a vector of (type, value) pairs above the environment
 * on the initial stack: the program headers' address, the page size, the
 * credentials the image started with, the CPU feature word, the address of
 * sixteen random bytes, the name of the executable.  The C runtime records
 * where the vector is at start-up; getauxval() looks entries up in it. */
#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H

#include <elf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The value for `type', or 0 (with errno ENOENT) when there is no such
 * entry.  AT_NULL itself has value 0 and is never "found". */
unsigned long getauxval(unsigned long type);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_AUXV_H */
