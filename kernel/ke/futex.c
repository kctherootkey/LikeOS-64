// LikeOS-64 Futex (Fast Userspace Mutex) Subsystem
// ============================================================================
// Hash-bucket implementation for scalable futex operations.
//
// Futexes are the foundation for userspace synchronization primitives like
// pthread_mutex, condition variables, semaphores, and barriers.
//
// Architecture:
//   - 256-bucket hash table to reduce lock contention
//   - Per-bucket spinlock for waiters
//   - Physical address key for shared futexes, virtual+PML4 for private
//   - Robust futex support for pthread_mutex_t with PTHREAD_MUTEX_ROBUST
// ============================================================================

#include "../../include/kernel/futex.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/types.h"
#include "../../include/kernel/syscall.h"

// ============================================================================
// INTERNAL STRUCTURES
// ============================================================================

// Futex wait queue entry
typedef struct futex_waiter {
    uint64_t uaddr;              // User address being waited on
    uint64_t key;                // Hash key (physical address for shared, virtual for private)
    task_t* task;                // Waiting task
    struct futex_waiter* next;   // Next waiter in bucket
    uint32_t bitset;             // For FUTEX_WAIT_BITSET
} futex_waiter_t;

// Hash bucket
typedef struct futex_bucket {
    spinlock_t lock;
    futex_waiter_t* head;
} futex_bucket_t;

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Global futex hash table
static futex_bucket_t futex_hash[FUTEX_HASH_BUCKETS];
static bool futex_initialized = false;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Validate user pointer is in user space
static inline bool validate_user_ptr(uint64_t ptr, size_t len) {
    if (ptr < 0x10000) return false;  // Reject low addresses (NULL deref protection)
    if (ptr >= 0x7FFFFFFFFFFF) return false;  // Beyond user space
    if (ptr + len < ptr) return false;  // Overflow check
    return true;
}

// Hash function for futex addresses (MurmurHash3 finalizer)
static inline uint32_t futex_hash_fn(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return (uint32_t)(key % FUTEX_HASH_BUCKETS);
}

// Get the key for a futex address
// - Shared futexes: physical address (same across processes)
// - Private futexes: combine PML4 base with virtual address
static uint64_t futex_get_key(uint64_t uaddr, bool shared) {
    if (shared) {
        // For shared futexes, use physical address as key
        return mm_get_physical_address(uaddr);
    } else {
        // For private futexes, combine PML4 base with virtual address
        task_t* cur = sched_current();
        uint64_t pml4_phys = cur && cur->pml4 ? 
            mm_get_physical_address((uint64_t)cur->pml4) : 0;
        return pml4_phys ^ uaddr;
    }
}

// ============================================================================
// FUTEX OPERATIONS
// ============================================================================

void futex_init(void) {
    if (futex_initialized) return;
    
    for (int i = 0; i < FUTEX_HASH_BUCKETS; i++) {
        spinlock_init(&futex_hash[i].lock, "futex_bucket");
        futex_hash[i].head = NULL;
    }
    
    futex_initialized = true;
}

