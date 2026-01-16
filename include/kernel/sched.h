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

typedef struct task {
    uint64_t* sp;          // Saved stack pointer
    task_entry_t entry;    // Entry function
    void* arg;             // Entry argument
    task_state_t state;
    struct task* next;
    int id;
} task_t;

void sched_init(void);
void sched_add_task(task_entry_t entry, void* arg, void* stack_mem, size_t stack_size);
void sched_tick(void);
void sched_yield(void);
void sched_run_ready(void);
task_t* sched_current(void);

#endif // _KERNEL_SCHED_H_
