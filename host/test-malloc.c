/*
 * Stress test of libc's allocator, on the build machine.
 *
 * The question it answers is a single one, and it is the one that matters:
 * CAN THE ALLOCATOR EVER HAND OUT MEMORY THAT IS ALREADY IN USE?
 *
 * Every other kind of allocator bug announces itself.  That one does not: the
 * two owners simply write over each other, and the damage surfaces somewhere
 * else entirely, as a corrupted heap chunk header, a freed object's poison
 * turning up inside a live structure, or a jump through a GOT slot that used to
 * be correct.  All three were seen on the target, which is why this exists.
 *
 * The test keeps every live allocation in a table, and after each malloc checks
 * the new block against all of them for overlap.  It also fills each block with
 * a pattern derived from its own address and re-checks that pattern before
 * freeing, so an overlap that the table check somehow misses is still caught by
 * the bytes.  Both checks run from several threads at once, because the
 * per-thread cache is the part of this allocator that takes no lock, and a
 * single-threaded run would never touch the interesting paths.
 *
 * The allocator is compiled AS IT SHIPS.  Only two things underneath it are
 * replaced, both because the host's own libc is using them for its own malloc:
 *
 *   - the thread control block, which the real one finds at %fs:0 -- on the
 *     host that address belongs to glibc, and writing our fields over it would
 *     destroy the C library running the test;
 *   - sbrk, which glibc's malloc is also growing; ours gets a private region so
 *     the two cannot interleave and the run is deterministic.
 *
 * Build and run:  ./host/test-malloc.sh
 */

#define _GNU_SOURCE
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- stand-in thread control block --------------------------------------
 *
 * Claiming the real header's include guard keeps it out, so that the fields
 * the allocator uses can be provided here without the %fs:0 accessor coming
 * with them.  Only the members malloc.c actually touches need to exist.
 */
#define _PTHREAD_INTERNAL_H

struct __pthread {
	struct __pthread *self;
	void *malloc_tcache;
	void *malloc_arena;
};

static __thread struct __pthread host_tcb;

static inline struct __pthread *__pthread_self(void)
{
	return &host_tcb;
}

/* ---- stand-in sbrk ------------------------------------------------------
 *
 * A private region handed out in order.  Contiguous, like a real brk, so the
 * allocator's top-extension path is exercised rather than bypassed.
 */
#define TEST_BRK_SIZE (256UL * 1024 * 1024)
static char *brk_base, *brk_cur, *brk_end;
static pthread_mutex_t brk_lock = PTHREAD_MUTEX_INITIALIZER;

