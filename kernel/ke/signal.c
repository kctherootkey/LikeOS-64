// LikeOS-64 Kernel Signal Implementation
#include "../../include/kernel/signal.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/status.h"
#include "../../include/kernel/syscall.h"

// Extern: signal number to pass to handler (set by signal_setup_frame)
extern volatile uint64_t syscall_signal_pending;

// Global POSIX timer pool
static kernel_timer_t g_posix_timers[MAX_POSIX_TIMERS];
static ktimer_t g_next_timerid = 1;

// Initialize signal state for a new task
void signal_init_task(task_t* task) {
    if (!task) return;
    
    task_signal_state_t* sig = &task->signals;
    
    // Clear all handlers to default
    for (int i = 0; i < NSIG; i++) {
        sig->action[i].sa_handler = SIG_DFL;
        sig->action[i].sa_flags = 0;
        sig->action[i].sa_restorer = NULL;
        sigemptyset_k(&sig->action[i].sa_mask);
    }
    
    // Clear blocked and pending masks
    sigemptyset_k(&sig->blocked);
    sigemptyset_k(&sig->pending);
    sigemptyset_k(&sig->saved_mask);
    
    sig->pending_queue = NULL;
    sig->in_sigsuspend = 0;
    
    // Clear alternate stack
    sig->altstack.ss_sp = NULL;
    sig->altstack.ss_flags = SS_DISABLE;
    sig->altstack.ss_size = 0;
    
    // Clear timers
    mm_memset(&sig->itimer_real, 0, sizeof(sig->itimer_real));
    mm_memset(&sig->itimer_virtual, 0, sizeof(sig->itimer_virtual));
    mm_memset(&sig->itimer_prof, 0, sizeof(sig->itimer_prof));
    sig->alarm_ticks = 0;
    
    // Clear signal frame address
    sig->signal_frame_addr = 0;
}

// Copy signal handlers from parent to child during fork
// POSIX: signal dispositions are inherited across fork
void signal_fork_copy(task_t* child, task_t* parent) {
    if (!child || !parent) return;
    
    task_signal_state_t* csig = &child->signals;
    task_signal_state_t* psig = &parent->signals;
    
    // Copy signal handlers (dispositions are inherited)
    for (int i = 0; i < NSIG; i++) {
        csig->action[i] = psig->action[i];
    }
    
    // Copy blocked mask (inherited across fork)
    csig->blocked = psig->blocked;
    
    // Clear pending signals (not inherited - child starts fresh)
    sigemptyset_k(&csig->pending);
    csig->pending_queue = NULL;
    
    // Clear saved mask and sigsuspend state
    sigemptyset_k(&csig->saved_mask);
    csig->in_sigsuspend = 0;
    
    // Alternate stack is NOT inherited across fork
    csig->altstack.ss_sp = NULL;
    csig->altstack.ss_flags = SS_DISABLE;
    csig->altstack.ss_size = 0;
    
    // Timers are NOT inherited (child gets fresh timer state)
    mm_memset(&csig->itimer_real, 0, sizeof(csig->itimer_real));
    mm_memset(&csig->itimer_virtual, 0, sizeof(csig->itimer_virtual));
    mm_memset(&csig->itimer_prof, 0, sizeof(csig->itimer_prof));
    csig->alarm_ticks = 0;
    
    // Clear signal frame address
    csig->signal_frame_addr = 0;
}

// Cleanup signal state when task exits
void signal_cleanup_task(task_t* task) {
    if (!task) return;
    
    task_signal_state_t* sig = &task->signals;
    
    // Free pending signal queue
    pending_signal_t* ps = sig->pending_queue;
    while (ps) {
        pending_signal_t* next = ps->next;
        kfree(ps);
        ps = next;
    }
    sig->pending_queue = NULL;
    
    // Clear any POSIX timers owned by this task
    for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
        if (g_posix_timers[i].in_use && g_posix_timers[i].owner_pid == task->id) {
            g_posix_timers[i].in_use = 0;
        }
    }
}

// Allocate a pending signal entry
static pending_signal_t* alloc_pending_signal(void) {
    pending_signal_t* ps = (pending_signal_t*)kalloc(sizeof(pending_signal_t));
    if (ps) {
        mm_memset(ps, 0, sizeof(*ps));
    }
    return ps;
}

