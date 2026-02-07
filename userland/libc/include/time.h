#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

typedef long time_t;

#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
#endif

// Clock IDs
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

typedef int clockid_t;

time_t time(time_t* tloc);
int clock_gettime(clockid_t clk_id, struct timespec* tp);
int clock_getres(clockid_t clk_id, struct timespec* res);

#endif