static void *test_sbrk(intptr_t incr)
{
	pthread_mutex_lock(&brk_lock);
	if (!brk_base) {
		brk_base = mmap(NULL, TEST_BRK_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (brk_base == MAP_FAILED) {
			pthread_mutex_unlock(&brk_lock);
			return (void *)-1;
		}
		brk_cur = brk_base;
		brk_end = brk_base + TEST_BRK_SIZE;
	}
	char *old = brk_cur;
	if (incr > 0 && brk_cur + incr > brk_end) {
		pthread_mutex_unlock(&brk_lock);
		return (void *)-1;
	}
	if (incr < 0 && brk_cur + incr < brk_base) {
		pthread_mutex_unlock(&brk_lock);
		return (void *)-1;
	}
	brk_cur += incr;
	pthread_mutex_unlock(&brk_lock);
	return old;
}

#define sbrk test_sbrk

/* ---- stand-in munmap ----------------------------------------------------
 *
 * heap_release_top() hands back the pages of a non-main heap that were used
 * and then freed, by unmapping a span in the MIDDLE of the heap.  On the
 * target that drops the physical pages and leaves the address range valid --
 * the next touch faults a zero page back in -- and the allocator is written to
 * exactly that: it carves the released span out of the top chunk again without
 * ever remapping it.
 *
 * The host's munmap really removes the range, so the next carve from top
 * writes a chunk header into a hole and the thread dies with SIGSEGV in
 * _int_malloc.  That is this harness diverging from the target, not an
 * allocator bug, but until it is modelled the trimming path cannot be tested
 * at all -- and the crash only appears once enough threads run for a secondary
 * arena to grow, trim, and grow again, so a short run looks clean.
 *
 * MADV_DONTNEED on a private anonymous mapping is precisely the target's
 * behaviour, so an interior release becomes that.  Every other unmap in the
 * allocator -- the head and tail trims that align a fresh heap, and the
 * release of a directly-mmapped chunk -- gives back a whole edge of a mapping
 * and must stay a real munmap, or nothing would ever return address space.
 *
 * The two are told apart by remembering where the heaps are, which is why mmap
 * is shimmed as well: an unmap that starts inside a known heap is an interior
 * release and nothing else can be, since a heap owns its whole aligned block.
 * Deciding it instead by inspecting the address -- is this block mapped end to
 * end, does its base hold a heap header -- reads memory that another thread is
 * free to unmap in the same instant, and does, which merely moves the crash
 * into the shim.
 */
static void *test_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
static int test_munmap(void *addr, size_t len);
#define mmap test_mmap
#define munmap test_munmap

/* ---- the futex primitives the arena lock is built on --------------------
 *
 * These are libc's own, defined in the pthread sources; here they go straight
 * to the host kernel's futex.  The LOCK ITSELF is the allocator's, unchanged --
 * only the sleep underneath it is the host's, which is the point: an arena
 * lock that lets two threads in is exactly the failure being looked for, and
 * substituting a pthread mutex would hide it.
 */
#include <sys/syscall.h>

/* The numeric op values rather than <linux/futex.h>: that header redefines
 * struct robust_list, which libc's own <pthread.h> has already declared. */
#define TEST_FUTEX_WAIT_PRIVATE 128 /* FUTEX_WAIT | FUTEX_PRIVATE_FLAG */
#define TEST_FUTEX_WAKE_PRIVATE 129 /* FUTEX_WAKE | FUTEX_PRIVATE_FLAG */

int futex_wait(volatile int *uaddr, int val, const struct timespec *to);
int futex_wake(volatile int *uaddr, int count);

int futex_wait(volatile int *uaddr, int val, const struct timespec *to)
{
	return (int)syscall(SYS_futex, uaddr, TEST_FUTEX_WAIT_PRIVATE, val, to, NULL,
			    0);
}

int futex_wake(volatile int *uaddr, int count)
{
	return (int)syscall(SYS_futex, uaddr, TEST_FUTEX_WAKE_PRIVATE, count, NULL,
			    NULL, 0);
}

/* libc's CPU-set population count, used to size the arena limit.  Lives in the
 * sched sources on the target; the host spells the same thing CPU_COUNT. */
int __cpu_count(const cpu_set_t *set);

int __cpu_count(const cpu_set_t *set)
{
	return CPU_COUNT(set);
}

/* The allocator, exactly as it ships.  host/test-malloc.sh renames the symbols
 * it defines so it can sit next to the host's own malloc. */
#include "../user/lib/libc/src/malloc/malloc.c"

#undef sbrk
#undef mmap
#undef munmap

/* Where the heaps are.  new_heap() is the one caller that asks for twice the
 * heap size -- it over-maps and trims back to an aligned block -- and heaps are
 * never given back once created, so the list only grows. */
#define MAX_HEAPS 1024
static uintptr_t heaps[MAX_HEAPS];
static int nheaps;
static pthread_mutex_t heaps_lock = PTHREAD_MUTEX_INITIALIZER;

static void *test_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	void *p = mmap(addr, len, prot, flags, fd, off);

	if (p != MAP_FAILED && len == 2 * HEAP_MAX_SIZE) {
		pthread_mutex_lock(&heaps_lock);
		if (nheaps == MAX_HEAPS) {
			/* Silence here would look like an allocator crash: the
			 * unregistered heap's next trim would really unmap. */
			fprintf(stderr, "test harness: more than %d heaps\n",
				MAX_HEAPS);
			abort();
		}
		heaps[nheaps++] = ((uintptr_t)p + HEAP_MAX_SIZE - 1) &
				  ~(HEAP_MAX_SIZE - 1);
		pthread_mutex_unlock(&heaps_lock);
	}
	return p;
}

static int is_heap(uintptr_t base)
{
	int found = 0;

	pthread_mutex_lock(&heaps_lock);
	for (int i = 0; i < nheaps; i++)
		if (heaps[i] == base) {
			found = 1;
			break;
		}
	pthread_mutex_unlock(&heaps_lock);
	return found;
}