// Send a signal to a task
int signal_send(task_t* task, int sig, siginfo_t* info) {
    if (!task || sig <= 0 || sig >= NSIG) {
        return -EINVAL;
    }
    
    task_signal_state_t* sigstate = &task->signals;
    struct k_sigaction* act = &sigstate->action[sig];
    
    // Check if signal is ignored (except SIGKILL/SIGSTOP)
    if (!sig_kernel_only(sig)) {
        if (act->sa_handler == SIG_IGN) {
            return 0;  // Signal ignored
        }
        // Check default action is ignore
        if (act->sa_handler == SIG_DFL && sig_default_action(sig) == SIG_DFL_IGN) {
            return 0;
        }
    }
    
    // Add to pending mask
    sigaddset_k(&sigstate->pending, sig);
    
    // Queue siginfo if provided and SA_SIGINFO is set
    if (info && (act->sa_flags & SA_SIGINFO)) {
        pending_signal_t* ps = alloc_pending_signal();
        if (ps) {
            ps->sig = sig;
            mm_memcpy(&ps->info, info, sizeof(siginfo_t));
            ps->next = sigstate->pending_queue;
            sigstate->pending_queue = ps;
        }
    }
    
    // Wake task if blocked - any signal that isn't blocked should wake the task
    if (task->state == TASK_BLOCKED || task->state == TASK_STOPPED) {
        int should_wake = 0;
        
        // SIGCONT always continues a stopped process
        if (sig == SIGCONT && task->state == TASK_STOPPED) {
            task->state = TASK_READY;
            should_wake = 1;
        }
        
        // Any unblocked signal wakes a blocked task
        // SIGKILL/SIGSTOP can't be blocked
        if (!should_wake && task->state == TASK_BLOCKED) {
            if (sig_kernel_only(sig) || !sigismember_k(&sigstate->blocked, sig)) {
                task->state = TASK_READY;
                task->wait_channel = 0;  // Clear wait channel so it doesn't re-block
            }
        }
    }
    
    return 0;
}

// Send signal to a process group
int signal_send_group(int pgid, int sig, siginfo_t* info) {
    if (pgid <= 0 || sig <= 0 || sig >= NSIG) {
        return -EINVAL;
    }
    
    int found = 0;
    
    // Iterate through all tasks (simplified - would use a proper list in production)
    for (int pid = 1; pid < 256; pid++) {
        task_t* t = sched_find_task_by_id(pid);
        if (t && t->pgid == pgid) {
            signal_send(t, sig, info);
            found++;
        }
    }
    
    return found > 0 ? 0 : -ESRCH;
}

// Check if any unblocked signals are pending
int signal_pending(task_t* task) {
    if (!task) return 0;
    
    task_signal_state_t* sig = &task->signals;
    kernel_sigset_t unblocked;
    
    // Compute pending & ~blocked
    signandset_k(&unblocked, &sig->pending, &sig->blocked);
    
    return !sigisemptyset_k(&unblocked);
}

// Check if all pending signals have SA_RESTART set
// Returns 1 if syscall should be restarted (all signals have SA_RESTART)
// Returns 0 if syscall should return -EINTR (at least one signal lacks SA_RESTART)
int signal_should_restart(task_t* task) {
    if (!task) return 0;
    
    task_signal_state_t* sig = &task->signals;
    
    // Check all pending, unblocked signals
    for (int s = 1; s < NSIG; s++) {
        if (sigismember_k(&sig->pending, s)) {
            // Skip blocked signals (except SIGKILL/SIGSTOP)
            if (!sig_kernel_only(s) && sigismember_k(&sig->blocked, s)) {
                continue;
            }
            // Check if this signal's handler has SA_RESTART
            if (!(sig->action[s].sa_flags & SA_RESTART)) {
                return 0;  // At least one signal lacks SA_RESTART
            }
        }
    }
    
    return 1;  // All pending signals have SA_RESTART (or no pending signals)
}

