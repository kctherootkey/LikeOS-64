// LikeOS-64 Signal Definitions
#ifndef _KERNEL_SIGNAL_H_
#define _KERNEL_SIGNAL_H_

#include "types.h"

// Signal numbers (Linux compatible)
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGIOT      6   // Alias for SIGABRT
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTKFLT   16
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22
#define SIGURG      23
#define SIGXCPU     24
#define SIGXFSZ     25
#define SIGVTALRM   26
#define SIGPROF     27
#define SIGWINCH    28
#define SIGIO       29
#define SIGPOLL     SIGIO
#define SIGPWR      30
#define SIGSYS      31
#define SIGUNUSED   31

#define SIGRTMIN    32
#define SIGRTMAX    64

#define NSIG        65      // Total number of signals (0-64)
#define _NSIG       NSIG

// Signal set operations
#define _SIGSET_NWORDS  1

typedef struct {
    uint64_t sig[_SIGSET_NWORDS];
} kernel_sigset_t;

// Signal handler types
typedef void (*sighandler_t)(int);
typedef void (*sigaction_handler_t)(int, void*, void*);  // siginfo_t*, ucontext_t*

#define SIG_DFL     ((sighandler_t)0)
#define SIG_IGN     ((sighandler_t)1)
#define SIG_ERR     ((sighandler_t)-1)

// sigaction flags
#define SA_NOCLDSTOP    0x00000001
#define SA_NOCLDWAIT    0x00000002
#define SA_SIGINFO      0x00000004
#define SA_ONSTACK      0x08000000
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000
#define SA_RESTORER     0x04000000

// sigprocmask how values
#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2

// signalfd flags
#define SFD_CLOEXEC     02000000
#define SFD_NONBLOCK    00004000

// siginfo_t structure
typedef struct siginfo {
    int      si_signo;      // Signal number
    int      si_errno;      // errno value
    int      si_code;       // Signal code
    int      _pad0;
    union {
        int _pad[28];       // Padding for extensibility
        
        // kill(), sigsend(), raise()
        struct {
            int32_t  si_pid;    // Sending process ID
            uint32_t si_uid;    // Real user ID of sender
        } _kill;
        
        // POSIX.1b timers
        struct {
            int32_t  si_tid;    // Timer ID
            int32_t  si_overrun;// Overrun count
            int32_t  si_int;    // Signal value (int)
            void*    si_ptr;    // Signal value (pointer)
        } _timer;
        
        // SIGCHLD
        struct {
            int32_t  si_pid;    // Child PID
            uint32_t si_uid;    // Real user ID of sender
            int32_t  si_status; // Exit value or signal
            int64_t  si_utime;  // User time consumed
            int64_t  si_stime;  // System time consumed
        } _sigchld;
        
        // SIGILL, SIGFPE, SIGSEGV, SIGBUS
        struct {
            void*    si_addr;   // Faulting instruction/memory address
            int16_t  si_addr_lsb;
            int16_t  _pad1;
        } _sigfault;
        
        // SIGPOLL
        struct {
            int64_t  si_band;   // Band event
            int32_t  si_fd;     // File descriptor
        } _sigpoll;
    } _sifields;
} siginfo_t;

// Accessor macros
#define si_pid      _sifields._kill.si_pid
#define si_uid      _sifields._kill.si_uid
#define si_timerid  _sifields._timer.si_tid
#define si_overrun  _sifields._timer.si_overrun
#define si_status   _sifields._sigchld.si_status
#define si_utime    _sifields._sigchld.si_utime
#define si_stime    _sifields._sigchld.si_stime
#define si_addr     _sifields._sigfault.si_addr
#define si_band     _sifields._sigpoll.si_band
#define si_fd       _sifields._sigpoll.si_fd
#define si_int      _sifields._timer.si_int
#define si_ptr      _sifields._timer.si_ptr

// si_code values
#define SI_USER     0       // kill()
#define SI_KERNEL   128     // Kernel
#define SI_QUEUE    -1      // sigqueue()
#define SI_TIMER    -2      // Timer
#define SI_MESGQ    -3      // Message queue
#define SI_ASYNCIO  -4      // Async I/O
#define SI_SIGIO    -5      // Signal I/O
#define SI_TKILL    -6      // tkill

// SIGCHLD si_code
#define CLD_EXITED      1   // Child exited
#define CLD_KILLED      2   // Child killed
#define CLD_DUMPED      3   // Child dumped core
#define CLD_TRAPPED     4   // Traced child trapped
#define CLD_STOPPED     5   // Child stopped
#define CLD_CONTINUED   6   // Child continued

// sigaction structure (kernel version)
struct k_sigaction {
    union {
        sighandler_t    sa_handler;
        sigaction_handler_t sa_sigaction;
    } _u;
    uint64_t        sa_flags;
    void            (*sa_restorer)(void);
    kernel_sigset_t sa_mask;
};

#define sa_handler      _u.sa_handler
#define sa_sigaction    _u.sa_sigaction

