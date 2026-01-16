// LikeOS-64 minimal cooperative scheduler
#ifndef _KERNEL_SCHED_H_
#define _KERNEL_SCHED_H_

#include "types.h"

typedef void (*task_entry_t)(void* arg);

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

// Task privilege level
typedef enum {
    TASK_KERNEL = 0,   // Ring 0
    TASK_USER = 3      // Ring 3
} task_privilege_t;

typedef struct task {
    uint64_t* sp;          // Saved stack pointer
    uint64_t* pml4;        // Page table base (CR3) - NULL for kernel tasks (uses kernel PML4)
    task_entry_t entry;    // Entry function
    void* arg;             // Entry argument
    task_state_t state;
    task_privilege_t privilege;  // Ring level
    struct task* next;
    int id;
    
    // User mode support
    uint64_t user_stack_top;    // User stack virtual address (for user tasks)
    uint64_t kernel_stack_top;  // Kernel stack for syscalls/interrupts (for user tasks)
} task_t;

void sched_init(void);
void sched_add_task(task_entry_t entry, void* arg, void* stack_mem, size_t stack_size);
task_t* sched_add_user_task(task_entry_t entry, void* arg, uint64_t* pml4, uint64_t user_stack, uint64_t kernel_stack);
void sched_tick(void);
void sched_yield(void);
void sched_run_ready(void);
task_t* sched_current(void);

#endif // _KERNEL_SCHED_H_
