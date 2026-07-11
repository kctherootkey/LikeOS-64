/*
 * LikeOS-64 libc dynamic memory allocator
 *
 * Classic bin-based heap allocator with per-thread caching and multiple
 * arenas:
 *
 *  - Chunks carry a size/flags header; free chunks additionally carry a
 *    trailing size copy (boundary tag) so physically adjacent free chunks
 *    can be coalesced immediately in both directions.  Allocated chunks
 *    have no footer: the low bit of the following chunk's size field
 *    (PREV_INUSE) encodes the allocation state instead.
 *  - Free chunks are kept in segregated bins: an array of very small
 *    LIFO "fast" lists, exact-size small bins, size-ordered large bins
 *    with a size skip-list for best-fit lookup, and one unsorted bin
 *    that newly freed chunks pass through before being sorted.
 *  - A per-thread cache (tcache) in front of everything serves the
 *    hottest small sizes without taking any lock.
 *  - The main arena grows with sbrk(); additional arenas live in large
 *    aligned mappings so a chunk's arena is found by address masking.
 *    Requests above a sliding threshold are mapped directly and returned
 *    to the kernel on free.
 *  - Single-linked list pointers (fast bins, tcache) are XOR-protected
 *    with a per-process random secret so a heap overwrite cannot easily
 *    forge them.
 *
 * Corruption policy: on detecting a broken invariant the allocator calls
 * malloc_printerr(), which reports the problem to fd 2 and aborts the
 * process - continuing on top of corrupt metadata is never safe.  DEBUG=1
 * builds print a detailed line (which check failed, the offending chunk
 * address and its size field); production builds print only that heap
 * corruption was detected, without exposing any internal state.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <malloc.h>
#include <sys/mman.h>

#include "../pthread/pthread_internal.h"

/* Futex wrappers (src/syscalls/sched.c) are not declared in any header. */
struct timespec;
extern int futex_wait(volatile int *uaddr, int val, const struct timespec *timeout);
extern int futex_wake(volatile int *uaddr, int count);

/* ------------------------------------------------------------------ */
/* Constants and macros                                               */
/* ------------------------------------------------------------------ */

#define MALLOC_ALIGNMENT   16UL
#define MALLOC_ALIGN_MASK  (MALLOC_ALIGNMENT - 1)
#define SIZE_SZ            (sizeof(size_t))
#define CHUNK_HDR_SZ       (2 * SIZE_SZ)
#define MINSIZE            32UL

#ifndef PAGE_SIZE
#define PAGE_SIZE          4096UL
#endif
#define PAGE_MASK          (PAGE_SIZE - 1)

/* Size-field flag bits */
#define PREV_INUSE         0x1UL
#define IS_MMAPPED         0x2UL
#define NON_MAIN_ARENA     0x4UL
#define SIZE_BITS          (PREV_INUSE | IS_MMAPPED | NON_MAIN_ARENA)

/* Bins: index 1 = unsorted, 2..63 small (exact 16-byte spaced sizes),
 * 64..126 large (logarithmically spaced ranges). */
#define NBINS              128
#define NSMALLBINS         64
#define SMALLBIN_WIDTH     16UL
#define MIN_LARGE_SIZE     (NSMALLBINS * SMALLBIN_WIDTH)   /* 1024 */
#define BINMAPSIZE         4                                /* 4 x 32 bits */

#define NFASTBINS          10
#define MAX_FAST_SIZE      160UL
#define DEFAULT_MAX_FAST   128UL
#define FASTBIN_CONSOLIDATION_THRESHOLD 65536UL

#define TCACHE_MAX_BINS    64
#define TCACHE_COUNT       7
#define TCACHE_DEAD        ((void *)1)

#define DEFAULT_MMAP_THRESHOLD      (128UL * 1024)
#define MMAP_THRESHOLD_MAX          (4UL * 1024 * 1024)
#define DEFAULT_MMAP_MAX            32
#define DEFAULT_TRIM_THRESHOLD      (128UL * 1024)
#define DEFAULT_TOP_PAD             (128UL * 1024)

/* Non-main arena heaps: fixed-size mappings, aligned to their own size so
 * any chunk address can be masked down to its heap header.  Pages are
 * demand-faulted, so the untouched tail of a heap costs no memory. */
#define HEAP_MAX_SIZE      (64UL * 1024 * 1024)

#define MAX_UNSORTED_ITERS 10000
#define MAX_LIST_WALK      1000000   /* hard bound on any list traversal */

#ifndef PTRDIFF_MAX
#define PTRDIFF_MAX ((ptrdiff_t)(~(size_t)0 >> 1))
#endif

struct malloc_chunk {
    size_t mchunk_prev_size;   /* size of previous chunk, only if it is free */
    size_t mchunk_size;        /* chunk size | flag bits */
    struct malloc_chunk *fd;   /* only used while free: bin links */
    struct malloc_chunk *bk;
    struct malloc_chunk *fd_nextsize; /* only while free in a large bin */
    struct malloc_chunk *bk_nextsize;
};
typedef struct malloc_chunk *mchunkptr;
typedef struct malloc_chunk *mbinptr;

#define chunk2mem(p)       ((void *)((char *)(p) + CHUNK_HDR_SZ))
#define mem2chunk(m)       ((mchunkptr)((char *)(m) - CHUNK_HDR_SZ))
#define chunksize(p)       ((p)->mchunk_size & ~SIZE_BITS)
#define chunk_at_offset(p, s) ((mchunkptr)((char *)(p) + (s)))
#define next_chunk(p)      chunk_at_offset(p, chunksize(p))
#define prev_chunk(p)      ((mchunkptr)((char *)(p) - (p)->mchunk_prev_size))
#define prev_inuse(p)      ((p)->mchunk_size & PREV_INUSE)
#define chunk_is_mmapped(p) ((p)->mchunk_size & IS_MMAPPED)
#define chunk_non_main(p)  ((p)->mchunk_size & NON_MAIN_ARENA)

#define set_head(p, s)          ((p)->mchunk_size = (s))
#define set_head_size(p, s)     ((p)->mchunk_size = ((p)->mchunk_size & SIZE_BITS) | (s))
#define set_foot(p, s)          (chunk_at_offset(p, s)->mchunk_prev_size = (s))
#define inuse_bit_at_offset(p, s)       (chunk_at_offset(p, s)->mchunk_size & PREV_INUSE)
#define set_inuse_bit_at_offset(p, s)   (chunk_at_offset(p, s)->mchunk_size |= PREV_INUSE)
#define clear_inuse_bit_at_offset(p, s) (chunk_at_offset(p, s)->mchunk_size &= ~PREV_INUSE)

#define misaligned_mem(m)  ((uintptr_t)(m) & MALLOC_ALIGN_MASK)

#define fastbin_index(sz)  ((unsigned)((sz) >> 4) - 2)
#define smallbin_index(sz) ((unsigned)((sz) >> 4))
#define in_smallbin_range(sz) ((size_t)(sz) < MIN_LARGE_SIZE)

static inline unsigned largebin_index(size_t sz)
{
    return (((sz >> 6) <= 48) ? 48 + (unsigned)(sz >> 6) :
            ((sz >> 9) <= 20) ? 91 + (unsigned)(sz >> 9) :
            ((sz >> 12) <= 10) ? 110 + (unsigned)(sz >> 12) :
            ((sz >> 15) <= 4)  ? 119 + (unsigned)(sz >> 15) :
            ((sz >> 18) <= 2)  ? 124 + (unsigned)(sz >> 18) : 126);
}

#define bin_index(sz) (in_smallbin_range(sz) ? smallbin_index(sz) : largebin_index(sz))

/* Bin sentinels are stored as fd/bk pairs; a bin "chunk" is a fake chunk
 * whose fd field lands on the pair. */
#define bin_at(av, i) \
    ((mbinptr)((char *)&((av)->bins[((i) - 1) * 2]) - offsetof(struct malloc_chunk, fd)))
#define unsorted_chunks(av) bin_at(av, 1)
#define first(bin)  ((bin)->fd)
#define last(bin)   ((bin)->bk)

#define idx2block(i) ((i) >> 5)
#define idx2bit(i)   (1u << ((i) & 31))
#define mark_bin(av, i)   ((av)->binmap[idx2block(i)] |= idx2bit(i))
#define unmark_bin(av, i) ((av)->binmap[idx2block(i)] &= ~idx2bit(i))

/* ------------------------------------------------------------------ */
/* Locks                                                              */
/* ------------------------------------------------------------------ */

typedef struct { volatile int state; } mlock_t;  /* 0 free, 1 locked, 2 contended */

static void mlock_lock(mlock_t *m)
{
    int c = __sync_val_compare_and_swap(&m->state, 0, 1);
    if (c == 0)
        return;
    do {
        if (c == 2 || __sync_val_compare_and_swap(&m->state, 1, 2) != 0)
            futex_wait(&m->state, 2, NULL);
    } while ((c = __sync_val_compare_and_swap(&m->state, 0, 2)) != 0);
}

