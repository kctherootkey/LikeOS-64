/*
 * iconv - character set conversion.
 *
 * Structured as a pivot: every supported encoding has a decoder to a Unicode
 * code point and an encoder from one, so adding an encoding costs two small
 * functions rather than a converter per pair.
 *
 * The set is deliberately the encodings that text on this system is actually
 * in, rather than a token one:
 *
 *   UTF-8                     the default everywhere here
 *   UTF-16LE / UTF-16BE / UTF-16  fixed and surrogate-paired forms
 *   UTF-32LE / UTF-32BE / UTF-32
 *   ISO-8859-1 (Latin-1)      the 8-bit set the X core fonts use
 *   ISO-8859-15 (Latin-9)     Latin-1 with the euro and friends
 *   US-ASCII
 *
 * An unsupported name fails in iconv_open with EINVAL, which is what callers
 * check -- so a program offering an encoding menu simply reports that one
 * unavailable instead of misconverting.
 */
#include <iconv.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------- charsets */

enum charset {
	CS_UTF8,
	CS_UTF16LE,
	CS_UTF16BE,
	CS_UTF16, /* BOM-sniffing on input, LE with BOM on output */
	CS_UTF32LE,
	CS_UTF32BE,
	CS_UTF32,
	CS_LATIN1,
	CS_LATIN9,
	CS_ASCII,
};

struct iconv_state {
	enum charset from;
	enum charset to;
	int in_bom_done; /* UTF-16/32 input: BOM has been consumed */
	int out_bom_done; /* UTF-16/32 output: BOM has been written */
	int translit; /* target was named with //TRANSLIT */
};

/* Compare charset names the way the standard converters do: case-insensitively
 * and ignoring '-', '_' and spaces, so "ISO-8859-1", "iso88591" and
 * "ISO_8859-1" are one name. */
static int cs_name_eq(const char *a, const char *b)
{
	for (;;) {
		unsigned char ca, cb;
		while (*a == '-' || *a == '_' || *a == ' ')
			a++;
		while (*b == '-' || *b == '_' || *b == ' ')
			b++;
		ca = (unsigned char)*a;
		cb = (unsigned char)*b;
		if (ca >= 'a' && ca <= 'z')
			ca = (unsigned char)(ca - 'a' + 'A');
		if (cb >= 'a' && cb <= 'z')
			cb = (unsigned char)(cb - 'a' + 'A');
		if (ca != cb)
			return 0;
		if (ca == '\0')
			return 1;
		a++;
		b++;
	}
}

/* Resolve a charset name.  A trailing "//TRANSLIT" or "//IGNORE" selects a
 * fallback behaviour rather than naming a different charset, so the suffix is
 * split off here: *translit reports whether TRANSLIT was asked for, and the
 * base name is what gets matched.  Recognising the base matters -- a program
 * asking for "UTF-8//TRANSLIT" wants UTF-8, and failing the open outright
 * would be worse than converting strictly. */
static int cs_lookup(const char *name, enum charset *out, int *translit)
{
	static const struct {
		const char *name;
		enum charset cs;
	} table[] = {
		{ "UTF-8", CS_UTF8 },
		{ "UTF8", CS_UTF8 },
		{ "UTF-16LE", CS_UTF16LE },
		{ "UTF-16BE", CS_UTF16BE },
		{ "UTF-16", CS_UTF16 },
		{ "UCS-2LE", CS_UTF16LE },
		{ "UCS-2BE", CS_UTF16BE },
		{ "UTF-32LE", CS_UTF32LE },
		{ "UTF-32BE", CS_UTF32BE },
		{ "UTF-32", CS_UTF32 },
		{ "UCS-4LE", CS_UTF32LE },
		{ "UCS-4BE", CS_UTF32BE },
		{ "ISO-8859-1", CS_LATIN1 },
		{ "ISO8859-1", CS_LATIN1 },
		{ "LATIN1", CS_LATIN1 },
		{ "L1", CS_LATIN1 },
		{ "ISO-8859-15", CS_LATIN9 },
		{ "ISO8859-15", CS_LATIN9 },
		{ "LATIN9", CS_LATIN9 },
		{ "L9", CS_LATIN9 },
		{ "US-ASCII", CS_ASCII },
		{ "ASCII", CS_ASCII },
		{ "ANSI_X3.4-1968", CS_ASCII },
	};
	char base[64];
	const char *sfx = strstr(name, "//");
	const char *n = name;
	size_t i;

	if (translit)
		*translit = 0;
	if (sfx) {
		size_t blen = (size_t)(sfx - name);
		if (blen >= sizeof(base))
			return -1;
		memcpy(base, name, blen);
		base[blen] = '\0';
		n = base;
		if (translit && cs_name_eq(sfx, "//TRANSLIT"))
			*translit = 1;
	}
	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (cs_name_eq(n, table[i].name)) {
			*out = table[i].cs;
			return 0;
		}
	}
	return -1;
}