static int test_munmap(void *addr, size_t len)
{
	uintptr_t base = (uintptr_t)addr & ~(HEAP_MAX_SIZE - 1);

	/* Strictly inside: the tail trim in new_heap() starts exactly on the
	 * boundary above a heap, and that one is a real unmap. */
	if ((uintptr_t)addr > base && is_heap(base))
		return madvise(addr, len, MADV_DONTNEED);
	return munmap(addr, len);
}

/* The allocator under its renamed names.  Declared explicitly: the renaming
 * happens after compilation, so nothing else declares these, and an implicit
 * declaration would return int and truncate every pointer to 32 bits. */
void *lk_malloc(size_t n);
void lk_free(void *p);
void *lk_realloc(void *p, size_t n);
void *lk_calloc(size_t n, size_t m);
int lk_posix_memalign(void **out, size_t align, size_t n);

/* ---- crash reporting ----------------------------------------------------
 *
 * The interesting failures here are intermittent and disappear under a
 * debugger, whose scheduling is not the scheduling that produces them.  So the
 * test reports its own: a fault handler that prints the backtrace of the thread
 * that died, from the process as it actually ran.
 */
static void fault_handler(int sig)
{
	void *frames[32];
	int n = backtrace(frames, 32);
	const char *name = sig == SIGSEGV ? "SIGSEGV" :
			   sig == SIGBUS  ? "SIGBUS" :
					    "SIGABRT";
	fprintf(stderr, "\n=== %s ===\n", name);
	fflush(stderr);
	backtrace_symbols_fd(frames, n, 2);
	_exit(2);
}

static void install_fault_handler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fault_handler;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
}

/* ---- scoreboard --------------------------------------------------------- */

static volatile int g_fail;
static volatile long g_allocs;

static void report(const char *what, const void *a, const void *b)
{
	__sync_fetch_and_add(&g_fail, 1);
	fprintf(stderr, "  FAIL %s (%p, %p)\n", what, a, b);
}

/* ---- live-block table ---------------------------------------------------
 *
 * Shared across threads and guarded by its own lock, so that an overlap
 * BETWEEN threads is caught and not just one within a thread -- which is the
 * case that matters, since the per-thread cache is what goes unlocked.
 */
#define MAX_LIVE 4096

struct live {
	unsigned char *p;
	size_t n;
	unsigned char seed;
};

static struct live live[MAX_LIVE];
static int nlive;
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned char seed_of(const void *p)
{
	uintptr_t v = (uintptr_t)p;
	return (unsigned char)((v >> 4) ^ (v >> 12) ^ 0x5a);
}

static void fill(unsigned char *p, size_t n, unsigned char seed)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (unsigned char)(seed + (unsigned char)i);
}

static int verify(const unsigned char *p, size_t n, unsigned char seed)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != (unsigned char)(seed + (unsigned char)i))
			return 0;
	return 1;
}

/* Record a new block, checking it against every block already live.
 *
 * The caller must have filled the block BEFORE calling this.  Publishing it
 * first would let another thread take it out of the table and free it while the
 * fill was still running, and the resulting write into freed memory is the
 * test's own bug rather than the allocator's. */
static void live_add(unsigned char *p, size_t n)
{
	pthread_mutex_lock(&live_lock);
	for (int i = 0; i < nlive; i++) {
		unsigned char *q = live[i].p;
		if (p < q + live[i].n && q < p + n) {
			report("allocation OVERLAPS a live block", p, q);
			break;
		}
	}
	if (nlive < MAX_LIVE) {
		live[nlive].p = p;
		live[nlive].n = n;
		live[nlive].seed = seed_of(p);
		nlive++;
	}
	pthread_mutex_unlock(&live_lock);
}

/* Take a block out at random and hand it back for freeing. */
static int live_take(struct live *out, unsigned rnd)
{
	int got = 0;
	pthread_mutex_lock(&live_lock);
	if (nlive > 0) {
		int i = (int)(rnd % (unsigned)nlive);
		*out = live[i];
		live[i] = live[nlive - 1];
		nlive--;
		got = 1;
	}
	pthread_mutex_unlock(&live_lock);
	return got;
}

