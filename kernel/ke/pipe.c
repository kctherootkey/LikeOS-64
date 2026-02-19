// LikeOS-64 Pipe Implementation
#include <kernel/pipe.h>
#include <kernel/memory.h>
#include <kernel/sched.h>

bool pipe_is_end(const void* ptr) {
    if (!ptr) {
        return false;
    }
    const pipe_end_t* end = (const pipe_end_t*)ptr;
    return end->magic == PIPE_MAGIC;
}

pipe_t* pipe_create(size_t size) {
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

    return pipe_create_end(end->pipe, end->is_read != 0);
}

void pipe_close_end(pipe_end_t* end) {
    if (!end || end->magic != PIPE_MAGIC) {
        return;
    }

    pipe_t* pipe = end->pipe;
    if (pipe) {
        uint64_t flags;
        spin_lock_irqsave(&pipe->lock, &flags);
        
        bool should_free = false;
        if (end->is_read) {
            if (pipe->readers > 0) {
                pipe->readers--;
            }
        } else {
            if (pipe->writers > 0) {
                pipe->writers--;
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