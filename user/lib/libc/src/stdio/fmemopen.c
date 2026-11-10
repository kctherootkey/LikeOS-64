/*
 * fmemopen.c - fmemopen(3): a stream over a caller-supplied memory buffer
 *
 * Built on fopencookie().  The buffer has a fixed capacity (`size') and a
 * current content length: "r"/"r+" start with all `size' bytes as content,
 * "w"/"w+" with none (the buffer is truncated to an empty string), "a"/"a+"
 * with everything up to the first NUL.  Reads stop at the content length,
 * writes stop at the capacity (a write that does not fit is short, and one
 * with no room at all fails with ENOSPC), and in append mode every write goes
 * to the end of the content.  While there is room, the content is kept NUL
 * terminated, so a buffer written as text can be read back as a string.
 * Seeking is relative to the start, the position, or the content length,
 * and may not leave 0..size.
 *
 * With buf == NULL a zeroed buffer of `size' bytes is allocated and freed
 * again by fclose().
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct memfile {
	char *buf;
	size_t size; /* capacity */
	size_t end;  /* content length */
	size_t pos;  /* current position */
	int append;
	int owned;   /* buf was allocated here */
};

static ssize_t mem_read(void *cookie, char *out, size_t n)
{
	struct memfile *m = cookie;
	size_t avail = m->pos < m->end ? m->end - m->pos : 0;

	if (n > avail)
		n = avail;
	memcpy(out, m->buf + m->pos, n);
	m->pos += n;
	return (ssize_t)n;
}

static ssize_t mem_write(void *cookie, const char *in, size_t n)
{
	struct memfile *m = cookie;
	size_t room;

	if (m->append)
		m->pos = m->end;
	room = m->size - m->pos;
	if (n > room)
		n = room;
	if (n == 0) {
		errno = ENOSPC;
		return 0; /* the cookie convention for a failed write */
	}
	memcpy(m->buf + m->pos, in, n);
	m->pos += n;
	if (m->pos > m->end)
		m->end = m->pos;
	if (m->end < m->size)
		m->buf[m->end] = '\0';
	return (ssize_t)n;
}

static int mem_seek(void *cookie, off64_t *offset, int whence)
{
	struct memfile *m = cookie;
	off64_t base;

	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = (off64_t)m->pos;
		break;
	case SEEK_END:
		base = (off64_t)m->end;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if ((*offset < 0 && -*offset > base) ||
	    (*offset > 0 && *offset > (off64_t)m->size - base)) {
		errno = EINVAL;
		return -1;
	}
	m->pos = (size_t)(base + *offset);
	*offset = (off64_t)m->pos;
	return 0;
}

static int mem_close(void *cookie)
{
	struct memfile *m = cookie;

	if (m->owned)
		free(m->buf);
	free(m);
	return 0;
}

FILE *fmemopen(void *buf, size_t size, const char *mode)
{
	cookie_io_functions_t io = { mem_read, mem_write, mem_seek, mem_close };
	struct memfile *m;
	FILE *f;

	if (!mode || size == 0 ||
	    (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) {
		errno = EINVAL;
		return NULL;
	}

	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	if (buf) {
		m->buf = buf;
	} else {
		m->buf = calloc(1, size);
		if (!m->buf) {
			free(m);
			return NULL;
		}
		m->owned = 1;
	}
	m->size = size;

	switch (mode[0]) {
	case 'r':
		m->end = size;
		break;
	case 'w':
		m->buf[0] = '\0';
		m->end = 0;
		break;
	case 'a':
		m->append = 1;
		m->end = strnlen(m->buf, size);
		m->pos = m->end;
		break;
	}

	f = fopencookie(m, mode, io);
	if (!f) {
		mem_close(m);
		return NULL;
	}
	return f;
}
