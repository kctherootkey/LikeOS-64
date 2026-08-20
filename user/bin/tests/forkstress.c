/*
 * forkstress - hunt for a physical page that is freed while still mapped.
 *
 * The bug this exists to reproduce shows up in ordinary programs as memory
 * that changes on its own: hexchat reads a pointer field and gets the kernel's
 * freed-page poison, scp executes its own libcrypto text and finds malloc
 * chunk headers there.  Both are one fault -- a physical frame handed back to
 * the allocator while a live page table entry still points at it -- seen from
 * either side of the reuse.  Whoever gets the frame next decides which.
 *
 * Reproducing it needs the shape of the programs it happens to, not their
 * size.  What hexchat and scp have in common is forking from a process whose
 * other threads are running, and a child that exits or execs immediately
 * afterwards.  That is the whole recipe: the fork marks every page
 * copy-on-write and takes a reference on each, the siblings keep faulting on
 * those pages, and the child's teardown drops the references again -- three
 * things touching one reference count, two of them concurrently.
 *
 * Two kinds of damage are watched for, because the two crashes showed
 * different ones:
 *
 *   Anonymous memory.  Each worker owns private pages holding a pattern only
 *   it writes, so nothing else has any business changing them.
 *
 *   A read-only file mapping.  A few megabytes of a shared library, mapped
 *   and checksummed at start-up, then re-checked as it runs.  Nothing in the
 *   system writes to it, so a change there cannot be a stray store from
 *   anywhere -- it can only be the frame underneath it being replaced.  That
 *   is the scp crash, whose faulting instruction was inside libcrypto's text
 *   and found malloc chunk headers there, and no test that watches only its
 *   own data would ever see it.
 *
 * A failure prints what it found and how far it extends.  The extent is the
 * useful part: damage that starts and ends on a page boundary and is exactly
 * one page long says a single frame was lost, which is a very different bug
 * from a stray write that happens to land nearby.
 */
#include <dlfcn.h>
#include <sched.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PGSZ 4096UL

/* The kernel's page poisons, which name the fault outright when they turn up
 * in a user page: 0xFEEDFACE is written when a frame is FREED, 0xCCCCCCCC
 * when a fresh one is handed out.  Seeing the first means the frame this
 * process is still mapping has been given back to the allocator.  Seeing the
 * second means it was handed to somebody else and they have not written to it
 * yet.  (Both are debug-build behaviour; on a production kernel the same bug
 * shows up as another process's data instead, which the "foreign" case below
 * covers.) */
#define POISON_FREED 0xFEEDFACEFEEDFACEULL
#define POISON_FRESH 0xCCCCCCCCCCCCCCCCULL

static int g_seconds = 60;
static int g_workers = 4;
static int g_pages = 64;
static volatile int g_stop;
static volatile long g_forks;
static volatile long g_sweeps;
static volatile int g_failures;

/* Which kinds of address-space churn to run alongside the canaries.  All of
 * them by default; -x narrows the run to a named few.
 *
 * Separately selectable because a reproduction is only half an answer.  Once
 * something does fail, the next question is which of these caused it, and
 * turning them off one at a time answers that in minutes -- against a bug that
 * otherwise takes an hour per data point. */
static int do_fork = 1, do_mmap = 1, do_threads = 1, do_dl = 1, do_tlb = 1;
static volatile long g_maps, g_thrs, g_dls, g_tlb;

/* ---- the read-only canary ----------------------------------------------
 *
 * A file mapped read-only is the same kind of memory as a shared library's
 * text: demand-paged from the file, never written, and identical in every
 * process that maps it.  Mapping a few megabytes and checksumming it gives a
 * large target for exactly the failure that hit scp, without needing the
 * program's own text (which is small, and would need linker symbols this
 * script does not define).
 *
 * The first pass faults every page in, so a later mismatch is a page that WAS
 * resident and correct and then stopped being either. */
static const char *g_ro_path = "/lib/libc.so";
static const uint64_t *g_ro;
static size_t g_ro_words;
static uint64_t g_ro_sum;

static uint64_t ro_sum(void)
{
	uint64_t sum = 0;

	for (size_t i = 0; i < g_ro_words; i++)
		sum = sum * 1000003ULL + g_ro[i];
	return sum;
}

static int ro_map(void)
{
	int fd = open(g_ro_path, O_RDONLY);
	off_t len;
	void *p;

	if (fd < 0)
		return -1;
	len = lseek(fd, 0, SEEK_END);
	if (len <= 0) {
		close(fd);
		return -1;
	}
	if (len > (off_t)(8UL << 20))
		len = (off_t)(8UL << 20);
	p = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return -1;
	g_ro = (const uint64_t *)p;
	g_ro_words = (size_t)len / 8;
	return 0;
}

