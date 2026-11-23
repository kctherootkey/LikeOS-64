/* Kernel services the page cache calls, for the host test of its extensions
 * (host/test-pagecache-ext.sh).  Memory comes from the host allocator (the
 * test runs under AddressSanitizer, which catches double frees and use after
 * free); physical pages are page-aligned host blocks reached through the
 * kernel's own phys_to_virt() arithmetic.  Every kernel message is printed,
 * and warnings are counted so the test can demand exactly the ones it
 * provokes.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/icache.h>

extern void *malloc(unsigned long);
extern void free(void *);
extern void *aligned_alloc(unsigned long, unsigned long);
extern void abort(void);
extern int printf(const char *, ...);
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern char *strstr(const char *, const char *);

uint64_t test_irq_flag = 0x200;
int test_locks_held;
long test_allocs; /* live kalloc blocks */
long test_pages; /* live physical pages */
int test_warnings; /* kernel WARNING lines printed */
vfs_superblock_t *g_root_sb;

void test_fatal(const char *what, const char *name)
{
	printf("FATAL: %s (%s)\n", what, name ? name : "?");
	abort();
}

int kprintf(const char *fmt, ...)
{
	char buf[512];
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	if (strstr(buf, "WARNING"))
		test_warnings++;
	return printf("  kernel: %s", buf);
}

void panic(const char *fmt, ...)
{
	(void)fmt;
	test_fatal("panic", fmt);
	abort();
}

void *kalloc(size_t size)
{
	void *p = malloc(size ? size : 1);

	if (p) {
		memset(p, 0xa5, size); /* not zeroed: catch missing init */
		test_allocs++;
	}
	return p;
}

void *kcalloc(size_t count, size_t size)
{
	void *p = kalloc(count * size);

	if (p)
		memset(p, 0, count * size);
	return p;
}

void kfree(void *ptr)
{
	if (!ptr)
		return;
	test_allocs--;
	free(ptr);
}

uint64_t mm_allocate_physical_page(void)
{
	void *p = aligned_alloc(PAGE_SIZE, PAGE_SIZE);

	if (!p)
		return 0;
	memset(p, 0x5a, PAGE_SIZE); /* stale contents of a reused frame */
	test_pages++;
	return (uint64_t)p - PHYS_MAP_BASE;
}

void mm_free_physical_page(uint64_t physical_address)
{
	if (!physical_address)
		test_fatal("free of physical page 0", "mm_free_physical_page");
	test_pages--;
	free(phys_to_virt(physical_address));
}

uint64_t mm_get_free_pages(void)
{
	return 1UL << 20; /* plenty: reclaim never runs on its own */
}

void mm_memset(void *dest, int val, size_t len)
{
	memset(dest, val, len);
}

void mm_memcpy(void *dest, const void *src, size_t len)
{
	memcpy(dest, src, len);
}

/* The filesystems under test give positional ids (block_at): the chain
 * cache is never consulted. */
unsigned long icache_chain_get(unsigned long start_cluster, unsigned long idx,
			       struct vfs_superblock *sb)
{
	(void)start_cluster;
	(void)idx;
	(void)sb;
	test_fatal("icache_chain_get with block_at present", "icache");
	return 0;
}

int icache_flush_all(void)
{
	return 0;
}

void sched_yield_in_kernel(void)
{
	test_fatal("sched_yield_in_kernel: would wait for ever", "yield");
}
