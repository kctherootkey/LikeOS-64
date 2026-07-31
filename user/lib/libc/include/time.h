#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

/* time_t is now defined in sys/types.h */

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

/* The unit clock() counts in.  POSIX fixes this at exactly one million
 * regardless of how fast the system timer actually ticks, so it is a property
 * of the interface and not of the hardware -- do not derive it from
 * sysconf(_SC_CLK_TCK), which is the (different) unit times() counts in. */
#define CLOCKS_PER_SEC ((clock_t)1000000)

/* Processor time consumed by this process, in CLOCKS_PER_SEC units, or
 * (clock_t)-1 if it cannot be determined.  Note this is CPU time, not elapsed
 * time: a process that sleeps for an hour has consumed almost none of it. */
clock_t clock(void);

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

/* difftime(): seconds between two times, as a double (ISO C). */
double difftime(time_t time1, time_t time0);

/* strptime(): the inverse of strftime().  Returns the first unconsumed
 * character, or NULL if the input does not match the format.  Fields the
 * format does not mention are left untouched, so several calls can build up
 * one struct tm. */
char *strptime(const char *s, const char *format, struct tm *tm);

int nanosleep(const struct timespec *req, struct timespec *rem);

char *ctime(const time_t *timep);
char *ctime_r(const time_t *timep, char *buf);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
void tzset(void);
extern char *tzname[2];
extern long timezone;
extern int daylight;

#endif
