#ifndef _SYS_PROCINFO_H
#define _SYS_PROCINFO_H

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

#endif /* _SYS_PROCINFO_H */
