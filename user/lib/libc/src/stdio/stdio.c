/*
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <limits.h>
#include <wchar.h> /* %ls / %lc convert through wcrtomb */

// Standard file streams - with buffering
static unsigned char __stdin_rbuf[BUFSIZ];
static unsigned char __stdout_wbuf[BUFSIZ];

static FILE __stdin = { .fd = 0,
			.buffer = __stdin_rbuf,
			.buf_size = BUFSIZ,
			.buf_pos = 0,
			.buf_end = 0,
			.wbuf = NULL,
			.wbuf_size = 0,
			.wbuf_pos = 0,
			.buf_mode = _IOFBF,
			.flags = 0,
			.eof = 0,
			.error = 0,
			.ungetc_buf = -1 };
static FILE __stdout = { .fd = 1,
			 .buffer = NULL,
			 .buf_size = 0,
			 .buf_pos = 0,
			 .buf_end = 0,
			 .wbuf = __stdout_wbuf,
			 .wbuf_size = BUFSIZ,
			 .wbuf_pos = 0,
			 .buf_mode = _IOLBF,
			 .flags = 0,
			 .eof = 0,
			 .error = 0,
			 .ungetc_buf = -1 };
static FILE __stderr = { .fd = 2,
			 .buffer = NULL,
			 .buf_size = 0,
			 .buf_pos = 0,
			 .buf_end = 0,
			 .wbuf = NULL,
			 .wbuf_size = 0,
			 .wbuf_pos = 0,
			 .buf_mode = _IONBF,
			 .flags = 0,
			 .eof = 0,
			 .error = 0,
			 .ungetc_buf = -1 };

FILE *stdin = &__stdin;
FILE *stdout = &__stdout;
FILE *stderr = &__stderr;

/* ------------------------------------------------------------------ */
/* Every stream that is open                                           */
/*                                                                     */
/* fflush(NULL) is defined to flush all of them, and exit() has to do  */
/* the same before the process goes -- otherwise anything a program    */
/* wrote to a stream it did not fclose is simply lost, silently, which  */
/* is the sort of thing that shows up much later as a truncated config */
/* or half-written cache file.  Only stdout and stderr were being      */
/* flushed.                                                            */
/*                                                                     */
/* Kept alongside the streams rather than as a link inside FILE: the   */
/* structure is shared with everything already compiled against this   */
/* library, and growing it would mean rebuilding all of it.            */
/* ------------------------------------------------------------------ */
static FILE **open_streams;
static size_t open_streams_len;
static size_t open_streams_cap;

/* A spin lock, for the same reason the exit handlers use one: this runs
 * before the threading machinery is up and after other threads may be gone. */
static volatile int streams_lock;

static void streams_lock_acquire(void)
{
	while (__atomic_exchange_n(&streams_lock, 1, __ATOMIC_ACQUIRE))
		__builtin_ia32_pause();
}

static void streams_lock_release(void)
{
	__atomic_store_n(&streams_lock, 0, __ATOMIC_RELEASE);
}

/* Best effort: a stream that cannot be recorded still works, it just will not
 * be flushed by fflush(NULL).  Failing the fopen instead would be worse. */
static int __memstream_publish(FILE *stream);

static void stream_register(FILE *fp)
{
	streams_lock_acquire();
	if (open_streams_len == open_streams_cap) {
		size_t ncap = open_streams_cap ? open_streams_cap * 2 : 8;
		FILE **n = realloc(open_streams, ncap * sizeof(*n));

		if (!n) {
			streams_lock_release();
			return;
		}
		open_streams = n;
		open_streams_cap = ncap;
	}
	open_streams[open_streams_len++] = fp;
	streams_lock_release();
}

static void stream_unregister(FILE *fp)
{
	streams_lock_acquire();
	for (size_t i = 0; i < open_streams_len; i++) {
		if (open_streams[i] == fp) {
			open_streams[i] = open_streams[--open_streams_len];
			break;
		}
	}
	streams_lock_release();
}

/* ------------------------------------------------------------------ */
/* The stream's backing I/O: its descriptor, or fopencookie()'s four   */
/* functions.  Every read, write, seek and close of a stream goes      */
/* through these.                                                      */
/* ------------------------------------------------------------------ */
static ssize_t __io_read(FILE *stream, void *buf, size_t n)
{
	if (stream->ck) {
		if (!stream->ck_io.read)
			return 0; /* no reader: always at end of file */
		return stream->ck_io.read(stream->ck_cookie, buf, n);
	}
	return read(stream->fd, buf, n);
}

static ssize_t __io_write(FILE *stream, const void *buf, size_t n)
{
	if (stream->ck) {
		ssize_t r;

		if (!stream->ck_io.write)
			return (ssize_t)n; /* no writer: output is discarded */
		r = stream->ck_io.write(stream->ck_cookie, buf, n);
		/* A cookie writer reports failure as 0; the callers here
		 * expect -1, and would otherwise retry forever. */
		if (r == 0 && n > 0) {
			if (errno == 0)
				errno = EIO;
			return -1;
		}
		return r;
	}
	return write(stream->fd, buf, n);
}

static off_t __io_seek(FILE *stream, off_t offset, int whence)
{
	if (stream->ck) {
		off64_t pos = offset;

		if (!stream->ck_io.seek) {
			errno = ESPIPE;
			return -1;
		}
		if (stream->ck_io.seek(stream->ck_cookie, &pos, whence) < 0)
			return -1;
		return (off_t)pos;
	}
	return lseek(stream->fd, offset, whence);
}

static int __io_close(FILE *stream)
{
	if (stream->ck)
		return stream->ck_io.close ?
			       stream->ck_io.close(stream->ck_cookie) :
			       0;
	return close(stream->fd);
}

FILE *fopen(const char *pathname, const char *mode)
{
	if (!pathname || !mode) {
		return NULL;
	}

	int flags = 0;

	// Parse mode string
	switch (mode[0]) {
	case 'r':
		flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
		break;
	case 'w':
		flags = O_CREAT | O_TRUNC |
			((mode[1] == '+') ? O_RDWR : O_WRONLY);
		break;
	case 'a':
		flags = O_CREAT | O_APPEND |
			((mode[1] == '+') ? O_RDWR : O_WRONLY);
		break;
	default:
		return NULL;
	}

	/* 0666, and it MUST be passed.
	 *
	 * "w" and "a" set O_CREAT above, and open() is variadic: it reads the
	 * creation mode with va_arg() whenever O_CREAT is present.  Calling it
	 * with two arguments therefore did not mean "no mode", it meant open()
	 * read whatever happened to be in the third argument register -- a
	 * different value per call site, the same value every time for any one
	 * of them.  Every file this system ever created through stdio got that
	 * junk as its permissions: Claws Mail's IMAP cache came out as
	 * -------x, which its own reader could not open again.
	 *
	 * 0666 is what POSIX specifies for fopen(); the kernel subtracts the
	 * caller's umask (creat_mode()), so this lands as 0644 under the usual
	 * 022 rather than being world-writable. */
	int fd = open(pathname, flags, 0666);
	if (fd < 0) {
		return NULL;
	}

	FILE *fp = malloc(sizeof(FILE));
	if (!fp) {
		close(fd);
		return NULL;
	}

	fp->fd = fd;
	fp->buffer = malloc(BUFSIZ); /* read buffer */
	fp->buf_size = fp->buffer ? BUFSIZ : 0;
	fp->buf_pos = 0;
	fp->buf_end = 0;
	fp->wbuf = malloc(BUFSIZ); /* write buffer */
	fp->wbuf_size = fp->wbuf ? BUFSIZ : 0;
	fp->wbuf_pos = 0;
	fp->buf_mode = _IOFBF; /* fully buffered by default */
	fp->flags = flags;
	fp->error = 0;
	fp->eof = 0;
	fp->ungetc_buf = -1;
	/* Orientation is decided by the first read or write, and the wide
	 * decoder starts in the initial shift state. */
	fp->wide_mode = 0;
	fp->wc_count = 0;
	fp->wc_value = 0;
	/* Not a memstream.  malloc'd, so every field must be set: a stale
	 * nonzero ms sent fclose() down the memstream path and realloc'd
	 * whatever wbuf happened to be. */
	fp->ms = 0;
	fp->ms_bufp = NULL;
	fp->ms_sizep = NULL;
	/* Not a cookie stream either, for the same reason. */
	fp->ck = 0;
	fp->ck_cookie = NULL;
	memset(&fp->ck_io, 0, sizeof(fp->ck_io));

	stream_register(fp);
	return fp;
}

