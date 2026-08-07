/*
 * LikeOS-64 stdio_ext.c - Inspecting a FILE from outside the C library
 *
 * See <stdio_ext.h> for what these are and why they are here.  Each one is a
 * few lines; what matters is that they exist, because their absence makes
 * every GNU package that bundles gnulib fail to build with a #error naming
 * neither the package nor the cause.
 */

#include <stdio_ext.h>
#include <fcntl.h>
#include <stddef.h>

size_t __fbufsize(FILE *fp)
{
	if (!fp)
		return 0;
	/* Reading and writing use separate buffers here.  Report whichever the
	 * stream actually has; a stream open for both has the same size for
	 * each, so the choice only matters for one-directional streams. */
	return fp->wbuf_size ? fp->wbuf_size : fp->buf_size;
}

int __freading(FILE *fp)
{
	if (!fp)
		return 0;
	if ((fp->flags & O_ACCMODE) == O_RDONLY)
		return 1;
	/* Something has been read into the buffer and not yet consumed. */
	return fp->buf_pos < fp->buf_end;
}

int __fwriting(FILE *fp)
{
	if (!fp)
		return 0;
	if ((fp->flags & O_ACCMODE) == O_WRONLY)
		return 1;
	return fp->wbuf_pos > 0;
}

int __freadable(FILE *fp)
{
	int acc;

	if (!fp)
		return 0;
	acc = fp->flags & O_ACCMODE;
	return acc == O_RDONLY || acc == O_RDWR;
}

int __fwritable(FILE *fp)
{
	int acc;

	if (!fp)
		return 0;
	acc = fp->flags & O_ACCMODE;
	return acc == O_WRONLY || acc == O_RDWR;
}

int __flbf(FILE *fp)
{
	return fp && fp->buf_mode == _IOLBF;
}

void __fpurge(FILE *fp)
{
	if (!fp)
		return;
	/* Deliberately without flushing: the caller is discarding this output,
	 * not deferring it.  A child process that has decided to give up uses
	 * this so its copy of the parent's buffer is not written a second
	 * time. */
	fp->wbuf_pos = 0;
	fp->buf_pos = 0;
	fp->buf_end = 0;
	fp->ungetc_buf = -1;
}

size_t __fpending(FILE *fp)
{
	if (!fp)
		return 0;
	return fp->wbuf_pos;
}

void __flushlbf(void)
{
	/*
	 * The line-buffered streams, which in practice are the standard ones:
	 * this library keeps no registry of open streams, so there is no way to
	 * find any others.  The same limitation is why fflush(NULL) flushes
	 * stdout alone.
	 *
	 * Flushing more than was asked for would be wrong in a way that is hard
	 * to see, so this flushes only streams that ARE line buffered.
	 */
	if (stdout && stdout->buf_mode == _IOLBF)
		fflush(stdout);
	if (stderr && stderr->buf_mode == _IOLBF)
		fflush(stderr);
}

int __fsetlocking(FILE *fp, int type)
{
	(void)fp;
	(void)type;
	/* Nothing is serialised per stream here, so there is nothing for a
	 * caller to take over.  Answering INTERNAL says the library is
	 * responsible, which is the truth; answering BYCALLER would tell it
	 * that it had acquired a responsibility it cannot discharge. */
	return FSETLOCKING_INTERNAL;
}

void __fseterr(FILE *fp)
{
	if (fp)
		fp->error = 1;
}
