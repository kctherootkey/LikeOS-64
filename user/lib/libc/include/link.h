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

/* Expand a generic ELF structure name to this platform's width: ElfW(Nhdr)
 * is Elf64_Nhdr here.  The conventional spelling code uses so one source
 * serves 32- and 64-bit systems; this system only has the one width, but the
 * macro is where portable callers (JavaScriptCore's build-id walk was the
 * first) expect it. */
#define ElfW(type) Elf64_##type

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

/* ---- The debugger rendezvous ------------------------------------------------
 *
 * dl_iterate_phdr above answers "what is loaded" for code running INSIDE the
 * process.  A debugger is outside it, stopped, and cannot call anything -- so it
 * needs the same answer as plain data it can read out of the process's memory.
 * That is what these are for, and the layout is not ours to choose: it is the
 * long-standing SVR4 arrangement every ELF debugger already knows how to read,
 * which is precisely why following it means not having to teach one.
 *
 * How a debugger finds it, with no cooperation from us:
 *
 *   1. read DT_DEBUG out of the executable's PT_DYNAMIC -- the loader stores
 *      the address of _r_debug there;
 *   2. walk r_map, a doubly-linked list with one link_map per loaded object,
 *      each giving its load bias, its path, and its dynamic section;
 *   3. set a breakpoint at r_brk, which the loader calls whenever that list is
 *      about to change and again once it has, so the debugger can re-read the
 *      list at a moment when it is consistent -- r_state says which of the two
 *      it is looking at.
 *
 * Reading the list while it is mid-edit is the failure this protocol exists to
 * prevent, so r_state must be honoured rather than assumed.
 */
struct link_map {
    Elf64_Addr        l_addr;   /* load bias: add to a p_vaddr for runtime addr */
    char             *l_name;   /* object path; "" for the main executable      */
    Elf64_Dyn        *l_ld;     /* its PT_DYNAMIC                               */
    struct link_map  *l_next;
    struct link_map  *l_prev;
};

/* r_state: what the loader is in the middle of, read at an r_brk stop. */
#define RT_CONSISTENT  0   /* the list is stable and safe to read */
#define RT_ADD         1   /* an object is being added            */
#define RT_DELETE      2   /* an object is being removed          */

struct r_debug {
    int               r_version;  /* 1 */
    struct link_map  *r_map;      /* head of the loaded-object list */
    Elf64_Addr        r_brk;      /* breakpoint here for load/unload notice */
    int               r_state;    /* RT_* above */
    Elf64_Addr        r_ldbase;   /* where the loader itself is mapped */
};

/* Exported by the dynamic loader.  A debugger normally reaches it through
 * DT_DEBUG rather than by name, since it has to work on a process it did not
 * link against; the symbol is here for code inside the process. */
extern struct r_debug _r_debug;

/* The loader calls this immediately before and immediately after it changes the
 * object list.  It does nothing -- its only purpose is to be a stable address
 * worth stopping at, which is why it must not be inlined or optimised away. */
void _dl_debug_state(void);

#ifdef __cplusplus
}
#endif

#endif /* _LINK_H */