int fclose(FILE *stream)
{
	if (!stream) {
		return EOF;
	}

	fflush(stream);
	stream_unregister(stream);
	if (stream->ms) {
		/* Final publish; the BUFFER survives -- it is the whole point
		 * of the stream, and the caller frees it. */
		__memstream_publish(stream);
		free(stream->buffer);
		free(stream);
		return 0;
	}
	int result = __io_close(stream);
	if (stream != stdin && stream != stdout && stream != stderr) {
		free(stream->buffer);
		free(stream->wbuf);
		free(stream);
	}
	return result == 0 ? 0 : EOF;
}

/* ------------------------------------------------------------------ */
/* Internal: flush write buffer to fd                                   */
/* ------------------------------------------------------------------ */
/* Publish a memstream's buffer to the caller's pointers.  Kept NUL
 * terminated past the reported size, per POSIX, and re-published on every
 * grow because realloc may move the block. */
static int __memstream_publish(FILE *stream)
{
	if (stream->wbuf_pos + 1 > stream->wbuf_size) {
		size_t ns = stream->wbuf_size ? stream->wbuf_size * 2 : 128;
		while (ns < stream->wbuf_pos + 1)
			ns *= 2;
		unsigned char *nb = realloc(stream->wbuf, ns);
		if (!nb) {
			stream->error = 1;
			return EOF;
		}
		stream->wbuf = nb;
		stream->wbuf_size = ns;
	}
	stream->wbuf[stream->wbuf_pos] = 0;
	*stream->ms_bufp = (char *)stream->wbuf;
	*stream->ms_sizep = stream->wbuf_pos;
	return 0;
}

static int __flush_wbuf(FILE *stream)
{
	if (!stream)
		return 0;
	/* A memstream never writes to a descriptor: "flushing" grows the
	 * buffer (so the writer that found it full can continue) and
	 * publishes it.  wbuf_pos is NOT reset -- it is the stream's size. */
	if (stream->ms) {
		/* Callers reach here for two different reasons and both must
		 * be satisfied: fwrite's buffered loop flushes when the buffer
		 * is FULL and needs space to make progress afterwards; a
		 * user's fflush() wants the current contents published.  Grow
		 * only when nearly full -- an unconditional doubling here made
		 * every fflush() inflate the buffer. */
		if (stream->wbuf_size - stream->wbuf_pos < 64) {
			size_t ns = stream->wbuf_size ? stream->wbuf_size * 2 :
						       128;
			unsigned char *nb = realloc(stream->wbuf, ns);
			if (!nb) {
				stream->error = 1;
				return EOF;
			}
			stream->wbuf = nb;
			stream->wbuf_size = ns;
		}
		return __memstream_publish(stream);
	}
	if (stream->wbuf_pos == 0)
		return 0;

	size_t remaining = stream->wbuf_pos;
	const unsigned char *p = stream->wbuf;
	while (remaining > 0) {
		ssize_t n = __io_write(stream, p, remaining);
		if (n < 0) {
			stream->error = 1;
			return EOF;
		}
		p += n;
		remaining -= n;
	}
	stream->wbuf_pos = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Internal: fill read buffer from fd                                   */
/* ------------------------------------------------------------------ */
static int __fill_rbuf(FILE *stream)
{
	if (!stream || !stream->buffer || stream->buf_size == 0)
		return -1;

	stream->buf_pos = 0;
	stream->buf_end = 0;
	ssize_t n = __io_read(stream, stream->buffer, stream->buf_size);
	if (n < 0) {
		stream->error = 1;
		return -1;
	}
	if (n == 0) {
		stream->eof = 1;
		return -1;
	}
	stream->buf_end = n;
	return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if (!stream || !ptr || size == 0 || nmemb == 0)
		return 0;

	size_t total = size * nmemb;
	unsigned char *dst = (unsigned char *)ptr;
	size_t got = 0;

	/* First: return any ungotten char */
	if (stream->ungetc_buf >= 0 && got < total) {
		*dst++ = (unsigned char)stream->ungetc_buf;
		stream->ungetc_buf = -1;
		got++;
	}

	/* Drain read buffer first */
	if (stream->buffer && stream->buf_pos < stream->buf_end) {
		size_t avail = stream->buf_end - stream->buf_pos;
		size_t take = (total - got < avail) ? total - got : avail;
		memcpy(dst, stream->buffer + stream->buf_pos, take);
		stream->buf_pos += take;
		dst += take;
		got += take;
	}

	/* If we still need data: for large reads, bypass buffer */
	if (got < total &&
	    (total - got) >= (stream->buf_size > 0 ? stream->buf_size : 1)) {
		ssize_t n = __io_read(stream, dst, total - got);
		if (n < 0) {
			stream->error = 1;
		} else if (n == 0) {
			stream->eof = 1;
		} else {
			got += n;
		}
		return got / size;
	}

	/* For small remaining reads, fill buffer and copy */
	while (got < total) {
		if (!stream->buffer || stream->buf_size == 0) {
			/* No read buffer — direct read */
			ssize_t n = __io_read(stream, dst, total - got);
			if (n < 0) {
				stream->error = 1;
				break;
			}
			if (n == 0) {
				stream->eof = 1;
				break;
			}
			got += n;
			break;
		}
		if (__fill_rbuf(stream) < 0)
			break;
		size_t avail = stream->buf_end - stream->buf_pos;
		size_t take = (total - got < avail) ? total - got : avail;
		memcpy(dst, stream->buffer + stream->buf_pos, take);
		stream->buf_pos += take;
		dst += take;
		got += take;
	}

	return got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if (!stream || !ptr || size == 0 || nmemb == 0)
		return 0;

	size_t total = size * nmemb;
	const unsigned char *src = (const unsigned char *)ptr;

	/* Unbuffered: write directly */
	if (stream->buf_mode == _IONBF || !stream->wbuf ||
	    stream->wbuf_size == 0) {
		ssize_t n = __io_write(stream, src, total);
		if (n < 0) {
			stream->error = 1;
			return 0;
		}
		return n / size;
	}

	/* Line buffered: copy to buffer, flush on '\n' or when full */
	if (stream->buf_mode == _IOLBF) {
		size_t written = 0;
		while (written < total) {
			/* Find next newline in remaining data */
			size_t remaining = total - written;
			const unsigned char *nl =
				memchr(src + written, '\n', remaining);
			size_t chunk;
			int do_flush = 0;

			if (nl) {
				chunk = (nl - (src + written)) +
					1; /* include the '\n' */
				do_flush = 1;
			} else {
				chunk = remaining;
			}

			/* Copy to buffer, flushing as needed */
			while (chunk > 0) {
				size_t space =
					stream->wbuf_size - stream->wbuf_pos;
				size_t copy = (chunk < space) ? chunk : space;
				memcpy(stream->wbuf + stream->wbuf_pos,
				       src + written, copy);
				stream->wbuf_pos += copy;
				written += copy;
				chunk -= copy;
				if (stream->wbuf_pos >= stream->wbuf_size) {
					if (__flush_wbuf(stream) == EOF)
						return written / size;
				}
			}
			if (do_flush) {
				if (__flush_wbuf(stream) == EOF)
					return written / size;
			}
		}
		return written / size;
	}

	/* Fully buffered: copy to buffer, flush when full */
	size_t written = 0;
	while (written < total) {
		size_t space = stream->wbuf_size - stream->wbuf_pos;
		size_t chunk = total - written;
		if (chunk <= space) {
			memcpy(stream->wbuf + stream->wbuf_pos, src + written,
			       chunk);
			stream->wbuf_pos += chunk;
			written += chunk;
		} else {
			/* Fill remainder of buffer and flush */
			memcpy(stream->wbuf + stream->wbuf_pos, src + written,
			       space);
			stream->wbuf_pos += space;
			written += space;
			if (__flush_wbuf(stream) == EOF)
				return written / size;
		}
	}
	return written / size;
}

int fgetc(FILE *stream)
{
	if (!stream)
		return EOF;

	/* Check ungetc buffer first */
	if (stream->ungetc_buf >= 0) {
		int c = stream->ungetc_buf;
		stream->ungetc_buf = -1;
		return c;
	}

	/* Read from buffer */
	if (stream->buffer && stream->buf_size > 0) {
		if (stream->buf_pos >= stream->buf_end) {
			if (__fill_rbuf(stream) < 0)
				return EOF;
		}
		return stream->buffer[stream->buf_pos++];
	}

	/* No read buffer — direct 1-byte read */
	unsigned char c;
	ssize_t n = __io_read(stream, &c, 1);
	if (n <= 0) {
		if (n == 0)
			stream->eof = 1;
		else
			stream->error = 1;
		return EOF;
	}
	return c;
}

int getchar(void)
{
	return fgetc(stdin);
}

char *fgets(char *s, int size, FILE *stream)
{
	if (!s || size <= 0 || !stream) {
		return NULL;
	}

	int i;
	for (i = 0; i < size - 1; i++) {
		int c = fgetc(stream);
		if (c == EOF) {
			if (i == 0) {
				return NULL;
			}
			break;
		}
		s[i] = c;
		if (c == '\n') {
			i++;
			break;
		}
	}
	s[i] = '\0';
	return s;
}

int fputc(int c, FILE *stream)
{
	unsigned char ch = c;
	if (fwrite(&ch, 1, 1, stream) != 1) {
		return EOF;
	}
	return (unsigned char)c;
}

int putchar(int c)
{
	return fputc(c, stdout);
}

int getc(FILE *stream)
{
	return fgetc(stream);
}

int putc(int c, FILE *stream)
{
	return fputc(c, stream);
}

FILE *fdopen(int fd, const char *mode)
{
	if (fd < 0 || !mode)
		return NULL;

	FILE *fp = malloc(sizeof(FILE));
	if (!fp)
		return NULL;

	int flags = 0;
	switch (mode[0]) {
	case 'r':
		flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
		break;
	case 'w':
		flags = O_CREAT | O_TRUNC |
			((mode[1] == '+') ? O_RDWR : O_WRONLY);
		break;
	case 'a':
		flags = O_CREAT | O_APPEND |
			((mode[1] == '+') ? O_RDWR : O_WRONLY);
		break;
	default:
		free(fp);
		return NULL;
	}

	fp->fd = fd;
	fp->buffer = malloc(BUFSIZ);
	fp->buf_size = fp->buffer ? BUFSIZ : 0;
	fp->buf_pos = 0;
	fp->buf_end = 0;
	fp->wbuf = malloc(BUFSIZ);
	fp->wbuf_size = fp->wbuf ? BUFSIZ : 0;
	fp->wbuf_pos = 0;
	fp->buf_mode = _IOFBF;
	fp->flags = flags;
	fp->error = 0;
	fp->eof = 0;
	fp->ungetc_buf = -1;
	/* Orientation is decided by the first read or write, and the wide
	 * decoder starts in the initial shift state. */
	fp->wide_mode = 0;
	fp->wc_count = 0;
	fp->wc_value = 0;
	/* Not a memstream.  malloc'd, so every field must be set: a stale
	 * nonzero ms sent fclose() down the memstream path and realloc'd
	 * whatever wbuf happened to be. */
	fp->ms = 0;
	fp->ms_bufp = NULL;
	fp->ms_sizep = NULL;
	/* Not a cookie stream either, for the same reason. */
	fp->ck = 0;
	fp->ck_cookie = NULL;
	memset(&fp->ck_io, 0, sizeof(fp->ck_io));

	stream_register(fp);
	return fp;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream)
{
	if (!mode || !stream)
		return NULL;

	/* Flush and close the underlying fd, but keep the FILE* alive */
	fflush(stream);
	__io_close(stream);
	stream->fd = -1;
	/* Whatever the stream was before, it is a descriptor stream now. */
	stream->ck = 0;
	stream->ck_cookie = NULL;

	int flags = 0;
	switch (mode[0]) {
	case 'r':
		flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
		break;
	case 'w':
		flags = O_CREAT | O_TRUNC |
			((mode[1] == '+') ? O_RDWR : O_WRONLY);
		break;
	case 'a':
		flags = O_CREAT | O_APPEND |
			((mode[1] == '+') ? O_RDWR : O_WRONLY);
		break;
	default:
		return NULL;
	}

	/* 0666 for the same reason fopen() passes it -- see the comment there.
	 * O_CREAT without a mode argument makes open() read a garbage
	 * register. */
	int fd = open(pathname, flags, 0666);
	if (fd < 0)
		return NULL;

	stream->fd = fd;
	stream->flags = flags;
	stream->buf_pos = 0;
	stream->buf_end = 0;
	stream->wbuf_pos = 0;
	stream->error = 0;
	stream->eof = 0;
	stream->ungetc_buf = -1;
	return stream;
}

int fputs(const char *s, FILE *stream)
{
	size_t len = strlen(s);
	if (fwrite(s, 1, len, stream) != len) {
		return EOF;
	}
	return 0;
}

int puts(const char *s)
{
	if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF) {
		return EOF;
	}
	return 0;
}

