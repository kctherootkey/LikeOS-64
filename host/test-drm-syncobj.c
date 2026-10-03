/* Host test of synchronisation objects (kernel/dev/gpu/drm/drm_syncobj.c):
 * timelines on fence chains with no limit on pending points, lookups,
 * WAIT / TIMELINE_WAIT with WAIT_ALL, WAIT_FOR_SUBMIT, WAIT_AVAILABLE and
 * deadlines, the device-poll fallback, timeouts, QUERY, TRANSFER,
 * TIMELINE_SIGNAL, RESET, SIGNAL, kernel-side WAIT_FOR_SUBMIT lookups.
 *
 * The ioctls are called through drm_syncobj_ioctl() with the test's own
 * buffers standing for user memory.  A sleep runs a hook that plays the
 * other side (a submission, the device, the clock).  Every allocation is
 * counted, so each case ends by checking that everything was freed.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_fence_chain.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>

extern int printf(const char *, ...);
extern long test_allocs;
extern int test_warnings;
extern int test_locks_held;
extern int test_sleeps;
extern int test_timer_wakes;
extern int test_signal_pending;
extern uint64_t test_now;
extern void (*test_sleep_hook)(void);
int test_check_quarantine(void);

static int fails;
#define CHECK(cond, msg)                                              \
	do {                                                          \
		if (!(cond)) {                                        \
			printf("FAIL line %d: %s\n", __LINE__, msg); \
			fails++;                                      \
		}                                                     \
	} while (0)

/* The device: completes work only when asked, unless told otherwise. */
static int polls;
static uint32_t poll_completes_upto; /* what the next poll reports done */
static int polls_until_complete; /* polls before it reports it */
static struct drm_device dev;

static void test_fence_poll(struct drm_device *d)
{
	polls++;
	if (polls_until_complete > 0 && --polls_until_complete == 0)
		drm_fence_signal_upto(d, poll_completes_upto);
}

static uint64_t last_deadline;
static void test_set_deadline(struct drm_device *d, struct drm_fence *f,
			      uint64_t deadline_ns)
{
	(void)d;
	(void)f;
	last_deadline = deadline_ns;
}

static struct drm_driver test_drv = {
	.name = "testgpu",
	.fence_poll = test_fence_poll,
	.fence_set_deadline = test_set_deadline,
};
static struct drm_file file;

static long ioctl(unsigned nr, void *arg)
{
	int handled = 0;
	long rc = drm_syncobj_ioctl(&dev, &file, nr, arg, 64, &handled);

	if (!handled)
		return -ENOTTY;
	return rc;
}

static uint32_t create(uint32_t flags)
{
	struct drm_syncobj_create c = { .flags = flags };

	CHECK(ioctl(0xBF, &c) == 0, "CREATE");
	return c.handle;
}

static void destroy(uint32_t h)
{
	struct drm_syncobj_destroy d = { .handle = h };

	CHECK(ioctl(0xC0, &d) == 0, "DESTROY");
}

/* A device fence not yet passed. */
static struct drm_fence *pending_fence(void)
{
	return drm_fence_create(&dev, ++dev.fence_seq, 0);
}

static void add_point(uint32_t h, struct drm_fence *f, uint64_t point)
{
	struct drm_syncobj *so = drm_syncobj_lookup(&file, h);

	CHECK(so != NULL, "lookup");
	CHECK(drm_syncobj_add_point(so, f, point) == 0, "add_point");
	drm_syncobj_put(so);
}

static uint64_t query(uint32_t h, uint32_t flags)
{
	uint64_t point = ~0ULL;
	struct drm_syncobj_timeline_array q = {
		.handles = (uint64_t)(uintptr_t)&h,
		.points = (uint64_t)(uintptr_t)&point,
		.count_handles = 1,
		.flags = flags,
	};

	CHECK(ioctl(0xCB, &q) == 0, "QUERY");
	return point;
}

