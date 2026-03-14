/* scratch_buffer.gl.h -- gnulib version of scratch_buffer
   Generated for LikeOS build. */

#ifndef _GL_MALLOC_SCRATCH_BUFFER_H
#define _GL_MALLOC_SCRATCH_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

/* Scratch buffer.  Must be initialized with scratch_buffer_init
   before its use.  */
struct scratch_buffer {
  void *data;    /* Pointer to the beginning of the scratch area.  */
  size_t length; /* Allocated space at the data pointer, in bytes.  */
  union { long double __align; char __c[1024]; } __space;
};

/* Initializes *BUFFER so that BUFFER->data points to BUFFER->__space
   and BUFFER->length reflects the available space.  */
static inline void
scratch_buffer_init (struct scratch_buffer *buffer)
{
  buffer->data = buffer->__space.__c;
  buffer->length = sizeof (buffer->__space);
}

/* Deallocates *BUFFER (if it was heap-allocated).  */
static inline void
scratch_buffer_free (struct scratch_buffer *buffer)
{
  if (buffer->data != buffer->__space.__c)
    free (buffer->data);
}

/* Grow *BUFFER by some arbitrary amount.  The buffer contents is NOT
   preserved.  Return true on success, false on allocation failure.  */
extern bool __libc_scratch_buffer_grow (struct scratch_buffer *buffer);

static inline bool
scratch_buffer_grow (struct scratch_buffer *buffer)
{
  return __libc_scratch_buffer_grow (buffer);
}

/* Like scratch_buffer_grow, but preserve the old buffer contents.  */
extern bool __libc_scratch_buffer_grow_preserve (struct scratch_buffer *buffer);

static inline bool
scratch_buffer_grow_preserve (struct scratch_buffer *buffer)
{
  return __libc_scratch_buffer_grow_preserve (buffer);
}

/* Grow *BUFFER so that it can store at least NELEM elements of SIZE bytes.  */
extern bool __libc_scratch_buffer_set_array_size (struct scratch_buffer *buffer,
                                                  size_t nelem, size_t size);

static inline bool
scratch_buffer_set_array_size (struct scratch_buffer *buffer,
                               size_t nelem, size_t size)
{
  return __libc_scratch_buffer_set_array_size (buffer, nelem, size);
}

#endif /* _GL_MALLOC_SCRATCH_BUFFER_H */