int fflush(FILE *stream)
{
	if (!stream) {
		/* Every open stream, as the standard says -- not just stdout.
		 * Anything a program wrote and did not fclose used to be
		 * discarded here without a word. */
		int ret = 0;

		if (__flush_wbuf(stdout) == EOF)
			ret = EOF;
		if (__flush_wbuf(stderr) == EOF)
			ret = EOF;

		streams_lock_acquire();
		for (size_t i = 0; i < open_streams_len; i++) {
			FILE *fp = open_streams[i];

			/* The lock is held across the writes.  Registration and
			 * removal only touch the array, so this cannot deadlock
			 * against them -- and dropping it to walk a list another
			 * thread may be reshuffling would be worse. */
			if (fp && __flush_wbuf(fp) == EOF)
				ret = EOF;
		}
		streams_lock_release();
		return ret;
	}
	return __flush_wbuf(stream);
}

/* fopencookie(): a buffered stream whose I/O is done by the caller's four
 * functions (see <stdio.h> for their contract).  The mode string decides
 * only what the stream may do -- "r" read, "w"/"a" write, '+' both; what
 * reading, writing and positioning mean is entirely the functions' business,
 * appending included. */
FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io_funcs)
{
	int flags;

	if (!mode) {
		errno = EINVAL;
		return NULL;
	}
	switch (mode[0]) {
	case 'r':
		flags = strchr(mode, '+') ? O_RDWR : O_RDONLY;
		break;
	case 'w':
		flags = strchr(mode, '+') ? O_RDWR : O_WRONLY;
		break;
	case 'a':
		flags = O_APPEND | (strchr(mode, '+') ? O_RDWR : O_WRONLY);
		break;
	default:
		errno = EINVAL;
		return NULL;
	}

	FILE *fp = calloc(1, sizeof(FILE));
	if (!fp)
		return NULL;
	fp->fd = -1; /* no descriptor behind it */
	fp->buffer = malloc(BUFSIZ);
	fp->buf_size = fp->buffer ? BUFSIZ : 0;
	fp->wbuf = malloc(BUFSIZ);
	fp->wbuf_size = fp->wbuf ? BUFSIZ : 0;
	fp->buf_mode = _IOFBF;
	fp->flags = flags;
	fp->ungetc_buf = -1;
	fp->ck = 1;
	fp->ck_cookie = cookie;
	fp->ck_io = io_funcs;
	stream_register(fp);
	return fp;
}

FILE *open_memstream(char **bufp, size_t *sizep)
{
	if (!bufp || !sizep) {
		errno = EINVAL;
		return NULL;
	}
	FILE *f = calloc(1, sizeof(FILE));
	if (!f)
		return NULL;
	f->fd = -1; /* no descriptor behind it, ever */
	f->wbuf = malloc(128);
	if (!f->wbuf) {
		free(f);
		return NULL;
	}
	f->wbuf_size = 128;
	f->wbuf_pos = 0;
	f->buf_mode = _IOFBF;
	f->ungetc_buf = -1;
	f->ms = 1;
	f->ms_bufp = bufp;
	f->ms_sizep = sizep;
	f->wbuf[0] = 0;
	*bufp = (char *)f->wbuf;
	*sizep = 0;
	stream_register(f);
	return f;
}

int fseek(FILE *stream, long offset, int whence)
{
	if (stream && stream->ms) {
		/* Position IS wbuf_pos for a memstream.  Seeking past the end
		 * zero-fills the gap, per POSIX; the published size follows
		 * the high-water mark at the next flush/close. */
		long base = 0;
		if (whence == SEEK_CUR)
			base = (long)stream->wbuf_pos;
		else if (whence == SEEK_END)
			base = (long)stream->wbuf_pos; /* size == pos here */
		long np = base + offset;
		if (np < 0) {
			errno = EINVAL;
			return -1;
		}
		while ((size_t)np + 1 > stream->wbuf_size) {
			size_t ns = stream->wbuf_size * 2;
			unsigned char *nb = realloc(stream->wbuf, ns);
			if (!nb)
				return -1;
			stream->wbuf = nb;
			stream->wbuf_size = ns;
		}
		if ((size_t)np > stream->wbuf_pos)
			memset(stream->wbuf + stream->wbuf_pos, 0,
			       (size_t)np - stream->wbuf_pos);
		stream->wbuf_pos = (size_t)np;
		return 0;
	}
	if (!stream) {
		return -1;
	}

	/* Flush any pending writes */
	__flush_wbuf(stream);

	/* Invalidate read buffer */
	stream->buf_pos = 0;
	stream->buf_end = 0;
	stream->ungetc_buf = -1;

	/* Clear EOF flag on seek */
	stream->eof = 0;

	off_t result = __io_seek(stream, offset, whence);
	if (result < 0) {
		stream->error = 1;
		return -1;
	}
	return 0;
}

