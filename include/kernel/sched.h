// LikeOS-64 minimal cooperative scheduler
#ifndef _KERNEL_SCHED_H_
#define _KERNEL_SCHED_H_

#include "types.h"
#include "vfs.h"
#include "signal.h"

// Forward declaration
struct vfs_file;
struct tty;

// Maximum file descriptors per task
#define TASK_MAX_FDS    1024

// Maximum memory regions per task (for mmap tracking)
#define TASK_MAX_MMAP   64

typedef void (*task_entry_t)(void* arg);

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_STOPPED,
    TASK_ZOMBIE
} task_state_t;

// Task privilege level
typedef enum {
    TASK_KERNEL = 0,   // Ring 0
    TASK_USER = 3      // Ring 3
} task_privilege_t;

// Memory region for mmap tracking
typedef struct mmap_region {
    uint64_t start;     // Virtual start address
    uint64_t length;    // Length in bytes
    uint64_t prot;      // Protection flags (PROT_READ, PROT_WRITE, PROT_EXEC)
    uint64_t flags;     // MAP_ANONYMOUS, MAP_PRIVATE, etc.
    int fd;             // File descriptor (-1 for anonymous)
    uint64_t offset;    // Offset in file
    bool in_use;        // Whether this slot is used
} mmap_region_t;

typedef struct task {
    uint64_t* sp;          // Saved stack pointer
    uint64_t* pml4;        // Page table base (CR3) - NULL for kernel tasks (uses kernel PML4)
    task_entry_t entry;    // Entry function
    void* arg;             // Entry argument
    task_state_t state;
    task_privilege_t privilege;  // Ring level
    struct task* next;     // Scheduler circular list
    int id;
    
    // Process hierarchy
    struct task* parent;        // Parent task (NULL for init)
    struct task* first_child;   // First child in linked list
    struct task* next_sibling;  // Next sibling in parent's child list
    
    // Exit status tracking
    int exit_code;              // Exit status for waitpid
    bool has_exited;            // True when exit() was called
    bool is_fork_child;         // True if this is a newly forked child (should return 0)
    
    // User mode support
    uint64_t user_stack_top;    // User stack virtual address (for user tasks)
    uint64_t kernel_stack_top;  // Kernel stack for syscalls/interrupts (for user tasks)

    // Job control / session
    int pgid;
    int sid;
    struct tty* ctty;

    // Wait linkage for blocking I/O
    struct task* wait_next;
    void* wait_channel;
    
    // Signal handling state
    task_signal_state_t signals;    // Full signal state
    
    // Saved syscall context for signal delivery (per-task, not global)
    uint64_t syscall_rsp;           // User RSP on syscall entry
    uint64_t syscall_rip;           // User RIP (return address)
    uint64_t syscall_rflags;        // User RFLAGS
    uint64_t syscall_rbp;           // Callee-saved
    uint64_t syscall_rbx;           // Callee-saved
    uint64_t syscall_r12;           // Callee-saved
    uint64_t syscall_r13;           // Callee-saved
    uint64_t syscall_r14;           // Callee-saved
    uint64_t syscall_r15;           // Callee-saved
    
    // Current working directory
    char cwd[256];
    
    // File descriptor table
    struct vfs_file* fd_table[TASK_MAX_FDS];
    
    // Memory management
    uint64_t brk;               // Current program break (heap end)
    uint64_t brk_start;         // Initial program break (heap start)
    mmap_region_t mmap_regions[TASK_MAX_MMAP];  // mmap'd regions
    uint64_t mmap_base;         // Base address for mmap allocations
} task_t;

void sched_init(void);
void sched_add_task(task_entry_t entry, void* arg, void* stack_mem, size_t stack_size);
task_t* sched_add_user_task(task_entry_t entry, void* arg, uint64_t* pml4, uint64_t user_stack, uint64_t kernel_stack);
void sched_tick(void);
void sched_yield(void);
void sched_run_ready(void);
task_t* sched_current(void);
int sched_has_user_tasks(void);  // Check if any user tasks are running

// Process management
task_t* sched_fork_current(void);           // Fork current task with COW
void sched_remove_task(task_t* task);       // Remove task from scheduler
task_t* sched_find_task_by_id(uint32_t pid); // Find task by PID
void sched_add_child(task_t* parent, task_t* child);  // Add child to parent
void sched_remove_child(task_t* parent, task_t* child);  // Remove child from parent
void sched_reparent_children(task_t* task); // Reparent children to init (task 1)
uint32_t sched_get_ppid(task_t* task);      // Get parent PID
void sched_reap_zombies(task_t* parent);    // Reap all zombie children of parent
void sched_mark_task_exited(task_t* task, int status);
void sched_signal_task(task_t* task, int sig);
void sched_signal_pgrp(int pgid, int sig);
int sched_pgid_exists(int pgid);

#endif // _KERNEL_SCHED_H_
