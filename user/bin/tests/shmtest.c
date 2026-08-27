/*
 * Does WebKit's shared-memory idiom work here?
 *
 * WebKit paints into a ShareableBitmap backed by POSIX shared memory, and on
 * this system that is the ONLY way a page can reach the screen: accelerated
 * compositing is off (no GL), so the non-composited path -- cairo into a
 * ShareableBitmap -- is what draws.  Pages come out blank, and the only clue
 * is "Failed to create shared memory: Cannot allocate memory".
 *
 * That message covers just ONE of the four steps.  SharedMemoryUnix.cpp:
 *
 *     fd = shm_open(name, O_CREAT|O_RDWR, 0600);   <- only this one logs
 *     shm_unlink(name);                            <- note: BEFORE the rest
 *     ftruncate(fd, size);                         <- returns nullptr, silent
 *     mmap(0, size, RW, MAP_SHARED, fd, 0);        <- returns nullptr, silent
 *
 * So a blank page with nothing in the log means ftruncate or mmap failed and
 * said nothing.  This runs that exact sequence, in that exact order, printing
 * each step BEFORE it runs and the errno after, so the failing step names
 * itself.
 *
 * Then it repeats the cycle to catch the other possibility: an object or slot
 * that is not released, so the table fills and later attempts fail with ENOMEM
 * even though nothing is really exhausted.  The kernel has SHM_MAX_OBJECTS=64,
 * which a browser would burn through quickly if release were broken.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

/* A plausible WebKit backing store: 1280x1024 at 4 bytes per pixel. */
#define BITMAP_SIZE (1280UL * 1024UL * 4UL)

static int one_cycle(const char *name, unsigned long size, int verbose)
{
	int fd;
	void *p;

	if (verbose) printf("  shm_open(\"%s\") ... ", name);
	fd = shm_open(name, O_CREAT | O_RDWR, 0600);
	if (fd < 0) {
		if (verbose) printf("FAILED: %s\n", strerror(errno));
		return -1;
	}
	if (verbose) printf("ok (fd %d)\n", fd);

	/* WebKit unlinks immediately, while the descriptor is still open: the
	 * object must keep working for everyone still holding it. */
	if (verbose) printf("  shm_unlink (while fd is open) ... ");
	if (shm_unlink(name) < 0) {
		if (verbose) printf("FAILED: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (verbose) printf("ok\n");

	if (verbose) printf("  ftruncate(%lu) ... ", size);
	if (ftruncate(fd, (off_t)size) < 0) {
		if (verbose) printf("FAILED: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (verbose) printf("ok\n");

	if (verbose) printf("  mmap(MAP_SHARED, %lu) ... ", size);
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		if (verbose) printf("FAILED: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (verbose) printf("ok (%p)\n", p);

	if (verbose) printf("  write then read back ... ");
	memset(p, 0xA5, 4096);
	((unsigned char *)p)[size - 1] = 0x5A;
	if (((unsigned char *)p)[0] != 0xA5 || ((unsigned char *)p)[size - 1] != 0x5A) {
		if (verbose) printf("MISMATCH -- the mapping does not hold data\n");
		munmap(p, size); close(fd);
		return -1;
	}
	if (verbose) printf("ok\n");

	munmap(p, size);
	close(fd);
	return 0;
}

int main(void)
{
	int i, fails;

	setvbuf(stdout, NULL, _IONBF, 0);

	printf("1. one full cycle, WebKit's order and size (%lu bytes):\n", BITMAP_SIZE);
	if (one_cycle("/shmtest.single", BITMAP_SIZE, 1) < 0) {
		printf("\nFAILED at the step above -- that is why pages are blank.\n");
		return 1;
	}

	printf("\n2. a small one, in case size is the issue ... ");
	printf("%s\n", one_cycle("/shmtest.small", 4096, 0) == 0 ? "ok" : "FAILED");

	/* SHM_MAX_OBJECTS is 64.  200 cycles means every slot must be reused
	 * three times over; if release is broken this stops at about 64. */
	printf("\n3. 200 sequential cycles (slots must be reused) ... ");
	fails = 0;
	for (i = 0; i < 200; i++) {
		char nm[64];
		snprintf(nm, sizeof(nm), "/shmtest.loop%d", i);
		if (one_cycle(nm, 65536, 0) < 0) {
			if (!fails) printf("\n   first failure at cycle %d: %s\n", i, strerror(errno));
			fails++;
		}
	}
	printf("%s\n", fails ? "" : "ok (no leak)");
	if (fails)
		printf("   %d of 200 failed -- objects are not being released.\n", fails);

	/* Holding many at once is a different question from reusing one slot. */
	printf("\n4. 32 held open at once ... ");
	{
		int fds[32]; int held = 0;
		for (i = 0; i < 32; i++) {
			char nm[64];
			snprintf(nm, sizeof(nm), "/shmtest.hold%d", i);
			fds[i] = shm_open(nm, O_CREAT | O_RDWR, 0600);
			if (fds[i] < 0) break;
			shm_unlink(nm);
			if (ftruncate(fds[i], 65536) == 0) held++;
		}
		printf("%d of 32 held\n", held);
		for (i = 0; i < 32; i++) if (fds[i] >= 0) close(fds[i]);
	}

	printf("\n%s\n", fails ? "shared memory is BROKEN -- see above"
			       : "all shared-memory steps pass");
	return fails ? 1 : 0;
}
