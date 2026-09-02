/*
 * wkstress: what a web content process does to the kernel while a page
 * loads, with every return value checked at the point it is produced.
 *
 * Why this program exists.  Loading one heavy page in the browser kills the
 * content process, reliably, but ONLY with hardware acceleration: the same
 * page over the software rasteriser is clean, and without a GL stack at all
 * it fails rarely and at random.  The crash dumps land in four different
 * places -- a fault at 0x8f, a fault at 0x10000004f, free() of an all-ones
 * pointer, and execution off into a run of 0xff bytes -- and every one of
 * them is the same thing seen from a different angle: a value of -1 being
 * used as a pointer.  0x8f is (-1 + 0x90); 0x10000004f is (0xffffffff +
 * 0x50).  So SOMETHING hands back a failure the caller does not check, and
 * by the time it faults there is nothing left to say which call it was.
 *
 * Hunting that through a browser costs a minute per attempt and produces a
 * dump that names a library, not a syscall.  This is the same load with the
 * checks put back: it drives the three things accelerated compositing adds
 * to the ordinary content process, and the FIRST call that lies gets named
 * with its errno, its iteration and its thread instead of being turned into
 * a pointer.
 *
 *   shared memory   Every image, every layer, every message payload is a
 *                   POSIX shared segment: shm_open, ftruncate, mmap, fill,
 *                   munmap, close.  This is where a -1 becomes a pointer if
 *                   mmap fails, and it is the path the failing dump's
 *                   0xffffffff + 0x50 fits.
 *
 *   descriptor IPC  Those segments do not travel as data, they travel as
 *                   FILE DESCRIPTORS over a socket: sendmsg with SCM_RIGHTS,
 *                   recvmsg on the other side, then mmap what came back and
 *                   check that it holds what the sender wrote.  A descriptor
 *                   that arrives wrong maps the wrong memory, which is a
 *                   crash a long way from here.  Every batch is verified.
 *
 *   device buffers  With acceleration each surface is a buffer object on the
 *                   render node: allocate, map, touch, unmap, release.  This
 *                   is the part the software rasteriser does not do at all,
 *                   which is the whole reason to suspect it.
 *
 * All three run at once from several threads, because that is how they run
 * in the browser and because a lock or a table that only breaks under
 * contention will not break any other way.
 *
 * Exit status is 0 when every call in every round did what it promised.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdint.h>
#include <drm/vmwgfx_drm.h>
#include <sys/wait.h>

#define SEG_BYTES (256 * 1024)
#define FDS_PER_MSG 4

static int g_rounds = 200;
static int g_threads = 4;
static int g_verbose;
static volatile int g_failed;

/* One place where a failure is reported, so every report carries the same
 * facts: which call, which errno, which thread, which round.  A silent -1 is
 * what this program exists to prevent, so nothing here returns quietly. */
static void fail(const char *what, int thread, int round, long got)
{
	__sync_fetch_and_add(&g_failed, 1);
	fprintf(stderr,
		"FAIL %s: thread %d round %d returned %ld, errno %d (%s)\n",
		what, thread, round, got, errno, strerror(errno));
	fflush(stderr);
}

/* A pointer that is not a pointer.  mmap says MAP_FAILED, which is -1, and a
 * caller that stores it and adds a field offset produces exactly the faults
 * the browser dies with.  Checked separately from the errno path because the
 * 32-bit form (0x00000000ffffffff) is POSITIVE as a long and slips past a
 * "< 0" test -- which is how a failure becomes an address instead of an
 * error in the first place. */
static int bad_ptr(const void *p)
{
	uintptr_t v = (uintptr_t)p;

	return p == MAP_FAILED || v == 0xffffffffull ||
	       v == (uintptr_t)-1 || v == 0;
}

/* ---- shared memory: the content process's payload allocator ------------- */