/* Name the page that changed, once the checksum says one has. */
static void ro_locate_damage(void)
{
	const char *base = (const char *)g_ro;
	size_t nbytes = g_ro_words * 8;

	for (size_t off = 0; off + PGSZ <= nbytes; off += PGSZ) {
		const uint64_t *w = (const uint64_t *)(const void *)(base + off);

		for (unsigned i = 0; i < PGSZ / 8; i++) {
			if (w[i] != POISON_FREED && w[i] != POISON_FRESH)
				continue;
			printf("    page %p (file offset 0x%lx) holds %s at +0x%x\n",
			       (const void *)(base + off), (unsigned long)off,
			       w[i] == POISON_FREED ? "FREED-PAGE POISON"
						    : "FRESH-PAGE POISON",
			       i * 8);
			return;
		}
	}
	printf("    (no poison in it: the frame was reused and written over)\n");
}

/* ---- worker data -------------------------------------------------------- */

static inline uint64_t expect_word(unsigned tid, size_t idx)
{
	/* Never zero and never either poison, so a mismatch cannot be read as
	 * a match by accident. */
	return (((uint64_t)tid + 1) << 56) ^
	       ((uint64_t)(idx + 1) * 0x9E3779B97F4A7C15ULL) ^ 1;
}

/* Describe a mismatch and how far it runs.  The extent is what separates a
 * lost frame from a stray write. */
static void report_damage(unsigned tid, uint64_t *base, size_t nwords,
			  size_t at)
{
	uint64_t got = base[at];
	const char *what = got == POISON_FREED ? "FREED-PAGE POISON" :
			   got == POISON_FRESH ? "FRESH-PAGE POISON" :
						 "foreign data";
	size_t first = at, last = at;
	uintptr_t addr = (uintptr_t)&base[at];

	while (first > 0 && base[first - 1] != expect_word(tid, first - 1))
		first--;
	while (last + 1 < nwords && base[last + 1] != expect_word(tid, last + 1))
		last++;

	__sync_fetch_and_add(&g_failures, 1);
	printf("\nFAIL worker %u: %s at %p\n", tid, what, (void *)addr);
	printf("    expected %016llx, got %016llx\n",
	       (unsigned long long)expect_word(tid, at),
	       (unsigned long long)got);
	printf("    damage runs %p..%p (%lu bytes)\n", (void *)&base[first],
	       (void *)&base[last + 1],
	       (unsigned long)((last - first + 1) * 8));
	{
		uintptr_t s = (uintptr_t)&base[first];
		uintptr_t e = (uintptr_t)&base[last + 1];

		printf("    %s\n",
		       (s % PGSZ == 0 && e % PGSZ == 0 && e - s == PGSZ) ?
			       "exactly ONE page, page-aligned: a single frame was lost" :
		       (s % PGSZ == 0 && e % PGSZ == 0) ?
			       "a whole number of pages, page-aligned: frames were lost" :
			       "NOT page-aligned: this is a stray write, not a lost frame");
	}
	fflush(stdout);
}

