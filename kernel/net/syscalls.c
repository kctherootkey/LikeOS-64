// LikeOS-64 -- the socket-layer syscalls.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>

// Helper: extract socket index from a process fd (via fd_table marker)
/* Hand an accepted peer address back to userspace.
 *
 * addrlen is IN/OUT and both halves matter: on the way in it states how big
 * the caller's buffer is, and NOTHING may be written past it; on the way out it
 * reports the address's true size, which may be larger -- that is how a caller
 * learns the answer was truncated.
 *
 * The accept arms used to ignore the incoming value entirely and copy a whole
 * sizeof(struct sockaddr_un) -- 110 bytes -- into whatever the caller passed.
 * `struct sockaddr' is 16, and passing one is completely ordinary:
 * menu-cached does exactly that, so every client connection wrote 94 bytes
 * past a stack buffer.  It flattened the saved registers and return addresses
 * below it and the function returned into the wreckage, which showed up as
 * SIGSEGV at RIP 0 with no call frame to walk.  validate_user_ptr() cannot
 * catch it: the stack beyond the buffer is perfectly writable memory, it just
 * belongs to somebody else. */
static void sock_put_peer_addr(uint64_t uaddr, uint64_t ulenp,
			       const void *kaddr, socklen_t kaddrlen)
{
	socklen_t ulen = 0;

	if (!uaddr || !ulenp)
		return;
	if (!validate_user_ptr(ulenp, sizeof(socklen_t)))
		return;
	if (copy_from_user(&ulen, (const void *)ulenp, sizeof(socklen_t)) < 0)
		return;

	if (ulen > 0) {
		socklen_t n = (kaddrlen < ulen) ? kaddrlen : ulen;

		if (n && validate_user_ptr(uaddr, n))
			copy_to_user((void *)uaddr, kaddr, n);
	}
	/* The real length, not the truncated one. */
	copy_to_user((void *)ulenp, &kaddrlen, sizeof(socklen_t));
}

static int sock_idx_from_fd(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS)
		return -EBADF;
	void *entry = task_fds(cur)[fd];
	if (!entry)
		return -EBADF;
	if (!IS_SOCKET_FD(entry))
		return -ENOTSOCK;
	return SOCKET_FD_IDX(entry);
}

/* Resolve a descriptor to a UNIX socket, and hold the socket for the rest of
 * this syscall.
 *
 * The descriptor read and the reference are taken together under the
 * descriptor-table lock, so a sibling thread closing the same descriptor
 * cannot slip between them.  The reference is parked on the task and released
 * by syscall_handler() once the call returns -- one acquire, one release,
 * rather than a release on each of the many early returns these arms have. */
static unix_socket_t *unix_sock_from_fd(uint64_t fd)
{
	task_t *cur = sched_current();
	uint64_t lflags;
	void *entry;

	if (!cur || fd >= TASK_MAX_FDS)
		return NULL;

	fds_lock(cur, &lflags);
	entry = task_fds(cur)[fd];
	if (!entry || !unix_sock_is(entry)) {
		fds_unlock(cur, lflags);
		return NULL;
	}
	/* One resolution per syscall: every arm resolves a1 once.  A second
	 * would strand the first reference, so say so rather than leak. */
	WARN_ON_ONCE(cur->syscall_unix_ref != NULL);
	if (!cur->syscall_unix_ref)
		cur->syscall_unix_ref =
			unix_sock_lookup_hold((unix_socket_t *)entry);
	fds_unlock(cur, lflags);

	return cur->syscall_unix_ref;
}

// ---------------------------------------------------------------------------
// UNIX-domain sendmsg / recvmsg helpers.
// Both carry large stack objects (iov[256], 4 KB data buffer) so they are
// kept out of syscall_handler_inner to avoid bloating the kernel stack.
// ---------------------------------------------------------------------------

__attribute__((noinline)) static int unix_do_sendmsg(unix_socket_t *ufd,
						     struct msghdr *kmsg)
{
	unix_socket_t *us = ufd;
	if (!us)
		return -EBADF;
	/* The peer is deliberately NOT resolved here.  Reading it once and
	 * using it further down is what let an in-band descriptor be queued on
	 * a socket that had been replaced in the meantime; unix_send_fd()
	 * finds and pins it for the length of the operation instead. */
	if (!us->connected)
		return -ENOTCONN;

	/* Process control data first so the fd arrives before (or with) the
     * byte that the receiver associates it with.  The imsg framing tmux
     * uses sends one fd per message and the receiver pops the next pending
     * fd when it parses each imsg header. */
	if (kmsg->msg_control &&
	    kmsg->msg_controllen >= sizeof(struct cmsghdr)) {
		size_t clen = kmsg->msg_controllen;
		if (clen > 256)
			clen = 256;
		unsigned char cbuf[256];
		if (!validate_user_ptr((uint64_t)kmsg->msg_control, clen))
			return -EFAULT;
		copy_from_user(cbuf, kmsg->msg_control, clen);
		size_t off = 0;
		task_t *cur = sched_current();
		if (!cur)
			return -EFAULT;
		while (off + sizeof(struct cmsghdr) <= clen) {
			struct cmsghdr *cmsg = (struct cmsghdr *)(cbuf + off);
			if (cmsg->cmsg_len < sizeof(struct cmsghdr))
				break;
			if (off + cmsg->cmsg_len > clen)
				break;
			if (cmsg->cmsg_level == SOL_SOCKET &&
			    cmsg->cmsg_type == SCM_RIGHTS) {
				size_t hdr_align =
					CMSG_ALIGN(sizeof(struct cmsghdr));
				int n = (int)((cmsg->cmsg_len - hdr_align) /
					      sizeof(int));
				int *fds = (int *)(cbuf + off + hdr_align);
				for (int i = 0; i < n; i++) {
					int sfd = fds[i];
					if (sfd < 0 || sfd >= TASK_MAX_FDS)
						continue;
					/* Take the reference that keeps the
					 * entry alive until the peer receives
					 * it -- the sender may close its own
					 * descriptor first.
					 *
					 * Read and reference happen together:
					 * this open-coded the whole of
					 * fd_dup_entry, including its race
					 * against a sibling thread closing the
					 * same descriptor between the two. */
					void *entry;

					/* A standard descriptor still attached
					 * to the terminal has NO vfs_file
					 * behind it -- the console is
					 * represented by an empty slot -- so
					 * fd_dup_entry_at() finds nothing to
					 * reference and this used to `continue`,
					 * silently sending no descriptor at all
					 * while reporting success.  A terminal
					 * multiplexer hands its stdin to its
					 * server over exactly this socket, so
					 * the server was left with no terminal
					 * and answered "open terminal failed:
					 * not a terminal".
					 *
					 * dup() already encodes this case as the
					 * marker value, the pending-fd queue
					 * documents stdio markers as one of the
					 * things it carries, and
					 * fd_release_entry() knows not to
					 * release one.  The sender was the only
					 * place that did not take part. */
					if (task_fd_is_console(cur, sfd)) {
						entry = (void *)(uintptr_t)(sfd +
									    1);
					} else {
						entry = fd_dup_entry_at(cur,
									sfd);
						if (!entry)
							continue;
					}
					(void)unix_send_fd(us, entry);
				}
			}
			off += CMSG_ALIGN(cmsg->cmsg_len);
		}
	}

