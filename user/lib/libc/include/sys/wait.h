#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>
#include <sys/time.h>

// Options for waitpid
#define WNOHANG     1   // Don't block
#define WUNTRACED   2   // Also wait for stopped children

// Macros to interpret status
#define WIFEXITED(status)    (((status) & 0x7f) == 0)
#define WEXITSTATUS(status)  (((status) >> 8) & 0xff)
#define WIFSIGNALED(status)  (((status) & 0x7f) != 0)
#define WTERMSIG(status)     ((status) & 0x7f)
#define WIFSTOPPED(status)   (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)     (((status) >> 8) & 0xff)

/* Resource usage (subset used by wait4/getrusage) */
struct rusage {
    struct timeval ru_utime;   /* user CPU time used */
    struct timeval ru_stime;   /* system CPU time used */
    long ru_maxrss;            /* maximum resident set size (KB) */
    long ru_minflt;            /* page faults (soft) */
    long ru_majflt;            /* page faults (hard) */
    long ru_nvcsw;             /* voluntary context switches */
    long ru_nivcsw;            /* involuntary context switches */
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait4(pid_t pid, int* status, int options, struct rusage* rusage);

#endif