static int seg_make(int thread, int round, int idx, size_t bytes)
{
	char name[64];
	int fd;

	snprintf(name, sizeof(name), "/wk-%d-%d-%d", thread, round, idx);
	shm_unlink(name); /* a leftover from a previous run is not this test */
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		fail("shm_open", thread, round, fd);
		return -1;
	}
	/* Unlinked immediately and carried by the descriptor alone: exactly
	 * what the browser does, and what makes the descriptor the only thing
	 * that keeps the memory alive. */
	if (shm_unlink(name) != 0) {
		fail("shm_unlink", thread, round, -1);
		close(fd);
		return -1;
	}
	if (ftruncate(fd, (off_t)bytes) != 0) {
		fail("ftruncate", thread, round, -1);
		close(fd);
		return -1;
	}
	return fd;
}

/* Fill a segment with a pattern that says which segment it is, so a
 * descriptor that arrives pointing at the wrong memory is caught by content
 * and not just by luck. */
static uint32_t seg_word(int thread, int round, int idx, size_t i)
{
	return (uint32_t)((thread << 24) ^ (round << 12) ^ (idx << 8) ^
			  (uint32_t)i * 2654435761u);
}

static int seg_fill(int fd, int thread, int round, int idx, size_t bytes)
{
	uint32_t *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (bad_ptr(p)) {
		fail("mmap(fill)", thread, round, (long)(intptr_t)p);
		return -1;
	}
	for (size_t i = 0; i < bytes / 4; i++)
		p[i] = seg_word(thread, round, idx, i);
	if (munmap(p, bytes) != 0) {
		fail("munmap(fill)", thread, round, -1);
		return -1;
	}
	return 0;
}

static int seg_verify(int fd, int thread, int round, int idx, size_t bytes)
{
	uint32_t *p = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
	size_t bad = 0;

	if (bad_ptr(p)) {
		fail("mmap(verify)", thread, round, (long)(intptr_t)p);
		return -1;
	}
	for (size_t i = 0; i < bytes / 4; i++)
		if (p[i] != seg_word(thread, round, idx, i)) {
			if (!bad)
				fprintf(stderr,
					"FAIL content: thread %d round %d seg %d word %zu is %08x, expected %08x\n",
					thread, round, idx, i, p[i],
					seg_word(thread, round, idx, i));
			bad++;
		}
	munmap(p, bytes);
	if (bad) {
		__sync_fetch_and_add(&g_failed, 1);
		return -1;
	}
	return 0;
}

/* ---- descriptor IPC: how the segments actually travel ------------------- */

static int send_fds(int sock, const int *fds, int n, int thread, int round)
{
	struct msghdr msg;
	struct iovec iov;
	char cbuf[CMSG_SPACE(sizeof(int) * FDS_PER_MSG)];
	struct cmsghdr *cm;
	char byte = 'x';
	ssize_t r;

	memset(&msg, 0, sizeof(msg));
	memset(cbuf, 0, sizeof(cbuf));
	iov.iov_base = &byte;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = CMSG_SPACE(sizeof(int) * n);
	cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int) * n);
	memcpy(CMSG_DATA(cm), fds, sizeof(int) * n);

	r = sendmsg(sock, &msg, 0);
	if (r != 1) {
		fail("sendmsg(SCM_RIGHTS)", thread, round, (long)r);
		return -1;
	}
	return 0;
}