	if (kmsg->msg_iovlen <= 0)
		return 0;
	/* Cap the iov count so the on-stack copy of the array is bounded. */
	int kiovcnt = kmsg->msg_iovlen;
	if (kiovcnt > 256)
		kiovcnt = 256;
	struct iovec iov[256];
	if (!validate_user_ptr((uint64_t)kmsg->msg_iov,
			       sizeof(struct iovec) * (size_t)kiovcnt))
		return -EFAULT;
	copy_from_user(iov, kmsg->msg_iov,
		       sizeof(struct iovec) * (size_t)kiovcnt);

	/* Send each iovec in turn, stopping at the first short write, exactly as
	 * sys_writev does.  A stream socket has no message boundaries, so this
	 * is indistinguishable from one big write -- and unix_send already
	 * bounces user memory through its own small on-stack buffer, so no
	 * flattening buffer is needed here at all.
	 *
	 * It used to flatten into a `static uint8_t sbuf[4096]`: ONE buffer for
	 * the whole system, with no lock.  Two processes sending on unix sockets
	 * at the same time overwrote each other's bytes, so each peer received a
	 * stream with someone else's data spliced into it.  Every byte an X
	 * client writes goes through here, which is how a window resize -- the
	 * window manager and the terminal both bursting at once -- ended in
	 * "[xcb] Unknown sequence number while processing queue".  Same bug
	 * class as the ext4 xattr static scratch list. */
	int64_t sent_total = 0;
	for (int i = 0; i < kiovcnt; i++) {
		size_t want = iov[i].iov_len;
		if (want == 0)
			continue;
		/* Bounded per call so the byte count cannot overflow the int this
		 * returns.  A caller that asked for more sees a short write and
		 * comes back for the rest, which is what it must already do for a
		 * full peer ring. */
		if (want > 65536)
			want = 65536;
		if (!validate_user_ptr((uint64_t)iov[i].iov_base, want))
			return sent_total ? (int)sent_total : -EFAULT;
		int r = unix_send(ufd, iov[i].iov_base, want, 0);
		if (r < 0)
			return sent_total ? (int)sent_total : r;
		sent_total += r;
		if ((size_t)r < want)
			break; /* peer's ring is full; report what went */
	}
	return (int)sent_total;
}

__attribute__((noinline)) static int unix_do_recvmsg(unix_socket_t *ufd,
						     struct msghdr *kmsg)
{
	unix_socket_t *us = ufd;
	if (!us)
		return -EBADF;

	if (kmsg->msg_iovlen <= 0)
		return 0;
	int riovcnt = kmsg->msg_iovlen;
	if (riovcnt > 256)
		riovcnt = 256;
	struct iovec iov[256];
	if (!validate_user_ptr((uint64_t)kmsg->msg_iov,
			       sizeof(struct iovec) * (size_t)riovcnt))
		return -EFAULT;
	copy_from_user(iov, kmsg->msg_iov,
		       sizeof(struct iovec) * (size_t)riovcnt);
	size_t total = 0;
	for (int i = 0; i < riovcnt; i++)
		total += iov[i].iov_len;
	if (total == 0)
		return 0;

	/* Stream-mode SCM_RIGHTS framing: if a pending fd is queued at byte
     * offset N (in the receiver's bytes_read coordinate system), clamp
     * this recvmsg to either:
     *   - bytes_read < N: deliver only N - bytes_read bytes (no fd this
     *     round; fd waits for the next call);
     *   - bytes_read == N: deliver the fd and at most up to the next
     *     pending fd's offset bytes.
     * This keeps fds aligned with the imsg frame the sender attached them to. */
	int deliver_fd_now = 0;
	uint64_t fd_off = 0;
	/* Where this call starts reading.  Captured before anything is
	 * received, because the pending-fd question has to be asked again
	 * afterwards against this same position -- see below. */
	uint64_t start_br = us->bytes_read;
	int has_fd = (unix_peek_fd_offset(us, &fd_off) == 0);
	if (has_fd) {
		uint64_t br = start_br;
		if (br < fd_off) {
			size_t cap = (size_t)(fd_off - br);
			if (total > cap)
				total = cap;
		} else {
			deliver_fd_now = 1;
			/* Clamp to the offset of the next pending fd, if any, so we
             * don't accidentally pull data past it.  Look one slot ahead. */
			uint64_t irq_flags;
			spin_lock_irqsave(&us->lock, &irq_flags);
			int nxt = (us->pending_fd_head + 1) % 16;
			if (nxt != us->pending_fd_tail) {
				uint64_t nxt_off = us->pending_fd_off[nxt];
				if (nxt_off > br) {
					size_t cap = (size_t)(nxt_off - br);
					if (total > cap)
						total = cap;
				}
			}
			spin_unlock_irqrestore(&us->lock, irq_flags);
		}
	}

	/* Receive straight into the caller's iovecs.  unix_recv copies to user
	 * memory itself (with no lock held, so a demand fault may sleep), so
	 * there is nothing to stage through.
	 *
	 * This used to drain into a `static uint8_t rbuf[4096]`: ONE buffer for
	 * the whole system, with no lock.  Two processes reading unix sockets at
	 * the same time overwrote each other's bytes.  libxcb reads EVERY byte
	 * of the X protocol through recvmsg(), so a client got a stream with
	 * another client's data spliced into it and aborted with "[xcb] Unknown
	 * sequence number while processing queue" -- which needed simultaneous
	 * traffic to show up, hence a window resize triggering it.
	 *
	 * Only the first read may block.  Once any byte has been handed over the
	 * call must return what it has, so later iovecs use MSG_DONTWAIT and a
	 * drained ring ends the loop. */
	int got = 0;
	size_t off = 0;
	for (int i = 0; i < riovcnt && off < total; i++) {
		size_t want = iov[i].iov_len;
		if (want == 0)
			continue;
		if (off + want > total)
			want = total - off;
		if (!validate_user_ptr((uint64_t)iov[i].iov_base, want))
			return off ? (int)off : -EFAULT;
		int n = unix_recv(ufd, iov[i].iov_base, want,
				  off ? MSG_DONTWAIT : 0);
		if (n < 0) {
			if (off)
				break; /* keep what was already delivered */
			return n;
		}
		if (n == 0)
			break; /* peer closed, or nothing more queued */
		off += (size_t)n;
		if ((size_t)n < want)
			break; /* ring drained */
	}
	got = (int)off;

