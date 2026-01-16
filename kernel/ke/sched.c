// LikeOS-64 minimal cooperative scheduler
#include "../../include/kernel/sched.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/types.h"

// External assembly trampoline for IRET to user mode
extern void user_mode_iret_trampoline(void);

// External assembly context switch function
extern void ctx_switch_asm(uint64_t** old_sp, uint64_t* new_sp);

#define SCHED_SLICE_TICKS 10

static task_t g_bootstrap_task;
static task_t g_idle_task;
static uint8_t g_idle_stack[4096] __attribute__((aligned(16)));
static task_t* g_current = 0;
static int g_next_id = 1;
static uint64_t g_slice_accum = 0;
static uint64_t* g_kernel_pml4 = 0;  // Kernel PML4 (saved at init)
static uint64_t g_default_kernel_stack = 0;  // Default kernel stack for non-user tasks

static inline int is_idle_task(const task_t* t) {
    return t == &g_idle_task;
}

static void task_trampoline(void);
static void idle_entry(void* arg);

// Switch address space (CR3) if needed, and update TSS for user mode
static inline void switch_address_space(task_t* prev, task_t* next) {
    // Get the PML4 to use for each task
    // NULL pml4 means use kernel PML4
    uint64_t* prev_pml4 = prev->pml4 ? prev->pml4 : g_kernel_pml4;
    uint64_t* next_pml4 = next->pml4 ? next->pml4 : g_kernel_pml4;
    
    // Update TSS.RSP0 for user mode tasks
    // When an interrupt occurs in user mode, CPU loads RSP0 from TSS
    if (next->privilege == TASK_USER && next->kernel_stack_top != 0) {
        tss_set_kernel_stack(next->kernel_stack_top);
    } else {
        // Restore default kernel stack for kernel tasks
        tss_set_kernel_stack(g_default_kernel_stack);
    }
    
    // Only switch CR3 if address spaces are different
    if (prev_pml4 != next_pml4) {
        MmSwitchAddressSpace(next_pml4);
    }
}

// ctx_switch is now implemented in assembly (syscall.asm) as ctx_switch_asm

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
    // Save kernel PML4 for kernel tasks
    g_kernel_pml4 = MmGetCurrentAddressSpace();
    
    // Save the default kernel stack from TSS
    // This is the interrupt stack set up in tss_init
    g_default_kernel_stack = tss_get_kernel_stack();
    
    // Bootstrap task represents the currently running context
    __asm__ volatile ("mov %%rsp, %0" : "=r"(g_bootstrap_task.sp));
    g_bootstrap_task.pml4 = NULL;  // Uses kernel PML4
    g_bootstrap_task.entry = 0;
    g_bootstrap_task.arg = 0;
    g_bootstrap_task.state = TASK_RUNNING;
    g_bootstrap_task.privilege = TASK_KERNEL;
    g_bootstrap_task.id = 0;
    g_bootstrap_task.next = &g_bootstrap_task;
    g_bootstrap_task.user_stack_top = 0;
    g_bootstrap_task.kernel_stack_top = 0;
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
    t->pml4 = NULL;  // Kernel task uses kernel PML4
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->privilege = TASK_KERNEL;
    t->id = g_next_id++;
    t->user_stack_top = 0;
    t->kernel_stack_top = 0;

    enqueue_task(t);
}

// Add a user-mode task with its own address space
task_t* sched_add_user_task(task_entry_t entry, void* arg, uint64_t* pml4, uint64_t user_stack, uint64_t kernel_stack) {
    if (!entry || !pml4) {
        return NULL;
    }
    
    task_t* t = (task_t*)kalloc(sizeof(task_t));
    if (!t) {
        return NULL;
    }
    
    // For user tasks, we need to set up both a kernel stack and user stack
    // The kernel stack is used for syscalls and interrupts
    // For now, we set up a minimal kernel stack frame that will IRET to user mode
    
    // Allocate kernel stack for this task (4KB)
    uint8_t* k_stack_mem = (uint8_t*)kalloc(4096);
    if (!k_stack_mem) {
        kfree(t);
        return NULL;
    }
    
    uint64_t* k_sp = (uint64_t*)(k_stack_mem + 4096);
    k_sp = (uint64_t*)((uint64_t)k_sp & ~0xFUL); // align
    
    kprintf("DEBUG: Setting up user task kernel stack at %p (top=%p)\n", 
            k_stack_mem, k_sp);
    
    // Set up IRET frame for returning to user mode
    // Stack layout for IRET (in order of pop): SS, RSP, RFLAGS, CS, RIP
    // GDT layout (for SYSRET compatibility):
    //   0x08 = kernel code, 0x10 = kernel data
    //   0x18 = user data,   0x20 = user code
    // User SS = 0x18 | 3 = 0x1B, User CS = 0x20 | 3 = 0x23
    *(--k_sp) = 0x1B;                    // SS (user data segment 0x18 with RPL 3)
    *(--k_sp) = user_stack - 8;          // RSP (user stack, leave room for alignment)
    *(--k_sp) = 0x202;                   // RFLAGS (IF enabled)
    *(--k_sp) = 0x23;                    // CS (user code segment 0x20 with RPL 3)
    *(--k_sp) = (uint64_t)entry;         // RIP (entry point in user space)
    
    // Callee-saved registers and return address to user trampoline (naked IRET)
    *(--k_sp) = (uint64_t)user_mode_iret_trampoline; // return RIP - asm function that does IRET
    *(--k_sp) = 0; // rbp
    *(--k_sp) = 0; // rbx
    *(--k_sp) = 0; // r12
    *(--k_sp) = 0; // r13
    *(--k_sp) = 0; // r14
    *(--k_sp) = 0; // r15

    // Debug: dump the stack layout
    kprintf("DEBUG: Stack layout (top to bottom):\n");
    uint64_t* ptr = k_sp;
    kprintf("  [%p] r15     = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] r14     = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] r13     = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] r12     = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] rbx     = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] rbp     = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] ret_rip = %p (user_mode_iret_trampoline)\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] RIP     = %p (entry=%p)\n", ptr, (void*)ptr[0], entry); ptr++;
    kprintf("  [%p] CS      = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] RFLAGS  = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] RSP     = %p\n", ptr, (void*)ptr[0]); ptr++;
    kprintf("  [%p] SS      = %p\n", ptr, (void*)ptr[0]);
    kprintf("DEBUG: t->sp = %p\n", k_sp);

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
    kprintf("Added user task %d with PML4=%p, user_stack=%p\n", 
            t->id, pml4, (void*)user_stack);
    
    return t;
}

