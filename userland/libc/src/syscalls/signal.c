#include "../../include/signal.h"
#include "../../include/errno.h"
#include "../../include/unistd.h"

#define MAX_SIG 32

static sighandler_t g_handlers[MAX_SIG];
static sigset_t g_mask = 0;
static sigset_t g_pending = 0;

static void handle_signal(int sig) {
    if (sig <= 0 || sig >= MAX_SIG) {
        return;
    }
    sighandler_t h = g_handlers[sig];
    if (h == SIG_IGN) {
        return;
    }
    if (h == SIG_DFL || h == 0) {
        // Default: ignore SIGCHLD, terminate otherwise
        if (sig == SIGCHLD) {
            return;
        }
        _exit(128 + sig);
    }
    h(sig);
}

int raise(int sig) {
    if (sig <= 0 || sig >= MAX_SIG) {
        errno = EINVAL;
        return -1;
    }
    if (g_mask & (1UL << sig)) {
        g_pending |= (1UL << sig);
        return 0;
    }
    handle_signal(sig);
    return 0;
}

sighandler_t signal(int sig, sighandler_t handler) {
    if (sig <= 0 || sig >= MAX_SIG) {
        errno = EINVAL;
        return SIG_ERR;
    }
    sighandler_t old = g_handlers[sig];
    g_handlers[sig] = handler;
    return old ? old : SIG_DFL;
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    if (sig <= 0 || sig >= MAX_SIG) {
        errno = EINVAL;
        return -1;
    }
    if (oldact) {
        oldact->sa_handler = g_handlers[sig] ? g_handlers[sig] : SIG_DFL;
        oldact->sa_mask = g_mask;
        oldact->sa_flags = 0;
    }
    if (act) {
        g_handlers[sig] = act->sa_handler;
    }
    return 0;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    if (oldset) *oldset = g_mask;
    if (!set) return 0;
    switch (how) {
        case 0: // SIG_BLOCK
            g_mask |= *set;
            break;
        case 1: // SIG_UNBLOCK
            g_mask &= ~(*set);
            break;
        case 2: // SIG_SETMASK
            g_mask = *set;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    // deliver any pending unblocked signals
    for (int sig = 1; sig < MAX_SIG; ++sig) {
        if ((g_pending & (1UL << sig)) && !(g_mask & (1UL << sig))) {
            g_pending &= ~(1UL << sig);
            handle_signal(sig);
        }
    }
    return 0;
}

int sigemptyset(sigset_t* set) {
    if (!set) { errno = EINVAL; return -1; }
    *set = 0;
    return 0;
}

int sigaddset(sigset_t* set, int sig) {
    if (!set || sig <= 0 || sig >= MAX_SIG) { errno = EINVAL; return -1; }
    *set |= (1UL << sig);
    return 0;
}

int sigdelset(sigset_t* set, int sig) {
    if (!set || sig <= 0 || sig >= MAX_SIG) { errno = EINVAL; return -1; }
    *set &= ~(1UL << sig);
    return 0;
}

int sigismember(const sigset_t* set, int sig) {
    if (!set || sig <= 0 || sig >= MAX_SIG) { errno = EINVAL; return -1; }
    return ((*set) & (1UL << sig)) ? 1 : 0;
}

int sigsuspend(const sigset_t* mask) {
    if (mask) {
        g_mask = *mask;
    }
    errno = EINTR;
    return -1;
}
