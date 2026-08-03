// LikeOS-64 Unicode support for the console
//
// The console decodes UTF-8 and addresses glyphs by code point, which leaves
// one question the byte-oriented code never had to answer: how many cells does
// a character take?  Most take one, a combining mark takes none, and an East
// Asian wide character takes two.  Getting it wrong is not cosmetic -- the
// program writing to the console computes its own columns the same way, and a
// disagreement leaves the cursor and the text permanently out of step.

#ifndef _KERNEL_UNICODE_H_
#define _KERNEL_UNICODE_H_

#include <kernel/uapi/types.h>

// Cells a code point occupies: 0, 1 or 2.  Table in kernel/io/unicode.c,
// generated from the same probe as the C library's wcwidth so the two cannot
// disagree.  Control characters are the console's business, not this one's:
// it filters them before drawing, and they answer 1 here.
uint32_t unicode_width(uint32_t cp);

#endif // _KERNEL_UNICODE_H_
