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
int settimeofday(const struct timeval* tv, const void* tz);

/* Convenience timeval arithmetic - BSD legacy macros. */
#define timerisset(tvp)         ((tvp)->tv_sec || (tvp)->tv_usec)
#define timerclear(tvp)         ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define timercmp(a, b, CMP)                                       \
    (((a)->tv_sec == (b)->tv_sec) ?                               \
        ((a)->tv_usec CMP (b)->tv_usec) :                         \
        ((a)->tv_sec  CMP (b)->tv_sec))
#define timeradd(a, b, res)                                       \
    do {                                                          \
        (res)->tv_sec  = (a)->tv_sec  + (b)->tv_sec;              \
        (res)->tv_usec = (a)->tv_usec + (b)->tv_usec;             \
        if ((res)->tv_usec >= 1000000) {                          \
            (res)->tv_sec++;                                      \
            (res)->tv_usec -= 1000000;                            \
        }                                                         \
    } while (0)
#define timersub(a, b, res)                                       \
    do {                                                          \
        (res)->tv_sec  = (a)->tv_sec  - (b)->tv_sec;              \
        (res)->tv_usec = (a)->tv_usec - (b)->tv_usec;             \
        if ((res)->tv_usec < 0) {                                 \
            (res)->tv_sec--;                                      \
            (res)->tv_usec += 1000000;                            \
        }                                                         \
    } while (0)

#endif
