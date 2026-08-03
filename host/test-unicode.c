/*
 * test-unicode - differential test of the libc's UTF-8 and Unicode code.
 *
 * Compiles the LikeOS conversion, classification and width sources against the
 * host toolchain (with their public names prefixed so both libraries can be
 * linked at once) and compares every answer with the host C library's, over
 * every code point.  A table generated from a reference implementation is only
 * as good as the code that searches it, and this is what checks that code.
 *
 * Run from the repository root:
 *
 *     sh host/test-unicode.sh
 *
 * Two deliberate divergences are asserted rather than compared: glibc's
 * converter accepts F5 80 80 80 as U+140000 and F8 as a five-byte lead, both
 * of which RFC 3629 removed from UTF-8.  Matching it there would be a bug.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <errno.h>

/* Renamed under test (see the -D flags in the build command). */
size_t likeos_mbrtowc(wchar_t *, const char *, size_t, mbstate_t *);
size_t likeos_wcrtomb(char *, wchar_t, mbstate_t *);
int likeos_mbtowc(wchar_t *, const char *, size_t);
size_t likeos_mbstowcs(wchar_t *, const char *, size_t);
size_t likeos_wcstombs(char *, const wchar_t *, size_t);
size_t likeos_mbsrtowcs(wchar_t *, const char **, size_t, mbstate_t *);
int likeos_mbsinit(const mbstate_t *);
int likeos_wcwidth(wchar_t);
int likeos_wcswidth(const wchar_t *, size_t);
int likeos_iswalpha(unsigned);
int likeos_iswupper(unsigned);
int likeos_iswlower(unsigned);
int likeos_iswpunct(unsigned);
int likeos_iswprint(unsigned);
int likeos_iswspace(unsigned);
unsigned likeos_towupper(unsigned);
unsigned likeos_towlower(unsigned);

static int fails;
static void fail(const char *what, unsigned cp, long got, long want)
{
	if (fails++ < 25)
		printf("MISMATCH %-12s U+%04X: got %ld want %ld\n", what, cp,
		       got, want);
}

