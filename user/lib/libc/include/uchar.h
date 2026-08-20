/*
 * uchar.h - conversion between the multibyte encoding and UTF-16/UTF-32.
 *
 * C11 7.28.  The multibyte encoding here is UTF-8 in every locale (see
 * src/locale/multibyte.c for why), so these are UTF-8 to UTF-16 and UTF-8 to
 * UTF-32 converters with a resumable state.
 *
 * The four functions are the restartable ones, and mbrtoc16 has a wrinkle none
 * of the others do: one multibyte character can produce TWO char16_t units.
 * It returns the high surrogate and keeps the low one in the mbstate_t, and
 * the caller gets it from the next call -- which consumes no input and returns
 * (size_t)-3 to say so.  Anything that stops calling as soon as it has the
 * bytes it asked for will silently truncate astral characters.
 *
 * The header exists as much for what it DECLARES as for what it defines: a
 * good deal of C code tests __has_include(<uchar.h>) and, failing to find it,
 * falls back to `#define char16_t uint16_t' -- which is a fair guess in C and
 * a disaster in C++, where char16_t is a keyword and cannot be a macro.
 */
#ifndef _UCHAR_H
#define _UCHAR_H

#include <stddef.h>

/* mbstate_t, shared with the wchar_t conversions: a conversion in progress is
 * the same conversion whichever of these functions is driving it. */
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* char16_t and char32_t are KEYWORDS in C++ (and in C23), with the types
 * already defined by the language.  Declaring them here would be an error, not
 * a redefinition, which is why this is guarded rather than typedef'd
 * unconditionally.
 *
 * In C they are the types the standard names: the least-width unsigned types
 * able to hold a UTF-16 code unit and a Unicode code point.  Spelled through
 * <stdint.h>'s uint_least16_t/uint_least32_t exactly as C11 7.28 requires, so
 * that a translation unit which gets them from <stdint.h> and one which gets
 * them from here agree. */
#if !defined(__cplusplus) && __STDC_VERSION__ < 202311L
#include <stdint.h>
#ifndef __char16_t_defined
#define __char16_t_defined
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif
#endif

/* Both are always true here: the encodings are UTF-16 and UTF-32, not some
 * implementation-defined 16- and 32-bit encoding.  C11 7.28p2 says to define
 * them in that case and to leave them undefined otherwise.
 *
 * Guarded because GCC predefines both -- it knows its own char16_t and
 * char32_t are Unicode -- and redefining a macro to the same value is still a
 * diagnostic. */
#ifndef __STDC_UTF_16__
#define __STDC_UTF_16__ 1
#endif
#ifndef __STDC_UTF_32__
#define __STDC_UTF_32__ 1
#endif

/* Multibyte to UTF-16.
 *
 * Returns the number of bytes consumed, 0 for the null character, (size_t)-1
 * with EILSEQ for an invalid sequence, (size_t)-2 if the n bytes given are a
 * valid but incomplete character, and (size_t)-3 when the char16_t stored came
 * from the state rather than from the input -- the low half of a surrogate
 * pair, for which no byte was read. */
size_t mbrtoc16(char16_t *__pc16, const char *__s, size_t __n,
		mbstate_t *__ps);

/* UTF-16 to multibyte.
 *
 * Returns the number of bytes written.  A high surrogate writes nothing and
 * returns 0: it is held in the state until the low half arrives on the next
 * call, and the whole character is written then.  A low surrogate with no high
 * one before it, or a high one followed by anything else, is EILSEQ. */
size_t c16rtomb(char *__s, char16_t __c16, mbstate_t *__ps);

/* Multibyte to UTF-32.  A code point is a code point, so this is the wchar_t
 * conversion under another name and never returns (size_t)-3. */
size_t mbrtoc32(char32_t *__pc32, const char *__s, size_t __n,
		mbstate_t *__ps);

/* UTF-32 to multibyte. */
size_t c32rtomb(char *__s, char32_t __c32, mbstate_t *__ps);

#ifdef __cplusplus
}
#endif

#endif /* _UCHAR_H */
