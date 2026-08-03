/*
 * multibyte.c - conversion between the multibyte character set and wide
 * characters.
 *
 * The multibyte encoding on this system is UTF-8, in every locale.  That is a
 * deliberate simplification rather than an omission: the console decodes UTF-8,
 * the terminal emulator emits it, filenames are byte strings that carry it and
 * the network tools speak it.  A "C" locale that treated each byte as its own
 * character would not make any of those single-byte -- it would only make the
 * library disagree with the rest of the system about what a character is, and
 * the disagreement shows up as mojibake.
 *
 * wchar_t is 32 bits here (the compiler's __WCHAR_TYPE__ on x86_64), so a wide
 * character is a Unicode code point with no surrogate games.
 *
 * Malformed input is rejected rather than patched over: an overlong form, a
 * surrogate half or a value past U+10FFFF sets EILSEQ and returns (size_t)-1.
 * Accepting them would let one encoding of a character slip past a check
 * written against another, which is the shape of a long line of security bugs.
 */
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h> /* EOF, which btowc/wctob are specified in terms of */
#include <errno.h>
#include <limits.h>

/* mbstate_t is two unsigned ints.  __value holds the partially assembled code
 * point; __count packs both how many continuation bytes are still expected and
 * how long the whole sequence is.  The total has to be remembered, not just
 * the remainder, because the overlong check compares the finished value
 * against the smallest one its length can legally express -- and the sequence
 * may have been split across two calls by then.
 *
 * Zero means "initial state", which is what mbsinit() reports on. */
#define ST_PACK(need, total) ((unsigned)(need) | ((unsigned)(total) << 4))
#define ST_NEED(c) ((c) & 0xF)
#define ST_TOTAL(c) ((c) >> 4)

/* Private state for the calls that are allowed to be non-reentrant
 * (mbtowc/wctomb/mbstowcs/wcstombs take no mbstate_t).  A conversion is
 * complete on return from those, so this only ever holds the initial state --
 * it exists so mbsinit-style resets have something to clear. */
static mbstate_t __private_state;

static inline mbstate_t *state_or_private(mbstate_t *ps)
{
	return ps ? ps : &__private_state;
}

