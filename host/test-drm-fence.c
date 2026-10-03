/* Host test of the fence library: callbacks, arrays, chains, unwrapping
 * and merging, reservation objects (kernel/dev/gpu/drm/drm_fence.c,
 * drm_fence_array.c, drm_fence_chain.c, drm_fence_unwrap.c, drm_resv.c).
 *
 * Every allocation is counted, so each case ends by checking that
 * dropping the last references freed everything; the spinlocks of the
 * prelude abort on recursion, which in the kernel would be a deadlock.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_fence_array.h>
#include <kernel/dev/gpu/drm_fence_chain.h>
#include <kernel/dev/gpu/drm_fence_unwrap.h>
#include <kernel/dev/gpu/drm_resv.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>

extern int printf(const char *, ...);
extern long test_allocs;
extern int test_warnings;
extern int test_locks_held;
extern uint64_t test_irq_flag;
int test_check_quarantine(void);

static int fails;
#define CHECK(cond, msg)                                              \
	do {                                                          \
		if (!(cond)) {                                        \
			printf("FAIL line %d: %s\n", __LINE__, msg); \
			fails++;                                      \
		}                                                     \
	} while (0)

static int polls;
static void test_fence_poll(struct drm_device *dev)
{
	(void)dev;
	polls++;
}

static uint64_t last_deadline;
static void test_set_deadline(struct drm_device *dev, struct drm_fence *f,
			      uint64_t deadline_ns)
{
	(void)dev;
	(void)f;
	last_deadline = deadline_ns;
}

static struct drm_driver test_drv = {
	.name = "testgpu",
	.fence_poll = test_fence_poll,
	.fence_set_deadline = test_set_deadline,
};
static struct drm_device dev;

/* A callback that records how it was called. */
struct rec_cb {
	struct drm_fence_cb cb;
	int calls;
	int irq_was_on;
	int locks_held;
};

static void rec_func(struct drm_fence *f, struct drm_fence_cb *cb)
{
	struct rec_cb *r = (struct rec_cb *)cb;
	(void)f;
	r->calls++;
	r->irq_was_on = test_irq_flag != 0;
	r->locks_held = test_locks_held;
}

static void check_clean(const char *what)
{
	if (test_allocs != 0) {
		printf("FAIL: %s leaves %ld allocation(s)\n", what, test_allocs);
		fails++;
	}
	if (test_locks_held != 0) {
		printf("FAIL: %s leaves %d lock(s) held\n", what, test_locks_held);
		fails++;
	}
	if (dev.fences) {
		printf("FAIL: %s leaves fences on the device list\n", what);
		fails++;
	}
	test_allocs = 0;
}

/* Device-wide sequence fences: callbacks, removal, status, timestamps. */
static void test_callbacks(void)
{
	struct rec_cb a = { 0 }, b = { 0 }, c = { 0 };
	struct drm_fence *f = drm_fence_create(&dev, dev.fence_passed + 1, 0);

	CHECK(f && !f->signaled, "a fresh fence is pending");
	CHECK(drm_fence_get_status(f) == 0, "status pending");
	CHECK(drm_fence_add_callback(f, &a.cb, rec_func) == 0, "add a");
	CHECK(drm_fence_add_callback(f, &b.cb, rec_func) == 0, "add b");
	CHECK(drm_fence_remove_callback(f, &b.cb), "b removed before signal");
	CHECK(!drm_fence_remove_callback(f, &b.cb), "b removed twice: false");
	drm_fence_set_deadline(f, 12345);
	CHECK(last_deadline == 12345, "deadline reaches the driver");
	drm_fence_signal_upto(&dev, dev.fence_passed + 1);
	CHECK(f->signaled, "signal_upto signals");
	CHECK(a.calls == 1, "a called once");
	CHECK(b.calls == 0, "b removed: not called");
	CHECK(!a.irq_was_on, "callbacks run with interrupts off");
	CHECK(a.locks_held == 0, "callbacks run with no lock held");
	CHECK(!drm_fence_remove_callback(f, &a.cb), "a already ran: false");
	CHECK(drm_fence_add_callback(f, &c.cb, rec_func) == -ENOENT,
	      "adding to a signalled fence: -ENOENT");
	CHECK(c.calls == 0, "and it is not called");
	CHECK(drm_fence_get_status(f) == 1, "status signalled");
	CHECK(drm_fence_timestamp(f) != 0, "timestamped");
	CHECK(drm_fence_signal_timestamp(f, 1) == -EINVAL, "signal twice: -EINVAL");
	drm_fence_put(f);

	/* error, explicit timestamp, individual signal */
	f = drm_fence_create_ctx(&dev, 0x1001, 7);
	CHECK(drm_fence_add_callback(f, &a.cb, rec_func) == 0, "add on ctx fence");
	drm_fence_set_error(f, -EIO);
	CHECK(drm_fence_signal_timestamp(f, 777) == 0, "signal with timestamp");
	CHECK(a.calls == 2, "called on individual signal");
	CHECK(drm_fence_timestamp(f) == 777, "timestamp kept");
	CHECK(drm_fence_get_status(f) == -EIO, "status carries the error");
	drm_fence_put(f);
	check_clean("callbacks");
}

