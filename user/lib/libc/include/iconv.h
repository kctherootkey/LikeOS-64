/*
 * iconv.h - character set conversion, as specified by POSIX.
 */
#ifndef _ICONV_H
#define _ICONV_H

#include <stddef.h>

/* Opaque conversion descriptor.  POSIX says only that it is a type suitable
 * for holding a conversion descriptor; a pointer is the conventional choice
 * and is what makes the documented (iconv_t)-1 failure value work. */
typedef void *iconv_t;

/* Open a descriptor converting FROMCODE to TOCODE.  Returns (iconv_t)-1 and
 * sets errno to EINVAL when the pair is not supported. */
iconv_t iconv_open(const char *tocode, const char *fromcode);

/* Convert.  Advances the two buffer pointers and decrements the two counts as
 * it goes, so an interrupted call can simply be repeated.
 *
 * Returns the number of characters converted in a non-reversible way, or
 * (size_t)-1 with errno set:
 *   EILSEQ  invalid byte sequence in the input
 *   EINVAL  input ends in the middle of a valid sequence
 *   E2BIG   no room left in the output buffer
 *
 * Called with inbuf NULL (or *inbuf NULL) it returns the conversion to its
 * initial state, which for the stateless encodings here is a no-op. */
size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft, char **outbuf,
	     size_t *outbytesleft);

int iconv_close(iconv_t cd);

#endif /* _ICONV_H */
