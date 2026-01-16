// LikeOS-64 minimal cooperative scheduler
#include "../../include/kernel/sched.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/types.h"

#define SCHED_SLICE_TICKS 10

static task_t g_bootstrap_task;
static task_t g_idle_task;
static uint8_t g_idle_stack[4096] __attribute__((aligned(16)));
static task_t* g_current = 0;
static int g_next_id = 1;
static uint64_t g_slice_accum = 0;

static inline int is_idle_task(const task_t* t) {
    return t == &g_idle_task;
}

static void task_trampoline(void);
static void idle_entry(void* arg);

// Minimal context switch: save/restore callee-saved regs + return address
// naked attribute prevents compiler from generating prologue/epilogue
__attribute__((naked))
static void ctx_switch(uint64_t** old_sp, uint64_t* new_sp) {
    // old_sp in %rdi, new_sp in %rsi (System V AMD64 ABI)
    __asm__ volatile(
        "pushq %%rbp\n\t"
        "pushq %%rbx\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"
        "movq %%rsp, (%%rdi)\n\t"
        "movq %%rsi, %%rsp\n\t"
        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%rbx\n\t"
        "popq %%rbp\n\t"
        "ret\n\t"
        ::: "memory");
}

static void enqueue_task(task_t* t) {
    if (!g_current) {
        g_current = t;
        t->next = t;
        return;
    }
    // Insert after current
    t->next = g_current->next;
    g_current->next = t;
}

void sched_init(void) {
    // Bootstrap task represents the currently running context
    __asm__ volatile ("mov %%rsp, %0" : "=r"(g_bootstrap_task.sp));
    g_bootstrap_task.entry = 0;
    g_bootstrap_task.arg = 0;
    g_bootstrap_task.state = TASK_RUNNING;
    g_bootstrap_task.id = 0;
    g_bootstrap_task.next = &g_bootstrap_task;
    g_current = &g_bootstrap_task;

    // Add idle task
    sched_add_task(idle_entry, 0, g_idle_stack, sizeof(g_idle_stack));
    kprintf("Scheduler initialized (idle task id=%d)\n", g_idle_task.id);
}

void sched_add_task(task_entry_t entry, void* arg, void* stack_mem, size_t stack_size) {
    if (!entry || !stack_mem || stack_size < 128) {
        return;
    }
    task_t* t = (entry == idle_entry) ? &g_idle_task : (task_t*)kalloc(sizeof(task_t));
    if (!t) {
        return;
    }

    uint64_t* sp = (uint64_t*)((uint8_t*)stack_mem + stack_size);
    sp = (uint64_t*)((uint64_t)sp & ~0xFUL); // align
    // Callee-saved registers and return address to trampoline
    *(--sp) = (uint64_t)task_trampoline; // return RIP
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->sp = sp;
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->id = g_next_id++;

    enqueue_task(t);
}

static task_t* pick_next(task_t* start) {
    if (!start) {
        return 0;
    }
    task_t* it = start->next;
    task_t* idle_candidate = 0;
    while (it != start) {
        if (it->state == TASK_READY || it->state == TASK_RUNNING) {
            if (!is_idle_task(it)) {
                return it; // prefer non-idle
            }
            idle_candidate = it;
        }
        it = it->next;
    }
    if (idle_candidate) {
        return idle_candidate; // only if nothing else ready
    }
    return start;
}

void sched_tick(void) {
    if (!g_current) {
        return;
    }
    g_slice_accum++;
    // Preemptive switch deferred to sched_run_ready called from main loop
}

void sched_yield(void) {
    if (!g_current) {
        return;
    }
    g_slice_accum = 0;
    task_t* next = pick_next(g_current);
    if (next == g_current) {
        return;
    }
    task_t* prev = g_current;
    g_current = next;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    ctx_switch(&prev->sp, next->sp);
}

void sched_run_ready(void) {
    if (!g_current || g_slice_accum < SCHED_SLICE_TICKS) {
        return;
    }
    g_slice_accum = 0;
    task_t* next = pick_next(g_current);
    // Don't switch if next is idle and current is not idle (keep running real work)
    if (next == g_current || (is_idle_task(next) && !is_idle_task(g_current))) {
        return;
    }
    task_t* prev = g_current;
    g_current = next;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    ctx_switch(&prev->sp, next->sp);
}

task_t* sched_current(void) {
    return g_current;
}

static void task_trampoline(void) {
    task_t* cur = g_current;
    if (cur && cur->entry) {
        cur->entry(cur->arg);
    }
    cur->state = TASK_ZOMBIE;
    for (;;) {
        sched_yield();
        __asm__ volatile ("hlt");
    }
}

static void idle_entry(void* arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
