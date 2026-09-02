// LikeOS-64 Pipe Implementation
#include <kernel/ke/waitq.h>
#include <kernel/ke/pipe.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/sched.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/syscall.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>

bool pipe_is_end(const void *ptr)
{
	if (!ptr) {
		return false;
	}
	/* Reject every tagged fd-table MARKER, not just the tiny stdio ones:
	 * sockets (0x10000), epoll (0x20000) and unix sockets (0x30000) are
	 * encoded as small integers too, and reading ->magic out of one of
	 * those faults the kernel.  Callers are expected to classify markers
	 * first, but this predicate is invoked from many fd paths, so it must
	 * be safe on any fd-table value.
	 *
	 * The bound this used to apply by hand accepted only the higher-half
	 * kernel range, which held for a pipe end because it is 24 bytes and
	 * so always comes from the slab.  kptr_plausible() accepts the direct
	 * map as well, so the test does not depend on the size of the object
	 * being asked about -- see its comment. */
	uintptr_t v = (uintptr_t)ptr;
	if (!kptr_plausible((uint64_t)v))
		return false;
	const pipe_end_t *end = (const pipe_end_t *)ptr;
	return end->magic == PIPE_MAGIC;
}

pipe_t *pipe_create(size_t size)
{
	might_sleep();
	if (size == 0) {
		return NULL;
	}

	pipe_t *pipe = (pipe_t *)kalloc(sizeof(pipe_t));
	if (!pipe) {
		return NULL;
	}
	mm_memset(pipe, 0, sizeof(pipe_t));

	pipe->buffer = (uint8_t *)kalloc(size);
	if (!pipe->buffer) {
		kfree(pipe);
		return NULL;
	}

	wq_head_init(&pipe->poll_wq, "pipe-poll");
	pipe->size = size;
	WARN_ON(pipe->size == 0); /* pipe buffer size is zero after create */
	WARN_ON(pipe->buffer == NULL); /* pipe buffer is NULL after create */
	pipe->read_pos = 0;
	pipe->write_pos = 0;
	pipe->used = 0;
	pipe->readers = 0;
	pipe->writers = 0;
	spinlock_init(&pipe->lock, "pipe");

	return pipe;
}

pipe_end_t *pipe_create_end(pipe_t *pipe, bool is_read)
{
	if (!pipe) {
		return NULL;
	}

	pipe_end_t *end = (pipe_end_t *)kalloc(sizeof(pipe_end_t));
	if (!end) {
		return NULL;
	}

	end->magic = PIPE_MAGIC;
	end->is_read = is_read ? 1 : 0;
	end->pad[0] = end->pad[1] = end->pad[2] = 0;
	end->pipe = pipe;
	end->flags = 0;
	end->refcount = 1; /* the descriptor that will own it */

	uint64_t flags;
	spin_lock_irqsave(&pipe->lock, &flags);
	if (is_read) {
		pipe->readers++;
	} else {
		pipe->writers++;
	}
	spin_unlock_irqrestore(&pipe->lock, flags);

	return end;
}

pipe_end_t *pipe_dup_end(pipe_end_t *end)
{
	if (!end || end->magic != PIPE_MAGIC) {
		return NULL;
	}
	WARN_ON(end->pipe ==
		NULL); /* dup'ing pipe_end with no backing pipe: pipe_create_end forgot to set pipe pointer */

	return pipe_create_end(end->pipe, end->is_read != 0);
}

bool pipe_end_hold(pipe_end_t *end)
{
	if (!end || end->magic != PIPE_MAGIC)
		return false;
	__atomic_fetch_add(&end->refcount, 1, __ATOMIC_ACQ_REL);
	return true;
}

void pipe_close_end(pipe_end_t *end)
{
	BUG_ON(end == NULL);
	if (!end || end->magic != PIPE_MAGIC) {
		WARN_ON_ONCE(
			!end ||
			end->magic !=
				PIPE_MAGIC); /* close with bad magic: double-close or corruption */
		return;
	}

	/* Only the last holder tears the end down.  Everything below -- the
	 * reader/writer tally, the wake, the free -- is what the DESCRIPTOR
	 * going away means, and it must not happen while an operation that
	 * looked this end up is still running. */
	if (__atomic_sub_fetch(&end->refcount, 1, __ATOMIC_ACQ_REL) > 0)
		return;

	// Invalidate magic BEFORE freeing to prevent double-close via stale pointer
	end->magic = 0;

	pipe_t *pipe = end->pipe;
	if (pipe) {
		uint64_t flags;
		spin_lock_irqsave(&pipe->lock, &flags);

		bool should_free = false;
		if (end->is_read) {
			if (pipe->readers > 0) {
				pipe->readers--;
			} else {
				WARN_ON_ONCE(
					1); /* pipe readers underflow: more closes than opens */
			}
		} else {
			if (pipe->writers > 0) {
				pipe->writers--;
			} else {
				WARN_ON_ONCE(
					1); /* pipe writers underflow: more closes than opens */
			}
		}

		if (pipe->readers == 0 && pipe->writers == 0) {
			should_free = true;
		}

		spin_unlock_irqrestore(&pipe->lock, flags);

		// Wake up waiters outside the lock
		sched_wake_channel(pipe);

		if (should_free) {
			if (pipe->buffer) {
				kfree(pipe->buffer);
			}
			kfree(pipe);
		}
	}

	kfree(end);
}

// SYS_PIPE - create a pipe
int64_t sys_pipe(uint64_t pipefd_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	if (!validate_user_ptr(pipefd_ptr, sizeof(int) * 2)) {
		return -EFAULT;
	}

	/* 64 KB, which is what a conventional Unix gives a pipe, and not the
	 * single page this used to be.
	 *
	 * The size is not just a throughput knob: a writer whose message does
	 * not fit blocks part-way through it, and a reader that waits for a
	 * whole message before consuming any of it then waits for a writer
	 * that cannot finish.  Protocols that send self-describing packets
	 * over a pipe -- a plugin scanner reporting what it found, for one --
	 * are built expecting the usual capacity and stall against a smaller
	 * one, part-way through the work, with nothing to show for it. */
	pipe_t *pipe = pipe_create(64 * 1024);
	if (!pipe) {
		return -ENOMEM;
	}

	pipe_end_t *read_end = pipe_create_end(pipe, true);
	if (!read_end) {
		if (pipe->buffer) {
			kfree(pipe->buffer);
		}
		kfree(pipe);
		return -ENOMEM;
	}

	pipe_end_t *write_end = pipe_create_end(pipe, false);
	if (!write_end) {
		pipe_close_end(read_end);
		return -ENOMEM;
	}

	/* Installing the read end also reserves its slot, so the write end
	 * cannot land on the same number. */
	int fd_read = fd_install(cur, (vfs_file_t *)read_end);
	if (fd_read < 0) {
		pipe_close_end(read_end);
		pipe_close_end(write_end);
		return fd_read;
	}

	int fd_write = fd_install(cur, (vfs_file_t *)write_end);
	if (fd_write < 0) {
		/* Undo the first install under the table lock: the slot is
		 * shared with every other thread of this process. */
		uint64_t fdflags = 0;

		fds_lock(cur, &fdflags);
		task_fds(cur)[fd_read] = NULL;
		fds_unlock(cur, fdflags);
		pipe_close_end(read_end);
		pipe_close_end(write_end);
		return fd_write;
	}

	// SMAP-aware write to user array
	int *user_pipefd = (int *)pipefd_ptr;
	smap_disable();
	user_pipefd[0] = fd_read;
	user_pipefd[1] = fd_write;
	smap_enable();

	return 0;
}
