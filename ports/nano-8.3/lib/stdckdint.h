/* stdckdint.h -- checked integer arithmetic for LikeOS
 * Generated from stdckdint.in.h for gnulib. */

#ifndef _GL_STDCKDINT_H
#define _GL_STDCKDINT_H

#include "intprops-internal.h"

#define ckd_add(r, a, b) ((bool) _GL_INT_ADD_WRAPV (a, b, r))
#define ckd_sub(r, a, b) ((bool) _GL_INT_SUBTRACT_WRAPV (a, b, r))
#define ckd_mul(r, a, b) ((bool) _GL_INT_MULTIPLY_WRAPV (a, b, r))

#endif /* _GL_STDCKDINT_H */
