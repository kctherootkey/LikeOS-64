// LikeOS-64 -- user-visible constants for the anonymous descriptor kinds.
// Mirrored by the libc's <sys/eventfd.h>, <sys/timerfd.h>, <sys/signalfd.h>
// and <sys/mman.h>; the values are the conventional ones.
#ifndef KERNEL_UAPI_ANONFD_H
#define KERNEL_UAPI_ANONFD_H

#include <kernel/uapi/types.h>

/* eventfd2 flags */
#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC 02000000
#define EFD_NONBLOCK 04000

/* timerfd flags */
#define TFD_CLOEXEC 02000000
#define TFD_NONBLOCK 04000
#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2

/* signalfd4 flags */
#define SFD_CLOEXEC 02000000
#define SFD_NONBLOCK 04000

/* struct signalfd_siginfo (what read() on a signalfd returns) is defined
 * in <kernel/ke/signal.h>. */

/* memfd_create flags */
#define MFD_CLOEXEC 1u
#define MFD_ALLOW_SEALING 2u
#define MFD_HUGETLB 4u

/* fcntl seals */
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#define F_SEAL_FUTURE_WRITE 0x0010

#endif