/* ------------------------------------------------- Latin-9 exception table */

/* ISO-8859-15 is Latin-1 with eight positions replaced.  Storing only the
 * differences keeps the relationship visible instead of burying it in a
 * 256-entry table. */
static const struct {
	unsigned char byte;
	uint32_t cp;
} latin9_diff[] = {
	{ 0xA4, 0x20AC }, /* EURO SIGN */
	{ 0xA6, 0x0160 }, /* S with caron */
	{ 0xA8, 0x0161 }, /* s with caron */
	{ 0xB4, 0x017D }, /* Z with caron */
	{ 0xB8, 0x017E }, /* z with caron */
	{ 0xBC, 0x0152 }, /* OE ligature */
	{ 0xBD, 0x0153 }, /* oe ligature */
	{ 0xBE, 0x0178 }, /* Y with diaeresis */
};

static uint32_t latin9_to_cp(unsigned char b)
{
	size_t i;
	for (i = 0; i < sizeof(latin9_diff) / sizeof(latin9_diff[0]); i++)
		if (latin9_diff[i].byte == b)
			return latin9_diff[i].cp;
	return b;
}

static int latin9_from_cp(uint32_t cp, unsigned char *out)
{
	size_t i;
	for (i = 0; i < sizeof(latin9_diff) / sizeof(latin9_diff[0]); i++) {
		if (latin9_diff[i].cp == cp) {
			*out = latin9_diff[i].byte;
			return 0;
		}
		/* A byte whose Latin-9 meaning differs must NOT pass through as
		 * itself, or the euro position would silently encode U+00A4. */
		if (latin9_diff[i].byte == cp)
			return -1;
	}
	if (cp > 0xFF)
		return -1;
	*out = (unsigned char)cp;
	return 0;
}

/* ------------------------------------------------------------- decode step */

/* Pull one code point from `in`.  Returns bytes consumed, 0 if the input is a
 * truncated but so-far-valid sequence (EINVAL territory), or -1 on an invalid
 * sequence (EILSEQ). */