static long timeline_wait(uint32_t *h, uint64_t *points, uint32_t n,
			  uint32_t flags, int64_t timeout, uint32_t *first)
{
	struct drm_syncobj_timeline_wait w = {
		.handles = (uint64_t)(uintptr_t)h,
		.points = (uint64_t)(uintptr_t)points,
		.timeout_nsec = timeout,
		.count_handles = n,
		.flags = flags,
		.deadline_nsec = 777,
	};
	long rc = ioctl(0xCA, &w);

	if (first)
		*first = w.first_signaled;
	return rc;
}

static void check_clean(const char *what)
{
	/* the file's handle table stays until the file is released */
	if (test_allocs != (file.syncobjs ? 1 : 0)) {
		printf("FAIL: %s leaves %ld allocation(s)\n", what, test_allocs);
		fails++;
	}
	if (test_locks_held != 0) {
		printf("FAIL: %s leaves %d lock(s) held\n", what, test_locks_held);
		fails++;
	}
	if (dev.fences) {
		printf("FAIL: %s leaves device fences alive\n", what);
		fails++;
	}
	test_sleep_hook = NULL;
	test_signal_pending = 0;
	polls_until_complete = 0;
}

static void complete_all(void)
{
	drm_fence_signal_upto(&dev, dev.fence_seq);
}

/* ---- timelines ------------------------------------------------------- */

static void test_timeline_unbounded(void)
{
	uint32_t h = create(0);
	struct drm_fence *f;
	struct drm_syncobj *so;

	/* far more points than the old fixed table held */
	for (uint64_t p = 1; p <= 1000; p++) {
		f = pending_fence();
		add_point(h, f, p);
		drm_fence_put(f);
	}
	CHECK(query(h, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) == 1000,
	      "last submitted is 1000");
	CHECK(query(h, 0) == 0, "nothing signalled yet");

	/* the device passes the first 500 */
	drm_fence_signal_upto(&dev, dev.fence_seq - 500);
	CHECK(query(h, 0) == 500, "500 signalled");

	so = drm_syncobj_lookup(&file, h);
	CHECK(drm_syncobj_find_fence(so, 300, &f) == 0 && f && f->signaled,
	      "a passed point gives a signalled fence");
	CHECK(f && f->dev == &dev, "the signalled fence is the device's");
	drm_fence_put(f);
	CHECK(drm_syncobj_find_fence(so, 700, &f) == 0 && f && !f->signaled,
	      "a pending point gives its link");
	CHECK(f && f->seqno64 == 700, "the link of point 700");
	drm_fence_put(f);
	CHECK(drm_syncobj_find_fence(so, 1001, &f) == -ENOENT && !f,
	      "a point beyond the newest is not there");
	drm_syncobj_put(so);

	complete_all();
	CHECK(query(h, 0) == 1000, "all signalled");
	destroy(h);
	check_clean("unbounded timeline");
}