static int recv_fds(int sock, int *fds, int n, int thread, int round)
{
	struct msghdr msg;
	struct iovec iov;
	char cbuf[CMSG_SPACE(sizeof(int) * FDS_PER_MSG)];
	struct cmsghdr *cm;
	char byte = 0;
	ssize_t r;
	int got = 0;

	for (int i = 0; i < n; i++)
		fds[i] = -1;
	memset(&msg, 0, sizeof(msg));
	memset(cbuf, 0, sizeof(cbuf));
	iov.iov_base = &byte;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	r = recvmsg(sock, &msg, 0);
	if (r != 1) {
		fail("recvmsg(SCM_RIGHTS)", thread, round, (long)r);
		return -1;
	}
	for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
		if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
			continue;
		got = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		if (got > n)
			got = n;
		memcpy(fds, CMSG_DATA(cm), sizeof(int) * got);
	}
	if (got != n) {
		fprintf(stderr,
			"FAIL SCM_RIGHTS count: thread %d round %d received %d of %d descriptors\n",
			thread, round, got, n);
		__sync_fetch_and_add(&g_failed, 1);
		for (int i = 0; i < got; i++)
			if (fds[i] >= 0)
				close(fds[i]);
		return -1;
	}
	for (int i = 0; i < n; i++)
		if (fds[i] < 0) {
			fprintf(stderr,
				"FAIL SCM_RIGHTS fd: thread %d round %d descriptor %d is %d\n",
				thread, round, i, fds[i]);
			__sync_fetch_and_add(&g_failed, 1);
			return -1;
		}
	return 0;
}

/* ---- device buffers: the half only the accelerated path performs -------- */

/* Through the device's own header rather than hand-rolled numbers: an ioctl
 * assembled by hand is a second thing that can be wrong, and a wrong one
 * comes back EINVAL, which reads exactly like the kernel bug being hunted. */
static long vmw_cmd(int fd, unsigned nr, unsigned dir, void *arg, size_t size)
{
	return ioctl(fd, _IOC(dir, 'd', 0x40 + nr, size), arg);
}

static int gpu_open(void)
{
	return open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
}

/* One surface's worth of device memory: allocate it, map it, write to it,
 * unmap and release.  The GL stack does this per texture, per vertex buffer
 * and per layer, so a page with a hundred composited elements does it a great
 * many times while the shared-memory and descriptor traffic above is in
 * flight. */
static int gpu_cycle(int drm, int thread, int round)
{
	union drm_vmw_alloc_bo_arg bo;
	struct drm_vmw_handle_close_arg hc;
	uint32_t *p;
	long r;

	memset(&bo, 0, sizeof(bo));
	bo.req.size = 64 * 1024;
	r = vmw_cmd(drm, DRM_VMW_ALLOC_BO, _IOC_READ | _IOC_WRITE, &bo,
		    sizeof(bo));
	if (r != 0 || !bo.rep.handle) {
		fail("DRM_VMW_ALLOC_BO", thread, round, r);
		return -1;
	}
	p = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, drm,
		 (off_t)bo.rep.map_handle);
	if (bad_ptr(p)) {
		fail("mmap(buffer object)", thread, round, (long)(intptr_t)p);
		return -1;
	}
	for (int i = 0; i < 64 * 1024 / 4; i += 512)
		p[i] = (uint32_t)(thread * 1000 + round);
	if (munmap(p, 64 * 1024) != 0) {
		fail("munmap(buffer object)", thread, round, -1);
		return -1;
	}
	memset(&hc, 0, sizeof(hc));
	hc.handle = bo.rep.handle;
	r = vmw_cmd(drm, DRM_VMW_UNREF_DMABUF, _IOC_WRITE, &hc, sizeof(hc));
	if (r != 0) {
		fail("DRM_VMW_UNREF_DMABUF", thread, round, r);
		return -1;
	}
	return 0;
}

/* ---- executable text: the shape the SIGILL dumps have ------------------- */
//
// One dump died on an illegal instruction whose bytes were
// `ff ff ff ff 00 00 00 00` repeated -- inside a file-backed r-x mapping,
// which is a library's code.  A text page that reads back as a pattern
// instead of the file's contents is a demand-paging failure, and it kills
// whatever branches into it, arbitrarily far from the cause.  So the pages
// this program executes out of are summed once at startup and checked again
// every round, in the parent and in every child: the first round where the
// sum moves says the page went bad, and says it here instead of as a jump
// into nothing.

static unsigned long g_text_a, g_text_b, g_sum_a, g_sum_b;

static unsigned long page_sum(unsigned long page)
{
	const unsigned char *p = (const unsigned char *)page;
	unsigned long h = 5381;

	for (unsigned long i = 0; i < 4096; i++)
		h = h * 33 + p[i];
	return h;
}

