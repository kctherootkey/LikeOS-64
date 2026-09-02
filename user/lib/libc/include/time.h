#ifndef _TIME_H
#define _TIME_H

#ifdef __cplusplus
extern "C" {
#endif

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
/* Further names for the monotonic and real-time clocks.  Nothing here slews
 * the monotonic clock or suspends the machine, so RAW and BOOTTIME read the
 * same as MONOTONIC, and the COARSE variants are not any coarser. */
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
/* clock_nanosleep() flag: the request is an absolute time on the clock. */
#define TIMER_ABSTIME            1
/* Per-thread CPU clocks for OTHER threads: pthread_getcpuclockid() encodes
 * the target's kernel tid into the clockid.  clock_gettime() on such an id
 * reports that thread's consumed CPU time (user + system ticks), or EINVAL
 * once the thread is gone. */
#define CLOCK_TID_CPUTIME_BASE 0x40000000

/* clockid_t and timer_t.  POSIX puts both in <time.h>; <signal.h> defines
 * them as well, because the per-process timer calls take a struct sigevent
 * and are declared beside it.  The guards keep the two headers from
 * colliding when a translation unit includes both under a standard older
 * than C11, which is where repeating a typedef is an error rather than
 * merely redundant. */
#ifndef __clockid_t_defined
#define __clockid_t_defined
typedef int clockid_t;
#endif
#ifndef __timer_t_defined
#define __timer_t_defined
typedef int timer_t;
#endif

/* The interval-timer value POSIX puts in <time.h> beside timer_t: the
 * period and the initial expiry of a per-process timer.  <signal.h>
 * defines it too, under the same guard, because the calls that take it are
 * declared there next to struct sigevent. */
#ifndef _STRUCT_ITIMERSPEC
#define _STRUCT_ITIMERSPEC
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};
#endif

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
/* mktime with the fields taken as UTC; here the same operation (no timezones). */
time_t timegm(struct tm* tm);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

/* difftime(): seconds between two times, as a double (ISO C). */
double difftime(time_t time1, time_t time0);

/* strptime(): the inverse of strftime().  Returns the first unconsumed
 * character, or NULL if the input does not match the format.  Fields the
 * format does not mention are left untouched, so several calls can build up
 * one struct tm. */
char *strptime(const char *s, const char *format, struct tm *tm);

int nanosleep(const struct timespec *req, struct timespec *rem);
/* Sleep on a named clock, relative or (TIMER_ABSTIME) to an absolute
 * deadline.  Returns 0 or an errno value (not -1/errno): EINTR with *rem
 * filled in for a relative sleep cut short by a signal. */
int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *req,
		    struct timespec *rem);

char *ctime(const time_t *timep);
char *ctime_r(const time_t *timep, char *buf);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
void tzset(void);
extern char *tzname[2];
extern long timezone;
extern int daylight;

#ifdef __cplusplus
}
#endif

#endif
