/*
 * LikeOS-64 link.h - Walking the loaded shared objects
 *
 * dl_iterate_phdr() hands a callback one description per object currently
 * loaded: where it sits in memory, what it is called, and its program headers.
 * It is the portable way to ask what is mapped -- profilers, crash handlers and
 * stack unwinders all use it -- and it is the only such interface that does not
 * require the caller to know anything about the loader.
 *
 * The work is done by ld-likeos.so, which is the only thing that knows the
 * object list; this header and the wrapper beside it are the public face of it.
 */

#ifndef _LINK_H
#define _LINK_H

#include <stddef.h>
#include <stdint.h>
#include <elf.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One loaded object.
 *
 * dlpi_addr is what to add to a program header's p_vaddr to get a runtime
 * address: the load bias, which is zero for a non-relocatable executable and
 * the load base for everything else here, since all binaries are position
 * independent.
 *
 * dlpi_name is the object's name, and is the EMPTY STRING for the main
 * executable -- that is how callers tell it apart, not by position in the walk.
 *
 * dlpi_adds and dlpi_subs count objects loaded and unloaded since the process
 * started.  A caller that caches what it learns compares them between walks:
 * unchanged means the cache is still good, and a change in dlpi_subs means an
 * entry may now point into unmapped memory.
 */
struct dl_phdr_info {
    Elf64_Addr         dlpi_addr;
    const char        *dlpi_name;
    const Elf64_Phdr  *dlpi_phdr;
    Elf64_Half         dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t             dlpi_tls_modid;
    void              *dlpi_tls_data;
};

/*
 * Call `callback` once per loaded object, in load order.  The second argument
 * is the size of the structure, so a callback compiled against an older
 * definition can tell which fields are present.
 *
 * A callback returning non-zero stops the walk, and that value is what
 * dl_iterate_phdr() returns -- the usual way to say "this is the object I was
 * looking for".  Returns 0 if every object was visited.
 */
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size,
                                    void *data),
                    void *data);

#ifdef __cplusplus
}
#endif

#endif /* _LINK_H */
