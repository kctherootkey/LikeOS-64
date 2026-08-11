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


/* Address-space report for one process (SYS_GETPROCMAPS).
 *
 * ps reports a single VSZ total, which cannot distinguish a region table
 * filling up from a few regions growing -- different faults with different
 * fixes.  These carry the region table itself. */
typedef struct procmap {
    uint64_t start;
    uint64_t length;
    uint64_t prot;
    uint64_t flags;
    uint64_t offset;
    int      file_backed;
    int      lazy;
    int      device;
    int      pad;
} procmap_t;

typedef struct procmapinfo {
    int      pid;
    int      tgid;
    uint64_t brk_start;
    uint64_t brk;
    uint64_t mmap_base;
    uint64_t total_bytes;   /* sum of in-use region lengths */
    uint32_t n_regions;     /* in-use records */
    uint32_t capacity;      /* records the table can hold */
} procmapinfo_t;

/* Report the address space of `pid`.  Fills *info always; fills up to `max`
 * region records if buf is non-NULL.  Returns the number of records written,
 * or -1 (errno set).  info->n_regions is the true count, which may exceed
 * what was written. */
int getprocmaps(int pid, procmapinfo_t *info, procmap_t *buf, int max);

/* Retrieve information about all processes.
 * buf:       array of procinfo_t to fill
 * max_count: number of entries the array can hold
 * Returns:   number of entries filled, or -1 on error (errno set) */
int getprocinfo(procinfo_t* buf, int max_count);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PROCINFO_H */
