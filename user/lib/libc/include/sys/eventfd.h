/* <sys/eventfd.h> -- a 64-bit counter behind a descriptor.
 *
 * write() adds to it, read() takes it (all of it, or 1 in semaphore mode)
 * and blocks while it is zero, poll() reports readable while it is not.
 * The wake-up primitive event loops use to interrupt a poll() from another
 * thread. */
#ifndef _SYS_EVENTFD_H
#define _SYS_EVENTFD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   02000000
#define EFD_NONBLOCK  04000

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_EVENTFD_H */
