/*
 * malloc — segregated size-class allocator.
 *
 * Design:
 *  - Every allocation carries a 16-byte header {payload size, tag}; user
 *    pointers stay 16-byte aligned.
 *  - Small sizes (<= 2048) round up to a power-of-two class with a per-class
 *    singly-linked free list: O(1) malloc and free, no walking, ever.
 *    Class storage is carved from 64 KB heap chunks (one sbrk per ~64 KB of
 *    demand instead of one per allocation).
 *  - Large sizes use a dedicated free list kept ADDRESS-SORTED with
 *    coalescing of physically adjacent free blocks; large allocations are
 *    rare, so this list stays short.  Growth is rounded to 4 KB.
 *  - A tiny test-and-set lock guards the allocator (pthreads exist).
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#define MHDR_SIZE 16
#define SMALL_MAX 2048
#define NUM_CLASSES 8 /* 16,32,64,128,256,512,1024,2048 */
#define CHUNK_SIZE (64 * 1024)
#define TAG_SMALL 0x51414C4Cu /* 'SMALL'-ish magic; low bits: class index */
#define TAG_LARGE 0x4C415247u /* 'LARG' */

typedef struct mhdr {
	uint64_t size; /* payload bytes (class size for small blocks) */
	uint64_t tag; /* low 32: TAG_SMALL/TAG_LARGE; high 32: class index */
} mhdr_t;

typedef struct fblk {
	struct fblk *next; /* overlays the free payload */
} fblk_t;

/* Large free block: header stays in place; the payload holds the link. */
typedef struct lblk {
	struct lblk *next; /* address-ordered */
} lblk_t;

static fblk_t *g_class_free[NUM_CLASSES];
static lblk_t *g_large_free; /* address-sorted */

/* Bump arena carved from CHUNK_SIZE sbrk chunks for small classes. */
static uint8_t *g_chunk_ptr;
static size_t g_chunk_left;

static volatile int g_mlock;

static void mlock_acquire(void)
{
	while (__sync_lock_test_and_set(&g_mlock, 1))
		__asm__ volatile("pause");
}

static void mlock_release(void)
{
	__sync_lock_release(&g_mlock);
}

static const uint16_t g_class_size[NUM_CLASSES] = { 16,  32,   64,  128,
						    256, 512, 1024, 2048 };

static int size_to_class(size_t size)
{
	for (int i = 0; i < NUM_CLASSES; i++)
		if (size <= g_class_size[i])
			return i;
	return -1;
}

static void *grow(size_t bytes)
{
	void *p = sbrk((intptr_t)bytes);
	if (p == (void *)-1)
		return NULL;
	/* Keep 16-byte alignment: brk always advances by multiples of 16
	 * here (CHUNK_SIZE and large sizes are 16-aligned), so once aligned
	 * it stays aligned.  Align the very first break if needed. */
	uintptr_t up = (uintptr_t)p;
	if (up & 15) {
		size_t fix = 16 - (up & 15);
		if (sbrk((intptr_t)fix) == (void *)-1)
			return NULL;
		up += fix;
	}
	return (void *)up;
}

/* ---- small classes ---- */

static void *small_alloc(int cls)
{
	fblk_t *f = g_class_free[cls];
	if (f) {
		g_class_free[cls] = f->next;
		return (void *)f; /* payload address */
	}

	size_t need = (size_t)g_class_size[cls] + MHDR_SIZE;
	if (g_chunk_left < need) {
		/* Waste the chunk tail (< largest class + header, bounded);
		 * grab a fresh chunk. */
		void *c = grow(CHUNK_SIZE);
		if (!c)
			return NULL;
		g_chunk_ptr = (uint8_t *)c;
		g_chunk_left = CHUNK_SIZE;
	}

	mhdr_t *h = (mhdr_t *)g_chunk_ptr;
	g_chunk_ptr += need;
	g_chunk_left -= need;
	h->size = g_class_size[cls];
	h->tag = ((uint64_t)cls << 32) | TAG_SMALL;
	return (void *)(h + 1);
}

/* ---- large blocks ---- */

static void large_insert_coalesce(mhdr_t *h)
{
	lblk_t *b = (lblk_t *)(h + 1);
	lblk_t **pp = &g_large_free;
	while (*pp && (uintptr_t)*pp < (uintptr_t)b)
		pp = &(*pp)->next;

	b->next = *pp;
	*pp = b;

	/* Coalesce forward: [b][b->next] physically adjacent? */
	if (b->next) {
		mhdr_t *nh = (mhdr_t *)b->next - 1;
		if ((uint8_t *)(h + 1) + h->size == (uint8_t *)nh) {
			h->size += MHDR_SIZE + nh->size;
			b->next = b->next->next;
		}
	}
	/* Coalesce backward: predecessor adjacent to b? */
	if (pp != &g_large_free) {
		lblk_t *prev =
			(lblk_t *)((uint8_t *)pp -
				   __builtin_offsetof(lblk_t, next));
		mhdr_t *ph = (mhdr_t *)prev - 1;
		if ((uint8_t *)(ph + 1) + ph->size == (uint8_t *)h) {
			ph->size += MHDR_SIZE + h->size;
			prev->next = b->next;
		}
	}
}