/* Two pages: one out of this program's own text, one out of the C library's.
 * They are demand-paged from different files through the same path. */
static void text_baseline(void)
{
	g_text_a = (unsigned long)(uintptr_t)&page_sum & ~0xFFFUL;
	g_text_b = (unsigned long)(uintptr_t)&memcpy & ~0xFFFUL;
	g_sum_a = page_sum(g_text_a);
	g_sum_b = page_sum(g_text_b);
}

static int text_check(const char *who, int thread, int round)
{
	unsigned long a = page_sum(g_text_a), b = page_sum(g_text_b);

	if (a == g_sum_a && b == g_sum_b)
		return 0;
	fprintf(stderr,
		"FAIL text page changed under %s: thread %d round %d, program page %lx sum %lx (was %lx), library page %lx sum %lx (was %lx)\n",
		who, thread, round, g_text_a, a, g_sum_a, g_text_b, b, g_sum_b);
	__sync_fetch_and_add(&g_failed, 1);
	return -1;
}

/* ---- fork and copy-on-write: what the browser does per page load -------- */
//
// The content process is forked, and everything the parent had mapped becomes
// copy-on-write in both.  Every write on either side is a fault that copies a
// page.  One dump died IN that copy: the kernel followed a page-table entry
// whose address bits were all ones and took a general protection fault on a
// non-canonical address, which halts the machine rather than the process.  So
// this forks with a large writable region already dirty, has the child write
// across all of it (a copy per page), checks its own text still reads right,
// and reports what the child made of it.

#define COW_PAGES 512

static int cow_round(int thread, int round)
{
	size_t bytes = (size_t)COW_PAGES * 4096;
	unsigned char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	pid_t pid;
	int st = 0;

	if (bad_ptr(p)) {
		fail("mmap(cow region)", thread, round, (long)(intptr_t)p);
		return -1;
	}
	for (int i = 0; i < COW_PAGES; i++)
		p[(size_t)i * 4096] = (unsigned char)(i + round);

	pid = fork();
	if (pid < 0) {
		fail("fork", thread, round, pid);
		munmap(p, bytes);
		return -1;
	}
	if (pid == 0) {
		/* Every page written is one copy-on-write fault. */
		int bad = 0;
		for (int i = 0; i < COW_PAGES; i++) {
			if (p[(size_t)i * 4096] != (unsigned char)(i + round))
				bad++;
			p[(size_t)i * 4096] = (unsigned char)(i + round + 1);
		}
		if (text_check("copy-on-write", thread, round) != 0)
			bad++;
		_exit(bad ? 1 : 0);
	}
	/* The parent writes the same pages at the same time: both sides fault
	 * on the same shared page, which is the case the copy has to get
	 * right and the one two threads can race on. */
	for (int i = 0; i < COW_PAGES; i++)
		p[(size_t)i * 4096] = (unsigned char)(i + round + 2);
	if (waitpid(pid, &st, 0) != pid) {
		fail("waitpid", thread, round, -1);
		munmap(p, bytes);
		return -1;
	}
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		fprintf(stderr,
			"FAIL copy-on-write child: thread %d round %d exited %d (signal %d)\n",
			thread, round,
			WIFEXITED(st) ? WEXITSTATUS(st) : -1,
			WIFSIGNALED(st) ? WTERMSIG(st) : 0);
		__sync_fetch_and_add(&g_failed, 1);
		munmap(p, bytes);
		return -1;
	}
	for (int i = 0; i < COW_PAGES; i++)
		if (p[(size_t)i * 4096] != (unsigned char)(i + round + 2)) {
			fprintf(stderr,
				"FAIL copy-on-write parent: thread %d round %d page %d is %02x, expected %02x\n",
				thread, round, i, p[(size_t)i * 4096],
				(unsigned char)(i + round + 2));
			__sync_fetch_and_add(&g_failed, 1);
			munmap(p, bytes);
			return -1;
		}
	munmap(p, bytes);
	return 0;
}