// Stack for signal handlers
typedef struct sigaltstack {
    void*       ss_sp;      // Stack pointer
    int         ss_flags;   // Flags
    size_t      ss_size;    // Stack size
} stack_t;

// sigaltstack flags
#define SS_ONSTACK      1
#define SS_DISABLE      2
#define MINSIGSTKSZ     2048
#define SIGSTKSZ        8192

// Interval timer types
#define ITIMER_REAL     0   // Real time (SIGALRM)
#define ITIMER_VIRTUAL  1   // Virtual time (SIGVTALRM)  
#define ITIMER_PROF     2   // Profiling time (SIGPROF)

struct k_timeval {
    int64_t tv_sec;     // Seconds
    int64_t tv_usec;    // Microseconds
};

struct k_itimerval {
    struct k_timeval it_interval;  // Interval for periodic timer
    struct k_timeval it_value;     // Time until next expiration
};

// POSIX timers
typedef int32_t clockid_t;
typedef int32_t ktimer_t;

#define CLOCK_REALTIME          0
#define CLOCK_MONOTONIC         1
#define CLOCK_PROCESS_CPUTIME   2
#define CLOCK_THREAD_CPUTIME    3
#define CLOCK_MONOTONIC_RAW     4
#define CLOCK_REALTIME_COARSE   5
#define CLOCK_MONOTONIC_COARSE  6

struct k_timespec {
    int64_t tv_sec;     // Seconds
    int64_t tv_nsec;    // Nanoseconds
};

struct k_itimerspec {
    struct k_timespec it_interval;
    struct k_timespec it_value;
};

// sigevent for timer creation
#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD    2
#define SIGEV_THREAD_ID 4

union k_sigval {
    int     sival_int;
    void*   sival_ptr;
};

struct k_sigevent {
    union k_sigval  sigev_value;
    int             sigev_signo;
    int             sigev_notify;
    int             sigev_tid;      // Thread ID for SIGEV_THREAD_ID
    int             _pad;
};

// signalfd_siginfo structure
struct signalfd_siginfo {
    uint32_t ssi_signo;     // Signal number
    int32_t  ssi_errno;     // Error number
    int32_t  ssi_code;      // Signal code
    uint32_t ssi_pid;       // Sender PID
    uint32_t ssi_uid;       // Sender UID
    int32_t  ssi_fd;        // File descriptor
    uint32_t ssi_tid;       // Timer ID
    uint32_t ssi_band;      // Band event
    uint32_t ssi_overrun;   // Overrun count
    uint32_t ssi_trapno;    // Trap number
    int32_t  ssi_status;    // Exit status
    int32_t  ssi_int;       // sigqueue int
    uint64_t ssi_ptr;       // sigqueue pointer
    uint64_t ssi_utime;     // User CPU time
    uint64_t ssi_stime;     // System CPU time
    uint64_t ssi_addr;      // Fault address
    uint16_t ssi_addr_lsb;  // LSB of address
    uint8_t  _pad[46];      // Padding to 128 bytes
};

// Signal set operations (inline for kernel use)
static inline void sigemptyset_k(kernel_sigset_t* set) {
    set->sig[0] = 0;
}

static inline void sigfillset_k(kernel_sigset_t* set) {
    set->sig[0] = ~0ULL;
}

static inline void sigaddset_k(kernel_sigset_t* set, int sig) {
    if (sig > 0 && sig < NSIG) {
        set->sig[0] |= (1ULL << (sig - 1));
    }
}

static inline void sigdelset_k(kernel_sigset_t* set, int sig) {
    if (sig > 0 && sig < NSIG) {
        set->sig[0] &= ~(1ULL << (sig - 1));
    }
}

static inline int sigismember_k(const kernel_sigset_t* set, int sig) {
    if (sig <= 0 || sig >= NSIG) return 0;
    return (set->sig[0] & (1ULL << (sig - 1))) ? 1 : 0;
}

static inline void sigorset_k(kernel_sigset_t* dest, const kernel_sigset_t* a, const kernel_sigset_t* b) {
    dest->sig[0] = a->sig[0] | b->sig[0];
}

static inline void sigandset_k(kernel_sigset_t* dest, const kernel_sigset_t* a, const kernel_sigset_t* b) {
    dest->sig[0] = a->sig[0] & b->sig[0];
}

static inline void signandset_k(kernel_sigset_t* dest, const kernel_sigset_t* a, const kernel_sigset_t* b) {
    dest->sig[0] = a->sig[0] & ~b->sig[0];
}

static inline int sigisemptyset_k(const kernel_sigset_t* set) {
    return set->sig[0] == 0;
}

// Unmaskable signals
#define sig_kernel_only(sig) ((sig) == SIGKILL || (sig) == SIGSTOP)

// Default action types
#define SIG_DFL_TERM    0   // Terminate
#define SIG_DFL_IGN     1   // Ignore
#define SIG_DFL_CORE    2   // Core dump + terminate
#define SIG_DFL_STOP    3   // Stop process
#define SIG_DFL_CONT    4   // Continue if stopped

