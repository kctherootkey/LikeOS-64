#ifndef _SCHED_H
#define _SCHED_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Scheduling policies
#define SCHED_NORMAL    0
#define SCHED_OTHER     0  // Alias for SCHED_NORMAL
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_IDLE      5
#define SCHED_DEADLINE  6

// Clone flags
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_PTRACE         0x00002000
#define CLONE_VFORK          0x00004000
#define CLONE_PARENT         0x00008000
#define CLONE_THREAD         0x00010000
#define CLONE_NEWNS          0x00020000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED       0x00400000
#define CLONE_UNTRACED       0x00800000
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_STOPPED        0x02000000
#define CLONE_NEWUTS         0x04000000
#define CLONE_NEWIPC         0x08000000

// CPU set for affinity (supports up to 64 CPUs)
#define CPU_SETSIZE 64

typedef struct {
    unsigned long bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;

// CPU set macros
#define CPU_ZERO(set)       do { (set)->bits[0] = 0; } while(0)
#define CPU_SET(cpu, set)   do { (set)->bits[0] |= (1UL << (cpu)); } while(0)
#define CPU_CLR(cpu, set)   do { (set)->bits[0] &= ~(1UL << (cpu)); } while(0)
#define CPU_ISSET(cpu, set) (((set)->bits[0] & (1UL << (cpu))) != 0)

// Count bits set in CPU mask (simple implementation without __builtin_popcount)
static inline int __cpu_count(const cpu_set_t* set) {
    unsigned long v = set->bits[0];
    int count = 0;
    while (v) {
        count += (v & 1);
        v >>= 1;
    }
    return count;
}
#define CPU_COUNT(set)      __cpu_count(set)

// Scheduling parameters
struct sched_param {
    int sched_priority;
};

// Basic scheduling
int sched_yield(void);

// Thread/process creation
pid_t clone(int (*fn)(void*), void* stack, int flags, void* arg, ...);
pid_t vfork(void);
pid_t gettid(void);

// CPU affinity
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t* mask);
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask);

// Scheduling policy and parameters
int sched_setscheduler(pid_t pid, int policy, const struct sched_param* param);
int sched_getscheduler(pid_t pid);
int sched_setparam(pid_t pid, const struct sched_param* param);
int sched_getparam(pid_t pid, struct sched_param* param);

// Priority range
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_rr_get_interval(pid_t pid, struct timespec* tp);

// Exit all threads in process group
void exit_group(int status) __attribute__((noreturn));

// Thread ID management
int set_tid_address(int* tidptr);

// Robust futex list
int set_robust_list(void* head, size_t len);
int get_robust_list(pid_t pid, void** head_ptr, size_t* len_ptr);

// arch_prctl codes for TLS
#define ARCH_SET_GS     0x1001
#define ARCH_SET_FS     0x1002
#define ARCH_GET_FS     0x1003
#define ARCH_GET_GS     0x1004

// Architecture-specific thread state (TLS)
int arch_prctl(int code, unsigned long addr);

#ifdef __cplusplus
}
#endif

#endif