static int decode(enum charset cs, const unsigned char *in, size_t left,
		  uint32_t *cp, struct iconv_state *st)
{
	switch (cs) {
	case CS_UTF8: {
		unsigned char c = in[0];
		int n;
		uint32_t v;
		if (c < 0x80) {
			*cp = c;
			return 1;
		}
		if (c == 0xC0 || c == 0xC1) {
			/* These two lead bytes can only ever produce an
			 * over-long encoding of something that fits in one
			 * byte, so no continuation can rescue them: invalid
			 * now, not "incomplete". */
			return -1;
		} else if ((c & 0xE0) == 0xC0) {
			n = 2;
			v = c & 0x1Fu;
		} else if ((c & 0xF0) == 0xE0) {
			n = 3;
			v = c & 0x0Fu;
		} else if ((c & 0xF8) == 0xF0) {
			n = 4;
			v = c & 0x07u;
		} else if ((c & 0xFC) == 0xF8) {
			n = 5; /* the withdrawn 5-byte form */
			v = 0;
		} else if ((c & 0xFE) == 0xFC) {
			n = 6; /* the withdrawn 6-byte form */
			v = 0;
		} else {
			/* A bare continuation byte, or 0xFE/0xFF which have
			 * never been lead bytes in any version of UTF-8. */
			return -1;
		}
		/* Validate the continuation bytes that are PRESENT before
		 * judging completeness.  A byte that is not a continuation
		 * makes the sequence invalid immediately, however many more
		 * were expected -- so "FD E7" is EILSEQ (E7 cannot follow)
		 * while "FC 9A" is EINVAL (9A can, there is just no more
		 * input yet).  Deciding length first would report the first
		 * as merely truncated and leave a caller waiting for bytes
		 * that could never make it valid. */
		{
			size_t avail = left < (size_t)n ? left : (size_t)n;
			for (size_t i = 1; i < avail; i++)
				if ((in[i] & 0xC0) != 0x80)
					return -1;
		}
		if (left < (size_t)n)
			return 0;
		if (n > 4) {
			/* The 5- and 6-byte forms were removed from UTF-8 and
			 * encode nothing in Unicode's range. */
			return -1;
		}
		for (int i = 1; i < n; i++)
			v = (v << 6) | (uint32_t)(in[i] & 0x3F);
		/* Reject the encodings that are valid bit patterns but not
		 * valid UTF-8: over-long forms, surrogates, and above the
		 * Unicode maximum.  Accepting them is how UTF-8 decoders turn
		 * into security holes. */
		if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) ||
		    (n == 4 && v < 0x10000))
			return -1;
		if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
			return -1;
		*cp = v;
		return n;
	}
	case CS_UTF16:
	case CS_UTF16LE:
	case CS_UTF16BE: {
		int be = (cs == CS_UTF16BE);
		uint32_t hi;
		if (cs == CS_UTF16 && !st->in_bom_done) {
			if (left < 2)
				return 0;
			if (in[0] == 0xFF && in[1] == 0xFE) {
				st->in_bom_done = 1;
				st->from = CS_UTF16LE;
				return 2 | 0x1000; /* consumed, no code point */
			}
			if (in[0] == 0xFE && in[1] == 0xFF) {
				st->in_bom_done = 1;
				st->from = CS_UTF16BE;
				return 2 | 0x1000;
			}
			/* No BOM: the standard default is big-endian. */
			st->in_bom_done = 1;
			st->from = CS_UTF16BE;
			be = 1;
		}
		if (left < 2)
			return 0;
		hi = be ? (uint32_t)((in[0] << 8) | in[1]) :
			  (uint32_t)((in[1] << 8) | in[0]);
		if (hi >= 0xD800 && hi <= 0xDBFF) {
			uint32_t lo;
			if (left < 4)
				return 0;
			lo = be ? (uint32_t)((in[2] << 8) | in[3]) :
				  (uint32_t)((in[3] << 8) | in[2]);
			if (lo < 0xDC00 || lo > 0xDFFF)
				return -1;
			*cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
			return 4;
		}
		if (hi >= 0xDC00 && hi <= 0xDFFF)
			return -1; /* unpaired low surrogate */
		*cp = hi;
		return 2;
	}
	case CS_UTF32:
	case CS_UTF32LE:
	case CS_UTF32BE: {
		int be = (cs == CS_UTF32BE);
		uint32_t v;
		if (cs == CS_UTF32 && !st->in_bom_done) {
			if (left < 4)
				return 0;
			if (in[0] == 0xFF && in[1] == 0xFE && in[2] == 0 &&
			    in[3] == 0) {
				st->in_bom_done = 1;
				st->from = CS_UTF32LE;
				return 4 | 0x1000;
			}
			if (in[0] == 0 && in[1] == 0 && in[2] == 0xFE &&
			    in[3] == 0xFF) {
				st->in_bom_done = 1;
				st->from = CS_UTF32BE;
				return 4 | 0x1000;
			}
			st->in_bom_done = 1;
			st->from = CS_UTF32BE;
			be = 1;
		}
		if (left < 4)
			return 0;
		v = be ? ((uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 |
			  (uint32_t)in[2] << 8 | in[3]) :
			 ((uint32_t)in[3] << 24 | (uint32_t)in[2] << 16 |
			  (uint32_t)in[1] << 8 | in[0]);
		if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
			return -1;
		*cp = v;
		return 4;
	}
	case CS_LATIN1:
		*cp = in[0];
		return 1;
	case CS_LATIN9:
		*cp = latin9_to_cp(in[0]);
		return 1;
	case CS_ASCII:
		if (in[0] > 0x7F)
			return -1;
		*cp = in[0];
		return 1;
	}
	return -1;
}