// Get default signal action
static inline int sig_default_action(int sig) {
    switch (sig) {
        case SIGCHLD:
        case SIGURG:
        case SIGWINCH:
            return SIG_DFL_IGN;
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            return SIG_DFL_STOP;
        case SIGCONT:
            return SIG_DFL_CONT;
        case SIGQUIT:
        case SIGILL:
        case SIGTRAP:
        case SIGABRT:
        case SIGBUS:
        case SIGFPE:
        case SIGSEGV:
        case SIGXCPU:
        case SIGXFSZ:
        case SIGSYS:
            return SIG_DFL_CORE;
        default:
            return SIG_DFL_TERM;
    }
}

// Maximum pending signals per task
#define MAX_PENDING_SIGNALS 32

// Signal frame - saved on user stack during signal delivery
// This must match what sys_rt_sigreturn expects
typedef struct signal_frame {
    // Return address (points to sigreturn trampoline)
    uint64_t        pretcode;
    
    // Saved registers (for restoration by sigreturn)
    uint64_t        rax;
    uint64_t        rbx;
    uint64_t        rcx;
    uint64_t        rdx;
    uint64_t        rsi;
    uint64_t        rdi;
    uint64_t        rbp;
    uint64_t        rsp;        // Original user RSP before signal
    uint64_t        r8;
    uint64_t        r9;
    uint64_t        r10;
    uint64_t        r11;
    uint64_t        r12;
    uint64_t        r13;
    uint64_t        r14;
    uint64_t        r15;
    uint64_t        rip;        // Original return address
    uint64_t        rflags;
    
    // Signal info
    int             sig;
    siginfo_t       info;
    
    // Saved signal mask
    kernel_sigset_t saved_mask;
    
    // Sigreturn trampoline code (if needed)
    uint8_t         retcode[16];
} __attribute__((packed)) signal_frame_t;

// Pending signal queue entry
typedef struct pending_signal {
    int             sig;
    siginfo_t       info;
    struct pending_signal* next;
} pending_signal_t;

// Task signal state (embedded in task_t)
typedef struct task_signal_state {
    struct k_sigaction  action[NSIG];       // Signal handlers
    kernel_sigset_t     blocked;            // Blocked signals mask
    kernel_sigset_t     pending;            // Pending signals bitmask
    pending_signal_t*   pending_queue;      // Queue for siginfo
    kernel_sigset_t     saved_mask;         // Saved mask for sigsuspend
    int                 in_sigsuspend;      // Currently in sigsuspend
    stack_t             altstack;           // Alternate signal stack
    struct k_itimerval  itimer_real;        // ITIMER_REAL
    struct k_itimerval  itimer_virtual;     // ITIMER_VIRTUAL
    struct k_itimerval  itimer_prof;        // ITIMER_PROF
    uint64_t            alarm_ticks;        // alarm() expiration tick
    uint64_t            signal_frame_addr;  // Address of current signal frame (for sigreturn)
} task_signal_state_t;

// Kernel POSIX timer
#define MAX_POSIX_TIMERS 32

typedef struct kernel_timer {
    int             in_use;
    ktimer_t        timerid;
    clockid_t       clockid;
    struct k_sigevent sevp;
    struct k_itimerspec spec;
    uint64_t        next_tick;      // Next expiration in ticks
    uint64_t        interval_ticks; // Interval in ticks
    int             overrun;
    int             owner_pid;
} kernel_timer_t;

// Forward declaration
struct task;

// Saved user context for signal delivery (set by syscall entry)
extern uint64_t syscall_saved_user_rip;
extern uint64_t syscall_saved_user_rsp;
extern uint64_t syscall_saved_user_rflags;
extern uint64_t syscall_saved_user_rbp;
extern uint64_t syscall_saved_user_rbx;
extern uint64_t syscall_saved_user_r12;
extern uint64_t syscall_saved_user_r13;
extern uint64_t syscall_saved_user_r14;
extern uint64_t syscall_saved_user_r15;
extern uint64_t syscall_saved_user_rax;

// Signal API for kernel use
void signal_init_task(struct task* task);
void signal_fork_copy(struct task* child, struct task* parent);
void signal_cleanup_task(struct task* task);
int signal_send(struct task* task, int sig, siginfo_t* info);
int signal_send_group(int pgid, int sig, siginfo_t* info);
int signal_pending(struct task* task);
int signal_should_restart(struct task* task);
int signal_dequeue(struct task* task, kernel_sigset_t* mask, siginfo_t* info);
void signal_deliver(struct task* task);
void signal_check_timers(struct task* task, uint64_t current_tick);

// Signal frame setup/restore for syscall handling
int signal_setup_frame(struct task* task, int sig, siginfo_t* info, struct k_sigaction* act);
int signal_restore_frame(struct task* task);

#endif // _KERNEL_SIGNAL_H_