int futex_wait(uint64_t uaddr, uint32_t expected_val, uint64_t timeout_ns) {
    if (!validate_user_ptr(uaddr, sizeof(uint32_t))) {
        return -EFAULT;
    }
    
    task_t* cur = sched_current();
    if (!cur) return -ESRCH;
    
    // Check current value
    smap_disable();
    uint32_t curval = *(volatile uint32_t*)uaddr;
    smap_enable();
    
    if (curval != expected_val) {
        return -EAGAIN;  // Value changed, don't wait
    }
    
    // Compute hash key and bucket
    uint64_t key = futex_get_key(uaddr, false);  // Private futex
    uint32_t bucket_idx = futex_hash_fn(key);
    futex_bucket_t* bucket = &futex_hash[bucket_idx];
    
    // Allocate waiter
    futex_waiter_t* waiter = (futex_waiter_t*)kalloc(sizeof(futex_waiter_t));
    if (!waiter) return -ENOMEM;
    
    waiter->uaddr = uaddr;
    waiter->key = key;
    waiter->task = cur;
    waiter->bitset = 0xFFFFFFFF;  // Default bitset
    
    // Add to wait queue
    uint64_t flags;
    spin_lock_irqsave(&bucket->lock, &flags);
    
    // Re-check value under lock (double-check pattern)
    smap_disable();
    curval = *(volatile uint32_t*)uaddr;
    smap_enable();
    
    if (curval != expected_val) {
        spin_unlock_irqrestore(&bucket->lock, flags);
        kfree(waiter);
        return -EAGAIN;
    }
    
    // Insert at head of bucket list
    waiter->next = bucket->head;
    bucket->head = waiter;
    
    // Block the task
    cur->state = TASK_BLOCKED;
    cur->wait_channel = (void*)uaddr;
    
    // Set wakeup time if timeout specified
    if (timeout_ns > 0) {
        uint64_t ticks = (timeout_ns / 10000000) + 1;  // Convert to ~10ms ticks
        cur->wakeup_tick = timer_ticks() + ticks;
    }
    
    spin_unlock_irqrestore(&bucket->lock, flags);
    
    // Schedule away - will return when woken or timed out
    sched_schedule();
    
    // We've been woken up
    // Note: waiter structure may have been freed by futex_wake
    
    return 0;
}

int futex_wake(uint64_t uaddr, int nr_wake) {
    if (nr_wake <= 0) return 0;
    
    uint64_t key = futex_get_key(uaddr, false);
    uint32_t bucket_idx = futex_hash_fn(key);
    futex_bucket_t* bucket = &futex_hash[bucket_idx];
    
    int woken = 0;
    
    uint64_t flags;
    spin_lock_irqsave(&bucket->lock, &flags);
    
    futex_waiter_t** pp = &bucket->head;
    while (*pp && woken < nr_wake) {
        futex_waiter_t* w = *pp;
        
        if (w->key == key) {
            // Remove from list
            *pp = w->next;
            
            // Wake the task
            task_t* task = w->task;
            if (task->state == TASK_BLOCKED) {
                task->state = TASK_READY;
                task->wait_channel = NULL;
                task->wakeup_tick = 0;
                sched_enqueue_ready(task);
            }
            
            kfree(w);
            woken++;
        } else {
            pp = &w->next;
        }
    }
    
    spin_unlock_irqrestore(&bucket->lock, flags);
    
    return woken;
}

int futex_requeue(uint64_t uaddr, uint64_t uaddr2, int nr_wake, int nr_requeue) {
    if (nr_wake < 0 || nr_requeue < 0) return -EINVAL;
    
    uint64_t key1 = futex_get_key(uaddr, false);
    uint64_t key2 = futex_get_key(uaddr2, false);
    uint32_t bucket_idx1 = futex_hash_fn(key1);
    uint32_t bucket_idx2 = futex_hash_fn(key2);
    
    futex_bucket_t* bucket1 = &futex_hash[bucket_idx1];
    futex_bucket_t* bucket2 = &futex_hash[bucket_idx2];
    
    // Lock ordering: always lock lower bucket index first to prevent deadlock
    uint64_t flags;
    if (bucket_idx1 < bucket_idx2) {
        spin_lock_irqsave(&bucket1->lock, &flags);
        spin_lock(&bucket2->lock);
    } else if (bucket_idx1 > bucket_idx2) {
        spin_lock_irqsave(&bucket2->lock, &flags);
        spin_lock(&bucket1->lock);
    } else {
        // Same bucket - only lock once
        spin_lock_irqsave(&bucket1->lock, &flags);
    }
    
    int woken = 0;
    int requeued = 0;
    
    futex_waiter_t** pp = &bucket1->head;
    while (*pp) {
        futex_waiter_t* w = *pp;
        
        if (w->key == key1) {
            // Remove from bucket1
            *pp = w->next;
            
            if (woken < nr_wake) {
                // Wake this waiter
                task_t* task = w->task;
                if (task->state == TASK_BLOCKED) {
                    task->state = TASK_READY;
                    task->wait_channel = NULL;
                    task->wakeup_tick = 0;
                    sched_enqueue_ready(task);
                }
                kfree(w);
                woken++;
            } else if (requeued < nr_requeue) {
                // Requeue to bucket2
                w->uaddr = uaddr2;
                w->key = key2;
                w->task->wait_channel = (void*)uaddr2;
                w->next = bucket2->head;
                bucket2->head = w;
                requeued++;
            } else {
                // Already hit limits, re-insert at head
                w->next = *pp;
                *pp = w;
                pp = &w->next;
            }
        } else {
            pp = &w->next;
        }
    }
    
    // Unlock in reverse order
    if (bucket_idx1 < bucket_idx2) {
        spin_unlock(&bucket2->lock);
        spin_unlock_irqrestore(&bucket1->lock, flags);
    } else if (bucket_idx1 > bucket_idx2) {
        spin_unlock(&bucket1->lock);
        spin_unlock_irqrestore(&bucket2->lock, flags);
    } else {
        spin_unlock_irqrestore(&bucket1->lock, flags);
    }
    
    return woken + requeued;
}

