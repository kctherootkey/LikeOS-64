#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#define FD_SETSIZE 1024

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(s)   do { for (unsigned _i = 0; _i < sizeof((s)->fds_bits)/sizeof((s)->fds_bits[0]); _i++) (s)->fds_bits[_i] = 0; } while(0)
#define FD_SET(fd,s) ((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] |= (1UL << ((fd) % (8*sizeof(unsigned long)))))
#define FD_CLR(fd,s) ((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] &= ~(1UL << ((fd) % (8*sizeof(unsigned long)))))
#define FD_ISSET(fd,s) (((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] & (1UL << ((fd) % (8*sizeof(unsigned long))))) != 0)

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const void *sigmask);

#endif /* _SYS_SELECT_H */
