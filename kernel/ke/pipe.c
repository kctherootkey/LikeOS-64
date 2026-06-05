// LikeOS-64 Pipe Implementation
#include <kernel/pipe.h>
#include <kernel/memory.h>
#include <kernel/sched.h>
#include <kernel/bug.h>

bool pipe_is_end(const void* ptr) {
    if (!ptr) {
        return false;
    }
    /* Reject small marker values stashed in the fd table (e.g. KEYBOARD_FD=1,
     * TTY_FD=2, etc.) and any non-canonical kernel pointer. */
    uintptr_t v = (uintptr_t)ptr;
    if (v < 0x1000) return false;
    if (v < 0xffffffff80000000ULL && v > 0x00007fffffffffffULL) return false;
    const pipe_end_t* end = (const pipe_end_t*)ptr;
    return end->magic == PIPE_MAGIC;
}

pipe_t* pipe_create(size_t size) {
    might_sleep();
    if (size == 0) {
        return NULL;
    }

    pipe_t* pipe = (pipe_t*)kalloc(sizeof(pipe_t));
    if (!pipe) {
        return NULL;
    }
    mm_memset(pipe, 0, sizeof(pipe_t));

    pipe->buffer = (uint8_t*)kalloc(size);
    if (!pipe->buffer) {
        kfree(pipe);
        return NULL;
    }

    pipe->size = size;
    WARN_ON(pipe->size == 0);  /* pipe buffer size is zero after create */
    WARN_ON(pipe->buffer == NULL);  /* pipe buffer is NULL after create */
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->used = 0;
    pipe->readers = 0;
    pipe->writers = 0;
    spinlock_init(&pipe->lock, "pipe");

    return pipe;
}

pipe_end_t* pipe_create_end(pipe_t* pipe, bool is_read) {
    if (!pipe) {
        return NULL;
    }

    pipe_end_t* end = (pipe_end_t*)kalloc(sizeof(pipe_end_t));
    if (!end) {
        return NULL;
    }

    end->magic = PIPE_MAGIC;
    end->is_read = is_read ? 1 : 0;
    end->pad[0] = end->pad[1] = end->pad[2] = 0;
    end->pipe = pipe;
    end->flags = 0;

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

pipe_end_t* pipe_dup_end(pipe_end_t* end) {
    if (!end || end->magic != PIPE_MAGIC) {
        return NULL;
    }
    WARN_ON(end->pipe == NULL);  /* dup'ing pipe_end with no backing pipe: pipe_create_end forgot to set pipe pointer */

    return pipe_create_end(end->pipe, end->is_read != 0);
}

void pipe_close_end(pipe_end_t* end) {
    BUG_ON(end == NULL);
    if (!end || end->magic != PIPE_MAGIC) {
        WARN_ON_ONCE(!end || end->magic != PIPE_MAGIC);  /* close with bad magic: double-close or corruption */
        return;
    }

    // Invalidate magic BEFORE freeing to prevent double-close via stale pointer
    end->magic = 0;

    pipe_t* pipe = end->pipe;
    if (pipe) {
        uint64_t flags;
        spin_lock_irqsave(&pipe->lock, &flags);
        
        bool should_free = false;
        if (end->is_read) {
            if (pipe->readers > 0) {
                pipe->readers--;
            } else {
                WARN_ON_ONCE(1);  /* pipe readers underflow: more closes than opens */
            }
        } else {
            if (pipe->writers > 0) {
                pipe->writers--;
            } else {
                WARN_ON_ONCE(1);  /* pipe writers underflow: more closes than opens */
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