/* An array over three stream fences. */
static void test_array(void)
{
	struct drm_fence **v = kcalloc(3, sizeof(*v));
	struct rec_cb r = { 0 };

	for (int i = 0; i < 3; i++)
		v[i] = drm_fence_create_ctx(&dev, 0x2000 + i, 1);
	struct drm_fence *keep1 = drm_fence_ref(v[1]);
	struct drm_fence *keep2 = drm_fence_ref(v[2]);
	struct drm_fence *keep0 = drm_fence_ref(v[0]);
	struct drm_fence_array *arr = drm_fence_array_create(3, v, drm_fence_context_alloc(1), 1, false);
	CHECK(arr != NULL, "array created");
	struct drm_fence *af = &arr->base;
	CHECK(drm_fence_is_array(af) && drm_fence_is_container(af), "is an array");
	CHECK(!drm_fence_is_signaled(af), "pending");
	CHECK(drm_fence_add_callback(af, &r.cb, rec_func) == 0, "callback on the array");
	drm_fence_set_error(keep1, -ENODEV);
	drm_fence_signal(keep1);
	drm_fence_signal(keep0);
	CHECK(!af->signaled && r.calls == 0, "two of three: still pending");
	drm_fence_signal(keep2);
	CHECK(af->signaled && r.calls == 1, "all three: signalled, callback ran");
	CHECK(drm_fence_get_status(af) == -ENODEV, "array carries the first error");
	drm_fence_put(keep0);
	drm_fence_put(keep1);
	drm_fence_put(keep2);
	drm_fence_put(af);
	check_clean("array");

	/* signal on any, never watched: asked */
	v = kcalloc(2, sizeof(*v));
	v[0] = drm_fence_create_ctx(&dev, 0x2100, 1);
	v[1] = drm_fence_create_ctx(&dev, 0x2101, 1);
	keep1 = drm_fence_ref(v[1]);
	arr = drm_fence_array_create(2, v, drm_fence_context_alloc(1), 1, true);
	af = &arr->base;
	CHECK(!drm_fence_is_signaled(af), "any: pending");
	drm_fence_signal(keep1);
	CHECK(drm_fence_is_signaled(af), "any: one part is enough (counted)");
	CHECK(drm_fence_get_status(af) == 1, "any: no error");
	drm_fence_put(keep1);
	drm_fence_put(af);
	check_clean("array any");

	/* signal on any, watched; the other part signals after the array is gone */
	v = kcalloc(2, sizeof(*v));
	v[0] = drm_fence_create_ctx(&dev, 0x2200, 1);
	v[1] = drm_fence_create_ctx(&dev, 0x2201, 1);
	keep0 = drm_fence_ref(v[0]);
	keep1 = drm_fence_ref(v[1]);
	arr = drm_fence_array_create(2, v, drm_fence_context_alloc(1), 1, true);
	af = &arr->base;
	drm_fence_enable_signaling(af);
	drm_fence_signal(keep0);
	CHECK(af->signaled, "any watched: signalled by the callback");
	drm_fence_put(af); /* the other part's callback still holds it */
	CHECK(test_allocs > 2, "array alive while a callback is armed");
	drm_fence_signal(keep1);
	drm_fence_put(keep0);
	drm_fence_put(keep1);
	check_clean("array any watched");
}