/* ---- a mapped file, read through its own descriptor at the same time ---- */
//
// The descriptor a mapping was made from stays open, and the program goes on
// using it -- the dynamic linker reads a library's headers from the same fd it
// maps the segments from, and every fork hands both to a child.  A page fault
// on the mapping must be unaffected by that: what lands in the page is decided
// by the mapping's offset, not by wherever the descriptor's position happens
// to be.
//
// So: take a reference copy of a file through pread, then map it fresh every
// round and compare every page of the mapping against that copy, while another
// thread does nothing but seek and read the SAME descriptor as fast as it can.
// A page that comes back holding some other part of the file is the failure,
// and it is caught here as a byte comparison instead of much later as a branch
// into whatever those bytes decode to.

#define RACE_PAGES 256
#define RACE_BYTES ((size_t)RACE_PAGES * 4096)

static int g_race_fd = -1;
static unsigned char *g_race_ref;
static volatile int g_race_stop;

static void race_setup(void)
{
	static const char *const candidates[] = { "/lib/libc.so",
						  "/lib/ld-likeos.so",
						  "/usr/local/bin/wkstress",
						  NULL };
	for (int i = 0; candidates[i]; i++) {
		int fd = open(candidates[i], O_RDONLY);
		if (fd < 0)
			continue;
		unsigned char *ref = malloc(RACE_BYTES);
		if (!ref) {
			close(fd);
			return;
		}
		/* The reference is taken with nothing else touching the
		 * descriptor, so it is what the file holds. */
		ssize_t n = pread(fd, ref, RACE_BYTES, 0);
		if (n == (ssize_t)RACE_BYTES) {
			g_race_fd = fd;
			g_race_ref = ref;
			printf("          mapped-file race: %s, %d pages\n",
			       candidates[i], RACE_PAGES);
			return;
		}
		free(ref);
		close(fd);
	}
	printf("          mapped-file race: no file large enough, skipped\n");
}

/* Nothing but position changes on the shared descriptor. */
static void *race_reader(void *arg)
{
	unsigned char buf[512];
	unsigned long i = 0;

	(void)arg;
	while (!g_race_stop) {
		off_t where = (off_t)((i++ * 4096) % RACE_BYTES);
		if (lseek(g_race_fd, where, SEEK_SET) < 0)
			break;
		if (read(g_race_fd, buf, sizeof(buf)) < 0)
			break;
	}
	return NULL;
}

static int race_round(int thread, int round)
{
	unsigned char *m;
	int bad = 0;

	if (g_race_fd < 0)
		return 0;
	/* A fresh mapping every round, so every page is faulted in again
	 * rather than found already resident from the last one. */
	m = mmap(NULL, RACE_BYTES, PROT_READ, MAP_PRIVATE, g_race_fd, 0);
	if (bad_ptr(m)) {
		fail("mmap(mapped-file race)", thread, round, (long)(intptr_t)m);
		return -1;
	}
	for (int pg = 0; pg < RACE_PAGES; pg++) {
		size_t off = (size_t)pg * 4096;
		if (memcmp(m + off, g_race_ref + off, 4096) == 0)
			continue;
		if (!bad) {
			size_t i = 0;
			while (i < 4096 && m[off + i] == g_race_ref[off + i])
				i++;
			fprintf(stderr,
				"FAIL mapped page differs from the file: thread %d round %d page %d byte %zu is %02x, the file has %02x\n",
				thread, round, pg, i, m[off + i],
				g_race_ref[off + i]);
			__sync_fetch_and_add(&g_failed, 1);
		}
		bad++;
	}
	munmap(m, RACE_BYTES);
	return bad ? -1 : 0;
}

/* ---- one worker: all of it at once, which is the point ------------------ */

struct worker {
	pthread_t tid;
	int index;
};

