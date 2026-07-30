/*
 * The whole scanf family: fscanf, scanf, sscanf and their v- forms.
 *
 * ONE engine, two kinds of input.  There used to be two engines -- this one
 * for streams and a second, much smaller one in stdio.c for strings -- and
 * they diverged exactly as you would expect: the string one never grew float
 * conversions, so sscanf("1.5", "%g", &f) quietly returned 0 while
 * fscanf did the right thing.  X.Org's xkbcomp scans its geometry files with
 * sscanf and reported every coordinate as "Malformed number", which stopped
 * the display server from compiling a keymap.
 *
 * A duplicate implementation of a wide interface always ends up being the
 * weaker one, so the string source is now just another `struct src`.
 *
 * Conversions: %d %i %u %o %x %X %c %s %[...] %f %e %g %a %n %p %%, with
 * assignment suppression (*), a maximum field width, and the h/hh/l/ll/L/z/j/t
 * length modifiers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_CAP_L, LEN_Z, LEN_J, LEN_T };

/*
 * One-character pull with one character of pushback, and a running count of
 * how much input has been consumed (which is what %n reports).
 *
 * A single character of look-ahead is all the standard needs: every conversion
 * stops at the first character it cannot use, and that character must be left
 * for the next one.
 */
struct src {
	FILE *fp;        /* stream source, or NULL for a string */
	const char *str; /* string source, used when fp is NULL */
	size_t pos;
	int nread;
};

static int src_get(struct src *s)
{
	int c;

	if (s->fp) {
		c = fgetc(s->fp);
	} else {
		c = (unsigned char)s->str[s->pos];
		if (c == '\0')
			c = EOF; /* end of string reads as end of input */
		else
			s->pos++;
	}
	if (c != EOF)
		s->nread++;
	return c;
}

static void src_unget(struct src *s, int c)
{
	if (c == EOF)
		return; /* nothing was consumed, so nothing to put back */
	if (s->fp)
		ungetc(c, s->fp);
	else if (s->pos > 0)
		s->pos--;
	s->nread--;
}

