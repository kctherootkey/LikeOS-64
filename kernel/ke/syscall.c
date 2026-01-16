// LikeOS-64 System Call Handler
// Handles system calls from user mode

#include "../../include/kernel/console.h"
#include "../../include/kernel/types.h"
#include "../../include/kernel/sched.h"

// System call numbers
#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_READ    2
#define SYS_YIELD   3
#define SYS_GETPID  4

// System call handler - called from syscall.asm
// Returns result in RAX
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, 
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;  // Unused for now
    
    switch (num) {
        case SYS_EXIT: {
            // Exit current task
            task_t* cur = sched_current();
            kprintf("[SYSCALL] Task %d exiting with code %llu\n", 
                    cur ? cur->id : -1, a1);
            if (cur) {
                cur->state = TASK_ZOMBIE;
            }
            // Yield to next task
            sched_yield();
            // Should not return
            return 0;
        }
        
        case SYS_WRITE: {
            // Write to console: write(fd, buf, len)
            // For now, only fd=1 (stdout) supported
            if (a1 == 1) {
                const char* buf = (const char*)a2;
                size_t len = (size_t)a3;
                
                // Safety check: validate user pointer is in user space
                if (a2 < 0x400000 || a2 >= 0x7FFFFFFFFFFF) {
                    return -1;  // Invalid pointer
                }
                
                for (size_t i = 0; i < len && buf[i]; i++) {
                    console_putchar(buf[i]);
                }
                return (int64_t)len;
            }
            return -1;  // Invalid fd
        }
        
        case SYS_READ: {
            // Read - not implemented yet
            (void)a1; (void)a2; (void)a3;
            return -1;
        }
        
        case SYS_YIELD: {
            // Yield CPU to other tasks
            sched_yield();
            return 0;
        }
        
        case SYS_GETPID: {
            // Get current task ID
            task_t* cur = sched_current();
            return cur ? cur->id : -1;
        }
        
        default:
            kprintf("[SYSCALL] Unknown syscall %llu\n", num);
            return -1;
    }
}