static void test_binary_and_signal(void)
{
	uint32_t a = create(0), b = create(DRM_SYNCOBJ_CREATE_SIGNALED);
	uint32_t hs[2] = { a, b };
	uint32_t first = 99;
	struct drm_syncobj_wait w = {
		.handles = (uint64_t)(uintptr_t)hs,
		.count_handles = 2,
	};
	struct drm_syncobj_array arr = {
		.handles = (uint64_t)(uintptr_t)&a,
		.count_handles = 1,
	};

	/* an empty object without WAIT_FOR_SUBMIT: -EINVAL */
	CHECK(ioctl(0xC3, &w) == -EINVAL, "WAIT on an empty object");
	w.handles = (uint64_t)(uintptr_t)&hs[1];
	w.count_handles = 1;
	CHECK(ioctl(0xC3, &w) == 0 && w.first_signaled == 0,
	      "WAIT on a created-signalled object");
	CHECK(ioctl(0xC5, &arr) == 0, "SIGNAL");
	w.handles = (uint64_t)(uintptr_t)hs;
	w.count_handles = 2;
	w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	CHECK(ioctl(0xC3, &w) == 0, "WAIT_ALL on two signalled objects");
	CHECK(ioctl(0xC4, &arr) == 0, "RESET");
	CHECK(ioctl(0xC3, &w) == -EINVAL, "WAIT after RESET");
	CHECK(timeline_wait(hs, NULL, 2, 0, 0, &first) == -EINVAL,
	      "wait-any with an empty object, no WAIT_FOR_SUBMIT");
	CHECK(timeline_wait(hs, NULL, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0,
			    &first) == 0 && first == 1,
	      "wait-any names the signalled one");
	arr.pad = 1;
	CHECK(ioctl(0xC4, &arr) == -EINVAL, "RESET with padding");
	arr.pad = 0;
	arr.count_handles = 0;
	CHECK(ioctl(0xC5, &arr) == -EINVAL, "SIGNAL of nothing");
	w.count_handles = 0;
	CHECK(ioctl(0xC3, &w) == 0, "WAIT for nothing");
	destroy(a);
	destroy(b);
	{
		struct drm_syncobj_destroy d = { .handle = a };
		CHECK(ioctl(0xC0, &d) == -EINVAL, "DESTROY twice");
	}
	{
		struct drm_syncobj_eventfd e = { .handle = b };
		CHECK(ioctl(0xCF, &e) == -ENOENT, "EVENTFD of an unknown handle");
	}
	check_clean("binary objects");
}

/* ---- waiting --------------------------------------------------------- */

static uint32_t hook_handle;
static int hook_step;
static struct drm_fence *hook_fence;

/* First sleep: a submission attaches point 5.  Second sleep: the device
 * completes it, through its interrupt (the fence is signalled directly). */
static void hook_submit_then_complete(void)
{
	hook_step++;
	if (hook_step == 1) {
		hook_fence = pending_fence();
		add_point(hook_handle, hook_fence, 5);
	} else if (hook_step == 2) {
		drm_fence_signal(hook_fence);
		drm_fence_put(hook_fence);
		hook_fence = NULL;
	}
}

static void test_wait_for_submit(void)
{
	uint32_t h = create(0);
	uint64_t point = 5;
	int64_t far = (int64_t)test_now + 1000000000LL;

	CHECK(timeline_wait(&h, &point, 1, 0, far, NULL) == -EINVAL,
	      "point not submitted, no WAIT_FOR_SUBMIT");

	hook_handle = h;
	hook_step = 0;
	test_sleep_hook = hook_submit_then_complete;
	test_sleeps = 0;
	test_timer_wakes = 0;
	CHECK(timeline_wait(&h, &point, 1,
			    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, far, NULL) == 0,
	      "WAIT_FOR_SUBMIT returns once submitted and signalled");
	CHECK(hook_step == 2, "slept until the submission, then until completion");
	CHECK(test_timer_wakes == 0,
	      "woken by the submission and by the fence, not by the timer");
	CHECK(query(h, 0) == 5, "point 5 signalled");
	destroy(h);
	check_clean("wait for submit");
}

static void hook_submit_only(void)
{
	hook_step++;
	if (hook_step == 1) {
		hook_fence = pending_fence();
		add_point(hook_handle, hook_fence, 3);
		drm_fence_put(hook_fence);
		hook_fence = NULL;
	}
}

static void test_wait_available(void)
{
	uint32_t h = create(0);
	uint64_t point = 3;
	int64_t far = (int64_t)test_now + 1000000000LL;

	hook_handle = h;
	hook_step = 0;
	test_sleep_hook = hook_submit_only;
	test_timer_wakes = 0;
	CHECK(timeline_wait(&h, &point, 1,
			    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, far, NULL) == 0,
	      "WAIT_AVAILABLE returns once the point is there");
	CHECK(hook_step == 1 && test_timer_wakes == 0,
	      "one sleep, ended by the submission");
	CHECK(query(h, 0) == 0, "and the point is still pending");
	/* a point below the newest is available too */
	point = 2;
	CHECK(timeline_wait(&h, &point, 1,
			    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0, NULL) == 0,
	      "a lower point is available");
	complete_all();
	destroy(h);
	check_clean("wait available");
}

