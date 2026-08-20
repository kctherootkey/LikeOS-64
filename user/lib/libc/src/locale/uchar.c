/*
 * uchar.c - the UTF-16 and UTF-32 conversions of C11 7.28.
 *
 * Built on mbrtowc/wcrtomb in multibyte.c rather than beside them: the
 * multibyte encoding is UTF-8 and wchar_t is a 32-bit code point, so the
 * UTF-32 pair is those functions under a different name and the UTF-16 pair is
 * those functions plus surrogate arithmetic.  Duplicating the UTF-8 decoder
 * would mean two places to get the overlong and surrogate checks right.
 *
 * The one piece of state of its own is the pending low surrogate.  A character
 * above U+FFFF is one multibyte sequence and TWO char16_t units, so mbrtoc16
 * has to hand back the second one on a later call, with no input consumed.  It
 * lives in the caller's mbstate_t, flagged by a bit that multibyte.c's own
 * packing cannot produce -- see ST_PENDING below.
 */
#include <uchar.h>
#include <wchar.h>
#include <errno.h>
#include <limits.h> /* MB_LEN_MAX, for the throwaway buffer a null s writes to */

/* The surrogate range, and the arithmetic that maps it to and from the
 * supplementary planes.  Named rather than written out because the same four
 * constants appear in five places below and a mistyped one produces text that
 * is subtly wrong rather than absent. */
#define SUR_HIGH_MIN 0xD800u
#define SUR_HIGH_MAX 0xDBFFu
#define SUR_LOW_MIN 0xDC00u
#define SUR_LOW_MAX 0xDFFFu
#define SUR_BASE 0x10000u

/* Our claim on the shared mbstate_t.
 *
 * multibyte.c packs __count as (bytes still needed) | (total length << 4),
 * where the total is at most 4 -- so every value it can hold is below 0x100
 * and the top bit is ours to use.  __value then holds the pending unit
 * instead of a partly assembled code point.
 *
 * The two uses never overlap.  Every path below drains a pending surrogate and
 * returns before it would call into multibyte.c, so mbrtowc is never handed a
 * state with this bit set and never has to know about it.  mbsinit() needs no
 * change either: __count is non-zero while a surrogate is pending, which is
 * exactly the answer -- the conversion is not in its initial state.
 */
#define ST_PENDING 0x80000000u

/* The private state for a NULL ps.
 *
 * The standard allows each function its own, and one shared object would be
 * wrong: a program converting in both directions with ps == NULL would have
 * c16rtomb's half-written surrogate pair overwritten by mbrtoc16's. */
static mbstate_t mbrtoc16_state;
static mbstate_t c16rtomb_state;

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps)
{
	mbstate_t *st = ps ? ps : &mbrtoc16_state;
	wchar_t wc = 0;
	size_t r;
	unsigned cp;

	/* A low surrogate left over from the previous call.  It is handed back
	 * before anything is read, and -3 says the return value is NOT a byte
	 * count -- the input pointer must not be advanced. */
	if (st->__count & ST_PENDING) {
		if (pc16)
			*pc16 = (char16_t)st->__value;
		st->__count = 0;
		st->__value = 0;
		return (size_t)-3;
	}

	/* C11 7.28.1.2p3: a null s is the same call made on "" with n of 1,
	 * ignoring pc16 -- which converts the null character and so resets. */
	if (!s) {
		s = "";
		n = 1;
		pc16 = 0;
	}

	r = mbrtowc(&wc, s, n, st);
	if (r == (size_t)-1 || r == (size_t)-2)
		return r; /* invalid, or incomplete: errno already set for -1 */

	cp = (unsigned)wc;
	if (cp < SUR_BASE) {
		/* One unit.  mbrtowc has already rejected the surrogate range,
		 * so this cannot be half of a pair that came in as UTF-8. */
		if (pc16)
			*pc16 = (char16_t)cp;
		return r; /* 0 for the null character, as mbrtowc returned */
	}

	/* Above the basic plane: split into a pair, return the high half now
	 * and keep the low one for the next call. */
	cp -= SUR_BASE;
	if (pc16)
		*pc16 = (char16_t)(SUR_HIGH_MIN + (cp >> 10));
	st->__value = SUR_LOW_MIN + (cp & 0x3FF);
	st->__count = ST_PENDING;
	return r;
}

size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps)
{
	mbstate_t *st = ps ? ps : &c16rtomb_state;
	char buf[MB_LEN_MAX];
	unsigned u = (unsigned)c16;
	unsigned cp;

	/* C11 7.28.1.3p3: a null s is the same call made on an internal buffer
	 * with a null wide character, which resets the state. */
	if (!s) {
		st->__count = 0;
		st->__value = 0;
		s = buf;
		c16 = 0;
		u = 0;
	}

	if (st->__count & ST_PENDING) {
		/* A high surrogate is waiting.  Only its low half completes
		 * the character; anything else is a broken pair, and leaving
		 * the state set would make every following call fail too. */
		if (u < SUR_LOW_MIN || u > SUR_LOW_MAX) {
			st->__count = 0;
			st->__value = 0;
			errno = EILSEQ;
			return (size_t)-1;
		}
		cp = SUR_BASE + ((st->__value - SUR_HIGH_MIN) << 10) +
		     (u - SUR_LOW_MIN);
		st->__count = 0;
		st->__value = 0;
		return wcrtomb(s, (wchar_t)cp, st);
	}

	if (u >= SUR_HIGH_MIN && u <= SUR_HIGH_MAX) {
		/* Half a character: nothing can be written yet.  Zero bytes
		 * written is the correct answer, not an error. */
		st->__value = u;
		st->__count = ST_PENDING;
		return 0;
	}

	if (u >= SUR_LOW_MIN && u <= SUR_LOW_MAX) {
		/* A low surrogate with no high one before it encodes nothing. */
		errno = EILSEQ;
		return (size_t)-1;
	}

	return wcrtomb(s, (wchar_t)u, st);
}

size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps)
{
	/* wchar_t is a 32-bit code point here, so this is mbrtowc exactly --
	 * including its rejection of surrogates and of anything above
	 * U+10FFFF, which is what makes the result a valid char32_t.
	 *
	 * The cast is between two 32-bit unsigned-representable types holding
	 * the same value; it is not reinterpreting anything. */
	wchar_t wc = 0;
	size_t r = mbrtowc(&wc, s, n, ps);

	if (pc32 && r != (size_t)-1 && r != (size_t)-2)
		*pc32 = (char32_t)wc;
	return r;
}

size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps)
{
	return wcrtomb(s, (wchar_t)c32, ps);
}
