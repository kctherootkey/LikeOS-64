/*
 * wcwidth.c - how many terminal columns a wide character occupies.
 *
 * Three answers are possible.  Combining marks and format characters take no
 * column of their own: they attach to the character before them, so counting
 * them would push every later column one to the right.  East Asian Wide and
 * Fullwidth characters take two.  Everything else that prints takes one, and
 * what does not print at all answers -1.
 *
 * Getting this wrong is not cosmetic.  Every line editor, pager and terminal
 * multiplexer computes cursor positions from wcwidth, so a character reported
 * at the wrong width leaves the cursor and the text permanently out of step.
 */
#include <wchar.h>
#include "unicode.h"

int wcwidth(wchar_t wc)
{
	unsigned cp = (unsigned)wc;

	if (cp >= 0x110000u)
		return -1;
	return __uni_width(cp);
}

int wcswidth(const wchar_t *s, size_t n)
{
	int total = 0;

	for (; n && *s; n--, s++) {
		int w = wcwidth(*s);
		if (w < 0)
			return -1;
		total += w;
	}
	return total;
}
