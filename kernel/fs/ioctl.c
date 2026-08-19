// LikeOS-64 -- the ioctl dispatcher.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/pipe.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>


int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t argp)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	vfs_file_t *file = NULL;
	if (fd < TASK_MAX_FDS) {
		file = task_fds(cur)[fd];
	}

	// Socket fd markers - route to network ioctl handler
	if (file && IS_SOCKET_FD(file)) {
		int idx = SOCKET_FD_IDX(file);
		size_t arg_len = 0;
		switch (req) {
		case SIOCGIFCONF:
			arg_len = sizeof(struct ifconf);
			break;
		case SIOCGIFFLAGS:
		case SIOCSIFFLAGS:
		case SIOCGIFADDR:
		case SIOCSIFADDR:
		case SIOCGIFNETMASK:
		case SIOCSIFNETMASK:
		case SIOCGIFBRDADDR:
		case SIOCSIFBRDADDR:
		case SIOCGIFMTU:
		case SIOCSIFMTU:
		case SIOCGIFHWADDR:
		case SIOCGIFINDEX:
		case SIOCGIFNAME:
			arg_len = sizeof(struct ifreq);
			break;
		case 0x5421: /* FIONBIO */
		case 0x541B: /* FIONREAD */
			arg_len = sizeof(int);
			break;
		default:
			arg_len = 0;
			break;
		}
		if (arg_len > 0) {
			if (!argp)
				return -EFAULT;
			if (!validate_user_ptr(argp, arg_len))
				return -EFAULT;
		}
		/* Mutating interface configuration (address, netmask, flags, MTU)
		 * is a privileged, system-wide change; the query ioctls are not. */
		switch (req) {
		case SIOCSIFFLAGS:
		case SIOCSIFADDR:
		case SIOCSIFNETMASK:
		case SIOCSIFBRDADDR:
		case SIOCSIFMTU:
			if (!capable())
				return -EPERM;
			break;
		default:
			break;
		}
		smap_disable();
		int64_t ret =
			sock_ioctl_net(idx, (unsigned long)req, (void *)argp);
		smap_enable();
		return ret;
	}

	if (task_fd_is_console(cur, fd)) {
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
		return tty_ioctl(tty, (unsigned long)req, (void *)argp, cur);
	}
	/* Duplicated stdio markers: dup()/SCM_RIGHTS store the marker value
     * (1, 2 or 3 = oldfd+1) into the fd_table.  These should still be
     * routed to the controlling TTY, otherwise isatty()/tcgetattr() on
     * a dup'd stdin/stdout/stderr would fail. */
	if (file) {
		uintptr_t mk = (uintptr_t)file;
		if (mk >= 1 && mk <= 3) {
			tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
			return tty_ioctl(tty, (unsigned long)req, (void *)argp,
					 cur);
		}
	}
	if (!file) {
		return -EBADF;
	}

	/* AF_UNIX and epoll descriptors are fd-table MARKERS (small tagged
	 * integers), not vfs_file pointers, and a pipe end is a pipe object
	 * rather than a devfs file.  All three have to be classified HERE:
	 * the devfs fallthrough below dereferences whatever it is handed, so
	 * an ioctl() on a unix socket faulted the kernel on the marker value
	 * itself (scp hit this — ssh probes its socketpair with tcgetattr).
	 * The numeric marker tests come first because they dereference
	 * nothing. */
	if (unix_sock_is(file)) {
		unix_socket_t *us = (unix_socket_t *)file;
		if (!us)
			return -EBADF;
		if (req == 0x5421 /* FIONBIO */) {
			if (!argp || !validate_user_ptr(argp, sizeof(int)))
				return -EFAULT;
			int on = 0;
			if (copy_from_user(&on, (void *)argp, sizeof(on)) != 0)
				return -EFAULT;
			us->nonblock = on ? 1 : 0;
			return 0;
		}
		/* Not a terminal: what tcgetattr()/isatty() expect to see. */
		return -ENOTTY;
	}
	if (IS_EPOLL_FD(file))
		return -ENOTTY;
	if (pipe_is_end(file))
		return -ENOTTY;

	return devfs_ioctl(file, (unsigned long)req, (void *)argp, cur);
}

