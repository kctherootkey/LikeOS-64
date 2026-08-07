#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct tms {
    clock_t tms_utime;   /* user CPU time */
    clock_t tms_stime;   /* system CPU time */
    clock_t tms_cutime;  /* user CPU time of terminated children */
    clock_t tms_cstime;  /* system CPU time of terminated children */
};

clock_t times(struct tms *buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIMES_H */