long ftell(FILE *stream)
{
	if (stream && stream->ms)
		return (long)stream->wbuf_pos;
	if (!stream) {
		return -1;
	}

	/* Flush any pending writes so the fd position is up to date */
	__flush_wbuf(stream);

	long pos = (long)__io_seek(stream, 0, SEEK_CUR);
	if (pos < 0)
		return -1;

	/* Subtract unread data still sitting in the read buffer.
     * The kernel fd position is *ahead* of what the user has consumed. */
	if (stream->buffer && stream->buf_end > stream->buf_pos)
		pos -= (long)(stream->buf_end - stream->buf_pos);

	/* Account for ungetc'd character */
	if (stream->ungetc_buf >= 0)
		pos--;

	return pos;
}

void rewind(FILE *stream)
{
	fseek(stream, 0, SEEK_SET);
}

int feof(FILE *stream)
{
	return stream ? stream->eof : 0;
}

int ferror(FILE *stream)
{
	return stream ? stream->error : 0;
}

void clearerr(FILE *stream)
{
	if (stream) {
		stream->eof = 0;
		stream->error = 0;
	}
}

int ungetc(int c, FILE *stream)
{
	if (!stream || c == EOF)
		return EOF;
	stream->ungetc_buf = (unsigned char)c;
	stream->eof = 0;
	return (unsigned char)c;
}

int fileno(FILE *stream)
{
	if (!stream)
		return -1;
	return stream->fd;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
	if (!stream)
		return -1;
	if (mode != _IONBF && mode != _IOLBF && mode != _IOFBF)
		return -1;

	/* Flush any pending output first */
	__flush_wbuf(stream);

	stream->buf_mode = mode;

	if (mode == _IONBF) {
		/* Unbuffered: no write buffer needed */
		return 0;
	}

	/* If user provides a buffer, we can't really use it for both r+w,
       so just set the mode and keep our own buffers */
	(void)buf;
	(void)size;
	return 0;
}