/* ------------------------------------------------------------- encode step */

/* Write one code point.  Returns bytes written, -1 if it does not exist in the
 * target set (a non-reversible conversion the caller is told about), or -2 if
 * there is not enough room (E2BIG). */
static int encode(enum charset cs, uint32_t cp, unsigned char *out, size_t room,
		  struct iconv_state *st)
{
	switch (cs) {
	case CS_UTF8:
		if (cp < 0x80) {
			if (room < 1)
				return -2;
			out[0] = (unsigned char)cp;
			return 1;
		}
		if (cp < 0x800) {
			if (room < 2)
				return -2;
			out[0] = (unsigned char)(0xC0 | (cp >> 6));
			out[1] = (unsigned char)(0x80 | (cp & 0x3F));
			return 2;
		}
		if (cp < 0x10000) {
			if (room < 3)
				return -2;
			out[0] = (unsigned char)(0xE0 | (cp >> 12));
			out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
			out[2] = (unsigned char)(0x80 | (cp & 0x3F));
			return 3;
		}
		if (room < 4)
			return -2;
		out[0] = (unsigned char)(0xF0 | (cp >> 18));
		out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (unsigned char)(0x80 | (cp & 0x3F));
		return 4;
	case CS_UTF16:
	case CS_UTF16LE:
	case CS_UTF16BE: {
		int be = (cs == CS_UTF16BE);
		int off = 0;
		if (cs == CS_UTF16 && !st->out_bom_done) {
			/* Plain "UTF-16" output carries a BOM; that is what
			 * makes it self-describing, and the reader above
			 * depends on it. */
			if (room < 2)
				return -2;
			out[0] = 0xFF;
			out[1] = 0xFE;
			st->out_bom_done = 1;
			off = 2;
			room -= 2;
		}
		if (cp >= 0x10000) {
			uint32_t v = cp - 0x10000;
			uint32_t hi = 0xD800 + (v >> 10);
			uint32_t lo = 0xDC00 + (v & 0x3FF);
			if (room < 4)
				return -2;
			if (be) {
				out[off + 0] = (unsigned char)(hi >> 8);
				out[off + 1] = (unsigned char)hi;
				out[off + 2] = (unsigned char)(lo >> 8);
				out[off + 3] = (unsigned char)lo;
			} else {
				out[off + 0] = (unsigned char)hi;
				out[off + 1] = (unsigned char)(hi >> 8);
				out[off + 2] = (unsigned char)lo;
				out[off + 3] = (unsigned char)(lo >> 8);
			}
			return off + 4;
		}
		if (room < 2)
			return -2;
		if (be) {
			out[off + 0] = (unsigned char)(cp >> 8);
			out[off + 1] = (unsigned char)cp;
		} else {
			out[off + 0] = (unsigned char)cp;
			out[off + 1] = (unsigned char)(cp >> 8);
		}
		return off + 2;
	}
	case CS_UTF32:
	case CS_UTF32LE:
	case CS_UTF32BE: {
		int be = (cs == CS_UTF32BE);
		int off = 0;
		if (cs == CS_UTF32 && !st->out_bom_done) {
			if (room < 4)
				return -2;
			out[0] = 0xFF;
			out[1] = 0xFE;
			out[2] = 0;
			out[3] = 0;
			st->out_bom_done = 1;
			off = 4;
			room -= 4;
		}
		if (room < 4)
			return -2;
		if (be) {
			out[off + 0] = (unsigned char)(cp >> 24);
			out[off + 1] = (unsigned char)(cp >> 16);
			out[off + 2] = (unsigned char)(cp >> 8);
			out[off + 3] = (unsigned char)cp;
		} else {
			out[off + 0] = (unsigned char)cp;
			out[off + 1] = (unsigned char)(cp >> 8);
			out[off + 2] = (unsigned char)(cp >> 16);
			out[off + 3] = (unsigned char)(cp >> 24);
		}
		return off + 4;
	}
	case CS_LATIN1:
		if (cp > 0xFF)
			return -1;
		if (room < 1)
			return -2;
		out[0] = (unsigned char)cp;
		return 1;
	case CS_LATIN9: {
		unsigned char b;
		if (latin9_from_cp(cp, &b) != 0)
			return -1;
		if (room < 1)
			return -2;
		out[0] = b;
		return 1;
	}
	case CS_ASCII:
		if (cp > 0x7F)
			return -1;
		if (room < 1)
			return -2;
		out[0] = (unsigned char)cp;
		return 1;
	}
	return -1;
}

