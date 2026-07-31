#ifndef _MALLOC_H
#define _MALLOC_H

#include <stddef.h>

// Core allocation API (also declared in stdlib.h)
void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);

// Aligned allocation
void* memalign(size_t alignment, size_t size);
void* valloc(size_t size);
void* pvalloc(size_t size);

// Introspection
size_t malloc_usable_size(void* ptr);

// Return unused heap pages to the OS where possible.
// Returns 1 if any memory was released, 0 otherwise.
int malloc_trim(size_t pad);

// Allocation statistics.  mallinfo() truncates to int and is kept for
// source compatibility; prefer mallinfo2().
struct mallinfo {
    int arena;      // Bytes obtained from the OS for arena heaps (non-mmap)
    int ordblks;    // Number of free chunks
    int smblks;     // Number of fast free chunks
    int hblks;      // Number of directly mapped allocations
    int hblkhd;     // Bytes in directly mapped allocations
    int usmblks;    // Unused, always 0
    int fsmblks;    // Bytes in fast free chunks
    int uordblks;   // Bytes in use
    int fordblks;   // Bytes in free chunks (including top)
    int keepcost;   // Releasable top-chunk bytes of the main heap
};

struct mallinfo2 {
    size_t arena;
    size_t ordblks;
    size_t smblks;
    size_t hblks;
    size_t hblkhd;
    size_t usmblks;
    size_t fsmblks;
    size_t uordblks;
    size_t fordblks;
    size_t keepcost;
};

struct mallinfo mallinfo(void);
struct mallinfo2 mallinfo2(void);

#endif /* _MALLOC_H */

