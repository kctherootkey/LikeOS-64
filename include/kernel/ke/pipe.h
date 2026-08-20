// LikeOS-64 Pipe Support
#ifndef _KERNEL_PIPE_H_
#define _KERNEL_PIPE_H_

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/waitq.h> /* struct wait_queue_head, for the poll queue */

#define PIPE_MAGIC 0x50495045U // "PIPE"

typedef struct pipe {
	uint8_t *buffer;
	size_t size;
	size_t read_pos;
	size_t write_pos;
	size_t used;
	int readers;
	int writers;
	spinlock_t lock; // Protects all pipe state
	/* Who is polling this pipe.  A readiness change wakes only these
	 * tasks -- see <kernel/ke/waitq.h>. */
	struct wait_queue_head poll_wq;
} pipe_t;

typedef struct pipe_end {
	uint32_t magic;
	uint8_t is_read;
	uint8_t pad[3];
	pipe_t *pipe;
	uint32_t flags; // O_NONBLOCK etc.
	/* Holders of this end.  One for the descriptor that owns it, plus one
	 * for each in-progress operation that looked it up (see fdget()).
	 *
	 * Without it there was no way to name a pipe end for the duration of a
	 * read or a write: the end IS the descriptor, so a sibling thread
	 * closing that descriptor freed the object out from under the
	 * operation.  Counting holders means the end survives until the last
	 * one lets go, and the reader/writer tally the other side watches for
	 * end-of-file moves only then -- which is the moment the descriptor is
	 * really finished with. */
	volatile int refcount;
} pipe_end_t;

bool pipe_is_end(const void *ptr);
pipe_t *pipe_create(size_t size);
pipe_end_t *pipe_create_end(pipe_t *pipe, bool is_read);
pipe_end_t *pipe_dup_end(pipe_end_t *end);
/* Take one more hold on an end that the caller already knows is live (it came
 * out of a descriptor table under that table's lock).  Returns false if it is
 * not a pipe end at all. */
bool pipe_end_hold(pipe_end_t *end);
/* Drop one hold.  The end is freed, and the pipe's reader/writer tally
 * updated, when the last one goes. */
void pipe_close_end(pipe_end_t *end);

#endif // _KERNEL_PIPE_H_