	/* Ask again, now that the data is in hand.
	 *
	 * The question "is a descriptor queued for this point in the stream"
	 * was answered above, BEFORE the receive -- and the receive blocks.  A
	 * reader that arrives at an empty socket and waits there is answered
	 * "no descriptor pending" while the sender has not sent anything yet;
	 * the sender then queues the descriptor and writes the byte that goes
	 * with it, the reader wakes and returns that byte, and the answer from
	 * before it slept is the one that decides the call.  The byte arrives
	 * with no control message attached.
	 *
	 * That is a race on which side gets there first, so it fails only
	 * sometimes.  It is what a login over ssh runs into: the monitor
	 * process passes the terminal it just allocated across this socket,
	 * one descriptor with one byte of payload, and the session's half of
	 * the handshake reaches the socket first about as often as not.  It
	 * reads the byte, finds no header on it, gives up -- and the monitor's
	 * own send then fails with a broken pipe because the reader is already
	 * gone.  Which of the two errors gets reported depends on the timing;
	 * the cause is the same either way.
	 *
	 * Re-asking against start_br -- where this call began reading, not
	 * where it ended -- is what makes the answer belong to the bytes being
	 * returned.  A descriptor queued at or before that point accompanies
	 * data this call has already handed over, so it is due now; one queued
	 * after it belongs to a later frame and must wait, exactly as the
	 * clamp above arranges when the answer was known in time. */
	if (!deliver_fd_now && got > 0 &&
	    unix_peek_fd_offset(us, &fd_off) == 0 && fd_off <= start_br)
		deliver_fd_now = 1;

	/* Deliver one pending fd via SCM_RIGHTS, but only at the correct
     * byte boundary. */
	kmsg->msg_flags = 0;
	if (deliver_fd_now && kmsg->msg_control &&
	    kmsg->msg_controllen >= CMSG_SPACE(sizeof(int))) {
		void *entry = NULL;
		if (unix_pop_fd(us, &entry) == 0 && entry) {
			task_t *cur = sched_current();
			/* fd_install_from, not a bare scan: it claims the slot
			 * under the descriptor-table lock (so two threads of one
			 * process cannot be handed the same number) and clears
			 * the slot's flag byte -- a recycled slot that kept a
			 * stale FD_CLOEXEC made the received descriptor vanish
			 * at the next exec. */
			int newfd = cur ? fd_install_from(cur, entry, 0) : -1;
			if (newfd < 0) {
				/* No descriptor slot: give the reference back.
				 *
				 * Exactly the mirror of the fd_dup_entry_at()
				 * that took it, which is the point of routing
				 * it through the same place.  Hand-written, it
				 * unwound only some of what had been taken --
				 * it knew a socket had a descriptor count but
				 * not that the socket also had a lifetime
				 * reference, and it had no case at all for a
				 * regular file, so those were simply kept
				 * forever. */
				fd_release_entry((vfs_file_t *)entry);
				kmsg->msg_flags |= MSG_CTRUNC;
				kmsg->msg_controllen = 0;
			} else {
				unsigned char cbuf[CMSG_SPACE(sizeof(int))];
				struct cmsghdr *c = (struct cmsghdr *)cbuf;
				c->cmsg_len = CMSG_LEN(sizeof(int));
				c->cmsg_level = SOL_SOCKET;
				c->cmsg_type = SCM_RIGHTS;
				*(int *)CMSG_DATA(c) = newfd;
				if (!validate_user_ptr(
					    (uint64_t)kmsg->msg_control,
					    CMSG_SPACE(sizeof(int))))
					return -EFAULT;
				copy_to_user(kmsg->msg_control, cbuf,
					     CMSG_SPACE(sizeof(int)));
				kmsg->msg_controllen = CMSG_SPACE(sizeof(int));
			}
		} else {
			kmsg->msg_controllen = 0;
		}
	} else {
		kmsg->msg_controllen = 0;
	}
	return got;
}

int64_t sys_socket(uint64_t a1, uint64_t a2, uint64_t a3)
{
	/* Both descriptor flags are honoured here.
	 *
	 * SOCK_CLOEXEC was masked off the type and then forgotten, so
	 * a socket asked to close on exec did not -- it was inherited
	 * by every program the process went on to run.  socketpair()
	 * and accept4() both honour it; only this one did not.
	 *
	 * The slot is claimed from 3 upward, as it always has been.
	 * fd_install() would start at 0 and hand out a freed stdio
	 * descriptor, which is correct by the letter of the standard
	 * and a behaviour change this call has never had -- not
	 * something to introduce in passing while fixing a flag. */
	int real_type = (int)a2 & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	task_t *cur = sched_current();
	int newfd;

	if (!cur)
		return -EFAULT;

	if ((int)a1 == AF_UNIX) {
		unix_socket_t *ufd = NULL;
		int rc = unix_create(real_type, &ufd);

		if (rc < 0)
			return rc;
		if ((int)a2 & SOCK_NONBLOCK) {
			ufd->nonblock = 1;
		}
		newfd = fd_install_from(cur, (vfs_file_t *)ufd, 3);
		if (newfd < 0) {
			unix_close(ufd);
			return newfd;
		}
	} else {
		int sock_idx = sock_create((int)a1, real_type, (int)a3);

		if (sock_idx < 0)
			return sock_idx;
		if ((int)a2 & SOCK_NONBLOCK) {
			net_socket_t *_s = sock_get(sock_idx);

			if (_s)
				_s->nonblock = 1;
		}
		newfd = fd_install_from(cur, MAKE_SOCKET_FD(sock_idx),
					3);
		if (newfd < 0) {
			sock_close(sock_idx);
			return newfd;
		}
	}
	if ((int)a2 & SOCK_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)newfd, FD_CLOEXEC);
	return newfd;
}

int64_t sys_bind(uint64_t a1, uint64_t a2, uint64_t a3)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		/* Validate and copy only the DECLARED length, into a
		 * zeroed structure.
		 *
		 * Requiring the full sizeof(struct sockaddr_un) to be
		 * readable rejects a caller that allocated exactly
		 * SUN_LEN bytes -- which is legal, and which the
		 * address is normally sized by.  Zeroing first means
		 * the untouched tail is deterministic rather than
		 * stack garbage. */
		socklen_t alen = (socklen_t)a3;

		if (alen < sizeof(sa_family_t))
			return -EINVAL;
		if (alen > sizeof(struct sockaddr_un))
			alen = sizeof(struct sockaddr_un);
		if (!validate_user_ptr(a2, alen))
			return -EFAULT;
		struct sockaddr_un kaddr;
		mm_memset(&kaddr, 0, sizeof(kaddr));
		if (copy_from_user(&kaddr, (const void *)a2, alen) != 0)
			return -EFAULT;
		return unix_bind(ufd, &kaddr, alen);
	}
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	struct sockaddr_in kaddr;
	if (!validate_user_ptr(a2, sizeof(struct sockaddr_in)))
		return -EFAULT;
	copy_from_user(&kaddr, (const void *)a2,
		       sizeof(struct sockaddr_in));
	return sock_bind(idx, &kaddr);
}