int mbsinit(const mbstate_t *ps)
{
	return !ps || ps->__count == 0;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
	mbstate_t *st = state_or_private(ps);
	unsigned char b;
	unsigned cp;
	unsigned need;
	unsigned total;
	size_t i = 0;

	if (!s) {
		/* mbrtowc(NULL, NULL, ...) means "reset to the initial state
		 * and report whether the encoding is stateful".  UTF-8 is not. */
		st->__count = 0;
		st->__value = 0;
		return 0;
	}

	if (st->__count == 0) {
		if (n == 0)
			return (size_t)-2;
		b = (unsigned char)s[i++];
		if (b < 0x80) {
			if (pwc)
				*pwc = (wchar_t)b;
			return b ? 1 : 0;
		}
		if (b < 0xC2) {
			/* 0x80..0xBF is a stray continuation byte; 0xC0/0xC1
			 * could only ever encode an overlong ASCII value. */
			errno = EILSEQ;
			return (size_t)-1;
		}
		if (b < 0xE0) {
			cp = b & 0x1F;
			total = 2;
		} else if (b < 0xF0) {
			cp = b & 0x0F;
			total = 3;
		} else if (b < 0xF5) {
			cp = b & 0x07;
			total = 4;
		} else {
			/* 0xF5..0xFF would start a sequence above U+10FFFF. */
			errno = EILSEQ;
			return (size_t)-1;
		}
		need = total - 1;
	} else {
		cp = st->__value;
		need = ST_NEED(st->__count);
		total = ST_TOTAL(st->__count);
	}

	while (need) {
		if (i == n) {
			/* Ran out of input mid-character: remember where we
			 * got to so the next call can continue. */
			st->__value = cp;
			st->__count = ST_PACK(need, total);
			return (size_t)-2;
		}
		b = (unsigned char)s[i++];
		if ((b & 0xC0) != 0x80) {
			st->__count = 0;
			st->__value = 0;
			errno = EILSEQ;
			return (size_t)-1;
		}
		cp = (cp << 6) | (b & 0x3F);
		need--;
	}

	st->__count = 0;
	st->__value = 0;

	/* Reject what a shorter sequence could have expressed, the surrogate
	 * range (which has no meaning outside UTF-16) and anything past the
	 * last code point. */
	if ((total == 2 && cp < 0x80) || (total == 3 && cp < 0x800) ||
	    (total == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) ||
	    cp > 0x10FFFF) {
		errno = EILSEQ;
		return (size_t)-1;
	}

	if (pwc)
		*pwc = (wchar_t)cp;
	return cp ? i : 0;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps)
{
	static mbstate_t internal;
	return mbrtowc(0, s, n, ps ? ps : &internal);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
	unsigned cp = (unsigned)wc;

	(void)ps; /* UTF-8 output carries no shift state */

	if (!s) {
		/* Defined as wcrtomb(buf, L'\0', ps): the length needed to
		 * return to the initial state, which for UTF-8 is one NUL. */
		return 1;
	}
	if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
		errno = EILSEQ;
		return (size_t)-1;
	}

	if (cp < 0x80) {
		s[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		s[0] = (char)(0xC0 | (cp >> 6));
		s[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		s[0] = (char)(0xE0 | (cp >> 12));
		s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		s[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	s[0] = (char)(0xF0 | (cp >> 18));
	s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	s[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	s[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

int mbtowc(wchar_t *pwc, const char *s, size_t n)
{
	mbstate_t st = { 0, 0 };
	size_t r;

	if (!s)
		return 0; /* UTF-8 has no shift states */
	r = mbrtowc(pwc, s, n, &st);
	if (r == (size_t)-1)
		return -1;
	if (r == (size_t)-2) {
		/* An incomplete character is an error for the non-restartable
		 * form: it has nowhere to keep the partial state. */
		errno = EILSEQ;
		return -1;
	}
	return (int)r;
}

int mblen(const char *s, size_t n)
{
	return mbtowc(0, s, n);
}

int wctomb(char *s, wchar_t wc)
{
	size_t r;

	if (!s)
		return 0;
	r = wcrtomb(s, wc, 0);
	return (r == (size_t)-1) ? -1 : (int)r;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps)
{
	mbstate_t internal = { 0, 0 };
	mbstate_t *st = ps ? ps : &internal;
	const char *s = *src;
	size_t written = 0;

	for (;;) {
		wchar_t wc;
		size_t r;

		if (dst && written == len)
			break;
		r = mbrtowc(&wc, s, (size_t)-1 / 2, st);
		if (r == (size_t)-1) {
			if (dst)
				*src = s;
			return (size_t)-1;
		}
		if (r == 0) {
			/* Terminating NUL: the source pointer is set to NULL,
			 * and the NUL is not counted. */
			if (dst) {
				dst[written] = 0;
				*src = 0;
			}
			return written;
		}
		if (dst)
			dst[written] = wc;
		written++;
		s += r;
	}
	*src = s;
	return written;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps)
{
	const wchar_t *s = *src;
	size_t written = 0;
	char buf[MB_LEN_MAX];

	(void)ps;

	for (;;) {
		size_t r;

		if (*s == 0) {
			if (dst) {
				if (written < len)
					dst[written] = 0;
				*src = 0;
			}
			return written;
		}
		r = wcrtomb(buf, *s, 0);
		if (r == (size_t)-1) {
			if (dst)
				*src = s;
			return (size_t)-1;
		}
		if (dst) {
			/* A character that would only partly fit is not
			 * written at all -- a truncated sequence is not a
			 * character. */
			if (written + r > len)
				break;
			for (size_t i = 0; i < r; i++)
				dst[written + i] = buf[i];
		}
		written += r;
		s++;
	}
	*src = s;
	return written;
}

size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nmc, size_t len,
		  mbstate_t *ps)
{
	mbstate_t internal = { 0, 0 };
	mbstate_t *st = ps ? ps : &internal;
	const char *s = *src;
	size_t left = nmc;
	size_t written = 0;

	for (;;) {
		wchar_t wc;
		size_t r;

		if (dst && written == len)
			break;
		if (left == 0)
			break;
		r = mbrtowc(&wc, s, left, st);
		if (r == (size_t)-1) {
			if (dst)
				*src = s;
			return (size_t)-1;
		}
		if (r == (size_t)-2) {
			/* Input exhausted mid-character; the state carries the
			 * partial one. */
			s += left;
			left = 0;
			break;
		}
		if (r == 0) {
			if (dst) {
				dst[written] = 0;
				*src = 0;
			}
			return written;
		}
		if (dst)
			dst[written] = wc;
		written++;
		s += r;
		left -= r;
	}
	if (dst)
		*src = s;
	return written;
}

size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
		  mbstate_t *ps)
{
	const wchar_t *s = *src;
	size_t left = nwc;
	size_t written = 0;
	char buf[MB_LEN_MAX];

	(void)ps;

	while (left) {
		size_t r;

		if (*s == 0) {
			if (dst) {
				if (written < len)
					dst[written] = 0;
				*src = 0;
			}
			return written;
		}
		r = wcrtomb(buf, *s, 0);
		if (r == (size_t)-1) {
			if (dst)
				*src = s;
			return (size_t)-1;
		}
		if (dst) {
			if (written + r > len)
				break;
			for (size_t i = 0; i < r; i++)
				dst[written + i] = buf[i];
		}
		written += r;
		s++;
		left--;
	}
	if (dst)
		*src = s;
	return written;
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
{
	const char *p = src;
	mbstate_t st = { 0, 0 };
	/* The non-restartable form must not touch the caller's pointer, so it
	 * converts from a copy. */
	return mbsrtowcs(dst, &p, n, &st);
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n)
{
	const wchar_t *p = src;
	return wcsrtombs(dst, &p, n, 0);
}

wint_t btowc(int c)
{
	unsigned char b;

	if (c == EOF)
		return WEOF;
	b = (unsigned char)c;
	/* Only bytes that are complete characters on their own convert; in
	 * UTF-8 that is exactly ASCII. */
	return (b < 0x80) ? (wint_t)b : WEOF;
}

int wctob(wint_t c)
{
	if (c == WEOF || (unsigned)c >= 0x80)
		return EOF;
	return (int)c;
}
