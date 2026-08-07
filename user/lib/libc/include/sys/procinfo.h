#ifndef _SYS_PROCINFO_H
#define _SYS_PROCINFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Process information structure (matches kernel procinfo_t) */
typedef struct procinfo {
    int     pid;            /* Process ID */
    int     ppid;           /* Parent PID */
    int     tgid;           /* Thread group ID */
    int     pgid;           /* Process group ID */
    int     sid;            /* Session ID */
    int     uid;            /* Real user ID */
    int     gid;            /* Real group ID */
    int     euid;           /* Effective user ID */
    int     egid;           /* Effective group ID */
    int     state;          /* 0=READY 1=RUNNING 2=BLOCKED 3=STOPPED 4=ZOMBIE */
    int     nice;           /* Nice value */
    int     nr_threads;     /* Number of threads in thread group */
    int     on_cpu;         /* CPU number */
    int     exit_code;      /* Exit status (for zombies) */
    int     tty_nr;         /* Controlling terminal (0 = none) */
    int     is_kernel;      /* 1 if kernel task, 0 if user */
    uint64_t start_tick;    /* Tick when process started */
    uint64_t utime_ticks;   /* User-mode ticks */
    uint64_t stime_ticks;   /* Kernel-mode ticks */
    uint64_t vsz;           /* Virtual memory size (bytes) */
    uint64_t rss;           /* Resident set size (pages) */
    /* Kernel address of the call that put this task to sleep -- the WCHAN.
     * 0 when the task is not blocked.  Symbolise it with:
     *   rm build/kernel.elf && make NO_STRIP=1
     *   addr2line -f -e build/kernel.elf <wchan>
     * A process that is hung tells you nothing without this. */
    uint64_t wchan;
    char    comm[256];      /* Process name (basename of executable) */
    char    cmdline[1024];  /* Full command line (argv joined by spaces) */
    char    environ[2048];  /* Environment (envp joined by spaces) */
    char    cwd[256];       /* Current working directory */
} procinfo_t;

/* Retrieve information about all processes.
 * buf:       array of procinfo_t to fill
 * max_count: number of entries the array can hold
 * Returns:   number of entries filled, or -1 on error (errno set) */
int getprocinfo(procinfo_t* buf, int max_count);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PROCINFO_H */