/* --------------------------------------------------------------------- API */

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
	struct iconv_state *st;
	enum charset to, from;
	int translit = 0;

	if (!tocode || !fromcode) {
		errno = EINVAL;
		return (iconv_t)-1;
	}
	if (cs_lookup(tocode, &to, &translit) != 0 ||
	    cs_lookup(fromcode, &from, NULL) != 0) {
		errno = EINVAL;
		return (iconv_t)-1;
	}
	st = malloc(sizeof(*st));
	if (!st) {
		errno = ENOMEM;
		return (iconv_t)-1;
	}
	st->from = from;
	st->to = to;
	st->in_bom_done = 0;
	st->out_bom_done = 0;
	st->translit = translit;
	return (iconv_t)st;
}

int iconv_close(iconv_t cd)
{
	if (cd == (iconv_t)-1 || cd == NULL) {
		errno = EBADF;
		return -1;
	}
	free(cd);
	return 0;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft, char **outbuf,
	     size_t *outbytesleft)
{
	struct iconv_state *st = (struct iconv_state *)cd;
	size_t nonreversible = 0;

	if (cd == (iconv_t)-1 || cd == NULL) {
		errno = EBADF;
		return (size_t)-1;
	}

	/* Reset request.  These encodings carry no shift state, so there is
	 * nothing to flush -- but the BOM bookkeeping does have to go back to
	 * the start, or a reused descriptor would omit the next BOM. */
	if (!inbuf || !*inbuf) {
		st->in_bom_done = 0;
		st->out_bom_done = 0;
		return 0;
	}

	while (*inbytesleft > 0) {
		uint32_t cp = 0;
		int used, put;

		used = decode(st->from, (const unsigned char *)*inbuf,
			      *inbytesleft, &cp, st);
		if (used == 0) {
			errno = EINVAL; /* truncated sequence */
			return (size_t)-1;
		}
		if (used < 0) {
			errno = EILSEQ;
			return (size_t)-1;
		}
		if (used & 0x1000) { /* a BOM was consumed, no character */
			used &= 0xFFF;
			*inbuf += used;
			*inbytesleft -= (size_t)used;
			continue;
		}

		put = encode(st->to, cp, (unsigned char *)*outbuf,
			     outbytesleft ? *outbytesleft : 0, st);
		if (put == -2) {
			errno = E2BIG;
			return (size_t)-1;
		}
		if (put == -1) {
			/* Not representable in the target set.
			 *
			 * This is EILSEQ, NOT a silent substitution: plain
			 * iconv reports the failure and only //TRANSLIT asks
			 * for a replacement character.  Substituting by default
			 * would turn "this text cannot be saved as Latin-1"
			 * into a file quietly full of question marks. */
			if (!st->translit) {
				errno = EILSEQ;
				return (size_t)-1;
			}
			put = encode(st->to, (uint32_t)'?',
				     (unsigned char *)*outbuf,
				     outbytesleft ? *outbytesleft : 0, st);
			if (put == -2) {
				errno = E2BIG;
				return (size_t)-1;
			}
			if (put < 0) {
				errno = EILSEQ;
				return (size_t)-1;
			}
			nonreversible++;
		}
		*outbuf += put;
		*outbytesleft -= (size_t)put;
		*inbuf += used;
		*inbytesleft -= (size_t)used;
	}
	return nonreversible;
}
