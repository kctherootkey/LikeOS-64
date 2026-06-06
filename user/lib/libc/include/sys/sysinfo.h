#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H

#include <stdint.h>

struct sysinfo {
    long     uptime;          /* Seconds since boot */
    unsigned long loads[3];   /* 1, 5, 15 minute load averages (fixed-point << 16) */
    unsigned long totalram;   /* Total usable main memory in bytes */
    unsigned long freeram;    /* Available memory in bytes */
    unsigned long sharedram;  /* Shared memory */
    unsigned long bufferram;  /* Memory used by buffers */
    unsigned long totalswap;  /* Total swap space (0) */
    unsigned long freeswap;   /* Free swap space (0) */
    unsigned short procs;     /* Number of current processes */
    unsigned long totalhigh;  /* Total high memory (0) */
    unsigned long freehigh;   /* Free high memory (0) */
    unsigned int  mem_unit;   /* Memory unit size in bytes */
    unsigned long cached;     /* Cache memory */
    unsigned long available;  /* Estimated available memory */
};

int sysinfo(struct sysinfo *info);

#endif /* _SYS_SYSINFO_H */