static void *large_alloc(size_t size)
{
	size = (size + 15) & ~(size_t)15;

	lblk_t **pp = &g_large_free;
	while (*pp) {
		mhdr_t *h = (mhdr_t *)*pp - 1;
		if (h->size >= size) {
			lblk_t *b = *pp;
			/* Split when the remainder is worth keeping. */
			if (h->size >= size + MHDR_SIZE + 64) {
				mhdr_t *rest =
					(mhdr_t *)((uint8_t *)(h + 1) + size);
				rest->size = h->size - size - MHDR_SIZE;
				rest->tag = TAG_LARGE;
				h->size = size;
				lblk_t *rb = (lblk_t *)(rest + 1);
				rb->next = b->next;
				*pp = rb;
			} else {
				*pp = b->next;
			}
			h->tag = TAG_LARGE;
			return (void *)(h + 1);
		}
		pp = &(*pp)->next;
	}

	size_t need = (size + MHDR_SIZE + 4095) & ~(size_t)4095;
	mhdr_t *h = (mhdr_t *)grow(need);
	if (!h)
		return NULL;
	h->size = size;
	h->tag = TAG_LARGE;
	/* Donate the page-rounding remainder to the free list if usable. */
	size_t rem = need - size - MHDR_SIZE;
	if (rem >= MHDR_SIZE + 64) {
		mhdr_t *rest = (mhdr_t *)((uint8_t *)(h + 1) + size);
		rest->size = rem - MHDR_SIZE;
		rest->tag = TAG_LARGE;
		large_insert_coalesce(rest);
	} else {
		h->size += rem; /* keep the tail with this block */
	}
	return (void *)(h + 1);
}

/* ---- public API ---- */

void *malloc(size_t size)
{
	if (size == 0)
		size = 1;

	void *p;
	mlock_acquire();
	int cls = (size <= SMALL_MAX) ? size_to_class(size) : -1;
	if (cls >= 0)
		p = small_alloc(cls);
	else
		p = large_alloc(size);
	mlock_release();
	if (!p)
		errno = ENOMEM;
	return p;
}

#if DEBUG
/* Emit a diagnostic without touching malloc/stdio (we may be mid-free with
 * a corrupt heap): manual hex formatting straight to fd 2. */
static void free_bad_tag_abort(const void *ptr, uint64_t tag)
{
	static const char hex[] = "0123456789abcdef";
	char buf[64];
	int n = 0;
	const char *msg = "free(): bad tag ptr=0x";
	while (*msg)
		buf[n++] = *msg++;
	for (int i = 60; i >= 0; i -= 4)
		buf[n++] = hex[((uint64_t)(uintptr_t)ptr >> i) & 0xF];
	buf[n++] = ' ';
	buf[n++] = 't';
	buf[n++] = 'a';
	buf[n++] = 'g';
	buf[n++] = '=';
	buf[n++] = '0';
	buf[n++] = 'x';
	for (int i = 60; i >= 0; i -= 4)
		buf[n++] = hex[(tag >> i) & 0xF];
	buf[n++] = '\n'; /* total 62 bytes — fits buf[64] */
	write(2, buf, (size_t)n);
	abort();
}
#endif

void free(void *ptr)
{
	if (!ptr)
		return;
	mhdr_t *h = (mhdr_t *)ptr - 1;

	mlock_acquire();
	if ((uint32_t)h->tag == TAG_SMALL) {
		int cls = (int)(h->tag >> 32);
		if (cls >= 0 && cls < NUM_CLASSES) {
			fblk_t *f = (fblk_t *)ptr;
			f->next = g_class_free[cls];
			g_class_free[cls] = f;
		}
	} else if ((uint32_t)h->tag == TAG_LARGE) {
		large_insert_coalesce(h);
	} else {
		/* Unknown tag: heap corruption or foreign pointer.  Production
		 * builds leak it rather than corrupt a free list; DEBUG builds
		 * report and abort (release the lock first — abort's teardown
		 * must not deadlock on the allocator). */
#if DEBUG
		uint64_t bad_tag = h->tag;
		mlock_release();
		free_bad_tag_abort(ptr, bad_tag);
#endif
	}
	mlock_release();
}

void *calloc(size_t nmemb, size_t size)
{
	/* Overflow-checked total. */
	if (size && nmemb > (size_t)-1 / size) {
		errno = ENOMEM;
		return NULL;
	}
	size_t total = nmemb * size;
	void *ptr = malloc(total);
	if (ptr)
		memset(ptr, 0, total);
	return ptr;
}

void *realloc(void *ptr, size_t size)
{
	if (!ptr)
		return malloc(size);
	if (size == 0) {
		free(ptr);
		return NULL;
	}

	mhdr_t *h = (mhdr_t *)ptr - 1;
	if (h->size >= size)
		return ptr; /* current block already fits (incl. class slack) */

	void *np = malloc(size);
	if (!np)
		return NULL;
	memcpy(np, ptr, h->size);
	free(ptr);
	return np;
}