void setbuf(FILE *stream, char *buf)
{
	setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

void setlinebuf(FILE *stream)
{
	setvbuf(stream, NULL, _IOLBF, BUFSIZ);
}

/*
 * Digits of a magnitude, in the given base.  UNSIGNED, and it emits no sign:
 * the caller decides that, because only the caller knows whether the
 * conversion is a signed one.
 *
 * It used to take a `long long` and prefix a '-' of its own whenever the value
 * came out negative in base 10, which was wrong for both kinds of caller and
 * for the same reason -- a 64-bit magnitude does not fit in a signed 64-bit
 * parameter:
 *
 *   - %u, %lu, %llu and %zu read an unsigned value correctly and then cast it
 *     to pass it here.  Anything with the top bit set arrived negative, so
 *     printf("%lu", (unsigned long)-1) produced "-1" instead of
 *     18446744073709551615, and every size, count and hash above 2^63 printed
 *     as a small negative number.
 *
 *   - %d and %lld already reduce their argument to a magnitude and print the
 *     sign themselves.  For LLONG_MIN that magnitude is 2^63, which read back
 *     as negative here and picked up a SECOND '-': printf("%lld", LLONG_MIN)
 *     gave "--9223372036854775808", one character longer than the value it was
 *     asked to print.
 *
 * Hex and octal were unaffected only because the sign branch was gated on
 * base 10, which is what kept %p and %x honest while %lu was not.
 */
static int num_to_str(unsigned long long num, char *buf, int base,
		      int uppercase)
{
	const char *digits =
		uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
	char temp[64];
	int i = 0;

	if (num == 0) {
		buf[0] = '0';
		buf[1] = '\0';
		return 1;
	}

	while (num > 0) {
		temp[i++] = digits[num % (unsigned)base];
		num /= (unsigned)base;
	}

	int len = 0;
	while (i > 0) {
		buf[len++] = temp[--i];
	}
	buf[len] = '\0';

	return len;
}

/*
 * Floating-point conversions: %f %F %e %E %g %G, double and long double.
 *
 * The value is taken apart into its bits -- sign, an integer mantissa m and
 * a binary exponent e2, value = m * 2^e2 -- and expanded into decimal with
 * integer arithmetic only, in limbs of nine decimal digits:
 *
 *   integer part   I = m * 2^e2 (e2 >= 0), or m >> -e2
 *   fraction       r / 2^k = r * 5^k / 10^k, so its k digits are those of
 *                  D = r * 5^k, zero-filled on the left to k places
 *
 * Every digit printed is then the true digit of the value, whatever its
 * magnitude and whatever the precision, and the rounding at the last printed
 * place is exact: to nearest, a tie to the even digit.
 *
 * The conversions this replaces scaled the value through an unsigned long
 * long.  Anything from 2^64 up converted out of range -- "%.0f" of 1e20
 * printed "1" -- the digits of %e came from repeated division by ten and
 * drifted in the last places, precisions above 19 overflowed the power of
 * ten, and a long double was narrowed to double first, so "%.0Lf" of
 * 2^63 - 1 printed 9223372036854775808.  Infinities and NaNs were run
 * through the same arithmetic.
 */

/* Limbs for the longest expansion: the fraction of the smallest long double
 * (k = 16445, r < 2^64: at most 11514 digits) or the integer part of the
 * largest (4933 digits). */
#define FPD_BASE 1000000000u
#define FPD_LIMBS 1284

static const unsigned int fpd_p10[10] = { 1,	   10,	    100,     1000,
					  10000,   100000,  1000000, 10000000,
					  100000000, 1000000000 };

enum { FP_ZERO, FP_FINITE, FP_INF, FP_NAN };

struct fp_val {
	int neg; /* sign bit (-0.0 and -nan included) */
	int cls;
	unsigned long long m;
	int e2;
};

static void fp_decode_dbl(struct fp_val *v, double d)
{
	unsigned long long b;
	int ex;

	memcpy(&b, &d, sizeof(b));
	v->neg = (int)(b >> 63);
	ex = (int)((b >> 52) & 0x7ff);
	v->m = b & ((1ULL << 52) - 1);
	if (ex == 0x7ff) {
		v->cls = v->m ? FP_NAN : FP_INF;
		return;
	}
	if (ex == 0) {
		v->cls = v->m ? FP_FINITE : FP_ZERO;
		v->e2 = 1 - 1075;
		return;
	}
	v->cls = FP_FINITE;
	v->m |= 1ULL << 52;
	v->e2 = ex - 1075;
}

/* x86-64 long double: 64-bit mantissa with an explicit integer bit, then
 * sign and a 15-bit exponent. */
static void fp_decode_ldbl(struct fp_val *v, long double ld)
{
	struct {
		unsigned long long m;
		unsigned short se;
	} b;
	int ex;

	memcpy(&b, &ld, 10);
	v->neg = b.se >> 15;
	ex = b.se & 0x7fff;
	v->m = b.m;
	if (ex == 0x7fff) {
		v->cls = (b.m << 1) ? FP_NAN : FP_INF;
		return;
	}
	v->cls = b.m ? FP_FINITE : FP_ZERO;
	v->e2 = (ex ? ex : 1) - 16383 - 63;
}

struct fpd {
	unsigned int big[FPD_LIMBS]; /* I when e2 >= 0, else D */
	unsigned int small[3]; /* I when e2 < 0 (below 2^64) */
	const unsigned int *il; /* integer part, least significant first */
	int ni;
	const unsigned int *fl; /* D, least significant first */
	int nf;
	int k; /* digits of the fraction */
	int msd; /* weight of the leading digit (0 for zero) */
};

static int fpd_set(unsigned int *l, unsigned long long v)
{
	int n = 0;

	while (v) {
		l[n++] = (unsigned int)(v % FPD_BASE);
		v /= FPD_BASE;
	}
	return n;
}

/* l *= f, f < 2^32 */
static int fpd_mul(unsigned int *l, int n, unsigned int f)
{
	unsigned long long c = 0;

	for (int i = 0; i < n; i++) {
		c += (unsigned long long)l[i] * f;
		l[i] = (unsigned int)(c % FPD_BASE);
		c /= FPD_BASE;
	}
	while (c && n < FPD_LIMBS) {
		l[n++] = (unsigned int)(c % FPD_BASE);
		c /= FPD_BASE;
	}
	return n;
}

static int fpd_ndig(const unsigned int *l, int n)
{
	int d = 0;

	if (!n)
		return 0;
	while (d < 9 && l[n - 1] >= fpd_p10[d])
		d++;
	return 9 * (n - 1) + d;
}

static void fpd_expand(struct fpd *d, const struct fp_val *v)
{
	unsigned long long m = v->m, r;
	int k;

	d->il = d->fl = d->big;
	d->ni = d->nf = d->k = 0;
	if (v->cls != FP_FINITE) {
		d->msd = 0;
		return;
	}
	if (v->e2 >= 0) {
		int e = v->e2;

		d->ni = fpd_set(d->big, m);
		for (; e > 0; e -= 29)
			d->ni = fpd_mul(d->big, d->ni,
					1u << (e > 29 ? 29 : e));
		d->msd = fpd_ndig(d->il, d->ni) - 1;
		return;
	}
	k = -v->e2;
	if (k < 64) {
		d->il = d->small;
		d->ni = fpd_set(d->small, m >> k);
		r = m & ((1ULL << k) - 1);
	} else {
		r = m;
	}
	if (r) {
		int t = __builtin_ctzll(r);

		r >>= t;
		k -= t;
		d->k = k;
		d->nf = fpd_set(d->big, r);
		for (; k > 0; k -= 13) {
			unsigned int f = 1;

			for (int i = 0; i < (k > 13 ? 13 : k); i++)
				f *= 5;
			d->nf = fpd_mul(d->big, d->nf, f);
		}
	}
	if (d->ni)
		d->msd = fpd_ndig(d->il, d->ni) - 1;
	else
		d->msd = fpd_ndig(d->fl, d->nf) - d->k - 1;
}

static int fpd_ldig(const unsigned int *l, int n, int p)
{
	if (p < 0 || p / 9 >= n)
		return 0;
	return (int)(l[p / 9] / fpd_p10[p % 9] % 10);
}

/* The digit of weight 10^w. */
static int fpd_digit(const struct fpd *d, int w)
{
	return w >= 0 ? fpd_ldig(d->il, d->ni, w) :
			fpd_ldig(d->fl, d->nf, d->k + w);
}

/* Any nonzero digit among positions [0, lim). */
static int fpd_nz_low(const unsigned int *l, int n, int lim)
{
	int full;

	if (lim <= 0)
		return 0;
	full = lim / 9;
	for (int i = 0; i < full && i < n; i++)
		if (l[i])
			return 1;
	return full < n && l[full] % fpd_p10[lim % 9] != 0;
}

/* Any nonzero digit of weight below 10^w. */
static int fpd_nz_below(const struct fpd *d, int w)
{
	if (w > 0 && fpd_nz_low(d->il, d->ni, w))
		return 1;
	return fpd_nz_low(d->fl, d->nf, d->k + w);
}

/* Rounding at the digit of weight 10^cut: to nearest, a tie to even.  On a
 * round up the digit of weight w0 grows by one and those below it, down to
 * the cut, become 0. */
struct fpr {
	int cut;
	int up;
	int w0;
};

static void fpd_round(const struct fpd *d, int cut, struct fpr *r)
{
	int rd = fpd_digit(d, cut - 1);

	r->cut = cut;
	r->up = rd > 5 || (rd == 5 && (fpd_nz_below(d, cut - 1) ||
				       (fpd_digit(d, cut) & 1)));
	r->w0 = cut;
	if (r->up)
		while (fpd_digit(d, r->w0) == 9)
			r->w0++;
}

static int fpd_rdigit(const struct fpd *d, const struct fpr *r, int w)
{
	if (w < r->cut)
		return 0;
	if (!r->up || w > r->w0)
		return fpd_digit(d, w);
	return w == r->w0 ? fpd_digit(d, w) + 1 : 0;
}

/* The leading digit's weight after rounding. */
static int fpd_top(const struct fpd *d, const struct fpr *r)
{
	return r->up && r->w0 > d->msd ? r->w0 : d->msd;
}

/* Format one value; returns the new output position (every character is
 * counted, those past `cap' are not stored -- vsnprintf_body's contract). */
static __attribute__((noinline)) size_t fp_format(char *str, size_t cap, size_t pos,
			const struct fp_val *v, char conv, int width, int prec,
			int fl_minus, int fl_zero, int fl_plus, int fl_space,
			int fl_hash)
{
	struct fpd d;
	struct fpr r;
	int upper = conv == 'F' || conv == 'E' || conv == 'G';
	char sign = v->neg ? '-' : fl_plus ? '+' : fl_space ? ' ' : 0;
	int hi, nint, nfrac, point, elen = 0, total, pad;
	char ebuf[8];

#define FP_PUT(c)                         \
	do {                              \
		char fp_c_ = (char)(c);   \
		if (pos < cap)            \
			str[pos] = fp_c_; \
		pos++;                    \
	} while (0)

	if (v->cls == FP_INF || v->cls == FP_NAN) {
		const char *t = v->cls == FP_INF ? (upper ? "INF" : "inf") :
						   (upper ? "NAN" : "nan");

		total = 3 + (sign ? 1 : 0);
		pad = width > total ? width - total : 0;
		if (!fl_minus)
			for (int p = 0; p < pad; p++)
				FP_PUT(' ');
		if (sign)
			FP_PUT(sign);
		for (int i = 0; i < 3; i++)
			FP_PUT(t[i]);
		if (fl_minus)
			for (int p = 0; p < pad; p++)
				FP_PUT(' ');
		return pos;
	}

	fpd_expand(&d, v);
	if (prec < 0)
		prec = 6;
	if (conv == 'g' || conv == 'G') {
		int x;

		if (prec == 0)
			prec = 1;
		/* The exponent %e would print with prec - 1 digits after the
		 * point, after its rounding. */
		fpd_round(&d, d.msd - (prec - 1), &r);
		x = fpd_top(&d, &r);
		if (x < prec && x >= -4) {
			conv = 'f';
			prec = prec - 1 - x;
			fpd_round(&d, -prec, &r);
		} else {
			conv = 'e';
			prec = prec - 1;
		}
		/* Trailing zeros of the fraction go, unless '#'. */
		if (!fl_hash) {
			int low = conv == 'f' ? -prec : fpd_top(&d, &r) - prec;

			while (prec > 0 && fpd_rdigit(&d, &r, low) == 0) {
				prec--;
				low++;
			}
		}
	} else if (conv == 'e' || conv == 'E') {
		conv = 'e';
		fpd_round(&d, d.msd - prec, &r);
	} else {
		conv = 'f';
		fpd_round(&d, -prec, &r);
	}

	if (conv == 'f') {
		hi = fpd_top(&d, &r);
		if (hi < 0)
			hi = 0;
		nint = hi + 1;
	} else {
		int x = fpd_top(&d, &r), ax = x < 0 ? -x : x, n = 0;
		char t[8];

		hi = x;
		nint = 1;
		ebuf[elen++] = upper ? 'E' : 'e';
		ebuf[elen++] = x < 0 ? '-' : '+';
		do {
			t[n++] = (char)('0' + ax % 10);
			ax /= 10;
		} while (ax);
		if (n < 2)
			t[n++] = '0';
		while (n)
			ebuf[elen++] = t[--n];
	}
	nfrac = prec;
	point = nfrac > 0 || fl_hash;

	total = (sign ? 1 : 0) + nint + point + nfrac + elen;
	pad = width > total ? width - total : 0;
	if (!fl_minus && !fl_zero)
		for (int p = 0; p < pad; p++)
			FP_PUT(' ');
	if (sign)
		FP_PUT(sign);
	if (!fl_minus && fl_zero)
		for (int p = 0; p < pad; p++)
			FP_PUT('0');
	for (int i = 0; i < nint; i++)
		FP_PUT('0' + fpd_rdigit(&d, &r, hi - i));
	if (point)
		FP_PUT('.');
	for (int i = 0; i < nfrac; i++)
		FP_PUT('0' + fpd_rdigit(&d, &r, hi - nint - i));
	for (int i = 0; i < elen; i++)
		FP_PUT(ebuf[i]);
	if (fl_minus)
		for (int p = 0; p < pad; p++)
			FP_PUT(' ');
#undef FP_PUT
	return pos;
}

/* Store one character if it fits, and count it either way.
 *
 * The argument is evaluated EXACTLY ONCE, and unconditionally.  Call sites pass
 * expressions with side effects -- SP_PUT(*format++) is the main loop's way of
 * consuming a literal character -- so evaluating it only when the character
 * fits would stop advancing the format string as soon as the buffer filled,
 * and the loop would never end. */
#define SP_PUT(c)                                  \
	do {                                       \
		char sp_c_ = (char)(c);            \
		if (pos < cap)                     \
			str[pos] = sp_c_;          \
		pos++;                             \
	} while (0)

/* The formatter.
 *
 * `pos` counts every character the format PRODUCES, whether or not it fits.
 * Only the first `size - 1` are stored.  That distinction is the whole of C99's
 * contract for snprintf: the return value is the length the output would have
 * had, which is what lets a caller detect truncation and size a buffer.
 *
 * It used to return the truncated length instead, so
 *     if (snprintf(buf, sizeof buf, ...) >= (int)sizeof buf)
 * -- the standard truncation check -- could never fire, and a caller sizing a
 * buffer from the return value silently under-allocated. */
static int vsnprintf_body(char *str, size_t size, const char *format,
			  va_list ap)
{
	size_t pos = 0;
	const size_t cap = size - 1; /* size >= 1 is guaranteed by the caller */

	while (*format) {
		if (*format != '%') {
			SP_PUT(*format++);
			continue;
		}
		format++; /* skip '%' */

		/* %% */
		if (*format == '%') {
			SP_PUT('%');
			format++;
			continue;
		}

		/* --- Parse flags --- */
		int fl_minus = 0, fl_zero = 0, fl_plus = 0, fl_space = 0,
		    fl_hash = 0;
		for (;;) {
			if (*format == '-') {
				fl_minus = 1;
				format++;
			} else if (*format == '0') {
				fl_zero = 1;
				format++;
			} else if (*format == '+') {
				fl_plus = 1;
				format++;
			} else if (*format == ' ') {
				fl_space = 1;
				format++;
			} else if (*format == '#') {
				fl_hash = 1;
				format++;
			} else if (*format == '\'') {
				/* Group the digits of a decimal conversion by
				 * the locale's thousands separator.  Accepted
				 * and ignored: this is the C locale, whose
				 * grouping is "none", so the correct output is
				 * the ungrouped number.
				 *
				 * Ignoring it is not the same as not knowing
				 * it.  An unrecognised character here ended
				 * the flag loop, and the conversion that
				 * followed was not one -- so "%'llu" printed
				 * the literal text 'llu, which is what a file
				 * manager's properties window showed instead
				 * of a byte count. */
				format++;
			} else
				break;
		}
		if (fl_minus)
			fl_zero = 0;

		/* --- Width --- */
		int width = 0;
		if (*format == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				fl_minus = 1;
				width = -width;
			}
			format++;
		} else {
			while (*format >= '0' && *format <= '9') {
				width = width * 10 + (*format - '0');
				format++;
			}
		}

		/* --- Precision --- */
		int prec = -1;
		if (*format == '.') {
			format++;
			prec = 0;
			if (*format == '*') {
				prec = va_arg(ap, int);
				if (prec < 0)
					prec = -1;
				format++;
			} else {
				while (*format >= '0' && *format <= '9') {
					prec = prec * 10 + (*format - '0');
					format++;
				}
			}
		}

		/* --- Length modifiers --- */
		int lng = 0; /* 1 = long, 2 = long long */
		/* 1 = h (short), 2 = hh (char).  Default argument promotion
		 * passes both as int, so the value has to be narrowed back
		 * here.  The modifiers were parsed and then ignored, so
		 * printf("%hhd", 300) printed 300 instead of 44. */
		int sht = 0;
		int ldbl = 0; /* 'L' = long double argument */
		if (*format == 'l') {
			format++;
			lng = 1;
			if (*format == 'l') {
				format++;
				lng = 2;
			}
		} else if (*format == 'z' || *format == 'j') {
			format++;
			lng = 2;
		} else if (*format == 'h') {
			format++;
			sht = 1;
			if (*format == 'h') {
				format++;
				sht = 2;
			}
		} else if (*format == 'L') {
			/* long double.  It MUST be consumed as a long double
			 * (16 bytes here, not 8) or every later argument is
			 * read from the wrong offset; it is formatted with
			 * its full precision (fp_format). */
			format++;
			ldbl = 1;
		}

		/* --- Specifier --- */
		char buf[64];
		int len = 0;

		switch (*format) {
		/* ---- signed integer ---- */
		case 'd':
		case 'i': {
			long long val;
			if (lng == 2)
				val = va_arg(ap, long long);
			else if (lng == 1)
				val = va_arg(ap, long);
			else
				val = va_arg(ap, int);
			if (sht == 1)
				val = (short)val;
			else if (sht == 2)
				val = (signed char)val;

			char sign = 0;
			unsigned long long uval;
			if (val < 0) {
				sign = '-';
				uval = (unsigned long long)(-(val + 1)) + 1;
			} else {
				uval = (unsigned long long)val;
				if (fl_plus)
					sign = '+';
				else if (fl_space)
					sign = ' ';
			}

			len = num_to_str(uval, buf, 10, 0);

			/* C99 integer precision: at least `prec` digits,
			 * zero-filled on the left.  A precision of 0 with a
			 * value of 0 produces NO digits at all, and an explicit
			 * precision makes the '0' flag inoperative.  None of
			 * this was implemented -- prec was read and then used
			 * only by %s and the float conversions. */
			int zeros = 0;
			if (prec >= 0) {
				if (prec == 0 && uval == 0)
					len = 0;
				if (prec > len)
					zeros = prec - len;
			}
			int zpad = fl_zero && prec < 0;

			int total = len + zeros + (sign ? 1 : 0);
			int pad = (width > total) ? width - total : 0;

			if (!fl_minus && !zpad)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');
			if (sign)
				SP_PUT(sign);
			if (!fl_minus && zpad)
				for (int p = 0; p < pad; p++)
					SP_PUT('0');
			for (int z = 0; z < zeros; z++)
				SP_PUT('0');
			for (int i = 0; i < len; i++)
				SP_PUT(buf[i]);
			if (fl_minus)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');

			format++;
			continue;
		}

		/* ---- unsigned integer ---- */
		case 'u': {
			unsigned long long val;
			if (lng == 2)
				val = va_arg(ap, unsigned long long);
			else if (lng == 1)
				val = va_arg(ap, unsigned long);
			else
				val = va_arg(ap, unsigned int);
			if (sht == 1)
				val = (unsigned short)val;
			else if (sht == 2)
				val = (unsigned char)val;

			len = num_to_str(val, buf, 10, 0);

			/* C99 integer precision: at least `prec` digits,
			 * zero-filled on the left.  A precision of 0 with a
			 * value of 0 produces NO digits at all, and an explicit
			 * precision makes the '0' flag inoperative.  None of
			 * this was implemented -- prec was read and then used
			 * only by %s and the float conversions. */
			int zeros = 0;
			if (prec >= 0) {
				if (prec == 0 && val == 0)
					len = 0;
				if (prec > len)
					zeros = prec - len;
			}
			int zpad = fl_zero && prec < 0;

			int total = len + zeros;
			int pad = (width > total) ? width - total : 0;
			if (!fl_minus) {
				char pc = zpad ? '0' : ' ';
				for (int p = 0; p < pad; p++)
					SP_PUT(pc);
			}
			for (int z = 0; z < zeros; z++)
				SP_PUT('0');
			for (int i = 0; i < len; i++)
				SP_PUT(buf[i]);
			if (fl_minus)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');

			format++;
			continue;
		}

		/* ---- hex ---- */
		case 'x':
		case 'X': {
			unsigned long long val;
			if (lng == 2)
				val = va_arg(ap, unsigned long long);
			else if (lng == 1)
				val = va_arg(ap, unsigned long);
			else
				val = va_arg(ap, unsigned int);
			if (sht == 1)
				val = (unsigned short)val;
			else if (sht == 2)
				val = (unsigned char)val;

			len = num_to_str(val, buf, 16,
					 *format == 'X');
			/* C99 integer precision: at least `prec` digits,
			 * zero-filled on the left.  A precision of 0 with a
			 * value of 0 produces NO digits at all, and an explicit
			 * precision makes the '0' flag inoperative.  None of
			 * this was implemented -- prec was read and then used
			 * only by %s and the float conversions. */
			int zeros = 0;
			if (prec >= 0) {
				if (prec == 0 && val == 0)
					len = 0;
				if (prec > len)
					zeros = prec - len;
			}
			int zpad = fl_zero && prec < 0;

			int pfx = (fl_hash && val != 0) ? 2 : 0;
			int total = len + zeros + pfx;
			int pad = (width > total) ? width - total : 0;

			if (!fl_minus) {
				if (zpad) {
					if (pfx) {
						SP_PUT('0');
						SP_PUT((*format == 'X' ? 'X' : 'x'));
					}
					for (int p = 0;
					     p < pad; p++)
						SP_PUT('0');
				} else {
					for (int p = 0;
					     p < pad; p++)
						SP_PUT(' ');
					if (pfx) {
						SP_PUT('0');
						SP_PUT((*format == 'X' ? 'X' : 'x'));
					}
				}
			} else {
				if (pfx) {
					SP_PUT('0');
					SP_PUT((*format == 'X' ? 'X' : 'x'));
				}
			}
			for (int z = 0; z < zeros; z++)
				SP_PUT('0');
			for (int i = 0; i < len; i++)
				SP_PUT(buf[i]);
			if (fl_minus)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');

			format++;
			continue;
		}

		/* ---- octal ---- */
		case 'o': {
			unsigned long long val;
			if (lng == 2)
				val = va_arg(ap, unsigned long long);
			else if (lng == 1)
				val = va_arg(ap, unsigned long);
			else
				val = va_arg(ap, unsigned int);
			if (sht == 1)
				val = (unsigned short)val;
			else if (sht == 2)
				val = (unsigned char)val;

			len = num_to_str(val, buf, 8, 0);

			/* C99 integer precision: at least `prec` digits,
			 * zero-filled on the left.  A precision of 0 with a
			 * value of 0 produces NO digits at all, and an explicit
			 * precision makes the '0' flag inoperative.  None of
			 * this was implemented -- prec was read and then used
			 * only by %s and the float conversions. */
			int zeros = 0;
			if (prec >= 0) {
				if (prec == 0 && val == 0)
					len = 0;
				if (prec > len)
					zeros = prec - len;
			}
			/* '#' forces the first digit to be 0, which C99 states
			 * as "increase the precision if and only if necessary".
			 * It was not implemented at all. */
			if (fl_hash && zeros == 0 && (len == 0 || buf[0] != '0'))
				zeros = 1;
			int zpad = fl_zero && prec < 0;

			int total = len + zeros;
			int pad = (width > total) ? width - total : 0;
			if (!fl_minus) {
				char pc = zpad ? '0' : ' ';
				for (int p = 0; p < pad; p++)
					SP_PUT(pc);
			}
			for (int z = 0; z < zeros; z++)
				SP_PUT('0');
			for (int i = 0; i < len; i++)
				SP_PUT(buf[i]);
			if (fl_minus)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');

			format++;
			continue;
		}

		/* ---- pointer ---- */
		case 'p': {
			void *ptr = va_arg(ap, void *);
			SP_PUT('0');
			SP_PUT('x');
			len = num_to_str((unsigned long long)(unsigned long)ptr,
					 buf, 16, 0);
			for (int i = 0; i < len; i++)
				SP_PUT(buf[i]);
			format++;
			continue;
		}

		/* ---- string ---- */
		case 's': {
			/* %ls takes a wide string and converts it to the
			 * multibyte encoding.  Precision counts BYTES of the
			 * conversion, and truncation happens on a character
			 * boundary: half a multibyte character is not a
			 * character, and emitting one would corrupt every byte
			 * that follows it in the output. */
			if (lng) {
				const wchar_t *ws = va_arg(ap, wchar_t *);
				char mb[MB_LEN_MAX];
				int wlen = 0;

				if (!ws) {
					ws = L"(null)";
				}
				/* Measure first, so width padding is right. */
				for (const wchar_t *w = ws; *w; w++) {
					size_t n = wcrtomb(mb, *w, 0);
					if (n == (size_t)-1)
						break;
					if (prec >= 0 &&
					    wlen + (int)n > prec)
						break;
					wlen += (int)n;
				}
				int wpad = (width > wlen) ? width - wlen : 0;

				if (!fl_minus)
					for (int p = 0; p < wpad; p++)
						SP_PUT(' ');
				int emitted = 0;
				for (const wchar_t *w = ws; *w; w++) {
					size_t n = wcrtomb(mb, *w, 0);
					if (n == (size_t)-1)
						break;
					if (emitted + (int)n > wlen)
						break;
					for (size_t i = 0; i < n; i++)
						SP_PUT(mb[i]);
					emitted += (int)n;
				}
				if (fl_minus)
					for (int p = 0; p < wpad; p++)
						SP_PUT(' ');

				format++;
				continue;
			}

			char *s = va_arg(ap, char *);
			if (!s)
				s = "(null)";
			int slen = 0;
			while (s[slen])
				slen++;
			if (prec >= 0 && prec < slen)
				slen = prec;
			int pad = (width > slen) ? width - slen : 0;

			if (!fl_minus)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');
			for (int i = 0; i < slen; i++)
				SP_PUT(s[i]);
			if (fl_minus)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');

			format++;
			continue;
		}

		/* ---- the count of characters produced so far ----
		 * Stored, not printed: the number the call would have
		 * written up to this point, truncated characters included.
		 * Programs build a buffer with it and continue writing where
		 * the formatted part ended; printed as a letter, the count was
		 * never stored and the caller went on from garbage. */
		case 'n':
			if (lng == 2)
				*va_arg(ap, long long *) = (long long)pos;
			else if (lng == 1)
				*va_arg(ap, long *) = (long)pos;
			else if (sht == 2)
				*va_arg(ap, signed char *) = (signed char)pos;
			else if (sht == 1)
				*va_arg(ap, short *) = (short)pos;
			else
				*va_arg(ap, int *) = (int)pos;
			format++;
			continue;

		/* ---- character ---- */
		case 'c': {
			/* %lc takes a wint_t and writes the multibyte encoding
			 * of that character -- one to four bytes, but always a
			 * single field for width purposes. */
			if (lng) {
				char mb[MB_LEN_MAX];
				wint_t wc = (wint_t)va_arg(ap, int);
				size_t n = wcrtomb(mb, (wchar_t)wc, 0);
				if (n == (size_t)-1)
					n = 0;
				int wpad = (width > 1) ? width - 1 : 0;
				if (!fl_minus)
					for (int p = 0; p < wpad; p++)
						SP_PUT(' ');
				for (size_t i = 0; i < n; i++)
					SP_PUT(mb[i]);
				if (fl_minus)
					for (int p = 0; p < wpad; p++)
						SP_PUT(' ');
				format++;
				continue;
			}

			char c = (char)va_arg(ap, int);
			int pad = (width > 1) ? width - 1 : 0;
			if (!fl_minus)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');
			SP_PUT(c);
			if (fl_minus)
				for (int p = 0; p < pad; p++)
					SP_PUT(' ');
			format++;
			continue;
		}

		/* ---- floating-point ---- */
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G': {
			struct fp_val fv;

			if (ldbl)
				fp_decode_ldbl(&fv, va_arg(ap, long double));
			else
				fp_decode_dbl(&fv, va_arg(ap, double));
			pos = fp_format(str, cap, pos, &fv, *format, width, prec,
					fl_minus, fl_zero, fl_plus, fl_space,
					fl_hash);
			format++;
			continue;
		}

		/* ---- unknown ---- */
		default:
			SP_PUT(*format);
			format++;
			continue;
		}
	}

	/* Terminate within the buffer: pos may now exceed it. */
	str[pos < cap ? pos : cap] = '\0';
	return (int)pos;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	/* C99: with a NULL buffer or zero size, nothing is written and the
	 * return value is the length the output would have had.
	 *
	 * Implemented by formatting with a one-byte buffer, so the counting is
	 * done by exactly the same code that does the writing -- there is no
	 * second implementation to disagree with the first, and no upper limit.
	 * The previous version formatted into an 8KB stack buffer and returned
	 * the truncated count, so a two-pass caller sizing a buffer for output
	 * longer than that under-allocated and then overran it. */
	if (!str || size == 0) {
		char dummy[1];
		return vsnprintf_body(dummy, 1, format, ap);
	}
	return vsnprintf_body(str, size, format, ap);
}