static void hook_advance_clock(void)
{
	test_now += 100000000ULL; /* 100 ms per sleep */
}

static void test_timeout_and_signal(void)
{
	uint32_t h = create(0);
	struct drm_fence *f = pending_fence();
	int64_t soon = (int64_t)test_now + 350000000LL;

	add_point(h, f, 0);
	test_sleep_hook = hook_advance_clock;
	test_sleeps = 0;
	CHECK(timeline_wait(&h, NULL, 1, 0, soon, NULL) == -ETIME,
	      "a fence that never signals times out");
	CHECK(test_sleeps >= 3 && test_sleeps <= 5, "slept until the timeout");
	CHECK(timeline_wait(&h, NULL, 1, 0, 0, NULL) == -ETIME,
	      "a zero timeout only looks");

	test_signal_pending = 1;
	CHECK(timeline_wait(&h, NULL, 1, 0, (int64_t)test_now + 1000000000LL,
			    NULL) == -ERESTARTSYS,
	      "a signal ends the wait");
	test_signal_pending = 0;

	drm_fence_signal(f);
	drm_fence_put(f);
	destroy(h);
	check_clean("timeout");
}

/* The device only notices completion when polled: the third poll does. */
static void test_poll_fallback(void)
{
	uint32_t h[2] = { create(0), create(0) };
	struct drm_fence *f1 = pending_fence(), *f2 = pending_fence();
	uint64_t pts[2] = { 1, 0 };
	uint32_t first = 0;

	add_point(h[0], f1, 1);
	add_point(h[1], f2, 0);
	polls = 0;
	poll_completes_upto = f2->seqno;
	polls_until_complete = 3;
	test_sleeps = 0;
	test_timer_wakes = 0;
	last_deadline = 0;
	CHECK(timeline_wait(h, pts, 2,
			    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE,
			    (int64_t)test_now + 1000000000LL, &first) == 0,
	      "the poll completes both");
	CHECK(polls == 3 && test_sleeps == 2, "polled once per round, slept between");
	CHECK(test_timer_wakes == 2, "the poll timer is what woke it");
	CHECK(last_deadline == 777, "the deadline reached the driver");
	drm_fence_put(f1);
	drm_fence_put(f2);
	destroy(h[0]);
	destroy(h[1]);
	check_clean("poll fallback");
}

static void test_kernel_wait_for_submit(void)
{
	uint32_t h = create(0);
	struct drm_fence *f = NULL;

	CHECK(drm_syncobj_find_fence_handle(&file, h, 5, 0, &f) == -EINVAL && !f,
	      "no fence without waiting");
	CHECK(drm_syncobj_find_fence_handle(&file, h + 100, 5, 0, &f) == -ENOENT,
	      "unknown handle");
	CHECK(drm_syncobj_find_fence_handle(&file, h, 5, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
					    &f) == -EINVAL,
	      "only WAIT_FOR_SUBMIT is a valid flag");
	hook_handle = h;
	hook_step = 0;
	test_sleep_hook = hook_submit_then_complete;
	test_timer_wakes = 0;
	CHECK(drm_syncobj_find_fence_handle(&file, h, 5,
					    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
					    &f) == 0 && f,
	      "WAIT_FOR_SUBMIT lookup gets the fence once submitted");
	CHECK(hook_step == 1, "and does not wait for it to signal");
	CHECK(test_timer_wakes == 0, "woken by the submission");
	CHECK(f && drm_fence_is_chain(f) && f->seqno64 == 5, "the link of point 5");
	drm_fence_put(f);
	f = NULL;
	test_sleep_hook = hook_advance_clock;
	CHECK(drm_syncobj_find_fence_handle(&file, h, 9,
					    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
					    &f) == -ETIME && !f,
	      "WAIT_FOR_SUBMIT lookup gives up after five seconds");
	test_sleep_hook = NULL;
	drm_fence_signal(hook_fence);
	drm_fence_put(hook_fence);
	hook_fence = NULL;
	destroy(h);
	check_clean("kernel wait for submit");
}