// Dequeue a pending signal (returns signal number, 0 if none)
int signal_dequeue(task_t* task, kernel_sigset_t* mask, siginfo_t* info) {
    if (!task) return 0;
    
    task_signal_state_t* sig = &task->signals;
    kernel_sigset_t effective_mask;
    
    // Use provided mask or current blocked mask
    if (mask) {
        effective_mask = *mask;
    } else {
        effective_mask = sig->blocked;
    }
    
    // Find first unblocked pending signal
    // Priority: SIGKILL, SIGSTOP first, then by number
    for (int s = 1; s < NSIG; s++) {
        int signum = s;
        // Check SIGKILL/SIGSTOP first
        if (s == 1) signum = SIGKILL;
        else if (s == 2) signum = SIGSTOP;
        else if (s <= SIGKILL) signum = s - 2;
        else if (s <= SIGSTOP) signum = s - 1;
        else signum = s;
        
        // Simplified: just iterate in order
        signum = s;
        
        if (sigismember_k(&sig->pending, signum)) {
            // Check if blocked (SIGKILL/SIGSTOP can't be blocked)
            if (!sig_kernel_only(signum) && sigismember_k(&effective_mask, signum)) {
                continue;
            }
            
            // Remove from pending
            sigdelset_k(&sig->pending, signum);
            
            // Find and remove from queue if present
            if (info) {
                mm_memset(info, 0, sizeof(*info));
                info->si_signo = signum;
                info->si_code = SI_USER;
                
                pending_signal_t** pp = &sig->pending_queue;
                while (*pp) {
                    if ((*pp)->sig == signum) {
                        pending_signal_t* ps = *pp;
                        *pp = ps->next;
                        mm_memcpy(info, &ps->info, sizeof(siginfo_t));
                        kfree(ps);
                        break;
                    }
                    pp = &(*pp)->next;
                }
            }
            
            return signum;
        }
    }
    
    return 0;
}

// Setup a signal frame on the user stack
// Returns 0 on success, -1 on failure
int signal_setup_frame(task_t* task, int sig, siginfo_t* info, struct k_sigaction* act) {
    if (!task || !act) return -1;
    
    // Get current user context from task's saved syscall registers (per-task, not globals)
    uint64_t user_rsp = task->syscall_rsp;
    uint64_t user_rip = task->syscall_rip;
    uint64_t user_rflags = task->syscall_rflags;
    
    // Calculate new stack position for signal frame (16-byte aligned)
    uint64_t frame_addr = (user_rsp - sizeof(signal_frame_t)) & ~0xFULL;
    
    // Validate the stack address is in user space
    if (frame_addr < 0x10000 || frame_addr >= 0x7FFFFFFFFFFF) {
        return -1;
    }
    
    // Build the signal frame in kernel memory first
    signal_frame_t kframe;
    mm_memset(&kframe, 0, sizeof(kframe));
    
    // Save all registers
    kframe.rip = user_rip;
    kframe.rsp = user_rsp;
    kframe.rflags = user_rflags;
    kframe.rbp = task->syscall_rbp;
    kframe.rbx = task->syscall_rbx;
    kframe.r12 = task->syscall_r12;
    kframe.r13 = task->syscall_r13;
    kframe.r14 = task->syscall_r14;
    kframe.r15 = task->syscall_r15;
    kframe.rcx = 0;
    kframe.rdx = 0;
    kframe.rsi = 0;
    kframe.rdi = 0;
    kframe.r8 = 0;
    kframe.r9 = 0;
    kframe.r10 = 0;
    kframe.r11 = 0;
    
    // Save syscall return value for sigreturn
    kframe.rax = task->syscall_rax;
    
    // Signal info
    kframe.sig = sig;
    if (info) {
        mm_memcpy(&kframe.info, info, sizeof(siginfo_t));
    }
    
    // Save current blocked mask
    kframe.saved_mask = task->signals.blocked;
    
    // Set up sigreturn trampoline code in the frame
    // mov rax, SYS_RT_SIGRETURN (256)
    // syscall
    // This is the fallback if sa_restorer is not set
    kframe.retcode[0] = 0x48;  // REX.W
    kframe.retcode[1] = 0xc7;  // mov rax, imm32
    kframe.retcode[2] = 0xc0;  
    kframe.retcode[3] = 0x00;  // SYS_RT_SIGRETURN = 256 = 0x100
    kframe.retcode[4] = 0x01;
    kframe.retcode[5] = 0x00;
    kframe.retcode[6] = 0x00;
    kframe.retcode[7] = 0x0f;  // syscall
    kframe.retcode[8] = 0x05;
    
    // Set return address - use sa_restorer if provided, else use embedded trampoline
    if (act->sa_restorer) {
        kframe.pretcode = (uint64_t)act->sa_restorer;
    } else {
        // Point to the retcode in the frame itself
        kframe.pretcode = frame_addr + __builtin_offsetof(signal_frame_t, retcode);
    }
    
    // Copy frame to user stack (SMAP-aware)
    smap_disable();
    mm_memcpy((void*)frame_addr, &kframe, sizeof(kframe));
    smap_enable();
    
    // Update signal mask - block sa_mask and current signal (unless SA_NODEFER)
    sigorset_k(&task->signals.blocked, &task->signals.blocked, &act->sa_mask);
    if (!(act->sa_flags & SA_NODEFER)) {
        sigaddset_k(&task->signals.blocked, sig);
    }
    
    // Reset handler if SA_RESETHAND
    if (act->sa_flags & SA_RESETHAND) {
        act->sa_handler = SIG_DFL;
    }
    
    // Save frame address in task for sigreturn to find
    task->signals.signal_frame_addr = frame_addr;
    
    // Also update task's saved values
    task->syscall_rsp = frame_addr;
    task->syscall_rip = (uint64_t)act->sa_handler;
    
    // CRITICAL: Disable interrupts before modifying global syscall return context
    // This prevents a race where a timer interrupt could cause a context switch
    // to another task that overwrites these globals before we return via sysret
    __asm__ volatile("cli" ::: "memory");
    
    // Modify the syscall return context to call the signal handler
    // RSP = signal frame (handler should see pretcode as return address)
    // RIP = handler address
    syscall_saved_user_rsp = frame_addr;
    syscall_saved_user_rip = (uint64_t)act->sa_handler;
    
    // Set signal pending flag - this tells syscall.asm to use signal return path
    // The value is the signal number which will be loaded into RDI
    // NOTE: Interrupts remain disabled until after sysret in syscall.asm
    syscall_signal_pending = (uint64_t)sig;
    
    return 0;
}