static int mlock_trylock(mlock_t *m)
{
    return __sync_val_compare_and_swap(&m->state, 0, 1) == 0;
}

static void mlock_unlock(mlock_t *m)
{
    if (__sync_fetch_and_sub(&m->state, 1) != 1) {
        m->state = 0;
        futex_wake(&m->state, 1);
    }
}

static void mlock_reset(mlock_t *m)
{
    m->state = 0;
}

/* ------------------------------------------------------------------ */
/* Data structures                                                    */
/* ------------------------------------------------------------------ */

/* Arena flags */
#define ARENA_NONCONTIGUOUS 0x1

struct malloc_state {
    mlock_t mutex;                     /* one lock per arena */
    int flags;
    int have_fastchunks;               /* set on fast free, cleared by consolidation */
    mchunkptr fastbinsY[NFASTBINS];    /* LIFO single-linked, links protected */
    mchunkptr top;                     /* remainder chunk new memory is carved from */
    mchunkptr last_remainder;          /* most recent small split remainder */
    mchunkptr bins[(NBINS - 1) * 2];   /* circular double-linked bin lists */
    unsigned int binmap[BINMAPSIZE];   /* one bit per bin: may be non-empty */
    struct malloc_state *next;         /* circular list of all arenas */
    size_t attached_threads;
    size_t system_mem;                 /* bytes obtained from the OS (non-mmap) */
    size_t max_system_mem;
};

/* Header at the base of every non-main arena heap (HEAP_MAX_SIZE aligned). */
typedef struct _heap_info {
    struct malloc_state *ar_ptr;
    struct _heap_info *prev;           /* previous heap of the same arena */
    size_t size;                       /* always HEAP_MAX_SIZE */
    char *dirty_top;                   /* highest address ever handed out here */
} heap_info;

typedef struct tcache_entry {
    struct tcache_entry *next;         /* protected pointer */
    uintptr_t key;                     /* marks "currently in a tcache" */
} tcache_entry;

typedef struct tcache_perthread_struct {
    uint16_t counts[TCACHE_MAX_BINS];
    tcache_entry *entries[TCACHE_MAX_BINS];
} tcache_perthread_struct;

#define csize2tidx(sz) (((sz) - MINSIZE) / 16)

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

static struct malloc_state main_arena;
static mlock_t arena_list_lock;        /* global allocator lock: arena list,
                                          arena creation, one-time init */
static unsigned narenas = 0;
static unsigned narenas_limit = 1;
static struct malloc_state *next_to_use = NULL;

static volatile int malloc_initialized = 0;

static uintptr_t ptr_secret;           /* free-list pointer protection secret */
static uintptr_t tcache_entry_key;     /* random tag for tcache double-free checks */
static pthread_key_t tcache_key;
static int tcache_key_valid = 0;

static size_t global_max_fast = DEFAULT_MAX_FAST;

/* Tunables and process-wide statistics */
static struct {
    size_t mmap_threshold;
    size_t trim_threshold;
    size_t top_pad;
    int    n_mmaps_max;
    int    no_dyn_threshold;
    int    perturb_byte;
    volatile long n_mmaps;
    volatile long max_n_mmaps;
    volatile long mmapped_mem;
    volatile long max_mmapped_mem;
} mp_;

/* Protect/reveal single-linked list pointers.  The operation is its own
 * inverse: stored = addr-mix ^ secret ^ ptr. */
static inline void *protect_ptr(void *pos, void *ptr)
{
    return (void *)((((uintptr_t)pos) >> 12) ^ ptr_secret ^ (uintptr_t)ptr);
}
#define REVEAL_PTR(pos, mangled) protect_ptr((pos), (mangled))

/* ------------------------------------------------------------------ */
/* Corruption reporting                                               */
/* ------------------------------------------------------------------ */

#if defined(DEBUG) && DEBUG
static size_t err_append(char *dst, size_t off, const char *s)
{
    while (*s)
        dst[off++] = *s++;
    return off;
}

static size_t err_append_hex(char *dst, size_t off, uint64_t v)
{
    static const char hexd[] = "0123456789abcdef";
    dst[off++] = '0';
    dst[off++] = 'x';
    for (int i = 60; i >= 0; i -= 4)
        dst[off++] = hexd[(v >> i) & 0xf];
    return off;
}
#endif

/* Report a broken heap invariant and abort - corrupt metadata makes any
 * further allocator action unsafe.  DEBUG builds spell out which check
 * failed and the offending chunk; production builds print only a generic
 * notice so no heap layout is leaked. */
