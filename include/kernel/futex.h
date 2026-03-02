// LikeOS-64 Futex (Fast Userspace Mutex) Subsystem
// ============================================================================
// Hash-bucket implementation for scalable futex operations
// ============================================================================

#ifndef _KERNEL_FUTEX_H_
#define _KERNEL_FUTEX_H_

#include "types.h"
#include "sched.h"

// Number of hash buckets for futex wait queues
#define FUTEX_HASH_BUCKETS 256

// Futex operation codes (Linux-compatible)
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_FD            2   // Deprecated
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAKE_OP       5
#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_TRYLOCK_PI    8
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET   10

// Futex flags
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256

// Futex owner died flag (for robust mutexes)
#define FUTEX_OWNER_DIED    (1 << 30)
#define FUTEX_TID_MASK      0x3FFFFFFF

// ============================================================================
// FUTEX API
// ============================================================================

// Initialize the futex hash table (called once during kernel init)
void futex_init(void);

// Futex wait: block until futex value changes or timeout
// Returns: 0 on success, -EAGAIN if value changed, -ETIMEDOUT on timeout
int futex_wait(uint64_t uaddr, uint32_t expected_val, uint64_t timeout_ns);

// Futex wake: wake up to nr_wake waiters
// Returns: number of waiters woken
int futex_wake(uint64_t uaddr, int nr_wake);

// Futex requeue: wake some waiters and move others to a different futex
// Returns: total number of waiters processed (woken + requeued)
int futex_requeue(uint64_t uaddr, uint64_t uaddr2, int nr_wake, int nr_requeue);

// ============================================================================
// ROBUST FUTEX SUPPORT
// ============================================================================

// Robust futex list structures (Linux ABI compatible)
struct robust_list {
    struct robust_list* next;
};

struct robust_list_head {
    struct robust_list* list;
    long futex_offset;
    struct robust_list* list_op_pending;
};

// Process robust futex list on thread exit
// Marks owned futexes as FUTEX_OWNER_DIED and wakes waiters
void exit_robust_list(task_t* task);

#endif // _KERNEL_FUTEX_H_
