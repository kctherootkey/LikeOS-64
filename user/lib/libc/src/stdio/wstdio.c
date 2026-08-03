/*
 * wstdio.c - the wide-character stream functions.
 *
 * These are a layer on top of the byte functions rather than a second I/O
 * path: a stream holds bytes either way, and the only difference is that the
 * wide functions convert between those bytes and wchar_t on the way through.
 * Sharing the buffer is what keeps fflush, seeking and line buffering working
 * identically for both.
 *
 * ISO C gives a stream an orientation, fixed by the first operation on it and
 * unchangeable afterwards.  It is tracked here (and settable through fwide)
 * because programs query it; the implementation would work without it, but a
 * program that asks is entitled to a truthful answer.
 */
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

/* Commit a stream to wide orientation, or report that it is already byte
 * oriented.  Returns 0 on success, -1 if the stream cannot be used wide. */
static int want_wide(FILE *stream)
{
	if (!stream)
		return -1;
	if (stream->wide_mode < 0)
		return -1;
	stream->wide_mode = 1;
	return 0;
}

int fwide(FILE *stream, int mode)
{
	if (!stream)
		return 0;
	if (mode != 0 && stream->wide_mode == 0)
		stream->wide_mode = (mode > 0) ? 1 : -1;
	return stream->wide_mode;
}

wint_t fgetwc(FILE *stream)
{
	mbstate_t st;
	char buf[MB_LEN_MAX];
	size_t used = 0;

	if (want_wide(stream) != 0)
		return WEOF;

	/* Continue any character the previous call stopped inside of. */
	st.__count = stream->wc_count;
	st.__value = stream->wc_value;

	for (;;) {
		wchar_t wc;
		size_t r;
		int c = fgetc(stream);

		if (c == EOF) {
			/* End of input with a character half read is not a
			 * character; the bytes are consumed and reported as an
			 * encoding error, per the standard. */
			if (!mbsinit(&st)) {
				stream->wc_count = 0;
				stream->wc_value = 0;
				stream->error = 1;
				errno = EILSEQ;
			}
			return WEOF;
		}

		buf[0] = (char)c;
		used = 1;
		r = mbrtowc(&wc, buf, used, &st);
		if (r == (size_t)-1) {
			stream->wc_count = 0;
			stream->wc_value = 0;
			stream->error = 1;
			return WEOF;
		}
		if (r == (size_t)-2) {
			/* Need more bytes; the state remembers what we have. */
			stream->wc_count = st.__count;
			stream->wc_value = st.__value;
			continue;
		}
		stream->wc_count = 0;
		stream->wc_value = 0;
		return (wint_t)wc;
	}
}

wint_t getwc(FILE *stream)
{
	return fgetwc(stream);
}

wint_t getwchar(void)
{
	return fgetwc(stdin);
}

wint_t fputwc(wchar_t wc, FILE *stream)
{
	char buf[MB_LEN_MAX];
	size_t n;

	if (want_wide(stream) != 0)
		return WEOF;

	n = wcrtomb(buf, wc, 0);
	if (n == (size_t)-1) {
		stream->error = 1;
		return WEOF;
	}
	if (fwrite(buf, 1, n, stream) != n)
		return WEOF;
	return (wint_t)wc;
}

wint_t putwc(wchar_t wc, FILE *stream)
{
	return fputwc(wc, stream);
}

wint_t putwchar(wchar_t wc)
{
	return fputwc(wc, stdout);
}

wint_t ungetwc(wint_t wc, FILE *stream)
{
	char buf[MB_LEN_MAX];
	size_t n;

	if (wc == WEOF || want_wide(stream) != 0)
		return WEOF;

	n = wcrtomb(buf, (wchar_t)wc, 0);
	if (n == (size_t)-1)
		return WEOF;
	/* The byte layer holds one pushed-back character, so only a
	 * single-byte character can be pushed back reliably.  Report failure
	 * rather than silently dropping bytes of a multibyte one -- a caller
	 * that checks gets to do something about it. */
	if (n != 1)
		return WEOF;
	if (ungetc((unsigned char)buf[0], stream) == EOF)
		return WEOF;
	return wc;
}

wchar_t *fgetws(wchar_t *s, int n, FILE *stream)
{
	int i = 0;

	if (!s || n <= 0)
		return 0;
	while (i < n - 1) {
		wint_t wc = fgetwc(stream);
		if (wc == WEOF) {
			if (i == 0)
				return 0;
			break;
		}
		s[i++] = (wchar_t)wc;
		if (wc == L'\n')
			break;
	}
	s[i] = L'\0';
	return s;
}

int fputws(const wchar_t *s, FILE *stream)
{
	for (; *s; s++)
		if (fputwc(*s, stream) == WEOF)
			return -1;
	return 0;
}