// Restore context from signal frame (called by sys_rt_sigreturn)
int signal_restore_frame(task_t* task) {
    if (!task) return -1;
    
    // Get the frame address that was saved when the frame was set up
    uint64_t frame_addr = task->signals.signal_frame_addr;
    
    // Validate
    if (frame_addr < 0x10000 || frame_addr >= 0x7FFFFFFFFFFF) {
        kprintf("signal_restore_frame: invalid frame addr 0x%lx\n", frame_addr);
        return -1;
    }
    
    // Clear the saved frame address
    task->signals.signal_frame_addr = 0;
    
    // Read the frame from user space
    signal_frame_t kframe;
    smap_disable();
    mm_memcpy(&kframe, (void*)frame_addr, sizeof(kframe));
    smap_enable();
    
    // Update task's saved values first (safe without cli)
    task->syscall_rip = kframe.rip;
    task->syscall_rsp = kframe.rsp;
    task->syscall_rflags = kframe.rflags;
    task->syscall_rbp = kframe.rbp;
    task->syscall_rbx = kframe.rbx;
    task->syscall_r12 = kframe.r12;
    task->syscall_r13 = kframe.r13;
    task->syscall_r14 = kframe.r14;
    task->syscall_r15 = kframe.r15;
    
    // Save RAX (syscall return value) for assembly to restore
    task->syscall_rax = kframe.rax;
    
    // Restore signal mask
    task->signals.blocked = kframe.saved_mask;
    
    // Clear sigsuspend flag if set
    task->signals.in_sigsuspend = 0;
    
    // CRITICAL: Disable interrupts before modifying global syscall return context
    // This prevents a race where a timer interrupt could cause a context switch
    // to another task that overwrites these globals before we return via sysret
    __asm__ volatile("cli" ::: "memory");
    
    // Restore registers to globals (output to syscall.asm)
    syscall_saved_user_rip = kframe.rip;
    syscall_saved_user_rsp = kframe.rsp;
    syscall_saved_user_rflags = kframe.rflags;
    syscall_saved_user_rbp = kframe.rbp;
    syscall_saved_user_rbx = kframe.rbx;
    syscall_saved_user_r12 = kframe.r12;
    syscall_saved_user_r13 = kframe.r13;
    syscall_saved_user_r14 = kframe.r14;
    syscall_saved_user_r15 = kframe.r15;
    syscall_saved_user_rax = kframe.rax;  // Syscall return value (e.g., -EINTR)
    
    // Tell syscall.asm to use the restored context
    // Use special value 0xFFFFFFFFFFFFFFFF (-1) to indicate sigreturn (not a handler call)
    // NOTE: Interrupts remain disabled until after sysret in syscall.asm
    syscall_signal_pending = 0xFFFFFFFFFFFFFFFFULL;
    
    return 0;
}

