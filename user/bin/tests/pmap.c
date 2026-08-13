/* pmap - show a process's address space.
 *
 * ps reports one VSZ number, and a single total cannot tell a region table
 * that is filling up with records from a handful of records that are growing.
 * Those are different faults with different fixes, so the total on its own
 * sends you looking in the wrong place.  This prints the table.
 *
 * Watch mode (-w) is the point of it: it re-reads at an interval and reports
 * what CHANGED -- how many records appeared, how much the brk moved, how much
 * the mapped total moved.  A leak shows up as a count that only rises; a heap
 * that is never returned shows up as a brk that only rises with the count
 * flat.  Run it against a process that is doing nothing and whatever still
 * moves is the thing to chase.
 *
 * Usage:
 *   pmap <pid>              one snapshot, every region listed
 *   pmap -s <pid>           summary only
 *   pmap -w [-n SEC] <pid>  watch, printing deltas
 *
 * Only your own processes: the report says where another process keeps its
 * code, its stacks and its heap, so the kernel hands it out only to the user
 * that owns the process (and to root, who may look at any of them).  A pid
 * belonging to someone else answers "Operation not permitted".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/procinfo.h>

#define MAX_REGIONS 65536
#define MAX_PROCS   512

static procmap_t g_maps[MAX_REGIONS];
static procinfo_t g_procs[MAX_PROCS];

/* Live threads in the process, or -1 if it cannot be determined.
 *
 * Address space alone cannot say WHY it is growing.  A stack arriving every
 * few seconds means either threads are piling up (each holding its own stack,
 * legitimately) or they exit and their stacks are never given back.  Those are
 * different faults, and the thread count is what separates them. */
static int thread_count(int pid)
{
	int n = getprocinfo(g_procs, MAX_PROCS);
	int i, live = 0;

	if (n <= 0)
		return -1;
	for (i = 0; i < n; i++)
		if (g_procs[i].tgid == pid)
			live++;
	return live;
}

static const char *prot_str(uint64_t prot, char out[5])
{
	out[0] = (prot & 0x1) ? 'r' : '-';
	out[1] = (prot & 0x2) ? 'w' : '-';
	out[2] = (prot & 0x4) ? 'x' : '-';
	out[3] = '\0';
	return out;
}

static void human(uint64_t bytes, char *out, size_t n)
{
	if (bytes >= (1UL << 30))
		snprintf(out, n, "%llu.%1llu GB",
			 (unsigned long long)(bytes >> 30),
			 (unsigned long long)((bytes % (1UL << 30)) * 10 >> 30));
	else if (bytes >= (1UL << 20))
		snprintf(out, n, "%llu.%1llu MB",
			 (unsigned long long)(bytes >> 20),
			 (unsigned long long)((bytes % (1UL << 20)) * 10 >> 20));
	else
		snprintf(out, n, "%llu KB", (unsigned long long)(bytes >> 10));
}

static void print_summary(const procmapinfo_t *in)
{
	char t[32], b[32];

	human(in->total_bytes, t, sizeof(t));
	human(in->brk > in->brk_start ? in->brk - in->brk_start : 0, b,
	      sizeof(b));
	printf("pid %d (tgid %d)\n", in->pid, in->tgid);
	printf("  regions   : %u in use, table holds %u\n", in->n_regions,
	       in->capacity);
	printf("  mapped    : %s in regions\n", t);
	printf("  brk       : %s (%016llx-%016llx)\n", b,
	       (unsigned long long)in->brk_start, (unsigned long long)in->brk);
	printf("  mmap_base : %016llx\n", (unsigned long long)in->mmap_base);
}

int main(int argc, char **argv)
{
	int watch = 0, summary = 0, interval = 2, pid = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w"))
			watch = 1;
		else if (!strcmp(argv[i], "-s"))
			summary = 1;
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			interval = atoi(argv[++i]);
		else
			pid = atoi(argv[i]);
	}
	if (pid <= 0) {
		fprintf(stderr,
			"usage: pmap [-s] [-w [-n SEC]] <pid>\n"
			"  -s  summary only\n"
			"  -w  watch, reporting what changed each interval\n");
		return 2;
	}
	if (interval < 1)
		interval = 1;

	if (!watch) {
		procmapinfo_t info;
		int n = getprocmaps(pid, &info, summary ? NULL : g_maps,
				    summary ? 0 : MAX_REGIONS);

		if (n < 0) {
			fprintf(stderr, "pmap: pid %d: %s\n", pid,
				strerror(errno));
			return 1;
		}
		print_summary(&info);
		if (!summary && n > 0) {
			char p[5], sz[32];

			printf("\n%-18s %-18s %-6s %-4s %s\n", "START", "END",
			       "SIZE", "PROT", "BACKING");
			for (i = 0; i < n; i++) {
				human(g_maps[i].length, sz, sizeof(sz));
				printf("%016llx-%016llx %-6s %-4s %s%s%s\n",
				       (unsigned long long)g_maps[i].start,
				       (unsigned long long)(g_maps[i].start +
							    g_maps[i].length),
				       sz, prot_str(g_maps[i].prot, p),
				       g_maps[i].file_backed ? "file" : "anon",
				       g_maps[i].lazy ? " lazy" : "",
				       g_maps[i].device ? " device" : "");
			}
			if ((unsigned)n < info.n_regions)
				printf("(%u more not shown)\n",
				       info.n_regions - (unsigned)n);
		}
		return 0;
	}

	/* Watch: only the deltas matter, so the first sample is the baseline
	 * and every line after it is movement since the one before. */
	{
		procmapinfo_t prev, cur;
		int first = 1, prev_thr = -1;

		printf("watching pid %d every %ds; ^C to stop\n", pid,
		       interval);
		printf("%8s %10s %14s %12s %8s %9s\n", "REGIONS",
		       "d(REGIONS)", "MAPPED", "BRK", "THREADS", "d(THR)");
		for (;;) {
			char m[32], b[32];

			if (getprocmaps(pid, &cur, NULL, 0) < 0) {
				fprintf(stderr, "pmap: pid %d: %s\n", pid,
					strerror(errno));
				return 1;
			}
			human(cur.total_bytes, m, sizeof(m));
			human(cur.brk > cur.brk_start ? cur.brk - cur.brk_start
						      : 0,
			      b, sizeof(b));
			int thr = thread_count(pid);

			if (first) {
				printf("%8u %10s %14s %12s %8d %9s\n",
				       cur.n_regions, "-", m, b, thr, "-");
				first = 0;
				prev_thr = thr;
			} else {
				long dregions = (long)cur.n_regions -
						(long)prev.n_regions;
				long long dmapped =
					(long long)cur.total_bytes -
					(long long)prev.total_bytes;
				long long dbrk = (long long)cur.brk -
						 (long long)prev.brk;

				printf("%8u %+10ld %14s %12s %8d %+9d   (mapped %+lld KB, brk %+lld KB)\n",
				       cur.n_regions, dregions, m, b, thr,
				       (prev_thr >= 0 && thr >= 0) ?
					       thr - prev_thr : 0,
				       dmapped / 1024, dbrk / 1024);
			}
			prev_thr = thr;
			fflush(stdout);
			prev = cur;
			sleep(interval);
		}
	}
	return 0;
}