/* A timeline of four points on one stream each. */
static void test_chain(void)
{
	struct drm_fence *f[5], *head = NULL;
	struct rec_cb r = { 0 };

	for (int i = 1; i <= 4; i++) {
		f[i] = drm_fence_create_ctx(&dev, 0x3000, i);
		struct drm_fence_chain *c = drm_fence_chain_alloc();
		drm_fence_chain_init(c, head, drm_fence_ref(f[i]), i);
		head = &c->base;
	}
	CHECK(head->seqno64 == 4, "head is point 4");
	CHECK(to_drm_fence_chain(head)->prev_seqno == 3, "prev point 3");

	struct drm_fence *p = drm_fence_ref(head);
	CHECK(drm_fence_chain_find_seqno(&p, 2) == 0, "find point 2");
	CHECK(p && drm_fence_chain_contained(p) == f[2], "point 2 is the link holding f2");
	drm_fence_put(p);
	p = drm_fence_ref(head);
	CHECK(drm_fence_chain_find_seqno(&p, 5) == -EINVAL, "point 5 is beyond the chain");
	drm_fence_put(p);

	CHECK(drm_fence_add_callback(head, &r.cb, rec_func) == 0, "watch the head");
	drm_fence_signal(f[4]);
	CHECK(!head->signaled, "head's own fence alone is not enough");
	drm_fence_signal(f[1]);
	drm_fence_signal(f[2]);
	p = drm_fence_ref(head);
	CHECK(drm_fence_chain_find_seqno(&p, 2) == 0 && p == NULL,
	      "point 2 signalled: nothing to wait for");
	CHECK(!head->signaled && r.calls == 0, "point 3 still pending");
	drm_fence_signal(f[3]);
	CHECK(head->signaled && r.calls == 1, "all points: head signalled");

	/* unwrap sees each contained fence once */
	struct drm_fence_unwrap it;
	struct drm_fence *x;
	int n = 0;
	drm_fence_unwrap_for_each(x, &it, head)
		n++;
	CHECK(n >= 1 && n <= 4, "unwrap walks the (collected) chain");

	for (int i = 1; i <= 4; i++)
		drm_fence_put(f[i]);
	drm_fence_put(head);
	check_clean("chain");

	/* a long chain is released without recursing per link */
	head = NULL;
	for (int i = 1; i <= 20000; i++) {
		struct drm_fence *g = drm_fence_create_ctx(&dev, 0x3100, i);
		struct drm_fence_chain *c = drm_fence_chain_alloc();
		drm_fence_chain_init(c, head, g, i);
		head = &c->base;
	}
	drm_fence_signal_upto_ctx(&dev, 0x3100, 20000);
	CHECK(drm_fence_is_signaled(head), "long chain signalled");
	drm_fence_put(head);
	check_clean("long chain");
}

