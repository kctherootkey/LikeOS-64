// memstat - Display memory statistics for LikeOS-64
// Usage: memstat

#include <stdio.h>
#include <stdint.h>

#define SYS_MEMSTATS 300

typedef struct {
	uint64_t total_memory;
	uint64_t free_memory;
	uint64_t used_memory;
	uint64_t total_pages;
	uint64_t free_pages;
	uint64_t used_pages;
	uint64_t heap_allocated;
	uint64_t heap_free;
	uint32_t allocations;
	uint32_t deallocations;
	/* Ownership breakdown (must match kernel memory_stats_t) */
	uint64_t slab_pages;
	uint64_t slab_large_active;
	uint64_t pagecache_pages;
} memory_stats_t;

static long syscall1(long num, long a1)
{
	long ret;
	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(num), "D"(a1)
			 : "rcx", "r11", "memory");
	return ret;
}

int main(void)
{
	memory_stats_t stats = { 0 };
	long ret = syscall1(SYS_MEMSTATS, (long)&stats);
	if (ret < 0) {
		fprintf(stderr, "memstat: failed (%ld)\n", ret);
		return 1;
	}
	printf("=== Memory Statistics ===\n");
	printf("Physical Memory:\n");
	printf("  Total: %llu MB (%llu pages)\n",
	       (unsigned long long)(stats.total_memory / (1024ULL * 1024)),
	       (unsigned long long)stats.total_pages);
	printf("  Used:  %llu MB (%llu pages)\n",
	       (unsigned long long)(stats.used_memory / (1024ULL * 1024)),
	       (unsigned long long)stats.used_pages);
	printf("  Free:  %llu MB (%llu pages)\n",
	       (unsigned long long)(stats.free_memory / (1024ULL * 1024)),
	       (unsigned long long)stats.free_pages);
	/* Attribute used pages to their owners.  "other" is everything the
	 * kernel takes straight from the page allocator: user process pages,
	 * page tables, kernel stacks, DMA buffers. */
	uint64_t slab_kb = stats.slab_pages * 4;
	uint64_t pc_kb = stats.pagecache_pages * 4;
	uint64_t accounted = stats.slab_pages + stats.pagecache_pages;
	uint64_t other_pages = stats.used_pages > accounted ?
				       stats.used_pages - accounted :
				       0;
	printf("Used breakdown:\n");
	printf("  Slab:      %llu KB (%llu pages, %llu large allocs active)\n",
	       (unsigned long long)slab_kb,
	       (unsigned long long)stats.slab_pages,
	       (unsigned long long)stats.slab_large_active);
	printf("  Pagecache: %llu KB (%llu pages)\n", (unsigned long long)pc_kb,
	       (unsigned long long)stats.pagecache_pages);
	printf("  Other:     %llu KB (%llu pages)\n",
	       (unsigned long long)(other_pages * 4),
	       (unsigned long long)other_pages);
	printf("Heap:\n");
	printf("  Allocated: %llu KB\n",
	       (unsigned long long)(stats.heap_allocated / 1024));
	printf("  Free:      %llu KB\n",
	       (unsigned long long)(stats.heap_free / 1024));
	printf("========================\n");
	return 0;
}