static task_t* pick_next(task_t* start) {
    if (!start) {
        kprintf("FATAL: pick_next called with NULL start!\n");
        return 0;
    }
    
    task_t* it = start->next;
    if (!it) {
        kprintf("FATAL: start->next is NULL! start=%p, &g_bootstrap_task=%p, &g_idle_task=%p\n", 
                start, &g_bootstrap_task, &g_idle_task);
        kprintf("  bootstrap.next=%p, idle.next=%p\n", 
                g_bootstrap_task.next, g_idle_task.next);
        return start; // Return start rather than crash
    }
    task_t* idle_candidate = 0;
    while (it != start) {
        if (it->state == TASK_READY || it->state == TASK_RUNNING) {
            if (!is_idle_task(it)) {
                return it; // prefer non-idle
            }
            idle_candidate = it;
        }
        it = it->next;
        if (!it) {
            kprintf("FATAL: it->next is NULL in pick_next loop!\n");
            break;
        }
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
    kprintf("[YIELD] cur=%d next=%d\n", g_current->id, next->id);
    if (next == g_current) {
        kprintf("[YIELD] same task, returning\n");
        return;
    }
    
    // Debug: Check for null stack pointers before switching
    if (next->sp == 0) {
        kprintf("FATAL: sched_yield next->sp is NULL! next=%p, id=%d\n", next, next->id);
        for(;;) __asm__ volatile("hlt");
    }
    
    task_t* prev = g_current;
    g_current = next;
    // Only set READY if task is not already ZOMBIE (exited)
    if (prev->state != TASK_ZOMBIE) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    
    // Switch address space before context switch
    switch_address_space(prev, next);
    
    kprintf("[YIELD] switching to task %d, sp=%p\n", next->id, next->sp);
    ctx_switch_asm(&prev->sp, next->sp);
    kprintf("[YIELD] returned from ctx_switch\n");
}

void sched_run_ready(void) {
    if (!g_current || g_slice_accum < SCHED_SLICE_TICKS) {
        return;
    }
    g_slice_accum = 0;
    task_t* next = pick_next(g_current);
    
    // Safety check: if pick_next returns NULL, something is very wrong
    if (!next) {
        kprintf("FATAL: pick_next returned NULL! g_current=%p\n", g_current);
        for(;;) __asm__ volatile("hlt");
    }
    
    // Don't switch if next is idle and current is not idle (keep running real work)
    if (next == g_current || (is_idle_task(next) && !is_idle_task(g_current))) {
        return;
    }
    
    // Debug: Check for null stack pointers before switching
    if (next->sp == 0) {
        kprintf("FATAL: next->sp is NULL! next=%p, id=%d\n", next, next->id);
        for(;;) __asm__ volatile("hlt");
    }
    
    task_t* prev = g_current;
    g_current = next;
    // Only set READY if task is not already ZOMBIE (exited)
    if (prev->state != TASK_ZOMBIE) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    
    // Switch address space before context switch
    switch_address_space(prev, next);
    
    kprintf("[RUN_READY] switching %d->%d, sp=%p\n", prev->id, next->id, next->sp);
    ctx_switch_asm(&prev->sp, next->sp);
    kprintf("[RUN_READY] returned from ctx_switch\n");
}

task_t* sched_current(void) {
    return g_current;
}

// Debug function to check task list integrity
void sched_debug_check(const char* label) {
    kprintf("[SCHED DEBUG %s] bootstrap.next=%p, idle.next=%p\n",
            label, g_bootstrap_task.next, g_idle_task.next);
}

// Note: user_mode_iret_trampoline is now in syscall.asm

// Trampoline for kernel tasks - just calls entry function directly
static void task_trampoline(void) {
    task_t* cur = g_current;
    
    // Kernel task - call entry directly
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