/* ---- transfer and timeline signal ------------------------------------ */

static void test_transfer(void)
{
	uint32_t bin = create(0), tl = create(0), bin2 = create(0);
	struct drm_fence *f = pending_fence();
	struct drm_syncobj_transfer t = { 0 };
	uint64_t point;

	add_point(bin, f, 0);
	/* binary -> timeline point 4 */
	t.src_handle = bin;
	t.dst_handle = tl;
	t.dst_point = 4;
	CHECK(ioctl(0xCC, &t) == 0, "TRANSFER to a timeline");
	CHECK(query(tl, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) == 4, "point 4 there");
	CHECK(query(tl, 0) == 0, "and pending");
	/* timeline point 4 -> binary */
	t.src_handle = tl;
	t.src_point = 4;
	t.dst_handle = bin2;
	t.dst_point = 0;
	CHECK(ioctl(0xCC, &t) == 0, "TRANSFER to a binary object");
	point = 0;
	CHECK(timeline_wait(&bin2, &point, 1, 0, 0, NULL) == -ETIME, "pending in the copy");
	/* a missing source point */
	t.src_point = 9;
	CHECK(ioctl(0xCC, &t) == -EINVAL, "TRANSFER of a point not there");
	t.pad = 1;
	CHECK(ioctl(0xCC, &t) == -EINVAL, "TRANSFER with padding");

	drm_fence_signal(f);
	drm_fence_put(f);
	CHECK(query(tl, 0) == 4, "signalled through the transfer");
	CHECK(timeline_wait(&bin2, &point, 1, 0, 0, NULL) == 0, "the copy too");
	destroy(bin);
	destroy(tl);
	destroy(bin2);
	check_clean("transfer");
}

static void test_timeline_signal(void)
{
	uint32_t h[2] = { create(0), create(0) };
	uint64_t pts[2] = { 10, 20 };
	struct drm_syncobj_timeline_array s = {
		.handles = (uint64_t)(uintptr_t)h,
		.points = (uint64_t)(uintptr_t)pts,
		.count_handles = 2,
	};

	CHECK(ioctl(0xCD, &s) == 0, "TIMELINE_SIGNAL");
	CHECK(query(h[0], 0) == 10 && query(h[1], 0) == 20, "both points signalled");
	pts[0] = 5;
	pts[1] = 20;
	CHECK(timeline_wait(h, pts, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, NULL) == 0,
	      "waits on passed points end at once");
	s.flags = 1;
	CHECK(ioctl(0xCD, &s) == -EINVAL, "TIMELINE_SIGNAL with flags");
	destroy(h[0]);
	destroy(h[1]);
	check_clean("timeline signal");
}

int main(void)
{
	spinlock_init(&dev.lock, "test_dev");
	wq_head_init(&dev.vbl_wq, "vbl");
	dev.drv = &test_drv;
	dev.fence_passed = 100;
	dev.fence_seq = 100;
	spinlock_init(&file.lock, "test_file");
	file.dev = &dev;

	test_timeline_unbounded();
	test_binary_and_signal();
	test_wait_for_submit();
	test_wait_available();
	test_timeout_and_signal();
	test_poll_fallback();
	test_kernel_wait_for_submit();
	test_transfer();
	test_timeline_signal();

	drm_syncobj_file_release(&file);
	check_clean("file release");
	if (test_check_quarantine())
		fails++;
	if (test_warnings) {
		printf("FAIL: %d warning(s)\n", test_warnings);
		fails++;
	}
	if (fails) {
		printf("%d check(s) FAILED\n", fails);
		return 1;
	}
	printf("drm syncobj: all checks passed\n");
	return 0;
}
