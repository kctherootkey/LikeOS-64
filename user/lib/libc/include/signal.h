#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef volatile int sig_atomic_t;

typedef void (*sighandler_t)(int);
typedef void (*__sighandler_t)(int);

// sigset_t - 64-bit set for signals 1-64 (also exposed by <sys/types.h>,
// as POSIX allows; the guard keeps the two definitions from clashing)
#ifndef __likeos_sigset_t_defined
#define __likeos_sigset_t_defined
typedef unsigned long sigset_t;
#endif

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

// sigprocmask how values
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// Signal numbers
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12

/* si_code values for the synchronous fault signals.
 *
 * A debugger reads these: they are the difference between "it crashed" and
 * "it wrote to an address that is not mapped".  gdb surfaces them through
 * $_siginfo and in the message it prints when the inferior stops. */
#define SEGV_MAPERR 1 /* address not mapped */
#define SEGV_ACCERR 2 /* mapped, but the access was not permitted */
#define ILL_ILLOPC 1 /* illegal opcode */
#define ILL_ILLOPN 2 /* illegal operand */
#define ILL_PRVOPC 5 /* privileged opcode */
#define FPE_INTDIV 1 /* integer divide by zero */
#define FPE_INTOVF 2 /* integer overflow */
#define FPE_FLTDIV 3 /* floating-point divide by zero */
#define FPE_FLTINV 7 /* invalid floating-point operation */
#define BUS_ADRALN 1 /* invalid address alignment */
#define BUS_ADRERR 2 /* non-existent physical address */
#define TRAP_BRKPT 1 /* process breakpoint */
#define TRAP_TRACE 2 /* process trace trap */
#define TRAP_HWBKPT 4 /* hardware breakpoint or watchpoint */

#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGPOLL  29
#define SIGPWR   30
#define SIGSYS   31
#define SIGUNUSED 31
#define SIGRTMIN 32
#define SIGRTMAX 64
#define NSIG     65

// sigaction flags
#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002
#define SA_SIGINFO    0x00000004
#define SA_RESTORER   0x04000000
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000

// si_code values
#define SI_USER    0
#define SI_KERNEL  0x80
#define SI_QUEUE   -1
#define SI_TIMER   -2
#define SI_MESGQ   -3
#define SI_ASYNCIO -4
#define SI_SIGIO   -5
#define SI_TKILL   -6

// Alternate stack flags
#define SS_ONSTACK  1
#define SS_DISABLE  2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

// Interval timer types
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

// Clock IDs for POSIX timers
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

// sigevent notification types
#define SIGEV_SIGNAL  0
#define SIGEV_NONE    1
#define SIGEV_THREAD  2

// Timer flags
#define TIMER_ABSTIME 1

// siginfo_t structure
/*
 * siginfo_t -- the description that comes with a signal.
 *
 * THE LAYOUT IS AN ABI CONTRACT WITH THE KERNEL, which fills one of these in
 * and copies it verbatim: into the signal frame for an SA_SIGINFO handler, and
 * out through PTRACE_GETSIGINFO for a debugger.  Nothing negotiates a size or
 * a field position, so this declaration has to be byte-identical to the
 * kernel's (include/kernel/ke/signal.h) or every field past the third is read
 * from the wrong offset.
 *
 * It was NOT identical: this used to be a flat struct of 120 bytes with
 * si_addr at offset 24, against the kernel's 128 with si_addr at 16.  A
 * handler asking a SIGSEGV where it faulted got whatever happened to sit at
 * offset 24, and PTRACE_GETSIGINFO overran its caller's buffer by 8 bytes --
 * seen as a stack-protector abort with a zeroed canary.
 *
 * The union is what keeps the size fixed at 128 bytes while letting each
 * signal describe itself: only one arm is meaningful for any given signal, and
 * which one is decided by si_signo and si_code.  The accessor macros below are
 * how it is normally read, so `si.si_addr' works without naming the arm.
 */
