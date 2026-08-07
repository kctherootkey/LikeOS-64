#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#define FD_SETSIZE 1024

typedef unsigned long fd_mask;
#define NFDBITS (8 * sizeof(fd_mask))

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

/* Reaching into an fd_set without going through FD_SET and friends.
 *
 * Portable code has no business doing this, but X11 does: its <X11/Xpoll.h>
 * copies and ORs whole descriptor sets a word at a time, which is much faster
 * than looping over FD_ISSET/FD_SET, and it needs the member's name to do it.
 * What it actually reaches for is __fds_bits, the name one particular libc
 * uses internally -- but only after giving the system a chance to say
 * otherwise:
 *
 *     #ifndef __FDS_BITS
 *     # define __FDS_BITS(p)  ((p)->__X_FDS_BITS)
 *     #endif
 *
 * Defining it here is taking that offer.  The member here is called fds_bits,
 * which is the name X/Open gives it; without this every X client fails to
 * compile on "fd_set has no member named __fds_bits".
 */
#define __FDS_BITS(p) ((p)->fds_bits)

#define FD_ZERO(s)   do { for (unsigned _i = 0; _i < sizeof((s)->fds_bits)/sizeof((s)->fds_bits[0]); _i++) (s)->fds_bits[_i] = 0; } while(0)
#define FD_SET(fd,s) ((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] |= (1UL << ((fd) % (8*sizeof(unsigned long)))))
#define FD_CLR(fd,s) ((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] &= ~(1UL << ((fd) % (8*sizeof(unsigned long)))))
#define FD_ISSET(fd,s) (((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] & (1UL << ((fd) % (8*sizeof(unsigned long))))) != 0)

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const void *sigmask);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SELECT_H */