static void *worker(void *arg)
{
	unsigned tid = (unsigned)(uintptr_t)arg;
	size_t nbytes = (size_t)g_pages * PGSZ;
	size_t nwords = nbytes / 8;
	uint64_t *buf = mmap(NULL, nbytes, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (buf == MAP_FAILED) {
		printf("worker %u: mmap failed\n", tid);
		return NULL;
	}
	for (size_t i = 0; i < nwords; i++)
		buf[i] = expect_word(tid, i);

	while (!g_stop) {
		for (size_t i = 0; i < nwords; i++) {
			if (buf[i] != expect_word(tid, i)) {
				report_damage(tid, buf, nwords, i);
				/* Repair and carry on: one lost frame should
				 * not end the run, and a second one is worth
				 * seeing. */
				for (size_t j = 0; j < nwords; j++)
					buf[j] = expect_word(tid, j);
				break;
			}
		}
		/* Write every page back, so each one is dirtied again between
		 * forks and has to be copied by the next one. */
		for (size_t i = 0; i < nwords; i += PGSZ / 8)
			buf[i] = expect_word(tid, i);
		__sync_fetch_and_add(&g_sweeps, 1);
	}
	munmap(buf, nbytes);
	return NULL;
}

/* ---- mapping churn ------------------------------------------------------
 *
 * Map, touch, re-protect and unmap, over and over.  None of this is exotic;
 * it is what a program does every time it loads a plugin or grows its heap,
 * and it is the part of the address space that forkstress otherwise leaves
 * completely still.  mprotect is in here because it SPLITS a region, which is
 * where the page accounting for a partially-covered range has to be got
 * exactly right. */
static void *churn_mmap(void *arg)
{
	unsigned n = 0;

	(void)arg;
	while (!g_stop) {
		size_t pages = 1 + (n % 17);
		size_t len = pages * PGSZ;
		unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED) {
			usleep(1000);
			continue;
		}
		for (size_t i = 0; i < len; i += PGSZ)
			p[i] = (unsigned char)n;
		/* Protect a slice out of the middle, so the region has to be
		 * split and later rejoined. */
		if (pages >= 3)
			mprotect(p + PGSZ, PGSZ, PROT_READ);
		if (pages >= 3)
			mprotect(p + PGSZ, PGSZ, PROT_READ | PROT_WRITE);
		/* Unmap in two pieces as often as in one. */
		if ((n & 1) && pages >= 2) {
			munmap(p, PGSZ);
			munmap(p + PGSZ, len - PGSZ);
		} else {
			munmap(p, len);
		}
		n++;
		__sync_fetch_and_add(&g_maps, 1);
	}
	return NULL;
}

/* ---- thread churn -------------------------------------------------------
 *
 * Threads that start, touch their stack and exit.  Each one is a stack mapped
 * and then unmapped, which is the same map/unmap pair as above but performed
 * by the thread machinery on a stack that was in use moments earlier -- the
 * case where unmapping too early, or twice, has a history. */
static void *short_lived(void *arg)
{
	volatile char buf[2048];

	(void)arg;
	for (unsigned i = 0; i < sizeof buf; i += 512)
		buf[i] = (char)i;
	return NULL;
}

static void *churn_threads(void *arg)
{
	unsigned n = 0;

	(void)arg;
	while (!g_stop) {
		pthread_t t;

		if (pthread_create(&t, NULL, short_lived, NULL) != 0) {
			usleep(1000);
			continue;
		}
		/* Both dispositions: a joined stack is released by the joiner,
		 * a detached one by the thread's own exit. */
		if (n & 1)
			pthread_detach(t);
		else
			pthread_join(t, NULL);
		n++;
		__sync_fetch_and_add(&g_thrs, 1);
		if (n % 64 == 0)
			usleep(1000);
	}
	return NULL;
}

/* ---- shared-object churn ------------------------------------------------
 *
 * This is the one the crash backtrace points at: hexchat dies inside
 * plugin_add(), and loading a plugin means mapping an object, write-protecting
 * its relocated data and, on unload, taking the whole thing back out. */
static const char *g_dl_path = "/lib/libz.so.1";

static void *churn_dl(void *arg)
{
	(void)arg;
	while (!g_stop) {
		void *h = dlopen(g_dl_path, RTLD_NOW | RTLD_LOCAL);

		if (!h) {
			usleep(10000);
			continue;
		}
		(void)dlsym(h, "zlibVersion");
		dlclose(h);
		__sync_fetch_and_add(&g_dls, 1);
		usleep(1000);
	}
	return NULL;
}

/* ---- stale translation check --------------------------------------------
 *
 * After a page is unmapped, a processor that still holds its translation can
 * go on reaching the frame -- which by then has gone back to the allocator to
 * be handed to somebody else.  Writes through it land in whatever the frame
 * became: another program's heap, or its program TEXT.  This is the one
 * failure a page-table walk cannot find, because the entry it would look at is
 * gone; only the processor knows.
 *
 * Three things this has to get right.
 *
 * It must span PROCESSORS.  Unmapping invalidates the translation on the CPU
 * that ran munmap; the others are told by an interprocessor message, and it is
 * that message going astray or arriving late that leaves the entry behind.  So
 * the toucher and the unmapper are pinned apart -- doing both on one thread
 * tests only the local invalidation, which never fails.
 *
 * It must not FAULT to reach its verdict.  Probing an address that should be
 * unmapped means the correct outcome is a fatal fault, and a kernel that
 * reports every one of those buries the machine in crash dumps.  So the page
 * is not left unmapped: the unmapper immediately maps a FRESH anonymous page
 * over the same address.  Anonymous pages read as zero, so the toucher looking
 * again sees 0 if its translation was refreshed, and the byte IT wrote if the
 * old translation survived.  Same question, no fault either way.
 *
 * And the verdict must not depend on a signal handler.  Catching SIGSEGV and
 * returning is its own machinery here, and a test that leans on it cannot tell
 * a stale translation from a handler that did not run.
 */