/* ---- Wide formatted output ---------------------------------------------
 *
 * Wide format strings are transcribed to bytes and handed to the narrow
 * formatter.  Every character with meaning to printf is ASCII, so the
 * transcription is exact, and there is one formatter to keep correct rather
 * than two.  A wide format string containing non-ASCII literal text is
 * converted as UTF-8 like any other text.
 */

static char *format_to_bytes(const wchar_t *format)
{
	size_t cap = 0;
	char *buf;
	char *p;

	for (const wchar_t *w = format; *w; w++)
		cap += MB_LEN_MAX;
	buf = malloc(cap + 1);
	if (!buf)
		return 0;
	p = buf;
	for (const wchar_t *w = format; *w; w++) {
		size_t n = wcrtomb(p, *w, 0);
		if (n == (size_t)-1) {
			free(buf);
			errno = EILSEQ;
			return 0;
		}
		p += n;
	}
	*p = '\0';
	return buf;
}

int vfwprintf(FILE *stream, const wchar_t *format, va_list ap)
{
	char *fmt;
	int r;

	if (want_wide(stream) != 0)
		return -1;
	fmt = format_to_bytes(format);
	if (!fmt)
		return -1;
	r = vfprintf(stream, fmt, ap);
	free(fmt);
	return r;
}

int fwprintf(FILE *stream, const wchar_t *format, ...)
{
	va_list ap;
	int r;
	va_start(ap, format);
	r = vfwprintf(stream, format, ap);
	va_end(ap);
	return r;
}

int vwprintf(const wchar_t *format, va_list ap)
{
	return vfwprintf(stdout, format, ap);
}

int wprintf(const wchar_t *format, ...)
{
	va_list ap;
	int r;
	va_start(ap, format);
	r = vfwprintf(stdout, format, ap);
	va_end(ap);
	return r;
}

int vswprintf(wchar_t *s, size_t n, const wchar_t *format, va_list ap)
{
	char *fmt;
	char *out;
	size_t outcap;
	int r;
	size_t produced = 0;
	const char *p;
	mbstate_t st = { 0, 0 };

	if (!s || n == 0)
		return -1;

	fmt = format_to_bytes(format);
	if (!fmt)
		return -1;

	/* Bytes needed can exceed wide characters produced, never the reverse,
	 * so a buffer of n*MB_LEN_MAX always holds what will fit in s. */
	outcap = n * MB_LEN_MAX + 1;
	out = malloc(outcap);
	if (!out) {
		free(fmt);
		return -1;
	}
	r = vsnprintf(out, outcap, fmt, ap);
	free(fmt);
	if (r < 0) {
		free(out);
		return -1;
	}

	/* swprintf returns -1 on truncation, unlike snprintf: there is no
	 * "would have been" length in its contract. */
	p = out;
	while (*p) {
		wchar_t wc;
		size_t used = mbrtowc(&wc, p, (size_t)-1 / 2, &st);
		if (used == (size_t)-1 || used == (size_t)-2) {
			free(out);
			errno = EILSEQ;
			return -1;
		}
		if (produced + 1 >= n) {
			s[n - 1] = L'\0';
			free(out);
			return -1;
		}
		s[produced++] = wc;
		p += used;
	}
	s[produced] = L'\0';
	free(out);
	return (int)produced;
}

int swprintf(wchar_t *s, size_t n, const wchar_t *format, ...)
{
	va_list ap;
	int r;
	va_start(ap, format);
	r = vswprintf(s, n, format, ap);
	va_end(ap);
	return r;
}

size_t wcsftime(wchar_t *s, size_t n, const wchar_t *format,
		const struct tm *tm)
{
	char *fmt;
	char *out;
	size_t outcap;
	size_t r;
	size_t produced = 0;
	const char *p;
	mbstate_t st = { 0, 0 };

	if (!s || n == 0)
		return 0;

	fmt = format_to_bytes(format);
	if (!fmt)
		return 0;

	outcap = n * MB_LEN_MAX + 1;
	out = malloc(outcap);
	if (!out) {
		free(fmt);
		return 0;
	}
	r = strftime(out, outcap, fmt, tm);
	free(fmt);
	if (r == 0) {
		free(out);
		return 0;
	}

	p = out;
	while (*p) {
		wchar_t wc;
		size_t used = mbrtowc(&wc, p, (size_t)-1 / 2, &st);
		if (used == (size_t)-1 || used == (size_t)-2) {
			free(out);
			return 0;
		}
		if (produced + 1 >= n) {
			free(out);
			return 0; /* did not fit; contents unspecified */
		}
		s[produced++] = wc;
		p += used;
	}
	s[produced] = L'\0';
	free(out);
	return produced;
}
