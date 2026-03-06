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

struct tm {
    int tm_sec;     /* seconds (0-60) */
    int tm_min;     /* minutes (0-59) */
    int tm_hour;    /* hours (0-23) */
    int tm_mday;    /* day of the month (1-31) */
    int tm_mon;     /* month (0-11) */
    int tm_year;    /* year - 1900 */
    int tm_wday;    /* day of the week (0-6, Sunday = 0) */
    int tm_yday;    /* day in the year (0-365) */
    int tm_isdst;   /* daylight saving time */
};

struct tm *gmtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);
time_t mktime(struct tm *tm);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

#endif