static void test_merge(void)
{
	struct drm_fence *a = drm_fence_create_ctx(&dev, 0x4000, 5);
	struct drm_fence *a2 = drm_fence_create_ctx(&dev, 0x4000, 7);
	struct drm_fence *b = drm_fence_create_ctx(&dev, 0x4001, 1);
	struct drm_fence *s = drm_fence_create_ctx(&dev, 0x4002, 1);

	drm_fence_signal_timestamp(s, 5000);

	/* one pending fence: that fence itself */
	struct drm_fence *in1[2] = { a, s };
	struct drm_fence *m = drm_fence_unwrap_merge(2, in1);
	CHECK(m == a, "one pending: returned as is");
	drm_fence_put(m);

	/* a and a2 share a stream: only a2 stays */
	struct drm_fence **v = kcalloc(2, sizeof(*v));
	v[0] = drm_fence_ref(a);
	v[1] = drm_fence_ref(b);
	struct drm_fence_array *arr = drm_fence_array_create(2, v, drm_fence_context_alloc(1), 1, false);
	struct drm_fence *in2[3] = { &arr->base, a2, s };
	m = drm_fence_unwrap_merge(3, in2);
	CHECK(m && drm_fence_is_array(m), "several pending: an array");
	struct drm_fence_array *ma = to_drm_fence_array(m);
	CHECK(ma && ma->num_fences == 2, "deduplicated to two");
	int has_a2 = 0, has_b = 0, has_a = 0;
	for (unsigned i = 0; ma && i < ma->num_fences; i++) {
		has_a2 |= ma->fences[i] == a2;
		has_b |= ma->fences[i] == b;
		has_a |= ma->fences[i] == a;
	}
	CHECK(has_a2 && has_b && !has_a, "the later of a stream's fences kept");
	drm_fence_put(&arr->base);

	/* the merged-pair path with a container part */
	struct drm_fence *d = drm_fence_create(&dev, dev.fence_passed + 1, 0);
	struct drm_fence *mp = drm_fence_merge(d, m);
	CHECK(mp && mp->deps[0], "merged pair");
	drm_fence_signal_upto(&dev, dev.fence_passed + 1);
	drm_fence_signal(b);
	CHECK(!mp->signaled, "pair: array part still pending");
	drm_fence_signal(a2);
	CHECK(m->signaled, "the array signalled by its parts");
	CHECK(mp->signaled, "pair signalled once the array has");
	drm_fence_put(mp);
	drm_fence_put(d);
	drm_fence_put(m);

	/* everything signalled: a stub with the latest timestamp */
	drm_fence_signal(a);
	struct drm_fence *in3[2] = { a, s };
	m = drm_fence_unwrap_merge(2, in3);
	CHECK(m && m->signaled && drm_fence_timestamp(m) >= 5000, "all signalled: a stub");
	CHECK(m != a && m != s, "a new private fence");
	drm_fence_put(m);

	drm_fence_put(a);
	drm_fence_put(a2);
	drm_fence_put(b);
	drm_fence_put(s);
	check_clean("merge");
}

