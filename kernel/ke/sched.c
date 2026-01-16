// LikeOS-64 Cooperative Scheduler
#include "../../include/kernel/sched.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/types.h"

extern void user_mode_iret_trampoline(void);
extern void ctx_switch_asm(uint64_t** old_sp, uint64_t* new_sp);

#define SCHED_SLICE_TICKS 10

static task_t g_bootstrap_task;
static task_t g_idle_task;
static uint8_t g_idle_stack[4096] __attribute__((aligned(16)));
static task_t* g_current = 0;
static int g_next_id = 1;
static uint64_t g_slice_accum = 0;
static uint64_t* g_kernel_pml4 = 0;
static uint64_t g_default_kernel_stack = 0;

static inline int is_idle_task(const task_t* t) {
    return t == &g_idle_task;
}

static void task_trampoline(void);
static void idle_entry(void* arg);

static inline void switch_address_space(task_t* prev, task_t* next) {
    uint64_t* prev_pml4 = prev->pml4 ? prev->pml4 : g_kernel_pml4;
    uint64_t* next_pml4 = next->pml4 ? next->pml4 : g_kernel_pml4;
    
    if (next->privilege == TASK_USER && next->kernel_stack_top != 0) {
        tss_set_kernel_stack(next->kernel_stack_top);
    } else {
        tss_set_kernel_stack(g_default_kernel_stack);
    }
    
    if (prev_pml4 != next_pml4) {
        MmSwitchAddressSpace(next_pml4);
    }
}

static void enqueue_task(task_t* t) {
    if (!g_current) {
        g_current = t;
        t->next = t;
        return;
    }
    t->next = g_current->next;
    g_current->next = t;
}

void sched_init(void) {
    g_kernel_pml4 = MmGetCurrentAddressSpace();
    g_default_kernel_stack = tss_get_kernel_stack();
    
    __asm__ volatile ("mov %%rsp, %0" : "=r"(g_bootstrap_task.sp));
    g_bootstrap_task.pml4 = NULL;
    g_bootstrap_task.entry = 0;
    g_bootstrap_task.arg = 0;
    g_bootstrap_task.state = TASK_RUNNING;
    g_bootstrap_task.privilege = TASK_KERNEL;
    g_bootstrap_task.id = 0;
    g_bootstrap_task.next = &g_bootstrap_task;
    g_bootstrap_task.user_stack_top = 0;
    g_bootstrap_task.kernel_stack_top = 0;
    g_current = &g_bootstrap_task;

    sched_add_task(idle_entry, 0, g_idle_stack, sizeof(g_idle_stack));
    kprintf("Scheduler initialized\n");
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
    sp = (uint64_t*)((uint64_t)sp & ~0xFUL);
    *(--sp) = (uint64_t)task_trampoline;
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->sp = sp;
    t->pml4 = NULL;
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->privilege = TASK_KERNEL;
    t->id = g_next_id++;
    t->user_stack_top = 0;
    t->kernel_stack_top = 0;

    enqueue_task(t);
}

task_t* sched_add_user_task(task_entry_t entry, void* arg, uint64_t* pml4, uint64_t user_stack, uint64_t kernel_stack) {
    (void)kernel_stack;
    if (!entry || !pml4) {
        return NULL;
    }
    
    task_t* t = (task_t*)kalloc(sizeof(task_t));
    if (!t) {
        return NULL;
    }
    
    uint8_t* k_stack_mem = (uint8_t*)kalloc(4096);
    if (!k_stack_mem) {
        kfree(t);
        return NULL;
    }
    
    uint64_t* k_sp = (uint64_t*)(k_stack_mem + 4096);
    k_sp = (uint64_t*)((uint64_t)k_sp & ~0xFUL);
    
    // IRET frame: SS, RSP, RFLAGS, CS, RIP
    *(--k_sp) = 0x1B;                    // SS
    *(--k_sp) = user_stack - 8;          // RSP
    *(--k_sp) = 0x202;                   // RFLAGS (IF enabled)
    *(--k_sp) = 0x23;                    // CS
    *(--k_sp) = (uint64_t)entry;         // RIP
    
    // Callee-saved registers
    *(--k_sp) = (uint64_t)user_mode_iret_trampoline;
    *(--k_sp) = 0; // rbp
    *(--k_sp) = 0; // rbx
    *(--k_sp) = 0; // r12
    *(--k_sp) = 0; // r13
    *(--k_sp) = 0; // r14
    *(--k_sp) = 0; // r15

    t->sp = k_sp;
    t->pml4 = pml4;
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->privilege = TASK_USER;
    t->id = g_next_id++;
    t->user_stack_top = user_stack;
    t->kernel_stack_top = (uint64_t)(k_stack_mem + 4096);

    enqueue_task(t);
    kprintf("Added user task %d\n", t->id);
    
    return t;
}

static task_t* pick_next(task_t* start) {
    if (!start || !start->next) {
        return start;
    }
    
    task_t* it = start->next;
    task_t* idle_candidate = 0;
    
    while (it != start) {
        if (it->state == TASK_READY || it->state == TASK_RUNNING) {
            if (!is_idle_task(it)) {
                return it;
            }
            idle_candidate = it;
        }
        it = it->next;
        if (!it) break;
    }
    
    if (idle_candidate) {
        return idle_candidate;
    }
    return start;
}

void sched_tick(void) {
    if (!g_current) {
        return;
    }
    g_slice_accum++;
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
    
    if (next->sp == 0) {
        kprintf("FATAL: sched_yield next->sp is NULL!\n");
        for(;;) __asm__ volatile("hlt");
    }
    
    task_t* prev = g_current;
    g_current = next;
    if (prev->state != TASK_ZOMBIE) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    
    switch_address_space(prev, next);
    ctx_switch_asm(&prev->sp, next->sp);
}

void sched_run_ready(void) {
    if (!g_current || g_slice_accum < SCHED_SLICE_TICKS) {
        return;
    }
    g_slice_accum = 0;
    task_t* next = pick_next(g_current);
    
    if (!next) {
        return;
    }
    
    if (next == g_current || (is_idle_task(next) && !is_idle_task(g_current))) {
        return;
    }
    
    if (next->sp == 0) {
        kprintf("FATAL: next->sp is NULL!\n");
        for(;;) __asm__ volatile("hlt");
    }
    
    task_t* prev = g_current;
    g_current = next;
    if (prev->state != TASK_ZOMBIE) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    
    switch_address_space(prev, next);
    ctx_switch_asm(&prev->sp, next->sp);
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
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
}