static unsigned xorshift(unsigned *s)
{
	unsigned x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

/* A spread of sizes that reaches every path: tcache bins, fastbins, smallbins,
 * largebins and the direct-mmap threshold. */
static size_t pick_size(unsigned r)
{
	switch (r % 8) {
	case 0:
	case 1:
	case 2:
		return 1 + (r >> 3) % 120; /* tcache range */
	case 3:
	case 4:
		return 128 + (r >> 3) % 900; /* small bins */
	case 5:
		return 1024 + (r >> 3) % 60000; /* large bins */
	case 6:
		return 1 + (r >> 3) % 16; /* tiny */
	default:
		return 132 * 1024 + (r >> 3) % 65536; /* mmap threshold */
	}
}

/* Iterations per worker.  Settable because this test runs on the developer's
 * own desktop: the default is a size that finishes in a few seconds, and a
 * long hunt is asked for explicitly rather than being the thing that happens
 * by accident.  See host/test-malloc.sh, which also caps cores and memory. */
static int g_iters = 8000;

static void *worker(void *arg)
{
	unsigned s = (unsigned)(uintptr_t)arg * 2654435761u + 1;

	for (int iter = 0; iter < g_iters; iter++) {
		unsigned r = xorshift(&s);

		if ((r & 3) == 3) {
			/* free */
			struct live L;
			if (live_take(&L, xorshift(&s))) {
				if (!verify(L.p, L.n, L.seed))
					report("block was MODIFIED while live",
					       L.p, NULL);
				lk_free(L.p);
			}
			continue;
		}

		if ((r & 7) == 1) {
			/* realloc: the contents up to the smaller of the two
			 * sizes must survive, which is the whole contract. */
			struct live L;
			if (live_take(&L, xorshift(&s))) {
				if (!verify(L.p, L.n, L.seed))
					report("block MODIFIED before realloc",
					       L.p, NULL);
				size_t n2 = pick_size(xorshift(&s));
				unsigned char *q = (unsigned char *)lk_realloc(L.p, n2);
				if (!q) {
					lk_free(L.p);
					continue;
				}
				size_t keep = L.n < n2 ? L.n : n2;
				if (!verify(q, keep, L.seed))
					report("realloc LOST the contents", q,
					       NULL);
				/* The block is out of the table, so it is ours
				 * alone until it goes back in. */
				fill(q, n2, seed_of(q));
				live_add(q, n2);
			}
			continue;
		}

		/* allocate */
		size_t n = pick_size(r);
		unsigned char *p;
		if ((r & 15) == 5) {
			p = (unsigned char *)lk_calloc(1, n);
			if (p) {
				for (size_t i = 0; i < n; i++)
					if (p[i] != 0) {
						report("calloc returned NON-ZERO memory",
						       p, NULL);
						break;
					}
			}
		} else if ((r & 15) == 9) {
			p = NULL;
			if (lk_posix_memalign((void **)&p, 64, n) != 0)
				p = NULL;
			else if (((uintptr_t)p & 63) != 0)
				report("posix_memalign returned a MISALIGNED block",
				       p, NULL);
		} else {
			p = (unsigned char *)lk_malloc(n);
		}
		if (!p)
			continue;

		if (((uintptr_t)p & 0xf) != 0)
			report("allocation is not 16-byte aligned", p, NULL);

		__sync_fetch_and_add(&g_allocs, 1);
		fill(p, n, seed_of(p));
		live_add(p, n);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int nthreads = argc > 1 ? atoi(argv[1]) : 2;
	if (nthreads < 1)
		nthreads = 1;
	if (nthreads > 64)
		nthreads = 64;
	if (argc > 2 && atoi(argv[2]) > 0)
		g_iters = atoi(argv[2]);

	install_fault_handler();
	printf("libc allocator stress: %d threads, %d iterations each\n",
	       nthreads, g_iters);
	fflush(stdout);

	pthread_t th[64];
	for (int i = 0; i < nthreads; i++)
		pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)(i + 1));
	for (int i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);

	/* Everything still live must still hold its pattern. */
	for (int i = 0; i < nlive; i++)
		if (!verify(live[i].p, live[i].n, live[i].seed))
			report("block was MODIFIED (final sweep)", live[i].p,
			       NULL);

	printf("%ld allocations, %d failure%s\n", g_allocs, g_fail,
	       g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