int64_t sys_listen(uint64_t a1, uint64_t a2)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd)
		return unix_listen(ufd, (int)a2);
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	return sock_listen(idx, (int)a2);
}

int64_t sys_accept(uint64_t a1, uint64_t a2, uint64_t a3)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		struct sockaddr_un kaddr;
		socklen_t kaddrlen = sizeof(struct sockaddr_un);
		unix_socket_t *new_ufd = NULL;
		int arc = unix_accept(ufd, &kaddr, &kaddrlen, &new_ufd);

		if (arc < 0)
			return arc;
		task_t *cur = sched_current();
		if (!cur) {
			unix_close(new_ufd);
			return -EFAULT;
		}
		/* fd_install_from, not a hand-rolled scan: claiming
		 * the slot and storing the socket must be one locked
		 * step, or two threads accepting on the same listener
		 * at the same moment are handed the same number and
		 * one of the two connections is simply lost.  From 3,
		 * as this call has always allocated. */
		int newfd =
			fd_install_from(cur, (vfs_file_t *)new_ufd, 3);

		if (newfd < 0) {
			unix_close(new_ufd);
			return newfd;
		}
		sock_put_peer_addr(a2, a3, &kaddr, kaddrlen);
		return newfd;
	}
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	struct sockaddr_in kaddr;
	socklen_t kaddrlen = sizeof(struct sockaddr_in);
	int new_sock_idx = sock_accept(idx, &kaddr, &kaddrlen);
	if (new_sock_idx < 0)
		return new_sock_idx;
	// Allocate fd for the new accepted socket
	task_t *cur = sched_current();
	if (!cur) {
		sock_close(new_sock_idx);
		return -EFAULT;
	}
	{
		int newfd = fd_install_from(
			cur, MAKE_SOCKET_FD(new_sock_idx), 3);

		if (newfd < 0) {
			sock_close(new_sock_idx);
			return newfd;
		}
		sock_put_peer_addr(a2, a3, &kaddr, kaddrlen);
		return newfd;
	}
}

int64_t sys_connect(uint64_t a1, uint64_t a2, uint64_t a3)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		/* Validate and copy only the DECLARED length, into a
		 * zeroed structure.
		 *
		 * Requiring the full sizeof(struct sockaddr_un) to be
		 * readable rejects a caller that allocated exactly
		 * SUN_LEN bytes -- which is legal, and which the
		 * address is normally sized by.  Zeroing first means
		 * the untouched tail is deterministic rather than
		 * stack garbage. */
		socklen_t alen = (socklen_t)a3;

		if (alen < sizeof(sa_family_t))
			return -EINVAL;
		if (alen > sizeof(struct sockaddr_un))
			alen = sizeof(struct sockaddr_un);
		if (!validate_user_ptr(a2, alen))
			return -EFAULT;
		struct sockaddr_un kaddr;
		mm_memset(&kaddr, 0, sizeof(kaddr));
		if (copy_from_user(&kaddr, (const void *)a2, alen) != 0)
			return -EFAULT;
		return unix_connect(ufd, &kaddr, alen);
	}
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	struct sockaddr_in kaddr;
	if (!validate_user_ptr(a2, sizeof(struct sockaddr_in)))
		return -EFAULT;
	copy_from_user(&kaddr, (const void *)a2,
		       sizeof(struct sockaddr_in));
	return sock_connect(idx, &kaddr);
}

int64_t sys_sendto(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		if (!validate_user_ptr(a2, a3))
			return -EFAULT;
		return unix_send(ufd, (const void *)a2, (size_t)a3,
				 (int)a4);
	}
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	if (!validate_user_ptr(a2, a3))
		return -EFAULT;
	struct sockaddr_in kaddr;
	const struct sockaddr_in *dest = NULL;
	if (a5 && validate_user_ptr(a5, sizeof(struct sockaddr_in))) {
		copy_from_user(&kaddr, (const void *)a5,
			       sizeof(struct sockaddr_in));
		dest = &kaddr;
	}
	return sock_sendto(idx, (const void *)a2, (size_t)a3, (int)a4,
			   dest, dest ? sizeof(struct sockaddr_in) : 0);
}

int64_t sys_recvfrom(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		if (!validate_user_ptr(a2, a3))
			return -EFAULT;
		return unix_recv(ufd, (void *)a2, (size_t)a3, (int)a4);
	}
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	if (!validate_user_ptr(a2, a3))
		return -EFAULT;
	struct sockaddr_in kaddr;
	socklen_t kaddrlen = sizeof(struct sockaddr_in);
	int ret = sock_recvfrom(idx, (void *)a2, (size_t)a3, (int)a4,
				&kaddr, &kaddrlen);
	if (ret >= 0 && a5 &&
	    validate_user_ptr(a5, sizeof(struct sockaddr_in)))
		copy_to_user((void *)a5, &kaddr,
			     sizeof(struct sockaddr_in));
	return ret;
}

int64_t sys_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		if (!validate_user_ptr(a2, a3))
			return -EFAULT;
		return unix_send(ufd, (const void *)a2, (size_t)a3,
				 (int)a4);
	}
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	if (!validate_user_ptr(a2, a3))
		return -EFAULT;
	return sock_send(idx, (const void *)a2, (size_t)a3, (int)a4);
}

int64_t sys_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		if (!validate_user_ptr(a2, a3))
			return -EFAULT;
		return unix_recv(ufd, (void *)a2, (size_t)a3, (int)a4);
	}
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	if (!validate_user_ptr(a2, a3))
		return -EFAULT;
	return sock_recv(idx, (void *)a2, (size_t)a3, (int)a4);
}

int64_t sys_shutdown(uint64_t a1, uint64_t a2)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd)
		return unix_shutdown(ufd, (int)a2);
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	return sock_shutdown(idx, (int)a2);
}

