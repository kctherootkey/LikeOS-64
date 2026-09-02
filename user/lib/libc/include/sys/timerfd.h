/* <sys/timerfd.h> -- timers whose expirations are read from a descriptor.
 *
 * read() returns the number of expirations since the last read (as a
 * uint64_t) and blocks until there is one; poll() reports readable when
 * there is.  Deadlines are met with the kernel's high-resolution timer. */
#ifndef _SYS_TIMERFD_H
#define _SYS_TIMERFD_H

#include <time.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFD_CLOEXEC  02000000
#define TFD_NONBLOCK 04000
#define TFD_TIMER_ABSTIME       1
#define TFD_TIMER_CANCEL_ON_SET 2

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
		    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIMERFD_H */
