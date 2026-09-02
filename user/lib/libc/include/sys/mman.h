#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

// mmap protection flags
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

// mmap flags
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
/* The older BSD spelling; still what a lot of code writes. */
#define MAP_ANON      MAP_ANONYMOUS
/* Historical BSD spelling for "this mapping is backed by a file" -- the
 * default, so the flag carries no bits.  Kept because portable code still
 * ORs it in (WTF's file mapping does). */
#define MAP_FILE        0
/* Advisory "do not reserve backing store" bit, carried for source
 * compatibility: this kernel demand-pages every anonymous mapping and never
 * reserves swap, so the behaviour asked for is the only behaviour there is.
 * The kernel tests individual flag bits and ignores this one. */
#define MAP_NORESERVE   0x4000
/* Fault the whole range in at mmap time (honoured, not merely accepted). */
#define MAP_POPULATE    0x8000
/* Advisory: the mapping is a thread stack. */
#define MAP_STACK       0x20000
/* MAP_FIXED that fails with EEXIST rather than replace an existing mapping. */
#define MAP_FIXED_NOREPLACE 0x100000

// mmap error return
#define MAP_FAILED      ((void*)-1)

/* madvise advice values.  DONTNEED and FREE release the pages of the range
 * (the mapping stays; the next touch reads zeros); everything else is a
 * hint the kernel accepts and does nothing about. */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
#define MADV_REMOVE     9
#define MADV_DONTFORK   10
#define MADV_DOFORK     11
#define MADV_MERGEABLE  12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE   14
#define MADV_NOHUGEPAGE 15
#define MADV_DONTDUMP   16
#define MADV_DODUMP     17

int madvise(void* addr, size_t len, int advice);
/* One byte per page of the range into vec, bit 0 = resident.  Pages a
 * demand-paged mapping has not faulted in yet report 0. */
int mincore(void* addr, size_t length, unsigned char* vec);

// Memory mapping
void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t len, int prot);
/* mremap(): resize a mapping in place when the space beyond it is free,
 * else (MREMAP_MAYMOVE) relocate it, carrying its pages along unchanged;
 * MREMAP_FIXED names the destination (takes a fifth argument);
 * MREMAP_DONTUNMAP leaves the old range mapped but empty. */
#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
#define MREMAP_DONTUNMAP 4
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);
/* memfd_create(): an anonymous shared-memory file behind a descriptor
 * (ftruncate to size it, mmap(MAP_SHARED) to use it, pass it over a
 * socket to share it). */
#define MFD_CLOEXEC       1U
#define MFD_ALLOW_SEALING 2U
#define MFD_HUGETLB       4U
int memfd_create(const char *name, unsigned int flags);
#define mmap64 mmap
/* msync() flags */
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

/* POSIX shared memory.  Backed by the /dev/shm namespace, so the returned
 * descriptor is an ordinary fd: size it with ftruncate(), map it with
 * mmap(MAP_SHARED), and it is shared with any other process that opens the
 * same name. */
int shm_open(const char* name, int oflag, mode_t mode);
int shm_unlink(const char* name);

int msync(void* addr, size_t length, int flags);
int mlock(const void* addr, size_t len);
int munlock(const void* addr, size_t len);
int mlockall(int flags);
int munlockall(void);

#define MCL_CURRENT 1
#define MCL_FUTURE  2

#ifdef __cplusplus
}
#endif

#endif