int64_t sys_setsockopt(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	/* AF_UNIX first, as in every other socket arm.  Going straight
	 * to sock_idx_from_fd() answered -ENOTSOCK for every option on
	 * every local socket, and a caller cannot read that as "option
	 * unsupported" -- it says the descriptor is not a socket, so
	 * the sensible reaction is to abandon it.  PCManFM sets
	 * SO_REUSEADDR on its single-instance socket and bails out of
	 * the same expression as its bind(), which made it exit with
	 * status 1 and no message instead of opening a window. */
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	int idx = 0;

	if (!ufd) {
		idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
	}
	socklen_t koptlen = (socklen_t)a5;
	uint8_t koptbuf[256] = { 0 };
	if (koptlen > 0) {
		size_t copy_len = koptlen;
		if (!a4)
			return -EFAULT;
		if (!validate_user_ptr(a4, copy_len))
			return -EFAULT;
		if (copy_len > sizeof(koptbuf))
			copy_len = sizeof(koptbuf);
		int copy_rc = copy_from_user(koptbuf, (const void *)a4,
					     copy_len);
		if (copy_rc < 0)
			return copy_rc;
	}
	if (ufd)
		return unix_setsockopt(ufd, (int)a2, (int)a3,
				       koptlen > 0 ?
					       (const void *)koptbuf :
					       NULL,
				       koptlen);
	return sock_setsockopt(
		idx, (int)a2, (int)a3,
		koptlen > 0 ? (const void *)koptbuf : NULL, koptlen);
}

int64_t sys_getsockopt(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	/* AF_UNIX first — see SYS_SETSOCKOPT above. */
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	int idx = 0;

	if (!ufd) {
		idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
	}
	socklen_t koptlen = 0;
	uint8_t koptbuf[256] = { 0 };
	if (a5 && validate_user_ptr(a5, sizeof(socklen_t)))
		copy_from_user(&koptlen, (const void *)a5,
			       sizeof(socklen_t));
	if (koptlen > 0) {
		if (!a4)
			return -EFAULT;
		if (!validate_user_ptr(a4, koptlen))
			return -EFAULT;
		if (koptlen > sizeof(koptbuf))
			koptlen = sizeof(koptbuf);
	}
	int ret = ufd ? unix_getsockopt(ufd, (int)a2, (int)a3,
					koptlen > 0 ? (void *)koptbuf :
						      NULL,
					&koptlen) :
			sock_getsockopt(idx, (int)a2, (int)a3,
					koptlen > 0 ? (void *)koptbuf :
						      NULL,
					&koptlen);
	if (ret == 0 && a4 && koptlen > 0)
		copy_to_user((void *)a4, koptbuf, koptlen);
	if (ret == 0 && a5 && validate_user_ptr(a5, sizeof(socklen_t)))
		copy_to_user((void *)a5, &koptlen, sizeof(socklen_t));
	return ret;
}

int64_t sys_getpeername(uint64_t a1, uint64_t a2, uint64_t a3)
{
	/* AF_UNIX first: sock_idx_from_fd() only knows AF_INET, so a
	 * Unix socket used to fail here -- and an X client that cannot
	 * name its own socket cannot choose an authorisation record,
	 * so it sends none and the server rejects it with
	 * "Authorization required, but no authorization protocol
	 * specified".  Neither message mentions getpeername(). */
	unix_socket_t *ufd = unix_sock_from_fd(a1);

	if (ufd) {
		struct sockaddr_un ukaddr;
		socklen_t ulen = sizeof(struct sockaddr_un);
		int uret;

		if (a3 && validate_user_ptr(a3, sizeof(socklen_t)))
			copy_from_user(&ulen, (const void *)a3,
				       sizeof(socklen_t));
		uret = unix_getname(ufd, 1, &ukaddr, &ulen);
		if (uret < 0)
			return uret;
		/* Copy only as much as the caller's buffer holds, but
		 * report the length the address really needs -- that
		 * is how a caller learns to retry with a bigger one. */
		{
			socklen_t cap = ulen;

			if (a3 &&
			    validate_user_ptr(a3, sizeof(socklen_t))) {
				socklen_t given = 0;

				copy_from_user(&given, (const void *)a3,
					       sizeof(socklen_t));
				if (given < cap)
					cap = given;
			}
			if (a2 && cap > 0 && validate_user_ptr(a2, cap))
				copy_to_user((void *)a2, &ukaddr, cap);
			if (a3 &&
			    validate_user_ptr(a3, sizeof(socklen_t)))
				copy_to_user((void *)a3, &ulen,
					     sizeof(socklen_t));
		}
		return 0;
	}

	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	struct sockaddr_in kaddr;
	socklen_t kaddrlen = sizeof(struct sockaddr_in);
	int ret = sock_getpeername(idx, &kaddr, &kaddrlen);
	if (ret == 0 && a2 &&
	    validate_user_ptr(a2, sizeof(struct sockaddr_in)))
		copy_to_user((void *)a2, &kaddr,
			     sizeof(struct sockaddr_in));
	if (ret == 0 && a3 && validate_user_ptr(a3, sizeof(socklen_t)))
		copy_to_user((void *)a3, &kaddrlen, sizeof(socklen_t));
	return ret;
}

int64_t sys_getsockname(uint64_t a1, uint64_t a2, uint64_t a3)
{
	/* AF_UNIX first: sock_idx_from_fd() only knows AF_INET, so a
	 * Unix socket used to fail here -- and an X client that cannot
	 * name its own socket cannot choose an authorisation record,
	 * so it sends none and the server rejects it with
	 * "Authorization required, but no authorization protocol
	 * specified".  Neither message mentions getsockname(). */
	unix_socket_t *ufd = unix_sock_from_fd(a1);

	if (ufd) {
		struct sockaddr_un ukaddr;
		socklen_t ulen = sizeof(struct sockaddr_un);
		int uret;

		if (a3 && validate_user_ptr(a3, sizeof(socklen_t)))
			copy_from_user(&ulen, (const void *)a3,
				       sizeof(socklen_t));
		uret = unix_getname(ufd, 0, &ukaddr, &ulen);
		if (uret < 0)
			return uret;
		/* Copy only as much as the caller's buffer holds, but
		 * report the length the address really needs -- that
		 * is how a caller learns to retry with a bigger one. */
		{
			socklen_t cap = ulen;

			if (a3 &&
			    validate_user_ptr(a3, sizeof(socklen_t))) {
				socklen_t given = 0;

				copy_from_user(&given, (const void *)a3,
					       sizeof(socklen_t));
				if (given < cap)
					cap = given;
			}
			if (a2 && cap > 0 && validate_user_ptr(a2, cap))
				copy_to_user((void *)a2, &ukaddr, cap);
			if (a3 &&
			    validate_user_ptr(a3, sizeof(socklen_t)))
				copy_to_user((void *)a3, &ulen,
					     sizeof(socklen_t));
		}
		return 0;
	}

	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	struct sockaddr_in kaddr;
	socklen_t kaddrlen = sizeof(struct sockaddr_in);
	int ret = sock_getsockname(idx, &kaddr, &kaddrlen);
	if (ret == 0 && a2 &&
	    validate_user_ptr(a2, sizeof(struct sockaddr_in)))
		copy_to_user((void *)a2, &kaddr,
			     sizeof(struct sockaddr_in));
	if (ret == 0 && a3 && validate_user_ptr(a3, sizeof(socklen_t)))
		copy_to_user((void *)a3, &kaddrlen, sizeof(socklen_t));
	return ret;
}