int main(void)
{
	if (!setlocale(LC_ALL, "C.UTF-8") && !setlocale(LC_ALL, "en_US.UTF-8")) {
		printf("no UTF-8 locale on host; cannot compare\n");
		return 2;
	}

	/* ---- Round trip every code point through wcrtomb + mbrtowc ---- */
	unsigned long checked = 0;
	for (unsigned cp = 1; cp < 0x110000; cp++) {
		char a[8], b[8];
		size_t na, nb;
		mbstate_t sa, sb;

		if (cp >= 0xD800 && cp <= 0xDFFF)
			continue;
		memset(&sa, 0, sizeof sa);
		memset(&sb, 0, sizeof sb);
		na = likeos_wcrtomb(a, (wchar_t)cp, &sa);
		nb = wcrtomb(b, (wchar_t)cp, &sb);
		if (na != nb || memcmp(a, b, nb)) {
			fail("wcrtomb", cp, (long)na, (long)nb);
			continue;
		}
		wchar_t wa = 0, wb = 0;
		memset(&sa, 0, sizeof sa);
		memset(&sb, 0, sizeof sb);
		size_t ra = likeos_mbrtowc(&wa, a, na, &sa);
		size_t rb = mbrtowc(&wb, b, nb, &sb);
		if (ra != rb || wa != wb)
			fail("mbrtowc", cp, (long)ra, (long)rb);
		checked++;
	}
	printf("round-trip: %lu code points\n", checked);

	/* ---- Byte-at-a-time decoding must agree with a whole-buffer one -- */
	for (unsigned cp = 1; cp < 0x110000; cp += 7) {
		char a[8];
		mbstate_t s;
		if (cp >= 0xD800 && cp <= 0xDFFF)
			continue;
		memset(&s, 0, sizeof s);
		size_t n = likeos_wcrtomb(a, (wchar_t)cp, &s);
		memset(&s, 0, sizeof s);
		wchar_t wc = 0;
		size_t total = 0;
		int ok = 1;
		for (size_t i = 0; i < n; i++) {
			size_t r = likeos_mbrtowc(&wc, a + i, 1, &s);
			if (i + 1 < n) {
				if (r != (size_t)-2) {
					ok = 0;
					break;
				}
			} else {
				if (r != 1) {
					ok = 0;
					break;
				}
				total = 1;
			}
		}
		if (!ok || !total || wc != (wchar_t)cp)
			fail("split-decode", cp, (long)wc, (long)cp);
	}
	printf("split decoding checked\n");

	/* ---- Malformed input must be rejected exactly as glibc does ---- */
	static const char *bad[] = {
		"\x80",		    /* stray continuation      */
		"\xC0\x80",	    /* overlong NUL            */
		"\xC1\xBF",	    /* overlong '?'            */
		"\xE0\x80\x80",	    /* overlong                */
		"\xF0\x80\x80\x80", /* overlong                */
		"\xED\xA0\x80",	    /* surrogate D800          */
		"\xED\xBF\xBF",	    /* surrogate DFFF          */
		"\xFF",		    /* never a lead byte       */
		"\xE2\x28\xA1",	    /* bad continuation        */
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		wchar_t wa = 0, wb = 0;
		mbstate_t sa, sb;
		memset(&sa, 0, sizeof sa);
		memset(&sb, 0, sizeof sb);
		size_t n = strlen(bad[i]);
		size_t ra = likeos_mbrtowc(&wa, bad[i], n, &sa);
		size_t rb = mbrtowc(&wb, bad[i], n, &sb);
		if (ra != rb)
			fail("bad-input", (unsigned)i, (long)ra, (long)rb);
	}
	/* Two forms where matching the reference would be WRONG: glibc's
	 * converter accepts F5 80 80 80 as U+140000 and takes F8 as the start
	 * of a five-byte sequence.  Both were removed from UTF-8 by RFC 3629;
	 * accepting them lets a value above U+10FFFF through.  These must be
	 * rejected here even though glibc does not reject them. */
	{
		static const char *lax[] = { "\xF5\x80\x80\x80",
					     "\xF8\x88\x80\x80" };
		for (size_t i = 0; i < 2; i++) {
			wchar_t w = 0;
			mbstate_t st;
			memset(&st, 0, sizeof st);
			size_t r = likeos_mbrtowc(&w, lax[i], 4, &st);
			if (r != (size_t)-1)
				fail("lax-reject", (unsigned)i, (long)r, -1);
		}
	}
	printf("malformed input checked (stricter than glibc on 2 forms)\n");

	/* ---- mbstowcs / wcstombs on a real string --------------------- */
	{
		const char *s = "Grüße aus München — 日本語 ok";
		wchar_t wa[64], wb[64];
		char ba[128], bb[128];
		size_t na = likeos_mbstowcs(wa, s, 64);
		size_t nb = mbstowcs(wb, s, 64);
		if (na != nb || wmemcmp(wa, wb, nb))
			fail("mbstowcs", 0, (long)na, (long)nb);
		size_t ca = likeos_wcstombs(ba, wa, sizeof ba);
		size_t cb = wcstombs(bb, wb, sizeof bb);
		if (ca != cb || memcmp(ba, bb, cb))
			fail("wcstombs", 0, (long)ca, (long)cb);
		const char *pa = s;
		wchar_t wc2[64];
		size_t ma = likeos_mbsrtowcs(wc2, &pa, 64, NULL);
		if (ma != nb || pa != NULL)
			fail("mbsrtowcs", 0, (long)ma, (long)nb);
	}
	printf("string conversions checked\n");

	/* ---- Classification and case mapping over all of Unicode ------ */
	unsigned long w_diff = 0;
	for (unsigned cp = 0; cp < 0x110000; cp++) {
		if (cp >= 0xD800 && cp <= 0xDFFF)
			continue;
		if (!!likeos_iswalpha(cp) != !!iswalpha(cp))
			fail("iswalpha", cp, likeos_iswalpha(cp), iswalpha(cp));
		if (!!likeos_iswupper(cp) != !!iswupper(cp))
			fail("iswupper", cp, likeos_iswupper(cp), iswupper(cp));
		if (!!likeos_iswlower(cp) != !!iswlower(cp))
			fail("iswlower", cp, likeos_iswlower(cp), iswlower(cp));
		if (likeos_towupper(cp) != (unsigned)towupper(cp))
			fail("towupper", cp, likeos_towupper(cp), towupper(cp));
		if (likeos_towlower(cp) != (unsigned)towlower(cp))
			fail("towlower", cp, likeos_towlower(cp), towlower(cp));
		if (!!likeos_iswspace(cp) != !!iswspace(cp))
			fail("iswspace", cp, likeos_iswspace(cp), iswspace(cp));
		if (!!likeos_iswpunct(cp) != !!iswpunct(cp))
			fail("iswpunct", cp, likeos_iswpunct(cp), iswpunct(cp));
		if (!!likeos_iswprint(cp) != !!iswprint(cp))
			fail("iswprint", cp, likeos_iswprint(cp), iswprint(cp));
		if (likeos_wcwidth(cp) != wcwidth(cp)) {
			w_diff++;
			fail("wcwidth", cp, likeos_wcwidth(cp), wcwidth(cp));
		}
	}
	printf("classification and wcwidth checked over all of Unicode "
	       "(%lu width diffs)\n", w_diff);

	printf(fails ? "\n%d MISMATCHES\n" : "\nALL OK (%d mismatches)\n", fails);
	return fails != 0;
}
