// memstat - Display memory statistics for LikeOS-64
// Usage: memstat

#include <stdio.h>
#include <stdint.h>
#include <errno.h>

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

static long syscall2(long num, long a1, long a2)
{
	long ret;
	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(num), "D"(a1), "S"(a2)
			 : "rcx", "r11", "memory");
	return ret;
}

int main(int argc, char **argv)
{
	/* -o also asks the kernel to print which call sites hold the allocated
	 * pages, and what is still holding address spaces.  That report goes to
	 * the kernel log (dmesg / serial), because it is the only place a
	 * variable-length answer belongs.
	 *
	 * It only exists in a kernel built with DEBUG=1: the tracking costs 4
	 * bytes per physical page and a lock on every address-space create and
	 * destroy, so it is compiled out of ordinary builds.  On one of those
	 * the flag is accepted and simply produces nothing.
	 *
	 * It is also root-only: the report names every process's address space
	 * and it is written to the kernel log, which only root may read.  The
	 * statistics without it are open to anyone. */
	int owners = (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'o');
	memory_stats_t stats = { 0 };
	long ret = syscall2(SYS_MEMSTATS, (long)&stats, owners);
	if (ret == -EPERM) {
		fprintf(stderr,
			"memstat: -o: permission denied (must be root)\n");
		return 1;
	}
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
	if (owners)
		printf("(page-owner report written to the kernel log: dmesg;\n"
		       " nothing there means this kernel was not built with DEBUG=1)\n");
	return 0;
}