// ============================================================================
// ROBUST FUTEX HANDLING
// ============================================================================

void exit_robust_list(task_t* task) {
    if (!task || !task->robust_list) return;
    
    // The robust_list_head structure format (Linux ABI):
    // struct robust_list_head {
    //     struct robust_list *list;           // Pointer to first entry
    //     long futex_offset;                  // Offset of futex word in entry
    //     struct robust_list *list_op_pending; // Currently being acquired
    // };
    //
    // Each entry in the list contains:
    //   - Pointer to next entry
    //   - Futex word at futex_offset from entry start
    //
    // On thread death, we walk the list and for each futex we own:
    //   1. Set FUTEX_OWNER_DIED bit (bit 30)
    //   2. Wake one waiter
    
    smap_disable();
    struct robust_list_head* head = task->robust_list;
    
    // Limit iterations to prevent infinite loops from corrupted lists
    int count = 0;
    const int max_entries = 1024;
    
    // Process pending futex first (thread died while acquiring)
    if (head->list_op_pending && 
        head->list_op_pending != (struct robust_list*)head) {
        uint64_t futex_addr = (uint64_t)head->list_op_pending + head->futex_offset;
        if (validate_user_ptr(futex_addr, sizeof(uint32_t))) {
            uint32_t* futex = (uint32_t*)futex_addr;
            // Only mark if we're the owner
            if ((*futex & FUTEX_TID_MASK) == (uint32_t)task->id) {
                *futex |= FUTEX_OWNER_DIED;
                smap_enable();
                futex_wake(futex_addr, 1);
                smap_disable();
            }
        }
    }
    
    // Walk the circular list
    struct robust_list* entry = (struct robust_list*)head->list;
    while (entry && entry != (struct robust_list*)head && count < max_entries) {
        // The futex is at offset futex_offset from the list entry
        uint64_t futex_addr = (uint64_t)entry + head->futex_offset;
        
        if (validate_user_ptr(futex_addr, sizeof(uint32_t))) {
            uint32_t* futex = (uint32_t*)futex_addr;
            // Check if we own this futex (TID in low 30 bits)
            if ((*futex & FUTEX_TID_MASK) == (uint32_t)task->id) {
                *futex |= FUTEX_OWNER_DIED;
                smap_enable();
                futex_wake(futex_addr, 1);
                smap_disable();
            }
        }
        
        // Move to next entry
        if (!validate_user_ptr((uint64_t)entry, sizeof(struct robust_list))) {
            break;
        }
        entry = entry->next;
        count++;
    }
    
    smap_enable();
}
