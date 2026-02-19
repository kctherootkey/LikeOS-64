// LikeOS-64 Pipe Support
#ifndef _KERNEL_PIPE_H_
#define _KERNEL_PIPE_H_

#include "types.h"
#include "sched.h"

#define PIPE_MAGIC 0x50495045U  // "PIPE"

typedef struct pipe {
    uint8_t* buffer;
    size_t size;
    size_t read_pos;
    size_t write_pos;
    size_t used;
    int readers;
    int writers;
    spinlock_t lock;  // Protects all pipe state
} pipe_t;

typedef struct pipe_end {
    uint32_t magic;
    uint8_t is_read;
    uint8_t pad[3];
    pipe_t* pipe;
} pipe_end_t;

bool pipe_is_end(const void* ptr);
pipe_t* pipe_create(size_t size);
pipe_end_t* pipe_create_end(pipe_t* pipe, bool is_read);
pipe_end_t* pipe_dup_end(pipe_end_t* end);
void pipe_close_end(pipe_end_t* end);

#endif // _KERNEL_PIPE_H_