__attribute__((noreturn))
static void malloc_printerr(const char *msg, void *chunk)
{
#if defined(DEBUG) && DEBUG
    char buf[256];
    size_t off = 0;
    off = err_append(buf, off, "malloc: ");
    off = err_append(buf, off, msg);
    off = err_append(buf, off, " chunk=");
    off = err_append_hex(buf, off, (uint64_t)(uintptr_t)chunk);
    if (chunk && ((uintptr_t)chunk & 7) == 0) {
        off = err_append(buf, off, " size=");
        off = err_append_hex(buf, off, (uint64_t)((mchunkptr)chunk)->mchunk_size);
    }
    buf[off++] = '\n';
    (void)write(2, buf, off);
#else
    (void)msg;
    (void)chunk;
    static const char m[] = "malloc: heap corruption detected, aborting\n";
    (void)write(2, m, sizeof(m) - 1);
#endif
    abort();
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static inline size_t page_align_up(size_t n)
{
    return (n + PAGE_MASK) & ~PAGE_MASK;
}

/* Convert a request into a usable chunk size; 0 means overflow. */
static inline size_t checked_request2size(size_t req)
{
    if (req > (size_t)(PTRDIFF_MAX - MINSIZE))
        return 0;
    size_t nb = req + SIZE_SZ;
    if (nb < MINSIZE)
        return MINSIZE;
    return (nb + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK;
}

static inline void alloc_perturb(void *mem, size_t bytes)
{
    if (mp_.perturb_byte)
        memset(mem, mp_.perturb_byte ^ 0xff, bytes);
}

static inline void free_perturb(void *mem, size_t bytes)
{
    if (mp_.perturb_byte)
        memset(mem, mp_.perturb_byte, bytes);
}

static inline heap_info *heap_for_ptr(void *ptr)
{
    return (heap_info *)((uintptr_t)ptr & ~(HEAP_MAX_SIZE - 1));
}

static inline struct malloc_state *arena_for_chunk(mchunkptr p)
{
    return chunk_non_main(p) ? heap_for_ptr(p)->ar_ptr : &main_arena;
}

/* Record that memory below the (new) top of a non-main heap may have been
 * written, for trimming decisions. */
static inline void note_top_carve(struct malloc_state *av)
{
    if (av != &main_arena && av->top) {
        heap_info *h = heap_for_ptr(av->top);
        char *d = (char *)av->top + CHUNK_HDR_SZ;
        if (d > h->dirty_top)
            h->dirty_top = d;
    }
}

static inline tcache_perthread_struct *tcache_current(void)
{
    void *tc = __pthread_self()->malloc_tcache;
    return (tc == TCACHE_DEAD) ? NULL : (tcache_perthread_struct *)tc;
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void *_int_malloc(struct malloc_state *av, size_t nb);
static void _int_free(struct malloc_state *av, mchunkptr p, int have_lock);
static void malloc_consolidate(struct malloc_state *av);
static void *sysmalloc(struct malloc_state *av, size_t nb);
static void tcache_maybe_init(void);
static void malloc_init_once(void);

/* ------------------------------------------------------------------ */
/* Bin list surgery                                                   */
/* ------------------------------------------------------------------ */

/* Remove p from its double-linked bin list.  Every surrounding-metadata
 * check aborts (via malloc_printerr) rather than returning, so on return
 * the unlink has always succeeded. */
static void unlink_chunk(struct malloc_state *av, mchunkptr p)
{
    (void)av;
    size_t size = chunksize(p);

    if (chunk_at_offset(p, size)->mchunk_prev_size != size)
        malloc_printerr("corrupted size vs. prev_size", p);

    mchunkptr fd = p->fd;
    mchunkptr bk = p->bk;
    if (fd->bk != p || bk->fd != p)
        malloc_printerr("corrupted double-linked list", p);

    if (!in_smallbin_range(size) && p->fd_nextsize != NULL) {
        if (p->fd_nextsize->bk_nextsize != p ||
            p->bk_nextsize->fd_nextsize != p)
            malloc_printerr("corrupted double-linked list (not small)", p);
        if (fd->fd_nextsize == NULL) {
            /* fd takes over p's slot in the size skip-list */
            if (p->fd_nextsize == p) {
                fd->fd_nextsize = fd->bk_nextsize = fd;
            } else {
                fd->fd_nextsize = p->fd_nextsize;
                fd->bk_nextsize = p->bk_nextsize;
                p->fd_nextsize->bk_nextsize = fd;
                p->bk_nextsize->fd_nextsize = fd;
            }
        } else {
            p->fd_nextsize->bk_nextsize = p->bk_nextsize;
            p->bk_nextsize->fd_nextsize = p->fd_nextsize;
        }
    }

    fd->bk = bk;
    bk->fd = fd;
}

/* ------------------------------------------------------------------ */
/* tcache                                                             */
/* ------------------------------------------------------------------ */

static void tcache_put(tcache_perthread_struct *tc, mchunkptr p, size_t tc_idx)
{
    tcache_entry *e = (tcache_entry *)chunk2mem(p);
    e->key = tcache_entry_key;
    e->next = protect_ptr(&e->next, tc->entries[tc_idx]);
    tc->entries[tc_idx] = e;
    ++tc->counts[tc_idx];
}

static void *tcache_get(tcache_perthread_struct *tc, size_t tc_idx)
{
    tcache_entry *e = tc->entries[tc_idx];
    if (!e)
        return NULL;
    if (misaligned_mem(e))
        malloc_printerr("malloc(): unaligned tcache chunk detected", e);
    tc->entries[tc_idx] = REVEAL_PTR(&e->next, e->next);
    --tc->counts[tc_idx];
    e->key = 0;
    return (void *)e;
}

/* Free every cached chunk back to its arena; runs as a TSD destructor at
 * thread exit and marks the cache slot dead. */
static void tcache_thread_shutdown(void *arg)
{
    struct __pthread *tcb = __pthread_self();
    tcache_perthread_struct *tc = (tcache_perthread_struct *)arg;

    tcb->malloc_tcache = TCACHE_DEAD;
    if (!tc || (void *)tc == TCACHE_DEAD)
        return;

    for (int i = 0; i < TCACHE_MAX_BINS; i++) {
        tcache_entry *e = tc->entries[i];
        tc->entries[i] = NULL;
        tc->counts[i] = 0;
        while (e) {
            if (misaligned_mem(e))
                malloc_printerr("tcache_thread_shutdown(): unaligned tcache chunk detected", e);
            tcache_entry *next = REVEAL_PTR(&e->next, e->next);
            e->key = 0;
            mchunkptr p = mem2chunk(e);
            struct malloc_state *av = arena_for_chunk(p);
            mlock_lock(&av->mutex);
            _int_free(av, p, 1);
            mlock_unlock(&av->mutex);
            e = next;
        }
    }

    mchunkptr p = mem2chunk(tc);
    struct malloc_state *av = arena_for_chunk(p);
    mlock_lock(&av->mutex);
    _int_free(av, p, 1);
    mlock_unlock(&av->mutex);

    av = (struct malloc_state *)tcb->malloc_arena;
    if (av)
        __sync_fetch_and_sub(&av->attached_threads, 1);
}

/* ------------------------------------------------------------------ */
/* Heaps and arenas                                                   */
/* ------------------------------------------------------------------ */

/* Map a new HEAP_MAX_SIZE-aligned heap.  Over-map twice the size and trim
 * both edges so exactly one aligned region remains. */
static heap_info *new_heap(void)
{
    char *raw = mmap(NULL, 2 * HEAP_MAX_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;

    char *aligned = (char *)(((uintptr_t)raw + HEAP_MAX_SIZE - 1) & ~(HEAP_MAX_SIZE - 1));
    size_t head = (size_t)(aligned - raw);
    size_t tail = HEAP_MAX_SIZE - head;
    if (head)
        munmap(raw, head);
    if (tail)
        munmap(aligned + HEAP_MAX_SIZE, tail);

    heap_info *h = (heap_info *)aligned;
    h->ar_ptr = NULL;
    h->prev = NULL;
    h->size = HEAP_MAX_SIZE;
    h->dirty_top = aligned + sizeof(heap_info);
    return h;
}

static void malloc_init_state(struct malloc_state *av)
{
    for (int i = 1; i < NBINS; i++) {
        mbinptr bin = bin_at(av, i);
        bin->fd = bin->bk = bin;
    }
    for (int i = 0; i < BINMAPSIZE; i++)
        av->binmap[i] = 0;
    for (int i = 0; i < NFASTBINS; i++)
        av->fastbinsY[i] = NULL;
    av->top = NULL;
    av->last_remainder = NULL;
    av->have_fastchunks = 0;
    av->flags = 0;
    av->system_mem = 0;
    av->max_system_mem = 0;
    av->attached_threads = 0;
    mlock_reset(&av->mutex);
}

/* Create a new arena inside a fresh heap.  Caller holds arena_list_lock. */
static struct malloc_state *arena_new(void)
{
    heap_info *h = new_heap();
    if (!h)
        return NULL;

    char *base = (char *)h + ((sizeof(heap_info) + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK);
    struct malloc_state *av = (struct malloc_state *)base;
    malloc_init_state(av);
    h->ar_ptr = av;

    char *top = base + ((sizeof(struct malloc_state) + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK);
    av->top = (mchunkptr)top;
    set_head(av->top, (size_t)(((char *)h + HEAP_MAX_SIZE) - top) | PREV_INUSE);
    av->system_mem = av->max_system_mem = HEAP_MAX_SIZE;
    h->dirty_top = top + CHUNK_HDR_SZ;

    /* Publish on the circular arena list (readers walk without the lock;
     * arenas are never removed). */
    av->next = main_arena.next;
    __sync_synchronize();
    main_arena.next = av;
    narenas++;
    return av;
}

/* Pick an existing arena, preferring one that is not currently locked.
 * Returns the arena LOCKED. */
static struct malloc_state *reused_arena(void)
{
    struct malloc_state *start = next_to_use ? next_to_use : &main_arena;
    struct malloc_state *av = start;

    do {
        if (mlock_trylock(&av->mutex))
            goto out;
        av = av->next;
    } while (av != start);

    av = start;
    mlock_lock(&av->mutex);
out:
    next_to_use = av->next;
    return av;
}

/* Return the calling thread's arena, LOCKED.  Creates a new arena when the
 * preferred one is contended and the arena limit allows it. */
static struct malloc_state *arena_get(void)
{
    struct __pthread *tcb = __pthread_self();
    struct malloc_state *av = (struct malloc_state *)tcb->malloc_arena;

    if (av) {
        if (mlock_trylock(&av->mutex))
            return av;
        if (narenas < narenas_limit) {
            mlock_lock(&arena_list_lock);
            struct malloc_state *fresh =
                (narenas < narenas_limit) ? arena_new() : NULL;
            mlock_unlock(&arena_list_lock);
            if (fresh) {
                mlock_lock(&fresh->mutex);
                __sync_fetch_and_sub(&av->attached_threads, 1);
                __sync_fetch_and_add(&fresh->attached_threads, 1);
                tcb->malloc_arena = fresh;
                return fresh;
            }
        }
        mlock_lock(&av->mutex);
        return av;
    }

    /* First allocation in this thread */
    if (mlock_trylock(&main_arena.mutex)) {
        av = &main_arena;
    } else if (narenas < narenas_limit) {
        mlock_lock(&arena_list_lock);
        av = (narenas < narenas_limit) ? arena_new() : NULL;
        mlock_unlock(&arena_list_lock);
        if (av)
            mlock_lock(&av->mutex);
    }
    if (!av)
        av = reused_arena();

    __sync_fetch_and_add(&av->attached_threads, 1);
    tcb->malloc_arena = av;
    return av;
}

/* ------------------------------------------------------------------ */
/* One-time initialization                                            */
/* ------------------------------------------------------------------ */

static size_t env_size(const char *name, size_t dflt)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return dflt;
    return (size_t)strtoul(s, NULL, 10);
}

static void malloc_init_once(void)
{
    mlock_lock(&arena_list_lock);
    if (malloc_initialized) {
        mlock_unlock(&arena_list_lock);
        return;
    }

    /* Force the permanent main-thread TCB into place before any per-thread
     * allocator state is stored through %fs (the early bootstrap TLS page
     * is replaced on first pthread use). */
    pthread_self();

    /* Pointer-protection secrets */
    uint64_t r[2] = { 0, 0 };
    if (getrandom(r, sizeof(r), 0) != (ssize_t)sizeof(r) || r[0] == 0) {
        r[0] = (uintptr_t)&main_arena ^ ((uintptr_t)gettid() << 32) ^ 0x9e3779b97f4a7c15ULL;
        r[1] = r[0] * 0xff51afd7ed558ccdULL;
    }
    ptr_secret = (uintptr_t)r[0];
    tcache_entry_key = (uintptr_t)r[1] | 1;   /* never 0 */

    /* Tunables */
    mp_.mmap_threshold = env_size("MALLOC_MMAP_THRESHOLD_", DEFAULT_MMAP_THRESHOLD);
    mp_.no_dyn_threshold = getenv("MALLOC_MMAP_THRESHOLD_") != NULL;
    mp_.trim_threshold = env_size("MALLOC_TRIM_THRESHOLD_", DEFAULT_TRIM_THRESHOLD);
    mp_.top_pad = env_size("MALLOC_TOP_PAD_", DEFAULT_TOP_PAD);
    mp_.n_mmaps_max = (int)env_size("MALLOC_MMAP_MAX_", DEFAULT_MMAP_MAX);
    mp_.perturb_byte = (int)(env_size("MALLOC_PERTURB_", 0) & 0xff);

    /* Arena limit: one per CPU, capped */
    cpu_set_t set;
    int ncpu = 0;
    memset(&set, 0, sizeof(set));
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        ncpu = __cpu_count(&set);
    if (ncpu < 1)
        ncpu = 4;
    if (ncpu > 8)
        ncpu = 8;
    narenas_limit = (unsigned)ncpu;
    size_t arena_max = env_size("MALLOC_ARENA_MAX", 0);
    if (arena_max >= 1 && arena_max <= 64)
        narenas_limit = (unsigned)arena_max;

    malloc_init_state(&main_arena);
    main_arena.next = &main_arena;
    narenas = 1;

    if (pthread_key_create(&tcache_key, tcache_thread_shutdown) == 0)
        tcache_key_valid = 1;

    __sync_synchronize();
    malloc_initialized = 1;
    mlock_unlock(&arena_list_lock);
}

/* Allocate this thread's cache lazily.  Never recurses into the public
 * entry points. */
static void tcache_maybe_init(void)
{
    struct __pthread *tcb = __pthread_self();
    if (tcb->malloc_tcache != NULL)
        return;

    size_t nb = checked_request2size(sizeof(tcache_perthread_struct));
    struct malloc_state *av = arena_get();
    void *mem = _int_malloc(av, nb);
    mlock_unlock(&av->mutex);
    if (!mem)
        return;

    memset(mem, 0, sizeof(tcache_perthread_struct));
    tcb->malloc_tcache = mem;
    if (tcache_key_valid)
        pthread_setspecific(tcache_key, mem);
}

/* ------------------------------------------------------------------ */
/* System memory: direct mappings                                     */
/* ------------------------------------------------------------------ */

static void *sysmalloc_mmap(size_t nb)
{
    size_t size = page_align_up(nb + SIZE_SZ);
    if (size < nb)
        return NULL;

    char *base = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;

    mchunkptr p = (mchunkptr)base;
    p->mchunk_prev_size = 0;
    set_head(p, size | IS_MMAPPED);

    long n = __sync_add_and_fetch(&mp_.n_mmaps, 1);
    if (n > mp_.max_n_mmaps)
        mp_.max_n_mmaps = n;
    long mm = __sync_add_and_fetch(&mp_.mmapped_mem, (long)size);
    if (mm > mp_.max_mmapped_mem)
        mp_.max_mmapped_mem = mm;

    return chunk2mem(p);
}

static void munmap_chunk(mchunkptr p)
{
    size_t size = chunksize(p);
    uintptr_t block = (uintptr_t)p - p->mchunk_prev_size;
    size_t total = p->mchunk_prev_size + size;

    if ((block | total) & PAGE_MASK)
        malloc_printerr("munmap_chunk(): invalid pointer", p);

    __sync_fetch_and_sub(&mp_.n_mmaps, 1);
    __sync_fetch_and_sub(&mp_.mmapped_mem, (long)total);
    munmap((void *)block, total);
}

/* ------------------------------------------------------------------ */
/* System memory: growing an arena                                    */
/* ------------------------------------------------------------------ */

/* Shrink the old top and write two closed 16-byte marker headers at its end
 * so coalescing can never walk past the border, then free the remainder. */
static void fencepost_old_top(struct malloc_state *av, mchunkptr old_top, size_t old_size)
{
    if (!old_top || old_size == 0)
        return;

    if (old_size >= MINSIZE + 2 * CHUNK_HDR_SZ) {
        size_t rest = (old_size - MINSIZE) & ~MALLOC_ALIGN_MASK;
        set_head(chunk_at_offset(old_top, rest + 2 * SIZE_SZ), 0 | PREV_INUSE);
        set_head(chunk_at_offset(old_top, rest), (2 * SIZE_SZ) | PREV_INUSE);
        set_foot(chunk_at_offset(old_top, rest), (2 * SIZE_SZ));
        set_head(old_top, rest | PREV_INUSE |
                 (av != &main_arena ? NON_MAIN_ARENA : 0));
        if (rest >= MINSIZE)
            _int_free(av, old_top, 1);
    } else {
        /* Too small to split: leave it marked in use (a bounded leak). */
        set_head(old_top, old_size | PREV_INUSE |
                 (av != &main_arena ? NON_MAIN_ARENA : 0));
    }
}

/* Grow av so a request of nb can be satisfied, then carve and return the
 * chunk.  Called with the arena locked. */
static void *sysmalloc(struct malloc_state *av, size_t nb)
{
    /* Large requests are mapped directly. */
    if (nb >= mp_.mmap_threshold && mp_.n_mmaps < mp_.n_mmaps_max) {
        void *mm = sysmalloc_mmap(nb);
        if (mm)
            return mm;
    }

    mchunkptr old_top = av->top;
    size_t old_size = old_top ? chunksize(old_top) : 0;

    if (av != &main_arena) {
        heap_info *h = new_heap();
        if (h) {
            h->ar_ptr = av;
            h->prev = old_top ? heap_for_ptr(old_top) : NULL;
            av->system_mem += HEAP_MAX_SIZE;
            if (av->system_mem > av->max_system_mem)
                av->max_system_mem = av->system_mem;

            char *top = (char *)h + ((sizeof(heap_info) + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK);
            av->top = (mchunkptr)top;
            set_head(av->top, (size_t)(((char *)h + HEAP_MAX_SIZE) - top) | PREV_INUSE);
            h->dirty_top = top + CHUNK_HDR_SZ;

            fencepost_old_top(av, old_top, old_size);
        }
        /* On failure fall through: the final direct-map fallback below. */
    } else {
        size_t need = nb + mp_.top_pad + MINSIZE;
        size_t grow = page_align_up(need > old_size ? need - old_size : PAGE_SIZE);

        char *brk = sbrk((intptr_t)grow);
        if (brk == (char *)-1) {
            need = nb + MINSIZE;
            grow = page_align_up(need > old_size ? need - old_size : PAGE_SIZE);
            brk = sbrk((intptr_t)grow);
        }

        if (brk != (char *)-1) {
            if (old_top && brk == (char *)old_top + old_size) {
                /* Contiguous extension of the existing top chunk */
                set_head(av->top, (old_size + grow) | PREV_INUSE);
            } else {
                /* First growth, or someone else moved the break */
                char *aligned = (char *)(((uintptr_t)brk + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK);
                size_t waste = (size_t)(aligned - brk);
                av->top = (mchunkptr)aligned;
                set_head(av->top, (grow - waste) | PREV_INUSE);
                if (old_top) {
                    av->flags |= ARENA_NONCONTIGUOUS;
                    fencepost_old_top(av, old_top, old_size);
                }
                /* Make sure the new top is big enough on this path */
                if (chunksize(av->top) < nb + MINSIZE) {
                    size_t extra = page_align_up(nb + MINSIZE - chunksize(av->top));
                    char *more = sbrk((intptr_t)extra);
                    if (more == (char *)av->top + chunksize(av->top)) {
                        set_head(av->top, (chunksize(av->top) + extra) | PREV_INUSE);
                        grow += extra;
                    } else if (more != (char *)-1) {
                        /* Disjoint again; give the piece back */
                        sbrk(-(intptr_t)extra);
                    }
                }
            }
            av->system_mem += grow;
            if (av->system_mem > av->max_system_mem)
                av->max_system_mem = av->system_mem;
        }
    }

    /* Carve the request from the (possibly new) top chunk. */
    mchunkptr top = av->top;
    size_t top_size = top ? chunksize(top) : 0;
    if (top && top_size >= nb + MINSIZE) {
        mchunkptr victim = top;
        av->top = chunk_at_offset(victim, nb);
        set_head(av->top, (top_size - nb) | PREV_INUSE);
        set_head(victim, nb | PREV_INUSE |
                 (av != &main_arena ? NON_MAIN_ARENA : 0));
        note_top_carve(av);
        return chunk2mem(victim);
    }

    /* Last resort: map the single request directly, below the threshold. */
    return sysmalloc_mmap(nb);
}

/* ------------------------------------------------------------------ */
/* Trimming                                                           */
/* ------------------------------------------------------------------ */

/* Give the tail of the main heap back via a negative sbrk.  (The kernel
 * keeps the pages mapped; this only shrinks the address-space footprint
 * and keeps the top chunk small.) */
static int systrim(struct malloc_state *av, size_t pad)
{
    if (!av->top || (av->flags & ARENA_NONCONTIGUOUS))
        return 0;

    size_t top_size = chunksize(av->top);
    if (top_size <= pad + MINSIZE + PAGE_SIZE)
        return 0;

    size_t extra = (top_size - pad - MINSIZE - 1) & ~PAGE_MASK;
    if (extra == 0)
        return 0;

    char *top_end = (char *)av->top + top_size;
    if ((char *)sbrk(0) != top_end)
        return 0;   /* someone else moved the break */
    if (sbrk(-(intptr_t)extra) == (void *)-1)
        return 0;

    set_head(av->top, (top_size - extra) | PREV_INUSE);
    av->system_mem -= extra;
    return 1;
}

/* Physically release the used-then-freed page span inside a non-main
 * heap's top chunk.  Interior unmaps of a demand-paged mapping drop the
 * pages while keeping the range valid: the next touch faults in zeros.
 * The last page of the heap is always kept so the unmap stays interior. */
static int heap_release_top(struct malloc_state *av, size_t pad, size_t min_release)
{
    if (!av->top)
        return 0;

    heap_info *h = heap_for_ptr(av->top);
    char *heap_end = (char *)h + h->size;

    char *rs = (char *)page_align_up((uintptr_t)((char *)av->top + MINSIZE + pad));
    char *re = heap_end - PAGE_SIZE;
    char *dirty_end = (char *)page_align_up((uintptr_t)h->dirty_top);
    if (dirty_end < re)
        re = dirty_end;

    if (re <= rs || (size_t)(re - rs) < min_release)
        return 0;

    munmap(rs, (size_t)(re - rs));
    if (h->dirty_top > rs)
        h->dirty_top = rs;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Fast-bin consolidation                                             */
/* ------------------------------------------------------------------ */

static void malloc_consolidate(struct malloc_state *av)
{
    av->have_fastchunks = 0;

    for (int i = 0; i < NFASTBINS; i++) {
        mchunkptr p = av->fastbinsY[i];
        av->fastbinsY[i] = NULL;

        while (p) {
            if (misaligned_mem(chunk2mem(p)))
                malloc_printerr("malloc_consolidate(): unaligned fastbin chunk detected", p);
            mchunkptr nextp = REVEAL_PTR(&p->fd, p->fd);

            size_t size = chunksize(p);
            mchunkptr nextchunk = chunk_at_offset(p, size);
            size_t nextsize = chunksize(nextchunk);

            if (!prev_inuse(p)) {
                size_t prevsize = p->mchunk_prev_size;
                mchunkptr prv = chunk_at_offset(p, -(ptrdiff_t)prevsize);
                if (chunksize(prv) != prevsize)
                    malloc_printerr("corrupted size vs. prev_size in fastbins", p);
                unlink_chunk(av, prv);
                size += prevsize;
                p = prv;
            }

            if (nextchunk != av->top) {
                if (!inuse_bit_at_offset(nextchunk, nextsize)) {
                    unlink_chunk(av, nextchunk);
                    size += nextsize;
                } else {
                    clear_inuse_bit_at_offset(nextchunk, 0);
                }

                mbinptr unsorted = unsorted_chunks(av);
                mchunkptr fwd = unsorted->fd;
                if (fwd->bk != unsorted)
                    malloc_printerr("malloc_consolidate(): corrupted unsorted chunks", fwd);
                p->fd = fwd;
                p->bk = unsorted;
                unsorted->fd = p;
                fwd->bk = p;
                if (!in_smallbin_range(size))
                    p->fd_nextsize = p->bk_nextsize = NULL;
                set_head(p, size | PREV_INUSE);
                set_foot(p, size);
            } else {
                size += nextsize;
                set_head(p, size | PREV_INUSE);
                av->top = p;
            }

            p = nextp;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Core allocation                                                    */
/* ------------------------------------------------------------------ */

static void *_int_malloc(struct malloc_state *av, size_t nb)
{
    tcache_perthread_struct *tc = tcache_current();
    size_t tc_idx = csize2tidx(nb);
    int tcache_usable = tc && tc_idx < TCACHE_MAX_BINS;
    unsigned idx;

    /* --- fast bins: exact size, LIFO, first fit --- */
    if (nb <= global_max_fast) {
        idx = fastbin_index(nb);
        mchunkptr victim = av->fastbinsY[idx];
        if (victim) {
            if (misaligned_mem(chunk2mem(victim)))
                malloc_printerr("malloc(): unaligned fastbin chunk detected", victim);
            if (fastbin_index(chunksize(victim)) != idx)
                malloc_printerr("malloc(): memory corruption (fast)", victim);
            av->fastbinsY[idx] = REVEAL_PTR(&victim->fd, victim->fd);

            /* refill the thread cache from the same fast bin */
            if (tcache_usable) {
                while (tc->counts[tc_idx] < TCACHE_COUNT) {
                    mchunkptr tcv = av->fastbinsY[idx];
                    if (!tcv)
                        break;
                    if (misaligned_mem(chunk2mem(tcv)) ||
                        fastbin_index(chunksize(tcv)) != idx)
                        malloc_printerr("malloc(): unaligned fastbin chunk detected 2", tcv);
                    av->fastbinsY[idx] = REVEAL_PTR(&tcv->fd, tcv->fd);
                    tcache_put(tc, tcv, tc_idx);
                }
            }
            return chunk2mem(victim);
        }
    }

    /* --- small bins: exact size, FIFO --- */
    if (in_smallbin_range(nb)) {
        idx = smallbin_index(nb);
        mbinptr bin = bin_at(av, idx);
        mchunkptr victim = last(bin);
        if (victim != bin) {
            mchunkptr bck = victim->bk;
            if (bck->fd != victim)
                malloc_printerr("malloc(): smallbin double linked list corrupted", victim);
            set_inuse_bit_at_offset(victim, nb);
            bin->bk = bck;
            bck->fd = bin;
            if (av != &main_arena)
                victim->mchunk_size |= NON_MAIN_ARENA;

            /* refill the thread cache from the same bin */
            if (tcache_usable) {
                while (tc->counts[tc_idx] < TCACHE_COUNT) {
                    mchunkptr tcv = last(bin);
                    if (tcv == bin)
                        break;
                    bck = tcv->bk;
                    if (bck->fd != tcv)
                        malloc_printerr("malloc(): smallbin double linked list corrupted 2", tcv);
                    set_inuse_bit_at_offset(tcv, nb);
                    bin->bk = bck;
                    bck->fd = bin;
                    if (av != &main_arena)
                        tcv->mchunk_size |= NON_MAIN_ARENA;
                    tcache_put(tc, tcv, tc_idx);
                }
            }
            return chunk2mem(victim);
        }
    } else {
        idx = largebin_index(nb);
        if (av->have_fastchunks)
            malloc_consolidate(av);
    }

    for (;;) {
        int iters = 0;
        int return_cached = 0;
        mbinptr unsorted = unsorted_chunks(av);
        mchunkptr victim;

        /* --- unsorted bin: take exact fits, sort the rest into bins --- */
        while ((victim = unsorted->bk) != unsorted) {
            mchunkptr bck = victim->bk;
            size_t size = chunksize(victim);

            if (misaligned_mem(chunk2mem(victim)) ||
                size <= CHUNK_HDR_SZ || size > av->system_mem)
                malloc_printerr("malloc(): invalid size (unsorted)", victim);
            mchunkptr nextu = chunk_at_offset(victim, size);
            size_t nextu_size = chunksize(nextu);
            if (nextu_size <= CHUNK_HDR_SZ || nextu_size > av->system_mem ||
                nextu->mchunk_prev_size != size)
                malloc_printerr("malloc(): invalid next size (unsorted)", victim);
            if (bck->fd != victim || victim->fd != unsorted)
                malloc_printerr("malloc(): unsorted double linked list corrupted", victim);

            /* Split the most recent remainder for streams of small requests */
            if (in_smallbin_range(nb) &&
                bck == unsorted &&
                victim == av->last_remainder &&
                size > nb + MINSIZE) {
                size_t remainder_size = size - nb;
                mchunkptr remainder = chunk_at_offset(victim, nb);
                unsorted->bk = unsorted->fd = remainder;
                av->last_remainder = remainder;
                remainder->bk = remainder->fd = unsorted;
                if (!in_smallbin_range(remainder_size))
                    remainder->fd_nextsize = remainder->bk_nextsize = NULL;
                set_head(victim, nb | PREV_INUSE |
                         (av != &main_arena ? NON_MAIN_ARENA : 0));
                set_head(remainder, remainder_size | PREV_INUSE);
                set_foot(remainder, remainder_size);
                return chunk2mem(victim);
            }

            /* remove from unsorted */
            unsorted->bk = bck;
            bck->fd = unsorted;

            if (size == nb) {
                set_inuse_bit_at_offset(victim, size);
                if (av != &main_arena)
                    victim->mchunk_size |= NON_MAIN_ARENA;
                if (tcache_usable && tc->counts[tc_idx] < TCACHE_COUNT) {
                    tcache_put(tc, victim, tc_idx);
                    return_cached = 1;
                    if (tc->counts[tc_idx] >= TCACHE_COUNT)
                        return tcache_get(tc, tc_idx);
                    goto next_unsorted;
                }
                return chunk2mem(victim);
            }

            /* place the chunk in its proper bin */
            if (in_smallbin_range(size)) {
                unsigned vidx = smallbin_index(size);
                mbinptr bin = bin_at(av, vidx);
                mchunkptr fwd = bin->fd;
                victim->bk = bin;
                victim->fd = fwd;
                fwd->bk = victim;
                bin->fd = victim;
                mark_bin(av, vidx);
            } else {
                unsigned vidx = largebin_index(size);
                mbinptr bin = bin_at(av, vidx);
                mchunkptr fwd = bin->fd;
                mchunkptr lbck;

                if (fwd == bin) {
                    /* empty bin */
                    victim->fd_nextsize = victim->bk_nextsize = victim;
                    lbck = bin;
                } else if (size < chunksize(bin->bk)) {
                    /* smaller than everything: link at the tail */
                    if (fwd->bk_nextsize->fd_nextsize != fwd)
                        malloc_printerr("malloc(): largebin double linked list corrupted (nextsize)", fwd);
                    victim->fd_nextsize = fwd;
                    victim->bk_nextsize = fwd->bk_nextsize;
                    fwd->bk_nextsize = victim;
                    victim->bk_nextsize->fd_nextsize = victim;
                    lbck = bin->bk;
                    fwd = bin;
                } else {
                    int walk = 0;
                    while (size < chunksize(fwd)) {
                        fwd = fwd->fd_nextsize;
                        if (++walk > MAX_LIST_WALK)
                            malloc_printerr("malloc(): largebin nextsize walk overrun", fwd);
                    }
                    if (size == chunksize(fwd)) {
                        fwd = fwd->fd;   /* equal size: behind the leader */
                    } else {
                        victim->fd_nextsize = fwd;
                        victim->bk_nextsize = fwd->bk_nextsize;
                        fwd->bk_nextsize = victim;
                        victim->bk_nextsize->fd_nextsize = victim;
                    }
                    lbck = fwd->bk;
                    if (lbck->fd != fwd)
                        malloc_printerr("malloc(): largebin double linked list corrupted (bk)", lbck);
                }

                victim->bk = lbck;
                victim->fd = fwd;
                fwd->bk = victim;
                lbck->fd = victim;
                mark_bin(av, vidx);
            }

next_unsorted:
            if (++iters >= MAX_UNSORTED_ITERS)
                break;
        }

        if (return_cached)
            return tcache_get(tc, tc_idx);

        /* --- large bins: best fit via the size skip-list --- */
        if (!in_smallbin_range(nb)) {
            mbinptr bin = bin_at(av, idx);
            victim = first(bin);
            if (victim != bin && chunksize(victim) >= nb) {
                int walk = 0;
                victim = victim->bk_nextsize;
                while (chunksize(victim) < nb) {
                    victim = victim->bk_nextsize;
                    if (++walk > MAX_LIST_WALK)
                        malloc_printerr("malloc(): largebin bk_nextsize walk overrun", victim);
                }

                /* prefer a same-size follower: no skip-list surgery */
                if (victim != last(bin) &&
                    chunksize(victim) == chunksize(victim->fd))
                    victim = victim->fd;

                size_t size = chunksize(victim);
                unlink_chunk(av, victim);
                size_t remainder_size = size - nb;
                if (remainder_size < MINSIZE) {
                    set_inuse_bit_at_offset(victim, size);
                    if (av != &main_arena)
                        victim->mchunk_size |= NON_MAIN_ARENA;
                } else {
                    mchunkptr remainder = chunk_at_offset(victim, nb);
                    mbinptr u = unsorted_chunks(av);
                    mchunkptr fwd = u->fd;
                    if (fwd->bk != u)
                        malloc_printerr("malloc(): corrupted unsorted chunks", fwd);
                    remainder->bk = u;
                    remainder->fd = fwd;
                    u->fd = remainder;
                    fwd->bk = remainder;
                    if (in_smallbin_range(nb))
                        av->last_remainder = remainder;
                    if (!in_smallbin_range(remainder_size))
                        remainder->fd_nextsize = remainder->bk_nextsize = NULL;
                    set_head(victim, nb | PREV_INUSE |
                             (av != &main_arena ? NON_MAIN_ARENA : 0));
                    set_head(remainder, remainder_size | PREV_INUSE);
                    set_foot(remainder, remainder_size);
                }
                return chunk2mem(victim);
            }
        }

        /* --- bitmap scan: smallest chunk in the next non-empty bin --- */
        {
            unsigned i = idx + 1;
            while (i < NBINS - 1) {
                unsigned block = idx2block(i);
                if (av->binmap[block] == 0) {
                    i = (block + 1) << 5;
                    continue;
                }
                if (!(av->binmap[block] & idx2bit(i))) {
                    i++;
                    continue;
                }
                mbinptr bin = bin_at(av, i);
                victim = last(bin);
                if (victim == bin) {
                    unmark_bin(av, i);   /* stale bit */
                    i++;
                    continue;
                }
                size_t size = chunksize(victim);
                if (size < nb + MINSIZE && size != nb) {
                    i++;
                    continue;
                }
                unlink_chunk(av, victim);

                size_t remainder_size = size - nb;
                if (remainder_size < MINSIZE) {
                    set_inuse_bit_at_offset(victim, size);
                    if (av != &main_arena)
                        victim->mchunk_size |= NON_MAIN_ARENA;
                } else {
                    mchunkptr remainder = chunk_at_offset(victim, nb);
                    mbinptr u = unsorted_chunks(av);
                    mchunkptr fwd = u->fd;
                    if (fwd->bk != u)
                        malloc_printerr("malloc(): corrupted unsorted chunks 2", fwd);
                    remainder->bk = u;
                    remainder->fd = fwd;
                    u->fd = remainder;
                    fwd->bk = remainder;
                    if (in_smallbin_range(nb))
                        av->last_remainder = remainder;
                    if (!in_smallbin_range(remainder_size))
                        remainder->fd_nextsize = remainder->bk_nextsize = NULL;
                    set_head(victim, nb | PREV_INUSE |
                             (av != &main_arena ? NON_MAIN_ARENA : 0));
                    set_head(remainder, remainder_size | PREV_INUSE);
                    set_foot(remainder, remainder_size);
                }
                return chunk2mem(victim);
            }
        }

        /* --- top chunk --- */
        if (av->top) {
            size_t top_size = chunksize(av->top);
            if (top_size >= nb + MINSIZE) {
                mchunkptr v = av->top;
                av->top = chunk_at_offset(v, nb);
                set_head(av->top, (top_size - nb) | PREV_INUSE);
                set_head(v, nb | PREV_INUSE |
                         (av != &main_arena ? NON_MAIN_ARENA : 0));
                note_top_carve(av);
                return chunk2mem(v);
            }
        }

        /* --- deferred fast chunks may coalesce into something usable --- */
        if (av->have_fastchunks) {
            malloc_consolidate(av);
            if (in_smallbin_range(nb))
                idx = smallbin_index(nb);
            else
                idx = largebin_index(nb);
            continue;
        }

        /* --- ask the OS --- */
        return sysmalloc(av, nb);
    }
}

/* ------------------------------------------------------------------ */
/* Core deallocation                                                  */
/* ------------------------------------------------------------------ */

static void _int_free(struct malloc_state *av, mchunkptr p, int have_lock)
{
    size_t size = chunksize(p);

    if ((uintptr_t)p > (uintptr_t)-size || misaligned_mem(chunk2mem(p)))
        malloc_printerr("free(): invalid pointer", p);
    if (size < MINSIZE || (size & MALLOC_ALIGN_MASK))
        malloc_printerr("free(): invalid size", p);

    /* --- fast path for small chunks --- */
    if (size <= global_max_fast) {
        if (!have_lock)
            mlock_lock(&av->mutex);

        size_t nextsz = chunksize(chunk_at_offset(p, size));
        if (nextsz <= CHUNK_HDR_SZ || nextsz > av->system_mem)
            malloc_printerr("free(): invalid next size (fast)", p);

        unsigned idx = fastbin_index(size);
        if (av->fastbinsY[idx] == p)
            malloc_printerr("double free or corruption (fasttop)", p);

        free_perturb(chunk2mem(p), size - CHUNK_HDR_SZ);
        p->fd = protect_ptr(&p->fd, av->fastbinsY[idx]);
        av->fastbinsY[idx] = p;
        av->have_fastchunks = 1;
        if (!have_lock)
            mlock_unlock(&av->mutex);
        return;
    }

    /* --- normal path: coalesce and park in the unsorted bin --- */
    if (!have_lock)
        mlock_lock(&av->mutex);

    mchunkptr nextchunk = chunk_at_offset(p, size);
    size_t nextsize = chunksize(nextchunk);

    if (p == av->top)
        malloc_printerr("double free or corruption (top)", p);
    if (av == &main_arena && !(av->flags & ARENA_NONCONTIGUOUS) && av->top &&
        (char *)nextchunk > (char *)av->top)
        malloc_printerr("double free or corruption (out)", p);
    if (!inuse_bit_at_offset(p, size))
        malloc_printerr("double free or corruption (!prev)", p);
    if (nextsize <= CHUNK_HDR_SZ || nextsize > av->system_mem)
        malloc_printerr("free(): invalid next size (normal)", p);

    free_perturb(chunk2mem(p), size - CHUNK_HDR_SZ);

    /* coalesce backward */
    if (!prev_inuse(p)) {
        size_t prevsize = p->mchunk_prev_size;
        mchunkptr prv = chunk_at_offset(p, -(ptrdiff_t)prevsize);
        if (chunksize(prv) != prevsize)
            malloc_printerr("corrupted size vs. prev_size while consolidating", p);
        unlink_chunk(av, prv);
        size += prevsize;
        p = prv;
    }

    if (nextchunk != av->top) {
        /* coalesce forward */
        if (!inuse_bit_at_offset(nextchunk, nextsize)) {
            unlink_chunk(av, nextchunk);
            size += nextsize;
        } else {
            clear_inuse_bit_at_offset(nextchunk, 0);
        }

        mbinptr unsorted = unsorted_chunks(av);
        mchunkptr fwd = unsorted->fd;
        if (fwd->bk != unsorted)
            malloc_printerr("free(): corrupted unsorted chunks", fwd);
        p->fd = fwd;
        p->bk = unsorted;
        unsorted->fd = p;
        fwd->bk = p;
        if (!in_smallbin_range(size))
            p->fd_nextsize = p->bk_nextsize = NULL;
        set_head(p, size | PREV_INUSE);
        set_foot(p, size);
    } else {
        /* merge into top */
        size += nextsize;
        set_head(p, size | PREV_INUSE);
        av->top = p;
    }

    /* Big frees: consolidate deferred fast chunks and consider trimming. */
    if (size >= FASTBIN_CONSOLIDATION_THRESHOLD) {
        if (av->have_fastchunks)
            malloc_consolidate(av);
        if (av->top && chunksize(av->top) >= mp_.trim_threshold) {
            if (av == &main_arena)
                systrim(av, mp_.top_pad);
            else
                heap_release_top(av, mp_.top_pad, mp_.trim_threshold);
        }
    }

    if (!have_lock)
        mlock_unlock(&av->mutex);
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                */
/* ------------------------------------------------------------------ */

void *malloc(size_t bytes)
{
    if (!malloc_initialized)
        malloc_init_once();

    size_t nb = checked_request2size(bytes);
    if (!nb) {
        errno = ENOMEM;
        return NULL;
    }

    /* thread cache first: no locks at all */
    size_t tc_idx = csize2tidx(nb);
    tcache_perthread_struct *tc = tcache_current();
    if (tc && tc_idx < TCACHE_MAX_BINS && tc->counts[tc_idx] > 0) {
        void *mem = tcache_get(tc, tc_idx);
        if (mem) {
            alloc_perturb(mem, bytes);
            return mem;
        }
    }
    if (!tc && __pthread_self()->malloc_tcache == NULL)
        tcache_maybe_init();

    struct malloc_state *av = arena_get();
    void *mem = _int_malloc(av, nb);
    if (!mem && av != &main_arena) {
        /* retry on the main arena before giving up */
        mlock_unlock(&av->mutex);
        av = &main_arena;
        mlock_lock(&av->mutex);
        mem = _int_malloc(av, nb);
    }
    mlock_unlock(&av->mutex);

    if (!mem) {
        errno = ENOMEM;
        return NULL;
    }
    alloc_perturb(mem, bytes);
    return mem;
}

void free(void *mem)
{
    if (!mem)
        return;

    if (!malloc_initialized)
        malloc_printerr("free(): invalid pointer (uninitialized)", mem);
    if (misaligned_mem(mem))
        malloc_printerr("free(): invalid pointer", mem);

    mchunkptr p = mem2chunk(mem);
    size_t size = chunksize(p);

    if ((uintptr_t)p > (uintptr_t)-size)
        malloc_printerr("free(): invalid pointer", p);

    if (chunk_is_mmapped(p)) {
        /* Let the direct-map threshold follow the workload so blocks this
         * size are served from the heap next time. */
        if (!mp_.no_dyn_threshold &&
            size > mp_.mmap_threshold && size <= MMAP_THRESHOLD_MAX) {
            mp_.mmap_threshold = size;
            mp_.trim_threshold = 2 * size;
        }
        munmap_chunk(p);
        return;
    }

    if (size < MINSIZE || (size & MALLOC_ALIGN_MASK))
        malloc_printerr("free(): invalid size", p);

    /* thread cache */
    size_t tc_idx = csize2tidx(size);
    tcache_perthread_struct *tc = tcache_current();
    if (tc && tc_idx < TCACHE_MAX_BINS) {
        tcache_entry *e = (tcache_entry *)mem;
        if (e->key == tcache_entry_key) {
            /* Very likely a double free: scan the (short) bin to be sure. */
            tcache_entry *tmp = tc->entries[tc_idx];
            int walked = 0;
            while (tmp) {
                if (tmp == e)
                    malloc_printerr("free(): double free detected in tcache 2", p);
                if (misaligned_mem(tmp) || ++walked > TCACHE_COUNT)
                    break;
                tmp = REVEAL_PTR(&tmp->next, tmp->next);
            }
        }
        if (tc->counts[tc_idx] < TCACHE_COUNT) {
            free_perturb(mem, size - CHUNK_HDR_SZ);
            tcache_put(tc, p, tc_idx);
            return;
        }
    }

    _int_free(arena_for_chunk(p), p, 0);
}

void *calloc(size_t nmemb, size_t size)
{
    if (size != 0 && nmemb > (size_t)-1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t bytes = nmemb * size;

    void *mem = malloc(bytes);
    if (!mem)
        return NULL;

    mchunkptr p = mem2chunk(mem);
    if (chunk_is_mmapped(p)) {
        /* Fresh mappings are demand-zeroed by the kernel - unless the
         * perturb option already scribbled on them. */
        if (mp_.perturb_byte)
            memset(mem, 0, bytes);
        return mem;
    }
    return memset(mem, 0, bytes);
}

void *realloc(void *oldmem, size_t bytes)
{
    if (!oldmem)
        return malloc(bytes);
    if (bytes == 0) {
        free(oldmem);
        return NULL;
    }

    if (!malloc_initialized || misaligned_mem(oldmem))
        malloc_printerr("realloc(): invalid pointer", oldmem);

    mchunkptr p = mem2chunk(oldmem);
    size_t oldsize = chunksize(p);

    if ((uintptr_t)p > (uintptr_t)-oldsize)
        malloc_printerr("realloc(): invalid pointer", p);

    size_t nb = checked_request2size(bytes);
    if (!nb) {
        errno = ENOMEM;
        return NULL;
    }

    if (chunk_is_mmapped(p)) {
        size_t newsize = page_align_up(nb + SIZE_SZ);
        if (oldsize >= newsize) {
            /* Give the tail pages back when the shrink is worthwhile. */
            if (p->mchunk_prev_size == 0 && oldsize - newsize >= 2 * PAGE_SIZE) {
                munmap((char *)p + newsize, oldsize - newsize);
                set_head(p, newsize | IS_MMAPPED);
                __sync_fetch_and_sub(&mp_.mmapped_mem, (long)(oldsize - newsize));
            }
            return oldmem;
        }
        void *newmem = malloc(bytes);
        if (!newmem)
            return NULL;
        size_t copy = oldsize - CHUNK_HDR_SZ;
        if (copy > bytes)
            copy = bytes;
        memcpy(newmem, oldmem, copy);
        munmap_chunk(p);
        return newmem;
    }

    if (oldsize < MINSIZE || (oldsize & MALLOC_ALIGN_MASK))
        malloc_printerr("realloc(): invalid old size", p);

    struct malloc_state *av = arena_for_chunk(p);
    mlock_lock(&av->mutex);

    void *result = NULL;
    mchunkptr next = chunk_at_offset(p, oldsize);
    size_t nextsize = chunksize(next);

    if (nextsize <= CHUNK_HDR_SZ || nextsize > av->system_mem)
        malloc_printerr("realloc(): invalid next size", p);

    if (oldsize >= nb) {
        /* shrink in place; split off the tail when big enough */
        size_t rem = oldsize - nb;
        if (rem >= MINSIZE) {
            mchunkptr remainder = chunk_at_offset(p, nb);
            set_head_size(p, nb);
            set_head(remainder, rem | PREV_INUSE |
                     (av != &main_arena ? NON_MAIN_ARENA : 0));
            _int_free(av, remainder, 1);
        }
        result = oldmem;
    } else if (next == av->top && oldsize + nextsize >= nb + MINSIZE) {
        /* grow into the top chunk */
        set_head_size(p, nb);
        av->top = chunk_at_offset(p, nb);
        set_head(av->top, (oldsize + nextsize - nb) | PREV_INUSE);
        note_top_carve(av);
        result = oldmem;
    } else if (next != av->top && !inuse_bit_at_offset(next, nextsize) &&
               oldsize + nextsize >= nb) {
        /* grow into the adjacent free chunk */
        unlink_chunk(av, next);
        size_t newsize = oldsize + nextsize;
        size_t rem = newsize - nb;
        if (rem >= MINSIZE) {
            mchunkptr remainder = chunk_at_offset(p, nb);
            set_head_size(p, nb);
            set_head(remainder, rem | PREV_INUSE |
                     (av != &main_arena ? NON_MAIN_ARENA : 0));
            set_inuse_bit_at_offset(remainder, rem);
            _int_free(av, remainder, 1);
        } else {
            set_head_size(p, newsize);
            set_inuse_bit_at_offset(p, newsize);
        }
        result = oldmem;
    } else {
        /* move */
        void *newmem = _int_malloc(av, nb);
        if (newmem) {
            size_t copy = oldsize - SIZE_SZ;
            if (copy > bytes)
                copy = bytes;
            memcpy(newmem, oldmem, copy);
            _int_free(av, p, 1);
            result = newmem;
        }
    }

    mlock_unlock(&av->mutex);
    if (!result)
        errno = ENOMEM;
    return result;
}

/* ------------------------------------------------------------------ */
/* Aligned allocation                                                 */
/* ------------------------------------------------------------------ */

static void *internal_memalign(size_t alignment, size_t bytes)
{
    if (alignment <= MALLOC_ALIGNMENT)
        return malloc(bytes);
    if (alignment < MINSIZE)
        alignment = MINSIZE;

    size_t nb = checked_request2size(bytes);
    if (!nb || alignment > ((size_t)1 << 62)) {
        errno = ENOMEM;
        return NULL;
    }

    void *mem = malloc(nb + alignment + MINSIZE);
    if (!mem)
        return NULL;

    mchunkptr p = mem2chunk(mem);

    if (((uintptr_t)mem & (alignment - 1)) != 0) {
        /* Carve an aligned chunk out of the middle and give the leader back */
        char *br = (char *)(((uintptr_t)mem + alignment - 1) & ~(alignment - 1));
        mchunkptr newp = mem2chunk(br);
        /* the leader must be a valid chunk of its own */
        if ((size_t)((char *)newp - (char *)p) < MINSIZE)
            newp = (mchunkptr)((char *)newp + alignment);
        size_t leadsize = (size_t)((char *)newp - (char *)p);
        size_t newsize = chunksize(p) - leadsize;

        if (chunk_is_mmapped(p)) {
            newp->mchunk_prev_size = p->mchunk_prev_size + leadsize;
            set_head(newp, newsize | IS_MMAPPED);
            return chunk2mem(newp);
        }

        struct malloc_state *av = arena_for_chunk(p);
        mlock_lock(&av->mutex);
        set_head(newp, newsize | PREV_INUSE |
                 (av != &main_arena ? NON_MAIN_ARENA : 0));
        set_inuse_bit_at_offset(newp, 0);
        set_head_size(p, leadsize);
        _int_free(av, p, 1);
        mlock_unlock(&av->mutex);
        p = newp;
    }

    /* Trim the tail of an arena chunk */
    if (!chunk_is_mmapped(p)) {
        size_t size = chunksize(p);
        if (size > nb + MINSIZE) {
            struct malloc_state *av = arena_for_chunk(p);
            mlock_lock(&av->mutex);
            size_t remainder_size = size - nb;
            mchunkptr remainder = chunk_at_offset(p, nb);
            set_head(remainder, remainder_size | PREV_INUSE |
                     (av != &main_arena ? NON_MAIN_ARENA : 0));
            set_head_size(p, nb);
            _int_free(av, remainder, 1);
            mlock_unlock(&av->mutex);
        }
    }

    return chunk2mem(p);
}

void *memalign(size_t alignment, size_t size)
{
    if (alignment == 0)
        alignment = MALLOC_ALIGNMENT;
    /* legacy interface: round odd alignments up to a power of two */
    if (alignment & (alignment - 1)) {
        size_t a = MALLOC_ALIGNMENT;
        while (a < alignment)
            a <<= 1;
        alignment = a;
    }
    return internal_memalign(alignment, size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (alignment % sizeof(void *) != 0 ||
        (alignment & (alignment - 1)) != 0 ||
        alignment == 0)
        return EINVAL;

    void *mem = internal_memalign(alignment, size);
    if (!mem)
        return ENOMEM;
    *memptr = mem;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    return internal_memalign(alignment, size);
}

void *valloc(size_t size)
{
    return internal_memalign(PAGE_SIZE, size);
}

void *pvalloc(size_t size)
{
    return internal_memalign(PAGE_SIZE, page_align_up(size ? size : 1));
}

/* ------------------------------------------------------------------ */
/* Introspection                                                      */
/* ------------------------------------------------------------------ */

size_t malloc_usable_size(void *mem)
{
    if (!mem || misaligned_mem(mem))
        return 0;

    mchunkptr p = mem2chunk(mem);
    if (chunk_is_mmapped(p))
        return chunksize(p) - CHUNK_HDR_SZ;
    if (inuse_bit_at_offset(p, chunksize(p)))
        return chunksize(p) - SIZE_SZ;

#if defined(DEBUG) && DEBUG
    malloc_printerr("malloc_usable_size(): pointer is not allocated", p);
#endif
    return 0;
}

int malloc_trim(size_t pad)
{
    if (!malloc_initialized)
        malloc_init_once();

    int released = 0;
    struct malloc_state *av = &main_arena;
    do {
        mlock_lock(&av->mutex);
        if (av->have_fastchunks)
            malloc_consolidate(av);
        if (av == &main_arena)
            released |= systrim(av, pad);
        else
            released |= heap_release_top(av, pad, PAGE_SIZE);
        mlock_unlock(&av->mutex);
        av = av->next;
    } while (av != &main_arena);

    return released;
}

struct mallinfo2 mallinfo2(void)
{
    struct mallinfo2 m;
    memset(&m, 0, sizeof(m));

    if (!malloc_initialized)
        malloc_init_once();

    struct malloc_state *av = &main_arena;
    do {
        mlock_lock(&av->mutex);

        size_t avail = 0;
        size_t nblocks = 0;
        size_t nfast = 0, fastavail = 0;

        if (av->top) {
            avail += chunksize(av->top);
            nblocks++;
        }

        for (int i = 0; i < NFASTBINS; i++) {
            mchunkptr p = av->fastbinsY[i];
            int walked = 0;
            while (p && !misaligned_mem(chunk2mem(p)) && ++walked < MAX_LIST_WALK) {
                nfast++;
                fastavail += chunksize(p);
                p = REVEAL_PTR(&p->fd, p->fd);
            }
        }

        for (int i = 1; i < NBINS; i++) {
            mbinptr bin = bin_at(av, i);
            int walked = 0;
            for (mchunkptr q = bin->fd; q != bin && ++walked < MAX_LIST_WALK; q = q->fd) {
                nblocks++;
                avail += chunksize(q);
            }
        }

        avail += fastavail;
        m.arena += av->system_mem;
        m.ordblks += nblocks;
        m.smblks += nfast;
        m.fsmblks += fastavail;
        m.fordblks += avail;
        m.uordblks += av->system_mem - avail;
        if (av == &main_arena && av->top)
            m.keepcost = chunksize(av->top);

        mlock_unlock(&av->mutex);
        av = av->next;
    } while (av != &main_arena);

    m.hblks = (size_t)mp_.n_mmaps;
    m.hblkhd = (size_t)mp_.mmapped_mem;
    m.usmblks = 0;
    return m;
}

struct mallinfo mallinfo(void)
{
    struct mallinfo2 m2 = mallinfo2();
    struct mallinfo m;
    m.arena = (int)m2.arena;
    m.ordblks = (int)m2.ordblks;
    m.smblks = (int)m2.smblks;
    m.hblks = (int)m2.hblks;
    m.hblkhd = (int)m2.hblkhd;
    m.usmblks = (int)m2.usmblks;
    m.fsmblks = (int)m2.fsmblks;
    m.uordblks = (int)m2.uordblks;
    m.fordblks = (int)m2.fordblks;
    m.keepcost = (int)m2.keepcost;
    return m;
}

/* ------------------------------------------------------------------ */
/* Fork integration                                                   */
/* ------------------------------------------------------------------ */

/* Called by fork() around the clone so the child never inherits an
 * allocator lock held by a thread that does not exist in the child. */
void __malloc_fork_prepare(void)
{
    if (!malloc_initialized)
        return;
    mlock_lock(&arena_list_lock);
    struct malloc_state *av = &main_arena;
    do {
        mlock_lock(&av->mutex);
        av = av->next;
    } while (av != &main_arena);
}

void __malloc_fork_parent(void)
{
    if (!malloc_initialized)
        return;
    struct malloc_state *av = &main_arena;
    do {
        mlock_unlock(&av->mutex);
        av = av->next;
    } while (av != &main_arena);
    mlock_unlock(&arena_list_lock);
}

void __malloc_fork_child(void)
{
    if (!malloc_initialized)
        return;
    struct malloc_state *av = &main_arena;
    do {
        mlock_reset(&av->mutex);
        av = av->next;
    } while (av != &main_arena);
    mlock_reset(&arena_list_lock);
}
