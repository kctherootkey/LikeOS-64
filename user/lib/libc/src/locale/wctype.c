/*
 * wctype.c - wide character classification and case mapping.
 *
 * Every answer comes from the generated tables in unicode.c, so these classify
 * the whole of Unicode rather than the ASCII range with everything else
 * reported as "not a letter".  That distinction is what lets a shell word-split
 * a line containing "Grüße" and an editor upper-case it.
 *
 * The classes are the POSIX ones, which are not quite the Unicode general
 * categories -- the Arabic-Indic digits are alpha rather than digit, a
 * titlecase letter is both upper and lower.  Deriving the tables from a working
 * implementation (see host/gen-unicode-tables.c) is what keeps those right.
 */
#include <wctype.h>
#include <wchar.h>
#include <string.h>
#include "unicode.h"

/* WEOF is (wint_t)-1, which is not a code point; every class must reject it
 * rather than run it through a table lookup. */
#define VALID(wc) ((unsigned)(wc) < 0x110000u)

int iswalpha(wint_t wc)
{
	return VALID(wc) && __uni_isalpha((unsigned)wc);
}

int iswdigit(wint_t wc)
{
	/* Deliberately ASCII-only, and that is not a shortcut: iswdigit
	 * identifies the characters strtol and friends accept, and those are
	 * '0'..'9'.  The decimal digits of other scripts are alpha. */
	return (unsigned)wc - '0' < 10;
}

int iswalnum(wint_t wc)
{
	return VALID(wc) && __uni_isalnum((unsigned)wc);
}

int iswspace(wint_t wc)
{
	return VALID(wc) && __uni_isspace((unsigned)wc);
}

int iswblank(wint_t wc)
{
	return VALID(wc) && __uni_isblank((unsigned)wc);
}

int iswcntrl(wint_t wc)
{
	return VALID(wc) && __uni_iscntrl((unsigned)wc);
}

int iswprint(wint_t wc)
{
	return VALID(wc) && __uni_isprint((unsigned)wc);
}

int iswgraph(wint_t wc)
{
	return VALID(wc) && __uni_isgraph((unsigned)wc);
}

int iswpunct(wint_t wc)
{
	return VALID(wc) && __uni_ispunct((unsigned)wc);
}

int iswupper(wint_t wc)
{
	return VALID(wc) && __uni_isupper((unsigned)wc);
}

int iswlower(wint_t wc)
{
	return VALID(wc) && __uni_islower((unsigned)wc);
}

int iswxdigit(wint_t wc)
{
	return VALID(wc) && __uni_isxdigit((unsigned)wc);
}

wint_t towupper(wint_t wc)
{
	if (!VALID(wc))
		return wc;
	return (wint_t)__uni_toupper((unsigned)wc);
}

wint_t towlower(wint_t wc)
{
	if (!VALID(wc))
		return wc;
	return (wint_t)__uni_tolower((unsigned)wc);
}

/* ---- Class objects ------------------------------------------------------
 *
 * wctype() turns a class name into a token; iswctype() applies it.  The token
 * is a small integer rather than a pointer so that an invalid one is rejected
 * instead of dereferenced.                                                  */

enum {
	WCT_NONE = 0,
	WCT_ALNUM,
	WCT_ALPHA,
	WCT_BLANK,
	WCT_CNTRL,
	WCT_DIGIT,
	WCT_GRAPH,
	WCT_LOWER,
	WCT_PRINT,
	WCT_PUNCT,
	WCT_SPACE,
	WCT_UPPER,
	WCT_XDIGIT,
};

wctype_t wctype(const char *name)
{
	static const struct {
		const char *name;
		int id;
	} classes[] = {
		{ "alnum", WCT_ALNUM }, { "alpha", WCT_ALPHA },
		{ "blank", WCT_BLANK }, { "cntrl", WCT_CNTRL },
		{ "digit", WCT_DIGIT }, { "graph", WCT_GRAPH },
		{ "lower", WCT_LOWER }, { "print", WCT_PRINT },
		{ "punct", WCT_PUNCT }, { "space", WCT_SPACE },
		{ "upper", WCT_UPPER }, { "xdigit", WCT_XDIGIT },
	};
	if (!name)
		return WCT_NONE;
	for (size_t i = 0; i < sizeof(classes) / sizeof(*classes); i++)
		if (strcmp(name, classes[i].name) == 0)
			return (wctype_t)classes[i].id;
	return WCT_NONE;
}

int iswctype(wint_t wc, wctype_t type)
{
	switch ((int)type) {
	case WCT_ALNUM:
		return iswalnum(wc);
	case WCT_ALPHA:
		return iswalpha(wc);
	case WCT_BLANK:
		return iswblank(wc);
	case WCT_CNTRL:
		return iswcntrl(wc);
	case WCT_DIGIT:
		return iswdigit(wc);
	case WCT_GRAPH:
		return iswgraph(wc);
	case WCT_LOWER:
		return iswlower(wc);
	case WCT_PRINT:
		return iswprint(wc);
	case WCT_PUNCT:
		return iswpunct(wc);
	case WCT_SPACE:
		return iswspace(wc);
	case WCT_UPPER:
		return iswupper(wc);
	case WCT_XDIGIT:
		return iswxdigit(wc);
	default:
		return 0;
	}
}

/* ---- Case-mapping objects (wctrans/towctrans) -------------------------- */

enum { WCTR_NONE = 0, WCTR_TOUPPER = 1, WCTR_TOLOWER = 2 };

wctrans_t wctrans(const char *name)
{
	if (name && strcmp(name, "toupper") == 0)
		return (wctrans_t)WCTR_TOUPPER;
	if (name && strcmp(name, "tolower") == 0)
		return (wctrans_t)WCTR_TOLOWER;
	return (wctrans_t)WCTR_NONE;
}

wint_t towctrans(wint_t wc, wctrans_t trans)
{
	if (trans == (wctrans_t)WCTR_TOUPPER)
		return towupper(wc);
	if (trans == (wctrans_t)WCTR_TOLOWER)
		return towlower(wc);
	return wc;
}