#define TLB_MARK 0x5a

static volatile unsigned char *tlb_page;
static volatile int tlb_phase; /* 0 idle, 1 remap please, 2 done, 3 failed */

static void tlb_pin(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

static void *tlb_remapper(void *arg)
{
	(void)arg;
	tlb_pin(1);
	while (!g_stop) {
		void *p;

		if (tlb_phase != 1) {
			sched_yield();
			continue;
		}
		p = (void *)tlb_page;
		/* Drop it and put a FRESH anonymous page at the same address.
		 * The replacement is what keeps the toucher from faulting. */
		if (munmap(p, PGSZ) != 0 ||
		    mmap(p, PGSZ, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
			 0) != p) {
			__sync_synchronize();
			tlb_phase = 3;
			continue;
		}
		__sync_synchronize();
		tlb_phase = 2;
	}
	return NULL;
}

static void *churn_tlb(void *arg)
{
	pthread_t r;

	(void)arg;
	tlb_pin(0);
	if (pthread_create(&r, NULL, tlb_remapper, NULL) != 0)
		return NULL;

	while (!g_stop) {
		volatile unsigned char *p = mmap(NULL, PGSZ,
						 PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS,
						 -1, 0);
		unsigned char after;

		if (p == MAP_FAILED) {
			usleep(1000);
			continue;
		}
		p[0] = TLB_MARK; /* populate THIS cpu's translation */

		tlb_page = p;
		__sync_synchronize();
		tlb_phase = 1;
		while (tlb_phase == 1 && !g_stop)
			sched_yield();
		if (tlb_phase == 3) {
			tlb_phase = 0;
			munmap((void *)p, PGSZ);
			continue;
		}
		__sync_synchronize();

		/* A fresh anonymous page reads as zero.  Seeing the byte this
		 * thread wrote means the old translation is still in use and
		 * the frame it names belongs to somebody else now. */
		after = p[0];
		if (after == TLB_MARK) {
			__sync_fetch_and_add(&g_failures, 1);
			printf("\nFAIL: a STALE TRANSLATION survived munmap at %p\n",
			       (void *)p);
			printf("    this CPU still reads the page it wrote before another\n");
			printf("    CPU unmapped it; the frame behind it has been handed\n");
			printf("    out again, so writes through it corrupt its new owner\n");
			fflush(stdout);
		}
		munmap((void *)p, PGSZ);
		tlb_phase = 0;
		__sync_fetch_and_add(&g_tlb, 1);
	}
	tlb_phase = 0;
	pthread_join(r, NULL);
	return NULL;
}

/* ---- the forker ---------------------------------------------------------
 *
 * Three shapes of child, because they tear the address space down by different
 * routes: an immediate exit, an exit after touching pages (so the child has
 * resolved some copies of its own first), and an exec (which destroys the
 * address space while the process lives on). */
static void *forker(void *arg)
{
	unsigned n = 0;

	(void)arg;
	while (!g_stop) {
		pid_t kid = fork();
		int st = 0;

		if (kid < 0) {
			usleep(1000);
			continue;
		}
		if (kid == 0) {
			switch (n % 3) {
			case 0:
				_exit(0);
			case 1: {
				/* Resolve a few copies before dying. */
				static volatile char sink;
				char *p = (char *)&sink;

				(void)*p;
				_exit(0);
			}
			default:
				execl("/bin/true", "true", (char *)NULL);
				_exit(0);
			}
		}
		while (waitpid(kid, &st, 0) < 0 && errno == EINTR)
			;
		n++;
		__sync_fetch_and_add(&g_forks, 1);
	}
	return NULL;
}

static void usage(const char *me)
{
	printf("usage: %s [-t seconds] [-w workers] [-p pages-per-worker]\n"
	       "          [-x fork,mmap,thread,dl,tlb]\n"
	       "\n"
	       "  -x  run only the named kinds of churn (default: all of them).\n"
	       "      Use it to find which one a failure depends on.\n",
	       me);
}

static int select_churn(const char *list)
{
	const char *p = list;

	do_fork = do_mmap = do_threads = do_dl = do_tlb = 0;
	while (*p) {
		size_t n = strcspn(p, ",");

		if (n == 4 && !strncmp(p, "fork", 4))
			do_fork = 1;
		else if (n == 4 && !strncmp(p, "mmap", 4))
			do_mmap = 1;
		else if (n == 6 && !strncmp(p, "thread", 6))
			do_threads = 1;
		else if (n == 2 && !strncmp(p, "dl", 2))
			do_dl = 1;
		else if (n == 3 && !strncmp(p, "tlb", 3))
			do_tlb = 1;
		else
			return -1;
		p += n;
		if (*p == ',')
			p++;
	}
	return (do_fork || do_mmap || do_threads || do_dl || do_tlb) ? 0 : -1;
}

int main(int argc, char **argv)
{
	pthread_t *th;
	pthread_t fk, cm, ct, cd, ct2;
	time_t start;
	long last_report = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc)
			g_seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-w") && i + 1 < argc)
			g_workers = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			g_pages = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-x") && i + 1 < argc) {
			if (select_churn(argv[++i]) != 0) {
				usage(argv[0]);
				return 2;
			}
		}
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (g_seconds < 1 || g_workers < 1 || g_workers > 64 || g_pages < 1) {
		usage(argv[0]);
		return 2;
	}

	printf("forkstress: %d workers x %d pages, forking, for %ds\n",
	       g_workers, g_pages, g_seconds);
	if (ro_map() != 0) {
		printf("could not map %s -- running without the read-only canary\n",
		       g_ro_path);
	} else {
		g_ro_sum = ro_sum();
		printf("read-only canary: %s, %lu KB at %p, checksum %016llx\n",
		       g_ro_path, (unsigned long)(g_ro_words * 8 / 1024),
		       (const void *)g_ro, (unsigned long long)g_ro_sum);
	}
	fflush(stdout);

	th = calloc((size_t)g_workers, sizeof(*th));
	if (!th) {
		printf("out of memory\n");
		return 1;
	}
	printf("churn:");
	if (do_fork)
		printf(" fork");
	if (do_mmap)
		printf(" mmap");
	if (do_threads)
		printf(" thread");
	if (do_dl)
		printf(" dl(%s)", g_dl_path);
	if (do_tlb)
		printf(" tlb");
	printf("\n");
	fflush(stdout);

	for (long i = 0; i < g_workers; i++)
		pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)i);
	if (do_fork)
		pthread_create(&fk, NULL, forker, NULL);
	if (do_mmap)
		pthread_create(&cm, NULL, churn_mmap, NULL);
	if (do_threads)
		pthread_create(&ct, NULL, churn_threads, NULL);
	if (do_dl)
		pthread_create(&cd, NULL, churn_dl, NULL);
	if (do_tlb)
		pthread_create(&ct2, NULL, churn_tlb, NULL);

	start = time(NULL);
	while (time(NULL) - start < g_seconds) {
		usleep(200000);
		/* The canary check runs here rather than in a worker: it is one
		 * pass over several megabytes and only needs doing now and
		 * then, and a worker doing it would stop hammering its own
		 * pages. */
		if (g_ro && ro_sum() != g_ro_sum) {
			__sync_fetch_and_add(&g_failures, 1);
			printf("\nFAIL: the READ-ONLY file mapping changed\n");
			printf("    nothing in the system writes to it -- the frame\n");
			printf("    behind it was handed to somebody else\n");
			ro_locate_damage();
			fflush(stdout);
			g_ro_sum = ro_sum(); /* resync and keep going */
		}
		if (time(NULL) - start >= last_report + 10) {
			last_report = time(NULL) - start;
			printf("  %lds: %ld forks, %ld maps, %ld thr, %ld dl, %ld tlb, %ld sweeps, %d failure%s\n",
			       last_report, g_forks, g_maps, g_thrs, g_dls,
			       g_tlb, g_sweeps, g_failures,
			       g_failures == 1 ? "" : "s");
			fflush(stdout);
		}
	}

	g_stop = 1;
	if (do_fork)
		pthread_join(fk, NULL);
	if (do_mmap)
		pthread_join(cm, NULL);
	if (do_threads)
		pthread_join(ct, NULL);
	if (do_dl)
		pthread_join(cd, NULL);
	if (do_tlb)
		pthread_join(ct2, NULL);
	for (int i = 0; i < g_workers; i++)
		pthread_join(th[i], NULL);

	printf("\n%ld forks, %ld maps, %ld thr, %ld dl, %ld tlb, %ld sweeps, %d failure%s\n",
	       g_forks, g_maps, g_thrs, g_dls, g_tlb, g_sweeps, g_failures,
	       g_failures == 1 ? "" : "s");
	free(th);
	return g_failures ? 1 : 0;
}
