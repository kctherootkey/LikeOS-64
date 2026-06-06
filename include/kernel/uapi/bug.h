// LikeOS-64 — Central debug / assertion infrastructure
//
// Single place to define all kernel debugging macros.  Include this header
// (directly or via a per-subsystem header) in every kernel .c file so the
// assertions are uniformly available across the whole tree.
//
// Provided facilities:
//
//   BUG()                     Unconditional fatal kernel BUG (ud2 trap).
//   BUG_ON(cond)              Fatal if cond is true.
//   WARN_ON(cond)             Print warning and continue; evaluates to cond.
//   WARN(cond, fmt, ...)      Warn with a custom message; evaluates to cond.
//   WARN_ON_ONCE(cond)        Like WARN_ON but fires at most once per site.
//   WARN_RATELIMIT(cond, ...) Rate-limited warning (≤ 10 per call site).
//   VM_BUG_ON(cond)           MM-specific fatal assertion (DEBUG builds only).
//   VM_WARN_ON(cond)          MM-specific non-fatal warning (DEBUG only).
//   BUILD_BUG_ON(cond)        Compile-time assertion.
//   lockdep_assert_held(lk)   Assert spinlock is held (DEBUG only).
//   might_sleep()             Assert sleeping is legal here (DEBUG only).
//   irqs_disabled()           True when local IRQ-enable flag is clear.
//   refcount_t                Hardened reference-counter type.
//   refcount_set/read/inc/dec_and_test/inc_not_zero  Refcount operations.
//   likely(x) / unlikely(x)  Branch-prediction hints.

#ifndef _KERNEL_BUG_H_
#define _KERNEL_BUG_H_

#include <kernel/uapi/types.h>
#include <kernel/io/console.h>    /* kprintf() */

// panic() is declared in interrupt.h; forward-declared here so bug.h can be
// included without dragging in the full interrupt header.
extern void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

// ============================================================================
// Branch-prediction hints
// ============================================================================
#ifndef likely
# define likely(x)   __builtin_expect(!!(x), 1)
# define unlikely(x) __builtin_expect(!!(x), 0)
#endif

// ============================================================================
// BUG() / BUG_ON() — unrecoverable kernel bugs
//
// Prints location information, then raises an invalid-opcode fault (ud2) so
// the CPU generates a #UD exception that is immediately visible in any
// debugger or serial log.  __builtin_unreachable() tells the compiler that
// control never continues past this point.
// ============================================================================
#define BUG() do {                                                              \
    kprintf("BUG: kernel BUG at %s:%d in %s()!\n",                             \
            __FILE__, __LINE__, __func__);                                      \
    __asm__ volatile("ud2");                                                    \
    __builtin_unreachable();                                                    \
} while (0)

#define BUG_ON(cond) do {                                                       \
    if (unlikely(cond)) {                                                       \
        kprintf("BUG: condition (%s) true at %s:%d in %s()!\n",                \
                #cond, __FILE__, __LINE__, __func__);                           \
        __asm__ volatile("ud2");                                                \
        __builtin_unreachable();                                                \
    }                                                                           \
} while (0)

// ============================================================================
// WARN_ON / WARN — non-fatal warnings, execution continues
//
// Both macros evaluate to the boolean value of the condition so that callers
// can write:
//     if (WARN_ON(ptr == NULL)) return -EINVAL;
// ============================================================================
#define WARN_ON(cond) ({                                                        \
    int __ret_warn_on = !!(cond);                                               \
    if (unlikely(__ret_warn_on))                                                \
        kprintf("WARNING: at %s:%d %s()\n", __FILE__, __LINE__, __func__);     \
    __ret_warn_on;                                                              \
})

#define WARN(cond, fmt, ...) ({                                                 \
    int __ret_warn = !!(cond);                                                  \
    if (unlikely(__ret_warn))                                                   \
        kprintf("WARNING: at %s:%d %s(): " fmt "\n",                           \
                __FILE__, __LINE__, __func__, ##__VA_ARGS__);                   \
    __ret_warn;                                                                 \
})

// ============================================================================
// WARN_ON_ONCE — fire at most once per call site (silences log spam)
// ============================================================================
#define WARN_ON_ONCE(cond) ({                                                   \
    static int __warned_once = 0;                                               \
    int __ret_once = !!(cond);                                                  \
    if (unlikely(__ret_once && !__warned_once)) {                               \
        __warned_once = 1;                                                      \
        kprintf("WARNING (once): at %s:%d %s()\n",                             \
                __FILE__, __LINE__, __func__);                                  \
    }                                                                           \
    __ret_once;                                                                 \
})

// ============================================================================
// WARN_RATELIMIT — rate-limited warning (at most 10 messages per call site)
//
// Useful in hot paths (interrupt handlers, network RX) where a single bad
// condition can generate thousands of log lines per second.
// ============================================================================
#define WARN_RATELIMIT(cond, fmt, ...) ({                                       \
    int __ret_rl = !!(cond);                                                    \
    if (unlikely(__ret_rl)) {                                                   \
        static int __rl_count = 0;                                              \
        if (__rl_count < 10) {                                                  \
            ++__rl_count;                                                       \
            kprintf("WARNING (ratelimit %d/10): at %s:%d %s(): " fmt "\n",     \
                    __rl_count, __FILE__, __LINE__, __func__, ##__VA_ARGS__);   \
            if (__rl_count == 10)                                               \
                kprintf("WARNING (ratelimit): further occurrences suppressed"   \
                        " at %s:%d\n", __FILE__, __LINE__);                     \
        }                                                                       \
    }                                                                           \
    __ret_rl;                                                                   \
})

