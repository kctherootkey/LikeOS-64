/* C11 <threads.h> over pthreads. */
#include <threads.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

struct thrd_start {
	thrd_start_t func;
	void *arg;
};

static void *thrd_trampoline(void *p)
{
	struct thrd_start s = *(struct thrd_start *)p;
	free(p);
	return (void *)(intptr_t)s.func(s.arg);
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
	struct thrd_start *s = malloc(sizeof(*s));
	if (!s)
		return thrd_nomem;
	s->func = func;
	s->arg = arg;
	int rc = pthread_create(thr, NULL, thrd_trampoline, s);
	if (rc) {
		free(s);
		return rc == EAGAIN ? thrd_nomem : thrd_error;
	}
	return thrd_success;
}

int thrd_equal(thrd_t a, thrd_t b) { return pthread_equal(a, b); }
thrd_t thrd_current(void) { return pthread_self(); }
int thrd_sleep(const struct timespec *d, struct timespec *r)
{
	int rc = nanosleep(d, r);
	if (rc == 0)
		return 0;
	return errno == EINTR ? -1 : -2;
}
void thrd_yield(void) { sched_yield(); }
_Noreturn void thrd_exit(int res) { pthread_exit((void *)(intptr_t)res); }
int thrd_detach(thrd_t t) { return pthread_detach(t) ? thrd_error : thrd_success; }
int thrd_join(thrd_t t, int *res)
{
	void *r;
	if (pthread_join(t, &r))
		return thrd_error;
	if (res)
		*res = (int)(intptr_t)r;
	return thrd_success;
}

int mtx_init(mtx_t *m, int type)
{
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	if (type & mtx_recursive)
		pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	int rc = pthread_mutex_init(m, &a);
	pthread_mutexattr_destroy(&a);
	return rc ? thrd_error : thrd_success;
}
void mtx_destroy(mtx_t *m) { pthread_mutex_destroy(m); }
int mtx_lock(mtx_t *m) { return pthread_mutex_lock(m) ? thrd_error : thrd_success; }
int mtx_timedlock(mtx_t *m, const struct timespec *ts)
{
	int rc = pthread_mutex_timedlock(m, ts);
	if (rc == 0)
		return thrd_success;
	return rc == ETIMEDOUT ? thrd_timedout : thrd_error;
}
int mtx_trylock(mtx_t *m)
{
	int rc = pthread_mutex_trylock(m);
	if (rc == 0)
		return thrd_success;
	return rc == EBUSY ? thrd_busy : thrd_error;
}
int mtx_unlock(mtx_t *m) { return pthread_mutex_unlock(m) ? thrd_error : thrd_success; }

void call_once(once_flag *f, void (*func)(void)) { pthread_once(f, func); }

int cnd_init(cnd_t *c) { return pthread_cond_init(c, NULL) ? thrd_error : thrd_success; }
void cnd_destroy(cnd_t *c) { pthread_cond_destroy(c); }
int cnd_signal(cnd_t *c) { return pthread_cond_signal(c) ? thrd_error : thrd_success; }
int cnd_broadcast(cnd_t *c) { return pthread_cond_broadcast(c) ? thrd_error : thrd_success; }
int cnd_wait(cnd_t *c, mtx_t *m) { return pthread_cond_wait(c, m) ? thrd_error : thrd_success; }
int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *ts)
{
	int rc = pthread_cond_timedwait(c, m, ts);
	if (rc == 0)
		return thrd_success;
	return rc == ETIMEDOUT ? thrd_timedout : thrd_error;
}

int tss_create(tss_t *k, tss_dtor_t d) { return pthread_key_create(k, d) ? thrd_error : thrd_success; }
void tss_delete(tss_t k) { pthread_key_delete(k); }
void *tss_get(tss_t k) { return pthread_getspecific(k); }
int tss_set(tss_t k, void *v) { return pthread_setspecific(k, v) ? thrd_error : thrd_success; }
