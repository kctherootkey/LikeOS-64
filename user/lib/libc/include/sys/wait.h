#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/time.h>

// Options for waitpid
#define WNOHANG     1   // Don't block
#define WUNTRACED   2   // Also wait for stopped children
#define WCONTINUED  8   // Also wait for SIGCONT-resumed children
#define WSTOPPED    WUNTRACED

// Macros to interpret status
#define WIFEXITED(status)    (((status) & 0x7f) == 0)
#define WEXITSTATUS(status)  (((status) >> 8) & 0xff)
/* Terminated by a signal: low 7 bits are the signal, but 0 means normal
 * exit and 0x7f means stopped - both must test false here. */
#define WIFSIGNALED(status)  (((status) & 0x7f) != 0 && ((status) & 0x7f) != 0x7f)
#define WTERMSIG(status)     ((status) & 0x7f)
#define WIFSTOPPED(status)   (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)     (((status) >> 8) & 0xff)
#define WIFCONTINUED(status) ((status) == 0xffff)

/*
 * Resource usage, in the layout every other Unix uses on x86-64: two timevals
 * followed by fourteen longs, 144 bytes in total.
 *
 * The field list is not a menu.  getrusage(2) and wait4(2) take no length
 * argument, so there is no negotiation between the two sides -- the kernel
 * copies a fixed-size structure and this declaration has to be byte-identical
 * to the one it copies.  This used to declare seven fields, 72 bytes, against
 * the kernel's 144, and every getrusage() wrote 72 bytes past the end of the
 * caller's structure.  gdb's get_run_time() keeps its rusage in an 88-byte
 * frame with the return address just past it, so the overflow replaced that
 * return address with a zero and the process jumped to 0 on return.
 *
 * Fields the kernel does not account for yet are reported as zero.  They are
 * declared anyway: dropping them is what caused the overflow, and software
 * ported here refers to ru_inblock, ru_nsignals and the rest by name.
 */
struct rusage {
    struct timeval ru_utime;   /* user CPU time used */
    struct timeval ru_stime;   /* system CPU time used */
    long ru_maxrss;            /* maximum resident set size (KB) */
    long ru_ixrss;             /* integral shared memory size */
    long ru_idrss;             /* integral unshared data size */
    long ru_isrss;             /* integral unshared stack size */
    long ru_minflt;            /* page reclaims (soft page faults) */
    long ru_majflt;            /* page faults (hard page faults) */
    long ru_nswap;             /* swaps */
    long ru_inblock;           /* block input operations */
    long ru_oublock;           /* block output operations */
    long ru_msgsnd;            /* IPC messages sent */
    long ru_msgrcv;            /* IPC messages received */
    long ru_nsignals;          /* signals received */
    long ru_nvcsw;             /* voluntary context switches */
    long ru_nivcsw;            /* involuntary context switches */
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait4(pid_t pid, int* status, int options, struct rusage* rusage);

#ifdef __cplusplus
}
#endif

#endif