// Deliver pending signals to a task (called before returning to userspace)
void signal_deliver(task_t* task) {
    if (!task || task->privilege != TASK_USER) return;
    
    task_signal_state_t* sig = &task->signals;
    siginfo_t info;
    
    int signum = signal_dequeue(task, NULL, &info);
    if (signum == 0) return;
    
    struct k_sigaction* act = &sig->action[signum];
    
    // Handle based on disposition
    if (act->sa_handler == SIG_IGN) {
        return;  // Ignore
    }
    
    if (act->sa_handler == SIG_DFL) {
        // Default action
        int action = sig_default_action(signum);
        switch (action) {
            case SIG_DFL_TERM:
            case SIG_DFL_CORE:
                // Terminate (core dump not implemented)
                sched_mark_task_exited(task, 128 + signum);
                break;
            case SIG_DFL_STOP:
                task->state = TASK_STOPPED;
                // Notify parent
                if (task->parent) {
                    siginfo_t chld_info;
                    mm_memset(&chld_info, 0, sizeof(chld_info));
                    chld_info.si_signo = SIGCHLD;
                    chld_info.si_code = CLD_STOPPED;
                    chld_info.si_pid = task->id;
                    chld_info.si_status = signum;
                    signal_send(task->parent, SIGCHLD, &chld_info);
                }
                break;
            case SIG_DFL_CONT:
                if (task->state == TASK_STOPPED) {
                    task->state = TASK_READY;
                }
                break;
            case SIG_DFL_IGN:
                // Ignore
                break;
        }
        return;
    }
    
    // User-defined handler - set up signal frame
    if (signal_setup_frame(task, signum, &info, act) < 0) {
        // Failed to set up frame - terminate with signal
        sched_mark_task_exited(task, 128 + signum);
    }
}

// Check and fire interval timers (called from timer tick)
void signal_check_timers(task_t* task, uint64_t current_tick) {
    if (!task) return;
    
    task_signal_state_t* sig = &task->signals;
    
    // Check alarm
    if (sig->alarm_ticks > 0 && current_tick >= sig->alarm_ticks) {
        sig->alarm_ticks = 0;
        siginfo_t info;
        mm_memset(&info, 0, sizeof(info));
        info.si_signo = SIGALRM;
        info.si_code = SI_TIMER;
        signal_send(task, SIGALRM, &info);
    }
    
    // Check ITIMER_REAL
    if (sig->itimer_real.it_value.tv_sec > 0 || sig->itimer_real.it_value.tv_usec > 0) {
        // Decrement timer (simplified: assume 10ms per tick at 100Hz)
        int64_t usec = sig->itimer_real.it_value.tv_usec - 10000;
        if (usec < 0) {
            sig->itimer_real.it_value.tv_sec--;
            usec += 1000000;
        }
        sig->itimer_real.it_value.tv_usec = usec;
        
        if (sig->itimer_real.it_value.tv_sec <= 0 && sig->itimer_real.it_value.tv_usec <= 0) {
            // Timer expired
            siginfo_t info;
            mm_memset(&info, 0, sizeof(info));
            info.si_signo = SIGALRM;
            info.si_code = SI_TIMER;
            signal_send(task, SIGALRM, &info);
            
            // Reload if interval set
            if (sig->itimer_real.it_interval.tv_sec > 0 || sig->itimer_real.it_interval.tv_usec > 0) {
                sig->itimer_real.it_value = sig->itimer_real.it_interval;
            }
        }
    }
}

// POSIX timer functions