int snprintf(char *str, size_t size, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int ret = vsnprintf(str, size, format, ap);
	va_end(ap);
	return ret;
}

int sprintf(char *str, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	/* Unbounded, like vsprintf: see the note there.  A 4096 cap protected no
	 * caller with a smaller buffer and silently truncated those with a
	 * bigger one. */
	int ret = vsnprintf(str, (size_t)-1, format, ap);
	va_end(ap);
	return ret;
}

/* vsprintf - the va_list form of sprintf.
 *
 * NOT bounded.  It used to cap the output at 4096 bytes on the theory that a
 * truncated message beats a smashed stack, but that reasoning does not hold:
 * the cap protects nothing, because a caller whose buffer is smaller than 4096
 * -- which is most of them -- is overrun exactly as before.  All it added was
 * silent truncation for the callers whose buffers ARE big enough, turning a
 * correct program into one that quietly loses output.
 *
 * sprintf has no length argument, so the buffer's size is the caller's promise
 * to keep; there is no size here to check against and pretending otherwise
 * helped nobody.  Code that wants a guarantee uses snprintf. */
int vsprintf(char *str, const char *format, va_list ap)
{
	return vsnprintf(str, (size_t)-1, format, ap);
}

/* vprintf - the va_list form of printf.  C89, and missing until now. */
int vprintf(const char *format, va_list ap)
{
	return vfprintf(stdout, format, ap);
}

