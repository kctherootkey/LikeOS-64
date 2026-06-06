#ifndef _SYSLOG_H
#define _SYSLOG_H

#include <stdarg.h>

/* Severity levels */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

/* Facility codes */
#define LOG_KERN    (0<<3)
#define LOG_USER    (1<<3)
#define LOG_MAIL    (2<<3)
#define LOG_DAEMON  (3<<3)
#define LOG_AUTH    (4<<3)
#define LOG_SYSLOG  (5<<3)
#define LOG_LPR     (6<<3)
#define LOG_NEWS    (7<<3)
#define LOG_LOCAL0  (16<<3)
#define LOG_LOCAL1  (17<<3)
#define LOG_LOCAL2  (18<<3)
#define LOG_LOCAL3  (19<<3)
#define LOG_LOCAL4  (20<<3)
#define LOG_LOCAL5  (21<<3)
#define LOG_LOCAL6  (22<<3)
#define LOG_LOCAL7  (23<<3)

/* Options for openlog */
#define LOG_PID     0x01
#define LOG_CONS    0x02
#define LOG_ODELAY  0x04
#define LOG_NDELAY  0x08
#define LOG_NOWAIT  0x10
#define LOG_PERROR  0x20

#define LOG_MAKEPRI(fac, pri) ((fac) | (pri))
#define LOG_PRIMASK 0x07
#define LOG_PRI(p)  ((p) & LOG_PRIMASK)
#define LOG_FAC(p)  (((p) & ~LOG_PRIMASK) >> 3)
#define LOG_MASK(pri) (1 << (pri))
#define LOG_UPTO(pri) ((1 << ((pri)+1)) - 1)

#include <stdio.h>

/* On LikeOS syslog writes to stderr */
static inline void openlog(const char *ident, int option, int facility)
{
    (void)ident; (void)option; (void)facility;
}
static inline void closelog(void) {}
static inline int setlogmask(int mask) { (void)mask; return 0xff; }

static inline void syslog(int priority, const char *fmt, ...)
{
    (void)priority;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static inline void vsyslog(int priority, const char *fmt, va_list ap)
{
    (void)priority;
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

#endif /* _SYSLOG_H */
