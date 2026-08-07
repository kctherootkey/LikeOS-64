/*
 * LikeOS-64 stdio_ext.h - Inspecting a FILE from outside the C library
 *
 * Not standard, and not an extension invented here: this is the interface
 * Solaris introduced and glibc, musl and the BSDs adopted, and it is what
 * portable software reaches for when it needs to know something about a stream
 * that <stdio.h> does not expose -- whether output is pending, whether the
 * buffer is line buffered, whether anything can be read from it.
 *
 * gnulib is the reason it exists here.  Its replacement modules need to reach
 * into FILE, and each one carries a list of every C library's internal layout
 * ending in "#error Please port gnulib to your platform".  When the C library
 * provides these functions instead, gnulib uses them and compiles that file out
 * entirely -- so one header here settles it for every GNU package that will
 * ever be built, rather than one patch per package against a copy of the same
 * code.
 *
 * __fseterr is the musl spelling rather than the Solaris one; gnulib looks for
 * exactly that name.
 */

#ifndef _STDIO_EXT_H
#define _STDIO_EXT_H

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values for __fsetlocking(). */
#define FSETLOCKING_QUERY    0
#define FSETLOCKING_INTERNAL 1
#define FSETLOCKING_BYCALLER 2

/* Size of the stream's buffer, in bytes. */
size_t __fbufsize(FILE *fp);

/*
 * Whether the stream is currently reading or writing.
 *
 * The conventional definition is "read-only, or the last operation was a
 * read".  This library keeps the two buffers separate rather than recording
 * which was used last, so the second half of that is answered by asking
 * whether the buffer in question holds anything -- which is the property every
 * caller of these actually wants: gnulib uses them to decide whether there is
 * something to flush or to discard.
 */
int __freading(FILE *fp);
int __fwriting(FILE *fp);

/* Whether the stream permits reading / writing at all, from the mode it was
 * opened with.  Unlike the two above, these do not depend on what has happened
 * to the stream since. */
int __freadable(FILE *fp);
int __fwritable(FILE *fp);

/* Non-zero if the stream is line buffered. */
int __flbf(FILE *fp);

/* Discard the contents of both buffers WITHOUT writing anything out.  Pending
 * output is lost, which is the point: a process about to abandon a stream (a
 * failed child after fork, say) uses this so the buffer is not written twice. */
void __fpurge(FILE *fp);

/* Bytes of output buffered and not yet written. */
size_t __fpending(FILE *fp);

/* Flush every line-buffered stream. */
void __flushlbf(void);

/*
 * Locking discipline for a stream.  This library serialises nothing per
 * stream, so the answer is always FSETLOCKING_INTERNAL -- the caller is told
 * that locking is the library's business, which is true, rather than being led
 * to believe it has taken over something that does not exist.
 */
int __fsetlocking(FILE *fp, int type);

/* Set the stream's error indicator, as ferror() reports it.  There is no
 * standard way to do this from outside, which is why the function is here. */
void __fseterr(FILE *fp);

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_EXT_H */