ktimer_t timer_create_internal(task_t* task, clockid_t clockid, struct k_sigevent* sevp) {
    if (!task) return -1;
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
        if (!g_posix_timers[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) return -1;
    
    kernel_timer_t* kt = &g_posix_timers[slot];
    kt->in_use = 1;
    kt->timerid = g_next_timerid++;
    kt->clockid = clockid;
    kt->owner_pid = task->id;
    kt->overrun = 0;
    kt->next_tick = 0;
    kt->interval_ticks = 0;
    
    if (sevp) {
        mm_memcpy(&kt->sevp, sevp, sizeof(struct k_sigevent));
    } else {
        // Default: SIGEV_SIGNAL with SIGALRM
        kt->sevp.sigev_notify = SIGEV_SIGNAL;
        kt->sevp.sigev_signo = SIGALRM;
    }
    
    mm_memset(&kt->spec, 0, sizeof(kt->spec));
    
    return kt->timerid;
}

int timer_settime_internal(ktimer_t timerid, int flags, 
                           const struct k_itimerspec* new_value,
                           struct k_itimerspec* old_value) {
    (void)flags;  // TODO: handle TIMER_ABSTIME
    
    // Find timer
    kernel_timer_t* kt = NULL;
    for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
        if (g_posix_timers[i].in_use && g_posix_timers[i].timerid == timerid) {
            kt = &g_posix_timers[i];
            break;
        }
    }
    
    if (!kt) return -EINVAL;
    
    if (old_value) {
        mm_memcpy(old_value, &kt->spec, sizeof(struct k_itimerspec));
    }
    
    if (new_value) {
        mm_memcpy(&kt->spec, new_value, sizeof(struct k_itimerspec));
        
        // Calculate next expiration in ticks (100Hz = 10ms per tick)
        uint64_t current = timer_ticks();
        uint64_t nsec = new_value->it_value.tv_sec * 1000000000ULL + new_value->it_value.tv_nsec;
        uint64_t ticks = nsec / 10000000;  // 10ms per tick
        kt->next_tick = current + ticks;
        
        // Calculate interval
        nsec = new_value->it_interval.tv_sec * 1000000000ULL + new_value->it_interval.tv_nsec;
        kt->interval_ticks = nsec / 10000000;
        
        kt->overrun = 0;
    }
    
    return 0;
}

int timer_gettime_internal(ktimer_t timerid, struct k_itimerspec* curr_value) {
    kernel_timer_t* kt = NULL;
    for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
        if (g_posix_timers[i].in_use && g_posix_timers[i].timerid == timerid) {
            kt = &g_posix_timers[i];
            break;
        }
    }
    
    if (!kt) return -EINVAL;
    
    if (curr_value) {
        // Calculate remaining time
        uint64_t current = timer_ticks();
        if (kt->next_tick > current) {
            uint64_t remaining = (kt->next_tick - current) * 10000000;  // ns
            curr_value->it_value.tv_sec = remaining / 1000000000ULL;
            curr_value->it_value.tv_nsec = remaining % 1000000000ULL;
        } else {
            curr_value->it_value.tv_sec = 0;
            curr_value->it_value.tv_nsec = 0;
        }
        curr_value->it_interval = kt->spec.it_interval;
    }
    
    return 0;
}

int timer_getoverrun_internal(ktimer_t timerid) {
    for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
        if (g_posix_timers[i].in_use && g_posix_timers[i].timerid == timerid) {
            return g_posix_timers[i].overrun;
        }
    }
    return -EINVAL;
}

int timer_delete_internal(ktimer_t timerid) {
    for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
        if (g_posix_timers[i].in_use && g_posix_timers[i].timerid == timerid) {
            g_posix_timers[i].in_use = 0;
            return 0;
        }
    }
    return -EINVAL;
}

// Check and fire POSIX timers (called from timer tick)
void signal_check_posix_timers(uint64_t current_tick) {
    for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
        kernel_timer_t* kt = &g_posix_timers[i];
        if (!kt->in_use) continue;
        if (kt->next_tick == 0) continue;
        
        if (current_tick >= kt->next_tick) {
            // Timer expired
            task_t* owner = sched_find_task_by_id(kt->owner_pid);
            if (owner && kt->sevp.sigev_notify == SIGEV_SIGNAL) {
                siginfo_t info;
                mm_memset(&info, 0, sizeof(info));
                info.si_signo = kt->sevp.sigev_signo;
                info.si_code = SI_TIMER;
                info.si_timerid = kt->timerid;
                info.si_overrun = kt->overrun;
                signal_send(owner, kt->sevp.sigev_signo, &info);
            }
            
            // Reload or disarm
            if (kt->interval_ticks > 0) {
                // Count overruns
                while (kt->next_tick <= current_tick) {
                    kt->next_tick += kt->interval_ticks;
                    kt->overrun++;
                }
                kt->overrun--;  // First expiration isn't an overrun
            } else {
                kt->next_tick = 0;  // Disarm
            }
        }
    }
}