int64_t sys_socketpair(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	if (!validate_user_ptr(a4, 2 * sizeof(int)))
		return -EFAULT;
	if ((int)a1 == AF_UNIX) {
		int real_type =
			(int)a2 & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
		unix_socket_t *usv[2];
		int ret = unix_socketpair(real_type, usv);
		if (ret < 0)
			return ret;
		task_t *cur = sched_current();
		if (!cur) {
			unix_close(usv[0]);
			unix_close(usv[1]);
			return -EFAULT;
		}
		/* Install through fd_install: it claims each slot and
		 * clears the slot's stale FD_CLOEXEC.  The old
		 * hand-rolled scan did neither, so a pair could inherit
		 * close-on-exec from whatever previously used those
		 * slots and vanish across the next exec. */
		int pfd[2];
		pfd[0] = fd_install(cur, (vfs_file_t *)usv[0]);
		if (pfd[0] < 0) {
			unix_close(usv[0]);
			unix_close(usv[1]);
			return pfd[0];
		}
		pfd[1] = fd_install(cur, (vfs_file_t *)usv[1]);
		if (pfd[1] < 0) {
			/* Undo the first install under the table lock: the
		 * slot is shared with every other thread here. */
		uint64_t fdflags = 0;

		fds_lock(cur, &fdflags);
		task_fds(cur)[pfd[0]] = NULL;
		fds_unlock(cur, fdflags);
			unix_close(usv[0]);
			unix_close(usv[1]);
			return pfd[1];
		}
		if ((int)a2 & SOCK_CLOEXEC) {
			task_set_fd_flags(cur, (unsigned)pfd[0],
					  FD_CLOEXEC);
			task_set_fd_flags(cur, (unsigned)pfd[1],
					  FD_CLOEXEC);
		}
		if ((int)a2 & SOCK_NONBLOCK) {
			usv[0]->nonblock = 1;
			usv[1]->nonblock = 1;
		}
		copy_to_user((void *)a4, pfd, 2 * sizeof(int));
		return 0;
	}
	int sv[2];
	int ret = sock_socketpair((int)a1, (int)a2, (int)a3, sv);
	if (ret < 0)
		return ret;
	// Allocate two process fds
	task_t *cur = sched_current();
	if (!cur) {
		sock_close(sv[0]);
		sock_close(sv[1]);
		return -EFAULT;
	}
	/* Same as the AF_UNIX path above: install via fd_install so the
	 * slots are claimed atomically and their stale FD_CLOEXEC is
	 * cleared. */
	int ufd[2];
	ufd[0] = fd_install(cur, MAKE_SOCKET_FD(sv[0]));
	if (ufd[0] < 0) {
		sock_close(sv[0]);
		sock_close(sv[1]);
		return ufd[0];
	}
	ufd[1] = fd_install(cur, MAKE_SOCKET_FD(sv[1]));
	if (ufd[1] < 0) {
		/* Undo the first install under the table lock: the
	 * slot is shared with every other thread here. */
	uint64_t fdflags = 0;

	fds_lock(cur, &fdflags);
	task_fds(cur)[ufd[0]] = NULL;
	fds_unlock(cur, fdflags);
		sock_close(sv[0]);
		sock_close(sv[1]);
		return ufd[1];
	}
	if ((int)a2 & SOCK_CLOEXEC) {
		task_set_fd_flags(cur, (unsigned)ufd[0], FD_CLOEXEC);
		task_set_fd_flags(cur, (unsigned)ufd[1], FD_CLOEXEC);
	}
	copy_to_user((void *)a4, ufd, 2 * sizeof(int));
	return 0;
}

int64_t sys_accept4(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		struct sockaddr_un kaddr;
		socklen_t kaddrlen = sizeof(struct sockaddr_un);
		unix_socket_t *new_ufd = NULL;
		int arc = unix_accept(ufd, &kaddr, &kaddrlen, &new_ufd);

		if (arc < 0)
			return arc;
		task_t *cur = sched_current();
		if (!cur) {
			unix_close(new_ufd);
			return -EFAULT;
		}
		if ((int)a4 & SOCK_NONBLOCK)
			new_ufd->nonblock = 1;
		/* One locked step, and both flags honoured.
		 * SOCK_CLOEXEC was accepted and then ignored here, so
		 * a connection asked to close on exec was inherited by
		 * every program the process went on to run. */
		int newfd =
			fd_install_from(cur, (vfs_file_t *)new_ufd, 3);

		if (newfd < 0) {
			unix_close(new_ufd);
			return newfd;
		}
		if ((int)a4 & SOCK_CLOEXEC)
			task_set_fd_flags(cur, (unsigned)newfd,
					  FD_CLOEXEC);
		sock_put_peer_addr(a2, a3, &kaddr, kaddrlen);
		return newfd;
	}
	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	struct sockaddr_in kaddr;
	socklen_t kaddrlen = sizeof(struct sockaddr_in);
	int new_sock_idx =
		sock_accept4(idx, &kaddr, &kaddrlen, (int)a4);
	if (new_sock_idx < 0)
		return new_sock_idx;
	task_t *cur = sched_current();
	if (!cur) {
		sock_close(new_sock_idx);
		return -EFAULT;
	}
	{
		int newfd = fd_install_from(
			cur, MAKE_SOCKET_FD(new_sock_idx), 3);

		if (newfd < 0) {
			sock_close(new_sock_idx);
			return newfd;
		}
		if ((int)a4 & SOCK_CLOEXEC)
			task_set_fd_flags(cur, (unsigned)newfd,
					  FD_CLOEXEC);
		sock_put_peer_addr(a2, a3, &kaddr, kaddrlen);
		return newfd;
	}
}

int64_t sys_sendmsg(uint64_t a1, uint64_t a2, uint64_t a3)
{
	if (!validate_user_ptr(a2, sizeof(struct msghdr)))
		return -EFAULT;
	struct msghdr kmsg;
	copy_from_user(&kmsg, (const void *)a2, sizeof(struct msghdr));

	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd)
		return unix_do_sendmsg(ufd, &kmsg);

	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	return sock_sendmsg(idx, &kmsg, (int)a3);
}

int64_t sys_recvmsg(uint64_t a1, uint64_t a2, uint64_t a3)
{
	if (!validate_user_ptr(a2, sizeof(struct msghdr)))
		return -EFAULT;
	struct msghdr kmsg;
	copy_from_user(&kmsg, (const void *)a2, sizeof(struct msghdr));

	unix_socket_t *ufd = unix_sock_from_fd(a1);
	if (ufd) {
		int ret = unix_do_recvmsg(ufd, &kmsg);
		if (ret >= 0)
			copy_to_user((void *)a2, &kmsg,
				     sizeof(struct msghdr));
		return ret;
	}

	int idx = sock_idx_from_fd(a1);
	if (idx < 0)
		return idx;
	int ret = sock_recvmsg(idx, &kmsg, (int)a3);
	if (ret >= 0)
		copy_to_user((void *)a2, &kmsg, sizeof(struct msghdr));
	return ret;
}

