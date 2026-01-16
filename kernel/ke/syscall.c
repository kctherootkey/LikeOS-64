// LikeOS-64 System Call Handler
#include "../../include/kernel/console.h"
#include "../../include/kernel/types.h"
#include "../../include/kernel/sched.h"

#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_READ    2
#define SYS_YIELD   3
#define SYS_GETPID  4

int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, 
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    
    switch (num) {
        case SYS_EXIT: {
            task_t* cur = sched_current();
            if (cur) {
                cur->state = TASK_ZOMBIE;
            }
            __asm__ volatile ("sti");
            sched_yield();
            for (;;) {
                __asm__ volatile ("cli; hlt");
            }
        }
        
        case SYS_WRITE: {
            if (a1 == 1) {
                const char* buf = (const char*)a2;
                size_t len = (size_t)a3;
                
                if (a2 < 0x400000 || a2 >= 0x7FFFFFFFFFFF) {
                    return -1;
                }
                
                for (size_t i = 0; i < len && buf[i]; i++) {
                    console_putchar(buf[i]);
                }
                return (int64_t)len;
            }
            return -1;
        }
        
        case SYS_READ: {
            (void)a1; (void)a2; (void)a3;
            return -1;
        }
        
        case SYS_YIELD: {
            __asm__ volatile ("sti");
            sched_yield();
            return 0;
        }
        
        case SYS_GETPID: {
            task_t* cur = sched_current();
            return cur ? cur->id : -1;
        }
        
        default:
            return -1;
    }
}
