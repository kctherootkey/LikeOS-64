/* <sys/signalfd.h> -- receive signals by reading a descriptor.
 *
 * Block the signals in `mask' with sigprocmask() first; the descriptor then
 * delivers them as signalfd_siginfo records, one per read, and poll()
 * reports it readable while any is pending. */
#ifndef _SYS_SIGNALFD_H
#define _SYS_SIGNALFD_H

#include <stdint.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFD_CLOEXEC  02000000
#define SFD_NONBLOCK 04000

struct signalfd_siginfo {
	uint32_t ssi_signo;
	int32_t ssi_errno;
	int32_t ssi_code;
	uint32_t ssi_pid;
	uint32_t ssi_uid;
	int32_t ssi_fd;
	uint32_t ssi_tid;
	uint32_t ssi_band;
	uint32_t ssi_overrun;
	uint32_t ssi_trapno;
	int32_t ssi_status;
	int32_t ssi_int;
	uint64_t ssi_ptr;
	uint64_t ssi_utime;
	uint64_t ssi_stime;
	uint64_t ssi_addr;
	uint16_t ssi_addr_lsb;
	uint8_t __pad[46];
};

/* fd == -1 creates a new descriptor; an existing signalfd's mask is
 * replaced when its descriptor is passed. */
int signalfd(int fd, const sigset_t *mask, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SIGNALFD_H */