int64_t sys_sendfile(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	int64_t koffset = 0;
	int64_t *koffp = NULL;
	if (a3) {
		if (!validate_user_ptr(a3, sizeof(int64_t)))
			return -EFAULT;
		copy_from_user(&koffset, (void *)a3, sizeof(int64_t));
		koffp = &koffset;
	}
	int ret = sock_sendfile((int)a1, (int)a2, koffp, (size_t)a4);
	if (a3 && ret >= 0) {
		copy_to_user((void *)a3, &koffset, sizeof(int64_t));
	}
	return ret;
}

int64_t sys_dns_resolve(uint64_t a1, uint64_t a2)
{
	if (!validate_user_ptr(a1, 1))
		return -EFAULT;
	if (!validate_user_ptr(a2, sizeof(uint32_t)))
		return -EFAULT;
	// Copy hostname from user space (max 255 chars).
	// Copy page-by-page to avoid faulting across page boundaries.
	char khost[256];
	size_t off = 0;
	khost[0] = '\0';
	smap_disable();
	while (off < 255) {
		uintptr_t addr = a1 + off;
		// Check that the page containing this byte is mapped
		if (!mm_is_page_mapped(addr))
			break;
		// Copy up to end of this page (or remaining buffer)
		size_t page_end = (addr | 0xFFF) + 1;
		size_t chunk = page_end - addr;
		if (off + chunk > 255)
			chunk = 255 - off;
		for (size_t j = 0; j < chunk; j++) {
			khost[off] = ((const char *)a1)[off];
			if (khost[off] == '\0')
				goto dns_str_done;
			off++;
		}
	}
dns_str_done:
	smap_enable();
	khost[off] = '\0';
	if (off == 0)
		return -EFAULT;
	uint32_t ip = 0;
	int ret = dns_resolve(khost, &ip);
	if (ret == 0) {
		copy_to_user((void *)a2, &ip, sizeof(uint32_t));
	}
	return ret;
}

int64_t sys_net_getinfo(uint64_t a1, uint64_t a2, uint64_t a3)
{
	int subcmd = (int)a1;
	if (!validate_user_ptr(a2, 1))
		return -EFAULT;
	int max_entries = (int)a3;
	if (max_entries <= 0)
		return -EINVAL;

	switch (subcmd) {
	case NET_GET_ARP_TABLE: {
		size_t sz =
			(size_t)max_entries * sizeof(net_arp_info_t);
		if (!validate_user_ptr(a2, sz))
			return -EFAULT;
		net_arp_info_t kbuf[64];
		int n = max_entries > 64 ? 64 : max_entries;
		int count = net_get_arp_table(kbuf, n);
		copy_to_user((void *)a2, kbuf,
			     (size_t)count * sizeof(net_arp_info_t));
		return count;
	}
	case NET_GET_ROUTE_TABLE: {
		size_t sz =
			(size_t)max_entries * sizeof(net_route_info_t);
		if (!validate_user_ptr(a2, sz))
			return -EFAULT;
		net_route_info_t kbuf[32];
		int n = max_entries > 32 ? 32 : max_entries;
		int count = net_get_route_table(kbuf, n);
		copy_to_user((void *)a2, kbuf,
			     (size_t)count * sizeof(net_route_info_t));
		return count;
	}
	case NET_GET_TCP_CONNECTIONS: {
		size_t sz =
			(size_t)max_entries * sizeof(net_tcp_info_t);
		if (!validate_user_ptr(a2, sz))
			return -EFAULT;
		net_tcp_info_t kbuf[64];
		int n = max_entries > 64 ? 64 : max_entries;
		int count = net_get_tcp_connections(kbuf, n);
		copy_to_user((void *)a2, kbuf,
			     (size_t)count * sizeof(net_tcp_info_t));
		return count;
	}
	case NET_GET_UDP_SOCKETS: {
		size_t sz =
			(size_t)max_entries * sizeof(net_udp_info_t);
		if (!validate_user_ptr(a2, sz))
			return -EFAULT;
		net_udp_info_t kbuf[64];
		int n = max_entries > 64 ? 64 : max_entries;
		int count = net_get_udp_sockets(kbuf, n);
		copy_to_user((void *)a2, kbuf,
			     (size_t)count * sizeof(net_udp_info_t));
		return count;
	}
	case NET_GET_IFACE_STATS: {
		size_t sz =
			(size_t)max_entries * sizeof(net_iface_info_t);
		if (!validate_user_ptr(a2, sz))
			return -EFAULT;
		net_iface_info_t kbuf[8];
		int n = max_entries > 8 ? 8 : max_entries;
		int count = net_get_iface_info(kbuf, n);
		copy_to_user((void *)a2, kbuf,
			     (size_t)count * sizeof(net_iface_info_t));
		return count;
	}
	case NET_GET_NETSTATS: {
		if (!validate_user_ptr(a2, sizeof(net_stats_info_t)))
			return -EFAULT;
		net_stats_info_t kbuf;
		if (net_get_stats(&kbuf) != 0)
			return -EINVAL;
		copy_to_user((void *)a2, &kbuf,
			     sizeof(net_stats_info_t));
		return 1;
	}
	case NET_DNS_QUERY: {
		if (!validate_user_ptr(a2, sizeof(dns_query_buf_t)))
			return -EFAULT;
		dns_query_buf_t kbuf;
		copy_from_user(&kbuf, (void *)a2,
			       sizeof(dns_query_buf_t));
		kbuf.name[255] = '\0';
		int rlen = dns_query_raw(kbuf.name, kbuf.qtype,
					 kbuf.response, 512);
		kbuf.response_len = rlen;
		copy_to_user((void *)a2, &kbuf,
			     sizeof(dns_query_buf_t));
		return rlen > 0 ? 0 : rlen;
	}
	default:
		return -EINVAL;
	}
}

int64_t sys_dhcp_control(uint64_t a1)
{
	int subcmd = (int)a1;
	net_device_t *dev = net_get_default_device();
	if (!dev)
		return -ENETDOWN;
	/* DISCOVER/RELEASE/RENEW reconfigure the interface address and
	 * are privileged; STATUS is read-only and open to all. */
	if (subcmd != DHCP_CMD_STATUS && !capable())
		return -EPERM;
	switch (subcmd) {
	case DHCP_CMD_DISCOVER:
		return dhcp_discover(dev);
	case DHCP_CMD_RELEASE:
		return dhcp_release(dev);
	case DHCP_CMD_RENEW:
		return dhcp_renew(dev);
	case DHCP_CMD_STATUS:
		return dhcp_get_status();
	default:
		return -EINVAL;
	}
}

