#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>

#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

int gettimeofday(struct timeval* tv, void* tz);

#endif