static void test_resv(void)
{
	struct drm_resv obj;
	struct drm_fence *w = drm_fence_create_ctx(&dev, 0x5000, 1);
	struct drm_fence *w2 = drm_fence_create_ctx(&dev, 0x5000, 2);
	struct drm_fence *r = drm_fence_create_ctx(&dev, 0x5001, 1);
	struct drm_fence *k = drm_fence_create_ctx(&dev, 0x5002, 1);
	struct drm_fence *bk = drm_fence_create_ctx(&dev, 0x5003, 1);
	struct drm_resv_iter cur;
	struct drm_fence *f;
	int n;

	drm_resv_init(&obj);
	drm_resv_lock(&obj);
	CHECK(drm_resv_reserve_fences(&obj, 4) == 0, "reserve");
	drm_resv_add_fence(&obj, w, DRM_RESV_USAGE_WRITE);
	drm_resv_add_fence(&obj, r, DRM_RESV_USAGE_READ);
	drm_resv_add_fence(&obj, k, DRM_RESV_USAGE_KERNEL);
	drm_resv_add_fence(&obj, bk, DRM_RESV_USAGE_BOOKKEEP);
	drm_resv_unlock(&obj);

	n = 0;
	drm_resv_iter_begin(&cur, &obj, DRM_RESV_USAGE_WRITE);
	drm_resv_for_each_fence_unlocked(&cur, f)
		n++;
	drm_resv_iter_end(&cur);
	CHECK(n == 2, "WRITE: kernel and write fences");
	n = 0;
	drm_resv_iter_begin(&cur, &obj, DRM_RESV_USAGE_READ);
	drm_resv_for_each_fence_unlocked(&cur, f)
		n++;
	drm_resv_iter_end(&cur);
	CHECK(n == 3, "READ: not bookkeeping");
	CHECK(!drm_resv_test_signaled(&obj, DRM_RESV_USAGE_KERNEL), "kernel fence pending");

	/* the later fence of the write stream replaces the earlier */
	drm_resv_lock(&obj);
	CHECK(drm_resv_reserve_fences(&obj, 1) == 0, "reserve one more");
	drm_resv_add_fence(&obj, w2, DRM_RESV_USAGE_WRITE);
	n = 0;
	drm_resv_for_each_fence(&cur, &obj, DRM_RESV_USAGE_BOOKKEEP, f)
		n++;
	CHECK(n == 4, "replaced in place, not added");
	drm_resv_unlock(&obj);

	struct drm_fence *single = NULL;
	CHECK(drm_resv_get_singleton(&obj, DRM_RESV_USAGE_READ, &single) == 0 && single &&
	      drm_fence_is_array(single), "singleton of three is an array");
	drm_fence_signal(k);
	drm_fence_signal(r);
	drm_fence_signal(w2);
	CHECK(drm_fence_is_signaled(single), "singleton follows its parts");
	drm_fence_put(single);
	CHECK(drm_resv_test_signaled(&obj, DRM_RESV_USAGE_READ), "all but bookkeeping signalled");
	CHECK(!drm_resv_test_signaled(&obj, DRM_RESV_USAGE_BOOKKEEP), "bookkeeping pending");
	CHECK(drm_resv_wait_timeout(&obj, DRM_RESV_USAGE_READ, 1, 0) == 0, "nothing to wait for");

	/* growing leaves the signalled fences behind */
	drm_resv_lock(&obj);
	CHECK(drm_resv_reserve_fences(&obj, 8) == 0, "grow");
	n = 0;
	drm_resv_for_each_fence(&cur, &obj, DRM_RESV_USAGE_BOOKKEEP, f)
		n++;
	CHECK(n == 1, "only the pending fence moved over");
	drm_resv_unlock(&obj);

	drm_resv_fini(&obj);
	drm_fence_signal(bk);
	drm_fence_signal(w);
	drm_fence_put(w);
	drm_fence_put(w2);
	drm_fence_put(r);
	drm_fence_put(k);
	drm_fence_put(bk);
	check_clean("resv");
}

static void test_wait_any(void)
{
	struct drm_fence *f[2];
	uint32_t idx = 99;

	f[0] = drm_fence_create_ctx(&dev, 0x6000, 1);
	f[1] = drm_fence_create_ctx(&dev, 0x6001, 1);
	CHECK(drm_fence_wait_any_timeout(f, 2, 1, 0, &idx) == -ETIMEDOUT, "none: timed out");
	drm_fence_signal(f[1]);
	CHECK(drm_fence_wait_any_timeout(f, 2, 1, 1000000, &idx) == 0 && idx == 1,
	      "second signalled: index 1");
	drm_fence_put(f[0]);
	drm_fence_put(f[1]);
	check_clean("wait any");
}

int main(void)
{
	spinlock_init(&dev.lock, "test_dev");
	wq_head_init(&dev.vbl_wq, "vbl");
	dev.drv = &test_drv;
	dev.fence_passed = 100;

	test_callbacks();
	test_array();
	test_chain();
	test_merge();
	test_resv();
	test_wait_any();
	(void)polls;
	if (test_check_quarantine()) {
		printf("FAIL: memory written after it was freed\n");
		fails++;
	}
	if (test_warnings) {
		printf("FAIL: %d warning(s) printed\n", test_warnings);
		fails++;
	}
	if (fails) {
		printf("test-drm-fence: %d failure(s)\n", fails);
		return 1;
	}
	printf("test-drm-fence: all checks passed\n");
	return 0;
}