int vasprintf(char **strp, const char *format, va_list ap)
{
	va_list ap2;
	va_copy(ap2, ap);
	/* First pass: measure */
	int n = vsnprintf(NULL, 0, format, ap2);
	va_end(ap2);
	if (n < 0) {
		*strp = NULL;
		return -1;
	}
	char *buf = (char *)malloc((size_t)n + 1);
	if (!buf) {
		*strp = NULL;
		return -1;
	}
	va_copy(ap2, ap);
	vsnprintf(buf, (size_t)n + 1, format, ap2);
	va_end(ap2);
	*strp = buf;
	return n;
}

int asprintf(char **strp, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int ret = vasprintf(strp, format, ap);
	va_end(ap);
	return ret;
}

/* vsnprintf() returns the length the output WOULD have had, not what fit, so
 * a line longer than the stack buffer is formatted again into one of its own
 * size.  Writing `len' bytes from the stack buffer regardless put whatever
 * lay past its end on the stack into the output: a browser logging a long
 * URL wrote binary garbage to the log after its first 4 KB. */
int vfprintf(FILE *stream, const char *format, va_list ap)
{
	char stackbuf[4096];
	char *buf = stackbuf;
	va_list ap2;
	int len;

	va_copy(ap2, ap);
	len = vsnprintf(stackbuf, sizeof(stackbuf), format, ap2);
	va_end(ap2);
	if (len < 0)
		return len;
	if ((size_t)len >= sizeof(stackbuf)) {
		buf = (char *)malloc((size_t)len + 1);
		if (!buf) {
			errno = ENOMEM;
			return -1;
		}
		va_copy(ap2, ap);
		vsnprintf(buf, (size_t)len + 1, format, ap2);
		va_end(ap2);
	}
	if (len > 0)
		fwrite(buf, 1, (size_t)len, stream);
	if (buf != stackbuf)
		free(buf);
	return len;
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int ret = vfprintf(stream, format, ap);
	va_end(ap);
	return ret;
}