int64_t sys_raw_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	// a1 = subcmd (1=ICMP echo, 2=ARP request)
	// a2 = dst_ip, a3 = id/seq packed, a4 = ttl, a5 = data_ptr (optional)
	int subcmd = (int)a1;
	net_device_t *dev = net_get_default_device();
	if (!dev)
		return -ENETDOWN;
	if (subcmd == 1) {
		// ICMP echo: a2=dst_ip, a3=id<<16|seq, a4=ttl
		uint32_t dst_ip = (uint32_t)a2;
		uint16_t id = (uint16_t)(a3 >> 16);
		uint16_t seq = (uint16_t)(a3 & 0xFFFF);
		uint8_t ttl = (uint8_t)a4;
		if (ttl == 0)
			ttl = 64;
		// 56 bytes of padding data
		uint8_t pad[56];
		for (int pi = 0; pi < 56; pi++)
			pad[pi] = (uint8_t)pi;
		int send_ret = icmp_send_echo(dev, dst_ip, id, seq, pad,
					      56, ttl);
		loopback_process_pending();
		return send_ret;
	} else if (subcmd == 2) {
		// ARP request: a2=target_ip
		uint32_t target_ip = (uint32_t)a2;
		return arp_send_request(dev, target_ip);
	}
	return -EINVAL;
}

int64_t sys_raw_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	// a1 = subcmd (1=ICMP reply, 2=ARP reply)
	// a2 = ptr to result struct, a3 = expected_id or target_ip,
	// a4 = timeout in MILLISECONDS (0 = default).
	//
	// Milliseconds, not ticks: the tick rate is measured at boot
	// and is not a number userspace can know, yet this used to take
	// a tick count -- so ping, arping and traceroute each did their
	// own `seconds * 100' and every one of them waited for the
	// wrong length of time on a machine whose rate was not 100Hz.
	int subcmd = (int)a1;
	if (subcmd == 1) {
		// ICMP reply
		if (!validate_user_ptr(a2, 24))
			return -EFAULT;
		uint32_t src_ip = 0;
		uint8_t type = 0, code = 0, recv_ttl = 0;
		uint16_t seq = 0;
		uint16_t expected_id = (uint16_t)(a3 >> 16);
		uint16_t expected_seq = (uint16_t)(a3 & 0xFFFF);
		uint64_t timeout = timer_ms_to_ticks(a4 ? a4 : 5000);
		uint64_t rtt_us = 0;
		int ret = icmp_recv_reply(&src_ip, expected_id, &type,
					  &code, &seq, timeout, &rtt_us,
					  expected_seq, &recv_ttl);
		if (ret == 0) {
			// Pack result: [src_ip(4), type(1), code(1), seq(2), rtt_us(8), ttl(1), pad(7)]
			uint8_t result[24];
			for (int i = 0; i < 24; i++)
				result[i] = 0;
			result[0] = (src_ip >> 24) & 0xFF;
			result[1] = (src_ip >> 16) & 0xFF;
			result[2] = (src_ip >> 8) & 0xFF;
			result[3] = src_ip & 0xFF;
			result[4] = type;
			result[5] = code;
			result[6] = (seq >> 8) & 0xFF;
			result[7] = seq & 0xFF;
			// Pack RTT in microseconds (little-endian uint64_t)
			for (int i = 0; i < 8; i++)
				result[8 + i] =
					(rtt_us >> (i * 8)) & 0xFF;
			result[16] = recv_ttl;
			copy_to_user((void *)a2, result, 24);
		}
		return ret;
	} else if (subcmd == 2) {
		// ARP reply
		if (!validate_user_ptr(a2, 6))
			return -EFAULT;
		uint32_t target_ip = (uint32_t)a3;
		uint64_t timeout = timer_ms_to_ticks(a4 ? a4 : 5000);
		uint8_t mac[6];
		int ret = arp_recv_reply(target_ip, mac, timeout);
		if (ret == 0) {
			copy_to_user((void *)a2, mac, 6);
		}
		return ret;
	}
	return -EINVAL;
}

int64_t sys_dns_resolve_reverse(uint64_t a1, uint64_t a2, uint64_t a3)
{
	// a1 = IP address in network byte order
	// a2 = pointer to output hostname buffer (user)
	// a3 = max length of output buffer
	uint32_t ip_nbo = (uint32_t)a1;
	int maxlen = (int)a3;
	if (maxlen <= 0 || maxlen > 256)
		return -EINVAL;
	if (!validate_user_ptr(a2, (size_t)maxlen))
		return -EFAULT;

	char kbuf[256];
	int ret = dns_resolve_reverse(ip_nbo, kbuf, sizeof(kbuf));
	if (ret == 0) {
		// Copy result to user space
		size_t slen = 0;
		while (kbuf[slen])
			slen++;
		if ((int)(slen + 1) > maxlen)
			return -ENAMETOOLONG;
		copy_to_user((void *)a2, kbuf, slen + 1);
	}
	return ret;
}

int64_t sys_set_dns_server(uint64_t a1, uint64_t a2)
{
	// a1 = ifname (user, NUL-terminated, may be NULL/empty for "all")
	// a2 = IPv4 address in network byte order (0 to clear)
	// RFC 3493: install resolver server.  Used by the userland
	// /etc/resolv.conf parser at boot before DHCP completes, and
	// by `dhclient` for manual overrides.
	uint32_t ip_nbo = (uint32_t)a2;
	char ifname[16] = { 0 };
	int have_name = 0;
	if (a1) {
		if (!validate_user_ptr(a1, 1))
			return -EFAULT;
		copy_from_user(ifname, (void *)a1, sizeof(ifname) - 1);
		ifname[sizeof(ifname) - 1] = 0;
		if (ifname[0])
			have_name = 1;
	}
	int updated = 0;
	for (int i = 0; i < 16; i++) {
		net_device_t *d = net_get_device(i);
		if (!d)
			continue;
		if (have_name) {
			int match = 1;
			for (int k = 0; k < 16; k++) {
				if (d->name[k] != ifname[k]) {
					match = 0;
					break;
				}
				if (!ifname[k])
					break;
			}
			if (!match)
				continue;
		}
		d->dns_server = ip_nbo;
		updated++;
	}
	// Loopback too, so test_libc on loopback still has a resolver.
	net_device_t *lo = net_get_loopback();
	if (lo &&
	    (!have_name || (ifname[0] == 'l' && ifname[1] == 'o' &&
			    ifname[2] == 0))) {
		lo->dns_server = ip_nbo;
		updated++;
	}
	return updated > 0 ? 0 : -ENODEV;
}