static int digit_val(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Store an integer into the correctly sized target for the length modifier. */
static void store_int(va_list *ap, int length, int is_signed,
		      unsigned long long v)
{
	if (is_signed) {
		long long sv = (long long)v;
		switch (length) {
		case LEN_HH: *va_arg(*ap, char *) = (char)sv; break;
		case LEN_H:  *va_arg(*ap, short *) = (short)sv; break;
		case LEN_L:  *va_arg(*ap, long *) = (long)sv; break;
		case LEN_LL: *va_arg(*ap, long long *) = sv; break;
		case LEN_J:  *va_arg(*ap, long long *) = sv; break;
		case LEN_Z:
		case LEN_T:  *va_arg(*ap, long *) = (long)sv; break;
		default:     *va_arg(*ap, int *) = (int)sv; break;
		}
	} else {
		switch (length) {
		case LEN_HH: *va_arg(*ap, unsigned char *) = (unsigned char)v; break;
		case LEN_H:  *va_arg(*ap, unsigned short *) = (unsigned short)v; break;
		case LEN_L:  *va_arg(*ap, unsigned long *) = (unsigned long)v; break;
		case LEN_LL: *va_arg(*ap, unsigned long long *) = v; break;
		case LEN_J:  *va_arg(*ap, unsigned long long *) = v; break;
		case LEN_Z:
		case LEN_T:  *va_arg(*ap, unsigned long *) = (unsigned long)v; break;
		default:     *va_arg(*ap, unsigned int *) = (unsigned int)v; break;
		}
	}
}

static int scan_engine(struct src *sp, const char *format, va_list ap_in)
{
	int matched = 0;
	int c;
	va_list ap;
	va_copy(ap, ap_in);

	for (; *format; format++) {
		if (isspace((unsigned char)*format)) {
			/* A whitespace directive matches any run of it. */
			do { c = src_get(sp); } while (c != EOF &&
						       isspace((unsigned char)c));
			src_unget(sp, c);
			continue;
		}
		if (*format != '%') {
			c = src_get(sp);
			if (c != (unsigned char)*format) {
				src_unget(sp, c);
				goto done;
			}
			continue;
		}

		/* --- a conversion specification --- */
		format++;
		if (*format == '%') {
			c = src_get(sp);
			if (c != '%') { src_unget(sp, c); goto done; }
			continue;
		}

		int suppress = 0;
		if (*format == '*') { suppress = 1; format++; }

		int width = 0, have_width = 0;
		while (*format >= '0' && *format <= '9') {
			width = width * 10 + (*format - '0');
			have_width = 1;
			format++;
		}

		int length = LEN_NONE;
		switch (*format) {
		case 'h':
			length = LEN_H; format++;
			if (*format == 'h') { length = LEN_HH; format++; }
			break;
		case 'l':
			length = LEN_L; format++;
			if (*format == 'l') { length = LEN_LL; format++; }
			break;
		case 'L': length = LEN_CAP_L; format++; break;
		case 'j': length = LEN_J; format++; break;
		case 'z': length = LEN_Z; format++; break;
		case 't': length = LEN_T; format++; break;
		default: break;
		}

		char conv = *format;
		switch (conv) {
		case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
		case 'p': {
			int base = (conv == 'd' || conv == 'i' || conv == 'u') ? 10
				 : (conv == 'o') ? 8 : 16;
			int is_signed = (conv == 'd' || conv == 'i');
			int maxw = have_width ? width : INT_MAX;
			int n = 0, any = 0, neg = 0;
			unsigned long long val = 0;

			/* leading whitespace is always skipped for numerics */
			do { c = src_get(sp); } while (c != EOF &&
						       isspace((unsigned char)c));
			if ((c == '+' || c == '-') && n < maxw) {
				neg = (c == '-');
				c = src_get(sp); n++;
			}
			/* optional 0x / base autodetect for %i / %p */
			if ((conv == 'i' || conv == 'x' || conv == 'X' ||
			     conv == 'p') && c == '0' && n < maxw) {
				any = 1; c = src_get(sp); n++;
				if ((c == 'x' || c == 'X') && n < maxw) {
					base = 16; c = src_get(sp); n++;
					any = 0;
				} else if (conv == 'i') {
					base = 8;
				}
			}
			for (; c != EOF && n < maxw; c = src_get(sp), n++) {
				int d = digit_val(c);
				if (d < 0 || d >= base)
					break;
				val = val * base + (unsigned)d;
				any = 1;
			}
			src_unget(sp, c);
			if (!any)
				goto done;
			if (neg)
				val = (unsigned long long)(-(long long)val);
			if (!suppress) {
				if (conv == 'p')
					*va_arg(ap, void **) = (void *)(unsigned long)val;
				else
					store_int(&ap, length, is_signed, val);
				matched++;
			}
			break;
		}
		case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
			int maxw = have_width ? width : INT_MAX;
			char buf[64];
			int bi = 0, n = 0;
			do { c = src_get(sp); } while (c != EOF &&
						       isspace((unsigned char)c));
			if ((c == '+' || c == '-') && n < maxw &&
			    bi < (int)sizeof(buf) - 1) {
				buf[bi++] = (char)c; c = src_get(sp); n++;
			}
			int seen_digit = 0, seen_dot = 0;
			for (; c != EOF && n < maxw && bi < (int)sizeof(buf) - 1;
			     c = src_get(sp), n++) {
				if (c >= '0' && c <= '9') { seen_digit = 1; }
				else if (c == '.' && !seen_dot) { seen_dot = 1; }
				else if ((c == 'e' || c == 'E') && seen_digit) {
					buf[bi++] = (char)c;
					c = src_get(sp); n++;
					if ((c == '+' || c == '-') &&
					    bi < (int)sizeof(buf) - 1) {
						buf[bi++] = (char)c;
						continue;
					}
					/* fallthrough to store this digit */
					if (c >= '0' && c <= '9') { buf[bi++] = (char)c; continue; }
					break;
				} else break;
				buf[bi++] = (char)c;
			}
			src_unget(sp, c);
			buf[bi] = '\0';
			if (!seen_digit)
				goto done;
			if (!suppress) {
				double dv = strtod(buf, NULL);
				if (length == LEN_L)
					*va_arg(ap, double *) = dv;
				else if (length == LEN_CAP_L)
					*va_arg(ap, long double *) = (long double)dv;
				else
					*va_arg(ap, float *) = (float)dv;
				matched++;
			}
			break;
		}
		case 'c': {
			int cnt = have_width ? width : 1;
			char *out = suppress ? NULL : va_arg(ap, char *);
			int got = 0;
			for (int i = 0; i < cnt; i++) {
				c = src_get(sp);
				if (c == EOF) break;
				if (out) out[i] = (char)c;
				got++;
			}
			if (got < cnt)
				goto done;
			if (!suppress) matched++;
			break;
		}
		case 's': {
			int maxw = have_width ? width : INT_MAX;
			char *out = suppress ? NULL : va_arg(ap, char *);
			int got = 0;
			do { c = src_get(sp); } while (c != EOF &&
						       isspace((unsigned char)c));
			for (; c != EOF && !isspace((unsigned char)c) &&
			       got < maxw; c = src_get(sp), got++) {
				if (out) out[got] = (char)c;
			}
			src_unget(sp, c);
			if (got == 0)
				goto done;
			if (out) out[got] = '\0';
			if (!suppress) matched++;
			break;
		}
		case '[': {
			format++;
			int negate = 0;
			if (*format == '^') { negate = 1; format++; }
			/* build the accept set */
			unsigned char set[256] = { 0 };
			if (*format == ']') { set[(unsigned char)']'] = 1; format++; }
			while (*format && *format != ']') {
				if (format[1] == '-' && format[2] &&
				    format[2] != ']') {
					unsigned char lo = (unsigned char)format[0];
					unsigned char hi = (unsigned char)format[2];
					for (unsigned char x = lo; x <= hi; x++)
						set[x] = 1;
					format += 3;
				} else {
					set[(unsigned char)*format] = 1;
					format++;
				}
			}
			int maxw = have_width ? width : INT_MAX;
			char *out = suppress ? NULL : va_arg(ap, char *);
			int got = 0;
			for (; got < maxw; got++) {
				c = src_get(sp);
				if (c == EOF) break;
				int in = set[(unsigned char)c] ? 1 : 0;
				if (negate) in = !in;
				if (!in) { src_unget(sp, c); break; }
				if (out) out[got] = (char)c;
			}
			if (got == 0)
				goto done;
			if (out) out[got] = '\0';
			if (!suppress) matched++;
			break;
		}
		case 'n':
			if (!suppress)
				store_int(&ap, length, 1, (unsigned long long)sp->nread);
			break;
		default:
			/* Unknown conversion — stop, matching glibc's behaviour. */
			goto done;
		}
	}

done:
	va_end(ap);
	/* Distinguish "matched nothing because of EOF" (return EOF) from
	 * "matched nothing because of a conversion failure" (return 0).  Only
	 * probe the stream when nothing matched, so `c` is never read unset. */
	if (matched == 0) {
		c = src_get(sp);
		if (c == EOF)
			return EOF;
		src_unget(sp, c);
	}
	return matched;
}

/* ------------------------------------------------------------------ */
/* Entry points.  All four are the same engine over a different source. */
/* ------------------------------------------------------------------ */

int vfscanf(FILE *stream, const char *format, va_list ap)
{
	struct src s = { stream, NULL, 0, 0 };

	return scan_engine(&s, format, ap);
}

int vsscanf(const char *str, const char *format, va_list ap)
{
	struct src s = { NULL, str, 0, 0 };

	if (!str)
		return EOF;
	return scan_engine(&s, format, ap);
}

int fscanf(FILE *stream, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int r = vfscanf(stream, format, ap);
	va_end(ap);
	return r;
}

int scanf(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int r = vfscanf(stdin, format, ap);
	va_end(ap);
	return r;
}

int vscanf(const char *format, va_list ap)
{
	return vfscanf(stdin, format, ap);
}

int sscanf(const char *str, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int r = vsscanf(str, format, ap);
	va_end(ap);
	return r;
}
