#include "../../include/signal.h"
#include "../../include/errno.h"
#include "../../include/unistd.h"
#include "../../include/time.h"
#include "syscall.h"
#include "../../include/string.h"

#define _NSIG_WORDS (sizeof(sigset_t))

// ============================================================================
// sigset manipulation functions
// ============================================================================

int sigemptyset(sigset_t* set) {
    if (!set) { 
        errno = EINVAL; 
        return -1; 
    }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t* set) {
    if (!set) { 
        errno = EINVAL; 
        return -1; 
    }
    *set = ~0UL;
    return 0;
}

int sigaddset(sigset_t* set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) { 
        errno = EINVAL; 
        return -1; 
    }
    *set |= (1UL << (sig - 1));
    return 0;
}

int sigdelset(sigset_t* set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) { 
        errno = EINVAL; 
        return -1; 
    }
    *set &= ~(1UL << (sig - 1));
    return 0;
}

int sigismember(const sigset_t* set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) { 
        errno = EINVAL; 
        return -1; 
    }
    return ((*set) & (1UL << (sig - 1))) ? 1 : 0;
}

// ============================================================================
// Signal handlers
// ============================================================================

// External sigreturn trampoline (defined in crt0.S)
extern void __restore_rt(void);

sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction act, oldact;
    
    if (sig <= 0 || sig >= NSIG) {
        errno = EINVAL;
        return SIG_ERR;
    }
    
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART | SA_RESTORER;  // Use restorer
    act.sa_restorer = __restore_rt;
    
    if (sigaction(sig, &act, &oldact) < 0) {
        return SIG_ERR;
    }
    
    return oldact.sa_handler;
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) {
        errno = EINVAL;
        return -1;
    }
    
    // If act is provided and doesn't have a restorer set, we must provide one
    // because the kernel can't execute code on the non-executable stack
    struct sigaction modified_act;
    const struct sigaction* act_to_use = act;
    
    if (act && !(act->sa_flags & SA_RESTORER)) {
        modified_act = *act;
        modified_act.sa_flags |= SA_RESTORER;
        modified_act.sa_restorer = __restore_rt;
        act_to_use = &modified_act;
    }
    
    long ret = syscall4(SYS_RT_SIGACTION, sig, (long)act_to_use, (long)oldact, _NSIG_WORDS);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

// ============================================================================
// Signal mask manipulation
// ============================================================================

int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    long ret = syscall4(SYS_RT_SIGPROCMASK, how, (long)set, (long)oldset, _NSIG_WORDS);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int sigpending(sigset_t* set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    
    long ret = syscall2(SYS_RT_SIGPENDING, (long)set, _NSIG_WORDS);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int sigsuspend(const sigset_t* mask) {
    if (!mask) {
        errno = EINVAL;
        return -1;
    }
    
    long ret = syscall2(SYS_RT_SIGSUSPEND, (long)mask, _NSIG_WORDS);
    // sigsuspend always returns -1 with errno = EINTR
    if (ret < 0) {
        errno = (int)(-ret);
    }
    return -1;
}

// ============================================================================
// Signal sending
// ============================================================================

int raise(int sig) {
    // raise sends signal to current thread
    pid_t pid = getpid();
    return kill(pid, sig);
}

int kill(pid_t pid, int sig) {
    long ret = syscall2(SYS_KILL, pid, sig);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int killpg(pid_t pgrp, int sig) {
    // killpg(pgrp, sig) is equivalent to kill(-pgrp, sig)
    if (pgrp <= 0) {
        errno = EINVAL;
        return -1;
    }
    return kill(-pgrp, sig);
}

int sigqueue(pid_t pid, int sig, const sigval_t value) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code = SI_QUEUE;
    info.si_pid = getpid();
    info.si_value.si_value_ptr = value.sival_ptr;
    
    long ret = syscall3(SYS_RT_SIGQUEUEINFO, pid, sig, (long)&info);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

// ============================================================================
// Signal waiting
// ============================================================================

int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    
    long ret = syscall4(SYS_RT_SIGTIMEDWAIT, (long)set, (long)info, (long)timeout, _NSIG_WORDS);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;  // Returns signal number
}

int sigwaitinfo(const sigset_t* set, siginfo_t* info) {
    return sigtimedwait(set, info, 0);  // No timeout = wait indefinitely
}

// ============================================================================
// Alternate signal stack
// ============================================================================

int sigaltstack(const stack_t* ss, stack_t* old_ss) {
    long ret = syscall2(SYS_SIGALTSTACK, (long)ss, (long)old_ss);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

// ============================================================================
// Timers
// ============================================================================

unsigned int alarm(unsigned int seconds) {
    long ret = syscall1(SYS_ALARM, seconds);
    if (ret < 0) {
        return 0;  // alarm doesn't set errno
    }
    return (unsigned int)ret;
}

int setitimer(int which, const struct itimerval* new_value, struct itimerval* old_value) {
    long ret = syscall3(SYS_SETITIMER, which, (long)new_value, (long)old_value);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int getitimer(int which, struct itimerval* curr_value) {
    long ret = syscall2(SYS_GETITIMER, which, (long)curr_value);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

// ============================================================================
// POSIX timers
// ============================================================================

int timer_create(clockid_t clockid, struct sigevent* sevp, timer_t* timerid) {
    if (!timerid) {
        errno = EINVAL;
        return -1;
    }
    
    long ret = syscall3(SYS_TIMER_CREATE, clockid, (long)sevp, (long)timerid);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int timer_settime(timer_t timerid, int flags, const struct itimerspec* new_value, struct itimerspec* old_value) {
    if (!new_value) {
        errno = EINVAL;
        return -1;
    }
    
    long ret = syscall4(SYS_TIMER_SETTIME, timerid, flags, (long)new_value, (long)old_value);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int timer_gettime(timer_t timerid, struct itimerspec* curr_value) {
    if (!curr_value) {
        errno = EINVAL;
        return -1;
    }
    
    long ret = syscall2(SYS_TIMER_GETTIME, timerid, (long)curr_value);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int timer_getoverrun(timer_t timerid) {
    long ret = syscall1(SYS_TIMER_GETOVERRUN, timerid);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int timer_delete(timer_t timerid) {
    long ret = syscall1(SYS_TIMER_DELETE, timerid);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

// ============================================================================
// Sleep functions
// ============================================================================

int pause(void) {
    long ret = syscall0(SYS_PAUSE);
    // pause always returns -1 with errno = EINTR
    errno = (ret < 0) ? (int)(-ret) : EINTR;
    return -1;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }
    
    long ret = syscall2(SYS_NANOSLEEP, (long)req, (long)rem);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req, rem;
    req.tv_sec = (long)seconds;
    req.tv_nsec = 0;
    
    if (nanosleep(&req, &rem) < 0) {
        if (errno == EINTR) {
            return (unsigned int)rem.tv_sec;
        }
        return seconds;
    }
    return 0;
}

int usleep(unsigned int usec) {
    struct timespec req, rem;
    req.tv_sec = usec / 1000000;
    req.tv_nsec = (usec % 1000000) * 1000;
    
    if (nanosleep(&req, &rem) < 0 && errno != EINTR) {
        return -1;
    }
    return 0;
}

int clock_gettime(clockid_t clk_id, struct timespec* tp) {
    long ret = syscall2(SYS_CLOCK_GETTIME, (long)clk_id, (long)tp);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int clock_getres(clockid_t clk_id, struct timespec* res) {
    long ret = syscall2(SYS_CLOCK_GETRES, (long)clk_id, (long)res);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}