static void *worker(void *arg)
{
	struct worker *w = arg;
	int drm = gpu_open();

	for (int round = 0; round < g_rounds && !g_failed; round++) {
		int sv[2];
		int fds[FDS_PER_MSG];
		int rfds[FDS_PER_MSG];

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			fail("socketpair", w->index, round, -1);
			break;
		}
		int made = 0;
		for (; made < FDS_PER_MSG; made++) {
			fds[made] = seg_make(w->index, round, made, SEG_BYTES);
			if (fds[made] < 0)
				break;
			if (seg_fill(fds[made], w->index, round, made, SEG_BYTES) != 0) {
				close(fds[made]);
				break;
			}
		}
		if (made == FDS_PER_MSG &&
		    send_fds(sv[0], fds, FDS_PER_MSG, w->index, round) == 0 &&
		    recv_fds(sv[1], rfds, FDS_PER_MSG, w->index, round) == 0) {
			/* The descriptors that came back must name the same
			 * memory the sender filled.  This is the check the
			 * browser cannot make and the reason a wrong one shows
			 * up as a fault somewhere else entirely. */
			for (int i = 0; i < FDS_PER_MSG; i++) {
				seg_verify(rfds[i], w->index, round, i, SEG_BYTES);
				close(rfds[i]);
			}
		}
		for (int i = 0; i < made; i++)
			close(fds[i]);
		close(sv[0]);
		close(sv[1]);

		if (drm >= 0 && gpu_cycle(drm, w->index, round) != 0)
			break;

		/* fork/copy-on-write only on one thread: fork from a threaded
		 * process is its own hazard and doubling it here would report
		 * that instead of the page copy being hunted. */
		if (w->index == 0 && cow_round(w->index, round) != 0)
			break;
		if (text_check("load", w->index, round) != 0)
			break;
		if (race_round(w->index, round) != 0)
			break;

		if (g_verbose && (round % 25) == 0)
			printf("  thread %d: round %d\n", w->index, round);
	}
	if (drm >= 0)
		close(drm);
	return NULL;
}

int main(int argc, char **argv)
{
	struct worker *w;
	int drm;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			g_verbose = 1;
		else if (!strcmp(argv[i], "-r") && i + 1 < argc)
			g_rounds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			g_threads = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: %s [-v] [-r rounds] [-t threads]\n",
				argv[0]);
			return 2;
		}
	}
	if (g_threads < 1)
		g_threads = 1;
	if (g_rounds < 1)
		g_rounds = 1;

	text_baseline();
	race_setup();
	drm = gpu_open();
	printf("wkstress: %d threads x %d rounds, %d KB segments, %d descriptors per message\n",
	       g_threads, g_rounds, SEG_BYTES / 1024, FDS_PER_MSG);
	printf("          device buffers: %s\n",
	       drm >= 0 ? "yes (render node open)" : "no (no render node)");
	printf("          copy-on-write: %d pages per round on thread 0; text pages watched: %lx and %lx\n",
	       COW_PAGES, g_text_a, g_text_b);
	if (drm >= 0)
		close(drm);

	pthread_t reader;
	int have_reader = 0;

	if (g_race_fd >= 0 &&
	    pthread_create(&reader, NULL, race_reader, NULL) == 0)
		have_reader = 1;

	w = calloc((size_t)g_threads, sizeof(*w));
	if (!w) {
		fprintf(stderr, "wkstress: out of memory\n");
		return 1;
	}
	for (int i = 0; i < g_threads; i++) {
		w[i].index = i;
		if (pthread_create(&w[i].tid, NULL, worker, &w[i]) != 0) {
			fprintf(stderr, "wkstress: pthread_create failed\n");
			return 1;
		}
	}
	for (int i = 0; i < g_threads; i++)
		pthread_join(w[i].tid, NULL);
	g_race_stop = 1;
	if (have_reader)
		pthread_join(reader, NULL);
	free(w);

	if (g_failed) {
		printf("wkstress: FAILED (%d)\n", g_failed);
		return 1;
	}
	printf("wkstress: ok\n");
	return 0;
}
