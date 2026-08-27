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

// mmap error return
#define MAP_FAILED      ((void*)-1)

// madvise advice values (advisory only; the kernel takes no action)
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
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