typedef struct {
    int si_signo;               /* signal number */
    int si_errno;               /* errno value, if any */
    int si_code;                /* what KIND of event -- SEGV_MAPERR etc. */
    int _pad0;
    union {
        int _pad[28];           /* fixes the total at 128 bytes */

        struct {                /* kill(), raise() */
            int      si_pid;
            unsigned si_uid;
        } _kill;

        struct {                /* POSIX timers */
            int   si_tid;
            int   si_overrun;
            int   si_int;
            void *si_ptr;
        } _timer;

        struct {                /* SIGCHLD */
            int      si_pid;
            unsigned si_uid;
            int      si_status;
            long     si_utime;
            long     si_stime;
        } _sigchld;

        struct {                /* SIGILL, SIGFPE, SIGSEGV, SIGBUS */
            void  *si_addr;     /* the address that faulted */
            short  si_addr_lsb;
            short  _pad1;
        } _sigfault;

        struct {                /* SIGPOLL */
            long si_band;
            int  si_fd;
        } _sigpoll;
    } _sifields;
} siginfo_t;

/* Read the union without naming the arm, as every other system spells it. */
#define si_pid     _sifields._kill.si_pid
#define si_uid     _sifields._kill.si_uid
#define si_timerid _sifields._timer.si_tid
#define si_overrun _sifields._timer.si_overrun
#define si_int     _sifields._timer.si_int
#define si_ptr     _sifields._timer.si_ptr
#define si_status  _sifields._sigchld.si_status
#define si_utime   _sifields._sigchld.si_utime
#define si_stime   _sifields._sigchld.si_stime
#define si_addr    _sifields._sigfault.si_addr
#define si_addr_lsb _sifields._sigfault.si_addr_lsb
#define si_band    _sifields._sigpoll.si_band
#define si_fd      _sifields._sigpoll.si_fd

// union sigval
typedef union {
    int   sival_int;
    void* sival_ptr;
} sigval_t;

// sigaction structure (extended for SA_SIGINFO)
struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t*, void*);
    };
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

// Alternate signal stack
typedef struct {
    void*  ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;

// Time structures for timers (only define if not already defined)
#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec {
    long tv_sec;
    long tv_nsec;
};
#endif

#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

#ifndef _STRUCT_ITIMERVAL
#define _STRUCT_ITIMERVAL
struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};
#endif

#ifndef _STRUCT_ITIMERSPEC
#define _STRUCT_ITIMERSPEC
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};
#endif

// sigevent structure
struct sigevent {
    sigval_t sigev_value;
    int      sigev_signo;
    int      sigev_notify;
    void   (*sigev_notify_function)(sigval_t);
    void*    sigev_notify_attributes;
};

// Timer ID type
typedef int timer_t;
typedef int clockid_t;

// Signal functions
int raise(int sig);
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact);
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int sig);
int sigdelset(sigset_t* set, int sig);
int sigismember(const sigset_t* set, int sig);
int sigsuspend(const sigset_t* mask);
int sigpending(sigset_t* set);
int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout);
int sigwaitinfo(const sigset_t* set, siginfo_t* info);
int sigqueue(pid_t pid, int sig, const sigval_t value);

// Process signals
int kill(pid_t pid, int sig);
int killpg(pid_t pgrp, int sig);

// Alternate stack
int sigaltstack(const stack_t* ss, stack_t* old_ss);

// Timers
unsigned int alarm(unsigned int seconds);
int setitimer(int which, const struct itimerval* new_value, struct itimerval* old_value);
int getitimer(int which, struct itimerval* curr_value);

// POSIX timers
int timer_create(clockid_t clockid, struct sigevent* sevp, timer_t* timerid);
int timer_settime(timer_t timerid, int flags, const struct itimerspec* new_value, struct itimerspec* old_value);
int timer_gettime(timer_t timerid, struct itimerspec* curr_value);
int timer_getoverrun(timer_t timerid);
int timer_delete(timer_t timerid);

// Sleep functions
int pause(void);
int nanosleep(const struct timespec* req, struct timespec* rem);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

#ifdef __cplusplus
}
#endif

#endif