// ============================================================================
// VM_BUG_ON / VM_WARN_ON — MM-specific assertions
//
// VM_BUG_ON is fatal only in DEBUG builds; in production it compiles away.
// Prefer VM_WARN_ON for conditions that are suspicious but not immediately
// fatal to a running system.
// ============================================================================
#ifdef DEBUG
# define VM_BUG_ON(cond)  BUG_ON(cond)
# define VM_WARN_ON(cond) WARN_ON(cond)
#else
# define VM_BUG_ON(cond)  do { (void)(cond); } while (0)
# define VM_WARN_ON(cond) do { (void)(cond); } while (0)
#endif

// ============================================================================
// BUILD_BUG_ON — compile-time assertion
//
// Turns a logical error into a compilation failure so it is caught before
// the kernel ever boots.  Use for struct layout constraints, array-size
// assumptions, and similar static invariants.
// ============================================================================
#define BUILD_BUG_ON(cond) _Static_assert(!(cond), "BUILD_BUG_ON failed: " #cond)

// ============================================================================
// irqs_disabled() — query the local CPU's interrupt-enable flag
// ============================================================================
static inline int irqs_disabled(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return !(flags & (1ULL << 9));   /* bit 9 = EFLAGS.IF */
}

// ============================================================================
// might_sleep() — assert that sleeping is legal at this point (DEBUG only)
//
// Catches blocking operations called from atomic context (IRQs disabled).
// ============================================================================
#ifdef DEBUG
# define might_sleep() do {                                                     \
    if (unlikely(irqs_disabled()))                                              \
        kprintf("WARNING: might_sleep() called with IRQs disabled"              \
                " at %s:%d %s()\n", __FILE__, __LINE__, __func__);              \
} while (0)
#else
# define might_sleep() do { } while (0)
#endif

// ============================================================================
// lockdep_assert_held() — assert that a spinlock is currently held (DEBUG)
//
// The spinlock_t type must be in scope at each call site; this header does
// not include sched.h to avoid circular-include issues.  All callers that
// use spinlocks already include sched.h.
// ============================================================================
#ifdef DEBUG
# define lockdep_assert_held(lock) do {                                         \
    if (unlikely(!((lock)->locked)))                                            \
        kprintf("WARNING: lockdep_assert_held(%s) FAILED at %s:%d %s()\n",     \
                #lock, __FILE__, __LINE__, __func__);                           \
} while (0)
#else
# define lockdep_assert_held(lock) do { (void)(lock); } while (0)
#endif

// ============================================================================
// refcount_t — hardened reference counter
//
// Wraps a volatile int with atomic operations and overflow / underflow
// detection.  Use in place of raw integer counters wherever object lifetime
// is tracked by reference counting.
// ============================================================================
typedef struct {
    volatile int counter;
} refcount_t;

#define REFCOUNT_INIT(n)  { .counter = (n) }

static inline void refcount_set(refcount_t *r, int n)
{
    __atomic_store_n(&r->counter, n, __ATOMIC_RELAXED);
}

static inline int refcount_read(const refcount_t *r)
{
    return __atomic_load_n(&r->counter, __ATOMIC_RELAXED);
}

// Increment the reference count.
// Warns if incrementing from zero — this indicates a use-after-free.
static inline void refcount_inc(refcount_t *r)
{
    int old = __atomic_fetch_add(&r->counter, 1, __ATOMIC_RELAXED);
    WARN(old <= 0, "refcount_inc on zero/negative counter (use-after-free?)");
}

// Decrement the reference count and return non-zero if it reached zero.
// The caller must free the object when the return value is non-zero.
// Warns if the counter goes negative (double-free / unbalanced put).
static inline int refcount_dec_and_test(refcount_t *r)
{
    int n = __atomic_sub_fetch(&r->counter, 1, __ATOMIC_RELEASE);
    if (unlikely(n < 0)) {
        WARN(1, "refcount_dec_and_test went negative (double-free?)");
        __atomic_store_n(&r->counter, 0, __ATOMIC_RELAXED);   /* clamp */
        return 0;
    }
    return n == 0;
}

// Increment only if the counter is currently non-zero.
// Returns non-zero on success.  Safe against concurrent teardown.
static inline int refcount_inc_not_zero(refcount_t *r)
{
    int old, desired;
    do {
        old = __atomic_load_n(&r->counter, __ATOMIC_RELAXED);
        if (old <= 0)
            return 0;
        desired = old + 1;
    } while (!__atomic_compare_exchange_n(&r->counter, &old, desired,
                                          /* weak */ 1,
                                          __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));
    return 1;
}

#endif /* _KERNEL_BUG_H_ */