int printf(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int ret = vfprintf(stdout, format, ap);
	va_end(ap);
	return ret;
}

/* dprintf()/vdprintf() format straight to a descriptor with write(2): no FILE,
 * no buffering.  That is what makes them usable where a stream is unavailable
 * or unwanted - in a forked child before exec, or on an error path that must
 * not disturb a stream's buffer. */
int vdprintf(int fd, const char *format, va_list ap)
{
	char stackbuf[1024];
	char *buf = stackbuf;
	va_list ap2;
	int len, written = 0;

	va_copy(ap2, ap);
	len = vsnprintf(stackbuf, sizeof(stackbuf), format, ap2);
	va_end(ap2);
	if (len < 0)
		return -1;

	/* vsnprintf() reports what it managed to store, so a full buffer means
	 * the line may have been cut short.  Measure it properly and format it
	 * again rather than writing a truncated line. */
	if ((size_t)len >= sizeof(stackbuf) - 1) {
		va_copy(ap2, ap);
		len = vsnprintf(NULL, 0, format, ap2);
		va_end(ap2);
		if (len < 0)
			return -1;
		buf = (char *)malloc((size_t)len + 1);
		if (!buf)
			return -1;
		va_copy(ap2, ap);
		vsnprintf(buf, (size_t)len + 1, format, ap2);
		va_end(ap2);
	}

	while (written < len) {
		ssize_t n = write(fd, buf + written, (size_t)(len - written));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (buf != stackbuf)
				free(buf);
			return -1;
		}
		if (n == 0)
			break;
		written += (int)n;
	}

	if (buf != stackbuf)
		free(buf);
	return written;
}

int dprintf(int fd, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int ret = vdprintf(fd, format, ap);
	va_end(ap);
	return ret;
}

/* ------------------------------------------------------------------ */
/* sscanf / vsscanf - minimal implementation                           */
/* Supports: %d, %i, %u, %x, %X, %o, %s, %c, %n, %ld, %li, %lu,    */
/*           %lx, %hd, %hi, %hu, %hx, %hX, %[, width modifiers,     */
/*           and the * (suppress) flag.                                */
/* ------------------------------------------------------------------ */

static int _is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
	       c == '\v';
}

static int _hex_val(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* vsscanf() and sscanf() now live in vfscanf.c, sharing the one scanf
 * engine with the stream forms.  The separate implementation that used
 * to be here had no float conversions, so sscanf("1.5", "%g", &f)
 * returned 0 while fscanf on the same input worked -- a divergence that
 * is invisible until something depends on the missing half. */



/* fseeko / ftello - large-file aliases. We use 64-bit off_t already so
 * these are simple wrappers around fseek / ftell. */
int fseeko(FILE *stream, off_t offset, int whence)
{
	return fseek(stream, (long)offset, whence);
}

off_t ftello(FILE *stream)
{
	return (off_t)ftell(stream);
}

/* fgetpos / fsetpos -- ISO C's position save/restore pair.
 *
 * Built on ftello/fseeko rather than ftell/fseek so a position beyond 2GB
 * round-trips: off_t is 64-bit here, long is not guaranteed to be anywhere
 * else, and a truncated position would seek to the wrong place instead of
 * failing.
 *
 * fsetpos() must also discard any pushed-back character and the read-ahead
 * buffer, which fseeko() already does -- that is the whole reason to route
 * through it rather than assigning the offset directly. */
int fgetpos(FILE *stream, fpos_t *pos)
{
	off_t off;

	if (!stream || !pos) {
		errno = EINVAL;
		return -1;
	}
	off = ftello(stream);
	if (off == (off_t)-1)
		return -1; /* ftello set errno */
	pos->__pos = off;
	return 0;
}

int fsetpos(FILE *stream, const fpos_t *pos)
{
	if (!stream || !pos) {
		errno = EINVAL;
		return -1;
	}
	return fseeko(stream, pos->__pos, SEEK_SET);
}

void perror(const char *s)
{
	if (s && *s)
		fprintf(stderr, "%s: ", s);
	fprintf(stderr, "%s\n", strerror(errno));
}

/* tmpfile(): a scratch file that vanishes when it is closed.
 *
 * Created and then immediately unlinked — the open descriptor keeps the inode
 * alive, so the data survives exactly as long as the stream and nothing is
 * left behind if the process dies. That is the whole point of the call, and
 * it is why this cannot be built from tmpnam() plus fopen(): between those two
 * steps another process could claim the name. */
FILE *tmpfile(void)
{
	char path[] = "/tmp/tmpfXXXXXX";
	int fd = mkstemp(path);
	FILE *f;

	if (fd < 0)
		return NULL;
	unlink(path);
	f = fdopen(fd, "w+");
	if (!f)
		close(fd);
	return f;
}

/* tmpnam(): returns a name that did not exist a moment ago.  Inherently racy —
 * anything may create it before the caller does — which is why tmpfile() and
 * mkstemp() exist and why the standard marks this obsolescent. */
char *tmpnam(char *s)
{
	static char buf[L_tmpnam];
	char *out = s ? s : buf;
	static unsigned counter;

	for (int attempt = 0; attempt < 100; attempt++) {
		snprintf(out, L_tmpnam, "/tmp/tn%u_%u", (unsigned)getpid(),
			 counter++);
		if (access(out, F_OK) != 0)
			return out;
	}
	return NULL;
}
